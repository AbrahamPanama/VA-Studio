# SPDX-FileCopyrightText: 2024 Manpreet Singh <manpreet.singh.dev@proton.me>
#
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

from typing import List, Optional, Tuple

import pytest

from inkaf.parser.types import (
    AFDictObject,
    AFObjectMetadata,
    EnumT,
    Field,
    MetadataStatus,
    ObjectStatus,
)
from inkaf.svg.fill import AFGradientType
from inkaf.svg.shape import AFCornerType
from inkaf.svg.util import AFBoundingBox

## Fills


@pytest.fixture
def make_color():
    def _make_color(color: Tuple[float, ...], space: str = "RGBA") -> AFDictObject:
        return AFDictObject(
            id=0,
            types=[AFObjectMetadata(status=MetadataStatus.TAG_ID, id=1, tag=space)],
            fields={"_col": Field(type=20, value=color)},
            status=ObjectStatus.NON_SHARED,
        )

    return _make_color


@pytest.fixture
def make_gradient(make_color):
    def _make_gradient(
        type: AFGradientType,
        stops: List[List[float]],
        colors: List[List[float]],
        tfm: List[float],
        fdsc: bool,
    ) -> AFDictObject:
        return AFDictObject(
            id=0,
            types=[AFObjectMetadata(id=1, tag="FDsc", status=MetadataStatus.TAG_ID)],
            status=ObjectStatus.NON_SHARED,
            fields={
                "FDeF": Field(
                    type=49,
                    value=AFDictObject(
                        id=1,
                        types=[
                            AFObjectMetadata(
                                id=2, tag="FilG", status=MetadataStatus.TAG_ID
                            ),
                            AFObjectMetadata(
                                id=0, tag="Fill", status=MetadataStatus.TAG_ID
                            ),
                        ],
                        status=ObjectStatus.NON_SHARED,
                        fields={
                            "Type": Field(
                                type=42, value=EnumT(id=type.value, version=0)
                            ),
                            "Grad": Field(
                                type=49,
                                value=AFDictObject(
                                    id=2,
                                    types=[
                                        AFObjectMetadata(
                                            id=1,
                                            tag="Grad",
                                            status=MetadataStatus.TAG_ID,
                                        )
                                    ],
                                    status=ObjectStatus.NON_SHARED,
                                    fields={
                                        "Posn": Field(type=36, value=stops),
                                        "Cols": Field(
                                            type=49,
                                            value=[make_color(col) for col in colors],
                                        ),
                                    },
                                ),
                            ),
                        },
                    ),
                ),
                "FDeX": Field(type=40, value=tfm),
                "FDSc": Field(type=41, value=fdsc),
            },
        )

    return _make_gradient


## Shapes


@pytest.fixture
def make_base_shape():
    def _make_base_shape(bbox: AFBoundingBox, type_tag: str) -> AFDictObject:
        return AFDictObject(
            id=0,
            types=[
                AFObjectMetadata(id=2, tag="ShpN", status=MetadataStatus.TAG_ID),
                AFObjectMetadata(id=0, tag="VNod", status=MetadataStatus.TAG_ID),
                AFObjectMetadata(id=0, tag="Node", status=MetadataStatus.TAG),
            ],
            status=ObjectStatus.NULL,
            fields={
                "Shpe": Field(
                    type=49,
                    value=AFDictObject(
                        id=1,
                        types=[
                            AFObjectMetadata(
                                id=3, tag=type_tag, status=MetadataStatus.TAG_ID
                            ),
                            AFObjectMetadata(
                                id=0, tag="Shpe", status=MetadataStatus.TAG_ID
                            ),
                        ],
                        status=ObjectStatus.NULL,
                        fields={},
                    ),
                ),
                "ShpB": Field(type=38, value=[bbox.x1, bbox.y1, bbox.x2, bbox.y2]),
            },
        )

    return _make_base_shape


@pytest.fixture
def make_rectangle(make_base_shape):
    def _make_rectangle(
        bbox: AFBoundingBox,
        radii: Optional[List[float]] = None,
        corner_types: Optional[List[AFCornerType]] = None,
        absz: Optional[bool] = None,
        lock: Optional[bool] = None,
    ) -> AFDictObject:
        shape = make_base_shape(bbox, "ShNR")
        if radii is not None:
            shape["Shpe"].fields["ShCR"] = Field(type=33, value=radii)

        if corner_types is not None:
            shape["Shpe"].fields["CTyp"] = Field(
                type=42, value=[EnumT(ct.value, 0) for ct in corner_types]
            )

        if absz is not None:
            shape["Shpe"].fields["AbSz"] = Field(type=41, value=absz)

        if lock is not None:
            shape["Shpe"].fields["Lock"] = Field(type=41, value=lock)

        return shape

    return _make_rectangle


@pytest.fixture
def make_ellipse(make_base_shape):
    def _make_ellipse(bbox: AFBoundingBox) -> AFDictObject:
        return make_base_shape(bbox, "ShpE")

    return _make_ellipse


@pytest.fixture
def make_triangle(make_base_shape):
    def _make_triangle(
        bbox: AFBoundingBox, pos: Optional[float] = None
    ) -> AFDictObject:
        shape = make_base_shape(bbox, "ShpT")
        if pos is not None:
            shape["Shpe"].fields["Pos "] = Field(type=9, value=pos)
        return shape

    return _make_triangle


@pytest.fixture
def make_diamond(make_base_shape):
    def _make_diamond(bbox: AFBoundingBox, pos: Optional[float] = None) -> AFDictObject:
        shape = make_base_shape(bbox, "ShpD")
        if pos is not None:
            shape["Shpe"].fields["Pos "] = Field(type=9, value=pos)
        return shape

    return _make_diamond


@pytest.fixture
def make_trapezoid(make_base_shape):
    def _make_trapezoid(
        bbox: AFBoundingBox, posl: Optional[float] = None, posr: Optional[float] = None
    ) -> AFDictObject:
        shape = make_base_shape(bbox, "ShTz")
        if posl is not None:
            shape["Shpe"].fields["PosL"] = Field(type=9, value=posl)
        if posr is not None:
            shape["Shpe"].fields["PosR"] = Field(type=9, value=posr)
        return shape

    return _make_trapezoid
