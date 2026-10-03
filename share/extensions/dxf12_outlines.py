#!/usr/bin/env python3
# coding=utf-8
#
# Copyright (C) 2005,2007 Aaron Spike, aaron@ekips.org
# - template dxf_outlines.dxf added Feb 2008 by Alvin Penner, penner@vaxxine.com
# - layers, transformation, flattening added April 2008 by Bob Cook, bob@bobcookdev.com
# - added support for dxf R12, Nov. 2008 by Switcher
# - brought together to replace ps2edit version 2018 by Martin Owens
# - added support for colors and refactored by Joshu Coats, joshu@fearchar.net
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
#

from enum import Enum, IntEnum
from typing import IO, Union, Iterable

import inkex
import inkex.units
from inkex.bezier import cspsubdiv
from inkex.localization import inkex_gettext as _

from dxf_input import COLORS as DXF_COLORS

R12_HEADER = """ 0 
SECTION
 2 
HEADER
 9 
$ACADVER
 1 
AC1009
 9 
$EXTMIN
 10 
 0 
 20 
 0 
 9 
$EXTMAX
 10 
 8.5 
 20 
 11 
 0 
ENDSEC
 0 
SECTION
 2 
ENTITIES
"""

R12_FOOTER = """ 0 
ENDSEC
 0 
EOF"""


# DXF12 colors, derived from dxf_input.py, as (r,g,b) tuples
COLORS: dict[tuple[int, int, int], int] = {
    tuple(inkex.Color(hex_code)): index
    for index, hex_code in enumerate(DXF_COLORS)
    if hex_code != "PAD"
}


class Group(IntEnum):
    """DXF12 Group Codes"""

    ENTITY_START = 0
    LAYER_NAME = 8
    START_X = 10
    START_Y = 20
    COLOR = 62
    ENTITIES_FOLLOW = 66
    POLYLINE_FLAG = 70
    VERTEX_FLAG = 70


class PolylineFlag(IntEnum):
    none = 0
    closed = 1


class Entity(str, Enum):
    """DXF12 Entities"""

    POINT = "POINT"
    POLYLINE = "POLYLINE"
    VERTEX = "VERTEX"
    SEQEND = "SEQEND"

    def __str__(self) -> str:
        return self.value


def find_closest_color(
    color: tuple[int, int, int], colors: set[tuple[int, int, int]]
) -> tuple[int, int, int]:
    """Select the closest color from the set"""

    def _color_distance(c: tuple[int, int, int]) -> int:
        # No need to square-root the distance because only relative magnitude matters here
        return (c[0] - color[0]) ** 2 + (c[1] - color[1]) ** 2 + (c[2] - color[2]) ** 2

    sorted_colors = sorted(colors, key=_color_distance)
    return sorted_colors[0]


class DxfTwelve(inkex.OutputExtension):
    """
    Create dxf12 output from the svg

    DXF R12 reference is publicly available from AutoDesk:
    https://damassets.autodesk.net/content/dam/autodesk/www/developer-network/platform-technologies/autocad-dxf-archive/acad_r12_dxf.pdf
    """

    def __init__(self) -> None:
        super().__init__()
        self.color_mappings: dict[tuple[int, int, int], int] = {}
        self.handle: int = 255
        self.flatness: float = 0.1
        self._stream: IO

    def dxf_add(self, line: str) -> None:
        self._stream.write(line.encode("utf-8"))

    def dxf_insert_code(self, code: int, value: Union[str, float]) -> None:
        if isinstance(value, float):
            value = format(value, "f")
        self.dxf_add(f"{code}\n{value}\n")

    def dxf_start_entity(self, entity: Entity, layer: str) -> None:
        self.dxf_insert_code(Group.ENTITY_START, str(entity))
        self.dxf_insert_code(Group.LAYER_NAME, layer)

    def dxf_insert_color_code(self, color: inkex.Color) -> None:
        rgb_tuple = tuple(color.to_rgb())
        # If there's no color in the color_mappings dict, default to 256 ("inherit from layer")
        color_code = self.color_mappings.get(rgb_tuple, 256)
        self.dxf_insert_code(Group.COLOR, format(color_code, "d"))

    def dxf_point(
        self, layer: str, center: tuple[float, float], color: inkex.Color
    ) -> None:
        self.dxf_start_entity(Entity.POINT, layer)
        self.dxf_insert_color_code(color)
        self.dxf_insert_code(Group.START_X, center[0])
        self.dxf_insert_code(Group.START_Y, center[1])

    def dxf_polyline(
        self,
        layer: str,
        points: list[tuple[float, float]],
        color: inkex.Color,
        closed: bool,
    ) -> None:
        self.dxf_start_entity(Entity.POLYLINE, layer)
        self.dxf_insert_color_code(color)

        # Other values for the polyline flag are supported, per the docs provided by autodesk.net,
        # but we don't have them implemented here yet.
        polyline_flag = PolylineFlag.none
        if closed:
            polyline_flag |= PolylineFlag.closed
        self.dxf_insert_code(Group.POLYLINE_FLAG, polyline_flag)

        # DXF12's POLYLINEs start with a statement that X=0, Y=0. This is not because they all
        # start at the origin, but rather because in 3D mode, the Z value is used to give initial
        # elevation.
        self.dxf_insert_code(Group.START_X, 0.0)
        self.dxf_insert_code(Group.START_Y, 0.0)
        self.dxf_insert_code(Group.ENTITIES_FOLLOW, 1)
        for x, y in points:
            self.dxf_start_entity(Entity.VERTEX, layer)
            self.dxf_insert_code(Group.START_X, x)
            self.dxf_insert_code(Group.START_Y, y)
            self.dxf_insert_code(Group.VERTEX_FLAG, 0)
        self.dxf_start_entity(Entity.SEQEND, layer)

    def path_to_dxf_polyline(
        self, layer: str, path: inkex.CubicSuperPath, color: inkex.Color, closed: bool
    ) -> None:
        f = self.flatness
        is_flat = 0
        while is_flat < 1:
            try:
                # sub-divide paths in-place
                cspsubdiv(path, self.flatness)
                is_flat = 1
            except Exception:
                f += 0.1

        vertices: list[tuple[float, float]] = [
            vertex[1] for subpath in path for vertex in subpath
        ]
        self.dxf_polyline(layer, vertices, color, closed)

    def path_to_dxf_point(
        self, layer: str, path: inkex.CubicSuperPath, color: inkex.Color
    ) -> None:
        bbox = inkex.Path(path).bounding_box() or inkex.BoundingBox(0, 0)
        x, y = bbox.center
        self.dxf_point(layer, (x, y), color)

    def iter_relevant_nodes(
        self,
    ) -> Iterable[
        Union[
            inkex.PathElement,
            inkex.Rectangle,
            inkex.Circle,
            inkex.Polygon,
            inkex.Polyline,
            inkex.Ellipse,
            inkex.Line,
        ]
    ]:
        for node in self.svg.iterdescendants():
            if not isinstance(
                node,
                (
                    inkex.PathElement,
                    inkex.Rectangle,
                    inkex.Circle,
                    inkex.Polygon,
                    inkex.Polyline,
                    inkex.Ellipse,
                    inkex.Line,
                ),
            ):
                continue
            visible = True
            for parent in node.iterancestors():
                if isinstance(parent, (inkex.ClipPath, inkex.Mask, inkex.Defs)):
                    visible = False
                    break
            if not visible:
                continue
            yield node

    def populate_color_mappings(self, colors: set[tuple[int, int, int]]) -> None:
        """
        Using the set of colors used for paths in the SVG document, generate a mapping from those
        to the DXFv12-compatible colors.
        """

        if len(colors) > len(COLORS):
            raise inkex.AbortExtension(
                _(
                    "DXF12 supports a maximum of 249 distinct colors. Please simplify color "
                    "choices first."
                )
            )

        # Just in case there was something left over in here ...
        self.color_mappings.clear()

        # Also keep track of the colors we've already used
        color_codes_used: set[int] = set()

        # First, find all the colors that have an exact match
        for color in COLORS.keys() & colors:
            color_code = COLORS[color]
            self.color_mappings[color] = color_code
            color_codes_used.add(color_code)

        # Then choose colors from the remaining set for the unmatched ones
        remaining_colors = set(COLORS.keys() - self.color_mappings.keys())
        unmatched_colors = colors - COLORS.keys()
        for color in unmatched_colors:
            closest_color = find_closest_color(color, remaining_colors)
            color_code = COLORS[closest_color]
            self.color_mappings[color] = color_code
            color_codes_used.add(color_code)
            remaining_colors.discard(closest_color)

    def save(self, stream):
        # This extension only supports paths. Everything is converted to path.
        if len(self.svg.xpath("//svg:use|//svg:flowRoot|//svg:text")) > 0:
            self.preprocess(["flowRoot", "text"])

        self._stream = stream
        self.dxf_insert_code(999, '"DXF R12 Output" (www.mydxf.blogspot.com)')
        self.dxf_add(R12_HEADER)

        # Scale, but assume that the viewport is based on mm (dxf expects mm)
        scale = self.svg.scale / inkex.units.convert_unit("1mm", "px")
        h = self.svg.viewbox_height

        colors_used: set[tuple[int, int, int]] = set()

        for node in self.iter_relevant_nodes():
            node_style: inkex.Style = node.cascaded_style()
            colors_used.add(tuple(node_style.get_color("stroke").to_rgb()))

        self.populate_color_mappings(colors_used)

        for node in self.iter_relevant_nodes():
            # TODO: this assumes that all elements are direct descendants of layers
            layer = node.getparent().label

            if layer is None:
                layer = "Layer 1"

            node.transform = node.composed_transform()
            node.transform = (
                inkex.Transform(((scale, 0, 0), (0, -scale, h * scale)))
                @ node.transform
            )
            node_style: inkex.Style = node.cascaded_style()
            color: inkex.Color = node_style.get_color("stroke")

            transformed_path = node.path.transform(node.transform)

            if layer.lower().endswith("drill"):
                self.path_to_dxf_point(layer, transformed_path.to_superpath(), color)
                continue

            for subpath in transformed_path.break_apart():
                closed = any(isinstance(pe, inkex.paths.ZoneClose) for pe in subpath)
                self.path_to_dxf_polyline(layer, subpath.to_superpath(), color, closed)

        self.dxf_add(R12_FOOTER)


if __name__ == "__main__":
    DxfTwelve().run()
