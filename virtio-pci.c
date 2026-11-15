/* PCI probe, legacy BAR access, and the virtio device handshake.

   PCI config space is reached through libpciaccess, which on GNU/Hurd is a
   client of the pci-arbiter translator; this driver must not touch ports
   0xcf8/0xcfc itself (the kernel allows a single holder of that range, and
   pci-arbiter already holds it).  The legacy register BAR is I/O space on
   the i440fx test machine, so it is reached with ordinary port I/O after
   ioperm, the way rtc/main.c and console-client do it.  */

#include <errno.h>
#include <pciaccess.h>
#include <stdio.h>
#include <string.h>
#include <sys/io.h>
#include <unistd.h>

#include "virtio-9p.h"

struct pci_device *v9p_dev;
pciaddr_t v9p_io_base;
pciaddr_t v9p_io_size;
char v9p_tag[V9P_MAX_TAG_LEN + 1];
uint16_t v9p_tag_len;

#define PCI_REG_COMMAND 0x04
#define PCI_COMMAND_IO 0x1
#define PCI_COMMAND_MASTER 0x4

/* glibc's outb/inb take (value, port); the off parameter is ours.  */
void
v9p_outb (int off, uint8_t v) { outb (v, v9p_io_base + off); }
void
v9p_outw (int off, uint16_t v) { outw (v, v9p_io_base + off); }
void
v9p_outl (int off, uint32_t v) { outl (v, v9p_io_base + off); }
uint8_t
v9p_inb (int off) { return inb (v9p_io_base + off); }
uint16_t
v9p_inw (int off) { return inw (v9p_io_base + off); }
uint32_t
v9p_inl (int off) { return inl (v9p_io_base + off); }

/* Make the BAR's port range usable and the device a bus master; without
   mastering the device cannot DMA, and without I/O space the legacy
   registers are dead.  QEMU does not reliably do this for a userspace
   driver.  */
static error_t
v9p_pci_enable (void)
{
  uint16_t cmd = 0;
  error_t err;

  err = pci_device_cfg_read_u16 (v9p_dev, &cmd, PCI_REG_COMMAND);
  if (err)
    return err;
  cmd |= PCI_COMMAND_IO | PCI_COMMAND_MASTER;
  err = pci_device_cfg_write_u16 (v9p_dev, cmd, PCI_REG_COMMAND);
  if (err)
    return err;

  if (ioperm (v9p_io_base, v9p_io_size, 1) < 0)
    return errno;
  return 0;
}

error_t
v9p_pci_probe (const char *override)
{
  struct pci_slot_match match =
    { PCI_MATCH_ANY, PCI_MATCH_ANY, PCI_MATCH_ANY, PCI_MATCH_ANY, 0 };
  struct pci_device_iterator *iter;
  struct pci_device *dev;
  error_t err = 0;
  int scanned = 0, virtio = 0;

  /* The library does not initialize itself implicitly: the pci-arbiter
     process calls this in its own startup, and every other client of it
     (this translator included) must call it too, or the walk below stays
     empty.  */
  err = pci_system_init ();
  if (err)
    {
      fprintf (stderr, "virtio-9p: pci_system_init: %s\n", strerror (err));
      return err;
    }

  iter = pci_slot_match_iterator_create (&match);
  while ((dev = pci_device_next (iter)) != NULL)
    {
      scanned++;
      err = pci_device_probe (dev);
      if (err)
        {
          fprintf (stderr, "virtio-9p: probe of %04x:%02x:%02x.%x failed: %s\n",
                   dev->domain, dev->bus, dev->dev, dev->func, strerror (err));
          continue;
        }
      if (dev->vendor_id != V9P_VENDOR_REDHAT_QUMRANET)
        continue;
      virtio++;
      fprintf (stderr, "virtio-9p: virtio device %04x at %04x:%02x:%02x.%x\n",
               dev->device_id, dev->domain, dev->bus, dev->dev, dev->func);
      if (dev->device_id != V9P_DEVICE_VIRTIO_9P)
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
               "virtio-9p: no virtio-9p device found (%d PCI devices scanned, "
               "%d virtio)\n", scanned, virtio);
      return ENODEV;
    }

  v9p_dev = dev;

  if (dev->regions[0].is_IO == 0)
    {
      fprintf (stderr,
               "virtio-9p: BAR0 is not an I/O region (type %u); the legacy "
               "register layout this driver uses is only guaranteed on a\n"
               "legacy PCI bus (pc machine, disable-legacy=off)\n",
               dev->regions[0].is_IO);
      return ENODEV;
    }
  v9p_io_base = dev->regions[0].base_addr;
  v9p_io_size = dev->regions[0].size;

  err = v9p_pci_enable ();
  if (err)
    return err;

  fprintf (stderr,
           "virtio-9p: found %04x:%04x at %04x:%02x:%02x.%x, "
           "I/O BAR at 0x%llx size 0x%llx\n",
           dev->vendor_id, dev->device_id, dev->domain, dev->bus, dev->dev,
           dev->func, (unsigned long long) v9p_io_base,
           (unsigned long long) v9p_io_size);
  return 0;
}

/* Reset, ACKNOWLEDGE, DRIVER, require and acknowledge the mount-tag
   feature, read the config region.  Queue programming and DRIVER_OK happen
   after this returns, in that order.  */
error_t
v9p_pci_init (void)
{
  uint32_t features;

  v9p_outb (V9P_PCI_STATUS, 0);
  usleep (10 * 1000);

  v9p_outb (V9P_PCI_STATUS, V9P_S_ACKNOWLEDGE);
  v9p_outb (V9P_PCI_STATUS, V9P_S_ACKNOWLEDGE | V9P_S_DRIVER);

  features = v9p_inl (V9P_PCI_HOST_FEATURES);
  if (!(features & (1u << V9P_F_MOUNT_TAG)))
    {
      fprintf (stderr, "virtio-9p: device does not offer VIRTIO_9P_MOUNT_TAG\n");
      return ENODEV;
    }
  /* Writing guest_features = 0 would leave the mount tag undefined.  */
  v9p_outl (V9P_PCI_GUEST_FEATURES, 1u << V9P_F_MOUNT_TAG);

  v9p_tag_len = v9p_inw (V9P_PCI_CONFIG);
  if (v9p_tag_len == 0 || v9p_tag_len > V9P_MAX_TAG_LEN)
    {
      fprintf (stderr, "virtio-9p: implausible tag_len %u\n", v9p_tag_len);
      return EIO;
    }
  {
    unsigned i;
    for (i = 0; i < v9p_tag_len; i++)
      v9p_tag[i] = v9p_inb (V9P_PCI_CONFIG + 2 + i);
  }
  v9p_tag[v9p_tag_len] = '\0';

  fprintf (stderr, "virtio-9p: mount tag \"%s\"\n", v9p_tag);
  /* Ack any interrupt the handshake itself raised.  */
  v9p_read_isr ();
  return 0;
}

error_t
v9p_pci_driver_ok (void)
{
  v9p_outb (V9P_PCI_STATUS,
            V9P_S_ACKNOWLEDGE | V9P_S_DRIVER | V9P_S_DRIVER_OK);
  /* The device does not acknowledge DRIVER_OK; the readback only proves the
     bits are writable and readable.  */
  if ((v9p_inb (V9P_PCI_STATUS)
       & (V9P_S_ACKNOWLEDGE | V9P_S_DRIVER | V9P_S_DRIVER_OK))
      != (V9P_S_ACKNOWLEDGE | V9P_S_DRIVER | V9P_S_DRIVER_OK))
    return EIO;
  return 0;
}

uint32_t
v9p_read_guest_features (void)
{
  return v9p_inl (V9P_PCI_GUEST_FEATURES);
}

uint32_t
v9p_read_queue_pfn (void)
{
  return v9p_inl (V9P_PCI_QUEUE_PFN);
}

uint8_t
v9p_read_isr (void)
{
  return v9p_inb (V9P_PCI_ISR);
}
