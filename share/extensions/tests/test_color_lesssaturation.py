# coding=utf-8
from color_lesssaturation import LessSaturation
from .test_inkex_extensions import ColorBaseCase


class ColorLessSaturationTest(ColorBaseCase):
    effect_class = LessSaturation
    color_tests = [
        ("none", "none"),
        ("hsl(0, 0, 0)", "hsl(0, 0, 0)"),
        ("hsl(255, 255, 255)", "hsl(255, 95, 100)"),
        ((0, 0, 0), "#000000"),
        ((255, 255, 255), "#ffffff"),
        ((192, 192, 192), "#bfbfbf"),
        ((128, 128, 128), "#808080"),
        ((128, 0, 0), "#7c0303"),
        ((255, 0, 0), "#f90606"),
        ((128, 128, 0), "#7c7c03"),
        ((255, 255, 0), "#f9f906"),
        ((0, 128, 0), "#037c03"),
        ((0, 255, 0), "#06f906"),
        ((0, 128, 128), "#037c7c"),
        ((0, 255, 255), "#06f9f9"),
        ((0, 0, 128), "#03037c"),
        ((0, 0, 255), "#0606f9"),
        ((128, 0, 128), "#7c037c"),
        ((255, 0, 255), "#f906f9"),
    ]
