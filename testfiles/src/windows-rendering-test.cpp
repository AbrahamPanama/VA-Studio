// SPDX-License-Identifier: GPL-2.0-or-later
#include <gtest/gtest.h>
#include <optional>
#include <string>
#include "ui/windows-rendering.h"

namespace {
class WindowsRendering : public ::testing::Test {
    std::optional<std::string> renderer, dcomp;
    void SetUp() override {
        if (auto value = g_getenv("GSK_RENDERER")) renderer = value;
        if (auto value = g_getenv("GDK_WIN32_FORCE_DCOMP")) dcomp = value;
        g_unsetenv("GSK_RENDERER");
        g_unsetenv("GDK_WIN32_FORCE_DCOMP");
    }
    void TearDown() override {
        if (renderer) g_setenv("GSK_RENDERER", renderer->c_str(), true);
        else g_unsetenv("GSK_RENDERER");
        if (dcomp) g_setenv("GDK_WIN32_FORCE_DCOMP", dcomp->c_str(), true);
        else g_unsetenv("GDK_WIN32_FORCE_DCOMP");
    }
};
TEST_F(WindowsRendering, DefaultLeavesGtkSelectionUnchanged) {
    EXPECT_FALSE(Inkscape::UI::configure_windows_accelerated_rendering(false));
    EXPECT_EQ(g_getenv("GSK_RENDERER"), nullptr);
    EXPECT_EQ(g_getenv("GDK_WIN32_FORCE_DCOMP"), nullptr);
}
TEST_F(WindowsRendering, OptInRequestsBothRequiredBackends) {
    EXPECT_TRUE(Inkscape::UI::configure_windows_accelerated_rendering(true));
    EXPECT_STREQ(g_getenv("GSK_RENDERER"), "gl");
    EXPECT_STREQ(g_getenv("GDK_WIN32_FORCE_DCOMP"), "1");
}
TEST_F(WindowsRendering, ExplicitCairoRecoveryDoesNotEnableDcomp) {
    g_setenv("GSK_RENDERER", "cairo", true);
    EXPECT_FALSE(Inkscape::UI::configure_windows_accelerated_rendering(true));
    EXPECT_STREQ(g_getenv("GSK_RENDERER"), "cairo");
    EXPECT_EQ(g_getenv("GDK_WIN32_FORCE_DCOMP"), nullptr);
}
TEST_F(WindowsRendering, ExistingDcompOverrideIsNotPartiallyChanged) {
    // MSYS2 checks presence, so even "0" must not be treated as absent.
    g_setenv("GDK_WIN32_FORCE_DCOMP", "0", true);
    EXPECT_FALSE(Inkscape::UI::configure_windows_accelerated_rendering(true));
    EXPECT_STREQ(g_getenv("GDK_WIN32_FORCE_DCOMP"), "0");
    EXPECT_EQ(g_getenv("GSK_RENDERER"), nullptr);
}
TEST_F(WindowsRendering, DisabledPreferencePreservesExternalOverrides) {
    g_setenv("GSK_RENDERER", "cairo", true);
    EXPECT_FALSE(Inkscape::UI::configure_windows_accelerated_rendering(false));
    EXPECT_STREQ(g_getenv("GSK_RENDERER"), "cairo");
}
TEST(WindowsGdiBuffer, PreferenceAndEnvironmentDecision) {
    using Inkscape::UI::gdi_buffer_env_to_set;
    EXPECT_FALSE(gdi_buffer_env_to_set(true, nullptr));
    EXPECT_STREQ(*gdi_buffer_env_to_set(false, nullptr), "0");
    EXPECT_FALSE(gdi_buffer_env_to_set(false, "0"));
    EXPECT_FALSE(gdi_buffer_env_to_set(false, "1"));
    EXPECT_FALSE(gdi_buffer_env_to_set(false, ""));
    EXPECT_FALSE(gdi_buffer_env_to_set(true, "0"));
}
TEST(WindowsGdiBuffer, ModeRecognisesEveryOffSpelling) {
    using Inkscape::UI::gdi_buffer_mode;
    for (auto value : {"0", "false", "OFF"}) {
        g_setenv(Inkscape::UI::windows_gdi_buffer_env, value, true);
        EXPECT_STREQ(gdi_buffer_mode("GskCairoRenderer"), "off") << value;
    }
    g_setenv(Inkscape::UI::windows_gdi_buffer_env, "1", true);
    EXPECT_STREQ(gdi_buffer_mode("GskCairoRenderer"), "on");
    EXPECT_STREQ(gdi_buffer_mode("GskGLRenderer"), "bypassed (accelerated renderer)");
    g_unsetenv(Inkscape::UI::windows_gdi_buffer_env);
}
} // namespace
