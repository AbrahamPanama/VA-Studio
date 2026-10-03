from .context import Svg2XamlTester


class WPFBasicPrimitiveTests(Svg2XamlTester):
    """Basic tests for target=wpf:
    - shapes (lowlevel and canvas) and paths
    - gradients
    - stroke style
    """

    def test_rect_lowlevel(self):
        """Based on shapes-rect-01-t.svg"""
        dwg = self.run_to_dg("rect_basic.svg")
        self.assertEqual(len(dwg), 5)  # 4 rects + BoundingBox
        for i in range(4):
            self.assertTagEqual(dwg[i], "GeometryDrawing")
            self.assertTagEqual(dwg[i][0], "GeometryDrawing.Geometry")
            self.assertTagEqual(dwg[i][0][0], "RectangleGeometry")

        # Coordinates
        self.assertPropertyEqual(dwg[0][0][0], "Rect", [30, 46, 50, 80])
        # Stroke
        self.assertTagEqual(dwg[0][1], "GeometryDrawing.Pen")
        self.assertTagEqual(dwg[0][1][0], "Pen")
        self.assertPropertyEqual(dwg[0][1][0], "Brush", "#ff000000")
        self.assertPropertyEqual(dwg[0][1][0], "Thickness", 1)
        self.assertPropertyEqual(dwg[0][1][0], "DashCap", "Flat")
        # Fill
        self.assertPropertyEqual(dwg[1], "Brush", "#ffff00ff")
        # Check that both Fill and stroke are set.
        self.assertTagEqual(dwg[1][1][0], "Pen")
        # Rounding
        # If only rx is given, we need to set both
        self.assertPropertyEqual(dwg[2][0][0], "RadiusX", 30)
        self.assertPropertyEqual(dwg[2][0][0], "RadiusY", 30)
        # both are given
        self.assertPropertyEqual(dwg[3][0][0], "RadiusX", 30)
        self.assertPropertyEqual(dwg[3][0][0], "RadiusY", 50)

    def test_rect_defaults(self):
        """Based on shapes-rect-02-t.svg"""
        dwg = self.run_to_dg("rect_defaults.svg")
        self.assertEqual(len(dwg), 2)  # they are wrapped in a group
        grp = dwg[0]
        self.assertEqual(len(dwg[0]), 4)  # two rectangles are ignored (w/h = 0)
        self.assertPropertyEqual(grp[0][0][0], "Rect", [0, 46, 50, 80])
        self.assertPropertyEqual(grp[1][0][0], "Rect", [130, 0, 50, 80])
        # check that the radius is symmetric if one attribute is missing
        self.assertPropertyEqual(grp[2][0][0], "RadiusX", 20)
        self.assertPropertyEqual(grp[2][0][0], "RadiusY", 20)
        self.assertPropertyEqual(grp[3][0][0], "RadiusX", 20)
        self.assertPropertyEqual(grp[3][0][0], "RadiusY", 20)
        # fill and stroke were inherited from the group
        self.assertPropertyEqual(grp[0], "Brush", "#ff008000")
        self.assertPropertyEqual(grp[0][1][0], "Brush", "#ff000000")

    def test_rect_canvas(self):
        """Based on shapes-rect-01-t.svg"""
        dwg = self.run_to_canvas("rect_basic.svg")
        self.assertEqual(len(dwg), 5)  # 4 rects + Canvas.Clip
        for i in range(4):
            self.assertTagEqual(dwg[i], "Rectangle")

        # Coordinates
        self.assertPropertyEqual(dwg[0], "Canvas.Left", 29.5)  # 30 - 1/2 of the stroke
        self.assertPropertyEqual(dwg[0], "Canvas.Top", 45.5)
        self.assertPropertyEqual(dwg[0], "Width", 51)
        self.assertPropertyEqual(dwg[0], "Height", 81)
        # Stroke
        self.assertPropertyEqual(dwg[0], "Stroke", "#ff000000")
        self.assertPropertyEqual(dwg[0], "StrokeThickness", 1)
        self.assertPropertyEqual(dwg[0], "StrokeDashCap", "Flat")
        # Fill
        self.assertPropertyEqual(dwg[1], "Fill", "#ffff00ff")
        # Check that both Fill and stroke are set.
        self.assertPropertyEqual(dwg[1], "Stroke", "#ff000000")
        # Rounding
        # If only rx is given, we need to set both
        self.assertPropertyEqual(dwg[2], "RadiusX", 30)
        self.assertPropertyEqual(dwg[2], "RadiusY", 30)
        # both are given
        self.assertPropertyEqual(dwg[3], "RadiusX", 30)
        self.assertPropertyEqual(dwg[3], "RadiusY", 50)

    def test_rect_defaults_canvas(self):
        """Based on shapes-rect-02-t.svg"""
        dwg = self.run_to_canvas("rect_defaults.svg")
        self.assertEqual(len(dwg), 2)  # they are wrapped in a subcanvas
        grp = dwg[0]
        self.assertEqual(len(dwg[0]), 6)
        self.assertPropertyEqual(grp[0], "Canvas.Left", -0.5, default=0.0)
        self.assertPropertyEqual(grp[1], "Canvas.Top", -0.5, default=0.0)
        # two rectangles have no visibility (w/h = 0)
        self.assertPropertyEqual(grp[2], "Visibility", "Hidden")
        self.assertPropertyEqual(grp[3], "Visibility", "Hidden")
        # check that the radius is symmetric if one attribute is missing
        self.assertPropertyEqual(grp[4], "RadiusX", 20)
        self.assertPropertyEqual(grp[4], "RadiusY", 20)
        self.assertPropertyEqual(grp[5], "RadiusX", 20)
        self.assertPropertyEqual(grp[5], "RadiusY", 20)
        # fill and stroke were inherited from the group
        self.assertPropertyEqual(grp[0], "Fill", "#ff008000")
        self.assertPropertyEqual(grp[0], "Stroke", "#ff000000")

    def test_circle_defaults(self):
        """Based on shapes-circle-02-t.svg and shapes-ellipse-02-t.svg"""
        dwg = self.run_to_dg("circle_defaults.svg")
        self.assertEqual(len(dwg), 2)  # they are wrapped in a group
        grp = dwg[0]
        # one circle and two ellipses are ignored (r = 0)
        self.assertEqual(len(dwg[0]), 6)
        self.assertTagEqual(grp[0][0][0], "EllipseGeometry")
        # missing centers
        self.assertPropertyEqual(grp[0][0][0], "RadiusX", 50)
        self.assertPropertyEqual(grp[0][0][0], "RadiusY", 50)
        self.assertPropertyEqual(grp[0][0][0], "Center", [0, 0])
        # missing radius: not rendered
        self.assertPropertyEqual(grp[3][0][0], "RadiusX", 50)
        self.assertPropertyEqual(grp[3][0][0], "RadiusY", 50)
        self.assertPropertyEqual(grp[3][0][0], "Center", [150, 100])
        # fill and stroke were inherited from the group
        self.assertPropertyEqual(grp[0], "Brush", "#ff008000")
        self.assertPropertyEqual(grp[0][1][0], "Brush", "#ff000000")
        # Ellipses
        self.assertTagEqual(grp[4][0][0], "EllipseGeometry")
        self.assertPropertyEqual(grp[4][0][0], "RadiusX", 100)
        self.assertPropertyEqual(grp[4][0][0], "RadiusY", 50)
        self.assertPropertyEqual(grp[4][0][0], "Center", [0, 0])
        self.assertPropertyEqual(grp[5][0][0], "RadiusX", 100)
        self.assertPropertyEqual(grp[5][0][0], "RadiusY", 50)
        self.assertPropertyEqual(grp[5][0][0], "Center", [250, 250])

    def test_circle_defaults_canvas(self):
        """Based on shapes-circle-02-t.svg and shapes-ellipse-02-t.svg"""
        dwg = self.run_to_canvas("circle_defaults.svg")
        self.assertEqual(len(dwg), 2)  # they are wrapped in a group
        grp = dwg[0]
        self.assertEqual(len(dwg[0]), 9)
        self.assertTagEqual(grp[0], "Ellipse")
        # missing centers
        self.assertPropertyEqual(grp[0], "Width", 101)
        self.assertPropertyEqual(grp[0], "Height", 101)
        self.assertPropertyEqual(grp[0], "Canvas.Left", -50.5)
        self.assertPropertyEqual(grp[0], "Canvas.Top", -50.5)
        # missing radius: not rendered
        self.assertPropertyEqual(grp[3], "Visibility", "Hidden")
        self.assertPropertyEqual(grp[4], "Width", 101)
        self.assertPropertyEqual(grp[4], "Height", 101)
        self.assertPropertyEqual(grp[4], "Canvas.Left", 99.5)
        self.assertPropertyEqual(grp[4], "Canvas.Top", 49.5)
        # fill and stroke were inherited from the group
        self.assertPropertyEqual(grp[0], "Fill", "#ff008000")
        self.assertPropertyEqual(grp[0], "Stroke", "#ff000000")

        self.assertTagEqual(grp[5], "Ellipse")
        self.assertPropertyEqual(grp[5], "Width", 201)
        self.assertPropertyEqual(grp[5], "Height", 101)
        self.assertPropertyEqual(grp[5], "Canvas.Left", -100.5)
        self.assertPropertyEqual(grp[5], "Canvas.Top", -50.5)
        self.assertPropertyEqual(grp[8], "Width", 201)
        self.assertPropertyEqual(grp[8], "Height", 101)
        self.assertPropertyEqual(grp[8], "Canvas.Left", 149.5)
        self.assertPropertyEqual(grp[8], "Canvas.Top", 199.5)

    def test_line_polyx(self):
        """Test translations of line, polyline and polygon"""
        dwg = self.run_to_dg("polygon_polyline_line.svg")
        self.assertEqual(len(dwg), 5)

        # The first element is a simple line
        self.assertTagEqual(dwg[0][0][0], "LineGeometry")
        self.assertPropertyEqual(dwg[0][0][0], "StartPoint", [100, 100])
        self.assertPropertyEqual(dwg[0][0][0], "EndPoint", [200, 150])

        # The second element is a polygon, which is approximated by a closed
        # PathGeometry, since WPF doesn't have a polygon.
        self.assertTagEqual(dwg[1][0][0], "PathGeometry")
        self.assertPropertyEqual(
            dwg[1][0][0],
            "Figures",
            "M 270 225 L 300 245 L 320 225 L 340 245 L 280 280 L 390 280 L 420 240 L 280 185 Z",
        )

        # Same for polylines, except the path is open.
        self.assertTagEqual(dwg[2][0][0], "PathGeometry")
        self.assertPropertyEqual(
            dwg[2][0][0],
            "Figures",
            "M 270 75 L 300 95 L 320 75 L 340 95 L 280 130 L 390 130 L 420 90 L 280 35",
        )

        # The last is again a polygon, except that an extra (invalid) coordinate is
        # specified, this would be a syntax error in XAML and is thus ignored.
        self.assertTagEqual(dwg[3][0][0], "PathGeometry")
        self.assertPropertyEqual(
            dwg[3][0][0], "Figures", "M 80 200 L 80 300 L 150 250 L 80 200"
        )

    def test_line_polyx_canvas(self):
        """Test translations of line, polyline and polygon"""
        dwg = self.run_to_canvas("polygon_polyline_line.svg")
        self.assertEqual(len(dwg), 5)

        # The first element is a simple line
        self.assertTagEqual(dwg[0], "Line")
        self.assertPropertyEqual(dwg[0], "X1", 100.0)
        self.assertPropertyEqual(dwg[0], "X2", 200.0)
        self.assertPropertyEqual(dwg[0], "Y1", 100.0)
        self.assertPropertyEqual(dwg[0], "Y2", 150.0)

        # The second element is a polygon, which is approximated by a closed
        # PathGeometry, since WPF doesn't have a polygon.
        self.assertTagEqual(dwg[1], "Path")
        self.assertTagEqual(dwg[1][0], "Path.Data")
        self.assertTagEqual(dwg[1][0][0], "PathGeometry")
        self.assertPropertyEqual(
            dwg[1][0][0],
            "Figures",
            "M 270 225 L 300 245 L 320 225 L 340 245 L 280 280 L 390 280 L 420 240 L 280 185 Z",
        )

        # Same for polylines, except the path is open.
        self.assertTagEqual(dwg[2], "Path")
        self.assertTagEqual(dwg[2][0], "Path.Data")
        self.assertTagEqual(dwg[2][0][0], "PathGeometry")
        self.assertPropertyEqual(
            dwg[2][0][0],
            "Figures",
            "M 270 75 L 300 95 L 320 75 L 340 95 L 280 130 L 390 130 L 420 90 L 280 35",
        )

        # The last is again a polygon, except that an extra (invalid) coordinate is
        # specified, this would be a syntax error in XAML and is thus ignored.
        self.assertTagEqual(dwg[3][0][0], "PathGeometry")
        self.assertPropertyEqual(
            dwg[3][0][0], "Figures", "M 80 200 L 80 300 L 150 250 L 80 200"
        )

    def test_path_syntax(self):
        """Path syntax in XAML is identical, but it doesn't deal with invalid paths
        very well."""
        dgg = self.run_to_dg("paths.svg")
        canvas = self.run_to_canvas("paths.svg")

        for dwg in [dgg, canvas]:
            # Path geometries have the same nesting depth in Canvas and DrawingGroup,
            # so we can reuse the unit test
            self.assertEqual(len(dwg), 6)
            self.assertTagEqual(dwg[0][0][0], "PathGeometry")
            # First path tests cubic bezier commands, and is open
            self.assertPropertyEqual(
                dwg[0][0][0],
                "Figures",
                "m 396 258 c -65 0 -100 -50 -100 -50 c 0 0 -35 -55 -100 -55 m 0 105 c 65 0 100 -50 100 -50 c 0 0 35 -55 100 -55",
            )
            # The default for the fillrule is different, so fillrule needs to be specified
            self.assertPropertyEqual(
                dwg[0][0][0],
                "FillRule",
                "Nonzero",
            )
            # Second path tests quadratic bezier commands, and is closed
            self.assertPropertyEqual(
                dwg[1][0][0],
                "Figures",
                "m 285 20 q 15 100 115 10 m 50 50 Q 375 -20 287 20 Z",
            )
            # Third path tests absolute and relative path commands
            self.assertPropertyEqual(
                dwg[2][0][0],
                "Figures",
                "M 30 100 a 40 40 0 0 1 65 50 Z m 30 30 A 20 20 0 0 0 125 230 Z m 40 24 a 20 20 0 0 1 65 50 z",
            )
            # Fourth path tests implicit lineto
            self.assertPropertyEqual(
                dwg[3][0][0],
                "Figures",
                "m 62 56 l 51.9615 90 l -103.923 0 l 51.9615 -90 z m 0 15 l 38.9711 67.5 l -77.9123 0 l 38.9711 -67.5 z",
            )
            # Fifth path tests command repetition
            self.assertPropertyEqual(
                dwg[4][0][0],
                "Figures",
                "M 220 20 L 240 20 L 260 20",
            )

    def test_lingrad_simple(self):
        """Simple linear gradients, based on pservers-grad-01-t.svg"""
        dgg = self.run_to_dg("lineargradient_simple.svg")
        canvas = self.run_to_canvas("lineargradient_simple.svg")
        # Both linear gradients should yield the same code.
        # For WPF, gradient normal and userspace-normal can differ; for XAML they
        # cannot. To achieve visually identical rendering without computing the
        # bounding box, we wrap the WPF gradient (if userSpaceOnUse is enabled, which
        # is default for SVG, but not really used in Inkscape) in a DrawingBrush,
        # which is strechted to fill the viewbox.

        for dwg in [dgg, canvas]:
            for i in range(2):
                gdw = dwg[i]
                if dwg == dgg:
                    self.assertTagEqual(gdw[0], "GeometryDrawing.Brush")
                else:
                    self.assertTagEqual(gdw[0], "Rectangle.Fill")
                dbr = gdw[0][0]
                self.assertTagEqual(dbr, "DrawingBrush")
                self.assertPropertyEqual(dbr, "Viewbox", [0, 0, 1, 1])
                self.assertPropertyEqual(dbr, "Stretch", "Fill")
                self.assertEqual(len(dbr), 1)
                self.assertTagEqual(dbr[0], "DrawingBrush.Drawing")
                inner_gdr = dbr[0][0]
                self.assertTagEqual(inner_gdr, "GeometryDrawing")
                self.assertEqual(len(inner_gdr), 2)
                self.assertTagEqual(inner_gdr[0], "GeometryDrawing.Brush")
                lgb = inner_gdr[0][0]
                self.assertTagEqual(lgb, "LinearGradientBrush")
                self.assertPropertyEqual(
                    lgb,
                    "MappingMode",
                    "RelativeToBoundingBox",
                    default="RelativeToBoundingBox",
                )
                self.assertPropertyEqual(lgb, "StartPoint", [0, 0])
                self.assertPropertyEqual(lgb, "EndPoint", [1, 1])
                self.assertTagEqual(lgb[0], "GradientStop")
                self.assertPropertyEqual(lgb[0], "Color", "#ff0000ff")
                self.assertPropertyEqual(lgb[0], "Offset", 0)
                self.assertTagEqual(lgb[1], "GradientStop")
                self.assertPropertyEqual(lgb[1], "Color", "#ff00ff00")
                self.assertPropertyEqual(lgb[1], "Offset", 1)

                self.assertTagEqual(inner_gdr[1], "GeometryDrawing.Geometry")
                self.assertTagEqual(inner_gdr[1][0], "RectangleGeometry")
                self.assertPropertyEqual(inner_gdr[1][0], "Rect", [0, 0, 1, 1])
            # The third gradient fills the stroke, and uses absolute mapping.
            # For a canvas object, this requires an additional transform.
            gdw = dwg[2]
            if dwg == dgg:
                self.assertTagEqual(gdw[1][0][0], "Pen.Brush")
                self.assertPropertyEqual(gdw[1][0], "Thickness", 20)
                lgb = gdw[1][0][0][0]
            else:
                self.assertTagEqual(gdw[0], "Rectangle.Stroke")
                self.assertPropertyEqual(gdw, "StrokeThickness", 20)
                lgb = gdw[0][0]
            self.assertTagEqual(lgb, "LinearGradientBrush")
            self.assertPropertyEqual(
                lgb, "MappingMode", "Absolute", default="RelativeToBoundingBox"
            )
            # spreadMethod attribute
            self.assertPropertyEqual(lgb, "SpreadMethod", "Reflect")
            self.assertPropertyEqual(lgb, "StartPoint", [10, 10])
            self.assertPropertyEqual(lgb, "EndPoint", [200, 10])
            if dwg == canvas:
                self.assertPropertyEqual(lgb, "Transform", [1, 0, 0, 1, -15, -240])
            self.assertTagEqual(lgb[0], "GradientStop")
            self.assertPropertyEqual(lgb[0], "Color", "#ff0000ff")
            # test clamping of stop values (svg value: -1.0 would be invalid in XAML)
            self.assertPropertyEqual(lgb[0], "Offset", 0)
            self.assertTagEqual(lgb[1], "GradientStop")
            self.assertPropertyEqual(lgb[1], "Color", "#ff00ff00")
            self.assertPropertyEqual(lgb[1], "Offset", 0.2)

    def test_radialgradient(self):
        """Test unit types on radial gradient"""
        dgg = self.run_to_dg("radialgradient_focal.svg")
        canvas = self.run_to_canvas("radialgradient_focal.svg")
        for dwg in [dgg, canvas]:
            br = dwg[0][0][0][0]
            # Test default attributes
            if dwg == dgg:
                self.assertTagEqual(dwg[0][0][0], "GeometryDrawing.Brush")
            else:
                self.assertTagEqual(dwg[0][0][0], "Rectangle.Fill")
            self.assertTagEqual(br, "RadialGradientBrush")
            self.assertPropertyEqual(br, "RadiusX", 0.5)
            self.assertPropertyEqual(br, "RadiusY", 0.5)
            self.assertPropertyEqual(br, "Center", [0.5, 0.5])
            self.assertPropertyEqual(br, "GradientOrigin", [0.5, 0.5])
            self.assertEqual(len(br), 2)
            self.assertTagEqual(br[0], "GradientStop")
            self.assertTagEqual(br[1], "GradientStop")
            self.assertPropertyEqual(br[0], "Color", "#ff000099")
            self.assertPropertyEqual(br[1], "Color", "#ff00ff00")
            self.assertPropertyEqual(br[0], "Offset", 0)
            self.assertPropertyEqual(br[1], "Offset", 1)
            # Next element has all the properties set, but the stops are identical.
            br = dwg[0][1][0][0]
            self.assertTagEqual(br, "RadialGradientBrush")
            self.assertPropertyEqual(br, "RadiusX", 0.5)
            self.assertPropertyEqual(br, "RadiusY", 0.5)
            self.assertPropertyEqual(br, "Center", [0.2, 0.2])
            self.assertPropertyEqual(br, "GradientOrigin", [0.2, 0.2])
            # Next element: gradient is defined outside defs, has userSpaceOnUse,
            # and is also transformed. This does not affect the gradient though.
            # For canvas, the nesting depth is one lower than for lowlevel (which
            # does not support transforming RectangleGeometries)
            if dwg == dgg:
                br = dwg[0][2][0][0][0]
            else:
                br = dwg[0][2][0][0]
            self.assertTagEqual(br, "RadialGradientBrush")
            self.assertPropertyEqual(br, "RadiusX", 25)
            self.assertPropertyEqual(br, "RadiusY", 25)
            self.assertPropertyEqual(br, "Center", [25, 215])
            self.assertPropertyEqual(br, "GradientOrigin", [25, 215])
            self.assertPropertyEqual(br, "MappingMode", "Absolute")
            if dwg == dgg:
                self.assertPropertyEqual(
                    dwg[0][2],
                    "Transform",
                    (0, -1, 1, 0, 10, 260),
                )
            if dwg == canvas:
                # Comparison value is different because of Canvas.Left / strokewidth
                self.assertPropertyEqual(
                    dwg[0][2],
                    "RenderTransform",
                    (0, -1, 1, 0, 10, 260.5),
                )

    def test_radialgradient_focal(self):
        """Test clamping of focal points on radial gradients"""
        # Next element is a clone of a group. One element has its fill unset and is
        # thus inherited from the clone's fill. The gradient also uses the stop-opacity
        # attribute, and the stops and coordinates are specified as percentages.
        # We're also checking that the clone has the coorect transform set.
        dgg = self.run_to_dg("radialgradient_focal.svg")
        canvas = self.run_to_canvas("radialgradient_focal.svg")
        for dwg in [dgg, canvas]:
            group = dwg[0][3]
            if dwg == dgg:
                self.assertPropertyEqual(
                    group,
                    "Transform",
                    (1, 0, 0, 1, 17.5, 240),
                )
            else:
                self.assertPropertyEqual(
                    group,
                    "RenderTransform",
                    (1, 0, 0, 1, 17.5, 240),
                )
            self.assertEqual(len(group), 6)  # 6 children, the last has the gradient set
            br = group[5][0][0]
            self.assertPropertyEqual(br, "RadiusX", 0.75)
            self.assertPropertyEqual(br, "RadiusY", 0.75)
            self.assertPropertyEqual(br, "Center", [0.1, 0.1])
            self.assertPropertyEqual(br, "GradientOrigin", [0, 0.75])
            self.assertEqual(len(br), 3)
            self.assertTagEqual(br[0], "GradientStop")
            self.assertTagEqual(br[1], "GradientStop")
            self.assertTagEqual(br[2], "GradientStop")
            self.assertPropertyEqual(br[0], "Color", "#00000000")
            self.assertPropertyEqual(br[1], "Color", "#ff00008b")
            self.assertPropertyEqual(br[2], "Color", "#00000000")
            self.assertPropertyEqual(br[0], "Offset", 0)
            self.assertPropertyEqual(br[1], "Offset", 0.5)
            self.assertPropertyEqual(br[2], "Offset", 1)
            # The next element has a focal point outside the "gradient ellipse".
            # According to the specification, it is clamped to the radius.
            # We just test that the origin lies on the ellipse.
            br = dwg[0][4][5][0][0]
            origin = tuple(map(float, br.get("GradientOrigin").split(",")))
            dist = (origin[0] - 0.1) ** 2 + (origin[1] - 0.1) ** 2
            self.assertAlmostEqual(dist, 0.75**2)
            # Also check that the direction of the vector from center to origin
            # remains identical.
            self.assertAlmostEqual((origin[0] - 0.1) / (origin[1] - 0.1), 0.7333 / 0.65)

    def test_radialgradient_clamping(self):
        dgg = self.run_to_dg("gradient_with_mixed_offsets.svg")
        self.assertTagEqual(dgg[0][1][0], "GeometryDrawing.Brush")
        br = dgg[0][1][0][0]
        self.assertTagEqual(br, "RadialGradientBrush")
        self.assertPropertyEqual(br[0], "Color", "#ffff0000")
        self.assertPropertyEqual(br[1], "Color", "#ff0000ff")
        self.assertPropertyEqual(br[2], "Color", "#ffff0000")
        self.assertPropertyEqual(br[0], "Offset", 0)
        self.assertPropertyEqual(br[1], "Offset", 1)
        self.assertPropertyEqual(br[2], "Offset", 1)

    def test_strokestyle(self):
        """Test stroke-linecap and stroke-dasharray"""
        dgg = self.run_to_dg("strokestyle.svg")
        # First test has a round dashcap. All three dashcaps need to be set to achieve
        # equal rendering
        self.assertTagEqual(dgg[0][1], "GeometryDrawing.Pen")
        self.assertTagEqual(dgg[0][1][0], "Pen")
        self.assertPropertyEqual(dgg[0][1][0], "Thickness", 30)
        self.assertPropertyEqual(dgg[0][1][0], "DashCap", "Round")
        self.assertPropertyEqual(dgg[0][1][0], "EndLineCap", "Round")
        self.assertPropertyEqual(dgg[0][1][0], "StartLineCap", "Round")
        # Second test has flat dashcap ("butt"), which is "Flat" in XAML
        self.assertTagEqual(dgg[1][1], "GeometryDrawing.Pen")
        self.assertTagEqual(dgg[1][1][0], "Pen")
        self.assertPropertyEqual(dgg[1][1][0], "Thickness", 30)
        self.assertPropertyEqual(dgg[1][1][0], "DashCap", "Flat", default="Square")
        self.assertPropertyEqual(dgg[1][1][0], "EndLineCap", "Flat", default="Flat")
        self.assertPropertyEqual(dgg[1][1][0], "StartLineCap", "Flat", default="Flat")
        # Third test has Stroke-dasharray "none", so attribute should not be set
        # in XAML
        self.assertTagEqual(dgg[2][1], "GeometryDrawing.Pen")
        self.assertEqual(len(dgg[2][1][0]), 0)  # no children for lowlevel
        # The next two tests have the same linestyle, but the second one specified using
        # an odd number of argument to stroke-dasharray.
        # The XAML representation is different because dashes differ by a factor of
        # the line thickness
        self.assertPropertyEqual(dgg[3][1][0], "Thickness", 20)
        self.assertEqual(len(dgg[3][1][0]), 1)
        self.assertTagEqual(dgg[3][1][0][0], "Pen.DashStyle")
        self.assertTagEqual(dgg[3][1][0][0][0], "DashStyle")
        self.assertPropertyEqual(
            dgg[3][1][0][0][0], "Dashes", [0.25, 0.1, 0.25, 0.25, 0.1, 0.25]
        )
        self.assertPropertyEqual(dgg[4][1][0], "Thickness", 10)
        self.assertPropertyEqual(
            dgg[4][1][0][0][0], "Dashes", [0.5, 0.2, 0.5, 0.5, 0.2, 0.5]
        )

    def test_strokestyle_canvas(self):
        """Test stroke-linecap and stroke-dasharray"""
        canvas = self.run_to_canvas("strokestyle.svg")
        # First test has a round dashcap. All three dashcaps need to be set to achieve
        # equal rendering
        self.assertTagEqual(canvas[0], "Path")
        self.assertPropertyEqual(canvas[0], "StrokeThickness", 30)
        self.assertPropertyEqual(canvas[0], "StrokeDashCap", "Round")
        self.assertPropertyEqual(canvas[0], "StrokeEndLineCap", "Round")
        self.assertPropertyEqual(canvas[0], "StrokeStartLineCap", "Round")
        # Second test has flat dashcap ("butt"), which is "Flat" in XAML
        self.assertTagEqual(canvas[1], "Path")
        self.assertPropertyEqual(canvas[1], "StrokeThickness", 30)
        self.assertPropertyEqual(canvas[1], "StrokeDashCap", "Flat", default="Square")
        self.assertPropertyEqual(canvas[1], "StrokeEndLineCap", "Flat", default="Flat")
        self.assertPropertyEqual(
            canvas[1], "StrokeStartLineCap", "Flat", default="Flat"
        )
        # Third test has Stroke-dasharray "none", so attribute should not be set
        # in XAML
        self.assertTagEqual(canvas[2], "Path")
        self.assertPropertyEqual(canvas[2], "StrokeDashArray", None, default=None)
        # The next two tests have the same linestyle, but the second one specified using
        # an odd number of argument to stroke-dasharray.
        # The XAML representation is different because dashes differ by a factor of
        # the line thickness
        self.assertPropertyEqual(canvas[3], "StrokeThickness", 20)
        self.assertPropertyEqual(
            canvas[3], "StrokeDashArray", [0.25, 0.1, 0.25, 0.25, 0.1, 0.25]
        )
        self.assertPropertyEqual(canvas[4], "StrokeThickness", 10)
        self.assertPropertyEqual(
            canvas[4], "StrokeDashArray", [0.5, 0.2, 0.5, 0.5, 0.2, 0.5]
        )
