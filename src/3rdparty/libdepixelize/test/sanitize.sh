#! /usr/bin/env bash

mkdir build
cmake -S .. -B build -GNinja -DCMAKE_BUILD_TYPE="Debug" -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer -O1 -fno-optimize-sibling-calls"
ninja -C build

BIN="build/src/depixelize-tracer/depixelize"

export ASAN_OPTIONS="detect_leaks=1"
export LSAN_OPTIONS="suppressions=suppr.txt"
echo "leak:popt" > suppr.txt

for f in data/*.png; do
    for opt in "-v" "-g" "-n" ""; do
        ${BIN} "$f" -o /dev/null $opt
    done
done
