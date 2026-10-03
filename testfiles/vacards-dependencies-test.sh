#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later

set -eu

if [ "$#" -ne 4 ]; then
    echo "usage: $0 DEPENDENCY_TOOL LIBCDR_VERIFIER CAIRO_VERIFIER DEPENDENCY_MANIFEST" >&2
    exit 2
fi

dependency_tool=$1
libcdr_verifier=$2
cairo_verifier=$3
source_manifest=$4
test_root=$(mktemp -d "${TMPDIR:-/tmp}/vacards-dependencies-test.XXXXXX")
trap 'rm -rf "$test_root"' EXIT HUP INT TERM
test_root=$(CDPATH= cd -- "$test_root" && pwd)

assert_equal()
{
    expected=$1
    actual=$2
    [ "$actual" = "$expected" ] || {
        echo "expected '$expected', got '$actual'" >&2
        exit 1
    }
}

tool=$dependency_tool
manifest=$source_manifest
project_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
expected_abi=$(awk '$1 == "#define" && $2 == "VAC_NESTING_API_VERSION" {sub(/u$/, "", $3); print $3}' \
    "$project_root/src/3rdparty/vacards-nesting-rs/include/vacards_nesting.h")
assert_equal "$manifest" "$("$tool" --file "$manifest" --validate)"
assert_equal c01177fb08abb3f005db289370929a6a4164fcad \
    "$("$tool" --file "$manifest" --get source_baseline_commit)"
assert_equal 297ad1ad0b8e1772633abd7553649714b173f7e4 \
    "$("$tool" --file "$manifest" --get libcdr_commit)"
assert_equal 0.1.10 "$("$tool" --file "$manifest" --get libcdr_pkgconfig_version)"
assert_equal https://github.com/JeroenGar/jagua-rs.git \
    "$("$tool" --file "$manifest" --get nesting_engine_repository)"
assert_equal 0.8.0 "$("$tool" --file "$manifest" --get nesting_engine_version)"
assert_equal 9a19409bd38f3643c3d6d2d7571cddfba548ee17 \
    "$("$tool" --file "$manifest" --get nesting_engine_commit)"
assert_equal 1.88.0 "$("$tool" --file "$manifest" --get nesting_rust_toolchain)"
assert_equal "$expected_abi" "$("$tool" --file "$manifest" --get nesting_ffi_api_version)"
assert_equal 57c45cd295f5d2ce2a11edf6e765318a51d2b41e \
    "$("$tool" --file "$manifest" --get sparrow_commit)"
assert_equal 16d509b1390fb0a6fcbbfa9e230e9f60a78239ba5f58929571213795a76ab384 \
    "$("$tool" --file "$manifest" --get sparrow_darwin_arm64_sha256)"
assert_equal "$("$tool" --file "$manifest" --get sparrow_darwin_arm64_sha256)" \
    "$(shasum -a 256 "$project_root/src/3rdparty/sparrow/bin/darwin-arm64/sparrow" | awk '{print $1}')"
assert_equal 26.0 "$("$tool" --file "$manifest" --get macos_deployment_target)"
assert_equal 1.90.0 "$("$tool" --file "$manifest" --get sparrow_windows_rust_toolchain)"
assert_equal "$("$tool" --file "$manifest" --get sparrow_windows_x64_sha256)" \
    "$(shasum -a 256 "$project_root/src/3rdparty/sparrow/bin/windows-x64/sparrow.exe" | awk '{print $1}')"
assert_equal "$("$tool" --file "$manifest" --get sparrow_windows_cargo_lock_sha256)" \
    "$(shasum -a 256 "$project_root/src/3rdparty/sparrow/windows-x64.Cargo.lock" | awk '{print $1}')"
for field in sparrow_windows_x64_sha256 sparrow_windows_cargo_lock_sha256 sparrow_windows_rust_toolchain; do
    sed "s/^$field=.*/$field=invalid/" "$manifest" > "$test_root/invalid.env"
    if "$tool" --file "$test_root/invalid.env" --validate >/dev/null 2>&1; then
        echo "manifest accepted invalid $field" >&2
        exit 1
    fi
done
assert_equal TheBest.icc "$("$tool" --file "$manifest" --get tiff_rgb_profile_filename)"
assert_equal f3ae51fbbeb717b46b20a6f7eac769ae5c4c2b4fa1e2fc6816bb22283772b8e8 \
    "$("$tool" --file "$manifest" --get tiff_rgb_profile_sha256)"
assert_equal WITH_LIBCDR,WITH_LIBVISIO,WITH_LIBWPG,WITH_POPPLER,WITH_CAPYPDF,WITH_LIBSPELLING,WITH_GSOURCEVIEW,ENABLE_LCMS,ENABLE_POPPLER_CAIRO,WITH_NLS,WITH_VACARDS_NESTING,VACARDS_REQUIRE_PATCHED_LIBREVENGE \
    "$("$tool" --file "$manifest" --get required_cmake_features)"
assert_equal WITH_IMAGE_MAGICK,WITH_GRAPHICS_MAGICK,WITH_GNU_READLINE \
    "$("$tool" --file "$manifest" --get disabled_cmake_features)"
printf '%s\n' "$("$tool" --file "$manifest" --sha256)" | grep -Eq '^[0-9a-f]{64}$'

cp "$manifest" "$test_root/invalid.env"
printf 'unexpected=value\n' >> "$test_root/invalid.env"
if "$tool" --file "$test_root/invalid.env" --validate >/dev/null 2>&1; then
    echo "manifest with an unknown field was accepted" >&2
    exit 1
fi

sed 's/^disabled_cmake_features=.*/disabled_cmake_features=WITH_LIBCDR/' \
    "$manifest" > "$test_root/invalid.env"
if "$tool" --file "$test_root/invalid.env" --validate >/dev/null 2>&1; then
    echo "manifest accepted the same feature as required and disabled" >&2
    exit 1
fi

sed 's/^libcdr_commit=.*/libcdr_commit=short/' "$manifest" > "$test_root/invalid.env"
if "$tool" --file "$test_root/invalid.env" --validate >/dev/null 2>&1; then
    echo "manifest with a short commit was accepted" >&2
    exit 1
fi

sed 's/^source_baseline_commit=.*/source_baseline_commit=short/' "$manifest" > "$test_root/invalid.env"
if "$tool" --file "$test_root/invalid.env" --validate >/dev/null 2>&1; then
    echo "manifest with a short source baseline was accepted" >&2
    exit 1
fi

sed 's/^nesting_engine_commit=.*/nesting_engine_commit=short/' "$manifest" > "$test_root/invalid.env"
if "$tool" --file "$test_root/invalid.env" --validate >/dev/null 2>&1; then
    echo "manifest with a short nesting engine commit was accepted" >&2
    exit 1
fi

sed 's/^nesting_rust_toolchain=.*/nesting_rust_toolchain=stable/' "$manifest" > "$test_root/invalid.env"
if "$tool" --file "$test_root/invalid.env" --validate >/dev/null 2>&1; then
    echo "manifest with an unpinned Rust toolchain was accepted" >&2
    exit 1
fi

sed 's/^nesting_ffi_api_version=.*/nesting_ffi_api_version=0/' "$manifest" > "$test_root/invalid.env"
if "$tool" --file "$test_root/invalid.env" --validate >/dev/null 2>&1; then
    echo "manifest with an invalid nesting FFI version was accepted" >&2
    exit 1
fi

sed 's/^tiff_rgb_profile_sha256=.*/tiff_rgb_profile_sha256=short/' \
    "$manifest" > "$test_root/invalid.env"
if "$tool" --file "$test_root/invalid.env" --validate >/dev/null 2>&1; then
    echo "manifest with a short TIFF profile checksum was accepted" >&2
    exit 1
fi

prefix=$test_root/libcdr-prefix
mkdir -p "$prefix/lib/pkgconfig"
printf 'int vacards_test_libcdr(void) { return 1; }\n' > "$test_root/libcdr-fixture.c"
if [ "$(uname -s)" = "Darwin" ]; then
    cc -dynamiclib -mmacosx-version-min="$("$tool" --file "$manifest" --get macos_deployment_target)" \
        "$test_root/libcdr-fixture.c" -o "$prefix/lib/libcdr-0.1.1.dylib"
else
    cc -shared -fPIC "$test_root/libcdr-fixture.c" -o "$prefix/lib/libcdr-0.1.so.1"
fi
libcdr_library=$(find "$prefix/lib" -maxdepth 1 -type f -name 'libcdr-0.1*' -print | head -n 1)
library_sha=$(shasum -a 256 "$libcdr_library" | awk '{print $1}')
cat > "$prefix/lib/pkgconfig/libcdr-0.1.pc" <<EOF
prefix=$prefix
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: libcdr
Description: VACards dependency verifier fixture
Version: 0.1.10
Libs: -L\${libdir} -lcdr-0.1
Cflags: -I\${includedir}
EOF
cat > "$prefix/VACARDS-LIBCDR.env" <<EOF
format=1
dependency=libcdr
source_repository=$("$tool" --file "$manifest" --get libcdr_repository)
source_ref=$("$tool" --file "$manifest" --get libcdr_ref)
source_commit=$("$tool" --file "$manifest" --get libcdr_commit)
pkgconfig_version=$("$tool" --file "$manifest" --get libcdr_pkgconfig_version)
library_relative_path=${libcdr_library#"$prefix"/}
library_sha256=$library_sha
macos_deployment_target=$("$tool" --file "$manifest" --get macos_deployment_target)
created_utc=2026-09-01T00:00:00Z
EOF

"$libcdr_verifier" "$prefix" >/dev/null
printf 'tampered\n' >> "$libcdr_library"
if "$libcdr_verifier" "$prefix" >/dev/null 2>&1; then
    echo "tampered libcdr library was accepted" >&2
    exit 1
fi

cairo_prefix=$test_root/cairo-prefix
mkdir -p "$cairo_prefix/lib/pkgconfig"
printf 'int vacards_test_cairo(void) { return 1; }\n' > "$test_root/cairo-fixture.c"
if [ "$(uname -s)" = "Darwin" ]; then
    cc -dynamiclib -mmacosx-version-min="$("$tool" --file "$manifest" --get macos_deployment_target)" \
        "$test_root/cairo-fixture.c" -o "$cairo_prefix/lib/libcairo.2.dylib"
else
    cc -shared -fPIC "$test_root/cairo-fixture.c" -o "$cairo_prefix/lib/libcairo.2.dylib"
fi
cairo_library_sha=$(shasum -a 256 "$cairo_prefix/lib/libcairo.2.dylib" | awk '{print $1}')
cat > "$cairo_prefix/lib/pkgconfig/cairo.pc" <<EOF
prefix=$cairo_prefix
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: cairo
Description: VACards dependency verifier fixture
Version: $("$tool" --file "$manifest" --get cairo_release)
Libs: -L\${libdir} -lcairo
Cflags: -I\${includedir}
EOF
cat > "$cairo_prefix/VACARDS-CAIRO.txt" <<EOF
Cairo release: $("$tool" --file "$manifest" --get cairo_release)
Release SHA-256: $("$tool" --file "$manifest" --get cairo_release_sha256)
Upstream fix: $("$tool" --file "$manifest" --get cairo_fix_commit)
Patch: packaging/macos/vacards/cairo-1.18.4-clip-all.patch
Library SHA-256: $cairo_library_sha
macOS deployment target: $("$tool" --file "$manifest" --get macos_deployment_target)
EOF

"$cairo_verifier" "$cairo_prefix" >/dev/null
printf 'tampered\n' >> "$cairo_prefix/lib/libcairo.2.dylib"
if "$cairo_verifier" "$cairo_prefix" >/dev/null 2>&1; then
    echo "tampered Cairo library was accepted" >&2
    exit 1
fi

echo "VACards dependency provenance contract passed"
