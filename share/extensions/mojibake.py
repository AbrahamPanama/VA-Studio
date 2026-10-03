#!/usr/bin/env python3
"""mojibake: very basic character reinterpreter"""

# Copyright (c) 2025 WimPum
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

import inkex


class Mojibake(inkex.TextExtension):
    def add_arguments(self, pars):
        """Add command line arguments and inx parameter."""
        pars.add_argument("--source", type=str, default="utf_8")
        pars.add_argument("--target", type=str, default="cp932")

    def process_chardata(self, text):
        """
        Encodes the text in `source` encoding and decodes in `target` encoding.
        """
        if not self.options.source or not self.options.target:
            return text
        b = text.encode(self.options.source, "replace")
        return b.decode(self.options.target, "replace")


if __name__ == "__main__":
    Mojibake().run()
