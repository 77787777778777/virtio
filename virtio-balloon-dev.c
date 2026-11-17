/* Balloon protocol engine: device config, feature policy, and the
   inflate/deflate queue handling.

   Both classic queues carry driver-written, device-readable PFN arrays:
   one output descriptor per batch, no WRITE flag, no reply buffer, and the
   used length is logged rather than assumed (QEMU pushes 0 for these
   input buffers, but the protocol does not guarantee it).  Config fields
   are little-endian, PFNs are 4 KiB units, and `actual` is written after
   the used ring acknowledges each batch.  */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "virtio-balloon.h"

/* The legacy balloon config region and the PFN arrays are little-endian
   on the wire (the balloon device is the documented exception among virtio
   devices), independent of guest native endianness; the encoding helpers
   live with the shared transport.  */
static void
put_pfns (uint32_t *dst, const uint32_t *pfns, uint32_t count)
{
  uint32_t i;
  for (i = 0; i < count; i++)
    virtio_put_le32 (dst + i, pfns[i]);
}

static error_t
vballoon_dev_bring_up (struct vballoon *b, const char *override)
{
  struct virtio_queue *queues[2];
  uint32_t offered, accepted;
  error_t err;

  b->pci.name = "virtio-balloon";
  b->pci.device_id = VBALLOON_DEVICE_ID;

  err = virtio_pci_probe (&b->pci, override);
  if (err)
    return err;

  virtio_pci_reset (&b->pci);
  virtio_pci_set_driver (&b->pci);

  offered = virtio_pci_read_host_features (&b->pci);
  accepted = offered & (1u << VBALLOON_F_MUST_TELL_HOST);
  virtio_pci_set_guest_features (&b->pci, accepted);
  fprintf (stderr, "virtio-balloon: offered features 0x%x, accepted 0x%x\n",
           offered, accepted);

  /* Ack any interrupt the handshake itself raised.  */
  virtio_pci_read_isr (&b->pci);

  err = virtio_queue_setup (&b->pci, &b->ivq, VBALLOON_VQ_INFLATE);
  if (err)
    return err;
  err = virtio_queue_setup (&b->pci, &b->dvq, VBALLOON_VQ_DEFLATE);
  if (err)
    return err;

  err = virtio_dma_alloc (&b->pci, VBALLOON_BATCH_MAX * 4,
                          (void **) &b->ivq_pfns, &b->ivq_pfns_phys);
  if (err)
    return err;
  err = virtio_dma_alloc (&b->pci, VBALLOON_BATCH_MAX * 4,
                          (void **) &b->dvq_pfns, &b->dvq_pfns_phys);
  if (err)
    return err;

  err = virtio_pci_driver_ok (&b->pci);
  if (err)
    return err;

  /* Fresh start: nothing is ballooned, and the device must see actual == 0
     before later updates can be trusted.  */
  vballoon_dev_set_actual (b, 0);
  if (virtio_pci_read_config (&b->pci, VBALLOON_CFG_ACTUAL) != 0)
    {
      fprintf (stderr, "virtio-balloon: actual readback is not 0\n");
      return EIO;
    }
  b->actual = 0;
  b->target = virtio_pci_read_config (&b->pci, VBALLOON_CFG_NUM_PAGES);
  fprintf (stderr, "virtio-balloon: initial target %u pages, actual %u\n",
           b->target, b->actual);

  queues[0] = &b->ivq;
  queues[1] = &b->dvq;
  return virtio_irq_install (&b->irq, &b->pci, queues, 2);
}

error_t
vballoon_dev_init (struct vballoon *b, const char *override)
{
  error_t err = vballoon_dev_bring_up (b, override);

  if (err)
    /* Same rule as blk/net: a failed bring-up must not leave the device
       configured; the reset disarms the queues before the PFN arrays
       (driver DMA memory) go away with the dying task.  */
    virtio_pci_reset (&b->pci);
  return err;
}

uint32_t
vballoon_dev_read_target (struct vballoon *b)
{
  return virtio_pci_read_config (&b->pci, VBALLOON_CFG_NUM_PAGES);
}

void
vballoon_dev_set_actual (struct vballoon *b, uint32_t actual)
{
  virtio_pci_write_config (&b->pci, VBALLOON_CFG_ACTUAL, actual);
}

/* Post one batch of PFNs on the inflate queue and account for it only
   after the used ring acknowledges it.  *GOT is the number of pages the
   pool could claim, which may be less than WANT.  */
error_t
vballoon_dev_inflate (struct vballoon *b, uint32_t want, uint32_t *got)
{
  struct virtio_buf buf;
  struct virtio_used_elem used;
  uint32_t count;
  error_t err;

  if (want > VBALLOON_BATCH_MAX)
    want = VBALLOON_BATCH_MAX;

  err = vballoon_mem_grab (&b->pool, want, b->ivq_pfns, &count);
  if (err)
    return err;
  *got = count;
  if (count == 0)
    {
      fprintf (stderr, "virtio-balloon: out of puff: cannot claim %u pages\n",
               want);
      return 0;
    }

  put_pfns (b->ivq_pfns, b->ivq_pfns, count);

  buf.addr = b->ivq_pfns_phys;
  buf.len = count * 4;
  buf.flags = 0;
  err = virtio_queue_post (&b->ivq, &buf, 1);
  if (err)
    return err;
  err = virtio_queue_wait_used (&b->ivq, VBALLOON_TIMEOUT, &used);
  if (err)
    {
      fprintf (stderr, "virtio-balloon: inflate wait failed: %s\n",
               strerror (err));
      return err;
    }

  b->actual += count;
  vballoon_dev_set_actual (b, b->actual);
  if (vballoon_verbose)
    fprintf (stderr,
             "virtio-balloon: inflated %u pages (actual %u, used len %u)\n",
             count, b->actual, used.len);
  return 0;
}

/* Post one batch of held PFNs on the deflate queue.  The pages stay mapped
   until the ack, then and only then go back to Mach: the host is always
   told before the pages are reusable, which is what MUST_TELL_HOST
   demands.  *GONE is the number of pages actually selected, which may be
   less than WANT.  */
error_t
vballoon_dev_deflate (struct vballoon *b, uint32_t want, uint32_t *gone)
{
  struct virtio_buf buf;
  struct virtio_used_elem used;
  uint32_t count;
  error_t err;

  if (want > VBALLOON_BATCH_MAX)
    want = VBALLOON_BATCH_MAX;

  err = vballoon_mem_release_begin (&b->pool, want, b->dvq_pfns, &count);
  if (err)
    return err;
  *gone = count;
  if (count == 0)
    return 0;

  put_pfns (b->dvq_pfns, b->dvq_pfns, count);

  buf.addr = b->dvq_pfns_phys;
  buf.len = count * 4;
  buf.flags = 0;
  err = virtio_queue_post (&b->dvq, &buf, 1);
  if (err)
    return err;
  err = virtio_queue_wait_used (&b->dvq, VBALLOON_TIMEOUT, &used);
  if (err)
    {
      fprintf (stderr, "virtio-balloon: deflate wait failed: %s\n",
               strerror (err));
      return err;
    }

  vballoon_mem_release_commit (&b->pool);
  b->actual -= count;
  vballoon_dev_set_actual (b, b->actual);
  if (vballoon_verbose)
    fprintf (stderr,
             "virtio-balloon: deflated %u pages (actual %u, used len %u)\n",
             count, b->actual, used.len);
  return 0;
}

/* Best-effort shutdown deflation: tell the host about every held page and
   wait for each ack before the pages go back to Mach.  Errors are logged
   and ignored; teardown resets the device regardless.  */
void
vballoon_dev_release_all (struct vballoon *b)
{
  while (vballoon_pool_held (&b->pool) > 0)
    {
      uint32_t gone;
      error_t err = vballoon_dev_deflate (b, ~0u, &gone);
      if (err || gone == 0)
        {
          fprintf (stderr, "virtio-balloon: shutdown release incomplete "
                   "(held %u)\n", vballoon_pool_held (&b->pool));
          break;
        }
    }
}

void
vballoon_dev_quiesce (struct vballoon *b)
{
  virtio_irq_uninstall (&b->irq);
}
