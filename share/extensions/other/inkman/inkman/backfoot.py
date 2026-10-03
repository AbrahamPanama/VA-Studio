#!/usr/bin/env python3
# -*- coding: utf-8 -*-
#
# Copyright 2021 Martin Owens <doctormo@gmail.com>
#
# This program is free software: you can redistribute it and/or modify
#  it under the terms of the GNU General Public License as published by
#  the Free Software Foundation, either version 3 of the License, or
#  (at your option) any later version.
#
#  This program is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#  GNU General Public License for more details.
#
#  You should have received a copy of the GNU General Public License
#  along with this program.  If not, see <http://www.gnu.org/licenses/>
#
"""
When the extension manager fails, this allows us to upgrade automatically.
"""

import os
import sys
import traceback

# Note: All imports are being done as late as possible in this module.
# If any of the imports caused the error, then we'll still message the user.


def update_pip_package(package):
    """
    Update the package using the pip update.
    """
    from inkex.utils import get_user_directory
    from inkex.command import ProgramRunError, call

    try:
        pip = os.path.join(get_user_directory(), "bin", "pip")
        log = call(pip, "install", "--upgrade", package).decode("utf8")
        logs = [line for line in log.split("\n") if "skip" not in line]
        return "\n".join(logs)
    except ProgramRunError:
        raise IOError(f"Failed to update the package {package}")


def attempt_to_recover(exctype, value, tb):
    """
    Messages the user, provides a traceback and attempts a self-update
    """
    from gi.repository import GLib, Gtk
    from inkman import __pkgname__, __version__, __file__, __issues__

    try:
        update_pip_package(__pkgname__)
        update_msg = (
            "The package has already been updated in an attempt to fix the issue. "
            "Please reload the program and try again.\n\n"
        )
    except Exception:
        # Don't confuse the user with an additional error message, focus on the original message.
        # Pip likely failed because we weren't shipped as a pip package.
        update_msg = ""

    location = os.path.dirname(__file__)
    stack = "\n".join(traceback.format_exception(exctype, value, tb))

    dlg = Gtk.MessageDialog()
    dlg.set_markup("<b>An error occured with the extensions manager!</b>")
    dlg.add_button("_Close", 1)
    dlg.set_default_response(1)

    area = dlg.get_message_area()
    scroll = Gtk.ScrolledWindow()
    text = Gtk.TextView()
    text.get_buffer().set_text(
        f"""{update_msg}
Please report the error below
-----------------------------

Report URL: {__issues__}
Location: {location}
{__pkgname__}: {__version__}

{stack}
"""
    )
    text.set_editable(False)
    scroll.set_child(text)
    scroll.set_vexpand(True)
    area.append(scroll)
    dlg.set_default_size(400, 400)
    dlg.set_modal(True)
    dlg.set_resizable(True)

    loop = GLib.MainLoop()
    dlg.connect("response", lambda *args: loop.quit())
    dlg.show()
    loop.run()

    sys.exit(2)
