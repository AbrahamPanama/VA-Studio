# coding=utf-8
"""
Test the inkex.GenerateExtension class.
"""

import inkex
from inkex.tester import TestCase


class MyGenerateExtension(inkex.GenerateExtension):
    TEST_ITEM_ATTRIBS = {
        "transform": "rotate(123)",
        "d": "M3,4L5,6",
        "id": "yielded_test_item",
    }

    def generate(self):
        yield inkex.PathElement(**self.TEST_ITEM_ATTRIBS)


class TestExtensionGenerate(TestCase):
    effect_class = MyGenerateExtension

    def test_layer_transform(self):
        effect = self.assertEffect("svg", "transformed-layer.svg")
        item = effect.svg.getElementById(MyGenerateExtension.TEST_ITEM_ATTRIBS["id"])
        parent = item.getparent()

        # expect that generated items have not been modified
        for key, value in MyGenerateExtension.TEST_ITEM_ATTRIBS.items():
            self.assertEqual(item.get(key), value)

        # expect a new parent group which is a child of the current layer
        self.assertEqual(parent.getparent().get("id"), "inner_layer")

        # expect that the parent group compensates the transform of the current
        # layer and is positioned at the current view center
        self.assertTransformEqual(
            parent.get("transform"),
            "scale(0.5) translate(-20,-30) rotate(-30) translate(100,300)",
            3,
        )
