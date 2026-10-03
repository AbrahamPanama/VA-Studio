#! /bin/sh

test -z "$CSSLINT" && . ./global-test-vars.sh

$CSSLINT "$TEST_INPUTS_DIR"/several-media.css
