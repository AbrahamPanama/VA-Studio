# coding=utf-8
#
# Copyright 2022 Martin Owens <doctormo@geek-2.com>
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
Test GUI App, running main loops and threading from and to them.

Do NOT copy this test if you are looking for examples of how to write
unit tests for your GUI enabled inkscape extensions. This code contains
a lot of weird and confusing checking of loops and process running.
"""

import os
import sys
import time
import pytest

from inkex.tester import TestCase
from inkex.utils import DependencyError

try:
    from inkex.gui.tester import MainLoopProtection
    from inkex.gui.listview import label
    from inkex.gui import GtkApp, Window, asyncme
    from gi.repository import Gtk, GLib

except DependencyError:
    Window = object
    GtkApp = None

    class asyncme:
        run_or_none = classmethod(lambda a, f: f)
        run_or_wait = classmethod(lambda a, f: f)
        mainloop_only = classmethod(lambda a, f: f)


class LoopyWindow(Window):
    """A testing window with various loops"""

    def realized(self, widget=None):
        """Called by the window's realise signal, see glade ui xml"""
        # Spawn a thread to do something without locking up the UI
        self.thread_action()
        # Add a function to the GLib main loop manually
        GLib.idle_add(self.loop_action)

    def loop_action(self):
        """Called while we are in the idle loop"""
        # Test Mainloop to mainloop doesn't lock up.
        assert "pancakes" == self.get_pancakes()
        time.sleep(0.2)
        # Run some useful action we might want to test
        self.in_loop_action()
        # We actually want to quit to make the test faster
        self.close()
        return False

    def in_loop_action(self):
        """Defined by the test loader if needed"""
        pass

    @asyncme.run_or_none
    def thread_action(self):
        """Run in a different thread from Gtk"""
        self.in_thread_action()

    @asyncme.run_or_wait
    def in_thread_action(self):
        """Defined by the test loader if needed"""
        pass

    @asyncme.mainloop_only
    def get_pancakes(self):
        """
        A call which always runs in the mainloop,
        even if not called from one.
        """
        return "pancakes"

    def test_signal(self, widget=None):
        """A signal from Gtk widgets defined in the glade xml"""
        if widget:
            pass


@pytest.mark.skipif(GtkApp is None, reason="PyGObject is required")
class GtkAppTest(TestCase):
    """Tests for GUI App"""

    def construct_window(self, name, **kwargs):
        return type("_GtkWindow", (LoopyWindow,), {"name": name, **kwargs})

    def test_args(self):
        """Test app arguments"""
        self.assertEqual(label(4), "int")
        self.assertEqual(label((4, 5)), "int or int")
        app = GtkApp(
            app_name="inline-app",
            ui_dir=self.datadir(),
            prefix="ui",
            ui_file="app-test",
            extra="junk",
        )
        self.assertEqual(app.kwargs["extra"], "junk")

    def test_app_inline_run(self):
        """Test a basic gui loop"""

        cw = self.construct_window

        class InlineApp(GtkApp):
            app_name = "org.inkscape.inline-app"
            ui_dir = self.datadir()
            prefix = "ui"
            ui_file = "app-test"

            def on_activate(self):
                win = cw("basic_app")(self)
                win.window.present()

        app = InlineApp()
        with MainLoopProtection():
            app.run()

    def test_app_outside_run(self):
        """Test various threading in and out of Gtk"""

        cw = self.construct_window

        class BasicApp(GtkApp):
            app_name = "org.inkscape.basic-app"
            ui_dir = self.datadir()
            prefix = "ui"
            ui_file = "app-test"

            def on_activate(self):
                self.win1 = cw(
                    "basic_app",
                    in_thread_action=asyncme.mainloop_only(self._thread),
                )(self)
                self.win1.window.present()

            def _thread(self, widget=None):
                self.looped = self.win1.widget("button1").get_label()

        app = BasicApp()
        with MainLoopProtection():
            app.run()

        # Threading doesn't work in the CI builder, disabled for now.
        # self.assertEqual(app.looped, "gtk-apply")
