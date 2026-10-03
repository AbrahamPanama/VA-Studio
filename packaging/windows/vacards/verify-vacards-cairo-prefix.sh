#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Read-only UCRT64 prefix verification. --expected-record binds a later check
# to the exact record saved by a successful gate; --runtime-dir checks an
# unsigned staging directory against those same DLL bytes.
set -euo pipefail
export LC_ALL=C

fail() { echo "VACards Windows Cairo verification failed: $*" >&2; exit 1; }
usage() {
    echo "usage: $0 PREFIX [--expected-record FILE] [--runtime-dir DIRECTORY]" >&2
    exit 2
}
[[ $# -ge 1 ]] || usage
prefix_arg=$1; shift
expected_record= runtime_dir=
while [[ $# -gt 0 ]]; do
    [[ $# -ge 2 ]] || usage
    case $1 in
        --expected-record) [[ -z $expected_record ]] || usage; expected_record=$2 ;;
        --runtime-dir) [[ -z $runtime_dir ]] || usage; runtime_dir=$2 ;;
        *) usage ;;
    esac
    shift 2
done
[[ ${MSYSTEM:-} == UCRT64 && $(uname -m) == x86_64 ]] || fail "requires an x86_64 MSYS2 UCRT64 shell"
case $(uname -s) in MINGW*_NT-*) ;; *) fail "requires a native Windows UCRT64 host" ;; esac
for tool in cygpath bash shasum objdump awk find sort cmp grep uniq; do
    command -v "$tool" >/dev/null || fail "missing tool: $tool"
done
pkg_config=${PKG_CONFIG:-pkg-config}
command -v "$pkg_config" >/dev/null || fail "missing pkg-config: $pkg_config"
normalize_dir() {
    # pkgconf may escape spaces in pcfiledir even for --variable output.
    # Decode only that literal escape; never evaluate a pkg-config result.
    local directory=${1//\\ / } normalized
    [[ -n $directory ]] || return 1
    normalized=$(cygpath -u "$directory") || return 1
    [[ -n $normalized ]] || return 1
    (cd -- "$normalized" && pwd -P)
}
sha256() { shasum -a 256 "$1" | awk '{print $1}'; }
prefix=$(normalize_dir "$prefix_arg") || fail "prefix does not exist"
for directory in bin lib lib/pkgconfig include include/cairo share share/vacards-cairo; do
    [[ -d $prefix/$directory && ! -L $prefix/$directory ]] || fail "missing or symlinked directory: $directory"
done
script_dir=$(cd -- "$(dirname -- "$0")" && pwd -P)
source_root=$(cd -- "$script_dir/../../.." && pwd -P)
dependency_tool=$source_root/packaging/macos/vacards/vacards-dependencies.sh
common_patch=$source_root/packaging/macos/vacards/cairo-1.18.4-clip-all.patch
marker=$prefix/VACARDS-CAIRO.env
inventory=$prefix/VACARDS-CAIRO.sha256
[[ -f $marker && ! -L $marker && -f $inventory && ! -L $inventory ]] || fail "missing regular provenance record/inventory (stock Cairo is unsupported)"

# Never source a provenance file. Reject duplicate/unknown keys before reading.
keys='format platform architecture cairo_release source_archive_sha256 upstream_fix patch_sha256 prefix_posix prefix_windows compiler_target inventory_sha256 clipping_test'
awk -F= -v keys="$keys" '
    BEGIN {n=split(keys,a," "); for(i=1;i<=n;i++) allowed[a[i]]=1}
    !($1 in allowed) || ++seen[$1] != 1 || index($0,"=")==0 {bad=1}
    END {for(k in allowed) if(seen[k]!=1) bad=1; exit bad}
' "$marker" || fail "invalid provenance keys"
value() { awk -F= -v key="$1" '$1==key {sub(/^[^=]*=/, ""); print}' "$marker"; }
expect() { [[ $(value "$1") == "$2" ]] || fail "wrong $1"; }
expect format 1
expect platform windows-ucrt64
expect architecture x86_64
expect compiler_target x86_64-w64-mingw32
expect clipping_test passed
release=$(bash "$dependency_tool" --get cairo_release)
archive_sha=$(bash "$dependency_tool" --get cairo_release_sha256)
fix=$(bash "$dependency_tool" --get cairo_fix_commit)
expect cairo_release "$release"
expect source_archive_sha256 "$archive_sha"
expect upstream_fix "$fix"
expect patch_sha256 "$(sha256 "$common_patch")"
expect prefix_posix "$prefix"
expect prefix_windows "$(cygpath -w "$prefix")"
grep -Fxq "Upstream: https://gitlab.freedesktop.org/cairo/cairo/-/commit/$fix" "$common_patch" || fail "common patch does not identify the pinned fix"
[[ $(value inventory_sha256) =~ ^[0-9a-f]{64}$ ]] || fail "invalid inventory hash"
expect inventory_sha256 "$(sha256 "$inventory")"

if [[ -n $expected_record ]]; then
    [[ -f $expected_record ]] || fail "missing saved gate record"
    cmp -s "$expected_record" "$marker" || fail "prefix differs from the saved gate record"
fi
[[ -z $runtime_dir || -n $expected_record ]] || fail "runtime verification requires --expected-record"

# The fixed build recipe produces exactly these three components. The inventory
# also binds import libraries, all Cairo headers/pc files and retained inputs.
dlls=(libcairo-2.dll libcairo-gobject-2.dll libcairo-script-interpreter-2.dll)
required=(share/vacards-cairo/source.tar.xz share/vacards-cairo/clip-all.patch
          share/vacards-cairo/toolchain.txt share/vacards-cairo/clip-test.c
          share/vacards-cairo/clip-test.log)
for stem in cairo cairo-gobject cairo-script-interpreter; do
    required+=("bin/lib$stem-2.dll" "lib/lib$stem.dll.a" "lib/pkgconfig/$stem.pc")
done
required+=(include/cairo/cairo.h include/cairo/cairo-version.h)
for path in "${required[@]}"; do
    [[ -f $prefix/$path && ! -L $prefix/$path ]] || fail "missing regular file: $path"
done

declare -a recorded=()
while IFS= read -r line || [[ -n $line ]]; do
    [[ $line =~ ^([0-9a-f]{64})\ \ (.+)$ ]] || fail "invalid inventory line"
    hash=${BASH_REMATCH[1]}; path=${BASH_REMATCH[2]}
    [[ $path =~ ^(bin/libcairo(-gobject|-script-interpreter)?-2\.dll|lib/libcairo(-gobject|-script-interpreter)?\.dll\.a|lib/pkgconfig/cairo[a-z0-9-]*\.pc|include/cairo/[a-z0-9-]+\.h|share/vacards-cairo/(source\.tar\.xz|clip-all\.patch|toolchain\.txt|clip-test\.c|clip-test\.log))$ ]] || fail "unsafe/unexpected inventory path: $path"
    [[ -f $prefix/$path && ! -L $prefix/$path ]] || fail "missing or symlinked inventory file: $path"
    [[ $(sha256 "$prefix/$path") == "$hash" ]] || fail "checksum changed: $path"
    recorded+=("$path")
done < "$inventory"
[[ ${#recorded[@]} -gt 0 ]] || fail "empty inventory"
[[ $(printf '%s\n' "${recorded[@]}" | sort | uniq -d) == '' ]] || fail "duplicate inventory path"
for path in "${required[@]}"; do
    printf '%s\n' "${recorded[@]}" | grep -Fxq "$path" || fail "unrecorded required file: $path"
done
for dir in lib/pkgconfig include/cairo; do
    while IFS= read -r file; do
        path=${file#"$prefix/"}
        printf '%s\n' "${recorded[@]}" | grep -Fxq "$path" || fail "unrecorded header/pc file: $path"
    done < <(find "$prefix/$dir" \( -type f -o -type l \) \( -name 'cairo*.pc' -o -name '*.h' \) -print)
done
[[ $(sha256 "$prefix/share/vacards-cairo/source.tar.xz") == "$archive_sha" ]] || fail "source archive bytes differ from the pin"
cmp -s "$common_patch" "$prefix/share/vacards-cairo/clip-all.patch" || fail "actual patch bytes changed"
cmp -s "$source_root/testfiles/vacards-windows-cairo-test.c" "$prefix/share/vacards-cairo/clip-test.c" || fail "clipping regression source changed"
[[ -s $prefix/share/vacards-cairo/toolchain.txt && -s $prefix/share/vacards-cairo/clip-test.log ]] || fail "missing toolchain or clipping evidence"

check_dlls() {
    local directory=$1 actual expected dll headers
    actual=$(find "$directory" -maxdepth 1 \( -iname 'libcairo*.dll' -o -iname 'cairo*.dll' \) -exec basename {} \; | sort)
    expected=$(printf '%s\n' "${dlls[@]}" | sort)
    [[ $actual == "$expected" ]] || fail "Cairo DLL inventory differs in $directory"
    for dll in "${dlls[@]}"; do
        [[ -f $directory/$dll && ! -L $directory/$dll ]] || fail "DLL is not a regular file: $dll"
        headers=$(objdump -f "$directory/$dll") || fail "cannot inspect PE file: $dll"
        grep -Eq 'file format pei-x86-64[[:space:]]*$' <<< "$headers" || fail "DLL is not PE x86-64: $dll"
        grep -Eq 'architecture: i386:x86-64,' <<< "$headers" || fail "wrong PE architecture: $dll"
        headers=$(objdump -p "$directory/$dll") || fail "cannot inspect PE characteristics: $dll"
        grep -Eq '^[[:space:]]+DLL[[:space:]]*$' <<< "$headers" || fail "PE file is not a DLL: $dll"
        [[ $(sha256 "$directory/$dll") == "$(sha256 "$prefix/bin/$dll")" ]] || fail "staged DLL differs from tested bytes: $dll"
    done
}
check_dlls "$prefix/bin"
for module in cairo cairo-gobject cairo-script-interpreter; do
    [[ $("$pkg_config" --modversion "$module") == "$release" ]] || fail "wrong pkg-config version: $module"
    resolved=$("$pkg_config" --variable=prefix "$module") || fail "cannot query pkg-config prefix: $module"
    actual=$(normalize_dir "$resolved") || fail "invalid pkg-config prefix: $module"
    [[ $actual == "$prefix" ]] || fail "pkg-config resolves $module outside the expected prefix: $actual"
    resolved=$("$pkg_config" --variable=pcfiledir "$module") || fail "cannot query pkg-config directory: $module"
    actual=$(normalize_dir "$resolved") || fail "invalid pkg-config directory: $module"
    [[ $actual == "$prefix/lib/pkgconfig" ]] || fail "pkg-config file for $module is outside the expected prefix"
    for variable in libdir includedir; do
        resolved=$("$pkg_config" --variable="$variable" "$module") || fail "cannot query $variable: $module"
        actual=$(normalize_dir "$resolved") || fail "invalid $variable: $module"
        expected=$prefix/lib; [[ $variable != includedir ]] || expected=$prefix/include
        [[ $actual == "$expected" ]] || fail "pkg-config $variable escapes the prefix: $module"
    done
done
if [[ -n $runtime_dir ]]; then
    runtime_dir=$(normalize_dir "$runtime_dir") || fail "missing runtime directory"
    check_dlls "$runtime_dir"
fi
echo "Verified VACards Windows Cairo $release ($fix); PE x86-64, exact source/patch and DLL bytes"
