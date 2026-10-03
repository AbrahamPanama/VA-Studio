# coding=utf-8
from image_attributes import ImageAttributes
from inkex.tester import ComparisonMixin, TestCase


class TestSetAttrImageBasic(ComparisonMixin, TestCase):
    effect_class = ImageAttributes
    compare_file = "svg/images.svg"
    comparisons = [
        (),  # All images in the document (basic)
        ("--id=image174", "--aspect_ratio=xMinYMin", '--tab="tab_aspect_ratio"'),
        (
            "--id=embeded_image01",
            "--image_rendering=optimizeSpeed",
            '--tab="tab_image_rendering"',
        ),
    ]
