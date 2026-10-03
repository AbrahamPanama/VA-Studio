# coding=utf-8
"""
Test CubicSuperPath
"""

from inkex.tester import TestCase
from inkex.paths import CubicSuperPath


class CubicPathTest(TestCase):
    def test_LHV(self):
        p = [
            ["M", [1.2, 2.3]],
            ["L", [3.4, 4.5]],
            ["H", [5.6]],
            ["V", [6.7]],
        ]
        csp = CubicSuperPath(p)
        self.assertDeepAlmostEqual(
            csp,
            [
                [
                    [[1.2, 2.3], [1.2, 2.3], [1.2, 2.3]],
                    [[3.4, 4.5], [3.4, 4.5], [3.4, 4.5]],
                    [[5.6, 4.5], [5.6, 4.5], [5.6, 4.5]],
                    [[5.6, 6.7], [5.6, 6.7], [5.6, 6.7]],
                ]
            ],
        )

    def test_CS(self):
        p = [
            ["M", [1.2, 2.3]],
            ["C", [4.5, 3.4, 5.6, 6.7, 8.9, 7.8]],
            ["S", [9.1, 1.2, 2.3, 3.4]],
        ]
        csp = CubicSuperPath(p)
        self.assertDeepAlmostEqual(
            csp,
            [
                [
                    [[1.2, 2.3], [1.2, 2.3], [4.5, 3.4]],
                    [[5.6, 6.7], [8.9, 7.8], [12.2, 8.9]],
                    [[9.1, 1.2], [2.3, 3.4], [2.3, 3.4]],
                ]
            ],
        )

    def test_QT(self):
        p = [
            ["M", [0.0, 0.0]],
            ["Q", [3.0, 0.0, 3.0, 3.0]],
            ["T", [0.0, 6.0]],
        ]
        csp = CubicSuperPath(p)
        self.assertDeepAlmostEqual(
            csp,
            [
                [
                    [[0.0, 0.0], [0.0, 0.0], [2.0, 0.0]],
                    [[3.0, 1.0], [3.0, 3.0], [3.0, 5.0]],
                    [[2.0, 6.0], [0.0, 6.0], [0.0, 6.0]],
                ]
            ],
        )

    def test_AZ(self):
        p = [
            ["M", [0.0, 4.0]],
            ["A", [3.0, 6.0, 0.0, 1, 1, 5.0, 4.0]],
            ["Z", []],
        ]
        csp = CubicSuperPath(p)
        self.assertTrue(len(csp[0]) > 3)
