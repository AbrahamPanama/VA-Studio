#
# Copyright (C) 2020 Martin Owens
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
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110, USA.
#
"""
Test API collisions and other inkex portions that don't fit elsewhere.
"""

from unittest import TestCase as BaseCase

import inkex
import inkex.paths
import inkex.elements
from inkex.utils import PY3


class ProtectiveGlobals(dict):
    """Python 3.3 and above globals dictionary"""

    def __setitem__(self, name, value):
        # This only works because setitem is called during construction It
        # does not work for getitem and that's why the python docs discourage
        # the use of an inherited dictionary class for exec globals.
        if name in self and value is not self[name]:
            assert value is self[name], (
                "While importing {} the API name `{}` was re-defined:\n\t1. {}\n\t2. {}"
            ).format(self["__name__"], name, repr(value), repr(self[name]))
        super(ProtectiveGlobals, self).__setitem__(name, value)


class TestModuleCollisions(BaseCase):
    """Test imports to make sure the API is clean"""

    def assertNoCollisions(self, module):  # pylint: disable=invalid-name
        """Make sure there are no API collisions in the give module on import"""
        if not PY3:
            self.skipTest("API testing python 3.3 and above only.")

        with open(module.__file__, "r") as fhl:
            # name and package are esential to the exec pretending to
            # be an actual module during import (and not a script)
            exec(
                fhl.read(),
                ProtectiveGlobals(
                    {  # pylint: disable=exec-used
                        "__name__": module.__name__,
                        "__package__": module.__package__,
                    }
                ),
            )

    def test_inkex(self):
        """Test inkex API have no collisions"""
        self.assertNoCollisions(inkex)

    def test_inkex_elements(self):
        """Test elements API have no collisions"""
        self.assertNoCollisions(inkex.elements)
