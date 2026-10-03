# coding=utf-8
from image_embed import EmbedImage
from inkex.tester import ComparisonMixin, TestCase


class EmbedderBasicTest(ComparisonMixin, TestCase):
    effect_class = EmbedImage
    compare_file = "svg/images.svg"
    comparisons = [
        (),
        (
            "--id=image174",
            "--selectedonly=True",
        ),
    ]
