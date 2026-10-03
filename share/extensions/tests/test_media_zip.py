# coding=utf-8
import io
import zipfile

import re

from media_zip import CompressedMedia
from inkex.tester import ComparisonMixin, TestCase
from inkex.tester.filters import CompareSize


class CmoTestEmbed(TestCase):
    def test_complex(self):
        params = (self.data_file("svg", "embed.svg"), "--font_list=True")
        cmo = CompressedMedia()
        out = io.BytesIO()
        cmo.run(params, output=out)

        result = io.BytesIO(out.getvalue())

        with zipfile.ZipFile(result, mode="r") as f:
            files = f.filelist
            files = {i.filename for i in files}
            self.assertEqual(
                files,
                {
                    "images/linecap.png",  # this file is referenced twice
                    # this file has the same filename, but
                    # different source path, so a 1 is appended
                    "images/linecap1.png",
                    "images/markers.svg",
                    "embed.svg",
                    "fontlist.txt",
                },
            )
            # Assert that the image tags have updated links
            svg = f.read("embed.svg").decode("utf8")
            matches = re.finditer(r'xlink:href="(.*)"', svg)
            self.assertEqual(len(list(matches)), 5)
            for match in matches:
                if match.groups(1).startswith("data"):
                    continue
                self.assertTrue(match.groups(1).startswith("images/"))

            # Assert that the font list is correct
            fontlist = f.read("fontlist.txt").decode("utf8")
            fontlist = fontlist.splitlines()[1:]
            self.assertEqual(
                [i.replace('"', "'") for i in fontlist],  # normalize quote delimiters
                [
                    "'Courier New' normal",
                    "'Times New Roman'",
                    "Arial normal",
                    "Verdana normal",
                ],
            )
