#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Capture the platform-library inventory (SBOM) of a VA Studio macOS application
# bundle whose third-party libraries were copied from Homebrew.
#
# usage: capture-sbom-homebrew.sh APP_BUNDLE OUTPUT_PREFIX [--va-prefix DIR]...
#
#   APP_BUNDLE     the built "VA Studio.app"
#   OUTPUT_PREFIX  path prefix for the new files OUTPUT_PREFIX.tsv and
#                  OUTPUT_PREFIX.unattributed.txt (neither may exist)
#   --va-prefix    install prefix of a dependency VA Studio builds itself
#                  (patched Cairo, patched GTK, libcdr fork, patched librevenge);
#                  repeat for each. Libraries found there are "local-build".
#
# It walks the otool -L closure of every Mach-O file in the bundle. Each bundled
# library is matched by file name to the Homebrew keg it was copied from
# ($(brew --prefix)/opt/*/lib and opt/*/Frameworks), and the keg's formula,
# installed version, license (brew info --json=v2) and keg license files are
# recorded. The application's own executables are "local-build". A bundled
# Mach-O that matches neither Homebrew nor a --va-prefix is an error, and so is
# a closure dependency that is neither bundled nor a macOS system library: the
# script writes its report and exits 1 (fail closed).
#
# Read-only on the bundle and on Homebrew; works with the macOS /bin/bash 3.2.
# Output schema: va-studio-platform-sbom/1, consumed by
# generate-third-party-notices.py --sbom OUTPUT_PREFIX.tsv.
set -euo pipefail
export LC_ALL=C

fail() { echo "capture-sbom-homebrew: $*" >&2; exit 1; }

[[ $# -ge 2 ]] || { echo "usage: $0 APP_BUNDLE OUTPUT_PREFIX [--va-prefix DIR]..." >&2; exit 2; }
app=$1
out=$2
shift 2
va_prefixes=()
while [[ $# -gt 0 ]]; do
    case $1 in
        --va-prefix)
            [[ $# -ge 2 && -d $2 ]] || fail "--va-prefix needs an existing directory"
            va_prefixes+=("$(cd -- "$2" && pwd -P)")
            shift 2 ;;
        *) fail "unknown argument: $1" ;;
    esac
done
[[ $(uname -s) == Darwin ]] || fail "macOS only"
for tool in brew otool file shasum python3 find realpath; do
    command -v "$tool" >/dev/null 2>&1 || fail "required tool not found: $tool"
done
[[ -d $app/Contents ]] || fail "not an application bundle: $app"
[[ ! -e $out.tsv && ! -e $out.unattributed.txt ]] || fail "refusing to overwrite $out.tsv or $out.unattributed.txt"
app=$(cd -- "$app" && pwd -P)
brew_prefix=$(brew --prefix)
mkdir -p -- "$(dirname -- "$out")"
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT

# 1. Mach-O files in the bundle (relative paths).
: > "$work/macho"
while IFS= read -r -d '' path; do
    if file -b -- "$path" | grep -q 'Mach-O'; then
        printf '%s\n' "${path#"$app"/}" >> "$work/macho"
    fi
done < <(find "$app/Contents" -type f -print0)
[[ -s $work/macho ]] || fail "no Mach-O files in $app"
sort -o "$work/macho" "$work/macho"
sed 's|.*/||' "$work/macho" | sort -u > "$work/bundled-names"

# 2. The otool closure: every non-system dependency must be bundled.
: > "$work/unattributed"
while IFS= read -r rel; do
    otool -L "$app/$rel" | tail -n +2 | awk '{print $1}' > "$work/deps"
    while IFS= read -r dep; do
        case $dep in
            /usr/lib/*|/System/*) ;;
            @rpath/*|@loader_path/*|@executable_path/*)
                grep -qxF -- "${dep##*/}" "$work/bundled-names" ||
                    printf 'MISSING\t%s needs %s\n' "$rel" "$dep" >> "$work/unattributed" ;;
            *) printf 'OUTSIDE\t%s needs %s\n' "$rel" "$dep" >> "$work/unattributed" ;;
        esac
    done < "$work/deps"
done < "$work/macho"

# 3. Attribute each Mach-O file.
captured=$(date -u '+%Y-%m-%dT%H:%M:%SZ')
{
    printf '# schema=va-studio-platform-sbom/1 platform=macos-arm64-homebrew captured_utc=%s prefix=%s\n' \
        "$captured" "$brew_prefix"
    printf 'file\tsha256\tpackage\tversion\tlicense\tlicense_files\turl\n'
} > "$out.tsv"
: > "$work/formulae"
while IFS= read -r rel; do
    name=${rel##*/}
    digest=$(shasum -a 256 -- "$app/$rel" | awk '{print $1}')
    case $rel in
        Contents/MacOS/*|*/vacards-sparrow|*/libinkscape_base.dylib|*/lib2geom*.dylib)
            printf '%s\t%s\tlocal-build\tsee Contents/Resources/VACARDS-*.env\tsee THIRD-PARTY-NOTICES.md\t\t\n' \
                "$rel" "$digest" >> "$out.tsv"
            continue ;;
    esac
    local_match=""
    for prefix in "${va_prefixes[@]+"${va_prefixes[@]}"}"; do
        if [[ -n $(find "$prefix" -name "$name" -type f -print -quit) ]]; then
            local_match=$prefix
            break
        fi
    done
    if [[ -n $local_match ]]; then
        printf '%s\t%s\tlocal-build\t%s\tsee THIRD-PARTY-NOTICES.md\t\t\n' \
            "$rel" "$digest" "${local_match##*/}" >> "$out.tsv"
        continue
    fi
    candidate=$(find -L "$brew_prefix"/opt/*/lib "$brew_prefix"/opt/*/Frameworks -name "$name" -type f \
                -print -quit 2>/dev/null || true)
    keg=""
    [[ -z $candidate ]] || keg=$(realpath "$candidate")
    case $keg in
        */Cellar/*/*/*) ;;
        *) printf 'MODULE\t%s\n' "$rel" >> "$work/unattributed"; continue ;;
    esac
    formula=$(printf '%s\n' "$keg" | sed -E 's|.*/Cellar/([^/]+)/([^/]+)/.*|\1|')
    version=$(printf '%s\n' "$keg" | sed -E 's|.*/Cellar/([^/]+)/([^/]+)/.*|\2|')
    keg_root=$(printf '%s\n' "$keg" | sed -E 's|(.*/Cellar/[^/]+/[^/]+)/.*|\1|')
    meta=$(awk -F'\t' -v f="$formula" '$1 == f {print $2 "\t" $3; exit}' "$work/formulae")
    if [[ -z $meta ]]; then
        meta=$(brew info --json=v2 --formula "$formula" | python3 -c '
import json, sys
f = json.load(sys.stdin)["formulae"][0]
license = f.get("license")
text = json.dumps(license, sort_keys=True) if isinstance(license, (dict, list)) else (license or "UNKNOWN")
print(text.replace("\t", " ") + "\t" + (f.get("homepage") or ""))')
        printf '%s\t%s\n' "$formula" "$meta" >> "$work/formulae"
    fi
    license=${meta%%$'\t'*}
    homepage=${meta#*$'\t'}
    files=$(find "$keg_root" -maxdepth 1 -type f \( -iname 'licen[cs]e*' -o -iname 'copying*' \
            -o -iname 'notice*' -o -iname 'copyright*' -o -iname 'authors*' \) | sort | paste -sd ';' -)
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$rel" "$digest" "$formula" "$version" "$license" "$files" "$homepage" >> "$out.tsv"
done < "$work/macho"

cp "$work/unattributed" "$out.unattributed.txt"
rows=$(($(wc -l < "$out.tsv") - 2))
bad=$(wc -l < "$out.unattributed.txt" | tr -d ' ')
echo "capture-sbom-homebrew: $rows Mach-O files recorded -> $out.tsv"
if [[ $bad -gt 0 ]]; then
    fail "$bad unattributed or unbundled libraries (see $out.unattributed.txt)"
fi
