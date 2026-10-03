# coding=utf-8
"""
Test Inkex path parsing functionality.
"""

import re

from inkex.paths import (
    Path,
    PathCommand,
    CubicSuperPath,
    line,
    move,
    curve,
    smooth,
    quadratic,
    tepidQuadratic,
    arc,
    vert,
    horz,
    zoneClose,
    Line,
    Move,
    Horz,
    Vert,
    Curve,
    Smooth,
    Quadratic,
    TepidQuadratic,
    Arc,
    ZoneClose,
)
from inkex.transforms import Transform, Vector2d
from inkex.tester import TestCase

# pylint: disable=too-many-public-methods


def novector(func):
    """Raise if Vector2d() is called inside the decorated function.
    Used to check the performance of path operations."""

    def inner(self):
        old_init = Vector2d.__init__

        def temp_init(self, *args, **kwargs):
            raise ValueError("Vector2D() should not be called for path ops")

        Vector2d.__init__ = temp_init
        try:
            func(self)
        finally:
            Vector2d.__init__ = old_init

    return inner


class SegmentTest(TestCase):
    """
    Test specific segment functionality.
    """

    def get_random_cmd(self, Cmd):
        import random

        return Cmd(*[random.randint(0, 10) for i in range(Cmd.nargs)])

    @novector
    def test_equals(self):
        """Segments should be equalitive"""
        self.assertEqual(Move(10, 10), Move(10, 10))
        self.assertEqual(Line(10, 10), Line(10, 10))
        self.assertEqual(line(10, 10), line(10, 10))
        self.assertNotEqual(line(10, 10), Line(10, 10))
        self.assertEqual(Horz(10), Line(10, 0))
        self.assertEqual(Vert(10), Line(0, 10))
        self.assertNotEqual(Vert(10), Horz(10))

    def test_to_curves(self):
        """Segments can become curves"""
        self.assertRaises(ValueError, Move(0, 0).to_curve, None)
        self.assertEqual(
            Line(10, 10).to_curve(Vector2d(10, 5)), (10, 5, 10, 10, 10, 10)
        )
        self.assertEqual(Horz(10).to_curve(Vector2d(10, 5)), (10, 5, 10, 5, 10, 5))
        self.assertEqual(Vert(10).to_curve(Vector2d(5, 10)), (5, 10, 5, 10, 5, 10))
        self.assertEqual(
            Curve(5, 5, 10, 10, 4, 4).to_curve(Vector2d(0, 0)), (5, 5, 10, 10, 4, 4)
        )

        self.assertEqual(
            Smooth(10, 10, 4, 4).to_curve(Vector2d(4, 4), Vector2d(10, 10)),
            (-2, -2, 10, 10, 4, 4),
        )

        self.assertAlmostTuple(
            Quadratic(10, 10, 4, 4).to_curve(Vector2d(0, 0)).args,
            (6.666666666666666, 6.666666666666666, 8, 8, 4, 4),
        )

        self.assertAlmostTuple(
            TepidQuadratic(4, 4).to_curve(Vector2d(14, 19), Vector2d(11, 12)).args,
            #            (20.666666666666664, 30, 17.333333333333332, 25, 4, 4),
            (
                15.999999999999998,
                23.666666666666664,
                12.666666666666666,
                18.666666666666664,
                4,
                4,
            ),
        )

        curves = list(Arc(50, 50, 0, 0, 1, 85, 85).to_curves(Vector2d(0, 0)))
        self.assertEqual(len(curves), 3)
        self.assertAlmostTuple(
            curves[0].args,
            (
                19.77590700610636,
                -5.4865851247611115,
                38.18634924829132,
                -10.4196482558544,
                55.44095225512604,
                -5.796291314453416,
            ),
        )
        self.assertAlmostTuple(
            curves[1].args,
            (
                72.69555526196076,
                -1.172934373052433,
                86.17293437305243,
                12.30444473803924,
                90.79629131445341,
                29.559047744873958,
            ),
        )
        self.assertAlmostTuple(
            curves[2].args,
            (
                95.41964825585441,
                46.81365075170867,
                90.4865851247611,
                65.22409299389365,
                77.85533905932738,
                77.85533905932738,
            ),
        )

        def apply_to_curve(obj):
            obj.to_curve(Vector2d())

        def apply_to_curves(obj):
            obj.to_curve(Vector2d())

        self.assertRaises(ValueError, apply_to_curve, ZoneClose())
        self.assertRaises(ValueError, apply_to_curves, zoneClose())

        self.assertRaises(ValueError, apply_to_curve, Move(0, 0))
        self.assertRaises(ValueError, apply_to_curves, move(0, 0))

    def test_transformation(self):
        t = Transform(matrix=((1, 2, 3), (4, 5, 6)))

        first = Vector2d()
        prev = Vector2d(31, 97)
        prev_prev = Vector2d(5, 7)

        for Cmd in (Line, Move, Curve, Smooth, Quadratic, TepidQuadratic, Arc):
            random_seg = self.get_random_cmd(Cmd)
            self.assertTrue(
                random_seg.transform(t) is not random_seg
            )  # transform returns copy
            self.assertEqual(
                random_seg.transform(t).name, Cmd.name
            )  # transform does not change Command type

            T = Transform()
            T.add_translate(10, 20)
            A = [
                T.apply_to_point(p)
                for p in random_seg.control_points(first, prev, prev_prev)
            ]
            first2, prev2, prev_prev2 = (
                T.apply_to_point(p) for p in (first, prev, prev_prev)
            )
            B = list(
                random_seg.translate(Vector2d(10, 20)).control_points(
                    first2, prev2, prev_prev2
                )
            )
            self.assertAlmostTuple(A, B)

            T = Transform()
            T.add_scale(10, 20)
            A = [
                T.apply_to_point(p)
                for p in random_seg.control_points(first, prev, prev_prev)
            ]
            first2, prev2, prev_prev2 = (
                T.apply_to_point(p) for p in (first, prev, prev_prev)
            )
            B = list(
                random_seg.scale((10, 20)).control_points(first2, prev2, prev_prev2)
            )
            self.assertAlmostTuple(A, B)

            T = Transform()
            T.add_rotate(35, 15, 28)
            A = [
                T.apply_to_point(p)
                for p in random_seg.control_points(first, prev, prev_prev)
            ]
            first2, prev2, prev_prev2 = (
                T.apply_to_point(p) for p in (first, prev, prev_prev)
            )
            B = list(
                random_seg.rotate(35, Vector2d(15, 28)).control_points(
                    first2, prev2, prev_prev2
                )
            )
            self.assertAlmostTuple(A, B)

    @novector
    def test_absolute_relative(self):
        absolutes = (
            Line,
            Move,
            Curve,
            Smooth,
            Quadratic,
            TepidQuadratic,
            Arc,
            Vert,
            Horz,
            ZoneClose,
        )
        relatives = (
            line,
            move,
            curve,
            smooth,
            quadratic,
            tepidQuadratic,
            arc,
            vert,
            horz,
            zoneClose,
        )

        zero = 0
        for R, A in zip(relatives, absolutes):
            rel = self.get_random_cmd(R)
            ab = self.get_random_cmd(A)

            self.assertTrue(rel.is_relative)
            self.assertTrue(ab.is_absolute)

            self.assertFalse(rel.is_absolute)
            self.assertFalse(ab.is_relative)

            self.assertEqual(type(rel.to_absolute(zero)), A)
            self.assertEqual(type(ab.to_relative(zero)), R)
            self.assertTrue(rel.to_relative(zero) is not rel)
            self.assertTrue(ab.to_absolute(zero) is not ab)

    def test_to_line(self):
        self.assertEqual(Vert(3).to_line(Vector2d(5, 11)), Line(5, 3))
        self.assertEqual(Horz(3).to_line(Vector2d(5, 11)), Line(3, 11))

        self.assertEqual(vert(3).to_line(Vector2d(5, 11)), Line(5, 14))
        self.assertEqual(horz(3).to_line(Vector2d(5, 11)), Line(8, 11))

    def test_args(self):
        commands = (
            Line,
            Move,
            Curve,
            Smooth,
            Quadratic,
            TepidQuadratic,
            Arc,
            Vert,
            Horz,
            ZoneClose,
            line,
            move,
            curve,
            smooth,
            quadratic,
            tepidQuadratic,
            arc,
            vert,
            horz,
            zoneClose,
        )

        for Cmd in commands:
            cmd = self.get_random_cmd(Cmd)
            self.assertEqual(len(cmd.args), cmd.nargs)
            self.assertEqual(Cmd(*cmd.args), cmd)

    def test_letters(self):
        """For performance reasons, the letter are specified directly. Check that this
        is correct."""
        for letter, cl in PathCommand._letter_to_class.items():
            assert letter == cl.letter


class PathTest(TestCase):
    """Test path API and calculations"""

    def _assertPath(self, path, want_string):
        """Test a normalized path string against a good value"""
        return self.assertEqual(re.sub("\\s+", " ", str(path)), want_string)

    def test_new_empty(self):
        """Create a path from a path string"""
        self.assertEqual(str(Path()), "")

    def test_invalid(self):
        """Load an invalid path"""
        self._assertPath(Path("& 10 10 M 20 20"), "M 20 20")
        self.assertRaises(
            TypeError,
            Curve,
            [
                40,
            ],
        )

    @novector
    def test_copy(self):
        """Make a copy of a path"""
        self.assertEqual(str(Path("M 10 10").copy()), "M 10 10")

    @novector
    def test_repr(self):
        """Path representation"""
        self._assertPath(repr(Path("M 10 10 10 10")), "[Move(10, 10), Line(10, 10)]")

    @novector
    def test_list(self):
        """Path of previous commands"""
        path = Path(Path("M 10 10 20 20 30 30 Z")[1:-1])
        self._assertPath(path, "L 20 20 L 30 30")

    @novector
    def test_passthrough(self):
        """Create a path and test the re-rendering of the commands"""
        for path in (
            "M 50,50 L 10,10 m 10 10 l 2.1,2",
            "m 150 150 c 10 10 6 6 20 10 L 10 10",
        ):
            self._assertPath(Path(path), path.replace(",", " "))

    @novector
    def test_chained_conversion(self):
        """Paths always extrapolate chained commands"""
        for path, ret in (
            ("M 100 100 20 20", "M 100 100 L 20 20"),
            ("M 100 100 Z 20 20", "M 100 100 Z M 20 20"),
            ("M 100 100 L 20 20 40 40 30 10 Z", "M 100 100 L 20 20 L 40 40 L 30 10 Z"),
            ("m 50 50 l 20 20 40 40", "m 50 50 l 20 20 l 40 40"),
            ("m 50 50 20 20", "m 50 50 l 20 20"),
            ((("m", (50, 50)), ("l", (20, 20))), "m 50 50 l 20 20"),
        ):
            self._assertPath(Path(path), ret)

    @novector
    def test_create_from_points(self):
        """Paths can be made of simple list of tuples"""
        arg = ((10, 10), (4, 5), (16, -9), (20, 20))
        self.assertEqual(str(Path(arg)), "M 10 10 L 4 5 L 16 -9 L 20 20")

    def test_control_points(self):
        """Test how x,y points are extracted"""
        for path, ret in (
            ("M 100 100", ((100, 100),)),
            ("L 100 100", ((100, 100),)),
            ("H 133", ((133, 0),)),
            ("V 144", ((0, 144),)),
            (
                "Q 40 20 12 99 T 100 100",
                (
                    (40, 20),
                    (12, 99),
                    (-16, 178),
                    (100, 100),
                ),
            ),
            ("C 12 12 15 15 20 20", ((12, 12), (15, 15), (20, 20))),
            (
                "S 50 90 30 10",
                (
                    (0, 0),
                    (50, 90),
                    (30, 10),
                ),
            ),
            (
                "Q 40 20 12 99",
                (
                    (40, 20),
                    (12, 99),
                ),
            ),
            ("A 1,2,3,0,0,10,20", ((10, 20),)),
            ("Z", ((0, 0),)),
        ):
            points = list(Path(path).control_points)
            self.assertEqual(len(points), len(ret), msg=path)
            self.assertTrue(all(p.is_close(r) for p, r in zip(points, ret)), msg=path)

    def test_bounding_box_lines(self):
        """
        Test the bounding box calculations

        A diagonal line from 20,20 to 90,90 then to +10,+10  "\"

        """
        self.assertEqual(
            (20, 100), (20, 100), Path("M 20,20 L 90,90 l 10,10 Z").bounding_box()
        )
        self.assertEqual(
            (10, 90), (10, 90), Path("M 20,20 L 90,90 L 10,10 Z").bounding_box()
        )

    def test_bounding_box_curves(self):
        """
        Test the bounding box calculations of a curve
        """

        path = Path(
            "M 85,14 C 104.63953,33.639531 104.71989,65.441157"
            " 85,85 65.441157,104.71989 33.558843,104.71989 14,85"
            " -5.7198883,65.441157 -5.6395306,33.639531 14,14"
            " 33.639531,-5.6395306 65.360469,-5.6395306 85,14 Z"
        )
        bb_tuple = path.bounding_box()
        expected = (-0.760, -0.760 + 100.520), (-0.730, -0.730 + 100.520)
        precision = 3

        self.assertDeepAlmostEqual(tuple(bb_tuple.x), expected[0], places=precision)
        self.assertDeepAlmostEqual(tuple(bb_tuple.y), expected[1], places=precision)

    def test_bounding_box_arcs(self):
        """
        Test the bounding box calculations with arcs (currently is rough only)

        Bounding box around a circle with a radius of 50
        it should be from 0,0 -> 100, 100
        """
        path = Path(
            "M 85.355333,14.644651 "
            "A 50,50 0 0 1 85.355333,85.355341"
            " 50,50 0 0 1 14.644657,85.355341"
            " 50,50 0 0 1 14.644676,14.644651"
            " 50,50 0 0 1 85.355333,14.644651 Z"
        )

        bb_tuple = path.bounding_box()
        expected = (0, 100), (0, 100)
        precision = 4

        self.assertDeepAlmostEqual(tuple(bb_tuple.x), expected[0], places=precision)
        self.assertDeepAlmostEqual(tuple(bb_tuple.y), expected[1], places=precision)

        # self.assertEqual(('ERROR'), Path('M 10 10 S 100 100 300 0').bounding_box())
        # self.assertEqual(('ERRPR'), Path('M 10 10 Q 100 100 300 0').bounding_box())

    def test_adding_to_path(self):
        """Paths can be translated using addition"""
        ret = Path("M 20,20 L 90,90 l 10,10 Z").translate(50, 50)
        self._assertPath(ret, "M 70 70 L 140 140 l 10 10 Z")

    @novector
    def test_extending(self):
        """Paths can be extended using addition"""
        ret = Path("M 20 20") + Path("L 40 40 9 10")
        self.assertEqual(type(ret), Path)
        self._assertPath(ret, "M 20 20 L 40 40 L 9 10")

        ret = Path("M 20 20") + "C 40 40 9 10 10 10"
        self.assertEqual(type(ret), Path)
        self._assertPath(ret, "M 20 20 C 40 40 9 10 10 10")

        ret = Path("M 20 20")
        ret += "C 40 40 9 10 10 10"
        self.assertEqual(type(ret), Path)
        self._assertPath(ret, "M 20 20 C 40 40 9 10 10 10")

    def test_subtracting_from_path(self):
        """Paths can be translated using addition"""
        ret = Path("M 20,20 L 90,90 l 10,10 Z").translate(-10, -10)
        self._assertPath(ret, "M 10 10 L 80 80 l 10 10 Z")

    def test_scale(self):
        """Paths can be scaled using the times operator"""
        ret = Path("M 10,10 L 30,30 C 20 20 10 10 10 10 l 10 10").scale(2.5, 3)
        self._assertPath(ret, "M 25 30 L 75 90 C 50 60 25 30 25 30 l 25 30")

        ret = Path(
            "M 29.867708,101.68274 A 14.867708,14.867708 0 0 1 15,116.55045 14.867708,"
            "14.867708 0 0 1 0.13229179,101.68274 14.867708,14.867708 0 0 1 15,86.815031 "
            "14.867708,14.867708 0 0 1 29.867708,101.68274 Z"
        )
        ret = ret.scale(1.2, 0.8)
        self._assertPath(
            ret,
            "M 35.8412 81.3462 "
            "A 17.8412 11.8942 0 0 1 18 93.2404 "
            "A 17.8412 11.8942 0 0 1 0.15875 81.3462 "
            "A 17.8412 11.8942 0 0 1 18 69.452 "
            "A 17.8412 11.8942 0 0 1 35.8412 81.3462 Z",
        )

    def test_scale_relative_after_close(self):
        """Zone close moves current position correctly after transform"""
        # expected positions:
        # - before scale:
        #            M to (10,10), l by (+10,+10), Z back to (10,10), l by (+10,+10)
        #       <=>  M to (10,10), L to (20,20),   Z back to (10,10), L to (20,20)
        # - after scale:
        #            M to (20,20), L to (40,40),   Z back to (20,20), L to (40,40)
        #       <=>  M to (20,20), l by (+20,+20), Z back to (20,20), l by (+20,+20)
        ret = Path("M 10,10 l 10,10 Z l 10,10").scale(2, 2)
        self._assertPath(ret, "M 20 20 l 20 20 Z l 20 20")

    def test_scale_multiple_zones(self):
        """Zone close returns current position to start of zone (not start of path)"""
        ret = Path("M 100 100 Z M 200 200 Z h 0").scale(1, 1)
        self._assertPath(ret.to_absolute(), "M 100 100 Z M 200 200 Z L 200 200")

    def test_absolute(self):
        """Paths can be converted to absolute"""
        ret = Path("M 100 100 l 10 10 10 10 10 10")
        self._assertPath(ret.to_absolute(), "M 100 100 L 110 110 L 120 120 L 130 130")

        ret = Path("M 100 100 h 10 10 10 v 10 10 10")
        self._assertPath(
            ret.to_absolute(), "M 100 100 H 110 H 120 H 130 V 110 V 120 V 130"
        )

        ret = Path("M 150,150 a 76,55 0 1 1 283,128")
        self._assertPath(ret.to_absolute(), "M 150 150 A 76 55 0 1 1 433 278")

        ret = Path("m 5 5 h 5 v 5 h -5 z M 15 15 l 5 5 z m 10 10 h 5 v 5 h -5 z")
        self._assertPath(
            ret.to_absolute(),
            "M 5 5 H 10 V 10 H 5 Z M 15 15 L 20 20 Z M 25 25 H 30 V 30 H 25 Z",
        )

        ret = Path("m 1 2 h 2 v 1 z m 4 0 h 2 v 1 z m 0 2 h 2 v 1 z")
        self._assertPath(
            ret.to_absolute(), "M 1 2 H 3 V 3 Z M 5 2 H 7 V 3 Z M 5 4 H 7 V 5 Z"
        )

    @novector
    def test_relative(self):
        """Paths can be converted to relative"""
        ret = Path("M 100 100 L 110 120 140 140 300 300")
        self._assertPath(ret.to_relative(), "m 100 100 l 10 20 l 30 20 l 160 160")

        ret = Path("M 150,150 A 76,55 0 1 1 433,278")
        self._assertPath(ret.to_relative(), "m 150 150 a 76 55 0 1 1 283 128")

        ret = Path("M 1 2 H 3 V 3 Z M 5 2 H 7 V 3 Z M 5 4 H 7 V 5 Z")
        self._assertPath(
            ret.to_relative(), "m 1 2 h 2 v 1 z m 4 0 h 2 v 1 z m 0 2 h 2 v 1 z"
        )

    def test_rotate(self):
        """Paths can be rotated"""
        ret = Path("M 0.24999949,0.24999949 H 12.979167 V 12.979167 H 0.24999949 Z")
        ret = ret.rotate(35, (0, 0))
        self._assertPath(
            ret,
            "M 0.0613938 0.348181 L 10.4885 7.64933 L 3.18737 18.0765 L -7.23976 10.7753 Z",
        )

        ret = Path("M 0.24999949,0.24999949 H 12.979167 V 12.979167 H 0.24999949 Z")
        ret = ret.rotate(-35, (0, 0))
        self._assertPath(
            ret,
            "M 0.348181 0.0613938 L 10.7753 -7.23976 L 18.0765 3.18737 L 7.64933 10.4885 Z",
        )

        ret = Path("M 0.24999949,0.24999949 H 12.979167 V 12.979167 H 0.24999949 Z")
        ret = ret.rotate(90, (10, -10))
        self._assertPath(
            ret,
            "M -0.249999 -19.75 L -0.249999 -7.02083 L -12.9792 -7.02083 L -12.9792 -19.75 Z",
        )

        ret = Path("M 0.24999949,0.24999949 H 12.979167 V 12.979167 H 0.24999949 Z")
        ret = ret.rotate(90)
        self._assertPath(
            ret,
            "M 12.9792 0.249999 L 12.9792 12.9792 L 0.249999 12.9792 L 0.249999 0.249999 Z",
        )

    def test_to_arrays(self):
        """Return the full path as a bunch of arrays"""
        ret = Path("M 100 100 L 110 120 H 20 C 120 0 6 10 10 2 Z").to_arrays()
        self.assertEqual(len(ret), 5)
        self.assertEqual(ret[0][0], "M")
        self.assertEqual(ret[1][0], "L")
        self.assertEqual(ret[2][0], "L")
        self.assertEqual(ret[3][0], "C")

    def test_transform(self):
        """Transform by a whole matrix"""
        ret = Path("M 100 100 L 110 120 L 140 140 L 300 300")
        ret = ret.transform(Transform(translate=(10, 10)))
        self.assertEqual(str(ret), "M 110 110 L 120 130 L 150 150 L 310 310")
        ret = ret.transform(Transform(translate=(-10, -10)))
        self.assertEqual(str(ret), "M 100 100 L 110 120 L 140 140 L 300 300")
        ret = Path("M 5 5 H 10 V 15")
        ret = ret.transform(Transform(rotate=-10))
        self.assertEqual(
            "M 5.79228 4.0558 L 10.7163 3.18756 L 12.4528 13.0356", str(ret)
        )
        ret = Path("M 10 10 A 50,50 0 0 1 85.355333,85.355341 L 100 0")
        ret = ret.transform(Transform(scale=10))
        self.assertEqual(str(ret), "M 100 100 A 500 500 0 0 1 853.553 853.553 L 1000 0")
        self.assertRaises(ValueError, Horz([10]).transform, Transform())

    @novector
    def test_transforms_without_vector(self):
        """Check that we can transform without invoking Vector2d()"""
        path = Path("M 10 10 A 50,50 0 0 1 85.355333,85.355341 L 100 0")
        transform = Transform().add_scale(1, 1)
        path.transform(transform)

    def test_inline_transformations(self):
        path = Path()
        self.assertTrue(path is not path.translate(10, 20))
        self.assertTrue(path is not path.transform(Transform(scale=10)))
        self.assertTrue(path is not path.rotate(10))
        self.assertTrue(path is not path.scale(10, 20))

        self.assertTrue(path is path.translate(10, 20, inplace=True))
        self.assertTrue(path is path.transform(Transform(scale=10), inplace=True))
        self.assertTrue(path is path.rotate(10, inplace=True))
        self.assertTrue(path is path.scale(10, 20, inplace=True))

    def test_transformation_preserve_type(self):
        import re

        paths = [
            "M 10 10 A 100 100 0 1 0 100 100 C 10 15 20 20 5 5 Z",
            "m 10 10 a 100 100 0 1 0 100 100 c 10 15 20 20 5 5 z",
            "m 10 10 l 100 200 L 20 30 C 10 20 30 40 11 12",
            "M 10 10 Q 12 13 14 15 T 11 32 T 32 11",
            "m 10 10 q 12 13 14 15 t 11 32 t 32 11",
        ]
        t = Transform(matrix=((1, 2, 3), (4, 5, 6)))
        for path_str in paths:
            path = Path(path_str)
            new_path = path.transform(t)
            cmds = "".join([cmd.letter for cmd in new_path])
            expected = re.sub(r"\d|\s|,", "", path_str)

            self.assertEqual(expected, cmds)
            self.assertAlmostTuple(
                [t.apply_to_point(p) for p in path.control_points],
                list(new_path.control_points),
            )

    def test_arc_transformation(self):
        cases = [
            (
                "M 10 10 A 100 100 0 1 0 100 100 Z",
                ((1, 0, 1), (0, 1, 0)),
                "M 11 10 A 100 100 0 1 0 101 100 Z",
            ),
            (
                "M 10 10 A 100 100 0 1 0 100 100 Z",
                ((1, 0, 0), (0, 1, 1)),
                "M 10 11 A 100 100 0 1 0 100 101 Z",
            ),
            (
                "M 10 10 A 100 100 0 1 0 100 100 Z",
                ((1, 0, 1), (0, 1, 1)),
                "M 11 11 A 100 100 0 1 0 101 101 Z",
            ),
            (
                "M 10 10 A 100 100 0 1 0 100 100 Z",
                ((2, 0, 0), (0, 1, 0)),
                "M 20 10 A 200 100 0 1 0 200 100 Z",
            ),
            (
                "M 10 10 A 100 100 0 1 0 100 100 Z",
                ((1, 0, 0), (0, 2, 0)),
                "M 10 20 A 200 100 90 1 0 100 200 Z",
            ),
            (
                "M 10 10 A 100 100 0 1 0 100 100 Z",
                ((1, 0, 0), (0, -1, 0)),
                "M 10 -10 A 100 100 0 1 1 100 -100 Z",
            ),
            (
                "M 10 10 A 100 100 0 1 0 100 100 Z",
                ((1, 2, 0), (0, 2, 0)),
                "M 30 20 A 292.081 68.4742 41.4375 1 0 300 200 Z",
            ),
            (
                "M 10 10 A 100 100 0 1 0 100 100 A 300 200 0 1 0 50 20 Z",
                ((1, 2, 0), (5, 6, 0)),
                "M 30,110 "
                "A 810.90492,49.327608 74.368134 1 1 "
                "300,1100 1981.2436,121.13604 75.800007 1 1 90,370 Z",
            ),
        ]
        for path, transform, expected in cases:
            expected = Path(expected)
            result = Path(path).transform(Transform(matrix=transform))
            self.assertDeepAlmostEqual(
                expected.to_arrays(), result.to_arrays(), places=4
            )

    def test_single_point_transform(self):
        from math import sqrt, sin, cos

        self.assertAlmostTuple(
            list(Path("M 10 10 30 20").control_points), ((10, 10), (30, 20))
        )
        self.assertAlmostTuple(
            list(
                Path("M 10 10 30 20")
                .transform(Transform(translate=(10, 7)))
                .control_points
            ),
            ((20, 17), (40, 27)),
        )
        self.assertAlmostTuple(
            list(
                Path("M 20 20 5 0 0 7 ").transform(Transform(scale=10)).control_points
            ),
            ((200, 200), (50, 0), (0, 70)),
        )

        self.assertAlmostTuple(
            list(Path("M 20 20 1 0").transform(Transform(rotate=90)).control_points),
            ((-20, 20), (0, 1)),
        )

        self.assertAlmostTuple(
            list(Path("M 20 20 1 0").transform(Transform(rotate=45)).control_points),
            ((0, sqrt(20**2 + 20**2)), (sqrt(2) / 2, sqrt(2) / 2)),
        )

        self.assertAlmostTuple(
            list(Path("M 1 0 0 1").transform(Transform(rotate=30)).control_points),
            ((sqrt(3) / 2, 0.5), (-0.5, sqrt(3) / 2)),
        )

    @novector
    def test_reverse(self):
        """Paths can be reversed"""
        # Testing reverse() with relative coordinates, closed path
        ret = Path(
            "m 10 50 h 40 v -40 l 50 39.9998 c -22 2 -35 12 -50 25 l -40 -15 l 0 -10 z"
        )
        ret = ret.reverse()
        self._assertPath(
            ret,
            "m 10 50 l 0 -0.0002 l -0 10 l 40 15 c 15 -13 28 -23 50 -25 l -50 -39.9998 v 40 z",
        )
        # Testing reverse() with relative coordinates, open path
        ret = Path(
            "m 10 50 h 40 v -40 l 50 39.9998 c -22 2 -35 12 -50 25 l -40 -15 l 0 -10"
        )
        ret = ret.reverse()
        self._assertPath(
            ret,
            "m 10 49.9998 l -0 10 l 40 15 c 15 -13 28 -23 50 -25 l -50 -39.9998 v 40 h -40",
        )
        # Testing reverse() with absolute coordinates, closed path
        ret = Path("M 100 35 L 100 25 L 60 10 C 45 23 32 33 10 35 L 60 75 L 60 35 Z")
        ret = ret.reverse()
        self._assertPath(
            ret,
            "M 100 35 L 60 35 L 60 75 L 10 35 C 32 33 45 23 60 10 L 100 25 Z",
        )
        # Testing reverse() with absolute coordinates, open path
        ret = Path(
            "M 100 35 L 100 25 L 60 10 C 45 23 32 33 10 35 L 60 75 L 60 35 L 100 35"
        )
        ret = ret.reverse()
        self._assertPath(
            ret,
            "M 100 35 L 60 35 L 60 75 L 10 35 C 32 33 45 23 60 10 L 100 25 L 100 35",
        )
        ret = Path("M 100,250 q 250,100 400,250")
        ret = ret.reverse()
        self._assertPath(ret, "M 500 500 q -150 -150 -400 -250")

    @novector
    def test_reverse_empty_path(self):
        """Reversing an empty path should not raise an error"""
        ret = Path()
        ret = ret.reverse()

    @novector
    def test_reverse_multiple_subpaths(self):
        """Test for https://gitlab.com/inkscape/extensions/-/issues/445. First two
        examples are from the issue"""

        ret = Path("M 128,64 L 128,128 M 128,196 L 128,256").reverse()
        self._assertPath(ret, "M 128 256 L 128 196 M 128 128 L 128 64")

        ret = Path("M 128,64 L 128,128 m 128,196 L 128,256").reverse()
        self._assertPath(ret, "M 128 256 L 256 324 m -128 -196 L 128 64")

        # More complex example with absolute and relative move commands
        ret = Path(
            "m 58,88 c -10,2 3,13 10,4 z M 32,67 c 14,-5 23,-3 35,7 m 2,-21 c"
            "10,11 20,19 34,11 M 24,43 c 23,-14 18,-5 39,4"
        ).reverse()
        self._assertPath(
            ret,
            "m 63 47 c -21 -9 -16 -18 -39 -4 M 103 64 c -14 8 -24 0 -34 -11 "
            "m -2 21 c -12 -10 -21 -12 -35 -7 M 58 88 l 10 4 c -7 9 -20 -2 -10 -4 z",
        )
        # Vector2d.__init__ = old_init

    @novector
    def test_break_apart(self):
        """Test breaking apart a path"""
        paths = [
            """m 233,142 a 12,13 0 0 1 16,0 12,13 0 0 1 2,17 12,13 0 0 1 -15,4 l 5,-12 z 
        m 30,-55 c 0,0 -22,25 2,35 24,9 31,1 23,15 -7,13 -7,13 -7,13 m -40,-28 -35,-20 35,-20 z""",
            # Contains two moves, should yield the same result
            """m 233,142 a 12,13 0 0 1 16,0 12,13 0 0 1 2,17 12,13 0 0 1 -15,4 l 5,-12 z 
        m 20,-55 m 10,0 c 0,0 -22,25 2,35 24,9 31,1 23,15 -7,13 -7,13 -7,13 m -40,-28 -35,-20 35,-20 z""",
        ]
        for data in paths:
            ret = Path(data).break_apart()
            self.assertEqual(len(ret), 3)
            self.assertEqual(ret[2].to_absolute(), Path("M 241,122 206,102 241,82 Z"))
            self.assertEqual(
                ret[1].to_relative(),
                Path("m 263,87 c 0,0 -22,25 2,35 24,9 31,1 23,15 -7,13 -7,13 -7,13"),
            )
            self.assertEqual(
                ret[0].to_relative(),
                Path(
                    "m 233,142 a 12,13 0 0 1 16,0 12,13 0 0 1 2,17 12,13 0 0 1 -15,4 l 5,-12 z"
                ),
            )


class SuperPathTest(TestCase):
    """Super path tests for testing the super path class"""

    def test_closing(self):
        """Closing paths create two arrays"""
        path = Path(
            "M 0,0 C 1.505,0 2.727,-0.823 2.727,-1.841 V -4.348 C 2.727,-5.363"
            " 1.505,-6.189 0,-6.189 H -8.3 V 0 Z m -10.713,1.991 h -0.211 V -8.178"
            " H 0 c 2.954,0 5.345,1.716 5.345,3.83 v 2.507 C 5.345,0.271 2.954,1.991"
            " 0,1.991 Z"
        )
        csp = path.to_superpath()
        self.assertEqual(len(csp), 2)

    def test_closing_without_z(self):
        """Closing paths without z create two arrays"""
        path = Path(
            "m 51.553104,253.58572 c -11.644086,-0.14509 -4.683516,-19.48876"
            " 2.096523,-8.48973 1.722993,2.92995 0.781608,6.73867 -2.096523,8.48973"
            " m -3.100522,-13.02176 c -18.971587,17.33811 15.454875,20.05577"
            " 6.51412,3.75474 -1.362416,-2.30812 -3.856221,-3.74395 -6.51412,-3.75474"
        )
        csp = path.to_superpath()
        self.assertEqual(len(csp), 2)

    def test_from_arrays(self):
        """SuperPath from arrays"""
        csp = CubicSuperPath(
            [
                [
                    [[14, 173], [14, 173], (14, 173)],
                    [(15, 171), (17, 168), (18, 168)],
                ],
                [
                    [(18, 167), (18, 167), [20, 165]],
                    ((21, 164), [22, 162], (23, 162)),
                ],
            ]
        )
        self.assertEqual(
            str(csp.to_path()),
            "M 14 173 C 14 173 15 171 17 168 M 18 167 C 20 165 21 164 22 162",
        )

    def test_is_line(self):
        """Test is super path segments can detect lines"""
        path = Path(
            "m 49,88 70,-1 c 18,17 1,59 1.7,59 "
            "0,0 -48.7,18 -70.5,-1 18,-15 25,-32.4 -1.5,-57.2 z"
        )
        csp = path.to_superpath()
        self.assertTrue(csp.is_line(csp[0][0], csp[0][1]), "Should be a line")
        self.assertFalse(
            csp.is_line(csp[0][3], csp[0][4]), "Both controls not detected"
        )
        self.assertFalse(
            csp.is_line(csp[0][1], csp[0][2]), "Start control not detected"
        )
        self.assertFalse(csp.is_line(csp[0][2], csp[0][3]), "End control not detected")
        # Also tests if zone close is applied correctly.
        self.assertEqual(
            str(csp.to_path()),
            "M 49 88 L 119 87 C 137 104 120 146 120.7 146 "
            "C 120.7 146 72 164 50.2 145 C 68.2 130 75.2 112.6 48.7 87.8 Z",
        )

    def test_is_line_simplify(self):
        """Test if super path segments can detect if a segment can be simplified to a line"""
        path = Path("M 10 10 C 20,20 30,30 40,40 C 100, 100 50, 50 60, 60")
        csp = path.to_superpath()

        self.assertTrue(csp.is_line(csp[0][0], csp[0][1]))  # line can be retracted
        self.assertFalse(
            csp.is_line(csp[0][1], csp[0][2])
        )  # is line, but shoots over endpoint

        self.assertEqual(str(csp.to_path()), "M 10 10 L 40 40 C 100 100 50 50 60 60")

    def test_is_line_2(self):
        assert CubicSuperPath.is_line(
            [
                [421.20367729252575, 115.75839791826027],
                [417.7753678856422, 120.70730419246635],
                [411.90718253542866, 129.17827219773602],
            ],
            [
                [407.12100999999996, 136.08731000000003],
                [407.12101, 136.08731000000003],
                [407.12101, 136.08731000000003],
            ],
        )

    def test_is_line_collinear(self):
        self.assertFalse(CubicSuperPath.collinear([1, 2], [2, 2.00001], [3, 2]))
        self.assertTrue(CubicSuperPath.collinear([1, 2], [2, 2], [3, 2]))
        self.assertTrue(CubicSuperPath.collinear([3, 2], [2, 2], [1, 2]))

    def test_is_within(self):
        self.assertTrue(CubicSuperPath.within(2, 1, 3))
        self.assertTrue(CubicSuperPath.within(2, 3, 1))
        self.assertTrue(CubicSuperPath.within(2, 2, 2))
        self.assertTrue(CubicSuperPath.within(2, 3, 2))
        self.assertFalse(CubicSuperPath.within(3, 2.9999, 2))

    def test_is_stable(self):
        """Test for https://gitlab.com/inkscape/extensions/-/issues/374"""
        path = Path("M 10 10 h 10 v 10 h -10 Z")

        tempsub = path.to_superpath()
        comparison = str(tempsub)
        for _ in range(15):
            tempsub = CubicSuperPath(tempsub[0])
            self.assertEqual(comparison, str(tempsub))

    def test_multiple_relative(self):
        """Test for https://gitlab.com/inkscape/extensions/-/issues/450"""

        def compare_complex(current, epts):
            for point, comp in zip(current.end_points, epts):
                self.assertAlmostTuple(point, comp, msg=f"got {point}, expected {comp}")
            for point, comp in zip(current.control_points, epts):
                self.assertAlmostTuple(point, comp, msg=f"got {point}, expected {comp}")
            # now reverse the path
            p_rev = current.reverse()
            for point, comp in zip(p_rev.end_points, epts[::-1]):
                self.assertAlmostTuple(point, comp, msg=f"got {point}, expected {comp}")
            # We expect to have the same amount of closed subpaths after the operation
            self.assertEqual(
                len(re.findall(r"[Zz]", str(p_rev))),
                len(re.findall(r"[Zz]", str(current))),
            )
            # now check that transform works correctly
            p_trans = current.transform(Transform("translate(10, 20)"))
            for point, comp in zip(p_trans.end_points, epts):
                comp = comp + Vector2d(10, 20)
                self.assertAlmostTuple(point, comp, msg=f"got {point}, expected {comp}")

        path = Path("m 50,20 v -10 h -10 z m 30,-20 v 20 h 20 z m -50,20 v -15 h -15 z")
        path2 = Path(
            "m 50,20 v -10 h -10 l 10, 10 m 30,-20 v 20 h 20 l -20,-20 m -50,20 v -15 h -15 z"
        )
        path3 = Path(
            "m 50,20 v -10 h -10 z m 30,-20 v 20 h 20 l -20,-20 m -50,20 v -15 h -15 l 15 15"
        )
        pts = [
            (50, 20),
            (50, 10),
            (40, 10),
            (50, 20),
            (80, 0),
            (80, 20),
            (100, 20),
            (80, 0),
            (30, 20),
            (30, 5),
            (15, 5),
            (30, 20),
        ]
        compare_complex(path, pts)
        compare_complex(path2, pts)
        compare_complex(path3, pts)
        path4 = Path("m 50,20 v -10 h -10 z z z")
        pts4 = [(50, 20), (50, 10), (40, 10), (50, 20), (50, 20), (50, 20)]
        compare_complex(path4, pts4)
        path5 = Path("m 50,20 z m 10, 10 m 20, 20 v -10 h -10 z")
        pts5 = [(50, 20), (50, 20), (60, 30), (80, 50), (80, 40), (70, 40), (80, 50)]
        compare_complex(path5, pts5)


class ProxyTest(TestCase):
    def test_simple_path(self):
        """Check coordinate computation"""
        path = Path("M 10 10 h 10 v 10 h -10 Z")

        proxycommands = list(path.proxy_iterator())

        self.assertAlmostTuple(list(proxycommands[1].previous_end_point), (10, 10))
        self.assertAlmostTuple(list(proxycommands[1].end_point), (20, 10))
        self.assertAlmostTuple(list(proxycommands[2].previous_end_point), (20, 10))


class TestPathErrorHandling(TestCase):
    """Path data error handling"""

    def test_incorrect_parameter_amount(self):
        """Check that extra args (or rather, missing args of the next path) is
        handled correctly, i.e. according to
        https://www.w3.org/TR/SVG/paths.html#PathDataErrorHandling"""
        path = Path("M 10,10 L 20,20,30")
        self.assertEqual(str(path), "M 10 10 L 20 20")
