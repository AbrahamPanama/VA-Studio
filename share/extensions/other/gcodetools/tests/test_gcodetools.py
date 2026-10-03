# coding=utf-8

import sys
import os

from gcodetools import Gcodetools
from inkex.tester import ComparisonMixin, InkscapeExtensionTestMixin, TestCase
from inkex.tester.filters import CompareOrderIndependentBytes

SETTINGS = (
    "--id=p1",
    "--max-area-curves=100",
    "--area-inkscape-radius=-10",
    "--area-tool-overlap=0",
    "--area-fill-angle=0",
    "--area-fill-shift=0",
    "--area-fill-method=0",
    "--area-fill-method=0",
    "--area-find-artefacts-diameter=5",
    "--area-find-artefacts-action=mark with an arrow",
    "--biarc-tolerance=1",
    "--biarc-max-split-depth=4",
    "--path-to-gcode-order=subpath by subpath",
    "--path-to-gcode-depth-function=d",
    "--path-to-gcode-sort-paths=false",
    "--Zscale=1",
    "--Zoffset=0",
    "--auto_select_paths=true",
    "--min-arc-radius=0.05000000074505806",
    "--comment-gcode-from-properties=false",
    "--create-log=false",
    "--add-numeric-suffix-to-filename=false",
    "--Zsafe=5",
    "--unit=G21 (All units in mm)",
    "--postprocessor= ",
)
FILESET = SETTINGS + (
    "--directory=/home",
    "--filename=output.ngc",
)


class TestGcodetoolsBasic(ComparisonMixin, InkscapeExtensionTestMixin, TestCase):
    stderr_protect = False
    effect_class = Gcodetools
    comparisons = [
        FILESET + ('--active-tab="area_fill"',),
        FILESET + ('--active-tab="area"',),
        FILESET + ('--active-tab="area_artefacts"',),
        FILESET + ('--active-tab="dxfpoints"',),
        FILESET + ('--active-tab="orientation"',),
        FILESET + ('--active-tab="tools_library"',),
        FILESET + ('--active-tab="lathe_modify_path"',),
        FILESET + ('--active-tab="offset"',),
        FILESET + ('--active-tab="plasma-prepare-path"',),
    ]
    compare_filters = [CompareOrderIndependentBytes()]
    compare_file_extension = "dxf"

    def test_all_comparisons(self):
        """
        gcodetools tries to write to a folder and filename specified
        on the command line, this needs to be handled carefully.
        """
        for tab in (
            ('--active-tab="path-to-gcode"',),
            # ('--active-tab="engraving"',),
            # ('--active-tab="graffiti"',),
            ('--active-tab="lathe"',),
        ):
            args = (
                SETTINGS
                + tab
                + (
                    "--directory={}".format(self.tempdir),
                    "--filename=output.ngc",
                )
            )
            outfile = os.path.join(self.tempdir, "output.ngc")
            self.assertCompare(self.compare_file, None, args, "output.ngc")


class TestGcodeToolsOrientationScientific(ComparisonMixin, TestCase):
    effect_class = Gcodetools
    compare_file = "svg/shapes.svg"
    comparisons = [
        (
            "--active-tab=orientation",
            "--Zsurface=0.00000000000001e-5",
            "--Zdepth=-9.71445146547012e-17",
            "--orientation-points-count=3",
        )
    ]
