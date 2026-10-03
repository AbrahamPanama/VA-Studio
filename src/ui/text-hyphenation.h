// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_UI_TEXT_HYPHENATION_H
#define INKSCAPE_UI_TEXT_HYPHENATION_H

#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include <glibmm/ustring.h>

namespace Inkscape::UI {

class TextHyphenator;

} // namespace Inkscape::UI

class SPDocument;
class SPObject;

namespace Inkscape::UI {

class TextHyphenator {
public:
    struct Data;

    TextHyphenator();
    static std::optional<TextHyphenator> fromDictionary(std::string_view contents);
    static std::optional<TextHyphenator> forLanguage(Glib::ustring language);

    std::vector<unsigned> hyphenate(Glib::ustring const &word,
                                    unsigned min_left = 2,
                                    unsigned min_right = 2) const;
    bool empty() const;

private:
    explicit TextHyphenator(std::shared_ptr<Data> data);
    std::shared_ptr<Data> _data;
};

bool paragraphHyphenation(SPObject const &paragraph);
bool paragraphHasHyphenationDictionary(SPObject const &paragraph);
bool setParagraphHyphenation(SPDocument &document,
                             std::vector<SPObject *> const &paragraphs,
                             bool enabled);

} // namespace Inkscape::UI

#endif // INKSCAPE_UI_TEXT_HYPHENATION_H
