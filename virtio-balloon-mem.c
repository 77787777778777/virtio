/* Balloon page pool: the PFNs exchanged with the host must be pages the
   driver owns and can withhold from guest use, so the backing is Mach VM
   memory taken through the same privileged-host-port path as the vring DMA
   buffers.  vm_allocate_contiguous returns the machine physical address of
   a power-of-two block, which is exactly the PFN the balloon protocol
   wants; inflating claims pages from chunks, deflating selects them, and a
   chunk is unmapped only once its last page has been acknowledged back.

   The pool is touched only by the single convergence thread, but the lock
   is kept because the accounting is shared state and the discipline costs
   nothing.  */

#include <errno.h>
#include <mach.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "virtio-balloon.h"

static error_t
vballoon_chunk_alloc (struct vballoon_pool *p)
{
  struct vballoon_chunk *c;
  void *vaddr;
  uint64_t paddr;
  error_t err;

  if (p->nchunks == p->chunks_cap)
    {
      size_t cap = p->chunks_cap ? p->chunks_cap * 2 : 8;
      struct vballoon_chunk *chunks =
        realloc (p->chunks, cap * sizeof *chunks);
      if (!chunks)
        return ENOMEM;
      p->chunks = chunks;
      p->chunks_cap = cap;
    }

  err = virtio_dma_alloc (p->pci,
                          (size_t) VBALLOON_CHUNK_PAGES << VBALLOON_PFN_SHIFT,
                          &vaddr, &paddr);
  if (err)
    return err;

  c = &p->chunks[p->nchunks++];
  c->vaddr = (vm_address_t) vaddr;
  c->paddr = paddr;
  c->total = VBALLOON_CHUNK_PAGES;
  c->held = 0;
  c->pending = 0;
  return 0;
}

void
vballoon_mem_init (struct vballoon_pool *p, struct virtio_pci_dev *pci)
{
  memset (p, 0, sizeof *p);
  pthread_mutex_init (&p->lock, NULL);
  p->pci = pci;
}

uint32_t
vballoon_pool_held (struct vballoon_pool *p)
{
  uint32_t held;

  pthread_mutex_lock (&p->lock);
  held = p->held;
  pthread_mutex_unlock (&p->lock);
  return held;
}

/* Claim up to WANT pages for the balloon and return their PFNs.  Pages are
   claimed [held, held+k) within each chunk so inflate and deflate meet in
   the middle and a chunk is unmapped only when both counters reach zero.
   Claiming here cannot overstate `actual`: the caller reports pages only
   after the used ring acknowledges the batch carrying them.  */
error_t
vballoon_mem_grab (struct vballoon_pool *p, uint32_t want, uint32_t *pfns,
                   uint32_t *count)
{
  *count = 0;
  if (want == 0)
    return 0;

  pthread_mutex_lock (&p->lock);
  while (*count < want)
    {
      struct vballoon_chunk *c = NULL;
      uint32_t room, take, i;
      size_t k;

      for (k = 0; k < p->nchunks; k++)
        if (p->chunks[k].held < p->chunks[k].total)
          {
            c = &p->chunks[k];
            break;
          }
      if (!c)
        {
          error_t err = vballoon_chunk_alloc (p);
          if (err)
            /* Partial batches are legal: the caller posts what it got.  */
            break;
          c = &p->chunks[p->nchunks - 1];
        }

      room = c->total - c->held;
      take = (want - *count < room) ? want - *count : room;
      for (i = 0; i < take; i++)
        pfns[(*count)++] =
          (uint32_t) ((c->paddr >> VBALLOON_PFN_SHIFT) + c->held + i);
      c->held += take;
      p->held += take;
    }
  pthread_mutex_unlock (&p->lock);
  return 0;
}

/* Select up to WANT held pages for return and mark them pending: they stay
   mapped until the deflate queue acknowledges the posting, which is the
   MUST_TELL_HOST ordering (the host is told before the pages are usable
   again).  */
error_t
vballoon_mem_release_begin (struct vballoon_pool *p, uint32_t want,
                            uint32_t *pfns, uint32_t *count)
{
  *count = 0;
  if (want == 0)
    return 0;

  pthread_mutex_lock (&p->lock);
  {
    size_t k = p->nchunks;
    while (k > 0 && *count < want)
      {
        struct vballoon_chunk *c = &p->chunks[k - 1];
        uint32_t take, i;

        k--;
        if (c->held == 0)
          continue;
        take = (want - *count < c->held) ? want - *count : c->held;
        for (i = 0; i < take; i++)
          pfns[(*count)++] =
            (uint32_t) ((c->paddr >> VBALLOON_PFN_SHIFT)
                        + c->held - 1 - i);
        c->held -= take;
        c->pending += take;
        p->held -= take;
      }
  }
  pthread_mutex_unlock (&p->lock);
  return 0;
}

/* Called only after the deflate posting has been acknowledged: the pages
   are back in guest use, so their chunks can go.  The single-outstanding
   batch per queue means pending always describes exactly the acked set.  */
void
vballoon_mem_release_commit (struct vballoon_pool *p)
{
  pthread_mutex_lock (&p->lock);
  {
    size_t k = 0;
    while (k < p->nchunks)
      {
        struct vballoon_chunk *c = &p->chunks[k];
        c->pending = 0;
        if (c->held == 0)
          {
            vm_deallocate (mach_task_self (), c->vaddr,
                           (vm_size_t) c->total << VBALLOON_PFN_SHIFT);
            p->chunks[k] = p->chunks[--p->nchunks];
            continue;
          }
        k++;
      }
  }
  pthread_mutex_unlock (&p->lock);
}
