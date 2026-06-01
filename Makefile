# Makefile for warpzone
#
# Targets:
#   make            - release build
#   make debug      - debug build
#   make clean      - remove build artifacts
#   make install    - install to /usr/local/bin (override PREFIX)
#   make deb        - build a .deb package (binary + man page)
#
# Usage:
#   make
#   make debug
#   make PREFIX=$HOME/.local install
#   make deb

CC      ?= gcc
PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin
MANDIR  ?= $(PREFIX)/share/man/man1

TARGET  := warpzone
SRC     := warpzone.c
MANPAGE := warpzone.1

# Version is pulled from the source so the package never drifts from the binary.
VERSION  := $(shell sed -n 's/.*WARPZONE_VERSION[ \t]*"\([^"]*\)".*/\1/p' $(SRC))

# Debian packaging settings.
DEB_ARCH   := $(shell dpkg --print-architecture 2>/dev/null || echo amd64)
MAINTAINER := Eli Fulkerson <elifulkerson@gmail.com>
DEB_ROOT   := debpkg
DEB_PKG    := $(TARGET)_$(VERSION)_$(DEB_ARCH).deb

CFLAGS_COMMON := -Wall -Wextra -pedantic
CFLAGS_RELEASE := -O2
CFLAGS_DEBUG   := -O0 -g

.PHONY: all release debug clean install uninstall deb

all: release

release: CFLAGS := $(CFLAGS_COMMON) $(CFLAGS_RELEASE)
release: $(TARGET)

debug: CFLAGS := $(CFLAGS_COMMON) $(CFLAGS_DEBUG)
debug: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^

install: $(TARGET)
	install -d $(BINDIR)
	install -m 0755 $(TARGET) $(BINDIR)/$(TARGET)
	install -d $(MANDIR)
	install -m 0644 $(MANPAGE) $(MANDIR)/$(MANPAGE)

uninstall:
	rm -f $(BINDIR)/$(TARGET)
	rm -f $(MANDIR)/$(MANPAGE)

# Build a .deb that installs the binary to /usr/bin and the man page to
# /usr/share/man/man1. Packaged files are owned by root via --root-owner-group,
# so neither root nor fakeroot is required to build the package.
deb: release $(MANPAGE)
	@test -n "$(VERSION)" || { echo "error: could not determine VERSION from $(SRC)"; exit 1; }
	rm -rf $(DEB_ROOT)
	install -d $(DEB_ROOT)/DEBIAN
	install -d $(DEB_ROOT)/usr/bin
	install -d $(DEB_ROOT)/usr/share/man/man1
	install -m 0755 $(TARGET) $(DEB_ROOT)/usr/bin/$(TARGET)
	-strip $(DEB_ROOT)/usr/bin/$(TARGET)
	gzip -9 -n -c $(MANPAGE) > $(DEB_ROOT)/usr/share/man/man1/$(MANPAGE).gz
	chmod 0644 $(DEB_ROOT)/usr/share/man/man1/$(MANPAGE).gz
	printf 'Package: %s\n' '$(TARGET)'                      >  $(DEB_ROOT)/DEBIAN/control
	printf 'Version: %s\n' '$(VERSION)'                     >> $(DEB_ROOT)/DEBIAN/control
	printf 'Section: utils\n'                               >> $(DEB_ROOT)/DEBIAN/control
	printf 'Priority: optional\n'                           >> $(DEB_ROOT)/DEBIAN/control
	printf 'Architecture: %s\n' '$(DEB_ARCH)'               >> $(DEB_ROOT)/DEBIAN/control
	printf 'Depends: libc6\n'                               >> $(DEB_ROOT)/DEBIAN/control
	printf 'Maintainer: %s\n' '$(MAINTAINER)'               >> $(DEB_ROOT)/DEBIAN/control
	printf 'Description: Seamless PTY wrapper with on-demand capture and local pipeline\n' >> $(DEB_ROOT)/DEBIAN/control
	printf ' warpzone wraps an interactive shell or program in a PTY and lets you\n'       >> $(DEB_ROOT)/DEBIAN/control
	printf ' capture its output on demand (Ctrl-]) and pipe it through a shell\n'          >> $(DEB_ROOT)/DEBIAN/control
	printf ' pipeline executed on the local machine.\n'                                    >> $(DEB_ROOT)/DEBIAN/control
	dpkg-deb --root-owner-group --build $(DEB_ROOT) $(DEB_PKG)
	@echo "Built $(DEB_PKG)"

clean:
	rm -f $(TARGET)
	rm -rf $(DEB_ROOT)
	rm -f $(TARGET)_*.deb
