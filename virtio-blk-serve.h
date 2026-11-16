/* virtio-blk-serve.h: per-open device handles for the MIG device server.

   The generated device server stubs (deviceServer.c, from
   include/device/device.defs) include this file through the
   DEVICE_IMPORTS mutation in mig-mutate.h, and translate every
   device_t argument through begin_using_device_port/payload /
   end_using_device — the same shape as libmachdev's mig-decls.h.

   A handle is a libports port (in vblk_server_bucket, class
   vblk_server_class) whose struct begins with struct port_info; the
   rest is the plan's §5 Mach semantics layer (virtio-blk-hurd.c) plus
   the name the device_open arrived with, for logging.  */

#ifndef VIRTIO_BLK_SERVE_H
#define VIRTIO_BLK_SERVE_H

#include <hurd/ports.h>
#include "virtio-blk.h"

extern struct port_bucket *vblk_server_bucket;
extern struct port_class *vblk_server_class;

struct vblk_port
{
  struct port_info pi;
  struct vblk_hurd hurd;
  char name[0];                 /* device_open name, NUL-terminated */
};
typedef struct vblk_port *vblk_port_t;

/* MIG stub helpers (called by the generated code).  NULL means "not
   one of ours": the stub passes it through and the handler answers
   D_NO_SUCH_DEVICE, so a foreign port on this server cannot be
   mistaken for a live device.  */
static inline vblk_port_t __attribute__ ((unused))
begin_using_device_port (mach_port_t port)
{
  return ports_lookup_port (vblk_server_bucket, port, vblk_server_class);
}

static inline vblk_port_t __attribute__ ((unused))
begin_using_device_payload (uintptr_t payload)
{
  return ports_lookup_payload (vblk_server_bucket, payload,
                               vblk_server_class);
}

static inline void __attribute__ ((unused))
end_using_device (vblk_port_t handle)
{
  if (handle)
    ports_port_deref (handle);
}

/* Serve forever (trivfs translator loop); called with DEV initialized.
   Never returns.  */
void vblk_serve (struct vblk_dev *dev, const char *name);

#endif /* VIRTIO_BLK_SERVE_H */
