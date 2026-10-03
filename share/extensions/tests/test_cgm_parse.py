from io import BytesIO
import cgm_input
import cgm_enums
import cgm_parse

import pytest


@pytest.mark.parametrize(
    "binary, expected",
    [
        (b"\x00$\x03DXF", cgm_parse.BeginMetafile(name="DXF")),
        # check that trailing characters are ignored
        (b"\x00@SOMEJUNK", cgm_parse.EndMetafile()),
        # check that we can parse commands with zero length having
        # a string argument
        (b"\x00 ", cgm_parse.BeginMetafile(name="")),
        (b"\x00\x80", cgm_parse.BeginPictureBody()),
        (b'\x10"\x00\x01', cgm_parse.MetafileVersion(version=1)),
        (b"\x10b\x00\x00", cgm_parse.VdcType(vdc_type=cgm_enums.VDCTypeEnum.INTEGER)),
        (
            b"TD\x02\x18\xe0\xe0",
            cgm_parse.ColourTable(
                colour_index=cgm_enums.IndexColour(index=2),
                colours=[cgm_enums.DirectColour(24, 224, 224, 0)],
            ),
        ),
        (
            b" &\x00\x01<\xd0\x13\xa9",
            cgm_parse.ScalingMode(
                scaling_mode=cgm_enums.ScalingModeEnum.METRIC,
                metric_scaling_factor=0.02539999969303608,
            ),
        ),
        (
            b"\x11\xc6\x00\x01\x034/1",
            cgm_parse.CharacterSetList(
                specification=[(cgm_enums.CharacterSetTypeEnum.G_SET_96, "4/1")]
            ),
        ),
        (
            b"\x11\xe2\x00\x00",
            cgm_parse.CharacterCodingAnnouncer(
                encoding=cgm_enums.CharacterCodingEnum.BASIC_7BIT
            ),
        ),
        (
            b"\x10_\x00\x1f\x1eCreated by UniConvertor 2.0rc4\x00",
            cgm_parse.MetafileDescription(description="Created by UniConvertor 2.0rc4"),
        ),
        (
            b'\x11\x8e0"\x00\x10 \xc8\x00\x00\x00\x00\x0f\xff\x0f\xff',
            cgm_parse.MetafileDefaultsReplacement(
                data=(
                    cgm_parse.VDCIntegerPrecision(integer_precision=16),
                    cgm_parse.VDCExtent(
                        first_corner=cgm_enums.Point(x=0, y=0),
                        second_corner=cgm_enums.Point(x=4095, y=4095),
                    ),
                )
            ),
        ),
        # synthetic example
        (
            b"A\x1f\x00$\x0b\x0e\xf7y\x00\x01\x07\xfe\xf7z\x00\x01\x04\xed\xf7z\x00\x01\xfc\xe3\xf7z\x00\x01\xf4\xd8\xf7z\x00\x01\xf4n\xf7{\x00\x00\x03",
            cgm_parse.PolygonSet(
                points=[
                    cgm_enums.PolygonSetEntry(
                        pt=cgm_enums.Point(x=2830, y=-2183),
                        linetype=cgm_enums.PolygonSetLineTypeEnum.VISIBLE,
                    ),
                    cgm_enums.PolygonSetEntry(
                        pt=cgm_enums.Point(x=2046, y=-2182),
                        linetype=cgm_enums.PolygonSetLineTypeEnum.VISIBLE,
                    ),
                    cgm_enums.PolygonSetEntry(
                        pt=cgm_enums.Point(x=1261, y=-2182),
                        linetype=cgm_enums.PolygonSetLineTypeEnum.VISIBLE,
                    ),
                    cgm_enums.PolygonSetEntry(
                        pt=cgm_enums.Point(x=-797, y=-2182),
                        linetype=cgm_enums.PolygonSetLineTypeEnum.VISIBLE,
                    ),
                    cgm_enums.PolygonSetEntry(
                        pt=cgm_enums.Point(x=-2856, y=-2182),
                        linetype=cgm_enums.PolygonSetLineTypeEnum.VISIBLE,
                    ),
                    cgm_enums.PolygonSetEntry(
                        pt=cgm_enums.Point(x=-2962, y=-2181),
                        linetype=cgm_enums.PolygonSetLineTypeEnum.INVISIBLE,
                    ),
                ]
            ),
        ),
        (
            b"\x11f\x00\x01\xff\xff\x00\x01",
            cgm_parse.MetafileElementList(elements=[(-1, 1)]),
        ),
        (
            b"\x11F\x00\x00\x00\xff\xff\xff",
            cgm_parse.ColourValueExtent(
                minimum=cgm_enums.DirectColour(v1=0, v2=0, v3=0, v4=0),
                maximum=cgm_enums.DirectColour(v1=255, v2=255, v3=255),
                first_component=0,
                second_component=0,
                third_component=0,
            ),
        ),
    ],
)
def test_cgm_parse(binary, expected):
    parser = cgm_parse.BinaryCGMCommandParser(BytesIO(binary))
    result = list(parser.parse())
    assert result == [expected]


@pytest.mark.parametrize(
    "commands, results",
    [
        # Float precision
        (
            b"\x10\xa6\x00\x00\x00\t\x00\x17"
            b"\x10b\x00\x01"
            b"@0\x00R\x80\x00\x00\r@\x00\x00O\xc0\x00\x00\x17@\x00",
            [
                cgm_parse.RealPrecision(
                    representation=cgm_enums.RealTypeEnum.FLOATING_POINT,
                    exponent_whole_size=9,
                    fraction_size=23,
                ),
                cgm_parse.VdcType(vdc_type=cgm_enums.VDCTypeEnum.REAL),
                cgm_parse.Polyline(
                    points=[
                        cgm_enums.Point(x=82.5, y=13.25),
                        cgm_enums.Point(x=79.75, y=23.25),
                    ]
                ),
            ],
        ),
        # Check that we always parse an even number of bytes, even
        # though an odd length is specified
        (
            b'\x00!\x00\xab\x10"\x00\x01',
            [cgm_parse.BeginMetafile(""), cgm_parse.MetafileVersion(1)],
        ),
        # Absolute width
        (
            b" \xa2\x00\x00S\x82\x00\x03",
            [
                cgm_parse.EdgeWidthSpecificationMode(
                    size_specification=cgm_enums.WidthSpecificationModeEnum.ABSOLUTE
                ),
                cgm_parse.EdgeWidth(width=3.0),
            ],
        ),
        # Scaled width
        (
            b" \xa2\x00\x01S\x84\x00\x03\x00\x00",
            [
                cgm_parse.EdgeWidthSpecificationMode(
                    size_specification=cgm_enums.WidthSpecificationModeEnum.SCALED
                ),
                cgm_parse.EdgeWidth(width=3.0),
            ],
        ),
        # Direct color, one padding byte at the end
        (
            b" B\x00\x01\x10\xe2\x00\x08R\xe3\xff\x00\xff\xab",
            [
                cgm_parse.ColourSelectionMode(
                    colour_selection_mode=cgm_enums.ColourSelectionModeEnum.DIRECT
                ),
                cgm_parse.ColourPrecision(colour_precision=8),
                cgm_parse.FillColour(
                    fill=cgm_enums.DirectColour(v1=255, v2=0, v3=255, v4=0)
                ),
            ],
        ),
        # Indexed colour selection mode incl. padding byte
        (
            b" B\x00\x00\x11\x02\x00\x08P\x81\x04\xab",
            [
                cgm_parse.ColourSelectionMode(
                    colour_selection_mode=cgm_enums.ColourSelectionModeEnum.INDEXED
                ),
                cgm_parse.ColourIndexPrecision(colour_index_precision=8),
                cgm_parse.LineColour(colour=cgm_enums.IndexColour(index=4)),
            ],
        ),
        # Some more parser modifier commands
        (
            b"\x10\x82\x00\x10\x10\xc2\x00\x10 b\x00\x01 \x82\x00\x00",
            [
                cgm_parse.IntegerPrecision(integer_precision=16),
                cgm_parse.IndexPrecision(index_precision=16),
                cgm_parse.LineWidthSpecificationMode(
                    size_specification=cgm_enums.WidthSpecificationModeEnum.SCALED
                ),
                cgm_parse.MarkerSizeSpecificationMode(
                    size_specification=cgm_enums.WidthSpecificationModeEnum.ABSOLUTE
                ),
            ],
        ),
        (
            b"0F\x00\x01\x00\x10\x00\x10",
            [
                cgm_parse.VDCRealPrecision(
                    representation=cgm_enums.RealTypeEnum.FIXED_POINT,
                    exponent_whole_size=16,
                    fraction_size=16,
                )
            ],
        ),
    ],
)
def test_cgm_parse_multiple(commands, results):
    parser = cgm_parse.BinaryCGMCommandParser(BytesIO(commands))
    result = list(parser.parse())
    assert result == results
