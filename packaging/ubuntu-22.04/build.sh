#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Runs inside the Ubuntu 22.04 container (see `make portable`): builds
# the release binary tarball and .deb from a copy of /repo and writes
# them to /out. The copy gets the GTK 4.6 / GLib 2.72 patch for gsurf;
# the checkout itself is never changed.
set -eu

mkdir /src
tar -C /repo \
	--exclude=./build --exclude=./node_modules --exclude=./dist \
	--exclude=./vendor/gsurf/build --exclude=./vendor/gsurf/deps/crispy/build \
	-cf - . | tar -C /src -xf -
cd /src
patch -s -d vendor/gsurf -p1 < packaging/ubuntu-22.04/gsurf-gtk4.6-glib2.72.patch
make -j"$(nproc)"
# One after the other: each runs its own install sub-make.
make bindist
make deb
cp build/google-home-*-x86_64-linux.tar.gz build/google-home_*_amd64.deb /out/
