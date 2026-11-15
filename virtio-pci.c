/* Generic legacy virtio PCI transport: probe, BAR access, handshake.

   PCI config space is reached through libpciaccess, which on GNU/Hurd is a
   client of the pci-arbiter translator; drivers must not touch ports
   0xcf8/0xcfc themselves (the kernel allows a single holder of that range,
   and pci-arbiter already holds it).  The legacy register BAR is I/O space
   on the i440fx test machine, so it is reached with ordinary port I/O after
   ioperm, the way rtc/main.c and console-client do it.  */

#include <errno.h>
#include <pciaccess.h>
#include <stdio.h>
#include <string.h>
#include <sys/io.h>
#include <unistd.h>

#include "virtio-pci.h"

#define PCI_REG_COMMAND 0x04
#define PCI_COMMAND_IO 0x1
#define PCI_COMMAND_MASTER 0x4

/* glibc's outb/inb take (value, port); the off parameter is ours.  */
void
virtio_pci_outb (struct virtio_pci_dev *v, int off, uint8_t val)
{
  outb (val, v->io_base + off);
}
void
virtio_pci_outw (struct virtio_pci_dev *v, int off, uint16_t val)
{
  outw (val, v->io_base + off);
}
void
virtio_pci_outl (struct virtio_pci_dev *v, int off, uint32_t val)
{
  outl (val, v->io_base + off);
}
uint8_t
virtio_pci_inb (struct virtio_pci_dev *v, int off)
{
  return inb (v->io_base + off);
}
uint16_t
virtio_pci_inw (struct virtio_pci_dev *v, int off)
{
  return inw (v->io_base + off);
}
uint32_t
virtio_pci_inl (struct virtio_pci_dev *v, int off)
{
  return inl (v->io_base + off);
}

/* Make the BAR's port range usable and the device a bus master; without
   mastering the device cannot DMA, and without I/O space the legacy
   registers are dead.  QEMU does not reliably do this for a userspace
   driver.  */
static error_t
virtio_pci_enable (struct virtio_pci_dev *v)
{
  uint16_t cmd = 0;
  error_t err;

  err = pci_device_cfg_read_u16 (v->dev, &cmd, PCI_REG_COMMAND);
  if (err)
    return err;
  cmd |= PCI_COMMAND_IO | PCI_COMMAND_MASTER;
  err = pci_device_cfg_write_u16 (v->dev, cmd, PCI_REG_COMMAND);
  if (err)
    return err;

  if (ioperm (v->io_base, v->io_size, 1) < 0)
    return errno;
  return 0;
}

error_t
virtio_pci_probe (struct virtio_pci_dev *v, const char *override)
{
  struct pci_slot_match match =
    { PCI_MATCH_ANY, PCI_MATCH_ANY, PCI_MATCH_ANY, PCI_MATCH_ANY, 0 };
  struct pci_device_iterator *iter;
  struct pci_device *dev;
  error_t err = 0;
  int scanned = 0, virtio = 0;

  /* The library does not initialize itself implicitly: the pci-arbiter
     process calls this in its own startup, and every other client of it
     (this driver included) must call it too, or the walk below stays
     empty.  */
  err = pci_system_init ();
  if (err)
    {
      fprintf (stderr, "%s: pci_system_init: %s\n", v->name, strerror (err));
      return err;
    }

  iter = pci_slot_match_iterator_create (&match);
  while ((dev = pci_device_next (iter)) != NULL)
    {
      scanned++;
      err = pci_device_probe (dev);
      if (err)
        {
          fprintf (stderr, "%s: probe of %04x:%02x:%02x.%x failed: %s\n",
                   v->name, dev->domain, dev->bus, dev->dev, dev->func,
                   strerror (err));
          continue;
        }
      if (dev->vendor_id != VIRTIO_VENDOR_REDHAT_QUMRANET)
        continue;
      virtio++;
      fprintf (stderr, "%s: virtio device %04x at %04x:%02x:%02x.%x\n",
               v->name, dev->device_id, dev->domain, dev->bus, dev->dev,
               dev->func);
      if (dev->device_id != v->device_id)
        continue;
      if (override)
        {
          char name[64];
          snprintf (name, sizeof name, "%04x:%02x:%02x.%x",
                    dev->domain, dev->bus, dev->dev, dev->func);
          if (strcmp (name, override) != 0)
            continue;
        }
      err = 0;
      break;
    }
  pci_iterator_destroy (iter);

  if (dev == NULL || err != 0)
    {
      fprintf (stderr,
               "%s: no %s device found (%d PCI devices scanned, %d virtio)\n",
               v->name, v->name, scanned, virtio);
      return ENODEV;
    }

  v->dev = dev;

  if (dev->regions[0].is_IO == 0)
    {
      fprintf (stderr,
               "%s: BAR0 is not an I/O region (type %u); the legacy "
               "register layout this driver uses is only guaranteed on a\n"
               "legacy PCI bus (pc machine, disable-legacy=off)\n",
               v->name, dev->regions[0].is_IO);
      return ENODEV;
    }
  v->io_base = dev->regions[0].base_addr;
  v->io_size = dev->regions[0].size;

  err = virtio_pci_enable (v);
  if (err)
    return err;

  fprintf (stderr,
           "%s: found %04x:%04x at %04x:%02x:%02x.%x, "
           "I/O BAR at 0x%llx size 0x%llx\n",
           v->name, dev->vendor_id, dev->device_id, dev->domain, dev->bus,
           dev->dev, dev->func, (unsigned long long) v->io_base,
           (unsigned long long) v->io_size);
  return 0;
}

/* Reset, ACKNOWLEDGE, DRIVER.  */
void
virtio_pci_reset (struct virtio_pci_dev *v)
{
  virtio_pci_outb (v, VIRTIO_PCI_STATUS, 0);
  usleep (10 * 1000);
}

void
virtio_pci_set_driver (struct virtio_pci_dev *v)
{
  virtio_pci_outb (v, VIRTIO_PCI_STATUS, VIRTIO_S_ACKNOWLEDGE);
  virtio_pci_outb (v, VIRTIO_PCI_STATUS,
                   VIRTIO_S_ACKNOWLEDGE | VIRTIO_S_DRIVER);
}

error_t
virtio_pci_driver_ok (struct virtio_pci_dev *v)
{
  virtio_pci_outb (v, VIRTIO_PCI_STATUS,
                   VIRTIO_S_ACKNOWLEDGE | VIRTIO_S_DRIVER | VIRTIO_S_DRIVER_OK);
  /* The device does not acknowledge DRIVER_OK; the readback only proves the
     bits are writable and readable.  */
  if ((virtio_pci_inb (v, VIRTIO_PCI_STATUS)
       & (VIRTIO_S_ACKNOWLEDGE | VIRTIO_S_DRIVER | VIRTIO_S_DRIVER_OK))
      != (VIRTIO_S_ACKNOWLEDGE | VIRTIO_S_DRIVER | VIRTIO_S_DRIVER_OK))
    return EIO;
  return 0;
}

/* Teardown-side mirror of the handshake: writing 0 to the status register
   is the legacy-virtio device reset.  QEMU answers by clearing the ISR
   register and deasserting INTx, so no new interrupt from this device can
   be raised or delivered after this point.  This must precede IRQ handler
   removal, mirroring the reset-then-free-interrupts order of NetBSD's
   virtio_child_detach and Linux's virtio_dev_remove.  */
void
virtio_pci_quiesce (struct virtio_pci_dev *v)
{
  virtio_pci_outb (v, VIRTIO_PCI_STATUS, 0);
  usleep (10 * 1000);
}

uint32_t
virtio_pci_read_host_features (struct virtio_pci_dev *v)
{
  return virtio_pci_inl (v, VIRTIO_PCI_HOST_FEATURES);
}

void
virtio_pci_set_guest_features (struct virtio_pci_dev *v, uint32_t features)
{
  virtio_pci_outl (v, VIRTIO_PCI_GUEST_FEATURES, features);
}

uint32_t
virtio_pci_read_guest_features (struct virtio_pci_dev *v)
{
  return virtio_pci_inl (v, VIRTIO_PCI_GUEST_FEATURES);
}

uint8_t
virtio_pci_read_isr (struct virtio_pci_dev *v)
{
  return virtio_pci_inb (v, VIRTIO_PCI_ISR);
}

uint32_t
virtio_pci_read_config (struct virtio_pci_dev *v, int off)
{
  return virtio_pci_inl (v, VIRTIO_PCI_CONFIG + off);
}

void
virtio_pci_write_config (struct virtio_pci_dev *v, int off, uint32_t val)
{
  virtio_pci_outl (v, VIRTIO_PCI_CONFIG + off, val);
}

uint16_t
virtio_pci_read_queue_num (struct virtio_pci_dev *v)
{
  return virtio_pci_inw (v, VIRTIO_PCI_QUEUE_NUM);
}

uint32_t
virtio_pci_read_queue_pfn (struct virtio_pci_dev *v)
{
  return virtio_pci_inl (v, VIRTIO_PCI_QUEUE_PFN);
}
