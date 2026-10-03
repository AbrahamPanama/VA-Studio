#!/usr/bin/env python3
# coding=utf-8
"""
Test elements extra logic from svg xml lxml custom classes.
"""

import os
import pathlib
import re
from glob import glob

from inkex.utils import PY3
from inkex.tester import TestCase
from inkex.tester.inx import InxMixin
from inkex.inx import InxFile


# white-listed icons (templates), don't check those
whitelist = {
    "presentation-icon",
    "dvd_box",
    "businesscard_landscape",
    "seamless_pattern",
    "org.inkscape.effect.bluredge",  # internal effect
    "org.inkscape.effect.grid",  # internal effect
}


class InxTestCase(InxMixin, TestCase):
    """Test INX files"""

    def test_inx_effect(self):
        inx = InxFile(
            """
<inkscape-extension xmlns="http://www.inkscape.org/namespace/inkscape/extension">
    <name>TestOne</name>
    <id>org.inkscape.test.inx_one</id>
    <effect needs-live-preview="false">
        <object-type>all</object-type>
        <effects-menu>
            <submenu name="Banana">
                <submenu name="Ice Cream"/>
            </submenu>
        </effects-menu>
    </effect>
    <script>
        <command location="inx" interpreter="python">inx_test.py</command>
    </script>
</inkscape-extension>"""
        )
        self.assertEqual(inx.name, "TestOne")
        self.assertEqual(inx.ident, "org.inkscape.test.inx_one")
        self.assertEqual(inx.slug, "InxOne")
        self.assertEqual(
            inx.metadata, {"type": "effect", "preview": False, "objects": "all"}
        )
        self.assertEqual(inx.menu, ["Banana", "Ice Cream", "TestOne"])
        self.assertEqual(inx.warnings, [])

    def test_inx_output(self):
        inx = InxFile(
            """
<inkscape-extension xmlns="http://www.inkscape.org/namespace/inkscape/extension">
    <_name>TestTwo</_name>
    <id>org.inkscape.test.inx_two</id>
    <output>
        <extension>.inx</extension>
        <mimetype>text/xml+inx</mimetype>
        <filetypename>Extension (*.inx)</filetypename>
        <_filetypetooltip>The extension extension repention suspension.</_filetypetooltip>
        <dataloss>true</dataloss>
    </output>
</inkscape-extension>"""
        )
        self.assertEqual(inx.name, "TestTwo")
        self.assertEqual(inx.ident, "org.inkscape.test.inx_two")
        self.assertEqual(
            inx.metadata,
            {
                "dataloss": True,
                "extension": ".inx",
                "mimetype": "text/xml+inx",
                "name": "Extension (*.inx)",
                "tooltip": "The extension extension repention suspension.",
                "type": "output",
            },
        )
        self.assertEqual(
            inx.warnings,
            [
                "Use of old translation scheme: <_filetypetooltip...>",
                "Use of old translation scheme: <_name...>",
            ],
        )

    def test_inx_input(self):
        inx = InxFile(
            """<inkscape-extension>
    <name>TestThree</name>
    <id>org.inkscape.test.inx_three</id>
    <input>
        <extension>.inx</extension>
        <mimetype>text/xml+inx</mimetype>
        <filetypename>Extension (*.inx)</filetypename>
        <filetypetooltip>The extension extension repention suspension.</filetypetooltip>
    </input>
</inkscape-extension>"""
        )
        self.assertEqual(inx.name, "TestThree")
        self.assertEqual(
            inx.metadata,
            {
                "extension": ".inx",
                "mimetype": "text/xml+inx",
                "name": "Extension (*.inx)",
                "tooltip": "The extension extension repention suspension.",
                "type": "input",
            },
        )
        self.assertEqual(inx.warnings, ["No inx xml prefix."])

    def test_inx_template(self):
        inx = InxFile(
            """<inkscape-extension xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape">
    <name>TestFour</name>
    <id>org.inkscape.test.inx_four</id>
        <effect needs-live-preview="false">
        <object-type>all</object-type>
        <effects-menu hidden="true" />
    </effect>
    <inkscape:templateinfo>
      <inkscape:name>Magic Number</inkscape:name>
      <inkscape:author>Donky Oaty</inkscape:author>
      <inkscape:shortdesc>Something might happen.</inkscape:shortdesc>
      <inkscape:date>2070-01-01</inkscape:date>
      <inkscape:keywords>word food strawberry</inkscape:keywords>
    </inkscape:templateinfo>
</inkscape-extension>"""
        )
        self.assertEqual(inx.name, "TestFour")
        self.assertEqual(
            inx.metadata,
            {
                "author": "Donky Oaty",
                "desc": "Something might happen.",
                "type": "template",
            },
        )
        self.assertEqual(inx.warnings, ["No inx xml prefix."])

    def test_inx_files(self):
        """Get all inx files and test each of them"""
        if not PY3:
            self.skipTest("No INX testing in python2")
            return
        for inx_file in glob(os.path.join(self._testdir(), "..", "*.inx")):
            with self.subTest(inx_file=inx_file):
                self.assertInxSchemaValid(inx_file)
                self.assertInxIsGood(inx_file)

        self.assertIcons(
            os.path.join(self._testdir(), "../icons"),
            os.path.join(self._testdir(), ".."),
        )

    # check that all icons have a matching extension
    def assertIcons(self, path_icons, path_ext):
        svgs = list(pathlib.Path(path_icons).glob("*.svg"))
        inxs = list(pathlib.Path(path_ext).glob("*.inx"))

        ids = set()
        # collect all extension IDs
        for inx in inxs:
            xml = inx.read_text()
            ext_id = re.search(r"(?sm)<id>(.*?)</id>", xml)
            if ext_id:
                ids.add(ext_id.group(1))

        # check icons
        for svg in svgs:
            fname = svg.stem
            if fname in whitelist:
                continue
            # skip names starting with '_' (those are "includes" linked by other icons)
            if fname[0] != "_":
                self.assertTrue(
                    fname in ids, f"Icon without matching extension: {svg.name}"
                )
