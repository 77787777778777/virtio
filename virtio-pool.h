/* virtio-pool.h: shared DMA slot-pool engine for single-request-queue
   virtio drivers.

   A pool is one contiguous DMA region carved into fixed-size slots, plus
   the two tables that make completions safe: slot_used (which slots are
   handed out) and head2slot (which descriptor head carries which slot,
   indexed by the used entry's id).  The post-and-wait path implements the
   stale-completion discipline both drivers rely on: a chain whose wait
   timed out keeps its slot marked, so the device can never race a reused
   buffer, and its used entry is consumed and discarded by whichever
   request waits next (plan blk §4.2 / net §4.3 completion validation).

   The pool takes no locks of its own: net serializes TX with the dev
   struct's tx_lock, blk is single-threaded per the MVP.  */

#ifndef VIRTIO_POOL_H
#define VIRTIO_POOL_H

#include <stdint.h>

#include "virtio-pci.h"
#include "virtio-queue.h"

/* Sentinel for "no slot"; valid slots are below the clamped count.  */
#define VIRTIO_POOL_NO_SLOT 0xffff

struct virtio_slot_pool
{
  char *slots;              /* one contiguous DMA region */
  uint64_t slots_phys;
  size_t slot_size;
  uint16_t nslots;          /* usable slots, clamped to the queue num */
  int *slot_used;
  uint16_t *head2slot;      /* indexed by descriptor head (q->num) */
  const char *name;         /* driver name for diagnostics */
  const int *verbose;       /* driver's verbose flag */
};

error_t virtio_pool_init (struct virtio_slot_pool *p,
                          struct virtio_pci_dev *pci,
                          const char *name, const int *verbose,
                          uint16_t max_slots, size_t slot_size,
                          uint16_t queue_num);
void virtio_pool_release (struct virtio_slot_pool *p);

/* Find a free slot: 0 with *SLOT set, or EAGAIN.  */
error_t virtio_pool_alloc_slot (struct virtio_slot_pool *p, uint16_t *slot);

static inline char *
virtio_pool_slot (struct virtio_slot_pool *p, uint16_t slot)
{
  return p->slots + (size_t) slot * p->slot_size;
}

static inline uint64_t
virtio_pool_slot_phys (struct virtio_slot_pool *p, uint16_t slot)
{
  return p->slots_phys + (uint64_t) slot * p->slot_size;
}

/* Mark SLOT used, post BUFS as one chain, and await this chain's
   completion, discarding completions of chains that timed out earlier.
   On any error the slot stays marked (the chain may still be
   device-owned); EIO means a completion that is not a posted chain.  On
   success the marks are cleared and *USED is this chain's completion.  */
error_t virtio_pool_post_wait (struct virtio_slot_pool *p,
                               struct virtio_queue *q, uint16_t slot,
                               const struct virtio_buf *bufs,
                               uint16_t nbufs, int timeout_sec,
                               struct virtio_used_elem *used);

#endif /* VIRTIO_POOL_H */
