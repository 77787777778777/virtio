/* 9P PDU framing and the Phase 1 selftest.

   This file stays a framing layer, not a protocol interpreter: it assembles
   the 4-byte little-endian size prefix, owns the TX/RX DMA buffers through
   the queue module, and runs the one hand-built exchange (Tversion) that
   proves the device, the feature negotiation, the queue and the DMA
   addresses all work before the stream adapter exists.  */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "virtio-9p.h"

static void
put_le16 (void *p, uint16_t v)
{
  uint8_t *b = p;
  b[0] = v;
  b[1] = v >> 8;
}

static void
put_le32 (void *p, uint32_t v)
{
  uint8_t *b = p;
  b[0] = v;
  b[1] = v >> 8;
  b[2] = v >> 16;
  b[3] = v >> 24;
}

static uint16_t
get_le16 (const void *p)
{
  const uint8_t *b = p;
  return b[0] | (b[1] << 8);
}

static uint32_t
get_le32 (const void *p)
{
  const uint8_t *b = p;
  return b[0] | (b[1] << 8) | (b[2] << 16) | ((uint32_t) b[3] << 24);
}

/* Hand-built Tversion: size[4] type=100[1] tag[2] msize[4] version[2+8].
   9pfs asks for 16 MiB; QEMU answers with its transport cap.  */
error_t
v9p_selftest (const char *required_tag)
{
  uint8_t pdu[32], reply[128];
  uint32_t pdu_len, rlen, rmsize, vlen;
  error_t err;

  pdu_len = 4 + 1 + 2 + 4 + 2 + 8;
  put_le32 (pdu + 0, pdu_len);
  pdu[4] = 100; /* Tversion */
  put_le16 (pdu + 5, 0);
  put_le32 (pdu + 7, 16u << 20);
  put_le16 (pdu + 11, 8);
  memcpy (pdu + 13, "9P2000.L", 8);

  err = v9p_exchange (pdu, pdu_len, reply, sizeof reply, &rlen);
  if (err)
    {
      fprintf (stderr, "virtio-9p selftest: Tversion exchange: %s\n",
               strerror (err));
      return err;
    }

  if (rlen < 7 + 4 + 2 || get_le32 (reply + 0) != rlen
      || reply[4] != 101 || get_le16 (reply + 5) != 0)
    {
      fprintf (stderr, "virtio-9p selftest: malformed Rversion "
               "(len %u type %u tag %u)\n",
               rlen, rlen > 4 ? reply[4] : 0, rlen > 5 ? get_le16 (reply + 5) : 0);
      return EIO;
    }
  rmsize = get_le32 (reply + 7);
  vlen = get_le16 (reply + 11);

  if (strcmp (required_tag, v9p_tag) != 0)
    {
      fprintf (stderr, "virtio-9p selftest: mount tag mismatch "
               "(wanted \"%s\", device has \"%s\")\n",
               required_tag, v9p_tag);
      return ENODEV;
  }

  {
    uint32_t guest_features = v9p_read_guest_features ();
    if (!(guest_features & (1u << V9P_F_MOUNT_TAG)))
      {
        fprintf (stderr, "virtio-9p selftest: mount tag feature not "
                 "acknowledged in guest_features (0x%x)\n", guest_features);
        return EIO;
      }
  }

  {
    uint32_t pfn = v9p_read_queue_pfn ();
    if (pfn != v9p_vring_phys >> 12)
      {
        fprintf (stderr, "virtio-9p selftest: QUEUE_PFN readback 0x%x != "
                 "phys >> 12 (0x%llx)\n", pfn,
                 (unsigned long long) (v9p_vring_phys >> 12));
        return EIO;
      }
    printf ("pfn=%u == phys 0x%llx >> 12\n", pfn,
            (unsigned long long) v9p_vring_phys);
  }

  if (rmsize != V9P_MSIZE_CAP)
    {
      fprintf (stderr, "virtio-9p selftest: negotiated msize %u, expected "
               "%u (transport cap)\n", rmsize, V9P_MSIZE_CAP);
      return EIO;
    }
  if (vlen != 8 || memcmp (reply + 13, "9P2000.L", 8) != 0)
    {
      fprintf (stderr, "virtio-9p selftest: bad Rversion version string\n");
      return EIO;
    }

  printf ("mount tag feature negotiated\n");
  printf ("Rversion ok\n");
  printf ("msize=%u\n", rmsize);
  printf ("version=%.*s\n", (int) vlen, reply + 13);
  return 0;
}
