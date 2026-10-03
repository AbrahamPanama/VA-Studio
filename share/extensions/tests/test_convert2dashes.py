# coding=utf-8
from convert2dashes import Dashit
from inkex.tester import ComparisonMixin, InkscapeExtensionTestMixin, TestCase
from inkex.tester.filters import CompareNumericFuzzy


class DashitBasicTest(ComparisonMixin, TestCase):
    comparisons = ([],)
    effect_class = Dashit

    def test_basic(self):
        args = ["--id=dashme", self.data_file("svg", "dash.svg")]
        self.effect.run(args)
        old_dashes = (
            self.effect.original_document.getroot().getElement("//svg:path").path
        )
        new_dashes = self.effect.svg.getElement("//svg:path").path
        assert len(new_dashes) > len(old_dashes)


class DashitCommaTest(ComparisonMixin, TestCase):
    comparisons = (["--id=dashme2"],)
    effect_class = Dashit
    compare_file = ["svg/dash.svg", "svg/dash_zerolength.svg"]


class DashitComplexTest(ComparisonMixin, TestCase):
    comparisons = (
        [
            "--id=nodes1",
            "--id=nodes2",
            "--id=nodes3",
            "--id=circle",
            "--id=offset1",
            "--id=offset2",
            "--id=closed1",
            "--id=closed2--id=shorthand1",
            "--id=multiple",
        ],
    )
    effect_class = Dashit
    compare_filters = [CompareNumericFuzzy()]
    compare_file = ["svg/dashes_examples.svg"]
