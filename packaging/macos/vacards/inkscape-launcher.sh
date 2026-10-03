#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later

set -eu

macos_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
resources_dir=$(CDPATH= cd -- "$macos_dir/../Resources" && pwd)
profile_dir=${INKSCAPE_PROFILE_DIR:-${TMPDIR:-/tmp}/inkscape-vacards-profile}
loader_cache=$profile_dir/gdk-pixbuf-loaders.cache

mkdir -p "$profile_dir"
sed "s#@APP_RESOURCES@#$resources_dir#g" \
    "$resources_dir/gdk-pixbuf-loaders.cache.in" > "$loader_cache"

export GDK_PIXBUF_MODULE_FILE="$loader_cache"
export GDK_PIXBUF_MODULEDIR="$resources_dir/lib/gdk-pixbuf-2.0/2.10.0/loaders"
export GIO_MODULE_DIR="$resources_dir/lib/gio/modules"
export GSETTINGS_SCHEMA_DIR="$resources_dir/share/glib-2.0/schemas"

exec "$macos_dir/inkscape-bin" "$@"
