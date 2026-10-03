#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Compatibility entry point; shared metadata lives in packaging/vacards.
set -eu
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec "$script_dir/../../vacards/vacards-dependencies.sh" "$@"
