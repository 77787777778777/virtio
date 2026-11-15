/* virtio-balloon driver: main, argp, lifecycle, selftest, convergence.

   A standalone long-lived daemon, unlike the virtio-9p translator:
   ballooning is driven entirely through the device config region and the
   two PFN virtqueues, so there is no device node and no trivfs front end,
   and the 9p translator is not on this path.  Exit paths are clean: a guest
   booted without -device virtio-balloon-pci must produce a diagnostic, not
   a hung daemon.  */

#include <argp.h>
#include <error.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "virtio-balloon.h"

const char *argp_program_version = "virtio-balloon 0.1";

int vballoon_verbose;

/* Long-only options need their own keys, and argp passes positional
   arguments through parse_opt with ARGP_KEY_ARG (0), so the key cannot be
   0 for an option.  */
#define OPT_STATUS 1000

static const char *opt_device;
static int opt_selftest;
static int opt_status;

static const struct argp_option options[] =
{
  {"device", 'd', "DOM:BUS:DEV.FN", 0,
   "Use the virtio-balloon device at this PCI slot instead of the first match"},
  {"selftest", 's', 0, 0,
   "Run the Phase 1 device selftest and exit"},
  {"status", OPT_STATUS, 0, 0,
   "Print the device's target/actual and exit, without disturbing a "
   "running driver"},
  {"verbose", 'v', 0, 0,
   "Log every queue posting and completion"},
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
    case 's':
      opt_selftest = 1;
      break;
    case 'v':
      vballoon_verbose = 1;
      break;
    case OPT_STATUS:
      opt_status = 1;
      break;
    default:
      return ARGP_ERR_UNKNOWN;
    }
  return 0;
}

static const struct argp argp =
{
  options, parse_opt,
  "[--device=SLOT] [--selftest] [--status] [--verbose]",
  "Virtio balloon driver: participates in host-driven memory ballooning "
  "through the QEMU virtio-balloon device"
};

static struct vballoon balloon;

static volatile sig_atomic_t quit_flag;

static void
quit_handler (int sig)
{
  quit_flag = 1;
}

error_t
vballoon_start_device (const char *override)
{
  vballoon_mem_init (&balloon.pool, &balloon.pci);
  return vballoon_dev_init (&balloon, override);
}

/* Print the device's view of the balloon without disturbing a running
   driver: the probe only enables the PCI device (it never touches the
   virtio status register, so no reset, no handshake), and the config
   region is readable in any device state.  */
static error_t
vballoon_status (const char *override)
{
  struct virtio_pci_dev v;
  uint32_t target, actual;
  error_t err;

  v.name = "virtio-balloon";
  v.device_id = VBALLOON_DEVICE_ID;
  err = virtio_pci_probe (&v, override);
  if (err)
    return err;
  target = virtio_pci_read_config (&v, VBALLOON_CFG_NUM_PAGES);
  actual = virtio_pci_read_config (&v, VBALLOON_CFG_ACTUAL);
  printf ("target=%u pages (%u KiB), actual=%u pages (%u KiB)\n",
          target, target << (VBALLOON_PFN_SHIFT - 10),
          actual, actual << (VBALLOON_PFN_SHIFT - 10));
  return 0;
}

void
vballoon_stop_device (void)
{
  vballoon_dev_quiesce (&balloon);
}

/* Phase 1 gate: device found, features negotiated, queues programmed,
   config readable and writable, and one inflate/deflate round trip that
   proves the PFN path end to end.  */
error_t
vballoon_selftest (void)
{
  struct vballoon *b = &balloon;
  uint32_t offered, accepted, target, got, gone;
  error_t err;

  offered = virtio_pci_read_host_features (&b->pci);
  accepted = virtio_pci_read_guest_features (&b->pci);
  if (accepted != (offered & (1u << VBALLOON_F_MUST_TELL_HOST)))
    {
      fprintf (stderr, "virtio-balloon selftest: guest_features 0x%x does "
               "not match offered & implemented 0x%x\n", accepted,
               offered & (1u << VBALLOON_F_MUST_TELL_HOST));
      return EIO;
    }

  if (b->actual != 0 || vballoon_pool_held (&b->pool) != 0)
    {
      fprintf (stderr, "virtio-balloon selftest: fresh start is not empty "
               "(actual %u, held %u)\n", b->actual,
               vballoon_pool_held (&b->pool));
      return EIO;
    }

  err = vballoon_dev_inflate (b, 1, &got);
  if (err)
    {
      fprintf (stderr, "virtio-balloon selftest: inflate: %s\n",
               strerror (err));
      return err;
    }
  if (got != 1 || b->actual != 1 || vballoon_pool_held (&b->pool) != 1
      || virtio_pci_read_config (&b->pci, VBALLOON_CFG_ACTUAL) != 1)
    {
      fprintf (stderr, "virtio-balloon selftest: inflate accounting "
               "(got %u, actual %u, held %u)\n", got, b->actual,
               vballoon_pool_held (&b->pool));
      return EIO;
    }

  err = vballoon_dev_deflate (b, 1, &gone);
  if (err)
    {
      fprintf (stderr, "virtio-balloon selftest: deflate: %s\n",
               strerror (err));
      return err;
    }
  if (gone != 1 || b->actual != 0 || vballoon_pool_held (&b->pool) != 0
      || virtio_pci_read_config (&b->pci, VBALLOON_CFG_ACTUAL) != 0)
    {
      fprintf (stderr, "virtio-balloon selftest: deflate accounting "
               "(gone %u, actual %u, held %u)\n", gone, b->actual,
               vballoon_pool_held (&b->pool));
      return EIO;
    }

  target = vballoon_dev_read_target (b);
  printf ("offered features 0x%x, accepted 0x%x\n", offered, accepted);
  printf ("target=%u pages\n", target);
  printf ("inflateq/deflateq round trip ok\n");
  printf ("actual tracked 0 -> 1 -> 0\n");
  return 0;
}

/* One convergence step: move the reported actual towards the device
   target in bounded batches, the way Linux's update_balloon_size_func
   does.  A batch that cannot claim pages (out of puff) is not an error:
   the next config change or tick retries.  */
error_t
vballoon_converge (void)
{
  struct vballoon *b = &balloon;

  b->target = vballoon_dev_read_target (b);
  while (b->target != b->actual)
    {
      uint32_t moved;
      error_t err;

      if (b->target > b->actual)
        err = vballoon_dev_inflate (b, b->target - b->actual, &moved);
      else
        err = vballoon_dev_deflate (b, b->actual - b->target, &moved);
      if (err)
        return err;
      if (moved == 0)
        break;
    }
  return 0;
}

int
main (int argc, char **argv)
{
  error_t err;
  struct sigaction sa;
  uint32_t seq = 0;

  argp_parse (&argp, argc, argv, 0, 0, 0);

  if (opt_status)
    {
      err = vballoon_status (opt_device);
      if (err)
        error (1, err, "virtio-balloon device not usable");
      return 0;
    }

  if (opt_selftest)
    {
      err = vballoon_start_device (opt_device);
      if (err)
        error (1, err, "device bring-up failed");
      err = vballoon_selftest ();
      if (err)
        error (1, err, "selftest failed");
      printf ("selftest ok\n");
      vballoon_stop_device ();
      return 0;
    }

  sa.sa_handler = quit_handler;
  sa.sa_flags = 0;
  sigemptyset (&sa.sa_mask);
  sigaction (SIGINT, &sa, NULL);
  sigaction (SIGTERM, &sa, NULL);

  err = vballoon_start_device (opt_device);
  if (err)
    /* No device, or one that will not talk: die visibly instead of leaving
       a daemon behind that looks alive.  */
    error (4, err, "virtio-balloon device not usable");

  for (; !quit_flag; )
    {
      err = virtio_irq_wait_config (&balloon.irq, VBALLOON_TICK_SEC, &seq);
      if (err && err != ETIMEDOUT)
        error (1, err, "waiting for config change");
      err = vballoon_converge ();
      if (err)
        error (1, err, "balloon convergence failed");
    }

  /* Hand the pages back before the device goes away.  */
  vballoon_dev_release_all (&balloon);
  vballoon_stop_device ();
  return 0;
}
