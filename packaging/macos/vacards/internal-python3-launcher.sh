#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu
macos_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
resources_dir=$(CDPATH= cd -- "$macos_dir/../Resources" && pwd)
export PYTHONHOME="$resources_dir/python"
export PYTHONPATH="$resources_dir/share/inkscape/extensions"
export PYTHONNOUSERSITE=1
export PYTHONDONTWRITEBYTECODE=1
exec "$macos_dir/python3-bin" "$@"
