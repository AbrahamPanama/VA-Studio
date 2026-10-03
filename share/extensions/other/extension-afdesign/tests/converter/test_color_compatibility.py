# SPDX-FileCopyrightText: 2026 Inkscape contributors
# SPDX-License-Identifier: GPL-2.0-or-later

"""Keep historical Affinity colors when using the current inkex color API."""

import unittest

from inkaf.svg.fill import AFColor, AFColorSpace


class TestColorCompatibility(unittest.TestCase):
    def test_hsl_normalized_channels_and_alpha(self):
        value = AFColor([0.2, 0.5, 0.8], 0.25, AFColorSpace.HSL).to_rgb()
        for actual, expected in zip(value.color, [0.86, 0.9, 0.7]):
            self.assertAlmostEqual(actual, expected)
        self.assertEqual(value.alpha, 0.25)
        self.assertEqual(value.space, AFColorSpace.RGB)

    def test_hsl_achromatic(self):
        value = AFColor([0.5, 0.0, 0.2], 1.0, AFColorSpace.HSL).to_rgb()
        self.assertEqual(value.color, [0.2, 0.2, 0.2])

    def test_midpoint_truncation_not_rounding(self):
        start = AFColor([0.0, 0.0, 0.0], 0.0, AFColorSpace.RGB)
        end = AFColor([1.0, 1.0, 1.0], 1.0, AFColorSpace.RGB)
        midpoint = start.interpolate(end, 0.5)
        self.assertEqual(midpoint.color, [127 / 255] * 3)
        self.assertEqual(midpoint.alpha, 0.5)
        self.assertEqual(midpoint.space, AFColorSpace.RGB)

    def test_integer_and_float_endpoint_semantics(self):
        start = AFColor([1, 0, 255], 0.0, AFColorSpace.RGB)
        end = AFColor([1.0, 0.0, 1.0], 1.0, AFColorSpace.RGB)
        self.assertEqual(start.interpolate(end, 0.0).color, [1 / 255, 0.0, 1.0])
        self.assertEqual(start.interpolate(end, 1.0).color, [1.0, 0.0, 1.0])

    def test_truncate_endpoints_before_interpolation(self):
        start = AFColor([0.2, 0.5, 0.8], 0.25, AFColorSpace.RGB)
        end = AFColor([0.8, 0.5, 0.2], 0.75, AFColorSpace.RGB)
        midpoint = start.interpolate(end, 0.5)
        self.assertEqual(midpoint.color, [127 / 255] * 3)
        self.assertEqual(midpoint.alpha, 0.5)

    def test_does_not_mutate_input_channels(self):
        start = AFColor([0.2, 0.5, 0.8], 0.25, AFColorSpace.RGB)
        end = AFColor([0.8, 0.5, 0.2], 0.75, AFColorSpace.RGB)
        start.interpolate(end, 0.5)
        self.assertEqual(start.color, [0.2, 0.5, 0.8])
        self.assertEqual(end.color, [0.8, 0.5, 0.2])


if __name__ == "__main__":
    unittest.main()
