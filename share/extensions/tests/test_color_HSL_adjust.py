# coding=utf-8
from color_hsl_adjust import HslAdjust
from .test_inkex_extensions import ColorBaseCase


class ColorHSLAdjustTest(ColorBaseCase):
    effect_class = HslAdjust
    color_tests = [
        ("none", "none"),
        ((255, 255, 255), "#ffffff"),
        ((0, 0, 0), "#000000"),
        ((91, 166, 176), "#5aa5af"),
        ((91, 166, 176), "#5a97af", ["-x 10"]),
        ((91, 166, 176), "#5aaf80", ["-x 320"]),
        ((91, 166, 176), "#5aa5af", ["-x 0"]),
        ((91, 166, 176), "#a45aaf", ["-x 12345"]),
        ((91, 166, 176), "#5aa7af", ["-x -1"]),
        ((91, 166, 176), "#4eafbc", ["-s 10"]),
        ((91, 166, 176), "#0ae2ff", ["-s 90"]),
        ((91, 166, 176), "#5aa5af", ["-s 0"]),
        ((91, 166, 176), "#0ae2ff", ["-s 100"]),
        ((91, 166, 176), "#0ae2ff", ["-s 12345"]),
        ((91, 166, 176), "#5ba5ae", ["-s -1"]),
        ((91, 166, 176), "#7cb8c0", ["-l 10"]),
        ((91, 166, 176), "#ffffff", ["-l 90"]),
        ((91, 166, 176), "#5aa5af", ["-l 0"]),
        ((91, 166, 176), "#ffffff", ["-l 100"]),
        ((91, 166, 176), "#ffffff", ["-l 12345"]),
        ((91, 166, 176), "#56a4ae", ["-l -1"]),
        ((91, 166, 176), "#5a86af", ["--random_h=true"]),
        ((91, 166, 176), "#cbe3e6", ["--random_l=true"]),
        ((91, 166, 176), "#44b6c5", ["--random_s=true"]),
    ]
