#!/usr/bin/env python3

# Copyright (C) 2024 Jonathan Neuhauser <jonathan.neuhauser@outlook.com>
# SPDX-License-Identifier: GPL-2.0-or-later


from io import BytesIO

import pytest

from inkaf.parser.enhance import process_dictobj
from inkaf.parser.parse import AFParser
from inkaf.parser.sharedaf import SharedAFDictObject
from inkaf.parser.types import (
    AFDictObject,
    AFObjectMetadata,
    EnumT,
    Field,
    MetadataStatus,
    ObjectStatus,
)
from tests.conftest import assert_remove_parents

color_data = [
    (
        b"1rloC\x01!\x0b\x00\x00\x01KYMCHloc_\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x80?\x00\x00\x80?\x00",
        SharedAFDictObject(
            id=2849,
            types=[AFObjectMetadata(id=0, tag="CMYK", status=MetadataStatus.TAG)],
            status=ObjectStatus.SHARED,
            fields={
                "_col": Field(
                    type=72,
                    value=(0.0, 0.0, 0.0, 1.0, 1.0),
                )
            },
        ),
    ),  # CMYK,
    (
        b"1rloC\x01n\x0e\x00\x00\x01ALSHDloc_\x00\x00\x00\x00\x00\x00\x80?\x00\x00\x80?\x00\x00\x80?\x00",
        SharedAFDictObject(
            id=3694,
            types=[AFObjectMetadata(id=0, tag="HSLA", status=MetadataStatus.TAG)],
            status=ObjectStatus.SHARED,
            fields={
                "_col": Field(
                    type=68,
                    value=(0.0, 1.0, 1.0, 1.0),
                )
            },
        ),
    ),  # HSLA
    (
        b"1rloC\x01P\x07\x00\x00\x00ABAL\x01\x00\x00\x02<loc_\xc9\xc2\x86M\xe7\xa7\xff\xff\x00",
        SharedAFDictObject(
            id=1872,
            types=[AFObjectMetadata(id=1, tag="LABA", status=MetadataStatus.TAG_ID)],
            status=ObjectStatus.SHARED,
            fields={
                "_col": Field(
                    type=60,
                    value=(49865, 19846, 42983, 65535),
                )
            },
        ),
    ),  # LABA
    (
        b"1rloC\x01G\x07\x00\x00\x01ABGRDloc_\x00\x00\x80?\x9a\x99\x19?\x8f\x8e\x8e>\x00\x00\x80?\x00",
        SharedAFDictObject(
            id=1863,
            types=[AFObjectMetadata(id=0, tag="RGBA", status=MetadataStatus.TAG)],
            status=ObjectStatus.SHARED,
            fields={
                "_col": Field(
                    type=68,
                    value=(1.0, 0.6000000238418579, 0.27843138575553894, 1.0),
                )
            },
        ),
    ),  # RGBA
    (
        b"1rloC\x01o\x00\x00\x00\x00YARG\x01\x00\x00\x02<loc_\x00\x00\x00\x00\x00\x00\x80?\x00",
        SharedAFDictObject(
            id=111,
            types=[AFObjectMetadata(id=1, tag="GRAY", status=MetadataStatus.TAG_ID)],
            status=ObjectStatus.SHARED,
            fields={
                "_col": Field(
                    type=60,
                    value=(0.0, 1.0),
                )
            },
        ),
    ),  # GRAY
    (
        b"1GLBF\x01\xa1\x01\x00\x00\x01drGB)drGU\x00\ntrtS\x00\x00\x00\x00\x00\x00\x00\x00\ncapS\xab\xaa\xaa\xaa\xaa*M@*otlR\x06\x00\x00\x008rloC\x00\xc0\xff\xff\x00",
        SharedAFDictObject(
            id=417,
            types=[AFObjectMetadata(id=0, tag="BGrd", status=MetadataStatus.TAG)],
            status=ObjectStatus.SHARED,
            fields={
                "UGrd": Field(
                    type=41,
                    value=False,
                ),
                "Strt": Field(
                    type=10,
                    value=0.0,
                ),
                "Spac": Field(
                    type=10,
                    value=58.333333333333336,
                ),
                "Rlto": Field(
                    type=42,
                    value=EnumT(id=6, version=0),
                ),
                "Colr": Field(
                    type=56,
                    value=(0, 192, 255, 255),
                ),
            },
        ),
    ),  # BGrd
]


@pytest.mark.parametrize("bytes, result", color_data)
def test_color_struct(bytes, result):
    parser = AFParser(BytesIO(bytes + b"\x00"))

    parent = AFDictObject(0, [], ObjectStatus.NULL, {})
    parser.load_fields(parent)

    parent = process_dictobj(parent, None)

    assert len(parent.fields) == 1

    tag = list(parent.fields.keys())[0]

    assert tag[::-1].encode("latin-1") in bytes

    assert_remove_parents(parent.fields[tag], result)
