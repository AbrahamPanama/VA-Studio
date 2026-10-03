# SPDX-FileCopyrightText: 2024 Manpreet Singh <manpreet.singh.dev@proton.me>
#
# SPDX-License-Identifier: GPL-2.0-or-later

from math import isclose

import pytest

from inkaf.parser.types import (
    AFDictObject,
)
from inkaf.svg.convert import AFConverter
from inkaf.svg.fill import AFColor, AFColorSpace, AFGradient, AFGradientType, parse_fdsc


def test_color_space_from_string():
    assert AFColorSpace.from_string("rgb") == AFColorSpace.RGB
    assert AFColorSpace.from_string("hsl") == AFColorSpace.HSL
    assert AFColorSpace.from_string("lab") == AFColorSpace.LAB
    assert AFColorSpace.from_string("rgba") == AFColorSpace.RGB
    assert AFColorSpace.from_string("hsla") == AFColorSpace.HSL
    assert AFColorSpace.from_string("laba") == AFColorSpace.LAB
    assert AFColorSpace.from_string("cmyk") == AFColorSpace.CMYK
    assert AFColorSpace.from_string("gray") == AFColorSpace.GRAY
    assert AFColorSpace.from_string("") == AFColorSpace.UNKNOWN


@pytest.mark.parametrize(
    "color, expected, expected_alpha",
    [
        (((0, 0, 0, 0), "RGBA"), "#000000", 0),
        (((51, 127, 229, 0.25), "RGBA"), "#337fe5", 0.25),
        (((0.2, 0.5, 0.8, 0.75), "RGBA"), "#3380cc", 0.75),
        (((0.0, 0.0, 0.0, 1.0), "HSLA"), "#000000", 1),
        (((0.2, 0.5, 0.8, 0.25), "HSLA"), "#dbe6b3", 0.25),
        (((0.5, 0.8, 0.2, 0.75), "HSLA"), "#0a5c5c", 0.75),
        (((0.0, 0.0), "GRAY"), "#000000", 0),
        (((0.2, 0.75), "GRAY"), "#333333", 0.75),
        (((0.0, 0.0, 0.0, 0.0, 1.0), "CMYK"), "#ffffff", 1.0),
        (((0.0, 0.0, 0.0, 1.0, 1.0), "CMYK"), "#000000", 1.0),
        (((0.0, 1.0, 1.0, 0.0, 0.75), "CMYK"), "#ff0000", 0.75),
        (((0.2, 0.5, 0.8, 0.1, 0.25), "CMYK"), "#b8732e", 0.25),
    ],
)
def test_color_to_svg(
    make_color, color: AFDictObject, expected: str, expected_alpha: float
):
    afcolor = AFColor.from_af(make_color(*color))

    assert (
        AFConverter._convert_color(afcolor) == expected
        and afcolor.alpha == expected_alpha
    ), f"Color mismatch: {AFConverter._convert_color(afcolor), afcolor.alpha} != {expected, expected_alpha}"


@pytest.mark.parametrize(
    "color1, color2, expected",
    [
        (
            AFColor([0.0, 0.0, 0.0], 1, AFColorSpace.RGB),
            AFColor([0.0, 0.0, 0.0], 1, AFColorSpace.RGB),
            AFColor([0.0, 0.0, 0.0], 1, AFColorSpace.RGB),
        ),
        (
            AFColor([0.0, 0.0, 0.0], 0, AFColorSpace.RGB),
            AFColor([1.0, 1.0, 1.0], 1, AFColorSpace.RGB),
            AFColor([0.5, 0.5, 0.5], 0.5, AFColorSpace.RGB),
        ),
        (
            AFColor([0.2, 0.5, 0.8], 1, AFColorSpace.RGB),
            AFColor([0.8, 0.5, 0.2], 1, AFColorSpace.RGB),
            AFColor([0.5, 0.5, 0.5], 1, AFColorSpace.RGB),
        ),
    ],
)
def test_interpolate(color1: AFColor, color2: AFColor, expected: AFColor):
    assert all(
        isclose(i, e, abs_tol=0.01)
        for i, e in zip(color1.interpolate(color2, 0.5).color, expected.color)
    ), f"Color mismatch: {color1.interpolate(color2, 0.5)} != {expected}"


@pytest.mark.parametrize(
    "fdef, expected",
    [
        (
            (
                AFGradientType.LINEAR,
                [[0.0, 0.5], [1.0, 0.5]],
                [(0.0, 0.0, 0.0, 0.0), (1.0, 1.0, 1.0, 1.0)],
                [100, 0, 0, 0, 100, 0],
                True,
            ),
            b'<linearGradient x1="0" y1="0" x2="1" y2="0" gradientTransform="scale(100, 100)" gradientUnits="userSpaceOnUse"><stop offset="0.0" style="stop-color:#000000;stop-opacity:0.0"/><stop offset="0.5" style="stop-color:#7f7f7f;stop-opacity:0.5"/><stop offset="1.0" style="stop-color:#ffffff;stop-opacity:1.0"/></linearGradient>',
        ),
        (
            (
                AFGradientType.RADIAL,
                [[0.0, 0.5], [1.0, 0.5]],
                [(0.0, 0.0, 0.0, 0.0), (1.0, 1.0, 1.0, 1.0)],
                [100, 0, 0, 0, 100, 0],
                True,
            ),
            b'<radialGradient cx="0" cy="0" r="1" gradientTransform="scale(100, 100)" gradientUnits="userSpaceOnUse">'
            + b'<stop offset="0.0" style="stop-color:#000000;stop-opacity:0.0"/>'
            + b'<stop offset="0.5" style="stop-color:#7f7f7f;stop-opacity:0.5"/>'
            + b'<stop offset="1.0" style="stop-color:#ffffff;stop-opacity:1.0"/>'
            + b"</radialGradient>",
        ),
        (
            (
                AFGradientType.ELLIPTICAL,
                [[0.0, 0.5], [1.0, 0.5]],
                [(0.0, 0.0, 0.0, 0.0), (1.0, 1.0, 1.0, 1.0)],
                [100, 0, 0, 0, 100, 0],
                True,
            ),
            b'<radialGradient cx="0" cy="0" r="1" gradientTransform="scale(100, 100)" gradientUnits="userSpaceOnUse">'
            + b'<stop offset="0.0" style="stop-color:#000000;stop-opacity:0.0"/>'
            + b'<stop offset="0.5" style="stop-color:#7f7f7f;stop-opacity:0.5"/>'
            + b'<stop offset="1.0" style="stop-color:#ffffff;stop-opacity:1.0"/>'
            + b"</radialGradient>",
        ),
    ],
)
def test_gradient_to_svg(make_gradient, fdef: AFDictObject, expected: str):
    gradient = parse_fdsc(make_gradient(*fdef))
    assert isinstance(gradient, AFGradient)

    svg = AFConverter._convert_gradient(gradient).tostring()
    assert svg == expected
