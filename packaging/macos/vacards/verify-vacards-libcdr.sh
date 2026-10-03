#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later

set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: $0 LIBCDR_INSTALL_PREFIX" >&2
    exit 2
fi

prefix=$(CDPATH= cd -- "$1" && pwd -P)
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
dependency_tool=$script_dir/vacards-dependencies.sh
manifest=$prefix/VACARDS-LIBCDR.env

fail()
{
    echo "VACards libcdr verification failed: $*" >&2
    exit 1
}

read_value()
{
    key=$1
    awk -F= -v key="$key" '$1 == key {sub(/^[^=]*=/, ""); print; exit}' "$manifest"
}

[ -f "$manifest" ] || fail "missing $manifest"
fields='format dependency source_repository source_ref source_commit pkgconfig_version library_relative_path library_sha256 macos_deployment_target created_utc'
for key in $fields; do
    count=$(awk -F= -v key="$key" '$1 == key {count++} END {print count + 0}' "$manifest")
    [ "$count" -eq 1 ] || fail "expected exactly one '$key' field"
done
unknown_fields=$(awk -F= '
    $1 != "format" && $1 != "dependency" &&
    $1 != "source_repository" && $1 != "source_ref" &&
    $1 != "source_commit" && $1 != "pkgconfig_version" &&
    $1 != "library_relative_path" && $1 != "library_sha256" &&
    $1 != "macos_deployment_target" && $1 != "created_utc" {print $1}
' "$manifest")
[ -z "$unknown_fields" ] || fail "unknown field(s): $unknown_fields"

[ "$(read_value format)" = "1" ] || fail "unsupported manifest format"
[ "$(read_value dependency)" = "libcdr" ] || fail "wrong dependency name"
[ "$(read_value source_repository)" = "$($dependency_tool --get libcdr_repository)" ] ||
    fail "source repository differs from VACARDS-DEPENDENCIES.env"
[ "$(read_value source_ref)" = "$($dependency_tool --get libcdr_ref)" ] ||
    fail "source ref differs from VACARDS-DEPENDENCIES.env"
[ "$(read_value source_commit)" = "$($dependency_tool --get libcdr_commit)" ] ||
    fail "source commit differs from VACARDS-DEPENDENCIES.env"
[ "$(read_value pkgconfig_version)" = "$($dependency_tool --get libcdr_pkgconfig_version)" ] ||
    fail "pkg-config version differs from VACARDS-DEPENDENCIES.env"
[ "$(read_value macos_deployment_target)" = "$($dependency_tool --get macos_deployment_target)" ] ||
    fail "deployment target differs from VACARDS-DEPENDENCIES.env"

relative=$(read_value library_relative_path)
case "$relative" in
    ''|/*|*../*) fail "unsafe library_relative_path '$relative'" ;;
esac
library=$prefix/$relative
[ -f "$library" ] || fail "missing attested library $library"
expected_sha256=$(read_value library_sha256)
printf '%s\n' "$expected_sha256" | grep -Eq '^[0-9a-f]{64}$' ||
    fail "library_sha256 is invalid"
actual_sha256=$(shasum -a 256 "$library" | awk '{print $1}')
[ "$actual_sha256" = "$expected_sha256" ] || fail "libcdr library checksum changed"
if [ "$(uname -s)" = "Darwin" ]; then
    actual_minos=$(vtool -show-build "$library" | awk '$1 == "minos" {print $2; exit}')
    [ "$actual_minos" = "$(read_value macos_deployment_target)" ] ||
        fail "library minimum macOS is '$actual_minos'"
fi

pkg_path=$prefix/lib/pkgconfig
actual_version=$(PKG_CONFIG_PATH="$pkg_path" \
    pkg-config --modversion libcdr-0.1 2>/dev/null || true)
[ "$actual_version" = "$(read_value pkgconfig_version)" ] ||
    fail "pkg-config resolves libcdr version '$actual_version'"
actual_prefix=$(PKG_CONFIG_PATH="$pkg_path" \
    pkg-config --variable=prefix libcdr-0.1 2>/dev/null || true)
# Native pkg-config can report C:/... while MSYS sh reports /c/.... Compare
# canonical directories; a missing or genuinely different prefix still fails.
[ -n "$actual_prefix" ] && actual_prefix=$(CDPATH= cd -- "$actual_prefix" && pwd -P) ||
    fail "pkg-config returned an unavailable libcdr prefix"
[ "$actual_prefix" = "$prefix" ] ||
    fail "pkg-config resolves libcdr outside the attested prefix: $actual_prefix"

echo "Verified VACards libcdr $(read_value pkgconfig_version) ($(read_value source_commit))"
