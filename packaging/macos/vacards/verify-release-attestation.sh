#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later

set -eu

candidate=0
if [ "$#" -eq 2 ] && [ "$1" = "--candidate" ]; then
    candidate=1
    shift
fi
if [ "$#" -ne 1 ]; then
    echo "usage: $0 [--candidate] INKSCAPE_INSTALL_PREFIX" >&2
    exit 2
fi

inkscape_prefix=$(CDPATH= cd -- "$1" && pwd)
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
source_root=$(CDPATH= cd -- "$script_dir/../../.." && pwd)
attestation=$inkscape_prefix/VACARDS-RELEASE-GATE.env
if [ "$candidate" -eq 1 ]; then
    attestation=$inkscape_prefix/VACARDS-RELEASE-GATE.pending.env
fi
executable=$inkscape_prefix/bin/inkscape
version_tool=$script_dir/vacards-version.sh
dependency_tool=$script_dir/vacards-dependencies.sh
dependency_manifest=$inkscape_prefix/VACARDS-DEPENDENCIES.env
libcdr_manifest=$inkscape_prefix/VACARDS-LIBCDR.env
build_provenance=$inkscape_prefix/VACARDS-BUILD-PROVENANCE.env
resolved_features=$inkscape_prefix/VACARDS-CMAKE-FEATURES.env
packaging_provenance=$script_dir/../../vacards/packaging-provenance.py

fail()
{
    echo "VACards release attestation rejected: $*" >&2
    exit 1
}

read_value()
{
    key=$1
    awk -F= -v key="$key" '$1 == key {sub(/^[^=]*=/, ""); print; exit}' "$attestation"
}

read_file_value()
{
    file=$1
    key=$2
    awk -F= -v key="$key" '$1 == key {sub(/^[^=]*=/, ""); print; exit}' "$file"
}

# The same production boundary protects both shell modes and direct bundle
# consumers. Structural suite closure cannot replace independent authentication
# and acceptance. Preserve every format-3/runtime binding below when that
# currently unavailable boundary is provisioned; never upgrade old records.
python3 -B "$packaging_provenance" require-release-authorization

[ -f "$attestation" ] || fail "missing $attestation"
[ -x "$executable" ] || fail "missing executable $executable"
for file in "$dependency_manifest" "$libcdr_manifest" "$build_provenance" "$resolved_features"; do
    [ -f "$file" ] || fail "missing provenance file $file"
done
if [ "$(uname -s)" = "Darwin" ]; then
    if [ "$candidate" -eq 1 ]; then
        python3 "$packaging_provenance" verify-attestation --candidate "$inkscape_prefix"
    else
        python3 "$packaging_provenance" verify-attestation "$inkscape_prefix"
    fi
    python3 "$packaging_provenance" verify-inputs "$inkscape_prefix/VACARDS-PACKAGING-INPUTS.json"
fi

format=$(read_value format)
scope=$(read_value scope)
base_version=$(read_value base_version)
build_number=$(read_value build_number)
release_version=$(read_value release_version)
source_commit=$(read_value source_commit)
source_baseline_commit=$(read_value source_baseline_commit)
binary_sha256=$(read_value binary_sha256)
binary_architectures=$(read_value binary_architectures)
macos_deployment_target=$(read_value macos_deployment_target)
dependency_manifest_sha256=$(read_value dependency_manifest_sha256)
libcdr_commit=$(read_value libcdr_commit)
libcdr_version=$(read_value libcdr_version)
libcdr_library_sha256=$(read_value libcdr_library_sha256)
libcdr_manifest_sha256=$(read_value libcdr_manifest_sha256)
build_provenance_sha256=$(read_value build_provenance_sha256)
cmake_cache_sha256=$(read_value cmake_cache_sha256)
cmake_features_sha256=$(read_value cmake_features_sha256)
submodules_sha256=$(read_value submodules_sha256)

[ "$format" = "3" ] || fail "unsupported format '$format'"
[ "$scope" = "full" ] || fail "scope '$scope' is not eligible for packaging"
[ "$base_version" = "$("$version_tool" --base)" ] ||
    fail "base version '$base_version' does not match the source version"
[ "$build_number" = "$("$version_tool" --build)" ] ||
    fail "build number '$build_number' does not match the source version"
[ "$release_version" = "$("$version_tool" --release)" ] ||
    fail "release '$release_version' does not match the source version"
[ -n "$source_commit" ] || fail "source_commit is empty"
[ "$source_baseline_commit" = "$("$dependency_tool" --get source_baseline_commit)" ] ||
    fail "attested source baseline differs from the dependency manifest"
[ "$(git -C "$source_root" cat-file -t "$source_baseline_commit" 2>/dev/null || true)" = "commit" ] ||
    fail "attested source baseline is not present"
git -C "$source_root" merge-base --is-ancestor "$source_baseline_commit" "$source_commit" ||
    fail "attested source commit omits required baseline $source_baseline_commit"
[ -n "$binary_sha256" ] || fail "binary_sha256 is empty"
for value in "$dependency_manifest_sha256" "$libcdr_library_sha256" \
             "$libcdr_manifest_sha256" "$build_provenance_sha256" \
             "$cmake_cache_sha256" "$cmake_features_sha256" "$submodules_sha256"; do
    printf '%s\n' "$value" | grep -Eq '^[0-9a-f]{64}$' ||
        fail "attestation contains an invalid SHA-256"
done

current_dependency_sha=$("$dependency_tool" --sha256)
[ "$current_dependency_sha" = "$dependency_manifest_sha256" ] ||
    fail "source dependency manifest changed after the gate"
installed_dependency_sha=$(shasum -a 256 "$dependency_manifest" | awk '{print $1}')
[ "$installed_dependency_sha" = "$dependency_manifest_sha256" ] ||
    fail "installed dependency manifest differs from the gated source"
[ "$libcdr_commit" = "$("$dependency_tool" --get libcdr_commit)" ] ||
    fail "attested libcdr commit differs from the dependency manifest"
[ "$libcdr_version" = "$("$dependency_tool" --get libcdr_pkgconfig_version)" ] ||
    fail "attested libcdr version differs from the dependency manifest"
[ "$(read_file_value "$libcdr_manifest" source_commit)" = "$libcdr_commit" ] ||
    fail "installed libcdr provenance identifies another commit"
[ "$(read_file_value "$libcdr_manifest" pkgconfig_version)" = "$libcdr_version" ] ||
    fail "installed libcdr provenance identifies another version"
[ "$(read_file_value "$libcdr_manifest" library_sha256)" = "$libcdr_library_sha256" ] ||
    fail "installed libcdr provenance identifies another library"
current_libcdr_manifest_sha=$(shasum -a 256 "$libcdr_manifest" | awk '{print $1}')
[ "$current_libcdr_manifest_sha" = "$libcdr_manifest_sha256" ] ||
    fail "installed libcdr provenance changed after the gate"
current_build_provenance_sha=$(shasum -a 256 "$build_provenance" | awk '{print $1}')
[ "$current_build_provenance_sha" = "$build_provenance_sha256" ] ||
    fail "build provenance changed after the gate"
current_features_sha=$(shasum -a 256 "$resolved_features" | awk '{print $1}')
[ "$current_features_sha" = "$cmake_features_sha256" ] ||
    fail "resolved CMake feature evidence changed after the gate"
for feature in $("$dependency_tool" --get required_cmake_features | tr ',' ' '); do
    [ "$(read_file_value "$resolved_features" "$feature")" = "ON" ] ||
        fail "installed feature evidence does not enable $feature"
done
for feature in $("$dependency_tool" --get disabled_cmake_features | tr ',' ' '); do
    [ "$(read_file_value "$resolved_features" "$feature")" = "OFF" ] ||
        fail "installed feature evidence does not disable $feature"
done

current_commit=$(git -C "$source_root" rev-parse HEAD)
[ "$current_commit" = "$source_commit" ] || \
    fail "source is $current_commit but gate covered $source_commit"

if [ -n "$(git -C "$source_root" status --porcelain --untracked-files=no)" ]; then
    fail "tracked source changes exist after the gate"
fi

submodule_status=$(mktemp "${TMPDIR:-/tmp}/vacards-submodules.XXXXXX")
trap 'rm -f "$submodule_status"' EXIT HUP INT TERM
git -C "$source_root" submodule status --recursive > "$submodule_status"
current_submodules_sha=$(shasum -a 256 "$submodule_status" | awk '{print $1}')
[ "$current_submodules_sha" = "$submodules_sha256" ] ||
    fail "submodule state changed after the gate"

current_sha256=$(shasum -a 256 "$executable" | awk '{print $1}')
[ "$current_sha256" = "$binary_sha256" ] || \
    fail "installed executable changed after the gate"

short_commit=$(printf '%s' "$source_commit" | cut -c1-10)
version=$("$executable" --version 2>/dev/null || true)
printf '%s\n' "$version" | grep -Fq "$short_commit" || \
    fail "executable version does not identify $short_commit"

if [ "$(uname -s)" = "Darwin" ]; then
    [ "$(lipo -archs "$executable")" = "$binary_architectures" ] ||
        fail "installed executable architecture changed after the gate"
    current_minos=$(vtool -show-build "$executable" | awk '$1 == "minos" {print $2; exit}')
    [ "$current_minos" = "$macos_deployment_target" ] ||
        fail "installed executable deployment target changed after the gate"
    [ "$macos_deployment_target" = "$("$dependency_tool" --get macos_deployment_target)" ] ||
        fail "deployment target differs from VACARDS-DEPENDENCIES.env"
fi

if [ "$candidate" -eq 1 ]; then
    echo "Validated VACards pending candidate (not packaging authorization): $release_version ($short_commit)"
else
    echo "Verified VACards release gate: $release_version ($short_commit)"
fi
