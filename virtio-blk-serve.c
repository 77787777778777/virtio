/*
 * virtio-blk-serve.c - MIG device handlers and vtable for the
 * virtio-blk driver (plan Stage C, D3/S2/S3).
 *
 * The translator scaffold (trivfs node, demuxer, notify plumbing, port
 * lifetime, goaway) lives in the shared virtio-serve.c; this file
 * supplies the blk-specific vtable and the ds_device_* handlers whose
 * signatures carry the driver's own port type:
 *
 *   - ds_device_open scaffolding is shared; handle_open here builds the
 *     struct vblk_hurd handle (init + mode policy), and the shared open
 *     hands the client a MOVE_SEND right to it (libmachdev
 *     machdev_create_device_port pattern, but serving block I/O instead
 *     of forwarding to the kernel);
 *   - subsequent ds_device_read/write/get_status calls resolve that
 *     port back to the handle via the DEVICE_* MIG mutations in
 *     mig-mutate.h, and run the plan's §5.1.1/§5.5 semantics through
 *     virtio-blk-hurd.c.
 *
 * MIG stubs are generated from the cross prefix's
 * include/device/device.defs (subsystem 2800) and mach/notify.defs by
 * the Makefile, with mig-mutate.h wired in through -imacros and
 * -DMACH_PAYLOAD_TO_PORT=ports_payload_get_name, matching the Hurd
 * tree's Makeconf `%.sdefs.stamp' chain.
 */

#include <mach.h>
#include <stdio.h>
#include <stddef.h>
#include <string.h>

#include <device/device.h>
#include <hurd/ports.h>

#include "virtio-blk-serve.h"
#include "virtio-serve.h"

/* The device_open name tail is written at port_size, so it must be the
   flexible array's offset exactly (virtio_serve.c's port-creation
   arithmetic depends on it).  */
_Static_assert (offsetof (struct vblk_port, name) == sizeof (struct vblk_port),
                "vblk_port name tail must start at the struct size");

/* The MIG-generated server routines (deviceServer.c from
   include/device/device.defs, notifyServer.c from mach/notify.defs)
   dispatch into the ds_device_* handlers below; their headers appear
   here through the Makefile's include path (-I.).  */
#include "device_S.h"
#include "notify_S.h"

/* The one device this translator owns (brought up before serving).  */
static struct vblk_dev *serving_dev;

static const char *
vblk_port_name (void *port)
{
  return ((struct vblk_port *) port)->name;
}

/* Per-device open: semantics-layer init and mode policy.  */
static error_t
vblk_handle_open (void *port, dev_mode_t mode)
{
  struct vblk_port *p = port;
  error_t err;

  err = vblk_hurd_init (&p->hurd, serving_dev);
  if (err)
    {
      if (vblk_verbose)
        fprintf (stderr, "virtio-blk-serve: open '%s': init failed %d\n",
                 p->name, err);
      return D_NO_MEMORY;
    }

  err = vblk_hurd_open (&p->hurd, mode);
  if (err)
    {
      if (vblk_verbose)
        fprintf (stderr, "virtio-blk-serve: open '%s': failed %d\n",
                 p->name, err);
      return err;
    }
  return 0;
}

/* Idempotent: also runs from the class clean hook, so double teardown
   after an explicit device_close is harmless.  */
static void
vblk_handle_close (void *port)
{
  struct vblk_port *p = port;

  vblk_hurd_close (&p->hurd);
}

/* dead_name stays NULL: this server never requests dead-name
   notifications, so libports' default behavior (interrupt notified
   RPCs) is what runs.  */

/* The generated stubs' dispatch: device_server_routine then
   notify_server_routine (both static inlines over per-binary id tables
   in the _S.h headers above).  */
static mig_routine_t
vblk_server_routine (mach_msg_header_t *inp)
{
  mig_routine_t routine;

  if ((routine = device_server_routine (inp)))
    return routine;
  return notify_server_routine (inp);
}

static const struct virtio_serve_ops vblk_serve_ops =
{
  .prefix = "virtio-blk-serve",
  .translator = "/hurd/virtio-blk",
  .verbose = &vblk_verbose,
  .port_size = sizeof (struct vblk_port),
  .port_name = vblk_port_name,
  .server_routine = vblk_server_routine,
  .handle_open = vblk_handle_open,
  .handle_close = vblk_handle_close,
  .quiesce = (void (*) (void *)) vblk_dev_quiesce,
  .dead_name = NULL,
};

/* --- device interface ------------------------------------------------  */

kern_return_t
ds_device_close (struct vblk_port *device)
{
  if (vblk_verbose)
    fprintf (stderr, "virtio-blk-serve: close %p name %s\n",
             device, device ? device->name : "(null)");
  if (device == NULL)
    return D_NO_SUCH_DEVICE;

  vblk_handle_close (device);
  ports_destroy_right (device);
  return 0;
}

kern_return_t
ds_device_write (vblk_port_t device, mach_port_t reply_port,
                 mach_msg_type_name_t reply_type, dev_mode_t mode,
                 recnum_t recnum, io_buf_ptr_t data,
                 mach_msg_type_number_t datalen, int *bytes_written)
{
  error_t err;

  if (device == NULL)
    return D_NO_SUCH_DEVICE;
  if (data == NULL)
    return D_INVALID_SIZE;

  err = vblk_hurd_write (&device->hurd, mode, recnum, data, datalen,
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
ds_device_read (vblk_port_t device, mach_port_t reply_port,
                mach_msg_type_name_t reply_type, dev_mode_t mode,
                recnum_t recnum, int bytes_wanted,
                io_buf_ptr_t *data, mach_msg_type_number_t *datalen)
{
  vm_address_t buf = 0;
  error_t err;
  int nread = 0;

  if (device == NULL)
    return D_NO_SUCH_DEVICE;
  if (bytes_wanted <= 0)
    return D_INVALID_SIZE;

  /* The reply carries the data out of line and deallocates it after
     the send (device.defs: `out data : io_buf_ptr_t, dealloc'), so it
     must live in its own vm region; read straight into it to keep a
     single copy on the path.  */
  err = vm_allocate (mach_task_self (), &buf, bytes_wanted, TRUE);
  if (err)
    return D_NO_MEMORY;

  err = vblk_hurd_read (&device->hurd, mode, recnum, (void *) buf,
                        bytes_wanted, &nread);
  if (vblk_verbose)
    fprintf (stderr, "virtio-blk-serve: read recnum %lu want %d -> err %d "
             "nread %d\n", (unsigned long) recnum, bytes_wanted, err, nread);
  if (err)
    {
      vm_deallocate (mach_task_self (), buf, bytes_wanted);
      return err;
    }

  *data = (io_buf_ptr_t) buf;
  *datalen = nread;
  return 0;
}

kern_return_t
ds_device_get_status (vblk_port_t device, dev_flavor_t flavor,
                      dev_status_t status,
                      mach_msg_type_number_t *statuslen)
{
  if (device == NULL)
    return D_NO_SUCH_DEVICE;

  return vblk_hurd_get_status (&device->hurd, flavor, status, statuslen);
}

kern_return_t
ds_device_set_status (vblk_port_t device, dev_flavor_t flavor,
                      dev_status_t status,
                      mach_msg_type_number_t statuslen)
{
  if (device == NULL)
    return D_NO_SUCH_DEVICE;

  return vblk_hurd_set_status (&device->hurd, flavor, status, statuslen);
}

kern_return_t
ds_device_write_inband (vblk_port_t device, mach_port_t reply_port,
                        mach_msg_type_name_t reply_type, dev_mode_t mode,
                        recnum_t recnum, const io_buf_ptr_inband_t data,
                        mach_msg_type_number_t datalen, int *bytes_written)
{
  (void) device;
  (void) reply_port;
  (void) reply_type;
  (void) mode;
  (void) recnum;
  (void) data;
  (void) datalen;
  *bytes_written = 0;
  /* IO_INBAND_MAX is 128 bytes, far below any useful block transfer;
     virtio-blk-hurd.c documents the same refusal for the raw layer.  */
  return D_INVALID_OPERATION;
}

kern_return_t
ds_device_read_inband (vblk_port_t device, mach_port_t reply_port,
                       mach_msg_type_name_t reply_type, dev_mode_t mode,
                       recnum_t recnum, int bytes_wanted,
                       io_buf_ptr_inband_t data,
                       mach_msg_type_number_t *datalen)
{
  (void) device;
  (void) reply_port;
  (void) reply_type;
  (void) mode;
  (void) recnum;
  (void) bytes_wanted;
  (void) data;
  *datalen = 0;
  return D_INVALID_OPERATION;
}

kern_return_t
ds_device_map (vblk_port_t device, vm_prot_t prot, vm_offset_t offset,
               vm_size_t size, memory_object_t *pager, int unmap)
{
  (void) device;
  (void) prot;
  (void) offset;
  (void) size;
  (void) pager;
  (void) unmap;
  return D_INVALID_OPERATION;
}

kern_return_t
ds_device_set_filter (vblk_port_t device, mach_port_t receive_port,
                      int priority, filter_array_t filter,
                      mach_msg_type_number_t filterlen)
{
  (void) device;
  (void) receive_port;
  (void) priority;
  (void) filter;
  (void) filterlen;
  return D_INVALID_OPERATION;
}

kern_return_t
ds_device_intr_register (vblk_port_t device, int id, int flags,
                         mach_port_t receive_port)
{
  (void) device;
  (void) id;
  (void) flags;
  (void) receive_port;
  return D_INVALID_OPERATION;
}

kern_return_t
ds_device_intr_ack (vblk_port_t device, mach_port_t receive_port)
{
  (void) device;
  (void) receive_port;
  return D_INVALID_OPERATION;
}

/* Serve forever.  Called with the device already brought up; never
   returns.  */
void
vblk_serve (struct vblk_dev *dev, const char *name)
{
  serving_dev = dev;
  virtio_serve_run (&vblk_serve_ops, dev, name);
}
