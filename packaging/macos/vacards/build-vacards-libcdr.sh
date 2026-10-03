#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later

set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: $0 OUTPUT_DIRECTORY" >&2
    exit 2
fi

output_dir=$1
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
dependency_tool=$script_dir/vacards-dependencies.sh
repository=$($dependency_tool --get libcdr_repository)
ref=$($dependency_tool --get libcdr_ref)
commit=$($dependency_tool --get libcdr_commit)
expected_version=$($dependency_tool --get libcdr_pkgconfig_version)
deployment_target=$($dependency_tool --get macos_deployment_target)
source_dir=$output_dir/source
build_dir=$output_dir/build
prefix=$output_dir/install
jobs=${VACARDS_DEPENDENCY_JOBS:-2}
dependency_pkg_path=${PKG_CONFIG_PATH:-}
dependency_cppflags=${CPPFLAGS:-}
source_root=$(CDPATH= cd -- "$script_dir/../../.." && pwd)

fail()
{
    echo "VACards libcdr build failed: $*" >&2
    exit 1
}

for tool in git make pkg-config shasum; do
    command -v "$tool" >/dev/null 2>&1 || fail "required tool not found: $tool"
done
case "$jobs" in
    ''|0|*[!0-9]*) fail "VACARDS_DEPENDENCY_JOBS must be a positive integer" ;;
esac

# Homebrew installs ICU keg-only, so pkg-config cannot discover it through the
# global search path even though libcdr requires it. Resolve that one declared
# build dependency explicitly instead of relying on a developer's shell.
if ! PKG_CONFIG_PATH="$dependency_pkg_path" pkg-config --exists icu-i18n; then
    command -v brew >/dev/null 2>&1 ||
        fail "icu-i18n.pc is unavailable; install ICU or add it to PKG_CONFIG_PATH"
    icu_prefix=$(brew --prefix icu4c 2>/dev/null || true)
    [ -f "$icu_prefix/lib/pkgconfig/icu-i18n.pc" ] ||
        fail "Homebrew ICU pkg-config metadata was not found"
    dependency_pkg_path=$icu_prefix/lib/pkgconfig${dependency_pkg_path:+:$dependency_pkg_path}
fi
if command -v brew >/dev/null 2>&1; then
    boost_prefix=$(brew --prefix boost 2>/dev/null || true)
    if [ -f "$boost_prefix/include/boost/algorithm/string.hpp" ]; then
        dependency_cppflags="-I$boost_prefix/include${dependency_cppflags:+ $dependency_cppflags}"
    fi
fi

if [ -e "$output_dir" ] && [ -n "$(find "$output_dir" -mindepth 1 -print -quit 2>/dev/null)" ]; then
    fail "refusing to overwrite nonempty output directory: $output_dir"
fi
mkdir -p "$output_dir"

# Public source tree: the pinned libcdr fork is part of this repository.
libcdr_record=$source_root/third_party/VACARDS-LIBCDR-SOURCE.env
[ -f "$libcdr_record" ] || fail "missing $libcdr_record"
record_commit=$(awk -F= '$1 == "source_commit" {print $2; exit}' "$libcdr_record")
[ "$record_commit" = "$commit" ] ||
    fail "third_party/libcdr-vacards holds $record_commit; VACARDS-DEPENDENCIES.env pins $commit"
(cd "$source_root/third_party/libcdr-vacards" && shasum -a 256 -c --status ../libcdr-vacards.sha256) ||
    fail "third_party/libcdr-vacards differs from third_party/libcdr-vacards.sha256"
mkdir -p "$source_dir"
cp -R "$source_root/third_party/libcdr-vacards/." "$source_dir/"

(cd "$source_dir" && ./autogen.sh)
mkdir -p "$build_dir"
(cd "$build_dir" && PKG_CONFIG_PATH="$dependency_pkg_path" CPPFLAGS="$dependency_cppflags" \
    MACOSX_DEPLOYMENT_TARGET="$deployment_target" \
    "$source_dir/configure" \
    --prefix="$prefix" --enable-tests --without-docs)
make -C "$build_dir" -j "$jobs"
make -C "$build_dir" check
make -C "$build_dir" install

pkg_path=$prefix/lib/pkgconfig
actual_version=$(PKG_CONFIG_PATH="$pkg_path${dependency_pkg_path:+:$dependency_pkg_path}" \
    pkg-config --modversion libcdr-0.1)
[ "$actual_version" = "$expected_version" ] ||
    fail "installed libcdr is $actual_version; expected $expected_version"

library=$(find "$prefix/lib" -maxdepth 1 -type f \
    \( -name 'libcdr-0.1*.dylib' -o -name 'libcdr-0.1.so.*' \) -print | head -n 1)
[ -n "$library" ] || fail "installed shared libcdr library was not found"
library_relative=${library#"$prefix"/}
library_sha256=$(shasum -a 256 "$library" | awk '{print $1}')
created_utc=$(date -u '+%Y-%m-%dT%H:%M:%SZ')

{
    printf 'format=1\n'
    printf 'dependency=libcdr\n'
    printf 'source_repository=%s\n' "$repository"
    printf 'source_ref=%s\n' "$ref"
    printf 'source_commit=%s\n' "$commit"
    printf 'pkgconfig_version=%s\n' "$actual_version"
    printf 'library_relative_path=%s\n' "$library_relative"
    printf 'library_sha256=%s\n' "$library_sha256"
    printf 'macos_deployment_target=%s\n' "$deployment_target"
    printf 'created_utc=%s\n' "$created_utc"
} > "$prefix/VACARDS-LIBCDR.env"

"$script_dir/verify-vacards-libcdr.sh" "$prefix"
echo "Pinned VACards libcdr installed in $prefix"
