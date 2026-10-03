// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_UI_WINDOWS_RENDERING_H
#define INKSCAPE_UI_WINDOWS_RENDERING_H
#include <glib.h>
#include <optional>

namespace Inkscape::UI {
inline constexpr char windows_accelerated_rendering_pref[] = "/options/rendering/windows_accelerated";
inline constexpr char windows_gdi_buffer_pref[] = "/options/rendering/windows_gdi_buffer";
inline constexpr char windows_gdi_buffer_env[] = "GDK_WIN32_CAIRO_GDI_BUFFER";

// Absence means GTK's patched default (on). An explicit environment value always wins.
inline std::optional<char const *> gdi_buffer_env_to_set(bool enabled, char const *existing_env)
{
    if (!enabled && !existing_env) return "0";
    return std::nullopt;
}

inline bool windows_gdi_buffer_env_forced = false;
inline char const *gdi_buffer_mode(char const *renderer_name)
{
    if (renderer_name && g_strcmp0(renderer_name, "unavailable") != 0 &&
        !g_strrstr(renderer_name, "Cairo"))
        return "bypassed (accelerated renderer)";
    auto const env = g_getenv(windows_gdi_buffer_env);
    // The patched GTK treats 0, false and off (any case) as disabled.
    bool const off = env && (g_strcmp0(env, "0") == 0 || g_ascii_strcasecmp(env, "false") == 0 ||
                             g_ascii_strcasecmp(env, "off") == 0);
    if (windows_gdi_buffer_env_forced) return off ? "env-forced off" : "env-forced on";
    return off ? "off" : "on";
}

// Call once before GTK opens the display. External overrides take priority as
// a pair, including an explicit Cairo recovery override. Never persist these
// variables in the system/user environment. Off restores GTK's usual selection
// on the next normal launch; it does not undo external environment overrides.
inline bool configure_windows_accelerated_rendering(bool enabled)
{
    if (!enabled || g_getenv("GSK_RENDERER") || g_getenv("GDK_WIN32_FORCE_DCOMP")) {
        return false;
    }
    g_setenv("GDK_WIN32_FORCE_DCOMP", "1", true);
    g_setenv("GSK_RENDERER", "gl", true);
    return true;
}
} // namespace Inkscape::UI
#endif
