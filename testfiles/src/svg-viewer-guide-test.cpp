// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * VIEW-2: which SVG handler File Explorer uses, and when the PowerToys guide
 * shows and reports each step done.
 *
 * Copyright 2026 VA Studio authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include <gtest/gtest.h>

#include "ui/svg-viewer-guide.h"

using namespace Inkscape::UI::SvgViewerGuide;

namespace {

// As recorded on a Windows test PC with PowerToys 0.101 and VA Studio beta 9 (per user).
Handler const powertoys_thumbnail{"{10144713-1526-46C9-88DA-1FB52807A9FF}",
                                  "C:\\Users\\user\\AppData\\Local\\PowerToys\\PowerToys.SvgThumbnailProviderCpp.dll"};
Handler const powertoys_preview{"{FCDD4EED-41AA-492F-8A84-31A1546226E0}",
                                "C:\\Users\\user\\AppData\\Local\\PowerToys\\PowerToys.SvgPreviewHandlerCpp.dll"};
Handler const vastudio_thumbnail{"{3a05aca6-6d9c-4fea-9240-e0f85bd15050}", "C:\\Program Files\\VA Studio\\bin\\vasvgthumb.dll"};
Handler const vastudio_preview{"{7828A5A8-03F2-4DAF-AE5E-E142BE538EA8}", "C:\\Program Files\\VA Studio\\bin\\vasvgthumb.dll"};

State state(Handler thumbnail, Handler preview, bool installed = true)
{
    State s;
    s.thumbnail = std::move(thumbnail);
    s.preview = std::move(preview);
    s.vastudio_registered = installed;
    return s;
}

TEST(SvgViewerGuide, RecognizesWhoHandlesSvgFiles)
{
    EXPECT_EQ(owner_of(powertoys_thumbnail), Owner::PowerToys);
    EXPECT_EQ(owner_of(powertoys_preview), Owner::PowerToys);
    EXPECT_EQ(owner_of(vastudio_thumbnail), Owner::VaStudio) << "CLSIDs compare without case";
    EXPECT_EQ(owner_of(vastudio_preview), Owner::VaStudio);
    EXPECT_EQ(owner_of({}), Owner::None);
    EXPECT_EQ(owner_of({"{11111111-2222-3333-4444-555555555555}", "C:\\Other\\viewer.dll"}), Owner::Other);
}

TEST(SvgViewerGuide, ShowsOnlyWhenPowerToysHidesAnInstalledVaStudio)
{
    EXPECT_TRUE(needs_guide(state(powertoys_thumbnail, powertoys_preview)));
    EXPECT_TRUE(needs_guide(state(vastudio_thumbnail, powertoys_preview)));
    EXPECT_FALSE(needs_guide(state(vastudio_thumbnail, vastudio_preview)));
    EXPECT_FALSE(needs_guide(state(powertoys_thumbnail, powertoys_preview, false))) << "VA Studio's viewer absent";
    EXPECT_FALSE(needs_guide(state({}, {})));
}

TEST(SvgViewerGuide, EachStepIsDoneWhenPowerToysLetsGoOfIt)
{
    auto p = progress(state(powertoys_thumbnail, powertoys_preview));
    EXPECT_FALSE(p.preview_off);
    EXPECT_FALSE(p.thumbnail_off);
    EXPECT_EQ(p.result, Progress::Result::Pending);

    p = progress(state(powertoys_thumbnail, vastudio_preview));
    EXPECT_TRUE(p.preview_off);
    EXPECT_FALSE(p.thumbnail_off);
    EXPECT_EQ(p.result, Progress::Result::Pending);

    p = progress(state(vastudio_thumbnail, vastudio_preview));
    EXPECT_EQ(p.result, Progress::Result::Done);

    // PowerToys off, but nothing (or another viewer) took over: say so.
    p = progress(state({}, vastudio_preview));
    EXPECT_TRUE(p.thumbnail_off);
    EXPECT_EQ(p.result, Progress::Result::NotActive);
}

TEST(SvgViewerGuide, IconsOnlyExplorerNeedsTheGuideToo)
{
    // Confirmed on a Windows test PC 2026-09-28: VA Studio's handlers were in effect but
    // File Explorer was set to "Always show icons, never thumbnails".
    auto s = state(vastudio_thumbnail, vastudio_preview);
    s.icons_only = true;
    EXPECT_TRUE(needs_guide(s));
    auto p = progress(s);
    EXPECT_FALSE(p.thumbnails_shown);
    EXPECT_EQ(p.result, Progress::Result::Pending);
    s.icons_only = false;
    EXPECT_FALSE(needs_guide(s));
    EXPECT_EQ(progress(s).result, Progress::Result::Done);
}

TEST(SvgViewerGuide, OutsideWindowsNothingIsRead)
{
#ifndef _WIN32
    auto const s = read_state();
    EXPECT_TRUE(s.thumbnail.clsid.empty());
    EXPECT_FALSE(s.vastudio_registered);
    EXPECT_FALSE(needs_guide(s));
#endif
}

} // namespace
