/* virtio-net packet buffer pools and the RX service loop.

   RX: one contiguous DMA region of `hdr + frame cap` buffers, all posted
   to receiveq as single F_WRITE descriptors at init and replenished after
   every completion (plan §4.3/§6.4).  One buffer per frame is enough
   without MRG_RXBUF: QEMU refuses to deliver a frame that does not fit
   the posted buffer, which the length validation below counts as a drop.

   TX: one contiguous DMA region of `hdr + frame cap` slots.  Submission
   is serialized under tx_lock with one chain in flight; a slot whose
   completion timed out stays marked so the device can never race a
   reused buffer, exactly the vblk_req_one discipline.

   Completion mapping: the used entry's id is the chain's head descriptor
   index; desc2slot/head2slot tables translate it back to a pool slot, so
   a foreign or stale id is counted as a bad completion instead of being
   dereferenced.  */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "virtio-net.h"

/* The header the device reads/writes at buffer start: zeroed for
   no-offload TX (flags=0, GSO_NONE), validated as present for RX.  */
void
vnet_hdr_zero (char *buf)
{
  memset (buf, 0, VNET_HDR_SIZE);
}

error_t
vnet_buf_init (struct vnet_dev *d)
{
  size_t rx_region;
  uint16_t n, i;
  error_t err;

  /* Buffer geometry fixed at init from the negotiated config: RX buffers
     carry the frame cap (plan RX_FRAME_CAP) bounded by the conservative
     2048-byte total, TX slots carry the same cap.  */
  if (d->rx_buf_size == 0)
    d->rx_buf_size = VNET_HDR_SIZE + VNET_FRAME_CAP;
  if (d->rx_buf_size > VNET_RX_BUF_MAX)
    d->rx_buf_size = VNET_RX_BUF_MAX;

  /* Clamp the RX pool to the device-reported queue depth: a pool larger
     than a queue would strand slots that can never be posted.  The TX
     pool clamps itself inside virtio_pool_init.  */
  d->rx_nslots = d->rxq.num < VNET_RX_SLOTS ? d->rxq.num : VNET_RX_SLOTS;

  rx_region = (size_t) d->rx_nslots * d->rx_buf_size;
  err = virtio_dma_alloc (&d->pci, rx_region, (void **) &d->rx_bufs,
                          &d->rx_bufs_phys);
  if (err)
    {
      fprintf (stderr, "virtio-net: cannot allocate RX pool (%zu)\n",
               rx_region);
      return ENOMEM;
    }
  memset (d->rx_bufs, 0, rx_region);

  err = virtio_pool_init (&d->txpool, &d->pci, "virtio-net", &vnet_verbose,
                          VNET_TX_SLOTS, VNET_HDR_SIZE + VNET_FRAME_CAP,
                          d->txq.num);
  if (err)
    {
      vm_deallocate (mach_task_self (), (vm_address_t) d->rx_bufs,
                     (vm_size_t) rx_region);
      d->rx_bufs = NULL;
      return err;
    }

  d->rx_desc2slot = malloc ((size_t) d->rxq.num * sizeof (uint16_t));
  if (!d->rx_desc2slot)
    {
      vnet_buf_release (d);
      return ENOMEM;
    }
  for (i = 0; i < d->rxq.num; i++)
    d->rx_desc2slot[i] = VNET_NO_SLOT;
  d->rx_nretired = 0;
  pthread_mutex_init (&d->tx_lock, NULL);

  /* Prefill the receive queue: every slot becomes one writable
     descriptor.  The head-descriptor map is recorded before each post,
     the way the serialized blk engine does.  */
  for (n = 0; n < d->rx_nslots; n++)
    {
      struct virtio_buf buf;
      uint16_t head = d->rxq.free_head;

      buf.addr = d->rx_bufs_phys + (uint64_t) n * d->rx_buf_size;
      buf.len = d->rx_buf_size;
      buf.flags = VIRTQ_DESC_F_WRITE;
      err = virtio_queue_post (&d->rxq, &buf, 1);
      if (err)
        {
          fprintf (stderr, "virtio-net: RX prefill stalled at %u/%u: %s\n",
                   n, d->rx_nslots, strerror (err));
          goto fail;
        }
      d->rx_desc2slot[head] = n;
    }
  fprintf (stderr, "virtio-net: %u RX buffers of %zu bytes posted, "
           "%u TX slots\n", d->rx_nslots, d->rx_buf_size, d->txpool.nslots);
  return 0;
fail:
  vnet_buf_release (d);
  return err;
}

void
vnet_buf_release (struct vnet_dev *d)
{
  free (d->rx_desc2slot);
  d->rx_desc2slot = NULL;
  virtio_pool_release (&d->txpool);
  if (d->rx_bufs)
    {
      vm_deallocate (mach_task_self (), (vm_address_t) d->rx_bufs,
                     (vm_size_t) d->rx_nslots * d->rx_buf_size);
      d->rx_bufs = NULL;
    }
}

/* Consume one RX completion, validate it against the pool, and hand the
   Ethernet frame (after the virtio-net header) to the Hurd layer.  The
   buffer is always reposted, on every path, so the queue never drains.  */
static void
vnet_rx_one (struct vnet_dev *d, struct virtio_used_elem *used)
{
  struct virtio_buf buf;
  uint16_t slot;

  if (used->id >= d->rxq.num
      || d->rx_desc2slot[used->id] == VNET_NO_SLOT)
    {
      fprintf (stderr, "virtio-net: RX completion id %u (len %u) is not a "
               "posted buffer\n", used->id, used->len);
      d->stats.bad_completions++;
      return;
    }

  slot = d->rx_desc2slot[used->id];
  d->rx_desc2slot[used->id] = VNET_NO_SLOT;

  /* QEMU writes used.len = bytes written (header + frame); anything else
     is malformed and dropped, but the buffer still goes back to the
     queue (plan §4.3 completion validation).  */
  if (used->len < VNET_HDR_SIZE || used->len > d->rx_buf_size)
    {
      if (vnet_verbose)
        fprintf (stderr, "virtio-net: RX completion len %u out of range, "
                 "dropped\n", used->len);
      d->stats.rx_bad++;
      goto repost;
    }

  {
    size_t frame_len = used->len - VNET_HDR_SIZE;

    if (frame_len < VNET_ETHER_HDR_SIZE)
      {
        if (vnet_verbose)
          fprintf (stderr, "virtio-net: RX frame of %zu bytes too short, "
                   "dropped\n", frame_len);
        d->stats.rx_bad++;
        goto repost;
      }

    d->stats.rx_packets++;
    d->stats.rx_bytes += frame_len;
    vnet_hurd_rx_all (d,
                      d->rx_bufs + (size_t) slot * d->rx_buf_size
                      + VNET_HDR_SIZE,
                      frame_len);
  }

 repost:
  {
    int retries;
    uint16_t post_head = d->rxq.free_head;

    buf.addr = d->rx_bufs_phys + (uint64_t) slot * d->rx_buf_size;
    buf.len = d->rx_buf_size;
    buf.flags = VIRTQ_DESC_F_WRITE;
    for (retries = 0; retries < VNET_RX_REPLENISH_RETRIES; retries++)
      {
        post_head = d->rxq.free_head;
        if (virtio_queue_post_head (&d->rxq, &buf, 1, &post_head) == 0)
          break;
      }
    if (retries >= VNET_RX_REPLENISH_RETRIES)
      {
        /* Persistent replenish failure: record the slot for recovery and
           continue with reduced RX capacity until it can be reposted.  */
        fprintf (stderr, "virtio-net: RX replenish of slot %u failed "
                 "after %d retries; deferring slot\n", slot, retries);
        d->stats.rx_bad++;
        if (d->rx_nretired < VNET_RX_SLOTS)
          {
            uint16_t k;
            int dup = 0;
            for (k = 0; k < d->rx_nretired; k++)
              if (d->rx_retired[k] == slot)
                { dup = 1; break; }
            if (!dup)
              d->rx_retired[d->rx_nretired++] = slot;
          }
        return;
      }
    d->rx_desc2slot[post_head] = slot;
  }
}

/* Try to repost any RX slots that failed replenishment earlier.  */
static void
vnet_rx_recover_retired (struct vnet_dev *d)
{
  uint16_t i, j = 0;

  for (i = 0; i < d->rx_nretired; i++)
    {
      uint16_t slot = d->rx_retired[i];
      struct virtio_buf buf;
      uint16_t post_head;
      int retries, ok = 0;

      buf.addr = d->rx_bufs_phys + (uint64_t) slot * d->rx_buf_size;
      buf.len = d->rx_buf_size;
      buf.flags = VIRTQ_DESC_F_WRITE;
      for (retries = 0; retries < VNET_RX_REPLENISH_RETRIES; retries++)
        {
          post_head = d->rxq.free_head;
          if (virtio_queue_post_head (&d->rxq, &buf, 1, &post_head) == 0)
            {
              ok = 1;
              break;
            }
        }
      if (ok)
        d->rx_desc2slot[post_head] = slot;
      else
        d->rx_retired[j++] = slot;
    }
  d->rx_nretired = j;
}

/* RX service loop: wait for completions, deliver, replenish.  Runs on its
   own thread; the periodic timeout is the lost-interrupt fallback that
   also provides the teardown poll interval.  */
void
vnet_buf_rx_loop (struct vnet_dev *d)
{
  struct virtio_used_elem used;
  error_t err;

  /* Atomic load: stopping is written by the teardown thread (quiesce
     or the daemon stop path) while this loop runs.  */
  while (!__atomic_load_n (&d->stopping, __ATOMIC_SEQ_CST))
    {
      vnet_rx_recover_retired (d);
      err = virtio_queue_wait_used (&d->rxq, 1, &used);
      if (err == ETIMEDOUT)
        continue;
      if (err)
        {
          fprintf (stderr, "virtio-net: RX wait failed: %s\n",
                   strerror (err));
          d->stats.bad_completions++;
          continue;
        }
      vnet_rx_one (d, &used);
      /* Batch-drain any additional completions already available.  */
      for (;;)
        {
          err = virtio_queue_try_pop_used (&d->rxq, &used);
          if (err == EAGAIN)
            break;
          if (err)
            {
              if (err == EIO)
                d->stats.bad_completions++;
              else
                {
                  fprintf (stderr, "virtio-net: RX pop failed: %s\n",
                           strerror (err));
                  d->stats.rx_bad++;
                }
              break;
            }
          vnet_rx_one (d, &used);
        }
      vnet_rx_recover_retired (d);
    }
}
