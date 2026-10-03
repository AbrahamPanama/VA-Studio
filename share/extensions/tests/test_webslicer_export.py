#!/usr/bin/env python3
from webslicer_export import Export
from inkex.tester import ComparisonMixin, TestCase


class TestWebSlicerExportBasic(ComparisonMixin, TestCase):
    stderr_protect = False  # Cover lack of ImageMagic in CI builder
    effect_class = Export

    @property
    def comparisons(self):
        return [("--dir", self.tempdir)]
