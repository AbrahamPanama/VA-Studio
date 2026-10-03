# coding=utf-8
from patternalongpath import PatternAlongPath
from inkex.tester import ComparisonMixin, TestCase
from inkex.tester.filters import CompareNumericFuzzy, CompareWithPathSpace


class TestPathAlongPathBasic(ComparisonMixin, TestCase):
    compare_file = "svg/pattern_along_path.svg"
    compare_filters = [CompareNumericFuzzy(), CompareWithPathSpace()]
    comparisons = [
        # Settings: Repeated, Snake, no distance. Tests a path with fillrule=evenodd
        ("--copymode=Repeated", "--kind=Snake", "--id=g3427", "--id=path2551"),
        # Settings: Repeated, Stretched, Ribbon, Vertical.
        # Tests a group pattern with multiple nested transforms
        (
            "--copymode=Repeated, stretched",
            "--kind=Ribbon",
            "--vertical=True",
            "--id=g3961",
            "--id=path10007",
        ),
        # Settings: Repeated, Stretched, Ribbon
        # Tests a group pattern with multiple nested clones
        (
            "--copymode=Repeated, stretched",
            "--kind=Ribbon",
            "--id=g4054",
            "--id=path4056",
        ),
        # Settings: Single, Stretched, Snake, not duplicated.
        # Tests putting a text (converted to a path) on a path and stretching it to fit on the
        # skeleton path
        (
            "--copymode=Single, stretched",
            "--kind=Snake",
            "--id=text4418",
            "--id=path4412",
        ),
        # Settings: Single, Stretched, Snake.
        # Tests selecting multiple sceleton paths, one consisting of multiple subpaths
        (
            "--copymode=Single, stretched",
            "--kind=Snake",
            "--id=path4585",
            "--id=path4608",
            "--id=path4610",
            "--id=path4612",
        ),
        # Settings: Repeated, Stretched, Snake, Space between copies=5, Normal offset=5.
        # Tests putting a path with multiple subpaths with a gradient on a closed path
        (
            "--copymode=Repeated, stretched",
            "--kind=Snake",
            "--noffset=5",
            "--space=5",
            "--id=path2408",
            "--id=path2405",
        ),
    ]
    effect_class = PatternAlongPath


class TestPathAlongPathCloneTransforms(ComparisonMixin, TestCase):
    """Tests for issue https://gitlab.com/inkscape/extensions/-/issues/241"""

    effect_class = PatternAlongPath
    compare_file = "svg/pattern_along_path_clone_transform.svg"
    comparisons = [
        # a clone with a transform in a group with a transform
        (
            "--copymode=Single",
            "--duplicate=True",
            "--kind=Snake",
            "--id=g5848",
            "--id=path3336",
        )
    ]
