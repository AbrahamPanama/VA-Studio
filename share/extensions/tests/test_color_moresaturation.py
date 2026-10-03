# coding=utf-8
from color_moresaturation import MoreSaturation
from .test_inkex_extensions import ColorBaseCase


class ColorMoreSaturationTest(ColorBaseCase):
    effect_class = MoreSaturation
    color_tests = [
        ("none", "none"),
        ("hsl(0, 0, 0)", "hsl(0, 5, 0)"),
        ("hsl(255, 255, 255)", "hsl(255, 100, 100)"),
        ((0, 0, 0), "#000000"),
        ((255, 255, 255), "#ffffff"),
        ((192, 192, 192), "#c2bcbc"),
        ((128, 128, 128), "#867979"),
        ((128, 0, 0), "#800000"),
        ((255, 0, 0), "#ff0000"),
        ((128, 128, 0), "#808000"),
        ((255, 255, 0), "#ffff00"),
        ((0, 128, 0), "#008000"),
        ((0, 255, 0), "#00ff00"),
        ((0, 128, 128), "#008080"),
        ((0, 255, 255), "#00ffff"),
        ((0, 0, 128), "#000080"),
        ((0, 0, 255), "#0000ff"),
        ((128, 0, 128), "#800080"),
        ((255, 0, 255), "#ff00ff"),
    ]
