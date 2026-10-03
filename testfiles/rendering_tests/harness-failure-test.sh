#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later

set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: $0 RENDERING_TEST_HARNESS" >&2
    exit 2
fi

harness_dir=$(CDPATH= cd -- "$(dirname -- "$1")" && pwd)
harness=$harness_dir/$(basename -- "$1")
scratch=$(mktemp -d "${TMPDIR:-/tmp}/vacards-render-harness.XXXXXX")
case "$scratch" in
    "${TMPDIR:-/tmp}"/vacards-render-harness.*) ;;
    *) echo "unexpected scratch directory: $scratch" >&2; exit 1 ;;
esac
cleanup()
{
    rm -rf "$scratch"
}
trap cleanup EXIT HUP INT TERM

mkdir -p "$scratch/case/expected_rendering"
: > "$scratch/case/sample.svg"

cat > "$scratch/renderer-crash" <<'EOF'
#!/bin/sh
exit 134
EOF
cat > "$scratch/renderer-no-output" <<'EOF'
#!/bin/sh
exit 0
EOF
chmod +x "$scratch/renderer-crash" "$scratch/renderer-no-output"

if (cd "$scratch" && bash "$harness" "$scratch/renderer-crash" "$scratch/case/sample" 0.1 >crash.log 2>&1); then
    echo "rendering harness accepted a crashed renderer" >&2
    exit 1
fi
grep -Fq 'Inkscape did not complete the rendering command' "$scratch/crash.log" || {
    cat "$scratch/crash.log" >&2
    echo "rendering harness did not report the crashed renderer" >&2
    exit 1
}

if (cd "$scratch" && bash "$harness" "$scratch/renderer-no-output" "$scratch/case/sample" 0.1 >missing.log 2>&1); then
    echo "rendering harness accepted a missing output image" >&2
    exit 1
fi
grep -Fq 'Inkscape produced no rendering output' "$scratch/missing.log" || {
    cat "$scratch/missing.log" >&2
    echo "rendering harness did not report the missing output image" >&2
    exit 1
}

echo "Rendering harness failure detection passed"
