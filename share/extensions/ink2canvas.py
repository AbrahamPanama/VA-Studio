#!/usr/bin/env python3
# coding=utf-8
# Copyright (C) 2011 Karlisson Bezerra <contact@hacktoon.com>
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
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
#
"""
Save an SVG file into an html canvas file.
"""

from textwrap import dedent
from typing import List
import inkex

import ink2canvas_lib.svg as svg
from ink2canvas_lib.canvas import Canvas


class Html5Canvas(inkex.OutputExtension):
    """Creates a canvas output"""

    def save(self, stream):
        svg_root = self.document.getroot()
        width = self.svg.viewbox_width
        height = self.svg.viewbox_height

        canvas = Canvas(self, width, height)
        self.create_patterns(canvas)
        self.walk_tree(svg_root, canvas)
        stream.write(canvas.output().encode("utf-8"))

    def create_patterns(self, parent_canvas: Canvas):
        patterns: List[inkex.Pattern] = self.svg.xpath("//svg:pattern")
        for pattern in patterns:
            canvas_id = parent_canvas.get_unique_id(pattern.get_id())
            viewbox = pattern.get_viewbox()
            width = viewbox[2] if viewbox else pattern.width
            height = viewbox[3] if viewbox else pattern.height
            canvas = Canvas(self, width, height, f"{canvas_id}_ctx")
            self.walk_tree(pattern, canvas)
            parent_canvas.patterns_dict[pattern.get_id()] = (
                canvas_id,
                dedent(f"""
            // pattern {pattern.get_id()}
                const {canvas_id} = document.createElement("canvas");
                {canvas_id}.width = {canvas.width};
                {canvas_id}.height = {canvas.height};
                const {canvas_id}_ctx = patternCanvas.getContext("2d");
                {"".join(canvas.code)}
            """),
            )

    def _shape_from_node(self, node, canvas):
        """
        Make a canvas shape object for the given node. Returns `None` if
        the node is not an SVG shape element.
        @rtype svg.AbstractShape or NoneType
        """
        if isinstance(node, inkex.Use):
            node = node.unlink()
        elif isinstance(node, inkex.Image):
            node.embed_image(self.options.input_file)
        try:
            prefix, _brace_, command = node.tag.partition("}")
        except AttributeError:
            return None  # skip comments
        if prefix != "{http://www.w3.org/2000/svg":
            return None

        # makes pylint happy
        assert _brace_ == "}"

        cls = getattr(svg, command.capitalize(), None)

        if not (isinstance(cls, type) and issubclass(cls, svg.ElementWrapper)):
            return None

        return cls(node, canvas)

    def walk_tree(self, root, canvas):
        """Walk through the whole svg tree"""

        for node in root:
            elem = self._shape_from_node(node, canvas)
            if elem is None:
                continue
            elem.start()
            try:
                elem.draw()
            except ValueError as error:
                # print out the reason if any element can not be exported
                canvas.write("// " + str(error))
                continue
            self.walk_tree(node, canvas)
            elem.end()


if __name__ == "__main__":
    Html5Canvas().run()
