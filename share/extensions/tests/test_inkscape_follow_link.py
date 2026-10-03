# coding=utf-8
from inkscape_follow_link import FollowLink
from inkex.tester import ComparisonMixin, InkscapeExtensionTestMixin, TestCase


class TestFollowLinkBasic(ComparisonMixin, InkscapeExtensionTestMixin, TestCase):
    effect_class = FollowLink
