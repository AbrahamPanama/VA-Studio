# coding=utf-8
#
# Copyright (C) 2020-2021 Jonathan Neuhauser, jonathan.neuhauser@outlook.com
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
"""Test interpolation inkex module functionality"""

from inkex.tester import TestCase

import inkex
import inkex.tween as tween
import pytest
import numpy as np


class TweenTest(TestCase):
    """Unit tests for the Inkscape inkex tween library"""

    black = inkex.Color("#000000")
    grey50 = inkex.Color("#080808")
    white = inkex.Color("#111111")

    def test_interpcoord(self):
        val = tween.interpcoord(0, 1, 0.5)
        assert val == pytest.approx(0.5, 1e-3)

    def test_interppoints(self):
        val = tween.interppoints((0, 0), (1, 1), 0.5)
        assert val == pytest.approx((0.5, 0.5), (1e-3, 1e-3))

    def initialize_gradient(self, lg, stoparray):
        for key, value in stoparray.items():
            stop = inkex.Stop()
            stop.style = inkex.Style()
            stop.style["stop-color"] = value
            stop.offset = key
            lg.add(stop)

    def initialize_linear_gradient(self, bounding_box, stoparray):
        lg = inkex.LinearGradient()
        self.initialize_gradient(lg, stoparray)
        lg.set("x1", bounding_box.left)
        lg.set("x2", bounding_box.right)
        lg.set("y1", bounding_box.center.y)
        lg.set("y2", bounding_box.center.y)
        return lg

    def initialize_radial_gradient(self, bounding_box, stoparray):
        grad = inkex.RadialGradient()
        self.initialize_gradient(grad, stoparray)
        grad.set("cx", bounding_box.center.x)
        grad.set("cy", bounding_box.center.y)
        grad.set("fx", bounding_box.center.x)
        grad.set("fy", bounding_box.center.y)
        grad.set("r", bounding_box.right - bounding_box.center.x)
        return grad

    def test_fill_interpolator(self):
        svg = inkex.SvgDocumentElement()
        p1 = inkex.PathElement(
            d="M 5.23564,33.586285 46.410969,-0.94834 89.873815,35.045501 Z"
        )
        p2 = inkex.PathElement(
            d="m 136.32372,31.390088 c 0,0 -65.213764,-4.27631 18.17433,-22.4506304 83.38809,-18.17434 64.14469,35.2795704 64.14469,35.2795704 z"
        )
        p3 = inkex.Rectangle()
        svg.add(p1, p2, p3)
        g1 = self.initialize_linear_gradient(
            p1.bounding_box(), {0: "#ff0000", 0.5: "#00ff00", 1: "#0000ff"}
        )
        g2 = self.initialize_linear_gradient(
            p2.bounding_box(),
            {0: "#ff0000", 0.25: "#ff0000", 0.75: "#0000ff", 1: "#0000ff"},
        )
        g3 = self.initialize_radial_gradient(
            p2.bounding_box(), {0: "#ff0000", 0.5: "#00ff00", 1: "#0000ff"}
        )
        grad1 = tween.GradientInterpolator.append_to_doc(svg, g1)
        grad2 = tween.GradientInterpolator.append_to_doc(svg, g2)
        grad3 = tween.GradientInterpolator.append_to_doc(svg, g3)
        expected = [
            [
                {"fill": "#640000"},
                {"fill": "#320000"},
                {"fill": [75, 0, 0]},
            ],  # both only fill
            [
                {"fill": "none"},
                {"fill": "#320000"},
                {"fill": [50, 0, 0], "fill-opacity": 0.5},
            ],  # interpolate via fill-opacity
            [{"fill": "none"}, {}, {"fill": [0, 0, 0]}],  # only one fill set to None
            [
                {"fill": "#00ff00"},
                {"fill": grad1},
                {
                    "fill/grad/stops": [
                        [0, "#808000"],
                        [0.5, "#00ff00"],
                        [1, "#008080"],
                    ],
                    "fill/grad/x1": "5.23564px",
                },
            ],
            [
                {"fill": "#00ff00"},
                {"fill": grad3},
                {
                    "fill/grad/stops": [
                        [0, "#808000"],
                        [0.5, "#00ff00"],
                        [1, "#008080"],
                    ],
                    "fill/grad/cx": "106.932px",
                },
            ],
            [{"fill": grad2}, {"fill": grad3}, {"fill": grad2}],
            [
                {"fill": grad1},
                {"fill": grad2},
                {
                    "fill/grad/stops": [
                        [0, "#ff0000"],
                        [0.25, "#c04000"],
                        [0.5, "#408040"],
                        [0.75, "#0040c0"],
                        [1, "#0000ff"],
                    ]
                },
            ],
            [
                {"fill": "none"},
                {"fill": grad1},
                {
                    "fill/grad/stops": [
                        [0, "#ff0000"],
                        [0.5, "#00ff00"],
                        [1, "#0000ff"],
                    ],
                    "fill/grad/x1": "5.23564px",
                },
            ],
        ]

        for sstyle, estyle, interpstyle in expected:
            p1.style = inkex.Style()
            p1.style.update(sstyle)
            p2.style = inkex.Style()
            p2.style.update(estyle)

            interpolator = tween.StyleInterpolator(p1, p2)
            p3.style = interpolator.interpolate(0.5)
            for key, value in interpstyle.items():
                keys = key.split("/")
                if keys[0] == "fill":
                    newfill = p3.style("fill")
                    if key == "fill":
                        assert newfill == value
                    elif keys[1] == "grad":
                        assert isinstance(
                            newfill, (inkex.LinearGradient, inkex.RadialGradient)
                        )
                        if keys[2] == "stops":
                            assert len(value) == len(newfill.stops)
                            for idx, _ in enumerate(value):
                                assert (
                                    newfill.stops[idx].style["stop-color"]
                                    == value[idx][1]
                                )
                                self.assertAlmostEqual(
                                    float(newfill.stop_offsets[idx]),
                                    value[idx][0],
                                    1e-3,
                                )
                        else:
                            assert newfill.get(keys[2]) == value
                else:
                    assert p3.style(key) == value

    def test_path_interpolation(self):
        p1 = inkex.Path("M 5.23564,33.586285 46.410969,-0.94834 89.873815,35.045501 Z")
        p2 = inkex.Path(
            "m 136.32372,31.390088 c 0,0 -65.213764,-4.27631 18.17433,-22.4506304 83.38809,-18.17434 64.14469,35.2795704 64.14469,35.2795704 z"
        )
        pel1 = inkex.PathElement()
        pel2 = inkex.PathElement()
        pel1.path = p1
        pel2.path = p2
        result_method_1 = "M 70.7798 32.4882 C 70.7798 32.4882 58.7606 13.0827 100.455 3.99558 C 142.149 -5.09157 154.258 39.6323 154.258 39.6323 Z"
        result_method_2 = "M 70.7798 32.4882 C 70.7798 32.4882 59.6785 13.1429 98.726 4.37785 C 99.2875 4.25181 100.254 4.44741 101.52 4.87808 C 126.751 3.94755 151.064 21.8659 153.864 27.4179 C 156.66 32.9612 150.143 39.5613 144.479 39.4637 C 131.98 39.2482 70.7798 32.4882 70.7798 32.4882"
        calls = [
            [None, result_method_1],
            [tween.FirstNodesInterpolator, result_method_1],
            [tween.EqualSubsegmentsInterpolator, result_method_2],
        ]

        for arg, expected in calls:
            interp = tween.AttributeInterpolator.create_from_attribute(
                pel1, pel2, "d", method=arg
            )
            result = interp.interpolate(0.5)
            print(result)
            assert np.allclose(result, inkex.CubicSuperPath(inkex.Path(expected)))
