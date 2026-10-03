# coding=utf-8
from jitternodes import JitterNodes
from inkex.tester import ComparisonMixin, TestCase


class JitterNodesBasicTest(ComparisonMixin, TestCase):
    effect_class = JitterNodes
    comparisons = [
        ("--id=p1", "--dist=gaussian", "--end=false", "--ctrl=true"),
        ("--id=p1", "--dist=uniform", "--ctrl=false"),
        ("--id=p1", "--dist=pareto", "--radiusy=100", "--ctrl=true"),
        ("--id=p1", "--dist=lognorm", "--radiusx=100", "--ctrl=true"),
    ]
