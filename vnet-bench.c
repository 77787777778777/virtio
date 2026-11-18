/* Time device_write on a Mach network device.

   usage: vnet-bench NODE NAME COUNT SIZE [nowait]

   Opens device NAME through the node NODE and writes COUNT frames of SIZE
   bytes, one after the other, as pfinet does (one synchronous RPC per
   frame).  The frames carry an experimental ethertype that no stack
   answers.  Prints the rate and the distribution of the time one call
   takes.  */

#define _GNU_SOURCE
#include <errno.h>
#include <error.h>
#include <fcntl.h>
#include <hurd.h>
#include <mach.h>
#include <mach_error.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <x86intrin.h>

#include <device/device.h>
#include <device/net_status.h>

static int
cmp_u64 (const void *a, const void *b)
{
  uint64_t x = *(const uint64_t *) a, y = *(const uint64_t *) b;
  return x < y ? -1 : x > y;
}

int
main (int argc, char **argv)
{
  static uint8_t frame[1514];
  struct timespec t0, t1;
  uint64_t *ticks, tsc0, tsc1;
  double ns, ns_per_tick;
  int count, size, mode, i, written;
  int errors = 0, shorts = 0, over1 = 0, over10 = 0, over100 = 0;
  error_t err, first_error = 0;
  device_t dev;
  file_t file;

  if (argc < 5)
    error (2, 0, "usage: %s NODE NAME COUNT SIZE [nowait]", argv[0]);
  count = atoi (argv[3]);
  size = atoi (argv[4]);
  mode = (argc > 5 && strcmp (argv[5], "nowait") == 0) ? D_NOWAIT : 0;
  if (count < 1 || size < 60 || size > (int) sizeof frame)
    error (2, 0, "COUNT must be positive and SIZE between 60 and 1514");

  file = file_name_lookup (argv[1], O_READ | O_WRITE, 0);
  if (file == MACH_PORT_NULL)
    error (1, errno, "%s", argv[1]);
  err = device_open (file, D_READ | D_WRITE, argv[2], &dev);
  if (err)
    error (1, err, "device_open %s", argv[2]);

  memcpy (frame, "\x02\x00\x00\x00\x00\x01", 6);
  memcpy (frame + 6, "\x02\x00\x00\x00\x00\x02", 6);
  frame[12] = 0x88;             /* IEEE 802 local experimental ethertype */
  frame[13] = 0xb5;

  ticks = malloc (count * sizeof *ticks);
  if (!ticks)
    error (1, ENOMEM, "malloc");

  clock_gettime (CLOCK_MONOTONIC, &t0);
  tsc0 = __rdtsc ();
  for (i = 0; i < count; i++)
    {
      uint64_t a = __rdtsc ();

      written = 0;
      err = device_write (dev, mode, 0, (io_buf_ptr_t) frame, size, &written);
      ticks[i] = __rdtsc () - a;
      if (err)
        {
          if (!errors)
            first_error = err;
          errors++;
        }
      else if (written != size)
        shorts++;
    }
  tsc1 = __rdtsc ();
  clock_gettime (CLOCK_MONOTONIC, &t1);

  ns = (t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec);
  ns_per_tick = ns / (double) (tsc1 - tsc0);
  for (i = 0; i < count; i++)
    {
      double us = ticks[i] * ns_per_tick / 1e3;

      over1 += us > 1e3;
      over10 += us > 1e4;
      over100 += us > 1e5;
    }
  qsort (ticks, count, sizeof *ticks, cmp_u64);

#define US(q) (ticks[(int) ((count - 1) * (q))] * ns_per_tick / 1e3)
  printf ("%s %s: %d frames of %d bytes, mode %s: %.3f s, %.0f frames/s, "
          "mean %.1f us\n", argv[1], argv[2], count, size,
          mode ? "D_NOWAIT" : "0", ns / 1e9, count / (ns / 1e9),
          ns / 1e3 / count);
  printf ("  per call, us: min %.1f  p50 %.1f  p90 %.1f  p99 %.1f  "
          "p99.9 %.1f  max %.1f\n", US (0), US (0.5), US (0.9), US (0.99),
          US (0.999), US (1));
  printf ("  calls over 1 ms: %d, over 10 ms: %d, over 100 ms: %d; "
          "errors: %d%s%s, short writes: %d\n", over1, over10, over100,
          errors, errors ? ", first: " : "",
          errors ? mach_error_string (first_error) : "", shorts);
  device_close (dev);
  return 0;
}
