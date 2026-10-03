# coding=utf-8
from color_desaturate import Desaturate
from .test_inkex_extensions import ColorBaseCase


class ColorDesaturateTest(ColorBaseCase):
    effect_class = Desaturate
    color_tests = [
        ("none", "none"),
        ((0, 0, 0), "#000000"),
        ((255, 255, 255), "#ffffff"),
        ((192, 192, 192), "#c0c0c0"),
        ((128, 128, 128), "#808080"),
        ((128, 0, 0), "#404040"),
        ((255, 0, 0), "#7f7f7f"),
        ((128, 128, 0), "#404040"),
        ((255, 255, 0), "#7f7f7f"),
        ((0, 128, 0), "#404040"),
        ((0, 255, 0), "#7f7f7f"),
        ((0, 128, 128), "#404040"),
        ((0, 255, 255), "#7f7f7f"),
        ((0, 0, 128), "#404040"),
        ((0, 0, 255), "#7f7f7f"),
        ((128, 0, 128), "#404040"),
        ((255, 0, 255), "#7f7f7f"),
    ]
