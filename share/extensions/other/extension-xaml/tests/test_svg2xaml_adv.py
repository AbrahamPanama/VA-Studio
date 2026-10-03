from math import sqrt

import inkex
from .context import Svg2XamlTester

"""More SVG2XAML custom tests"""


class XamlAdvancedPrimitiveTests(Svg2XamlTester):
    """Here we test more advanced primitives:
    - masks and clips
    - patterns
    - markers"""

    def test_clip_single(self):
        """Test clips consisting of a single element, applied to a single object."""
        dwg = self.run_to_dg("clip_single.svg")
        # For the first three tests, empty clip paths are provided. These can be
        # simplified into an empty string.
        for i in range(3):
            # Clips can only be specified on DrawingGroups.
            self.assertTagEqual(dwg[i], "DrawingGroup")
            self.assertTagEqual(dwg[i][0], "DrawingGroup")
            self.assertPropertyEqual(dwg[i][0], "ClipGeometry", "")

        # The next test has an invalid clip url set.
        # This means we don't even need a wrapper.
        self.assertTagEqual(dwg[3], "DrawingGroup")
        self.assertTagEqual(dwg[3][0], "GeometryDrawing")
        self.assertPropertyEqual(dwg[3], "ClipGeometry", None, default=None)

        # The next five clips specify different fill/stroke information, including
        # none and zero opacity, proving that this has no effect on the clip.

        for i in range(4, 9):
            self.assertTagEqual(dwg[i], "DrawingGroup")
            self.assertTagEqual(dwg[i][0], "DrawingGroup")
            self.assertTagEqual(dwg[i][0][1], "DrawingGroup.ClipGeometry")
            # We don't need a geometry group, as the clips only have one child
            self.assertTagEqual(dwg[i][0][1][0], "RectangleGeometry")
            self.assertPropertyEqual(dwg[i][0][1][0], "Rect", [40, 40, 20, 20])

    def test_clip_single_canvas(self):
        """Test clips consisting of a single element, applied to a single object,
        for canvas-type output"""
        canvas = self.run_to_canvas("clip_single.svg")
        # For the first three tests, empty clip paths are provided. These can be
        # simplified into an empty string.
        for i in range(3):
            self.assertTagEqual(canvas[i], "Canvas")
            self.assertTagEqual(canvas[i][0], "Rectangle")
            self.assertPropertyEqual(canvas[i][0], "Clip", "")

        # The next test has an invalid clip url set.
        self.assertTagEqual(canvas[3], "Canvas")
        self.assertTagEqual(canvas[3][0], "Rectangle")
        self.assertPropertyEqual(canvas[3][0], "ClipGeometry", None, default=None)

        # The next five clips specify different fill/stroke information, including
        # none and zero opacity, proving that this has no effect on the clip.
        for i in range(4, 9):
            self.assertTagEqual(canvas[i], "Canvas")
            self.assertTagEqual(canvas[i][0], "Rectangle")
            self.assertTagEqual(canvas[i][0][0], "Rectangle.Clip")
            # We don't need a geometry group, as the clips only have one child
            self.assertTagEqual(canvas[i][0][0][0], "RectangleGeometry")
            self.assertPropertyEqual(canvas[i][0][0][0], "Rect", [40, 40, 20, 20])

    def test_clip_multiple(self):
        """Test that a single clip applied to multiple objects works;
        the clip is applied to a group and a rectangle, and itself consists of
        two rectangles (which are unioned internally)."""

        dwg = self.run_to_dg("clip_complex.svg")
        for i in [0, 1]:
            self.assertTagEqual(dwg[i], "DrawingGroup")
            self.assertTagEqual(dwg[i][-1], "DrawingGroup.ClipGeometry")
            self.assertTagEqual(dwg[i][-1][0], "GeometryGroup")
            self.assertPropertyEqual(dwg[i][-1][0], "FillRule", "Nonzero")
            self.assertEqual(len(dwg[i][-1][0]), 2)  # The clip has two children
            self.assertTagEqual(dwg[i][-1][0][0], "RectangleGeometry")
            self.assertTagEqual(dwg[i][-1][0][1], "RectangleGeometry")
            self.assertPropertyEqual(dwg[i][-1][0][0], "Rect", [90, 50, 175, 200])
            self.assertPropertyEqual(dwg[i][-1][0][1], "Rect", [225, 160, 95, 75])
        self.assertTagEqual(dwg[0][0], "GeometryDrawing")
        self.assertTagEqual(dwg[1][0], "GeometryDrawing")
        self.assertTagEqual(dwg[1][1], "GeometryDrawing")

    def test_clip_multiple_canvas(self):
        """Test that a single clip applied to multiple objects works;
        the clip is applied to a group and a rectangle, and itself consists of
        two rectangles (which are unioned internally); now for a canvas"""

        dwg = self.run_to_canvas("clip_complex.svg")

        self.assertTagEqual(dwg[0], "Rectangle")
        self.assertTagEqual(dwg[1], "Canvas")

        self.assertTagEqual(dwg[0][-1], "Rectangle.Clip")
        self.assertTagEqual(dwg[1][-1], "Canvas.Clip")
        for i in [0, 1]:
            self.assertTagEqual(dwg[i][-1][0], "GeometryGroup")
            self.assertPropertyEqual(dwg[i][-1][0], "FillRule", "Nonzero")
            self.assertEqual(len(dwg[i][-1][0]), 2)  # The clip has two children
            self.assertTagEqual(dwg[i][-1][0][0], "RectangleGeometry")
            self.assertTagEqual(dwg[i][-1][0][1], "RectangleGeometry")
            self.assertPropertyEqual(dwg[i][-1][0][0], "Rect", [90, 50, 175, 200])
            self.assertPropertyEqual(dwg[i][-1][0][1], "Rect", [225, 160, 95, 75])
        self.assertTagEqual(dwg[1][0], "Rectangle")
        self.assertTagEqual(dwg[1][1], "Rectangle")

    def test_cliprule(self):
        """Test that the clip-rule attribute of a clippath is converted to fill-rule."""

        dwg = self.run_to_dg("clip_complex.svg")
        for i in [2, 3]:
            self.assertTagEqual(dwg[i], "DrawingGroup")
            self.assertPropertyEqual(dwg[i], "Transform", [1, 0, 0, 1, 150, 0])
            self.assertTagEqual(dwg[i][-1], "DrawingGroup.ClipGeometry")
            self.assertTagEqual(dwg[i][-1][0], "PathGeometry")
            self.assertPropertyEqual(
                dwg[i][-1][0],
                "FillRule",
                "EvenOdd" if i == 2 else "Nonzero",
                default="EvenOdd",
            )
            self.assertTagEqual(dwg[i][0], "GeometryDrawing")

        dwg = self.run_to_canvas("clip_complex.svg")
        for i in [2, 3]:
            self.assertTagEqual(dwg[i], "Rectangle")
            self.assertPropertyEqual(dwg[i], "RenderTransform", [1, 0, 0, 1, 150, 0])
            self.assertTagEqual(dwg[i][-1], "Rectangle.Clip")
            self.assertTagEqual(dwg[i][-1][0], "PathGeometry")
            self.assertPropertyEqual(
                dwg[i][-1][0],
                "FillRule",
                "EvenOdd" if i == 2 else "Nonzero",
                default="EvenOdd",
            )
            self.assertPropertyEqual(
                dwg[i][-1][0],
                "Transform",
                [
                    1,
                    0,
                    0,
                    1,
                    -float(dwg[i].get("Canvas.Left")),
                    -float(dwg[i].get("Canvas.Top")),
                ],
            )

    def test_opacitymask(self):
        """Test opacity masks."""
        # Objects 2 and 3 have an opacity mask consisting of 1 element applied;
        # the first is a linear gradient and the second is a solid brush.
        # Object 4 has an empty mask, and is clipped away completely.
        dwg = self.run_to_dg("masks.svg")
        canvas = self.run_to_canvas("masks.svg")
        for dgg in [dwg, canvas]:
            for i in [1, 2]:
                opm = dgg[i][1 if dwg == dgg else 0]
                if dgg == dwg:
                    self.assertTagEqual(
                        opm,
                        "DrawingGroup.OpacityMask"
                        if dgg == dwg
                        else "Rectangle.OpacityMask",
                    )
                self.assertTagEqual(opm[0], "DrawingBrush")
                self.assertTagEqual(opm[0][0], "DrawingBrush.Drawing")
                self.assertPropertyEqual(opm[0], "ViewboxUnits", "Absolute")
                self.assertPropertyEqual(opm[0], "ViewportUnits", "Absolute")
                vb = (
                    [60, 50, 100, 60]
                    if i == 1
                    else ([60, 120, 100, 30] if i == 2 else [0, 0, 0, 0])
                )
                self.assertPropertyEqual(opm[0], "Viewbox", vb, separator=" ")
                self.assertPropertyEqual(opm[0], "Viewport", vb, separator=" ")
                if dgg == canvas and i < 3:
                    self.assertPropertyEqual(
                        opm[0],
                        "Transform",
                        [1, 0, 0, 1, -60, -120] if i == 2 else [1, 0, 0, 1, -60, -50],
                    )
                # Two solutions are acceptable; optionally the GeometryDrawing can be
                # wrapped in a DrawingGroup.
                if i < 3:
                    if opm[0][0][0].tag == f"{{{self.xamlns}}}GeometryDrawing":
                        gdw = opm[0][0]
                    elif opm[0][0][0].tag == f"{{{self.xamlns}}}DrawingGroup":
                        gdw = opm[0][0][0][0]
                    self.assertTagEqual(gdw, "GeometryDrawing")
                    # The remainder is a standard group parser, so a cursory check is
                    # sufficient
                    if i == 1:
                        self.assertTagEqual(gdw[1][0], "RectangleGeometry")
                        self.assertTagEqual(gdw[0][0], "LinearGradientBrush")
                    if i == 2:
                        self.assertTagEqual(gdw[0][0], "RectangleGeometry")
                        self.assertPropertyEqual(gdw, "Brush", "#7fffffff")
                else:
                    if len(opm[0][0]) == 0:
                        # This would be ok
                        pass
                    if len(opm[0][0]) == 1:
                        self.assertTagEqual(opm[0][0][0], "DrawingGroup")
                        self.assertEqual(len(opm[0][0][0]), 0)

    def test_mask_noxywh(self):
        """Inkscape-created masks typically don't have x/y/width/height set"""
        dwg = self.run_to_dg("../svg/clips_and_masks.svg")
        mask = dwg[0][1][-1]
        self.assertTagEqual(mask, "DrawingGroup.OpacityMask")
        viewbox = mask[0].get("Viewbox")
        self.assertPropertyEqual(mask[0], "TileMode", "None", default="None")
        self.assertEqual(viewbox, mask[0].get("Viewport"))
        viewbox = list(map(float, viewbox.split()))
        # Tight bounding box
        self.assertLessEqual(viewbox[0], 119.103416442871)
        self.assertLessEqual(viewbox[1], 210.436676025391)
        self.assertGreaterEqual(viewbox[2], 51.0891952514648)
        self.assertGreaterEqual(viewbox[3], 53.5543518066406)

    def test_patterns(self):
        dwg = self.run_to_dg("patterns.svg")
        # Canvas works pretty much identical so we don't test it explicitly
        # The first pattern is absolute, but has a nonzero x / y value.
        # The first two elements reference the same pattern, but for the second one,
        # it's applied to the stroke.
        # The second pattern (third object) references the first one, but
        # applies an additional transform. This is representative of patterns created
        # in Inkscape. Apart from the transform, all should be identical.
        self.assertTagEqual(dwg[0][0], "GeometryDrawing.Brush")
        self.assertTagEqual(dwg[2][0], "GeometryDrawing.Brush")
        self.assertTagEqual(dwg[1][1], "GeometryDrawing.Pen")
        for i in (0, 1, 2):
            dbr = dwg[i][1 if i == 1 else 0][0]
            if i == 1:
                self.assertTagEqual(dbr, "Pen")
                self.assertPropertyEqual(dbr, "Thickness", 20)
                self.assertTagEqual(dbr[0], "Pen.Brush")
                dbr = dbr[0][0]
            self.assertTagEqual(dbr, "DrawingBrush")
            self.assertPropertyEqual(dbr, "TileMode", "Tile")
            self.assertPropertyEqual(dbr, "Viewbox", [10, 10, 20, 20], separator=" ")
            self.assertPropertyEqual(dbr, "Viewport", [10, 10, 20, 20], separator=" ")
            self.assertPropertyEqual(dbr, "ViewboxUnits", "Absolute")
            self.assertPropertyEqual(dbr, "ViewportUnits", "Absolute")
            self.assertTagEqual(dbr[0], "DrawingBrush.Drawing")
            self.assertTagEqual(dbr[0][0], "DrawingGroup")
            self.assertEqual(len(dbr[0][0]), 2)
            self.assertTagEqual(dbr[0][0][1], "GeometryDrawing")
            self.assertPropertyEqual(dbr[0][0], "Transform", [1, 0, 0, 1, 10, 10])
            self.assertPropertyEqual(dbr[0][0][0][0][0], "Rect", [5, 5, 10, 10])
            if i == 2:
                os2 = 1 / sqrt(2)
                self.assertPropertyEqual(
                    dbr,
                    "Transform",
                    (os2, os2, -os2, os2, 0, 0),
                )

        # The third pattern is defined in relative coordinates. The logic for this
        # is rather different than in SVG.
        dbr = dwg[3][0][0]
        self.assertTagEqual(dbr, "DrawingBrush")
        self.assertPropertyEqual(
            dbr,
            "Transform",
            (os2, os2, -os2, os2, 0, 0),
        )
        self.assertPropertyEqual(dbr, "TileMode", "Tile")
        self.assertPropertyEqual(dbr, "Viewbox", [0, 0, 0.1, 0.1], separator=" ")
        self.assertPropertyEqual(dbr, "Viewport", [0, 0, 0.1, 0.1], separator=" ")
        self.assertPropertyEqual(dbr, "ViewboxUnits", "Absolute")
        self.assertPropertyEqual(
            dbr,
            "ViewportUnits",
            "RelativeToBoundingBox",
            default="RelativeToBoundingBox",
        )
        self.assertTagEqual(dbr[0], "DrawingBrush.Drawing")
        self.assertTagEqual(dbr[0][0], "DrawingGroup")
        self.assertEqual(len(dbr[0][0]), 2)
        self.assertTagEqual(dbr[0][0][1], "GeometryDrawing")
        self.assertPropertyEqual(dbr[0][0], "Transform", [0.001, 0, 0, 0.001, 0, 0])
        self.assertPropertyEqual(dbr[0][0][0][0][0], "Rect", [0, 50, 50, 50])

    def test_markers(self):
        """Test marker export"""
        dwg = self.run_to_dg("markers_basic.svg")

        # There are four paths / four groups plus one global clip.
        self.assertEqual(len(dwg), 5)
        # Each group has four children, the path and three markers.
        for i in range(3):
            self.assertEqual(len(dwg[i]), 4)
            self.assertTagEqual(dwg[i][0], "GeometryDrawing")
            self.assertTagEqual(dwg[i][0][0][0], "PathGeometry")
            for j in range(1, 4):
                self.assertTagEqual(dwg[i][j], "DrawingGroup")
                self.assertTagEqual(dwg[i][j][0], "GeometryDrawing")
                self.assertTagEqual(dwg[i][j][0][0], "GeometryDrawing.Geometry")
                if i == 2:
                    self.assertTagEqual(dwg[i][j][0][0][0], "PathGeometry")
                if i == 0:
                    self.assertTagEqual(dwg[i][j][0][0][0], "RectangleGeometry")
                self.assertTagEqual(dwg[i][j][1], "DrawingGroup.ClipGeometry")
                self.assertTagEqual(dwg[i][j][1][0], "RectangleGeometry")
                self.assertPropertyEqual(dwg[i][j][1][0], "Rect", [0, 0, 10, 10])
            if i == 1:
                self.assertTagEqual(dwg[i][1][0][0][0], "RectangleGeometry")
                self.assertTagEqual(dwg[i][2][0][0][0], "EllipseGeometry")
                self.assertTagEqual(dwg[i][3][0][0][0], "PathGeometry")
        # Now for canvas.
        canvas = self.run_to_canvas("markers_basic.svg")
        # There are four paths / four groups plus one global clip.
        self.assertEqual(len(canvas), 5)
        # Each group has four children, the path and three markers.
        for i in range(3):
            self.assertEqual(len(canvas[i]), 4)
            self.assertTagEqual(canvas[i][0], "Path")
            self.assertTagEqual(canvas[i][0][0][0], "PathGeometry")
            for j in range(1, 4):
                self.assertTagEqual(canvas[i][j], "Canvas")
                if i == 2:
                    self.assertTagEqual(canvas[i][j][0], "Path")
                if i == 0:
                    self.assertTagEqual(canvas[i][j][0], "Rectangle")
                self.assertTagEqual(canvas[i][j][1], "Canvas.Clip")
                self.assertTagEqual(canvas[i][j][1][0], "RectangleGeometry")
                self.assertPropertyEqual(canvas[i][j][1][0], "Rect", [0, 0, 10, 10])
            if i == 1:
                self.assertTagEqual(canvas[i][1][0], "Rectangle")
                self.assertTagEqual(canvas[i][2][0], "Ellipse")
                self.assertTagEqual(canvas[i][3][0], "Path")
        for dgg in [dwg, canvas]:
            # Now we need to check the transforms.
            # first path
            trans_attr = "Transform" if dwg == dgg else "RenderTransform"
            for i, node in enumerate([(130, 40), (180, 40), (180, 90)]):
                p1m1 = inkex.Transform(
                    f"translate({node}) rotate(0) scale(8) translate(-1, -1) scale(0.2, 0.2)"
                )
                self.assertEqual(
                    p1m1, inkex.Transform(f"matrix({dgg[0][i+1].get(trans_attr)})")
                )
            for i, node in enumerate([(130, 135), (180, 135), (180, 185)]):
                p1m1 = inkex.Transform(
                    f"translate({node}) rotate(0) scale(8) translate(-1, -1) scale(0.2, 0.2)"
                )
                self.assertEqual(
                    p1m1, inkex.Transform(f"matrix({dgg[1][i+1].get(trans_attr)})")
                )
            for i, node in enumerate(
                [[(130, 230), 0], [(180, 230), 45], [(180, 280), 90]]
            ):
                p1m1 = inkex.Transform(
                    f"translate({node[0]}) rotate({node[1]}) scale(8) translate(-1, -1) scale(0.2, 0.2)"
                )
                self.assertEqual(
                    p1m1, inkex.Transform(f"matrix({dgg[2][i+1].get(trans_attr)})")
                )

    def test_markers_adv(self):
        """Test another marker. It doesn't have a viewbox, has display=none,
        is only set on the middle node and userSpaceOnUse"""
        dwg = self.run_to_dg("markers_basic.svg")
        # code mostly copied from above
        i = 3
        j = 1
        self.assertTagEqual(dwg[i][0], "GeometryDrawing")
        self.assertTagEqual(dwg[i][0][0][0], "PathGeometry")
        self.assertEqual(len(dwg[i]), 2)
        self.assertTagEqual(dwg[i][j], "DrawingGroup")
        self.assertTagEqual(dwg[i][j][0], "GeometryDrawing")
        self.assertTagEqual(dwg[i][j][0][0], "GeometryDrawing.Geometry")
        self.assertTagEqual(dwg[i][j][0][0][0], "PathGeometry")
        self.assertTagEqual(dwg[i][j][1], "DrawingGroup.ClipGeometry")
        self.assertTagEqual(dwg[i][j][1][0], "RectangleGeometry")
        self.assertPropertyEqual(dwg[i][j][1][0], "Rect", [0, 0, 10, 10])
        p1m1 = inkex.Transform(
            f"translate(325 50) rotate(0) translate(0, 0) scale(1, 1)"
        )
        self.assertEqual(p1m1, inkex.Transform(f'matrix({dwg[i][j].get("Transform")})'))

    def test_paintorder(self):
        """Test paint order, which we can only emulate by splitting up the objects"""

        dgg = self.run_to_dg("paint-order.svg")
        canvas = self.run_to_canvas("paint-order.svg")
        for dwg in [dgg, canvas]:
            self.assertEqual(len(dwg), 9)  # 7 objects + one clip
            for i in range(7):
                # The first 6 have markers, so they need to be a group.
                # The 7th also, because its stroke is painted before its fill.
                self.assertTagEqual(dwg[i], "DrawingGroup" if dwg == dgg else "Canvas")
            # The last one has "fill markers stroke", but since no markers are
            # set, it can be drawn as one.
            self.assertTagEqual(dwg[7], "GeometryDrawing" if dwg == dgg else "Path")

            # The first group has a drawing with fill and stroke and three markers.
            def assertMarkers(dwg, idx, start):
                for i in range(start, start + 3):
                    if dwg == dgg:
                        self.assertTagEqual(dwg[idx][i], "DrawingGroup")
                        self.assertPropertyEqual(
                            dwg[idx][i][0][0][0], "Rect", [0, 0, 10, 10]
                        )
                    else:
                        self.assertTagEqual(dwg[idx][i], "Canvas")
                        self.assertPropertyEqual(dwg[idx][i][0], "Canvas.Left", 0)
                        self.assertPropertyEqual(dwg[idx][i][0], "Width", 10)

            def assertStrokeFill(dwg, idx, fillidx, strokeidx, markeridx):
                self.assertEqual(
                    len(dwg[idx]),
                    (5 if strokeidx != fillidx else 4) - (0 if markeridx >= 0 else 3),
                )
                self.assertTagEqual(
                    dwg[idx][strokeidx], "GeometryDrawing" if dwg == dgg else "Path"
                )
                self.assertTagEqual(
                    dwg[idx][fillidx], "GeometryDrawing" if dwg == dgg else "Path"
                )
                if dwg == dgg:
                    if fillidx != strokeidx:
                        self.assertPropertyEqual(
                            dwg[idx][strokeidx], "Brush", None, default=None
                        )
                    self.assertPropertyEqual(
                        dwg[idx][strokeidx][1][0], "Brush", "#ff000000"
                    )
                    self.assertPropertyEqual(dwg[idx][fillidx], "Brush", "#7fff0000")
                else:
                    if fillidx != strokeidx:
                        self.assertPropertyEqual(
                            dwg[idx][strokeidx], "Fill", None, default=None
                        )
                    self.assertPropertyEqual(dwg[idx][strokeidx], "Stroke", "#ff000000")
                    self.assertPropertyEqual(dwg[idx][fillidx], "Fill", "#7fff0000")
                self.assertEqual(
                    dwg[idx][fillidx][0][0].get("Figures"),
                    dwg[idx][strokeidx][0][0].get("Figures"),
                )
                if markeridx != -1:
                    assertMarkers(dwg, idx, markeridx)

            assertStrokeFill(dwg, 0, 0, 0, 1)
            # The second group has stroke fill markers.
            assertStrokeFill(dwg, 1, 1, 0, 2)
            # The third group has fill markers stroke.
            assertStrokeFill(dwg, 2, 0, 4, 1)
            # The fourth group has markers fill stroke.
            assertStrokeFill(dwg, 3, 3, 3, 0)
            # The fifth group has stroke markers fill.
            assertStrokeFill(dwg, 4, 4, 0, 1)
            # The sixth group has markers stroke fill.
            assertStrokeFill(dwg, 5, 4, 3, 0)
            # The seventh group doesn't have markers, but stroke before fill
            assertStrokeFill(dwg, 6, 1, 0, -1)
        # The last group has fill markers stroke, and can be painted in one
        # because there are no markers.
        self.assertTagEqual(dgg[7], "GeometryDrawing")
        self.assertPropertyEqual(dgg[7][1][0], "Brush", "#ff000000")
        self.assertPropertyEqual(dgg[7], "Brush", "#7fff0000")

        self.assertTagEqual(canvas[7], "Path")
        self.assertPropertyEqual(canvas[7], "Stroke", "#ff000000")
        self.assertPropertyEqual(canvas[7], "Fill", "#7fff0000")

    def test_paint_order_rectangles(self):
        """For objects that are defined by top/left/width/height, the coordinates
        are different for the fill-object"""
        canvas = self.run_to_canvas("../svg/guides.svg")
        # Group of the last ellipse, which has stroke fill paint order and
        # half-transparent fill
        el_group = canvas[1][-1]
        self.assertPropertyEqual(el_group[0], "Canvas.Left", 776)
        self.assertPropertyEqual(el_group[0], "Canvas.Top", 292)
        self.assertPropertyEqual(el_group[0], "Width", 132)
        self.assertPropertyEqual(el_group[0], "Height", 132)
        self.assertPropertyEqual(el_group[0], "Fill", None, default=None)
        self.assertPropertyEqual(el_group[0], "Stroke", "#ffa40000")
        self.assertPropertyEqual(el_group[0], "StrokeThickness", 32)

        self.assertPropertyEqual(el_group[1], "Canvas.Left", 792)
        self.assertPropertyEqual(el_group[1], "Canvas.Top", 308)
        self.assertPropertyEqual(el_group[1], "Width", 100)
        self.assertPropertyEqual(el_group[1], "Height", 100)
        self.assertPropertyEqual(el_group[1], "Fill", "#7ffcaf3e")
        self.assertPropertyEqual(el_group[1], "Stroke", None, default=None)
        self.assertPropertyEqual(el_group[1], "StrokeThickness", None, default=None)

    def test_swatches_color(self):
        """Test the swatch-treatment setting.
        With swatch-treatment=color, all brushes are defined inline."""

        for i in ["lowlevel", "canvas"]:
            dct = Svg2XamlTester.run_to_tree(
                self.data_file("svg", "swatches.svg"),
                ["--target=wpf", f"--mode={i}", "--swatch-treatment=color"],
            )
            brushes = dct.xpath("//xaml:SolidColorBrush", namespaces=self.namespaces)
            self.assertEqual(len(brushes), 0)
            dwg = dct.xpath(
                "//xaml:DrawingGroup" if i == "lowlevel" else "//xaml:Canvas",
                namespaces=self.namespaces,
            )[0][0]
            if i == "lowlevel":
                self.assertPropertyEqual(dwg[0], "Brush", "#ff000000")
                self.assertPropertyEqual(dwg[0][1][0], "Brush", "#ffff0000")
                self.assertPropertyEqual(dwg[1], "Brush", "#7f000000")
                self.assertPropertyEqual(dwg[1][1][0], "Brush", "#ffff0000")
                self.assertPropertyEqual(dwg[2], "Brush", "#ffff0000")
                self.assertPropertyEqual(dwg[2][1][0], "Brush", "#7f000000")
                self.assertPropertyEqual(dwg[3], "Brush", "#ffffffff")
            if i == "canvas":
                self.assertPropertyEqual(dwg[0], "Fill", "#ff000000")
                self.assertPropertyEqual(dwg[0], "Stroke", "#ffff0000")
                self.assertPropertyEqual(dwg[1], "Fill", "#7f000000")
                self.assertPropertyEqual(dwg[1], "Stroke", "#ffff0000")
                self.assertPropertyEqual(dwg[2], "Fill", "#ffff0000")
                self.assertPropertyEqual(dwg[2], "Stroke", "#7f000000")
                self.assertPropertyEqual(dwg[3], "Fill", "#ffffffff")

    def test_blur(self):
        # Test blur, only works for Canvas.
        canvas = self.run_to_canvas("blur.svg")
        # The first element has a blur with stdDev given by two (identical) values
        for i in range(2):
            self.assertTagEqual(canvas[i], "Rectangle")
            self.assertTagEqual(canvas[i][0], "Rectangle.Effect")
            self.assertTagEqual(canvas[i][0][0], "BlurEffect")
        self.assertPropertyEqual(canvas[0][0][0], "Radius", 15.5)
        # The second element has a single value, and is transformed (scale transform)
        self.assertPropertyEqual(canvas[1][0][0], "Radius", 217)
        # The third element is a blurred path
        self.assertTagEqual(canvas[2][1], "Path.Effect")
        self.assertPropertyEqual(canvas[2][1][0], "Radius", 9.3)
        # The fourth element is a blurred group
        self.assertTagEqual(canvas[3], "Canvas")
        self.assertTagEqual(canvas[3][2], "Canvas.Effect")
        self.assertTagEqual(canvas[3][2][0], "BlurEffect")
        self.assertPropertyEqual(canvas[3][2][0], "Radius", 21.7)


class SvgToXAMLOptionsTests(Svg2XamlTester):
    """Test the CLI options of the converter."""

    def test_swatches_resources(self):
        """Test the swatch-treatment setting.
        With swatch-treatment=StaticResource oder DynamicResource"""

        for i in ["lowlevel", "canvas"]:
            for restype in ["StaticResource", "DynamicResource"]:
                dct = Svg2XamlTester.run_to_tree(
                    self.data_file("svg", "swatches.svg"),
                    ["--target=wpf", f"--mode={i}", f"--swatch-treatment={restype}"],
                )
                brushes = dct.xpath(
                    "//xaml:SolidColorBrush", namespaces=self.namespaces
                )
                self.assertEqual(len(brushes), 3)
                # The order doesn't matter
                brushes = sorted(brushes, key=lambda x: x.get(self.KEY_ATTR))
                self.assertPropertyEqual(brushes[0], self.KEY_ATTR, "accent")
                self.assertPropertyEqual(brushes[0], "Color", "#ffff0000")
                self.assertPropertyEqual(brushes[1], self.KEY_ATTR, "background")
                self.assertPropertyEqual(brushes[1], "Color", "#ffffffff")
                # The third swatch has a stop-opacity set
                self.assertPropertyEqual(brushes[2], self.KEY_ATTR, "foreground")
                self.assertPropertyEqual(brushes[2], "Color", "#7f000000")
                dwg = dct.xpath(
                    "//xaml:DrawingGroup" if i == "lowlevel" else "//xaml:Canvas",
                    namespaces=self.namespaces,
                )[0][0]
                if i == "lowlevel":
                    self.assertPropertyEqual(dwg[0], "Brush", "#ff000000")
                    self.assertPropertyEqual(dwg[0][1][0], "Brush", "#ffff0000")
                    self.assertPropertyEqual(
                        dwg[1], "Brush", f"{{{restype} foreground}}"
                    )
                    self.assertPropertyEqual(
                        dwg[1][1][0], "Brush", f"{{{restype} accent}}"
                    )
                    self.assertPropertyEqual(dwg[2], "Brush", f"{{{restype} accent}}")
                    self.assertPropertyEqual(
                        dwg[2][1][0], "Brush", f"{{{restype} foreground}}"
                    )
                    self.assertPropertyEqual(
                        dwg[3], "Brush", f"{{{restype} background}}"
                    )
                if i == "canvas":
                    # The swatches should be children of the toplevel Viewbox ResourceDictionary
                    for brush in brushes:
                        self.assertTagEqual(
                            brush.getparent().getparent(), "Viewbox.Resources"
                        )
                    self.assertPropertyEqual(dwg[0], "Fill", "#ff000000")
                    self.assertPropertyEqual(dwg[0], "Stroke", "#ffff0000")
                    self.assertPropertyEqual(
                        dwg[1], "Fill", f"{{{restype} foreground}}"
                    )
                    self.assertPropertyEqual(dwg[1], "Stroke", f"{{{restype} accent}}")
                    self.assertPropertyEqual(dwg[2], "Fill", f"{{{restype} accent}}")
                    self.assertPropertyEqual(
                        dwg[2], "Stroke", f"{{{restype} foreground}}"
                    )
                    self.assertPropertyEqual(
                        dwg[3], "Fill", f"{{{restype} background}}"
                    )

    def test_swatches_canvas_layers(self):
        """The swatches should be in the Grid's resources. Otherwise, all stays the same."""
        dct = Svg2XamlTester.run_to_tree(
            self.data_file("svg", "swatches.svg"),
            [
                "--target=wpf",
                "--mode=canvas",
                "--swatch-treatment=StaticResource",
                "--layers-as-resources=True",
            ],
        )
        brushes = dct.xpath("//xaml:SolidColorBrush", namespaces=self.namespaces)
        self.assertEqual(len(brushes), 3)
        for brush in brushes:
            self.assertTagEqual(brush.getparent().getparent(), "Grid.Resources")

    def layer_basic_checks(self, dct, idd, name, len_children, type_of_first_child):
        """Check a few basic properties for lowlevel ResourceDict entries"""
        self.assertTagEqual(dct[2 * idd], "DrawingGroup")
        self.assertPropertyEqual(dct[2 * idd], self.KEY_ATTR, name)
        self.assertTagEqual(dct[2 * idd + 1], "DrawingImage")
        self.assertPropertyEqual(dct[2 * idd + 1], self.KEY_ATTR, "di_" + name)
        self.assertPropertyEqual(
            dct[2 * idd + 1], "Drawing", f"{{StaticResource {name}}}"
        )
        self.assertEqual(len(dct[2 * idd]), len_children)
        self.assertTagEqual(dct[2 * idd][0], type_of_first_child)

    def layer_canvas_checks(self, dct, idd, name, len_children, type_of_first_child):
        """Check a few basic properties for canvas ResourceDict entries"""
        self.assertTagEqual(dct[idd], "Viewbox")
        self.assertTagEqual(dct[idd][0], "Canvas")
        self.assertPropertyEqual(dct[idd], "Name", name)
        self.assertEqual(len(dct[idd][0]), len_children)
        self.assertEqual(len(dct[idd]), 1)
        self.assertTagEqual(dct[idd][0][0], type_of_first_child)

    def test_layers_false_lowlevel(self):
        """Layers as resources is switched off. Test that the resulting ResourceDict
        is well-formed."""
        dct = Svg2XamlTester.run_to_tree(
            self.data_file("svg", "layers.svg"),
            ["--target=wpf", "--mode=lowlevel", "--layers-as-resources=false"],
        )
        self.assertEqual(len(dct), 2)
        # For layers as resources=false, the last non-layer group
        # in the root group is not ignored. There should be 5 children + 1 clip.
        self.layer_basic_checks(dct, 0, "layers", 6, "DrawingGroup")
        # The second layer is set to be invisible.
        self.assertPropertyEqual(dct[0][1], "ClipGeometry", "", default=None)
        self.assertEqual(len(dct[1]), 0)
        # If no sodipodi:docname is unset, check that a random id gets set and
        # correctly referenced.
        dct = Svg2XamlTester.run_to_tree(
            self.data_file("svg", "rect_basic.svg"),
            ["--target=wpf", "--mode=lowlevel", "--layers-as-resources=false"],
        )
        key = dct[0].get(self.KEY_ATTR)
        self.assertIn("DrawingGroup_", key)
        self.layer_basic_checks(dct, 0, key, 5, "GeometryDrawing")

    def test_referencingtype(self):
        """Test DrawingBrush as referencing element"""
        dct = Svg2XamlTester.run_to_tree(
            self.data_file("svg", "layers.svg"),
            [
                "--target=wpf",
                "--mode=lowlevel",
                "--layers-as-resources=false",
                "--referencing-type=DrawingBrush",
            ],
        )
        self.assertEqual(len(dct), 2)
        self.assertTagEqual(dct[0], "DrawingGroup")
        self.assertPropertyEqual(dct[0], self.KEY_ATTR, "layers")
        self.assertTagEqual(dct[1], "DrawingBrush")
        self.assertPropertyEqual(dct[1], self.KEY_ATTR, "db_layers")
        self.assertPropertyEqual(dct[1], "Drawing", "{StaticResource layers}")
        self.assertPropertyEqual(dct[1], "Stretch", "Uniform")

    def test_layers_false_canvas(self):
        """Same test for canvas. In this case, the root element is a ViewBox,
        which does not need a name."""
        vbx = Svg2XamlTester.run_to_tree(
            self.data_file("svg", "layers.svg"),
            ["--target=wpf", "--mode=canvas", "--layers-as-resources=false"],
        )
        self.assertTagEqual(vbx, "Viewbox")
        self.assertTagEqual(vbx[0], "Canvas")
        self.assertPropertyEqual(vbx, "Name", "layers")
        self.assertEqual(len(vbx[0]), 6)
        self.assertEqual(len(vbx), 1)
        self.assertTagEqual(vbx[0][0], "Canvas")
        # The second subgroup is invisible.
        self.assertPropertyEqual(vbx[0][1], "Visibility", "Hidden")

    def test_layers_true_lowlevel(self):
        """Layers as resources is enabled. There are layers that are hidden,
        layers with duplicate names, sublayers and "trick layers" starting with di_.

        Test that the resulting ResourceDictionary is well-formed."""
        dct = Svg2XamlTester.run_to_tree(
            self.data_file("svg", "layers.svg"),
            ["--target=wpf", "--mode=lowlevel", "--layers-as-resources=true"],
        )
        # The first layer has display:none. For layers as resources, this is ignored.
        # There are also invalid characters in the key, that also starts with a number
        # (invalid). These get fixed.
        self.layer_basic_checks(
            dct, 0, "_0__Open_with______1_special_chars", 2, "GeometryDrawing"
        )
        # The second layer has a sublayer as first child. Only top level layers are
        # stored as resources. The second child is a normal rect
        self.layer_basic_checks(dct, 1, "Save", 3, "DrawingGroup")
        # The third layer has the same label as the second layer. A random number is
        # appended. There are two children.
        self.layer_basic_checks(dct, 2, "Save_595", 3, "GeometryDrawing")
        # The fourth layer's is "di_Save", which already exists from the second one.
        self.layer_basic_checks(dct, 3, "di_Save_874", 1, "DrawingGroup.ClipGeometry")
        # In total, there are four layers. The last group is ignored because it's not
        # a layer.
        self.assertEqual(len(dct), 8)
        # The ClipGeometry should be identical for all children.
        for i in range(4):
            self.assertTagEqual(dct[2 * i][-1], "DrawingGroup.ClipGeometry")
            self.assertTagEqual(dct[2 * i][-1][0], "RectangleGeometry")
            self.assertPropertyEqual(dct[2 * i][-1][0], "Rect", [0, 0, 480, 360])

    def test_layers_true_canvas(self):
        """Same test as above for canvas. In this case, the root element is a grid"""
        grid = Svg2XamlTester.run_to_tree(
            self.data_file("svg", "layers.svg"),
            ["--target=wpf", "--mode=canvas", "--layers-as-resources=true"],
        )
        self.assertTagEqual(grid, "Grid")
        # The first layer has display:none. For layers as resources, this is ignored.
        # There are also invalid characters in the key, that also starts with a number
        # (invalid). These get fixed.
        self.layer_canvas_checks(
            grid, 0, "_0__Open_with______1_special_chars", 2, "Rectangle"
        )
        # The second layer has a sublayer as first child. Only top level layers are
        # stored as resources. The second child is a normal rect
        self.layer_canvas_checks(grid, 1, "Save", 3, "Canvas")
        # The third layer has the same label as the second layer. A random number is
        # appended. There are two children.
        self.layer_canvas_checks(grid, 2, "Save_595", 3, "Ellipse")
        # The fourth layer's is "di_Save". This leads to collisions only for lowlevel.
        self.layer_canvas_checks(grid, 3, "di_Save", 1, "Canvas.Clip")
        # In total, there are four layers. The last group is ignored because it's not
        # a layer.
        try:
            self.assertEqual(len(grid), 4)
        except AssertionError:
            # There might be an empty resource dictionary.
            self.assertEqual(len(grid), 5)
            self.assertTagEqual(grid[4], "Grid.Resources")
            try:
                self.assertEqual(len(grid[4]), 0)
            except AssertionError:
                self.assertEqual(len(grid[4]), 1)
                self.assertTagEqual(grid[4][0], "ResourceDictionary")
                self.assertEqual(len(grid[4][0]), 0)
        # The Clip should be identical for all children.
        for i in range(4):
            self.assertPropertyEqual(grid[i][0], "Width", 480)
            self.assertPropertyEqual(grid[i][0], "Height", 360)
            self.assertTagEqual(grid[i][0][-1], "Canvas.Clip")
            self.assertTagEqual(grid[i][0][-1][0], "RectangleGeometry")
            self.assertPropertyEqual(grid[i][0][-1][0], "Rect", [0, 0, 480, 360])
