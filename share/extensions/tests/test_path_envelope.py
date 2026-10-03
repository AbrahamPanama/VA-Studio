# coding=utf-8

from path_envelope import Envelope
from inkex.tester import ComparisonMixin, TestCase


class PathEnvelopeTest(ComparisonMixin, TestCase):
    """Test envelope similar to perspective"""

    effect_class = Envelope
    comparisons = [("--id=text", "--id=envelope")]
    compare_file = "svg/perspective.svg"


class PathEnvelopeGroupTest(ComparisonMixin, TestCase):
    effect_class = Envelope
    comparisons = [("--id=obj", "--id=envelope")]
    compare_file = "svg/perspective_groups.svg"
