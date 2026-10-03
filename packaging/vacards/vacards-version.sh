#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later

set -eu
LC_ALL=C
export LC_ALL

usage()
{
    cat >&2 <<'EOF'
usage: vacards-version.sh [--file VERSION_FILE] OPTION

Options:
  --base       native compatibility version (for example 1.5.0)
  --name       product name
  --display    user-visible product version
  --build      monotonically increasing build number
  --edition    lowercase edition identifier
  --release    complete SemVer release (for example 1.5.0-vacards.1)
  --tag        canonical Git tag for this release
  --pe         Windows PE version: MAJOR,MINOR,PATCH,BUILD
  --msi        Windows MSI ProductVersion: MAJOR.MINOR.BUILD
  --env        print all derived values as key=value records
EOF
    exit 2
}

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
version_file=$script_dir/VERSION.env
case "$#" in
    1) option=$1 ;;
    3)
        [ "$1" = "--file" ] || usage
        version_file=$2
        option=$3
        ;;
    *) usage ;;
esac

fail()
{
    echo "Invalid VACards version: $*" >&2
    exit 1
}

read_value()
{
    key=$1
    awk -F= -v key="$key" '$1 == key {sub(/^[^=]*=/, ""); sub(/\r$/, ""); print; exit}' "$version_file"
}

[ -f "$version_file" ] || fail "missing $version_file"
# Some awk implementations truncate records at NUL before regex validation.
tr -d '\000' < "$version_file" | cmp -s "$version_file" - || fail "NUL byte in version file"
# Accept LF or CRLF, but never shell syntax, empty records or bare keys.
awk '{sub(/\r$/, ""); if ($0 ~ /[[:cntrl:]]/ || $0 !~ /^[a-z_]+=[^[:space:]=]+$/) exit 1}' \
    "$version_file" || fail "malformed key=value record"

for key in format base_version edition build_number; do
    count=$(awk -F= -v key="$key" '$1 == key {count++} END {print count + 0}' "$version_file")
    [ "$count" -eq 1 ] || fail "expected exactly one '$key' field"
done
unknown_fields=$(awk -F= '
    $1 != "format" && $1 != "base_version" &&
    $1 != "edition" && $1 != "build_number" &&
    $1 != "product_name" && $1 != "product_version" && $1 != "display_version" {print $1}
' "$version_file")
[ -z "$unknown_fields" ] || fail "unknown field(s): $unknown_fields"

format=$(read_value format)
base_version=$(read_value base_version)
edition=$(read_value edition)
build_number=$(read_value build_number)

[ "$format" = "1" ] || [ "$format" = "2" ] || fail "unsupported format '$format'"
printf '%s\n' "$base_version" | grep -Eq '^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$' ||
    fail "base_version must contain exactly three numeric SemVer components"
printf '%s\n' "$edition" | grep -Eq '^[a-z0-9]+([.-][a-z0-9]+)*$' ||
    fail "edition must be a lowercase SemVer identifier"
case "$build_number" in
    ''|0|*[!0-9]*) fail "build_number must be a positive integer" ;;
    0*) fail "build_number must not contain leading zeroes" ;;
esac

# Bound lengths before numeric comparisons so huge integers cannot overflow a
# shell or round into an accepted value. MSI limits major/minor to 255; every
# PE component and the MSI build field must fit in 16 bits.
bounded_component()
{
    [ "${#1}" -le "${#2}" ] && [ "$1" -le "$2" ] ||
        fail "$3 must not exceed $2"
}
major=${base_version%%.*}
remainder=${base_version#*.}
minor=${remainder%%.*}
patch=${remainder#*.}
bounded_component "$major" 255 "base_version major"
bounded_component "$minor" 255 "base_version minor"
bounded_component "$patch" 65535 "base_version patch"
bounded_component "$build_number" 65535 "build_number"

release_version=$base_version-$edition.$build_number
product_name='VA Studio'
display_version=$release_version
if [ "$format" = "2" ]; then
    for key in product_name product_version display_version; do
        count=$(awk -F= -v key="$key" '$1 == key {count++} END {print count + 0}' "$version_file")
        [ "$count" -eq 1 ] || fail "expected exactly one '$key' field"
    done
    product_name=$(read_value product_name)
    display_version=$(read_value display_version)
    release_version=$(read_value product_version)
    printf '%s\n' "$product_name" | grep -Eq '^[A-Za-z][A-Za-z0-9_]*$' || fail "invalid product_name"
    printf '%s\n' "$display_version" | grep -Eq '^[0-9][A-Za-z0-9._-]*$' || fail "invalid display_version"
    printf '%s\n' "$release_version" | grep -Eq '^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)-(beta|rc)\.[1-9][0-9]*$' || fail "invalid product_version"
    product_name=$(printf '%s' "$product_name" | tr '_' ' ')
    display_version=$(printf '%s' "$display_version" | tr '_' ' ')
else
    for key in product_name product_version display_version; do
        [ -z "$(read_value "$key")" ] || fail "$key requires format 2"
    done
fi
release_tag=v$release_version
pe_version=$major,$minor,$patch,$build_number
msi_product_version=$major.$minor.$build_number

case "$option" in
    --name) printf '%s\n' "$product_name" ;;
    --display) printf '%s\n' "$display_version" ;;
    --base)    printf '%s\n' "$base_version" ;;
    --build)   printf '%s\n' "$build_number" ;;
    --edition) printf '%s\n' "$edition" ;;
    --release) printf '%s\n' "$release_version" ;;
    --tag)     printf '%s\n' "$release_tag" ;;
    --pe)      printf '%s\n' "$pe_version" ;;
    --msi)     printf '%s\n' "$msi_product_version" ;;
    --env)
        printf 'format=%s\n' "$format"
        printf 'base_version=%s\n' "$base_version"
        printf 'edition=%s\n' "$edition"
        printf 'build_number=%s\n' "$build_number"
        printf 'product_name=%s\n' "$product_name"
        printf 'display_version=%s\n' "$display_version"
        printf 'release_version=%s\n' "$release_version"
        printf 'release_tag=%s\n' "$release_tag"
        printf 'pe_version=%s\n' "$pe_version"
        printf 'msi_product_version=%s\n' "$msi_product_version"
        ;;
    *) usage ;;
esac
