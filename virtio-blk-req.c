/* virtio-blk request construction and completion.

   One request is one descriptor chain over one DMA slot of fixed layout:
   [outhdr 16 bytes (device-readable)] [data (direction per request type)]
   [status 1 byte (device-writable)].  The header and status live at fixed
   offsets, so a completion is validated without remembering the chain's
   lengths: the used entry's id must name a slot this driver posted, and
   the status is read from its fixed address.  The used ring's length is
   only bound-checked, because QEMU's virtio-blk reports the pushed length
   rather than a byte count the driver needs.

   The engine is serialized: one chain outstanding at a time, which is the
   MVP concurrency policy (plan R6).  Completion lookup is still by
   descriptor head through head2slot, so the table stays correct if
   several chains are ever posted concurrently.  A request whose wait
   times out keeps its slot marked: the chain is still device-owned, so
   the buffers cannot be reused, and its used entry is discarded by the
   next waiter instead of failing a live request.  */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "virtio-blk.h"

error_t
vblk_req_init (struct vblk_dev *d)
{
  pthread_mutex_init (&d->req_lock, NULL);
  return virtio_pool_init (&d->reqs, &d->pci, "virtio-blk", &vblk_verbose,
                           VBLK_MAX_REQ, VBLK_SLOT_SIZE, d->reqq.num);
}

void
vblk_req_release (struct vblk_dev *d)
{
  virtio_pool_release (&d->reqs);
  pthread_mutex_destroy (&d->req_lock);
}

/* Build, post, and await one chain under the dev-level request mutex:
   serve-mode RPCs arrive on concurrent libports worker threads, and the
   pool engine has no internal locking, so the one-chain-in-flight
   policy is enforced here (virtio-blk.h req_lock).  DATA_LEN 0 means
   the request type carries no data buffer (flush).  DATA is the
   caller's buffer: for a write it is copied into the slot before
   posting, for a read it is filled from the slot after the status is
   validated.  */
static error_t
vblk_req_one_locked (struct vblk_dev *d, uint32_t type, uint64_t sector,
                     uint32_t data_len, int data_writable, void *data,
                     struct virtio_used_elem *used)
{
  struct virtio_buf bufs[3];
  uint64_t base;
  char *slot;
  uint16_t slot_idx;
  int n = 0;
  error_t err;
  uint8_t status;

  err = virtio_pool_alloc_slot (&d->reqs, &slot_idx);
  if (err)
    {
      fprintf (stderr, "virtio-blk: no free request slot\n");
      return err;
    }

  base = virtio_pool_slot_phys (&d->reqs, slot_idx);
  slot = virtio_pool_slot (&d->reqs, slot_idx);

  virtio_put_le32 (slot + 0, type);
  virtio_put_le32 (slot + 4, 0); /* ioprio */
  virtio_put_le64 (slot + 8, sector);

  bufs[n].addr = base;
  bufs[n].len = VBLK_HDR_SIZE;
  bufs[n].flags = 0; /* the device reads the header */
  n++;
  if (data_len)
    {
      if (data && !data_writable)
        memcpy (slot + VBLK_HDR_SIZE, data, data_len);
      bufs[n].addr = base + VBLK_HDR_SIZE;
      bufs[n].len = data_len;
      bufs[n].flags = data_writable ? VIRTQ_DESC_F_WRITE : 0;
      n++;
    }
  bufs[n].addr = base + VBLK_HDR_SIZE + VBLK_DATA_MAX;
  bufs[n].len = 1;
  bufs[n].flags = VIRTQ_DESC_F_WRITE; /* the device writes the status */
  n++;

  /* The pool records the chain's head and marks the slot, then waits
     for this chain's completion, discarding completions of requests
     that timed out earlier.  A timeout keeps the slot marked: the chain
     is still device-owned and its buffers must not be handed to another
     request while the device may still be writing them.  If it never
     lands, the pool exhausts its slots and fails loudly instead of
     corrupting a live request.  */
  err = virtio_pool_post_wait (&d->reqs, &d->reqq, slot_idx, bufs, n,
                               VBLK_TIMEOUT, used);
  if (err)
    {
      if (err == EIO)
        d->stats.bad_completions++;
      else
        d->stats.errors++;
      return err;
    }

  /* The used length must be a possible chain length (device-written
     status plus at most the data buffer).  */
  if (used->len == 0 || used->len > VBLK_SLOT_SIZE)
    {
      fprintf (stderr, "virtio-blk: completion id %u (len %u) is not a "
              "posted chain\n", used->id, used->len);
      d->stats.bad_completions++;
      return EIO;
    }

  status = (uint8_t) slot[VBLK_HDR_SIZE + VBLK_DATA_MAX];

  if (status != VBLK_S_OK)
    {
      fprintf (stderr, "virtio-blk: %s sector %llu: device status %u\n",
               type == VBLK_T_IN ? "read" : type == VBLK_T_OUT ? "write"
               : "flush",
               (unsigned long long) sector, status);
      d->stats.errors++;
      if (status == VBLK_S_IOERR)
        return EIO;
      if (status == VBLK_S_UNSUPP)
        return EOPNOTSUPP;
      return EIO;
    }

  if (data && data_writable)
    memcpy (data, slot + VBLK_HDR_SIZE, data_len);
  return 0;
}

static error_t
vblk_req_one (struct vblk_dev *d, uint32_t type, uint64_t sector,
              uint32_t data_len, int data_writable, void *data,
              struct virtio_used_elem *used)
{
  error_t err;

  pthread_mutex_lock (&d->req_lock);
  err = vblk_req_one_locked (d, type, sector, data_len, data_writable,
                             data, used);
  pthread_mutex_unlock (&d->req_lock);
  return err;
}

/* Chunked read/write: one descriptor chain per chunk, bounded by the
   effective max transfer, keeping every chunk a whole number of the
   transfer unit (512 bytes, or blk_size when negotiated).  *XFERRED is
   the byte count actually moved, so a mid-chain failure reports the
   partial transfer the way device_read/device_write do.  */
error_t
vblk_req_rw (struct vblk_dev *d, int is_write, uint64_t sector, void *data,
             size_t len, size_t *xferred)
{
  struct virtio_used_elem used;
  size_t done = 0, chunk, unit;
  uint64_t pos = sector;
  error_t err;

  *xferred = 0;
  unit = d->blk_size ? d->blk_size : VBLK_SECTOR_SIZE;

  while (len > 0)
    {
      chunk = len < d->max_transfer ? len : d->max_transfer;
      chunk -= chunk % unit;
      if (chunk == 0)
        {
          fprintf (stderr, "virtio-blk: max transfer %u below transfer unit "
                  "%zu\n", d->max_transfer, unit);
          return EIO;
        }

      err = vblk_req_one (d, is_write ? VBLK_T_OUT : VBLK_T_IN, pos, chunk,
                          !is_write, (char *) data + done, &used);
      if (err)
        {
          *xferred = done;
          return err;
        }
      done += chunk;
      if (vblk_verbose)
        fprintf (stderr, "virtio-blk: %s sector %llu +%zu ok (used len %u)\n",
                 is_write ? "write" : "read", (unsigned long long) pos,
                 chunk, used.len);
      pos += chunk / VBLK_SECTOR_SIZE;
      len -= chunk;
    }
  *xferred = done;

  if (is_write)
    {
      d->stats.writes++;
      d->stats.write_bytes += done;
    }
  else
    {
      d->stats.reads++;
      d->stats.read_bytes += done;
    }
  return 0;
}

error_t
vblk_req_flush (struct vblk_dev *d)
{
  struct virtio_used_elem used;
  error_t err;

  err = vblk_req_one (d, VBLK_T_FLUSH, 0, 0, 0, NULL, &used);
  if (err)
    return err;
  d->stats.flushes++;
  return 0;
}
