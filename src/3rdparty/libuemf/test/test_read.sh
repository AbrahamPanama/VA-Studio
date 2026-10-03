#! /bin/bash

set -e

if [ "$#" -ne 3 ]; then
    echo "Usage: $0 EXE IN REF"
    exit 1
fi

EXE="$1" # The test binary
IN="$2" # The input file to the test binary
REF="$3" # The reference file the output should match

# NOTE! On Windows text files may differ by having \n\r instead of \n.
"$EXE" "$IN" | diff --strip-trailing-cr - "$REF"
