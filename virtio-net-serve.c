/*
 * virtio-net-serve.c - MIG device handlers and vtable for the
 * virtio-net driver (plan Stage B, D3/S2/S3).
 *
 * The translator scaffold (trivfs node, demuxer, notify plumbing,
 * port lifetime, goaway) lives in the shared virtio-serve.c; this file
 * supplies the net-specific vtable and the ds_device_* handlers whose
 * signatures carry the driver's own port type:
 *
 *   - ds_device_open scaffolding is shared; handle_open here builds the
 *     struct vnet_hurd handle (init + mode policy + registry), and the
 *     shared open hands the client a MOVE_SEND right to it (libmachdev
 *     machdev_create_device_port pattern, but serving network frames
 *     instead of block I/O);
 *   - subsequent ds_device_write/set_filter/get_status calls resolve
 *     that port back to the handle via the DEVICE_* MIG mutations in
 *     vnet-mutate.h, and run the plan's §5 semantics through
 *     virtio-net-hurd.c;
 *   - filters registered by a client are removed by the dead-name
 *     notification requested at set_filter time (the ports_dead_name
 *     override in virtio-serve.c dispatches into the filter walk).
 *
 * MIG stubs are generated from the cross prefix's
 * include/device/device.defs (subsystem 2800) and mach/notify.defs by
 * the Makefile, with vnet-mutate.h wired in through -imacros and
 * -DMACH_PAYLOAD_TO_PORT=ports_payload_get_name, matching the blk
 * precedent; the net outputs carry vnet-prefixed names so both servers
 * can be built side by side.
 */

#include <mach.h>
#include <stdio.h>
#include <stddef.h>
#include <string.h>

#include <device/device.h>
#include <hurd/ports.h>

#include "virtio-net-serve.h"
#include "virtio-serve.h"

/* The device_open name tail is written at port_size, so it must be the
   flexible array's offset exactly (virtio_serve.c's port-creation
   arithmetic depends on it).  */
_Static_assert (offsetof (struct vnet_port, name) == sizeof (struct vnet_port),
                "vnet_port name tail must start at the struct size");

/* The MIG-generated server routines (vnetDeviceServer.c from
   include/device/device.defs, vnetNotifyServer.c from mach/notify.defs)
   dispatch into the ds_device_* handlers below; their headers appear
   here through the Makefile's include path (-I.).  */
#include "vnet-device_S.h"
#include "vnet-notify_S.h"

/* The one device this translator owns (brought up before serving).  */
static struct vnet_dev *serving_dev;

static const char *
vnet_port_name (void *port)
{
  return ((struct vnet_port *) port)->name;
}

/* Per-device open: semantics-layer init, mode policy, registry.  The
   delivery path routes frames to registered handles only, so the handle
   is registered as soon as it opens.  */
static error_t
vnet_handle_open (void *port, dev_mode_t mode)
{
  struct vnet_port *p = port;
  error_t err;

  err = vnet_hurd_init (&p->hurd, serving_dev);
  if (err)
    {
      if (vnet_verbose)
        fprintf (stderr, "virtio-net-serve: open '%s': init failed %d\n",
                 p->name, err);
      return D_NO_MEMORY;
    }

  err = vnet_hurd_open (&p->hurd, mode);
  if (err)
    {
      if (vnet_verbose)
        fprintf (stderr, "virtio-net-serve: open '%s': failed %d\n",
                 p->name, err);
      return err;
    }
  vnet_hurd_register (&p->hurd);
  return 0;
}

/* Idempotent: also runs from the class clean hook after an explicit
   device_close.  The Hurd handle stays registered until the final deref
   so late RX completions still find it; the client's filters die with
   its receive port via dead-name.  */
static void
vnet_handle_close (void *port)
{
  struct vnet_port *p = port;

  vnet_hurd_close (&p->hurd);
  vnet_hurd_unregister (&p->hurd);
}

static void
vnet_serve_dead_name (mach_port_t dead_name)
{
  /* The notify port does not identify which handle registered the
     filter, so every open handle is walked (remove_dead_filter is a
     no-op for lists without the port).  */
  vnet_hurd_all_dead_name (dead_name);
}

/* The generated stubs' dispatch: device_server_routine then
   notify_server_routine (both static inlines over per-binary id tables
   in the _S.h headers above).  */
static mig_routine_t
vnet_server_routine (mach_msg_header_t *inp)
{
  mig_routine_t routine;

  if ((routine = device_server_routine (inp)))
    return routine;
  return notify_server_routine (inp);
}

static const struct virtio_serve_ops vnet_serve_ops =
{
  .prefix = "virtio-net-serve",
  .translator = "/hurd/virtio-net",
  .verbose = &vnet_verbose,
  .port_size = sizeof (struct vnet_port),
  .port_name = vnet_port_name,
  .server_routine = vnet_server_routine,
  .handle_open = vnet_handle_open,
  .handle_close = vnet_handle_close,
  .quiesce = (void (*) (void *)) vnet_dev_quiesce,
  .dead_name = vnet_serve_dead_name,
};

/* --- device interface ------------------------------------------------  */

kern_return_t
ds_device_close (struct vnet_port *device)
{
  if (vnet_verbose)
    fprintf (stderr, "virtio-net-serve: close %p name %s\n",
             device, device ? device->name : "(null)");
  if (device == NULL)
    return D_NO_SUCH_DEVICE;

  vnet_handle_close (device);
  ports_destroy_right (device);
  return 0;
}

kern_return_t
ds_device_write (vnet_port_t device, mach_port_t reply_port,
                 mach_msg_type_name_t reply_type, dev_mode_t mode,
                 recnum_t recnum, io_buf_ptr_t data,
                 mach_msg_type_number_t datalen, int *bytes_written)
{
  error_t err;

  if (device == NULL)
    return D_NO_SUCH_DEVICE;
  if (data == NULL)
    return D_INVALID_SIZE;

  err = vnet_hurd_write (&device->hurd, mode, recnum, data, datalen,
                         bytes_written);
  /* The write data arrives out of line (the MIG stub moves the pages
     into this task).  After a successful return it is ours to free.
     After an error the server loop destroys the request and the data
     with it (glibc's mach_msg_server), so freeing it here as well would
     unmap whatever another thread mapped at that address in between.  */
  if (!err)
    vm_deallocate (mach_task_self (), (vm_address_t) data, datalen);
  return err;
}

kern_return_t
ds_device_read (vnet_port_t device, mach_port_t reply_port,
                mach_msg_type_name_t reply_type, dev_mode_t mode,
                recnum_t recnum, int bytes_wanted,
                io_buf_ptr_t *data, mach_msg_type_number_t *datalen)
{
  (void) device;
  (void) reply_port;
  (void) reply_type;
  (void) mode;
  (void) recnum;
  (void) bytes_wanted;
  *data = (io_buf_ptr_t) 0;
  *datalen = 0;
  /* Inbound frames arrive as NET_RCV_MSG on the filter-registered
     receive port, never through device_read (plan §5.1.1).  */
  return D_INVALID_OPERATION;
}

kern_return_t
ds_device_get_status (vnet_port_t device, dev_flavor_t flavor,
                      dev_status_t status,
                      mach_msg_type_number_t *statuslen)
{
  if (device == NULL)
    return D_NO_SUCH_DEVICE;

  return vnet_hurd_get_status (&device->hurd, flavor, status, statuslen);
}

kern_return_t
ds_device_set_status (vnet_port_t device, dev_flavor_t flavor,
                      dev_status_t status,
                      mach_msg_type_number_t statuslen)
{
  if (device == NULL)
    return D_NO_SUCH_DEVICE;

  return vnet_hurd_set_status (&device->hurd, flavor, status, statuslen);
}

kern_return_t
ds_device_write_inband (vnet_port_t device, mach_port_t reply_port,
                        mach_msg_type_name_t reply_type, dev_mode_t mode,
                        recnum_t recnum, const io_buf_ptr_inband_t data,
                        mach_msg_type_number_t datalen, int *bytes_written)
{
  if (device == NULL)
    return D_NO_SUCH_DEVICE;
  return vnet_hurd_write_inband (&device->hurd, mode, recnum, data,
                                 datalen, bytes_written);
}

kern_return_t
ds_device_read_inband (vnet_port_t device, mach_port_t reply_port,
                       mach_msg_type_name_t reply_type, dev_mode_t mode,
                       recnum_t recnum, int bytes_wanted,
                       io_buf_ptr_inband_t data,
                       mach_msg_type_number_t *datalen)
{
  if (device == NULL)
    return D_NO_SUCH_DEVICE;
  *datalen = 0;
  return vnet_hurd_read_inband (&device->hurd, mode, recnum, bytes_wanted,
                                data);
}

kern_return_t
ds_device_map (vnet_port_t device, vm_prot_t prot, vm_offset_t offset,
               vm_size_t size, memory_object_t *pager, int unmap)
{
  if (device == NULL)
    return D_NO_SUCH_DEVICE;
  return vnet_hurd_map (&device->hurd, prot, offset, size, pager, unmap);
}

kern_return_t
ds_device_set_filter (vnet_port_t device, mach_port_t receive_port,
                      int priority, filter_array_t filter,
                      mach_msg_type_number_t filterlen)
{
  error_t err;

  if (device == NULL)
    return D_NO_SUCH_DEVICE;

  /* The same wiring as eth-multiplexer's ds_device_set_filter: a
     dead-name request on the client's receive port makes the filter
     removal (and the send-right drop inside net_free_dead_infp)
     automatic when the client closes.  A port that cannot take the
     request gets no filter, since nothing would ever remove it.  */
  err = ports_request_dead_name_notification (device, receive_port, NULL);
  if (err)
    return err;

  /* On an error return the send right is still in the request, and the
     server loop destroys the request (glibc's mach_msg_server).  A
     deallocation here would be a second one; for a port that already
     has a filter it would take the reference that filter holds.  */
  return vnet_hurd_set_filter (&device->hurd, receive_port, priority,
                               filter, filterlen);
}

kern_return_t
ds_device_intr_register (vnet_port_t device, int id, int flags,
                         mach_port_t receive_port)
{
  (void) device;
  (void) id;
  (void) flags;
  (void) receive_port;
  return D_INVALID_OPERATION;
}

kern_return_t
ds_device_intr_ack (vnet_port_t device, mach_port_t receive_port)
{
  (void) device;
  (void) receive_port;
  return D_INVALID_OPERATION;
}

/* Serve forever.  Called with the device already brought up; never
   returns.  */
void
vnet_serve (struct vnet_dev *dev, const char *name)
{
  serving_dev = dev;
  virtio_serve_run (&vnet_serve_ops, dev, name);
}
