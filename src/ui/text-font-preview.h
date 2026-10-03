// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_UI_TEXT_FONT_PREVIEW_H
#define INKSCAPE_UI_TEXT_FONT_PREVIEW_H

#include <vector>
#include <sigc++/scoped_connection.h>

#include "object/weakptr.h"
#include "ui/widget/font-selector-interface.h"

class SPDesktop;
class SPItem;

namespace Inkscape::UI {

struct TextPreviewRange {
    SPWeakPtr<SPItem> item;
    unsigned first_char = 0;
    unsigned last_char = 0; // half-open
    bool whole_object = false;
};

/** Coordinates transient, desktop-local font layouts and their single commit. */
class TextFontPreviewController final {
public:
    TextFontPreviewController() = default;
    ~TextFontPreviewController();

    void setDesktop(SPDesktop *desktop);
    void begin(std::vector<TextPreviewRange> targets);
    void request(FontChoice const &choice);
    bool commit(FontChoice const &choice);
    void cancel() noexcept;

    bool active() const;
    SPDesktop *desktopForTesting() const { return _desktop; }

private:
    SPDesktop *_desktop = nullptr;
    sigc::scoped_connection _desktop_destroy;
};

} // namespace Inkscape::UI

#endif // INKSCAPE_UI_TEXT_FONT_PREVIEW_H
