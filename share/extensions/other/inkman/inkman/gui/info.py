# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2018-2022 Martin Owens <doctormo@gmail.com>

"""Information about a package"""

from dataclasses import dataclass

from inkex.gui import TreeView, Window


@dataclass
class ExtensionTreeItem:
    """Shows the name of the item in the extensions tree"""

    name: str
    kind: str


class ExtensionTreeView(TreeView):
    """A list of extensions (inx file based)"""

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.parents = {}
        self.menus = {}

    def setup(self):
        column = self.create_column("Name", expand=True)
        column.add_text_renderer("name")
        self.create_sort(data=lambda item: item.name)
        super().setup()

    def get_menu(self, parent, remain):
        menu_id = "::".join([str(r) for r in remain])
        if menu_id not in self.menus:
            if len(remain) > 1:
                parent = self.get_menu(parent, remain[:-1])

            row = [ExtensionTreeItem(remain[-1], kind="menu")]
            self.menus[menu_id] = self.get_iter(
                self._add_item(row, parent=parent)
            )

        return self.menus[menu_id]

    def add_item(self, item, parent=None):
        if item.kind not in self.parents:
            row = [ExtensionTreeItem(item.kind.title(), kind="category")]
            self.parents[item.kind] = self.get_iter(
                self._add_item(row, parent=None)
            )
        parent = self.parents[item.kind]
        if item.kind == "effect" and len(item.menu) > 1:
            parent = self.get_menu(parent, item.menu[:-1])
        return self._add_item([item], parent)


class MoreInformation(Window):
    """Show further information for an installed package"""

    name = "info"

    def __init__(self, gapp, pixmaps, item, **kwargs):
        super().__init__(gapp, **kwargs)

        self.inx = ExtensionTreeView(
            self.widget("inx"), selected=self.select_inx
        )

        try:
            self.widget("info_website").set_uri(item.link)
            self.widget("info_website").set_label(item.link)
        except Exception:
            self.widget("info_website").hide()

        self.pixmaps = pixmaps
        self.window.set_title(item.name)
        self.widget("info_name").set_label(item.name)
        self.widget("info_desc").set_label(item.summary)
        self.widget("info_version").set_label(item.version)
        self.widget("info_license").set_label(item.license)
        self.widget("info_author").set_label(f"{item.author}")
        try:
            self.widget("info_image").set_pixbuf(pixmaps.get(item.get_icon()))
        except Exception:
            pass

        self.inx.clear()
        self.inx.add(item.get_inx_files())

        self.widget("info_close").grab_focus()

    def select_inx(self, item):
        pass
