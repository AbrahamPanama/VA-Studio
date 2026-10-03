#!/usr/bin/env python3
# coding=utf-8
"""
Test all selection code.
"""

from inkex.elements import PathElement, Circle, Rectangle
from inkex.elements._selected import ElementList

from .test_inkex_elements_base import SvgTestCase
from inkex.utils import AbortExtension


class ElementListTestCase(SvgTestCase):
    """Test Element Selections"""

    def setUp(self):
        super().setUp()
        self.svg.selection.set("G", "B", "D", "F")

    def test_creation(self):
        """Creating an elementList"""
        empty = ElementList(self.svg)
        self.assertEqual(tuple(empty.ids), ())
        self.assertEqual(empty.first(), None)
        lst = ElementList(self.svg, "ABC")
        self.assertEqual(tuple(lst.ids), ("A", "B", "C"))

    def test_to_dict(self):
        """test dictionary compact"""
        self.assertEqual(tuple(self.svg.selection.id_dict()), ("G", "B", "D", "F"))

    def test_getitem(self):
        """Can get an item"""
        self.assertEqual(self.svg.selection["B"].xml_path, "/*/*[4]/*[1]")
        self.assertRaises(KeyError, self.svg.selection.__getitem__, "A")

    def test_svg_selection(self):
        """Setting an svg selection"""
        self.assertEqual(tuple(self.svg.selection.ids), ("G", "B", "D", "F"))

    def test_rendering_order(self):
        """Test paint order"""
        items = self.svg.selection.rendering_order()
        self.assertTrue(isinstance(items, ElementList))
        self.assertEqual(tuple(items.ids), ("B", "D", "F", "G"))

    def test_set_nothing(self):
        """Clear existing selection"""
        self.svg.selection.set()
        self.assertEqual(tuple(self.svg.selection), ())

    def test_set_ids(self):
        """Set a new selection element ids"""
        a_to_g = "ABCDEFG"
        self.svg.selection.set(*a_to_g)
        self.assertEqual(tuple(self.svg.selection.ids), tuple(a_to_g))

    def test_set_elements(self):
        """Set a new selection from element objects"""
        a_to_g = "ABCDEFG"
        self.svg.selection.set(*[self.svg.getElementById(eid) for eid in a_to_g])
        self.assertEqual(tuple(self.svg.selection.ids), tuple(a_to_g))
        self.assertRaises(ValueError, self.svg.selection.add, None)
        self.assertRaises(
            ValueError,
            self.svg.selection.__setitem__,
            "A",
            self.svg.getElementById("B"),
        )

    def test_set_xpath(self):
        """Set a new selection from xpath"""
        self.svg.selection.set("//svg:g")
        self.assertEqual(tuple(self.svg.selection.ids), tuple("ABCKL"))

    def test_set_invalid_ids(self):
        """Set invalid ids"""
        self.svg.selection.set("X", "Y", "Z", "A")
        self.assertEqual(tuple(self.svg.selection.ids), ("A",))

    def test_pop_items(self):
        """Can remove items from the ElementList"""
        selection = self.svg.selection
        self.assertEqual(tuple(selection.ids), ("G", "B", "D", "F"))
        ret = selection.pop()
        self.assertEqual(ret.get("id"), "F")
        self.assertEqual(tuple(selection.ids), ("G", "B", "D"))
        selection.pop(0)
        self.assertEqual(tuple(selection.ids), ("B", "D"))
        selection.pop("B")
        self.assertEqual(tuple(selection.ids), ("D",))
        self.assertRaises(KeyError, selection.pop, "B")
        selection.set(*"ABDFH")
        self.assertEqual(tuple(selection.ids), ("A", "B", "D", "F", "H"))
        selection.pop(selection.first())
        self.assertEqual(tuple(selection.ids), ("B", "D", "F", "H"))

    def test_filtering(self):
        """Create a sub-list of selected items"""
        selection = self.svg.descendants()
        new_list = selection.filter(PathElement)
        self.assertEqual(tuple(new_list.ids), ("path1", "D"))

    def test_filternonzero(self):
        """Filter and raise an AbortException if the list is empty"""
        selection = self.svg.descendants()
        new_list = selection.filter(PathElement)
        # default error message
        with self.assertRaisesRegex(AbortExtension, "Circle.*Rectangle"):
            new_list.filter_nonzero(Circle, Rectangle)
        # custom error message
        with self.assertRaisesRegex(AbortExtension, "test string"):
            new_list.filter_nonzero(Circle, error_msg="test string")
        self.assertEqual(new_list, selection.filter_nonzero(PathElement))

    def test_getting_recursively(self):
        """Create a list of children of the given type"""
        selection = self.svg.selection
        selection.set("B")
        self.assertEqual(tuple(selection.ids), ("B",))
        self.assertEqual(tuple(selection.get().ids), tuple("BCDEFGHIJ"))

    def test_get_bounding_box(self):
        """Selection can get a bounding box"""
        self.assertEqual(int(self.svg.selection.bounding_box().width), 540)
        self.assertEqual(int(self.svg.selection.bounding_box().height), 550)

    def test_selecting_weird_ids(self):
        """Selection can contain some chars"""
        selection = self.svg.selection
        self.svg.append(PathElement(id="#asdf"))
        selection.set("#asdf")
        self.assertEqual(tuple(selection.ids), ("#asdf",))
