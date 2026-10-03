# coding=utf-8
from distribute_along_path import DistributeAlongPath
from inkex.tester import ComparisonMixin, InkscapeExtensionTestMixin, TestCase
from inkex.tester.filters import CompareWithoutIds


class TestPathScatterBasic(ComparisonMixin, InkscapeExtensionTestMixin, TestCase):
    effect_class = DistributeAlongPath
    compare_file = "svg/scatter.svg"
    comparisons = [
        # Test simple case
        ("--id=g12668", "--id=path8143", "--stretch=False", "--follow=False"),
        # Test follow and stretch of a path around a skeleton with multiple closed subpaths
        (
            "--id=path3990",
            "--id=path3982",
            "--stretch=True",
            "--follow=True",
            "--copymode=copy",
        ),
        # Test cloning and rotating
        (
            "--id=g12668",
            "--id=path8143",
            "--stretch=True",
            "--rotate=True",
            "--copymode=clone",
        ),
        # Test picking from a group pattern
        (
            "--id=g12668",
            "--id=path8143",
            "--stretch=True",
            "--copymode=copy",
            "--grouppick=True",
            "--pickmode=seq",
        ),
        # Test stretch and spac
        ("--id=g12668", "--id=path8143", "--stretch=True", "--space=10"),
    ]
    compare_filters = [CompareWithoutIds()]
