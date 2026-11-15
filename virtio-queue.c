/* Split virtqueue for the single 9P request queue.

   The vring lives in one physically contiguous, page-granular region: the
   legacy QUEUE_PFN register addresses the whole vring by physical page
   number, so the allocation must be page aligned.  vm_allocate_contiguous
   returns both the virtual and the machine physical address (the DMA32 pool
   below 4 GiB when pmax is 4 GiB), so the physical address goes straight
   into the descriptors and into QUEUE_PFN.

   The ring mutex discipline lives here: it is held only while the
   descriptors, avail index and used ring are touched.  The submit-to-reply
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

#include "virtio-9p.h"

#define V9P_VRING_ALIGN 4096

static struct v9p_vring q;
static uint8_t *tx_buf;
static uint8_t *rx_buf;
static uint64_t tx_phys;
static uint64_t rx_phys;
uint64_t v9p_vring_phys;

static pthread_mutex_t v9p_irq_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t v9p_irq_cond = PTHREAD_COND_INITIALIZER;
static struct irq *v9p_irq;

static inline void
v9p_wmb (void)
{
  __sync_synchronize ();
}

static inline uint16_t
v9p_idx_acquire (volatile uint16_t *p)
{
  uint16_t v = *p;
  v9p_wmb ();
  return v;
}

static error_t
v9p_dma_alloc (size_t size, void **vaddr, uint64_t *paddr)
{
  mach_port_t master_host, device_master;
  vm_address_t v;
  rpc_phys_addr_t p;
  kern_return_t kr;

  kr = get_privileged_ports (&master_host, &device_master);
  if (kr != KERN_SUCCESS)
    return EIO;
  mach_port_deallocate (mach_task_self (), device_master);

  /* pmin must be 0; palign 0 selects PAGE_SIZE; pmax 4 GiB selects the
     DMA32 pool, so the returned physical address is the machine address
     the device DMAes to.  Failure is fatal: undersized buffers would
     truncate PDUs the unmodified 9pfs client legitimately sends.  */
  kr = vm_allocate_contiguous (master_host, mach_task_self (), &v, &p,
                               size, 0, 0x100000000ULL, 0);
  if (kr != KERN_SUCCESS)
    {
      fprintf (stderr, "virtio-9p: vm_allocate_contiguous (%zu) = %d\n",
               size, kr);
      return ENOMEM;
    }
  *vaddr = (void *) v;
  *paddr = p;
  return 0;
}

error_t
v9p_queue_setup (void)
{
  uint16_t num;
  size_t desc_size, avail_off, used_off, region_size;
  void *region;
  uint64_t region_phys;
  error_t err;

  v9p_outw (V9P_PCI_QUEUE_SEL, 0);
  num = v9p_inw (V9P_PCI_QUEUE_NUM);
  if (num == 0 || num > V9P_QUEUE_MAX || (num & (num - 1)) != 0)
    {
      fprintf (stderr, "virtio-9p: implausible queue size %u\n", num);
      return EIO;
    }

  desc_size = num * sizeof (struct v9p_desc);
  avail_off = desc_size;
  /* avail: flags, idx, ring[num], used_event; used ring is 4-byte aligned
     per the virtio ring layout (vring_init).  */
  used_off = (avail_off + 4 + 2 * num + 2 + V9P_VRING_ALIGN - 1)
             & ~(V9P_VRING_ALIGN - 1);
  region_size = used_off + 4 + 2 * num + (size_t) num * 8 + 2;

  err = v9p_dma_alloc (region_size, &region, &region_phys);
  if (err)
    return err;
  memset (region, 0, region_size);

  q.num = num;
  q.desc = region;
  q.avail_idx = (volatile uint16_t *) ((char *) region + avail_off + 2);
  q.avail_ring = (uint16_t *) ((char *) region + avail_off + 4);
  q.used_idx = (volatile uint16_t *) ((char *) region + used_off + 2);
  q.used_ring = (struct v9p_used_elem *) ((char *) region + used_off + 4);
  q.last_used = 0;

  err = v9p_dma_alloc (V9P_TX_BUF_SIZE, (void **) &tx_buf, &tx_phys);
  if (err)
    return err;
  err = v9p_dma_alloc (V9P_RX_BUF_SIZE, (void **) &rx_buf, &rx_phys);
  if (err)
    return err;

  v9p_outl (V9P_PCI_QUEUE_PFN, region_phys >> 12);
  v9p_vring_phys = region_phys;
  if (v9p_inl (V9P_PCI_QUEUE_PFN) != region_phys >> 12)
    {
      fprintf (stderr, "virtio-9p: QUEUE_PFN readback mismatch "
              "(wrote 0x%llx, read 0x%llx)\n",
              (unsigned long long) (region_phys >> 12),
              (unsigned long long) v9p_inl (V9P_PCI_QUEUE_PFN));
      return EIO;
    }
  fprintf (stderr, "virtio-9p: queue 0: %u entries, region 0x%llx+%zu "
           "(phys 0x%llx), TX/RX 0x%llx/0x%llx\n",
           num, (unsigned long long) (uintptr_t) region, region_size,
           (unsigned long long) region_phys,
           (unsigned long long) tx_phys, (unsigned long long) rx_phys);
  return 0;
}

/* One request: the complete outbound PDU in TX, reply capacity in RX.  The
   reply lands in REPLY (up to REPLY_CAP bytes) and its length in REPLY_LEN.
   Submits the request, then blocks on the interrupt; the timeout is the
   fallback for a lost interrupt, not the normal path.  */
error_t
v9p_exchange (const void *pdu, uint32_t pdu_len,
              void *reply, uint32_t reply_cap, uint32_t *reply_len)
{
  uint32_t used_len;

  if (pdu_len < 7 || pdu_len > V9P_TX_BUF_SIZE)
    return EMSGSIZE;
  if (reply_cap > V9P_RX_BUF_SIZE)
    return EMSGSIZE;

  memcpy (tx_buf, pdu, pdu_len);

  q.desc[0].addr = tx_phys;
  q.desc[0].len = pdu_len;
  /* F_NEXT is what links the TX descriptor to the RX one; without it the
     device sees a one-descriptor chain and no reply buffer at all.  */
  q.desc[0].flags = V9P_DESC_F_NEXT;
  q.desc[0].next = 1;
  q.desc[1].addr = rx_phys;
  q.desc[1].len = reply_cap;
  q.desc[1].flags = V9P_DESC_F_WRITE;
  q.desc[1].next = 0;
  v9p_wmb ();
  q.avail_ring[*q.avail_idx % q.num] = 0;
  v9p_wmb ();
  (*q.avail_idx)++;
  v9p_wmb ();
  v9p_outw (V9P_PCI_QUEUE_NOTIFY, 0);

  {
    struct timespec deadline;
    int werr;

    clock_gettime (CLOCK_REALTIME, &deadline);
    deadline.tv_sec += V9P_EXCHANGE_TIMEOUT;

    pthread_mutex_lock (&v9p_irq_lock);
    /* The used index is the condition itself, not a flag: config-change
       interrupts and leftover signals produce wakeups with nothing new
       in the ring, which simply loop back to waiting.  */
    while (v9p_idx_acquire (q.used_idx) == q.last_used)
      {
        werr = pthread_cond_timedwait (&v9p_irq_cond, &v9p_irq_lock,
                                       &deadline);
        if (werr != 0)
          {
            pthread_mutex_unlock (&v9p_irq_lock);
            fprintf (stderr, "virtio-9p: device did not reply\n");
            return ETIMEDOUT;
          }
      }
    pthread_mutex_unlock (&v9p_irq_lock);
  }

  {
    uint32_t id = q.used_ring[q.last_used % q.num].id;
    used_len = q.used_ring[q.last_used % q.num].len;
    if (id >= q.num)
      {
        fprintf (stderr, "virtio-9p: used entry id %u out of range\n", id);
        return EIO;
      }
  }
  if (used_len > reply_cap)
    {
      fprintf (stderr, "virtio-9p: reply %u > capacity %u\n",
               used_len, reply_cap);
      return EMSGSIZE;
    }
  memcpy (reply, rx_buf, used_len);
  *reply_len = used_len;
  q.last_used++;
  return 0;
}

/* Runs on irqhelp's own thread.  The ISR read is the device-level ack and
   the ownership check in one: zero means another device on this shared
   line asserted it and nothing must be touched.  Bit 0 is set for queue
   completions and config changes alike (QEMU writes 0x3 for the latter),
   so the waiter re-checks the used index instead of trusting the signal.
   irqhelp calls device_intr_ack after this returns; that kernel-level
   unmask is why the handler, not the exchange, must read the ISR.  */
static void
v9p_irq_handler (void *ctx)
{
  if (!(v9p_read_isr () & 1))
    return;
  pthread_mutex_lock (&v9p_irq_lock);
  pthread_cond_broadcast (&v9p_irq_cond);
  pthread_mutex_unlock (&v9p_irq_lock);
}

error_t
v9p_irq_install (void)
{
  pthread_t thread;
  error_t err;

  err = irqhelp_init ();
  if (err)
    return err;

  /* gsi = -1 makes irqhelp translate bus/dev/fun through ACPI.  */
  v9p_irq = irqhelp_install_interrupt_handler (-1, v9p_dev->bus, v9p_dev->dev,
                                               v9p_dev->func,
                                               v9p_irq_handler, NULL);
  if (!v9p_irq)
    {
      fprintf (stderr, "virtio-9p: irqhelp_install_interrupt_handler "
              "failed\n");
      return EIO;
    }

  err = pthread_create (&thread, NULL, irqhelp_server_loop, v9p_irq);
  if (err)
    {
      v9p_irq_uninstall ();
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
v9p_irq_uninstall (void)
{
  struct irq *irq = v9p_irq;
  if (!irq)
    return;
  v9p_irq = NULL;
  v9p_pci_quiesce ();
  irqhelp_remove_interrupt_handler (irq);
}
