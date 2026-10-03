# Copyright (C) 2024 Jonathan Neuhauser <jonathan.neuhauser@outlook.com>
# SPDX-License-Identifier: GPL-2.0-or-later

import os
import sys

from inkaf.parser.types import AFDictObject, AFListObject


HERE = os.path.dirname(__file__) or "."
# This is suggested by https://docs.python-guide.org/writing/structure/.
sys.path.insert(0, os.path.abspath(os.path.join(HERE, "..")))


def assert_remove_parents(result, expected):
    """Remove parents from AFObjects and compare"""

    def _remove_parents(entry):
        if isinstance(entry, AFDictObject):
            for field in entry.fields.values():
                field.parent = None
        if isinstance(entry, AFListObject):
            for field in entry.fields:
                field.parent = None

    current = result.value
    if isinstance(current, list):
        for entry in current:
            _remove_parents(entry)
    else:
        _remove_parents(current)

    assert result.value == expected
