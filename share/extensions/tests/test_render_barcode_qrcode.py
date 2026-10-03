# coding=utf-8
import pytest

from render_barcode_qrcode import QrCode, QRCode
from inkex.tester import ComparisonMixin, TestCase
from inkex.tester.filters import CompareNumericFuzzy


class TestQRCodeInkscapeBasic(ComparisonMixin, TestCase):
    """Test basic use of QR codes"""

    effect_class = QrCode
    compare_file = "svg/empty.svg"
    comparisons = [
        (
            "--text=0123456789",
            "--typenumber=0",
            "--modulesize=10",
            "--drawtype=smooth",
            "--smoothness=greedy",
        ),
        (
            "--text=BreadRolls",
            "--typenumber=2",
            "--encoding=utf8",
            "--modulesize=10",
            "--drawtype=smooth",
            "--smoothness=greedy",
        ),
        (
            "--text=Blue Front Yard",
            "--typenumber=3",
            "--correctionlevel=1",
            "--modulesize=10",
            "--drawtype=smooth",
            "--smoothness=greedy",
        ),
        (
            "--text=Waterfall",
            "--typenumber=1",
            "--drawtype=pathpreset",
            "--pathtype=circle",
            "--modulesize=10",
        ),
        (
            "--text=groupid",
            "--groupid=testid",
            "--modulesize=10",
            "--drawtype=smooth",
            "--smoothness=greedy",
        ),
    ]
    compare_filters = [CompareNumericFuzzy()]


class TestQRCodeInkscapeSelection(ComparisonMixin, TestCase):
    """Test QR code with a selection as input"""

    effect_class = QrCode
    compare_file = "svg/shapes.svg"
    comparisons = [
        ("--text=test", "--drawtype=selection", "--id=r3", "--modulesize=10")
    ]
    compare_filters = [CompareNumericFuzzy()]


class TestQRCodeInkscapeSymbol(ComparisonMixin, TestCase):
    """Test symbols in qr codes"""

    effect_class = QrCode
    compare_file = "svg/symbol.svg"
    comparisons = [
        (
            "--text=ThingOne",
            "--drawtype=symbol",
            "--correctionlevel=2",
            "--symbolid=AirTransportation_Inv",
            "--modulesize=10",
        ),
    ]


class TestQRCodeInkscapeNewLine(ComparisonMixin, TestCase):
    """Test new lines in qr codes"""

    effect_class = QrCode
    compare_file = "svg/empty.svg"
    comparisons = [
        ("--text=Multiline test\\ntest\\ntest",),
    ]


class TestLargeQRCodes(ComparisonMixin, TestCase):
    """Test large qr codes with up to 2953 bytes of payload. Also tests numeric mode"""

    effect_class = QrCode
    compare_file = "svg/empty.svg"
    comparisons = [
        # the largest numeric QR code has 7089 characters
        ("--text=" + (("12345" * 2000)[0:7089]), "--qrmode=1", "--correctionlevel=1"),
    ]
    compare_filters = [CompareNumericFuzzy()]


class TestQRCodeClasses(ComparisonMixin, TestCase):
    """Test alphanumeric barcode"""

    effect_class = QrCode
    compare_file = "svg/empty.svg"
    comparisons = [
        (
            "--text=THIS IS A TEST OF AN ALPHANUMERIC QRCODE. IT CAN STORE A LARGER NUMBER OF "
            "UPPERSPACE CHARACTERS THAN A BYTE-ENCODED QRCODE: 123",
            "--qrmode=2",
            "--correctionlevel=1",
        ),
    ]
    compare_filters = [CompareNumericFuzzy()]
