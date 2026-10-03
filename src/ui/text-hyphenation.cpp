// SPDX-License-Identifier: GPL-2.0-or-later

#include "text-hyphenation.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

#include <glibmm/convert.h>
#include <glibmm/miscutils.h>
#include <glibmm/unicode.h>

#include "document.h"
#include "object/sp-object.h"
#include "path-prefix.h"
#include "xml/document.h"
#include "xml/node.h"
#include "xml/repr.h"

namespace Inkscape::UI {

struct TextHyphenator::Data {
    struct Node {
        std::map<gunichar, std::unique_ptr<Node>> children;
        std::vector<unsigned char> weights;
    };
    Node root;
    unsigned patterns = 0;
};

namespace {

bool isEncodingHeader(std::string const &line)
{
    auto upper = line;
    std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char character) {
        return static_cast<char>(std::toupper(character));
    });
    return upper.starts_with("UTF") || upper.starts_with("ISO") ||
           upper.starts_with("KOI") || upper.starts_with("CP") ||
           upper.starts_with("WINDOWS");
}

void addPattern(TextHyphenator::Data &data, Glib::ustring const &pattern)
{
    Glib::ustring letters;
    std::vector<unsigned char> weights(1, 0);
    for (auto character : pattern) {
        if (character >= '0' && character <= '9') {
            weights.back() = static_cast<unsigned char>(character - '0');
        } else {
            letters += character;
            weights.push_back(0);
        }
    }
    if (letters.empty()) return;

    auto node = &data.root;
    for (auto character : letters.lowercase()) {
        auto &child = node->children[character];
        if (!child) child = std::make_unique<TextHyphenator::Data::Node>();
        node = child.get();
    }
    node->weights = std::move(weights);
    ++data.patterns;
}

Glib::ustring normalizeLanguage(Glib::ustring language)
{
    if (language.empty()) language = "en_US";
    auto bytes = language.raw();
    std::replace(bytes.begin(), bytes.end(), '-', '_');
    language = bytes;
    auto dot = language.find('.');
    if (dot != Glib::ustring::npos) language.erase(dot);
    return language;
}

std::optional<std::string> readDictionary(std::filesystem::path const &path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) return std::nullopt;
    return std::string{std::istreambuf_iterator<char>{input}, {}};
}

constexpr char hyphenation_attribute[] = "inkscape:hyphenation";
constexpr char auto_hyphen_attribute[] = "inkscape:auto-hyphen";

bool tagged(XML::Node const &node, char const *attribute)
{
    auto value = node.attribute(attribute);
    return value && std::string_view{value} == "true";
}

bool removeAutomaticHyphens(XML::Node *parent)
{
    bool changed = false;
    for (auto child = parent ? parent->firstChild() : nullptr; child;) {
        auto next = child->next();
        if (child->type() == XML::NodeType::ELEMENT_NODE && tagged(*child, auto_hyphen_attribute)) {
            parent->removeChild(child);
            changed = true;
        } else if (child->type() == XML::NodeType::ELEMENT_NODE &&
                   !tagged(*child, "inkscape:list-marker")) {
            changed |= removeAutomaticHyphens(child);
        }
        child = next;
    }
    return changed;
}

bool insertAutomaticHyphens(XML::Node *parent, TextHyphenator const &hyphenator)
{
    bool changed = false;
    for (auto child = parent ? parent->firstChild() : nullptr; child;) {
        auto next = child->next();
        if (child->type() == XML::NodeType::ELEMENT_NODE) {
            if (!tagged(*child, "inkscape:list-marker") && !tagged(*child, auto_hyphen_attribute)) {
                changed |= insertAutomaticHyphens(child, hyphenator);
            }
            child = next;
            continue;
        }
        if (child->type() != XML::NodeType::TEXT_NODE || !child->content()) {
            child = next;
            continue;
        }

        Glib::ustring content{child->content()};
        std::vector<unsigned> breaks;
        unsigned word_start = 0;
        bool in_word = false;
        for (unsigned index = 0; index <= content.size(); ++index) {
            auto const word_character = index < content.size() &&
                (g_unichar_isalpha(content[index]) || g_unichar_ismark(content[index]));
            if (word_character && !in_word) {
                word_start = index;
                in_word = true;
            } else if (!word_character && in_word) {
                auto word = content.substr(word_start, index - word_start);
                for (auto offset : hyphenator.hyphenate(word)) breaks.push_back(word_start + offset);
                in_word = false;
            }
        }
        if (breaks.empty()) {
            child = next;
            continue;
        }

        auto xml = parent->document();
        auto cursor = child->prev();
        parent->removeChild(child);
        unsigned start = 0;
        for (auto position : breaks) {
            if (position > start) {
                auto text = xml->createTextNode(content.substr(start, position - start).c_str());
                parent->addChild(text, cursor);
                cursor = text;
                GC::release(text);
            }
            auto marker = xml->createElement("svg:tspan");
            marker->setAttribute(auto_hyphen_attribute, "true");
            auto soft_hyphen = xml->createTextNode("\xc2\xad");
            marker->addChild(soft_hyphen, nullptr);
            parent->addChild(marker, cursor);
            cursor = marker;
            GC::release(soft_hyphen);
            GC::release(marker);
            start = position;
        }
        if (start < content.size()) {
            auto text = xml->createTextNode(content.substr(start).c_str());
            parent->addChild(text, cursor);
            GC::release(text);
        }
        changed = true;
        child = next;
    }
    return changed;
}

} // namespace

TextHyphenator::TextHyphenator()
    : _data(std::make_shared<Data>())
{}

TextHyphenator::TextHyphenator(std::shared_ptr<Data> data)
    : _data(std::move(data))
{}

std::optional<TextHyphenator> TextHyphenator::fromDictionary(std::string_view contents)
{
    std::istringstream input(std::string{contents});
    std::string encoding;
    if (!std::getline(input, encoding)) return std::nullopt;
    if (!encoding.empty() && encoding.back() == '\r') encoding.pop_back();
    if (!isEncodingHeader(encoding)) {
        // A dictionary without an encoding header is interpreted as UTF-8;
        // feed its first pattern back through the normal parser.
        input.clear();
        input.seekg(0);
        encoding = "UTF-8";
    }

    auto data = std::make_shared<Data>();
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto first = line.find_first_not_of(" \t");
        if (first == std::string::npos || line[first] == '%' || line[first] == '#') continue;
        line.erase(0, first);
        if (line.starts_with("LEFTHYPHENMIN") || line.starts_with("RIGHTHYPHENMIN") ||
            line.starts_with("COMPOUND") || line.starts_with("NEXTLEVEL") ||
            line.find('=') != std::string::npos || line.find('/') != std::string::npos) {
            continue;
        }
        try {
            auto utf8 = encoding == "UTF-8" ? line : Glib::convert(line, "UTF-8", encoding);
            addPattern(*data, Glib::ustring{utf8});
        } catch (...) {
            // Ignore malformed or unsupported dictionary directives.
        }
    }
    if (!data->patterns) return std::nullopt;
    return TextHyphenator{std::move(data)};
}

std::optional<TextHyphenator> TextHyphenator::forLanguage(Glib::ustring language)
{
    language = normalizeLanguage(std::move(language));
    std::vector<std::filesystem::path> roots;
    if (auto custom = g_getenv("INKSCAPE_HYPHENATION_PATH")) roots.emplace_back(std::filesystem::u8path(custom));
    roots.emplace_back(std::filesystem::u8path(
        Glib::build_filename(get_inkscape_datadir(), "inkscape", "hyphen")));
    roots.emplace_back("/usr/share/hyphen");
    roots.emplace_back("/usr/share/myspell/dicts");
    roots.emplace_back("/Library/Spelling");
    roots.emplace_back("/System/Library/ScreenReader/BrailleTables/LiblouisBrailleTranslator.brailletable/Contents/Resources/liblouis/tables");

    std::array<Glib::ustring, 2> names = {language, language.substr(0, 2)};
    for (auto const &root : roots) {
        for (auto const &name : names) {
            std::array<Glib::ustring, 2> filenames = {"hyph_" + name + ".dic", name + ".dic"};
            for (auto const &filename : filenames) {
                auto contents = readDictionary(root / std::filesystem::u8path(filename.raw()));
                if (contents) {
                    if (auto result = fromDictionary(*contents)) return result;
                }
            }
        }
    }
    return std::nullopt;
}

std::vector<unsigned> TextHyphenator::hyphenate(Glib::ustring const &word,
                                                 unsigned min_left,
                                                 unsigned min_right) const
{
    std::vector<unsigned> result;
    if (empty() || word.size() < min_left + min_right + 1) return result;

    auto padded = Glib::ustring{"."} + word.lowercase() + ".";
    std::vector<gunichar> characters(padded.begin(), padded.end());
    std::vector<unsigned char> scores(characters.size() + 1, 0);
    for (unsigned start = 0; start < characters.size(); ++start) {
        auto node = &_data->root;
        for (unsigned offset = 0; start + offset < characters.size(); ++offset) {
            auto found = node->children.find(characters[start + offset]);
            if (found == node->children.end()) break;
            node = found->second.get();
            for (unsigned i = 0; i < node->weights.size(); ++i) {
                scores[start + i] = std::max(scores[start + i], node->weights[i]);
            }
        }
    }

    for (unsigned position = min_left; position + min_right <= word.size(); ++position) {
        if (scores[position + 1] % 2 == 1) result.push_back(position);
    }
    return result;
}

bool TextHyphenator::empty() const
{
    return !_data || !_data->patterns;
}

bool paragraphHyphenation(SPObject const &paragraph)
{
    auto repr = const_cast<SPObject &>(paragraph).getRepr();
    auto value = repr ? repr->attribute(hyphenation_attribute) : nullptr;
    return value && std::string_view{value} == "auto";
}

bool paragraphHasHyphenationDictionary(SPObject const &paragraph)
{
    auto language = paragraph.getLanguage();
    return TextHyphenator::forLanguage(language ? language : "").has_value();
}

bool setParagraphHyphenation(SPDocument &document,
                             std::vector<SPObject *> const &paragraphs,
                             bool enabled)
{
    bool changed = false;
    for (auto paragraph : paragraphs) {
        auto repr = paragraph ? paragraph->getRepr() : nullptr;
        if (!repr) continue;

        changed |= removeAutomaticHyphens(repr);
        if (enabled) {
            auto language = paragraph->getLanguage();
            auto hyphenator = TextHyphenator::forLanguage(language ? language : "");
            if (!hyphenator) continue;
            changed |= insertAutomaticHyphens(repr, *hyphenator);
        }
        auto const current = paragraphHyphenation(*paragraph);
        if (current != enabled) {
            repr->setAttribute(hyphenation_attribute, enabled ? "auto" : nullptr);
            changed = true;
        }
    }
    return changed;
}

} // namespace Inkscape::UI
