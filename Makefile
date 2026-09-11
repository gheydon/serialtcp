# SerialTCP -- virtual serial device + modem daemon for AmigaOS
#
# Cross-built with bebbo's m68k-amigaos-gcc, normally inside the
# amigadev/crosstools container.  Use ./build.sh to drive that, or run make
# directly if you have the toolchain on your PATH.

CC      = m68k-amigaos-gcc
STRIP   = m68k-amigaos-strip

BUILD   = build
INC     = -Iinclude -Idaemon -Itools

# -m68000 so it runs on everything from a stock A500 upwards.
# -fomit-frame-pointer and -O2 keep the resident code small, which matters
# more than raw speed on real hardware.
CFLAGS_COMMON = -m68000 -O2 -Wall -Wextra -Wno-unused-parameter \
                -fomit-frame-pointer $(INC)

# The device is a bare Exec device: no C runtime at all.
# DEV_EXTRA lets you trade diagnostics for size:
#   make DEV_EXTRA=-DST_MINIMAL
# drops the device's console error messages (~370 bytes). The specific
# io_Error codes are always present either way.
DEV_EXTRA  ?=
DEV_CFLAGS  = $(CFLAGS_COMMON) -nostdlib -nostartfiles $(DEV_EXTRA)
DEV_LDFLAGS = -nostdlib -nostartfiles -s

# The daemon and tools are ordinary programs. -noixemul links libnix instead
# of ixemul, so there is no ixemul.library dependency at runtime.
APP_CFLAGS  = $(CFLAGS_COMMON) -noixemul
APP_LDFLAGS = -noixemul -s

DEV_OBJS = $(BUILD)/device.o

DAEMON_OBJS = $(BUILD)/main.o $(BUILD)/node.o $(BUILD)/net.o \
              $(BUILD)/atcmd.o $(BUILD)/telnet.o $(BUILD)/config.o \
              $(BUILD)/util.o $(BUILD)/fifo.o $(BUILD)/queue.o $(BUILD)/qutil.o

STAT_OBJS   = $(BUILD)/stat.o $(BUILD)/graph.o $(BUILD)/statcommon.o
CLI_OBJS    = $(BUILD)/statcli.o $(BUILD)/statcommon.o
SERTEST_OBJS= $(BUILD)/sertest.o

TARGETS = $(BUILD)/serialtcp.device $(BUILD)/SerialTCPd \
          $(BUILD)/SerialTCPStat $(BUILD)/SerialTCPStatus \
          $(BUILD)/SerialTest

all: $(BUILD) $(TARGETS)

$(BUILD):
	mkdir -p $(BUILD)

# ---- device -------------------------------------------------------------

$(BUILD)/device.o: device/device.c include/serialtcp.h
	$(CC) $(DEV_CFLAGS) -c -o $@ $<

$(BUILD)/serialtcp.device: $(DEV_OBJS)
	$(CC) $(DEV_LDFLAGS) -o $@ $(DEV_OBJS)

# ---- daemon -------------------------------------------------------------

$(BUILD)/%.o: daemon/%.c daemon/daemon.h include/serialtcp.h
	$(CC) $(APP_CFLAGS) -c -o $@ $<

$(BUILD)/SerialTCPd: $(DAEMON_OBJS)
	$(CC) $(APP_LDFLAGS) -o $@ $(DAEMON_OBJS) -lamiga

# ---- status client ------------------------------------------------------

# MUI's own macros pass plain char* where the inlines want CONST_STRPTR, so the
# signedness warning here is the header's, not ours.
$(BUILD)/%.o: tools/%.c include/serialtcp.h
	$(CC) $(APP_CFLAGS) -Wno-pointer-sign -c -o $@ $<

$(BUILD)/SerialTCPStat: $(STAT_OBJS)
	$(CC) $(APP_LDFLAGS) -o $@ $(STAT_OBJS) -lamiga

# Shell-only client: no MUI, so it works over a serial console and in scripts.
$(BUILD)/SerialTCPStatus: $(CLI_OBJS)
	$(CC) $(APP_LDFLAGS) -o $@ $(CLI_OBJS) -lamiga

# Stand-in BBS node, for smoke-testing the device without a real BBS.
$(BUILD)/SerialTest: $(SERTEST_OBJS)
	$(CC) $(APP_LDFLAGS) -o $@ $(SERTEST_OBJS) -lamiga

# ---- native unit tests --------------------------------------------------
#
# The FIFO, telnet codec and AT parser are pure logic, so they can be built
# and run on the build host against the stub Amiga headers in tests/fake.
# Run with: make test  (uses the host cc, not the cross-compiler)

test: $(BUILD)
	$(HOSTCC) -g -O1 -Wall -Wno-unused-parameter -Itests/fake -Iinclude -Idaemon \
	    -o $(BUILD)/tests tests/test_main.c daemon/fifo.c daemon/telnet.c daemon/atcmd.c daemon/qutil.c
	$(BUILD)/tests

HOSTCC ?= cc

clean:
	rm -rf $(BUILD)

.PHONY: all clean test
