# virtio: virtio-9p translator and virtio-balloon driver for GNU/Hurd.
#
# Built against the x86_64-gnu cross prefix by the hurdkit toolchain
# (`make -C toolchain virtio-9p` passes CC, CPATH and LIBS); for a manual
# host-local build set CC/CPATH/LIBS the same way.

CC ?= cc
CFLAGS ?= -O2 -g -Wall

all: virtio-9p virtio-balloon

# The transport core is shared by both drivers; the 9p translator adds the
# trivfs/ports stack, the balloon daemon links lean.
TRANSPORT = virtio-pci.o virtio-queue.o
LIBS = shouldbeinlibc ihash ports iohelp fshelp trivfs pciaccess \
       hurduser machuser irqhelp
LIBS_BALLOON = shouldbeinlibc pciaccess machuser irqhelp

OBJS9P = virtio-9p.o virtio-9p-req.o virtio-9p-ios.o $(TRANSPORT)
OBJSBAL = virtio-balloon.o virtio-balloon-dev.o virtio-balloon-mem.o \
          $(TRANSPORT)

virtio-9p: $(OBJS9P)
	$(CC) $(CFLAGS) $^ -o $@ -pthread $(foreach LIB,$(LIBS),-l$(LIB))

virtio-balloon: $(OBJSBAL)
	$(CC) $(CFLAGS) $^ -o $@ -pthread $(foreach LIB,$(LIBS_BALLOON),-l$(LIB))

%.o: %.c virtio-pci.h virtio-queue.h virtio-9p.h virtio-balloon.h
	$(CC) $(CFLAGS) -D_GNU_SOURCE -c $< -o $@

clean:
	rm -f $(OBJS9P) $(OBJSBAL) virtio-9p virtio-balloon

.PHONY: clean all
