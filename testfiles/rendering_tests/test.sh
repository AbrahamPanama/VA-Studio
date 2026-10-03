#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later

if [ -n "${VACARDS_TEST_DYLD_LIBRARY_PATH:-}" ]; then
    export DYLD_LIBRARY_PATH="$VACARDS_TEST_DYLD_LIBRARY_PATH"
fi

MY_LOCATION=$(dirname "$0")
source "${MY_LOCATION}/../utils/functions.sh"

ensure_command "compare"
ensure_command "bc"

if [ "$#" -lt 2 ]; then
    echo "Pass the path of the inkscape executable as parameter then the name of the test" $#
    exit 1
fi

INKSCAPE_EXE="$1"
TEST="$2"
FUZZ="$3"
EXIT_STATUS=0
EXPECTED="$(dirname "$TEST")/expected_rendering/$(basename "$TEST")"
TESTNAME="$(basename "$TEST")"
export LC_NUMERIC=C
export INKSCAPE_FONTCONFIG="${MY_LOCATION}/fonts/isolated.conf"

if [ "$FUZZ" = "" ]; then
    METRIC="AE"
else
    METRIC="RMSE"
fi

perform_test()
{
    local SUFFIX="$1"
    local DPI="$2"
    local OUTPUT="${TESTNAME}${SUFFIX}.png"
    local DIFFERENCE="${TESTNAME}-compare${SUFFIX}.png"
    local REFERENCES=("${EXPECTED}${SUFFIX}.png")
    local REFERENCE
    local REFERENCE_NAME
    local FAILURE_DETAILS=()

    # A rendering can legitimately differ between shaping stacks while still
    # being deterministic for each stack. Keep those known-good images as
    # explicit references instead of weakening the comparison tolerance.
    shopt -s nullglob
    for REFERENCE in "${EXPECTED}${SUFFIX}".reference-*.png; do
        REFERENCES+=("$REFERENCE")
    done
    shopt -u nullglob

    if ! "${INKSCAPE_EXE}" --export-png-use-dithering false \
            --export-filename="$OUTPUT" -d "$DPI" "${TEST}.svg"; then
        echo "${TESTNAME}${SUFFIX} FAILED; Inkscape did not complete the rendering command."
        EXIT_STATUS=1
        return
    fi

    if [ ! -s "$OUTPUT" ]; then
        echo "${TESTNAME}${SUFFIX} FAILED; Inkscape produced no rendering output."
        EXIT_STATUS=1
        return
    fi

    for REFERENCE in "${REFERENCES[@]}"; do
        REFERENCE_NAME="$(basename "$REFERENCE")"
        COMPARE_OUTPUT="$(compare -metric "$METRIC" "$OUTPUT" "$REFERENCE" "$DIFFERENCE" 2>&1)"

        if [ "$FUZZ" = "" ]; then
            if [ "$COMPARE_OUTPUT" = 0 ] || [ "$COMPARE_OUTPUT" = "0 (0)" ]; then
                echo "${TESTNAME}${SUFFIX}" "PASSED against ${REFERENCE_NAME}; absolute difference is exactly zero."
                rm -f "$OUTPUT" "$DIFFERENCE"
                return
            fi
            FAILURE_DETAILS+=("${REFERENCE_NAME}: absolute difference ${COMPARE_OUTPUT}")
        else
            RELATIVE_ERROR=$(get_compare_result "$COMPARE_OUTPUT")
            PERCENTAGE_ERROR=$(fraction_to_percentage "$RELATIVE_ERROR")
            if (( $(is_relative_error_within_tolerance "$RELATIVE_ERROR" "$FUZZ") )); then
                echo "${TESTNAME}${SUFFIX}" "PASSED against ${REFERENCE_NAME}; error of ${PERCENTAGE_ERROR}% is within ${FUZZ}% tolerance."
                rm -f "$OUTPUT" "$DIFFERENCE"
                return
            fi
            FAILURE_DETAILS+=("${REFERENCE_NAME}: ${PERCENTAGE_ERROR}%")
        fi
    done

    echo "${TESTNAME}${SUFFIX} FAILED; no explicit reference matched (tolerance: ${FUZZ:-0}%)."
    printf '  %s\n' "${FAILURE_DETAILS[@]}"
    EXIT_STATUS=1
}

perform_test "" 96

if [ -f "${EXPECTED}-large.png" ]; then
    perform_test "-large" 384
else
    echo "${TESTNAME}-large" "SKIPPED"
fi

exit $EXIT_STATUS
