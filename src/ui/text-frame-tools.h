// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_UI_TEXT_FRAME_TOOLS_H
#define INKSCAPE_UI_TEXT_FRAME_TOOLS_H

#include <vector>

class SPDocument;
class SPText;

namespace Inkscape::UI {

enum class TextFrameVerticalAlignment { Top, Middle, Bottom };

struct TextFrameSettings {
    double width = 0.0;
    double height = 0.0;
    unsigned columns = 1;
    double gap = 0.0;
    TextFrameVerticalAlignment vertical_alignment = TextFrameVerticalAlignment::Top;
    bool generated = false;
};

TextFrameSettings textFrameSettings(SPText const &text);
bool setTextFrameSettings(SPDocument &document, std::vector<SPText *> const &texts,
                          TextFrameSettings const &settings);

} // namespace Inkscape::UI

#endif
