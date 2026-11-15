/* virtio-9p device translator: main, argp, trivfs glue.

   Installs as /dev/virtio-9p0 via `settrans -c /dev/virtio-9p0
   /hurd/virtio-9p --mount-tag=hostshare` and serves the raw 9P message
   stream over the virtio-9p PCI device, for the unmodified 9pfs translator
   on top.  Exit paths are clean: a guest booted without the -device flag
   must not leave a hung translator behind.  */

#include <argp.h>
#include <errno.h>
#include <error.h>
#include <fcntl.h>
#include <hurd.h>
#include <hurd/ports.h>
#include <hurd/trivfs.h>
#include <mach.h>
#include <mach/gnumach.h>
#include <mach/port.h>
#include <mach/vm_param.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/types.h>
#include <unistd.h>

#include "virtio-9p.h"

const char *argp_program_version = "virtio-9p 0.1";

static const char *opt_device;
static const char *opt_mount_tag = V9P_DEFAULT_MOUNT_TAG;
static int opt_selftest;

static const struct argp_option options[] =
{
  {"device", 'd', "DOM:BUS:DEV.FN", 0,
   "Use the virtio-9p device at this PCI slot instead of the first match"},
  {"mount-tag", 't', "NAME", 0,
   "Require the device's mount tag to be NAME (default hostshare)"},
  {"selftest", 's', 0, 0,
   "Run the Phase 1 device selftest and exit"},
  {0}
};

static error_t
parse_opt (int key, char *arg, struct argp_state *state)
{
  switch (key)
    {
    case 'd':
      opt_device = arg;
      break;
    case 't':
      opt_mount_tag = arg;
      break;
    case 's':
      opt_selftest = 1;
      break;
    default:
      return ARGP_ERR_UNKNOWN;
    }
  return 0;
}

static const struct argp argp =
{
  options, parse_opt,
  "[--device=SLOT] [--mount-tag=NAME] [--selftest]",
  "Virtio-9p device translator: serves the 9P message stream of the QEMU "
  "virtio-9p device on a character device node"
};

/* The node is opened once and the fd dup'ed into a reader and a writer
   stream (that is how 9pfs drives it), so a second open is refused.  The
   peropen hooks track the open count.  */
static int open_count;

static error_t
check_open_hook (struct trivfs_control *cntl, struct iouser *user, int flags)
{
  if (open_count > 0)
    return EBUSY;
  return 0;
}

static error_t
peropen_create (struct trivfs_peropen *po)
{
  open_count++;
  return 0;
}

static void
peropen_destroy (struct trivfs_peropen *po)
{
  open_count--;
}

int trivfs_fstype = FSTYPE_DEV;
int trivfs_fsid = 0;

int trivfs_support_read = 1;
int trivfs_support_write = 1;
int trivfs_support_exec = 0;

int trivfs_allow_open = O_READ | O_WRITE;

void
trivfs_modify_stat (struct trivfs_protid *cred, io_statbuf_t *st)
{
  st->st_blksize = vm_page_size * 256;
  st->st_size = 0;
  st->st_blocks = 0;
  st->st_mode &= ~S_IFMT;
  st->st_mode |= S_IFCHR;
}

error_t
trivfs_goaway (struct trivfs_control *fsys, int flags)
{
  exit (0);
}

kern_return_t
trivfs_S_io_map (struct trivfs_protid *cred,
                 mach_port_t reply, mach_msg_type_name_t replytype,
                 memory_object_t *rdobj,
                 mach_msg_type_name_t *rdtype,
                 memory_object_t *wrobj,
                 mach_msg_type_name_t *wrtype)
{
  return EOPNOTSUPP;
}

kern_return_t
trivfs_S_io_read (struct trivfs_protid *cred,
                  mach_port_t reply, mach_msg_type_name_t replytype,
                  data_t *data, mach_msg_type_number_t *dataCnt,
                  loff_t offs, vm_size_t amount)
{
  ssize_t n;
  int mapped = 0;

  if (!cred)
    return EOPNOTSUPP;
  else if (!(cred->po->openmodes & O_READ))
    return EBADF;
  else if (amount == 0)
    {
      *dataCnt = 0;
      *data = NULL;
      return 0;
    }
  else if (*dataCnt < amount)
    {
      /* Replace the stub-supplied buffer with one of the requested size.
         The incoming buffer is MiG reply scratch, not vm_allocate memory:
         deallocating it would unmap the serving thread's own stack, so it
         is simply dropped, exactly as trans/random.c does.  */
      *data = mmap (0, amount, PROT_READ | PROT_WRITE, MAP_ANON, 0, 0);
      if (*data == MAP_FAILED)
        {
          *data = NULL;
          *dataCnt = 0;
          return errno;
        }
      mapped = 1;
    }

  n = v9p_ios_read (*data, amount);
  if (n < 0)
    {
      if (mapped)
        munmap (*data, amount);
      *dataCnt = 0;
      return errno;
    }
  *dataCnt = n;
  return 0;
}

kern_return_t
trivfs_S_io_readable (struct trivfs_protid *cred,
                      mach_port_t reply, mach_msg_type_name_t replytype,
                      vm_size_t *amount)
{
  if (!cred)
    return EOPNOTSUPP;
  else if (!(cred->po->openmodes & O_READ))
    return EBADF;
  *amount = v9p_ios_can_read () ? vm_page_size : 0;
  return 0;
}

kern_return_t
trivfs_S_io_write (struct trivfs_protid *cred,
                   mach_port_t reply, mach_msg_type_name_t replytype,
                   const_data_t data, mach_msg_type_number_t dataCnt,
                   loff_t offs, vm_size_t *amount)
{
  ssize_t n;

  if (!cred)
    return EOPNOTSUPP;
  else if (!(cred->po->openmodes & O_WRITE))
    return EBADF;

  n = v9p_ios_write (data, dataCnt);
  if (n < 0)
    return errno;
  *amount = n;
  return 0;
}

kern_return_t
trivfs_S_io_seek (struct trivfs_protid *cred,
                  mach_port_t reply, mach_msg_type_name_t replytype,
                  off_t offset, int whence, off_t *new_offset)
{
  if (!cred)
    return EOPNOTSUPP;
  *new_offset = 0;
  return ESPIPE;
}

kern_return_t
trivfs_S_io_select (struct trivfs_protid *cred,
                    mach_port_t reply, mach_msg_type_name_t replytype,
                    int *type)
{
  int t;

  if (!cred)
    return EOPNOTSUPP;
  t = *type & (SELECT_READ | SELECT_WRITE);
  if ((t & SELECT_READ) && !v9p_ios_can_read ())
    t &= ~SELECT_READ;
  if ((t & SELECT_WRITE) && !v9p_ios_can_write ())
    t &= ~SELECT_WRITE;
  *type = t;
  return 0;
}

kern_return_t
trivfs_S_io_select_timeout (struct trivfs_protid *cred,
                            mach_port_t reply, mach_msg_type_name_t replytype,
                            struct timespec ts, int *type)
{
  return trivfs_S_io_select (cred, reply, replytype, type);
}

kern_return_t
trivfs_S_file_set_size (struct trivfs_protid *cred,
                        mach_port_t reply, mach_msg_type_name_t replytype,
                        loff_t size)
{
  if (!cred)
    return EOPNOTSUPP;
  return size < 0 ? EINVAL : 0;
}

kern_return_t
trivfs_S_io_get_openmodes (struct trivfs_protid *cred,
                           mach_port_t reply, mach_msg_type_name_t replytype,
                           int *bits)
{
  if (!cred)
    return EOPNOTSUPP;
  *bits = cred->po->openmodes;
  return 0;
}

kern_return_t
trivfs_S_io_set_all_openmodes (struct trivfs_protid *cred,
                               mach_port_t reply, mach_msg_type_name_t replytype,
                               int mode)
{
  if (!cred)
    return EOPNOTSUPP;
  return 0;
}

kern_return_t
trivfs_S_io_set_some_openmodes (struct trivfs_protid *cred,
                                mach_port_t reply, mach_msg_type_name_t replytype,
                                int bits)
{
  if (!cred)
    return EOPNOTSUPP;
  return 0;
}

kern_return_t
trivfs_S_io_clear_some_openmodes (struct trivfs_protid *cred,
                                  mach_port_t reply, mach_msg_type_name_t replytype,
                                  int bits)
{
  if (!cred)
    return EOPNOTSUPP;
  return 0;
}

kern_return_t
trivfs_S_io_get_owner (struct trivfs_protid *cred,
                       mach_port_t reply, mach_msg_type_name_t replytype,
                       pid_t *owner)
{
  if (!cred)
    return EOPNOTSUPP;
  *owner = 0;
  return 0;
}

kern_return_t
trivfs_S_io_mod_owner (struct trivfs_protid *cred,
                       mach_port_t reply, mach_msg_type_name_t replytype,
                       pid_t owner)
{
  if (!cred)
    return EOPNOTSUPP;
  return EINVAL;
}

static error_t
v9p_start_device (void)
{
  error_t err;

  err = v9p_pci_probe (opt_device);
  if (err)
    return err;
  err = v9p_pci_init ();
  if (err)
    return err;
  err = v9p_queue_setup ();
  if (err)
    return err;
  err = v9p_pci_driver_ok ();
  if (err)
    return err;
  err = v9p_irq_install ();
  if (err)
    return err;
  return v9p_ios_init ();
}

int
main (int argc, char **argv)
{
  error_t err;
  mach_port_t bootstrap;
  struct trivfs_control *fsys;

  argp_parse (&argp, argc, argv, 0, 0, 0);

  if (opt_selftest)
    {
      err = v9p_start_device ();
      if (err)
        error (1, err, "device bring-up failed");
      err = v9p_selftest (opt_mount_tag);
      if (err)
        error (1, err, "selftest failed");
      printf ("selftest ok\n");
      return 0;
    }

  err = v9p_start_device ();
  if (err)
    /* Die before trivfs_startup replies, so that settrans and the open of
       the node fail visibly instead of leaving a dead translator behind.  */
    error (4, err, "virtio-9p device not usable");

  task_get_bootstrap_port (mach_task_self (), &bootstrap);
  if (bootstrap == MACH_PORT_NULL)
    error (1, 0, "Must be started as a translator");

  err = trivfs_startup (bootstrap, 0, 0, 0, 0, 0, &fsys);
  mach_port_deallocate (mach_task_self (), bootstrap);
  if (err)
    error (3, err, "Contacting parent");

  trivfs_check_open_hook = check_open_hook;
  trivfs_peropen_create_hook = peropen_create;
  trivfs_peropen_destroy_hook = peropen_destroy;

  ports_manage_port_operations_multithread (fsys->pi.bucket, trivfs_demuxer,
                                            2 * 60 * 1000, 0, 0);
  return 0;
}
