# -*- coding: utf-8 -*-
import pytest
from clean_up_path import CleanUpPath
import inkex
from inkex.tester import ComparisonMixin, TestCase
from inkex.tester.filters import CompareNumericFuzzy, CompareWithPathSpace
from inkex.tester.mock import Capture


class RemoveTrueDuplicatesTest(ComparisonMixin, TestCase):
    effect_class = CleanUpPath
    comparisons = [("--id=path_w_duplicates",)]
    compare_filters = [
        CompareWithPathSpace(),
        CompareNumericFuzzy(),
    ]
    compare_file = "svg/clean_up_path.svg"


class RemoveClose2DuplicatesTest(ComparisonMixin, TestCase):
    effect_class = CleanUpPath
    comparisons = [
        (
            "--id=path_line",
            "--minUse=True",
            "--minlength=0.1",
            "--minUseSub=True",
            "--minlengthSub=0.1",
        )
    ]
    compare_filters = [
        CompareWithPathSpace(),
        CompareNumericFuzzy(),
    ]
    compare_file = "svg/clean_up_path.svg"


class SimpleJoinLoopTest(ComparisonMixin, TestCase):
    effect_class = CleanUpPath
    comparisons = [
        (
            "--id=path_simple_loop",
            "--joinEndSub=True",
            "--maxdistJoin=4",
        )
    ]
    compare_filters = [
        CompareWithPathSpace(),
        CompareNumericFuzzy(),
    ]
    compare_file = "svg/clean_up_path.svg"


class TwoCirclesDefaultTest(ComparisonMixin, TestCase):
    effect_class = CleanUpPath
    comparisons = [("--id=two_circles",)]
    compare_filters = [
        CompareWithPathSpace(),
        CompareNumericFuzzy(),
    ]
    compare_file = "svg/clean_up_path.svg"


class SelfIntersectTest(ComparisonMixin, TestCase):
    effect_class = CleanUpPath
    comparisons = [
        (
            "--id=self_intersect",
            "--joinEndSub=True",
            "--maxdistJoin=4",
        )
    ]
    compare_filters = [
        CompareWithPathSpace(),
        CompareNumericFuzzy(),
    ]
    compare_file = "svg/clean_up_path.svg"


class LoopCloseTest(ComparisonMixin, TestCase):
    effect_class = CleanUpPath
    comparisons = [
        (
            "--id=path_oo",
            "--closeSub=True",
            "--maxdistClose=1",
        )
    ]
    compare_filters = [
        CompareWithPathSpace(),
        CompareNumericFuzzy(),
    ]
    compare_file = "svg/clean_up_path.svg"


class MixDuplicateTest(ComparisonMixin, TestCase):
    effect_class = CleanUpPath
    comparisons = [
        (
            "--id=path_zmo",
            "--minUse=True",
            "--minlength=1",
        )
    ]
    compare_filters = [
        CompareWithPathSpace(),
        CompareNumericFuzzy(),
    ]
    compare_file = "svg/clean_up_path.svg"


class JoinRowTest(ComparisonMixin, TestCase):
    effect_class = CleanUpPath
    comparisons = [
        (
            "--id=path_loops_and_row",
            "--closeSub=True",
            "--maxdistJoin=4",
            "--joinEndSub=True",
            "--maxdistClose=4",
        )
    ]
    compare_filters = [
        CompareWithPathSpace(),
        CompareNumericFuzzy(),
    ]
    compare_file = "svg/clean_up_path.svg"


class JoinRowNoReverseTest(ComparisonMixin, TestCase):
    effect_class = CleanUpPath
    comparisons = [
        (
            "--id=path_loops_and_row",
            "--closeSub=True",
            "--maxdistClose=4",
            "--joinEndSub=True",
            "--maxdistJoin=4",
            "--allow_reverse=False",
        )
    ]
    compare_filters = [
        CompareWithPathSpace(),
        CompareNumericFuzzy(),
    ]
    compare_file = "svg/clean_up_path.svg"


class MixTest(ComparisonMixin, TestCase):
    effect_class = CleanUpPath
    comparisons = [
        (
            "--id=path_mix",
            "--minUse=True",
            "--minlength=2",
            "--closeSub=True",
            "--maxdistClose=4",
            "--joinEndSub=True",
            "--maxdistJoin=4",
        )
    ]
    compare_filters = [
        CompareWithPathSpace(),
        CompareNumericFuzzy(),
    ]
    compare_file = "svg/clean_up_path.svg"


class MixStraightTest(ComparisonMixin, TestCase):
    effect_class = CleanUpPath
    comparisons = [
        (
            "--id=path_mix",
            "--joinEndSub=True",
            "--maxdistJoin=4",
            "--option_join=2",
        )
    ]
    compare_filters = [
        CompareWithPathSpace(),
        CompareNumericFuzzy(),
    ]
    compare_file = "svg/clean_up_path.svg"


class RaiseErrorsTest(TestCase):
    """Test errormsg on selecting not accepted elements."""

    def test_effect(self):
        """Test that error is presented when inkscape:path-effect selected."""
        args = [
            self.data_file("svg", "clean_up_path.svg"),
            "--id=ink_effect",
        ]
        ext = CleanUpPath()

        with Capture("stderr") as stderr:
            ext.run(args)
            self.assertIn(
                "inkscape:path-effect applied",
                stderr.getvalue(),
            )


@pytest.mark.parametrize(
    "min_length, min_length_sub, data, compare",
    [
        (
            4,
            -1,
            """m 89.954197,135.32043
            c 4.76877,7.15315 -1.779916,-2.85251 2.383834,4.08709 
            0.292056,0.48676 0.669811,0.91764 1.02216,1.36271 
            3.886324,4.90904 6.775989,9.17581 12.261269,12.26126 
            0.7536,0.4239 1.58962,0.68106 2.38435,1.02165
            z""",
            """M 89.9542 135.32 
            C 94.723 142.474 88.1743 132.468 92.8491 140.089 
            C 97.2465 145.679 100.136 149.946 108.006 154.053 
            Z""",
        ),
        (
            1,
            -1,
            """m 194.65522,181.84827
            c -0.0317,0.004 -0.0636,0.009 -0.0955,0.0155 
            -0.0319,0.006 -0.064,0.0138 -0.096,0.0224 
            -0.0321,0.009 -0.0642,0.0183 -0.0963,0.029 
            -0.0321,0.0108 -0.0642,0.0226 -0.0962,0.0354 
            -0.032,0.0128 -0.064,0.0266 -0.0959,0.0414 
            -0.0319,0.0148 -0.0636,0.0305 -0.0952,0.0471 
            -0.0316,0.0166 -0.0631,0.0342 -0.0943,0.0526 
            -0.0312,0.0184 -0.0623,0.0377 -0.093,0.0577 
            -0.24616,0.16052 -0.47576,0.3727 -0.66325,0.61114 
            -0.37497,0.47686 -0.5815,1.05873 -0.41512,1.54227 
            0.8274,2.40463 6.20905,4.26009 7.36369,1.99434 
            0.53667,-1.05311 -0.25762,-2.23877 -1.47928,-3.12444 
            -0.61083,-0.44283 -1.3285,-0.81067 -2.04006,-1.04944 
            -0.35578,-0.11938 -0.71004,-0.20649 -1.04865,-0.25459 
            -0.0423,-0.006 -0.0844,-0.0114 -0.12621,-0.0162 
            -0.0418,-0.005 -0.0833,-0.009 -0.12459,-0.0125 
            -0.0412,-0.004 -0.0822,-0.006 -0.12279,-0.009 
            -0.0406,-0.002 -0.0809,-0.004 -0.12082,-0.005 
            -0.0399,-9.2e-4 -0.0795,-0.001 -0.1187,-7.6e-4 
            -0.0392,4.2e-4 -0.078,0.002 -0.1164,0.003 
            -0.0384,0.002 -0.0764,0.004 -0.11394,0.007 
            -0.0375,0.003 -0.0747,0.007 -0.11132,0.0116
            z""",
            """M 194.655 181.848 
            C 193.647 182.31 193.417 182.522 193.23 182.761 
            C 192.855 183.237 192.648 183.819 192.814 184.303 
            C 193.642 186.707 199.023 188.563 200.178 186.297 
            C 200.715 185.244 199.921 184.058 198.699 183.173 
            C 198.088 182.73 197.37 182.362 196.659 182.123 
            C 196.303 182.004 195.949 181.917 194.655 181.847 
            Z""",
        ),
    ],
)
def test_duplicate_nodes(min_length, min_length_sub, data, compare):
    ext = CleanUpPath()
    ext.min_length = min_length
    ext.min_length_sub = min_length_sub
    res = ext.remove_minlength_nodes(inkex.Path(data).to_absolute().to_non_shorthand())
    assert str(res) == str(inkex.Path(compare)), str(res)
