# AGC-1 build.
#
# The GPU stack lives beside the system one, so both the headers and the
# libraries come from explicit prefixes rather than the default search paths.

MESA    ?= /opt/mesa-adreno
GLDEV   ?= $(HOME)/.local/gldev
PREFIX  ?= $(HOME)/.local

CFLAGS  ?= -O2 -Wall -Wextra -Wno-unused-parameter
CPPFLAGS = -I$(GLDEV)/usr/include
# rpath-link lets the linker resolve libEGL's own dependency on libgallium
# without LD_LIBRARY_PATH being set; rpath does the same at run time, so the
# binary finds the side-by-side stack rather than the stale system driver.
LDFLAGS  = -L$(MESA)/lib -Wl,-rpath,$(MESA)/lib -Wl,-rpath-link,$(MESA)/lib
LDLIBS   = -lEGL -lGL -lm

BIN     = agc
LIBEXEC = $(PREFIX)/lib/agc/agc
TOOLS   = agc-play agc-rec agc-from agc-to

.PHONY: all test bench install uninstall clean

all: $(BIN)

$(BIN): agc.c glboot.h
	$(CC) $(CFLAGS) $(CPPFLAGS) agc.c -o $@ $(LDFLAGS) $(LDLIBS)

test: $(BIN)
	@. $(MESA)/activate.sh; EGL_PLATFORM=surfaceless ./test.sh ./$(BIN)

bench: $(BIN)
	@. $(MESA)/activate.sh; EGL_PLATFORM=surfaceless \
	 ./$(BIN) bench frame4k.gray 3840 2160 75

# Installs the binary under lib/ and a launcher on PATH that points the
# process at the Turnip/Zink stack, so `agc` needs no environment set up.
# The tools under tools/ are plain shell and only ever call `agc` by name,
# so they inherit that launcher rather than duplicating the environment.
install: $(BIN) agc.sh.in
	install -d "$(DESTDIR)$(PREFIX)/bin" "$(DESTDIR)$(PREFIX)/lib/agc"
	install -m 755 $(BIN) "$(DESTDIR)$(LIBEXEC)"
	sed -e 's|@MESA@|$(MESA)|g' -e 's|@LIBEXEC@|$(LIBEXEC)|g' agc.sh.in \
	    > "$(DESTDIR)$(PREFIX)/bin/agc"
	chmod 755 "$(DESTDIR)$(PREFIX)/bin/agc"
	for t in $(TOOLS); do install -m 755 "tools/$$t" "$(DESTDIR)$(PREFIX)/bin/$$t"; done
	@echo "installed $(DESTDIR)$(PREFIX)/bin/agc -> $(LIBEXEC)"
	@echo "installed $(TOOLS)"

uninstall:
	rm -f "$(DESTDIR)$(PREFIX)/bin/agc" "$(DESTDIR)$(LIBEXEC)"
	for t in $(TOOLS); do rm -f "$(DESTDIR)$(PREFIX)/bin/$$t"; done
	-rmdir "$(DESTDIR)$(PREFIX)/lib/agc" 2>/dev/null || true

clean:
	rm -f $(BIN)
