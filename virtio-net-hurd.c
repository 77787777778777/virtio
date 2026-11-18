/* virtio-net Hurd integration: the Mach network-device semantics layer.

   This module implements the plan's §5.5 MVP subset of the Mach net-device
   contract on top of the raw datapath, so the open/write/status/filter
   behaviors pfinet and lwip expect are fixed and testable before any port
   publication exists:

   - open/close with the D_READ/D_WRITE mode policy and a client-managed
     NET_FLAGS store;
   - device_write TX (D_NOWAIT, recnum 0, whole frames) through the
     serialized TX engine, with D_WOULD_BLOCK/D_IO_ERROR mapping;
   - NET_STATUS/NET_ADDRESS/NET_FLAGS following the eth-multiplexer server
     shapes (dev_stat.c/vdev_setstat), with min_packet_size = 60 (the
     Ethernet minimum frame; eth-multiplexer reports header_size there,
     but no inspected consumer asserts it and 60 is the accurate value);
   - real filter evaluation through libbpf (net_set_filter/bpf_do_filter),
     the same substrate eth-multiplexer's ds_device_set_filter uses, with
     dead-name notification removing a closed client's filter;
   - NET_RCV_MSG built and sent from userspace exactly like eth-multiplexer
     (vdev.c broadcast_pack/deliver_msg): packet_header prefix, packet_type
     count, msgh_size rounded to uintptr_t, COPY_SEND remote port, and a
     zero-timeout send that drops on queue pressure instead of blocking
     the RX thread (plan §6.3).

   What is deliberately absent: the MIG demuxer, port set, and name
   publication (virtio-net-serve.c).  */

#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <stdio.h>
#include <string.h>

#include <device/net_status.h>
#include <mach/message.h>

#include "virtio-net.h"

/* Link-state bit from virtio_net_config.status (VIRTIO_NET_F_STATUS).  */
#define VNET_STATUS_LINK_UP 1

/* Flags reported before the consumer sets any: up, running, broadcast.
   IFF_RUNNING tracks the virtio link state; the rest is the static shape
   eth-multiplexer gives its virtual devices.  */
#define VNET_FLAGS_DEFAULT (IFF_UP | IFF_BROADCAST | IFF_RUNNING)

/* Registered open handles.  RX frames go to every handle whose filters
   accept them, the way eth-multiplexer broadcasts to a device's filter
   list; the list is tiny (one open per stack in practice).  */
static struct vnet_hurd *vnet_handles;
static pthread_mutex_t vnet_handles_lock = PTHREAD_MUTEX_INITIALIZER;

void
vnet_hurd_register (struct vnet_hurd *h)
{
  pthread_mutex_lock (&vnet_handles_lock);
  h->next = vnet_handles;
  vnet_handles = h;
  pthread_mutex_unlock (&vnet_handles_lock);
}

void
vnet_hurd_unregister (struct vnet_hurd *h)
{
  struct vnet_hurd **p;

  pthread_mutex_lock (&vnet_handles_lock);
  for (p = &vnet_handles; *p; p = &(*p)->next)
    if (*p == h)
      {
        *p = h->next;
        break;
      }
  pthread_mutex_unlock (&vnet_handles_lock);
}

error_t
vnet_hurd_init (struct vnet_hurd *h, struct vnet_dev *d)
{
  h->dev = d;
  h->opened = 0;
  h->mode = 0;
  h->if_flags = VNET_FLAGS_DEFAULT;
  queue_init (&h->filters.if_rcv_port_list);
  queue_init (&h->filters.if_snd_port_list);
  pthread_mutex_init (&h->lock, NULL);
  return 0;
}

/* libstore-style mode policy: D_READ or D_READ|D_WRITE, with D_NOWAIT the
   only other bit allowed.  */
static int
vnet_hurd_mode_ok (int mode)
{
  int rw = mode & (D_READ | D_WRITE);

  if (mode & ~(D_READ | D_WRITE | D_NOWAIT))
    return 0;
  return rw != 0;
}

error_t
vnet_hurd_open (struct vnet_hurd *h, int mode)
{
  if (h->opened)
    return D_ALREADY_OPEN;
  if (!vnet_hurd_mode_ok (mode))
    {
      fprintf (stderr, "virtio-net: open: unsupported mode 0x%x\n", mode);
      return D_INVALID_OPERATION;
    }
  h->opened = 1;
  h->mode = mode & (D_READ | D_WRITE);
  return 0;
}

error_t
vnet_hurd_close (struct vnet_hurd *h)
{
  /* Idempotent, so a close after a failed open is not an error.  Filter
     entries outlive the handle on purpose: the registered receive port
     dies with the client, and the dead-name notification removes them
     (and drops our send right) then.  */
  h->opened = 0;
  h->mode = 0;
  return 0;
}

error_t
vnet_hurd_write (struct vnet_hurd *h, int mode, recnum_t recnum,
                 const void *data, int bytes, int *bytes_written)
{
  struct vnet_dev *d = h->dev;
  error_t err;

  *bytes_written = 0;
  if (!h->opened)
    return D_INVALID_OPERATION;
  if (!(h->mode & D_WRITE))
    return D_READ_ONLY;
  if (mode & ~D_NOWAIT)
    {
      fprintf (stderr, "virtio-net: write: unsupported mode 0x%x\n", mode);
      return D_INVALID_OPERATION;
    }
  if (recnum != 0)
    return D_INVALID_RECNUM;
  if (bytes <= 0)
    return D_INVALID_SIZE;
  if ((size_t) bytes > d->max_frame)
    {
      fprintf (stderr, "virtio-net: write of %d bytes exceeds the %u-byte "
               "frame limit\n", bytes, d->max_frame);
      return D_INVALID_SIZE;
    }
  if (bytes < VNET_ETHER_HDR_SIZE)
    {
      fprintf (stderr, "virtio-net: write of %d bytes is shorter than an "
               "Ethernet header\n", bytes);
      return D_INVALID_SIZE;
    }

  err = vnet_dev_tx (d, data, bytes);
  switch (err)
    {
    case 0:
      *bytes_written = bytes;
      return 0;
    case EAGAIN:
      /* D_NOWAIT semantics: the queues are full, do not hang the caller
         (plan §5.1.1); pfinet/lwip retry on their own reopen logic.  */
      return D_WOULD_BLOCK;
    case ETIMEDOUT:
    case EIO:
      return D_IO_ERROR;
    default:
      return err;
    }
}

error_t
vnet_hurd_get_status (struct vnet_hurd *h, dev_flavor_t flavor, int *status,
                      mach_msg_type_number_t *count)
{
  struct vnet_dev *d = h->dev;

  if (*count < 1)
    {
      *count = 0;
      return D_INVALID_OPERATION;
    }

  switch (flavor)
    {
    case NET_STATUS:
      {
        struct net_status *ns = (struct net_status *) status;

        if (*count < NET_STATUS_COUNT)
          {
            fprintf (stderr, "virtio-net: get_status: NET_STATUS array too "
                     "small (%u)\n", (unsigned) *count);
            *count = 0;
            return D_INVALID_OPERATION;
          }
        ns->min_packet_size = VNET_ETHER_MIN_FRAME;
        ns->max_packet_size = d->max_frame;
        ns->header_format = HDR_ETHERNET;
        ns->header_size = VNET_ETHER_HDR_SIZE;
        ns->address_size = 6;
        ns->flags = h->if_flags;
        ns->mapped_size = 0;
        *count = NET_STATUS_COUNT;
        return 0;
      }
    case NET_ADDRESS:
      {
        int *addr = status;

        if (*count < 2)
          {
            fprintf (stderr, "virtio-net: get_status: NET_ADDRESS array too "
                     "small (%u)\n", (unsigned) *count);
            *count = 0;
            return D_INVALID_OPERATION;
          }
        /* The consumer ntohl()s each int and then memcpy()s 6 bytes, so
           hand over the MAC in the same swapped-word form the
           eth-multiplexer server uses.  */
        memset (addr, 0, 2 * sizeof (int));
        memcpy (addr, d->mac, 6);
        addr[0] = htonl (addr[0]);
        addr[1] = htonl (addr[1]);
        *count = 2;
        return 0;
      }
    case NET_FLAGS:
      status[0] = h->if_flags;
      *count = 1;
      return 0;
    default:
      fprintf (stderr, "virtio-net: get_status: unknown flavor %u\n", flavor);
      *count = 0;
      return D_INVALID_OPERATION;
    }
}

error_t
vnet_hurd_set_status (struct vnet_hurd *h, dev_flavor_t flavor,
                      const int *status, mach_msg_type_number_t count)
{
  switch (flavor)
    {
    case NET_FLAGS:
      if (count != 1)
        return D_INVALID_SIZE;
      /* Accept and record the interface flags (plan §5.5.1): the driver
         delivers everything the backend sends and applies no semantic
         behavior to the bits, which is the compatibility surface the
         inspected stacks rely on.  */
      if (vnet_verbose)
        fprintf (stderr, "virtio-net: set_status NET_FLAGS 0x%x -> 0x%x\n",
                 h->if_flags, status[0]);
      h->if_flags = status[0];
      return 0;
    case NET_STATUS:
      /* Not observed from pfinet/lwip; refusing keeps the flags store the
         single source of truth.  */
      fprintf (stderr, "virtio-net: set_status: NET_STATUS not supported\n");
      return D_INVALID_OPERATION;
    case NET_ADDRESS:
      /* Refusing avoids claiming an address change the virtio device did
         not make; unobserved in the inspected stacks.  */
      fprintf (stderr, "virtio-net: set_status: NET_ADDRESS not supported\n");
      return D_INVALID_OPERATION;
    default:
      fprintf (stderr, "virtio-net: set_status: unknown flavor %u\n", flavor);
      return D_INVALID_OPERATION;
    }
}

error_t
vnet_hurd_set_filter (struct vnet_hurd *h, mach_port_t receive_port,
                      int priority, const filter_t *filter,
                      mach_msg_type_number_t filterlen)
{
  error_t err;

  if (!h->opened)
    return D_INVALID_OPERATION;

  pthread_mutex_lock (&h->lock);
  err = net_set_filter (&h->filters, receive_port, priority,
                        (filter_t *) filter, filterlen);
  pthread_mutex_unlock (&h->lock);
  return err;
}

void
vnet_hurd_dead_name (struct vnet_hurd *h, mach_port_t dead_port)
{
  pthread_mutex_lock (&h->lock);
  remove_dead_filter (&h->filters, &h->filters.if_rcv_port_list, dead_port);
  remove_dead_filter (&h->filters, &h->filters.if_snd_port_list, dead_port);
  pthread_mutex_unlock (&h->lock);
}

void
vnet_hurd_all_dead_name (mach_port_t dead_port)
{
  struct vnet_hurd *h;

  pthread_mutex_lock (&vnet_handles_lock);
  for (h = vnet_handles; h; h = h->next)
    vnet_hurd_dead_name (h, dead_port);
  pthread_mutex_unlock (&vnet_handles_lock);
}

/* Build one NET_RCV_MSG for FRAME (Ethernet header + payload) following
   eth-multiplexer's broadcast_pack: the link header goes into `header`,
   a packet_header prefix plus the payload go into `packet`, and the
   message size is rounded the way both gnumach's net_io.c and
   eth-multiplexer round it.  */
static void
vnet_build_rcv_msg (struct net_rcv_msg *msg, const char *frame,
                    size_t frame_len)
{
  static const mach_msg_type_t header_type =
  {
    .msgt_name = MACH_MSG_TYPE_BYTE,
    .msgt_size = 8,
    .msgt_number = NET_HDW_HDR_MAX,
    .msgt_inline = TRUE,
    .msgt_longform = FALSE,
    .msgt_deallocate = FALSE,
    .msgt_unused = 0
  };
  static const mach_msg_type_t packet_type =
  {
    .msgt_name = MACH_MSG_TYPE_BYTE,
    .msgt_size = 8,
    .msgt_number = 0,
    .msgt_inline = TRUE,
    .msgt_longform = FALSE,
    .msgt_deallocate = FALSE,
    .msgt_unused = 0
  };
  struct packet_header *packet;
  size_t pack_size = frame_len - VNET_ETHER_HDR_SIZE;

  msg->msg_hdr.msgh_bits = 0;
  msg->msg_hdr.msgh_remote_port = MACH_PORT_NULL;
  msg->msg_hdr.msgh_seqno = 0;
  msg->msg_hdr.msgh_size = sizeof (struct net_rcv_msg) - NET_RCV_MAX
                           + pack_size;
  msg->msg_hdr.msgh_size = (mach_msg_size_t)
    ((msg->msg_hdr.msgh_size + __alignof__ (uintptr_t) - 1)
     & ~(__alignof__ (uintptr_t) - 1));
  msg->msg_hdr.msgh_local_port = MACH_PORT_NULL;
  msg->msg_hdr.msgh_id = NET_RCV_MSG_ID;

  msg->header_type = header_type;
  memcpy (msg->header, frame, VNET_ETHER_HDR_SIZE);

  msg->packet_type = packet_type;
  packet = (struct packet_header *) msg->packet;
  memcpy (packet + 1, frame + VNET_ETHER_HDR_SIZE, pack_size);
  packet->type = ((const unsigned short *) frame)[6]; /* network order */
  packet->length = pack_size + sizeof (struct packet_header);
  msg->packet_type.msgt_number = packet->length;
  msg->net_rcv_msg_packet_count = packet->length;
  msg->sent = 0;
}

/* Deliver to one open handle: evaluate every registered filter and send
   the frame to each accepting receive port.  The send is COPY_SEND with a
   zero timeout (eth-multiplexer's deliver_msg): queue pressure drops the
   frame with a counter instead of stalling the virtio RX path, and a
   failed send deallocates the unconsumed copy.  Delivered and failed
   send counts accumulate into the caller's tallies.  */
static void
vnet_hurd_rx_one (struct vnet_hurd *h, const char *frame, size_t frame_len,
                  struct net_rcv_msg *template, int *template_built,
                  int *delivered, int *failed)
{
  struct net_rcv_msg local_msg;
  struct net_rcv_msg *m = NULL;
  queue_head_t *if_port_list;
  net_rcv_port_t infp, nextfp;
  size_t pack_size = frame_len - VNET_ETHER_HDR_SIZE;
  mach_msg_size_t packet_count = (mach_msg_size_t)
    (pack_size + sizeof (struct packet_header));

  if (!h->opened)
    return;
  if (queue_empty (&h->filters.if_rcv_port_list))
    return;

  if_port_list = &h->filters.if_rcv_port_list;
  pthread_mutex_lock (&h->lock);
  FILTER_ITERATE (if_port_list, infp, nextfp, &infp->input)
    {
      mach_port_t dest;
      net_hash_entry_t entp, *hash_headp;
      int ret_count;
      mach_msg_return_t mr;

      entp = (net_hash_entry_t) 0;
      /* Evaluate the filter against the frame views first, so the (costly)
         per-frame NET_RCV_MSG is built only when a filter actually accepts.  */
      ret_count = bpf_do_filter (infp,
                                 (char *) frame + VNET_ETHER_HDR_SIZE,
                                 packet_count,
                                 (char *) frame, VNET_ETHER_HDR_SIZE,
                                 &hash_headp, &entp);
      if (!ret_count)
        continue;

      if (!m)
        {
          if (template && template_built && *template_built)
            m = template;
          else if (template && template_built)
            {
              vnet_build_rcv_msg (template, frame, frame_len);
              *template_built = 1;
              m = template;
            }
          else
            {
              vnet_build_rcv_msg (&local_msg, frame, frame_len);
              m = &local_msg;
            }
        }

      dest = entp ? entp->rcv_port : infp->rcv_port;
      m->msg_hdr.msgh_bits = MACH_MSGH_BITS (MACH_MSG_TYPE_COPY_SEND, 0);
      m->msg_hdr.msgh_remote_port = dest;
      mr = mach_msg ((mach_msg_header_t *) m,
                     MACH_SEND_MSG | MACH_SEND_TIMEOUT,
                     m->msg_hdr.msgh_size, 0, MACH_PORT_NULL, 0,
                     MACH_PORT_NULL);
      if (mr == MACH_MSG_SUCCESS)
        {
          h->dev->stats.rx_delivered++;
          (*delivered)++;
        }
      else
        {
          mach_port_deallocate (mach_task_self (),
                                m->msg_hdr.msgh_remote_port);
          if (vnet_verbose)
            fprintf (stderr, "virtio-net: NET_RCV_MSG send failed: 0x%x\n",
                     mr);
          h->dev->stats.rx_delivery_fail++;
          (*failed)++;
        }
      m->msg_hdr.msgh_bits = 0;
      m->msg_hdr.msgh_remote_port = MACH_PORT_NULL;
    }
  FILTER_ITERATE_END
  pthread_mutex_unlock (&h->lock);
}

void
vnet_hurd_rx_all (struct vnet_dev *dev, const char *frame, size_t frame_len)
{
  struct vnet_hurd *h;
  int delivered = 0;
  int failed = 0;
  struct net_rcv_msg template;
  int template_built = 0;

  pthread_mutex_lock (&vnet_handles_lock);
  for (h = vnet_handles; h; h = h->next)
    if (h->dev == dev)
      vnet_hurd_rx_one (h, frame, frame_len, &template, &template_built,
                        &delivered, &failed);
  pthread_mutex_unlock (&vnet_handles_lock);

  /* One line per received frame, so verbose logs pair every TX
     completion with the RX it provoked (matches blk's per-request
     completion log for end-to-end tracing).  Zero counts are kept:
     0 delivered with no open handle is the common "nobody listening"
     case worth seeing when chasing missed frames.  */
  if (vnet_verbose)
    fprintf (stderr, "virtio-net: RX %zu-byte frame: %d delivered, "
             "%d failed\n", frame_len, delivered, failed);
}

/* Inband carries at most IO_INBAND_MAX (128) bytes and map is unused by
   the MVP consumers; answer with the compatible error rather than
   crashing, per the plan's defensive baseline.  */
error_t
vnet_hurd_read_inband (struct vnet_hurd *h, int mode, recnum_t recnum,
                       int bytes_wanted, io_buf_ptr_inband_t data)
{
  (void) h;
  (void) mode;
  (void) recnum;
  (void) bytes_wanted;
  (void) data;
  if (vnet_verbose)
    fprintf (stderr, "virtio-net: read_inband is not implemented\n");
  return D_INVALID_OPERATION;
}

error_t
vnet_hurd_write_inband (struct vnet_hurd *h, int mode, recnum_t recnum,
                        const io_buf_ptr_inband_t data, int bytes,
                        int *bytes_written)
{
  (void) h;
  (void) mode;
  (void) recnum;
  (void) data;
  (void) bytes;
  *bytes_written = 0;
  fprintf (stderr, "virtio-net: write_inband is not implemented\n");
  return D_INVALID_OPERATION;
}

error_t
vnet_hurd_map (struct vnet_hurd *h, vm_prot_t prot, vm_offset_t offset,
               vm_size_t size, memory_object_t *pager, int unmap)
{
  (void) h;
  (void) prot;
  (void) offset;
  (void) size;
  (void) pager;
  (void) unmap;
  if (vnet_verbose)
    fprintf (stderr, "virtio-net: device_map is not implemented\n");
  return D_INVALID_OPERATION;
}
