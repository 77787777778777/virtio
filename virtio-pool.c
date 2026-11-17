/* virtio-pool.c: the shared DMA slot-pool engine declared in
   virtio-pool.h.  See the header for the locking and stale-completion
   contract.  */

#include <errno.h>
#include <mach.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "virtio-pool.h"

error_t
virtio_pool_init (struct virtio_slot_pool *p,
                  struct virtio_pci_dev *pci,
                  const char *name, const int *verbose,
                  uint16_t max_slots, size_t slot_size,
                  uint16_t queue_num)
{
  size_t region;
  uint16_t i;
  error_t err;

  p->name = name;
  p->verbose = verbose;
  p->slot_size = slot_size;
  /* Clamp the pool to the device-reported queue depth: a pool larger
     than a queue would strand slots that can never be posted.  */
  p->nslots = queue_num < max_slots ? queue_num : max_slots;

  region = (size_t) p->nslots * slot_size;
  err = virtio_dma_alloc (pci, region, (void **) &p->slots, &p->slots_phys);
  if (err)
    {
      fprintf (stderr, "%s: cannot allocate slot pool (%zu)\n", name,
               region);
      return ENOMEM;
    }
  memset (p->slots, 0, region);

  p->head2slot = malloc ((size_t) queue_num * sizeof (uint16_t));
  p->slot_used = malloc ((size_t) p->nslots * sizeof (int));
  if (!p->head2slot || !p->slot_used)
    {
      virtio_pool_release (p);
      return ENOMEM;
    }
  for (i = 0; i < queue_num; i++)
    p->head2slot[i] = VIRTIO_POOL_NO_SLOT;
  for (i = 0; i < p->nslots; i++)
    p->slot_used[i] = 0;
  return 0;
}

void
virtio_pool_release (struct virtio_slot_pool *p)
{
  free (p->head2slot);
  p->head2slot = NULL;
  free (p->slot_used);
  p->slot_used = NULL;
  if (p->slots)
    {
      vm_deallocate (mach_task_self (), (vm_address_t) p->slots,
                     (vm_size_t) p->nslots * p->slot_size);
      p->slots = NULL;
    }
}

error_t
virtio_pool_alloc_slot (struct virtio_slot_pool *p, uint16_t *slot)
{
  uint16_t i;

  for (i = 0; i < p->nslots; i++)
    if (!p->slot_used[i])
      {
        *slot = i;
        return 0;
      }
  return EAGAIN;
}

error_t
virtio_pool_post_wait (struct virtio_slot_pool *p,
                       struct virtio_queue *q, uint16_t slot,
                       const struct virtio_buf *bufs,
                       uint16_t nbufs, int timeout_sec,
                       struct virtio_used_elem *used)
{
  uint16_t head;
  error_t err;

  /* The head descriptor index is what the device echoes back as the used
     entry's id, so record the chain's head before posting it.  */
  head = q->free_head;
  err = virtio_queue_post (q, bufs, nbufs);
  if (err)
    return err;
  p->head2slot[head] = slot;
  p->slot_used[slot] = 1;

  for (;;)
    {
      err = virtio_queue_wait_used (q, timeout_sec, used);
      if (err)
        {
          /* The chain is still device-owned; leave the slot marked so a
             reused buffer cannot race the device.  When the entry
             finally lands, whichever request is waiting next consumes
             and discards it below; if it never lands, the pool exhausts
             its slots and fails loudly instead of corrupting a live
             request.  */
          fprintf (stderr, "%s: completion wait failed: %s\n", p->name,
                   strerror (err));
          return err;
        }

      /* Completion validation: the id must name a descriptor head of a
         chain this driver posted.  */
      if (p->head2slot[used->id] == VIRTIO_POOL_NO_SLOT)
        {
          fprintf (stderr, "%s: completion id %u (len %u) is not a "
                   "posted chain\n", p->name, used->id, used->len);
          return EIO;
        }
      /* Entry of a chain that timed out earlier: drop it and keep
         waiting for this request's own completion.  */
      if (p->head2slot[used->id] != slot)
        {
          uint16_t stale = p->head2slot[used->id];

          if (p->verbose && *p->verbose)
            fprintf (stderr, "%s: discarding completion of the "
                     "timed-out request (slot %u)\n", p->name, stale);
          p->head2slot[used->id] = VIRTIO_POOL_NO_SLOT;
          p->slot_used[stale] = 0;
          continue;
        }
      break;
    }

  p->head2slot[used->id] = VIRTIO_POOL_NO_SLOT;
  p->slot_used[slot] = 0;
  return 0;
}
