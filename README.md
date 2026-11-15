# virtio

Userspace virtio drivers for GNU Hurd. AI patches are ENCOURAGED.

Two separate daemons share the generic legacy virtio PCI transport
(`virtio-pci.c`, `virtio-queue.c`) and run as independent processes, each
probing and owning its own PCI device:

- `virtio-9p` exposes QEMU's virtio-9p PCI device (`0x1af4:0x1009`) as a
  character device carrying the raw 9P message stream, so the existing,
  unmodified 9pfs translator can mount a host directory in the guest.
- `virtio-balloon` makes the guest participate in virtio memory ballooning
  through QEMU's virtio-balloon PCI device (`0x1af4:0x1002`), so the host
  can dynamically reclaim or return guest RAM at runtime.

## How it works

Both drivers use the same transport: PCI discovery through libpciaccess (a
pci-arbiter client, so the guest must run the pci-arbiter translator and
the driver must not touch the PCI config ports itself), the legacy virtio
register interface in an I/O BAR, the reset/ACK/DRIVER/feature/DRIVER_OK
handshake, split virtqueues in physically contiguous memory
(`vm_allocate_contiguous`, which returns the machine physical address the
device DMAes to), and INTx interrupts driven by libirqhelp with the
read-to-clear ISR discipline.

```
QEMU host
  |  virtio-9p PCI device                virtio-balloon PCI device
  v                                        v
/dev/virtio-9p0  this translator      /hurd/virtio-balloon  standalone daemon
  |  9P message stream                   |  config target/actual + PFN queues
  v                                      v
/hurd/9pfs         unmodified           Mach VM  driver-owned chunks whose
  |  Hurd filesystem RPCs               PFNs are posted on inflateq/deflateq
  v                                      v
/host              mount point          host reclaims/returns guest RAM
```

The balloon driver is a plain daemon, not a translator: ballooning is driven
entirely through the device config region and the two PFN virtqueues, so
there is no device node and the 9p translator is not on its path.

## Requirements

The guest must run the pci-arbiter translator, the devices must present the
legacy virtio PCI interface (the QEMU default on i440fx), and the balloon
daemon needs the privileges behind `get_privileged_ports` (it runs
`vm_allocate_contiguous` for its page pool), i.e. it runs as root.

## Building

Build with the x86_64-gnu cross toolchain. The Hurd toolchain harness at
https://github.com/hurdkit/hurdkit/ has a `virtio-9p` target that sets
CC/CPATH/LIBS; for a manual build, set them the same way.

```
make                     # builds virtio-9p and virtio-balloon
make virtio-balloon      # balloon driver only
make CFLAGS=-g           # debugging build
```

## virtio-9p usage

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

Options:

- `--device=SLOT`: use the virtio-9p device at this PCI slot instead of the
  first match
- `--mount-tag=NAME`: require the device's mount tag to be NAME (default:
  hostshare)
- `--selftest`: bring the device up, do one 9P Tversion round trip, and exit

The node is single-open (a second open gets EBUSY), matching 9pfs, which
opens it once and duplicates the fd into input and output streams. seek and
map are refused (ESPIPE and EOPNOTSUPP).

## virtio-balloon usage

On the host, start QEMU with the device:

```
-device virtio-balloon-pci
```

In the guest, start the daemon and keep it running:

```
# /hurd/virtio-balloon
```

Then drive it from the host monitor:

```
(qemu) info balloon      # guest actual, as the host sees it
(qemu) balloon 512       # lower the target: guest inflates
(qemu) balloon 1024      # raise it: guest deflates
```

Options:

- `--device=SLOT`: use the virtio-balloon device at this PCI slot instead
  of the first match
- `--selftest`: bring the device up, verify the handshake, features, both
  queues and the config region, run one inflate/deflate round trip with a
  single page, and print PASS/FAIL diagnostics
- `--status`: print the device's current target/actual and exit; reads the
  config region only, so it never resets or disturbs a running driver
- `--verbose`: log every queue posting and completion

Runtime logs (stderr) show the initial target, every batch's page counts and
the reported `actual`, and bring-up/teardown diagnostics.

## Balloon protocol notes

MVP scope is the classic inflate/deflate pair only: legacy PCI transport,
no stats/free-page-hint/reporting/poison features, no deflate-on-OOM, no
MSI-X. The only accepted feature is `VIRTIO_BALLOON_F_MUST_TELL_HOST`,
accepted when offered; the deflate path enforces its ordering regardless.

Both queues carry driver-written, device-readable arrays of little-endian
32-bit PFNs (4 KiB units) as single output descriptors. The accounting rule
is fixed: a page counts towards `actual` only after the used ring has
acknowledged the batch carrying it, and pages selected for deflation stay
mapped until that acknowledgement, so the host is always told before a page
is reusable. The page pool is driver-owned `vm_allocate_contiguous` chunks
of 16 pages; chunk granularity and allocation failures bound the reachable
balloon size. The convergence loop wakes on config-change interrupts with a
1 s tick, and shutdown deflates best-effort before resetting the device.

## Why not NetBSD's vio9p

NetBSD ships a complete kernel virtio-9p driver, but it is kernel code, and
the Hurd's microkernel has no virtio support at all. The rump route would
need a vio9p rump module that NetBSD never built, a character-device bridge
from rump into the Hurd, an unproven virtio-over-rump interrupt path, and a
whole NetBSD kernel personality per translator instance, all to feed the same
9pfs front end the Hurd already has. A native userspace driver costs less than
that bridge, so vio9p.c is kept as a design reference, not a dependency.

## Contributing

Post your AI vibeslop to https://github.com/hurdkit/virtio-9p/.

## License

virtio is distributed under the GNU General Public License, version 3
(see LICENSE).
