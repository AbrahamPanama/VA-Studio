// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "ui/widget/font-list.h"

using Inkscape::UI::Widget::clamp_compact_font_list_size;
using Inkscape::UI::Widget::compact_recent_font_families;

TEST(FontListCompactTest, ClampsPersistedAndDraggedDimensions)
{
    EXPECT_EQ(clamp_compact_font_list_size(360, 430),
              (Inkscape::UI::Widget::CompactFontListSize{360, 430}));
    EXPECT_EQ(clamp_compact_font_list_size(100, 200),
              (Inkscape::UI::Widget::CompactFontListSize{320, 320}));
    EXPECT_EQ(clamp_compact_font_list_size(900, 1000),
              (Inkscape::UI::Widget::CompactFontListSize{640, 720}));
    EXPECT_EQ(clamp_compact_font_list_size(400, 900),
              (Inkscape::UI::Widget::CompactFontListSize{400, 720}));
}

TEST(FontListCompactTest, ProjectsFiveUniqueNonemptyFamiliesInHistoryOrder)
{
    std::list<Glib::ustring> history{
        "Inter", "", "Roboto", "Inter", "Alegreya", "Noto Sans", "Futura", "Helvetica"
    };
    EXPECT_EQ(compact_recent_font_families(history),
              (std::vector<Glib::ustring>{"Inter", "Roboto", "Alegreya", "Noto Sans", "Futura"}));
}

TEST(FontListCompactTest, HonorsCustomAndZeroLimits)
{
    std::list<Glib::ustring> history{"Inter", "Roboto", "Alegreya"};
    EXPECT_EQ(compact_recent_font_families(history, 2),
              (std::vector<Glib::ustring>{"Inter", "Roboto"}));
    EXPECT_TRUE(compact_recent_font_families(history, 0).empty());
}
