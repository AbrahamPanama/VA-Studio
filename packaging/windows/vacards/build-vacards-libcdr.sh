#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Build the pinned VACards libcdr for Windows (MSYS2 UCRT64), the Windows
# counterpart of packaging/macos/vacards/build-vacards-libcdr.sh.
#
# Usage: build-vacards-libcdr.sh OUTPUT_DIR [REPOSITORY]
#   OUTPUT_DIR must not exist or be empty; it receives source/, build/,
#   install/ and libcdr-dll-imports.txt. Reads the repository, ref, commit,
#   pkg-config version and macOS deployment target from VACARDS-DEPENDENCIES.env.
#   The source is the in-tree fork third_party/libcdr-vacards, verified against
#   third_party/libcdr-vacards.sha256; REPOSITORY is accepted and ignored.
#   Environment: VACARDS_LIBREVENGE_PREFIX (required: the patched librevenge prefix),
#   VACARDS_DEPENDENCY_JOBS (default 2).
#
# Writes install/VACARDS-LIBCDR.env (the shared closed schema),
# install/VACARDS-LIBCDR-TESTS.env and install/VACARDS-LIBCDR-TESTS.log
# (make check must pass).
set -euo pipefail

fail() { echo "VACards libcdr build failed: $*" >&2; exit 1; }
[ "$#" -ge 1 ] && [ "$#" -le 2 ] || { echo "usage: $0 OUTPUT_DIR [REPOSITORY]" >&2; exit 2; }
output_dir=$1
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
dependency_tool=$script_dir/../../macos/vacards/vacards-dependencies.sh
repository=$($dependency_tool --get libcdr_repository)
ref=$($dependency_tool --get libcdr_ref)
commit=$($dependency_tool --get libcdr_commit)
expected_version=$($dependency_tool --get libcdr_pkgconfig_version)
deployment_target=$($dependency_tool --get macos_deployment_target)
clone_repository=${2:-$repository}
librevenge_prefix=${VACARDS_LIBREVENGE_PREFIX:?set VACARDS_LIBREVENGE_PREFIX to the patched librevenge install prefix}
jobs=${VACARDS_DEPENDENCY_JOBS:-2}

[[ "$commit" =~ ^[0-9a-f]{40}$ ]] || fail "commit must be a full lowercase Git object id"
case "$jobs" in
    ''|0|*[!0-9]*) fail "VACARDS_DEPENDENCY_JOBS must be a positive integer" ;;
esac
for tool in git make pkg-config sha256sum objdump; do
    command -v "$tool" >/dev/null 2>&1 || fail "required tool not found: $tool"
done
if [ -e "$output_dir" ] && [ -n "$(find "$output_dir" -mindepth 1 -print -quit 2>/dev/null)" ]; then
    fail "refusing to overwrite nonempty output directory: $output_dir"
fi
mkdir -p "$output_dir"
source_dir=$output_dir/source
build_dir=$output_dir/build
prefix=$output_dir/install

# Public source tree: the pinned libcdr fork is part of this repository.
source_root=$(cd -- "$script_dir/../../.." && pwd)
libcdr_record=$source_root/third_party/VACARDS-LIBCDR-SOURCE.env
[ -f "$libcdr_record" ] || fail "missing $libcdr_record"
record_commit=$(awk -F= '$1 == "source_commit" {print $2; exit}' "$libcdr_record")
[ "$record_commit" = "$commit" ] || fail "third_party/libcdr-vacards holds $record_commit; VACARDS-DEPENDENCIES.env pins $commit"
(cd "$source_root/third_party/libcdr-vacards" && sha256sum -c --status ../libcdr-vacards.sha256) ||
    fail "third_party/libcdr-vacards differs from third_party/libcdr-vacards.sha256"
mkdir -p "$source_dir"
cp -R "$source_root/third_party/libcdr-vacards/." "$source_dir/"

(cd "$source_dir" && ./autogen.sh)
mkdir -p "$build_dir"
pkg_path=$librevenge_prefix/lib/pkgconfig:/ucrt64/lib/pkgconfig:/ucrt64/share/pkgconfig
(cd "$build_dir" && PKG_CONFIG_PATH=$pkg_path "$source_dir/configure" \
    --enable-shared --disable-static --enable-tests --without-docs --prefix="$prefix")
make -C "$build_dir" -j "$jobs"
# The tests load the fresh DLL and the pinned librevenge.
PATH=$build_dir/src/lib/.libs:$librevenge_prefix/bin:$PATH make -C "$build_dir" check
make -C "$build_dir" install

actual_version=$(PKG_CONFIG_PATH=$prefix/lib/pkgconfig:$pkg_path pkg-config --modversion libcdr-0.1)
[ "$actual_version" = "$expected_version" ] || fail "installed libcdr is $actual_version; expected $expected_version"
library=$prefix/bin/libcdr-0.1.dll
[ -f "$library" ] || fail "installed libcdr DLL was not found"
library_sha256=$(sha256sum "$library" | awk '{print $1}')
created_utc=$(date -u '+%Y-%m-%dT%H:%M:%SZ')
{
    printf 'format=1\n'
    printf 'dependency=libcdr\n'
    printf 'source_repository=%s\n' "$repository"
    printf 'source_ref=%s\n' "$ref"
    printf 'source_commit=%s\n' "$commit"
    printf 'pkgconfig_version=%s\n' "$actual_version"
    printf 'library_relative_path=bin/libcdr-0.1.dll\n'
    printf 'library_sha256=%s\n' "$library_sha256"
    printf 'macos_deployment_target=%s\n' "$deployment_target"
    printf 'created_utc=%s\n' "$created_utc"
} > "$prefix/VACARDS-LIBCDR.env"

# Windows test evidence, bound to the same source and DLL.
test_log=$build_dir/src/test/test.log
[ -f "$test_log" ] || fail "libcdr test log not found"
grep -q '^OK ([1-9][0-9]*)$' "$test_log" || fail "libcdr tests did not report OK"
grep -q '^PASS test.exe (exit status: 0)$' "$test_log" || fail "libcdr test.exe did not pass"
! grep -Eq '\b(FAIL|ERROR|SKIP)\b' "$test_log" || fail "libcdr test log reports a failure"
cp "$test_log" "$prefix/VACARDS-LIBCDR-TESTS.log"
native_cases=$(sed -n 's/^OK (\([0-9]*\))$/\1/p' "$test_log" | tail -1)
test_log_sha256=$(sha256sum "$prefix/VACARDS-LIBCDR-TESTS.log" | awk '{print $1}')
[ -n "$test_log_sha256" ] || fail "libcdr test log checksum is empty"
{
    printf 'format=1\n'
    printf 'platform=windows\n'
    printf 'source_commit=%s\n' "$commit"
    printf 'library_sha256=%s\n' "$library_sha256"
    printf 'tests=make-check-passed\n'
    printf 'test_log_sha256=%s\n' "$test_log_sha256"
    printf 'native_cases=%s\n' "$native_cases"
} > "$prefix/VACARDS-LIBCDR-TESTS.env"
objdump -p "$library" > "$output_dir/libcdr-dll-imports.txt"
"$script_dir/../../macos/vacards/verify-vacards-libcdr.sh" "$prefix"
echo "Pinned VACards libcdr installed in $prefix ($native_cases native tests passed)"
