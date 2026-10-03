#!/usr/bin/env python3
from webslicer_create_group import CreateGroup
from inkex.tester import ComparisonMixin, TestCase


class TestWebSlicerCreateGroupBasic(ComparisonMixin, TestCase):
    effect_class = CreateGroup
    comparisons = [("--id", "slicerect1")]
