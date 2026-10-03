# Copyright (C) 2024 Jonathan Neuhauser <jonathan.neuhauser@outlook.com>
# SPDX-License-Identifier: GPL-2.0-or-later

from inkaf.afinput import AFInput
from inkex.tester import ComparisonMixin, TestCase
from inkex.tester.filters import CompareOrderIndependentStyle


class TestAFConverter(ComparisonMixin, TestCase):
    """Run-through tests of AFConverter"""

    effect_class = AFInput
    compare_file = [
        "./shapes.afdesign",
        "./powerstroke.afdesign",
        "./blend_modes.afdesign",
        "./blur.afdesign",
        "./vector_crop.afdesign",
        "./text.afdesign",
        "./artboards_and_guides.afdesign",
    ]
    comparisons = [tuple()]
    compare_filters = [CompareOrderIndependentStyle()]
