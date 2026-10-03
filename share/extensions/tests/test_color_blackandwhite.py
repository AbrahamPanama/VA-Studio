# coding=utf-8
from color_blackandwhite import BlackAndWhite
from .test_inkex_extensions import ColorBaseCase


class ColorBlackAndWhiteTest(ColorBaseCase):
    effect_class = BlackAndWhite
    color_tests = [
        # When converting to black and white the color white should be unchanged
        ("none", "none"),
        ((0, 0, 0), "#000000"),
        ((255, 255, 255), "#ffffff"),
        ((192, 192, 192), "#ffffff"),
        ((128, 128, 128), "#ffffff"),
        ((128, 0, 0), "#000000"),
        ((255, 0, 0), "#000000"),
        ((128, 128, 0), "#000000"),
        ((255, 255, 0), "#ffffff"),
        ((0, 128, 0), "#000000"),
        ((0, 255, 0), "#ffffff"),
        ((0, 128, 128), "#000000"),
        ((0, 255, 255), "#ffffff"),
        ((0, 0, 128), "#000000"),
        ((0, 0, 255), "#000000"),
        ((128, 0, 128), "#000000"),
        ((255, 0, 255), "#000000"),
        # Increasing the threshold means more colors will be black
        ((255, 0, 255), "#000000", ["-t 240"]),
        ((192, 192, 192), "#000000", ["-t 240"]),
        # Decreasing the threshold means more colors will be white
        ((255, 0, 255), "#ffffff", ["-t 80"]),
        ((192, 192, 192), "#ffffff", ["-t 80"]),
    ]
