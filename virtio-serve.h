/* virtio-serve.h: shared Mach-device translator scaffold for the
   virtio drivers.

   The blk and net binaries are both trivfs translators whose node
   accepts the Mach `device_open` protocol (device_server_routine
   chained before trivfs_demuxer, the devnode pattern), hand out
   libports ports wrapping per-open device handles, and tear the device
   down in trivfs_goaway.  That scaffold lives in virtio-serve.c; each
   driver supplies a `struct virtio_serve_ops' vtable plus its typed
   ds_device_* handlers (the MIG stubs pass the driver's own port type,
   so the typed handlers stay per-device).

   The MIG type plumbing (DEVICE_* mutations) resolves the generated
   stubs' device arguments through each driver's
   begin_using_device_port/payload helpers, which look up in
   virtio_serve_bucket under virtio_serve_class.  */

#ifndef VIRTIO_SERVE_H
#define VIRTIO_SERVE_H

#include <hurd/ports.h>
#include <hurd/trivfs.h>
#include <device/device.h>

/* The bucket and class every driver's device ports live in; the
   generated stubs reach them through the driver serve headers.  */
extern struct port_bucket *virtio_serve_bucket;
extern struct port_class *virtio_serve_class;

struct virtio_serve_ops
{
  const char *prefix;       /* log prefix, e.g. "virtio-blk-serve" */
  const char *translator;   /* advertised path for the bootstrap error */
  const int *verbose;       /* driver's verbose flag */
  size_t port_size;         /* sizeof the per-device port struct; the
                               device_open name is NUL-terminated at
                               that offset (the port's flexible tail) */

  /* Name tail of a port (for the clean-hook log).  */
  const char *(*port_name) (void *port);
  /* The generated stubs' dispatch (device_server_routine then
     notify_server_routine; both are static inlines over per-binary id
     tables in the generated _S.h headers, so the shared demuxer reaches
     them through this hook).  NULL means "not one of our subsystems".  */
  mig_routine_t (*server_routine) (mach_msg_header_t *inp);
  /* Full per-device open: handle init, mode policy, registration.
     Called with the port allocated and its name tail filled in;
     returns the D_* error to answer with (the scaffold destroys the
     port on any nonzero return).  */
  error_t (*handle_open) (void *port, dev_mode_t mode);
  /* Idempotent per-handle close: runs from the device_close RPC and
     from the class clean hook when the last reference drops.  */
  void (*handle_close) (void *port);
  /* Device teardown before the process exits (trivfs_goaway).  */
  void (*quiesce) (void *dev);
  /* Dead-name hook for client receive ports (net's filter walk).  NULL
     keeps libports' default behavior (interrupt notified RPCs), which
     is what the blk server has always used.  */
  void (*dead_name) (mach_port_t dead_name);
};

/* Become the translator and serve forever; called with DEV brought up.
   Never returns: exits when trivfs_goaway tears the translator down.  */
void virtio_serve_run (const struct virtio_serve_ops *ops, void *dev,
                       const char *name);

/* Shared ds_device_open scaffolding: applies the --name restriction,
   creates the port, and runs ops->handle_open.  Returns NULL with *ERR
   set on failure.  */
void *virtio_serve_open_port (const char *name, dev_mode_t mode,
                              error_t *err);

#endif /* VIRTIO_SERVE_H */
