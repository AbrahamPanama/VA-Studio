# coding=utf-8
from voronoi import Site


class TestVoronoiSiteBasic(object):
    def test_site_basic(self):
        new_site = Site(1, 2)
        assert new_site.x == 1
        assert new_site.y == 2
