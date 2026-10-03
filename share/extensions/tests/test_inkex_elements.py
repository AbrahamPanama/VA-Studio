#!/usr/bin/env python3
# coding=utf-8
"""
Test specific elements API from svg xml lxml custom classes.
"""

import pytest
import math
import inkex

from inkex import (
    Group,
    Layer,
    Pattern,
    Guide,
    Page,
    Polyline,
    Use,
    Defs,
    TextElement,
    TextPath,
    Tspan,
    FlowPara,
    FlowRoot,
    FlowRegion,
    FlowSpan,
    PathElement,
    Rectangle,
    Circle,
    Ellipse,
    Anchor,
    Line as LineElement,
    Transform,
    Style,
    LinearGradient,
    RadialGradient,
    Stop,
)
from inkex import paths
from inkex.colors import Color
from inkex.paths import Move, Line
from inkex.utils import FragmentError
from inkex.units import parse_unit, convert_unit
from inkex.transforms import BoundingBox

from .test_inkex_elements_base import SvgTestCase
from inkex.tester.svg import svg


class ElementTestCase(SvgTestCase):
    """Base element testing"""

    tag = "svg"

    def setUp(self):
        super().setUp()
        self.elem = self.svg.getElement("//svg:{}".format(self.tag))

    def test_print(self):
        """Print element as string"""
        self.assertEqual(str(self.elem), self.tag)

    def assertElement(self, elem, compare):  # pylint: disable=invalid-name
        """Assert an element"""
        self.assertEqual(elem.tostring(), compare)


class PathElementTestCase(ElementTestCase):
    """Test PathElements"""

    source_file = "with-lpe.svg"
    tag = "path"

    def test_new_path(self):
        """Test new path element"""
        path = PathElement.new(path=[Move(10, 10), Line(20, 20)])
        self.assertEqual(path.get("d"), "M 10 10 L 20 20")

    def test_original_path(self):
        """LPE paths can return their original paths"""
        lpe = self.svg.getElementById("lpe")
        nolpe = self.svg.getElementById("nolpe")
        self.assertEqual(str(lpe.path), "M 30 30 L -10 -10 Z")
        self.assertEqual(str(lpe.original_path), "M 20 20 L 10 10 Z")
        self.assertEqual(str(nolpe.path), "M 30 30 L -10 -10 Z")
        self.assertEqual(str(nolpe.original_path), "M 30 30 L -10 -10 Z")

        lpe.original_path = "M 60 60 L 5 5"
        self.assertEqual(lpe.get("inkscape:original-d"), "M 60 60 L 5 5")
        self.assertEqual(lpe.get("d"), "M 30 30 L -10 -10 Z")

        lpe.path = "M 60 60 L 15 15 Z"
        self.assertEqual(lpe.get("d"), "M 60 60 L 15 15 Z")

        nolpe.original_path = "M 60 60 L 5 5"
        self.assertEqual(nolpe.get("inkscape:original-d", None), None)
        self.assertEqual(nolpe.get("d"), "M 60 60 L 5 5")

    def _compare_paths(self, result, reference, precision=4):
        reference = inkex.Path(reference)
        result = inkex.Path(result)
        self.assertEqual(len(reference), len(result))
        for c1, c2 in zip(reference, result):
            self.assertEqual(c1.letter, c2.letter)
            self.assertAlmostTuple(c1.args, c2.args, precision=precision, msg=result)

    def test_arc(self):
        """Test arc generation"""

        def compare_arc(cx, cy, rx, ry, start, end, reference, type="arc"):
            arc = PathElement.arc((cx, cy), rx, ry, start=start, end=end, arctype=type)
            self.assertEqual(arc.get("sodipodi:arc-type"), type)
            self._compare_paths(arc.get("d"), reference)

        compare_arc(
            10,
            20,
            5,
            5,
            math.pi / 4,
            math.pi * 6 / 4,
            """m 13.535534,23.535534 a 5,5 0 0 1 -6.035534,0.794593
                       5,5 0 0 1 -2.3296291,-5.624222 5,5 0 0 1 4.8296291,-3.705905""",
        )
        compare_arc(
            10,
            20,
            5,
            5,
            math.pi / 4,
            math.pi * 6 / 4,
            """m 13.535534,23.535534 a 5,5 0 0 1 -6.035534,0.794593
                       5,5 0 0 1 -2.3296291,-5.624222 5,5 0 0 1 4.8296291,-3.705905 z""",
            type="chord",
        )
        compare_arc(
            10,
            20,
            5,
            5,
            math.pi / 4,
            math.pi * 28 / 18,
            """m 13.535534,23.535534 a 5,5 0 0 1 -6.2830789,0.641905 5,5 0 0 1 -1.8991927,-6.02347
                       5,5 0 0 1 5.5149786,-3.078008 l -0.868241,4.924039 z""",
            type="slice",
        )

    def test_stars(self):
        def compare_star(
            cx,
            cy,
            sides,
            r1,
            r2,
            arg1,
            arg2,
            flatsided,
            rounded,
            reference,
            precision=4,
        ):
            star = PathElement.star(
                (cx, cy), (r1, r2), sides, rounded, (arg1, arg2), flatsided
            )
            self.assertEqual(star.get("inkscape:flatsided"), str(flatsided).lower())
            self._compare_paths(star.get("d"), reference, precision=precision)

        # Test a simple polygon
        compare_star(
            10,
            5,
            6,
            5.5,
            10,
            7 / 8 * math.pi,
            42,
            True,
            0,
            """m 4.9186625,7.1047587 0.7178942,-5.4529467 5.0813373,-2.10475872
            4.363443,3.34818802 -0.717894,5.4529467 -5.0813372,2.104759 z""",
        )
        # Test a star
        compare_star(
            5,
            10,
            7,
            35,
            17,
            0.95545678,
            1.4790556,
            False,
            0,
            """m 25.203254,38.580212 -18.6458484,-11.651701 -11.3057927,16.6865
           -2.5158293,-21.842628 -20.0950776,1.564637 15.508661,-15.5856099
           -13.752359,-14.7354284 21.8548124,2.4076898 2.94616632,-19.9394165
           11.74384528,18.5879506 17.426168,-10.1286176 -7.210477,20.7711057
           18.783909,7.3092373 -20.735163,7.313194 z""",
        )
        # Test a rounded polygon
        compare_star(
            10,
            5,
            6,
            5.5,
            10,
            7 / 8 * math.pi,
            42,
            True,
            0.1,
            """m 4.9186625,7.1047587 c -0.2104759,-0.5081337 0.3830754,-5.0166024 0.7178942,-5.4529467
            0.3348188,-0.4363443 4.5360433,-2.17654814 5.0813373,-2.10475872
            0.545295,0.0717894 4.152968,2.84005422 4.363443,3.34818802
            0.210476,0.5081337 -0.383075,5.0166024 -0.717894,5.4529467
            -0.334819,0.4363443 -4.5360425,2.176548 -5.0813372,2.104759
            -0.5452947,-0.07179 -4.1529674,-2.8400545 -4.3634433,-3.3481883 z""",
        )
        compare_star(
            5,
            10,
            7,
            35,
            17,
            0.95545678,
            1.4790556,
            False,
            1,
            """m 25.203254,38.580212 c -18.8526653,11.31401 3.036933,-15.296807 -18.6458484,-11.651701
            -19.8769816,3.341532 7.5786704,23.731873 -11.3057927,16.6865
            -20.6000929,-7.685438 13.8530224,-7.163034 -2.5158293,-21.842628
            -15.0056106,-13.4570388 -13.8291026,20.721824 -20.0950776,1.564637
            -6.835231,-20.8975929 14.237504,6.364651 15.508661,-15.5856099
            1.165291,-20.1221851 -24.823279,2.1078182 -13.752359,-14.7354284
            12.076699,-18.3734357 3.900854,15.0996232 21.8548124,2.4076898
            16.4587046,-11.6349155 -17.1250194,-18.0934175 2.94616632,-19.9394165
            21.89462928,-2.013706 -9.37321772,12.464272 11.74384528,18.5879506
            19.358377,5.61368249 3.468728,-24.6699406 17.426168,-10.1286176
            15.225456,15.86238583 -15.589066,0.44307 -7.210477,20.7711057
            7.680797,18.6350633 21.450453,-12.6694955 18.783909,7.3092373
            -2.908795,21.793777 -10.066029,-11.91177319 -20.735163,7.313194
            -9.7805797,17.623861 23.27955,8.871339 5.996985,19.243087 z""",
            precision=2,
        )

    def test_context_manager(self):
        """Test that modifications inside the context manager are written back"""
        pel = PathElement.new(path=[Move(10, 10), Line(20, 20)])
        with pel.path as path:
            path.append(Line(20, 10))
            path.close()

        assert str(pel.path) == "M 10 10 L 20 20 L 20 10 Z"

        pline = Polyline.new(points="10,10 50,50 10,15 15,10")
        with pline.path as path:
            path.append(Line(20, 10))

        assert pline.get("points", "10,10 50,50 10,15 15,10, 20,10")


class PolylineElementTestCase(ElementTestCase):
    """Test the polyline elements support"""

    tag = "polyline"

    def test_type(self):
        """Polyline have their own types"""
        self.assertTrue(isinstance(self.elem, inkex.Polyline))

    def test_polyline_points(self):
        """Basic tests for points attribute as a path"""
        pol = Polyline(points="10,10 50,50 10,15 15,10")
        self.assertEqual(str(pol.path), "M 10 10 L 50 50 L 10 15 L 15 10")
        pol.path = "M 10 10 L 30 9 L 1 2 C 10 45 3 4 45 60 M 35 35"
        self.assertEqual(pol.get("points"), "10,10 30,9 1,2 45,60 35,35")
        pol = Polyline.new(points="M 10 10 L 50 50 L 10 15 L 15 10")
        self.assertEqual(pol.get("points"), "10,10 50,50 10,15 15,10")
        pol = Polyline.new(points="10,10 50,50 10,15 5,0")
        self.assertEqual(pol.get("points"), "10,10 50,50 10,15 5,0")
        pol = Polyline.new(points=[(1, 2), (3, 4), (5, 6)])
        self.assertEqual(str(pol.path), "M 1 2 L 3 4 L 5 6")


class PolygonElementTestCase(ElementTestCase):
    """Test Polygon Elements"""

    tag = "polygon"

    def test_type(self):
        """Polygons have their own types"""
        self.assertTrue(isinstance(self.elem, inkex.Polygon))

    def test_conversion(self):
        """Polygones are converted to paths"""
        pol = inkex.Polygon(points="10,10 50,50 10,15 15,10")
        self.assertEqual(str(pol.path), "M 10 10 L 50 50 L 10 15 L 15 10 Z")
        pol = inkex.Polygon.new(points=[(1, 2), (3, 4), (5, 6)])
        self.assertEqual(str(pol.path), "M 1 2 L 3 4 L 5 6 Z")
        pol.path = inkex.Path("M 20 20 L 30 10 L 10 40")
        self.assertEqual(str(pol.path), "M 20 20 L 30 10 L 10 40 Z")


class LineElementTestCase(ElementTestCase):
    """Test Line Elements"""

    tag = "line"

    def test_new_line(self):
        """Line creation"""
        line = LineElement.new((10, 10), (20, 20))
        self.assertElement(line, b'<line x1="10.0" y1="10.0" x2="20.0" y2="20.0"/>')

    def test_type(self):
        """Lines have their own types"""
        self.assertTrue(isinstance(self.elem, inkex.Line))

    def test_conversion(self):
        """Lines are converted to paths"""
        pol = inkex.elements.Line(x1="2", y1="3", x2="4", y2="5")
        self.assertEqual(str(pol.path), "M 2 3 L 4 5")


class PatternTestCase(ElementTestCase):
    """Test Pattern elements"""

    tag = "pattern"

    def test_pattern_transform(self):
        """Patterns have a transformation of their own"""
        pattern = Pattern()
        self.assertEqual(pattern.patternTransform, Transform())
        pattern.patternTransform.add_translate(10, 10)
        self.assertEqual(pattern.get("patternTransform"), "translate(10, 10)")


class GroupTest(ElementTestCase):
    """Test extra functionality on a group element"""

    tag = "g"

    def test_new_group(self):
        """Test creating groups"""
        svg = Layer.new("layerA", Group.new("groupA", Rectangle()))
        self.assertElement(
            svg,
            b'<g inkscape:groupmode="layer" inkscape:label="layerA">'
            b'<g inkscape:label="groupA"><rect/></g></g>',
        )

    def test_transform_property(self):
        """Test getting and setting a transform"""
        self.assertEqual(
            str(self.elem.transform), "matrix(1.44985 0 0 1.36417 -107.03 -167.362)"
        )
        self.elem.transform = "translate(12, 14)"
        self.assertEqual(self.elem.transform, Transform("translate(12, 14)"))
        self.assertEqual(str(self.elem.transform), "translate(12, 14)")

    def test_groupmode(self):
        """Get groupmode is layer"""
        self.assertEqual(self.svg.getElementById("A").groupmode, "layer")
        self.assertEqual(self.svg.getElementById("C").groupmode, "group")

    def test_get_path(self):
        """Group path is combined children"""
        print(str(self.svg.getElementById("A").get_path()))
        self.assertEqual(
            str(self.svg.getElementById("A").get_path()),
            "M -108.539 517.61 L -87.6093 496.117 L -98.3066 492.768 L -69.9353 492.301 L -55.5172"
            " 506.163 L -66.2146 502.814 L -87.1446 524.307 M 60.0914 498.694 L 156.784 439.145 L"
            " 240.218 491.183 L 143.526 550.731 z M -176.909 458.816 a 64.2385 38.9175 -7.86457 1"
            " 0 88.3701 -19.0784 a 64.2385 38.9175 -7.86457 0 0 -88.3701 19.0784 z M -300.162"
            " 513.715 L -282.488 509.9 Z M -214.583 540.504 L -209.001 448.77 M -193.189 547.201 "
            "L -238.536 486.266 L -185.049 503.008 L -230.396 442.073 M -193.189 547.201 L -238.536"
            " 486.266 L -185.049 503.008 L -230.396 442.073 Z M 15 15 L 15.5 20",
        )

    def test_bounding_box(self):
        """A group returns a bounding box"""
        empty = self.svg.add(Group(Group()))
        self.assertEqual(empty.bounding_box(), None)
        self.assertEqual(int(self.svg.getElementById("A").bounding_box().width), 783)
        self.assertEqual(int(self.svg.getElementById("B").bounding_box().height), 114)


class RectTest(ElementTestCase):
    """Test extra functionality on a rectangle element"""

    tag = "rect"

    def test_parse(self):
        """Test Rectangle parsed from XML"""
        rect = Rectangle(
            attrib={
                "x": "10px",
                "y": "20px",
                "width": "100px",
                "height": "200px",
                "rx": "15px",
                "ry": "30px",
            }
        )
        self.assertEqual(rect.left, 10)
        self.assertEqual(rect.top, 20)
        self.assertEqual(rect.right, 10 + 100)
        self.assertEqual(rect.bottom, 20 + 200)
        self.assertEqual(rect.width, 100)
        self.assertEqual(rect.height, 200)
        self.assertEqual(rect.rx, 15)
        self.assertEqual(rect.ry, 30)

    def test_compose_transform(self):
        """Composed transformation"""
        self.assertEqual(self.elem.transform, Transform("rotate(16.097889)"))
        self.assertEqual(
            str(self.elem.composed_transform()),
            "matrix(1.4019 -0.812338 1.20967 0.709877 -542.221 533.431)",
        )

    def test_effetive_stylesheet(self):
        """Test the non-parent combination of styles"""
        self.assertEqual(
            str(self.elem.effective_style()), "fill:#0000ff;stroke-width:1px"
        )
        self.assertEqual(
            str(self.elem.getparent().effective_style()),
            "fill:#0000ff;stroke-width:1px;stroke:#f00",
        )

    def test_compose_stylesheet(self):
        """Test finding the composed stylesheet for the shape"""
        self.assertEqual(str(self.elem.style), "fill:#0000ff;stroke-width:1px")
        self.assertEqual(
            str(self.elem.specified_style()),
            "fill:#0000ff;stroke-width:1px;stroke:#d88",
        )

    def test_path(self):
        """Rectangle path"""
        self.assertEqual(
            self.elem.get_path(), inkex.Path("M 200.0,200.0 h100.0v100.0h-100.0 z")
        )
        self.assertEqual(str(self.elem.path), "M 200 200 h 100 v 100 h -100 z")

    def test_bad_ry(self):
        """Test for https://gitlab.com/inkscape/extensions/-/issues/579"""
        rect = Rectangle(
            attrib={
                "x": "53.4809",
                "y": "40.852928",
                "width": "51.448818",
                "height": "44.433071",
                "rx": "40.130795",
                "ry": "1.6217518e-12",
            }
        )
        self.assertEqual(rect.ry, 1.6217518e-12)

        # Transform this path
        transformed = inkex.Path(rect.get_path()).transform(
            inkex.Transform("translate(10, 0)")
        )

        assert isinstance(transformed[2], paths.Arc)
        assert transformed[2].radius == 25.724409 + 1.6217518e-12j


class PathTest(ElementTestCase):
    """Test path extra functionality"""

    tag = "path"

    def test_apply_transform(self):
        """Transformation can be applied to path"""
        path = self.svg.getElementById("D")
        path.transform = Transform(translate=(10, 10))
        self.assertEqual(
            path.get("d"), "M30,130 L60,130 L60,120 L70,140 L60,160 L60,150 L30,150"
        )
        path.apply_transform()
        self.assertEqual(
            path.get("d"),
            "M 40 140 L 70 140 L 70 130 L 80 150 L 70 170 L 70 160 L 40 160",
        )
        self.assertFalse(path.transform)


class CircleTest(ElementTestCase):
    """Test extra functionality on a circle element"""

    tag = "circle"

    def test_parse(self):
        """Test Circle parsed from XML"""
        circle = Circle(attrib={"cx": "10px", "cy": "20px", "r": "30px"})
        self.assertEqual(circle.center.x, 10)
        self.assertEqual(circle.center.y, 20)
        self.assertEqual(circle.radius, 30)
        ellipse = Ellipse(
            attrib={"cx": "10px", "cy": "20px", "rx": "30px", "ry": "40px"}
        )
        self.assertEqual(ellipse.center.x, 10)
        self.assertEqual(ellipse.center.y, 20)
        self.assertEqual(ellipse.radius.x, 30)
        self.assertEqual(ellipse.radius.y, 40)

    def test_new(self):
        """Test new circles"""
        elem = Circle.new((10, 10), 50)
        self.assertElement(elem, b'<circle cx="10.0" cy="10.0" r="50.0"/>')
        elem = Ellipse.new((10, 10), (15, 10))
        self.assertElement(elem, b'<ellipse cx="10.0" cy="10.0" rx="15.0" ry="10.0"/>')

    def test_path(self):
        """Circle path"""
        self.assertEqual(
            self.elem.get_path(),
            inkex.Path(
                "M 100.0,50.0 a 50.0,50.0 0 1 0 50.0, "
                "50.0 a 50.0,50.0 0 0 0 -50.0, -50.0 z"
            ),
        )


class AnchorTest(ElementTestCase):
    """Test anchor tags"""

    def test_new(self):
        """Anchor tag creation"""
        link = Anchor.new("https://inkscape.org", Rectangle())
        self.assertElement(link, b'<a xlink:href="https://inkscape.org"><rect/></a>')


class NamedViewTest(ElementTestCase):
    """Test the sodipodi namedview tag"""

    def test_guides(self):
        """Create a guide and see a list of them"""
        self.svg.namedview.add_guide(0, False, 0)
        self.svg.namedview.add_guide(0, False, "90")
        self.assertEqual(len(self.svg.namedview.get_guides()), 2)
        self.svg.namedview.add_guide((1, 1), (1, 0))
        guides = self.svg.namedview.get_guides()
        self.assertAlmostEqual(
            guides[2].raw_position.y, self.svg.viewport_height - 1, 2
        )
        self.assertAlmostEqual(guides[2].raw_position.x, 1, 2)
        self.assertAlmostEqual(guides[2].position.y, 1, 2)
        self.assertAlmostEqual(guides[2].position.x, 1, 2)
        # Test angle specifications

    def test_guide_angles(self):
        g = self.svg.namedview.add_guide((1, 1), 30)
        self.assertAlmostEqual(g.angle, 30, 4)
        g = self.svg.namedview.add_guide((1, 1), (2.5, -5 * math.sqrt(3) / 2))
        self.assertAlmostEqual(g.angle, 30, 4)

    def test_guides_coincident(self):
        """Test the detection of coincident guides"""

        def coincidence_test(data, result):
            guide1 = self.svg.namedview.add_guide(data[0], orient=data[1])
            guide2 = self.svg.namedview.add_guide(data[2], orient=data[3])
            self.assertEqual(Guide.guides_coincident(guide1, guide2), result)

        # both are good
        coincidence_test(((0, 0), (0, 1), (2, 0), (0, 3)), True)
        # antiparallel
        coincidence_test(((0, 0), (0, 1), (2, 0), (0, -3)), True)
        # point not on guide
        coincidence_test(((0, 0), (0, 1), (0, 1), (0, 3)), False)
        # different orientation
        coincidence_test(((0, 0), (0, 1), (2, 0), (1, 0)), False)
        # try the same at an angle
        coincidence_test(((1, 1), (2, -1), (3, 5), (-2, 1)), True)
        coincidence_test(((1, 1), (2, -1), (3, 6), (-2, 1)), False)
        # and vertical
        coincidence_test(((0, 1), (1, 0), (0, 2), (3, 0)), True)
        coincidence_test(((0, 1), (1, 0), (0, 2), (-3, 0)), True)
        coincidence_test(((1, 1), (1, 0), (0, 2), (-3, 0)), False)

    def test_pages(self):
        """Create some extra pages and see a list of them"""
        self.assertEqual(len(self.svg.namedview.get_pages()), 1)
        self.svg.namedview.new_page(
            x="220", y="0", width="147.5", height="210", label="TEST"
        )
        self.assertEqual(len(self.svg.namedview.get_pages()), 2)
        self.assertEqual(self.svg.namedview.get_pages()[0].attrib["x"], "0")
        self.assertEqual(
            self.svg.namedview.get_pages()[1].get("inkscape:label"), "TEST"
        )
        self.assertEqual(self.svg.namedview.get_pages()[1].attrib["width"], "147.5")
        self.assertAlmostEqual(
            float(self.svg.namedview.get_pages()[0].attrib["height"]),
            297 * convert_unit("1mm", "px"),
        )
        self.assertEqual(self.svg.namedview.get_pages()[1].attrib["x"], "220")
        self.assertEqual(self.svg.namedview.get_pages()[1].attrib["y"], "0")

    def test_get_page_bbox(self):
        self.assertEqual(
            self.svg.get_page_bbox(),
            BoundingBox(
                (0, 210 * convert_unit("1mm", "px")),
                (0, 297 * convert_unit("1mm", "px")),
            ),
        )
        self.svg.namedview.add(Page(width="210", height="297", x="0", y="0"))
        self.svg.namedview.new_page(x="220", y="0", width="147.5", height="210")
        self.assertEqual(self.svg.get_page_bbox(), BoundingBox((0, 210), (0, 297)))
        self.assertEqual(self.svg.get_page_bbox(0), BoundingBox((0, 210), (0, 297)))
        self.assertEqual(self.svg.get_page_bbox(1), BoundingBox((220, 367.5), (0, 210)))

    def test_center(self):
        """Test that the center in mm based documents is correctly computed"""
        mmbased = svg(f'width="210mm" viewBox="0 0 210 297"')
        mmbased.namedview.set(
            "inkscape:cx", 396.57881
        )  # Values of a freshly opened mm document
        mmbased.namedview.set("inkscape:cy", 561.81998)

        self.assertAlmostTuple(
            mmbased.namedview.center,
            [mmbased.viewbox_width / 2, mmbased.viewport_height / 2],
            precision=0,
        )


class TextTest(ElementTestCase):
    """Test all text functions"""

    def test_append_superscript(self):
        """Test adding superscript"""
        tap = TextPath()
        tap.append(Tspan.superscript("th"))
        self.assertEqual(len(tap), 1)

    def test_path(self):
        """Test getting paths"""
        self.assertFalse(TextPath().get_path())
        self.assertFalse(TextElement().get_path())
        self.assertFalse(FlowRegion().get_path())
        self.assertFalse(FlowRoot().get_path())
        self.assertFalse(FlowPara().get_path())
        self.assertFalse(FlowSpan().get_path())
        self.assertFalse(Tspan().get_path())


class UseTest(ElementTestCase):
    """Test extra functionality on a use element"""

    tag = "use"

    def test_path(self):
        """Use path follows ref (including refs transform)"""
        self.assertEqual(str(self.elem.path), "M 0 0 L 10 10 Z")
        self.elem.href.transform = Transform("translate(100, 100)")
        self.assertEqual(str(self.elem.path), "M 100 100 L 110 110 Z")

    def test_empty_ref(self):
        """An empty ref or None ref doesn't cause an error"""
        use = Use()
        use.set("xlink:href", "something")
        self.assertRaises(FragmentError, getattr, use, "href")
        elem = self.svg.add(Use())
        self.assertEqual(elem.href, None)
        elem.set("xlink:href", "")
        self.assertEqual(elem.href, None)
        elem.set("xlink:href", "#badref")
        self.assertEqual(elem.href, None)
        elem.set("xlink:href", self.elem.get("xlink:href"))
        self.assertEqual(elem.href.get("id"), "path1")

    def test_href_compat_xlink_getElementsByHref(self):
        """Test attribute `href` is used according to SVG2 spec
        while considering `xlink:href` for SVG1.1 compatibility"""
        # test getElementsByHref `href`
        elem = self.svg.add(Use())
        elem.set("href", "#path1")
        href = self.svg.getElementsByHref("path1")[-1]
        self.assertEqual(href.TAG, elem.TAG)
        self.assertEqual(href.get_id(), elem.get_id())
        # test getElementsByHref `xlink:href`
        elem = self.svg.add(Use())
        elem.set("xlink:href", "#path2")
        href = self.svg.getElementsByHref("path2")[-1]
        self.assertEqual(href.TAG, elem.TAG)
        self.assertEqual(href.get_id(), elem.get_id())

    def test_href_compat_xlink_create_read_update(self):
        """Test attribute `href` is read and updated according to SVG2 spec
        while creating defaults to `xlink:href` for SVG1.1 compatibility"""
        # test setter (create) `xlink:href`
        elem = self.svg.add(Use())
        elem.href = self.elem.href
        self.assertEqual(elem.get("xlink:href"), "#path1")
        self.assertEqual(elem.href.get("id"), "path1")
        # test setter (update existing) `href` [SVG2]
        elem = self.svg.add(Use())
        elem.set("href", "#path2")
        elem.href = self.elem.href
        self.assertEqual(elem.get("href"), "#path1")
        self.assertEqual(elem.href.get("id"), "path1")
        # test getter (read existing) `href` [SVG2]
        elem = self.svg.add(Use())
        elem.set("href", "#path1")
        self.assertEqual(elem.get("href"), "#path1")
        self.assertEqual(elem.href.get("id"), "path1")
        # test getter (read existing) `xlink:href`
        elem = self.svg.add(Use())
        elem.set("xlink:href", "#path1")
        self.assertEqual(elem.get("xlink:href"), "#path1")
        self.assertEqual(elem.href.get("id"), "path1")

    def test_unlink(self):
        """Test use tag unlinking"""
        elem = self.elem.unlink()
        self.assertEqual(str(elem.path), "M 0 0 L 10 10 Z")
        self.assertEqual(elem.tag_name, "path")
        self.assertEqual(elem.getparent().get("id"), "C")

    def test_unlink_xy(self):
        """Check that unlink works if both transform and x, y are set on a clone"""
        elem = self.svg.add(Use())
        elem.set("xlink:href", "path1")
        elem.set("x", "100")
        elem.set("y", "20")
        elem.set("transform", "rotate(20,-100,100)")
        elem2 = elem.unlink()
        self.assertEqual(str(elem2.path), "M 0 0 L 10 10 Z")
        self.assertEqual(elem2.tag_name, "path")
        self.assertAlmostTuple(
            tuple(elem2.transform.to_hexad()),
            tuple(inkex.Transform("rotate(20,-206.71282,373.56409)").to_hexad()),
            3,
        )


class StopTests(ElementTestCase):
    """Color stop tests"""

    black = Color("#000000")
    grey50 = Color("#080808")
    white = Color("#111111")

    def test_interpolate(self):
        """Interpolate colours"""
        stl1 = Style({"stop-color": self.black, "stop-opacity": 0.0})
        stop1 = Stop(offset="0.0", style=str(stl1))
        stl2 = Style({"stop-color": self.white, "stop-opacity": 1.0})
        stop2 = Stop(offset="1.0", style=str(stl2))
        stop3 = stop1.interpolate(stop2, 0.5)
        assert stop3.style["stop-color"] == str(self.grey50)
        assert float(stop3.style["stop-opacity"]) == pytest.approx(0.5, 1e-3)


class GradientTests(ElementTestCase):
    """Gradient testing"""

    black = Color("#000000")
    grey50 = Color("#080808")
    white = Color("#111111")

    whiteop1 = Style({"stop-color": white, "stop-opacity": 1.0})
    blackop1 = Style({"stop-color": black, "stop-opacity": 1.0})
    whiteop0 = Style({"stop-color": white, "stop-opacity": 0.0})
    blackop0 = Style({"stop-color": black, "stop-opacity": 0.0})

    translate11 = Transform("translate(1.0, 1.0)")
    translate22 = Transform("translate(2.0, 2.0)")

    def test_parse(self):
        """Gradients parsed from XML"""
        values = [
            (
                LinearGradient,
                {"x1": "0px", "y1": "1px", "x2": "2px", "y2": "3px"},
                {"x1": 0.0, "y1": 1.0, "x2": 2.0, "y2": 3.0},
            ),
            (
                RadialGradient,
                {"cx": "0px", "cy": "1px", "fx": "2px", "fy": "3px", "r": "4px"},
                {"cx": 0.0, "cy": 1.0, "fx": 2.0, "fy": 3.0},
            ),
        ]
        for classname, attributes, expected in values:
            grad = classname(attrib=attributes)
            grad.apply_transform()  # identity transform
            for key, value in expected.items():
                assert float(grad.get(key)) == pytest.approx(value, 1e-3)
            grad = classname(attrib=attributes)

            grad = grad.interpolate(grad, 0.0)
            for key, value in expected.items():
                assert float(parse_unit(grad.get(key))[0]) == pytest.approx(value, 1e-3)

    def test_apply_transform(self):
        """Transform gradients"""
        values = [
            (
                LinearGradient,
                {"x1": 0.0, "y1": 0.0, "x2": 1.0, "y2": 1.0},
                {"x1": 1.0, "y1": 1.0, "x2": 2.0, "y2": 2.0},
            ),
            (
                RadialGradient,
                {"cx": 0.0, "cy": 0.0, "fx": 1.0, "fy": 1.0, "r": 1.0},
                {"cx": 1.0, "cy": 1.0, "fx": 2.0, "fy": 2.0, "r": 1.0},
            ),
        ]
        for classname, orientation, expected in values:
            grad = classname().update(**orientation)
            grad.gradientTransform = self.translate11
            grad.apply_transform()
            val = grad.get("gradientTransform")
            assert val is None
            for key, value in expected.items():
                assert float(grad.get(key)) == pytest.approx(value, 1e-3)

    def test_stops(self):
        """Gradients have stops"""
        for classname in [LinearGradient, RadialGradient]:
            grad = classname()
            stops = [
                Stop().update(offset=0.0, style=self.whiteop0),
                Stop().update(offset=1.0, style=self.blackop1),
            ]
            grad.add(*stops)
            assert [s1.tostring() == s2.tostring() for s1, s2 in zip(grad.stops, stops)]

    def test_stop_styles(self):
        """Gradients have styles"""
        for classname in [LinearGradient, RadialGradient]:
            grad = classname()
            stops = [
                Stop().update(offset=0.0, style=self.whiteop0),
                Stop().update(offset=1.0, style=self.blackop1),
            ]
            grad.add(*stops)
            assert [str(s1) == str(s2.style) for s1, s2 in zip(grad.stop_styles, stops)]

    def test_get_stop_offsets(self):
        """Gradients stop offsets"""
        for classname in [LinearGradient, RadialGradient]:
            grad = classname()
            stops = [
                Stop().update(offset=0.0, style=self.whiteop0),
                Stop().update(offset=1.0, style=self.blackop1),
            ]
            grad.add(*stops)
            for stop1, stop2 in zip(grad.stop_offsets, stops):
                self.assertEqual(float(stop1), pytest.approx(float(stop2.offset), 1e-3))

    def test_interpolate(self):
        """Gradients can be interpolated"""
        values = [
            (
                LinearGradient,
                {"x1": 0, "y1": 0, "x2": 1, "y2": 1},
                {"x1": 2, "y1": 2, "x2": 1, "y2": 1},
                {"x1": 1.0, "y1": 1.0, "x2": 1.0, "y2": 1.0},
            ),
            (
                RadialGradient,
                {"cx": 0, "cy": 0, "fx": 1, "fy": 1, "r": 0},
                {"cx": 2, "cy": 2, "fx": 1, "fy": 1, "r": 1},
                {"cx": 1.0, "cy": 1.0, "fx": 1.0, "fy": 1.0, "r": 0.5},
            ),
        ]
        for classname, orientation1, orientation2, expected in values:
            # gradient 1
            grad1 = classname()
            stops1 = [
                Stop().update(offset=0.0, style=self.whiteop0),
                Stop().update(offset=1.0, style=self.blackop1),
            ]
            grad1.add(*stops1)
            grad1.update(gradientTransform=self.translate11)
            grad1.update(**orientation1)

            # gradient 2
            grad2 = classname()
            stops2 = [
                Stop().update(offset=0.0, style=self.blackop1),
                Stop().update(offset=1.0, style=self.whiteop0),
            ]
            grad2.add(*stops2)
            grad2.update(gradientTransform=self.translate22)
            grad2.update(**orientation2)
            grad = grad1.interpolate(grad2, 0.5)
            comp = Style({"stop-color": self.grey50, "stop-opacity": 0.5})
            self.assertEqual(str(grad.stops[0].style), str(Style(comp)))
            self.assertEqual(str(grad.stops[1].style), str(Style(comp)))
            self.assertEqual(str(grad.gradientTransform), "translate(1.5, 1.5)")
            for key, value in expected.items():
                self.assertEqual(
                    float(parse_unit(grad.get(key))[0]), pytest.approx(value, 1e-3)
                )


class SymbolTest(ElementTestCase):
    """Test Symbol elements"""

    source_file = "symbol.svg"
    tag = "symbol"

    def test_unlink_symbol(self):
        """Test unlink symbols"""
        use = self.svg.getElementById("plane01")
        self.assertEqual(use.tag_name, "use")
        self.assertEqual(use.href.tag_name, "symbol")
        # Unlinking should replace symbol with group
        elem = use.unlink()
        self.assertEqual(elem.tag_name, "g")
        self.assertEqual(str(elem.transform), "translate(18, 16)")
        self.assertEqual(elem[0].tag_name, "title")
        self.assertEqual(elem[1].tag_name, "rect")


class DefsTest(ElementTestCase):
    """Test the definitions tag"""

    source_file = "shapes.svg"
    tag = "defs"

    def test_defs(self):
        """Make sure defs can be seen in the nodes of an svg"""
        self.assertTrue(isinstance(self.svg.defs, Defs))
        defs = self.svg.getElementById("defs33")
        self.assertTrue(isinstance(defs, Defs))


class StyleTest(ElementTestCase):
    """Test a style tag"""

    source_file = "css.svg"
    tag = "style"

    def test_style(self):
        """Make sure style tags can be loaded and saved"""
        css = self.svg.stylesheet
        self.assertTrue(css)
