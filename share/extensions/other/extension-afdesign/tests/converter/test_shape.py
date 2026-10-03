# SPDX-FileCopyrightText: 2024 Manpreet Singh <manpreet.singh.dev@proton.me>
#
# SPDX-License-Identifier: GPL-2.0-or-later

from typing import Type

import inkex
import inkex.base
import pytest


from inkaf.parser.types import AFDictObject
from inkaf.svg.shape import (
    AFCornerType,
    AFDiamond,
    AFEllipse,
    AFRectangle,
    AFTrapezoid,
    AFTriangle,
    parse_shape,
)
from inkaf.svg.util import AFBoundingBox
from inkaf.svg.convert import AFConverter


@pytest.fixture
def test_convert():
    def _test_convert(
        test_name: str,
        shape: AFDictObject,
        expected_type: Type,
        expected: inkex.ShapeElement,
    ) -> None:
        afshape = parse_shape(shape["Shpe"], shape["ShpB"])
        svg = AFConverter(None)._convert_shape(afshape)

        assert isinstance(afshape, expected_type), f"Test failed: {test_name}"
        assert svg.tostring() == expected.tostring(), f"Test failed: {test_name}"

    return _test_convert


@pytest.mark.parametrize(
    "test_name, rectangle, exp_shape, exp_effect",
    [
        (
            "Default",
            (AFBoundingBox(10, 20, 130, 140),),
            inkex.Rectangle.new(10, 20, 120, 120),
            None,
        ),
        (
            "No corners",
            (
                AFBoundingBox(5, 0, 100, 100),
                [0.2, 0.3, 0.4, 0.5],
                [AFCornerType.NONE] * 4,
                False,
                False,
            ),
            inkex.Rectangle.new(5, 0, 95, 100),
            None,
        ),
        (
            "Rounded corners",
            (
                AFBoundingBox(5, 0, 100, 100),
                [0.2, 0.3, 0.4, 0.5],
                [AFCornerType.ROUNDED] * 4,
                False,
                False,
            ),
            inkex.Rectangle.new(5, 0, 95, 100),
            inkex.PathEffect.new(
                effect="fillet_chamfer",
                lpeversion="1",
                method="arc",
                flexible="true",
                nodesatellites_param="F,1,0,1,0,0.19,0,1 @ F,1,0,1,0,0.3,0,1 @ F,1,0,1,0,0.38,0,1 @ F,1,0,1,0,0.5,0,1",
            ),
        ),
        (
            "Concave corners",
            (
                AFBoundingBox(5, 0, 100, 100),
                [0.2, 0.3, 0.4, 0.5],
                [AFCornerType.CONCAVE] * 4,
                False,
                False,
            ),
            inkex.Rectangle.new(5, 0, 95, 100),
            inkex.PathEffect.new(
                effect="fillet_chamfer",
                lpeversion="1",
                method="arc",
                flexible="true",
                nodesatellites_param="IF,1,0,1,0,0.19,0,1 @ IF,1,0,1,0,0.3,0,1 @ IF,1,0,1,0,0.38,0,1 @ IF,1,0,1,0,0.5,0,1",
            ),
        ),
        (
            "Cutout corners",
            (
                AFBoundingBox(5, 0, 100, 100),
                [0.2, 0.3, 0.4, 0.5],
                [AFCornerType.CUTOUT] * 4,
                False,
                False,
            ),
            inkex.PathElement.new(
                path="M 100 28.5 L 100 62 h -38 v 38 L 52.5 100 v -47.5 h -47.5 L 5 19 h 19 v -19 L 71.5 0 v 28.5 h 28.5 Z"
            ),
            None,
        ),
        (
            "Straight corners",
            (
                AFBoundingBox(5, 0, 100, 100),
                [0.2, 0.3, 0.4, 0.5],
                [AFCornerType.STRAIGHT] * 4,
                False,
                False,
            ),
            inkex.Rectangle.new(5, 0, 95, 100),
            inkex.PathEffect.new(
                effect="fillet_chamfer",
                lpeversion="1",
                method="arc",
                flexible="true",
                nodesatellites_param="C,1,0,1,0,0.19,0,1 @ C,1,0,1,0,0.3,0,1 @ C,1,0,1,0,0.38,0,1 @ C,1,0,1,0,0.5,0,1",
            ),
        ),
        (
            "Radii 100%",
            (
                AFBoundingBox(0, 0, 100, 100),
                [1.0] * 4,
                [AFCornerType.STRAIGHT] * 4,
                False,
                True,
            ),
            inkex.Rectangle.new(0, 0, 100, 100),
            inkex.PathEffect.new(
                effect="fillet_chamfer",
                lpeversion="1",
                method="arc",
                flexible="true",
                nodesatellites_param="C,1,0,1,0,0.5,0,1 @ C,1,0,1,0,0.5,0,1 @ C,1,0,1,0,0.5,0,1 @ C,1,0,1,0,0.5,0,1",
            ),
        ),
        (
            "Radii 50%",
            (
                AFBoundingBox(0, 0, 100, 100),
                [0.5] * 4,
                [AFCornerType.STRAIGHT] * 4,
                False,
                True,
            ),
            inkex.Rectangle.new(0, 0, 100, 100),
            inkex.PathEffect.new(
                effect="fillet_chamfer",
                lpeversion="1",
                method="arc",
                flexible="true",
                nodesatellites_param="C,1,0,1,0,0.5,0,1 @ C,1,0,1,0,0.5,0,1 @ C,1,0,1,0,0.5,0,1 @ C,1,0,1,0,0.5,0,1",
            ),
        ),
        (
            "Radii 0%",
            (
                AFBoundingBox(0, 0, 100, 100),
                [0.0] * 4,
                [AFCornerType.STRAIGHT] * 4,
                False,
                True,
            ),
            inkex.Rectangle.new(0, 0, 100, 100),
            None,
        ),
        (
            "Radii overlap",
            (
                AFBoundingBox(0, 0, 100, 100),
                [0.8, 0.2, 0.2, 1.0],
                [AFCornerType.STRAIGHT] * 4,
                False,
                False,
            ),
            inkex.Rectangle.new(0, 0, 100, 100),
            inkex.PathEffect.new(
                effect="fillet_chamfer",
                lpeversion="1",
                method="arc",
                flexible="true",
                nodesatellites_param="C,1,0,1,0,0.4444444444444444,0,1 @ C,1,0,1,0,0.1111111111111111,0,1 @ C,1,0,1,0,0.1111111111111111,0,1 @ C,1,0,1,0,0.5555555555555556,0,1",
            ),
        ),
    ],
)
def test_rectangle(
    make_rectangle,
    test_name,
    rectangle,
    exp_shape,
    exp_effect,
):
    doc: inkex.SvgDocumentElement = inkex.base.SvgOutputMixin.get_template(
        width=100, height=100, unit="px"
    ).getroot()

    rect = make_rectangle(*rectangle)
    afshape = parse_shape(rect["Shpe"], rect["ShpB"])
    conv = AFConverter(None)
    conv.document = doc
    svg = conv._convert_rectangle(afshape)

    assert isinstance(afshape, AFRectangle)
    assert isinstance(svg, type(exp_shape))

    if isinstance(exp_shape, inkex.PathElement):
        assert svg.tostring() == exp_shape.tostring()
    else:
        assert (
            svg.left == exp_shape.left
            and svg.top == exp_shape.top
            and svg.width == exp_shape.width
            and svg.height == exp_shape.height
        )
        assert svg.rx == exp_shape.rx and svg.ry == exp_shape.ry

    effect_id = svg.get("inkscape:path-effect", "")
    effect = doc.getElementById(effect_id)
    if exp_effect is None:
        assert effect is None
        assert effect_id == ""
        return
    assert effect is not None

    exp_effect.set("id", effect.get_id())
    for k, v in exp_effect.items():
        assert v == effect.get(k)


@pytest.mark.parametrize(
    "test_name, ellipse, exp_shape",
    [
        (
            "Circle",
            (AFBoundingBox(0, 0, 100, 100),),
            inkex.Ellipse.new(center=(50.0, 50.0), radius=(50.0, 50.0)),
        ),
        (
            "Ellipse",
            (AFBoundingBox(50, 25, 100, 100),),
            inkex.Ellipse.new(center=(75.0, 62.5), radius=(25.0, 37.5)),
        ),
    ],
)
def test_ellipse(test_convert, make_ellipse, test_name, ellipse, exp_shape):
    test_convert(test_name, make_ellipse(*ellipse), AFEllipse, exp_shape)


@pytest.mark.parametrize(
    "test_name, triangle, exp_shape",
    [
        (
            "Default",
            (AFBoundingBox(0, 0, 100, 100),),
            inkex.Polygon.new(points="50.0,0 100,100 0,100"),
        ),
        (
            "Pos 0.25",
            (AFBoundingBox(0, 0, 100, 100), 0.25),
            inkex.Polygon.new(points="25.0,0 100,100 0,100"),
        ),
    ],
)
def test_triangle(test_convert, make_triangle, test_name, triangle, exp_shape):
    test_convert(test_name, make_triangle(*triangle), AFTriangle, exp_shape)


@pytest.mark.parametrize(
    "test_name, diamond, exp_shape",
    [
        (
            "Default",
            (AFBoundingBox(0, 0, 100, 100),),
            inkex.Polygon.new(points="50.0,0 100,50.0 50.0,100 0,50.0"),
        ),
        (
            "Pos 0.25",
            (AFBoundingBox(0, 0, 100, 100), 0.25),
            inkex.Polygon.new(points="50.0,0 100,75.0 50.0,100 0,75.0"),
        ),
    ],
)
def test_diamond(test_convert, make_diamond, test_name, diamond, exp_shape):
    test_convert(test_name, make_diamond(*diamond), AFDiamond, exp_shape)


@pytest.mark.parametrize(
    "test_name, tpz, exp_shape",
    [
        (
            "Default",
            (AFBoundingBox(0, 0, 100, 100),),
            inkex.Polygon.new(points="100,100 0,100 25.0,0 75.0,0"),
        ),
        (
            "LPos 0.1 RPos 0.9",
            (AFBoundingBox(0, 0, 100, 100), 0.1, 0.9),
            inkex.Polygon.new(points="100,100 0,100 10.0,0 90.0,0"),
        ),
    ],
)
def test_trapezoid(test_convert, make_trapezoid, test_name, tpz, exp_shape):
    test_convert(test_name, make_trapezoid(*tpz), AFTrapezoid, exp_shape)
