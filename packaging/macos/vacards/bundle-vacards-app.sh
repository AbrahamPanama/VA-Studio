#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later

set -eu

if [ "$#" -ne 3 ]; then
    echo "usage: $0 CAIRO_PREFIX INKSCAPE_INSTALL_PREFIX APP_BUNDLE" >&2
    exit 2
fi

cairo_prefix=$1
inkscape_prefix=$2
app_bundle=$3
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
packaging_provenance=$script_dir/../../vacards/packaging-provenance.py
packaging_inputs=$inkscape_prefix/VACARDS-PACKAGING-INPUTS.json
resource_dir=$(CDPATH= cd -- "$script_dir/../res" && pwd)
executable=$inkscape_prefix/bin/inkscape
patched_cairo=$cairo_prefix/lib/libcairo.2.dylib
macos_dir=$app_bundle/Contents/MacOS
app_resources=$app_bundle/Contents/Resources
library_dir=$app_resources/lib
bundled_executable=$macos_dir/inkscape-bin
sparrow_helper=$inkscape_prefix/bin/vacards-sparrow
launcher=$macos_dir/inkscape
bundled_loader_query=$macos_dir/gdk-pixbuf-query-loaders-vacards
loader_dir=$library_dir/gdk-pixbuf-2.0/2.10.0/loaders
bundled_loader=$loader_dir/libpixbufloader_svg.so
bundled_tiff_loader=$loader_dir/libpixbufloader-tiff.so
gate_attestation=$inkscape_prefix/VACARDS-RELEASE-GATE.env
dependency_manifest=$inkscape_prefix/VACARDS-DEPENDENCIES.env
libcdr_manifest=$inkscape_prefix/VACARDS-LIBCDR.env
build_provenance=$inkscape_prefix/VACARDS-BUILD-PROVENANCE.env
resolved_features=$inkscape_prefix/VACARDS-CMAKE-FEATURES.env
version_file=$script_dir/../../vacards/VERSION.env
version_tool=$script_dir/vacards-version.sh
dependency_tool=$script_dir/vacards-dependencies.sh
base_version=$("$version_tool" --base)
build_number=$("$version_tool" --build)
release_version=$("$version_tool" --release)
macos_deployment_target=$("$dependency_tool" --get macos_deployment_target)
tiff_profile_filename=$("$dependency_tool" --get tiff_rgb_profile_filename)
tiff_profile_sha256=$("$dependency_tool" --get tiff_rgb_profile_sha256)
tiff_profile_source=${VACARDS_TIFF_ICC_PROFILE:?set VACARDS_TIFF_ICC_PROFILE to the TIFF ICC profile file}
tiff_profile_dir=$app_resources/share/inkscape/color/icc
tiff_profile_destination=$tiff_profile_dir/$tiff_profile_filename
source_commit=$(awk -F= '$1 == "source_commit" {print $2; exit}' "$gate_attestation" 2>/dev/null || true)

dedupe_rpath()
{
    macho_file=$1
    rpath_value=$2
    count=$(otool -l "$macho_file" | awk '/LC_RPATH/{getline; getline; print $2}' | grep -Fxc "$rpath_value" || true)
    while [ "$count" -gt 1 ]; do
        install_name_tool -delete_rpath "$rpath_value" "$macho_file"
        count=$((count - 1))
    done
}

rewrite_absolute_cairo_references()
{
    macho_file=$1
    bundled_reference=$2
    cairo_reference_count=$(otool -L "$macho_file" | \
        awk '$1 ~ /libcairo\.2\.dylib$/ {count++} END {print count + 0}')

    if [ "$cairo_reference_count" -gt 1 ]; then
        echo "Mach-O file links more than one Cairo runtime: $macho_file" >&2
        echo "configure all GTK-related CMake pkg-config cache entries to use CAIRO_PREFIX" >&2
        otool -L "$macho_file" | sed -n '/libcairo\.2\.dylib/p' >&2
        exit 1
    fi

    otool -L "$macho_file" | \
        awk '$1 ~ /^\// && $1 ~ /\/libcairo\.2\.dylib$/ {print $1}' | \
        while IFS= read -r old_path; do
            install_name_tool -change "$old_path" "$bundled_reference" "$macho_file"
        done
}

plist_set_string()
{
    key=$1
    value=$2
    plist=$app_bundle/Contents/Info.plist
    if plutil -extract "$key" raw "$plist" >/dev/null 2>&1; then
        plutil -replace "$key" -string "$value" "$plist"
    else
        plutil -insert "$key" -string "$value" "$plist"
    fi
}

for tool in c++filt codesign dylibbundler file gdk-pixbuf-query-loaders gsettings \
            glib-compile-schemas install_name_tool ldid lipo nm otool pkg-config \
            plutil python3 shasum vtool xattr; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "required tool not found: $tool" >&2
        exit 1
    fi
done

bundle_schema_dir=$app_resources/share/glib-2.0/schemas

if [ -e "$app_bundle" ]; then
    echo "refusing to overwrite existing app bundle: $app_bundle" >&2
    exit 1
fi
if [ ! -x "$executable" ]; then
    echo "Inkscape executable not found: $executable" >&2
    exit 1
fi
if [ ! -f "$patched_cairo" ] || [ ! -f "$cairo_prefix/VACARDS-CAIRO.txt" ]; then
    echo "not a VACards patched Cairo installation: $cairo_prefix" >&2
    exit 1
fi
if [ ! -f "$tiff_profile_source" ]; then
    echo "VACards RGB TIFF output profile not found: $tiff_profile_source" >&2
    echo "set VACARDS_TIFF_ICC_PROFILE to the approved $tiff_profile_filename file" >&2
    exit 1
fi
actual_tiff_profile_sha256=$(shasum -a 256 "$tiff_profile_source" | awk '{print $1}')
if [ "$actual_tiff_profile_sha256" != "$tiff_profile_sha256" ]; then
    echo "VACards RGB TIFF output profile checksum mismatch" >&2
    echo "expected: $tiff_profile_sha256" >&2
    echo "actual:   $actual_tiff_profile_sha256" >&2
    exit 1
fi
"$script_dir/verify-vacards-cairo-prefix.sh" "$cairo_prefix"

# Packaging is intentionally impossible without a clean, full regression gate
# tied to this exact commit and installed executable.
"$script_dir/verify-release-attestation.sh" "$inkscape_prefix"
python3 "$packaging_provenance" verify-inputs "$packaging_inputs" --cairo-prefix "$cairo_prefix"
libcdr_prefix=$(awk -F= '$1 == "libcdr_prefix" {sub(/^[^=]*=/, ""); print; exit}' \
    "$build_provenance")
[ -d "$libcdr_prefix" ] || {
    echo "attested libcdr prefix is unavailable: $libcdr_prefix" >&2
    exit 1
}
libcdr_prefix=$(CDPATH= cd -- "$libcdr_prefix" && pwd)
"$script_dir/verify-vacards-libcdr.sh" "$libcdr_prefix"
libcdr_relative=$(awk -F= '$1 == "library_relative_path" {sub(/^[^=]*=/, ""); print; exit}' \
    "$libcdr_manifest")
attested_libcdr=$libcdr_prefix/$libcdr_relative
[ -f "$attested_libcdr" ] || {
    echo "attested libcdr library is unavailable: $attested_libcdr" >&2
    exit 1
}

mkdir -p "$macos_dir" "$app_resources" "$loader_dir" "$library_dir/gio/modules"
cp "$executable" "$bundled_executable"
cp "$script_dir/inkscape-launcher.sh" "$launcher"
chmod +x "$launcher"
cp "$resource_dir/inkscape.plist" "$app_bundle/Contents/Info.plist"
cp "$resource_dir/VACards-AppIcon.icns" "$app_resources/inkscape.icns"
cp "$inkscape_prefix/VACARDS-CAIRO.txt" "$app_resources/VACARDS-CAIRO.txt"
cp "$packaging_inputs" "$app_resources/VACARDS-PACKAGING-INPUTS.json"
cp "$gate_attestation" "$app_resources/VACARDS-RELEASE-GATE.env"
cp "$dependency_manifest" "$app_resources/VACARDS-DEPENDENCIES.env"
cp "$libcdr_manifest" "$app_resources/VACARDS-LIBCDR.env"
cp "$build_provenance" "$app_resources/VACARDS-BUILD-PROVENANCE.env"
cp "$inkscape_prefix/VACARDS-GATE-RUN.json" "$app_resources/VACARDS-GATE-RUN.json"
cp "$resolved_features" "$app_resources/VACARDS-CMAKE-FEATURES.env"
cp "$version_file" "$app_resources/VACARDS-VERSION.env"
if [ -d "$inkscape_prefix/share" ]; then
    cp -R "$inkscape_prefix/share" "$app_resources/share"
fi
mkdir -p "$tiff_profile_dir"
cp "$tiff_profile_source" "$tiff_profile_destination"

# Package the exact XML/override set and compiled cache exercised by the gate.
python3 "$packaging_provenance" copy-schemas --inputs "$packaging_inputs" \
    --source "$inkscape_prefix/VACARDS-GTK-SCHEMAS" --destination "$bundle_schema_dir"
if ! GSETTINGS_SCHEMA_DIR="$bundle_schema_dir" gsettings list-schemas | \
        grep -Fxq 'org.gtk.gtk4.Settings.FileChooser'; then
    echo "bundled GTK FileChooser GSettings schema failed validation" >&2
    exit 1
fi

plist_set_string CFBundleName "VA Studio"
plist_set_string CFBundleIdentifier "com.vacards.inkscape"
plist_set_string CFBundleShortVersionString "$base_version"
plist_set_string CFBundleVersion "$build_number"
plist_set_string CFBundleGetInfoString "VA Studio $release_version"
plist_set_string VACardsReleaseVersion "$release_version"
plist_set_string VACardsSourceCommit "$source_commit"
plist_set_string LSMinimumSystemVersion "$macos_deployment_target"

# GdkPixbuf discovers SVG support as a plug-in at runtime. Bundle that plug-in
# too; otherwise it pulls a second Homebrew GLib/Pango/Cairo graph into the
# already running process.
loader_source=$(gdk-pixbuf-query-loaders 2>/dev/null | awk -F\" '/libpixbufloader_svg/{print $2; exit}')
if [ ! -f "$loader_source" ]; then
    echo "GdkPixbuf SVG loader not found" >&2
    exit 1
fi
librsvg_libdir=$(pkg-config --variable=libdir librsvg-2.0)
cp -L "$loader_source" "$bundled_loader"
dylibbundler -b \
    -x "$bundled_loader" \
    -d "$library_dir" \
    -p '@loader_path/../../..' \
    -s "$library_dir" \
    -s "$librsvg_libdir" \
    -cd -of -ns
dedupe_rpath "$bundled_loader" '@loader_path/../../../'

# TIFF input is a loadable module (PNG/JPEG are built into libgdk_pixbuf). The
# launcher points GDK_PIXBUF_MODULE_FILE at a generated cache, so the TIFF module
# must be bundled and registered there or File > Open cannot match .tif/.tiff
# (BUG-003). Freeze the module and its libtiff closure like the SVG plug-in.
tiff_loader_source=$(gdk-pixbuf-query-loaders 2>/dev/null | awk -F\" '/libpixbufloader-tiff/{print $2; exit}')
if [ ! -f "$tiff_loader_source" ]; then
    echo "GdkPixbuf TIFF loader not found" >&2
    exit 1
fi
libtiff_libdir=$(pkg-config --variable=libdir libtiff-4)
cp -L "$tiff_loader_source" "$bundled_tiff_loader"
dylibbundler -b \
    -x "$bundled_tiff_loader" \
    -d "$library_dir" \
    -p '@loader_path/../../..' \
    -s "$library_dir" \
    -s "$libtiff_libdir" \
    -cd -of -ns
dedupe_rpath "$bundled_tiff_loader" '@loader_path/../../../'

# Bundle and rewrite every dependency of the main executable after the loader.
# This restores executable-relative paths for libraries shared by both graphs.
dylibbundler -b \
    -x "$bundled_executable" \
    -d "$library_dir" \
    -p '@executable_path/../Resources/lib/' \
    -s "$inkscape_prefix/lib" \
    -s "$inkscape_prefix/lib/inkscape" \
    -s "$cairo_prefix/lib" \
    -s "$libcdr_prefix/lib" \
    -cd -of -ns

# Dylibbundler follows the executable's recorded path, but a stale build used
# to let it select Homebrew's stock libcdr. Replace that candidate explicitly
# with the commit-attested modern importer and then normalize its dependencies.
bundled_libcdr=$(find "$library_dir" -maxdepth 1 -type f -name 'libcdr-0.1*.dylib' -print | head -n 1)
[ -n "$bundled_libcdr" ] || {
    echo "bundled libcdr dependency was not found" >&2
    exit 1
}
cp "$attested_libcdr" "$bundled_libcdr"
dylibbundler -b \
    -x "$bundled_libcdr" \
    -d "$library_dir" \
    -p '@loader_path/' \
    -s "$libcdr_prefix/lib" \
    -cd -of -ns
install_name_tool -id "@rpath/${bundled_libcdr##*/}" "$bundled_libcdr"

# librsvg itself lives in the root library directory, while dylibbundler first
# saw it from the nested plug-in. Retarget only those inherited ../../.. paths;
# all referenced libraries were already bundled and then normalized above.
otool -L "$library_dir/librsvg-2.2.dylib" | \
    awk '/@loader_path\/\.\.\/\.\.\/\.\.\//{print $1}' | \
    while IFS= read -r old_path; do
        install_name_tool -change "$old_path" "@loader_path/${old_path##*/}" \
            "$library_dir/librsvg-2.2.dylib"
    done

# dylibbundler initially follows the dependency graph to Homebrew's release.
# Replace that copy with the pinned fixed build, then rewrite the fixed build's
# own dependency paths into the bundle as well.
cp "$patched_cairo" "$library_dir/libcairo.2.dylib"
dylibbundler -b \
    -x "$library_dir/libcairo.2.dylib" \
    -d "$library_dir" \
    -p '@loader_path/' \
    -s "$cairo_prefix/lib" \
    -cd -of -ns
install_name_tool -id '@rpath/libcairo.2.dylib' "$library_dir/libcairo.2.dylib"

# The executable can legitimately reach Cairo through more than one CMake
# dependency. dylibbundler rewrites the first path it followed, but can leave a
# second Homebrew alias (for example /opt/homebrew/opt/cairo/...) in the same
# Mach-O file. Normalize every remaining absolute Cairo reference explicitly so
# dyld cannot load both the bundled fixed Cairo and Homebrew's Cairo at runtime.
rewrite_absolute_cairo_references "$bundled_executable" \
    '@executable_path/../Resources/lib/libcairo.2.dylib'
find "$library_dir" -maxdepth 1 -type f -name '*.dylib' | \
    while IFS= read -r macho_file; do
        rewrite_absolute_cairo_references "$macho_file" \
            '@loader_path/libcairo.2.dylib'
    done
rewrite_absolute_cairo_references "$bundled_loader" \
    '@loader_path/../../../libcairo.2.dylib'
rewrite_absolute_cairo_references "$bundled_tiff_loader" \
    '@loader_path/../../../libcairo.2.dylib'

if find "$app_bundle/Contents" -type f \
        \( -name '*.dylib' -o -name '*.so' -o -name 'inkscape-bin' \) \
        -exec otool -L {} + 2>/dev/null | \
        grep -E '^[[:space:]]+/.*\/libcairo\.2\.dylib' >/dev/null; then
    echo "bundle still contains an absolute external Cairo reference" >&2
    exit 1
fi

# dylibbundler can collapse more than one original rpath to the same bundled
# path. dyld rejects duplicate LC_RPATH commands, so retain exactly one.
dedupe_rpath "$bundled_executable" '@executable_path/../Resources/lib/'
dedupe_rpath "$bundled_loader" '@loader_path/../../../'
dedupe_rpath "$bundled_tiff_loader" '@loader_path/../../../'
install_name_tool -id '@loader_path/libpixbufloader_svg.so' "$bundled_loader"
install_name_tool -id '@loader_path/libpixbufloader-tiff.so' "$bundled_tiff_loader"

# Freeze the discovered layout against tested sources (including GTK and
# cairo-gobject), hash the copies, and relocate only that approved graph.
python3 "$packaging_provenance" restore-bundle --app "$app_bundle" \
    --install "$inkscape_prefix" --cairo-prefix "$cairo_prefix"

# Every install_name_tool rewrite invalidates the copied Homebrew signature.
# Sign the completed library graph before asking GdkPixbuf to dlopen the SVG
# plug-in; otherwise macOS kills the helper and it silently emits an empty
# cache. The main executable is signed after cache generation below.
find "$library_dir" -type f \( -name '*.dylib' -o -name '*.so' \) -exec ldid -S {} \;

# Run the cache generator from Contents/MacOS so dependencies expressed with
# @executable_path resolve exactly as they will at runtime. Running Homebrew's
# copy in place incorrectly resolves them relative to its Cellar directory.
cp -L "$(command -v gdk-pixbuf-query-loaders)" "$bundled_loader_query"
chmod +x "$bundled_loader_query"
otool -L "$bundled_loader_query" | \
    awk 'NR > 1 && $1 ~ /^\// && $1 !~ /^\/usr\/lib\// && $1 !~ /^\/System\//{print $1}' | \
    while IFS= read -r old_path; do
        dependency=$library_dir/${old_path##*/}
        if [ ! -f "$dependency" ]; then
            echo "cache generator dependency was not bundled: $old_path" >&2
            exit 1
        fi
        install_name_tool -change "$old_path" \
            "@executable_path/../Resources/lib/${old_path##*/}" \
            "$bundled_loader_query"
    done
ldid -S "$bundled_loader_query"

# Generate the loader cache only after both dependency graphs have reached
# their final form. The second dylibbundler pass can touch a loader and
# reintroduce a duplicate rpath, which makes dyld reject the plug-in and leaves
# a deceptively valid but empty cache.
"$bundled_loader_query" "$bundled_loader" "$bundled_tiff_loader" | \
    sed "s#$app_resources#@APP_RESOURCES@#g" > "$app_resources/gdk-pixbuf-loaders.cache.in"
rm "$bundled_loader_query"
if ! grep -Fq 'libpixbufloader_svg.so' "$app_resources/gdk-pixbuf-loaders.cache.in"; then
    echo "bundled GdkPixbuf SVG loader failed validation" >&2
    exit 1
fi
if ! grep -Fq 'image/tiff' "$app_resources/gdk-pixbuf-loaders.cache.in"; then
    echo "bundled GdkPixbuf TIFF loader failed validation" >&2
    exit 1
fi

# ldid writes an ad-hoc signature without being blocked by the
# com.apple.provenance xattr attached to locally built executables on recent
# macOS releases.
ldid -S "$bundled_executable"

# Sign the completed bundle as one unit. This is still an ad-hoc development
# signature; Developer ID signing and notarization remain a separate release
# credential step.
# Copies from build/install trees can inherit com.apple.provenance, FinderInfo,
# or resource-fork attributes. They are not part of the application payload and
# make codesign reject an otherwise valid bundle, so strip them only from this
# newly-created packaging output immediately before signing.
xattr -cr "$app_bundle"
codesign --force --deep --sign - "$app_bundle"
# Preserve the exact, already ad-hoc-signed helper tested before packaging.
# The final outer signature below seals these bytes. A future Developer-ID
# pipeline must bind both pre-sign and post-sign identities, not reuse this hash.
sparrow_hash=$("$dependency_tool" --get sparrow_darwin_arm64_sha256)
test -x "$sparrow_helper" || { echo "Missing pinned Sparrow helper" >&2; exit 1; }
test "$(shasum -a 256 "$sparrow_helper" | awk '{print $1}')" = "$sparrow_hash" || {
    echo "Sparrow helper differs from the dependency pin" >&2; exit 1;
}
cp "$sparrow_helper" "$macos_dir/vacards-sparrow"
codesign --verify "$macos_dir/vacards-sparrow"
python3 "$packaging_provenance" finish-bundle "$app_bundle"
# Nested Mach-O bytes are final. Seal the new receipt without signing them again.
codesign --force --sign - "$app_bundle"

"$script_dir/verify-vacards-app.sh" "$app_bundle"
echo "Created VA Studio $release_version: $app_bundle"
