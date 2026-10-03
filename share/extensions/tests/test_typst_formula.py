# coding=utf-8
"""
Test calling typst_formula to convert a formula to svg. Based on
test_pdflatex.py

This test uses cached output from the `typst` command because this
test is not a test of typst, but of the extension only. The mocked
output also allows testing in the CI builder without dependencies.

To re-generate the cached files, run the pytest command:

NO_MOCK_COMMANDS=1 pytest tests/test_typst_formula.py -rP

This will use typst for missing mock commands, but will also store the output of
the call to `tests/data/cmd/typst_formula/[key].msg.output (and also to `cmd/inkscape/...`).
The generated file names will be displayed.

The key depends on the comparison arguments, so changing them will invalidate
the file, and you must regenerate them.

Remove the `.output` extension from the above file and commit it to the
repository only AFTER all the tests pass, and you are happy with them.

Do not use NO_MOCK_COMMANDS=2 when you just want to add/change a single comparison,
since all mock files will be regenerated, and the inkscape commands will get a different
filename (as the typst svg output will contain the timestamp and other unstable
data), and you won't know which of the new files you need to commit.

Clean up any old `.msg` files with invalid or old keys.

(use EXPORT_COMPARE to generate the output svgs, see inkex.tester docs)
"""

from typst_formula import TypstFormula
from inkex.tester import ComparisonMixin, TestCase


class TypstFormulaTest(ComparisonMixin, TestCase):
    """Test some basic typst formulas"""

    compare_file = "svg/empty.svg"
    effect_class = TypstFormula
    comparisons = [
        (
            "--font_size=14",
            r"""--typst_code=$mat(a, b, c; d, e, f; g, h, i)$""",
        ),
        (
            "--font_size=20",
            r"""--typst_code=$y = plus.minus sqrt(1 - x^2)$""",
        ),
    ]


class TypstFormulaTestMM(ComparisonMixin, TestCase):
    """Test some basic typst formulas in a svg document with mm as units"""

    compare_file = "svg/empty_mm.svg"
    effect_class = TypstFormula
    comparisons = [
        (
            "--font_size=12",
            r"""--typst_code=$y(t) = A dot sin(omega dot t)$""",
        ),
    ]
