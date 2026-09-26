# Build the shims that make phone hardware look like ordinary Linux devices:
#
#   libv4l2bridge.so  puts the phone's camera on /dev/video0
#   libpcibridge.so   keeps Chromium's GPU process alive so it can use the GPU
#   libnlbridge.so    lets udev-based device monitoring start, without which
#                     Chromium reports no cameras and no microphones
#   libsysfsbridge.so gives /sys/class/video4linux a readable entry, without
#                     which libudev and GStreamer enumerate no camera at all
#
# They are separate libraries, not one, so each can be disabled on its own
# when something goes wrong.
#
# No install target here on purpose: hw-enable owns installation, so that
# there is one place that knows what "installed" means and one place that
# can undo it.

CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -fPIC
LDFLAGS ?= -shared

# The client's location is compiled in as well as read from the environment,
# because an application that rewrites its environment (Chromium does) loses
# the variable before the camera is ever opened.
BRIDGE_CLIENT_PATH := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))../bridge_client)

V4L2    := libv4l2bridge.so
PCI     := libpcibridge.so
NL      := libnlbridge.so
SYSFS   := libsysfsbridge.so
TARGETS := $(V4L2) $(PCI) $(NL) $(SYSFS)

all: $(TARGETS)

$(V4L2): v4l2-shim.c
	$(CC) $(CFLAGS) -DBRIDGE_CLIENT_DEFAULT='"$(BRIDGE_CLIENT_PATH)"' \
	      $(LDFLAGS) -o $@ $< -ldl -lpthread

$(PCI): pci-shim.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< -ldl

$(NL): netlink-shim.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< -ldl -lpthread

$(SYSFS): sysfs-shim.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< -ldl

clean:
	rm -f $(TARGETS)

.PHONY: all clean
