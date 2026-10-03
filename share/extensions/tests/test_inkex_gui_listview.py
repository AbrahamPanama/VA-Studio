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
Test the ListView and TreeView features.
"""

import os
import sys
import time
import pytest

from inkex.tester import TestCase
from inkex.utils import DependencyError
from inkex.gui.tester import MainLoopProtection

try:
    from inkex.gui import GtkApp, Window
    from inkex.gui.pixmap import PixmapManager
    from inkex.gui.listview import (
        TreeView,
        IconView,
        ComboBox,
        ViewSort,
        ViewColumn,
        Separator,
    )
    from gi.repository import Gtk, Gdk, GLib
except DependencyError:
    Window = object
    GtkApp = None


class DataItem:
    def __init__(self, name):
        self.name = name
        self.icon = "edit-find"

    def get_id(self):
        return str(self.name).lower()

    def __repr__(self):
        return f"<Data {self.name}>"

    def get_name(self):
        return self.name


class ListWindow(Window):
    name = "listing_app"

    def realized(self, widget=None):
        """Called by the window's realise signal, see ui xml"""
        # Add a function to the GLib main loop manually
        GLib.idle_add(self.loop_action)

    def loop_action(self):
        """Called while we are in the idle loop"""
        self.gapp.test_case.test_func()
        time.sleep(0.2)
        self.close()
        return False

    def select_tree(self, tree):
        """Select some items"""
        items = [a for a, b, c in tree]
        tree.set_selected_items(*items[:2])


@pytest.mark.skipif(GtkApp is None, reason="PyGObject is required")
class GtkListTest(TestCase):
    """Tests all treeview types"""

    def run_in_app(self, callable):
        class ListApp(GtkApp):
            app_name = "org.inkscape.listapp"
            ui_dir = self.datadir()
            prefix = "ui"
            ui_file = "listing-app"

            def on_activate(self):
                win = ListWindow(self)
                win.window.present()

                self.test_case.window = win
                self.test_case.tree = win.widget("tree1")
                self.test_case.icon = win.widget("icon1")
                self.test_case.combo = win.widget("combo1")
                self.test_case.pixmaps = PixmapManager(self.test_case.datadir())

        app = ListApp()
        app.test_case = self
        self.test_func = callable
        with MainLoopProtection():
            app.run()

    def test_errors(self):
        def test_func():
            self.assertRaises(TypeError, TreeView, self.icon)
            self.assertRaises(TypeError, TreeView, self.tree, liststore="pig")
            self.assertRaises(TypeError, IconView, self.tree)

        self.run_in_app(test_func)

    def test_treeview(self):
        """Test TreeView Controls"""

        def test_func():
            def sel(item):
                pass

            def unsel(item):
                pass

            tv1 = TreeView(self.tree, selected=sel, unselected=unsel)
            parent = tv1.add_item(DataItem("green"))
            item1 = DataItem("blue")
            item2 = DataItem("vase")
            tv1.add([item1, item2], parent)
            self.assertTrue(tv1.get_iter(item1))
            self.assertTrue(tv1.get_iter(item2))
            self.assertRaises(ValueError, tv1.add_item, item1)
            self.assertEqual(len(list(tv1)), 3)

            tv1.set_selected_items(parent)
            self.assertTrue(tv1.is_selected(parent))
            self.assertFalse(tv1.is_selected(item1))
            self.assertTrue(tv1.get_selected_items())

            item3 = DataItem("red")
            tv1.replace(item3, tv1.get_iter(item2))
            self.assertTrue(tv1.get_model())

            tv1.expand_item(item1)

            self.assertRaises(ValueError, tv1.add_item, None)

            tv1.refresh()
            tv1.item_selected_signal()
            tv1.item_button_clicked(None, None)

        self.run_in_app(test_func)

    def test_iconview(self):
        """Test special icon view"""

        def test_func():
            iv1 = IconView(self.icon, self.pixmaps)
            item1 = DataItem("red")
            iv1.add_item(item1)
            iv1.add_item(DataItem("yellow"))
            iv1.add([DataItem("policebox"), DataItem("scarf")])
            iv1.set_selected_item(item1)

            iv1.replace(DataItem("purse"))
            self.assertEqual(len(list(iv1)), 1)
            self.assertTrue(iv1.get_model())
            iv1.item_selected_signal()

            self.assertRaises(ValueError, iv1.add_item, None)

        self.run_in_app(test_func)

    def test_combobox(self):
        """Test special combo box"""

        def test_func():
            item = DataItem("pea")
            combo = ComboBox(self.combo)
            combo.add_item(item)
            combo.set_selected_item(item)
            self.assertTrue(combo.is_selected(item))
            self.assertTrue(combo.get_selected_item())

            col = combo.create_column("Bold", expand=True)
            col.add_text_renderer("name", wrap=True)

        self.run_in_app(test_func)

    def test_sort(self):
        """Test list sorting"""

        def test_func():
            tv2 = TreeView(self.tree)
            tv2.add_item(DataItem("Bat"))
            tv2.add_item(DataItem("Ant"))
            tv2.add_item(DataItem("Cat"))
            tv2.add_item(DataItem("Dog"))
            tv2.add_item(DataItem("0"))
            tv2.add_item(DataItem(None))

            def ids():
                return [item.get_id()[0] for item, *_ in tv2]

            sorter = tv2.create_sort(data="name", ascending=False)
            self.assertEqual(ids(), ["0", "a", "b", "c", "d", "n"])

            sorter = tv2.create_sort(data="name", ascending=True)
            self.assertEqual(ids(), ["d", "c", "b", "a", "0", "n"])

            sorter = tv2.create_sort(data="name", exact="bat")
            self.assertEqual(ids(), ["b", "d", "c", "a", "0", "n"])

            sorter = tv2.create_sort(data="name", contains="at")
            self.assertEqual(ids(), ["b", "c", "d", "a", "0", "n"])

        self.run_in_app(test_func)

    def test_sort_func(self):
        def test_func():
            tv3 = TreeView(self.tree)

            def ids():
                return [item.get_id()[0] for item, *_ in tv3]

            def get_data(item):
                return len(item.name)

            tv3.add_item(DataItem("A"))
            tv3.add_item(DataItem("Be"))
            tv3.add_item(DataItem("Cat"))
            tv3.add_item(DataItem("Dog"))

            sorter = tv3.create_sort(data=get_data)
            self.assertEqual(ids(), ["a", "b", "c", "d"])

        self.run_in_app(test_func)

    def test_columns(self):
        def test_func():
            tv4 = TreeView(self.tree)

            col = tv4.create_column("Bold", expand=True)
            col.add_image_renderer("icon", pixmaps=self.pixmaps)
            col.add_text_renderer("name", wrap=True)

            self.assertEqual(col.clean(None), "")
            self.assertEqual(col.clean("<A&B>", markup=True), "&amp;lt;A&amp;B&amp;gt;")
            self.assertEqual(col.clean(["A", "B"]), ("A", "B"))
            self.assertEqual(col.clean({"A": "B"}), {"A": "B"})
            self.assertRaises(TypeError, col.clean, col)

        self.run_in_app(test_func)

    def test_in_runnning_app(self):
        def test_func():
            tree = TreeView(self.tree)
            tree.set_sensitive(True)

            col = tree.create_column("Bold", expand=True)
            col.add_image_renderer(None, pixmaps=self.pixmaps, size=32)
            col.add_text_renderer(None, wrap=True)
            col2 = tree.create_column("Italic", expand=False)
            col2.add_image_renderer("icon", pixmaps=None, size=32)
            col2.add_text_renderer("get_name", template="<b>{}</b>")
            col.add_text_renderer("get_nom", wrap=True)

            def set_bg(self, item):
                super().set_background(item)
                return "#ff0000"

            col.set_background = set_bg

            item_b = DataItem("B")
            item_b.icon = self.pixmaps.get(item_b.icon)

            tree.add_item(DataItem("A"))
            tree.add_item(item_b)
            tree.add_item(Separator())
            tree.add_item(DataItem("C"))

            self.window.select_tree(tree)

        self.run_in_app(test_func)
