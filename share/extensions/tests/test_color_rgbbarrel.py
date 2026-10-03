# coding=utf-8
from color_rgbbarrel import RgbBarrel
from .test_inkex_extensions import ColorBaseCase


class ColorBarrelTest(ColorBaseCase):
    effect_class = RgbBarrel
    color_tests = [
        ("none", "none"),
        ((0, 0, 0), "#000000"),
        ((255, 255, 255), "#ffffff"),
        ((192, 192, 192), "#c0c0c0"),
        ((128, 128, 128), "#808080"),
        ((128, 0, 0), "#008000"),
        ((255, 0, 0), "#00ff00"),
        ((128, 128, 0), "#008080"),
        ((255, 255, 0), "#00ffff"),
        ((0, 128, 0), "#000080"),
        ((0, 255, 0), "#0000ff"),
        ((0, 128, 128), "#800080"),
        ((0, 255, 255), "#ff00ff"),
        ((0, 0, 128), "#800000"),
        ((0, 0, 255), "#ff0000"),
        ((128, 0, 128), "#808000"),
        ((255, 0, 255), "#ffff00"),
        ("hsl(25, 14, 50)", "#6e917d"),
    ]
