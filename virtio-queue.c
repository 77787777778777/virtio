/* Generic split virtqueue and IRQ core.

   The vring lives in one physically contiguous, page-granular region: the
   legacy QUEUE_PFN register addresses the whole vring by physical page
   number, so the allocation must be page aligned.  vm_allocate_contiguous
   returns both the virtual and the machine physical address (the DMA32 pool
   below 4 GiB when pmax is 4 GiB), so the physical address goes straight
   into the descriptors and into QUEUE_PFN.

   The ring mutex discipline lives here: it is held only while the
   descriptors, avail index and used ring are touched.  The submit-to-ack
   wait blocks on a condvar signalled by the irqhelp handler thread, so no
   lock is held across it.  */

#include <errno.h>
#include <hurd/irqhelp.h>
#include <mach.h>
#include <mach/gnumach.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "virtio-queue.h"

static inline void
virtio_wmb (void)
{
  __sync_synchronize ();
}

static inline uint16_t
virtio_idx_acquire (volatile uint16_t *p)
{
  uint16_t v = *p;
  virtio_wmb ();
  return v;
}

error_t
virtio_dma_alloc (struct virtio_pci_dev *v, size_t size, void **vaddr,
                  uint64_t *paddr)
{
  mach_port_t master_host, device_master;
  vm_address_t addr;
  rpc_phys_addr_t phys;
  kern_return_t kr;

  kr = get_privileged_ports (&master_host, &device_master);
  if (kr != KERN_SUCCESS)
    return EIO;
  mach_port_deallocate (mach_task_self (), device_master);

  /* pmin must be 0; palign 0 selects PAGE_SIZE; pmax 4 GiB selects the
     DMA32 pool, so the returned physical address is the machine address
     the device DMAes to.  Failure is fatal: a buffer the device cannot
     reach silently truncates whatever the driver posts.  */
  kr = vm_allocate_contiguous (master_host, mach_task_self (), &addr, &phys,
                               size, 0, 0x100000000ULL, 0);
  if (kr != KERN_SUCCESS)
    {
      fprintf (stderr, "%s: vm_allocate_contiguous (%zu) = %d\n",
               v->name, size, kr);
      return ENOMEM;
    }
  *vaddr = (void *) addr;
  *paddr = phys;
  return 0;
}

error_t
virtio_queue_setup (struct virtio_pci_dev *v, struct virtio_queue *q,
                    uint16_t index)
{
  uint16_t num;
  size_t desc_size, avail_off, used_off, region_size;
  void *region;
  uint64_t region_phys;
  error_t err;
  unsigned i;

  q->pci = v;
  q->index = index;
  virtio_pci_outw (v, VIRTIO_PCI_QUEUE_SEL, index);
  num = virtio_pci_read_queue_num (v);
  if (num == 0 || num > VIRTIO_QUEUE_MAX || (num & (num - 1)) != 0)
    {
      fprintf (stderr, "%s: implausible queue size %u\n", v->name, num);
      return EIO;
    }

  desc_size = num * sizeof (struct virtio_desc);
  avail_off = desc_size;
  /* avail: flags, idx, ring[num], used_event; used ring is 4-byte aligned
     per the virtio ring layout (vring_init).  */
  used_off = (avail_off + 4 + 2 * num + 2 + VIRTIO_QUEUE_ALIGN - 1)
             & ~(VIRTIO_QUEUE_ALIGN - 1);
  region_size = used_off + 4 + 2 * num + (size_t) num * 8 + 2;

  err = virtio_dma_alloc (v, region_size, &region, &region_phys);
  if (err)
    return err;
  memset (region, 0, region_size);

  q->num = num;
  q->desc = region;
  q->avail_idx = (volatile uint16_t *) ((char *) region + avail_off + 2);
  q->avail_ring = (uint16_t *) ((char *) region + avail_off + 4);
  q->used_idx = (volatile uint16_t *) ((char *) region + used_off + 2);
  q->used_ring = (struct virtio_used_elem *) ((char *) region + used_off + 4);
  q->last_used = 0;
  q->region_phys = region_phys;

  pthread_mutex_init (&q->lock, NULL);
  pthread_cond_init (&q->cv, NULL);

  /* All descriptors start on the free list, linked through the same next
     field the chains use once allocated.  */
  q->free_head = 0;
  for (i = 0; i + 1 < num; i++)
    q->desc[i].next = i + 1;
  q->desc[num - 1].next = VIRTIO_QUEUE_NO_DESC;

  virtio_pci_outl (v, VIRTIO_PCI_QUEUE_PFN, region_phys >> 12);
  if (virtio_pci_inl (v, VIRTIO_PCI_QUEUE_PFN) != region_phys >> 12)
    {
      fprintf (stderr, "%s: queue %u: QUEUE_PFN readback mismatch "
              "(wrote 0x%llx, read 0x%llx)\n",
              v->name, index, (unsigned long long) (region_phys >> 12),
              (unsigned long long) virtio_pci_read_queue_pfn (v));
      return EIO;
    }
  fprintf (stderr, "%s: queue %u: %u entries, region 0x%llx+%zu "
           "(phys 0x%llx)\n", v->name, index, num,
           (unsigned long long) (uintptr_t) region, region_size,
           (unsigned long long) region_phys);
  return 0;
}

static uint16_t
virtio_queue_desc_alloc (struct virtio_queue *q)
{
  uint16_t idx = q->free_head;
  if (idx != VIRTIO_QUEUE_NO_DESC)
    q->free_head = q->desc[idx].next;
  return idx;
}

static void
virtio_queue_desc_free (struct virtio_queue *q, uint16_t idx)
{
  q->desc[idx].next = q->free_head;
  q->free_head = idx;
}

/* Return the descriptors of a completed chain to the free list.  */
static void
virtio_queue_free_chain (struct virtio_queue *q, uint16_t head)
{
  uint16_t idx = head;
  do
    {
      uint16_t next = (q->desc[idx].flags & VIRTQ_DESC_F_NEXT)
                      ? q->desc[idx].next : VIRTIO_QUEUE_NO_DESC;
      virtio_queue_desc_free (q, idx);
      idx = next;
    }
  while (idx != VIRTIO_QUEUE_NO_DESC);
}

error_t
virtio_queue_post (struct virtio_queue *q, const struct virtio_buf *bufs,
                   uint16_t nbufs)
{
  uint16_t head, prev, i;

  if (nbufs == 0 || nbufs > q->num)
    return EMSGSIZE;

  pthread_mutex_lock (&q->lock);
  head = VIRTIO_QUEUE_NO_DESC;
  prev = VIRTIO_QUEUE_NO_DESC;
  for (i = 0; i < nbufs; i++)
    {
      uint16_t idx = virtio_queue_desc_alloc (q);

      if (idx == VIRTIO_QUEUE_NO_DESC)
        {
          if (head != VIRTIO_QUEUE_NO_DESC)
            virtio_queue_free_chain (q, head);
          pthread_mutex_unlock (&q->lock);
          return EAGAIN;
        }
      if (head == VIRTIO_QUEUE_NO_DESC)
        head = idx;
      else
        {
          q->desc[prev].flags |= VIRTQ_DESC_F_NEXT;
          q->desc[prev].next = idx;
        }
      /* The core owns F_NEXT: a caller-supplied NEXT on the last buffer
         would make the device chase descriptor 0.  */
      q->desc[idx].addr = bufs[i].addr;
      q->desc[idx].len = bufs[i].len;
      q->desc[idx].flags = bufs[i].flags & ~VIRTQ_DESC_F_NEXT;
      q->desc[idx].next = 0;
      prev = idx;
    }

  /* F_NEXT is what links the descriptors of a chain; the last one has it
     clear, and only device-writable buffers carry F_WRITE.  */
  virtio_wmb ();
  q->avail_ring[(*q->avail_idx) % q->num] = head;
  virtio_wmb ();
  (*q->avail_idx)++;
  virtio_wmb ();
  virtio_pci_outw (q->pci, VIRTIO_PCI_QUEUE_NOTIFY, q->index);
  pthread_mutex_unlock (&q->lock);
  return 0;
}

error_t
virtio_queue_wait_used (struct virtio_queue *q, int timeout_sec,
                        struct virtio_used_elem *elem)
{
  struct timespec deadline;
  int werr;

  clock_gettime (CLOCK_REALTIME, &deadline);
  deadline.tv_sec += timeout_sec;

  pthread_mutex_lock (&q->lock);
  /* The used index is the condition itself, not a flag: config-change
     interrupts and leftover signals produce wakeups with nothing new in
     the ring, which simply loop back to waiting.  */
  while (virtio_idx_acquire (q->used_idx) == q->last_used)
    {
      werr = pthread_cond_timedwait (&q->cv, &q->lock, &deadline);
      if (werr != 0)
        {
          pthread_mutex_unlock (&q->lock);
          fprintf (stderr, "%s: queue %u: device did not reply\n",
                   q->pci->name, q->index);
          return ETIMEDOUT;
        }
    }

  *elem = q->used_ring[q->last_used % q->num];
  if (elem->id >= q->num)
    {
      pthread_mutex_unlock (&q->lock);
      fprintf (stderr, "%s: queue %u: used entry id %u out of range\n",
               q->pci->name, q->index, elem->id);
      return EIO;
    }
  virtio_queue_free_chain (q, elem->id);
  q->last_used++;
  pthread_mutex_unlock (&q->lock);
  return 0;
}

/* Runs on irqhelp's own thread.  The ISR read is the device-level ack and
   the ownership check in one: zero means another device on this shared
   line asserted it and nothing must be touched.  Queue completions set bit
   0, config changes bit 1 (QEMU writes 0x3 for the latter), so the single
   read dispatches both causes before irqhelp performs the kernel-level
   ack/unmask.  */
static void
virtio_irq_handler (void *ctx)
{
  struct virtio_irq *irq = ctx;
  size_t k;
  uint8_t isr = virtio_pci_read_isr (irq->pci);

  if (!(isr & (VIRTIO_PCI_ISR_USED | VIRTIO_PCI_ISR_CONFIG)))
    return;

  for (k = 0; k < irq->nqueues; k++)
    {
      struct virtio_queue *q = irq->queues[k];
      pthread_mutex_lock (&q->lock);
      pthread_cond_broadcast (&q->cv);
      pthread_mutex_unlock (&q->lock);
    }

  if (isr & VIRTIO_PCI_ISR_CONFIG)
    {
      pthread_mutex_lock (&irq->lock);
      irq->config_seq++;
      pthread_cond_broadcast (&irq->cv);
      pthread_mutex_unlock (&irq->lock);
    }
}

error_t
virtio_irq_install (struct virtio_irq *irq, struct virtio_pci_dev *v,
                    struct virtio_queue **queues, size_t nqueues)
{
  pthread_t thread;
  error_t err;
  size_t k;

  if (nqueues == 0 || nqueues > VIRTIO_IRQ_MAX_QUEUES)
    return EINVAL;

  irq->pci = v;
  irq->nqueues = nqueues;
  for (k = 0; k < nqueues; k++)
    irq->queues[k] = queues[k];
  pthread_mutex_init (&irq->lock, NULL);
  pthread_cond_init (&irq->cv, NULL);
  irq->config_seq = 0;

  err = irqhelp_init ();
  if (err)
    return err;

  /* gsi = -1 makes irqhelp translate bus/dev/fun through ACPI.  */
  irq->irq = irqhelp_install_interrupt_handler (-1, v->dev->bus, v->dev->dev,
                                                v->dev->func,
                                                virtio_irq_handler, irq);
  if (!irq->irq)
    {
      fprintf (stderr, "%s: irqhelp_install_interrupt_handler failed\n",
               v->name);
      return EIO;
    }

  err = pthread_create (&thread, NULL, irqhelp_server_loop, irq->irq);
  if (err)
    {
      virtio_irq_uninstall (irq);
      return err;
    }
  pthread_detach (thread);
  return 0;
}

/* Idempotent.  Order mirrors NetBSD's virtio_child_detach: reset the
   device first (stops new INTx asserts and clears the ISR), then drop the
   handler.  Dropping the receive right marks the kernel-side user_intr_t
   dead; gnumach has no device_intr_unregister RPC, so intr_thread reaps
   the entry (and unmasks the line) on its next sweep.  Doing this before
   exit means the entry never outlives the process with unacked irqs
   pending from our device.  */
void
virtio_irq_uninstall (struct virtio_irq *irq)
{
  struct irq *handler = irq->irq;
  if (!handler)
    return;
  irq->irq = NULL;
  virtio_pci_quiesce (irq->pci);
  irqhelp_remove_interrupt_handler (handler);
}

error_t
virtio_irq_wait_config (struct virtio_irq *irq, int timeout_sec,
                        uint32_t *seq)
{
  struct timespec deadline;
  int werr;

  clock_gettime (CLOCK_REALTIME, &deadline);
  deadline.tv_sec += timeout_sec;

  pthread_mutex_lock (&irq->lock);
  while (irq->config_seq == *seq)
    {
      werr = pthread_cond_timedwait (&irq->cv, &irq->lock, &deadline);
      if (werr != 0)
        {
          pthread_mutex_unlock (&irq->lock);
          return ETIMEDOUT;
        }
    }
  *seq = irq->config_seq;
  pthread_mutex_unlock (&irq->lock);
  return 0;
}
