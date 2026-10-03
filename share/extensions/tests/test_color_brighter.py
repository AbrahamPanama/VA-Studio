# coding=utf-8
from color_brighter import Brighter
from .test_inkex_extensions import ColorBaseCase


class ColorBrighterTest(ColorBaseCase):
    effect_class = Brighter
    color_tests = [
        ("none", "none"),
        ((0, 0, 0), "#0a0a0a"),
        ((255, 255, 255), "#ffffff"),
        ((192, 192, 192), "#d5d5d5"),
        ((128, 128, 128), "#8e8e8e"),
        ((128, 0, 0), "#8e0000"),
        ((255, 0, 0), "#ff0000"),
        ((128, 128, 0), "#8e8e00"),
        ((255, 255, 0), "#ffff00"),
        ((0, 128, 0), "#008e00"),
        ((0, 255, 0), "#00ff00"),
        ((0, 128, 128), "#008e8e"),
        ((0, 255, 255), "#00ffff"),
        ((0, 0, 128), "#00008e"),
        ((0, 0, 255), "#0000ff"),
        ((128, 0, 128), "#8e008e"),
        ((255, 0, 255), "#ff00ff"),
        ("hsl(33, 92, 128)", "hsl(33, 92, 142)"),
    ]
