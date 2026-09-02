# usbmon — native USB storage monitor (C99)
#
# Linux/macOS:   make            (daemon + usbmon-toast popup helper)
# Windows:       use MSVC or mingw-w64 (see "windows" target below)
#
# Cross-compile from Linux (if mingw-w64 is installed):
#   make CROSS=x86_64-w64-mingw32- windows
#
# Release artifacts (strict build → strip → tarball + SHA256SUMS):
#   make dist
#
# The daemon NEVER links X11: popups are rendered by usbmon-toast, a
# helper binary spawned per event.  When X11/Xft dev files are absent,
# the helper is simply not built and the daemon runs headless.

CC      ?= cc
CROSS   ?=
CFLAGS  ?= -std=c99 -O2 -Wall -Wextra -pedantic
LDFLAGS ?=

VERSION   := $(shell sed -n 's/^#define UM_VERSION "\(.*\)"/\1/p' src/usbmon.h)
DIST_NAME := usbmon-$(VERSION)-linux-amd64

SRC_COMMON  = src/main.c src/util.c src/logjson.c src/json.c src/hook.c \
              src/lock.c src/gui.c src/hotpath.c
SRC_LINUX   = $(SRC_COMMON) src/scan_linux.c
SRC_WIN     = $(SRC_COMMON) src/scan_win32.c src/gui_win32.c

# --- X11/Xft availability probe (for usbmon-toast only) ---------------------
XFT_CFLAGS := $(shell pkg-config --cflags x11 xft 2>/dev/null)
XFT_LIBS   := $(shell pkg-config --libs x11 xft 2>/dev/null)
ifeq ($(strip $(XFT_LIBS)),)
XFT_LIBS := -lX11 -lXft
endif
HAVE_X11 := $(shell printf 'int main(void){return 0;}' > /tmp/usbmon-x11probe.c && \
        $(CC) -std=c99 /tmp/usbmon-x11probe.c -o /tmp/usbmon-x11probe \
        $(XFT_CFLAGS) $(XFT_LIBS) >/dev/null 2>&1 && echo yes)

# --- targets ----------------------------------------------------------------
.PHONY: all clean windows strict analyze asan gui dist

all: usbmon $(if $(HAVE_X11),usbmon-toast,)

usbmon: $(SRC_LINUX) src/usbmon.h
	$(CC) $(CFLAGS) $(SRC_LINUX) -o $@ $(LDFLAGS)

# popup helper (X11 + Xft; C99-pedantic clean)
usbmon-toast: src/gui_toast.c src/usbmon.h
	$(CC) $(CFLAGS) $(XFT_CFLAGS) src/gui_toast.c -o $@ $(XFT_LIBS) $(LDFLAGS)

ifeq ($(HAVE_X11),yes)
gui: usbmon-toast
else
gui:
	@echo "usbmon: X11/Xft dev files not found — toast helper not built"
	@echo "        (install libx11-dev libxft-dev and re-run make)"
endif

# stricter build used before release (treats warnings as errors)
strict:
	$(CC) -std=c99 -O2 -Wall -Wextra -pedantic -Werror $(SRC_LINUX) -o usbmon $(LDFLAGS)
ifneq ($(strip $(HAVE_X11)),)
	$(CC) -std=c99 -O2 -Wall -Wextra -pedantic -Werror $(XFT_CFLAGS) src/gui_toast.c -o usbmon-toast $(XFT_LIBS) $(LDFLAGS)
endif

# GCC static analyzer pass (gcc >= 10)
analyze:
	$(CC) -std=c99 -O2 -Wall -Wextra -fanalyzer $(SRC_LINUX) -o /dev/null $(LDFLAGS)

# sanitizer build for the end-to-end demo
asan:
	$(CC) -std=c99 -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
	      $(SRC_LINUX) -o usbmon-asan $(LDFLAGS)

# release packaging: strict build → stripped copies → tarball + SHA256SUMS.
# Same artifacts the release workflow uploads (CI runs `make dist` too).
dist: strict
	@echo ">> packaging $(DIST_NAME) (version $(VERSION))"
	@rm -rf dist
	@mkdir -p dist/$(DIST_NAME)
	@cp usbmon usbmon-toast README.md LICENSE dist/$(DIST_NAME)/
	@strip dist/$(DIST_NAME)/usbmon dist/$(DIST_NAME)/usbmon-toast
	@tar -C dist -czf dist/$(DIST_NAME).tar.gz $(DIST_NAME)
	@cd dist && sha256sum "$(DIST_NAME).tar.gz" > SHA256SUMS.txt
	@cd dist/$(DIST_NAME) && sha256sum usbmon usbmon-toast >> ../SHA256SUMS.txt
	@echo ">> dist artifacts:"
	@ls -la dist/
	@cat dist/SHA256SUMS.txt

windows: $(SRC_WIN) src/usbmon.h
	$(CROSS)gcc -std=c99 -O2 -Wall -Wextra $(SRC_WIN) -o usbmon.exe $(LDFLAGS) \
	    -luser32 -lgdi32 -lshell32

clean:
	rm -f usbmon usbmon-toast usbmon-asan usbmon.exe
	rm -rf dist
