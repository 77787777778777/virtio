/* virtio-serve.c - shared Mach-device translator scaffold for the
   virtio drivers (the blk and net `--serve` mode).

   Structure follows devnode + libmachdev:

   - a trivfs node published with settrans accepts `device_open` on
     its file port, because the demuxer chains device_server_routine
     before trivfs_demuxer (devnode.c devnode_demuxer pattern); the
     network stacks reach it with file_name_lookup + device_open;
   - each successful ds_device_open (implemented here, since its
     signature carries no driver-specific type) builds a fresh libports
     port in the shared bucket wrapping the driver's per-open handle,
     via the ops vtable, and the caller hands the client a MOVE_SEND
     right to it (ports_get_send_right arms the no-senders
     notification);
   - port lifetime is libports-managed: the class clean hook closes the
     handle when the last reference drops, and a client's registered
     filters die with its receive ports through the dead-name
     notification the drivers request at set_filter time.

   The driver-specific MIG handlers (whose first argument is the
   driver's own port type) stay in the driver's serve file; the stubs
   resolve that argument through each driver's begin_using_device_*
   mutations over the shared bucket/class.  */

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

#include "virtio-serve.h"

/* Trivfs hooks: the node is a pure device-open rendezvous, so no file
   I/O is supported; clients talk the Mach device protocol instead.  */
int trivfs_fstype = FSTYPE_MISC;
int trivfs_fsid = 0;
int trivfs_support_read = 0;
int trivfs_support_write = 0;
int trivfs_support_exec = 0;
int trivfs_allow_open = O_READ | O_WRITE;

struct port_bucket *virtio_serve_bucket;
static struct port_class *virtio_serve_trivfs_cntl_class;
static struct port_class *virtio_serve_trivfs_protid_class;
struct port_class *virtio_serve_class;

static struct trivfs_control *virtio_serve_fsys;
/* The vtable and the one device this translator owns (brought up
   before serving), set by virtio_serve_run.  */
static const struct virtio_serve_ops *serve_ops;
static void *serving_dev;
/* Optional --name restriction on device_open; empty accepts all.  */
static const char *serving_name;

/* Class clean hook: runs on the final deref, i.e. after the no-senders
   notification or after ports_destroy_right.  The driver's handle_close
   is idempotent, so double teardown after an explicit device_close is
   harmless.  */
static void
virtio_serve_clean_port (void *port)
{
  if (serve_ops->verbose && *serve_ops->verbose)
    fprintf (stderr, "%s: clean port %s\n", serve_ops->prefix,
             serve_ops->port_name (port));
  serve_ops->handle_close (port);
}

static int
virtio_serve_demuxer (mach_msg_header_t *inp, mach_msg_header_t *outp)
{
  mig_routine_t routine;

  /* Log before demuxing: a message that matches none of the three
     routines is dropped by libports silently, so this is the only way
     to see e.g. an unexpected notify or a foreign msgh_id.  */
  if (serve_ops->verbose && *serve_ops->verbose)
    fprintf (stderr, "%s: demux id %d bits 0x%x size %u\n",
             serve_ops->prefix, inp->msgh_id, inp->msgh_bits,
             inp->msgh_size);

  /* device_open must beat trivfs_demuxer: both demux the same node
     port, and the file port would otherwise answer with a file error
     instead of reaching ds_device_open (devnode demuxer ordering).  */
  if ((routine = serve_ops->server_routine (inp)) ||
      (routine = NULL, trivfs_demuxer (inp, outp)))
    {
      if (routine)
        (*routine) (inp, outp);
      return TRUE;
    }
  return FALSE;
}

/* Notify interface.  no-senders carries teardown semantics; dead-name
   reaches ports_do_mach_notify_dead_name, which dispatches into the
   ports_dead_name override below.  */
kern_return_t
do_mach_notify_no_senders (struct port_info *pi,
                           mach_port_mscount_t mscount)
{
  if (serve_ops->verbose && *serve_ops->verbose)
    fprintf (stderr, "%s: no-senders pi %p mscount %lu\n",
             serve_ops->prefix, pi, (unsigned long) mscount);
  return ports_do_mach_notify_no_senders (pi, mscount);
}

kern_return_t
do_mach_notify_dead_name (struct port_info *pi, mach_port_t name)
{
  return ports_do_mach_notify_dead_name (pi, name);
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

/* Override of libports' weak default: the notify argument is libports'
   notify-port info, which does not identify which client registered a
   dead-name request, so drivers that need port-specific cleanup (the
   net filter walk) get it through ops->dead_name; the blk server keeps
   the default interrupt-notified-RPCs behavior by passing NULL.  */
void
ports_dead_name (void *notify, mach_port_t dead_name)
{
  if (serve_ops->dead_name)
    {
      if (serve_ops->verbose && *serve_ops->verbose)
        fprintf (stderr, "%s: dead name %d\n", serve_ops->prefix,
                 dead_name);
      serve_ops->dead_name (dead_name);
    }
  else
    ports_interrupt_notified_rpcs (notify, dead_name,
                                   MACH_NOTIFY_DEAD_NAME);
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
  ports_inhibit_class_rpcs (virtio_serve_trivfs_cntl_class);
  ports_inhibit_class_rpcs (virtio_serve_trivfs_protid_class);
  ports_inhibit_class_rpcs (virtio_serve_class);

  count = ports_count_class (virtio_serve_trivfs_protid_class);
  if (count && !(flags & FSYS_GOAWAY_FORCE))
    {
      ports_enable_class (virtio_serve_trivfs_protid_class);
      ports_resume_class_rpcs (virtio_serve_trivfs_cntl_class);
      ports_resume_class_rpcs (virtio_serve_trivfs_protid_class);
      /* The device class was inhibited too; leaving it inhibited after
         a refused goaway would block every device RPC until a forced
         teardown (ports_resume undoes ports_inhibit_class_rpcs;
         ports_enable_class does not).  */
      ports_resume_class_rpcs (virtio_serve_class);
      return EBUSY;
    }

  /* Plan shutdown: quiesce the device (reset, IRQ uninstall, buffer
     pools) before the process exits.  */
  serve_ops->quiesce (serving_dev);
  exit (0);
}

/* --- device interface ------------------------------------------------  */

void *
virtio_serve_open_port (const char *name, dev_mode_t mode, error_t *err)
{
  struct port_info *p;
  error_t e;
  size_t size;

  if (serving_name && strcmp (serving_name, name) != 0)
    {
      *err = D_NO_SUCH_DEVICE;
      return NULL;
    }

  /* One extra byte for the NUL: dev_name_t is NUL-terminated but the
     MIG stub does not count the terminator.  */
  size = serve_ops->port_size + strlen (name) + 1;
  e = ports_create_port (virtio_serve_class, virtio_serve_bucket,
                         size, &p);
  if (e)
    {
      *err = D_NO_MEMORY;
      return NULL;
    }
  strcpy ((char *) p + serve_ops->port_size, name);

  e = serve_ops->handle_open (p, mode);
  if (e)
    {
      /* ports_destroy_right gives up the receive right only.  The
         reference that ports_create_port returned is still ours, and
         nothing else would drop it.  */
      ports_destroy_right (p);
      ports_port_deref (p);
      *err = e;
      return NULL;
    }
  return p;
}

kern_return_t
ds_device_open (mach_port_t master_port, mach_port_t reply_port,
                mach_msg_type_name_t reply_portPoly, dev_mode_t mode,
                const_dev_name_t name, mach_port_t *device,
                mach_msg_type_name_t *devicetype)
{
  void *p;
  error_t err;

  (void) master_port;
  (void) reply_port;
  (void) reply_portPoly;

  if (serve_ops->verbose && *serve_ops->verbose)
    fprintf (stderr, "%s: open '%s' mode 0x%x\n", serve_ops->prefix,
             name, mode);

  p = virtio_serve_open_port (name, mode, &err);
  if (!p)
    return err;

  /* MOVE_SEND hands our reference to the client; libports armed the
     no-senders notification inside ports_get_send_right.  */
  *device = ports_get_send_right (p);
  *devicetype = MACH_MSG_TYPE_MOVE_SEND;
  if (serve_ops->verbose && *serve_ops->verbose)
    fprintf (stderr, "%s: open '%s' ok, port %d\n", serve_ops->prefix,
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

/* Serve forever.  Called with the device already brought up; never
   returns.  */
void
virtio_serve_run (const struct virtio_serve_ops *ops, void *dev,
                  const char *name)
{
  mach_port_t bootstrap;
  error_t err;

  serve_ops = ops;
  serving_dev = dev;
  serving_name = name;

  task_get_bootstrap_port (mach_task_self (), &bootstrap);
  if (bootstrap == MACH_PORT_NULL)
    error (1, 0, "--serve requires being started as a translator "
           "(settrans -c NODE %s --serve)", ops->translator);

  virtio_serve_bucket = ports_create_bucket ();
  virtio_serve_class = ports_create_class (virtio_serve_clean_port, 0);
  virtio_serve_trivfs_cntl_class = ports_create_class (trivfs_clean_cntl, 0);
  virtio_serve_trivfs_protid_class =
    ports_create_class (trivfs_clean_protid, 0);
  if (!virtio_serve_bucket || !virtio_serve_class
      || !virtio_serve_trivfs_cntl_class
      || !virtio_serve_trivfs_protid_class)
    error (1, ENOMEM, "cannot create port classes");

  err = trivfs_startup (bootstrap, 0, virtio_serve_trivfs_cntl_class,
                        virtio_serve_bucket,
                        virtio_serve_trivfs_protid_class,
                        virtio_serve_bucket, &virtio_serve_fsys);
  if (err)
    error (1, err, "trivfs_startup");

  mach_port_deallocate (mach_task_self (), bootstrap);

  /* devnode's loop: trivfs_goaway returns EBUSY while clients are
     attached and only exits (inside trivfs_goaway) when the translator
     is told to go away for good.  */
  do
    {
      ports_manage_port_operations_one_thread (virtio_serve_bucket,
                                               virtio_serve_demuxer, 0);
    }
  while (trivfs_goaway (virtio_serve_fsys, 0) == EBUSY);

  exit (0);
}
