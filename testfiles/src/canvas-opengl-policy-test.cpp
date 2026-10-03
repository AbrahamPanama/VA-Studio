// SPDX-License-Identifier: GPL-2.0-or-later
#include <gtest/gtest.h>

#include "ui/widget/canvas/opengl-policy.h"

using Inkscape::UI::Widget::CanvasDetail::renderer_allows_canvas_opengl;

TEST(CanvasOpenGLPolicy, CairoVetoesOpenGL)
{
    EXPECT_FALSE(renderer_allows_canvas_opengl("GskCairoRenderer"));
}

TEST(CanvasOpenGLPolicy, GpuRenderersPreserveOpenGLAvailability)
{
    EXPECT_TRUE(renderer_allows_canvas_opengl("GskGLRenderer"));
    EXPECT_TRUE(renderer_allows_canvas_opengl("GskNglRenderer"));
    EXPECT_TRUE(renderer_allows_canvas_opengl("GskVulkanRenderer"));
}

TEST(CanvasOpenGLPolicy, UnknownRenderersPreserveExistingBehavior)
{
    EXPECT_TRUE(renderer_allows_canvas_opengl(nullptr));
    EXPECT_TRUE(renderer_allows_canvas_opengl(""));
    EXPECT_TRUE(renderer_allows_canvas_opengl("FutureRenderer"));
}
