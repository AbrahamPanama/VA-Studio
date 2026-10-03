#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Verify an existing Windows UCRT64 VACards patched librevenge prefix without
# modifying it. Fails closed on a stock 0.0.6 DLL or a changed hash.
set -euo pipefail
export LC_ALL=C

fail() { echo "VACards Windows librevenge verification failed: $*" >&2; exit 1; }
[[ $# == 1 ]] || { echo "usage: $0 LIBREVENGE_INSTALL_PREFIX" >&2; exit 2; }
[[ ${MSYSTEM:-} == UCRT64 ]] || fail "requires a UCRT64 shell"

prefix=$(cd -- "$(LC_ALL=C.UTF-8 cygpath -u "$1")" && pwd -P)
script_dir=$(cd -- "$(dirname -- "$0")" && pwd -P)
source_root=$(cd -- "$script_dir/../../.." && pwd -P)
bundle_manifest=$source_root/packaging/dependencies/librevenge-0.0.6/VACARDS-LIBREVENGE-BUNDLE.env
manifest=$prefix/VACARDS-LIBREVENGE.env
[[ -f $manifest ]] || fail "missing $manifest"
[[ -f $bundle_manifest ]] || fail "missing tracked bundle manifest"

read_value() {
    awk -F= -v key="$1" '$1 == key {sub(/^[^=]*=/, ""); print; exit}' "$2"
}
sha256() { shasum -a 256 "$1" | awk '{print $1}'; }

fields='format dependency release source_identity archive_filename archive_sha256 patch_filename patch_sha256 capability_name capability_value capability_binary_marker library_relative_path library_sha256 test_total test_crop test_text test_other make_check_total make_check_pass make_check_fail created_utc'
for key in $fields; do
    count=$(awk -F= -v key="$key" '$1 == key {count++} END {print count + 0}' "$manifest")
    [[ $count == 1 ]] || fail "expected exactly one '$key' field"
done
unknown_fields=$(awk -F= '
    $1 != "format" && $1 != "dependency" && $1 != "release" &&
    $1 != "source_identity" && $1 != "archive_filename" &&
    $1 != "archive_sha256" && $1 != "patch_filename" &&
    $1 != "patch_sha256" && $1 != "capability_name" &&
    $1 != "capability_value" && $1 != "capability_binary_marker" &&
    $1 != "library_relative_path" && $1 != "library_sha256" &&
    $1 != "test_total" && $1 != "test_crop" && $1 != "test_text" &&
    $1 != "test_other" && $1 != "make_check_total" &&
    $1 != "make_check_pass" && $1 != "make_check_fail" &&
    $1 != "created_utc" {print $1}
' "$manifest")
[[ -z $unknown_fields ]] || fail "unknown field(s): $unknown_fields"

[[ $(read_value format "$manifest") == 1 ]] || fail "unsupported manifest format"
[[ $(read_value dependency "$manifest") == librevenge ]] || fail "wrong dependency name"
[[ $(read_value release "$manifest") == 0.0.6 ]] || fail "wrong librevenge release"
[[ $(read_value archive_sha256 "$manifest") == "$(read_value archive_sha256 "$bundle_manifest")" ]] ||
    fail "archive hash differs from the tracked bundle manifest"
[[ $(read_value patch_sha256 "$manifest") == "$(read_value patch_sha256 "$bundle_manifest")" ]] ||
    fail "patch hash differs from the tracked bundle manifest"
[[ $(read_value capability_name "$manifest") == "$(read_value capability_name "$bundle_manifest")" ]] ||
    fail "capability name differs from the tracked bundle manifest"
[[ $(read_value capability_value "$manifest") == "$(read_value capability_value "$bundle_manifest")" ]] ||
    fail "capability value differs from the tracked bundle manifest"
[[ $(read_value capability_binary_marker "$manifest") == "$(read_value capability_binary_marker "$bundle_manifest")" ]] ||
    fail "capability binary marker differs from the tracked bundle manifest"

relative=$(read_value library_relative_path "$manifest")
case $relative in ''|/*|*../*) fail "unsafe library_relative_path '$relative'" ;; esac
library=$prefix/$relative
[[ -f $library ]] || fail "missing attested library $library"
expected_sha256=$(read_value library_sha256 "$manifest")
[[ $expected_sha256 =~ ^[0-9a-f]{64}$ ]] || fail "library_sha256 is invalid"
[[ $(sha256 "$library") == "$expected_sha256" ]] || fail "librevenge DLL checksum changed"

marker=$(read_value capability_binary_marker "$manifest")
[[ -n $marker ]] || fail "empty capability marker"
grep -a -q "$marker" "$library" || fail "DLL is missing capability marker '$marker' (stock 0.0.6 is not the patched build)"

pkg_config=${PKG_CONFIG:-pkg-config}
# Use exactly one deterministic Windows-native pkg-config directory for the
# identity queries. Assigning (not appending) PKG_CONFIG_PATH replaces any
# inherited/ambient search list, so a stock librevenge cannot win. An inherited
# list can arrive in Windows form (backslashes, ';') and mixing it with the
# POSIX ':${prefix}/lib/pkgconfig' prefix makes native pkg-config fall through
# to the ambient stock prefix. librevenge-0.0.pc declares no Requires, so no
# other search directory is needed here.
pkg_path=$(LC_ALL=C.UTF-8 cygpath -am "$prefix/lib/pkgconfig")
actual_version=$(PKG_CONFIG_PATH="$pkg_path" \
    "$pkg_config" --modversion librevenge-0.0 2>/dev/null || true)
[[ $actual_version == "$(read_value release "$manifest")" ]] ||
    fail "pkg-config resolves librevenge version '$actual_version'"
actual_prefix=$(PKG_CONFIG_PATH="$pkg_path" \
    "$pkg_config" --variable=prefix librevenge-0.0 2>/dev/null || true)
[[ -n $actual_prefix ]] || fail "pkg-config returned an unavailable librevenge prefix"
# pkgconf 3.x backslash-escapes spaces when printing --variable values. Undo
# only that escape so a legitimate prefix containing spaces is not split into
# path components by cygpath; real ASCII prefixes are byte-for-byte unaffected.
actual_prefix=${actual_prefix//\\ / }
actual_prefix=$(cd -- "$(LC_ALL=C.UTF-8 cygpath -u "$actual_prefix")" && pwd -P)
[[ $actual_prefix == "$prefix" ]] ||
    fail "pkg-config resolves librevenge outside the attested prefix: $actual_prefix"

echo "Verified VACards librevenge $(read_value release "$manifest") patch $(read_value patch_sha256 "$manifest")"
