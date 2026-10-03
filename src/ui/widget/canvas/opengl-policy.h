// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_UI_WIDGET_CANVAS_OPENGL_POLICY_H
#define INKSCAPE_UI_WIDGET_CANVAS_OPENGL_POLICY_H

#include <cstring>

namespace Inkscape::UI::Widget::CanvasDetail {

// With the Cairo window renderer (GskCairoRenderer, VA Studio's default on
// Windows) an OpenGL canvas is read back from the GPU into a new CPU image
// every frame, which made "Enable OpenGL" 1.5-2x slower to paint than the
// Cairo canvas (renderer research 2026-09-29, prototype P-a). The canvas
// then draws with Cairo instead. Only this known readback path is vetoed;
// GL, Vulkan and unknown renderers keep the preference.
inline bool renderer_allows_canvas_opengl(char const *renderer_type_name)
{
    return !renderer_type_name || std::strcmp(renderer_type_name, "GskCairoRenderer") != 0;
}

} // namespace Inkscape::UI::Widget::CanvasDetail

#endif // INKSCAPE_UI_WIDGET_CANVAS_OPENGL_POLICY_H
