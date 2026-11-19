/* vblk-client: direct Mach device RPC harness client for virtio-blk.

   This client exercises size query, sector reads, and disposable
   writes/flush directly over the Mach device RPCs.  It bypasses libstore
   on purpose: storeio maps D_* errors through dev_error into errnos
   (hurd/libstore/device.c), which would lose the exact codes the
   semantics layer is supposed to return, while this client asserts them
   raw off the RPCs.

   Usage (NODE is a translator node published by `virtio-blk --serve`):

       vblk-client status NODE            DEV_GET_SIZE/DEV_GET_RECORDS
       vblk-client read NODE REC COUNT    dump COUNT records to stdout
       vblk-client write NODE REC FILE    write FILE (whole records), flush,
                                          read back and compare
       vblk-client check NODE             negative-contract assertions

   Exit status: 0 all assertions passed, 1 otherwise.  */

#include <argp.h>
#include <limits.h>
#include <errno.h>
#include <error.h>
#include <fcntl.h>
#include <hurd.h>
#include <mach.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <device/device.h>

static const char *opt_name;   /* device_open name ("" by default) */
static int opt_readonly;

static const struct argp_option options[] =
{
  {"name", 'n', "NAME", 0, "device_open name (default: empty, which the "
   "translator accepts when started without --name)"},
  {"readonly", 'r', 0, 0, "Open D_READ only (for write-refusal checks)"},
  {0}
};

static error_t
parse_opt (int key, char *arg, struct argp_state *state)
{
  switch (key)
    {
    case 'n':
      opt_name = arg;
      break;
    case 'r':
      opt_readonly = 1;
      break;
    default:
      return ARGP_ERR_UNKNOWN;
    }
  (void) state;
  return 0;
}

static const struct argp argp =
{
  options, parse_opt,
  "status NODE | read NODE REC COUNT | write NODE REC FILE | check NODE",
  "Direct Mach device RPC client for the virtio-blk device translator"
};

/* libstore maps these to errnos (hurd/libstore/device.c dev_error); the
   client prints the raw codes so the semantics layer's choices stay
   visible in the harness log.  */
static const char *
d_strerror (error_t err)
{
  switch (err)
    {
    case 0:                     return "success";
    case D_IO_ERROR:            return "D_IO_ERROR";
    case D_WOULD_BLOCK:         return "D_WOULD_BLOCK";
    case D_NO_SUCH_DEVICE:      return "D_NO_SUCH_DEVICE";
    case D_ALREADY_OPEN:        return "D_ALREADY_OPEN";
    case D_INVALID_OPERATION:   return "D_INVALID_OPERATION";
    case D_INVALID_RECNUM:      return "D_INVALID_RECNUM";
    case D_INVALID_SIZE:        return "D_INVALID_SIZE";
    case D_NO_MEMORY:           return "D_NO_MEMORY";
    case D_READ_ONLY:           return "D_READ_ONLY";
    case D_DEVICE_DOWN:         return "D_DEVICE_DOWN";
    default:                    return "unknown D_* code";
    }
}

static int failures;

static void
record (const char *what, int ok, const char *detail)
{
  printf ("%s: %s%s%s\n", ok ? "PASS" : "FAIL", what,
          detail ? ": " : "", detail ? detail : "");
  if (!ok)
    failures++;
}

/* Open NODE as a Mach device through the translator: the node's file
   port is what libstore's dopen passes as the device_open master port
   in its `@master:rest' form (hurd/libstore/device.c), and the
   translator's demuxer answers device_open on it.  MODE selects
   D_READ / D_READ|D_WRITE exactly like the store layer's open.  */
static error_t
open_device_mode (const char *node, int mode, device_t *dev)
{
  file_t file;
  error_t err;

  file = file_name_lookup (node, O_READ | ((mode & D_WRITE) ? O_WRITE : 0),
                           0);
  if (file == MACH_PORT_NULL)
    error (1, errno, "cannot look up %s", node);

  err = device_open (file, mode, opt_name ?: "", dev);
  mach_port_deallocate (mach_task_self (), file);
  return err;
}

static error_t
open_device (const char *node, device_t *dev)
{
  return open_device_mode (node, opt_readonly ? D_READ : (D_READ | D_WRITE),
                           dev);
}

static error_t
open_device_ro (const char *node, device_t *dev)
{
  return open_device_mode (node, D_READ, dev);
}

static int
cmd_status (const char *node)
{
  device_t dev;
  dev_status_data_t sizes;
  mach_msg_type_number_t count;
  error_t err;

  err = open_device (node, &dev);
  if (err)
    error (1, 0, "device_open %s: %s", node, d_strerror (err));

  count = DEV_STATUS_MAX;
  err = device_get_status (dev, DEV_GET_RECORDS, sizes, &count);
  if (err)
    error (1, 0, "DEV_GET_RECORDS: %s", d_strerror (err));
  if (count != DEV_GET_RECORDS_COUNT)
    error (1, 0, "DEV_GET_RECORDS returned %u ints, want %u",
           count, DEV_GET_RECORDS_COUNT);
  printf ("records=%d record_size=%d\n", sizes[0], sizes[1]);

  count = DEV_STATUS_MAX;
  err = device_get_status (dev, DEV_GET_SIZE, sizes, &count);
  if (err)
    error (1, 0, "DEV_GET_SIZE: %s", d_strerror (err));
  if (count != DEV_GET_SIZE_COUNT)
    error (1, 0, "DEV_GET_SIZE returned %u ints, want %u",
           count, DEV_GET_SIZE_COUNT);
  printf ("size=%d\n", sizes[0]);

  mach_port_deallocate (mach_task_self (), dev);
  return 0;
}

static int
cmd_read (const char *node, recnum_t recnum, int count)
{
  device_t dev;
  io_buf_ptr_t buf;
  mach_msg_type_number_t nread;
  dev_status_data_t sizes;
  mach_msg_type_number_t scount = DEV_STATUS_MAX;
  int recsize;
  error_t err;

  err = open_device (node, &dev);
  if (err)
    error (1, 0, "device_open %s: %s", node, d_strerror (err));

  /* COUNT is in records, the Mach block unit; the RPC wants bytes.  */
  err = device_get_status (dev, DEV_GET_RECORDS, sizes, &scount);
  if (err || scount != DEV_GET_RECORDS_COUNT)
    error (1, 0, "DEV_GET_RECORDS: %s", d_strerror (err));
  recsize = sizes[1];
  if (recsize <= 0 || count > INT_MAX / recsize)
    error (1, EOVERFLOW, "record size %d x count %d", recsize, count);

  err = device_read (dev, 0, recnum, count * recsize, &buf, &nread);
  if (err)
    error (1, 0, "device_read at %lu: %s", (unsigned long) recnum,
           d_strerror (err));
  if (nread > 0 && fwrite (buf, 1, nread, stdout) != nread)
    error (1, errno, "stdout");
  if (nread > 0)
    vm_deallocate (mach_task_self (), (vm_address_t) buf, nread);
  return 0;
}

static int
cmd_write (const char *node, recnum_t recnum, const char *path)
{
  device_t dev;
  char *data;
  ssize_t got;
  off_t size;
  int fd, written = 0;
  io_buf_ptr_t back;
  mach_msg_type_number_t nread;
  error_t err;

  fd = open (path, O_RDONLY);
  if (fd < 0)
    error (1, errno, "open %s", path);
  size = lseek (fd, 0, SEEK_END);
  lseek (fd, 0, SEEK_SET);
  if (size <= 0)
    error (1, EINVAL, "%s is empty", path);
  data = malloc (size);
  if (!data)
    error (1, ENOMEM, "read buffer");
  got = read (fd, data, size);
  if (got != size)
    error (1, errno ?: EIO, "read %s", path);
  close (fd);

  err = open_device (node, &dev);
  if (err)
    error (1, 0, "device_open %s: %s", node, d_strerror (err));

  err = device_write (dev, 0, recnum, data, size, &written);
  if (err)
    error (1, 0, "device_write at %lu: %s", (unsigned long) recnum,
           d_strerror (err));
  if (written != size)
    error (1, EIO, "short write: %d of %ld bytes", written, (long) size);
  printf ("wrote %d bytes at record %lu\n", written,
          (unsigned long) recnum);

  err = device_set_status (dev, DEV_FLUSH_CACHE, NULL, 0);
  if (err)
    error (1, 0, "DEV_FLUSH_CACHE: %s", d_strerror (err));

  err = device_read (dev, 0, recnum, size, &back, &nread);
  if (err)
    error (1, 0, "read back: %s", d_strerror (err));
  if (nread != (mach_msg_type_number_t) size
      || memcmp (back, data, size) != 0)
    error (1, EIO, "read back does not match what was written");
  vm_deallocate (mach_task_self (), (vm_address_t) back, nread);

  printf ("flush and read-back ok\n");
  free (data);
  return 0;
}

static int
expect (const char *what, error_t got, error_t want)
{
  char detail[128];

  snprintf (detail, sizeof detail, "got %s (%d), want %s (%d)",
            d_strerror (got), got, d_strerror (want), want);
  record (what, got == want, detail);
  return got == want ? 0 : 1;
}

/* The negative half of the baseline, exercised through
   real RPCs: bounds, alignment, unknown flavors, the unimplemented
   surface, and read-only refusal.  */
static int
cmd_check (const char *node)
{
  device_t dev, dev_ro;
  dev_status_data_t sizes;
  mach_msg_type_number_t count;
  char buf[512];
  int written = 0;
  unsigned int recsize, records;
  error_t err;

  err = open_device (node, &dev);
  if (err)
    error (1, 0, "device_open %s: %s", node, d_strerror (err));

  count = DEV_STATUS_MAX;
  err = device_get_status (dev, DEV_GET_RECORDS, sizes, &count);
  if (err)
    error (1, 0, "DEV_GET_RECORDS: %s", d_strerror (err));
  records = sizes[0];
  recsize = sizes[1];
  count = DEV_STATUS_MAX;
  err = device_get_status (dev, DEV_GET_SIZE, sizes, &count);
  expect ("records/size flavor agrees with DEV_GET_SIZE",
          (err == 0 && count == DEV_GET_SIZE_COUNT) ? 0 : D_INVALID_OPERATION,
          0);

  {
    io_buf_ptr_t back;
    mach_msg_type_number_t nread;

    /* One past the last record must be refused before any I/O.  */
    err = device_read (dev, 0, records, recsize, &back, &nread);
    expect ("read one past the last record refused",
            err, D_INVALID_RECNUM);

    /* A transfer starting inside the device but running past its end
       must be refused too (libstore's enforced() math relies on the
       size flavors, the RPC layer must not truncate silently).  */
    err = device_read (dev, 0, records - 1, recsize * 2, &back, &nread);
    expect ("read crossing the device end refused",
            err, D_INVALID_RECNUM);

    err = device_read (dev, 0, 0, recsize / 2 ?: 1, &back, &nread);
    expect ("misaligned read size refused",
            err, D_INVALID_SIZE);
  }

  count = DEV_STATUS_MAX;
  err = device_get_status (dev, 0x1234, sizes, &count);
  expect ("unknown get_status flavor refused",
          err, D_INVALID_OPERATION);

  err = device_set_status (dev, 0x1234, NULL, 0);
  expect ("unknown set_status flavor refused",
          err, D_INVALID_OPERATION);

  {
    char inbuf[256];
    mach_msg_type_number_t inlen;

    err = device_read_inband (dev, 0, 0, sizeof inbuf, inbuf, &inlen);
    expect ("inband read refused (IO_INBAND_MAX too small for blocks)",
            err, D_INVALID_OPERATION);
  }

  {
    mach_port_t pager;

    err = device_map (dev, VM_PROT_READ, 0, 0, &pager, 0);
    expect ("device_map refused (no pager)",
            err, D_INVALID_OPERATION);
  }

  /* A second open is a fresh handle: multiple opens are legal (the
     D_ALREADY_OPEN rule guards one handle's open state, not the
     device).  A read-only open must refuse writes.  */
  err = open_device_ro (node, &dev_ro);
  if (err)
    error (1, 0, "read-only device_open: %s", d_strerror (err));
  err = device_write (dev_ro, 0, 0, buf, recsize, &written);
  expect ("write through a read-only open refused",
          err, D_READ_ONLY);
  mach_port_deallocate (mach_task_self (), dev_ro);

  err = device_close (dev);
  expect ("device_close succeeds", err, 0);

  printf ("%d checks, %d failures\n", 10, failures);
  return failures ? 1 : 0;
}

int
main (int argc, char **argv)
{
  int argi;

  argp_parse (&argp, argc, argv, ARGP_IN_ORDER, &argi, 0);
  if (argi >= argc)
    error (1, EINVAL, "missing subcommand (status|read|write|check)");

  if (strcmp (argv[argi], "status") == 0 && argi + 2 == argc)
    return cmd_status (argv[argi + 1]);
  if (strcmp (argv[argi], "read") == 0 && argi + 4 == argc)
    return cmd_read (argv[argi + 1], strtoul (argv[argi + 2], 0, 0),
                     atoi (argv[argi + 3]));
  if (strcmp (argv[argi], "write") == 0 && argi + 4 == argc)
    return cmd_write (argv[argi + 1], strtoul (argv[argi + 2], 0, 0),
                      argv[argi + 3]);
  if (strcmp (argv[argi], "check") == 0 && argi + 2 == argc)
    return cmd_check (argv[argi + 1]);

  argp_help (&argp, stderr, ARGP_HELP_STD_USAGE, argv[0]);
  return 2;
}
