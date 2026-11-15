# virtio: virtio-9p translator, virtio-balloon and virtio-blk drivers for
# GNU/Hurd.
#
# Built against the x86_64-gnu cross prefix by the hurdkit toolchain
# (`make -C toolchain virtio-9p` passes CC, CPATH and LIBS); for a manual
# host-local build set CC/CPATH/LIBS the same way.

CC ?= cc
CFLAGS ?= -O2 -g -Wall

all: virtio-9p virtio-balloon virtio-blk

# The transport core is shared by all drivers; the 9p translator adds the
# trivfs/ports stack, the daemons link lean.
TRANSPORT = virtio-pci.o virtio-queue.o
LIBS = shouldbeinlibc ihash ports iohelp fshelp trivfs pciaccess \
       hurduser machuser irqhelp
LIBS_DAEMON = shouldbeinlibc pciaccess machuser irqhelp

OBJS9P = virtio-9p.o virtio-9p-req.o virtio-9p-ios.o $(TRANSPORT)
OBJSBAL = virtio-balloon.o virtio-balloon-dev.o virtio-balloon-mem.o \
          $(TRANSPORT)
OBJSBLK = virtio-blk.o virtio-blk-dev.o virtio-blk-req.o virtio-blk-hurd.o \
          $(TRANSPORT)

virtio-9p: $(OBJS9P)
	$(CC) $(CFLAGS) $^ -o $@ -pthread $(foreach LIB,$(LIBS),-l$(LIB))

virtio-balloon: $(OBJSBAL)
	$(CC) $(CFLAGS) $^ -o $@ -pthread $(foreach LIB,$(LIBS_DAEMON),-l$(LIB))

virtio-blk: $(OBJSBLK)
	$(CC) $(CFLAGS) $^ -o $@ -pthread $(foreach LIB,$(LIBS_DAEMON),-l$(LIB))

%.o: %.c virtio-pci.h virtio-queue.h virtio-9p.h virtio-balloon.h virtio-blk.h
	$(CC) $(CFLAGS) -D_GNU_SOURCE -c $< -o $@

clean:
	rm -f $(OBJS9P) $(OBJSBAL) $(OBJSBLK) virtio-9p virtio-balloon virtio-blk

.PHONY: clean all
