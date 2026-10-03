# Copyright (C) 2024 Jonathan Neuhauser <jonathan.neuhauser@outlook.com>
# SPDX-License-Identifier: GPL-2.0-or-later

from inkaf.parser.extract import undo_cumulative_sum


def test_undo_cumulative_sum():
    input = bytearray()
    input.extend([10, 246, 255, 20, 254])
    data = undo_cumulative_sum(bytes(input))
    expected = bytearray()
    expected.extend([10, 0, 255, 19, 17])
    assert data == expected
