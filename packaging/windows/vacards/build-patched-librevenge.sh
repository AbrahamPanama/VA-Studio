#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Isolated, pinned UCRT64 build of the VACards patched librevenge 0.0.6 prefix.
# It never installs packages and never writes into /ucrt64. Retain OUTPUT
# (including failed attempts) as evidence.
set -euo pipefail
export LC_ALL=C

fail() { echo "VACards Windows librevenge build failed: $*" >&2; exit 1; }
[[ $# == 1 || ( $# == 3 && $2 == --archive ) ]] || {
    echo "usage: $0 EMPTY_OUTPUT_DIRECTORY [--archive PINNED_TARBALL]" >&2
    exit 2
}
output_arg=$1
archive_input=${3:-}
[[ ${MSYSTEM:-} == UCRT64 && $(uname -m) == x86_64 ]] || fail "requires an x86_64 MSYS2 UCRT64 shell"
case $(uname -s) in MINGW*_NT-*) ;; *) fail "requires a native Windows UCRT64 host" ;; esac
for tool in cygpath bash shasum tar patch make gcc g++ awk find sort grep cmp; do
    command -v "$tool" >/dev/null || fail "missing tool: $tool"
done

sha256() { shasum -a 256 "$1" | awk '{print $1}'; }
normalize_dir() { (cd -- "$(cygpath -u "$1")" && pwd -P); }
script_dir=$(cd -- "$(dirname -- "$0")" && pwd -P)
source_root=$(cd -- "$script_dir/../../.." && pwd -P)
bundle_dir=$source_root/packaging/dependencies/librevenge-0.0.6
bundle_manifest=$bundle_dir/VACARDS-LIBREVENGE-BUNDLE.env
bundle_patch=$bundle_dir/vacards-librevenge-0.0.6.patch
[[ -f $bundle_manifest && -f $bundle_patch ]] || fail "tracked bundle is incomplete"

read_bundle() {
    awk -F= -v key="$1" '$1 == key {sub(/^[^=]*=/, ""); print; exit}' "$bundle_manifest"
}
release=$(read_bundle release)
archive_sha=$(read_bundle archive_sha256)
patch_sha=$(read_bundle patch_sha256)
expected_marker=$(read_bundle capability_binary_marker)
expected_generator=$(read_bundle final_generator_sha256)
jobs=${VACARDS_DEPENDENCY_JOBS:-2}
case $jobs in
    1|2) ;;
    *) fail "VACARDS_DEPENDENCY_JOBS must be 1 or 2 (the VACards local build cap)" ;;
esac
[[ $(sha256 "$bundle_patch") == "$patch_sha" ]] || fail "tracked bundle patch checksum mismatch"

if [[ -z $archive_input ]]; then
    fail "a librevenge $release tarball is required via --archive; the archive is not fetched"
fi
archive_input=$(cygpath -u "$archive_input")
[[ -f $archive_input ]] || fail "missing supplied archive: $archive_input"

ucrt_prefix=$(normalize_dir /ucrt64)
for compiler in "$(command -v gcc)" "$(command -v g++)"; do
    [[ $(normalize_dir "$(dirname -- "$compiler")") == "$ucrt_prefix/bin" ]] ||
        fail "compiler is outside /ucrt64/bin: $compiler"
    [[ $("$compiler" -dumpmachine) == x86_64-w64-mingw32 ]] || fail "wrong compiler target: $compiler"
done
[[ -z ${DESTDIR:-} ]] || fail "DESTDIR would redirect installation"

output=$(cygpath -u "$output_arg")
[[ -n $output && $output == /* ]] || fail "use an absolute output directory"
if [[ -e $output ]]; then
    [[ -d $output && ! -L $output ]] || fail "output is not a regular directory"
    [[ -z $(find "$output" -mindepth 1 -maxdepth 1 -print -quit) ]] ||
        fail "output must be empty; use the verifier for existing builds"
fi
parent=$(normalize_dir "$(dirname -- "$output")") || fail "output parent must exist"
output=$parent/$(basename -- "$output")
case $output in "$ucrt_prefix"|"$ucrt_prefix"/*|/usr/*|/bin/*|/lib/*|/etc/*)
    fail "refusing a system dependency directory" ;; esac
mkdir -p -- "$output"
output=$(normalize_dir "$output")
prefix=$output/install
build_dir=$output/build
source_dir=$output/librevenge-$release
mkdir -p -- "$source_dir"
cp -- "$archive_input" "$output/librevenge-$release.tar.xz"
archive=$output/librevenge-$release.tar.xz
[[ $(sha256 "$archive") == "$archive_sha" ]] || fail "source archive checksum mismatch"
tar -xf "$archive" -C "$output"

( cd "$source_dir" && patch --batch --forward --fuzz=0 -p1 < "$bundle_patch" ) \
    > "$output/patch.log" 2>&1 || fail "bundle patch did not apply cleanly"
[[ $(sha256 "$source_dir/src/lib/RVNGSVGDrawingGenerator.cpp") == "$expected_generator" ]] ||
    fail "patched generator does not match the recorded final hash"

mkdir -p -- "$build_dir"
( cd "$build_dir" && CPPFLAGS="${CPPFLAGS:-}" LDFLAGS="${LDFLAGS:-}" \
    "$source_dir/configure" --prefix="$(cygpath -am "$prefix")" \
    --enable-tests --disable-werror --disable-docs )
make -C "$build_dir" -j "$jobs"
# Serial check: the LibreOffice-derived fixtures are not parallel-safe.
make -C "$build_dir" check

built_dll=$(find "$build_dir/src/lib/.libs" -maxdepth 1 -type f -name 'librevenge-0.[0-9]*.dll' -print | head -n 1)
[[ -n $built_dll ]] || fail "built librevenge DLL was not found"
grep -a -q "$expected_marker" "$built_dll" || fail "built DLL is missing capability marker '$expected_marker'"
make -C "$build_dir" install

library=$(find "$prefix/bin" -maxdepth 1 -type f -name 'librevenge-0.[0-9]*.dll' -print | head -n 1)
[[ -n $library ]] || fail "installed librevenge DLL was not found"
grep -a -q "$expected_marker" "$library" || fail "installed DLL is missing capability marker '$expected_marker'"
library_relative=${library#"$prefix"/}
library_sha256=$(sha256 "$library")
created_utc=$(date -u '+%Y-%m-%dT%H:%M:%SZ')

{
    printf 'format=1\n'
    printf 'dependency=librevenge\n'
    printf 'release=%s\n' "$release"
    printf 'source_identity=%s\n' "$(read_bundle source_identity)"
    printf 'archive_filename=%s\n' "$(read_bundle archive_filename)"
    printf 'archive_sha256=%s\n' "$archive_sha"
    printf 'patch_filename=%s\n' "$(read_bundle patch_filename)"
    printf 'patch_sha256=%s\n' "$patch_sha"
    printf 'capability_name=%s\n' "$(read_bundle capability_name)"
    printf 'capability_value=%s\n' "$(read_bundle capability_value)"
    printf 'capability_binary_marker=%s\n' "$expected_marker"
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

bash "$script_dir/verify-vacards-librevenge.sh" "$prefix"
echo "Patched VACards librevenge installed in $prefix"
