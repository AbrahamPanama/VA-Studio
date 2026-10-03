# coding=utf-8
"""Test base inkex module functionality"""

from __future__ import absolute_import, print_function, unicode_literals
from pathlib import Path
import warnings

from inkex.deprecated import _deprecated
from inkex.tester import TestCase


class DeprecatedTests(TestCase):
    """Test ways in which we deprecate code"""

    maxDiff = 10000

    def assertDeprecated(self, call, msg, *args, **kwargs):  # pylint: disable=invalid-name
        """Catch deprecation warnings and test their output"""
        with warnings.catch_warnings(record=True) as warns:
            warnings.simplefilter("always")
            call(*args, **kwargs)
            if msg is None:
                self.assertFalse(warns, "Expected no warnings, got warnings!")
            else:
                self.assertTrue(warns, "No warning was returned, expected warning!")
                if msg is not False:
                    self.assertEqual(
                        str(warns[0].category.__name__), "DeprecationWarning"
                    )
                return warns[0]

    def test_warning(self):
        """What happens when we deprecate things"""
        self.assertDeprecated(_deprecated, None, "", stack=0, level=0)
        self.assertDeprecated(_deprecated, "FOO", "FOO", stack=0, level=1)

    def test_traceback(self):
        """Traceback is possible for deprecation warnings"""
        warn = self.assertDeprecated(_deprecated, False, "BAR", stack=0, level=2)
        self.assertIn(str(Path("inkex") / "deprecated"), str(warn.message))
        self.assertIn("test_inkex_deprecated.py", str(warn.message))
