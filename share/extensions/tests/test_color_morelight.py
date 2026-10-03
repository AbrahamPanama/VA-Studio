# coding=utf-8
from color_morelight import MoreLight
from .test_inkex_extensions import ColorBaseCase


class ColorMoreLightTest(ColorBaseCase):
    effect_class = MoreLight
    color_tests = [
        ("none", "none"),
        ("hsl(0, 0, 0)", "hsl(0, 0, 5)"),
        ("hsl(255, 255, 255)", "hsl(255, 100, 100)"),
        ((0, 0, 0), "#0d0d0d"),
        ((255, 255, 255), "#ffffff"),
        ((192, 192, 192), "#cccccc"),
        ((128, 128, 128), "#8c8c8c"),
        ((128, 0, 0), "#990000"),
        ((255, 0, 0), "#ff1a1a"),
        ((128, 128, 0), "#999900"),
        ((255, 255, 0), "#ffff1a"),
        ((0, 128, 0), "#009900"),
        ((0, 255, 0), "#1aff1a"),
        ((0, 128, 128), "#009999"),
        ((0, 255, 255), "#1affff"),
        ((0, 0, 128), "#000099"),
        ((0, 0, 255), "#1a1aff"),
        ((128, 0, 128), "#990099"),
        ((255, 0, 255), "#ff1aff"),
    ]
