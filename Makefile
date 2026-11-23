CC ?= cc
PKGS = cairo pangocairo wayland-client xkbcommon
PROTO_DIR := protocols
SRC_DIR := src

CFLAGS ?= -march=native -O2 -g -std=c11 -Wall -Wextra -flto -s
CFLAGS += $(shell pkg-config --cflags $(PKGS)) -pthread -I$(PROTO_DIR)
LDLIBS = $(shell pkg-config --libs $(PKGS)) -lm -pthread

PROTO_SRC = $(PROTO_DIR)/xdg-shell-protocol.c \
            $(PROTO_DIR)/xdg-decoration-protocol.c \
            $(PROTO_DIR)/xdg-toplevel-icon-protocol.c \
            $(PROTO_DIR)/cursor-shape-protocol.c \
            $(PROTO_DIR)/tablet-protocol.c

PROTO_HDRS = $(PROTO_DIR)/xdg-shell-client-protocol.h \
             $(PROTO_DIR)/xdg-decoration-client-protocol.h \
             $(PROTO_DIR)/xdg-toplevel-icon-client-protocol.h \
             $(PROTO_DIR)/cursor-shape-client-protocol.h \
             $(PROTO_DIR)/tablet-client-protocol.h

PREFIX ?= /usr/local
BINDIR := $(PREFIX)/bin
ICONDIR := $(PREFIX)/share/icons/hicolor
APPDIR := $(PREFIX)/share/applications

all: jtfiles jtfilesctl

jtfiles: jtfiles.c $(PROTO_SRC) $(PROTO_HDRS)
	$(CC) $(CFLAGS) -o $@ jtfiles.c $(PROTO_SRC) $(LDFLAGS) $(LDLIBS)

jtfilesctl: jtfilesctl.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) $(LDLIBS)

install: jtfiles jtfilesctl
	install -Dm755 jtfiles $(DESTDIR)$(BINDIR)/jtfiles
	install -Dm755 jtfilesctl $(DESTDIR)$(BINDIR)/jtfilesctl
	install -Dm644 $(SRC_DIR)/icons/jtfiles.svg \
		$(DESTDIR)$(ICONDIR)/scalable/apps/jtfiles.svg
	install -Dm644 $(SRC_DIR)/icons/jtfiles.png \
		$(DESTDIR)$(ICONDIR)/256x256/apps/jtfiles.png
	install -Dm644 $(SRC_DIR)/jtfiles.desktop \
		$(DESTDIR)$(APPDIR)/jtfiles.desktop
	@if command -v gtk-update-icon-cache >/dev/null 2>&1; then \
		gtk-update-icon-cache -f -t $(DESTDIR)$(ICONDIR) >/dev/null 2>&1 || true; \
	fi
	@if command -v update-desktop-database >/dev/null 2>&1; then \
		update-desktop-database $(DESTDIR)$(APPDIR) >/dev/null 2>&1 || true; \
	fi
	@echo "installed to $(DESTDIR)$(PREFIX)"

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/jtfiles $(DESTDIR)$(BINDIR)/jtfilesctl
	rm -f $(DESTDIR)$(ICONDIR)/scalable/apps/jtfiles.svg
	rm -f $(DESTDIR)$(ICONDIR)/256x256/apps/jtfiles.png
	rm -f $(DESTDIR)$(APPDIR)/jtfiles.desktop
	@if command -v gtk-update-icon-cache >/dev/null 2>&1; then \
		gtk-update-icon-cache -f -t $(DESTDIR)$(ICONDIR) >/dev/null 2>&1 || true; \
	fi
	@if command -v update-desktop-database >/dev/null 2>&1; then \
		update-desktop-database $(DESTDIR)$(APPDIR) >/dev/null 2>&1 || true; \
	fi

clean:
	rm -f jtfiles jtfilesctl

.PHONY: all install uninstall clean
