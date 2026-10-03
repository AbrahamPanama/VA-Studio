# coding=utf-8
"""
Test Inkex style parsing functionality.
"""

import pytest
import warnings

from inkex.styles import Style
from inkex.colors import ColorRGB
from inkex.tester import TestCase
from inkex.tester.svg import svg_file


class StyleTest(TestCase):
    """Test path API and calculations"""

    def test_new_style(self):
        """Create a style from a path string"""
        stl = Style("border-color: blue; border-width: 4px;")
        self.assertEqual(str(stl), "border-color:blue;border-width:4px")

    def test_composite(self):
        """Test chaining styles together"""
        stl = Style("border-color: blue;")
        stl += "border-color: red; border-issues: true;"
        self.assertEqual(str(stl), "border-color:red;border-issues:true")
        st2 = stl + "border-issues: false;"
        self.assertEqual(str(st2), "border-color:red;border-issues:false")

    def test_inbuilts(self):
        """Test inbuild style functions"""
        stadd = Style("a: 1") + Style("b: 2")
        self.assertTrue(stadd == Style("b: 2; a: 1"))
        self.assertFalse(stadd == Style("b: 2"))
        self.assertFalse(stadd != Style("b: 2; a: 1"))
        self.assertEqual(stadd - "a: 4", "b: 2")
        stadd -= "b: 3; c: 4"
        self.assertEqual(stadd, Style("a: 1"))

    def test_set_property(self):
        """Set the style attribute directly"""
        stl = Style()
        stl["border-pain"] = "green"
        self.assertEqual(str(stl), "border-pain:green")

    def test_color_property(self):
        """Color special handling"""
        stl = Style("fill-opacity:0.7;fill:red;")
        self.assertEqual(stl.get_color("fill").alpha, 0.7)
        self.assertEqual(str(stl.get_color("fill")), "rgba(255, 0, 0, 70%)")
        stl.set_color("rgba(0, 127, 0, 0.5)", "stroke")
        self.assertEqual(
            str(stl), "fill-opacity:0.7;fill:red;stroke-opacity:0.5;stroke:#007f00"
        )

    def test_interpolate(self):
        """Test interpolation method."""
        stl1 = Style(
            {"stroke-width": "0px", "fill-opacity": 1.0, "fill": ColorRGB((200, 0, 0))}
        )
        stl2 = Style(
            {
                "stroke-width": "1pc",
                "fill-opacity": 0.0,
                "fill": ColorRGB((100, 0, 100)),
            }
        )
        stl3 = stl1.interpolate(stl2, 0.5)
        print(stl3)
        self.assertAlmostEqual(stl3("fill-opacity"), 0.5, 1e-3)
        assert stl3("fill") == [150, 0, 50]
        assert stl3["stroke-width"] == "8px"

    def test_callback(self):
        """Test callback."""
        calls = 0

        def cb(style):
            nonlocal calls
            self.assertNotIn("fill-opacity", style)
            calls += 1

        st = Style(
            {"stroke-width": "0px", "fill-opacity": 1.0, "fill": ColorRGB((200, 0, 0))},
            callback=cb,
        )
        self.assertEqual(calls, 0)
        st.pop("fill-opacity")
        self.assertEqual(calls, 1)

        def cb2(style):
            nonlocal calls
            self.assertEqual(style["fill-opacity"], ".75")
            calls += 1

        st.callback = cb2
        st["fill-opacity"] = ".75"
        self.assertEqual(calls, 2)

        # del and pop should both work, and call the callback only once.
        st.callback = cb
        del st["fill-opacity"]
        self.assertEqual(calls, 3)


class StyleSheetTest(TestCase):
    """Test parsing style sheets"""

    def setUp(self):
        super(StyleSheetTest, self).setUp()
        self.svg = svg_file(self.data_file("svg", "css.svg"))
        self.css = self.svg.stylesheet

    def test_classes(self):
        """Test element class manipulation"""
        rect = self.svg.getElementById("rect2")
        self.assertEqual(rect.get("class"), "two")
        self.assertEqual(rect.classes, ["two"])
        rect.classes[0] = "twa"
        self.assertEqual(rect.get("class"), "twa")
        rect.classes.append("tri")
        rect.classes.append("four")
        self.assertEqual(rect.get("class"), "twa tri four")
        rect.classes.remove("twa")
        self.assertEqual(rect.get("class"), "tri four")
        rect.classes.toggle("toggle")
        self.assertEqual(rect.get("class"), "tri four toggle")
        rect.classes.toggle("toggle")
        self.assertEqual(rect.get("class"), "tri four")

    def test_creation(self):
        """Stylesheet is created when needed"""
        self.svg = svg_file(self.data_file("svg", "empty.svg"))
        self.assertEqual(len(self.svg.stylesheets), 0)
        self.assertEqual(len(self.svg.stylesheet), 0)
        self.assertEqual(len(self.svg.stylesheets), 1)
        self.svg.stylesheet.append(".cls1 { fill: blue; }")
        self.assertIn(
            b"style><![CDATA[\n.cls1 {\n  fill:blue;\n}\n]]><", self.svg.tostring()
        )

    def test_parsing(self):
        """SVG parsing provides access to stylesheets"""
        sheets = self.svg.stylesheets
        self.assertEqual(len(sheets), 3)
        self.assertEqual(len(sheets[0]), 7)
        self.assertEqual(len(sheets[1]), 0)
        self.assertEqual(len(sheets[2]), 2)

    def test_string(self):
        """Rendered to a string"""
        sheets = self.svg.stylesheets
        self.assertEqual(str(sheets[0][0]), "#layer1 {\n  stroke:yellow;\n}")
        self.assertEqual(str(sheets[2][1]), ".rule {}")

    def test_lookup_by_id(self):
        """ID CSS lookup"""
        matches = list(self.css[0].all_matches(self.svg))
        assert len(matches) == 1
        elem = matches[0]
        self.assertEqual(elem.get("id"), "layer1")

    def test_lookup_by_element(self):
        """Element name CSS lookup"""
        elems = list(self.css[1].all_matches(self.svg))
        self.assertEqual(len(elems), 2)
        self.assertEqual(elems[0].get("id"), "circle1")
        self.assertEqual(elems[1].get("id"), "circle2")

    def test_lookup_by_class(self):
        """Class name CSS lookup"""
        matches = list(self.css[2].all_matches(self.svg))
        assert len(matches) == 1
        elem = matches[0]
        self.assertEqual(elem.get("id"), "rect2")

    def test_lookup_and(self):
        """Multiple CSS lookups"""
        with self.assertWarns(DeprecationWarning):
            xpath = self.css[3].to_xpath()
            self.assertIn(
                xpath,
                [
                    "//*[@id='rect3']"
                    "[contains(concat(' ', normalize-space(@class), ' '), ' three ')]",
                    # cssselect 1.1.0
                    "descendant-or-self::*[@id = 'rect3' and "
                    "(@class and contains(concat(' ', normalize-space(@class), ' '), ' three '))]",
                    # cssselect 1.2.0
                    "descendant-or-self::*[(@id = 'rect3') and "
                    "(@class and contains(concat(' ', normalize-space(@class), ' '), ' three '))]",
                ],
            )
            elem_old = self.svg.getElement(xpath)

        matches = list(self.css[3].all_matches(self.svg))
        assert len(matches) == 1
        elem = matches[0]
        assert elem == elem_old
        self.assertEqual(elem.get("id"), "rect3")

    def test_lookup_or(self):
        """SVG rules can look up the right elements"""
        elems = list(self.css[6].all_matches(self.svg))
        self.assertEqual(len(elems), 3)
        self.assertEqual(elems[0].get("id"), "rect2")
        self.assertEqual(elems[1].get("id"), "circle1")
        self.assertEqual(elems[2].get("id"), "circle2")

    def test_applied_styles(self):
        """Are styles applied to the svg elements correctly"""
        font_default = (
            "font-style:normal;font-variant:normal;"
            "font-weight:normal;font-stretch:normal;font-size:Homie;"
            "line-height:normal;font-family:sans-serif"
        )
        self.assertEqual(
            str(self.svg.getElementById("rect1").cascaded_style()), "fill:blue"
        )
        self.assertEqual(
            str(self.svg.getElementById("rect2").cascaded_style()),
            "fill:green;" + font_default,
        )
        self.assertEqual(
            str(self.svg.getElementById("rect3").cascaded_style()), "fill:cyan"
        )
        self.assertEqual(
            str(self.svg.getElementById("rect4").cascaded_style()),
            "fill:grey;stroke:red",
        )

        self.assertEqual(
            str(self.svg.getElementById("circle1").cascaded_style()),
            "fill:red;" + font_default,
        )
        self.assertEqual(
            str(self.svg.getElementById("circle2").cascaded_style()),
            "fill:red;" + font_default,
        )
