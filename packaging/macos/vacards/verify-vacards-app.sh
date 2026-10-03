#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later

set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: $0 APP_BUNDLE" >&2
    exit 2
fi

app_bundle=$(CDPATH= cd -- "$1" && pwd)
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
packaging_provenance=$script_dir/../../vacards/packaging-provenance.py
resource_dir=$(CDPATH= cd -- "$script_dir/../res" && pwd)
macos_dir=$app_bundle/Contents/MacOS
resources=$app_bundle/Contents/Resources
schema_dir=$resources/share/glib-2.0/schemas
attestation=$resources/VACARDS-RELEASE-GATE.env
bundled_version=$resources/VACARDS-VERSION.env
bundled_dependencies=$resources/VACARDS-DEPENDENCIES.env
bundled_libcdr_manifest=$resources/VACARDS-LIBCDR.env
bundled_build_provenance=$resources/VACARDS-BUILD-PROVENANCE.env
bundled_features=$resources/VACARDS-CMAKE-FEATURES.env
executable=$macos_dir/inkscape-bin
sparrow_helper=$macos_dir/vacards-sparrow
launcher=$macos_dir/inkscape
app_icon=$resources/inkscape.icns
expected_icon=$resource_dir/VACards-AppIcon.icns
version_tool=$script_dir/vacards-version.sh
dependency_tool=$script_dir/vacards-dependencies.sh
tiff_profile_filename=$("$dependency_tool" --get tiff_rgb_profile_filename)
tiff_profile_sha256=$("$dependency_tool" --get tiff_rgb_profile_sha256)
tiff_profile=$resources/share/inkscape/color/icc/$tiff_profile_filename

fail()
{
    echo "VACards app verification failed: $*" >&2
    exit 1
}

read_value()
{
    key=$1
    awk -F= -v key="$key" '$1 == key {sub(/^[^=]*=/, ""); print; exit}' "$attestation"
}

# Validate tested-input and transformation evidence before launching any payload.
# This is portable with the app/DMG: the helper does not open recorded host paths.
python3 "$packaging_provenance" verify-bundle "$app_bundle"

for file in "$launcher" "$executable" "$sparrow_helper" "$resources/share/inkscape/sparrow/LICENSE" "$app_icon" "$expected_icon" \
            "$attestation" "$bundled_version" "$bundled_dependencies" \
            "$bundled_libcdr_manifest" "$bundled_build_provenance" \
            "$bundled_features" \
            "$resources/VACARDS-CAIRO.txt" \
            "$tiff_profile" \
            "$schema_dir/gschemas.compiled" \
            "$schema_dir/org.gtk.gtk4.Settings.FileChooser.gschema.xml" \
            "$resources/gdk-pixbuf-loaders.cache.in" \
            "$resources/lib/gdk-pixbuf-2.0/2.10.0/loaders/libpixbufloader_svg.so" \
            "$resources/lib/gdk-pixbuf-2.0/2.10.0/loaders/libpixbufloader-tiff.so"; do
    [ -f "$file" ] || fail "required file is missing: $file"
done
[ -x "$launcher" ] || fail "launcher is not executable"
[ -x "$executable" ] || fail "main binary is not executable"
[ -x "$sparrow_helper" ] || fail "Sparrow helper is not executable"
[ "$(shasum -a 256 "$sparrow_helper" | awk '{print $1}')" = \
  "$("$dependency_tool" --file "$bundled_dependencies" --get sparrow_darwin_arm64_sha256)" ] ||
    fail "Sparrow helper differs from its pinned artifact"
codesign --verify "$sparrow_helper" || fail "Sparrow helper signature is invalid"
cmp -s "$expected_icon" "$app_icon" || fail "bundle does not contain the official VACards app icon"
[ "$(shasum -a 256 "$tiff_profile" | awk '{print $1}')" = "$tiff_profile_sha256" ] ||
    fail "bundled RGB TIFF output profile checksum is wrong"

base_version=$("$version_tool" --file "$bundled_version" --base)
build_number=$("$version_tool" --file "$bundled_version" --build)
release_version=$("$version_tool" --file "$bundled_version" --release)
[ "$(read_value format)" = "3" ] || fail "release attestation is not provenance format 3"
[ "$(read_value base_version)" = "$base_version" ] ||
    fail "attested base version does not match the bundled version"
[ "$(read_value build_number)" = "$build_number" ] ||
    fail "attested build number does not match the bundled version"
[ "$(read_value release_version)" = "$release_version" ] ||
    fail "attested release does not match the bundled version"
[ "$(plutil -extract CFBundleShortVersionString raw "$app_bundle/Contents/Info.plist")" = "$base_version" ] ||
    fail "CFBundleShortVersionString does not match $base_version"
[ "$(plutil -extract CFBundleVersion raw "$app_bundle/Contents/Info.plist")" = "$build_number" ] ||
    fail "CFBundleVersion does not match build $build_number"
[ "$(plutil -extract VACardsReleaseVersion raw "$app_bundle/Contents/Info.plist")" = "$release_version" ] ||
    fail "Info.plist does not identify $release_version"
[ "$(plutil -extract CFBundleIconFile raw "$app_bundle/Contents/Info.plist")" = "inkscape.icns" ] ||
    fail "Info.plist does not reference the bundled VACards app icon"
expected_deployment_target=$("$dependency_tool" --get macos_deployment_target)
[ "$(plutil -extract LSMinimumSystemVersion raw "$app_bundle/Contents/Info.plist")" = "$expected_deployment_target" ] ||
    fail "Info.plist minimum macOS does not match $expected_deployment_target"
[ "$(read_value macos_deployment_target)" = "$expected_deployment_target" ] ||
    fail "binary deployment target does not match $expected_deployment_target"
[ "$(vtool -show-build "$executable" | awk '$1 == "minos" {print $2; exit}')" = "$expected_deployment_target" ] ||
    fail "bundled executable minimum macOS does not match $expected_deployment_target"
[ "$(lipo -archs "$executable")" = "$(read_value binary_architectures)" ] ||
    fail "bundled executable architectures differ from the gate"

# Info.plist cannot make a binary compatible with an older OS. Inspect every
# bundled Mach-O and reject one whose load command exceeds the declared target
# or omits an architecture carried by the main executable.
expected_architectures=$(read_value binary_architectures)
compatibility_errors=$(find "$app_bundle/Contents" -type f \
    \( -name '*.dylib' -o -name '*.so' -o -name '*.bundle' -o -perm -111 \) -print |
    while IFS= read -r candidate; do
        file "$candidate" | grep -Fq 'Mach-O' || continue
        minos_values=$(vtool -show-build "$candidate" 2>/dev/null |
            awk '$1 == "minos" {print $2}')
        if [ -z "$minos_values" ]; then
            printf '%s: missing Mach-O minimum system version\n' "$candidate"
            continue
        fi
        for minos in $minos_values; do
            if ! awk -v actual="$minos" -v expected="$expected_deployment_target" 'BEGIN {
                split(actual, a, "."); split(expected, e, ".");
                exit !((a[1] + 0 < e[1] + 0) ||
                       (a[1] + 0 == e[1] + 0 && a[2] + 0 <= e[2] + 0))
            }'; then
                printf '%s: minimum macOS %s exceeds %s\n' \
                    "$candidate" "$minos" "$expected_deployment_target"
            fi
        done
        candidate_architectures=$(lipo -archs "$candidate" 2>/dev/null || true)
        for architecture in $expected_architectures; do
            printf '%s\n' "$candidate_architectures" | tr ' ' '\n' | \
                grep -Fxq "$architecture" ||
                printf '%s: missing architecture %s\n' "$candidate" "$architecture"
        done
    done)
[ -z "$compatibility_errors" ] || {
    printf '%s\n' "$compatibility_errors" >&2
    fail "bundle contains incompatible Mach-O files"
}

"$dependency_tool" --file "$bundled_dependencies" --validate >/dev/null
bundled_dependency_sha=$(shasum -a 256 "$bundled_dependencies" | awk '{print $1}')
[ "$bundled_dependency_sha" = "$(read_value dependency_manifest_sha256)" ] ||
    fail "bundled dependency manifest differs from the gate"
[ "$bundled_dependency_sha" = "$("$dependency_tool" --sha256)" ] ||
    fail "bundled dependency manifest differs from this source"
bundled_libcdr_manifest_sha=$(shasum -a 256 "$bundled_libcdr_manifest" | awk '{print $1}')
[ "$bundled_libcdr_manifest_sha" = "$(read_value libcdr_manifest_sha256)" ] ||
    fail "bundled libcdr provenance differs from the gate"
bundled_build_provenance_sha=$(shasum -a 256 "$bundled_build_provenance" | awk '{print $1}')
[ "$bundled_build_provenance_sha" = "$(read_value build_provenance_sha256)" ] ||
    fail "bundled build provenance differs from the gate"
bundled_features_sha=$(shasum -a 256 "$bundled_features" | awk '{print $1}')
[ "$bundled_features_sha" = "$(read_value cmake_features_sha256)" ] ||
    fail "bundled resolved CMake features differ from the gate"
for feature in $("$dependency_tool" --get required_cmake_features | tr ',' ' '); do
    [ "$(awk -F= -v key="$feature" '$1 == key {print $2; exit}' "$bundled_features")" = "ON" ] ||
        fail "bundled feature evidence does not enable $feature"
done
for feature in $("$dependency_tool" --get disabled_cmake_features | tr ',' ' '); do
    [ "$(awk -F= -v key="$feature" '$1 == key {print $2; exit}' "$bundled_features")" = "OFF" ] ||
        fail "bundled feature evidence does not disable $feature"
done
[ "$(awk -F= '$1 == "source_commit" {print $2; exit}' "$bundled_libcdr_manifest")" = \
   "$("$dependency_tool" --get libcdr_commit)" ] ||
    fail "bundled libcdr provenance identifies another source commit"

bundled_libcdr=$(find "$resources/lib" -maxdepth 1 -type f -name 'libcdr-0.1*.dylib' -print | head -n 1)
[ -n "$bundled_libcdr" ] || fail "bundled modern libcdr library is missing"
collect_bmp_overloads=$(nm "$bundled_libcdr" 2>/dev/null | c++filt | \
    grep -Fc 'CDRStylesCollector::collectBmp' || true)
[ "$collect_bmp_overloads" -ge 4 ] ||
    fail "bundled libcdr lacks the modern bitmap-alpha collector ($collect_bmp_overloads overloads)"
bundled_libtiff=$(find "$resources/lib" -maxdepth 1 -type f -name 'libtiff*.dylib' -print | head -n 1)
[ -n "$bundled_libtiff" ] || fail "native TIFF runtime library is missing"

grep -Fq 'export GSETTINGS_SCHEMA_DIR="$resources_dir/share/glib-2.0/schemas"' "$launcher" || \
    fail "launcher does not expose bundled GSettings schemas"
GSETTINGS_SCHEMA_DIR="$schema_dir" gsettings list-schemas | \
    grep -Fxq 'org.gtk.gtk4.Settings.FileChooser' || \
    fail "GTK FileChooser schema cannot be loaded"
grep -Fq 'libpixbufloader_svg.so' "$resources/gdk-pixbuf-loaders.cache.in" || \
    fail "SVG loader is absent from the bundled cache"
grep -Fq 'libpixbufloader-tiff.so' "$resources/gdk-pixbuf-loaders.cache.in" || \
    fail "TIFF loader is absent from the bundled cache"
grep -Fq 'image/tiff' "$resources/gdk-pixbuf-loaders.cache.in" || \
    fail "bundled cache does not register the image/tiff format"

"$script_dir/verify-patched-cairo-runtime.sh" "$app_bundle"

external_dependencies=$({
    otool -L "$executable"
    otool -L "$sparrow_helper"
    find "$resources/lib" -type f \( -name '*.dylib' -o -name '*.so' \) -exec otool -L {} +
} | awk '
    /^[[:space:]]+\// {
        path=$1
        if (path !~ /^\/usr\/lib\// && path !~ /^\/System\/Library\//) print path
    }' | sort -u)
[ -z "$external_dependencies" ] || {
    printf '%s\n' "$external_dependencies" >&2
    fail "bundle contains non-system absolute dependencies"
}

source_commit=$(read_value source_commit)
short_commit=$(printf '%s' "$source_commit" | cut -c1-10)
[ "$(plutil -extract VACardsSourceCommit raw "$app_bundle/Contents/Info.plist")" = "$source_commit" ] ||
    fail "Info.plist source commit does not match the attestation"
profile_dir=$(mktemp -d "${TMPDIR:-/tmp}/vacards-package-profile.XXXXXX")
trap 'rm -rf "$profile_dir"' EXIT HUP INT TERM
version=$(env -i HOME="${HOME:-/tmp}" LANG=en_US.UTF-8 LC_ALL=en_US.UTF-8 \
    TMPDIR="${TMPDIR:-/tmp}" PATH=/usr/bin:/bin:/usr/sbin:/sbin \
    INKSCAPE_PROFILE_DIR="$profile_dir" "$launcher" --version 2>/dev/null || true)
printf '%s\n' "$version" | grep -Fq "$short_commit" || \
    fail "clean-environment launch did not identify $short_commit"

# Prove the packaged app can perform a real color-managed TIFF export without
# Homebrew or Python on PATH. The unit test verifies sample values and the full
# ICC payload; this package smoke verifies the self-contained runtime graph.
tiff_smoke_svg=$profile_dir/tiff-smoke.svg
tiff_smoke_output=$profile_dir/tiff-smoke.tiff
printf '%s\n' \
    '<svg xmlns="http://www.w3.org/2000/svg" width="1in" height="1in" viewBox="0 0 1 1">' \
    '<rect width="1" height="1" fill="#cc9933" fill-opacity="0.5"/>' \
    '</svg>' > "$tiff_smoke_svg"
env -i HOME="${HOME:-/tmp}" LANG=en_US.UTF-8 LC_ALL=en_US.UTF-8 \
    TMPDIR="${TMPDIR:-/tmp}" PATH=/usr/bin:/bin:/usr/sbin:/sbin \
    INKSCAPE_PROFILE_DIR="$profile_dir" \
    "$launcher" --export-dpi=300 --export-filename="$tiff_smoke_output" \
    "$tiff_smoke_svg" >/dev/null 2>&1 || fail "clean-environment TIFF export failed"
[ -s "$tiff_smoke_output" ] || fail "clean-environment TIFF export produced no file"
tiff_smoke_metadata=$(/usr/bin/sips -g format -g pixelWidth -g pixelHeight \
    -g dpiWidth -g dpiHeight -g samplesPerPixel -g bitsPerSample -g hasAlpha \
    -g space -g profile "$tiff_smoke_output" 2>/dev/null)
printf '%s\n' "$tiff_smoke_metadata" | grep -Fq 'format: tiff' ||
    fail "packaged TIFF smoke output is not a TIFF"
printf '%s\n' "$tiff_smoke_metadata" | grep -Fq 'pixelWidth: 300' ||
    fail "packaged TIFF smoke output width is wrong"
printf '%s\n' "$tiff_smoke_metadata" | grep -Fq 'pixelHeight: 300' ||
    fail "packaged TIFF smoke output height is wrong"
printf '%s\n' "$tiff_smoke_metadata" | grep -Fq 'dpiWidth: 300.000' ||
    fail "packaged TIFF smoke output x-DPI is wrong"
printf '%s\n' "$tiff_smoke_metadata" | grep -Fq 'dpiHeight: 300.000' ||
    fail "packaged TIFF smoke output y-DPI is wrong"
printf '%s\n' "$tiff_smoke_metadata" | grep -Fq 'samplesPerPixel: 4' ||
    fail "packaged TIFF smoke output is not RGBA"
printf '%s\n' "$tiff_smoke_metadata" | grep -Fq 'bitsPerSample: 8' ||
    fail "packaged TIFF smoke output is not 8-bit"
printf '%s\n' "$tiff_smoke_metadata" | grep -Fq 'hasAlpha: yes' ||
    fail "packaged TIFF smoke output lost alpha"
printf '%s\n' "$tiff_smoke_metadata" | grep -Fq 'space: RGB' ||
    fail "packaged TIFF smoke output is not RGB"
if printf '%s\n' "$tiff_smoke_metadata" | grep -Fq 'profile: <nil>'; then
    fail "packaged TIFF smoke output does not embed its ICC profile"
fi

codesign --verify --deep --strict "$app_bundle" || fail "ad-hoc signature is invalid"
echo "Verified self-contained VACards app: $release_version ($short_commit)"
