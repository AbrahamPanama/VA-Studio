#!/usr/bin/env python3
# coding=utf-8
#
# Copyright (C) 2005,2007 Aaron Spike, aaron@ekips.org
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
#
# pylint: disable=ungrouped-imports
"""
Embed images so they are base64 encoded data inside the svg.
"""

from __future__ import unicode_literals


import inkex
from inkex import Image
from inkex.localization import inkex_gettext as _


class EmbedImage(inkex.EffectExtension):
    """Allow selected image tags to become embeded image tags"""

    def add_arguments(self, pars):
        pars.add_argument(
            "--selectedonly", type=inkex.Boolean, help="embed only selected images"
        )

    def effect(self):
        # if slectedonly is enabled and there is a selection
        # only embed selected images. otherwise embed all images
        if self.options.selectedonly:
            images = self.svg.selection.get(Image)
        else:
            images = self.svg.xpath("//svg:image")

        for node in images:
            try:
                node.embed_image(self.options.input_file)
            except Exception as e:
                inkex.errormsg(e)


if __name__ == "__main__":
    EmbedImage().run()
