from cgm_input import CgmInput
from inkex.tester import ComparisonMixin, TestCase
from inkex.tester.filters import CompareOrderIndependentStyle


class TestCGMFileBasic(ComparisonMixin, TestCase):
    """Run-through tests of CGM"""

    effect_class = CgmInput
    compare_file = [
        # Abydos.cgm from https://snisurset.net/code/abydos/samples.html, GPL2+
        "cgm/abydos.cgm",
    ]
    comparisons = [tuple()]  # in mm
    compare_filters = [CompareOrderIndependentStyle()]
