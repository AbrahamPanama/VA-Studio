#! /bin/sh

test -z "$CSSLINT" && . ./global-test-vars.sh

$CSSLINT "$TEST_INPUTS_DIR"/unknown-at-rule2.css
