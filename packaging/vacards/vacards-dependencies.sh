#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later

set -eu
LC_ALL=C
export LC_ALL

usage()
{
    cat >&2 <<'EOF'
usage: vacards-dependencies.sh [--file MANIFEST] OPTION [ARGUMENT]

Options:
  --validate        validate the manifest and print its path
  --get FIELD       print one validated field
  --env             print the validated manifest
  --sha256          print the manifest SHA-256
EOF
    exit 2
}

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
source_root=$(CDPATH= cd -- "$script_dir/../.." && pwd)
manifest=$source_root/VACARDS-DEPENDENCIES.env

if [ "$#" -ge 2 ] && [ "$1" = "--file" ]; then
    manifest=$2
    shift 2
fi

[ "$#" -ge 1 ] || usage
option=$1
shift

fail()
{
    echo "Invalid VACards dependency manifest: $*" >&2
    exit 1
}

read_value()
{
    key=$1
    awk -F= -v key="$key" '$1 == key {sub(/^[^=]*=/, ""); sub(/\r$/, ""); print; exit}' "$manifest"
}

fields='format source_baseline_commit libcdr_repository libcdr_ref libcdr_commit libcdr_pkgconfig_version cairo_release cairo_release_sha256 cairo_fix_commit nesting_engine_repository nesting_engine_version nesting_engine_commit nesting_rust_toolchain nesting_ffi_api_version sparrow_repository sparrow_commit sparrow_darwin_arm64_sha256 sparrow_windows_x64_sha256 sparrow_windows_rust_toolchain sparrow_windows_cargo_lock_sha256 tiff_rgb_profile_filename tiff_rgb_profile_sha256 macos_deployment_target required_cmake_features disabled_cmake_features pango_windows_release pango_windows_platform pango_windows_source_archive_sha256 pango_windows_patch_filename pango_windows_patch_sha256 pango_windows_bundle pango_windows_prefix'

[ -f "$manifest" ] || fail "missing $manifest"
tr -d '\000' < "$manifest" | cmp -s "$manifest" - || fail "NUL byte in manifest"
awk '{sub(/\r$/, ""); if ($0 ~ /[[:cntrl:]]/ || $0 !~ /^[a-z][a-z0-9_]*=[^[:space:]=]+$/) exit 1}' \
    "$manifest" || fail "malformed key=value record"
# Count every required field in one process. Repeated awk launches are costly
# under MSYS; retain the same required-field order and first-failure diagnostic.
invalid_field=$(awk -F= -v fields="$fields" '
    { counts[$1]++ }
    END {
        n = split(fields, required, " ")
        for (i = 1; i <= n; i++) {
            if (counts[required[i]] != 1) { print required[i]; exit }
        }
    }
' "$manifest")
[ -z "$invalid_field" ] || fail "expected exactly one '$invalid_field' field"

unknown_fields=$(awk -F= '
    $1 != "format" &&
    $1 != "source_baseline_commit" &&
    $1 != "libcdr_repository" &&
    $1 != "libcdr_ref" &&
    $1 != "libcdr_commit" &&
    $1 != "libcdr_pkgconfig_version" &&
    $1 != "cairo_release" &&
    $1 != "cairo_release_sha256" &&
    $1 != "cairo_fix_commit" &&
    $1 != "nesting_engine_repository" &&
    $1 != "nesting_engine_version" &&
    $1 != "nesting_engine_commit" &&
    $1 != "nesting_rust_toolchain" &&
    $1 != "nesting_ffi_api_version" &&
    $1 != "sparrow_repository" &&
    $1 != "sparrow_commit" &&
    $1 != "sparrow_darwin_arm64_sha256" &&
    $1 != "sparrow_windows_x64_sha256" &&
    $1 != "sparrow_windows_rust_toolchain" &&
    $1 != "sparrow_windows_cargo_lock_sha256" &&
    $1 != "tiff_rgb_profile_filename" &&
    $1 != "tiff_rgb_profile_sha256" &&
    $1 != "macos_deployment_target" &&
    $1 != "required_cmake_features" &&
    $1 != "disabled_cmake_features" &&
    $1 != "pango_windows_release" &&
    $1 != "pango_windows_platform" &&
    $1 != "pango_windows_source_archive_sha256" &&
    $1 != "pango_windows_patch_filename" &&
    $1 != "pango_windows_patch_sha256" &&
    $1 != "pango_windows_bundle" &&
    $1 != "pango_windows_prefix" {print $1}
' "$manifest")
[ -z "$unknown_fields" ] || fail "unknown field(s): $unknown_fields"

# Records above are unique and contain no whitespace. Extract in the fixed
# required-field order with one awk, then use shell builtins to assign values.
# read -r preserves literal backslashes; manifest data is never evaluated.
manifest_values=$(awk -F= -v fields="$fields" '
    { sub(/\r$/, ""); values[$1] = $2 }
    END {
        n = split(fields, required, " ")
        for (i = 1; i <= n; i++) {
            printf "%s%s", values[required[i]], (i == n ? "\n" : " ")
        }
    }
' "$manifest")
IFS=' ' read -r format source_baseline_commit libcdr_repository libcdr_ref \
    libcdr_commit libcdr_pkgconfig_version cairo_release cairo_release_sha256 \
    cairo_fix_commit nesting_engine_repository nesting_engine_version \
    nesting_engine_commit nesting_rust_toolchain nesting_ffi_api_version \
    sparrow_repository sparrow_commit sparrow_darwin_arm64_sha256 \
    sparrow_windows_x64_sha256 sparrow_windows_rust_toolchain \
    sparrow_windows_cargo_lock_sha256 tiff_rgb_profile_filename \
    tiff_rgb_profile_sha256 macos_deployment_target required_cmake_features \
    disabled_cmake_features pango_windows_release pango_windows_platform \
    pango_windows_source_archive_sha256 pango_windows_patch_filename \
    pango_windows_patch_sha256 pango_windows_bundle pango_windows_prefix <<EOF
$manifest_values
EOF

# Keep the same ordered value checks in one POSIX awk invocation. Starting a
# separate grep for every field dominates native Windows/MSYS validation time.
invalid_value=$(awk -F= '
    function require(ok, message) { if (!ok) { print message; exit } }
    { sub(/\r$/, ""); v[$1] = $2 }
    END {
        require(v["format"] == "1", "unsupported format \047" v["format"] "\047")
        require(v["sparrow_repository"] ~ /^https:\/\/[^[:space:]]+$/, "sparrow_repository must be an HTTPS URL")
        require(v["sparrow_commit"] ~ /^[0-9a-f]{40}$/, "sparrow_commit must be a full lowercase Git object id")
        require(v["sparrow_darwin_arm64_sha256"] ~ /^[0-9a-f]{64}$/, "sparrow_darwin_arm64_sha256 must be a lowercase SHA-256")
        require(v["sparrow_windows_x64_sha256"] ~ /^[0-9a-f]{64}$/, "sparrow_windows_x64_sha256 must be a lowercase SHA-256")
        require(v["sparrow_windows_cargo_lock_sha256"] ~ /^[0-9a-f]{64}$/, "sparrow_windows_cargo_lock_sha256 must be a lowercase SHA-256")
        require(v["sparrow_windows_rust_toolchain"] ~ /^[0-9]+\.[0-9]+\.[0-9]+$/, "sparrow_windows_rust_toolchain must have three numeric components")
        require(v["source_baseline_commit"] ~ /^[0-9a-f]{40}$/, "source_baseline_commit must be a full lowercase Git object id")
        require(v["libcdr_repository"] ~ /^https:\/\/[^[:space:]]+$/, "libcdr_repository must be an HTTPS URL")
        require(v["libcdr_ref"] ~ /^[A-Za-z0-9][A-Za-z0-9._\/-]*$/, "libcdr_ref is not a safe Git ref")
        require(v["libcdr_ref"] !~ /\.\./, "libcdr_ref must not contain \047..\047")
        require(v["libcdr_commit"] ~ /^[0-9a-f]{40}$/, "libcdr_commit must be a full lowercase Git object id")
        require(v["libcdr_pkgconfig_version"] ~ /^[0-9]+\.[0-9]+\.[0-9]+$/, "libcdr_pkgconfig_version must have three numeric components")
        require(v["cairo_release"] ~ /^[0-9]+\.[0-9]+\.[0-9]+$/, "cairo_release must have three numeric components")
        require(v["cairo_release_sha256"] ~ /^[0-9a-f]{64}$/, "cairo_release_sha256 must be a lowercase SHA-256")
        require(v["cairo_fix_commit"] ~ /^[0-9a-f]{40}$/, "cairo_fix_commit must be a full lowercase Git object id")
        require(v["nesting_engine_repository"] ~ /^https:\/\/[^[:space:]]+$/, "nesting_engine_repository must be an HTTPS URL")
        require(v["nesting_engine_version"] ~ /^[0-9]+\.[0-9]+\.[0-9]+$/, "nesting_engine_version must have three numeric components")
        require(v["nesting_engine_commit"] ~ /^[0-9a-f]{40}$/, "nesting_engine_commit must be a full lowercase Git object id")
        require(v["nesting_rust_toolchain"] ~ /^[0-9]+\.[0-9]+\.[0-9]+$/, "nesting_rust_toolchain must have three numeric components")
        require(v["nesting_ffi_api_version"] ~ /^[1-9][0-9]*$/, "nesting_ffi_api_version must be a positive integer")
        require(v["tiff_rgb_profile_filename"] ~ /^[A-Za-z0-9][A-Za-z0-9._-]*\.(icc|icm)$/, "tiff_rgb_profile_filename must be a safe ICC filename")
        require(v["tiff_rgb_profile_sha256"] ~ /^[0-9a-f]{64}$/, "tiff_rgb_profile_sha256 must be a lowercase SHA-256")
        require(v["macos_deployment_target"] ~ /^[0-9]+\.[0-9]+$/, "macos_deployment_target must be MAJOR.MINOR")
        require(v["required_cmake_features"] ~ /^[A-Z][A-Z0-9_]*(,[A-Z][A-Z0-9_]*)*$/, "required_cmake_features must be a comma-separated CMake option list")
        require(v["disabled_cmake_features"] ~ /^[A-Z][A-Z0-9_]*(,[A-Z][A-Z0-9_]*)*$/, "disabled_cmake_features must be a comma-separated CMake option list")
        require(v["pango_windows_release"] ~ /^[0-9]+\.[0-9]+\.[0-9]+$/, "pango_windows_release must have three numeric components")
        require(v["pango_windows_platform"] ~ /^[a-z0-9][a-z0-9-]*$/, "pango_windows_platform must be a safe platform name")
        require(v["pango_windows_source_archive_sha256"] ~ /^[0-9a-f]{64}$/, "pango_windows_source_archive_sha256 must be a lowercase SHA-256")
        require(v["pango_windows_patch_filename"] ~ /^[A-Za-z0-9][A-Za-z0-9._-]*\.patch$/, "pango_windows_patch_filename must be a safe patch filename")
        require(v["pango_windows_patch_sha256"] ~ /^[0-9a-f]{64}$/, "pango_windows_patch_sha256 must be a lowercase SHA-256")
        require(v["pango_windows_bundle"] ~ /^packaging\/dependencies\/[A-Za-z0-9._\/-]+\.env$/ && v["pango_windows_bundle"] !~ /\.\./, "pango_windows_bundle must be a repository-relative packaging/dependencies path")
        require(v["pango_windows_prefix"] ~ /^[A-Za-z]:\/[A-Za-z0-9._\/-]+$/, "pango_windows_prefix must be an absolute Windows path")
    }
' "$manifest")
[ -z "$invalid_value" ] || fail "$invalid_value"
[ -z "$(printf '%s,%s' "$required_cmake_features" "$disabled_cmake_features" | tr ',' '\n' | sort | uniq -d)" ] ||
    fail "a CMake feature cannot be both required and disabled"

case "$option" in
    --validate)
        [ "$#" -eq 0 ] || usage
        printf '%s\n' "$manifest"
        ;;
    --get)
        [ "$#" -eq 1 ] || usage
        requested=$1
        case " $fields " in
            *" $requested "*) read_value "$requested" ;;
            *) fail "unknown requested field '$requested'" ;;
        esac
        ;;
    --env)
        [ "$#" -eq 0 ] || usage
        cat "$manifest"
        ;;
    --sha256)
        [ "$#" -eq 0 ] || usage
        # Use the evidence hasher runtime; read bytes even on Windows.
        python3 -c 'import hashlib, sys; print(hashlib.sha256(open(sys.argv[1], "rb").read()).hexdigest())' "$manifest"
        ;;
    *) usage ;;
esac
