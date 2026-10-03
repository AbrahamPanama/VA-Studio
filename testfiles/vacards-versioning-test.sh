#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later

set -eu

if [ "$#" -ne 3 ]; then
    echo "usage: $0 VERSION_TOOL VERSION_FILE VERSION_CMAKE" >&2
    exit 2
fi

version_tool=$1
version_file=$2
version_cmake=$3
project_root=$(CDPATH= cd -- "$(dirname -- "$version_cmake")/.." && pwd)
test_root=$(mktemp -d "${TMPDIR:-/tmp}/vacards-version-test.XXXXXX")
trap 'rm -rf "$test_root"' EXIT HUP INT TERM
# Copy the sole parser, even when the caller supplies the compatibility wrapper.
cp "$project_root/packaging/vacards/vacards-version.sh" "$test_root/vacards-version.sh"
chmod +x "$test_root/vacards-version.sh"

assert_equal()
{
    expected=$1
    actual=$2
    [ "$actual" = "$expected" ] || {
        echo "expected '$expected', got '$actual'" >&2
        exit 1
    }
}

write_version()
{
    format=$1
    base=$2
    edition=$3
    build=$4
    printf 'format=%s\nbase_version=%s\nedition=%s\nbuild_number=%s\n' \
        "$format" "$base" "$edition" "$build" > "$test_root/VERSION.env"
}

cp "$version_file" "$test_root/VERSION.env"
source_release=$("$version_tool" --file "$version_file" --release)
printf '%s\n' "$source_release" | grep -Eq \
    '^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)-[a-z0-9.-]+\.(0|[1-9][0-9]*)$'
assert_equal "$source_release" "$("$test_root/vacards-version.sh" --release)"

write_version 1 2.3.4 vacards 42
assert_equal 2.3.4-vacards.42 "$("$test_root/vacards-version.sh" --release)"
assert_equal v2.3.4-vacards.42 "$("$test_root/vacards-version.sh" --tag)"
assert_equal 42 "$("$test_root/vacards-version.sh" --build)"
assert_equal 2,3,4,42 "$("$test_root/vacards-version.sh" --pe)"
assert_equal 2.3.42 "$("$test_root/vacards-version.sh" --msi)"
assert_equal 2.3.4-vacards.42 \
    "$("$version_tool" --file "$test_root/VERSION.env" --release)"

# Product branding is independent of the native upgrade/ABI version.
write_version 2 1.5.0 vacards 8
printf 'product_name=VA_Studio\nproduct_version=1.0.0-beta.2\ndisplay_version=1.0_beta_2\n' >> "$test_root/VERSION.env"
cp "$test_root/VERSION.env" "$test_root/valid-beta2.env"
assert_equal 'VA Studio' "$("$test_root/vacards-version.sh" --name)"
assert_equal '1.0 beta 2' "$("$test_root/vacards-version.sh" --display)"
assert_equal '1.0.0-beta.2' "$("$test_root/vacards-version.sh" --release)"
assert_equal '1.5.0' "$("$test_root/vacards-version.sh" --base)"
assert_equal '1,5,0,8' "$("$test_root/vacards-version.sh" --pe)"
assert_equal '1.5.8' "$("$test_root/vacards-version.sh" --msi)"
for invalid_field in 'product_name=Other' 'product_version=1.0.0-beta.2' 'display_version=invalid'
do
    cp "$test_root/valid-beta2.env" "$test_root/VERSION.env"
    printf '%s\n' "$invalid_field" >> "$test_root/VERSION.env"
    if "$test_root/vacards-version.sh" --release >/dev/null 2>&1; then
        echo "duplicate product field accepted: $invalid_field" >&2
        exit 1
    fi
done
for invalid_product in '1.0-beta.2' '01.0.0-beta.2' '1.0.0-beta.02' '1.0.0;exit' '1.0.0-beta.2/evil'
do
    sed "s|product_version=.*|product_version=$invalid_product|" "$test_root/valid-beta2.env" > "$test_root/VERSION.env"
    if "$test_root/vacards-version.sh" --release >/dev/null 2>&1; then
        echo "invalid product version accepted: $invalid_product" >&2
        exit 1
    fi
done

for invalid in \
    '2 1.5.0 vacards 1' \
    '1 1.5 vacards 1' \
    '1 01.5.0 vacards 1' \
    '1 1.5.0 VACards 1' \
    '1 1.5.0 vacards 0' \
    '1 1.5.0 vacards 01' \
    '1 1.5.0 vacards 65536' \
    '1 256.5.0 vacards 1' \
    '1 1.256.0 vacards 1' \
    '1 1.5.65536 vacards 1'
do
    # The fixture fields are deliberately whitespace-separated and contain no
    # shell metacharacters.
    set -- $invalid
    write_version "$1" "$2" "$3" "$4"
    if "$test_root/vacards-version.sh" --release >/dev/null 2>&1; then
        echo "invalid version was accepted: $invalid" >&2
        exit 1
    fi
done

printf 'format=1\nbase_version=1.5.0\nedition=vacards\nbuild_number=1\nextra=value\n' \
    > "$test_root/VERSION.env"
if "$test_root/vacards-version.sh" --release >/dev/null 2>&1; then
    echo "version with an unknown field was accepted" >&2
    exit 1
fi

printf 'format=1\nbase_version=1.5.0\nedition=vacards\nbuild_number=1\nbuild_number=2\n' \
    > "$test_root/VERSION.env"
if "$test_root/vacards-version.sh" --release >/dev/null 2>&1; then
    echo "version with a duplicate field was accepted" >&2
    exit 1
fi

# The release attestation uses a ten-character source identifier. Exercise the
# real CMake generator in a repository whose local Git configuration asks for
# seven-character abbreviations, proving that the generated executable version
# cannot vary with clone- or runner-local core.abbrev settings.
fixture_repo=$test_root/version-repo
fixture_build=$test_root/version-build
mkdir -p "$fixture_repo/src" "$fixture_build/src"
cp "$project_root/src/inkscape-version.cpp.in" "$fixture_build/src/inkscape-version.cpp.in"
printf '%s\n' 'int vacards_version_fixture;' > "$fixture_repo/src/fixture.cpp"

fixture_git()
{
    (
        unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE
        git -C "$fixture_repo" "$@"
    )
}

fixture_git init -q
fixture_git config user.name 'VACards Test'
fixture_git config user.email 'vacards-test@example.invalid'
fixture_git config core.abbrev 7
fixture_git add src/fixture.cpp
fixture_git commit -q -m 'Version fixture'

default_short=$(fixture_git rev-parse --short HEAD)
[ "${#default_short}" -eq 7 ] || {
    echo "version fixture did not reproduce a seven-character Git abbreviation" >&2
    exit 1
}
expected_short=$(fixture_git rev-parse HEAD | cut -c1-10)
(
    unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE
    cmake -DINKSCAPE_SOURCE_DIR="$fixture_repo" \
          -DINKSCAPE_BINARY_DIR="$fixture_build" \
          -P "$version_cmake"
) > "$test_root/version-cmake.log" 2>&1 || {
    cat "$test_root/version-cmake.log" >&2
    exit 1
}
grep -Fq "revision_string = \"$expected_short\";" \
    "$fixture_build/src/inkscape-version.cpp" || {
    cat "$fixture_build/src/inkscape-version.cpp" >&2
    echo "generated revision does not use the required ten-character identifier" >&2
    exit 1
}

echo "VACards versioning contract passed"
