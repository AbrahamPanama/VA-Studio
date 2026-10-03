#!/bin/bash

START=$PWD
TMP=$START/output.tmp

poetry show -n --tree --no-dev --no-ansi > $TMP

for dir in other/*; do
    if [ -d "$dir" ]; then
        cd $dir
        # We don't know if the other dir is locked or not
        poetry lock > /dev/null
        poetry show -n --tree --no-dev --no-ansi >> $TMP
        cd $START
    fi
done

python3 docs/poetry-parse.py < $TMP

rm $TMP
