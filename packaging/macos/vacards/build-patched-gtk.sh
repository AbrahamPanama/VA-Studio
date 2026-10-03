#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Development dependency builder; not a release attestation or installation.
set -eu
if [ "$#" -ne 3 ]; then
    echo "usage: sh $0 GTK_4_22_4_ARCHIVE NEW_OUTPUT_DIRECTORY PATCHED_CAIRO_PREFIX" >&2
    exit 2
fi
archive=$1
output=$2
cairo=$3
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
expected=51bd9f60c7d23a665a556c7364c21fb2e4e282566b3e7e092455e8f910330893
[ -z "${DESTDIR-}" ] || { echo "Unset DESTDIR for this isolated build" >&2; exit 1; }
for tool in shasum tar patch meson pkg-config; do
    command -v "$tool" >/dev/null || exit 1
done
[ "$(uname -s)" = Darwin ] || { echo "macOS builder only" >&2; exit 1; }
[ "$(shasum -a 256 "$archive" | awk '{print $1}')" = "$expected" ] || {
    echo "GTK 4.22.4 archive checksum mismatch" >&2; exit 1;
}
[ ! -e "$output" ] || { echo "Output must be new: $output" >&2; exit 1; }
case "$output:$cairo" in /*:/*) ;; *) echo "Use absolute output and Cairo paths" >&2; exit 1;; esac
sh "$script_dir/verify-vacards-cairo-prefix.sh" "$cairo"
cairo=$(CDPATH= cd -- "$cairo" && pwd -P)
mkdir "$output"
output=$(CDPATH= cd -- "$output" && pwd)
source_dir=$output/gtk-4.22.4
prefix=$output/install
tar -xf "$archive" -C "$output"
for name in gtk-4.22.4-macos-unmapped-freeze.patch gtk-4.22.4-icon-cache-dispose.patch \
    gtk-4.22.4-macos-deferred-file-menu.patch gtk-4.22.4-recent-groups.patch; do
    cp "$script_dir/$name" "$output/$name"
    patch --batch --forward -d "$source_dir" -p1 < "$output/$name"
done
export PKG_CONFIG_PATH="$cairo/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export DYLD_LIBRARY_PATH="$cairo/lib${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}"
resolved_cairo=$(pkg-config --variable=prefix cairo)
[ "$(CDPATH= cd -- "$resolved_cairo" && pwd -P)" = "$cairo" ] || {
    echo "pkg-config did not resolve the requested Cairo prefix" >&2; exit 1;
}
# Same version and release-mode options as the tested isolated candidate.
meson setup "$output/build" "$source_dir" --prefix "$prefix" --libdir lib \
    --wrap-mode=nofallback --buildtype=release \
    -Dx11-backend=false -Dmacos-backend=true -Dvulkan=disabled \
    -Dmedia-gstreamer=disabled -Dintrospection=disabled -Dman-pages=false \
    -Ddocumentation=false -Dbuild-demos=false -Dbuild-examples=false \
    -Dbuild-tests=false -Dbuild-testsuite=false
meson compile -C "$output/build" -j 2
meson install -C "$output/build" --no-rebuild
cp "$output/build/meson-info/intro-buildoptions.json" "$output/build-options.json"
cp "$output/build/meson-info/intro-dependencies.json" "$output/resolved-dependencies.json"
shasum -a 256 "$archive" "$output/gtk-4.22.4-macos-unmapped-freeze.patch" \
    "$output/gtk-4.22.4-icon-cache-dispose.patch" \
    "$output/gtk-4.22.4-macos-deferred-file-menu.patch" \
    "$output/gtk-4.22.4-recent-groups.patch" "$prefix/lib/libgtk-4.1.dylib"
echo "Development GTK prefix: $prefix (not installed or release-qualified)"
