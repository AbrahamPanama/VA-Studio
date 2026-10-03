#! /bin/sh

set -e

echo "Generating updated POTFILES list..."
mydir=`dirname "$0"`
cd "$mydir"

# enforce consistent sort order and date format
export LC_ALL=C

(
 echo "\${_build_dir}/share/filters/filters.svg.h"
 echo "\${_build_dir}/share/markers/markers.svg.h"
 echo "\${_build_dir}/share/palettes/palettes.h"
 echo "\${_build_dir}/share/paint/patterns.svg.h"
 echo "\${_build_dir}/share/symbols/symbols.h"
 echo "\${_build_dir}/share/templates/templates.h"

 find ../src \( -name '*.cpp' -o -name '*.[ch]' \) -type f -print0 | xargs -0 egrep -l '(\<[QNC]?_|gettext) *\(' | sort

) | grep -vx -f POTFILES.skip > POTFILES.src.in


find ../share/extensions -name '*.py' -type f -print0 | xargs -0 egrep -l '(\<[QNC]?_|gettext) *\(' | grep -vx -f POTFILES.skip | sort > POTFILES.py.in
find ../share/extensions -name '*.inx' -type f -print | grep -vx -f POTFILES.skip | sort > POTFILES.inx.in

# Keep .glade files and .ui files separated.
(
    find ../share/ui -name  '*.glade' -type f -print0 | xargs -0 egrep -l 'translatable' | sort
    find ../share/ui -name  '*.ui'    -type f -print0 | xargs -0 egrep -l 'translatable' | sort
) | grep -vx -f POTFILES.skip > POTFILES.ui.in
