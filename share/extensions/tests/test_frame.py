# coding=utf-8
#
# Copyright (C) 2016 Richard White, rwhite8282@gmail.com
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
"""
An Inkscape frame extension test class.
"""

from __future__ import absolute_import, print_function, unicode_literals

import inkex
from frame import Frame
from inkex.tester import ComparisonMixin, InkscapeExtensionTestMixin, TestCase


class FrameTest(TestCase):
    """tests for the Frame extension"""

    effect_class = Frame

    @staticmethod
    def get_frame(svg):
        """Find the frame in the document tree"""
        return svg.getElement(
            '//svg:g[@id="layer1"]//svg:rect[@inkscape:label="Frame"] | '
            '//svg:g[@id="layer1"]//svg:ellipse[@inkscape:label="Frame"]'
        )

    def test_single_frame(self):
        """Test a simple frame."""
        args = [
            "--corner_radius=20",
            "--fill_color=-16777124",
            "--id=rect3006",
            "--z_position=top",
            "--stroke_color=255",
            '--tab="stroke"',
            "--width=10",
            self.data_file("svg", "single_box.svg"),
        ]
        uut = Frame()
        uut.run(args)
        new_frame = self.get_frame(uut.svg)
        self.assertIsNotNone(new_frame)
        self.assertEqual("{http://www.w3.org/2000/svg}rect", new_frame.tag)
        new_frame_style = new_frame.attrib["style"].lower()
        self.assertTrue(
            "fill-opacity:0.36" in new_frame_style,
            'Invalid fill-opacity in "' + new_frame_style + '".',
        )
        self.assertTrue(
            "stroke:#000000" in new_frame_style,
            'Invalid stroke in "' + new_frame_style + '".',
        )
        self.assertTrue(
            "stroke-width:10.0" in new_frame_style,
            'Invalid stroke-width in "' + new_frame_style + '".',
        )
        self.assertTrue(
            "fill:#ff0000" in new_frame_style,
            'Invalid fill in "' + new_frame_style + '".',
        )

    def test_single_frame_grouped(self):
        """Test a grouped frame"""
        args = [
            "--corner_radius=20",
            "--fill_color=-16777124",
            "--group=True",
            "--id=rect3006",
            "--z_position=top",
            "--stroke_color=255",
            '--tab="stroke"',
            "--width=10",
            self.data_file("svg", "single_box.svg"),
        ]
        uut = Frame()
        uut.run(args)
        new_frame = self.get_frame(uut.svg)
        self.assertIsNotNone(new_frame)
        self.assertEqual("{http://www.w3.org/2000/svg}rect", new_frame.tag)
        group = new_frame.getparent()
        self.assertEqual("{http://www.w3.org/2000/svg}g", group.tag)
        self.assertEqual("{http://www.w3.org/2000/svg}rect", group[0].tag)
        self.assertEqual("{http://www.w3.org/2000/svg}rect", group[1].tag)
        self.assertEqual("Frame", group[1].label)

    def test_single_frame_clipped(self):
        """Test a clipped frame"""
        uut = self.assertEffect(
            "svg",
            "single_box.svg",
            clip=True,
            corner_radius=20,
            fill_color=-16777124,
            id="rect3006",
            stroke_color=255,
            tab="stroke",
            width=10,
        )
        new_frame = self.get_frame(uut.svg)
        self.assertIsNotNone(new_frame)
        self.assertEqual("{http://www.w3.org/2000/svg}rect", new_frame.tag)
        orig = list(uut.svg.selection.values())[0]
        self.assertEqual("url(#clipPath5815)", orig.get("clip-path"))
        clip_path = uut.svg.getElement("//svg:defs/svg:clipPath")
        self.assertEqual("{http://www.w3.org/2000/svg}clipPath", clip_path.tag)

    def test_frame_split(self):
        """Test a frame that is split between bottom and top"""
        args = [
            "--type=ellipse",
            "--fill_color=#AAA",
            "--group=True",
            "--id=rect3006",
            "--z_position=split",
            "--stroke_color=#F00",
            "--offset_relative=-10",
            "--width=2",
            self.data_file("svg", "single_box.svg"),
        ]
        uut = Frame()
        uut.run(args)
        new_frame = self.get_frame(uut.svg)
        top_frame = new_frame.getparent()[2]
        self.assertIsNotNone(new_frame)
        self.assertIsInstance(new_frame, inkex.Ellipse)
        self.assertIsInstance(new_frame.getparent(), inkex.Group)
        self.assertIsInstance(new_frame.getparent()[1], inkex.Rectangle)
        self.assertIsInstance(top_frame, inkex.Ellipse)

        self.assertIn("stroke", top_frame.style)
        self.assertNotIn("stroke", new_frame.style)
        self.assertIn("fill", top_frame.style)
        self.assertEqual(top_frame.style("fill"), None)


class TestFrameInOut(ComparisonMixin, TestCase):
    """Test some full runs of Frame"""

    effect_class = Frame
    comparisons = [
        (  # test relative offset
            "--id=c2",
            "--id=c1",
            "--type=ellipse",
            "--offset_relative=10",
            "--fill_color=#AAA",
            "--stroke_color=#F00",
            "--z_position=split",
        ),
        (  # test absolute offset
            "--id=c2",
            "--id=c1",
            "--type=ellipse",
            "--offset_absolute=20",
            "--fill_color=#AAA",
            "--stroke_color=#F00",
            "--z_position=split",
        ),
        (  # test asgroup
            "--id=c2",
            "--id=c1",
            "--id=p1",
            "--type=rect",
            "--group=True",
            "--asgroup=True",
            "--offset_relative=-10",
            "--fill_color=#AAA",
            "--stroke_color=#F00",
            "--z_position=bottom",
            "--clip=True",
        ),
        (  # test text bbox querying
            "--id=t3",
            "--type=rect",
            "--offset_relative=10",
            "--fill_color=#AAA",
            "--stroke_color=#F00",
            "--z_position=split",
        ),
        (  # Test clip on a transformed element
            "--id=u1",
            "--fill_color=#AAA",
            "--stroke_color=#F00",
            "--clip=True",
        ),
    ]
