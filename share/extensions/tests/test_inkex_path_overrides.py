import pytest
from inkex.paths import *


@pytest.mark.parametrize(
    "command",
    [
        Line(1 + 2j),
        line(1 + 2j),
        Horz(1),
        horz(1),
        vert(1),
        Vert(1),
        Curve(1 + 2j, 5 - 3j, 1.5 + 0.4j),
        curve(1 + 2j, 5 - 3j, 1.5 + 0.4j),
        Smooth(5 - 3j, 1.5 + 0.4j),
        Smooth(5 - 3j, 1.5 + 0.4j),
        Quadratic(5 - 3j, 1.5 + 0.4j),
        quadratic(5 - 3j, 1.5 + 0.4j),
        TepidQuadratic(1 + 2j),
        tepidQuadratic(1 + 2j),
    ],
)
def test_overrides(command: PathCommand):
    first = -6 + 16j
    prev = 0 + 5.2j
    prev_prev = 0.3333 + 0.16j
    assert Curve(*command.ccurve_points(first, prev, prev_prev)) == command.to_absolute(
        prev
    ).to_non_shorthand(prev, prev_prev).to_curve(prev, prev_prev)
    assert command.cend_point(first, prev) == command.to_absolute(prev).cend_point(
        first, prev
    )
