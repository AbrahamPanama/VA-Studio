# coding=utf-8
"""
Test calling pdflatex to convert formule to svg.

This test uses cached output from the `pdflatex` command because this
test is not a test of pdflatex, but of the extension only. The mocked
output also allows testing in the CI builder without dependencies.

To re-generate the cached files, run the pytest command:

NO_MOCK_COMMANDS=1 pytest tests/test_pdflatex.py -rP

This will use pdflatex for missing mock commands, but will also store the output of
the call to `tests/data/cmd/pdflatex/[key].msg.output (and also to `cmd/inkscape/...`).
The generated file names will be displayed.

The key depends on the comparison arguments, so changing them will invalidate
the file and you must regenerate them.

Remove the `.output` extension from the above file and commit it to the
repository only AFTER all the tests pass and you are happy with them.

Do not use NO_MOCK_COMMANDS=2 when you just want to add/change a single comparison,
since all mock files will be regenerated, and the inkscape commands will get a different
filename (as the pdflatex pdf output will contain the timestamp and other unstable
data), and you won't know which of the new files you need to commit.

Clean up any old `.msg` files with invalid or old keys.

(use EXPORT_COMPARE to generate the output svgs, see inkex.tester docs)
"""

from pdflatex import PdfLatex
from inkex.tester import ComparisonMixin, TestCase


class PdfLatexTest(ComparisonMixin, TestCase):
    """Test some basic latex formulas"""

    compare_file = "svg/empty.svg"
    effect_class = PdfLatex
    comparisons = [
        ("--font_size=15",),  # pdflatex ef9b4005, inkscape e9076ae5
        # pdflatex acb70405, inkscape 387e8338
        ("--font_size=15", "--standalone=False"),
        (
            "--font_size=8",
            r"""--formule=\(\begin{matrix}  a & b & c \\  d & e & f \\  g & h & i \end{matrix}\)""",
        ),  # pdflatex acb70405, inkscape 387e8338
    ]


class PdfLatexTestmm(ComparisonMixin, TestCase):
    compare_file = "svg/empty_mm.svg"
    effect_class = PdfLatex
    comparisons = [
        (
            "--font_size=20",
            r"--formule=\(\frac{1+\sqrt{5}}{2}\)",
        ),  # pdflatex f85674c7, # inkscape e08fd903
        ("--font_size=20", "--standalone=False", r"--formule=\(\frac{1+\sqrt{5}}{2}\)"),
        # pdflatex b78a3100, inkscape 7f9fbd32
    ]
