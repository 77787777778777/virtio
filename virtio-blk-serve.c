/*
 * virtio-blk-serve.c - MIG device server and trivfs translator for the
 * virtio-blk driver (plan Stage C, D3/S2/S3).
 *
 * Structure follows devnode + libmachdev:
 *
 *   - a trivfs node published with settrans accepts `device_open` on
 *     its file port, because the demuxer chains device_server_routine
 *     before trivfs_demuxer (devnode.c devnode_demuxer pattern);
 *   - each successful ds_device_open builds a fresh libports port in
 *     our own bucket wrapping a struct vblk_hurd handle, and hands the
 *     client a MOVE_SEND right to it (libmachdev machdev_create_device_port
 *     pattern, but serving block I/O instead of forwarding to the
 *     kernel);
 *   - subsequent ds_device_read/write/get_status calls resolve that
 *     port back to the handle via the DEVICE_* MIG mutations in
 *     mig-mutate.h, and run the plan's §5.1.1/§5.5 semantics through
 *     virtio-blk-hurd.c;
 *   - port lifetime is libports-managed: ports_get_send_right arms a
 *     no-senders notification (libports get-right.c), the demuxer runs
 *     notify_server_routine so mach_notify_no_senders reaches
 *     ports_do_mach_notify_no_senders, and the class's clean hook
 *     closes the underlying handle when the last client goes away.
 *
 * MIG stubs are generated from the cross prefix's
 * include/device/device.defs (subsystem 2800) and mach/notify.defs by
 * the Makefile, with mig-mutate.h wired in through -imacros and
 * -DMACH_PAYLOAD_TO_PORT=ports_payload_get_name, matching the Hurd
 * tree's Makeconf `%.sdefs.stamp' chain.
 */

#include <argz.h>
#include <error.h>
#include <fcntl.h>
#include <mach.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <device/device.h>
#include <hurd/ports.h>
#include <hurd/trivfs.h>

#include "virtio-blk-serve.h"

/* The MIG-generated server routines (deviceServer.c from
   include/device/device.defs, notifyServer.c from mach/notify.defs)
   dispatch into the ds_device_* handlers below; their headers appear
   here through the Makefile's include path (-I.).  */
#include "device_S.h"
#include "notify_S.h"

/* Trivfs hooks: the node is a pure device-open rendezvous, so no file
   I/O is supported; clients talk the Mach device protocol instead.  */
int trivfs_fstype = FSTYPE_MISC;
int trivfs_fsid = 0;
int trivfs_support_read = 0;
int trivfs_support_write = 0;
int trivfs_support_exec = 0;
int trivfs_allow_open = O_READ | O_WRITE;

struct port_bucket *vblk_server_bucket;
static struct port_class *vblk_trivfs_cntl_class;
static struct port_class *vblk_trivfs_protid_class;
struct port_class *vblk_server_class;

static struct trivfs_control *vblk_fsys;
/* The one device this translator owns (brought up before serving).  */
static struct vblk_dev *serving_dev;
/* Optional --name restriction on device_open; empty accepts all.  */
static const char *serving_name;

/* Class clean hook: runs on the final deref, i.e. after the no-senders
   notification (see libports no-senders.c -> ports_no_senders ->
   ports_port_deref) or after ports_destroy_right.  vblk_hurd_close is
   idempotent, so double teardown after an explicit device_close is
   harmless.  */
static void
vblk_clean_port (void *port)
{
  struct vblk_port *p = port;

  if (vblk_verbose)
    fprintf (stderr, "virtio-blk-serve: clean port %s\n", p->name);
  vblk_hurd_close (&p->hurd);
}

static int
vblk_demuxer (mach_msg_header_t *inp, mach_msg_header_t *outp)
{
  mig_routine_t routine;

  /* Log before demuxing: a message that matches none of the three
     routines is dropped by libports silently, so this is the only way
     to see e.g. an unexpected notify or a foreign msgh_id.  */
  if (vblk_verbose)
    fprintf (stderr, "virtio-blk-serve: demux id %d bits 0x%x size %u\n",
             inp->msgh_id, inp->msgh_bits, inp->msgh_size);

  /* device_open must beat trivfs_demuxer: both demux the same node
     port, and the file port would otherwise answer with a file error
     instead of reaching ds_device_open (devnode demuxer ordering).  */
  if ((routine = device_server_routine (inp)) ||
      (routine = notify_server_routine (inp)) ||
      (routine = NULL, trivfs_demuxer (inp, outp)))
    {
      if (routine)
        (*routine) (inp, outp);
      return TRUE;
    }
  return FALSE;
}

/* Notify interface.  Only no-senders carries teardown semantics: it is
   what closes the underlying handle when a client dies holding the
   device port (see vblk_clean_port); everything else is refused the
   way devnode does.  */
kern_return_t
do_mach_notify_no_senders (struct port_info *pi,
                           mach_port_mscount_t mscount)
{
  if (vblk_verbose)
    fprintf (stderr, "virtio-blk-serve: no-senders pi %p mscount %lu\n",
             pi, (unsigned long) mscount);
  return ports_do_mach_notify_no_senders (pi, mscount);
}

kern_return_t
do_mach_notify_port_deleted (struct port_info *pi, mach_port_t name)
{
  (void) pi;
  (void) name;
  return EOPNOTSUPP;
}

kern_return_t
do_mach_notify_msg_accepted (struct port_info *pi, mach_port_t name)
{
  (void) pi;
  (void) name;
  return EOPNOTSUPP;
}

kern_return_t
do_mach_notify_port_destroyed (struct port_info *pi, mach_port_t port)
{
  (void) pi;
  (void) port;
  return EOPNOTSUPP;
}

kern_return_t
do_mach_notify_send_once (struct port_info *pi)
{
  (void) pi;
  return EOPNOTSUPP;
}

kern_return_t
do_mach_notify_dead_name (struct port_info *pi, mach_port_t name)
{
  (void) pi;
  (void) name;
  return EOPNOTSUPP;
}

error_t
trivfs_append_args (struct trivfs_control *fsys, char **argz,
                    size_t *argz_len)
{
  error_t err = 0;

  if (serving_name)
    err = argz_add (argz, argz_len, "--name");
  if (!err && serving_name)
    err = argz_add (argz, argz_len, (char *) serving_name);
  err = err ?: argz_add (argz, argz_len, "--serve");
  return err;
}

void
trivfs_modify_stat (struct trivfs_protid *cred, io_statbuf_t *stat)
{
}

error_t
trivfs_goaway (struct trivfs_control *fsys, int flags)
{
  int count;

  /* Stop new requests, then count live clients (devnode goaway
     shape).  */
  ports_inhibit_class_rpcs (vblk_trivfs_cntl_class);
  ports_inhibit_class_rpcs (vblk_trivfs_protid_class);
  ports_inhibit_class_rpcs (vblk_server_class);

  count = ports_count_class (vblk_trivfs_protid_class);
  if (count && !(flags & FSYS_GOAWAY_FORCE))
    {
      ports_enable_class (vblk_trivfs_protid_class);
      ports_resume_class_rpcs (vblk_trivfs_cntl_class);
      ports_resume_class_rpcs (vblk_trivfs_protid_class);
      return EBUSY;
    }

  /* Plan §6.2 shutdown: quiesce the device (reset, IRQ uninstall,
     request drain) before the process exits.  */
  vblk_dev_quiesce (serving_dev);
  exit (0);
}

/* --- device interface ------------------------------------------------  */

kern_return_t
ds_device_open (mach_port_t master_port, mach_port_t reply_port,
                mach_msg_type_name_t reply_portPoly, dev_mode_t mode,
                const_dev_name_t name, mach_port_t *device,
                mach_msg_type_name_t *devicetype)
{
  struct vblk_port *p;
  error_t err;
  size_t size;

  (void) master_port;
  (void) reply_port;
  (void) reply_portPoly;

  if (vblk_verbose)
    fprintf (stderr, "virtio-blk-serve: open '%s' mode 0x%x\n", name, mode);

  if (serving_name && strcmp (serving_name, name) != 0)
    return D_NO_SUCH_DEVICE;

  /* One extra byte for the NUL: dev_name_t is NUL-terminated but the
     MIG stub does not count the terminator.  */
  size = sizeof *p + strlen (name) + 1;
  err = ports_create_port (vblk_server_class, vblk_server_bucket,
                           size, &p);
  if (err)
    return D_NO_MEMORY;
  strcpy (p->name, name);

  err = vblk_hurd_init (&p->hurd, serving_dev);
  if (err)
    {
      if (vblk_verbose)
        fprintf (stderr, "virtio-blk-serve: open '%s': init failed %d\n",
                 name, err);
      ports_destroy_right (p);
      return D_NO_MEMORY;
    }

  err = vblk_hurd_open (&p->hurd, mode);
  if (err)
    {
      if (vblk_verbose)
        fprintf (stderr, "virtio-blk-serve: open '%s': failed %d\n", name, err);
      ports_destroy_right (p);
      return err;
    }

  /* MOVE_SEND hands our reference to the client; libports armed the
     no-senders notification inside ports_get_send_right.  */
  *device = ports_get_send_right (p);
  *devicetype = MACH_MSG_TYPE_MOVE_SEND;
  if (vblk_verbose)
    fprintf (stderr, "virtio-blk-serve: open '%s' ok, port %d\n",
             name, *device);
  ports_port_deref (p);
  return 0;
}

kern_return_t
ds_device_open_new (mach_port_t master_port, mach_port_t reply_port,
                    mach_msg_type_name_t reply_portPoly, dev_mode_t mode,
                    const_dev_name_t name, mach_port_t *device,
                    mach_msg_type_name_t *devicetype)
{
  /* device_open_new is an alias everywhere else (gnumach, devnode,
     libmachdev); keep that behavior so a newer client cannot tell the
     difference.  */
  return ds_device_open (master_port, reply_port, reply_portPoly, mode,
                         name, device, devicetype);
}

kern_return_t
ds_device_close (struct vblk_port *device)
{
  if (vblk_verbose)
    fprintf (stderr, "virtio-blk-serve: close %p name %s\n",
             device, device ? device->name : "(null)");
  if (device == NULL)
    return D_NO_SUCH_DEVICE;

  vblk_hurd_close (&device->hurd);
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
  /* The write data arrives out of line and is ours to free (the MIG
     stub moves the pages into this task; see the eth-multiplexer
     ds_device_write note).  */
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
  mach_port_t bootstrap;
  error_t err;

  serving_dev = dev;
  serving_name = name;

  task_get_bootstrap_port (mach_task_self (), &bootstrap);
  if (bootstrap == MACH_PORT_NULL)
    error (1, 0, "--serve requires being started as a translator "
           "(settrans -c NODE /hurd/virtio-blk --serve)");

  vblk_server_bucket = ports_create_bucket ();
  vblk_server_class = ports_create_class (vblk_clean_port, 0);
  vblk_trivfs_cntl_class = ports_create_class (trivfs_clean_cntl, 0);
  vblk_trivfs_protid_class = ports_create_class (trivfs_clean_protid, 0);
  if (!vblk_server_bucket || !vblk_server_class
      || !vblk_trivfs_cntl_class || !vblk_trivfs_protid_class)
    error (1, ENOMEM, "cannot create port classes");

  err = trivfs_startup (bootstrap, 0, vblk_trivfs_cntl_class,
                        vblk_server_bucket, vblk_trivfs_protid_class,
                        vblk_server_bucket, &vblk_fsys);
  if (err)
    error (1, err, "trivfs_startup");

  mach_port_deallocate (mach_task_self (), bootstrap);

  /* devnode's loop: trivfs_goaway returns EBUSY while clients are
     attached and only exits (inside trivfs_goaway) when the translator
     is told to go away for good.  */
  do
    {
      ports_manage_port_operations_one_thread (vblk_server_bucket,
                                               vblk_demuxer, 0);
    }
  while (trivfs_goaway (vblk_fsys, 0) == EBUSY);

  exit (0);
}
