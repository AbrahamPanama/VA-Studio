# coding=utf-8
from color_removeblue import RemoveBlue
from .test_inkex_extensions import ColorBaseCase


class ColorRemoveBlueTest(ColorBaseCase):
    effect_class = RemoveBlue
    color_tests = [
        ("none", "none"),
        ((0, 0, 0), "#000000"),
        ((255, 255, 255), "#ffff00"),
        ((192, 192, 192), "#c0c000"),
        ((128, 128, 128), "#808000"),
        ((128, 0, 0), "#800000"),
        ((255, 0, 0), "#ff0000"),
        ((128, 128, 0), "#808000"),
        ((255, 255, 0), "#ffff00"),
        ((0, 128, 0), "#008000"),
        ((0, 255, 0), "#00ff00"),
        ((0, 128, 128), "#008000"),
        ((0, 255, 255), "#00ff00"),
        ((0, 0, 128), "#000000"),
        ((0, 0, 255), "#000000"),
        ((128, 0, 128), "#800000"),
        ((255, 0, 255), "#ff0000"),
    ]
