#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Build a candidate helper; never replace the app's pinned binary automatically.
set -euo pipefail
[[ $# == 1 && ${MSYSTEM:-} == UCRT64 ]] || {
    echo 'Usage (MSYS2 UCRT64): build-sparrow.sh NEW_OUTPUT_DIR' >&2; exit 2;
}
script_dir=$(cd -- "$(dirname -- "$0")" && pwd)
source_root=$(cd -- "$script_dir/../../.." && pwd)
dependency_tool=$source_root/packaging/vacards/vacards-dependencies.sh
pin=$(bash "$dependency_tool" --get sparrow_commit)
repository=$(bash "$dependency_tool" --get sparrow_repository)
toolchain=$(bash "$dependency_tool" --get sparrow_windows_rust_toolchain)
lock_sha=$(bash "$dependency_tool" --get sparrow_windows_cargo_lock_sha256)
lock=$source_root/src/3rdparty/sparrow/windows-x64.Cargo.lock
[[ $(sha256sum "$lock" | cut -d ' ' -f 1) == "$lock_sha" ]] || {
    echo 'Sparrow Cargo.lock does not match the dependency manifest.' >&2; exit 1;
}
output=$(cygpath -am "$1")
[[ ! -e $output ]] || { echo 'Use a fresh output directory; retain old attempts.' >&2; exit 1; }
rust_bin=${VACARDS_SPARROW_RUST_BIN:-$(cygpath -u "$USERPROFILE")/.rustup/toolchains/$toolchain-x86_64-pc-windows-gnu/bin}
[[ -x $rust_bin/rustc.exe && -x $rust_bin/cargo.exe ]] || {
    echo "Install the separate pinned helper toolchain: $toolchain-x86_64-pc-windows-gnu" >&2; exit 1;
}
export PATH="/ucrt64/bin:$rust_bin:/usr/bin:$PATH"
export RUSTC=$(cygpath -am "$rust_bin/rustc.exe")
export RUSTDOC=$(cygpath -am "$rust_bin/rustdoc.exe")
[[ $("$RUSTC" --version | cut -d ' ' -f 2) == "$toolchain" ]] || exit 1
[[ $(gcc -dumpmachine) == x86_64-w64-mingw32 ]] || exit 1
export CARGO_BUILD_JOBS=2
export CARGO_TARGET_DIR=$output/target
export RUSTFLAGS=-Clink-arg=-Wl,--no-insert-timestamp
mkdir -p "$output/source" "$output/install"
git -C "$output/source" init
git -C "$output/source" fetch --depth=1 "$repository" "$pin"
git -C "$output/source" checkout --detach FETCH_HEAD
[[ $(git -C "$output/source" rev-parse HEAD) == "$pin" ]] || exit 1
cp "$lock" "$output/source/Cargo.lock"
(
    cd -- "$output/source"
    "$rust_bin/cargo.exe" build --locked --release --features only_final_svg \
        --bin sparrow --target x86_64-pc-windows-gnu --jobs 2
) > "$output/build.log" 2>&1
cp "$output/target/x86_64-pc-windows-gnu/release/sparrow.exe" "$output/install/sparrow.exe"
cp "$output/source/LICENSE" "$output/install/LICENSE"
{
    printf 'source_commit=%s\n' "$pin"
    printf 'cargo_lock_sha256=%s\n' "$lock_sha"
    printf 'rust_toolchain=%s\ntarget=x86_64-pc-windows-gnu\nfeatures=only_final_svg\n' "$toolchain"
    printf 'rustflags=%s\n' "$RUSTFLAGS"
    printf 'compiler=%s\n' "$(gcc -dumpfullversion)"
    printf 'executable_sha256=%s\n' "$(sha256sum "$output/install/sparrow.exe" | cut -d ' ' -f 1)"
} > "$output/SPARROW-BUILD.env"
objdump -p "$output/install/sparrow.exe" > "$output/pe-imports.txt"
cat "$output/SPARROW-BUILD.env"
echo 'Candidate only. Review imports and run the Windows app acceptance corpus before updating the pinned artifact.'
