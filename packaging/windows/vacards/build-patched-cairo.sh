#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Isolated, pinned UCRT64 dependency build. This does not install packages or
# overwrite /ucrt64. Retain OUTPUT (including failed builds) as evidence.
set -euo pipefail
export LC_ALL=C

fail() { echo "VACards Windows Cairo build failed: $*" >&2; exit 1; }
[[ $# == 1 || ( $# == 3 && $2 == --archive ) ]] || {
    echo "usage: $0 EMPTY_OUTPUT_DIRECTORY [--archive PINNED_ARCHIVE]" >&2
    exit 2
}
output_arg=$1
archive_input=${3:-}
[[ ${MSYSTEM:-} == UCRT64 && $(uname -m) == x86_64 ]] || fail "requires an x86_64 MSYS2 UCRT64 shell"
case $(uname -s) in MINGW*_NT-*) ;; *) fail "requires a native Windows UCRT64 host" ;; esac
for tool in cygpath bash curl shasum tar patch meson ninja gcc g++ objdump awk find sort grep cmp tee; do
    command -v "$tool" >/dev/null || fail "missing tool: $tool"
done
pkg_config=${PKG_CONFIG:-pkg-config}
command -v "$pkg_config" >/dev/null || fail "missing pkg-config: $pkg_config"
meson_runner=(meson)
if [[ -n ${MESON_PYTHON:-} ]]; then
    [[ -x $MESON_PYTHON ]] || fail "MESON_PYTHON must name an installed executable"
    meson_runner=("$MESON_PYTHON" -m mesonbuild.mesonmain)
fi
normalize_dir() { (cd -- "$(cygpath -u "$1")" && pwd -P); }
sha256() { shasum -a 256 "$1" | awk '{print $1}'; }
script_dir=$(cd -- "$(dirname -- "$0")" && pwd -P)
source_root=$(cd -- "$script_dir/../../.." && pwd -P)
dependency_tool=$source_root/packaging/macos/vacards/vacards-dependencies.sh
common_patch=$source_root/packaging/macos/vacards/cairo-1.18.4-clip-all.patch
clip_test=$source_root/testfiles/vacards-windows-cairo-test.c
version=$(bash "$dependency_tool" --get cairo_release)
archive_sha=$(bash "$dependency_tool" --get cairo_release_sha256)
fix=$(bash "$dependency_tool" --get cairo_fix_commit)
[[ $version == 1.18.4 ]] || fail "the current common patch/recipe supports only pinned 1.18.4"
grep -Fxq "Upstream: https://gitlab.freedesktop.org/cairo/cairo/-/commit/$fix" "$common_patch" || fail "common patch does not identify the pinned fix"
ucrt_prefix=$(normalize_dir /ucrt64)
gcc=$(command -v gcc); gxx=$(command -v g++)
for compiler in "$gcc" "$gxx"; do
    [[ $(normalize_dir "$(dirname -- "$compiler")") == "$ucrt_prefix/bin" ]] || fail "compiler is outside /ucrt64/bin: $compiler"
    [[ $("$compiler" -dumpmachine) == x86_64-w64-mingw32 ]] || fail "wrong compiler target: $compiler"
done
[[ -z ${DESTDIR:-} ]] || fail "DESTDIR would redirect installation"
output=$(cygpath -u "$output_arg")
[[ -n $output && $output == /* ]] || fail "use an absolute output directory"
if [[ -e $output ]]; then
    [[ -d $output && ! -L $output ]] || fail "output is not a regular directory"
    [[ -z $(find "$output" -mindepth 1 -maxdepth 1 -print -quit) ]] || fail "output must be empty; use the verifier for existing builds"
fi
# Resolve the parent before creating anything, including symlink aliases.
parent=$(normalize_dir "$(dirname -- "$output")") || fail "output parent must exist"
output=$parent/$(basename -- "$output")
case $output in "$ucrt_prefix"|"$ucrt_prefix"/*|/usr/*|/bin/*|/lib/*|/etc/*) fail "refusing a system dependency directory" ;; esac
[[ -z $archive_input ]] || archive_input=$(cygpath -u "$archive_input")
mkdir -p -- "$output"
output=$(normalize_dir "$output")
prefix=$output/install
inputs=$prefix/share/vacards-cairo
build_dir=$output/build
source_dir=$output/cairo-$version
mkdir -p "$inputs"
archive=$inputs/source.tar.xz
if [[ -n $archive_input ]]; then
    [[ -f $archive_input ]] || fail "missing supplied archive"
    cp -- "$archive_input" "$archive"
else
    curl --fail --location --proto '=https' --tlsv1.2 \
        --output "$archive" "https://cairographics.org/releases/cairo-$version.tar.xz"
fi
[[ $(sha256 "$archive") == "$archive_sha" ]] || fail "source archive checksum mismatch"
cp -- "$common_patch" "$inputs/clip-all.patch"
cp -- "$clip_test" "$inputs/clip-test.c"
patch_sha=$(sha256 "$inputs/clip-all.patch")
tar -xf "$archive" -C "$output"
patch --batch --forward --fuzz=0 -d "$source_dir" -p1 < "$inputs/clip-all.patch" 2>&1 | tee "$output/patch.log"

# Capture actual tools, executable hashes, versions and resolved package inputs.
# The immutable build-image inventory and full application qualification remain
# separate WP-02/WP-08 requirements.
{
    printf 'msystem=%s\narchitecture=%s\n' "$MSYSTEM" "$(uname -m)"
    for tool in gcc g++ meson ninja pkg-config objdump patch; do
        if [[ $tool == pkg-config ]]; then
            executable=$(command -v "$pkg_config")
        elif [[ $tool == meson && -n ${MESON_PYTHON:-} ]]; then
            executable=$MESON_PYTHON
        else
            executable=$(command -v "$tool")
        fi
        # Native Windows commands may be resolved without their .exe suffix.
        [[ ! -f $executable.exe ]] || executable=$executable.exe
        tool_sha=$(sha256 "$executable")
        printf '%s_path=%s\n%s_sha256=%s\n' "$tool" "$executable" "$tool" "$tool_sha"
        if [[ $tool == meson ]]; then
            printf 'meson_command='; printf '%q ' "${meson_runner[@]}"; printf '\n'
            "${meson_runner[@]}" --version
        else
            "$executable" --version
        fi
    done
    printf 'gcc_target=%s\ng++_target=%s\n' "$("$gcc" -dumpmachine)" "$("$gxx" -dumpmachine)"
    for module in pixman-1 fontconfig freetype2 glib-2.0 gobject-2.0 libpng zlib; do
        module_version=$("$pkg_config" --modversion "$module")
        module_directory=$("$pkg_config" --variable=pcfiledir "$module")
        printf '%s_version=%s\n' "$module" "$module_version"
        printf '%s_pcfiledir=%s\n' "$module" "$module_directory"
    done
    printf 'CFLAGS=%s\nCXXFLAGS=%s\nCPPFLAGS=%s\nLDFLAGS=%s\n' "${CFLAGS:-}" "${CXXFLAGS:-}" "${CPPFLAGS:-}" "${LDFLAGS:-}"
    printf 'PKG_CONFIG_PATH=%s\nPKG_CONFIG_LIBDIR=%s\n' "${PKG_CONFIG_PATH:-}" "${PKG_CONFIG_LIBDIR:-}"
    printf 'recipe=shared release; tests disabled; lzo/xlib/xcb/quartz/symbol-lookup disabled; fontconfig/freetype/glib/png/zlib/dwrite enabled; compile jobs=2\n'
} > "$inputs/toolchain.txt"

# Native Meson paths use Windows form; verifier normalizes both representations.
env CC="$gcc" CXX="$gxx" PKG_CONFIG="$pkg_config" "${meson_runner[@]}" setup "$build_dir" "$source_dir" \
    --prefix "$(cygpath -m "$prefix")" --libdir lib --buildtype release \
    --default-library shared --wrap-mode nodownload \
    -Dtests=disabled -Dlzo=disabled -Dxlib=disabled -Dxcb=disabled \
    -Dquartz=disabled -Dsymbol-lookup=disabled -Dfontconfig=enabled \
    -Dfreetype=enabled -Dglib=enabled -Dpng=enabled -Dzlib=enabled -Ddwrite=enabled \
    2>&1 | tee "$output/configure.log"
"${meson_runner[@]}" compile -C "$build_dir" -j 2 2>&1 | tee "$output/build.log"
"${meson_runner[@]}" install -C "$build_dir" --no-rebuild 2>&1 | tee "$output/install.log"

# Keep the executable adjacent to the DLLs. Windows searches that directory
# first; the test additionally checks GetModuleFileNameW for the loaded Cairo.
"$gcc" -std=c11 -Wall -Wextra -Werror "$inputs/clip-test.c" \
    -I"$prefix/include/cairo" "$prefix/lib/libcairo.dll.a" \
    -o "$prefix/bin/vacards-cairo-clip-test.exe" 2>&1 | tee "$output/clip-test-build.log"
env PATH="$prefix/bin:$ucrt_prefix/bin:$PATH" "$prefix/bin/vacards-cairo-clip-test.exe" \
    2>&1 | tee "$inputs/clip-test.log"
[[ $(sha256 "$archive") == "$archive_sha" && $(sha256 "$inputs/clip-all.patch") == "$patch_sha" ]] || fail "build inputs changed during compilation"
cmp -s "$common_patch" "$inputs/clip-all.patch" || fail "common patch changed during compilation"

inventory=$prefix/VACARDS-CAIRO.sha256
(
    cd -- "$prefix"
    find bin lib include/cairo share/vacards-cairo -type f \
        \( -name 'libcairo*.dll' -o -name 'libcairo*.dll.a' -o -name 'cairo*.pc' \
           -o -name '*.h' -o -path 'share/vacards-cairo/*' \) -print | sort | \
    while IFS= read -r file; do
        printf '%s  %s\n' "$(sha256 "$file")" "$file"
    done
) > "$inventory"
marker=$prefix/VACARDS-CAIRO.env
{
    printf 'format=1\nplatform=windows-ucrt64\narchitecture=x86_64\n'
    printf 'cairo_release=%s\nsource_archive_sha256=%s\nupstream_fix=%s\npatch_sha256=%s\n' "$version" "$archive_sha" "$fix" "$patch_sha"
    printf 'prefix_posix=%s\nprefix_windows=%s\n' "$prefix" "$(cygpath -w "$prefix")"
    printf 'compiler_target=x86_64-w64-mingw32\ninventory_sha256=%s\nclipping_test=passed\n' "$(sha256 "$inventory")"
} > "$marker"
if ! env PKG_CONFIG="$pkg_config" PKG_CONFIG_PATH="$prefix/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}" \
    bash "$script_dir/verify-vacards-cairo-prefix.sh" "$prefix"; then
    mv -- "$marker" "$prefix/VACARDS-CAIRO.failed.env"
    fail "installed prefix did not verify; failed record retained"
fi
echo "Patched UCRT64 Cairo installed in $prefix"
echo "Save VACARDS-CAIRO.env with gate evidence; reverify with --expected-record before packaging."
