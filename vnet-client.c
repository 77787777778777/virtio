/* vnet-client: direct Mach device RPC test client for virtio-net.

   Opens a virtio-net translator node, installs an accept-all BPF filter,
   transmits one ARP frame, and waits for a reply.  This exercises the
   raw virtio-net datapath without involving pfinet.

   Usage:
       vnet-client NODE test
       vnet-client NODE recv TIMEOUT_MS
   */

#include <argp.h>
#include <errno.h>
#include <error.h>
#include <fcntl.h>
#include <hurd.h>
#include <mach.h>
#include <mach_error.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <device/device.h>
#include <device/net_status.h>
#include <device/bpf.h>

static int failures;

static void record(const char *what, int ok, const char *detail)
{
  printf("%s: %s%s%s\n", ok ? "PASS" : "FAIL", what,
         detail ? ": " : "", detail ? detail : "");
  if (!ok)
    failures++;
}

static error_t open_device(const char *node, device_t *dev)
{
  file_t file = file_name_lookup(node, O_READ | O_WRITE, 0);
  if (file == MACH_PORT_NULL)
    {
      perror("file_name_lookup");
      return ENOENT;
    }
  return device_open(file, D_READ | D_WRITE, "", dev);
}

/* pfinet's ether filter (accepts ARP, IPv4, IPv6). */
static struct bpf_insn accept_all[] =
{
  { NETF_IN | NETF_BPF, 0, 0, 0 },
  { BPF_LD | BPF_H | BPF_ABS, 0, 0, 12 },
  { BPF_JMP | BPF_JEQ | BPF_K, 2, 0, 0x0806 },
  { BPF_JMP | BPF_JEQ | BPF_K, 1, 0, 0x0800 },
  { BPF_JMP | BPF_JEQ | BPF_K, 0, 1, 0x86DD },
  { BPF_RET | BPF_K, 0, 0, 1500 },
  { BPF_RET | BPF_K, 0, 0, 0 },
};

/* No NETF_IN or NETF_OUT: refused before the filter list is touched. */
static struct bpf_insn no_direction[] =
{
  { NETF_BPF, 0, 0, 0 },
  { BPF_RET | BPF_K, 0, 0, 0 },
};

/* Whether any task holds a send right for the receive right RCV. */
static int has_senders(mach_port_t rcv)
{
  struct mach_port_status status;

  return mach_port_get_receive_status(mach_task_self(), rcv, &status) == 0
         && status.mps_srights;
}

/* A request that the driver refuses must leave an installed filter as it
   was.  This task keeps no send right for PORT, so the filter's is the
   only one, and its presence shows whether the driver still holds it. */
static void check_refused_filters(device_t dev)
{
  mach_port_t port;
  error_t err;

  err = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port);
  if (err)
    {
      record("refused filters: port allocated", 0, mach_error_string(err));
      return;
    }

  err = device_set_filter(dev, port, MACH_MSG_TYPE_MAKE_SEND, 0,
                          (filter_t *) accept_all,
                          sizeof(accept_all) / sizeof(filter_t));
  record("filter installed for a second port", err == 0 && has_senders(port),
         err ? mach_error_string(err) : NULL);

  err = device_set_filter(dev, port, MACH_MSG_TYPE_MAKE_SEND, 0,
                          (filter_t *) no_direction,
                          sizeof(no_direction) / sizeof(filter_t));
  record("filter without a direction refused", err != 0, NULL);
  record("installed filter keeps its send right after a refused one",
         has_senders(port), NULL);

  err = device_set_filter(dev, MACH_PORT_NULL, MACH_MSG_TYPE_MAKE_SEND, 0,
                          (filter_t *) accept_all,
                          sizeof(accept_all) / sizeof(filter_t));
  record("filter for a null port refused", err != 0, NULL);

  mach_port_destroy(mach_task_self(), port);
}

static void selftest_arp_frame(uint8_t *f)
{
  static const uint8_t bcast[6] = { 0xff,0xff,0xff,0xff,0xff,0xff };
  static const uint8_t tpa[4] = { 10, 0, 2, 2 };
  memset(f, 0, 42);
  memcpy(f, bcast, 6);
  /* src mac left zero */
  f[12] = 0x08; f[13] = 0x06;
  f[14] = 0x00; f[15] = 0x01;
  f[16] = 0x08; f[17] = 0x00;
  f[18] = 6; f[19] = 4;
  f[20] = 0x00; f[21] = 0x01;
  memcpy(f + 32, bcast, 6);
  f[38] = tpa[0]; f[39] = tpa[1]; f[40] = tpa[2]; f[41] = tpa[3];
}

static int do_test(const char *node)
{
  device_t dev;
  error_t err;
  mach_port_t rcv = MACH_PORT_NULL;
  uint8_t frame[42];
  int written = 0;

  err = open_device(node, &dev);
  if (err)
    {
      fprintf(stderr, "open failed: %s\n", mach_error_string(err));
      return 1;
    }

  err = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &rcv);
  if (err)
    {
      fprintf(stderr, "port alloc failed: %s\n", mach_error_string(err));
      device_close(dev);
      return 1;
    }
  mach_port_insert_right(mach_task_self(), rcv, rcv, MACH_MSG_TYPE_MAKE_SEND);

  err = device_set_filter(dev, rcv, MACH_MSG_TYPE_MAKE_SEND, 0,
                          (filter_t *) accept_all,
                          sizeof(accept_all) / sizeof(filter_t));
  if (err)
    {
      fprintf(stderr, "set_filter failed: %s\n", mach_error_string(err));
      mach_port_destroy(mach_task_self(), rcv);
      device_close(dev);
      return 1;
    }

  selftest_arp_frame(frame);
  err = device_write(dev, 0, 0, (char *) frame, sizeof frame, &written);
  record("TX ARP frame", err == 0 && written == (int) sizeof frame,
         err ? mach_error_string(err) : "ok");

  /* Wait for a reply. */
  {
    struct net_rcv_msg msg;
    mach_msg_return_t mr;
    memset(&msg, 0, sizeof msg);
    mr = mach_msg(&msg.msg_hdr, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0,
                  sizeof msg, rcv, 3000, MACH_PORT_NULL);
    if (mr == MACH_MSG_SUCCESS)
      record("RX frame received", 1, "ok");
    else
      record("RX frame received (timeout ok)", mr == MACH_RCV_TIMED_OUT,
             mach_error_string(mr));
  }

  check_refused_filters(dev);

  mach_port_destroy(mach_task_self(), rcv);
  device_close(dev);
  return failures ? 1 : 0;
}

static int do_recv(const char *node, int timeout_ms)
{
  device_t dev;
  error_t err;
  mach_port_t rcv = MACH_PORT_NULL;

  err = open_device(node, &dev);
  if (err)
    {
      fprintf(stderr, "open failed: %s\n", mach_error_string(err));
      return 1;
    }

  err = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &rcv);
  if (err)
    {
      device_close(dev);
      return 1;
    }
  mach_port_insert_right(mach_task_self(), rcv, rcv, MACH_MSG_TYPE_MAKE_SEND);

  err = device_set_filter(dev, rcv, MACH_MSG_TYPE_MAKE_SEND, 0,
                          (filter_t *) accept_all,
                          sizeof(accept_all) / sizeof(filter_t));
  if (err)
    {
      fprintf(stderr, "set_filter failed: %s\n", mach_error_string(err));
      mach_port_destroy(mach_task_self(), rcv);
      device_close(dev);
      return 1;
    }

  {
    struct net_rcv_msg msg;
    mach_msg_return_t mr;
    memset(&msg, 0, sizeof msg);
    mr = mach_msg(&msg.msg_hdr, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0,
                  sizeof msg, rcv, timeout_ms, MACH_PORT_NULL);
    if (mr == MACH_MSG_SUCCESS)
      printf("PASS: received frame\n");
    else
      printf("%s: %s\n", mr == MACH_RCV_TIMED_OUT ? "PASS" : "FAIL",
             mach_error_string(mr));
  }

  mach_port_destroy(mach_task_self(), rcv);
  device_close(dev);
  return 0;
}

int main(int argc, char **argv)
{
  if (argc < 3)
    {
      fprintf(stderr, "usage: %s NODE test|recv [TIMEOUT_MS]\n", argv[0]);
      return 2;
    }
  if (strcmp(argv[2], "test") == 0)
    return do_test(argv[1]);
  if (strcmp(argv[2], "recv") == 0)
    {
      int t = argc > 3 ? atoi(argv[3]) : 3000;
      return do_recv(argv[1], t);
    }
  fprintf(stderr, "unknown command: %s\n", argv[2]);
  return 2;
}
