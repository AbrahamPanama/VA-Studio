#!/usr/bin/env python3
# coding=utf-8
"""
Test the element API base classes and basic functionality
"""

import random
import lxml
import pytest
from lxml import etree
from packaging.version import Version

from inkex.elements import (
    load_svg,
    ShapeElement,
    Group,
    Rectangle,
    Tspan,
    TextElement,
    Line,
)
from inkex.elements._base import NodeBasedLookup, BaseElement
from inkex.transforms import Transform
from inkex.styles import Style
from inkex.utils import FragmentError
from inkex.tester import TestCase
from inkex.tester.svg import svg_file


class FakeShape(ShapeElement):  # pylint: disable=abstract-method
    """A protend shape"""

    tag_name = "fake"


class SvgTestCase(TestCase):
    """Test SVG"""

    source_file = "complextransform.test.svg"

    def setUp(self):
        super().setUp()
        self.svg = svg_file(self.data_file("svg", self.source_file))


class OverridenElementTestCase(SvgTestCase):
    """Test element overriding functionality"""

    def test_tag_names(self):
        """
        Test tag names for custom and unknown tags
        """
        doc = load_svg(
            """
<svg xmlns="http://www.w3.org/2000/svg" xmlns:x="http://x.com/x">
    <g id="good"></g>
    <badsvg id="bad">Unknown SVG tag</badsvg>
    <x:othertag id="ugly"></x:othertag>
</svg>"""
        )
        svg = doc.getroot()

        good = svg.getElementById("good")
        self.assertEqual(good.TAG, "g")
        bad = svg.getElementById("bad")
        self.assertEqual(bad.TAG, "badsvg")
        ugly = svg.getElementById("ugly")
        self.assertEqual(ugly.TAG, "othertag")

    def test_reference_count(self):
        """
        Test inkex.element.BaseElement-derived object type is preserved on adding to group

        See https://gitlab.com/inkscape/extensions/-/issues/81 for details
        """
        grp = Group()
        for _ in range(10):
            rect = Rectangle()
            grp.add(rect)

        for elem in grp:
            self.assertEqual(type(elem), Rectangle)

    def test_class_lookup(self):
        """Can we find element classes with simple xpaths"""
        self.assertEqual(NodeBasedLookup.find_class("svg:rect"), Rectangle)
        self.assertEqual(NodeBasedLookup.find_class("//svg:svg/svg:g"), Group)
        self.assertRaises(KeyError, NodeBasedLookup.find_class, "svg:g[layer]")

    def test_abstract_raises(self):
        """Abstract classes cannot be instantiated"""
        self.assertRaises(NotImplementedError, FakeShape().get_path)
        self.assertRaises(AttributeError, FakeShape().set_path, None)

    def test_add(self):
        """Can add single or multiple elements with passthrough"""
        elem = self.svg.getElementById("D")
        group = elem.add(Group(id="foo"))
        self.assertEqual(group.get("id"), "foo")
        groups = elem.add(Group(id="f1"), Group(id="f2"))
        self.assertEqual(len(groups), 2)
        self.assertEqual(groups[0].get("id"), "f1")
        self.assertEqual(groups[1].get("id"), "f2")

    def test_creation(self):
        """Create elements with attributes"""
        group = Group().update(inkscape__label="Foo")
        self.assertEqual(group.get("inkscape:label"), "Foo")
        group = Group().update(inkscape__label="Bar")
        self.assertEqual(group.label, "Bar")

    def test_tostring(self):
        """Elements can be printed as strings"""
        self.assertEqual(Group().tostring(), b"<g/>")
        elem = Group(id="bar")
        path = elem.add(Tspan(id="foo"))
        elem.transform.add_translate(50, 50)
        path.style["fill"] = "red"
        self.assertEqual(
            elem.tostring(),
            b'<g transform="translate(50, 50)"><tspan id="foo" style="fill:red"/></g>',
        )


class AttributeHandelingTestCase(SvgTestCase):
    """Test how attributes are handled"""

    def test_chained_multiple_attrs(self):
        """Set multiple attributes at a time"""
        group = Group().update(attr1="A", attr2="B").update(attr3="C", attr4="D")
        self.assertEqual(group.get("attr1"), "A")
        self.assertEqual(group.get("attr2"), "B")
        self.assertEqual(group.get("attr3"), "C")
        self.assertEqual(group.get("attr4"), "D")

        # remove attributes, setting them to None
        group.update(attr1=None, attr4=None)

        self.assertEqual(group.get("attr1"), None)
        self.assertEqual(group.get("attr2"), "B")
        self.assertEqual(group.get("attr3"), "C")
        self.assertEqual(group.get("attr4"), None)

        self.assertEqual(group.pop("attr2"), "B")
        self.assertEqual(group.pop("attr3"), "C")

    def test_set_wrapped_attribute(self):
        """Remove wrapped attribute using .set()"""
        group = Group().update(transform=Transform(scale=2))
        self.assertEqual(group.transform.matrix[0][0], 2)
        self.assertEqual(group.transform.matrix[1][1], 2)

        group.update(transform=None)
        self.assertEqual(group.transform, Transform())

    def test_pop_wrapped_attribute(self):
        """Remove wrapped attribute using .pop()"""
        group = Group()

        self.assertEqual(group.pop("transform"), Transform())

        group.update(transform=Transform(scale=2))
        self.assertEqual(group.pop("transform"), Transform(scale=2))
        self.assertEqual(group.pop("transform"), Transform())
        self.assertRaises(AttributeError, getattr, group, "foo")

    def test_pop_regular_attribute(self):
        """Remove wrapped attribute using .pop()"""
        group = Group()

        self.assertEqual(group.get("attr1"), None)

        group.update(attr1="42")
        self.assertEqual(group.pop("attr1"), "42")
        self.assertEqual(group.pop("attr1"), None)

    def test_update_consistant(self):
        """Update doesn't keep callbacks around"""
        elem = self.svg.getElementById("D")
        tr_a = Transform(translate=(10, 10))
        tr_b = Transform(translate=(-20, 15))
        elem.transform = tr_a
        elem.transform = tr_b
        self.assertEqual(str(elem.transform), "translate(-20, 15)")
        tr_a.add_translate(10, 10)
        self.assertEqual(str(elem.transform), "translate(-20, 15)")
        elem.set("transform", None)
        self.assertEqual(elem.get("transform"), None)

    def test_in_place_style(self):
        """Do styles update when we set them"""
        elem = self.svg.getElementById("D")
        elem.style["fill"] = "purple"
        self.assertEqual(elem.get("style"), "fill:purple")
        elem.style = {"stroke-dashoffset": "1"}
        self.assertEqual(elem.get("style"), "stroke-dashoffset:1")
        elem.style = Style(stroke="red")
        self.assertEqual(elem.get("style"), "stroke:red")
        elem.style.update("grape:2;strawberry:nice;")
        self.assertEqual(elem.get("style"), "stroke:red;grape:2;strawberry:nice")

    def test_random_id(self):
        """Test setting a random id"""
        elem = self.svg.getElementById("D")
        elem.set_random_id("Thing")
        self.assertEqual(elem.get("id"), "Thing5815")
        elem.set_random_id("Thing", size=2)
        self.assertEqual(elem.get("id"), "Thing85")
        elem.set_random_id()
        self.assertEqual(elem.get("id"), "path5392")
        # No document root, no random id allowed
        self.assertRaises(FragmentError, elem.copy().set_random_id)

    def test_random_ids(self):
        """Test setting a tree of ids"""
        elem = self.svg.getElementById("D")
        self.svg.set_random_ids(prefix="TreeItem")
        self.assertEqual(self.svg.get("id"), "TreeItem5815")
        self.assertEqual(self.svg[0].get("id"), "TreeItem8555")
        self.assertEqual(elem.get("id"), "TreeItem2036")

    def test_set_id_backlinks(self):
        """Changing an id can update backlinks"""
        elem = self.svg.getElementById("path1")
        rect1 = Rectangle()
        rect1.set("clip-path", "url(#path1)")
        rect2 = Rectangle()
        rect2.set("mask", "url(#path1)")
        elem.addnext(rect1)
        self.svg.getElementById("G").add(rect2)
        elem.set_id("plant54", True)
        self.assertEqual(self.svg.getElementById("G").get("xlink:href"), "#plant54")
        self.assertEqual(self.svg.getElementById("G").href, elem)
        self.assertEqual(
            str(self.svg.getElementById("B").style), "fill:#eee;joker:url(#plant54)"
        )
        self.assertEqual(rect1.get("clip-path"), "url(#plant54)")
        self.assertEqual(rect2.get("mask"), "url(#plant54)")

    def test_set_id_blacklist(self):
        """Test that the set_random_id function respects a blacklist"""
        elem: BaseElement = self.svg.getElementById("D")

        state = random.getstate()
        elem.set_random_id("Thing")
        self.assertEqual(elem.get("id"), "Thing5815")
        random.setstate(state)
        elem.set_random_id("Other", blacklist=["Other5815"])
        self.assertEqual(elem.get("id"), "Other8555")

    def test_get_element_by_name(self):
        """Get elements by name"""
        self.assertEqual(self.svg.getElementByName("Key").get("id"), "K")
        self.assertEqual(self.svg.getElementByName("Elm", "svg:g").get("id"), "L")
        self.assertEqual(self.svg.getElementByName("Mine").get("id"), "M")
        self.assertEqual(self.svg.getElementByName("doesntexist"), None)
        self.assertEqual(self.svg.getElementByName("Key", "rect"), None)

    def test_insensitive(self):
        """Element inkscape sensitivity"""
        elem = self.svg.getElementByName("Key")
        self.assertTrue(elem.is_sensitive())
        elem.set_sensitive(False)
        self.assertFalse(elem.is_sensitive())
        self.assertEqual(elem.get("sodipodi:insensitive"), "true")
        elem.set_sensitive(True)
        self.assertTrue(elem.is_sensitive())
        self.assertEqual(elem.get("sodipodi:insensitive"), None)

    def test_title_description(self):
        """Adding a description to an element"""

        def _content(elem):
            return tuple(f"{child.TAG}:{child.text}" for child in elem)

        self.svg.getElementById("K").title = "Before"
        elem = self.svg.getElementById("L")
        elem.desc = "Dancing"
        elem.title = "Start"
        self.assertEqual(elem.desc, "Dancing")
        self.assertEqual(elem.title, "Start")
        self.svg.getElementById("G").title = "After"
        # Title and Desc elements have been added
        self.assertEqual(_content(elem), ("title:Start", "desc:Dancing", "line:None"))
        # No extra elements created, no conflicts.
        elem.title = "Stop"
        self.assertEqual(_content(elem), ("title:Stop", "desc:Dancing", "line:None"))
        # Deletion works too, try twice so we know it doesn't error on not-found
        del elem.desc
        del elem.desc
        self.assertEqual(_content(elem), ("title:Stop", "line:None"))


class TransformationTestCase(SvgTestCase):
    """Test transformative functions"""

    def test_bounding_box(self):
        """Elements can have bounding boxes"""
        elem = self.svg.getElementById("D")
        self.assertEqual(tuple(elem.bounding_box()), ((60.0, 100.0), (130.0, 170.00)))
        self.assertTrue(elem.bounding_box().center.is_close((80.0, 150.0)))
        self.assertEqual(
            tuple(TextElement(x="10", y="5").bounding_box()), ((10, 10), (5, 5))
        )
        group = Group(elem)
        self.assertEqual(elem.bounding_box(), group.bounding_box())

    def test_transform(self):
        """In-place modified transforms are retained"""
        elem = self.svg.getElementById("D")
        self.assertEqual(str(elem.transform), "translate(30, 10)")
        elem.transform.add_translate(-10, 10)
        self.assertEqual(str(elem.transform), "translate(20, 20)")

    def test_scale(self):
        """In-place scaling from blank transform"""
        elem = self.svg.getElementById("F")
        self.assertEqual(elem.transform, Transform())
        self.assertEqual(elem.get("transform"), None)
        elem.transform.add_scale(1.0666666666666667, 1.0666666666666667)
        self.assertEqual(elem.get("transform"), Transform(scale=1.06667))
        self.assertIn(b"transform", etree.tostring(elem))

    def test_in_place_transforms(self):
        """Do style and transforms update correctly"""
        elem = self.svg.getElementById("D")
        self.assertEqual(type(elem.transform), Transform)
        self.assertEqual(type(elem.style), Style)
        self.assertTrue(elem.transform)
        elem.transform = Transform()
        self.assertEqual(elem.transform, Transform())
        self.assertEqual(elem.get("transform"), None)
        self.assertNotIn(b"transform", etree.tostring(elem))
        elem.transform.add_translate(10, 10)
        self.assertIn(b"transform", etree.tostring(elem))
        elem.transform.add_translate(-10, -10)
        self.assertNotIn(b"transform", etree.tostring(elem))

    def test_composed_transform(self):
        """Calculate every transform down to the other element"""
        elem = self.svg.getElementById("F")
        grandparent = elem.getparent().getparent()
        transform3 = elem.composed_transform(grandparent)
        transform4 = elem.composed_transform()
        self.assertNotEqual(transform3, transform4)


class RelationshipTestCase(SvgTestCase):
    """Test relationships between elements"""

    def test_findall(self):
        """Findall elements in svg"""
        groups = self.svg.findall("svg:g")
        self.assertEqual(len(groups), 1)

    def test_copy(self):
        """Test copying elements"""
        elem = self.svg.getElementById("D")
        cpy = elem.copy()
        self.assertFalse(cpy.getparent())
        self.assertFalse(cpy.get("id"))

    def test_duplicate(self):
        """Test duplicating elements"""
        elem = self.svg.getElementById("D")
        dup = elem.duplicate()
        self.assertTrue(dup.get("id"))
        self.assertNotEqual(elem.get("id"), dup.get("id"))
        self.assertEqual(elem.getparent(), dup.getparent())

    @pytest.mark.skipif(
        Version(lxml.__version__) < Version("5.0.0"), reason="lxml upstream bug"
    )
    def test_duplicate_text(self):
        """Test duplicating text elements, see
        https://gitlab.com/inkscape/extensions/-/issues/480"""
        root = TextElement()
        self.svg.append(root)
        el = Tspan()
        el.text = "inner"
        root.append(el)
        el.tail = "after"
        d = el.duplicate()
        assert el.tail == "after"
        assert d.tail == "after"
        assert d.text == "inner"

    def test_duplicate_group(self):
        """Check that when duplicating a group, all ids are replaced
        (Issue https://gitlab.com/inkscape/extensions/-/issues/379)"""
        elem: BaseElement = self.svg.getElementById("A")
        dup = elem.duplicate()
        elem_ids = [i.get_id() for i in elem.iter() if hasattr(i, "get_id")]
        dup_ids = [i.get_id() for i in dup.iter() if hasattr(i, "get_id")]

        in_both = [i for i in elem_ids if i in dup_ids]

        self.assertListEqual(in_both, [])

    def test_replace_with(self):
        """Replacing nodes in a tree"""
        rect = self.svg.getElementById("E")
        path = rect.to_path_element()
        rect.replace_with(path)
        self.assertEqual(rect.getparent(), None)
        self.assertEqual(path.getparent(), self.svg.getElementById("C"))

    def test_descendants(self):
        """Elements can walk their descendants"""
        self.assertEqual(
            tuple(self.svg.descendants().ids),
            (
                "mydoc",
                "path1",
                "base",
                "metadata7",
                "A",
                "B",
                "C",
                "D",
                "E",
                "F",
                "G",
                "H",
                "I",
                "J",
                "K",
                "L",
                "M",
            ),
        )
        get = self.svg.getElementById
        self.assertEqual(tuple(get("L").descendants().ids), ("L", "M"))
        self.assertEqual(tuple(get("M").descendants().ids), ("M",))

    def test_deep_descendants(self):
        """Create a very deep svg and test getting decendants"""
        svg = '<svg xmlns="http://www.w3.org/2000/svg">'
        for i in range(1000):
            svg += f'<g id="{i}">'
        svg = load_svg(svg + ("</g>" * 1000) + "</svg>").getroot()

        self.assertEqual(
            tuple(svg.descendants().ids), tuple(str(i) for i in range(1000))
        )
        self.assertEqual(
            tuple(svg.getElementById("998").descendants().ids), ("998", "999")
        )
        self.assertEqual(tuple(svg.getElementById("999").descendants().ids), ("999",))

    def test_ancestors(self):
        """Element descendants of elements"""
        get = self.svg.getElementById
        self.assertEqual(tuple(get("M").ancestors().ids), ("L", "K", "A", "mydoc"))
        self.assertEqual(
            tuple(get("M").ancestors(stop_at=[None]).ids), ("L", "K", "A", "mydoc")
        )
        self.assertEqual(tuple(get("M").ancestors(stop_at=[get("K")]).ids), ("L", "K"))
        self.assertEqual(tuple(get("M").ancestors(stop_at=[get("L")]).ids), ("L",))

    def test_deep_ancestors(self):
        """Create a very deep svg and test getting ancestors"""
        svg = '<svg xmlns="http://www.w3.org/2000/svg">'
        for i in range(1000):
            svg += f'<g id="{i}">'
        svg = load_svg(svg + ("</g>" * 1000) + "</svg>").getroot()
        self.assertEqual(
            tuple(svg.getElementById("999").ancestors().ids),
            tuple(str(i) for i in range(998, -1, -1)),
        )

    def test_luca(self):
        """Test last common ancestor"""
        get = self.svg.getElementById
        self.assertEqual(tuple(get("M").ancestors(get("M")).ids), ("L",))
        self.assertEqual(tuple(get("G").ancestors(get("H")).ids), ("C",))
        self.assertEqual(tuple(get("M").ancestors(get("H")).ids), ("L", "K", "A"))

    def test_comment_callback(self):
        """Test that foreign elements can be inserted /removed
        (despite caching)

        Fix for https://gitlab.com/inkscape/extensions/-/issues/587"""

        defs = self.svg.defs
        comment = etree.Comment("Hello World")
        defs.extend([comment])
        defs.append(etree.Element("testelement"))
        defs.remove(comment)
