# coding=utf-8
from restack import Restack
from inkex.tester import ComparisonMixin, TestCase


class RestackBasicTest(ComparisonMixin, TestCase):
    effect_class = Restack
    old_defaults = ("--direction=tb", "--xanchor=m", "--yanchor=m")
    comparisons = [
        ("--tab=positional", "--id=p1", "--id=r3") + old_defaults,
        ("--tab=z_order", "--id=p1", "--id=r3") + old_defaults,
        ("--tab=z_order", "--id=r3", "--id=p1", "--id=t5", "--id=r2") + old_defaults,
        ("--tab=z_order", "--id=r2", "--id=t5", "--id=p1", "--id=r3") + old_defaults,
        (
            "--nb_direction=custom",
            "--angle=50.0",
            "--id=s1",
            "--id=p1",
            "--id=c3",
            "--id=slicerect1",
        )
        + old_defaults,
    ]


class RestackMillimeterGrouped(ComparisonMixin, TestCase):
    """Test for https://gitlab.com/inkscape/extensions/-/issues/372"""

    effect_class = Restack
    compare_file = "svg/restack_grouped.svg"
    comparisons = [
        (
            "--id=g20858",
            "--id=g21085",
            "--id=g20940",
            "--id=g26580",
            "--id=g21081",
            "--id=g20854",
        ),
    ]
