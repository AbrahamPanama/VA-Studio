#!/usr/bin/env python3
from webslicer_create_rect import CreateRect
from inkex.tester import ComparisonMixin, TestCase


class TestWebSlicerCreateRectBasic(ComparisonMixin, TestCase):
    effect_class = CreateRect
