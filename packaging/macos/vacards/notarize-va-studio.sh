#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# DRAFT, not wired into the packaging scripts: sign VA Studio with a Developer
# ID certificate and the hardened runtime, notarize it with Apple and staple
# the tickets. It needs an Apple Developer Program membership (owner decision)
# and must be tested on a real build before it is used for a release.
#
# usage: notarize-va-studio.sh APP_BUNDLE OUTPUT_DMG \
#            --identity "Developer ID Application: NAME (TEAMID)" \
#            --keychain-profile PROFILE [--entitlements FILE] [--dry-run]
#
#   APP_BUNDLE        an unsigned or ad-hoc signed "VA Studio.app" (it is re-signed in place)
#   OUTPUT_DMG        new disk image to create, sign, notarize and staple
#   --identity        codesign identity in the login keychain
#   --keychain-profile  notarytool credentials stored beforehand by the owner with
#                     `xcrun notarytool store-credentials PROFILE`; this script never
#                     reads, prints or stores passwords or API keys
#   --entitlements    default: packaging/macos/res/vastudio.entitlements
#   --dry-run         print the commands instead of running them
#
# Order: every nested Mach-O (libraries, loaders, helpers) inside out, then the
# main executable and the bundle with the entitlements; verify; notarize a zip
# of the app and staple it; build the DMG; sign, notarize and staple the DMG;
# assess both with Gatekeeper. Any failure stops the script.
set -euo pipefail

fail() { echo "notarize-va-studio: $*" >&2; exit 1; }

[[ $# -ge 2 ]] || fail "usage: $0 APP_BUNDLE OUTPUT_DMG --identity ID --keychain-profile PROFILE [--entitlements FILE] [--dry-run]"
app=$1
dmg=$2
shift 2
script_dir=$(cd -- "$(dirname -- "$0")" && pwd)
identity= profile= dry_run=0
entitlements=$script_dir/../res/vastudio.entitlements
while [[ $# -gt 0 ]]; do
    case $1 in
        --identity) identity=${2:-}; shift 2 ;;
        --keychain-profile) profile=${2:-}; shift 2 ;;
        --entitlements) entitlements=${2:-}; shift 2 ;;
        --dry-run) dry_run=1; shift ;;
        *) fail "unknown argument: $1" ;;
    esac
done
[[ $(uname -s) == Darwin ]] || fail "macOS only"
[[ -n $identity && -n $profile ]] || fail "--identity and --keychain-profile are required"
[[ -d $app/Contents/MacOS ]] || fail "not an application bundle: $app"
[[ -f $entitlements ]] || fail "missing entitlements file: $entitlements"
[[ ! -e $dmg ]] || fail "output exists: $dmg"
for tool in codesign xcrun ditto hdiutil spctl file; do
    command -v "$tool" >/dev/null 2>&1 || fail "required tool not found: $tool"
done
app=$(cd -- "$app" && pwd -P)

run() {
    printf '+'; printf ' %q' "$@"; printf '\n'
    if [[ $dry_run -eq 0 ]]; then "$@"; fi
}

sign() {
    run codesign --force --options runtime --timestamp --sign "$identity" "$@"
}

# 1. Nested Mach-O files, deepest paths first, except the main executable.
main_executable=$app/Contents/MacOS/inkscape-bin
while IFS= read -r path; do
    [[ $path == "$main_executable" ]] && continue
    if file -b -- "$path" | grep -q 'Mach-O'; then
        case $path in
            */python3-bin) sign --entitlements "$entitlements" "$path" ;;
            *) sign "$path" ;;
        esac
    fi
done < <(find "$app/Contents" -type f | awk '{print length($0) "\t" $0}' | sort -rn | cut -f2-)

# 2. Main executable and the bundle (seals the launcher scripts and resources).
[[ -f $main_executable ]] || fail "missing main executable: $main_executable"
sign --entitlements "$entitlements" "$main_executable"
sign --entitlements "$entitlements" "$app"
run codesign --verify --deep --strict --verbose=2 "$app"

# 3. Notarize and staple the application.
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
run ditto -c -k --keepParent "$app" "$work/app.zip"
run xcrun notarytool submit "$work/app.zip" --keychain-profile "$profile" --wait
run xcrun stapler staple "$app"
run xcrun stapler validate "$app"

# 4. Disk image: create, sign, notarize, staple.
run hdiutil create -volname "VA Studio" -srcfolder "$app" -fs HFS+ -format UDZO "$dmg"
run codesign --force --timestamp --sign "$identity" "$dmg"
run xcrun notarytool submit "$dmg" --keychain-profile "$profile" --wait
run xcrun stapler staple "$dmg"
run xcrun stapler validate "$dmg"

# 5. Gatekeeper assessment of both.
run spctl --assess --type execute --verbose=4 "$app"
run spctl --assess --type open --context context:primary-signature --verbose=4 "$dmg"
echo "notarize-va-studio: signed, notarized and stapled $app and $dmg"
