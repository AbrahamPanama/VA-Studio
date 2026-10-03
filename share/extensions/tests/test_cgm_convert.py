from io import BytesIO
import cgm_input
import cgm_enums
import cgm_parse

import pytest

import inkex


def stream_commands(*commands):
    conv = cgm_input.CGMConverter()
    for command in commands:
        conv.stream(command)
    return conv.svg


def test_polygon_set():
    """Test polygon sets with re-shuffling and points, as well as fill and stroke."""
    data = [
        cgm_parse.BeginPicture(""),
        cgm_parse.PolygonSet(
            [
                cgm_enums.PolygonSetEntry(
                    cgm_enums.Point(500, 500), cgm_enums.PolygonSetLineTypeEnum.VISIBLE
                ),
                cgm_enums.PolygonSetEntry(
                    cgm_enums.Point(800, 1000),
                    cgm_enums.PolygonSetLineTypeEnum.INVISIBLE,
                ),
                cgm_enums.PolygonSetEntry(
                    cgm_enums.Point(1100, 500),
                    cgm_enums.PolygonSetLineTypeEnum.CLOSE_VISIBLE,
                ),
                cgm_enums.PolygonSetEntry(
                    cgm_enums.Point(600, 600), cgm_enums.PolygonSetLineTypeEnum.VISIBLE
                ),
                cgm_enums.PolygonSetEntry(
                    cgm_enums.Point(800, 800), cgm_enums.PolygonSetLineTypeEnum.VISIBLE
                ),
                cgm_enums.PolygonSetEntry(
                    cgm_enums.Point(1000, 600),
                    cgm_enums.PolygonSetLineTypeEnum.CLOSE_VISIBLE,
                ),
            ]
        ),
    ]

    svg = stream_commands(*data)

    assert (
        str(svg[1][0].path)
        == "M 800 1000 L 1100 500 L 500 500 Z M 600 600 L 800 800 L 1000 600 Z"
    )
    assert (
        str(svg[1][1].path)
        == "M 800 1000 M 1100 500 L 500 500 L 800 1000 M 600 600 L 800 800 L 1000 600 L 600 600 Z"
    )


def test_defaults_replacement():
    """Check that the metafile defaults replacement is applied to every page"""
    svg = stream_commands(
        cgm_parse.MetafileDefaultsReplacement(
            data=(
                cgm_parse.VDCExtent(
                    first_corner=cgm_enums.Point(x=0, y=0),
                    second_corner=cgm_enums.Point(x=4095, y=4095),
                ),
            )
        ),
        cgm_parse.BeginPicture(""),
        cgm_parse.EndPicture(),
        cgm_parse.BeginPicture(""),
        cgm_parse.EndPicture(),
    )
    # Check that we now have two pages with the specified width/height
    assert len(svg.namedview) == 2
    assert isinstance(svg.namedview[0], inkex.Page)
    assert svg.namedview[0].width == 4095
    assert svg.namedview[0].height == 4095
    assert isinstance(svg.namedview[1], inkex.Page)
    assert svg.namedview[1].width == 4095
    assert svg.namedview[1].height == 4095


def test_rectangle():
    """Draw a rectangle (fill + edge) with indexed colours"""
    svg = stream_commands(
        cgm_parse.ColourTable(
            colour_index=cgm_enums.IndexColour(index=0),
            colours=[
                cgm_enums.DirectColour(v1=255, v2=255, v3=255, v4=0),
                cgm_enums.DirectColour(v1=255, v2=0, v3=0, v4=0),
                cgm_enums.DirectColour(v1=255, v2=0, v3=255, v4=0),
                cgm_enums.DirectColour(v1=255, v2=255, v3=0, v4=0),
            ],
        ),
        cgm_parse.BeginPicture(""),
        cgm_parse.FillColour(cgm_enums.IndexColour(index=2)),
        cgm_parse.EdgeColour(cgm_enums.IndexColour(index=3)),
        cgm_parse.Rectangle(
            p1=cgm_enums.Point(x=10, y=20), p2=cgm_enums.Point(x=30, y=50)
        ),
        cgm_parse.EdgeVisibility(visibility=cgm_enums.EdgeVisibilityEnum.ON),
        cgm_parse.InteriorStyle(interior_style=cgm_enums.InteriorStyleEnum.SOLID),
        cgm_parse.Rectangle(
            p1=cgm_enums.Point(x=30, y=40), p2=cgm_enums.Point(x=70, y=50)
        ),
    )

    assert len(svg[1]) == 2
    assert isinstance(svg[1][0], inkex.Rectangle)
    assert svg[1][0].left == 10
    assert svg[1][0].top == 20
    assert svg[1][0].width == 20
    assert svg[1][0].height == 30

    assert svg[1][0].style("fill") == None
    assert svg[1][0].style("stroke") == None
    assert isinstance(svg[1][1], inkex.Rectangle)
    assert svg[1][1].style("fill") == inkex.ColorRGB([255, 0, 255])
    assert svg[1][1].style("stroke") == inkex.ColorRGB([255, 255, 0])


def test_circle():
    """Draw a circle, assert fill and stroke"""
    svg = stream_commands(
        cgm_parse.BeginPicture(""),
        cgm_parse.FillColour(cgm_enums.DirectColour(255, 255, 0)),
        cgm_parse.InteriorStyle(interior_style=cgm_enums.InteriorStyleEnum.SOLID),
        cgm_parse.EdgeColour(cgm_enums.DirectColour(0, 127, 127)),
        cgm_parse.EdgeVisibility(visibility=cgm_enums.EdgeVisibilityEnum.ON),
        cgm_parse.Circle(cgm_enums.Point(x=10, y=20), 10),
    )

    assert len(svg[1]) == 1
    circle = svg[1][0]
    assert isinstance(circle, inkex.Circle)
    assert circle.center == 10 + 20j
    assert circle.radius == 10
    assert circle.style("fill") == inkex.ColorRGB([255, 255, 0])
    assert circle.style("stroke") == inkex.ColorRGB([0, 127, 127])


def test_polyline():
    """Draw a polyline, assert no fill and stroke taken from Edge config"""
    svg = stream_commands(
        cgm_parse.BeginPicture(""),
        cgm_parse.FillColour(cgm_enums.DirectColour(255, 255, 0)),
        cgm_parse.InteriorStyle(interior_style=cgm_enums.InteriorStyleEnum.SOLID),
        cgm_parse.EdgeColour(cgm_enums.DirectColour(0, 127, 127)),
        cgm_parse.LineColour(cgm_enums.DirectColour(0, 30, 40)),
        cgm_parse.EdgeVisibility(visibility=cgm_enums.EdgeVisibilityEnum.ON),
        cgm_parse.Polyline(
            points=[
                cgm_enums.Point(x=580, y=286),
                cgm_enums.Point(x=630, y=329),
                cgm_enums.Point(x=636, y=325),
                cgm_enums.Point(x=630, y=329),
            ]
        ),
        cgm_parse.DisjointPolyline(
            points=[
                cgm_enums.Point(x=390, y=388),
                cgm_enums.Point(x=388, y=386),
                cgm_enums.Point(x=369, y=391),
                cgm_enums.Point(x=369, y=398),
            ]
        ),
    )

    assert len(svg[1]) == 2
    polyline = svg[1][0]
    assert isinstance(polyline, inkex.PathElement)
    assert polyline.path == inkex.Path("M 580, 286 L 630, 329 L 636 325 L 630 329")
    assert polyline.style("fill") == None
    assert polyline.style("stroke") == inkex.ColorRGB([0, 30, 40])

    disjoint = svg[1][1]
    assert isinstance(disjoint, inkex.PathElement)
    assert disjoint.path == inkex.Path("M 390 388 L 388 386 M 369 391 L 369 398")
    assert disjoint.style("fill") == None
    assert disjoint.style("stroke") == inkex.ColorRGB([0, 30, 40])


@pytest.mark.parametrize(
    "linestyle, expected",
    [
        (cgm_enums.LineTypeEnum.DASH, [9, 9]),
        (cgm_enums.LineTypeEnum.SOLID, []),
        (cgm_enums.LineTypeEnum.PRIVATE, []),
        (cgm_enums.LineTypeEnum.DASH_DOT, [12, 6, 3, 6]),
        (cgm_enums.LineTypeEnum.DOT, [3, 3]),
        (cgm_enums.LineTypeEnum.DASH_DOT_DOT, [12, 6, 3, 6, 3, 6]),
    ],
)
def test_line_styles(linestyle, expected):
    svg = stream_commands(
        cgm_parse.BeginPicture(""),
        cgm_parse.EdgeColour(cgm_enums.DirectColour(0, 127, 127)),
        cgm_parse.EdgeWidth(3),
        cgm_parse.EdgeType(linestyle),
        cgm_parse.EdgeVisibility(visibility=cgm_enums.EdgeVisibilityEnum.ON),
        cgm_parse.Circle(cgm_enums.Point(x=10, y=20), 10),
    )
    assert len(svg[1]) == 1
    assert svg[1][0].style("stroke-dasharray") == expected


@pytest.mark.parametrize(
    "command, expected",
    [
        (
            cgm_parse.Polybezier(
                cgm_enums.PolybezierContinuityEnum.CONTINUOUS,
                [
                    cgm_enums.Point(5, 5),
                    cgm_enums.Point(5, 10),
                    cgm_enums.Point(10, 10),
                    cgm_enums.Point(10, 5),
                    cgm_enums.Point(10, 0),
                    cgm_enums.Point(15, 0),
                    cgm_enums.Point(15, 5),
                ],
            ),
            f"M 5 5 C 5 10 10 10 10 5 C 10 0 15 0 15 5",
        ),
        (
            cgm_parse.Polybezier(
                cgm_enums.PolybezierContinuityEnum.DISCONTINUOUS,
                [
                    cgm_enums.Point(5, 5),
                    cgm_enums.Point(5, 10),
                    cgm_enums.Point(10, 10),
                    cgm_enums.Point(10, 5),
                    cgm_enums.Point(10, 2),
                    cgm_enums.Point(10, 0),
                    cgm_enums.Point(15, 0),
                    cgm_enums.Point(15, 5),
                ],
            ),
            f"M 5 5 C 5 10 10 10 10 5 M 10 2 C 10 0 15 0 15 5",
        ),
    ],
)
def test_polybezier(command, expected):
    svg = stream_commands(
        cgm_parse.BeginPicture(""),
        cgm_parse.LineColour(cgm_enums.DirectColour(0, 127, 127)),
        cgm_parse.LineWidth(3),
        cgm_parse.LineType(cgm_enums.LineTypeEnum.SOLID),
        command,
    )
    assert len(svg[1]) == 1
    assert int(svg[1][0].style("stroke-width")) == 3
    assert svg[1][0].path == inkex.Path(expected)


@pytest.mark.parametrize(
    "command, expected",
    [
        (
            cgm_parse.Ellipse(
                cgm_enums.Point(5, 5), cgm_enums.Point(5, 10), cgm_enums.Point(10, 10)
            ),
            f"M 0 0 a 8.09017 3.09017 58.2825 1 1 5 10 a 8.09017 3.09017 58.2825 0 1 -5 -10 z",
        ),
        (
            cgm_parse.CircularArc3Point(
                cgm_enums.Point(5, 5), cgm_enums.Point(5, 10), cgm_enums.Point(10, 10)
            ),
            f"m 10 10 a 3.53553 3.53553 0 0 1 -5 0 a 3.53553 3.53553 0 0 1 -2.66454e-15 -5",
        ),
        (
            cgm_parse.CircularArc3PointClose(
                cgm_enums.Point(5, 5),
                cgm_enums.Point(5, 10),
                cgm_enums.Point(8, 11),
                cgm_enums.ArcClosureEnum.CHORD,
            ),
            f"m 8 11 a 3.53553 3.53553 0 0 1 -3.66228 -1.91886 a 3.53553 3.53553 0 0 1 0.662278 -4.08114 z",
        ),
        (
            cgm_parse.CircularArc3PointClose(
                cgm_enums.Point(5, 5),
                cgm_enums.Point(5, 10),
                cgm_enums.Point(8, 11),
                cgm_enums.ArcClosureEnum.PIE,
            ),
            f"m 8 11 a 3.53553 3.53553 0 0 1 -3.66228 -1.91886 a 3.53553 3.53553 0 0 1 0.662278 -4.08114 l 2.5 2.5 z",
        ),
        (
            cgm_parse.CircularArcCentre(cgm_enums.Point(5, 5), 3, 6, 0, 5, 5),
            f"m 7.23607 9.47214 a 5 5 0 0 1 -2.23607 0.527864",
        ),
        (
            cgm_parse.CircularArcCentreClose(
                cgm_enums.Point(5, 5), 3, 6, 0, 5, 5, cgm_enums.ArcClosureEnum.PIE
            ),
            f"m 7.23607 9.47214 a 5 5 0 0 1 -2.23607 0.527864 l 0 -5 z",
        ),
    ],
)
def test_ellipse_arc(command, expected):
    svg = stream_commands(cgm_parse.BeginPicture(""), command)
    assert len(svg[1]) == 1
    assert svg[1][0].path == inkex.Path(expected), str(svg[1][0].path)


def test_text():
    svg = stream_commands(
        cgm_parse.BeginPicture(""),
        cgm_parse.FontList(fonts=["Times-Roman", "Helvetica"]),
        cgm_parse.TextFontIndex(index=2),
        cgm_parse.CharacterHeight(10),
        cgm_parse.CharacterOrientation(0, 10, 10, 0),
        cgm_parse.TextColour(cgm_enums.DirectColour(255, 120, 5)),
        cgm_parse.TextAlignment(
            cgm_enums.TextHorizontalAlignmentEnum.CENTER,
            cgm_enums.TextVerticalAlignmentEnum.HALF,
            0,
            0,
        ),
        cgm_parse.CharacterExpansionFactor(1.5),
        cgm_parse.CharacterSpacing(1.5),
        cgm_parse.Text(
            point=cgm_enums.Point(x=105.0, y=290.0),
            flag=cgm_enums.TextFinalFlag.FINAL,
            string="Text in Helvetica",
        ),
    )
    assert len(svg[1]) == 1
    text = svg[1][0]
    assert len(text) == 1
    assert isinstance(text, inkex.TextElement)
    assert isinstance(text[0], inkex.Tspan)
    assert text[0].text == "Text in Helvetica"
    assert text[0].style("font-family") == "Helvetica"
    assert float(text.get("x")) == 105
    assert float(text.get("y")) == -290
    assert text[0].style("font-size") == 10
    assert text.style("font-size") == 10
    assert text.style("text-anchor") == "middle"
    assert text[0].style("fill") == inkex.ColorRGB([255, 120, 5])
    assert text[0].style("font-stretch") == "150.0%"
    assert text.get("dy") == "0.4em"
    assert text[0].style("letter-spacing") == "1.5"
    assert text.transform == inkex.Transform((1, 0, 0, -1, 0, 0))
