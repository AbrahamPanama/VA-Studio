# coding=utf-8
from jessyink_mouse_handler import AddMouseHandler
from inkex.tester import ComparisonMixin, TestCase


class JessyInkAddMouseHandlerTest(ComparisonMixin, TestCase):
    """Test jessy ink mouse handler"""

    effect_class = AddMouseHandler
    comparisons = [
        ("--mouseSetting=default",),
        ("--mouseSetting=noclick",),
        ("--mouseSetting=draggingZoom",),
    ]
