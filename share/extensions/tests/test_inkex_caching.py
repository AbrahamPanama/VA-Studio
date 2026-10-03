from copy import deepcopy
import pytest
from lxml import etree
import inkex
from inkex.tester.svg import svg_file


@pytest.fixture(name="shapes")
def shapes_fixture():
    """Return the shapes SVG file"""
    return svg_file("tests/data/svg/shapes.svg")


def test_move_object(shapes: inkex.SvgDocumentElement):
    """Test moving an object in the tree"""
    p1 = shapes.getElementById("p1")
    assert isinstance(p1, inkex.PathElement)
    assert "p1" in shapes.ids
    assert p1.getparent().get_id() == "layer2"

    # Move it into layer1
    layer1 = shapes.getElementById("layer1")
    layer1.append(p1)
    assert "p1" in shapes.ids
    assert p1.getparent().get_id() == "layer1"

    # Pop it out of the document
    p1.getparent().remove(p1)
    assert "p1" not in shapes.ids
    assert p1.get_id() == "p1"
    assert shapes.getElementById("p1") is None

    # Now attach it back
    shapes.getElementById("p2").addprevious(p1)
    assert "p1" in shapes.ids
    assert p1.get_id() == "p1"
    assert p1.getparent().get_id() == "layer2"

    # Now attach it back
    shapes.getElementById("p2").addnext(p1)
    assert "p1" in shapes.ids
    assert p1.get_id() == "p1"
    assert p1.getparent().get_id() == "layer2"

    # Replace p1 with p1
    p1.getparent().replace(p1, p1)
    assert p1.get_id() == "p1"
    assert "p1" in shapes.ids

    # Replace p1 with p1 using replace_with
    p1.replace_with(p1)
    assert p1.get_id() == "p1"
    assert "p1" in shapes.ids

    # Add p1 a second time
    copy = deepcopy(p1)
    assert etree.ElementBase.get(copy, "id") == "p1"
    assert copy.getparent() is None
    layer1.addnext(copy)
    assert copy.get_id() == "p1-1"


def test_move_objects_advanced(shapes: inkex.SvgDocumentElement):
    """Test extend, clear"""
    p1 = shapes.getElementById("p1")
    p2 = shapes.getElementById("p2")

    l1 = shapes.getElementById("layer1")

    c1 = inkex.Circle(attrib={"id": "testcircle"})

    l1.extend([p1, p2, c1])
    for i in ["p1", "p2", "testcircle"]:
        assert i in shapes.ids
        assert shapes.getElementById(i).get_id() == i

    shapes.getElementById("layer2").insert(0, p1)
    assert p1.getparent().get_id() == "layer2"
    assert "p1" in shapes.ids

    # Test clear

    p1.clear()
    assert "p1" not in shapes.ids


def test_extend_iterator(shapes: inkex.SvgDocumentElement):
    """Test for https://gitlab.com/inkscape/extensions/-/issues/597"""
    l1 = shapes.getElementById("layer1")

    p1 = shapes.getElementById("p1")
    p2 = shapes.getElementById("p2")
    it = iter([p1, p2])
    l1.extend(it)
    assert p1 in l1
    assert p2 in l1


def test_error(shapes: inkex.SvgDocumentElement):
    """Assert that the IDs are unchanged if the LXML append function errors out"""

    old_ids = shapes.ids.copy()

    p1 = shapes.getElementById("p1")
    layer2 = p1.getparent()
    with pytest.raises(ValueError):
        p1.insert(1, p1)

    assert old_ids == shapes.ids

    with pytest.raises(ValueError):
        p1.replace(layer2, p1)

    assert old_ids == shapes.ids


def test_insert_into_recursive(shapes: inkex.SvgDocumentElement):
    """Addition and removal of subtrees"""
    copy = deepcopy(shapes)
    idlen = len(shapes.ids)
    shapes.getElementById("layer1").append(copy)
    for element in copy:
        assert element.root == shapes
        # All ids have been reassigned
        assert "-" in element.get("id")

    assert len(shapes.ids) == 2 * idlen

    # Remove it again
    shapes.getElementById("layer1").replace(copy, inkex.Circle())
    assert len(shapes.ids) == idlen
