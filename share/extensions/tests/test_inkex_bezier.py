# coding=utf-8
from inkex.bezier import pointdistance


class TestPointDistance(object):
    def test_points_on_horizontal_line(self):
        p1 = (0, 0)
        p2 = (10, 0)

        assert 10 == pointdistance(p1, p2)
