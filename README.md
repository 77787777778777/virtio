# virtio-9p

This is a vibe-coded virtio-9p device translator for GNU Hurd. AI patches are ENCOURAGED.

It exposes QEMU's virtio-9p PCI device as a character device carrying the raw
9P message stream, so the existing, unmodified 9pfs translator can mount a
host directory in the guest, with no network configuration.

## How it works

```
QEMU host share
        |
        | virtio-9p PCI device
        v
/dev/virtio-9p0   this translator (virtio rings <-> 9P byte stream)
        |
        | 9P message stream
        v
/hurd/9pfs        unmodified 9P client translator
        |
        | Hurd filesystem RPCs
        v
/host             mount point
```

On the driver side this is a normal userspace translator: PCI probe and the
legacy virtio handshake through libpciaccess, one split virtqueue in
physically contiguous memory, interrupt-driven request/response, and a
byte-stream front end (short reads are normal; one 9P message is in flight
at a time) that matches how 9pfs drives a socket.

## Requirements

The guest must run the pci-arbiter translator (Hurd's libpciaccess is a
client of it, so the driver must not touch the PCI config ports itself), and
the device must present the legacy virtio PCI interface (the QEMU default on
i440fx).

## Building

Build with the x86_64-gnu cross toolchain. The Hurd toolchain harness at
https://github.com/hurdkit/hurdkit/ has a `virtio-9p` target that sets
CC/CPATH/LIBS; for a manual build, set them the same way.

```
make
make CFLAGS=-g    # debugging build
```

## Usage

On the host, start QEMU with a share:

```
-fsdev local,id=virtfs0,path=/share,security_model=none
-device virtio-9p-pci,fsdev=virtfs0,mount_tag=hostshare
```

`security_model=none` gives the guest read-write access to the host
directory; point it at a throwaway directory.

In the guest:

```
# settrans -c /dev/virtio-9p0 /hurd/virtio-9p --mount-tag=hostshare
# settrans -c /host /hurd/9pfs /dev/virtio-9p0
# ls /host
```

## Options

- `--device=SLOT`: use the virtio-9p device at this PCI slot instead of the
  first match
- `--mount-tag=NAME`: require the device's mount tag to be NAME (default:
  hostshare)
- `--selftest`: bring the device up, do one 9P Tversion round trip, and exit

The node is single-open (a second open gets EBUSY), matching 9pfs, which
opens it once and duplicates the fd into input and output streams. seek and
map are refused (ESPIPE and EOPNOTSUPP).

## Why not NetBSD's vio9p

NetBSD ships a complete kernel virtio-9p driver, but it is kernel code, and
the Hurd's microkernel has no virtio support at all. The rump route would
need a vio9p rump module that NetBSD never built, a character-device bridge
from rump into the Hurd, an unproven virtio-over-rump interrupt path, and a
whole NetBSD kernel personality per translator instance, all to feed the same
9pfs front end the Hurd already has. A native userspace driver costs less
than that bridge, so vio9p.c is kept as a design reference, not a dependency.

## Contributing

Post your AI vibeslop to https://github.com/hurdkit/virtio-9p/.

## License

virtio-9p is distributed under the GNU General Public License, version 3
(see LICENSE).
