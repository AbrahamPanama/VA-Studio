// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "ui/modifiers.h"

namespace Inkscape::Modifiers {

TEST(CanvasScrollModifiers, PreservesControl)
{
    EXPECT_EQ(canvas_scroll_modifier_state(CTRL), CTRL);
    EXPECT_EQ(canvas_scroll_modifier_state(CTRL | SHIFT), CTRL | SHIFT);
}

TEST(CanvasScrollModifiers, UsesCommandAsControlOnlyOnMacOS)
{
#ifdef __APPLE__
    EXPECT_EQ(canvas_scroll_modifier_state(META), CTRL);
    EXPECT_EQ(canvas_scroll_modifier_state(META | SHIFT), CTRL | SHIFT);
    EXPECT_EQ(canvas_scroll_modifier_state(META | ALT), CTRL | ALT);
#else
    EXPECT_EQ(canvas_scroll_modifier_state(META), META);
    EXPECT_EQ(canvas_scroll_modifier_state(META | SHIFT), META | SHIFT);
#endif
}

TEST(CanvasScrollModifiers, DoesNotReplaceExplicitControl)
{
    EXPECT_EQ(canvas_scroll_modifier_state(CTRL | META), CTRL | META);
}

} // namespace Inkscape::Modifiers
