// SPDX-License-Identifier: GPL-2.0-or-later
#include <gtest/gtest.h>

#include "inkscape.h"
#include "extension/db.h"
#include "extension/init.h"
#include "extension/input.h"
#include "extension/output.h"
#include "preferences.h"

TEST(MetafileRemoval, RegistryExcludesMetafilesAndRetainsSupportedFormats)
{
    Inkscape::Application::create(false);
    auto prefs = Inkscape::Preferences::get();
    prefs->setString("/dialogs/save_as/default", "org.inkscape.output.emf");
    Inkscape::Extension::init();
    EXPECT_EQ(prefs->getString("/dialogs/save_as/default"), "org.inkscape.output.svg.inkscape");
    auto &db = Inkscape::Extension::db;
    for (auto id : {"org.inkscape.input.emf", "org.inkscape.input.wmf",
                    "org.inkscape.output.emf", "org.inkscape.output.wmf",
                    "org.inkscape.print.emf", "org.inkscape.print.wmf"}) {
        EXPECT_EQ(db.get(id), nullptr) << id;
    }
    // Also catches optional pixbuf loaders and packaged script extensions.
    auto check = [](auto const &extensions) {
        for (auto extension : extensions) {
            auto suffix = Glib::ustring(extension->get_extension()).lowercase();
            EXPECT_NE(suffix, ".emf") << extension->get_id();
            EXPECT_NE(suffix, ".wmf") << extension->get_id();
            EXPECT_NE(suffix, ".emz") << extension->get_id();
            EXPECT_NE(suffix, ".wmz") << extension->get_id();
        }
    };
    Inkscape::Extension::DB::InputList inputs;
    Inkscape::Extension::DB::OutputList outputs;
    check(db.get_input_list(inputs));
    check(db.get_output_list(outputs));
    for (auto id : {"org.inkscape.input.svg", "org.inkscape.output.svg.inkscape",
                    "org.inkscape.raster.tiff_output"}) {
        EXPECT_NE(db.get(id), nullptr) << id;
    }
}
