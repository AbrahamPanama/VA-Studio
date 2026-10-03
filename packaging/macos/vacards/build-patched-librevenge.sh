#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later

set -eu

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
    echo "usage: $0 OUTPUT_DIRECTORY [LIBREVENGE-0.0.6-TARBALL]" >&2
    exit 2
fi

output_dir=$1
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
source_root=$(CDPATH= cd -- "$script_dir/../../.." && pwd)
bundle_dir=$source_root/packaging/dependencies/librevenge-0.0.6
bundle_manifest=$bundle_dir/VACARDS-LIBREVENGE-BUNDLE.env
patch_file=$bundle_dir/vacards-librevenge-0.0.6.patch
jobs=${VACARDS_DEPENDENCY_JOBS:-2}

archive=${2:-${VACARDS_LIBREVENGE_ARCHIVE:-}}
if [ -z "$archive" ]; then
    echo "A librevenge 0.0.6 tarball is required: pass it as the second argument or" >&2
    echo "set VACARDS_LIBREVENGE_ARCHIVE. The archive is not fetched automatically." >&2
    exit 2
fi
case "$archive" in
    /*) ;;
    *) archive=$(CDPATH= cd -- "$(dirname -- "$archive")" && pwd -P)/$(basename -- "$archive") ;;
esac

fail()
{
    echo "VACards librevenge build failed: $*" >&2
    exit 1
}

read_bundle()
{
    key=$1
    awk -F= -v key="$key" '$1 == key {sub(/^[^=]*=/, ""); print; exit}' "$bundle_manifest"
}

for tool in make shasum tar patch; do
    command -v "$tool" >/dev/null 2>&1 || fail "required tool not found: $tool"
done
case "$jobs" in
    1|2) ;;
    *) fail "VACARDS_DEPENDENCY_JOBS must be 1 or 2 (the VACards local build cap)" ;;
esac

[ -f "$bundle_manifest" ] || fail "missing bundle manifest $bundle_manifest"
[ -f "$patch_file" ] || fail "missing bundle patch $patch_file"
[ -f "$archive" ] || fail "librevenge archive not found: $archive"

expected_archive_sha=$(read_bundle archive_sha256)
expected_patch_sha=$(read_bundle patch_sha256)
expected_release=$(read_bundle release)
archive_filename=$(read_bundle archive_filename)

actual_archive_sha=$(shasum -a 256 "$archive" | awk '{print $1}')
[ "$actual_archive_sha" = "$expected_archive_sha" ] ||
    fail "archive SHA-256 mismatch: got $actual_archive_sha"
actual_patch_sha=$(shasum -a 256 "$patch_file" | awk '{print $1}')
[ "$actual_patch_sha" = "$expected_patch_sha" ] ||
    fail "bundle patch SHA-256 mismatch: got $actual_patch_sha"

if [ -e "$output_dir" ] && [ -n "$(find "$output_dir" -mindepth 1 -print -quit 2>/dev/null)" ]; then
    fail "refusing to overwrite nonempty output directory: $output_dir"
fi
mkdir -p "$output_dir"

source_dir=$output_dir/source
build_dir=$output_dir/build
prefix=$output_dir/install
mkdir -p "$source_dir"
tar -C "$source_dir" -xf "$archive" || fail "could not extract archive"
source_root_dir=$source_dir/librevenge-$expected_release
[ -d "$source_root_dir" ] || fail "archive did not contain librevenge-$expected_release"

# Confirm the extracted tree is the expected release and not a repack.
actual_release=$(sed -n 's/.*version_micro=\?\([0-9][0-9]*\).*/\1/p' "$source_root_dir/configure.ac" 2>/dev/null || true)
if [ -n "$actual_release" ] && [ "$actual_release" != "${expected_release#0.0.}" ]; then
    fail "extracted source does not declare release $expected_release"
fi

( cd "$source_root_dir" && patch -p1 < "$patch_file" ) > "$output_dir/apply-patch.log" 2>&1 ||
    fail "bundle patch did not apply cleanly"
after_patch_sha=$(shasum -a 256 "$source_root_dir/src/lib/RVNGSVGDrawingGenerator.cpp" | awk '{print $1}')
[ "$after_patch_sha" = "$(read_bundle final_generator_sha256)" ] ||
    fail "patched generator does not match the recorded final hash"

mkdir -p "$build_dir"
# Homebrew's cppunit/boost headers are needed on this host; keep the include and
# library paths local to the build instead of exporting them to the caller.
dependency_cppflags=${CPPFLAGS:-}
dependency_ldflags=${LDFLAGS:-}
if command -v brew >/dev/null 2>&1; then
    boost_prefix=$(brew --prefix boost 2>/dev/null || true)
    if [ -f "$boost_prefix/include/boost/algorithm/string.hpp" ]; then
        dependency_cppflags="-I$boost_prefix/include${dependency_cppflags:+ $dependency_cppflags}"
    fi
    cppunit_prefix=$(brew --prefix cppunit 2>/dev/null || true)
    if [ -d "$cppunit_prefix/lib" ]; then
        dependency_ldflags="-L$cppunit_prefix/lib${dependency_ldflags:+ $dependency_ldflags}"
    fi
fi

( cd "$build_dir" && CPPFLAGS="$dependency_cppflags" LDFLAGS="$dependency_ldflags" \
    "$source_root_dir/configure" \
    --prefix="$prefix" --enable-tests --disable-werror --disable-docs )
make -C "$build_dir" -j "$jobs"
# Serial `make check` is required: the LibreOffice-derived tests write shared
# fixtures and are not parallel-safe.
make -C "$build_dir" check

# The capability must exist in the built (not yet installed) library before it
# is installed, so a stale stock prefix cannot be mistaken for the patched one.
installed_library=$(find "$build_dir/src/lib/.libs" -maxdepth 1 -type f \
    \( -name 'librevenge-0.0*.dylib' -o -name 'librevenge-0.0.so.0*' \) -print | head -n 1)
[ -n "$installed_library" ] || fail "built librevenge shared library was not found"
marker=$(read_bundle capability_binary_marker)
if ! LC_ALL=C grep -a -q "$marker" "$installed_library"; then
    fail "built library is missing capability marker '$marker'"
fi

make -C "$build_dir" install

library=$(find "$prefix/lib" -maxdepth 1 -type f \
    \( -name 'librevenge-0.0*.dylib' -o -name 'librevenge-0.0.so.0*' \) -print | head -n 1)
[ -n "$library" ] || fail "installed shared librevenge library was not found"
if ! LC_ALL=C grep -a -q "$marker" "$library"; then
    fail "installed library is missing capability marker '$marker'"
fi
library_relative=${library#"$prefix"/}
library_sha256=$(shasum -a 256 "$library" | awk '{print $1}')
created_utc=$(date -u '+%Y-%m-%dT%H:%M:%SZ')

{
    printf 'format=1\n'
    printf 'dependency=librevenge\n'
    printf 'release=%s\n' "$expected_release"
    printf 'source_identity=%s\n' "$(read_bundle source_identity)"
    printf 'archive_filename=%s\n' "$archive_filename"
    printf 'archive_sha256=%s\n' "$expected_archive_sha"
    printf 'patch_filename=%s\n' "$(read_bundle patch_filename)"
    printf 'patch_sha256=%s\n' "$expected_patch_sha"
    printf 'capability_name=%s\n' "$(read_bundle capability_name)"
    printf 'capability_value=%s\n' "$(read_bundle capability_value)"
    printf 'capability_binary_marker=%s\n' "$marker"
    printf 'library_relative_path=%s\n' "$library_relative"
    printf 'library_sha256=%s\n' "$library_sha256"
    printf 'test_total=%s\n' "$(read_bundle test_total)"
    printf 'test_crop=%s\n' "$(read_bundle test_crop)"
    printf 'test_text=%s\n' "$(read_bundle test_text)"
    printf 'test_other=%s\n' "$(read_bundle test_other)"
    printf 'make_check_total=%s\n' "$(read_bundle make_check_total)"
    printf 'make_check_pass=%s\n' "$(read_bundle make_check_pass)"
    printf 'make_check_fail=%s\n' "$(read_bundle make_check_fail)"
    printf 'created_utc=%s\n' "$created_utc"
} > "$prefix/VACARDS-LIBREVENGE.env"

"$script_dir/verify-vacards-librevenge.sh" "$prefix"
echo "Patched VACards librevenge installed in $prefix"
