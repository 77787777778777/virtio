#ifndef VIRTIO_PCI_H
#define VIRTIO_PCI_H

/* Generic legacy virtio PCI transport for GNU/Hurd userspace drivers.

   This is the transport virtio-9p proved on the i440fx test machine, factored
   device-neutral: BAR0 in I/O space, the legacy register layout (no MSI-X,
   which would shift the config region), and INTx interrupts serviced through
   libirqhelp.  Nothing in here knows a virtio device ID, a feature bit or a
   queue role; those policies live in the per-device files.  PCI config space
   is reached through libpciaccess (a pci-arbiter client), never through
   ports 0xcf8/0xcfc, and the legacy registers with ordinary port I/O after
   ioperm.  */

#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include <pciaccess.h>
#include <hurd.h>

/* Legacy virtio PCI register offsets within BAR0 (no MSI-X).  */
#define VIRTIO_PCI_HOST_FEATURES  0x00
#define VIRTIO_PCI_GUEST_FEATURES 0x04
#define VIRTIO_PCI_QUEUE_PFN      0x08
#define VIRTIO_PCI_QUEUE_NUM      0x0c
#define VIRTIO_PCI_QUEUE_SEL      0x0e
#define VIRTIO_PCI_QUEUE_NOTIFY   0x10
#define VIRTIO_PCI_STATUS         0x12
#define VIRTIO_PCI_ISR            0x13
#define VIRTIO_PCI_CONFIG         0x14

/* Virtio 1.0 legacy device status bits.  */
#define VIRTIO_S_ACKNOWLEDGE 1
#define VIRTIO_S_DRIVER      2
#define VIRTIO_S_DRIVER_OK   4

/* Legacy ISR bits.  The register is read-to-clear, and QEMU reports a config
   change as 0x3, so a handler must read the ISR once per interrupt and
   dispatch every set bit before returning.  */
#define VIRTIO_PCI_ISR_USED   0x1
#define VIRTIO_PCI_ISR_CONFIG 0x2

/* Descriptor flags.  WRITE means "the device writes this buffer", so
   device-readable (driver-written) buffers never carry it.  */
#define VIRTQ_DESC_F_NEXT  1
#define VIRTQ_DESC_F_WRITE 2

#define VIRTIO_VENDOR_REDHAT_QUMRANET 0x1af4

struct virtio_pci_dev {
  struct pci_device *dev;
  pciaddr_t io_base;
  pciaddr_t io_size;
  /* Log prefix and the virtio PCI device ID to match, set by the driver.  */
  const char *name;
  uint16_t device_id;
};

/* Legacy register access on the I/O BAR.  */
void virtio_pci_outb (struct virtio_pci_dev *, int off, uint8_t v);
void virtio_pci_outw (struct virtio_pci_dev *, int off, uint16_t v);
void virtio_pci_outl (struct virtio_pci_dev *, int off, uint32_t v);
uint8_t virtio_pci_inb (struct virtio_pci_dev *, int off);
uint16_t virtio_pci_inw (struct virtio_pci_dev *, int off);
uint32_t virtio_pci_inl (struct virtio_pci_dev *, int off);

/* Find the device, verify the legacy I/O BAR, enable I/O and bus mastering.  */
error_t virtio_pci_probe (struct virtio_pci_dev *, const char *override);

/* Handshake primitives; each device composes its own policy.  */
void virtio_pci_reset (struct virtio_pci_dev *);
void virtio_pci_set_driver (struct virtio_pci_dev *);
uint32_t virtio_pci_read_host_features (struct virtio_pci_dev *);
void virtio_pci_set_guest_features (struct virtio_pci_dev *, uint32_t);
error_t virtio_pci_driver_ok (struct virtio_pci_dev *);
void virtio_pci_quiesce (struct virtio_pci_dev *);
uint32_t virtio_pci_read_guest_features (struct virtio_pci_dev *);

/* ISR (device-level ack) and legacy config access.  The config region is
   little-endian on the wire; the host is x86_64, so inl/outl are LE.  */
uint8_t virtio_pci_read_isr (struct virtio_pci_dev *);
uint32_t virtio_pci_read_config (struct virtio_pci_dev *, int off);
void virtio_pci_write_config (struct virtio_pci_dev *, int off, uint32_t val);
uint16_t virtio_pci_read_queue_num (struct virtio_pci_dev *);
uint32_t virtio_pci_read_queue_pfn (struct virtio_pci_dev *);

/* Virtio wire fields are little-endian regardless of guest native
   endianness.  The host is x86_64, but the helpers keep every wire
   field write explicit; shared by the device drivers that build
   request or config structures by hand.  */
static inline void
virtio_put_le32 (void *p, uint32_t v)
{
  uint8_t *b = p;
  b[0] = v;
  b[1] = v >> 8;
  b[2] = v >> 16;
  b[3] = v >> 24;
}

static inline void
virtio_put_le64 (void *p, uint64_t v)
{
  uint8_t *b = p;
  virtio_put_le32 (b, (uint32_t) v);
  virtio_put_le32 (b + 4, (uint32_t) (v >> 32));
}

#endif /* VIRTIO_PCI_H */
