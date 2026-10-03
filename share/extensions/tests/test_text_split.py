# coding=utf-8
from inkex.tester.filters import CompareWithoutIds
from text_split import TextSplit
from inkex.tester import ComparisonMixin, TestCase
from inkex.tester.filters import CompareWithoutIds


class TestSplitBasic(ComparisonMixin, TestCase):
    """Test split effect"""

    effect_class = TextSplit
    compare_filters = [CompareWithoutIds()]
    compare_file = "svg/text_types.svg"
    all_shapes = (
        "--id=regular",
        "--id=regular-transform",
        "--id=inline-size",
        "--id=kerning",
        "--id=flowroot",
        "--id=flowroot-abs-lineheight",
        "--id=flowroot-no-lineheight",
        "--id=manual-kerns",
        "--id=rtl",
        "--id=shape-inside",
    )
    comparisons = [
        all_shapes + ("--splittype=line", "--preserve=True"),  # ad3188
        all_shapes + ("--splittype=line", "--preserve=False"),  # c242ad
        all_shapes + ("--splittype=word", "--preserve=True"),  # 547875
        all_shapes + ("--splittype=word", "--preserve=False"),  # d8b155
        all_shapes
        + ("--splittype=word", "--preserve=False", "--separation=0.0"),  # 897ab8
        all_shapes + ("--splittype=letter", "--preserve=True"),  # 74947d
        all_shapes + ("--splittype=letter", "--preserve=False"),  # dd77d3
    ]
    print("test")
