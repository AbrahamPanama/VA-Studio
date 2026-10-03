// SPDX-License-Identifier: GPL-2.0-or-later

#include "text-font-preview.h"

#include "desktop.h"
#include "text-style-controller.h"

namespace Inkscape::UI {

TextFontPreviewController::~TextFontPreviewController()
{
    cancel();
}

void TextFontPreviewController::setDesktop(SPDesktop *desktop)
{
    if (_desktop == desktop) return;
    cancel();
    _desktop_destroy.disconnect();
    _desktop = desktop;
    if (_desktop) {
        _desktop_destroy = _desktop->connectDestroy([this](SPDesktop *) { _desktop = nullptr; });
    }
}

void TextFontPreviewController::begin(std::vector<TextPreviewRange> targets)
{
    if (!_desktop) return;
    std::vector<TextStyleTarget> converted;
    converted.reserve(targets.size());
    for (auto &target : targets) {
        converted.push_back({std::move(target.item), target.first_char,
                             target.last_char, target.whole_object});
    }
    _desktop->textStyleController().begin(std::move(converted));
}

void TextFontPreviewController::request(FontChoice const &choice)
{
    if (_desktop) _desktop->textStyleController().requestFontChoice(choice);
}

bool TextFontPreviewController::commit(FontChoice const &choice)
{
    if (!_desktop) return false;
    auto confirmed = choice;
    confirmed.phase = FontChoicePhase::Commit;
    // Font Browser keeps its normal face policy regardless of panel UI state.
    return _desktop->textStyleController().requestFontChoice(confirmed);
}

void TextFontPreviewController::cancel() noexcept
{
    if (_desktop) _desktop->textStyleController().cancelPreview();
}

bool TextFontPreviewController::active() const
{
    return _desktop && _desktop->textStyleController().hasTargets();
}

} // namespace Inkscape::UI
