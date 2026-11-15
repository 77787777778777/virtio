#ifndef VIRTIO_BALLOON_H
#define VIRTIO_BALLOON_H

/* virtio-balloon driver for GNU/Hurd.

   The device model is the classic inflate/deflate pair: the host writes a
   target page count into the device config, and the guest posts arrays of
   little-endian PFNs (4 KiB units) on the inflate queue to hand pages to
   the host and on the deflate queue to take them back.  PFN buffers are
   device-readable output descriptors (no WRITE flag, no reply buffer),
   unlike the 9P request/response exchange.

   Two invariants fix the accounting: `actual` and the pool's held count
   advance only after the used ring has acknowledged the batch that moved
   the pages, and pages selected for deflation stay mapped until that ack,
   which is the MUST_TELL_HOST ordering the deflate queue exists for.  */

#include <stdint.h>
#include <stddef.h>
#include <pthread.h>
#include <mach.h>

#include "virtio-pci.h"
#include "virtio-queue.h"

/* PCI device 0x1af4:0x1002 (virtio device ID 5, balloon).  */
#define VBALLOON_DEVICE_ID 0x1002

/* Feature bits.  The MVP implements the classic inflate/deflate protocol
   only: no stats vq, no free-page hint/reporting, no page poison, no
   deflate-on-OOM, so none of those bits is accepted and none of those
   queues is created or serviced.  MUST_TELL_HOST is accepted when offered
   because the deflate path always posts the PFNs and waits for the ack
   before the pages leave driver ownership, which is exactly the ordering
   the bit requires.  */
#define VBALLOON_F_MUST_TELL_HOST 0

/* Config region fields, little-endian, at VIRTIO_PCI_CONFIG + off.  Only
   num_pages and actual exist without the advanced features.  */
#define VBALLOON_CFG_NUM_PAGES 0
#define VBALLOON_CFG_ACTUAL    4

/* Queue roles.  QEMU realizes inflateq/deflateq at indices 0/1 with 128
   entries; the indices are bound by role here, not hard-coded deeper in
   the transport.  */
#define VBALLOON_VQ_INFLATE 0
#define VBALLOON_VQ_DEFLATE 1

/* Batching: one posting is one descriptor carrying an array of PFNs, so
   the batch is bounded by the DMA buffer, not the ring.  Chunks are what
   one vm_allocate_contiguous call hands out.  */
#define VBALLOON_PFN_SHIFT   12
#define VBALLOON_BATCH_MAX   64
#define VBALLOON_CHUNK_PAGES 16

/* Convergence loop tick, and the per-batch completion bound (the fallback
   for a lost interrupt, not the normal completion path).  */
#define VBALLOON_TICK_SEC 1
#define VBALLOON_TIMEOUT  30

/* One physically contiguous, driver-owned backing allocation.  Pages
   [0, held) are in the balloon; [held, held+pending) have been posted on
   the deflate queue and stay mapped until the used-ring ack returns them
   to Mach.  */
struct vballoon_chunk {
  vm_address_t vaddr;
  uint64_t paddr;
  uint32_t total;
  uint32_t held;
  uint32_t pending;
};

struct vballoon_pool {
  struct virtio_pci_dev *pci;
  struct vballoon_chunk *chunks;
  size_t nchunks, chunks_cap;
  uint32_t held;
  pthread_mutex_t lock;
};

struct vballoon {
  struct virtio_pci_dev pci;
  struct virtio_queue ivq, dvq;
  struct virtio_irq irq;

  uint32_t target;     /* num_pages from the device config */
  uint32_t actual;     /* pages reported in the config */

  /* PFN batch buffers, one per queue, in DMA memory.  */
  uint32_t *ivq_pfns;
  uint64_t ivq_pfns_phys;
  uint32_t *dvq_pfns;
  uint64_t dvq_pfns_phys;

  struct vballoon_pool pool;
};

extern int vballoon_verbose;

/* virtio-balloon.c */
error_t vballoon_start_device (const char *override);
void vballoon_stop_device (void);
error_t vballoon_selftest (void);
error_t vballoon_converge (void);

/* virtio-balloon-dev.c */
error_t vballoon_dev_init (struct vballoon *, const char *override);
uint32_t vballoon_dev_read_target (struct vballoon *);
void vballoon_dev_set_actual (struct vballoon *, uint32_t);
error_t vballoon_dev_inflate (struct vballoon *, uint32_t want, uint32_t *got);
error_t vballoon_dev_deflate (struct vballoon *, uint32_t want,
                              uint32_t *gone);
void vballoon_dev_release_all (struct vballoon *);
void vballoon_dev_quiesce (struct vballoon *);

/* virtio-balloon-mem.c */
void vballoon_mem_init (struct vballoon_pool *, struct virtio_pci_dev *);
uint32_t vballoon_pool_held (struct vballoon_pool *);
error_t vballoon_mem_grab (struct vballoon_pool *, uint32_t want,
                           uint32_t *pfns, uint32_t *count);
error_t vballoon_mem_release_begin (struct vballoon_pool *, uint32_t want,
                                    uint32_t *pfns, uint32_t *count);
void vballoon_mem_release_commit (struct vballoon_pool *);

#endif /* VIRTIO_BALLOON_H */
