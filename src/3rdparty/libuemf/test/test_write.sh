#! /bin/bash

set -e

if [ "$#" -ne 4 ]; then
    echo "Usage: $0 EXE ARG OUT REF"
    exit 1
fi

EXE="$1" # The test binary
ARG="$2" # The argument to the test binary
OUT="$3" # The output file of the test binary
REF="$4" # The reference file the output should match

"$EXE" "$ARG" > /dev/null
diff "$OUT" "$REF"
rm "$OUT"
