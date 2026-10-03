# coding=utf-8
"""
Test Inkex transformational logic.
"""

from math import sqrt, pi

import pytest
import numpy as np
from inkex.transforms import (
    Vector2d,
    ImmutableVector2d,
    BoundingBox,
    BoundingInterval,
    Transform,
    DirectedLineSegment,
)
from inkex.utils import PY3
from inkex.tester import TestCase


class ImmutableVector2dTest(TestCase):
    """Test the ImmutableVector2d object"""

    def test_numpy_conversion(self):
        """Check that vectors work fine in numpy datatypes (they are complex under
        the hood)"""
        import numpy as np

        vecs = [ImmutableVector2d(1, 2), 1 + 2j, Vector2d(0, 5)]
        res = np.array(vecs)
        assert "complex" in str(res.dtype)
        assert np.allclose(vecs, [1 + 2j, 1 + 2j, 0 + 5j])

    def test_vector_creation(self):
        """Test ImmutableVector2d creation"""
        vec0 = ImmutableVector2d(15, 22)
        self.assertEqual(vec0.x, 15)
        self.assertEqual(vec0.y, 22)

        vec1 = ImmutableVector2d()
        self.assertEqual(vec1.x, 0)
        self.assertEqual(vec1.y, 0)

        vec2 = ImmutableVector2d((17, 32))
        self.assertEqual(vec2.x, 17)
        self.assertEqual(vec2.y, 32)

        vec3 = ImmutableVector2d(vec0)
        self.assertEqual(vec3.x, 15)
        self.assertEqual(vec3.y, 22)

        vec4 = ImmutableVector2d("-5,8")
        self.assertEqual(vec4.x, -5)
        self.assertEqual(vec4.y, 8)

        self.assertRaises(ValueError, ImmutableVector2d, (1, 2, 3))

    def test_binary_operators(self):
        """Test binary operators for vector2d"""
        vec1 = ImmutableVector2d(15, 22)
        vec2 = ImmutableVector2d(5, 3)

        self.assertTrue((vec1 - vec2).is_close((10, 19)))
        self.assertTrue((vec1 - (5, 3)).is_close((10, 19)))
        self.assertTrue(((15, 22) - vec2).is_close((10, 19)))
        self.assertTrue((vec1 + vec2).is_close((20, 25)))
        self.assertTrue((vec1 + (5, 3)).is_close((20, 25)))
        self.assertTrue(((15, 22) + vec2).is_close((20, 25)))
        self.assertTrue((vec1 * 2).is_close((30, 44)))
        self.assertTrue((2 * vec1).is_close((30, 44)))
        self.assertTrue((vec1 / 2).is_close((7.5, 11)))
        self.assertTrue((vec1.__div__(2)).is_close((7.5, 11)))
        self.assertTrue((vec1 // 2).is_close((7.5, 11)))

    def test_ioperators(self):
        """Test operators for vector2d"""
        vec0 = vec = ImmutableVector2d(15, 22)
        vec += (1, 1)
        vec = ImmutableVector2d(vec)
        self.assertTrue(vec.is_close((16, 23)))
        vec -= (10, 20)
        vec = ImmutableVector2d(vec)
        self.assertTrue(vec.is_close((6, 3)))
        vec *= 5
        vec = ImmutableVector2d(vec)
        self.assertTrue(vec.is_close((30, 15)))
        vec /= 90
        vec = ImmutableVector2d(vec)
        self.assertTrue(vec.is_close((1.0 / 3, 1.0 / 6)))
        vec //= 1.0 / 3
        vec = ImmutableVector2d(vec)
        self.assertTrue(vec.is_close((1, 0.5)))
        self.assertTrue(vec0.is_close((15, 22)))
        self.assertFalse(vec0.is_close(vec))

    def test_unary_operators(self):
        """Test unary operators"""
        vec = ImmutableVector2d(1, 2)
        self.assertTrue((-vec).is_close((-1, -2)))
        self.assertTrue((+vec).is_close(vec))
        self.assertAlmostEqual(abs(vec), sqrt(5))
        self.assertTrue(+vec is not vec)  # returned value is a copy

    def test_representations(self):
        """Test ImmutableVector2d Repr"""
        self.assertEqual(str(ImmutableVector2d(1, 2)), "1, 2")
        self.assertEqual(repr(ImmutableVector2d(1, 2)), "Vector2d(1, 2)")
        self.assertEqual(ImmutableVector2d(1, 2).to_tuple(), (1, 2))

    def test_assign(self):
        """Test ImmutableVector2d assignement"""
        vec = ImmutableVector2d(10, 20)
        with pytest.raises(AttributeError):
            vec.assign(5, 10)

    def test_getitem(self):
        """Test getitem for ImmutableVector2d"""
        vec = ImmutableVector2d(10, 20)
        self.assertEqual(len(vec), 2)
        self.assertEqual(vec[0], 10)
        self.assertEqual(vec[1], 20)

    def test_cross(self):
        """Test cross product for ImmutableVector2d"""
        vec1 = ImmutableVector2d(0, 2)
        vec2 = ImmutableVector2d(0, 3)
        vec3 = ImmutableVector2d(0, -3)
        vec4 = ImmutableVector2d(3, 0)
        vec5 = ImmutableVector2d(-3, 0)
        vec6 = ImmutableVector2d(1, 1)
        self.assertAlmostEqual(vec1.cross(vec2), 0)
        self.assertAlmostEqual(vec2.cross(vec3), 0)
        self.assertAlmostEqual(vec1.cross(vec4), -6)
        self.assertAlmostEqual(vec1.cross(vec5), 6)
        self.assertAlmostEqual(vec1.cross(vec6), -2.0)
        self.assertAlmostEqual(vec6.cross(vec1), 2.0)


class Vector2dTest(TestCase):
    """Test the Vector2d object"""

    def test_vector_creation(self):
        """Test Vector2D creation"""
        vec0 = Vector2d(15, 22)
        self.assertEqual(vec0.x, 15)
        self.assertEqual(vec0.y, 22)

        vec1 = Vector2d()
        self.assertEqual(vec1.x, 0)
        self.assertEqual(vec1.y, 0)

        vec2 = Vector2d((17, 32))
        self.assertEqual(vec2.x, 17)
        self.assertEqual(vec2.y, 32)

        vec3 = Vector2d(vec0)
        self.assertEqual(vec3.x, 15)
        self.assertEqual(vec3.y, 22)

        self.assertRaises(ValueError, Vector2d, (1, 2, 3))
        self.assertRaises(ValueError, Vector2d, 1, 2, 3)

    def test_vector_default_creation(self):
        """Test fallback for vectors"""

        # no fallback
        vec0 = Vector2d("1,2", fallback=None)
        self.assertEqual(vec0.x, 1)
        self.assertEqual(vec0.y, 2)

        self.assertRaises(ValueError, Vector2d, "a,2")

        # fallback
        vec0 = Vector2d("a,3", fallback=(1, 2))
        self.assertEqual(vec0.x, 1)
        self.assertEqual(vec0.y, 2)

        vec0 = Vector2d(("a", "b"), fallback=(1, 2))
        self.assertEqual(vec0.x, 1)
        self.assertEqual(vec0.y, 2)

        vec0 = Vector2d((3, 4, 5), fallback=(1, 2))
        self.assertEqual(vec0.x, 1)
        self.assertEqual(vec0.y, 2)

    def test_binary_operators(self):
        """Test binary operators for vector2d"""
        vec1 = Vector2d(15, 22)
        vec2 = Vector2d(5, 3)

        self.assertTrue((vec1 - vec2).is_close((10, 19)))
        self.assertTrue((vec1 - (5, 3)).is_close((10, 19)))
        self.assertTrue(((15, 22) - vec2).is_close((10, 19)))
        self.assertTrue((vec1 + vec2).is_close((20, 25)))
        self.assertTrue((vec1 + (5, 3)).is_close((20, 25)))
        self.assertTrue(((15, 22) + vec2).is_close((20, 25)))
        self.assertTrue((vec1 * 2).is_close((30, 44)))
        self.assertTrue((2 * vec1).is_close((30, 44)))
        self.assertTrue((vec1 / 2).is_close((7.5, 11)))
        self.assertTrue((vec1.__div__(2)).is_close((7.5, 11)))
        self.assertTrue((vec1 // 2).is_close((7.5, 11)))

    def test_ioperators(self):
        """Test operators for vector2d"""
        vec0 = vec = Vector2d(15, 22)
        vec += (1, 1)
        self.assertTrue(vec.is_close((16, 23)))
        vec -= (10, 20)
        self.assertTrue(vec.is_close((6, 3)))
        vec *= 5
        self.assertTrue(vec.is_close((30, 15)))
        vec /= 90
        self.assertTrue(vec.is_close((1.0 / 3, 1.0 / 6)))
        vec //= 1.0 / 3
        self.assertTrue(vec.is_close((1, 0.5)))

    def test_unary_operators(self):
        """Test unary operators"""
        vec = Vector2d(1, 2)
        self.assertTrue((-vec).is_close((-1, -2)))
        self.assertTrue((+vec).is_close(vec))
        self.assertTrue(+vec is not vec)  # returned value is a copy

    def test_representations(self):
        """Test Vector2D Repr"""
        self.assertEqual(str(Vector2d(1, 2)), "1, 2")
        self.assertEqual(repr(Vector2d(1, 2)), "Vector2d(1, 2)")
        self.assertEqual(Vector2d(1, 2).to_tuple(), (1, 2))

    def test_getitem(self):
        """Test getitem for Vector2D"""
        vec = Vector2d(10, 20)
        self.assertEqual(len(vec), 2)
        self.assertEqual(vec[0], 10)
        self.assertEqual(vec[1], 20)

    def test_polar_operations(self):
        """Test polar coordinates operations"""
        #               x  y  r  pi
        equivilents = [
            (0, 0, 0, 0),
            (0, 0, 0, 1),
            (0, 0, 0, -1),
            (0, 0, 0, 0.5),
            (1, 0, 1, 0),
            (0, 1, 1, 0.5),
            (0, -1, 1, -0.5),
            (3, 0, 3, 0),
            (0, 3, 3, 0.5),
            (0, -3, 3, -0.5),
            (sqrt(2), sqrt(2), 2, 0.25),
            (-sqrt(2), sqrt(2), 2, 0.75),
            (sqrt(2), -sqrt(2), 2, -0.25),
            (-sqrt(2), -sqrt(2), 2, -0.75),
        ]
        for x, y, r, t in equivilents:
            theta = t * pi if r != 0 else None
            for ts in [0, 2, -2]:
                ctx_msg = "Test values are x: {} y: {} r: {} θ: {} * pi".format(
                    x, y, r, t + ts
                )
                polar = Vector2d.from_polar(r, (t + ts) * pi)
                cart = Vector2d(x, y)
                self.assertEqual(cart.length, r, msg=ctx_msg)
                self.assertEqual(polar.length, r, msg=ctx_msg)
                self.assertAlmostEqual(cart.angle, theta, msg=ctx_msg, delta=1e-12)
                self.assertAlmostEqual(polar.angle, theta, msg=ctx_msg, delta=1e-12)
                self.assertEqual(cart.to_polar_tuple(), (r, cart.angle), msg=ctx_msg)
                self.assertEqual(polar.to_polar_tuple(), (r, polar.angle), msg=ctx_msg)
                self.assertEqual(cart.to_tuple(), (x, y), msg=ctx_msg)
                self.assertAlmostEqual(polar.to_tuple()[0], x, msg=ctx_msg, delta=1e-12)
                self.assertAlmostEqual(polar.to_tuple()[1], y, msg=ctx_msg, delta=1e-12)
        # Test special handling of from_polar with None theta
        self.assertEqual(Vector2d.from_polar(0, None).to_tuple(), (0.0, 0.0))
        self.assertIsNone(Vector2d.from_polar(4, None))


class TransformTest(TestCase):
    """Test transformation API and calculations"""

    def test_new_empty(self):
        """Create a transformation from two triplets matrix"""
        self.assertEqual(Transform(), ((1, 0, 0), (0, 1, 0)))

    def test_new_from_triples(self):
        """Create a transformation from two triplets matrix"""
        self.assertEqual(Transform(((1, 2, 3), (4, 5, 6))), ((1, 2, 3), (4, 5, 6)))

    def test_new_from_sextlet(self):
        """Create a transformation from a list of six numbers"""
        self.assertEqual(Transform((1, 2, 3, 4, 5, 6)), ((1, 3, 5), (2, 4, 6)))

    def test_new_from_matrix_str(self):
        """Create a transformation from a list of six numbers"""
        self.assertEqual(Transform("matrix(1, 2, 3, 4, 5, 6)"), ((1, 3, 5), (2, 4, 6)))

    def test_new_from_scale(self):
        """Create a scale based transformation"""
        self.assertEqual(Transform("scale(10)"), ((10, 0, 0), (0, 10, 0)))
        self.assertEqual(Transform("scale(10, 3.3)"), ((10, 0, 0), (0, 3.3, 0)))

    def test_new_from_translate(self):
        """Create a translate transformation"""
        self.assertEqual(Transform("translate(12)"), ((1, 0, 12), (0, 1, 0)))
        self.assertEqual(Transform("translate(12, 14)"), ((1, 0, 12), (0, 1, 14)))

    def test_new_from_rotate(self):
        """Create a rotational transformation"""
        self.assertEqual(str(Transform("rotate(90)")), "rotate(90)")
        self.assertEqual(
            str(Transform("rotate(90 10 12)")),
            "matrix(6.12323e-17 1 -1 6.12323e-17 22 2)",
        )

    def test_new_from_skew(self):
        """Create skew x/y transformations"""
        self.assertEqual(str(Transform("skewX(10)")), "matrix(1 0 0.176327 1 0 0)")
        self.assertEqual(str(Transform("skewY(10)")), "matrix(1 0.176327 0 1 0 0)")

    def test_invalid_creation_string(self):
        """Test creating invalid transforms"""
        self.assertEqual(Transform("boo(4)"), ((1, 0, 0), (0, 1, 0)))

    def test_invalid_creation_matrix(self):
        """Test creating invalid transforms"""
        self.assertRaises(ValueError, Transform, 0.0)
        self.assertRaises(ValueError, Transform, (0.0,))
        self.assertRaises(ValueError, Transform, (0.0, 0.0, 0.0))

    def test_repr(self):
        """Test repr string"""
        self.assertEqual(repr(Transform()), "Transform(((1, 0, 0), (0, 1, 0)))")

    def test_matrix_inversion(self):
        """Test the negative of a transformation"""
        self.assertEqual(-Transform("rotate(45)"), Transform("rotate(-45)"))
        self.assertEqual(
            -Transform("translate(12, 10)"), Transform("translate(-12, -10)")
        )
        self.assertEqual(-Transform("scale(4)"), Transform("scale(0.25)"))

    def test_apply_to_point(self):
        """Test applying the transformation to a point"""
        trans = Transform("translate(10, 10)")
        self.assertEqual(trans.apply_to_point((10, 10)).to_tuple(), (20, 20))
        self.assertRaises(ValueError, trans.apply_to_point, "")

    def test_translate(self):
        """Test making translate specific items"""
        self.assertEqual(
            str(Transform(translate=(10.6, 99.9))), "translate(10.6, 99.9)"
        )

    def test_scale(self):
        """Test making scale specific items"""
        self.assertEqual(str(Transform(scale=(1.0, 2.2))), "scale(1, 2.2)")

    def test_rotate(self):
        """Test making rotate specific items"""
        self.assertEqual(str(Transform(rotate=45)), "rotate(45)")
        self.assertEqual(
            str(Transform(rotate=(45, 10, 10))),
            "matrix(0.707107 0.707107 -0.707107 0.707107 10 -4.14214)",
        )

    def test_add_transform(self):
        """Test add_TRANSFORM syntax for quickly composing known transforms"""
        tr1 = Transform()
        tr1.add_scale(5.0, 1.0)
        self.assertEqual(str(tr1), "scale(5, 1)")
        tr1.add_translate(10, 10)
        self.assertEqual(str(tr1), "matrix(5 0 0 1 50 10)")
        self.assertEqual(str(Transform().add_scale(5.0, 1.0)), "scale(5, 1)")
        self.assertEqual(
            str(Transform().add_scale(5.0, 1.0).add_translate(10, 10)),
            "matrix(5 0 0 1 50 10)",
        )
        tr2 = Transform()
        self.assertTrue(
            tr2.add_scale(1, 1)
            .add_translate(0, 0)
            .add_skewy(0)
            .add_skewx(0)
            .add_rotate(0)
            is tr2
        )
        self.assertEqual(
            str(tr2.add_kwargs(translate=(10, 10), scale=(5.0, 1.0))),
            "matrix(5 0 0 1 50 10)",
        )

    def test_is_unity(self):
        """Test that unix matrix looks like rotate, scale, and translate"""
        unity = Transform()
        self.assertTrue(unity.is_rotate())
        self.assertTrue(unity.is_scale())
        self.assertTrue(unity.is_translate())

    def test_is_rotation(self):
        """Test that rotations about origin are correctly identified"""
        rot1 = Transform(rotate=21)
        rot2 = Transform(rotate=35)
        rot3 = Transform(rotate=53)

        self.assertFalse(Transform(translate=1e-9).is_rotate(exactly=True))
        self.assertFalse(Transform(scale=1 + 1e-9).is_rotate(exactly=True))
        self.assertFalse(Transform(skewx=1e-9).is_rotate(exactly=True))
        self.assertFalse(Transform(skewy=1e-9).is_rotate(exactly=True))

        self.assertTrue(Transform(translate=1e-9).is_rotate(exactly=False))
        self.assertTrue(Transform(scale=1 + 1e-9).is_rotate(exactly=False))
        self.assertTrue(Transform(skewx=1e-9).is_rotate(exactly=False))
        self.assertTrue(Transform(skewy=1e-9).is_rotate(exactly=False))

        self.assertTrue(rot1.is_rotate())
        self.assertTrue(rot2.is_rotate())
        self.assertTrue(rot3.is_rotate())

        self.assertFalse(rot1.is_translate())
        self.assertFalse(rot2.is_translate())
        self.assertFalse(rot3.is_translate())

        self.assertFalse(rot1.is_scale())
        self.assertFalse(rot2.is_scale())
        self.assertFalse(rot3.is_scale())

        self.assertTrue((rot1 @ rot1).is_rotate())
        self.assertTrue((rot1 @ rot2).is_rotate())
        self.assertTrue((rot1 @ rot2 @ rot3 @ rot2 @ rot1).is_rotate())

    def test_is_translate(self):
        """Test that translations are correctly identified"""
        tr1 = Transform(translate=(1.1,))
        tr2 = Transform(translate=(1.3, 2.7))
        tr3 = Transform(translate=(sqrt(2) / 2, pi))

        self.assertFalse(Transform(rotate=1e-9).is_translate(exactly=True))
        self.assertFalse(Transform(scale=1 + 1e-9).is_translate(exactly=True))
        self.assertFalse(Transform(skewx=1e-9).is_translate(exactly=True))
        self.assertFalse(Transform(skewy=1e-9).is_translate(exactly=True))

        self.assertTrue(Transform(rotate=1e-9).is_translate(exactly=False))
        self.assertTrue(Transform(scale=1 + 1e-9).is_translate(exactly=False))
        self.assertTrue(Transform(skewx=1e-9).is_translate(exactly=False))
        self.assertTrue(Transform(skewy=1e-9).is_translate(exactly=False))

        self.assertTrue(tr1.is_translate())
        self.assertTrue(tr2.is_translate())
        self.assertTrue(tr3.is_translate())
        self.assertFalse(tr1.is_rotate())
        self.assertFalse(tr2.is_rotate())
        self.assertFalse(tr3.is_rotate())
        self.assertFalse(tr1.is_scale())
        self.assertFalse(tr2.is_scale())
        self.assertFalse(tr3.is_scale())

        self.assertTrue((tr1 @ tr1).is_translate())
        self.assertTrue((tr1 @ tr2).is_translate())
        self.assertTrue((tr1 @ tr2 @ tr3 @ tr2 @ tr1).is_translate())
        self.assertFalse(tr1 @ tr2 @ tr3 @ -tr1 @ -tr2 @ -tr3)  # is almost unity

    def test_is_scale(self):
        """Test that scale transformations are correctly identified"""
        s1 = Transform(scale=(1.1,))
        s2 = Transform(scale=(1.3, 2.7))
        s3 = Transform(scale=(sqrt(2) / 2, pi))

        self.assertFalse(Transform(translate=1e-9).is_scale(exactly=True))
        self.assertFalse(Transform(rotate=1e-9).is_scale(exactly=True))
        self.assertFalse(Transform(skewx=1e-9).is_scale(exactly=True))
        self.assertFalse(Transform(skewy=1e-9).is_scale(exactly=True))

        self.assertTrue(Transform(translate=1e-9).is_scale(exactly=False))
        self.assertTrue(Transform(rotate=1e-9).is_scale(exactly=False))
        self.assertTrue(Transform(skewx=1e-9).is_scale(exactly=False))
        self.assertTrue(Transform(skewy=1e-9).is_scale(exactly=False))

        self.assertFalse(s1.is_translate())
        self.assertFalse(s2.is_translate())
        self.assertFalse(s3.is_translate())
        self.assertFalse(s1.is_rotate())
        self.assertFalse(s2.is_rotate())
        self.assertFalse(s3.is_rotate())
        self.assertTrue(s1.is_scale())
        self.assertTrue(s2.is_scale())
        self.assertTrue(s3.is_scale())

    def test_rotation_degrees(self):
        """Test parsing and composition of different rotations"""
        self.assertAlmostEqual(Transform(rotate=30).rotation_degrees(), 30)
        self.assertAlmostEqual(Transform(translate=(10, 20)).rotation_degrees(), 0)
        self.assertAlmostEqual(Transform(scale=(1, 1)).rotation_degrees(), 0)

        self.assertAlmostEqual(
            Transform(rotate=35, translate=(10, 20)).rotation_degrees(), 35
        )
        self.assertAlmostEqual(
            Transform(rotate=35, translate=(10, 20), scale=5).rotation_degrees(), 35
        )
        self.assertAlmostEqual(
            Transform(rotate=35, translate=(10, 20), scale=(5, 5)).rotation_degrees(),
            35,
        )

        def rotation_degrees(**kwargs):
            return Transform(**kwargs).rotation_degrees()

        self.assertRaises(ValueError, rotation_degrees, rotate=35, skewx=1)
        self.assertRaises(ValueError, rotation_degrees, rotate=35, skewy=1)
        self.assertRaises(ValueError, rotation_degrees, rotate=35, scale=(10, 11))
        self.assertRaises(ValueError, rotation_degrees, rotate=35, scale=(10, 11))

    def test_construction_order(self):
        """Test transform kwargs construction order"""
        if not PY3:
            self.skipTest("Construction order is known to fail on python2 (by design).")
            return

        self.assertEqual(
            str(Transform(scale=2.0, translate=(5, 6))), "matrix(2 0 0 2 5 6)"
        )
        self.assertEqual(
            str(Transform(scale=2.0, rotate=45)),
            "matrix(1.41421 1.41421 -1.41421 1.41421 0 0)",
        )

        x, y, angle = 5, 7, 31
        rotation = Transform(rotate=angle)
        translation = Transform(translate=(x, y))

        rotation_then_translation = translation @ rotation
        translation_then_rotation = rotation @ translation

        tr1 = Transform(rotate=angle, translate=(x, y))
        tr2 = Transform(translate=(x, y), rotate=angle)

        self.assertNotEqual(tr1, tr2)
        self.assertDeepAlmostEqual(tr1.matrix, rotation_then_translation.matrix)
        self.assertDeepAlmostEqual(tr2.matrix, translation_then_rotation.matrix)

    def test_interpolate(self):
        """Test interpolate with other transform"""
        t1 = Transform((0, 0, 0, 0, 0, 0))
        t2 = Transform((1, 1, 1, 1, 1, 1))
        val = t1.interpolate(t2, 0.5)
        assert all(getattr(val, a) == pytest.approx(0.5, 1e-3) for a in "abcdef")


class ScaleTest(TestCase):
    """Test scale class"""

    def test_creation(self):
        """Creating scales"""
        self.assertEqual(BoundingInterval(0, 0), (0, 0))
        self.assertEqual(BoundingInterval(1), (1, 1))
        self.assertEqual(BoundingInterval(10), (10, 10))
        self.assertEqual(BoundingInterval(10, 20), (10, 20))
        self.assertEqual(BoundingInterval((2, 50)), (2, 50))
        self.assertEqual(repr(BoundingInterval((5, 10))), "BoundingInterval(5, 10)")

    def test_center(self):
        """Center of a scale"""
        self.assertEqual(BoundingInterval(0, 0).center, 0)
        self.assertEqual(BoundingInterval(0, 10).center, 5)
        self.assertEqual(BoundingInterval(-10, 10).center, 0)

    def test_neg(self):
        """-Span(...)"""
        self.assertEqual(tuple(-BoundingInterval(-10, 10)), (-10, 10))
        self.assertEqual(tuple(-BoundingInterval(-15, 2)), (-2, 15))
        self.assertEqual(tuple(-BoundingInterval(100, 110)), (-110, -100))
        self.assertEqual(tuple(-BoundingInterval(-110, -100)), (100, 110))

    def test_size(self):
        """Size of the scale"""
        self.assertEqual(BoundingInterval(0, 0).size, 0)
        self.assertEqual(BoundingInterval(10, 30).size, 20)
        self.assertEqual(BoundingInterval(-10, 10).size, 20)
        self.assertEqual(BoundingInterval(-30, -10).size, 20)

    def test_combine(self):
        """Combine scales together"""
        self.assertEqual(BoundingInterval(9, 10) + BoundingInterval(4, 5), (4, 10))
        self.assertEqual(
            sum([BoundingInterval(4), BoundingInterval(3), BoundingInterval(10)], None),
            (3, 10),
        )
        self.assertEqual(BoundingInterval(2, 2) * 2, (4, 4))

    def test_errors(self):
        """Expected errors"""
        self.assertRaises(ValueError, BoundingInterval, "foo")


class BoundingBoxTest(TestCase):
    """Test bounding box calculations"""

    def test_bbox(self):
        """Creating bounding boxes"""
        self.assertEqual(tuple(BoundingBox(1, 3)), ((1, 1), (3, 3)))
        self.assertEqual(tuple(BoundingBox((1, 2), 3)), ((1, 2), (3, 3)))
        self.assertEqual(tuple(BoundingBox(1, (3, 4))), ((1, 1), (3, 4)))
        self.assertEqual(tuple(BoundingBox((1, 2), (3, 4))), ((1, 2), (3, 4)))
        self.assertEqual(
            repr(BoundingBox((1, 2), (3, 4))), "BoundingBox((1, 2),(3, 4))"
        )

    def test_bbox_sum(self):
        """Test adding bboxes together"""
        self.assertEqual(
            tuple(BoundingBox((0, 10), (0, 10)) + BoundingBox((-10, 0), (-10, 0))),
            ((-10, 10), (-10, 10)),
        )
        ret = sum(
            [
                BoundingBox((-5, 0), (0, 0)),
                BoundingBox((0, 5), (0, 0)),
                BoundingBox((0, 0), (-5, 0)),
                BoundingBox((0, 0), (0, 5)),
            ],
            None,
        )
        self.assertEqual(tuple(ret), ((-5, 5), (-5, 5)))
        self.assertEqual(tuple(BoundingBox(-10, 2) + ret), ((-10, 5), (-5, 5)))
        self.assertEqual(tuple(ret + BoundingBox(1, -10)), ((-5, 5), (-10, 5)))

    def test_bbox_neg(self):
        self.assertEqual(tuple(-BoundingBox(-10, 2)), ((10, 10), (-2, -2)))
        self.assertEqual(
            tuple(-BoundingBox((-10, 15), (2, 10))), ((-15, 10), (-10, -2))
        )

    def test_bbox_scale(self):
        """Bounding Boxes can be scaled"""
        self.assertEqual(tuple(BoundingBox(1, 3) * 2), ((2, 2), (6, 6)))

    def test_bbox_area(self):
        self.assertEqual(BoundingBox((-3, 7), (-5, 5)).area, 100)

    def test_bbox_anchor_left_right(self):
        """Bunding box anchoring (left to right)"""
        bbox = BoundingBox((-1, 1), (10, 20))
        self.assertEqual(
            [
                bbox.get_anchor("l", "t", "lr"),
                bbox.get_anchor("m", "t", "lr"),
                bbox.get_anchor("r", "t", "lr"),
                bbox.get_anchor("l", "t", "rl"),
                bbox.get_anchor("m", "t", "rl"),
                bbox.get_anchor("r", "t", "rl"),
            ],
            [-1, 0.0, 1, 1, -0.0, -1],
        )

    def test_bbox_anchor_top_bottom(self):
        """Bunding box anchoring (top to bottom)"""
        bbox = BoundingBox((10, 20), (-1, 1))
        self.assertEqual(
            [
                bbox.get_anchor("l", "t", "tb"),
                bbox.get_anchor("l", "m", "tb"),
                bbox.get_anchor("l", "b", "tb"),
                bbox.get_anchor("l", "t", "bt"),
                bbox.get_anchor("l", "m", "bt"),
                bbox.get_anchor("l", "b", "bt"),
            ],
            [-1, 0.0, 1, 1, -0.0, -1],
        )

    def test_bbox_anchor_custom(self):
        """Bounding box anchoring custom angle"""
        bbox = BoundingBox((10, 10), (5, 5))
        self.assertEqual(
            [
                bbox.get_anchor("l", "t", 0),
                bbox.get_anchor("l", "t", 90),
                bbox.get_anchor("l", "t", 180),
                bbox.get_anchor("l", "t", 270),
                bbox.get_anchor("l", "t", 45),
            ],
            [10, -5, -10, 5, 3.5355339059327378],
        )

    def test_bbox_anchor_radial(self):
        """Bounding box anchoring radial in/out"""
        bbox = BoundingBox((10, 10), (5, 5))
        self.assertRaises(ValueError, bbox.get_anchor, "m", "m", "ro")
        selbox = BoundingBox((100, 100), (100, 100))
        self.assertEqual(int(bbox.get_anchor("m", "m", "ro", selbox)), 130)


class SegmentTest(TestCase):
    """Test special Segments"""

    def test_segment_creation(self):
        """Test segments"""
        self.assertEqual(DirectedLineSegment((1, 2), (3, 4)), (1, 3, 2, 4))
        self.assertEqual(
            repr(DirectedLineSegment((1, 2), (3, 4))),
            "DirectedLineSegment((1, 2), (3, 4))",
        )

    def test_segment_maths(self):
        """Segments have calculations"""
        self.assertEqual(DirectedLineSegment((0, 0), (10, 0)).angle, 0)
        self.assertAlmostEqual(
            DirectedLineSegment((0, 0), (0.5 * sqrt(3), 0.5)).angle, pi / 6, delta=1e-6
        )

    def test_segment_dx(self):
        """Test segment dx calculation"""
        self.assertEqual(DirectedLineSegment((0, 0), (0, 0)).dx, 0)
        self.assertEqual(DirectedLineSegment((0, 0), (0, 3)).dx, 0)
        self.assertEqual(DirectedLineSegment((0, 0), (3, 0)).dx, 3)
        self.assertEqual(DirectedLineSegment((0, 0), (-3, 0)).dx, -3)
        self.assertEqual(DirectedLineSegment((5, 0), (1, 0)).dx, -4)
        self.assertEqual(DirectedLineSegment((-3, 0), (1, 0)).dx, 4)

    def test_segment_dy(self):
        """Test segment dy calculation"""
        self.assertEqual(DirectedLineSegment((0, 0), (0, 0)).dy, 0)
        self.assertEqual(DirectedLineSegment((0, 0), (3, 0)).dy, 0)
        self.assertEqual(DirectedLineSegment((0, 0), (0, 3)).dy, 3)
        self.assertEqual(DirectedLineSegment((0, 0), (0, -3)).dy, -3)
        self.assertEqual(DirectedLineSegment((0, 5), (0, 1)).dy, -4)
        self.assertEqual(DirectedLineSegment((0, -3), (0, 1)).dy, 4)

    def test_segment_vector(self):
        """Test segment delta vector"""
        self.assertEqual(DirectedLineSegment((0, 0), (2, 3)).vector.to_tuple(), (2, 3))
        self.assertEqual(
            DirectedLineSegment((-2, -3), (2, 3)).vector.to_tuple(), (4, 6)
        )

    def test_segment_length(self):
        """Test segment length calculation"""
        self.assertEqual(DirectedLineSegment((0, 0), (0, 0)).length, 0)
        self.assertEqual(DirectedLineSegment((0, 0), (3, 0)).length, 3)
        self.assertEqual(DirectedLineSegment((0, 0), (-3, 0)).length, 3)
        self.assertEqual(DirectedLineSegment((0, 0), (0, 5)).length, 5)
        self.assertEqual(DirectedLineSegment((0, 0), (0, -5)).length, 5)
        self.assertEqual(DirectedLineSegment((2, 0), (0, 0)).length, 2)
        self.assertEqual(DirectedLineSegment((-2, 0), (0, 0)).length, 2)
        self.assertEqual(DirectedLineSegment((0, 4), (0, 0)).length, 4)
        self.assertEqual(DirectedLineSegment((0, -4), (0, 0)).length, 4)
        self.assertEqual(DirectedLineSegment((0, 0), (3, 4)).length, 5)
        self.assertEqual(DirectedLineSegment((-3, -4), (0, 0)).length, 5)

    def test_segment_angle(self):
        """Test segment angle calculation"""
        self.assertEqual(DirectedLineSegment((0, 0), (3, 0)).angle, 0)
        self.assertEqual(DirectedLineSegment((0, 0), (-3, 0)).angle, pi)
        self.assertEqual(DirectedLineSegment((0, 0), (0, 5)).angle, pi / 2)
        self.assertEqual(DirectedLineSegment((0, 0), (0, -5)).angle, -pi / 2)
        self.assertEqual(DirectedLineSegment((2, 0), (0, 0)).angle, pi)
        self.assertEqual(DirectedLineSegment((-2, 0), (0, 0)).angle, 0)
        self.assertEqual(DirectedLineSegment((0, 4), (0, 0)).angle, -pi / 2)
        self.assertEqual(DirectedLineSegment((0, -4), (0, 0)).angle, pi / 2)
        self.assertEqual(DirectedLineSegment((0, 0), (1, 1)).angle, pi / 4)
        self.assertEqual(DirectedLineSegment((0, 0), (-1, 1)).angle, 3 * pi / 4)
        self.assertEqual(DirectedLineSegment((0, 0), (-1, -1)).angle, -3 * pi / 4)
        self.assertEqual(DirectedLineSegment((0, 0), (1, -1)).angle, -pi / 4)


class ExtremaTest(TestCase):
    """Test school formula implementation"""

    def test_cubic_extrema_1(self):
        from inkex.transforms import cubic_extrema

        a, b, c, d = (
            14.644651000000003194,
            -4.881549508464541276,
            -4.8815495084645448287,
            14.644651000000003194,
        )
        cmin, cmax = cubic_extrema(a, b, c, d)
        self.assertAlmostEqual(cmin, 0, delta=1e-6)
        self.assertAlmostEqual(cmax, a, delta=1e-6)

    def test_quadratic_extrema_1(self):
        from inkex.transforms import quadratic_extrema

        a, b = 5.0, 12.0
        cmin, cmax = quadratic_extrema(a, b, a)
        self.assertAlmostEqual(cmin, 5, delta=1e-6)
        self.assertAlmostEqual(cmax, 8.5, delta=1e-6)

    def test_quadratic_extrema_2(self):
        from inkex.transforms import quadratic_extrema

        a = 5.0
        cmin, cmax = quadratic_extrema(a, a, a)
        self.assertAlmostEqual(cmin, a, delta=1e-6)
        self.assertAlmostEqual(cmax, a, delta=1e-6)

    def test_numpy_conversion(self):
        """Conversion to numpy"""

        arr = [1 + 2j, Vector2d(2, 3)]
        npa = np.array(arr)
        assert npa.dtype == np.complex128
        assert npa[0] == 1 + 2j
        assert npa[1] == 2 + 3j
