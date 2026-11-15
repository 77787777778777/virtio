#ifndef VIRTIO_9P_H
#define VIRTIO_9P_H

/* virtio-9p device translator for GNU/Hurd.

   Boundary conventions: the legacy PCI interface (BAR0 in I/O space), one
   split virtqueue, one outstanding PDU, and a byte-stream front end for the
   unmodified 9pfs translator.  */

#include <stdint.h>
#include <stddef.h>
#include <pciaccess.h>
#include <hurd.h>

/* Legacy virtio PCI register offsets within BAR0 (no MSI-X).  */
#define V9P_PCI_HOST_FEATURES  0x00
#define V9P_PCI_GUEST_FEATURES 0x04
#define V9P_PCI_QUEUE_PFN      0x08
#define V9P_PCI_QUEUE_NUM      0x0c
#define V9P_PCI_QUEUE_SEL      0x0e
#define V9P_PCI_QUEUE_NOTIFY   0x10
#define V9P_PCI_STATUS         0x12
#define V9P_PCI_ISR            0x13
#define V9P_PCI_CONFIG         0x14

/* Virtio 1.0 legacy device status bits.  */
#define V9P_S_ACKNOWLEDGE 1
#define V9P_S_DRIVER      2
#define V9P_S_DRIVER_OK   4

/* VIRTIO_9P_MOUNT_TAG is feature bit 0 and is required: without it the
   mount tag is not delivered.  */
#define V9P_F_MOUNT_TAG 0

#define V9P_VENDOR_REDHAT_QUMRANET 0x1af4
#define V9P_DEVICE_VIRTIO_9P       0x1009

/* Virtqueue geometry: QEMU's virtio-9p device offers a single queue of 128.  */
#define V9P_QUEUE_MAX 128
/* Per-exchange bound on the interrupt wait; the fallback for a lost
   interrupt, not the normal completion path.  */
#define V9P_EXCHANGE_TIMEOUT 30

/* QEMU's virtio-9p transport caps the negotiated msize at
   (VIRTQUEUE_MAX_SIZE - 2) * 4096, and 9pfs issues PDUs close to that, so
   both DMA directions need full-size buffers.  vm_allocate_contiguous rounds
   to power-of-two blocks, so these must not be silently undersized.  */
#define V9P_TX_BUF_SIZE (4u << 20)
#define V9P_RX_BUF_SIZE (4u << 20)

/* QEMU's virtio-9p transport msize cap, asserted by the selftest.  */
#define V9P_MSIZE_CAP ((1024u - 2) * 4096u)

#define V9P_MAX_TAG_LEN 32
#define V9P_DEFAULT_MOUNT_TAG "hostshare"

/* Descriptor flags.  WRITE means "the device writes this buffer", so the
   request chain never has it and the reply chain always does.  */
#define V9P_DESC_F_NEXT  1
#define V9P_DESC_F_WRITE 2

struct v9p_desc {
  uint64_t addr;
  uint32_t len;
  uint16_t flags;
  uint16_t next;
};

struct v9p_used_elem {
  uint32_t id;
  uint32_t len;
};

struct v9p_vring {
  uint16_t num;
  struct v9p_desc *desc;
  volatile uint16_t *avail_idx;
  uint16_t *avail_ring;
  volatile uint16_t *used_idx;
  struct v9p_used_elem *used_ring;
  uint16_t last_used;
};

/* virtio-pci.c */
extern struct pci_device *v9p_dev;
extern pciaddr_t v9p_io_base;
extern pciaddr_t v9p_io_size;
extern char v9p_tag[V9P_MAX_TAG_LEN + 1];
extern uint16_t v9p_tag_len;

/* Legacy register access on the I/O BAR.  */
void v9p_outb (int off, uint8_t v);
void v9p_outw (int off, uint16_t v);
void v9p_outl (int off, uint32_t v);
uint8_t v9p_inb (int off);
uint16_t v9p_inw (int off);
uint32_t v9p_inl (int off);

error_t v9p_pci_probe (const char *override);
error_t v9p_pci_init (void);
error_t v9p_pci_driver_ok (void);
uint32_t v9p_read_guest_features (void);
uint8_t v9p_read_isr (void);

/* virtio-queue.c */
error_t v9p_queue_setup (void);
error_t v9p_irq_install (void);
error_t v9p_exchange (const void *pdu, uint32_t pdu_len,
                      void *reply, uint32_t reply_cap, uint32_t *reply_len);
extern uint64_t v9p_vring_phys;
uint32_t v9p_read_queue_pfn (void);

/* virtio-9p-req.c */
error_t v9p_selftest (const char *required_tag);

/* virtio-9p-ios.c */
error_t v9p_ios_init (void);
ssize_t v9p_ios_write (const void *buf, size_t len);
ssize_t v9p_ios_read (void *buf, size_t len);
int v9p_ios_can_read (void);
int v9p_ios_can_write (void);

#endif /* VIRTIO_9P_H */
