#ifndef VIRTIO_QUEUE_H
#define VIRTIO_QUEUE_H

/* Split virtqueues over the generic legacy PCI transport.

   Each queue's vring lives in one physically contiguous, page-granular
   region (the legacy QUEUE_PFN register addresses the whole ring by page
   number, so the allocation must be page aligned), and completions are
   signalled per queue: the IRQ handler broadcasts every queue's condvar
   and a separate config-change condvar, and waiters re-check the used
   index instead of trusting the wakeup, the way virtio-9p always has.

   Descriptors come from a free list, so any number of chains may be in
   flight; both current drivers keep one buffer per queue outstanding and
   consume completions in order.  */

#include <stdint.h>
#include <stddef.h>
#include <pthread.h>
#include <hurd/irqhelp.h>

#include "virtio-pci.h"

/* Sanity bound on queue size; QEMU's virtio queues are far below this.  */
#define VIRTIO_QUEUE_MAX 1024
#define VIRTIO_QUEUE_ALIGN 4096
/* Sentinel for "no descriptor"; valid indices are below VIRTIO_QUEUE_MAX.  */
#define VIRTIO_QUEUE_NO_DESC 0xffff

/* One buffer posted to a virtqueue: one descriptor.  */
struct virtio_buf {
  uint64_t addr;
  uint32_t len;
  uint16_t flags;
};

struct virtio_desc {
  uint64_t addr;
  uint32_t len;
  uint16_t flags;
  uint16_t next;
};

struct virtio_used_elem {
  uint32_t id;
  uint32_t len;
};

struct virtio_queue {
  struct virtio_pci_dev *pci;
  uint16_t index;       /* QUEUE_SEL index on the device */
  uint16_t num;
  struct virtio_desc *desc;
  volatile uint16_t *avail_idx;
  uint16_t *avail_ring;
  volatile uint16_t *used_idx;
  struct virtio_used_elem *used_ring;
  uint16_t last_used;
  uint16_t free_head;
  pthread_mutex_t lock;
  pthread_cond_t cv;
  uint64_t region_phys;
};

#define VIRTIO_IRQ_MAX_QUEUES 8

struct virtio_irq {
  struct virtio_pci_dev *pci;
  struct irq *irq;
  struct virtio_queue *queues[VIRTIO_IRQ_MAX_QUEUES];
  size_t nqueues;
  pthread_mutex_t lock;
  pthread_cond_t cv;
  /* Bumped on every config-change interrupt; a waiter compares it against
     the value it last saw.  */
  uint32_t config_seq;
};

/* Contiguous, page-granular DMA memory; the physical address returned is
   the machine address the device DMAes to.  */
error_t virtio_dma_alloc (struct virtio_pci_dev *, size_t size, void **vaddr,
                          uint64_t *paddr);

/* Program one queue (QUEUE_SEL/NUM/PFN) and map its vring.  */
error_t virtio_queue_setup (struct virtio_pci_dev *, struct virtio_queue *,
                            uint16_t index);

/* Post a chain of buffers as one buffer and notify the device.  The first
   descriptor is the head; the core links the chain with F_NEXT.  */
error_t virtio_queue_post (struct virtio_queue *, const struct virtio_buf *,
                           uint16_t nbufs);

/* Wait for one used entry and return it; the completed descriptor chain is
   recycled.  TIMEOUT_SEC bounds the interrupt wait (the fallback for a lost
   interrupt, not the normal completion path).  */
error_t virtio_queue_wait_used (struct virtio_queue *, int timeout_sec,
                                struct virtio_used_elem *);

/* Install/remove the INTx handler (libirqhelp).  Removal resets the device
   first (stopping new INTx asserts and clearing the ISR), then drops the
   handler, mirroring the NetBSD/Linux teardown order.  */
error_t virtio_irq_install (struct virtio_irq *, struct virtio_pci_dev *,
                            struct virtio_queue **queues, size_t nqueues);
void virtio_irq_uninstall (struct virtio_irq *);

/* Block until the config-change sequence number differs from *SEQ, or the
   timeout elapses; on success *SEQ holds the new value.  */
error_t virtio_irq_wait_config (struct virtio_irq *, int timeout_sec,
                                uint32_t *seq);

#endif /* VIRTIO_QUEUE_H */
