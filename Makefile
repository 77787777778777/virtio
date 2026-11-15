# virtio-9p: a virtio-9p device translator for GNU/Hurd.
#
# Built against the x86_64-gnu cross prefix by `make -C toolchain virtio-9p`
# (which passes CC, CPATH and LIBS); for a manual host-local build set
# CC/CPATH/LIBS the same way.

CC ?= cc
CFLAGS ?= -O2 -g -Wall

SRCS = virtio-9p.c virtio-pci.c virtio-queue.c virtio-9p-req.c virtio-9p-ios.c
OBJS = $(SRCS:.c=.o)

LIBS = shouldbeinlibc ihash ports iohelp fshelp trivfs pciaccess \
       hurduser machuser irqhelp

virtio-9p: $(OBJS)
	$(CC) $(CFLAGS) $^ -o $@ -pthread $(foreach LIB,$(LIBS),-l$(LIB))

%.o: %.c virtio-9p.h
	$(CC) $(CFLAGS) -D_GNU_SOURCE -c $< -o $@

clean:
	rm -f $(OBJS) virtio-9p

.PHONY: clean
