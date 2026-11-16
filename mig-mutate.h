/* MIG type-translation macros for the virtio-blk device server.

   Mirrors libmachdev's mig-mutate.h: the device_t argument of every
   generated ds_* stub is translated from a Mach port name (or a
   protected payload) into the translator's own per-open handle via
   libports lookups, and deref'd after the handler returns.  The
   helpers live in virtio-blk-serve.h, which DEVICE_IMPORTS pulls into
   the generated stubs; notify.defs uses libports' generic port_info
   helpers so mach_notify_no_senders reaches ports_do_mach_notify_no_senders
   (the no-senders teardown for our device ports).

   The Makefile feeds this file to the defs preprocessing pass with
   -imacros and sets -DMACH_PAYLOAD_TO_PORT=ports_payload_get_name, the
   same wiring as the Hurd tree's Makeconf MIGSFLAGS.  */

#ifndef VIRTIO_BLK_MIG_MUTATE_H
#define VIRTIO_BLK_MIG_MUTATE_H

#define DEVICE_INTRAN \
  vblk_port_t begin_using_device_port (mach_port_t)
/* The payload form is a bare function name: mig generates the call with
   the payload argument itself (same shape as libmachdev's mutation).  */
#define DEVICE_INTRAN_PAYLOAD \
  vblk_port_t begin_using_device_payload
#define DEVICE_DESTRUCTOR \
  end_using_device (vblk_port_t)
#define DEVICE_IMPORTS \
  import "virtio-blk-serve.h";

#define NOTIFY_INTRAN \
  port_info_t begin_using_port_info_port (mach_port_t)
#define NOTIFY_INTRAN_PAYLOAD \
  port_info_t begin_using_port_info_payload
#define NOTIFY_DESTRUCTOR \
  end_using_port_info (port_info_t)
#define NOTIFY_IMPORTS \
  import "libports/mig-decls.h";

#endif /* VIRTIO_BLK_MIG_MUTATE_H */
