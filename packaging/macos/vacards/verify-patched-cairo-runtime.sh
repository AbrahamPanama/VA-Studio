#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later

set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: $0 APP_BUNDLE" >&2
    exit 2
fi

app_bundle=$1
inkscape_executable=$app_bundle/Contents/MacOS/inkscape-bin
expected_library=$app_bundle/Contents/Resources/lib/libcairo.2.dylib
loader_cache=$app_bundle/Contents/Resources/gdk-pixbuf-loaders.cache.in

if [ ! -f "$expected_library" ]; then
    echo "patched Cairo library not found: $expected_library" >&2
    exit 1
fi
if [ ! -s "$loader_cache" ] || grep -F '/opt/homebrew' "$loader_cache" >/dev/null; then
    echo "the bundled GdkPixbuf loader cache is missing or references Homebrew" >&2
    exit 1
fi

trace_file=$(mktemp -t vacards-cairo-trace.XXXXXX)
trap 'rm -f "$trace_file"' EXIT HUP INT TERM

DYLD_PRINT_LIBRARIES=1 \
    "$inkscape_executable" --version \
    > /dev/null 2> "$trace_file"

if ! grep -F "$expected_library" "$trace_file" >/dev/null; then
    echo "the process did not load the VACards patched Cairo" >&2
    sed -n '/libcairo/p' "$trace_file" >&2
    exit 1
fi

if grep '/opt/homebrew/.*/libcairo\.2\.dylib' "$trace_file" >/dev/null; then
    echo "the process also loaded an external Homebrew Cairo" >&2
    sed -n '/libcairo/p' "$trace_file" >&2
    exit 1
fi

loaded_count=$(grep '/libcairo\.2\.dylib' "$trace_file" | sort -u | wc -l | tr -d ' ')
if [ "$loaded_count" -ne 1 ]; then
    echo "expected exactly one loaded libcairo.2.dylib, found $loaded_count" >&2
    sed -n '/libcairo/p' "$trace_file" >&2
    exit 1
fi

echo "Verified runtime Cairo: $expected_library"
