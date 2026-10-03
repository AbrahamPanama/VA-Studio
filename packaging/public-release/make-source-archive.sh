#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Build the complete corresponding source archive of one VA Studio release.
#
# usage: make-source-archive.sh (--public-repo DIR | --dev-repo DIR) --commit REV \
#            --output FILE.tar.xz --downloads DIR [--fetch] \
#            --icc FILE|none --hyphenation DIR|none
#
# The archive holds the release commit's source tree (flattened modules, the
# libcdr fork, the vendored Rust crates, the packaging scripts and patches), the
# upstream Cairo, GTK and librevenge release archives and MSYS2 GTK recipe files
# verified against their pinned SHA-256, the external inputs given as
# parameters, SOURCE-ARCHIVE.json and SHA256SUMS. Every step fails closed; see
# make_source_archive.py for the details. Nothing is downloaded without --fetch.
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec python3 -B "$here/make_source_archive.py" "$@"
