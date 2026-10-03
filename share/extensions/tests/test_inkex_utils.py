# coding=utf-8
"""
Unit test file for ../inkex.py
"""

# Revision history:
#   * 2012-01-27 (jazzynico): check errormsg function.
#
from __future__ import absolute_import, print_function

from argparse import ArgumentTypeError

import pytest

from inkex.utils import (
    debug,
    errormsg,
    filename_arg,
    Boolean,
    parse_percent,
    to,
    strargs,
    math_eval,
    is_number,
)
from inkex.tester import TestCase

from inkex import addNS


class TestInkexBasic(object):
    """Test basic utiltiies of inkex"""

    def test_boolean(self):
        """Inkscape boolean input"""
        assert Boolean("TRUE") is True
        assert Boolean("true") is True
        assert Boolean("True") is True
        assert Boolean("FALSE") is False
        assert Boolean("false") is False
        assert Boolean("False") is False
        assert Boolean("Banana") is None

    def test_debug(self, capsys):
        """Debug messages go to stderr"""
        debug("Hello World")
        assert capsys.readouterr().err == "Hello World\n"

    def test_to(self):
        """Decorator for generators"""

        @to(list)
        def mylist(a, b, c):
            """Yield as a list"""
            yield a
            yield c
            yield b

        assert isinstance(mylist(1, 2, 3), list)
        assert mylist(1, 2, 3) == [1, 3, 2]

        @to(dict)
        def mydict(a, b, c):
            """Yield as a dictionary"""
            yield ("age", a)
            yield ("name", c)
            yield ("home", b)

        assert isinstance(mydict(1, 2, 3), dict)
        assert mydict(1, 2, 3) == {"age": 1, "name": 3, "home": 2}

    def test_filename(self):
        """Filename argument input"""
        assert filename_arg(__file__) == __file__
        with pytest.raises(ArgumentTypeError):
            filename_arg("doesntexist.txt")

    def test_add_ns(self):
        """Test addNS function"""
        assert (
            addNS("inkscape:foo") == "{http://www.inkscape.org/namespaces/inkscape}foo"
        )
        assert (
            addNS("bar", "inkscape")
            == "{http://www.inkscape.org/namespaces/inkscape}bar"
        )
        assert addNS("url", "rdf") == "{http://www.w3.org/1999/02/22-rdf-syntax-ns#}url"
        assert (
            addNS("{http://www.inkscape.org/namespaces/inkscape}bar")
            == "{http://www.inkscape.org/namespaces/inkscape}bar"
        )
        assert (
            addNS("http://www.inkscape.org/namespaces/inkscape:bar")
            == "{http://www.inkscape.org/namespaces/inkscape}bar"
        )
        assert (
            addNS("car", "http://www.inkscape.org/namespaces/inkscape")
            == "{http://www.inkscape.org/namespaces/inkscape}car"
        )
        assert (
            addNS("{http://www.inkscape.org/namespaces/inkscape}bar", "rdf")
            == "{http://www.w3.org/1999/02/22-rdf-syntax-ns#}bar"
        )

    def test_strargs(self):
        """Test strargs function"""
        assert strargs("1.0 2.0 3.0 4.0") == [1.0, 2.0, 3.0, 4.0]
        assert strargs("1 -2 3 -4") == [1.0, -2.0, 3.0, -4.0]
        assert strargs("1,-2,3,-4") == [1.0, -2.0, 3.0, -4.0]
        assert strargs("1-2 3-4") == [1.0, -2.0, 3.0, -4.0]
        assert strargs("1-2,3-4") == [1.0, -2.0, 3.0, -4.0]
        assert strargs("1-2-3-4") == [1.0, -2.0, -3.0, -4.0]
        assert strargs("+1 +2") == [1.0, 2.0]
        assert strargs("1.2.3.4.5") == [1.2, 0.3, 0.4, 0.5]
        assert strargs("1. 2..3.4") == [1.0, 2.0, 0.3, 0.4]
        assert strargs("1.0e-1 -2.0e-1 3.0e10 -4.0e10") == [
            0.1,
            -0.2,
            30000000000,
            -40000000000,
        ]
        assert strargs("1.0e-1,-2.0e-1,3.0e10,-4.0e10") == [
            0.1,
            -0.2,
            30000000000,
            -40000000000,
        ]
        assert strargs("1.0E-1 -2.0E-1 3.0E10 -4.0E10") == [
            0.1,
            -0.2,
            30000000000,
            -40000000000,
        ]
        assert strargs("1.0E-1,-2.0E-1,3.0E10,-4.0E10") == [
            0.1,
            -0.2,
            30000000000,
            -40000000000,
        ]
        assert strargs("1.E2+.3E+4") == [100, 3000]

    def test_ascii(self, capsys):
        """Parse ABCabc"""
        errormsg("ABCabc")
        assert capsys.readouterr().err == "ABCabc\n"

    def test_nonunicode_latin1(self, capsys):
        # Py2 has issues with unicode in docstrings.   *sigh*
        # """Parse Àûïàèé"""
        errormsg("Àûïàèé")
        assert capsys.readouterr().err, "Àûïàèé\n"

    def test_unicode_latin1(self, capsys):
        # Py2 has issues with unicode in docstrings.   *sigh*
        # """Parse Àûïàèé (unicode)"""
        errormsg("Àûïàèé")
        assert capsys.readouterr().err, "Àûïàèé\n"

    def test_parse_percent(self):
        assert parse_percent("75%") == 0.75
        assert parse_percent("0.75") == 0.75
        assert parse_percent("75") == 75


import math


class TestMathFunctions(TestCase):
    def testExp(self):
        """Test if the math_eval function works"""
        function = "exp(x)"
        f = math_eval(function)
        self.assertAlmostEqual(f(1), math.exp(1))

    def testErf(self):
        """Only available in python3"""
        function = "erf(x)"
        f = math_eval(function)
        self.assertAlmostEqual(f(1), math.erf(1))

    def test_is_number(self):
        self.assertTrue(is_number("155"))
        self.assertTrue(is_number("12.5"))
        self.assertTrue(is_number("12.5e-5"))
        self.assertFalse(is_number("10=3"))
        self.assertFalse(is_number(""))
        self.assertFalse(is_number("a"))
