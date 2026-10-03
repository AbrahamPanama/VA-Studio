# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2018-2022 Martin Owens <doctormo@gmail.com>

"""All graphical interfaces bound into a GtkApp"""

import os

from inkex.gui import GtkApp

from .main import ExtensionManagerWindow


class ManagerApp(GtkApp):
    """Load the inkscape extensions ui file and attach to window"""

    ui_dir = os.path.join(os.path.dirname(__file__), "..", "data")
    app_name = "org.inkscape.extensions-manager"

    def on_activate(self):
        win = ExtensionManagerWindow(self)
        win.window.present()
