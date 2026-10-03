#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later

set -eu

if [ "$#" -ne 3 ]; then
    echo "usage: $0 PATCHED_CAIRO_PREFIX INKSCAPE_INSTALL_PREFIX OUTPUT_DIRECTORY" >&2
    exit 2
fi

cairo_prefix=$1
inkscape_prefix=$2
output_arg=$3
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
version_tool=$script_dir/vacards-version.sh
release_version=$("$version_tool" --release)
build_number=$("$version_tool" --build)
executable=$inkscape_prefix/bin/inkscape

fail()
{
    echo "VACards DMG creation failed: $*" >&2
    exit 1
}

for tool in codesign ditto hdiutil lipo mktemp shasum; do
    command -v "$tool" >/dev/null 2>&1 || fail "required tool not found: $tool"
done
[ -x "$executable" ] || fail "Inkscape executable not found: $executable"

arch_list=$(lipo -archs "$executable")
arch_count=$(printf '%s\n' "$arch_list" | awk '{print NF}')
case "$arch_count" in
    1) architecture=$arch_list ;;
    2) architecture=universal2 ;;
    *) fail "unsupported architecture list: $arch_list" ;;
esac

mkdir -p "$output_arg"
output_dir=$(CDPATH= cd -- "$output_arg" && pwd)
artifact_name=VA-Studio-$release_version-$architecture
dmg=$output_dir/$artifact_name.dmg
checksum_file=$dmg.sha256
[ ! -e "$dmg" ] || fail "refusing to overwrite $dmg"
[ ! -e "$checksum_file" ] || fail "refusing to overwrite $checksum_file"

stage=$(mktemp -d /private/tmp/vacards-package.$build_number.XXXXXX)
case "$stage" in
    /private/tmp/vacards-package.$build_number.*) ;;
    *) fail "unexpected temporary directory: $stage" ;;
esac
mount_point=$stage/mount
mounted=0
cleanup()
{
    if [ "$mounted" -eq 1 ]; then
        hdiutil detach "$mount_point" >/dev/null 2>&1 || true
    fi
    rm -rf "$stage"
}
trap cleanup EXIT HUP INT TERM

app_name='VA Studio.app'
app_bundle=$stage/$app_name
dmg_root=$stage/dmg-root
temporary_dmg=$stage/$artifact_name.dmg

"$script_dir/bundle-vacards-app.sh" "$cairo_prefix" "$inkscape_prefix" "$app_bundle"
mkdir "$dmg_root"
ditto "$app_bundle" "$dmg_root/$app_name"
ln -s /Applications "$dmg_root/Applications"
codesign --verify --deep --strict "$dmg_root/$app_name"

hdiutil create -volname 'VA Studio' -srcfolder "$dmg_root" \
    -format UDZO -imagekey zlib-level=9 "$temporary_dmg"
hdiutil verify "$temporary_dmg"

mkdir "$mount_point"
hdiutil attach -nobrowse -readonly -mountpoint "$mount_point" "$temporary_dmg" >/dev/null
mounted=1
"$script_dir/verify-vacards-app.sh" "$mount_point/$app_name"
hdiutil detach "$mount_point" >/dev/null
mounted=0

ditto "$temporary_dmg" "$dmg"
checksum=$(shasum -a 256 "$dmg" | awk '{print $1}')
printf '%s  %s\n' "$checksum" "$(basename "$dmg")" > "$checksum_file"

echo "Created VACards release $release_version (build $build_number):"
echo "  $dmg"
echo "  $checksum_file"
