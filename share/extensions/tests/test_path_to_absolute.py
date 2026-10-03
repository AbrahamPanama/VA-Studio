# coding=utf-8

from path_to_absolute import ToAbsolute
from inkex.tester import ComparisonMixin, TestCase


class PathToAbsoluteTest(ComparisonMixin, TestCase):
    """Test converting objects to absolute"""

    effect_class = ToAbsolute
    comparisons = [
        (
            "--id=c1",
            "--id=c2",
            "--id=c3",
        ),
        ("--id=r1", "--id=r2", "--id=r3", "--id=slicerect1"),
        ("--id=p1", "--id=p2", "--id=s1", "--id=u1"),
    ]
