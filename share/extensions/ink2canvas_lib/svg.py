# coding=utf-8
#
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
Element parsing and context for ink2canvas extensions
"""

from __future__ import unicode_literals
import math
import abc

import inkex
from inkex.localization import inkex_gettext as _
from .canvas import Canvas


# pylint: disable=missing-function-docstring, missing-class-docstring
# pylint: disable=too-few-public-methods


class ElementWrapper(metaclass=abc.ABCMeta):
    def __init__(self, node, ctx):
        self.node = node
        self.ctx: Canvas = ctx

    def get_style(self):
        return self.node.specified_style()

    def set_style(self, style):
        """Translates style properties names into method calls"""
        self.ctx.set_opacity(style("opacity"))
        if style("fill") is not None:
            self.ctx.set_fill(style("fill"), style("fill-opacity"))

        if style("stroke") is not None:
            self.ctx.set_stroke(style("stroke"), style("stroke-opacity"))
            self.ctx.set_stroke_width(
                self.node.root.to_dimensionless(style("stroke-width"))
            )
            self.ctx.set_stroke_miterlimit(style("stroke-miterlimit"))
            self.ctx.set_stroke_linejoin(style("stroke-linejoin"))
            self.ctx.set_stroke_linecap(style("stroke-linecap"))
            self.ctx.set_stroke_dasharray(style("stroke-dasharray"))

    def has_transform(self):
        return self.node.transform != inkex.Transform()

    def get_transform(self):
        return self.node.transform

    def has_clip(self):
        return self.node.get("clip-path") is not None

    def apply_clip(self):
        if self.has_clip():
            clipPath = self.node.root.getElementById(self.node.get("clip-path"))
            id = self.ctx.get_unique_id(clipPath.get_id())
            self.ctx.create_path2d(id)
            if len(clipPath):
                Path.draw_path(clipPath.get_path(), self.ctx, id)
                self.ctx.clip(id)

    def has_gradient(self):
        return isinstance(self.get_style()("fill"), inkex.Gradient) or isinstance(
            self.get_style()("stroke"), inkex.Gradient
        )

    def start(self):
        self.ctx.write(f"\n// #{self.node.get_id()}")
        if self.has_transform() or self.has_clip() or self.has_gradient():
            self.ctx.save()

    def end(self):
        if self.has_transform() or self.has_clip() or self.has_gradient():
            self.ctx.restore()


class ShapeWrapper(ElementWrapper, metaclass=abc.ABCMeta):
    def draw(self):
        style = self.get_style()
        self.ctx.begin_path()
        self.ctx.transform(self.get_transform())  # unpacks argument list
        self.apply_clip()
        self.set_style(style)
        self.draw_shape()
        if style("fill") is not None:
            self.ctx.fill()
        if style("stroke") is not None:
            self.ctx.stroke()

    @abc.abstractmethod
    def draw_shape(self): ...


class G(ElementWrapper):  # pylint: disable=invalid-name
    def draw(self):
        self.ctx.transform(self.get_transform())
        self.apply_clip()


class Rect(ShapeWrapper):
    def draw_shape(self):
        self.node: inkex.Rectangle
        rx = self.node.rx
        ry = self.node.ry
        if rx or ry or self.ctx.residual_transform != inkex.Transform():
            Path.draw_path(self.node.get_path(), self.ctx)
        else:
            self.ctx.rect(
                self.node.left, self.node.top, self.node.width, self.node.height
            )


class Circle(ShapeWrapper):
    def draw_shape(self):
        self.node: inkex.Circle
        if self.ctx.residual_transform != inkex.Transform():
            Path.draw_path(self.node.get_path(), self.ctx)
        else:
            self.ctx.arc(*self.node.center, self.node.radius, 0, math.pi * 2, True)


class Path(ShapeWrapper):
    @staticmethod
    def draw_path(path: inkex.Path, ctx, target="ctx"):
        """Gets the node type and calls the given method"""
        # Draws path commands
        # Make sure we only have Lines and curves (no arcs etc)
        for comm in path.to_absolute().to_non_shorthand().proxy_iterator():
            if comm.letter == "A":
                for sub in comm.to_curves():
                    ctx.path_command(sub, target)
            else:
                ctx.path_command(comm.command, target)

    def draw_shape(self):
        Path.draw_path(self.node.get_path(), self.ctx)


class Line(Path):
    pass


class Ellipse(Path):
    pass


class Polygon(Path):
    pass


class Polyline(Polygon):
    pass


class Text(ElementWrapper):
    def text_helper(self, tspan):
        if tspan is not None:
            return tspan.text
        for ts_cur in tspan:
            return ts_cur.text + self.text_helper(ts_cur) + ts_cur.tail

    def set_text_style(self, style):
        keys = ("font-style", "font-weight", "font-size", "font-family")
        text = []
        for key in keys:
            if key in style:
                text.append(str(style(key)) + ("px" if key == "font-size" else ""))
        self.ctx.set_font(" ".join([i.replace('"', "'") for i in text]))

    def draw(self):
        for tspan in self.node:
            if isinstance(tspan, inkex.TextPath):
                raise ValueError(_("TextPath elements are not supported"))
        style = self.get_style()
        self.ctx.transform(self.get_transform())
        self.apply_clip()
        self.set_style(style)
        self.set_text_style(style)

        if self.ctx.residual_transform != inkex.Transform and self.has_gradient():
            self.ctx.write("// Transformed gradient on text will look wrong")
        self.ctx.transform(self.ctx.residual_transform)

        for tspan in self.node:
            text = self.text_helper(tspan)
            cur_x = float(tspan.get("x").split()[0])
            cur_y = float(tspan.get("y").split()[0])
            self.ctx.fill_text(text, cur_x, cur_y)


class Image(ElementWrapper):
    def draw(self):
        self.ctx.transform(self.get_transform())
        self.apply_clip()

        href = self.node.get("href") or self.node.get("xlink:href")
        # Remove unescaped line breaks
        href = href.replace("\n", "")

        self.ctx.draw_image(
            self.node.get_id(),
            href,
            self.node.get("x"),
            self.node.get("y"),
            self.node.get("width"),
            self.node.get("height"),
        )
