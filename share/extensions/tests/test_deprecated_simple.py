# coding=utf-8
"""Test deprecated-simple modules"""

from __future__ import absolute_import, print_function

import warnings
import math
import os
import re

from pytest import approx

import inkex
from inkex.tester import TestCase


class DeprecatedTest(TestCase):
    """Tests for Deprecated API (Inkscape 0.92 and below)"""

    def setUp(self):
        # All the functions in this test suite are deprecated, so
        # we don't need the warnings here.
        self.warner = warnings.catch_warnings()
        self.warner.__enter__()
        warnings.simplefilter("ignore", category=DeprecationWarning)

    def tearDown(self):
        self.warner.__exit__()

    def test_simple_imports(self):
        """Can import each module"""
        # TODO add tests for these modules
        import bezmisc
        import cspsubdiv
        import cubicsuperpath
        import ffgeom

        # pylint: disable=unused-variable
        from inkex import debug, errormsg, localize

    def test_simplepath(self):
        """Test simplepath API"""
        import simplepath

        data = "M12 34L56 78Z"
        path = simplepath.parsePath(data)
        self.assertEqual(path, [["M", [12.0, 34.0]], ["L", [56.0, 78.0]], ["Z", []]])

        d_out = simplepath.formatPath(path)
        d_out = d_out.replace(".0", "")
        self.assertEqual(data.replace(" ", ""), d_out.replace(" ", ""))

        simplepath.translatePath(path, -3, -4)
        self.assertEqual(path, [["M", [9.0, 30.0]], ["L", [53.0, 74.0]], ["Z", []]])

        simplepath.scalePath(path, 10, 20)
        self.assertEqual(
            path, [["M", [90.0, 600.0]], ["L", [530.0, 1480.0]], ["Z", []]]
        )

        simplepath.rotatePath(path, math.pi / 2.0, cx=5, cy=7)
        approxed = [[code, approx(coords)] for (code, coords) in path]
        self.assertEqual(
            approxed, [["M", [-588.0, 92.0]], ["L", [-1468.0, 532.0]], ["Z", []]]
        )

    def test_simplepath_shorthand(self):
        """simplepath with shorthand notation"""
        import simplepath

        data = (
            "M10 20v30V30h40H40c 1 2 3 4 5 6S7 8 9 10s7 8 9 10q11 12 13 14t15 16T15 16"
        )
        path = simplepath.parsePath(data)
        self.assertEqual(
            path,
            [
                ["M", [10.0, 20.0]],
                ["L", [10.0, 50.0]],
                ["L", [10.0, 30.0]],
                ["L", [50.0, 30.0]],
                ["L", [40.0, 30.0]],
                ["C", [41.0, 32.0, 43.0, 34.0, 45.0, 36.0]],
                ["C", [47.0, 38.0, 7.0, 8.0, 9.0, 10.0]],
                ["C", [11.0, 12.0, 16.0, 18.0, 18.0, 20.0]],
                ["Q", [29.0, 32.0, 31.0, 34.0]],
                ["Q", [33.0, 36.0, 46.0, 50.0]],
                ["Q", [59.0, 64.0, 15.0, 16.0]],
            ],
        )

    def test_simplestyle(self):
        """Test simplestyle API"""
        import simplestyle

        self.assertEqual(simplestyle.svgcolors["blue"], "#0000ff")
        self.assertEqual(
            simplestyle.parseStyle("foo: bar; abc-def: 123em"),
            {"foo": "bar", "abc-def": "123em"},
        )
        self.assertEqual(simplestyle.formatStyle({"foo": "bar"}), "foo:bar")
        self.assertTrue(simplestyle.isColor("#ff0000"))
        self.assertTrue(simplestyle.isColor("#f00"))
        self.assertTrue(simplestyle.isColor("blue"))
        self.assertFalse(simplestyle.isColor("none"))
        self.assertFalse(simplestyle.isColor("nosuchcolor"))
        self.assertEqual(simplestyle.parseColor("#0000ff"), (0, 0, 0xFF))
        self.assertEqual(simplestyle.parseColor("red"), (0xFF, 0, 0))
        self.assertEqual(simplestyle.formatColoria([0, 0x99, 0]), "#009900")
        self.assertEqual(simplestyle.formatColor3i(0, 0x99, 0), "#009900")
        self.assertEqual(simplestyle.formatColorfa([0, 1.0, 0]), "#00ff00")
        self.assertEqual(simplestyle.formatColor3f(0, 1.0, 0), "#00ff00")

    def test_simpletransform(self):
        """Test simpletransform API"""
        import simpletransform

        self.assertEqual(
            simpletransform.parseTransform("scale(10)"), [[10, 0, 0], [0, 10, 0]]
        )
        self.assertEqual(
            simpletransform.parseTransform("translate(2,3)"), [[1, 0, 2], [0, 1, 3]]
        )
        self.assertEqual(
            simpletransform.parseTransform("translate(2,3) rotate(90)"),
            [approx([0, -1, 2]), approx([1, 0, 3])],
        )
        m = simpletransform.formatTransform([[0, -1, 2], [1, 0, 3]])
        self.assertEqual(
            re.sub(r",", " ", re.sub(r"\.0*\b", "", m)), "matrix(0 1 -1 0 2 3)"
        )
        self.assertEqual(
            simpletransform.invertTransform([[1, 0, 2], [0, 1, 3]]),
            [[1, 0, -2], [0, 1, -3]],
        )
        self.assertEqual(
            simpletransform.composeTransform(
                [[1, 0, 2], [0, 1, 3]], [[0, -1, 0], [1, 0, 0]]
            ),
            [[0, -1, 2], [1, 0, 3]],
        )

        pt = [4, 5]
        self.assertEqual(
            simpletransform.applyTransformToPoint([[0, -1, 2], [1, 0, 3]], pt), None
        )
        self.assertEqual(pt, [-3, 7])

        self.assertEqual(
            simpletransform.boxunion([3, 5, 2, 4], [4, 6, 1, 3]), (3, 6, 1, 4)
        )
        self.assertEqual(simpletransform.cubicExtrema(1, 2, 3, 4), (1, 4))

        # TODO need cubic superpath
        self.assertTrue(simpletransform.applyTransformToPath)
        self.assertTrue(simpletransform.roughBBox)
        self.assertTrue(simpletransform.refinedBBox)

        # TODO need node
        self.assertTrue(simpletransform.fuseTransform)
        self.assertTrue(simpletransform.composeParents)
        self.assertTrue(simpletransform.applyTransformToNode)
        self.assertTrue(simpletransform.computeBBox)
        self.assertTrue(simpletransform.computePointInNode)

    def test_inkex_effect(self):
        """Test original Effect base class"""
        from inkex import Effect

        args = [
            "--id",
            "curve",
            os.path.join(os.path.dirname(__file__), "data", "svg/curves.svg"),
        ]

        e = Effect()
        e.affect(args)

        # assigned in __init__
        self.assertNotEqual(e.document.getroot(), None)
        self.assertTrue(isinstance(e.selected, dict))
        self.assertEqual(list(e.selected), ["curve"])
        self.assertTrue(isinstance(e.doc_ids, dict))
        self.assertTrue(isinstance(e.options.ids, list))
        self.assertEqual(e.args, args[-1:])
        self.assertNotEqual(e.OptionParser.add_option, None)

        # methods
        self.assertEqual(e.getselected(), None)
        node = e.getElementById("arc")
        self.assertEqual(node.tag, "{http://www.w3.org/2000/svg}path")
        self.assertEqual(node.get("id"), "arc")
        self.assertEqual(e.getParentNode(node).tag, "{http://www.w3.org/2000/svg}g")
        self.assertEqual(
            e.getNamedView().tag,
            "{http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd}namedview",
        )
        self.assertEqual(
            e.createGuide(10, 20, 45).tag,
            "{http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd}guide",
        )
        self.assertTrue(e.uniqueId("foo").startswith("foo"))
        self.assertEqual(
            e.xpathSingle("//svg:path").tag, "{http://www.w3.org/2000/svg}path"
        )
        self.assertEqual(e.getDocumentWidth(), "1000")
        self.assertEqual(e.getDocumentHeight(), "1000")
        self.assertEqual(e.getDocumentUnit(), "px")
        self.assertEqual(e.unittouu("1in"), 96)
        self.assertEqual(e.uutounit(192, "in"), 2)
        self.assertEqual(e.addDocumentUnit("3"), "3px")

        # skip:
        # - e.ctx
        # - e.getposinlayer
        # - e.original_document
