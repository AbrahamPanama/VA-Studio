# coding=utf-8
"""
Make sure inkex can be imported even if stdout has been closed.
"""

import subprocess
import sys
from pathlib import Path
from inkex.tester import TestCase


class StdoutTest(TestCase):
    """Test import with stdout closed"""

    def test_stdout(self):
        """We can't run this test directly, since pytest needs stdout."""
        thisdir = Path(__file__).parent
        helper = thisdir / "aux_stdout.py"
        process = subprocess.Popen([sys.executable, str(helper)])
        process.communicate()
        self.assertEqual(process.returncode, 0)
