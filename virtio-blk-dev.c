/* virtio-blk device engine: probe, feature negotiation, config, queue.

   Legacy PCI policy per the plan's MVP: probe 0x1af4:0x1001, reset/ACK/
   DRIVER, accept exactly the implemented feature bits (each has matching
   behavior, so the accepted mask is not wishful), program requestq 0,
   read the negotiated config, reach DRIVER_OK, and service queue and
   config-change interrupts through the transport's IRQ core.

   Config reads follow the feature gates: capacity is unconditional,
   size_max/seg_max/blk_size only when their bits were accepted.  The
   64-bit capacity is two little-endian 32-bit config reads, the way the
   legacy config region is addressed.  */

#include <errno.h>
#include <stdio.h>

#include "virtio-blk.h"

error_t
vblk_dev_init (struct vblk_dev *d, const char *override)
{
  struct virtio_queue *queues[1];
  uint32_t lo, hi;
  error_t err;

  d->pci.name = "virtio-blk";
  d->pci.device_id = VBLK_DEVICE_ID;

  err = virtio_pci_probe (&d->pci, override);
  if (err)
    return err;

  virtio_pci_reset (&d->pci);
  virtio_pci_set_driver (&d->pci);

  d->offered = virtio_pci_read_host_features (&d->pci);
  d->accepted = d->offered & VBLK_FEATURES;
  virtio_pci_set_guest_features (&d->pci, d->accepted);
  fprintf (stderr, "virtio-blk: offered features 0x%x, accepted 0x%x\n",
           d->offered, d->accepted);
  /* Ack any interrupt the handshake itself raised.  */
  virtio_pci_read_isr (&d->pci);

  err = virtio_queue_setup (&d->pci, &d->reqq, 0);
  if (err)
    return err;
  err = vblk_req_init (d);
  if (err)
    return err;

  err = virtio_pci_driver_ok (&d->pci);
  if (err)
    return err;

  /* struct virtio_blk_config, little-endian, field order per QEMU's
     include/standard-headers/linux/virtio_blk.h (capacity :62-64,
     size_max :65-66, seg_max :67-68, blk_size :75-76): capacity is
     unconditional, the optional fields are read only under their
     accepted bits, and the 64-bit capacity is two 32-bit config reads
     the way the legacy config region is addressed.  */
  lo = virtio_pci_read_config (&d->pci, VBLK_CFG_CAPACITY);
  hi = virtio_pci_read_config (&d->pci, VBLK_CFG_CAPACITY + 4);
  d->capacity = (uint64_t) lo | ((uint64_t) hi << 32);
  if (d->capacity > UINT64_MAX / VBLK_SECTOR_SIZE)
    {
      fprintf (stderr, "virtio-blk: capacity %llu overflows byte size\n",
               (unsigned long long) d->capacity);
      return EIO;
    }
  d->size_bytes = d->capacity * VBLK_SECTOR_SIZE;

  if (d->accepted & (1u << VBLK_F_SIZE_MAX))
    d->size_max = virtio_pci_read_config (&d->pci, VBLK_CFG_SIZE_MAX);
  if (d->accepted & (1u << VBLK_F_SEG_MAX))
    d->seg_max = virtio_pci_read_config (&d->pci, VBLK_CFG_SEG_MAX);
  if (d->accepted & (1u << VBLK_F_BLK_SIZE))
    d->blk_size = virtio_pci_read_config (&d->pci, VBLK_CFG_BLK_SIZE);
  d->ro = (d->accepted & (1u << VBLK_F_RO)) != 0;
  d->flush = (d->accepted & (1u << VBLK_F_FLUSH)) != 0;

  /* The request chain carries one data descriptor, so SEG_MAX only has
     to admit 1; SIZE_MAX caps the per-request data length; BLK_SIZE
     sets the transfer unit the request builder rounds chunks to.  */
  d->max_transfer = VBLK_DATA_MAX;
  if (d->size_max && d->size_max < d->max_transfer)
    d->max_transfer = d->size_max;
  if ((d->accepted & (1u << VBLK_F_SEG_MAX)) && d->seg_max < 1)
    fprintf (stderr, "virtio-blk: seg_max %u below the single data "
            "descriptor each request uses\n", d->seg_max);
  if (d->blk_size % VBLK_SECTOR_SIZE || d->blk_size > VBLK_DATA_MAX)
    {
      fprintf (stderr, "virtio-blk: blk_size %u is not a whole number of "
              "512-byte sectors, using 512 as the transfer unit\n",
              d->blk_size);
      d->blk_size = 0;
    }

  fprintf (stderr, "virtio-blk: capacity %llu sectors (%llu bytes)%s%s\n",
           (unsigned long long) d->capacity,
           (unsigned long long) d->size_bytes,
           d->ro ? ", read-only" : "",
           d->flush ? ", flush" : "");
  fprintf (stderr,
           "virtio-blk: blk_size %u, size_max %u, seg_max %u, "
           "max transfer %u\n",
           d->blk_size, d->size_max, d->seg_max, d->max_transfer);

  queues[0] = &d->reqq;
  return virtio_irq_install (&d->irq, &d->pci, queues, 1);
}

/* Config-change interrupts re-read the fields the device may change;
   capacity changes are uncommon but must not leave stale limits behind.
   */
void
vblk_dev_reread_config (struct vblk_dev *d)
{
  uint32_t lo, hi;
  uint64_t capacity;

  lo = virtio_pci_read_config (&d->pci, VBLK_CFG_CAPACITY);
  hi = virtio_pci_read_config (&d->pci, VBLK_CFG_CAPACITY + 4);
  capacity = (uint64_t) lo | ((uint64_t) hi << 32);
  if (capacity != d->capacity)
    {
      fprintf (stderr, "virtio-blk: capacity changed %llu -> %llu sectors\n",
               (unsigned long long) d->capacity, (unsigned long long) capacity);
      d->capacity = capacity;
      d->size_bytes = capacity * VBLK_SECTOR_SIZE;
    }
  d->stats.config_changes++;
}

/* Read/write with the MVP alignment and bound policy: whole transfer
   units, in range, and writes refused on a read-only device before any
   descriptor is posted.  */
static error_t
vblk_dev_rw_check (struct vblk_dev *d, int is_write, uint64_t sector,
                   size_t len)
{
  uint32_t unit = d->blk_size ? d->blk_size : VBLK_SECTOR_SIZE;
  uint64_t count = len / VBLK_SECTOR_SIZE;

  if (is_write && d->ro)
    {
      fprintf (stderr, "virtio-blk: write refused on a read-only device\n");
      return EROFS;
    }
  if (len == 0 || len % unit || sector % (unit / VBLK_SECTOR_SIZE))
    return EINVAL;
  if (count > d->capacity || sector > d->capacity - count)
    return EIO;
  return 0;
}

error_t
vblk_dev_read (struct vblk_dev *d, uint64_t sector, void *data, size_t len,
               size_t *nread)
{
  error_t err = vblk_dev_rw_check (d, 0, sector, len);
  if (err)
    {
      *nread = 0;
      return err;
    }
  return vblk_req_rw (d, 0, sector, data, len, nread);
}

error_t
vblk_dev_write (struct vblk_dev *d, uint64_t sector, const void *data,
                size_t len, size_t *nwritten)
{
  error_t err = vblk_dev_rw_check (d, 1, sector, len);
  if (err)
    {
      *nwritten = 0;
      return err;
    }
  return vblk_req_rw (d, 1, sector, (void *) data, len, nwritten);
}

error_t
vblk_dev_flush (struct vblk_dev *d)
{
  if (!d->flush)
    {
      fprintf (stderr, "virtio-blk: flush requested without VIRTIO_BLK_F_FLUSH\n");
      return EOPNOTSUPP;
    }
  return vblk_req_flush (d);
}

void
vblk_dev_quiesce (struct vblk_dev *d)
{
  virtio_irq_uninstall (&d->irq);
  vblk_req_release (d);
}

const struct vblk_stats *
vblk_dev_stats (struct vblk_dev *d)
{
  return &d->stats;
}
