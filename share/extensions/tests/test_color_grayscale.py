# coding=utf-8
from color_grayscale import Grayscale
from .test_inkex_extensions import ColorBaseCase


class ColorGrayscaleTest(ColorBaseCase):
    effect_class = Grayscale
    color_tests = [
        ("none", "none"),
        ((0, 0, 0), "#000000"),
        ((255, 255, 255), "#ffffff"),
        ((192, 192, 192), "#c0c0c0"),
        ((128, 128, 128), "#808080"),
        ((128, 0, 0), "#262626"),
        ((255, 0, 0), "#4c4c4c"),
        ((128, 128, 0), "#717171"),
        ((255, 255, 0), "#e2e2e2"),
        ((0, 128, 0), "#4b4b4b"),
        ((0, 255, 0), "#969696"),
        ((0, 128, 128), "#5a5a5a"),
        ((0, 255, 255), "#b3b3b3"),
        ((0, 0, 128), "#0f0f0f"),
        ((0, 0, 255), "#1d1d1d"),
        ((128, 0, 128), "#353535"),
        ((255, 0, 255), "#696969"),
    ]
