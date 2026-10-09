#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Build pinned Pango 1.58.2 for the Windows UCRT64 runtime with the VACards
# Win32 NULL-face guard. Only the official archive is downloaded; all configure,
# compile and install steps use the verified local inputs with Meson fallback
# disabled.
set -euo pipefail
export LC_ALL=C GIT_TERMINAL_PROMPT=0

fail() { echo "VACards Windows patched-Pango build failed: $*" >&2; exit 1; }
version=1.58.2
archive=pango-${version}.tar.xz
archive_sha=342385b6ca3b7c73455d7c80a13b7dbe4489e00bc3bd4c5bd6ed4dce421e374a
archive_url="https://download.gnome.org/sources/pango/1.58/${archive}"
patch_name=pango-1.58.2-win32-null-face.patch
patch_sha_expected=1b8f30ef477db49d98ea036b141030819891a12aea448bb664ccb0fd3de6d4fe

usage() { echo "usage: $0 --output EMPTY_DIR [--download] [--jobs 1|2]" >&2; }
output_arg= download=0 jobs=2
while (($#)); do
    case $1 in
        --output) output_arg=${2:-}; shift 2 ;;
        --download) download=1; shift ;;
        --jobs) jobs=${2:-}; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) usage; fail "unknown argument: $1" ;;
    esac
done
[[ -n $output_arg ]] || { usage; fail "--output is required"; }
[[ $jobs == 1 || $jobs == 2 ]] || fail "--jobs must be 1 or 2"
[[ ${MSYSTEM:-} == UCRT64 && $(uname -m) == x86_64 ]] || fail "requires x86_64 MSYS2 UCRT64"
case $(uname -s) in MINGW*_NT-*) ;; *) fail "requires a native Windows UCRT64 host" ;; esac
for tool in bash cygpath tar patch meson ninja gcc pkg-config sha256sum awk find sort cmp; do
    command -v "$tool" >/dev/null || fail "missing tool: $tool"
done

script_dir=$(cd -- "$(dirname -- "$0")" && pwd -P)
output=$(cygpath -u "$output_arg") || fail "invalid output path"
[[ $output == /* ]] || fail "--output must be absolute"
[[ ! -e $output ]] || fail "output must be a new, empty path: $output_arg"
mkdir -p -- "$output"
output=$(cd -- "$output" && pwd -P)
prefix="$output/install"
prefix_windows="$(cygpath -m "$prefix")"
mkdir -p "$output/downloads"
tarball="$output/downloads/$archive"
patch_file="$script_dir/$patch_name"
[[ -f $patch_file ]] || fail "missing tracked patch: $patch_file"

sha256() { sha256sum "$1" | awk '{print $1}'; }
if [[ ! -f $tarball ]]; then
    [[ $download == 1 ]] || fail "missing pinned archive; rerun with --download"
    curl --fail --location --proto '=https' --tlsv1.2 --output "$tarball.part" "$archive_url"
    mv -- "$tarball.part" "$tarball"
fi
actual_archive_sha=$(sha256 "$tarball")
[[ $actual_archive_sha == "$archive_sha" ]] || fail "archive SHA-256 mismatch: $actual_archive_sha"
patch_sha=$(sha256 "$patch_file")
[[ $patch_sha == "$patch_sha_expected" ]] || fail "patch SHA-256 mismatch: $patch_sha"

# No network operation occurs after the pinned source archive is verified.
mkdir "$output/src"
tar -xf "$tarball" -C "$output/src"
source_dir="$output/src/pango-$version"
[[ -d $source_dir ]] || fail "unexpected archive layout"
patch -d "$source_dir" -p1 --forward < "$patch_file" | tee "$output/patch.log"
mkdir "$output/build"
meson setup "$output/build" "$source_dir" \
    "--prefix=$prefix_windows" \
    --default-library=shared \
    --buildtype=plain \
    --auto-features=enabled \
    -Ddocumentation=false \
    -Dman-pages=false \
    -Dintrospection=disabled \
    -Dxft=disabled \
    --wrap-mode=nofallback 2>&1 | tee "$output/configure.log"
meson compile -C "$output/build" -j "$jobs" 2>&1 | tee "$output/build.log"
meson install -C "$output/build" --no-rebuild 2>&1 | tee "$output/install.log"

mkdir -p "$prefix/abi"
for dll in libpango-1.0-0.dll libpangocairo-1.0-0.dll libpangoft2-1.0-0.dll libpangowin32-1.0-0.dll; do
    [[ -f $prefix/bin/$dll ]] || fail "installed DLL missing: $dll"
done
(cd "$prefix" && sha256sum bin/libpango*.dll | sed -E 's# \*?bin/#  bin/#' | sort -k2,2) > "$prefix/abi/pango-dlls.sha256"
expected_inventory="$script_dir/../../dependencies/pango-1.58.2-win32-null-face/pango-dlls.sha256"
cmp -s "$expected_inventory" "$prefix/abi/pango-dlls.sha256" || {
    diff -u "$expected_inventory" "$prefix/abi/pango-dlls.sha256" >&2 || true
    fail "rebuilt Pango DLL hashes differ from the verified bundle pin"
}
{
    printf 'format=1\ndependency=pango\npango_version=%s\nplatform=windows-ucrt64\narchitecture=x86_64\n' "$version"
    printf 'source_archive=%s\nsource_archive_sha256=%s\npatch_filename=%s\npatch_sha256=%s\n' "$archive" "$archive_sha" "$patch_name" "$patch_sha"
    printf 'prefix_posix=%s\nprefix_windows=%s\n' "$prefix" "$prefix_windows"
    printf 'meson_options=--default-library=shared --buildtype=plain --auto-features=enabled -Ddocumentation=false -Dman-pages=false -Dintrospection=disabled -Dxft=disabled --wrap-mode=nofallback\n'
    printf 'jobs=%s\n' "$jobs"
} > "$prefix/VACARDS-PANGO.env"
cp "$prefix/abi/pango-dlls.sha256" "$output/pango-dlls.sha256"
cp "$prefix/VACARDS-PANGO.env" "$output/VACARDS-PANGO.env"
echo "Patched Pango $version installed in $prefix_windows"
cat "$prefix/abi/pango-dlls.sha256"
