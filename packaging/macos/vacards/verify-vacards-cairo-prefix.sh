#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later

set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: $0 CAIRO_INSTALL_PREFIX" >&2
    exit 2
fi

prefix=$(CDPATH= cd -- "$1" && pwd -P)
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
dependency_tool=$script_dir/vacards-dependencies.sh
marker=$prefix/VACARDS-CAIRO.txt
library=$prefix/lib/libcairo.2.dylib

fail()
{
    echo "VACards Cairo verification failed: $*" >&2
    exit 1
}

[ -f "$marker" ] || fail "missing $marker"
[ -f "$library" ] || fail "missing $library"

expected_release=$("$dependency_tool" --get cairo_release)
expected_archive_sha=$("$dependency_tool" --get cairo_release_sha256)
expected_fix=$("$dependency_tool" --get cairo_fix_commit)
expected_deployment_target=$("$dependency_tool" --get macos_deployment_target)
grep -Fxq "Cairo release: $expected_release" "$marker" || fail "wrong Cairo release"
grep -Fxq "Release SHA-256: $expected_archive_sha" "$marker" || fail "wrong source archive"
grep -Fxq "Upstream fix: $expected_fix" "$marker" || fail "wrong upstream fix"
grep -Fxq "macOS deployment target: $expected_deployment_target" "$marker" ||
    fail "wrong macOS deployment target"
expected_library_sha=$(sed -n 's/^Library SHA-256: //p' "$marker")
printf '%s\n' "$expected_library_sha" | grep -Eq '^[0-9a-f]{64}$' ||
    fail "missing or invalid library checksum"
actual_library_sha=$(shasum -a 256 "$library" | awk '{print $1}')
[ "$actual_library_sha" = "$expected_library_sha" ] || fail "Cairo library checksum changed"
# Keep the original standalone prefix interface/fixtures usable. Full-gate
# input capture requires these additional fields and explains how to migrate
# old prefixes; an extended marker may never partially omit them.
if grep -Eq '^(Patch|GObject Library) SHA-256:' "$marker"; then
    patch_sha=$(sed -n 's/^Patch SHA-256: //p' "$marker")
    gobject_sha=$(sed -n 's/^GObject Library SHA-256: //p' "$marker")
    for value in "$patch_sha" "$gobject_sha"; do
        [ "${#value}" -eq 64 ] && printf '%s\n' "$value" | grep -Eq '^[0-9a-f]{64}$' ||
            fail "incomplete extended Cairo provenance; rebuild with build-patched-cairo.sh"
    done
    [ "$patch_sha" = "$(shasum -a 256 "$script_dir/cairo-1.18.4-clip-all.patch" | awk '{print $1}')" ] ||
        fail "Cairo patch bytes changed; rebuild with build-patched-cairo.sh"
    [ -f "$prefix/lib/libcairo-gobject.2.dylib" ] || fail "Cairo GObject library is missing"
    [ "$gobject_sha" = "$(shasum -a 256 "$prefix/lib/libcairo-gobject.2.dylib" | awk '{print $1}')" ] ||
        fail "Cairo GObject library checksum changed"
fi
if [ "$(uname -s)" = "Darwin" ]; then
    actual_minos=$(vtool -show-build "$library" | awk '$1 == "minos" {print $2; exit}')
    [ "$actual_minos" = "$expected_deployment_target" ] ||
        fail "library minimum macOS is '$actual_minos', expected $expected_deployment_target"
fi

pkg_path=$prefix/lib/pkgconfig
actual_prefix=$(PKG_CONFIG_PATH="$pkg_path" \
    pkg-config --variable=prefix cairo 2>/dev/null || true)
# Native pkg-config and MSYS sh may spell the same directory differently.
[ -n "$actual_prefix" ] && actual_prefix=$(CDPATH= cd -- "$actual_prefix" && pwd -P) ||
    fail "pkg-config returned an unavailable Cairo prefix"
[ "$actual_prefix" = "$prefix" ] || fail "pkg-config resolves Cairo outside this prefix: $actual_prefix"

echo "Verified patched VACards Cairo $expected_release ($expected_fix)"
