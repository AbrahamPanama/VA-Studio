#!/usr/bin/env python
# coding=utf-8
#
# Copyright (C) 2020 Ellen Wasboe, ellen@wasbo.net
# Copyright (C) 2025 Jonathan Neuhauser, jonathan.neuhauser@outlook.com
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.

# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor,
# Boston, MA  02110-1301, USA.
"""
Clean up path by removing duplicate nodes and interpolate close nodes.

Optionally:
    join start and end node of each subpath if distance < threshold
    join separate subpaths if end nodes closer than threshold
Joining subpaths can be done either by interpolating or straight line segment.
"""

from dataclasses import dataclass
from typing import Dict, Optional, Tuple, List
import inkex
from inkex.paths import Move, Curve, Line, ZoneClose, Path
from inkex.localization import inkex_gettext as _
from pathmodifier import PathModifier


@dataclass
class SubpathInfo:
    """Base class to store subpath metadata"""

    subpath: Path


@dataclass
class OpenSubpathInfo(SubpathInfo):
    """Subpath metadata for an open subpath"""

    xy_start: complex
    xy_end: complex


@dataclass
class JoinCandidate:
    """Structure for join info: endpoint id / location on
    both subpaths, and whether to reverse any of them.
    The second subpath is always joined to the first"""

    endpoint1: Optional[Tuple[int, int]]  # (path_id, 0 or 1)
    endpoint2: Optional[Tuple[int, int]]
    reverse1: bool
    reverse2: bool


class CleanUpPath(PathModifier):
    """Simplifies a path by joining nearby nodes, deleting empty or very small
    subpaths, closing almost-closed subpaths and joining nearby open subpaths"""

    def add_arguments(self, pars):
        pars.add_argument("--name", default="options")
        pars.add_argument("--minlength", type=float, default=0)
        pars.add_argument("--minUse", type=inkex.Boolean, default=False)
        pars.add_argument("--minlengthSub", type=float, default=0)
        pars.add_argument("--minUseSub", type=inkex.Boolean, default=False)
        pars.add_argument("--maxdistClose", type=float, default=0)
        pars.add_argument("--closeSub", type=inkex.Boolean, default=False)
        pars.add_argument("--maxdistJoin", type=float, default=0)
        pars.add_argument("--joinEndSub", type=inkex.Boolean, default=False)
        pars.add_argument("--allow_reverse", type=inkex.Boolean, default=True)
        pars.add_argument("--option_join", default="1")

    def effect(self):
        """Clean up path."""
        self.min_length = self.options.minlength
        self.min_length_sub = self.options.minlengthSub
        self.max_dist_close = self.options.maxdistClose
        self.max_dist_join = self.options.maxdistJoin
        if self.options.minUse is False:
            self.min_length = 0
        if self.options.minUseSub is False:
            self.min_length_sub = 0
        if self.options.closeSub is False:
            self.max_dist_close = -1
        if self.options.joinEndSub is False:
            self.max_dist_join = -1

        num_patheffect = 0
        for elem in self.svg.selection.filter_nonzero(inkex.PathElement):
            if elem.get("inkscape:path-effect") is None:
                elem.path = self.process_path(elem.path)
            else:
                num_patheffect += 1

        if num_patheffect > 0:
            inkex.errormsg(
                _(
                    f"""{num_patheffect} selected elements have an
                inkscape:path-effect applied. These elements will be
                ignored to avoid confusing results. Apply Paths->Object
                to path (Shift+Ctrl+C) and retry .""",
                )
            )

    def process_path(self, path):
        """Apply the changes to a single path"""
        # Lossless simplification first
        path = path.to_absolute().to_non_shorthand()
        path = self.remove_minlength_nodes(path)
        if self.max_dist_close > -1:
            path = self.close_subpaths(path)
        if self.max_dist_join > -1:
            path = self.join_subpaths(path)
        return path

    def remove_minlength_nodes(self, path: inkex.Path) -> inkex.Path:
        """Combine nearby nodes (closer than min_length),
        and delete subpaths smaller than min_length_sub"""
        newpath = []
        for subpath in path.subpath_iterator():
            newsub: List[inkex.paths.AbsolutePathCommand] = []
            subpath_length = 0
            subit = subpath.proxy_iterator()
            while True:
                lengthsum = 0
                collected = []
                while True:
                    cand = next(subit, None)

                    if cand is None:  # We hit the end of the path
                        break
                    if cand.letter in "MZ":
                        break
                    length = cand.length()
                    subpath_length += length
                    if lengthsum + length <= self.min_length:
                        collected.append(cand.command)
                        lengthsum += length
                    else:
                        break
                cand_to_curve = False
                if len(collected) > 0:
                    if newsub[-1].letter != "M":
                        # TODO This could use proper re-discretisation of the
                        # collected commands
                        # For now we just modify the endpoint of the last command.
                        *_, last = Path(newsub).proxy_iterator()
                        tmp = last.to_curve()
                        if (
                            cand is None or cand.letter in "MZ"
                        ) and lengthsum != self.min_length:
                            # We preserve the end point, and use the control points
                            # of the last added bezier
                            end = collected[-1].cend_point(0j, 0j)
                        else:
                            # set the previous_end as
                            # 1/2 * (previous_end + last_of_sequence_end)
                            end = 0.5 * (tmp.arg3 + collected[-1].cend_point(0j, 0j))
                            cand_to_curve = abs(end - tmp.arg3) > 1e-6

                        newsub[-1] = Curve(
                            tmp.arg1,
                            tmp.arg2,
                            end,
                        )

                if cand is not None and (
                    (len(newsub) > 0 and newsub[-1].letter == "M")
                    or cand.command not in collected
                ):
                    if cand_to_curve:
                        newsub.append(cand.to_curve())
                    else:
                        newsub.append(cand.command)

                if cand is None:
                    break

            if subpath_length > self.min_length_sub:
                newpath.extend(newsub)
        return Path(newpath)

    def build_subpaths_list(self, path: Path) -> List[SubpathInfo]:
        """Build a list of subpaths with their start and end points."""
        subpaths = []

        for sub in path.subpath_iterator():
            subpath = Path(sub)
            if isinstance(sub[-1], ZoneClose):
                info = SubpathInfo(subpath=subpath)
            else:
                xy_start = sub[0].arg1
                xy_end = sub[-1].cend_point(
                    0 + 0j, 0 + 0j
                )  # assuming absolute coordinates
                info = OpenSubpathInfo(
                    subpath=subpath, xy_start=xy_start, xy_end=xy_end
                )
            subpaths.append(info)

        return subpaths

    def close_subpaths(self, path: inkex.Path) -> inkex.Path:
        """Close all subpaths where the endpoints are less than
        max_dist_close away from each other"""
        newp: List[inkex.paths.AbsolutePathCommand] = []
        for sub in self.build_subpaths_list(path):
            if isinstance(sub, OpenSubpathInfo):
                if abs(sub.xy_start - sub.xy_end) < self.max_dist_close:
                    newp.extend(self.join_sub(sub.subpath, sub.subpath))
                    continue
            newp.extend(sub.subpath)
        return Path(newp)

    def find_join_pairs(
        self, path_points: Dict[int, Tuple[complex, complex]]
    ) -> List[JoinCandidate]:
        candidates = []

        def _join_candidate(
            ep1: Tuple[int, int], ep2: Tuple[int, int], rev1: bool, rev2: bool
        ):
            if ep1 != ep2:
                if (
                    abs(path_points[ep1[0]][ep1[1]] - path_points[ep2[0]][ep2[1]])
                    < self.max_dist_join
                ):
                    yield JoinCandidate(ep1, ep2, rev1, rev2)

        for id1, _ in path_points.items():
            for id2, _ in path_points.items():
                if id1 >= id2:
                    continue
                candidates += _join_candidate((id1, 1), (id2, 0), False, False)
                candidates += _join_candidate((id2, 1), (id1, 0), False, False)
                if self.options.allow_reverse:
                    candidates += _join_candidate((id1, 1), (id2, 1), False, True)
                    candidates += _join_candidate((id1, 0), (id2, 0), True, False)

        return candidates

    def join_subpaths(self, path: inkex.Path) -> inkex.Path:
        """Join subpaths based on proximity of start and end points."""
        # First build an array of start and end coordinates
        subpaths: List[Optional[SubpathInfo]] = list(self.build_subpaths_list(path))
        path_points = {
            id: (s.xy_start, s.xy_end)
            for id, s in enumerate(subpaths)
            if isinstance(s, OpenSubpathInfo)
        }

        candidates = self.find_join_pairs(path_points)

        def replace_in_candidates(old: Tuple[int, int], new: Optional[Tuple[int, int]]):
            """Replace a tuple element."""
            for cand in candidates:
                if cand.endpoint1 == old:
                    cand.endpoint1 = new
                if cand.endpoint2 == old:
                    cand.endpoint2 = new

        def flip_candidates(subpath_id: int):
            for cand in candidates:
                if cand.endpoint1 is None or cand.endpoint2 is None:
                    continue
                if cand.endpoint1[0] == subpath_id:
                    cand.reverse1 = not cand.reverse1
                if cand.endpoint2[0] == subpath_id:
                    cand.reverse2 = not cand.reverse2

        def maybe_flip(path, flip):
            return path.reverse() if flip else path

        while candidates:
            c = candidates.pop(0)
            pt1 = c.endpoint1
            pt2 = c.endpoint2
            flip1 = c.reverse1
            flip2 = c.reverse2

            if pt1 is None or pt2 is None:
                # One of the endpoints don't exist anymore, skip this candidate
                continue
            id1, end1 = pt1
            id2, end2 = pt2

            if id1 == id2:
                flip1 = flip2 = False
            entry1 = subpaths[id1]
            entry2 = subpaths[id2]
            if entry1 is None or entry2 is None:
                raise inkex.AbortExtension(
                    "Something went wrong while trying to figure out "
                    "connections between subpaths. Please report your file."
                )

            path1 = maybe_flip(entry1.subpath, flip1)
            path2 = maybe_flip(entry2.subpath, flip2)
            joined = self.join_sub(path1, path2)
            entry1.subpath = joined
            if id2 != id1:
                subpaths[id2] = None

            # Update the mapping for the endpoints
            # Those endpoints do not exist anymore
            # Update the candidates
            replace_in_candidates((id1, end1), None)
            replace_in_candidates((id2, end2), None)
            # If we have flipped a path, we need to change the
            # flip attribute on other candidates
            if flip1:
                flip_candidates(id1)
            if flip2:
                flip_candidates(id2)

            # Those endpoints are on different subpaths now
            replace_in_candidates((id1, 1 - end1), (id1, 0))
            replace_in_candidates((id2, 1 - end2), (id1, 1))
        return Path(sum([i.subpath for i in subpaths if i is not None], []))

    def join_sub(self, sub1: inkex.Path, sub2: inkex.Path) -> inkex.Path:
        """Join Path() by interpolation or straight line segment."""
        same = sub1 is sub2
        if self.options.option_join == "1":
            # generate new interpolated join node from end/start nodes
            *_, last = sub1.proxy_iterator()
            tmp = last.to_curve()
            join = 0.5 * (tmp.arg3 + sub2[0].arg1)
            sub1[-1] = Curve(tmp.arg1, tmp.arg2, join)
            _, first, *_ = sub2.proxy_iterator()
            sub2[1] = first.to_curve()

            if same:
                sub1[0] = Move(join)

        elif self.options.option_join == "2" and not same:
            # straight line
            sub2.insert(1, Line(sub2[0].arg1))

        if same:
            sub1.append(ZoneClose())
            return sub1

        return sub1 + sub2[1:]


if __name__ == "__main__":
    CleanUpPath().run()
