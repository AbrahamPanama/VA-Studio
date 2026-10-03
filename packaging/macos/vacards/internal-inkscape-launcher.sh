#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu
macos_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
resources_dir=$(CDPATH= cd -- "$macos_dir/../Resources" && pwd)
export INKSCAPE_PROFILE_DIR="${INKSCAPE_PROFILE_DIR:-$HOME/Library/Application Support/Inkscape by VACards Test}"
export INKSCAPE_APP_ID_TAG=vacards_test
mkdir -p "$INKSCAPE_PROFILE_DIR"
export GDK_PIXBUF_MODULEDIR="$resources_dir/lib/gdk-pixbuf-2.0/2.10.0/loaders"
export GDK_PIXBUF_MODULE_FILE="$INKSCAPE_PROFILE_DIR/gdk-pixbuf-loaders.cache"
export GIO_MODULE_DIR="$resources_dir/lib/gio/modules"
export GSETTINGS_SCHEMA_DIR="$resources_dir/share/glib-2.0/schemas"
export XDG_DATA_DIRS="$resources_dir/share"
export FONTCONFIG_FILE="$resources_dir/etc/fonts/fonts.conf"
export FONTCONFIG_PATH="$resources_dir/etc/fonts"
export PATH="$macos_dir:/usr/bin:/bin:/usr/sbin:/sbin"
# Regenerate the cache from every bundled GdkPixbuf module. TIFF is module-only
# (PNG/JPEG are built into libgdk_pixbuf); omitting it removes the .tif/.tiff
# File > Open input extensions and reproduces BUG-003.
"$macos_dir/gdk-pixbuf-query-loaders" \
    "$GDK_PIXBUF_MODULEDIR/libpixbufloader_svg.so" \
    "$GDK_PIXBUF_MODULEDIR/libpixbufloader-tiff.so" > "$GDK_PIXBUF_MODULE_FILE"
grep -Fq 'libpixbufloader_svg.so' "$GDK_PIXBUF_MODULE_FILE" ||
    { echo "bundled GdkPixbuf SVG loader was not registered" >&2; exit 1; }
grep -Fq 'image/tiff' "$GDK_PIXBUF_MODULE_FILE" ||
    { echo "bundled GdkPixbuf TIFF loader was not registered" >&2; exit 1; }
exec "$macos_dir/inkscape-bin" "$@"
