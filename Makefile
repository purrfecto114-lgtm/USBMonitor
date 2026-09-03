# usbmon — native USB storage monitor (C99)
#
# Linux/macOS:   make            (daemon + usbmon-toast popup helper)
# Windows:       make CROSS=x86_64-w64-mingw32- windows
#                (mingw-w64 cross-compile; -static: the exe imports only
#                 Windows system DLLs — KERNEL32/msvcrt/user32/gdi32/
#                 shell32/advapi32 — and needs NO runtime installation on
#                 the target box; -mwindows: GUI subsystem, so double-
#                 clicking usbmon.exe shows the tray, not a black console)
#
# Release artifacts (primary target platform: Windows):
#   make static         (musl, fully self-contained Linux daemon)
#   make dist           (strict build -> package tarball + SHA256SUMS)
#   make dist-windows   (mingw exe -> zip; appends to SHA256SUMS —
#                        run it AFTER `make dist`)
#
# The daemon NEVER links X11: popups are rendered by usbmon-toast, a
# helper binary spawned per event.  When X11/Xft dev files are absent,
# the helper is simply not built and the daemon runs headless.

CC      ?= cc
CROSS   ?=
CFLAGS  ?= -std=c99 -O2 -Wall -Wextra -pedantic
LDFLAGS ?=

# musl toolchain for the fully-static release binary (daemon only;
# usbmon-toast needs X11/Xft/fontconfig and stays dynamically linked —
# CI builds it against a glibc 2.31 baseline, see release.yml).
# CI installs musl-tools; locally musl-gcc may be absent, in which case
# `make static` fails loudly and `make dist` falls back to the dynamic
# daemon with a warning.
MUSL_CC := $(shell command -v musl-gcc 2>/dev/null)

VERSION   := $(shell sed -n 's/^#define UM_VERSION "\(.*\)"/\1/p' src/usbmon.h)
DIST_NAME := usbmon-$(VERSION)-linux-amd64

SRC_COMMON  = src/main.c src/util.c src/logjson.c src/json.c src/hook.c \
	      src/lock.c src/gui.c src/hotpath.c
SRC_LINUX   = $(SRC_COMMON) src/scan_linux.c
SRC_WIN     = $(SRC_COMMON) src/scan_win32.c src/gui_win32.c src/tray_win32.c

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
.PHONY: all clean windows strict analyze asan gui dist dist-windows static

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

# fully static daemon via musl: no interpreter, no glibc symbol-version
# baseline — runs on any x86-64 Linux kernel the compiler targets.
# (The code never calls NSS/user-database/network resolution services,
# which are the classic reasons glibc -static misbehaves; musl has no
# such dlopen machinery in the first place.)
usbmon-static: $(SRC_LINUX) src/usbmon.h
ifneq ($(strip $(MUSL_CC)),)
	$(MUSL_CC) -std=c99 -O2 -Wall -Wextra -pedantic -Werror $(SRC_LINUX) \
		-o $@ -static $(LDFLAGS)
else
	@echo "usbmon: musl-gcc not found — static build unavailable"
	@echo "        (install musl-tools: sudo apt-get install musl-tools)"
	@exit 1
endif

static: usbmon-static

# release packaging: strict build -> stripped copies -> tarball + SHA256SUMS.
# Depends on the dynamic daemon target only (as a fallback); everything
# else is packaged as-is so a CI-built usbmon-static (musl) or a
# glibc-baseline usbmon-toast is NEVER overwritten by a rebuild here.
# usbmon-toast is built only if missing (requires X11/Xft dev files).
dist: usbmon
	@echo ">> packaging $(DIST_NAME) (version $(VERSION))"
	@rm -rf dist
	@mkdir -p dist/$(DIST_NAME)
	@if [ -f usbmon-static ]; then \
		cp usbmon-static dist/$(DIST_NAME)/usbmon; \
		echo ">> daemon: musl STATIC build (self-contained, no interpreter)"; \
	else \
		echo ">> WARNING: usbmon-static missing — shipping DYNAMIC daemon,"; \
		echo ">>          glibc symbol baseline of this build host applies."; \
		cp usbmon dist/$(DIST_NAME)/usbmon; \
	fi
	@if [ ! -f usbmon-toast ]; then \
		echo ">> usbmon-toast missing — building it now (needs X11/Xft)"; \
		$(MAKE) --no-print-directory usbmon-toast || true; \
	fi
	@if [ -f usbmon-toast ]; then \
		cp usbmon-toast dist/$(DIST_NAME)/; \
		echo ">> toast: dynamic (needs libX11/libXft/fontconfig at runtime)"; \
	else \
		echo ">> toast: not packaged (X11/Xft dev files absent)"; \
	fi
	@cp README.md LICENSE dist/$(DIST_NAME)/
	@strip dist/$(DIST_NAME)/usbmon \
		$(if $(wildcard usbmon-toast),dist/$(DIST_NAME)/usbmon-toast,)
	@tar -C dist --owner=0 --group=0 -czf dist/$(DIST_NAME).tar.gz $(DIST_NAME)
	@cd dist && sha256sum "$(DIST_NAME).tar.gz" > SHA256SUMS.txt
	@cd dist/$(DIST_NAME) && sha256sum usbmon \
		$(if $(wildcard usbmon-toast),usbmon-toast,) >> ../SHA256SUMS.txt
	@echo ">> dist artifacts:"
	@ls -la dist/
	@cat dist/SHA256SUMS.txt

# Windows exe: strict, fully static, GUI subsystem.  Requires mingw-w64:
#   sudo apt-get install gcc-mingw-w64-x86-64
#   make CROSS=x86_64-w64-mingw32- windows
# res/usbmon.ico (embedded tray/executable icon) is committed; regenerate
# with tools/make_icon.py.  windres ships with the mingw binutils.
res/usbmon.res.o: res/usbmon.rc res/usbmon.ico
	$(CROSS)windres res/usbmon.rc -O coff -o $@

windows: $(SRC_WIN) src/usbmon.h res/usbmon.res.o
	$(CROSS)gcc -std=c99 -O2 -Wall -Wextra -pedantic -Werror $(SRC_WIN) \
	    res/usbmon.res.o -o usbmon.exe $(LDFLAGS) -mwindows -static \
	    -luser32 -lgdi32 -lshell32 -ladvapi32

# Windows packaging: zip + append to dist/SHA256SUMS.txt.
# Run AFTER `make dist` (which creates dist/ and writes the Linux sums);
# dist-windows only appends so one checksum file covers both platforms.
DIST_NAME_WIN := usbmon-$(VERSION)-windows-amd64
dist-windows: windows
	@echo ">> packaging $(DIST_NAME_WIN) (version $(VERSION))"
	@mkdir -p dist/$(DIST_NAME_WIN)
	@cp usbmon.exe dist/$(DIST_NAME_WIN)/
	@cp README.md LICENSE dist/$(DIST_NAME_WIN)/
	@if command -v $(CROSS)strip >/dev/null 2>&1; then \
		$(CROSS)strip dist/$(DIST_NAME_WIN)/usbmon.exe; \
	else \
		echo '>> NOTE: cross-strip missing, shipping unstripped exe'; \
	fi
	@cd dist && zip -q -r "$(DIST_NAME_WIN).zip" "$(DIST_NAME_WIN)"
	@cd dist && sha256sum "$(DIST_NAME_WIN).zip" >> SHA256SUMS.txt
	@echo ">> dist artifacts:"
	@ls -la dist/
	@cat dist/SHA256SUMS.txt

clean:
	rm -f usbmon usbmon-toast usbmon-asan usbmon-static usbmon.exe
	rm -f res/usbmon.res.o
	rm -rf dist
