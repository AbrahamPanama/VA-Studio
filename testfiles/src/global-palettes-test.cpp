// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * @brief Regression tests for the global palette list and its rebrand compatibility.
 */
/*
 * Authors:
 * see git history
 *
 * Copyright (C) 2026 Authors
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "ui/dialog/global-palettes.h"

#include <string>
#include <glibmm/ustring.h>
#include <gtest/gtest.h>

namespace {

using Inkscape::UI::Dialog::GlobalPalettes;
using Inkscape::UI::Dialog::PaletteFileData;

// The name stored by profiles written before the VA Studio rebrand.
constexpr char const *legacy_default_palette_name = "Inkscape default";
// The name carried by share/palettes/inkscape.gpl after the rebrand.
constexpr char const *rebranded_default_palette_name = "VA Studio default";

PaletteFileData const *find_by_name(GlobalPalettes const &palettes, Glib::ustring const &name)
{
    for (auto const &palette : palettes.palettes()) {
        if (palette.name == name) {
            return &palette;
        }
    }
    return nullptr;
}

} // namespace

// The default palette is looked up by name as well as by id
// (GlobalPalettes indexes ids first and names second). The rebrand renamed the
// palette but not its file, and existing profiles still store the old display
// name in /dialogs/swatches/palette and /embedded/swatches/palette. Without the
// compatibility alias find_palette() returns nullptr and SwatchesPanel silently
// falls back to the "Auto" palette, changing a visible setting on upgrade.
TEST(GlobalPalettesRebrandTest, LegacyDefaultNameResolvesToRebrandedPalette)
{
    auto const &palettes = GlobalPalettes::get();

    auto const *rebranded = find_by_name(palettes, rebranded_default_palette_name);
    ASSERT_NE(rebranded, nullptr)
        << "No palette named '" << rebranded_default_palette_name
        << "'. Run this test through CTest so INKSCAPE_DATADIR points at the share/ resource tree.";
    EXPECT_NE(rebranded->id.raw().find("inkscape.gpl"), std::string::npos)
        << "The default palette id must remain the unchanged inkscape.gpl path, got '" << rebranded->id.raw() << "'";

    auto const *legacy = palettes.find_palette(legacy_default_palette_name);
    ASSERT_NE(legacy, nullptr) << "Profiles storing palette='" << legacy_default_palette_name
                               << "' must keep resolving; otherwise the swatches panel silently falls back to Auto.";
    EXPECT_EQ(legacy, rebranded) << "The legacy name must resolve to the palette now named '"
                                 << rebranded_default_palette_name << "'";
    EXPECT_EQ(legacy->id, rebranded->id);
    EXPECT_EQ(legacy->name, Glib::ustring(rebranded_default_palette_name));
}

TEST(GlobalPalettesRebrandTest, IdAndRebrandedNameLookupsStillWork)
{
    auto const &palettes = GlobalPalettes::get();

    auto const *rebranded = find_by_name(palettes, rebranded_default_palette_name);
    ASSERT_NE(rebranded, nullptr);

    EXPECT_EQ(palettes.find_palette(rebranded->id), rebranded);
    EXPECT_EQ(palettes.find_palette(rebranded_default_palette_name), rebranded);
    EXPECT_EQ(palettes.find_palette("no such palette"), nullptr);
}
