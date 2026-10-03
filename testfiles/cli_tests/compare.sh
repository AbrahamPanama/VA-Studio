#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later

MY_LOCATION=$(dirname "$0")
source "${MY_LOCATION}/../utils/functions.sh"

OUTPUT_FILENAME=$1
REFERENCE_FILENAME=$2
FUZZ_PERCENTAGE=$3

get_outputs

test -f "${OUTPUT_FILENAME}" || { echo "compare.sh: First file '${OUTPUT_FILENAME}' not found."; exit 1; }
test -f "${REFERENCE_FILENAME}" || { echo "compare.sh: Second file '${REFERENCE_FILENAME}' not found."; exit 1; }

filter_output()
{
    sed -e "s/LMSans..-......./'Latin Modern Sans'/" \
        -e '/inkscape:version/d' \
        -e '/sodipodi:namedview/,/>/d' \
        "$1"
}

filter_reference()
{
    sed -e '/inkscape:version/d' \
        -e '/sodipodi:namedview/,/>/d' \
        "$1"
}

if cmp <(filter_output "${OUTPUT_FILENAME}") <(filter_reference "${REFERENCE_FILENAME}"); then
    exit 0
fi

if [ -n "${FUZZ_PERCENTAGE}" ]; then
    TEMP_DIRECTORY=$(mktemp -d "${TMPDIR:-/tmp}/inkscape-svg-compare.XXXXXX") || exit 1
    trap 'rm -rf "${TEMP_DIRECTORY}"' EXIT

    if bash "${MY_LOCATION}/check_output.sh" \
        "${OUTPUT_FILENAME}" "" "${REFERENCE_FILENAME}" "" "" \
        "${TEMP_DIRECTORY}/output.png" "${FUZZ_PERCENTAGE}" 96 1
    then
        echo "compare.sh: XML differs, but rendered output is within ${FUZZ_PERCENTAGE}% tolerance."
        exit 0
    fi
fi

echo "compare.sh: Files '${OUTPUT_FILENAME}' and '${REFERENCE_FILENAME}' are not identical'."
keep_outputs
exit 1
