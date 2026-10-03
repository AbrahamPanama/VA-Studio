# coding=utf-8
"""
Test Inkex path parsing functionality.
"""

import math
from inkex.tester import TestCase
from inkex.paths import (
    PathCommand,
    Quadratic,
    quadratic,
    line,
    move,
    vert,
    horz,
    zoneClose,
    Line,
    Move,
    Horz,
    Vert,
    ZoneClose,
    Curve,
    curve,
    Arc,
    arc,
    Smooth,
    smooth,
    TepidQuadratic,
    tepidQuadratic,
    LengthSettings,
)
from inkex.transforms import Vector2d

# pylint: disable=invalid-name


class PointTests(TestCase):
    """Some more complex cubic and quadratic beziers"""

    def compare_points(self, start, curve: Curve, pts):
        """Helper function to compare the point() function of a bezier curve"""
        for s, x, y in pts:
            self.assertAlmostTuple(
                curve.point(Vector2d(), Vector2d(start), Vector2d(), s),
                Vector2d(x, y),
                precision=8,
            )

    def test_point(self):
        """Check the point computation for cubic beziers.

        See https://github.com/mathandy/svgpathtools/blob/f7e074339d579d0e44d455b92ecf57f24d799fb5/test/test_path.py#L290
        """
        self.compare_points(
            (100, 200),
            Curve(100, 100, 250, 100, 250, 200),
            [
                (0, 100, 200),
                (0.3, 132.4, 137),
                (0.5, 175, 125),
                (0.9, 245.8, 173),
                (1, 250, 200),
            ],
        )
        self.compare_points(
            (600, 800),
            Curve(625, 700, 725, 700, 750, 800),
            [
                (0, 600, 800),
                (0.3, 638.7, 737),
                (0.5, 675, 725),
                (0.9, 740.4, 773),
                (1, 750, 800),
            ],
        )

    def test_point_quadratic(self):
        self.compare_points(
            (200, 300),
            Quadratic(400, 50, 600, 300),
            [
                (0, 200, 300),
                (0.3, 320, 195),
                (0.5, 400, 175),
                (0.9, 560, 255),
                (1, 600, 300),
            ],
        )
        self.compare_points(
            (600, 300),
            Quadratic(800, 550, 1000, 300),
            [
                (0, 600, 300),
                (0.3, 720, 405),
                (0.5, 800, 425),
                (0.9, 960, 345),
                (1, 1000, 300),
            ],
        )


class DerivativeTests(TestCase):
    """Test derivative computation"""

    def compare_derivative(
        self, command, t, expected, n=1, first=Vector2d(0, 0), prev=Vector2d(0, 0)
    ):
        """Derivative comparison function"""
        result = command.derivative(Vector2d(first), Vector2d(prev), Vector2d(), t, n)
        self.assertAlmostTuple(result, expected, precision=12)

    def test_line_first_derivative(self):
        """Compute derivatives of some lines"""
        prev = (2, 1)
        first = (5, 5)
        for t in [None, 0, 0.5, 0.7, 1]:
            self.compare_derivative(Line(5, 5), t, (3, 4), n=1, prev=prev)
            self.compare_derivative(line(5, 5), t, (5, 5), 1, prev=prev)
            self.compare_derivative(horz(5), t, (5, 0), n=1, prev=prev)
            self.compare_derivative(Horz(5), t, (3, 0), n=1, prev=prev)
            self.compare_derivative(vert(5), t, (0, 5), n=1, prev=prev)
            self.compare_derivative(Vert(5), t, (0, 4), n=1, prev=prev)
            self.compare_derivative(zoneClose(), t, (3, 4), n=1, prev=prev, first=first)
            self.compare_derivative(ZoneClose(), t, (3, 4), n=1, prev=prev, first=first)

    def test_line_higher_derivative(self):
        """The higher derivative is 0 for all line-like commands"""
        for t in [None, 0, 0.5, 0.7, 1]:
            for com in [Line(5, 5), line(5, 5), Horz(5), vert(5), ZoneClose()]:
                for n in [2, 4, 200]:
                    self.compare_derivative(com, t, (0, 0), n, (5, 5), (2, 1))

    def test_line_invalid_parameters(self):
        """Some invalid line parameters"""

        # Line with zero length: derivative is undefined
        with self.assertRaises(ValueError):
            self.compare_derivative(Line(0, 0), 0.5, (0, 0), n=1)

        # t not in [0, 1]
        with self.assertRaises(ValueError):
            self.compare_derivative(Line(1, 1), -0.5, (0, 0), n=1)

        # n is not integer or not positive
        with self.assertRaises(ValueError):
            self.compare_derivative(Line(1, 1), -0.5, (0, 0), n=1.5)

        with self.assertRaises(ValueError):
            self.compare_derivative(Line(1, 1), -0.5, (0, 0), n=0)


class CurvatureTests(TestCase):
    """Test curvature computation"""

    def compare_curvature(
        self, command, t, expected, first=Vector2d(0, 0), prev=Vector2d(0, 0)
    ):
        """Curvature comparison function"""
        result = command.curvature(Vector2d(first), Vector2d(prev), t)
        self.assertAlmostEqual(result, expected, places=12)

    def test_line_curvature(self):
        """The curvature is 0 for all line-like commands"""
        for t in [None, 0, 0.5, 0.7, 1]:
            for com in [Line(5, 5), line(5, 5), Horz(5), vert(5), ZoneClose()]:
                self.compare_curvature(com, t, 0, (5, 5), (2, 1))


class NormalTests(TestCase):
    """Test Normal computation."""

    def compare_normal(
        self, command, t, expected, first=Vector2d(0, 0), prev=Vector2d(0, 0)
    ):
        """Normal comparison function"""
        result = command.normal(Vector2d(first), Vector2d(prev), t)
        self.assertAlmostTuple(result, expected, precision=12)

    def test_line_normals(self):
        """Compute unit normals of some lines"""
        prev = (2, 1)
        first = (5, 5)
        for t in [None, 0, 0.5, 0.7, 1]:
            self.compare_normal(Line(5, 5), t, (-4 / 5, 3 / 5), prev=prev)
            self.compare_normal(line(4, -3), t, (3 / 5, 4 / 5), prev=prev)
            self.compare_normal(horz(5), t, (0, 1), prev=prev)
            self.compare_normal(Horz(5), t, (0, 1), prev=prev)
            self.compare_normal(vert(5), t, (-1, 0), prev=prev)
            self.compare_normal(Vert(5), t, (-1, 0), prev=prev)
            self.compare_normal(zoneClose(), t, (-4 / 5, 3 / 5), prev=prev, first=first)
            self.compare_normal(ZoneClose(), t, (-4 / 5, 3 / 5), prev=prev, first=first)


class SplitTests(TestCase):
    """Test various splits"""

    def compare_split(
        self,
        command: PathCommand,
        t,
        command_1,
        command_2,
        first=Vector2d(0, 0),
        prev=Vector2d(0, 0),
        prev_control=Vector2d(0, 0),
    ):
        """Comparison function for splitting"""
        result = command.split(first, prev, prev_control, t)
        print(result[0], result[1])
        self.assertEqual(result[0].letter, command_1.letter)
        self.assertEqual(result[1].letter, command_2.letter)
        self.assertAlmostTuple(result[0].args, command_1.args)
        self.assertAlmostTuple(result[1].args, command_2.args)

    def test_split_line(self):
        """Splitting a relative line should return two relative lines"""
        self.compare_split(
            line(4, 8), 0.25, line(1, 2), line(3, 6), prev=Vector2d(5, 5)
        )

    def test_split_Line(self):
        """Splitting an absolute Line should return two absolute lines"""
        self.compare_split(
            Line(4, 6), 0.25, Line(2.5, 3), Line(4, 6), prev=Vector2d(2, 2)
        )

    def test_split_horz(self):
        """Splitting a relative horz should return two relative horz's"""
        self.compare_split(horz(-8), 0.125, horz(-1), horz(-7), prev=Vector2d(5, 6))

    def test_split_Horz(self):
        """Splitting an absolute Horz should return two absolute Horz's"""
        self.compare_split(Horz(10), 0.75, Horz(8), Horz(10), prev=Vector2d(2, 7))

    def test_split_vert(self):
        """Splitting a relative vert should return two relative verts"""
        self.compare_split(vert(-8), 0.125, vert(-1), vert(-7), prev=Vector2d(6, 5))

    def test_split_Vert(self):
        """Splitting an absolute Vert should return two absolute Verts"""
        self.compare_split(Vert(-3), 0.1, Vert(6), Vert(-3), prev=Vector2d(2, 7))

    def test_split_ZoneClose(self):
        """Splitting an absolute ZoneClose should return a Line and a ZoneClose"""
        self.compare_split(
            ZoneClose(), 0.75, Line(1.25, 1), ZoneClose(), 1 - 1j, 2 + 7j
        )

    def test_split_zoneClose(self):
        """Splitting a relative zoneClose should return a (relative) line and a
        zoneClose"""
        self.compare_split(
            zoneClose(), 0.75, line(-0.75, -6), zoneClose(), 1 - 1j, 2 + 7j
        )

    def test_split_move(self):
        """Splitting a move is not supported, because this would change the number
        of subpaths, i.e. introduce unnecessary complexity."""
        with self.assertRaises(ValueError):
            self.compare_split(Move(6, 6), 0.5, Move(3, 3), Move(6, 6))

        with self.assertRaises(ValueError):
            self.compare_split(move(6, 6), 0.5, move(3, 3), move(3, 3))

    def test_split_curves(self):
        """De Casteljau"""

        self.compare_split(
            Curve(300 + 0j, -100 - 10j, 250 + 100j),
            0.7,
            Curve(225 + 30j, 81.5 + 4.1j, 99.7 + 32.59j),
            Curve(107.5 + 44.8j, 145 + 67j, 250 + 100j),
            0j,
            50 + 100j,
        )
        # Same for relative
        self.compare_split(
            curve(250 - 100j, -150 - 110j, 200 + 0j),
            0.7,
            curve(175 - 70j, 31.5 - 95.9j, 49.7 - 67.41j),
            curve(7.8 + 12.21j, 45.3 + 34.41j, 150.3 + 67.41j),
            0j,
            50 + 100j,
        )

    def test_split_smooth(self):
        """Same for smooth 3rd order. Can only be split into curves"""
        prev = 250 + 100j
        prev_prev = -100 - 10j
        self.compare_split(
            Smooth(0 + 250j, 400 + 150j),
            0.3,
            Curve(355 + 133j, 374.5 + 159.7j, 361.15 + 178.21j),
            Curve(330 + 221.4j, 120 + 220j, 400 + 150j),
            0j,
            prev,
            prev_prev,
        )

        self.compare_split(
            Smooth(0 + 250j, 400 + 150j).to_relative(prev),
            0.3,
            curve(105 + 33j, 124.5 + 59.7j, 111.15 + 78.21j),
            curve(-31.15 + 43.19j, -241.15 + 41.79j, 38.85 - 28.21j),
            0j,
            prev,
            prev_prev,
        )

    def test_split_quadratic(self):
        """De Casteljau for 2nd order curves"""
        self.compare_split(
            Quadratic(300 + 0j, 10 + 10j),
            0.2,
            Quadratic(100 + 80j, 128.4 + 64.4j),
            Quadratic(242 + 2j, 10 + 10j),
            0j,
            50 + 100j,
        )
        self.compare_split(
            quadratic(250 - 100j, -40 - 90j),
            0.2,
            quadratic(50 - 20j, 78.4 - 35.6j),
            quadratic(113.6 - 62.4j, -118.4 - 54.4j),
            0j,
            50 + 100j,
        )

    def test_split_tepid_quadratic(self):
        """Smooth 2nd order"""
        self.compare_split(
            TepidQuadratic(100 + 100j),
            0.2,
            Quadratic(-48 + 12j, -79.2 + 16.8j),
            Quadratic(-204 + 36j, 100 + 100j),
            0j,
            10 + 10j,
            300 + 0j,
        )
        self.compare_split(
            tepidQuadratic(100 + 100j),
            0.2,
            quadratic(-58 + 2j, -88.8 + 7.2j),
            quadratic(-123.2 + 20.8j, 188.8 + 92.8j),
            0j,
            10 + 10j,
            300 + 0j,
        )

    def test_split_arc(self):
        """Splitting an Arc/arc"""

        initial = 725.307482225571 - 915.5548199281527j
        arc5 = Arc(
            (202.79421639137703 + 148.77294617167183j),
            225.6910319606926,
            1,
            1,
            (-624.6375539637027 + 896.5483089399895j),
        )

        a1, a2 = arc5.split(0j, initial, 0j, 0.4)
        print(a1, a2)
        self.assertAlmostEqual(
            arc5.cpoint(0j, initial, 0j, 0.4), a1.cpoint(0j, initial, 0j, 1), delta=TOL
        )

        self.assertAlmostEqual(
            arc5.cpoint(0j, initial, 0j, 0.4),
            a2.cpoint(0j, a1.cend_point(0j, initial), 0j, 0),
            delta=TOL,
        )

        self.assertAlmostEqual(
            arc5.cpoint(0j, initial, 0j, 1),
            a2.cpoint(0j, a1.cend_point(0j, initial), 0j, 1),
            delta=TOL,
        )

        # Same for relative arcs

        arc5rel = arc(
            (202.79421639137703 + 148.77294617167183j),
            225.6910319606926,
            1,
            1,
            (-624.6375539637027 + 896.5483089399895j) - initial,
        )

        a1, a2 = arc5rel.split(0j, initial, 0j, 0.4)
        print(a1, a2)
        self.assertAlmostEqual(
            arc5rel.cpoint(0j, initial, 0j, 0.4),
            a1.cpoint(0j, initial, 0j, 1),
            delta=TOL,
        )

        self.assertAlmostEqual(
            arc5rel.cpoint(0j, initial, 0j, 0.4),
            a2.cpoint(0j, a1.cend_point(0j, initial), 0j, 0),
            delta=TOL,
        )

        self.assertAlmostEqual(
            arc5rel.cpoint(0j, initial, 0j, 1),
            a2.cpoint(0j, a1.cend_point(0j, initial), 0j, 1),
            delta=TOL,
        )


class LengthTests(TestCase):
    """Test length computation"""

    def compare_length(
        self, prev, command: PathCommand, expected, t0=0, t1=1, first=Vector2d(0, 0)
    ):
        """Length comparison function"""
        result = command.length(
            Vector2d(first),
            Vector2d(prev),
            Vector2d(),
            t0,
            t1,
            LengthSettings(error=1e-12),
        )
        self.assertAlmostEqual(result, expected, places=8)

    def test_length_lines(self):
        """Test length computation for line-like commands"""

        prev = (2, 1)
        first = (5, 5)

        self.compare_length(prev, Line(5, 5), 5)
        self.compare_length(prev, Line(5, 5), 3, 0.2, 0.8)
        self.compare_length(prev, line(-5, 12), 13)
        self.compare_length(prev, horz(5), 4, 0, 0.8)
        self.compare_length(prev, Horz(5), 3)
        self.compare_length(prev, Vert(5), 3, 0.25, 1)
        self.compare_length(prev, vert(5), 5)
        self.compare_length(prev, ZoneClose(), 5, first=first)
        self.compare_length(prev, zoneClose(), 4, 0.2, first=first)

    def test_length_cubic(self):
        """based on https://github.com/mathandy/svgpathtools/blob/f7e074339d579d0e44d455b92ecf57f24d799fb5/test/test_path.py#L363"""
        # A straight line
        self.compare_length((0, 0), Curve(0, 0, 0, 100, 0, 100), 100)
        # A diagonal line
        self.compare_length((0, 0), Curve(0, 0, 100, 100, 100, 100), math.sqrt(2) * 100)
        # A cubic bezier that approximates a quarter arc.
        kappa = 4 * (math.sqrt(2) - 1) / 3
        self.compare_length(
            (0, 0),
            Curve(0, 100 * kappa, 100 - 100 * kappa, 100, 100, 100),
            157.10166980361998,
        )

    def test_length_quadratic(self):
        """based on https://github.com/mathandy/svgpathtools/blob/f7e074339d579d0e44d455b92ecf57f24d799fb5/test/test_path.py#L450"""
        q1 = [(200, 300), Quadratic(400, 50, 600, 300)]
        q2 = [(200, 300), Quadratic(400, 50, 500, 200)]
        self.compare_length(*q1, 487.77109389525975)
        self.compare_length(*q2, 379.90458193489155)
        # Closed quadratic
        closedq = [(6, 2), Quadratic(5, -1, 6, 2)]
        self.compare_length(*closedq, 3.1622776601683795)
        # lines
        linq1 = [(1, 0), Quadratic(2, 0, 3, 0)]
        linq2 = [(1, 3), Quadratic(2, 5, -9, -17)]
        self.compare_length(*linq1, 2)
        self.compare_length(*linq2, 22.73335777124786)
        # all nodes on the same point
        samepoint = [(1, 0), Quadratic(1, 0, 1, 0)]
        self.compare_length(*samepoint, 0)

        # partial length tests
        self.compare_length(*q1, 212.34775387566032, 0.25, 0.75)
        self.compare_length(*q2, 166.22170622052397, 0.25, 0.75)
        self.compare_length(*closedq, 0.7905694150420949, 0.25, 0.75)
        self.compare_length(*linq1, 1, 0.25, 0.75)
        self.compare_length(*samepoint, 0, 0.25, 0.75)


class IlengthTests(TestCase):
    """Test inverse length computation"""

    def compare_ilength(
        self,
        command,
        s,
        expected,
        first=Vector2d(0, 0),
        prev=Vector2d(0, 0),
        prev_control=Vector2d(0, 0),
        places=8,
    ):
        """Inverse Length comparison function"""
        result = command.ilength(
            Vector2d(first),
            Vector2d(prev),
            Vector2d(prev_control),
            s,
        )
        self.assertAlmostEqual(result, expected, places=places)

    def test_ilength_lines(self):
        """Test inverse length computation for line-like commands"""
        prev = (2, 1)
        first = (5, 5)

        self.compare_ilength(Line(5, 5), 5, 1, prev=prev)
        self.compare_ilength(Line(5, 5), 4, 0.8, prev=prev)

        self.compare_ilength(line(-5, 12), 6.5, 0.5, prev=prev)
        self.compare_ilength(horz(5), 4, 0.8, prev=prev)
        self.compare_ilength(Horz(5), 2, 2 / 3, prev=prev)
        self.compare_ilength(Vert(5), 3, 0.75, prev=prev)
        self.compare_ilength(vert(5), 5, 1, prev=prev)
        self.compare_ilength(ZoneClose(), 5, 1, prev=prev, first=first)
        self.compare_ilength(zoneClose(), 4, 0.8, prev=prev, first=first)

    def test_ilength_quadratics(self):
        """From https://github.com/mathandy/svgpathtools/blob/f7e074339d579d0e44d455b92ecf57f24d799fb5/test/test_path.py#L1214"""
        q1 = [(200, 300), Quadratic(400, 50, 600, 300)]
        q2 = [(200, 300), Quadratic(400, 50, 500, 200)]
        closedq = [(6, 2), Quadratic(5, -1, 6, 2)]
        linq = [(1, 3), Quadratic(2, 5, -9, -17)]

        tests = [
            (q1, 0.01, 6.364183310105577),
            (q1, 0.1, 60.23857499635088),
            (q1, 0.5, 243.8855469477619),
            (q1, 0.9, 427.53251889917294),
            (q1, 0.99, 481.40691058541813),
            (q2, 0.01, 6.365673533661836),
            (q2, 0.1, 60.31675895732397),
            (q2, 0.5, 233.24592830045907),
            (q2, 0.9, 346.42891253298706),
            (q2, 0.99, 376.32659156736844),
            (closedq, 0.01, 0.06261309767133393),
            (closedq, 0.1, 0.5692099788303084),
            (closedq, 0.5, 1.5811388300841898),
            (closedq, 0.9, 2.5930676813380713),
            (closedq, 0.99, 3.0996645624970456),
            (linq, 0.01, 0.04203807797699605),
            (linq, 0.1, 0.19379255804998186),
            (linq, 0.5, 4.844813951249544),
            (linq, 0.9, 18.0823363780483),
            (linq, 0.99, 22.24410609777091),
        ]

        for q, t, s in tests:
            self.compare_ilength(q[1], s, t, prev=q[0], places=4)
            rel = q[1].to_relative(Vector2d(q[0]))
            self.compare_ilength(rel, s, t, prev=q[0], places=4)

    def test_ilength_cubics(self):
        """From https://github.com/mathandy/svgpathtools/blob/f7e074339d579d0e44d455b92ecf57f24d799fb5/test/test_path.py#L1250"""
        c1 = [(200, 300), Curve(400, 50, 600, 100, -200, 0)]
        symc = [(1, -2), Curve(10, -1, 10, 1, 1, 2)]
        closedc = [(1, -2), Curve(10, -1, 10, 1, 1, -2)]

        tests = [
            (c1, 0.01, 9.53434737943073),
            (c1, 0.1, 88.89941848775852),
            (c1, 0.5, 278.5750942713189),
            (c1, 0.9, 651.4957786584646),
            (c1, 0.99, 840.2010603832538),
            (symc, 0.01, 0.2690118556702902),
            (symc, 0.1, 2.45230693868727),
            (symc, 0.5, 7.256147083644424),
            (symc, 0.9, 12.059987228602886),
            (symc, 0.99, 14.243282311619401),
            (closedc, 0.01, 0.26901140075538765),
            (closedc, 0.1, 2.451722765460998),
            (closedc, 0.5, 6.974058969750422),
            (closedc, 0.9, 11.41781741489913),
            (closedc, 0.99, 13.681324783697782),
        ]

        for q, t, s in tests:
            self.compare_ilength(q[1], s, t, prev=q[0], places=4)
        rel = q[1].to_relative(Vector2d(q[0]))
        self.compare_ilength(rel, s, t, prev=q[0], places=4)


TOL = 1e-4


class ArcTest(TestCase):
    def test_point(self):
        def assert_point(arc: Arc, start: complex, at: float, point: complex):
            self.assertAlmostEqual(arc.cpoint(0j, start, 0j, at), point, delta=TOL)

        arc1 = Arc(100 + 50j, 0, 0, 0, 100 + 50j)
        _, _, _, center, theta1, deltaTheta = arc1.parametrize(0j)
        self.assertAlmostEqual(center, 100 + 0j, delta=TOL)
        self.assertAlmostEqual(theta1, 180.0, delta=TOL)
        self.assertAlmostEqual(deltaTheta, -90.0, delta=TOL)

        for at, point in [
            (0.0, 0j),
            (0.1, 1.23116594049 + 7.82172325201j),
            (0.2, 4.89434837048 + 15.4508497187j),
            (0.3, 10.8993475812 + 22.699524987j),
            (0.4, 19.0983005625 + 29.3892626146j),
            (0.5, 29.2893218813 + 35.3553390593j),
            (0.6, 41.2214747708 + 40.4508497187j),
            (0.7, 54.6009500260 + 44.5503262094j),
            (0.8, 69.0983005625 + 47.5528258148j),
            (0.9, 84.3565534960 + 49.3844170298j),
            (1.0, 100 + 50j),
        ]:
            assert_point(Arc(100 + 50j, 0, 0, 0, 100 + 50j), 0j, at, point)

        arc2 = Arc(100 + 50j, 0, 1, 0, 100 + 50j)
        _, _, _, center, theta1, deltaTheta = arc2.parametrize(0j)
        self.assertAlmostEqual(center, 50j, delta=TOL)
        self.assertAlmostEqual(theta1, -90.0, delta=TOL)
        self.assertAlmostEqual(deltaTheta, -270.0, delta=TOL)

        for at, point in [
            (0.0, 0j),
            (0.1, (-45.399049974 + 5.44967379058j)),
            (0.2, (-80.9016994375 + 20.6107373854j)),
            (0.3, (-98.7688340595 + 42.178276748j)),
            (0.4, (-95.1056516295 + 65.4508497187j)),
            (0.5, (-70.7106781187 + 85.3553390593j)),
            (0.6, (-30.9016994375 + 97.5528258148j)),
            (0.7, (15.643446504 + 99.3844170298j)),
            (0.8, (58.7785252292 + 90.4508497187j)),
            (0.9, (89.1006524188 + 72.699524987j)),
            (1.0, (100 + 50j)),
        ]:
            assert_point(Arc(100 + 50j, 0, 1, 0, 100 + 50j), 0j, at, point)

        arc3 = Arc(100 + 50j, 0, 0, 1, 100 + 50j)
        _, _, _, center, theta1, deltaTheta = arc3.parametrize(0j)
        self.assertAlmostEqual(center, 50j, delta=TOL)
        self.assertAlmostEqual(theta1, -90.0, delta=TOL)
        self.assertAlmostEqual(deltaTheta, 90.0, delta=TOL)

        for at, point in [
            (0.0, 0j),
            (0.1, (15.643446504 + 0.615582970243j)),
            (0.2, (30.9016994375 + 2.44717418524j)),
            (0.3, (45.399049974 + 5.44967379058j)),
            (0.4, (58.7785252292 + 9.54915028125j)),
            (0.5, (70.7106781187 + 14.6446609407j)),
            (0.6, (80.9016994375 + 20.6107373854j)),
            (0.7, (89.1006524188 + 27.300475013j)),
            (0.8, (95.1056516295 + 34.5491502813j)),
            (0.9, (98.7688340595 + 42.178276748j)),
            (1.0, (100 + 50j)),
        ]:
            assert_point(Arc(100 + 50j, 0, 0, 1, 100 + 50j), 0j, at, point)

        arc4 = Arc(100 + 50j, 0, 1, 1, 100 + 50j)
        _, _, _, center, theta1, deltaTheta = arc4.parametrize(0j)
        self.assertAlmostEqual(center, 100 + 0j, delta=TOL)
        self.assertAlmostEqual(theta1, 180.0, delta=TOL)
        self.assertAlmostEqual(deltaTheta, 270.0, delta=TOL)

        for at, point in [
            (0.0, 0j),
            (0.1, (10.8993475812 - 22.699524987j)),
            (0.2, (41.2214747708 - 40.4508497187j)),
            (0.3, (84.3565534960 - 49.3844170298j)),
            (0.4, (130.901699437 - 47.5528258148j)),
            (0.5, (170.710678119 - 35.3553390593j)),
            (0.6, (195.105651630 - 15.4508497187j)),
            (0.7, (198.768834060 + 7.82172325201j)),
            (0.8, (180.901699437 + 29.3892626146j)),
            (0.9, (145.399049974 + 44.5503262094j)),
            (1.0, (100 + 50j)),
        ]:
            assert_point(Arc(100 + 50j, 0, 1, 1, 100 + 50j), 0j, at, point)

        initial = 725.307482225571 - 915.5548199281527j
        arc5 = Arc(
            (202.79421639137703 + 148.77294617167183j),
            225.6910319606926,
            1,
            1,
            (-624.6375539637027 + 896.5483089399895j),
        )
        arc5rel = arc(
            (202.79421639137703 + 148.77294617167183j),
            225.6910319606926,
            1,
            1,
            (-624.6375539637027 + 896.5483089399895j) - initial,
        )

        for at, point in [
            (0.0, initial),
            (0.0909090909091, (1023.47397369 - 597.730444283j)),
            (0.181818181818, (1242.80253007 - 232.251400124j)),
            (0.272727272727, (1365.52445614 + 151.273373978j)),
            (0.363636363636, (1381.69755131 + 521.772981736j)),
            (0.454545454545, (1290.01156757 + 849.231748376j)),
            (0.545454545455, (1097.89435807 + 1107.12091209j)),
            (0.636363636364, (820.910116547 + 1274.54782658j)),
            (0.727272727273, (481.49845896 + 1337.94855893j)),
            (0.818181818182, (107.156499251 + 1292.18675889j)),
            (0.909090909091, (-271.788803303 + 1140.96977533j)),
        ]:
            assert_point(arc5, initial, at, point)
            assert_point(arc5rel, initial, at, point)
