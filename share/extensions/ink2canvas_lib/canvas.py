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
Canvas module for ink2canvas extension
"""

from textwrap import dedent
from random import randint

from inkex import Color, Gradient, Pattern, LinearGradient, RadialGradient, Transform
from inkex.paths import Line, Move, Quadratic, Curve, ZoneClose, AbsolutePathCommand

# pylint: disable=too-many-public-methods, disable=missing-function-docstring


class Canvas:
    """Canvas API helper class"""

    def __init__(self, parent, width, height, context="ctx"):
        self.obj = context
        self.code = []  # stores the code
        self.parent = parent
        self.width = width
        self.height = height
        self.definitions_list = set()
        self.reset_state()
        self.saved_state = self.state
        self.patterns_dict = {}

    def reset_state(self):
        # Store persistent style attributes.
        # Initialized with default values according to MDN
        self.state = {
            "globalAlpha": 1,
            "lineWidth": 1,
            "lineCap": "'butt'",
            "lineJoin": "'miter'",
            "miterLimit": 10,
            "font": "'10px sans-serif'",
            "fillStyle": "'rgb(0, 0, 0, 255)'",
            "strokeStyle": "'rgb(0, 0, 0, 255)'",
            "setLineDash": "[]",
            "residualTransform": Transform(),
        }

    @property
    def residual_transform(self):
        return self.state["residualTransform"]

    @residual_transform.setter
    def residual_transform(self, value: Transform):
        self.state["residualTransform"] = value

    def write(self, text):
        self.code.append("\t" + text.replace("ctx", self.obj) + "\n")

    def output(self):
        html = """
        <!DOCTYPE html>
        <html>
        <head>
            <title>Inkscape Output</title>
        </head>
        <body>
            <canvas id='canvas' width='%d' height='%d'></canvas>
            <script>
            var %s = document.getElementById("canvas").getContext("2d");
            %s
            </script>
        </body>
        </html>
        """
        return dedent(html) % (self.width, self.height, self.obj, "".join(self.code))

    def begin_path(self):
        self.write("ctx.beginPath();")

    @staticmethod
    def get_color(rgb, alpha):
        color = Color(rgb).to_rgb()
        if alpha is not None:
            color.alpha = color.effective_alpha * alpha
        if color.alpha == 1.0:
            color.alpha = None
        return "'{}'".format(str(color.to_rgb()))

    def write_global(self, key, value: str, isfunc=False):
        """Write a global property if necessary,
        and update our cache"""
        if self.state[key] == value:
            return
        if isfunc:
            self.write(f"ctx.{key}({value});")
        else:
            self.write(f"ctx.{key} = {value};")
        self.state[key] = value

    @staticmethod
    def sanitize_id(id):
        id = id.replace("-", "_")
        return id

    def get_unique_id(self, initial):
        initial = self.sanitize_id(initial)
        guess = initial
        while guess in self.definitions_list:
            guess = initial + "_" + str(randint(0, len(self.definitions_list)))
        self.definitions_list.add(guess)
        return guess

    def write_gradient(self, gradient: Gradient, opacity: float) -> str:
        """Creates a gradient object if necessary and writes it to the canvas

        Inkscape heavily makes use of GradientTransform which we don't have in
        HTML5 Canvas. For most cases, we can sort-of fix this by transforming
        the canvas by gradientTransform, and applying the inverse transform to
        the draw commands.
        This doesn't work well for texts, or if both fill and stroke have
        transformed gradients.
        In those cases, we print a warning message into the file; and make sure
        that the object shape is correct (the gradient will not be)"""

        id = self.get_unique_id(gradient.get_id())

        stops, orientation = gradient.stops_and_orientation()

        transform = Transform(orientation.get("gradientTransform"))

        self.transform(transform)

        if self.residual_transform != Transform():
            # TODO we could also split the object into fill & stroke
            self.write(
                "// Transformed gradient both on fill and stroke will look incorrect"
            )

        self.residual_transform = (-transform) @ self.residual_transform

        if isinstance(orientation, LinearGradient):
            self.write(
                "const %s = ctx.createLinearGradient(%f, %f, %f, %f);"
                % (
                    id,
                    orientation.x1(gradient.root),
                    orientation.y1(gradient.root),
                    orientation.x2(gradient.root),
                    orientation.y2(gradient.root),
                )
            )
        elif isinstance(orientation, RadialGradient):
            self.write(
                "const %s = ctx.createRadialGradient(%f, %f, 0, %f, %f, %f);"
                % (
                    id,
                    orientation.fx(gradient.root),
                    orientation.fy(gradient.root),
                    orientation.cx(gradient.root),
                    orientation.cy(gradient.root),
                    orientation.r(gradient.root),
                )
            )
        # Write stops

        gradient.add(stops)
        for stop in stops.stops:
            style = stop.specified_style()
            col = self.get_color(style("stop-color"), style("stop-opacity") * opacity)

            self.write(f"{id}.addColorStop({stop.offset}, {col});")

        return id

    def write_pattern(self, pattern: Pattern):
        id = pattern.get_id()
        if id not in self.patterns_dict:
            self.write(f"// pattern {id} not defined")
            return "'black'"
        self.write(self.patterns_dict[id][1])
        self.write(
            f'const pattern = ctx.createPattern({self.patterns_dict[id][0]}, "repeat")'
        )
        return "pattern"

    def get_fill_stroke_value(self, color_like, opacity) -> str:
        val = None
        if isinstance(color_like, Color):
            val = self.get_color(color_like, opacity)
        if isinstance(color_like, Gradient):
            val = self.write_gradient(color_like, opacity)
        if isinstance(color_like, Pattern):
            val = self.write_pattern(color_like)
        # TODO fill None
        return val

    def set_opacity(self, value: float):
        self.write_global("globalAlpha", value)

    def set_fill(self, fill, opacity):
        val = self.get_fill_stroke_value(fill, opacity)
        self.write_global("fillStyle", val)

    def set_stroke(self, stroke, opacity):
        val = self.get_fill_stroke_value(stroke, opacity)
        self.write_global("strokeStyle", val)

    def set_stroke_width(self, value):
        self.write_global("lineWidth", value)

    def set_stroke_linecap(self, value):
        self.write_global("lineCap", f"'{value}'")

    def set_stroke_linejoin(self, value):
        self.write_global("lineJoin", f"'{value}'")

    def set_stroke_miterlimit(self, value):
        self.write_global("miterLimit", value)

    def set_stroke_dasharray(self, value):
        self.write_global("setLineDash", str(value), True)

    def set_font(self, value):
        self.write_global("font", f'"{value}"')

    def path_command(self, comm: AbsolutePathCommand, target="ctx"):
        comm = comm.transform(self.residual_transform)
        function = {
            Line: "lineTo",
            Move: "moveTo",
            ZoneClose: "closePath",
            Quadratic: "quadraticCurveTo",
            Curve: "bezierCurveTo",
        }[comm.__class__]
        self.write(f"{target}.{function}({', '.join(f'{i:.6f}' for i in comm.args)});")

    def rect(self, x, y, width, height):
        self.write("ctx.rect(%f, %f, %f, %f);" % (x, y, width, height))

    def arc(self, x, y, r, angle1, angle2, flag):
        data = (x, y, r, angle1, angle2, flag)
        self.write("ctx.arc(%f, %f, %f, %f, %.8f, %d);" % data)

    def fill_text(self, text, x, y):
        self.write('ctx.fillText("%s", %f, %f);' % (text, x, y))

    def transform(self, transform: Transform):
        if transform == Transform():
            return
        self.write(
            f"ctx.transform({', '.join(f'{i:.6f}' for i in transform.to_hexad())});"
        )

    def save(self):
        self.write("ctx.save();")
        self.saved_state = self.state.copy()

    def restore(self):
        self.write("ctx.restore();")
        self.state = self.saved_state

    def fill(self):
        self.write("ctx.fill();")

    def stroke(self):
        self.write("ctx.stroke();")

    def clip(self, id):
        self.write(f"ctx.clip({id})")

    def create_path2d(self, id):
        self.write(f"let {id} = new Path2D();")

    def draw_image(self, id, href, x, y, width, height):
        id = self.get_unique_id(id)
        self.write(f"const {id} = new Image();")
        self.write(f'{id}.src = "{href}";')
        self.write(f"ctx.drawImage({id}, {x},{y},{width},{height});")
