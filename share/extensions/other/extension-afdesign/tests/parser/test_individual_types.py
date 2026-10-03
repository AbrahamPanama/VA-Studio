#!/usr/bin/env python3

# Copyright (C) 2024 Jonathan Neuhauser <jonathan.neuhauser@outlook.com>
# SPDX-License-Identifier: GPL-2.0-or-later


from io import BytesIO
import pytest

from inkaf.parser.parse import AFParser
from inkaf.parser.types import (
    ObjectStatus,
    AFObjectMetadata,
    EmbeddedData,
    MetadataStatus,
    EnumT,
    AFListObject,
    AFDictObject,
    FlagsT,
    Field,
    Curve12,
    Curve16,
    UnknownStruct,
)

from tests.conftest import assert_remove_parents

data = [
    (b"\x01ataD\x01\x00", 1),  # 1
    (b"\x02zSlP\x00\x01", 256),  # 2
    (b"\x03niva\xff\xff\xff\x1f", 536870911),  # 3
    (b"\x04ziSF\x00\x00\x00\x00\x00\x00\x00\x00", 0),  # 4
    (b"\x06lacS\x01\x00", 1),  # 6
    (b"\x07rjaM\x02\x00\x00\x00", 2),  # 7
    (b"\x08tDrC3`if\x00\x00\x00\x00", 1718181939),  # 8
    (b"\tcapO\x00\x00\x80?", 1.0),  # 9
    (b"\ntluM\x00\x00\x00\x00\x00\x00\xf0?", 1.0),  # 10
    (b"\x15eziS\x03\x00\x00\x00\x03\x00\x00\x00", [3, 3]),  # 21
    (
        b"\x17RtiB\x01\x00\x00\x80\x01\x00\x00\x80\x01\x00\x00\x80\x01\x00\x00\x80",
        [-2147483647, -2147483647, -2147483647, -2147483647],
    ),  # 23
    (
        b"!RChS\x00\x00\x80>\x00\x00\x80>\x00\x00\x80>\x00\x00\x80>",
        [0.25, 0.25, 0.25, 0.25],
    ),  # 33
    (
        b"$PFrT\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00",
        [0.0, 0.0],
    ),  # 36
    (
        b"&xBrS\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00",
        [0.0, 0.0, 0.0, 0.0],
    ),  # 38
    (
        b"(mrfX\x00\x00\x00\x00\x00\x00\xf0?\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x8f\xeax\xa3Bf\xe3?\x00\x00\x00\x00\x00\x00\x00\x00",
        [1.0, 0.0, 0.0, 0.0, 0.6062329476183822, 0.0],
    ),  # 40
    (b")VFrT\x00", False),  # 41
    (b"*nArT\x00\x00\x00\x00", EnumT(id=0, version=0)),  # 42
    (
        b"+DIiM$\x00\x00\x00637C07A1-5B13-433C-B5B5-09D400711B46",
        "637C07A1-5B13-433C-B5B5-09D400711B46",
    ),  # 43
    (
        b",ataD\x0c\x00\x00\x00\x00\x00\x00\x00\x00@\x00\x02\x00\x00",
        Curve12(doubles=2.0, unsigned_ints=(0, 2, 0, 0)),
    ),  # 44
    (
        b",IseR \x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x01\x01\x00O\x00n\x00e",
        UnknownStruct(
            data=b"\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x01\x01\x00O\x00n\x00e"
        ),
    ),  # 44
    (
        b"-ataD\\\x01\x00\x00\x00\x00\x01\\lcms\x040\x00\x00mntrGRAYXYZ \x07\xe8\x00\x06\x00\x0c\x00\x13\x00*\x00\x14acspMSFT\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\xf6\xd6\x00\x01\x00\x00\x00\x00\xd3-lcms\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x04desc\x00\x00\x00\xb4\x00\x00\x006cprt\x00\x00\x00\xec\x00\x00\x00Lwtpt\x00\x00\x018\x00\x00\x00\x14kTRC\x00\x00\x01L\x00\x00\x00\x10mluc\x00\x00\x00\x00\x00\x00\x00\x01\x00\x00\x00\x0cenUS\x00\x00\x00\x1a\x00\x00\x00\x1c\x00G\x00r\x00e\x00y\x00s\x00c\x00a\x00l\x00e\x00 \x00D\x005\x000\x00\x00mluc\x00\x00\x00\x00\x00\x00\x00\x01\x00\x00\x00\x0cenUS\x00\x00\x000\x00\x00\x00\x1c\x00N\x00o\x00 \x00c\x00o\x00p\x00y\x00r\x00i\x00g\x00h\x00t\x00,\x00 \x00u\x00s\x00e\x00 \x00f\x00r\x00e\x00e\x00l\x00yXYZ \x00\x00\x00\x00\x00\x00\xf6\xd6\x00\x01\x00\x00\x00\x00\xd3-para\x00\x00\x00\x00\x00\x00\x00\x00\x00\x0233",
        b"\x00\x00\x01\\lcms\x040\x00\x00mntrGRAYXYZ \x07\xe8\x00\x06\x00\x0c\x00\x13\x00*\x00\x14acspMSFT\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\xf6\xd6\x00\x01\x00\x00\x00\x00\xd3-lcms\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x04desc\x00\x00\x00\xb4\x00\x00\x006cprt\x00\x00\x00\xec\x00\x00\x00Lwtpt\x00\x00\x018\x00\x00\x00\x14kTRC\x00\x00\x01L\x00\x00\x00\x10mluc\x00\x00\x00\x00\x00\x00\x00\x01\x00\x00\x00\x0cenUS\x00\x00\x00\x1a\x00\x00\x00\x1c\x00G\x00r\x00e\x00y\x00s\x00c\x00a\x00l\x00e\x00 \x00D\x005\x000\x00\x00mluc\x00\x00\x00\x00\x00\x00\x00\x01\x00\x00\x00\x0cenUS\x00\x00\x000\x00\x00\x00\x1c\x00N\x00o\x00 \x00c\x00o\x00p\x00y\x00r\x00i\x00g\x00h\x00t\x00,\x00 \x00u\x00s\x00e\x00 \x00f\x00r\x00e\x00e\x00l\x00yXYZ \x00\x00\x00\x00\x00\x00\xf6\xd6\x00\x01\x00\x00\x00\x00\xd3-para\x00\x00\x00\x00\x00\x00\x00\x00\x00\x0233",
    ),  # 45
    (b"/egsU\x00\x00\x00\x00", 0),  # 47
    (
        b"0  nO\x01\x00\x03\x01\x00\x00\x00$\x0f\xa4uhY^}@\x8c\x0c|-\xaaxr@\x15\x00\x00\x00\x00\x12\x00\x00\x00*\x01\x00\x00\x00\x00",
        AFListObject(
            id=0,
            types=[],
            status=ObjectStatus.NON_SHARED,
            fields=[
                Field(
                    type=1,
                    value=0,
                ),
                Field(
                    type=3,
                    value=1,
                ),
                Field(
                    type=36,
                    value=[469.89682813600024, 295.5415472837128],
                ),
                Field(
                    type=21,
                    value=[0, 18],
                ),
                Field(
                    type=42,
                    value=EnumT(id=1, version=0),
                ),
            ],
        ),
    ),  # 48
    (
        b"1cSxE\x01\x08\x00\x00\x00\x00cSxE\x02\x00\x00\x02*yTzS\x00\x00\x00\x00\ntluM\x00\x00\x00\x00\x00\x00\xf0?\x00",
        AFDictObject(
            id=8,
            types=[AFObjectMetadata(id=2, tag="ExSc", status=MetadataStatus.TAG_ID)],
            status=ObjectStatus.SHARED,
            fields={
                "SzTy": Field(type=42, value=EnumT(id=0, version=0)),
                "Mult": Field(type=10, value=1.0),
            },
        ),
    ),  # 49
    (
        b"1BFDP\x00",
        AFDictObject(id=0, types=[], status=ObjectStatus.NULL, fields={}),
    ),  # 49
    (
        b"2aRlB\x01RnlB\x01\x00\x07sreV\x02\x00\x00\x00\nmaGB333333\xf7?\x00",
        AFDictObject(
            id=0,
            types=[AFObjectMetadata(id=1, tag="BlnR", status=MetadataStatus.TAG_ID)],
            status=ObjectStatus.NON_SHARED,
            fields={
                "Vers": Field(type=7, value=2),
                "BGam": Field(
                    type=10,
                    value=1.45,
                ),
            },
        ),
    ),  # 50
    (
        b"3CbmEcDmE\x05\x00\x00\x00edc/1",
        EmbeddedData(tag="EmDc", size=5, data="edc/1"),
    ),  # 51
    (b"4DIUC\x00\x00\x00\x00", 0),  # 52
    (b"8rloC\x00\xc0\xff\xff", UnknownStruct(data=b"\x00\xc0\xff\xff")),  # 56
    (
        b"<loc_\xe1\xcb\xa4\x94\\\xbe\xff\xff",
        UnknownStruct(data=b"\xe1\xcb\xa4\x94\\\xbe\xff\xff"),
    ),  # 60
    (
        b"Dloc_\x00\x00\x80?\x00\x00\x80?\x00\x00\x80?\x00\x00\x80?",
        UnknownStruct(data=b"\x00\x00\x80?\x00\x00\x80?\x00\x00\x80?\x00\x00\x80?"),
    ),  # 68
    (
        b"Hloc_\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x80?",
        UnknownStruct(
            data=b"\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x80?"
        ),
    ),  # 72
    (b"usghC\x00\x00\x01\x04", FlagsT(version=0, flags=b"\x04")),  # 117
    (
        b"\x811\x02TM\x10\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00",
        [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0],
    ),  # 129
    (b"\x83nCTO\x01\x00\x00\x00\x12\x00\x00\x00", [18]),  # 131
    (
        b"\x87stnI\t\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\xff\xff\xff\xff\xff\xff\xff\xff\x00\x00\x00\x00\x00\x00\x00\x00",
        [0, 0, 0, 0, 0, -1, -1, 0, 0],
    ),  # 135
    (
        b"\x89CeuH\x06\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00",
        [0.0, 0.0, 0.0, 0.0, 0.0, 0.0],
    ),  # 137
    (b"\x8aWloC\x01\x00\x00\x00\x00\x00\x00\x00\x00 \x9c@", [1800.0]),  # 138
    (
        b"\x95sldH\x03\x00\x00\x00\x00\x00\x00\x00\x03\x00\x00\x00\x00\x00\x00\x00\x06\x00\x00\x00\x00\x00\x00\x00\t\x00\x00\x00",
        [[0, 3], [0, 6], [0, 9]],
    ),  # 149
    (
        b"\xa4nsoP\x02\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\xe0?\x00\x00\x00\x00\x00\x00\xf0?\x00\x00\x00\x00\x00\x00\xe0?",
        [[0.0, 0.5], [1.0, 0.5]],
    ),  # 164
    (
        b"\xa6AgrM\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00",
        [[0.0, 0.0, 0.0, 0.0]],
    ),  # 166
    (b"\xa8TClO\x00\x00\x00\x00", []),  # 168
    (b"\xa9rveR\x02\x00\x00\x00\x02", [False, True]),  # 169
    (
        b"\xaaOwrD\x02\x00\x00\x00\x00\x00\x00\x00\x01\x00",
        [EnumT(id=0, version=0), EnumT(id=1, version=0)],
    ),  # 170
    (
        b"\xabirtS\x15\x00\x00\x00\x04\x00\x00\x00\x00\x00\x00\x00\x05\x00\x00\x00hu-HU\x00\x00\x00\x00\x00\x00\x00\x00",
        ["", "hu-HU", "", ""],
    ),  # 171
    (
        b"\xacsdGC\x01\x00\x00\x00\x10\x00\x00\x00\x00\x00\x00\x00Y@\x01\x00\x00\x00\x01\x00\x00\x00",
        [Curve16(doubles=100.0, unsigned_ints=(1, 1))],
    ),  # 172
    (
        b"\xb1lFIL\x01\x00\x00\x00\x01\x10\x00\x00\x00\x01csDF1FeDF\x01\x11\x00\x00\x00\x00NliF\x01\x00\x00\x01lliF\x00\x00",
        [
            AFDictObject(
                id=16,
                types=[AFObjectMetadata(id=0, tag="FDsc", status=MetadataStatus.TAG)],
                status=ObjectStatus.SHARED,
                fields={
                    "FDeF": Field(
                        type=49,
                        value=AFDictObject(
                            id=17,
                            types=[
                                AFObjectMetadata(
                                    id=1, tag="FilN", status=MetadataStatus.TAG_ID
                                ),
                                AFObjectMetadata(
                                    id=0, tag="Fill", status=MetadataStatus.TAG
                                ),
                            ],
                            status=ObjectStatus.SHARED,
                            fields={},
                        ),
                    )
                },
            )
        ],
    ),  # 177
    (
        b"\xb2dxiM\x01\x00\x00\x00PSSG\x01\x00\x01\xb1sylG\x01\x00\x00\x00\x02\xa5\x00\x00\x00+8ftU\x00\x00\x00\x00\x00",
        [
            AFDictObject(
                id=0,
                types=[
                    AFObjectMetadata(id=1, tag="GSSP", status=MetadataStatus.TAG_ID)
                ],
                status=ObjectStatus.NON_SHARED,
                fields={
                    "Glys": Field(
                        type=49,
                        value=[
                            AFDictObject(
                                id=165, types=[], status=ObjectStatus.LINK, fields={}
                            )
                        ],
                    ),
                    "Utf8": Field(type=43, value=""),
                },
            )
        ],
    ),  # 178
]


@pytest.mark.parametrize("bytes, result", data)
def test_individual_types(bytes, result):
    parser = AFParser(BytesIO(bytes + b"\x00"))

    parent = AFDictObject(0, [], ObjectStatus.NULL, {})
    parser.load_fields(parent)

    assert len(parent.fields) == 1

    tag = list(parent.fields.keys())[0]

    assert tag[::-1].encode("latin-1") in bytes

    assert_remove_parents(parent.fields[tag], result)


@pytest.mark.parametrize(
    "value",
    [
        b"\xb3dxiM\x01\x00\x00\x00\x12\x00\x00\x00",  # incorrect array type
        b"\x78dcbA\x01",  # Type > 77
        b"\x12dcbA\x01",  # Unknown type
        b"1\x00",  # Attempt to load AFObject without type
        b",ataD\x0d\x00\x00\x00\x00\x00\x00\x00\x00@\x00\x02\x00\x00",  # Curve with bad size
        b"3CbmEcDmE\x05\x00\x00\x00ed&/1",  # Embedded data with bad character
        b"1cSxE\x01\x08\x00\x00\x00\x00cSxE\x02\x00\x00\x04*yTzS\x00\x00\x00\x00\ntluM\x00\x00\x00\x00\x00\x00\xf0?\x00",  # Bad Metadata Status flag
        b"1cSxE\x06\x08\x00\x00\x00\x00cSxE\x02\x00\x00\x02*yTzS\x00\x00\x00\x00\ntluM\x00\x00\x00\x00\x00\x00\xf0?\x00",  # Bad Object Status flag
    ],
)
def test_bad_structs(value):
    parser = AFParser(BytesIO(value + b"\x00"))

    parent = AFDictObject(0, [], ObjectStatus.NULL, {})
    with pytest.raises(ValueError):
        parser.load_fields(parent, False if value.startswith(b"1") else True)
