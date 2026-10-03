# coding=utf-8
"""
Tests for DPISwitcher extensions

python3 -m pytest tests/test_dpiswitcher.py

Generate references:
python3 dpiswitcher.py tests/data/svg/shapes.svg > tests/data/refs/dpiswitcher.out
python3 dpiswitcher.py --switcher=0 tests/data/svg/dpiswitcher_96dpi.svg >
        tests/data/refs/dpiswitcher__--switcher__0.out
python3 dpiswitcher.py --switcher=1 tests/data/svg/dpiswitcher_96dpi.svg >
        tests/data/refs/dpiswitcher__--switcher__1.out

"""

from dpiswitcher import DPISwitcher
from inkex.tester import ComparisonMixin, TestCase
from inkex.tester.filters import CompareNumericFuzzy


class TestDPISwitcherBasic(ComparisonMixin, TestCase):
    """Default Test with shapes.svg"""

    effect_class = DPISwitcher
    compare_filters = [CompareNumericFuzzy()]


class TestDPIto90to96(ComparisonMixin, TestCase):
    """Test file with transformed objects in root"""

    compare_file = "svg/dpiswitcher_96dpi.svg"
    comparisons = [("--switcher=0",), ("--switcher=1",)]
    compare_filters = [CompareNumericFuzzy()]
    effect_class = DPISwitcher
