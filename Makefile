# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Google Home Desktop: home.google.com in a libgsurf (WebKitGTK 6.0, GTK4)
# window. libgsurf is built from the vendor/gsurf submodule and linked
# statically, so the installed binary needs only the system WebKitGTK.
#
#   make                 build build/google-home
#   make run             build and run it from the tree
#   make install         install to ~/.local (PREFIX=/usr/local for all users)
#   make uninstall
#   make gsurfctl        build gsurf's control client (for --control)
#   make deps            fetch the gsurf submodule and its dependencies
#   make dist            source tarball of HEAD, gsurf included
#   make bindist         binary tarball that installs to /usr/local

VERSION := 0.2.0

PREFIX  ?= $(HOME)/.local
BINDIR  ?= $(PREFIX)/bin
DATADIR ?= $(PREFIX)/share
DESTDIR ?=

PKG_CONFIG ?= pkg-config
INSTALL    ?= install

GSURF_DIR := vendor/gsurf
GSURF_OUT := $(GSURF_DIR)/build/release
GSURF_LIB := $(GSURF_OUT)/libgsurf.a
# STATIC=1 only changes how gsurf links its own binaries (gsurfctl). It
# is passed every time because gsurf rebuilds when its flags change.
GSURF_FLAGS := GTK_BACKEND=gtk4 BUILD_TESTS=0 BUILD_MODULES=0 STATIC=1

# gsurf's submodules use SSH URLs; fetch them over HTTPS instead.
GIT_HTTPS := -c url.https://gitlab.com/.insteadOf=git@gitlab.com: \
             -c url.https://git.podbielniak.com/.insteadOf=git@git-ssh.podbielniak.com:

PKGS := webkitgtk-6.0 gtk4 javascriptcoregtk-6.0 libsoup-3.0 json-glib-1.0 \
        yaml-0.1 gmodule-2.0 sqlite3
# Needed by gsurf's build (crispy), not linked here.
BUILD_PKGS := $(PKGS) readline

FEDORA_PACKAGES := gcc make git pkgconf-pkg-config gtk4-devel webkitgtk6.0-devel \
                   libsoup3-devel json-glib-devel libyaml-devel sqlite-devel readline-devel

NO_DEPS_GOALS := clean distclean deps dist help uninstall
ifneq ($(filter-out $(NO_DEPS_GOALS),$(or $(MAKECMDGOALS),all)),)
MISSING := $(shell for p in $(BUILD_PKGS); do $(PKG_CONFIG) --exists $$p || echo $$p; done)
ifneq ($(strip $(MISSING)),)
$(error Missing development packages: $(MISSING). On Fedora: sudo dnf install $(FEDORA_PACKAGES))
endif
endif

CFLAGS   ?= -O2 -g
CFLAGS   += -std=gnu11 -Wall -Wextra -Wno-unused-parameter
CPPFLAGS += -DGSURF_BACKEND_GTK4=1 -DGHD_VERSION=\"$(VERSION)\" \
            -I$(GSURF_DIR)/src -I$(GSURF_OUT) \
            $(shell $(PKG_CONFIG) --cflags $(PKGS) 2>/dev/null)
LDLIBS   += $(GSURF_LIB) $(shell $(PKG_CONFIG) --libs $(PKGS) 2>/dev/null) -lm

SRCS := src/main.c src/firefox-import.c src/window-state.c src/log.c
HDRS := $(wildcard src/*.h)
OBJS := $(SRCS:src/%.c=build/obj/%.o)
BIN  := build/google-home

.PHONY: all run install uninstall gsurfctl deps dist bindist clean distclean help FORCE

all: $(BIN)

$(BIN): $(OBJS) $(GSURF_LIB)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

# The gsurf build is incremental, so ask it every time; the binary
# relinks only when the archive changes. Objects wait for it because
# gsurf generates one of the headers they include.
build/obj/%.o: src/%.c $(HDRS) | $(GSURF_LIB) build/obj
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(GSURF_LIB): FORCE | $(GSURF_DIR)/deps/crispy/Makefile
	$(MAKE) -C $(GSURF_DIR) $(GSURF_FLAGS) lib

gsurfctl: | $(GSURF_DIR)/deps/crispy/Makefile
	$(MAKE) -C $(GSURF_DIR) $(GSURF_FLAGS) gsurfctl
	@echo "gsurfctl: $(GSURF_OUT)/gsurfctl"

build/obj:
	mkdir -p $@

$(GSURF_DIR)/deps/crispy/Makefile:
	$(MAKE) deps

deps:
	git submodule update --init $(GSURF_DIR)
	git $(GIT_HTTPS) -C $(GSURF_DIR) submodule update --init deps/yaml-glib deps/crispy

run: $(BIN)
	./$(BIN)

# The launcher is generated at install time: its Exec= depends on BINDIR.
install: $(BIN)
	$(INSTALL) -Dm755 $(BIN) $(DESTDIR)$(BINDIR)/google-home
	$(INSTALL) -d $(DESTDIR)$(DATADIR)/applications
	sed 's|@BINDIR@|$(BINDIR)|g' data/google-home-desktop.desktop.in \
		> $(DESTDIR)$(DATADIR)/applications/google-home-desktop.desktop
	chmod 644 $(DESTDIR)$(DATADIR)/applications/google-home-desktop.desktop
	$(INSTALL) -Dm644 data/google-home-desktop.svg \
		$(DESTDIR)$(DATADIR)/icons/hicolor/scalable/apps/google-home-desktop.svg
	$(INSTALL) -Dm644 data/google-home-desktop.png \
		$(DESTDIR)$(DATADIR)/icons/hicolor/512x512/apps/google-home-desktop.png
	@if [ -z "$(DESTDIR)" ]; then \
		update-desktop-database "$(DATADIR)/applications" 2>/dev/null || true; \
		gtk-update-icon-cache -qt "$(DATADIR)/icons/hicolor" 2>/dev/null || true; \
	fi

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/google-home \
		$(DESTDIR)$(DATADIR)/applications/google-home-desktop.desktop \
		$(DESTDIR)$(DATADIR)/icons/hicolor/scalable/apps/google-home-desktop.svg \
		$(DESTDIR)$(DATADIR)/icons/hicolor/512x512/apps/google-home-desktop.png

# GitHub's release archives leave out submodules, so a release carries
# this one: HEAD plus the pinned gsurf and the two dependencies it builds
# from. It builds with plain `make', no git needed.
DIST := google-home-desktop-$(VERSION)
GSURF_DEPS := deps/yaml-glib deps/crispy

dist:
	rm -rf build/dist && mkdir -p build/dist
	git archive --prefix=$(DIST)/ HEAD | tar -x -C build/dist
	git -C $(GSURF_DIR) archive --prefix=$(DIST)/$(GSURF_DIR)/ HEAD | tar -x -C build/dist
	for d in $(GSURF_DEPS); do \
		git -C $(GSURF_DIR)/$$d archive --prefix=$(DIST)/$(GSURF_DIR)/$$d/ HEAD | \
			tar -x -C build/dist || exit 1; \
	done
	tar -C build/dist --owner=0 --group=0 --numeric-owner -czf build/$(DIST).tar.gz $(DIST)
	@echo "build/$(DIST).tar.gz"

# The installed files under /usr/local, without directory entries, so
# `sudo tar -xzf FILE -C /' never touches existing directories.
BINDIST := google-home-$(VERSION)-x86_64-linux

bindist: $(BIN)
	rm -rf build/bindist
	$(MAKE) install DESTDIR=$(CURDIR)/build/bindist PREFIX=/usr/local
	strip build/bindist/usr/local/bin/google-home
	cd build/bindist && find usr -type f | sort | \
		tar --owner=0 --group=0 --numeric-owner -czf ../$(BINDIST).tar.gz -T -
	@echo "build/$(BINDIST).tar.gz"

clean:
	rm -rf build

# Also clean the vendored gsurf build.
distclean: clean
	-$(MAKE) -C $(GSURF_DIR) clean

help:
	@sed -n 's/^#   //p' Makefile
