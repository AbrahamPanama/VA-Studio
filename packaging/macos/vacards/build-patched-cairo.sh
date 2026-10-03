#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later

set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: $0 OUTPUT_DIRECTORY" >&2
    exit 2
fi

output_dir=$1
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
dependency_tool=$script_dir/vacards-dependencies.sh
cairo_version=$("$dependency_tool" --get cairo_release)
release=cairo-$cairo_version
archive=$output_dir/$release.tar.xz
source_dir=$output_dir/$release
build_dir=$output_dir/build
prefix=$output_dir/install
expected_sha256=$("$dependency_tool" --get cairo_release_sha256)
fix_commit=$("$dependency_tool" --get cairo_fix_commit)
deployment_target=$("$dependency_tool" --get macos_deployment_target)
patch_file=$script_dir/cairo-1.18.4-clip-all.patch
jobs=${VACARDS_DEPENDENCY_JOBS-2}
case "$jobs" in
    ''|*[!0-9]*|0|0*)
        echo "VACARDS_DEPENDENCY_JOBS must be a positive integer without leading zeros" >&2
        exit 1 ;;
esac

for tool in curl shasum tar patch meson; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "required tool not found: $tool" >&2
        exit 1
    fi
done

if [ -e "$source_dir" ] || [ -e "$build_dir" ] || [ -e "$prefix" ]; then
    echo "refusing to overwrite an existing Cairo build in $output_dir" >&2
    exit 1
fi

mkdir -p "$output_dir"
curl --fail --location --output "$archive" \
    "https://cairographics.org/releases/$release.tar.xz"

actual_sha256=$(shasum -a 256 "$archive" | awk '{print $1}')
if [ "$actual_sha256" != "$expected_sha256" ]; then
    echo "Cairo archive checksum mismatch" >&2
    exit 1
fi

tar -C "$output_dir" -xf "$archive"
patch_sha256=$(shasum -a 256 "$patch_file" | awk '{print $1}')
patch -d "$source_dir" -p1 < "$patch_file"

MACOSX_DEPLOYMENT_TARGET="$deployment_target" meson setup "$build_dir" "$source_dir" \
    --prefix "$prefix" \
    --libdir lib \
    --buildtype release \
    -Dtests=disabled \
    -Dlzo=disabled \
    -Dxlib=enabled \
    -Dxcb=enabled \
    -Dquartz=enabled \
    -Dfontconfig=enabled \
    -Dfreetype=enabled \
    -Dglib=enabled \
    -Dpng=enabled
meson compile -C "$build_dir" -j "$jobs"
meson install -C "$build_dir"

library=$prefix/lib/libcairo.2.dylib
[ -f "$library" ] || {
    echo "patched Cairo library was not installed: $library" >&2
    exit 1
}
library_sha256=$(shasum -a 256 "$library" | awk '{print $1}')
gobject_library_sha256=$(shasum -a 256 "$prefix/lib/libcairo-gobject.2.dylib" | awk '{print $1}')
[ "$(shasum -a 256 "$patch_file" | awk '{print $1}')" = "$patch_sha256" ] || {
    echo "Cairo patch changed during the build; use a fresh build directory" >&2
    exit 1
}
{
    printf 'Cairo release: %s\n' "$cairo_version"
    printf 'Release SHA-256: %s\n' "$expected_sha256"
    printf 'Upstream fix: %s\n' "$fix_commit"
    printf 'Patch: packaging/macos/vacards/cairo-1.18.4-clip-all.patch\n'
    printf 'Patch SHA-256: %s\n' "$patch_sha256"
    printf 'Library SHA-256: %s\n' "$library_sha256"
    printf 'GObject Library SHA-256: %s\n' "$gobject_library_sha256"
    printf 'macOS deployment target: %s\n' "$deployment_target"
} > "$prefix/VACARDS-CAIRO.txt"

"$script_dir/verify-vacards-cairo-prefix.sh" "$prefix"

echo "Patched Cairo installed in $prefix"
