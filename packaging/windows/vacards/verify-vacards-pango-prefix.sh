#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Verify the exact pinned Pango DLL set in an install prefix and optional staged payload.
set -euo pipefail
export LC_ALL=C
fail() { echo "VACards Windows Pango verification failed: $*" >&2; exit 1; }
[[ $# == 2 ]] || { echo "usage: $0 PREFIX RUNTIME_DIR" >&2; exit 2; }
prefix=$(cygpath -u "$1")
runtime=$(cygpath -u "$2")
script_dir=$(cd -- "$(dirname -- "$0")" && pwd -P)
source_root=$(cd -- "$script_dir/../../.." && pwd -P)
bundle="$source_root/packaging/dependencies/pango-1.58.2-win32-null-face/VACARDS-PANGO-BUNDLE.env"
[[ -f $bundle ]] || fail "missing tracked Pango bundle pin"
get() { awk -F= -v key="$1" '$1==key {sub(/^[^=]*=/, ""); print}' "$bundle"; }
[[ $(get format) == 1 && $(get dependency) == pango && $(get pango_version) == 1.58.2 ]] || fail "invalid Pango bundle manifest"
[[ $(get source_archive_sha256) == 342385b6ca3b7c73455d7c80a13b7dbe4489e00bc3bd4c5bd6ed4dce421e374a ]] || fail "Pango source archive pin changed"
[[ -d $prefix/bin && -d $runtime/bin ]] || fail "prefix or payload bin directory missing"
expected="$source_root/packaging/dependencies/pango-1.58.2-win32-null-face/pango-dlls.sha256"
[[ -f $expected ]] || fail "missing tracked DLL hash pin"
tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT
(cd "$prefix" && sha256sum bin/libpango*.dll | sed -E 's# \*?bin/#  bin/#' | sort -k2,2) > "$tmp"
cmp -s "$expected" "$tmp" || fail "Pango prefix DLL hashes differ from the tracked build pin"
while read -r digest name; do
    [[ -n $digest && -n $name ]] || fail "malformed DLL pin"
    [[ -f $prefix/$name && -f $runtime/$name ]] || fail "missing pinned DLL $name"
    [[ $(sha256sum "$prefix/$name" | awk '{print $1}') == "$digest" ]] || fail "prefix hash mismatch: $name"
    [[ $(sha256sum "$runtime/$name" | awk '{print $1}') == "$digest" ]] || fail "staged payload hash mismatch: $name"
done < "$expected"
echo "PASS: staged Pango DLLs match pinned prefix ($prefix -> $runtime)"
