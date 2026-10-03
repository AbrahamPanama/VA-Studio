#!/usr/bin/env python3
# coding=utf-8
"""
Test the filter elements functionality
"""

from inkex.tester import TestCase
from inkex.tester.svg import svg_file
from inkex.elements._parser import load_svg
from inkex import Filter, Rectangle, LinearGradient, Stop


class GradientTestCase(TestCase):
    source_file = "gradient_with_mixed_offsets.svg"

    def setUp(self):
        super().setUp()
        self.svg = svg_file(self.data_file("svg", self.source_file))

    def test_gradient_offset_order(self):
        _gradient = self.svg.getElementById("MyGradient")
        offsets = [stop.attrib.get("offset") for stop in _gradient.stops]
        assert offsets == ["0%", "50", "100%"]

    def test_append_gradient(self):
        gradient = LinearGradient.new(Stop.new(offset=0.5))
        self.svg[1][2].style["fill"] = gradient
        assert gradient.getparent().TAG == "defs"
        fill = self.svg[1][2].style("fill")
        assert fill == gradient
        assert fill[0].TAG == "stop"


class FiltersTestCase(TestCase):
    """Test getting and setting the style["filter"] attribute"""

    source_file = "filters.svg"

    def setUp(self):
        super().setUp()
        self.svg = svg_file(self.data_file("svg", self.source_file))

    def test_filters_reading(self):
        """Check that filters are read correctly"""
        el = self.svg.getElementById("multifilter")
        self.assertTupleEqual(
            tuple(e.get_id() for e in el.style("filter")),
            ("filter186", "filter190", "filter186"),
        )

        el = self.svg.getElementById("onefilter")
        self.assertEqual(el.style("filter")[0], self.svg.getElementById("filter186"))

        el = self.svg.getElementById("missingfilter")
        self.assertEqual(el.style("filter"), [])

        el = self.svg.getElementById("nofilter")
        self.assertEqual(el.style("filter"), [])

    def test_filters_editing(self):
        """Inserting and removing filters"""
        # Insert a new filter
        el = self.svg.getElementById("onefilter")
        filters = el.style("filter")
        tf = Filter.new(Filter.Turbulence.new())
        filters.append(tf)
        el.style["filter"] = filters
        assert tf.getparent().TAG == "defs"
        self.assertTupleEqual(
            tuple(e.get_id() for e in el.style("filter")), ("filter186", tf.get_id())
        )

        # Insert an already attached filter
        el = self.svg.getElementById("nofilter")
        el.style["filter"] = tf
        self.assertEqual(el.style("filter")[0], tf)

        # Remove the filter
        el.style["filter"] = []
        self.assertEqual(el.style("filter"), [])

        # Remove the filter
        el.style["filter"] = [None]
        self.assertEqual(el.style("filter"), [])

        el.style["filter"] = None
        self.assertEqual(el.style("filter"), [])

    def test_filters_editing_inplace(self):
        """Inserting and removing filters"""
        # Insert a new filter
        el = self.svg.getElementById("multifilter")

        tf = Filter.new(Filter.Turbulence.new())
        el.style("filter").append(tf)
        assert tf.getparent().TAG == "defs"
        self.assertTupleEqual(
            tuple(e.get_id() for e in el.style("filter")),
            ("filter186", "filter190", "filter186", tf.get_id()),
        )

        el = self.svg.getElementById("nofilter")
        # Add a filter again
        el.style("filter").append("url(#filter186)")
        self.assertEqual(el.style("filter")[0].get_id(), "filter186")

    def test_by_id(self):
        """Attach by id"""
        el = self.svg.getElementById("onefilter")

        el.style["filter"] = "url(#filter190)"
        self.assertEqual(el.style("filter")[0].get_id(), "filter190")

        el = self.svg.getElementById("onefilter")

        el.style["filter"] = ["url(#filter190)", "url(#filter186)"]
        self.assertTupleEqual(
            tuple(e.get_id() for e in el.style("filter")), ("filter190", "filter186")
        )

    def test_filter_from_other(self):
        """Check that filters are automatically copied if they come from the wrong document"""
        # Create a new document and attach a filter to the rectangle.

        svg = load_svg('<svg id="svg2"><rect id="rect1"/></svg>').getroot()
        filter = Filter.new(Filter.GaussianBlur.new())

        svg.getElementById("rect1").style["filter"] = filter

        el = self.svg.getElementById("nofilter")
        el.style["filter"] = filter

        assert filter.root.get_id() == "svg2"
        assert el.style("filter")[0] is not filter
        assert el.style("filter")[0][0].TAG == "feGaussianBlur"

    def test_filters_unrooted(self):
        """Test filter attribute in unrooted documents"""
        el = Rectangle.new(
            left=0, top=0, width=10, height=10, style="filter: url(#filter1)"
        )
        filt = Filter.new(Filter.GaussianBlur.new(), id="testfilter")
        assert el.style("filter")[0] == "url(#filter1)"
        el.style["filter"] = filt
        assert el.style("filter")[0] == "url(#testfilter)"
