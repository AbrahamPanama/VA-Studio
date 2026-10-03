#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Internal experimental build, not a release gate or installer publisher.
set -euo pipefail
if [[ $# -lt 4 || $# -gt 5 ]]; then
    echo "Usage: $0 BUILD_DIR STAGE_DIR CUSTOM_LIBCDR_PREFIX PATCHED_CAIRO_PREFIX [configure|build]" >&2
    exit 2
fi
[[ ${MSYSTEM:-} == UCRT64 ]] || { echo 'Run in MSYS2 UCRT64.' >&2; exit 1; }
script_dir=$(cd -- "$(dirname -- "$0")" && pwd)
source_dir=$(cd -- "$script_dir/../.." && pwd)
build_dir=$(cygpath -am "$1")
stage_dir=$(cygpath -am "$2")
cdr_prefix=$(cygpath -am "$3")
cairo_prefix=$(cygpath -am "$4")
mode=${5:-build}
[[ $mode == configure || $mode == build ]] || exit 2
[[ $build_dir != "$stage_dir" && $build_dir != "$(cygpath -am "$source_dir")" ]] || exit 2
# Optional patched librevenge prefix. When the dependency manifest requires the
# patched CDR-fidelity extension, configure must never silently fall back to the
# stock /ucrt64 prefix.
required_features=$(sed -n 's/^required_cmake_features=//p' "$source_dir/VACARDS-DEPENDENCIES.env")
lr_prefix_raw=${VACARDS_PATCHED_LIBREVENGE_PREFIX:-}
lr_prefix=
lr_unix=
if [[ -n $lr_prefix_raw ]]; then
    lr_prefix=$(cygpath -am "$lr_prefix_raw")
    lr_unix=$(cygpath -u "$lr_prefix")
    [[ -f $lr_prefix/lib/pkgconfig/librevenge-0.0.pc ]] || {
        echo 'Missing patched librevenge prefix.' >&2; exit 1;
    }
elif [[ ,$required_features, == *,VACARDS_REQUIRE_PATCHED_LIBREVENGE,* ]]; then
    echo 'VACARDS-DEPENDENCIES.env requires VACARDS_REQUIRE_PATCHED_LIBREVENGE but VACARDS_PATCHED_LIBREVENGE_PREFIX is unset.' >&2
    echo 'Refusing to configure against stock librevenge; set VACARDS_PATCHED_LIBREVENGE_PREFIX to the patched install prefix.' >&2
    exit 1
fi
[[ -f $cdr_prefix/lib/pkgconfig/libcdr-0.1.pc ]] || { echo 'Missing custom libcdr prefix.' >&2; exit 1; }
rust_version=$(sed -n 's/^nesting_rust_toolchain=//p' "$source_dir/VACARDS-DEPENDENCIES.env")
rust_bin=${VACARDS_DEV_RUST_BIN:-$(cygpath -u "$USERPROFILE")/.rustup/toolchains/$rust_version-x86_64-pc-windows-gnu/bin}
[[ -x $rust_bin/rustc.exe && -x $rust_bin/cargo.exe ]] || { echo "Missing pinned GNU Rust: $rust_bin" >&2; exit 1; }
# UCRT64's compiler runtime DLLs must precede the older copies shipped by Rust.
export PATH="${lr_unix:+$lr_unix/bin:}$(cygpath -u "$cairo_prefix")/bin:$(cygpath -u "$cdr_prefix")/bin:/ucrt64/bin:$rust_bin:/usr/bin:$PATH"
jobs=${VACARDS_DEV_JOBS:-2}
[[ $jobs =~ ^[1-8]$ ]] || { echo 'Use 1–8 build jobs.' >&2; exit 2; }
export CARGO_BUILD_JOBS="$jobs"
export PKG_CONFIG_PATH="${lr_unix:+$lr_unix/lib/pkgconfig:}$(cygpath -u "$cairo_prefix")/lib/pkgconfig:$(cygpath -u "$cdr_prefix")/lib/pkgconfig:/ucrt64/lib/pkgconfig:/ucrt64/share/pkgconfig"
[[ $(gcc -dumpmachine) == x86_64-w64-mingw32 ]] || exit 1
[[ $(cygpath -am "$(pkg-config --variable=prefix libcdr-0.1)") == "$cdr_prefix" ]] || {
    echo 'pkg-config resolved a different libcdr prefix.' >&2; exit 1;
}
bash "$script_dir/vacards/verify-vacards-cairo-prefix.sh" "$cairo_prefix"
if [[ -n $lr_prefix ]]; then
    [[ $(cygpath -am "$(pkg-config --variable=prefix librevenge-0.0)") == "$lr_prefix" ]] || {
        echo 'pkg-config resolved a different librevenge prefix.' >&2; exit 1;
    }
    bash "$script_dir/vacards/verify-vacards-librevenge.sh" "$lr_prefix"
fi
mkdir -p "$build_dir" "$stage_dir"
if [[ -f $build_dir/CMakeCache.txt ]]; then
    configured_source=$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$build_dir/CMakeCache.txt")
    [[ $configured_source == "$(cygpath -am "$source_dir")" ]] || {
        echo 'Refusing to reuse another source tree build.' >&2; exit 1;
    }
fi
args=(
    -S "$(cygpath -am "$source_dir")" -B "$build_dir" -G Ninja
    -DCMAKE_BUILD_TYPE=RelWithDebInfo
    -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=ON -DTESTS_WITH_ASAN=OFF
    -DWITH_INTERNAL_2GEOM=ON -DWITH_INTERNAL_CAPYPDF=ON
    -DWITH_INTERNAL_DEPIXELIZE=ON
    -DWITH_INTERNAL_AUTOTRACE=ON -DWITH_INTERNAL_ADAPTAGRAMS=ON
    -DENABLE_NLS=ON
    -DVACARDS_REQUIRE_MODERN_CDR=ON -DVACARDS_REQUIRE_PATCHED_CAIRO=ON
    -DVACARDS_NESTING_BUILD_PHASE0_TESTS=ON -DVACARDS_NESTING_BUILD_PHASE2_TESTS=ON
    -DVACARDS_NESTING_RUST_TARGET=x86_64-pc-windows-gnu
    "-DVACARDS_LIBCDR_PREFIX=$cdr_prefix" "-DCMAKE_PREFIX_PATH=${lr_prefix:+$lr_prefix;}$cairo_prefix;$cdr_prefix"
    "-DCMAKE_INSTALL_PREFIX=$stage_dir"
    "-DVACARDS_NESTING_CARGO_EXECUTABLE=$(cygpath -am "$rust_bin/cargo.exe")"
    "-DVACARDS_NESTING_RUSTC_EXECUTABLE=$(cygpath -am "$rust_bin/rustc.exe")"
    "-DVACARDS_NESTING_RUSTDOC_EXECUTABLE=$(cygpath -am "$rust_bin/rustdoc.exe")"
)
IFS=, read -ra required <<< "$required_features"
if [[ -n ${VACARDS_DEV_PYTHON:-} ]]; then
    [[ -x $VACARDS_DEV_PYTHON ]] || { echo 'Missing selected Python interpreter.' >&2; exit 1; }
    args+=("-DPython3_EXECUTABLE=$(cygpath -am "$VACARDS_DEV_PYTHON")")
fi
IFS=, read -ra disabled <<< "$(sed -n 's/^disabled_cmake_features=//p' "$source_dir/VACARDS-DEPENDENCIES.env")"
for feature in "${required[@]}"; do args+=("-D$feature=ON"); done
for feature in "${disabled[@]}"; do args+=("-D$feature=OFF"); done
printf 'Developer configure:'
printf ' %q' cmake "${args[@]}"
printf '\n'
# Upstream package-list helpers create relative temporary files. Keep those in
# the build tree, where their cleanup expects them, never in the source tree.
(cd -- "$build_dir" && cmake "${args[@]}")
if [[ $mode == build ]]; then
    cmake --build "$build_dir" --parallel "$jobs"
fi
