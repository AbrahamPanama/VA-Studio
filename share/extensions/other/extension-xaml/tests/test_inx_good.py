import os

from inkex.tester import TestCase
from inkex.tester.inx import InxMixin


class SvgToXamlInxTestCase(InxMixin, TestCase):
    """Check that inx defaults match, and all parameters are there"""

    def test_my_inx_file(self):
        os.chdir("inkxaml")
        self.assertInxIsGood("svg2xaml.inx")
