#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Synthetic policy tests only: no Windows linker, DLL execution or Cairo build.
set -euo pipefail
script_dir=$(cd -- "$(dirname -- "$0")" && pwd -P)
exec python3 "$script_dir/vacards-windows-cairo-test.py" "$@"
