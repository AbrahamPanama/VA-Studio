"""Tests for Avalonia XAML export"""

from .context import Svg2XamlTester


class XamlAvaloniaTests(Svg2XamlTester):
    """We only test the things that are different from WPF XAML."""

    xamlns = "https://github.com/avaloniaui"
    namespaces = {
        "x": "http://schemas.microsoft.com/winfx/2006/xaml",
        "d": "http://schemas.microsoft.com/expression/blend/2008",
        "xaml": xamlns,
    }

    def run_to_dg(self, svg, args=None):
        return super().run_to_dg(
            svg, args=args or ["--target=avaloniaui", "--mode=lowlevel"]
        )

    def run_to_canvas(self, svg, args=None):
        return super().run_to_canvas(
            svg, args=args or ["--target=avaloniaui", "--mode=canvas"]
        )

    def test_rect_lowlevel(self):
        """Difference to WPF: there is no Radius for RectangleGeometry, and we have to
        manually clamp the radius"""
        dwg = self.run_to_dg("rect_basic.svg")
        self.assertEqual(len(dwg), 5)  # 4 rects + BoundingBox
        for i in range(2):
            self.assertTagEqual(dwg[i], "GeometryDrawing")
            self.assertTagEqual(dwg[i][0], "GeometryDrawing.Geometry")
            self.assertTagEqual(dwg[i][0][0], "RectangleGeometry")
        for i in [2, 3]:
            self.assertTagEqual(dwg[i], "GeometryDrawing")
            self.assertTagEqual(dwg[i][0], "GeometryDrawing.Geometry")
            self.assertTagEqual(dwg[i][0][0], "PathGeometry")
        # Check that radius clamping works properly
        self.assertPropertyEqual(
            dwg[2][0][0],
            "Figures",
            "M 375 46 L 375 46 A 25 30 0 0 1 400 76 L 400 96 A 25 30 0 0 1 375 126 L 375 126 A 25 30 0 0 1 350 96 L 350 76 A 25 30 0 0 1 375 46 z",
        )
        self.assertPropertyEqual(
            dwg[3][0][0],
            "Figures",
            "M 375 196 L 375 196 A 25 40 0 0 1 400 236 L 400 236 A 25 40 0 0 1 375 276 L 375 276 A 25 40 0 0 1 350 236 L 350 236 A 25 40 0 0 1 375 196 z",
        )

    def test_rect_canvas(self):
        """Also test the IsVisible property"""
        dwg = self.run_to_canvas("rect_defaults.svg")
        self.assertEqual(len(dwg), 2)  # they are wrapped in a subcanvas
        grp = dwg[0]
        self.assertEqual(len(dwg[0]), 6)
        self.assertPropertyEqual(grp[0], "Canvas.Left", -0.5, default=0.0)
        # default differs from WPF
        self.assertPropertyEqual(grp[0], "StrokeJoin", "Miter", default="Bevel")
        self.assertPropertyEqual(grp[1], "Canvas.Top", -0.5, default=0.0)
        # two rectangles have no visibility (w/h = 0)
        self.assertPropertyEqual(grp[2], "IsVisible", "False")
        self.assertPropertyEqual(grp[3], "IsVisible", "False")
        # check that the radius is symmetric if one attribute is missing
        # However, this is a path in Avalonia (note "A 20 20")
        self.assertPropertyEqual(
            grp[4][0][0],
            "Figures",
            "M 50 196 L 60 196 A 20 20 0 0 1 80 216 L 80 256 A 20 20 0 0 1 60 276 L 50 276 A 20 20 0 0 1 30 256 L 30 216 A 20 20 0 0 1 50 196 z",
        )
        self.assertPropertyEqual(grp[4][0][0], "FillRule", "NonZero")  # Nonzero in WPF
        self.assertPropertyEqual(
            grp[5][0][0],
            "Figures",
            "M 150 196 L 160 196 A 20 20 0 0 1 180 216 L 180 256 A 20 20 0 0 1 160 276 L 150 276 A 20 20 0 0 1 130 256 L 130 216 A 20 20 0 0 1 150 196 z",
        )
        self.assertPropertyEqual(grp[5][0][0], "FillRule", "NonZero")  # Nonzero in WPF
        # fill and stroke were inherited from the group
        self.assertPropertyEqual(grp[0], "Fill", "#ff008000")
        self.assertPropertyEqual(grp[0], "Stroke", "#ff000000")

    def test_radialgradient(self):
        """Radial gradients with ObjectBoundingBox have too much logical differences
        to SVG (and maybe the API will change in the future), and Inkscape isn't
        even using this. The output isn't ideal for now, so we don't test it."""
        dgg = self.run_to_dg("radialgradient_focal.svg")
        canvas = self.run_to_canvas("radialgradient_focal.svg")
        for dwg in [dgg, canvas]:
            # Next element: gradient is defined outside defs, has userSpaceOnUse,
            # and is also transformed. This does not affect the gradient though.
            # For canvas, the nesting depth is one lower than for lowlevel (which
            # does not support transforming RectangleGeometries)
            if dwg == dgg:
                br = dwg[0][2][0][0][0]
            else:
                br = dwg[0][2][0][0]
            self.assertTagEqual(br, "RadialGradientBrush")
            # Radius is relative to untransformed bounding box, but it's possible that
            # this will change in the future, see:
            # https://github.com/AvaloniaUI/Avalonia/blob/2737c5038755c251edcc8456650368a8b9043dae/src/Avalonia.Base/Media/RadialGradientBrush.cs#L62
            self.assertPropertyEqual(br, "Radius", 0.5)
            self.assertPropertyEqual(br, "Center", [25, 215])
            self.assertPropertyEqual(br, "GradientOrigin", [25, 215])
            if dwg == dgg:
                self.assertTagEqual(dwg[0][2][1], "DrawingGroup.Transform")
                self.assertTagEqual(dwg[0][2][1][0], "MatrixTransform")
                self.assertPropertyEqual(
                    dwg[0][2][1][0],
                    "Matrix",
                    (0, -1, 1, 0, 10, 260),
                )
            if dwg == canvas:
                # Comparison value is different because of Canvas.Left / strokewidth
                self.assertTagEqual(dwg[0][2][1], "Rectangle.RenderTransform")
                self.assertTagEqual(dwg[0][2][1][0], "MatrixTransform")
                self.assertPropertyEqual(dwg[0][2], "RenderTransformOrigin", [0, 0])
                self.assertPropertyEqual(
                    dwg[0][2][1][0], "Matrix", (0, -1, 1, 0, 10, 260.5)
                )

    def test_patterns(self):
        dwg = self.run_to_dg("patterns.svg")
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
            self.assertTagEqual(dbr, "VisualBrush")
            self.assertPropertyEqual(dbr, "TileMode", "Tile")
            self.assertPropertyEqual(dbr, "SourceRect", [10, 10, 20, 20], separator=" ")

            self.assertPropertyEqual(
                dbr, "DestinationRect", [-10, -10, 20, 20], separator=" "
            )
            self.assertPropertyEqual(dbr, "ViewboxUnits", None, default=None)
            self.assertPropertyEqual(dbr, "ViewportUnits", None, default=None)
            self.assertTagEqual(dbr[0], "VisualBrush.Visual")
            self.assertTagEqual(dbr[0][0], "Canvas")
            self.assertGreaterEqual(float(dbr[0][0].get("Width")), 30)
            self.assertGreaterEqual(float(dbr[0][0].get("Height")), 30)
            self.assertEqual(len(dbr[0][0]), 3)
            self.assertTagEqual(dbr[0][0][1], "Rectangle")
            self.assertPropertyEqual(dbr[0][0][2][0], "Matrix", [1, 0, 0, 1, 10, 10])
            self.assertPropertyEqual(dbr[0][0][0], "Canvas.Left", 5)
            self.assertPropertyEqual(dbr[0][0][0], "Canvas.Top", 5)
            self.assertPropertyEqual(dbr[0][0][0], "Width", 10)
            self.assertPropertyEqual(dbr[0][0][0], "Height", 10)

            self.assertPropertyEqual(dbr, "Transform", None)
