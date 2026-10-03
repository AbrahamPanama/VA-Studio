#! /bin/bash

set -e

if [ "$#" -ne 2 ]; then
    echo "Usage: $0 EXE REF"
    exit 1
fi

EXE="$1" # The test binary
REF="$2" # The directory containing the reference files

"$EXE" -vX 2000 -vY 1000 > /dev/null

for ITEM in anisotropic hienglish himetric isotropic loenglish lometric text twips; do
    diff "test_mm_$ITEM.emf" "$REF/test_mm_${ITEM}_ref.emf"
    rm "test_mm_$ITEM.emf"
done
