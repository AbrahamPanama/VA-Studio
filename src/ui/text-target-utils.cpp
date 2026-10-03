// SPDX-License-Identifier: GPL-2.0-or-later

#include "text-target-utils.h"

#include <algorithm>
#include <ranges>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

#include "libnrtype/Layout-TNG.h"
#include "object/sp-flowtext.h"
#include "object/sp-item-group.h"
#include "object/sp-text.h"
#include "style.h"
#include "text-editing.h"
#include "xml/node.h"

namespace Inkscape::UI {

namespace {

// Memoize ancestor eligibility as well as visited children: selecting many
// descendants of the same deep group must still take linear time.
class TextEligibility {
public:
    bool operator()(SPObject const *object)
    {
        std::vector<SPObject const *> ancestors;
        auto current = object;
        while (current && !cache.contains(current)) {
            ancestors.push_back(current);
            current = current->parent;
        }
        bool eligible = !current || cache.at(current);
        for (auto node : ancestors | std::views::reverse) {
            auto repr = node->getRepr();
            auto name = repr ? repr->name() : "";
            eligible = eligible && repr && !node->cloned &&
                std::strcmp(name, "svg:defs") && std::strcmp(name, "svg:clipPath") &&
                std::strcmp(name, "svg:mask") && std::strcmp(name, "svg:symbol") &&
                std::strcmp(name, "svg:use");
            if (auto item = cast<SPItem>(node)) {
                eligible = eligible && item->style && item->isSensitive() && !item->isHidden() &&
                    item->style->visibility.computed == SP_CSS_VISIBILITY_VISIBLE;
            }
            cache.emplace(node, eligible);
        }
        return eligible;
    }

private:
    std::unordered_map<SPObject const *, bool> cache;
};

Text::Layout::iterator iteratorAt(Text::Layout const &layout, unsigned index)
{
    auto iter = layout.begin();
    for (unsigned i = 0; i < index && iter != layout.end(); ++i) {
        iter.nextCharacter();
    }
    return iter;
}

Text::Layout *canonicalLayout(SPItem *item)
{
    if (auto text = cast<SPText>(item)) return &text->layout;
    if (auto flow = cast<SPFlowtext>(item)) return &flow->layout;
    return nullptr;
}

} // namespace

bool isEligibleTextItem(SPItem const *item)
{
    return item && (is<SPText>(item) || is<SPFlowtext>(item)) && TextEligibility{}(item);
}

std::vector<SPItem *> collectTextItems(std::vector<SPItem *> const &selection)
{
    std::vector<SPItem *> result;
    std::vector<SPItem *> pending(selection.rbegin(), selection.rend());
    std::unordered_set<SPItem *> visited;
    TextEligibility eligible;
    while (!pending.empty()) {
        auto item = pending.back();
        pending.pop_back();
        if (!item || !visited.insert(item).second || !eligible(item)) continue;
        if (is<SPText>(item) || is<SPFlowtext>(item)) {
            result.push_back(item);
        } else if (auto group = cast<SPGroup>(item);
                   group && group->layerMode() == SPGroup::GROUP &&
                   std::strcmp(group->getRepr()->name(), "svg:g") == 0) {
            auto const start = pending.size();
            for (auto &child : group->children) {
                if (auto child_item = cast<SPItem>(&child)) pending.push_back(child_item);
            }
            std::reverse(pending.begin() + start, pending.end());
        }
    }
    return result;
}

std::vector<TextLogicalRange> paragraphRanges(Text::Layout const &layout,
                                              unsigned first, unsigned last)
{
    auto const character_count =
        static_cast<unsigned>(layout.iteratorToCharIndex(layout.end()));
    if (character_count == 0) {
        return {};
    }

    first = std::min(first, character_count);
    last = std::min(last, character_count);
    if (last < first) {
        std::swap(first, last);
    }

    auto first_character = iteratorAt(layout, first);
    auto last_character = iteratorAt(layout, last);

    if (first == last) {
        // The end iterator belongs to the final paragraph for caret purposes.
        if (first_character == layout.end()) {
            first_character.prevCharacter();
        }
        last_character = first_character;
    } else {
        // [first, last) must not include a paragraph beginning exactly at last.
        last_character.prevCharacter();
        if (first_character == layout.end()) {
            first_character = last_character;
        }
    }

    first_character.thisStartOfParagraph();
    auto final_paragraph = layout.paragraphIndex(last_character);

    std::vector<TextLogicalRange> result;
    auto paragraph = first_character;
    while (paragraph != layout.end()) {
        auto const paragraph_index = layout.paragraphIndex(paragraph);
        auto next = paragraph;
        next.nextStartOfParagraph();

        result.push_back({
            static_cast<unsigned>(layout.iteratorToCharIndex(paragraph)),
            static_cast<unsigned>(layout.iteratorToCharIndex(next))
        });

        if (paragraph_index == final_paragraph || next == layout.end()) {
            break;
        }
        paragraph = next;
    }

    return result;
}

bool applyStyleToLogicalRanges(SPItem *item, std::vector<TextLogicalRange> const &ranges,
                               SPCSSAttr const *css)
{
    if (!item || !css || ranges.empty()) return false;
    bool changed = false;
    for (auto const &range : ranges | std::views::reverse) {
        auto layout = canonicalLayout(item);
        if (!layout || range.first >= range.last) continue;
        auto const count = static_cast<unsigned>(layout->iteratorToCharIndex(layout->end()));
        if (range.last > count) continue;
        auto first = iteratorAt(*layout, range.first);
        auto last = iteratorAt(*layout, range.last);
        sp_te_apply_style(item, first, last, css);
        changed = true;
    }
    return changed;
}

} // namespace Inkscape::UI
