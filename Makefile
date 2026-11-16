# virtio: virtio-9p translator, virtio-balloon and virtio-blk drivers for
# GNU/Hurd.
#
# Built against the x86_64-gnu cross prefix by the hurdkit toolchain
# (`make -C toolchain virtio-9p` passes CC, CPATH and LIBS); for a manual
# host-local build set CC/CPATH/LIBS the same way.

CC ?= cc
CFLAGS ?= -O2 -g -Wall

all: virtio-9p virtio-balloon virtio-blk vblk-client

# The transport core is shared by all drivers; the 9p translator adds the
# trivfs/ports stack, the daemons link lean.
TRANSPORT = virtio-pci.o virtio-queue.o
LIBS = shouldbeinlibc ihash ports iohelp fshelp trivfs pciaccess \
       hurduser machuser irqhelp
LIBS_DAEMON = shouldbeinlibc pciaccess machuser irqhelp
# virtio-blk links the trivfs/ports stack too since Stage C: the binary
# doubles as the Mach device translator (`--serve`).
LIBS_BLK = shouldbeinlibc ihash ports iohelp fshelp trivfs pciaccess \
           hurduser machuser irqhelp
# vblk-client is a plain RPC client: file_name_lookup (hurduser),
# device_* (machuser), vm_deallocate (mach).
LIBS_CLIENT = hurduser machuser

OBJS9P = virtio-9p.o virtio-9p-req.o virtio-9p-ios.o $(TRANSPORT)
OBJSBAL = virtio-balloon.o virtio-balloon-dev.o virtio-balloon-mem.o \
          $(TRANSPORT)
OBJSBLK = virtio-blk.o virtio-blk-dev.o virtio-blk-req.o virtio-blk-hurd.o \
          virtio-blk-serve.o deviceServer.o notifyServer.o $(TRANSPORT)

# MIG server stubs, generated the way the Hurd tree's Makeconf does it
# (cpp pass with -DSERVERPREFIX=S_, then `mig -ccat -subrprefix __`).
# The device stubs come from the cross prefix's gnumach headers, so the
# wire contract is exactly what the guest kernel's device.defs says.
HURD_PREFIX ?= $(HOME)/opt/x86_64-gnu
HURD_SRC ?= $(HOME)/src/hurdkit-2/3p/hurd
MIG ?= mig
# The defs preprocessing pass runs the host cpp directly: the cross
# compiler would treat the unknown `.defs' extension as a linker input
# ("linker input file unused because linking not done") instead of
# preprocessing it, and make's builtin CPP is $(CC) -E, so this uses a
# dedicated variable (Hurd's Makeconf rules use $(CPP) for the same
# reason).
MIG_CPP ?= cpp
MIGCPPFLAGS = -I$(HURD_PREFIX)/include -imacros mig-mutate.h \
              -DMACH_PAYLOAD_TO_PORT=ports_payload_get_name
MIGCOMFLAGS =
# mig re-runs its internal cpp over the input, so unsupported switches
# (like the older `-ccat -subrprefix __' pair the Hurd tree passes its
# MIGCOM) must not be given to this one: unknown switches are forwarded
# to the preprocessor.

virtio-9p: $(OBJS9P)
	$(CC) $(CFLAGS) $^ -o $@ -pthread $(foreach LIB,$(LIBS),-l$(LIB))

virtio-balloon: $(OBJSBAL)
	$(CC) $(CFLAGS) $^ -o $@ -pthread $(foreach LIB,$(LIBS_DAEMON),-l$(LIB))

virtio-blk: $(OBJSBLK)
	$(CC) $(CFLAGS) $^ -o $@ -pthread $(foreach LIB,$(LIBS_BLK),-l$(LIB))

vblk-client: vblk-client.o
	$(CC) $(CFLAGS) $^ -o $@ $(foreach LIB,$(LIBS_CLIENT),-l$(LIB))

# device.defs (subsystem 2800) carries device_open_new only under
# DEVICE_ENABLE_DEVICE_OPEN_NEW; with it, ds_device_open_new is
# generated and aliased in virtio-blk-serve.c, matching the
# gnumach/devnode/libmachdev precedent.
DEFS_CPPFLAGS = $(MIGCPPFLAGS) -DDEVICE_ENABLE_DEVICE_OPEN_NEW \
                -DHURD_SERVER=1 -DSERVERPREFIX=S_

device.sdefsi: $(HURD_PREFIX)/include/device/device.defs mig-mutate.h
	$(MIG_CPP) $(DEFS_CPPFLAGS) -MD -MF $@.d $< -o $@

deviceServer.c device_S.h: device.sdefsi
	$(MIG) $(MIGCOMFLAGS) -sheader device_S.h -server deviceServer.c \
	    -user /dev/null -header /dev/null $<

notify.sdefsi: $(HURD_PREFIX)/include/mach/notify.defs
	$(MIG_CPP) $(MIGCPPFLAGS) -DHURD_SERVER=1 -DSERVERPREFIX=S_ -MD -MF $@.d \
	    $< -o $@

notifyServer.c notify_S.h: notify.sdefsi
	$(MIG) $(MIGCOMFLAGS) -sheader notify_S.h -server notifyServer.c \
	    -user /dev/null -header /dev/null $<

%.o: %.c virtio-pci.h virtio-queue.h virtio-9p.h virtio-balloon.h \
     virtio-blk.h virtio-blk-serve.h
	$(CC) $(CFLAGS) -D_GNU_SOURCE -c $< -o $@

# The MIG stubs include "device_S.h"/"notify_S.h" (generated here) and
# libports/ports.h + libports/mig-decls.h (not installed in the prefix;
# they live in the pinned Hurd source tree).  The _S.h headers carry the
# same imports, so virtio-blk-serve.o needs the paths too.
deviceServer.o notifyServer.o virtio-blk-serve.o: virtio-blk-serve.h
deviceServer.o notifyServer.o virtio-blk-serve.o: \
    CFLAGS += -I. -I$(HURD_SRC)
virtio-blk-serve.o: device_S.h notify_S.h

clean:
	rm -f $(OBJS9P) $(OBJSBAL) $(OBJSBLK) virtio-9p virtio-balloon \
	    virtio-blk vblk-client vblk-client.o device.sdefsi* \
	    deviceServer.c device_S.h notify.sdefsi* notifyServer.c \
	    notify_S.h

.PHONY: clean all
