# coding=utf-8
"""
Test Inkex tester functionality
"""

from inkex.tester import TestCase
from inkex.tester.xmldiff import xmldiff


class TesterTest(TestCase):
    """Ironic"""

    maxDiff = 20000

    def get_file(self, filename):
        """Get the contents of a file"""
        with open(self.data_file("svg", filename), "rb") as fhl:
            return fhl.read()

    def test_xmldiff(self):
        """XML Diff"""
        xml_a = self.get_file("shapes.svg")
        xml_b = self.get_file("diff.svg")

        xml, delta = xmldiff(xml_a, xml_b)
        self.assertFalse(delta)
        self.assertEqual(str(delta), "9 xml differences")
        # self.assertEqual(str(xml), '')
        xml, delta = xmldiff(xml_a, xml_a)
        self.assertTrue(delta)
        self.assertEqual(str(delta), "No differences detected")
