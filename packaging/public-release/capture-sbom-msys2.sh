#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Capture the platform-library inventory (SBOM) of a staged VA Studio Windows
# payload built with MSYS2 UCRT64.
#
# usage: capture-sbom-msys2.sh PAYLOAD_DIR OUTPUT_PREFIX [MSYS2_PREFIX]
#
#   PAYLOAD_DIR    the staged payload (create-internal-test-installer.py writes it
#                  as OUTPUT/payload) or an installed VA Studio directory
#   OUTPUT_PREFIX  path prefix for the new files OUTPUT_PREFIX.tsv and
#                  OUTPUT_PREFIX.unattributed.txt (neither may exist)
#   MSYS2_PREFIX   default /ucrt64
#
# For every payload file whose bytes equal the same relative path under the
# MSYS2 prefix it records the owning pacman package, its version, license, the
# license files that package installs under share/licenses, and its project
# URL. Executable modules (.dll .exe .com .pyd) built by VA Studio (the
# application, the pinned Sparrow helper, the shell extension and the patched or
# forked libraries) are recorded as "local-build". Any other unattributed
# module is an error: the script writes its report and exits 1 (fail closed).
# Unattributed data files are listed for review in OUTPUT_PREFIX.unattributed.txt.
#
# Read-only on the payload and on the MSYS2 installation; it never installs or
# updates packages. Output schema: va-studio-platform-sbom/1, consumed by
# generate-third-party-notices.py --sbom OUTPUT_PREFIX.tsv.
set -euo pipefail
export LC_ALL=C

fail() { echo "capture-sbom-msys2: $*" >&2; exit 1; }

[[ $# -ge 2 && $# -le 3 ]] || { echo "usage: $0 PAYLOAD_DIR OUTPUT_PREFIX [MSYS2_PREFIX]" >&2; exit 2; }
payload=$1
out=$2
prefix=${3:-/ucrt64}
[[ ${MSYSTEM:-} == UCRT64 ]] || fail "run from an MSYS2 UCRT64 shell"
for tool in pacman sha256sum cmp find sort awk sed split xargs; do
    command -v "$tool" >/dev/null 2>&1 || fail "required tool not found: $tool"
done
[[ -d $payload ]] || fail "payload directory not found: $payload"
[[ -d $prefix/bin ]] || fail "MSYS2 prefix not found: $prefix"
[[ ! -e $out.tsv && ! -e $out.unattributed.txt ]] || fail "refusing to overwrite $out.tsv or $out.unattributed.txt"
payload=$(cd -- "$payload" && pwd -P)
mkdir -p -- "$(dirname -- "$out")"

work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT

# 1. Every payload file, relative, with its digest. Names with tabs or newlines
#    cannot be represented in the TSV and fail the capture.
( cd -- "$payload" && find . -type f -print0 ) > "$work/files0"
: > "$work/files"
while IFS= read -r -d '' path; do
    rel=${path#./}
    [[ $rel != *$'\t'* && $rel != *$'\n'* ]] || fail "unsupported file name: $rel"
    printf '%s\n' "$rel" >> "$work/files"
done < "$work/files0"
sort -o "$work/files" "$work/files"
[[ -s $work/files ]] || fail "payload is empty: $payload"
( cd -- "$payload" && tr '\n' '\0' < "$work/files" | xargs -0 sha256sum ) |
    sed 's/^\\//; s/  /\t/' > "$work/digests"

# 2. Files identical to the same path under the MSYS2 prefix are package candidates.
: > "$work/candidates"
while IFS= read -r rel; do
    if [[ -f $prefix/$rel ]] && cmp -s -- "$payload/$rel" "$prefix/$rel"; then
        printf '%s\n' "$rel" >> "$work/candidates"
    fi
done < "$work/files"

# 3. Owners, in batches. "pacman -Qo" prints "<path> is owned by <pkg> <version>".
: > "$work/owners"
if [[ -s $work/candidates ]]; then
    split -l 200 "$work/candidates" "$work/batch."
    for batch in "$work"/batch.*; do
        mapfile -t rels < "$batch"
        paths=()
        for rel in "${rels[@]}"; do paths+=("$prefix/$rel"); done
        pacman -Qo -- "${paths[@]}" 2>/dev/null |
            sed -n "s|^$prefix/\\(.*\\) is owned by \\([^ ]*\\) \\([^ ]*\\)\$|\\1\t\\2\t\\3|p" >> "$work/owners" || true
    done
fi
sort -u -o "$work/owners" "$work/owners"

# 4. Package metadata and the license files each package installs.
cut -f2 "$work/owners" | sort -u > "$work/packages"
: > "$work/metadata"
while IFS= read -r package; do
    [[ -n $package ]] || continue
    info=$(pacman -Qi -- "$package") || fail "pacman -Qi failed for $package"
    license=$(printf '%s\n' "$info" | sed -n 's/^Licenses *: *//p' | head -n1)
    url=$(printf '%s\n' "$info" | sed -n 's/^URL *: *//p' | head -n1)
    files=$(pacman -Qlq -- "$package" | sed -n "s|^$prefix/\\(share/licenses/.*[^/]\\)\$|\\1|p" | paste -sd ';' -)
    printf '%s\t%s\t%s\t%s\n' "$package" "${license:-UNKNOWN}" "${files:-}" "${url:-}" >> "$work/metadata"
done < "$work/packages"

# 5. Assemble the TSV in one pass.
captured=$(date -u '+%Y-%m-%dT%H:%M:%SZ')
{
    printf '# schema=va-studio-platform-sbom/1 platform=windows-x64-msys2-ucrt64 captured_utc=%s prefix=%s\n' \
        "$captured" "$prefix"
    printf 'file\tsha256\tpackage\tversion\tlicense\tlicense_files\turl\n'
} > "$out.tsv"
: > "$out.unattributed.txt"
bad=$(awk -F'\t' -v OFS='\t' -v out="$out.tsv" -v un="$out.unattributed.txt" '
    # Modules that VA Studio builds itself or pins outside MSYS2.
    function local_build(r) {
        return r ~ /^bin\/(inkscape|inkview)\.(exe|com)$/ ||
               r ~ /^bin\/(vacards-sparrow\.exe|vasvgthumb\.dll)$/ || r == "vacards-test.exe" ||
               r ~ /^bin\/(libcdr-0\.1|librevenge-[^\/]*|libcairo[^\/]*|libgtk-4-1)\.dll$/ ||
               r ~ /^lib\/inkscape\// || r ~ /^bin\/(libinkscape_base|lib2geom[^\/]*)\.dll$/
    }
    FILENAME == ARGV[1] { owner[$1] = $2; version[$1] = $3; next }
    FILENAME == ARGV[2] { lic[$1] = $2; files[$1] = $3; url[$1] = $4; next }
    {
        digest = $1; rel = $2
        if (rel in owner) {
            p = owner[rel]
            print rel, digest, p, version[rel], lic[p], files[p], url[p] >> out
            next
        }
        low = tolower(rel)
        if (low ~ /\.(dll|exe|com|pyd)$/) {
            if (local_build(low))
                print rel, digest, "local-build", "see share/vacards-test/VACARDS-*.env", "see THIRD-PARTY-NOTICES.md", "", "" >> out
            else { print "MODULE", rel >> un; bad++ }
        } else
            print "DATA", rel >> un
    }
    END { print bad + 0 }
' "$work/owners" "$work/metadata" "$work/digests")

rows=$(($(wc -l < "$out.tsv") - 2))
echo "capture-sbom-msys2: $rows files recorded from $(wc -l < "$work/packages") packages -> $out.tsv"
echo "capture-sbom-msys2: $(wc -l < "$out.unattributed.txt") unattributed files listed in $out.unattributed.txt"
if (( bad > 0 )); then
    fail "$bad executable modules have no package and are not known VA Studio builds (see $out.unattributed.txt)"
fi
