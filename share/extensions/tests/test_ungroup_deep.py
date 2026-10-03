# coding=utf-8
from ungroup_deep import UngroupDeep
from inkex.tester import ComparisonMixin, TestCase
from inkex.tester.filters import CompareOrderIndependentStyle


class TestUngroupBasic(ComparisonMixin, TestCase):
    effect_class = UngroupDeep
    compare_filters = [CompareOrderIndependentStyle()]
    comparisons = [(), ("--id=layer2",)]


class TestUngroupComplex(ComparisonMixin, TestCase):
    effect_class = UngroupDeep
    compare_filters = [CompareOrderIndependentStyle()]
    compare_file = "svg/deep-ungroup-complex.svg"
    comparisons = [
        # first one: Paths with clip-path:none (https://gitlab.com/inkscape/extensions/-/issues/184#note_490847336)
        # second one: Paths with nested transforms (https://gitlab.com/inkscape/extensions/-/issues/340)
        # third one: Transformed group with transformed clip-path, https://gitlab.com/inkscape/extensions/-/issues/184
        ("--id=g1935", "--id=g6577", "--id=g115")
    ]


class TestUngroupComments(ComparisonMixin, TestCase):
    effect_class = UngroupDeep
    compare_filters = [CompareOrderIndependentStyle()]
    compare_file = "svg/ellipse_group_comment.svg"
    comparisons = [
        # Groups with comment child elements (https://gitlab.com/inkscape/extensions/-/issues/405)
        ("--id=g13",)
    ]


class TestUngroupLayers(ComparisonMixin, TestCase):
    effect_class = UngroupDeep
    compare_filters = [CompareOrderIndependentStyle()]
    compare_file = "svg/layergroups.svg"
    comparisons = [("--preserve_layers=True",)]
