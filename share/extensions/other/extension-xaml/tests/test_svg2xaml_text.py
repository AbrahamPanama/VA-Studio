"""Text tests for XAML export"""

from .context import Svg2XamlTester
import inkex


class XamlTestCanvasTexts(Svg2XamlTester):
    """Test Text support"""

    def setUp(self):
        self.canvas = self.run_to_canvas("text_types.svg")[0]

    def assert_position(
        self,
        text,
        fontsize,
        lineheight,
        left,
        top,
        transform=inkex.Transform(),
        flowroot=False,
    ):
        if flowroot:
            top = top - fontsize + fontsize * (lineheight + 1) / 2
        else:
            top = top - fontsize + fontsize * (lineheight - 1) / 2

        self.assertPropertyEqual(text, "Canvas.Left", left)
        self.assertPropertyEqual(text, "Canvas.Top", top)
        self.assertPropertyEqual(text, "FontSize", fontsize)
        self.assertPropertyEqual(
            text, "LineHeight", lineheight * fontsize, default=fontsize
        )

        trans = (
            inkex.Transform(f"translate({-left}, {-top})")
            @ inkex.Transform(transform)
            @ inkex.Transform(f"translate({left}, {top})")
        )
        self.assertPropertyEqual(
            text, "RenderTransform", list(trans.to_hexad()), default="1, 0, 0, 1, 0, 0"
        )

    def test_regular_text(self):
        """The first text is a regular text."""
        text = self.canvas[0]
        self.assertTagEqual(text, "TextBlock")
        self.assert_position(text, 3, 1.25, 13, 5)

        self.assertPropertyEqual(text, "FontFamily", "Arial")
        self.assertPropertyEqual(text, "Foreground", "#ff000000")
        self.assertEqual(text.text.strip(), "This is a regular text")
        self.assertTagEqual(text[0], "LineBreak")
        self.assertEqual(text[0].tail.strip(), "With a")
        self.assertTagEqual(text[1], "Span")
        # This span can not be simplified because it has a decoration
        # applied (in form of a property child), we don't test the contents
        # here (see decorations tests)
        self.assertTagEqual(text[1][0], "Span.TextDecorations")
        self.assertEqual(text[1][0].tail.strip(), "line break.")
        self.assertTagEqual(text[2], "LineBreak")

    def test_transformed_text(self):
        """The text has a transform set, check that this
        is corrected for Canvas.Left/Canvas.Top"""
        # Check that position and transform are correct
        text = self.canvas[1]
        self.assert_position(text, 3, 1.25, 80, -6, "matrix(1.2,0.5,-0.4,0.7,0,0)")

        # check the contents
        self.assertEqual(text.text.strip(), "This is regular text")
        self.assertTagEqual(text[0], "LineBreak")
        self.assertEqual(text[0].tail.strip(), "with a line break")
        self.assertTagEqual(text[1], "LineBreak")
        self.assertEqual(text[1].tail.strip(), "and some transforms.")

    def test_inline_size(self):
        """Test that an SVG2 inline-size text is converted to
        an auto-wrapped textblock."""
        text = self.canvas[2]
        # Testing for position ommitted, same as above
        self.assertPropertyEqual(text, "TextWrapping", "Wrap")
        self.assertPropertyEqual(text, "Width", 25.75)
        self.assertEqual(
            text.text.strip(), "This is an SVG2 flowed text (inline size)."
        )
        self.assertEqual(text[0].tail.strip(), "This is a line break")
        self.assertTagEqual(text[0], "LineBreak")
        self.assertEqual(text[1].text.strip(), "inside")
        self.assertTagEqual(text[1], "Span")
        self.assertPropertyEqual(text[1], "FontWeight", "Bold")
        self.assertEqual(text[1].tail.strip(), "that text.")

    def test_shape_inside(self):
        """For a shape inside: rectangle, the text is converted
        to an auto-wrapped textblock"""
        text = self.canvas[3]
        self.assert_position(
            text, 20, 1.25, 100, 318.01509, "matrix(0.15,0,0,0.15,-3,-3)"
        )

        self.assertPropertyEqual(text, "TextWrapping", "Wrap")
        self.assertPropertyEqual(text, "Width", 200)
        self.assertEqual(
            text.text.strip(), "This is an SVG2 flowed text (shape inside)"
        )

    def test_svg12_flowed_text(self):
        """The position computation is slightly different for flowroots"""
        text = self.canvas[5]
        self.assert_position(
            text, 12, 1.25, 314, 70, "translate(-32,-8)scale(0.25)", True
        )
        self.assertPropertyEqual(text, "TextWrapping", "Wrap")
        self.assertPropertyEqual(text, "Width", 150)
        self.assertTagEqual(text[0], "Span")
        self.assertPropertyEqual(text[0], "FontSize", 10)
        self.assertEqual(
            text[0].text.strip(), "This is an SVG1.2 flowed text (flowroot)"
        )
        self.assertTagEqual(text[1], "LineBreak")
        self.assertEqual(text[1].tail.strip(), "And this is a linebreak")
        self.assertTagEqual(text[2], "Span")
        self.assertPropertyEqual(text[2], "FontSize", 20.5)
        self.assertEqual(text[2].text.strip(), "inside.")
        # Also check that absolute line heights on flowroots
        # are processed correctly
        text = self.canvas[6]
        self.assert_position(
            text, 12, 20 / 12, 300, 70, "translate(10, -10)scale(0.25)", True
        )
        # also no line height, assume default value of 1.2
        text = self.canvas[7]
        self.assert_position(text, 12, 1, 300, 70, "translate(10, 0)scale(0.25)", True)

    def test_style(self):
        """Test Blur and invisible text"""
        text = self.canvas[10]
        # Only test for the blur
        self.assertTagEqual(text[1], "TextBlock.Effect")
        self.assertTagEqual(text[1][0], "BlurEffect")
        self.assertPropertyEqual(text[1][0], "Radius", 1)
        self.assertEqual(text[1].tail.strip(), "Blurred text")

        text = self.canvas[11]
        self.assertPropertyEqual(text, "FontFamily", "Arial, Verdana")
        self.assertPropertyEqual(text, "Visibility", "Hidden")
        self.assertEqual(text.text.strip(), "This text should be invisible")

    def test_non_ink_attributes(self):
        """Test some code paths that can only be triggered with
        non-inkscape properties"""

        text = self.canvas[12]
        # Font weight specified numerically
        self.assertPropertyEqual(text, "FontWeight", "Heavy")
        # Font stretch in percent
        self.assertPropertyEqual(text, "FontStretch", "UltraCondensed")
        # Also the text decoration is set to blink
        # (which is not supported by XAML, so we don't test it explicitly)

    def test_variable_line_height(self):
        """An SVG2 text with differing line height is split into multiple."""
        first = 60.199661
        text = self.canvas[13]
        self.assertEqual(text.text.strip(), "This is a text")
        self.assertPropertyEqual(text, "Canvas.Top", first)
        text = self.canvas[14]
        self.assertEqual(text.text.strip(), "which changes")
        self.assertPropertyEqual(text, "Canvas.Top", first + 3.25)
        text = self.canvas[15]
        self.assertEqual(text.text.strip(), "line-height")
        self.assertPropertyEqual(text, "Canvas.Top", first + 3.25 + 4)
        text = self.canvas[16]
        self.assertEqual(text.text.strip(), "midway twice.")
        self.assertPropertyEqual(text, "Canvas.Top", first + 3.25 * 2 + 4)


class TextDecorationsText(Svg2XamlTester):
    def setUp(self):
        self.canvas = self.run_to_canvas("text-decorations.svg")

    def test_abbrev(self):
        """Check that the simplest decorations work"""
        canvas = self.canvas
        # The first text has the first word underlined.
        text = canvas[0]
        self.assertEqual(len(text), 1)
        self.assertTagEqual(text[0], "Span")
        self.assertPropertyEqual(text[0], "TextDecorations", "Underline")
        self.assertEqual(text[0].text, "Underlined")
        self.assertEqual(text[0].tail.strip(), "text")

        # The second text has a different color and the first word is overlined.
        text = canvas[1]
        self.assertEqual(len(text), 1)
        self.assertTagEqual(text[0], "Span")
        self.assertPropertyEqual(text, "Foreground", "#7fff00ff")
        self.assertPropertyEqual(text[0], "TextDecorations", "Overline")
        self.assertEqual(text[0].text, "Overlined")
        self.assertEqual(text[0].tail.strip(), "no shorthand")

    def test_linestyle(self):
        """Check dotted and dashed decoration"""
        canvas = self.canvas
        # The third text is dotted underlined. Here, we have to add a real
        # TextDecoration element.
        text = canvas[2]
        self.assertEqual(len(text), 1)
        self.assertTagEqual(text[0], "Span")
        self.assertEqual(len(text[0]), 1)
        self.assertTagEqual(text[0][0], "Span.TextDecorations")
        self.assertTagEqual(text[0][0][0], "TextDecorationCollection")
        self.assertTagEqual(text[0][0][0][0], "TextDecoration")
        self.assertPropertyEqual(text[0][0][0][0], "Location", "Underline")

        self.assertTagEqual(text[0][0][0][0][0], "TextDecoration.Pen")
        pen = text[0][0][0][0][0][0]
        self.assertTagEqual(pen, "Pen")
        self.assertPropertyEqual(pen, "Brush", "#ff008000")
        self.assertPropertyEqual(pen, "DashCap", "Round")
        self.assertPropertyEqual(pen, "Thickness", 1)

        self.assertTagEqual(pen[0], "Pen.DashStyle")
        self.assertTagEqual(pen[0][0], "DashStyle")
        dashes = pen[0][0].get("Dashes").split(",")
        # The first entry needs to be 0, the second entry larger than 1.
        # The spec leaves the exact style to the user agent
        self.assertEqual(float(dashes[0]), 0)
        self.assertGreater(float(dashes[1]), 1)
        # In this case the text follows the property child Span.TextDecorations as tail
        # It must not be before, this would be a syntax error if there are other
        # children as well
        self.assertEqual(text[0][0].tail.strip(), "Dotted underlined")
        self.assertEqual(text[0].tail.strip(), "text")

        # The fourth text is overlined with a dashed line. Only check what's different
        # to the above case.
        text = canvas[3]
        self.assertPropertyEqual(text[0][0][0][0], "Location", "Overline")
        pen = text[0][0][0][0][0][0]
        self.assertPropertyEqual(pen, "DashCap", "Flat")
        dashes = pen[0][0].get("Dashes").split(",")
        # The exact values dont matter, but both ned to be > 0 and equal
        self.assertEqual(dashes[0], dashes[1])
        self.assertGreater(float(dashes[0]), 1)

        self.assertEqual(text[0][0].tail.strip(), "dashed overlined")
        self.assertEqual(text[0].tail.strip(), "text")

    def test_multiple(self):
        """Check multiple text decoration"""
        canvas = self.canvas
        # The 9th text has a double over and underline in a part of the text,
        # and the font size changes midway.
        text = canvas[8]
        self.assertTagEqual(text[0][0], "Span")
        self.assertEqual(text[0][0].text, "Double")
        self.assertPropertyEqual(text[0][0], "FontSize", 6.35)
        self.assertEqual(text[0][0].tail.strip(), "over- and underline")

        coll = text[0][1][0]
        self.assertTagEqual(coll, "TextDecorationCollection")
        self.assertEqual(len(coll), 4)
        self.assertPropertyEqual(coll[0], "Location", "Underline")
        self.assertTagEqual(coll[0][0], "TextDecoration.Pen")
        self.assertTagEqual(coll[0][0][0], "Pen")
        brush = coll[0][0][0]
        self.assertPropertyEqual(brush, "Brush", "#ff000000")
        self.assertPropertyEqual(brush, "Thickness", 1)

        dec = text[0][1][0][1]
        self.assertPropertyEqual(dec, "Location", "Underline")
        self.assertPropertyEqual(dec, "PenOffset", 0.1)
        self.assertPropertyEqual(dec, "PenOffsetUnit", "FontRenderingEmSize")

        dec = text[0][1][0][2]
        self.assertPropertyEqual(dec, "Location", "Overline")

        dec = text[0][1][0][3]
        self.assertPropertyEqual(dec, "Location", "Overline")
        self.assertPropertyEqual(dec, "PenOffset", -0.1)
        self.assertPropertyEqual(dec, "PenOffsetUnit", "FontRenderingEmSize")

        self.assertEqual(text[0].tail.strip(), "text")

        # The 10th text has a red underline everywhere (decoration is applied to the
        # TextBlock) plus a text-colored strikethrough in the last word.
        text = canvas[9]

        self.assertEqual(len(text), 2)
        self.assertEqual(text[0].text, "here")
        self.assertPropertyEqual(text[0], "TextDecorations", "Strikethrough")
        self.assertTagEqual(text[1], "TextBlock.TextDecorations")
        self.assertEqual(
            text[1].tail.strip(), "Everything red underlined, plus black strikethrough"
        )
        self.assertTagEqual(text[1][0], "TextDecorationCollection")
        self.assertTagEqual(text[1][0][0], "TextDecoration")
        self.assertPropertyEqual(text[1][0][0], "Location", "Underline")
        self.assertPropertyEqual(text[1][0][0][0][0], "Brush", "#ffff0000")
