"""Handles imports for unit tests"""

import os
import sys
from lxml import etree

# This is suggested by https://docs.python-guide.org/writing/structure/.
sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))

from inkxaml.tester import Svg2XamlTester
from inkxaml.svg2xaml import XamlOutput

XamlOutput.__module__ = "svg2xaml"
