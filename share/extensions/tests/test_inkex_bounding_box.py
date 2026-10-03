# coding=utf-8
"""Test inkex `.bounding_box()` method functionality"""

from copy import deepcopy
import os
import subprocess

import pytest
from inkex import (
    BoundingBox,
    SvgDocumentElement,
    Circle,
    Rectangle,
    Group,
    PathElement,
    Transform,
    Path,
    Style,
)
from inkex.tester import TestCase
from tempfile import TemporaryDirectory
from inkex.command import is_inkscape_available
from inkex.tester.decorators import requires_inkscape
from inkex.tester.svg import svg_file
from inkex.paths import Line, Move

try:
    from typing import Optional, Tuple
except ImportError:
    pass

DISABLE_STROKE_TESTS = True
DISABLE_STROKE_CAP_TESTS = True
DISABLE_INKSCAPE_QUERY_CHECK = not is_inkscape_available()

skip_stroke_tests = pytest.mark.skipif(  # pylint: disable=invalid-name
    DISABLE_STROKE_TESTS, reason="Bounding box tests with stroke are disabled"
)

skip_stroke_cap_tests = pytest.mark.skipif(  # pylint: disable=invalid-name
    DISABLE_STROKE_TESTS or DISABLE_STROKE_CAP_TESTS,
    reason="Bounding box tests with stroke-cap are disabled",
)


class BoundingBoxTest(TestCase):
    """Test BoundingBox functionality"""

    atol = 3e-3
    source_file = "bounding_box.svg"

    def setUp(self):
        super().setUp()
        self.svg = svg_file(self.data_file("svg", self.source_file))

    def assert_bounding_box_is_equal(
        self, obj, xscale, yscale, disable_inkscape_check=DISABLE_INKSCAPE_QUERY_CHECK
    ):
        """
        Assert: bounding box of object is exactly expected_box or is close to it

        :param (ShapeElement) obj: object to calculate bounding box
        :param (Optional[Tuple[float,float]]) xscale: expected values of (xmin, xmax)
        :param (Optional[Tuple[float,float]]) yscale: expected values of (ymin, ymax)

        """
        bounding_box = obj.bounding_box()

        if bounding_box is None:
            self.assertEqual((None, None), (xscale, yscale))
            return

        box_array = list(bounding_box)
        expected_array = xscale, yscale

        def cmp(a, b, msg=None):
            self.assertEqual(len(a), len(b), msg=msg)
            for x, y, label in zip(a, b, ("x", "y")):
                self.assertDeepAlmostEqual(
                    tuple(x), tuple(y), delta=self.atol, msg=msg + " (%s)" % label
                )

        if not disable_inkscape_check:
            clip_paths = []
            if obj.clip is not None:
                clip_paths.append(obj.clip)
            for child in obj:
                if child.clip is not None:
                    clip_paths.append(child.clip)
            inkscape_array = self.get_inkscape_bounding_box(obj, clip_paths)

            if None not in inkscape_array:
                cmp(expected_array, inkscape_array, "expected != inkscape calculation")

        cmp(box_array, expected_array, "inkex.bounding_box != expected")

    def get_inkscape_bounding_box(self, obj, clip_paths=[]):
        """

        :param (ShapeElement) obj:
        :param (list[ClipPath]) clip_paths: a list of clip objects
        :return: (xmin, xmax, ymin, ymax) parsed from `inkscape --query` output
        """
        svg = SvgDocumentElement()
        obj = deepcopy(obj)
        obj_id = "testing-query-id"
        obj.set("id", obj_id)
        root = deepcopy(obj.getroottree().getroot())
        svg.add(root)
        for clip in clip_paths:
            clip = deepcopy(clip)
            svg.defs.add(clip)
        with TemporaryDirectory() as tmp:
            temp_svg = os.path.join(tmp, "tmp.svg")

            with open(temp_svg, "wb") as out:
                out.write(svg.tostring())

            with open(os.devnull, "w") as devnull:
                output = subprocess.check_output(
                    [
                        "inkscape",
                        "--query-all",
                        temp_svg,
                    ],
                    stderr=devnull,
                )

            out_lines = output.decode("utf-8").split("\n")
            for line in out_lines:
                if line.startswith(obj_id):
                    x, y, w, h = list(map(float, line.split(",")[1:]))
                    return (x, x + w), (y, y + h)
        return None, None, None, None

    def test_bbox_empty_is_false(self):
        self.assertFalse(bool(BoundingBox()))

    def test_bbox_nonempty_is_true(self):
        self.assertTrue(bool(BoundingBox((0, 0), (0, 0))))

    def test_bbox_empty_is_identity_for_addition(self):
        bbox = BoundingBox((0, 1), (2, 3))
        self.assertEqual(BoundingBox() + bbox, bbox)

    def test_bbox_empty_is_zero_for_intersection(self):
        bbox = BoundingBox((0, 1), (2, 3))
        self.assertEqual(BoundingBox() & bbox, BoundingBox())

    def test_bbox_nonintersection_is_empty(self):
        bbox1 = BoundingBox((0, 1), (2, 3))
        bbox2 = BoundingBox((2, 3), (1, 2))
        self.assertEqual(bbox1 & bbox2, BoundingBox())

    def test_bbox_empty_equivalent_to_none(self):
        bbox = BoundingBox((0, 1), (2, 3))
        self.assertEqual(bbox + None, bbox + BoundingBox())
        self.assertEqual(bbox & None, bbox & BoundingBox())
        self.assertEqual(None + bbox, BoundingBox() + bbox)
        self.assertEqual(None & bbox, BoundingBox() & bbox)

    def test_circle_without_attributes(self):
        circle = Circle()
        self.assert_bounding_box_is_equal(circle, (0, 0), (0, 0))

    def test_circle_with_radius(self):
        r = 10
        circle = Circle(r=str(r))
        self.assert_bounding_box_is_equal(circle, (-r, r), (-r, r))

    def test_circle_with_cx(self):
        cx = 10
        circle = Circle(cx=str(cx))
        self.assert_bounding_box_is_equal(circle, (cx, cx), (0, 0))

    def test_circle_with_cy(self):
        cy = 10
        circle = Circle(cy=str(cy))
        self.assert_bounding_box_is_equal(circle, (0, 0), (cy, cy))

    def test_circle_without_center(self):
        r = 10
        circle = Circle(r=str(r))
        self.assert_bounding_box_is_equal(circle, (-r, r), (-r, r))

    def test_regular_circle(self):
        r = 5
        cx = 10
        cy = 20

        circle = Circle(r=str(r), cx=str(cx), cy=str(cy))

        self.assert_bounding_box_is_equal(circle, (cx - r, cx + r), (cy - r, cy + r))

    @skip_stroke_tests
    def test_circle_with_stroke(self):
        r = 5
        cx = 10
        cy = 20

        stroke_half_width = 1.0

        circle = Circle(r=str(r), cx=str(cx), cy=str(cy))

        circle.style = Style("stroke-width:{};stroke:red".format(stroke_half_width * 2))

        self.assert_bounding_box_is_equal(
            circle,
            (cx - (r + stroke_half_width), cx + (r + stroke_half_width)),
            (cy - (r + stroke_half_width), cy + (r + stroke_half_width)),
        )

    @skip_stroke_tests
    def test_circle_with_stroke_scaled(self):
        r = 5
        cx = 10
        cy = 20

        scale_x = 2
        scale_y = 3

        stroke_half_width = 1.0

        circle = Circle(r=str(r), cx=str(cx), cy=str(cy))

        circle.style = Style("stroke-width:{};stroke:red".format(stroke_half_width * 2))

        circle.transform = Transform(scale=(scale_x, scale_y))

        self.assert_bounding_box_is_equal(
            circle,
            (
                scale_x * (cx - (r + stroke_half_width)),
                scale_x * (cx + (r + stroke_half_width)),
            ),
            (
                scale_y * (cy - (r + stroke_half_width)),
                scale_y * (cy + (r + stroke_half_width)),
            ),
        )

    def test_rectangle_without_attributes(self):
        rect = Rectangle()

        self.assert_bounding_box_is_equal(rect, (0, 0), (0, 0))

    def test_rectangle_without_dimensions(self):
        x, y = 10, 15
        w, h = 0, 0

        rect = Rectangle(x=str(x), y=str(y))

        self.assert_bounding_box_is_equal(rect, (x, x + w), (y, y + h))

    def test_rectangle_without_coordinates(self):
        x, y = 0, 0
        w, h = 7, 20

        rect = Rectangle(width=str(w), height=str(h))

        self.assert_bounding_box_is_equal(rect, (x, x + w), (y, y + h))

    def test_regular_rectangle(self):
        x, y = 10, 20
        w, h = 7, 20

        rect = Rectangle(width=str(w), height=str(h), x=str(x), y=str(y))

        self.assert_bounding_box_is_equal(rect, (x, x + w), (y, y + h))

    def test_regular_rectangle_scaled(self):
        x, y = 10, 20
        w, h = 7, 20

        scale_x = 2
        scale_y = 3

        rect = Rectangle(width=str(w), height=str(h), x=str(x), y=str(y))

        rect.transform = Transform(scale=(scale_x, scale_y))

        self.assert_bounding_box_is_equal(
            rect, (scale_x * x, scale_x * (x + w)), (scale_y * y, scale_y * (y + h))
        )

    @skip_stroke_tests
    def test_regular_rectangle_with_stroke(self):
        x, y = 10, 20
        w, h = 7, 20
        stroke_half_width = 1

        rect = Rectangle(width=str(w), height=str(h), x=str(x), y=str(y))

        rect.style = Style("stroke-width:{};stroke:red".format(stroke_half_width * 2))

        self.assert_bounding_box_is_equal(
            rect,
            (x - stroke_half_width, x + w + stroke_half_width),
            (y - stroke_half_width, y + h + stroke_half_width),
        )

    @skip_stroke_tests
    def test_regular_rectangle_with_stroke_scaled(self):
        x, y = 10, 20
        w, h = 7, 20
        stroke_half_width = 1

        scale_x = 2
        scale_y = 3

        rect = Rectangle(width=str(w), height=str(h), x=str(x), y=str(y))

        rect.style = Style("stroke-width:{};stroke:red".format(stroke_half_width * 2))
        rect.transform = Transform(scale=(scale_x, scale_y))

        self.assert_bounding_box_is_equal(
            rect,
            (scale_x * (x - stroke_half_width), scale_x * (x + w + stroke_half_width)),
            (scale_y * (y - stroke_half_width), scale_y * (y + h + stroke_half_width)),
        )

    def test_empty_path(self):
        path = PathElement()

        self.assert_bounding_box_is_equal(path, None, None)

    def test_invisible_path(self):
        path = PathElement()

        path.set_path("M 0 0 L 10 10 L 10 0")
        path.style["display"] = "none"

        self.assert_bounding_box_is_equal(path, (0, 10), (0, 10))

    def test_path_with_move_commands_only(self):
        path = PathElement()

        path.set_path("M 0 0 m 100 100 M 200 200")
        self.assert_bounding_box_is_equal(path, (0, 200), (0, 200))

    def test_path_straight_line(self):
        path = PathElement()

        path.set_path("M 0 0 L 10 10")
        self.assert_bounding_box_is_equal(path, (0, 10), (0, 10))

    def test_path_two_straight_lines_abosolute(self):
        path = PathElement()

        path.set_path("M 0 0 L 10 10 M -1 1 L 10 10")
        self.assert_bounding_box_is_equal(path, (-1, 10), (0, 10))

    def test_path_two_straight_lines_relative(self):
        path = PathElement()

        path.set_path("M 0 0 l 10 10 m -11 -9 l 12 12")
        self.assert_bounding_box_is_equal(path, (-1, 11), (0, 13))

    def test_path_straight_line_scaled(self):
        path = PathElement()

        scale_x = 2
        scale_y = 3

        path.set_path("M 10 10 L 20 20")

        path.transform = Transform(scale=(scale_x, scale_y))
        self.assert_bounding_box_is_equal(
            path, (scale_x * 10, 20 * scale_x), (scale_y * 10, 20 * scale_y)
        )

    @skip_stroke_cap_tests
    def test_path_horizontal_line_stroke_butt_cap(self):
        path = PathElement()

        path.set_path("M 0 0 L 1 0")

        stroke_half_width = 1.0
        path.style = Style("stroke-width:{};stroke:red".format(stroke_half_width * 2))
        path.set("stroke-linecap", "butt")

        self.assert_bounding_box_is_equal(
            path, (0, 1), (-stroke_half_width, stroke_half_width)
        )

    @skip_stroke_cap_tests
    def test_path_horizontal_line_stroke_round_cap(self):
        path = PathElement()

        path.set_path("M 0 0 L 1 0")

        stroke_half_width = 1.0
        path.style = Style("stroke-width:{};stroke:red".format(stroke_half_width * 2))
        path.set("stroke-linecap", "round")

        self.assert_bounding_box_is_equal(
            path,
            (-stroke_half_width, 1 + stroke_half_width),
            (-stroke_half_width, stroke_half_width),
        )

    @skip_stroke_cap_tests
    def test_path_horizontal_line_stroke_square_cap(self):
        path = PathElement()

        path.set_path("M 0 0 L 1 0")

        stroke_half_width = 1.0
        path.style = Style("stroke-width:{};stroke:red".format(stroke_half_width * 2))
        path.set("stroke-linecap", "square")

        self.assert_bounding_box_is_equal(
            path,
            (-stroke_half_width, 1 + stroke_half_width),
            (-stroke_half_width, stroke_half_width),
        )

    def test_empty_group(self):
        group = Group()
        self.assert_bounding_box_is_equal(group, None, None)

    def test_group_with_visible_and_invisible_path(self):
        group = Group()

        visible_path = PathElement()
        visible_path.set_path("M 0 0 L 10 10 L 10 0")
        group.add(visible_path)

        invisible_path = PathElement()
        invisible_path.set_path("M 0 0 L -10 -10 L -10 0")
        invisible_path.style["display"] = "none"
        group.add(invisible_path)

        self.assert_bounding_box_is_equal(group, (0, 10), (0, 10))

    def test_empty_group_with_translation(self):
        group = Group()
        group.transform = Transform(translate=(10, 15))
        self.assert_bounding_box_is_equal(group, None, None)

    def test_group_with_regular_rect(self):
        group = Group()
        x, y = 10, 20
        w, h = 7, 20

        rect = Rectangle(width=str(w), height=str(h), x=str(x), y=str(y))

        group.add(rect)

        self.assert_bounding_box_is_equal(group, (x, x + w), (y, y + h))

    def test_group_with_number_of_rects(self):
        group = Group()

        xmin, ymin = 1000, 1000
        xmax, ymax = -1000, -1000

        rects = []

        for x, y, w, h in [
            (10, 20, 5, 7),
            (30, 40, 5, 7),
        ]:
            rect = Rectangle(width=str(w), height=str(h), x=str(x), y=str(y))
            rects.append(rect)

            xmin = min(xmin, x)
            xmax = max(xmax, x + w)
            ymin = min(ymin, y)
            ymax = max(ymax, y + h)

            group.append(rect)

        self.assert_bounding_box_is_equal(group, (xmin, xmax), (ymin, ymax))

    def test_group_with_number_of_rects_scaled(self):
        group = Group()

        scale_x, scale_y = 5, 10

        xmin, ymin = 1000, 1000
        xmax, ymax = -1000, -1000
        rects = []

        for x, y, w, h in [
            (10, 20, 5, 7),
            (30, 40, 5, 7),
        ]:
            rect = Rectangle(width=str(w), height=str(h), x=str(x), y=str(y))
            rects.append(rect)

            xmin = min(xmin, x)
            xmax = max(xmax, x + w)
            ymin = min(ymin, y)
            ymax = max(ymax, y + h)

            group.add(rect)

        group.transform = Transform(scale=(scale_x, scale_y))
        self.assert_bounding_box_is_equal(
            group, (scale_x * xmin, scale_x * xmax), (scale_y * ymin, scale_y * ymax)
        )

    def test_group_with_number_of_rects_translated(self):
        group = Group()

        dx, dy = 5, 10

        xmin, ymin = 1000, 1000
        xmax, ymax = -1000, -1000
        rects = []

        for x, y, w, h in [
            (10, 20, 5, 7),
            (30, 40, 5, 7),
        ]:
            rect = Rectangle(width=str(w), height=str(h), x=str(x), y=str(y))
            rects.append(rect)

            xmin = min(xmin, x)
            xmax = max(xmax, x + w)
            ymin = min(ymin, y)
            ymax = max(ymax, y + h)

            group.add(rect)

        group.transform = Transform(translate=(dx, dy))

        self.assert_bounding_box_is_equal(
            group, (dx + xmin, dx + xmax), (dy + ymin, dy + ymax)
        )

    def test_group_nested_transform(self):
        group = Group()

        x, y = 10, 20
        w, h = 7, 20

        scale = 2

        rect = Rectangle(width=str(w), height=str(h), x=str(x), y=str(y))

        rect.transform = Transform(rotate=45, scale=scale)

        group.add(rect)

        group.transform = Transform(
            rotate=-45
        )  # rotation is compensated, but scale is not

        a = rect.composed_transform()
        self.assert_bounding_box_is_equal(
            group, (scale * x, scale * (x + w)), (scale * y, scale * (y + h))
        )

    def test_clipped(self):
        clipped_rect = self.svg.getElementById("clipped_rect1")
        self.assert_bounding_box_is_equal(clipped_rect, (300, 400), (100, 200))

    def test_group_with_clipped_child(self):
        group_with_clipped_child = self.svg.getElementById("group_with_clipped_child")
        self.assert_bounding_box_is_equal(
            group_with_clipped_child, (100, 400), (300, 400)
        )

    def test_group_with_invisible_clipped_child(self):
        group_with_invisible_clipped_child = self.svg.getElementById(
            "group_with_invisible_clipped_child"
        )
        self.assert_bounding_box_is_equal(
            group_with_invisible_clipped_child, (100, 200), (500, 600)
        )

    def test_clipped_group(self):
        clipped_group = self.svg.getElementById("clipped_group")
        self.assert_bounding_box_is_equal(clipped_group, (100, 200), (700, 800))

    def test_unrooted_group_with_invisible_parent(self):
        outer_group = Group()
        inner_group = Group()
        element = Rectangle(width=str(10), height=str(10), x=str(0), y=str(0))
        inner_group.add(element)
        outer_group.add(inner_group)
        outer_group.style["display"] = "none"
        self.assert_bounding_box_is_equal(inner_group, None, None)

    def test_path_Arc_long_sweep_off(self):
        path = Path("M 10 20 A 10 20 0 1 0 20 15")
        path_element = PathElement()
        path_element.path = path
        self.assert_bounding_box_is_equal(
            path_element, (7.078, 20 + 7.078), (15.0, 15.0 + 39.127)
        )

    def test_path_Arc_short_sweep_off(self):
        path = Path("M 10 20 A 10 20 0 0 0 20 15")
        path_element = PathElement()
        path_element.path = path
        self.assert_bounding_box_is_equal(path_element, (10, 20), (15, 15.0 + 5.873))

    def test_path_Arc_short_sweep_on(self):
        path = Path("M 10 20 A 10 20 0 0 1 20 15")
        path_element = PathElement()
        path_element.path = path
        self.assert_bounding_box_is_equal(
            path_element, (10, 20), (14.127, 14.127 + 5.873)
        )

    def test_path_Arc_long_sweep_on(self):
        path = Path("M 10 20 A 10 20 0 1 1 20 15")
        path_element = PathElement()
        path_element.path = path
        self.assert_bounding_box_is_equal(
            path_element, (2.922, 2.922 + 20), (-19.127, -19.127 + 39.127)
        )

    def test_path_Arc_long_sweep_on_axis_x_25(self):
        path = Path("M 10 20 A 10 20 25 1 1 20 15")
        path_element = PathElement()
        path_element.path = path
        self.assert_bounding_box_is_equal(
            path_element, (4.723, 4.723 + 24.786), (-17.149, -17.149 + 37.149)
        )

    def test_path_Move(self):
        path = Path("M 10 20")
        pe = PathElement()
        pe.path = path
        self.assert_bounding_box_is_equal(pe, (10, 10), (20, 20))

    def test_path_move(self):
        path = Path("M 15 30 m 10 20")
        pe = PathElement()
        pe.path = path
        self.assert_bounding_box_is_equal(pe, (15, 25), (30, 50))

    def test_path_Line(self):
        path = Path("M 15 30 L 10 20")
        pe = PathElement()
        pe.path = path
        self.assert_bounding_box_is_equal(pe, (10, 15), (20, 30))

    def test_path_line(self):
        path = Path("M 15 30 l 10 20")
        pe = PathElement()
        pe.path = path
        self.assert_bounding_box_is_equal(pe, (15, 25), (30, 50))

    def test_path_Zone(self):
        path = Path("M 15 30 Z")
        pe = PathElement()
        pe.path = path
        self.assert_bounding_box_is_equal(pe, (15, 15), (30, 30))

    def test_path_Horz(self):
        path = Path("M 15 30 H 20")
        pe = PathElement()
        pe.path = path
        self.assert_bounding_box_is_equal(pe, (15, 20), (30, 30))

    def test_path_horz(self):
        path = Path("M 15 30 h 20")
        pe = PathElement()
        pe.path = path
        self.assert_bounding_box_is_equal(pe, (15, 35), (30, 30))

    def test_path_Vert(self):
        path = Path("M 15 30 V 20")
        pe = PathElement()
        pe.path = path
        self.assert_bounding_box_is_equal(pe, (15, 15), (20, 30))

    def test_path_vert(self):
        path = Path("M 15 30 v 20")
        pe = PathElement()
        pe.path = path
        self.assert_bounding_box_is_equal(pe, (15, 15), (30, 50))

    def test_path_Curve(self):
        path = Path("M10 10 C 20 20, 40 20, 50 10")
        pe = PathElement()
        pe.path = path
        self.assert_bounding_box_is_equal(pe, (10, 50), (10, 17.5))

    def test_path_combined_relative(self):
        """Test concatenation of two paths where the second path has a lower-case
        first move command"""
        g = Group()
        g.append(PathElement.new(path="m -32,-16 h 16"))
        g.append(PathElement.new(path="m -32,0 h 16"))

        # Two relative paths can not be concatenated, but must be converted to absolute
        # (at least the first command) before concatenation.
        assert g.path[2] == Move(-32, 0)

        bbx = g.bounding_box()
        assert bbx.width == bbx.height == 16
        pbx = g.path.bounding_box()
        assert pbx.width == pbx.height == 16

    @requires_inkscape
    def test_path_combined_1(self):
        path = Path("M 0 0 C 11 14 33 3 85 98 H 84 V 91 L 13 78 C 26 83 65 24 94 77")
        # path = Path("M 0 0 C 11 14 33 3 85 98")
        pe = PathElement()
        pe.path = path
        ibb = self.get_inkscape_bounding_box(pe)

        self.assert_bounding_box_is_equal(pe, *ibb, disable_inkscape_check=True)

    def test_path_TepidQuadratic(self):
        path = Path("M 10 5 Q 15 30 25 15 T 50 40")
        pe = PathElement()
        pe.path = path
        ibb = (10, 50), (5, 40)

        self.assert_bounding_box_is_equal(pe, *ibb)

    def test_path_TepidQuadratic_2(self):
        path = Path("M 10 5 Q 15 30 25 15 T 50 40 T 15 20")
        pe = PathElement()
        pe.path = path
        ibb = (10, 10 + 43.462), (5, 56)
        self.assert_bounding_box_is_equal(pe, *ibb)

    @requires_inkscape
    def test_random_path_1(self):
        import random

        from inkex.paths import (
            Line,
            Vert,
            Horz,
            Curve,
            Move,
            Arc,
            Quadratic,
            TepidQuadratic,
            Smooth,
            ZoneClose,
        )

        klasses = (Line, Vert, Horz, Curve, Move, Quadratic)  # , ZoneClose, Arc

        def random_segment(klass):
            args = [random.randint(1, 100) for _ in range(klass.nargs)]
            if klass is Arc:
                args[2] = 0  # random.randint(0, 1)
                args[3] = 0  # random.randint(0, 1)
                args[4] = 0  # random.randint(0, 1)
            return klass(*args)

        random.seed(2128506)
        # random.seed(datetime.now())
        n_trials = 10
        n_elements = 15

        for i in range(n_trials):
            path = Path()
            path.append(Move(0, 0))

            for j in range(n_elements):
                k = random.choice(klasses)
                path.append(random_segment(k))
                if k is Curve:
                    while random.randint(0, 1) == 1:
                        path.append(random_segment(Smooth))
                if k is Quadratic:
                    while random.randint(0, 1) == 1:
                        path.append(random_segment(TepidQuadratic))

            pe = PathElement()
            pe.path = path
            ibb = self.get_inkscape_bounding_box(pe)

            self.assert_bounding_box_is_equal(pe, *ibb, disable_inkscape_check=True)
