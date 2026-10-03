// SPDX-License-Identifier: GPL-2.0-or-later

#include "text-style-controller.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <ranges>
#include <unordered_set>

#include <glibmm/i18n.h>
#include <glibmm/main.h>

#include "desktop.h"
#include "desktop-style.h"
#include "document.h"
#include "document-undo.h"
#include "libnrtype/font-feature-utils.h"
#include "libnrtype/font-factory.h"
#include "libnrtype/font-instance.h"
#include "libnrtype/font-lister.h"
#include "message-stack.h"
#include "object/sp-flowtext.h"
#include "object/sp-flowdiv.h"
#include "object/sp-text.h"
#include "object/sp-tspan.h"
#include "object/sp-tref.h"
#include "object/sp-textpath.h"
#include "selection.h"
#include "style.h"
#include "svg/css-ostringstream.h"
#include "text-editing.h"
#include "ui/icon-names.h"
#include "ui/text-target-utils.h"
#include "ui/text-hyphenation.h"
#include "ui/tools/text-tool.h"
#include "util/recently-used-fonts.h"
#include "util/signal-blocker.h"
#include "util/font-discovery.h"
#include "util-string/context-string.h"
#include "xml/repr.h"

namespace Inkscape::UI {
namespace {

constexpr unsigned pointer_dwell_ms = 150;
constexpr unsigned slow_pointer_dwell_ms = 250;
// Temporary stabilization switch; remove after one release cycle.
constexpr bool live_text_style_preview_enabled = true;

Text::Layout *canonical_layout(SPItem *item)
{
    if (auto text = cast<SPText>(item)) return &text->layout;
    if (auto flow = cast<SPFlowtext>(item)) return &flow->layout;
    return nullptr;
}

SPObject *nearest_style_source(SPObject *source, SPItem *fallback)
{
    while (source && source != fallback && (!source->style || !is<SPItem>(source))) {
        source = source->parent;
    }
    if (source && source->style && is<SPItem>(source)) {
        return source;
    }
    return fallback && fallback->style ? fallback : nullptr;
}

void clear_item_preview(SPItem *item, unsigned key) noexcept
{
    if (auto text = cast<SPText>(item)) {
        text->clearTextStylePreview(key);
    } else if (auto flow = cast<SPFlowtext>(item)) {
        flow->clearTextStylePreview(key);
    }
}

bool set_item_preview(SPItem *item, unsigned key,
                      std::vector<Text::Layout::TypographyOverride> overrides)
{
    if (auto text = cast<SPText>(item)) {
        return text->setTextStylePreview(key, std::move(overrides));
    }
    if (auto flow = cast<SPFlowtext>(item)) {
        return flow->setTextStylePreview(key, std::move(overrides));
    }
    return false;
}

double effective_space_advance(SPStyle const &style)
{
    Glib::ustring fontspec;
    if (auto spec = style.font_specification.value()) fontspec = spec;
    if (fontspec.empty()) {
        fontspec = style.font_family.value() ? style.font_family.value() : "sans-serif";
    }
    try {
        auto font = FontFactory::get().FaceFromPangoString(fontspec.c_str());
        if (font) {
            auto glyph = font->MapUnicodeChar(0x20);
            auto advance = font->Advance(glyph, false) * style.font_size.computed;
            if (std::isfinite(advance) && advance > 0) return advance;
        }
    } catch (...) {
    }
    return std::max(1.0, style.font_size.computed * 0.25);
}

Glib::ustring format_px(double value)
{
    CSSOStringStream stream;
    stream << value << "px";
    return stream.str();
}

Glib::ustring format_percent(double value)
{
    CSSOStringStream stream;
    stream << value << "%";
    return stream.str();
}

Glib::ustring decoration_line(SPStyle const &style, bool underline)
{
    Glib::ustring result;
    auto append = [&](char const *name) {
        if (!result.empty()) result += " ";
        result += name;
    };
    if (underline) append("underline");
    if (style.text_decoration_line.overline) append("overline");
    if (style.text_decoration_line.line_through) append("line-through");
    if (style.text_decoration_line.blink) append("blink");
    return result.empty() ? Glib::ustring{"none"} : result;
}

Glib::ustring ligature_value(SPStyle const &style, bool common)
{
    auto bits = static_cast<unsigned>(style.font_variant_ligatures.computed);
    if (common) {
        bits |= SP_CSS_FONT_VARIANT_LIGATURES_COMMON;
        bits &= ~SP_CSS_FONT_VARIANT_LIGATURES_NOCOMMON;
    } else {
        bits &= ~SP_CSS_FONT_VARIANT_LIGATURES_COMMON;
        bits |= SP_CSS_FONT_VARIANT_LIGATURES_NOCOMMON;
    }

    Glib::ustring result;
    auto append = [&](char const *name) {
        if (!result.empty()) result += " ";
        result += name;
    };
    append(common ? "common-ligatures" : "no-common-ligatures");
    if (bits & SP_CSS_FONT_VARIANT_LIGATURES_DISCRETIONARY) append("discretionary-ligatures");
    if (bits & SP_CSS_FONT_VARIANT_LIGATURES_NODISCRETIONARY) append("no-discretionary-ligatures");
    if (bits & SP_CSS_FONT_VARIANT_LIGATURES_HISTORICAL) append("historical-ligatures");
    if (bits & SP_CSS_FONT_VARIANT_LIGATURES_NOHISTORICAL) append("no-historical-ligatures");
    if (bits & SP_CSS_FONT_VARIANT_LIGATURES_CONTEXTUAL) append("contextual");
    if (bits & SP_CSS_FONT_VARIANT_LIGATURES_NOCONTEXTUAL) append("no-contextual");
    return result;
}

bool close_enough(double a, double b)
{
    return std::abs(a - b) <= 1e-6 * std::max({1.0, std::abs(a), std::abs(b)});
}

bool equivalent_fontspec(Glib::ustring const &a, Glib::ustring const &b)
{
    if (a == b) return true;
    Pango::FontDescription first(a);
    Pango::FontDescription second(b);
    // Pango and font backends legitimately alternate between "Regular" and
    // "Normal" for the same default face. Compare the effective face traits
    // and variable axes instead of those display names.
    return first.get_style() == second.get_style() &&
           first.get_weight() == second.get_weight() &&
           first.get_stretch() == second.get_stretch() &&
           first.get_variant() == second.get_variant() &&
           first.get_variations() == second.get_variations();
}

template <typename T, typename Mapper, typename Equal = std::equal_to<T>>
void populate_value(TextStyleValue<T> &field, std::vector<SPStyle const *> const &styles,
                    Mapper mapper, Equal equal = {})
{
    if (styles.empty()) return;
    field.value = mapper(*styles.front());
    field.valid = true;
    field.mixed = std::ranges::any_of(styles | std::views::drop(1), [&](auto style) {
        return !equal(field.value, mapper(*style));
    });
}

TextScriptPosition script_position(SPStyle const &style)
{
    if (style.font_variant_position.computed == SP_CSS_FONT_VARIANT_POSITION_SUPER ||
        (style.baseline_shift.type == SP_BASELINE_SHIFT_LITERAL &&
         style.baseline_shift.literal == SP_CSS_BASELINE_SHIFT_SUPER)) {
        return TextScriptPosition::Superscript;
    }
    if (style.font_variant_position.computed == SP_CSS_FONT_VARIANT_POSITION_SUB ||
        (style.baseline_shift.type == SP_BASELINE_SHIFT_LITERAL &&
         style.baseline_shift.literal == SP_CSS_BASELINE_SHIFT_SUB)) {
        return TextScriptPosition::Subscript;
    }
    return TextScriptPosition::Normal;
}

std::pair<CapitalizationMode, bool> capitalization(SPStyle const &style)
{
    bool const custom = style.text_transform.computed == SP_CSS_TEXT_TRANSFORM_LOWERCASE ||
                        style.text_transform.computed == SP_CSS_TEXT_TRANSFORM_CAPITALIZE;
    auto const mode = style.font_variant_caps_mode.value();
    if (mode && !std::strcmp(mode, "synthesized")) {
        return {CapitalizationMode::SmallCapsSynthesized, custom};
    }
    if (style.text_transform.computed == SP_CSS_TEXT_TRANSFORM_UPPERCASE) {
        return {CapitalizationMode::AllCaps, custom};
    }
    if (style.font_variant_caps.computed == SP_CSS_FONT_VARIANT_CAPS_TITLING) {
        return {CapitalizationMode::TitlingCaps, custom};
    }
    if (style.font_variant_caps.computed == SP_CSS_FONT_VARIANT_CAPS_SMALL) {
        return {CapitalizationMode::SmallCapsAuto, custom};
    }
    if (style.font_variant_caps.computed == SP_CSS_FONT_VARIANT_CAPS_ALL_SMALL) {
        return {CapitalizationMode::AllSmallCaps, custom};
    }
    if (query_font_feature(style.font_feature_settings.value(), "c2sc") == true) {
        return {CapitalizationMode::SmallCapsFromCaps, custom};
    }
    return {CapitalizationMode::None, custom};
}

bool needs_run_specific_css(TextStylePatch const &patch)
{
    return patch.underline || patch.script_position || patch.capitalization ||
           patch.standard_ligatures || patch.character_spacing_percent ||
           patch.word_spacing_percent;
}

struct CssAttrDeleter {
    void operator()(SPCSSAttr *css) const
    {
        if (css) sp_repr_css_attr_unref(css);
    }
};

using CssAttrPtr = std::unique_ptr<SPCSSAttr, CssAttrDeleter>;

} // namespace

bool TextStylePatch::empty() const
{
    return !family && !face && !fontspec && !font_size_px && !bold && !italic && !underline &&
           !script_position && !capitalization && !standard_ligatures &&
           !character_spacing_percent && !word_spacing_percent &&
           !character_spacing_px && !word_spacing_px && !language_spacing_percent &&
           !fill && !stroke;
}

bool TextParagraphPatch::empty() const
{
    return !alignment && !line_height && !first_line_indent_px &&
           !spacing_before_px && !spacing_after_px &&
           !list_mode && !list_start && !hyphenation && !drop_cap_lines &&
           !direction && !writing_mode && !orientation;
}

bool TextFramePatch::empty() const
{
    return !width_px && !height_px && !columns && !gap_px && !vertical_alignment;
}

TextStyleController::TextStyleController(SPDesktop *desktop)
    : _desktop(desktop)
{
    if (!_desktop) return;
    _tool_changed = _desktop->connectEventContextChanged([this](auto, auto) {
        if (!_committing) invalidateFontChoices();
    });
    _document_replaced = _desktop->connectDocumentReplaced([this](auto, auto) {
        if (!_committing) invalidateFontChoices();
        reconnectSelection();
    });
    _cursor_moved = _desktop->connect_text_cursor_moved([this](auto *) {
        if (!_committing) invalidateFontChoices();
    });
    reconnectSelection();
}

TextStyleController::~TextStyleController()
{
    cancelPreview();
    _selection_changed.disconnect();
    _selection_modified.disconnect();
    _tool_changed.disconnect();
    _document_replaced.disconnect();
    _cursor_moved.disconnect();
    _document_modified.disconnect();
    _desktop = nullptr;
}

void TextStyleController::reconnectSelection()
{
    _selection_changed.disconnect();
    _selection_modified.disconnect();
    _document_modified.disconnect();
    if (_desktop && _desktop->getDocument()) {
        _document_modified = _desktop->getDocument()->connectModified([this](auto) {
            // Ancestor locks, reparenting and referenced text may change without
            // modifying a selected text root itself.
            if (!_committing) invalidateFontChoices();
        });
    }
    if (!_desktop || !_desktop->getSelection()) return;
    _selection_changed = _desktop->getSelection()->connectChanged([this](auto *) {
        if (!_committing) invalidateFontChoices();
    });
    _selection_modified = _desktop->getSelection()->connectModified([this](auto *, auto) {
        if (!_committing) invalidateFontChoices();
    });
}

std::vector<SPItem *> TextStyleController::selectedTextItems() const
{
    std::vector<SPItem *> result;
    if (!_desktop || !_desktop->getSelection()) return result;
    auto items = _desktop->getSelection()->items();
    return collectTextItems({items.begin(), items.end()});
}

bool TextStyleController::containsUnsupportedTref(SPItem *item, unsigned first, unsigned last) const
{
    auto layout = canonical_layout(item);
    if (!layout || first >= last) return false;
    auto iter = layout->begin();
    for (unsigned i = 0; i < first && iter != layout->end(); ++i) iter.nextCharacter();
    for (unsigned i = first; i < last && iter != layout->end(); ++i, iter.nextCharacter()) {
        SPObject *source = nullptr;
        layout->getSourceOfCharacter(iter, &source);
        if (source && (is<SPTRef>(source) || is<SPTRef>(source->parent))) return true;
    }
    return false;
}

bool TextStyleController::captureTargets()
{
    if (!_desktop) return false;
    std::vector<TextStyleTarget> targets;
    bool had_subselection = false;
    if (auto tool = dynamic_cast<Tools::TextTool *>(_desktop->getTool())) {
        auto item = tool->textItem();
        auto layout = canonical_layout(item);
        if (item && layout && tool->text_sel_start != tool->text_sel_end) {
            had_subselection = true;
            if (!isEligibleTextItem(item)) return false;
            auto first = static_cast<unsigned>(layout->iteratorToCharIndex(
                std::min(tool->text_sel_start, tool->text_sel_end)));
            auto last = static_cast<unsigned>(layout->iteratorToCharIndex(
                std::max(tool->text_sel_start, tool->text_sel_end)));
            if (containsUnsupportedTref(item, first, last)) return false;
            targets.push_back({SPWeakPtr<SPItem>(item), first, last, false});
        }
    }
    if (!had_subselection) {
        for (auto item : selectedTextItems()) {
            auto layout = canonical_layout(item);
            if (!layout) continue;
            targets.push_back({SPWeakPtr<SPItem>(item), 0,
                static_cast<unsigned>(layout->iteratorToCharIndex(layout->end())), true});
        }
    }
    _targets = std::move(targets);
    if (!targetsValid()) {
        auto const had_targets = !_targets.empty();
        _targets.clear();
        if (had_targets) _desktop->messageStack()->flash(WARNING_MESSAGE,
            _("The selected text cannot be styled with its current range or transform."));
        return false;
    }
    return true;
}

bool TextStyleController::targetsValid() const
{
    if (!_desktop || _targets.empty()) return false;
    for (auto const &target : _targets) {
        auto item = target.item.get();
        if (!isEligibleTextItem(item) || item->document != _desktop->getDocument()) return false;
        auto const transform = item->i2doc_affine();
        for (unsigned i = 0; i < 6; ++i) {
            if (!std::isfinite(transform[i])) return false;
        }
        if (!std::isfinite(transform.descrim()) || transform.descrim() <= 0) return false;
        auto layout = canonical_layout(item);
        if (!layout || target.first_char > target.last_char ||
            target.last_char > static_cast<unsigned>(layout->iteratorToCharIndex(layout->end()))) return false;
        if (!target.whole_object && containsUnsupportedTref(item, target.first_char, target.last_char)) return false;
    }
    return true;
}

void TextStyleController::begin(std::vector<TextStyleTarget> targets)
{
    cancelPreview();
    std::unordered_set<SPItem *> seen;
    for (auto &target : targets) {
        if (seen.insert(target.item.get()).second) _targets.push_back(std::move(target));
    }
    // Keep invalid explicit targets until cancellation: falling back to the
    // current selection would turn a rejected stale range into a different edit.
}

bool TextStyleController::hasTargets() const
{
    if (!_targets.empty()) return targetsValid();
    if (!_desktop) return false;
    if (auto tool = dynamic_cast<Tools::TextTool *>(_desktop->getTool())) {
        if (tool->textItem() && tool->text_sel_start != tool->text_sel_end) {
            return !currentRunStyles().empty();
        }
    }
    return !selectedTextItems().empty();
}

SPItem *TextStyleController::representativeTextItem() const
{
    if (!_targets.empty()) {
        if (auto item = _targets.front().item.get(); isEligibleTextItem(item)) return item;
    }
    if (_desktop) {
        if (auto tool = dynamic_cast<Tools::TextTool *>(_desktop->getTool())) {
            if (auto item = tool->textItem(); isEligibleTextItem(item)) return item;
        }
    }
    auto items = selectedTextItems();
    return items.empty() ? nullptr : items.front();
}

SPStyle const *TextStyleController::representativeRunStyle() const
{
    auto const styles = currentRunStyles();
    return styles.empty() ? nullptr : styles.front();
}

std::vector<SPStyle const *> TextStyleController::currentRunStyles() const
{
    std::vector<TextStyleTarget> targets = _targets;
    if (!_desktop) return {};
    if (!targets.empty() && !targetsValid()) return {};
    if (targets.empty()) {
        if (auto tool = dynamic_cast<Tools::TextTool *>(_desktop->getTool())) {
            auto item = tool->textItem();
            auto layout = canonical_layout(item);
            if (item && layout && tool->text_sel_start != tool->text_sel_end) {
                if (!isEligibleTextItem(item)) return {};
                auto first = static_cast<unsigned>(layout->iteratorToCharIndex(
                    std::min(tool->text_sel_start, tool->text_sel_end)));
                auto last = static_cast<unsigned>(layout->iteratorToCharIndex(
                    std::max(tool->text_sel_start, tool->text_sel_end)));
                if (containsUnsupportedTref(item, first, last)) return {};
                targets.push_back({SPWeakPtr<SPItem>(item), first, last, false});
            }
        }
    }
    if (targets.empty()) {
        for (auto item : selectedTextItems()) {
            auto layout = canonical_layout(item);
            if (layout) {
                targets.push_back({SPWeakPtr<SPItem>(item), 0,
                    static_cast<unsigned>(layout->iteratorToCharIndex(layout->end())), true});
            }
        }
    }

    std::vector<SPStyle const *> result;
    std::unordered_set<SPStyle const *> seen;
    for (auto const &target : targets) {
        auto item = target.item.get();
        auto layout = canonical_layout(item);
        if (!isEligibleTextItem(item) || !layout) continue;
        bool target_has_style = false;
        auto iter = layout->begin();
        for (unsigned i = 0; i < target.first_char && iter != layout->end(); ++i) iter.nextCharacter();
        for (unsigned i = target.first_char; i < target.last_char && iter != layout->end();
             ++i, iter.nextCharacter()) {
            SPObject *source = nullptr;
            layout->getSourceOfCharacter(iter, &source);
            source = nearest_style_source(source, item);
            if (source) {
                target_has_style = true;
                if (seen.insert(source->style).second) result.push_back(source->style);
            }
        }
        if (!target_has_style && item->style && seen.insert(item->style).second) {
            result.push_back(item->style);
        }
    }
    return result;
}

std::vector<TextStyleTarget> TextStyleController::currentParagraphTargets() const
{
    if (!_desktop) return {};

    auto append_ranges = [&](std::vector<TextStyleTarget> &targets, SPItem *item,
                             unsigned first, unsigned last) {
        auto layout = canonical_layout(item);
        if (!isEligibleTextItem(item) || !layout) return;
        for (auto const &range : paragraphRanges(*layout, first, last)) {
            if (containsUnsupportedTref(item, range.first, range.last)) {
                targets.clear();
                return;
            }
            targets.push_back({SPWeakPtr<SPItem>(item), range.first, range.last, false});
        }
    };

    std::vector<TextStyleTarget> targets;
    if (auto tool = dynamic_cast<Tools::TextTool *>(_desktop->getTool())) {
        auto item = tool->textItem();
        auto layout = canonical_layout(item);
        if (item && layout) {
            auto first = static_cast<unsigned>(layout->iteratorToCharIndex(
                std::min(tool->text_sel_start, tool->text_sel_end)));
            auto last = static_cast<unsigned>(layout->iteratorToCharIndex(
                std::max(tool->text_sel_start, tool->text_sel_end)));
            append_ranges(targets, item, first, last);
            return targets;
        }
    }

    for (auto item : selectedTextItems()) {
        auto layout = canonical_layout(item);
        if (!layout) continue;
        auto const count = static_cast<unsigned>(layout->iteratorToCharIndex(layout->end()));
        append_ranges(targets, item, 0, count);
        if (targets.empty() && count != 0) return {};
    }
    return targets;
}

std::vector<SPStyle const *> TextStyleController::paragraphStyles(
    std::vector<TextStyleTarget> const &targets) const
{
    std::vector<SPStyle const *> result;
    std::unordered_set<SPStyle const *> seen;
    for (auto const &target : targets) {
        auto item = target.item.get();
        auto layout = canonical_layout(item);
        if (!item || !layout) continue;

        // Structural SVG/legacy-flow paragraphs own their paragraph style.
        // Reading the first layout character is incorrect once generated list
        // markers or automatic hyphens precede the author's text.
        if (auto paragraph = paragraphObject(target);
            paragraph && paragraph != item && paragraph->style) {
            if (seen.insert(paragraph->style).second) result.push_back(paragraph->style);
            continue;
        }
        bool target_has_style = false;

        auto iter = layout->begin();
        for (unsigned i = 0; i < target.first_char && iter != layout->end(); ++i) {
            iter.nextCharacter();
        }
        for (unsigned i = target.first_char;
             i < target.last_char && iter != layout->end(); ++i, iter.nextCharacter()) {
            SPObject *source = nullptr;
            layout->getSourceOfCharacter(iter, &source);
            while (source && !source->style) source = source->parent;
            if (source && source->style) {
                target_has_style = true;
                if (seen.insert(source->style).second) result.push_back(source->style);
                break;
            }
        }
        if (!target_has_style && item->style && seen.insert(item->style).second) {
            result.push_back(item->style);
        }
    }
    return result;
}

SPObject *TextStyleController::paragraphObject(TextStyleTarget const &target) const
{
    auto item = target.item.get();
    auto layout = canonical_layout(item);
    if (!item || !layout) return nullptr;

    auto iter = layout->charIndexToIterator(target.first_char);
    SPObject *source = nullptr;
    if (iter != layout->end()) layout->getSourceOfCharacter(iter, &source);

    for (auto current = source; current && current != item; current = current->parent) {
        if (auto tspan = cast<SPTSpan>(current);
            tspan && tspan->role == SP_TSPAN_ROLE_PARAGRAPH) {
            return tspan;
        }
        if (is<SPFlowpara>(current)) return current;
    }
    return item;
}

std::vector<SPObject *> TextStyleController::paragraphObjects(
    std::vector<TextStyleTarget> const &targets) const
{
    std::vector<SPObject *> result;
    std::unordered_set<SPObject *> seen;
    for (auto const &target : targets) {
        if (auto paragraph = paragraphObject(target);
            seen.insert(paragraph).second) result.push_back(paragraph);
    }
    return result;
}

namespace {

TextParagraphAlignment paragraph_alignment(SPStyle const &style)
{
    switch (style.text_align.computed) {
        case SP_CSS_TEXT_ALIGN_CENTER: return TextParagraphAlignment::Center;
        case SP_CSS_TEXT_ALIGN_END: return TextParagraphAlignment::End;
        case SP_CSS_TEXT_ALIGN_JUSTIFY: return TextParagraphAlignment::Justify;
        case SP_CSS_TEXT_ALIGN_LEFT: return TextParagraphAlignment::Left;
        case SP_CSS_TEXT_ALIGN_RIGHT: return TextParagraphAlignment::Right;
        case SP_CSS_TEXT_ALIGN_START:
        default: return TextParagraphAlignment::Start;
    }
}

// The native whole-object alignment writes text-align and text-anchor as one
// pair. A run whose text-anchor disagrees with the anchor implied by its
// computed alignment renders unaligned even though text-align alone reads as
// the requested value; the query must not report that state as settled.
bool alignment_anchor_agrees(SPStyle const &style)
{
    auto const alignment = paragraph_alignment(style);
    SPTextAnchor expected = SP_CSS_TEXT_ANCHOR_START;
    switch (alignment) {
        case TextParagraphAlignment::Center:
            expected = SP_CSS_TEXT_ANCHOR_MIDDLE;
            break;
        case TextParagraphAlignment::End:
            expected = SP_CSS_TEXT_ANCHOR_END;
            break;
        case TextParagraphAlignment::Justify:
            expected = SP_CSS_TEXT_ANCHOR_START;
            break;
        case TextParagraphAlignment::Left:
            expected = style.direction.computed == SP_CSS_DIRECTION_RTL
                ? SP_CSS_TEXT_ANCHOR_END : SP_CSS_TEXT_ANCHOR_START;
            break;
        case TextParagraphAlignment::Right:
            expected = style.direction.computed == SP_CSS_DIRECTION_RTL
                ? SP_CSS_TEXT_ANCHOR_START : SP_CSS_TEXT_ANCHOR_END;
            break;
        case TextParagraphAlignment::Start:
        default:
            expected = SP_CSS_TEXT_ANCHOR_START;
            break;
    }
    return style.text_anchor.computed == expected;
}

TextParagraphWritingMode paragraph_writing_mode(SPStyle const &style)
{
    switch (style.writing_mode.computed) {
        case SP_CSS_WRITING_MODE_TB_RL:
            return TextParagraphWritingMode::VerticalRightToLeft;
        case SP_CSS_WRITING_MODE_TB_LR:
            return TextParagraphWritingMode::VerticalLeftToRight;
        default:
            return TextParagraphWritingMode::Horizontal;
    }
}

TextParagraphOrientation paragraph_orientation(SPStyle const &style)
{
    switch (style.text_orientation.computed) {
        case SP_CSS_TEXT_ORIENTATION_UPRIGHT: return TextParagraphOrientation::Upright;
        case SP_CSS_TEXT_ORIENTATION_SIDEWAYS: return TextParagraphOrientation::Sideways;
        case SP_CSS_TEXT_ORIENTATION_MIXED:
        default: return TextParagraphOrientation::Mixed;
    }
}

bool has_authored_paragraph_descendant(SPObject const &object)
{
    for (auto const &child : object.children) {
        if (auto tspan = cast<SPTSpan>(&child);
            tspan && tspan->role == SP_TSPAN_ROLE_PARAGRAPH) {
            return true;
        }
        if (has_authored_paragraph_descendant(child)) return true;
    }
    return false;
}

// An ordinary object-scoped text reads its alignment from the root and every
// descendant: a hard line that authors a conflicting override must defeat the
// no-op check so the whole-object request can resolve it.
void collect_alignment_styles(SPObject const &object,
                              std::vector<SPStyle const *> &styles,
                              std::unordered_set<SPStyle const *> &seen)
{
    if (object.style && seen.insert(object.style).second) {
        styles.push_back(object.style);
    }
    for (auto const &child : object.children) {
        collect_alignment_styles(child, styles, seen);
    }
}

// An ordinary SVG <text> whose alignment and line height are a single
// object-scoped value, unlike wrapped text, flowtext or SVG2 text with authored
// role=paragraph tspans, whose style belongs to individual paragraphs.
bool is_ordinary_object_scoped_text(SPItem const *item)
{
    auto text = cast<SPText>(item);
    return text && !text->has_inline_size() && !text->has_shape_inside() &&
           !has_authored_paragraph_descendant(*text);
}

// Absolute line heights are stored in the item's local coordinate context by
// the native outer write; report them in document units so the panel request and
// the no-op comparison share one space. Relative units stay relative.
TextLineHeightValue to_document_line_height(TextLineHeightValue value, double descrim)
{
    auto const relative = value.unit == TextLineHeightUnit::Lines ||
                          value.unit == TextLineHeightUnit::Percent;
    if (!relative && std::isfinite(descrim) && descrim > 0) {
        value.value *= descrim;
    }
    return value;
}

// Inverse of to_document_line_height for paragraph-local writes: an absolute
// request is stored in the paragraph's local coordinate context, while relative
// units remain relative and untouched.
TextLineHeightValue from_document_line_height(TextLineHeightValue value, double descrim)
{
    auto const relative = value.unit == TextLineHeightUnit::Lines ||
                          value.unit == TextLineHeightUnit::Percent;
    if (!relative && std::isfinite(descrim) && descrim > 0) {
        value.value /= descrim;
    }
    return value;
}

// Absolute units are serialized to px before the inverse-transform write, so two
// absolute requests are equivalent when they denote the same length regardless
// of the entered unit. Relative values compare within their own unit.
bool equivalent_line_height(TextLineHeightValue const &a, TextLineHeightValue const &b)
{
    auto const relative = [](TextLineHeightUnit unit) {
        return unit == TextLineHeightUnit::Lines || unit == TextLineHeightUnit::Percent;
    };
    if (relative(a.unit) || relative(b.unit)) {
        return a.unit == b.unit && close_enough(a.value, b.value);
    }
    return close_enough(convertLineHeight(a, TextLineHeightUnit::Px, 1.0).value,
                        convertLineHeight(b, TextLineHeightUnit::Px, 1.0).value);
}

// Whole-object line-height style state for an ordinary SPText. Compare the
// root's CSS line-height with every descendant's, each mapped through
// lineHeightFromStyle and to_document_line_height before equivalent_line_height.
// A differing descendant marks the object mixed even when it resolves below the
// root's computed floor: this reports authored style state, not rendered metrics.
bool object_line_height_style(SPText const &text, TextLineHeightValue &value, bool &mixed)
{
    if (!text.style) return false;
    auto const descrim = text.i2doc_affine().descrim();
    value = to_document_line_height(lineHeightFromStyle(*text.style), descrim);
    mixed = false;
    std::vector<SPObject const *> pending;
    for (auto const &child : text.children) pending.push_back(&child);
    while (!pending.empty()) {
        auto object = pending.back();
        pending.pop_back();
        if (!object) continue;
        if (object->style) {
            auto const descendant = to_document_line_height(
                lineHeightFromStyle(*object->style), descrim);
            if (!equivalent_line_height(value, descendant)) {
                mixed = true;
                break;
            }
        }
        for (auto const &child : object->children) pending.push_back(&child);
    }
    return true;
}

// Original whole-object line-height scope, read from the active TextTool
// selection before paragraphRanges() expands a caret or full selection to
// paragraph boundaries. Caret, full text range and normal object/group
// selection are whole-object; an explicit partial character range is not.
std::vector<SPText *> whole_object_line_height_owners(
    SPDesktop *desktop, std::vector<SPItem *> const &selected)
{
    std::vector<SPItem *> candidates;
    if (auto tool = desktop ? dynamic_cast<Tools::TextTool *>(desktop->getTool()) : nullptr) {
        auto item = tool->textItem();
        auto layout = canonical_layout(item);
        if (item && layout) {
            auto const count = static_cast<unsigned>(layout->iteratorToCharIndex(layout->end()));
            auto const first = static_cast<unsigned>(layout->iteratorToCharIndex(
                std::min(tool->text_sel_start, tool->text_sel_end)));
            auto const last = static_cast<unsigned>(layout->iteratorToCharIndex(
                std::max(tool->text_sel_start, tool->text_sel_end)));
            bool const full_range = first == 0 && last == count;
            if (tool->text_sel_start != tool->text_sel_end && !full_range) {
                return {};
            }
            candidates.push_back(item);
        }
    }
    if (candidates.empty()) candidates = selected;

    std::vector<SPText *> owners;
    for (auto item : candidates) {
        if (auto text = cast<SPText>(item); text && is_ordinary_object_scoped_text(item)) {
            owners.push_back(text);
        }
    }
    return owners;
}

// An explicit, non-collapsed, non-full TextTool character range over an ordinary
// object-scoped SPText. Native line-height line-expands that range
// (text-toolbar.cpp thisStartOfLine/thisEndOfLine) and applies the inner
// operation; the controller must do the same instead of paragraphRanges().
// Logical indices are captured before any root mutation and are ordered so the
// shared engine sees a valid range; raw_start/raw_end keep the selection
// direction for reacquisition after layouts rebuild.
struct InnerLineHeightTarget {
    SPText *text = nullptr;
    unsigned raw_start = 0;
    unsigned raw_end = 0;
    unsigned line_first = 0;
    unsigned line_last = 0;
};

std::vector<InnerLineHeightTarget> inner_line_height_targets(SPDesktop *desktop)
{
    std::vector<InnerLineHeightTarget> targets;
    if (!desktop) return targets;
    auto tool = dynamic_cast<Tools::TextTool *>(desktop->getTool());
    if (!tool) return targets;
    auto item = tool->textItem();
    if (!item || !is_ordinary_object_scoped_text(item)) return targets;
    auto layout = canonical_layout(item);
    if (!layout) return targets;
    if (tool->text_sel_start == tool->text_sel_end) return targets;

    auto const count = static_cast<unsigned>(layout->iteratorToCharIndex(layout->end()));
    auto const raw_start = static_cast<unsigned>(layout->iteratorToCharIndex(tool->text_sel_start));
    auto const raw_end = static_cast<unsigned>(layout->iteratorToCharIndex(tool->text_sel_end));
    auto const first = std::min(raw_start, raw_end);
    auto const last = std::max(raw_start, raw_end);
    if (first == 0 && last == count) return targets;

    auto line_start = layout->charIndexToIterator(first);
    line_start.thisStartOfLine();
    auto line_end = layout->charIndexToIterator(last);
    line_end.thisEndOfLine();
    auto const line_first = static_cast<unsigned>(layout->iteratorToCharIndex(line_start));
    auto const line_last = static_cast<unsigned>(layout->iteratorToCharIndex(line_end));
    if (line_first >= line_last) return targets;

    targets.push_back({cast<SPText>(item), raw_start, raw_end, line_first, line_last});
    return targets;
}

// Authored line-height values over the line-expanded range the native inner
// operation would touch, in document units. Reading the paragraph's first run
// (or a single selected glyph) misreports the panel and makes the no-op check
// wrongly skip a real change. While the owner's root holds a nonzero authored
// value it also participates, because the root strut still forces the line;
// an explicit zero root is the sentinel written by prepareInnerRootLineHeight
// after normalization and must not keep the query mixed, or every repeated
// equal request would be stuck mixed and no-op/Redo would be destroyed.
std::vector<TextLineHeightValue> inner_line_height_values(SPText &text,
                                                          InnerLineHeightTarget const &target)
{
    std::vector<TextLineHeightValue> values;
    auto layout = canonical_layout(&text);
    if (!layout) return values;
    auto const descrim = text.i2doc_affine().descrim();
    if (text.style) {
        auto const root = to_document_line_height(lineHeightFromStyle(*text.style), descrim);
        if (!close_enough(root.value, 0.0)) values.push_back(root);
    }
    auto iter = layout->charIndexToIterator(static_cast<int>(target.line_first));
    for (unsigned index = target.line_first; index < target.line_last; ++index) {
        if (iter == layout->end()) break;
        SPObject *source = nullptr;
        layout->getSourceOfCharacter(iter, &source);
        source = nearest_style_source(source, &text);
        if (source && source->style) {
            values.push_back(to_document_line_height(lineHeightFromStyle(*source->style), descrim));
        }
        iter.nextCharacter();
    }
    return values;
}

} // namespace

TextParagraphSnapshot TextStyleController::queryParagraph() const
{
    TextParagraphSnapshot snapshot;
    auto const targets = currentParagraphTargets();
    auto const styles = paragraphStyles(targets);
    if (targets.empty() || styles.empty()) return snapshot;

    snapshot.has_text_target = true;
    // Ordinary text owns one object-scoped alignment value; reading the first
    // layout run would misreport it when runs carry inherited character styles.
    // Wrapped/flowtext/authored paragraphs keep their paragraph-level styles.
    std::vector<SPStyle const *> alignment_styles;
    std::unordered_set<SPStyle const *> alignment_seen;
    bool ordinary_anchor_conflict = false;
    for (auto const &target : targets) {
        auto item = target.item.get();
        if (!item || !item->style) continue;
        if (is_ordinary_object_scoped_text(item)) {
            auto const first_new = alignment_styles.size();
            collect_alignment_styles(*item, alignment_styles, alignment_seen);
            for (auto i = first_new; i < alignment_styles.size(); ++i) {
                if (!alignment_anchor_agrees(*alignment_styles[i])) {
                    ordinary_anchor_conflict = true;
                    break;
                }
            }
            continue;
        }
        for (auto style : paragraphStyles(std::vector<TextStyleTarget>{target})) {
            if (alignment_seen.insert(style).second) alignment_styles.push_back(style);
        }
    }
    if (alignment_styles.empty()) alignment_styles = styles;
    populate_value(snapshot.alignment, alignment_styles, paragraph_alignment);
    // A per-run anchor that disagrees with the computed alignment is invisible
    // to the text-align value alone; report it as mixed so paragraphPatchIsNoOp
    // does not skip the native whole-object repair.
    if (ordinary_anchor_conflict) snapshot.alignment.mixed = true;
    // Object-scoped line height for ordinary whole-object text: the authored CSS
    // style state, expressed in document units to match the native outer write.
    // A differing descendant value makes the snapshot mixed even below the root
    // floor. Paragraph-local (wrapped/authored) targets keep their first-run
    // value but use the same document units; paragraph-local describes the
    // selection scope, not a separate unit system. An ordinary explicit partial
    // range is the native inner scope, so it reports the line-expanded styles it
    // would actually write instead of the paragraph's first run.
    {
        auto const whole_owners = whole_object_line_height_owners(_desktop, selectedTextItems());
        std::unordered_set<SPText *> const whole_set(whole_owners.begin(), whole_owners.end());
        auto const inner_targets = inner_line_height_targets(_desktop);
        std::unordered_set<SPText *> inner_set;
        for (auto const &inner : inner_targets) {
            if (inner.text) inner_set.insert(inner.text);
        }
        std::unordered_set<SPText *> evaluated_owners;
        std::vector<TextLineHeightValue> values;
        bool forced_mixed = false;
        for (auto const &target : targets) {
            auto item = target.item.get();
            if (auto text = cast<SPText>(item); text && whole_set.contains(text)) {
                // A multi-line owner contributes one value no matter how many
                // paragraph targets expansion produced for it.
                if (!evaluated_owners.insert(text).second) continue;
                TextLineHeightValue value;
                bool mixed = false;
                if (object_line_height_style(*text, value, mixed)) {
                    values.push_back(value);
                    forced_mixed |= mixed;
                }
                continue;
            }
            if (auto text = cast<SPText>(item); text && inner_set.contains(text)) {
                // The native inner scope is the line-expanded range; report it
                // once per owner, not once per paragraph target.
                if (!evaluated_owners.insert(text).second) continue;
                auto const inner = std::ranges::find(inner_targets, text,
                                                     &InnerLineHeightTarget::text);
                if (inner != inner_targets.end()) {
                    auto const line_values = inner_line_height_values(*text, *inner);
                    values.insert(values.end(), line_values.begin(), line_values.end());
                }
                continue;
            }
            auto const descrim = item ? item->i2doc_affine().descrim() : 1.0;
            for (auto style : paragraphStyles(std::vector<TextStyleTarget>{target})) {
                values.push_back(to_document_line_height(lineHeightFromStyle(*style), descrim));
            }
        }
        if (!values.empty()) {
            snapshot.line_height.valid = true;
            snapshot.line_height.value = values.front();
            snapshot.line_height.mixed = forced_mixed ||
                std::ranges::any_of(values | std::views::drop(1), [&](auto const &other) {
                    return !equivalent_line_height(snapshot.line_height.value, other);
                });
        }
    }
    populate_value(snapshot.first_line_indent_px, styles, [](SPStyle const &style) {
        return style.text_indent.computed;
    }, close_enough);
    populate_value(snapshot.spacing_before_px, styles, [](SPStyle const &style) {
        return style.paragraph_spacing_before.computed;
    }, close_enough);
    populate_value(snapshot.spacing_after_px, styles, [](SPStyle const &style) {
        return style.paragraph_spacing_after.computed;
    }, close_enough);
    auto const paragraph_objects = paragraphObjects(targets);
    if (!paragraph_objects.empty()) {
        snapshot.list_mode.valid = true;
        snapshot.list_mode.value = paragraphListMode(*paragraph_objects.front());
        for (auto object : paragraph_objects) {
            if (paragraphListMode(*object) != snapshot.list_mode.value) {
                snapshot.list_mode.mixed = true;
                break;
            }
        }
        snapshot.drop_cap_lines.valid = true;
        snapshot.drop_cap_lines.value = paragraphDropCapLines(*paragraph_objects.front());
        for (auto object : paragraph_objects) {
            if (paragraphDropCapLines(*object) != snapshot.drop_cap_lines.value) {
                snapshot.drop_cap_lines.mixed = true;
                break;
            }
        }
        if (!snapshot.list_mode.mixed && snapshot.list_mode.value == TextListMode::Numbered) {
            snapshot.list_start.valid = true;
            snapshot.list_start.value = paragraphListStart(*paragraph_objects.front());
        }
        snapshot.hyphenation.valid = true;
        snapshot.hyphenation.value = paragraphHyphenation(*paragraph_objects.front());
        snapshot.hyphenation_available = std::ranges::all_of(paragraph_objects, [](auto object) {
            return paragraphHasHyphenationDictionary(*object);
        });
        for (auto object : paragraph_objects) {
            if (paragraphHyphenation(*object) != snapshot.hyphenation.value) {
                snapshot.hyphenation.mixed = true;
                break;
            }
        }
    }
    populate_value(snapshot.direction, styles, [](SPStyle const &style) {
        return style.direction.computed == SP_CSS_DIRECTION_RTL
             ? TextParagraphDirection::RightToLeft : TextParagraphDirection::LeftToRight;
    });
    populate_value(snapshot.writing_mode, styles, paragraph_writing_mode);
    populate_value(snapshot.orientation, styles, paragraph_orientation);
    return snapshot;
}

TextFrameSnapshot TextStyleController::queryFrame() const
{
    TextFrameSnapshot snapshot;
    std::vector<SPText *> texts;
    for (auto item : selectedTextItems()) {
        if (auto text = cast<SPText>(item)) texts.push_back(text);
    }
    if (texts.empty()) return snapshot;
    snapshot.has_text_target = true;
    auto effective_settings = [](SPText &text) {
        auto settings = textFrameSettings(text);
        if ((settings.width <= 0.0 || settings.height <= 0.0)) {
            if (auto bounds = text.geometricBounds()) {
                if (settings.width <= 0.0) settings.width = std::max(1.0, bounds->width());
                if (settings.height <= 0.0) settings.height = std::max(1.0, bounds->height());
            }
        }
        return settings;
    };
    auto first = effective_settings(*texts.front());
    auto set = [](auto &field, auto value) { field.valid = true; field.value = value; };
    set(snapshot.width_px, first.width);
    set(snapshot.height_px, first.height);
    set(snapshot.columns, first.columns);
    set(snapshot.gap_px, first.gap);
    set(snapshot.vertical_alignment, first.vertical_alignment);
    for (auto text : texts | std::views::drop(1)) {
        auto value = effective_settings(*text);
        if (!close_enough(value.width, first.width)) snapshot.width_px.mixed = true;
        if (!close_enough(value.height, first.height)) snapshot.height_px.mixed = true;
        if (value.columns != first.columns) snapshot.columns.mixed = true;
        if (!close_enough(value.gap, first.gap)) snapshot.gap_px.mixed = true;
        if (value.vertical_alignment != first.vertical_alignment) snapshot.vertical_alignment.mixed = true;
    }
    return snapshot;
}

bool TextStyleController::supportsOpenTypeFeature(std::string_view tag) const
{
    if (!_desktop || tag.size() != 4) return false;

    auto const styles = currentRunStyles();
    if (styles.empty()) return false;

    auto const feature = Glib::ustring{tag.data(), static_cast<Glib::ustring::size_type>(tag.size())};
    for (auto const *style : styles) {
        try {
            auto const spec = FontLister::get_instance()->fontspec_from_style(const_cast<SPStyle *>(style));
            auto font = FontFactory::get().FaceFromFontSpecification(spec.c_str());
            if (!font || !font->get_opentype_tables().contains(feature)) return false;
        } catch (...) {
            return false;
        }
    }
    return true;
}

TextStyleSnapshot TextStyleController::query() const
{
    TextStyleSnapshot snapshot;
    auto const styles = currentRunStyles();
    if (styles.empty()) return snapshot;
    snapshot.has_text_target = true;

    auto lister = FontLister::get_instance();
    populate_value(snapshot.family, styles, [](SPStyle const &style) {
        return Glib::ustring{style.font_family.value() ? style.font_family.value() : "sans-serif"};
    });
    populate_value(snapshot.fontspec, styles, [lister](SPStyle const &style) {
        return lister->fontspec_from_style(const_cast<SPStyle *>(&style));
    });
    populate_value(snapshot.face, styles, [lister](SPStyle const &style) {
        auto const spec = lister->fontspec_from_style(const_cast<SPStyle *>(&style));
        return lister->ui_from_fontspec(spec).second;
    });
    populate_value(snapshot.font_size_px, styles, [](SPStyle const &style) {
        auto item = cast<SPItem>(style.object);
        return style.font_size.computed * (item ? item->i2doc_affine().descrim() : 1.0);
    }, close_enough);
    populate_value(snapshot.bold, styles, [](SPStyle const &style) {
        return static_cast<int>(style.font_weight.computed) > 400;
    });
    populate_value(snapshot.italic, styles, [](SPStyle const &style) {
        return style.font_style.computed == SP_CSS_FONT_STYLE_ITALIC ||
               style.font_style.computed == SP_CSS_FONT_STYLE_OBLIQUE;
    });
    populate_value(snapshot.underline, styles, [](SPStyle const &style) {
        return style.text_decoration_line.underline;
    });
    populate_value(snapshot.script_position, styles, script_position);

    auto const first_caps = capitalization(*styles.front());
    snapshot.capitalization = {first_caps.first, true, false};
    snapshot.capitalization_custom = first_caps.second;
    for (auto style : styles | std::views::drop(1)) {
        auto const value = capitalization(*style);
        snapshot.capitalization.mixed |= value != first_caps;
        snapshot.capitalization_custom |= value.second;
    }

    populate_value(snapshot.standard_ligatures, styles, [](SPStyle const &style) {
        return bool(style.font_variant_ligatures.computed &
                    SP_CSS_FONT_VARIANT_LIGATURES_COMMON);
    });
    populate_value(snapshot.character_spacing_percent, styles, [](SPStyle const &style) {
        auto const advance = effective_space_advance(style);
        return style.letter_spacing.normal ? 0.0
             : style.letter_spacing.computed / advance * 100.0;
    }, close_enough);
    populate_value(snapshot.word_spacing_percent, styles, [](SPStyle const &style) {
        auto const advance = effective_space_advance(style);
        return style.word_spacing.normal ? 100.0
             : 100.0 + style.word_spacing.computed / advance * 100.0;
    }, close_enough);
    populate_value(snapshot.language_spacing_percent, styles, [](SPStyle const &style) {
        if (style.language_spacing.normal) return 0.0;
        if (style.language_spacing.unit == SP_CSS_UNIT_PERCENT) {
            return style.language_spacing.value * 100.0;
        }
        auto const advance = effective_space_advance(style);
        return style.language_spacing.computed / advance * 100.0;
    }, close_enough);
    populate_value(snapshot.fill, styles, [](SPStyle const &style) {
        return style.fill.get_value();
    });
    populate_value(snapshot.stroke, styles, [](SPStyle const &style) {
        return style.stroke.get_value();
    });
    return snapshot;
}

void TextStyleController::preview(TextStylePatch const &patch, TextStyleOrigin origin)
{
    if (!live_text_style_preview_enabled || patch.empty() ||
        (!_targets.empty() ? false : !captureTargets())) {
        return;
    }
    if ((_pending && *_pending == patch) || (_visible && *_visible == patch)) {
        return;
    }
    _scheduled.disconnect();
    _pending = patch;
    auto const generation = ++_generation;
    auto publish_latest = [this, generation] {
        if (_pending && generation == _generation) publish(*_pending, generation);
        return false;
    };
    if (origin == TextStyleOrigin::Keyboard) {
        _scheduled = Glib::signal_idle().connect(std::move(publish_latest), Glib::PRIORITY_HIGH_IDLE);
    } else if (origin == TextStyleOrigin::Pointer) {
        _scheduled = Glib::signal_timeout().connect(std::move(publish_latest),
            _slow_candidate ? slow_pointer_dwell_ms : pointer_dwell_ms);
    } else {
        publish(patch, generation);
    }
}

bool TextStyleController::publish(TextStylePatch const &patch, uint64_t generation)
{
    if (!_desktop || generation != _generation) return false;
    if (!targetsValid()) {
        cancelPreview();
        return false;
    }
    auto const started = std::chrono::steady_clock::now();
    std::vector<SPItem *> published;
    for (auto const &target : _targets) {
        auto item = target.item.get();
        if (!item) {
            cancelPreview();
            return false;
        }
        if (target.first_char == target.last_char) continue; // Empty text has nothing to preview.
        Text::Layout::TypographyOverride override;
        override.first_char = target.first_char;
        override.last_char = target.last_char;
        if (patch.family) override.family = *patch.family;
        override.face = patch.face;
        if (patch.fontspec) override.fontspec = *patch.fontspec;
        if (patch.capitalization) {
            override.c2sc = *patch.capitalization == CapitalizationMode::SmallCapsFromCaps;
            // Every choice in this menu replaces the menu-managed transform.
            // This is essential when previewing away from an existing forced
            // synthesized or uppercase state.
            override.text_transform = SP_CSS_TEXT_TRANSFORM_NONE;
            switch (*patch.capitalization) {
                case CapitalizationMode::None:
                    override.font_variant_caps = SP_CSS_FONT_VARIANT_CAPS_NORMAL;
                    break;
                case CapitalizationMode::AllCaps:
                    override.text_transform = SP_CSS_TEXT_TRANSFORM_UPPERCASE;
                    override.font_variant_caps = SP_CSS_FONT_VARIANT_CAPS_NORMAL;
                    break;
                case CapitalizationMode::TitlingCaps:
                    override.font_variant_caps = SP_CSS_FONT_VARIANT_CAPS_TITLING;
                    break;
                case CapitalizationMode::SmallCapsAuto:
                    override.font_variant_caps = SP_CSS_FONT_VARIANT_CAPS_SMALL;
                    break;
                case CapitalizationMode::AllSmallCaps:
                    override.font_variant_caps = SP_CSS_FONT_VARIANT_CAPS_ALL_SMALL;
                    break;
                case CapitalizationMode::SmallCapsFromCaps:
                    override.font_variant_caps = SP_CSS_FONT_VARIANT_CAPS_NORMAL;
                    break;
                case CapitalizationMode::SmallCapsSynthesized:
                    override.font_variant_caps = SP_CSS_FONT_VARIANT_CAPS_SMALL;
                    override.force_synthesized_caps = true;
                    break;
            }
        }
        if (patch.script_position) {
            switch (*patch.script_position) {
                case TextScriptPosition::Normal:
                    override.font_variant_position = SP_CSS_FONT_VARIANT_POSITION_NORMAL; break;
                case TextScriptPosition::Superscript:
                    override.font_variant_position = SP_CSS_FONT_VARIANT_POSITION_SUPER; break;
                case TextScriptPosition::Subscript:
                    override.font_variant_position = SP_CSS_FONT_VARIANT_POSITION_SUB; break;
            }
        }
        if (!set_item_preview(item, _desktop->dkey, {std::move(override)})) {
            // A preview is all-or-nothing across the frozen target set.  Clear
            // every target, rather than just the objects published in this
            // generation, so an older preview cannot survive on an object that
            // appears later in the target list.
            for (auto const &candidate : _targets) {
                if (auto target_item = candidate.item.get()) {
                    clear_item_preview(target_item, _desktop->dkey);
                }
            }
            _visible.reset();
            _pending.reset();
            return false;
        }
        published.push_back(item);
    }
    if (generation != _generation) {
        for (auto item : published) clear_item_preview(item, _desktop->dkey);
        return false;
    }
    _slow_candidate = std::chrono::steady_clock::now() - started > std::chrono::milliseconds(50);
    _visible = patch;
    _pending.reset();
    refreshDisplayGeometry();
    return true;
}

SPCSSAttr *TextStyleController::cssForPatch(TextStylePatch const &patch, SPStyle const &base) const
{
    auto css = sp_repr_css_attr_new();
    if (patch.face && patch.fontspec) {
        FontLister::get_instance()->fill_css(css, *patch.fontspec);
        // A face choice owns family/weight/slant/stretch and variation axes,
        // but it must not reset independent OpenType typography such as caps,
        // ligatures, numeric variants, or stylistic sets.
        sp_repr_css_unset_property(css, "font-variant");
        auto description = pango_font_description_from_string(patch.fontspec->c_str());
        auto const variations = pango_font_description_get_variations(description);
        if (!variations || !*variations) {
            // Exact static faces discard axes inherited from a previous
            // variable face. An empty patch value removes the target CSS.
            sp_repr_css_set_property(css, "font-variation-settings", "");
        }
        pango_font_description_free(description);
    } else if (patch.family) {
        auto family = *patch.family;
        css_font_family_quote(family);
        sp_repr_css_set_property(css, "font-family", family.c_str());
        sp_repr_css_set_property(css, "-inkscape-font-specification", "");
    }
    if (patch.font_size_px) sp_repr_css_set_property(css, "font-size", format_px(*patch.font_size_px).c_str());
    if (patch.bold) sp_repr_css_set_property(css, "font-weight", *patch.bold ? "bold" : "normal");
    if (patch.italic) sp_repr_css_set_property(css, "font-style", *patch.italic ? "italic" : "normal");
    if (patch.underline) {
        sp_repr_css_set_property(css, "text-decoration-line",
                                 decoration_line(base, *patch.underline).c_str());
    }
    if (patch.script_position) {
        char const *position = "normal";
        if (*patch.script_position == TextScriptPosition::Superscript) position = "super";
        if (*patch.script_position == TextScriptPosition::Subscript) position = "sub";
        sp_repr_css_set_property(css, "font-variant-position", position);
        sp_repr_css_set_property(css, "baseline-shift", "baseline");
        bool legacy = base.font_size.unit == SP_CSS_UNIT_PERCENT &&
                      std::abs(base.font_size.value - 0.65) < 0.02 &&
                      base.baseline_shift.type == SP_BASELINE_SHIFT_LITERAL;
        if (legacy) sp_repr_css_set_property(css, "font-size", "");
    }
    if (patch.capitalization) {
        auto features = Glib::ustring(base.font_feature_settings.value());
        features = merge_font_feature(features, "c2sc", std::nullopt);
        sp_repr_css_set_property(css, "text-transform", "none");
        sp_repr_css_set_property(css, "font-variant-caps", "normal");
        sp_repr_css_set_property(css, "-inkscape-font-variant-caps-mode", "");
        switch (*patch.capitalization) {
            case CapitalizationMode::None: break;
            case CapitalizationMode::AllCaps:
                sp_repr_css_set_property(css, "text-transform", "uppercase"); break;
            case CapitalizationMode::TitlingCaps:
                sp_repr_css_set_property(css, "font-variant-caps", "titling-caps"); break;
            case CapitalizationMode::SmallCapsAuto:
                sp_repr_css_set_property(css, "font-variant-caps", "small-caps"); break;
            case CapitalizationMode::AllSmallCaps:
                sp_repr_css_set_property(css, "font-variant-caps", "all-small-caps"); break;
            case CapitalizationMode::SmallCapsFromCaps:
                features = merge_font_feature(features, "c2sc", true); break;
            case CapitalizationMode::SmallCapsSynthesized:
                sp_repr_css_set_property(css, "font-variant-caps", "small-caps");
                sp_repr_css_set_property(css, "-inkscape-font-variant-caps-mode", "synthesized");
                break;
        }
        sp_repr_css_set_property(css, "font-feature-settings", features.c_str());
    }
    if (patch.standard_ligatures) {
        sp_repr_css_set_property(css, "font-variant-ligatures",
                                 ligature_value(base, *patch.standard_ligatures).c_str());
    }
    // The percent branches express a local advance as a length. Partial
    // LogicalRange writes go through sp_te_apply_style(), which inverse-scales
    // document-space lengths by the common ancestor's descrim; the caller marks
    // these percent properties as local via TextStyleLocalSpacing so the engine
    // restores the local value instead of compensating from a guessed scale.
    // px, font-size and line-height requests are already in the engine's
    // expected space and are written verbatim.
    auto const advance = effective_space_advance(base);
    if (patch.character_spacing_percent) {
        sp_repr_css_set_property(css, "letter-spacing",
            format_px(advance * *patch.character_spacing_percent / 100.0).c_str());
    } else if (patch.character_spacing_px) {
        sp_repr_css_set_property(css, "letter-spacing", format_px(*patch.character_spacing_px).c_str());
    }
    if (patch.word_spacing_percent) {
        sp_repr_css_set_property(css, "word-spacing",
            format_px(advance * (*patch.word_spacing_percent - 100.0) / 100.0).c_str());
    } else if (patch.word_spacing_px) {
        sp_repr_css_set_property(css, "word-spacing", format_px(*patch.word_spacing_px).c_str());
    }
    if (patch.language_spacing_percent) {
        sp_repr_css_set_property(css, "-inkscape-language-spacing",
                                 format_percent(*patch.language_spacing_percent).c_str());
    }
    if (patch.fill) sp_repr_css_set_property(css, "fill", patch.fill->c_str());
    if (patch.stroke) sp_repr_css_set_property(css, "stroke", patch.stroke->c_str());
    return css;
}

SPCSSAttr *TextStyleController::cssForParagraphPatch(TextParagraphPatch const &patch,
                                                     SPStyle const &base) const
{
    auto css = sp_repr_css_attr_new();
    if (patch.alignment) {
        char const *align = "start";
        char const *anchor = "start";
        switch (*patch.alignment) {
            case TextParagraphAlignment::Start:
                break;
            case TextParagraphAlignment::Center:
                align = "center";
                anchor = "middle";
                break;
            case TextParagraphAlignment::End:
                align = "end";
                anchor = "end";
                break;
            case TextParagraphAlignment::Justify:
                align = "justify";
                break;
            case TextParagraphAlignment::Left:
                align = "left";
                anchor = base.direction.computed == SP_CSS_DIRECTION_RTL ? "end" : "start";
                break;
            case TextParagraphAlignment::Right:
                align = "right";
                anchor = base.direction.computed == SP_CSS_DIRECTION_RTL ? "start" : "end";
                break;
        }
        sp_repr_css_set_property(css, "text-align", align);
        sp_repr_css_set_property(css, "text-anchor", anchor);
    }
    if (patch.line_height) {
        sp_repr_css_set_property(css, "line-height", serializeLineHeight(*patch.line_height).c_str());
    }
    if (patch.first_line_indent_px) {
        sp_repr_css_set_property(css, "text-indent", format_px(*patch.first_line_indent_px).c_str());
    }
    if (patch.spacing_before_px) {
        sp_repr_css_set_property(css, "-inkscape-paragraph-spacing-before",
                                 format_px(*patch.spacing_before_px).c_str());
    }
    if (patch.spacing_after_px) {
        sp_repr_css_set_property(css, "-inkscape-paragraph-spacing-after",
                                 format_px(*patch.spacing_after_px).c_str());
    }
    if (patch.direction) {
        sp_repr_css_set_property(css, "direction",
            *patch.direction == TextParagraphDirection::RightToLeft ? "rtl" : "ltr");
    }
    if (patch.writing_mode) {
        char const *mode = "lr-tb";
        if (*patch.writing_mode == TextParagraphWritingMode::VerticalRightToLeft) mode = "tb-rl";
        if (*patch.writing_mode == TextParagraphWritingMode::VerticalLeftToRight) mode = "vertical-lr";
        sp_repr_css_set_property(css, "writing-mode", mode);
    }
    if (patch.orientation) {
        char const *orientation = "mixed";
        if (*patch.orientation == TextParagraphOrientation::Upright) orientation = "upright";
        if (*patch.orientation == TextParagraphOrientation::Sideways) orientation = "sideways";
        sp_repr_css_set_property(css, "text-orientation", orientation);
    }
    return css;
}

bool TextStyleController::patchIsNoOp(TextStylePatch const &patch,
                                      TextStyleSnapshot const &snapshot) const
{
    if (!snapshot.has_text_target) return true;
    auto same = [](auto const &field, auto const &value) {
        return field.valid && !field.mixed && field.value == value;
    };
    if (patch.family && !same(snapshot.family, *patch.family)) return false;
    if (patch.face && !patch.fontspec && !same(snapshot.face, *patch.face)) return false;
    if (patch.face && patch.fontspec &&
        !(snapshot.fontspec.valid && !snapshot.fontspec.mixed &&
          equivalent_fontspec(snapshot.fontspec.value, *patch.fontspec))) return false;
    if (patch.font_size_px && !(snapshot.font_size_px.valid && !snapshot.font_size_px.mixed &&
                               close_enough(snapshot.font_size_px.value, *patch.font_size_px))) return false;
    if (patch.bold && !same(snapshot.bold, *patch.bold)) return false;
    if (patch.italic && !same(snapshot.italic, *patch.italic)) return false;
    if (patch.underline && !same(snapshot.underline, *patch.underline)) return false;
    if (patch.script_position && !same(snapshot.script_position, *patch.script_position)) return false;
    if (patch.capitalization &&
        (!same(snapshot.capitalization, *patch.capitalization) || snapshot.capitalization_custom)) {
        return false;
    }
    if (patch.standard_ligatures && !same(snapshot.standard_ligatures, *patch.standard_ligatures)) return false;
    if (patch.character_spacing_percent && !(snapshot.character_spacing_percent.valid &&
        !snapshot.character_spacing_percent.mixed && close_enough(snapshot.character_spacing_percent.value,
        *patch.character_spacing_percent))) return false;
    if (patch.word_spacing_percent && !(snapshot.word_spacing_percent.valid &&
        !snapshot.word_spacing_percent.mixed && close_enough(snapshot.word_spacing_percent.value,
        *patch.word_spacing_percent))) return false;
    if (patch.character_spacing_px) {
        auto const styles = currentRunStyles();
        if (styles.empty() || std::ranges::any_of(styles, [&](auto style) {
            auto const current = style->letter_spacing.normal ? 0.0 : style->letter_spacing.computed;
            return !close_enough(current, *patch.character_spacing_px);
        })) return false;
    }
    if (patch.word_spacing_px) {
        auto const styles = currentRunStyles();
        if (styles.empty() || std::ranges::any_of(styles, [&](auto style) {
            auto const current = style->word_spacing.normal ? 0.0 : style->word_spacing.computed;
            return !close_enough(current, *patch.word_spacing_px);
        })) return false;
    }
    if (patch.language_spacing_percent && !(snapshot.language_spacing_percent.valid &&
        !snapshot.language_spacing_percent.mixed && close_enough(snapshot.language_spacing_percent.value,
        *patch.language_spacing_percent))) return false;
    if (patch.fill && !same(snapshot.fill, *patch.fill)) return false;
    if (patch.stroke && !same(snapshot.stroke, *patch.stroke)) return false;
    return true;
}

bool TextStyleController::paragraphPatchIsNoOp(
    TextParagraphPatch const &patch, TextParagraphSnapshot const &snapshot) const
{
    if (!snapshot.has_text_target) return true;
    auto same = [](auto const &field, auto const &value) {
        return field.valid && !field.mixed && field.value == value;
    };
    if (patch.alignment && !same(snapshot.alignment, *patch.alignment)) return false;
    if (patch.line_height) {
        if (!(snapshot.line_height.valid && !snapshot.line_height.mixed &&
              equivalent_line_height(snapshot.line_height.value, *patch.line_height))) return false;
    }
    if (patch.first_line_indent_px && !(snapshot.first_line_indent_px.valid &&
        !snapshot.first_line_indent_px.mixed &&
        close_enough(snapshot.first_line_indent_px.value, *patch.first_line_indent_px))) return false;
    if (patch.spacing_before_px && !(snapshot.spacing_before_px.valid &&
        !snapshot.spacing_before_px.mixed &&
        close_enough(snapshot.spacing_before_px.value, *patch.spacing_before_px))) return false;
    if (patch.spacing_after_px && !(snapshot.spacing_after_px.valid &&
        !snapshot.spacing_after_px.mixed &&
        close_enough(snapshot.spacing_after_px.value, *patch.spacing_after_px))) return false;
    if (patch.list_mode && !same(snapshot.list_mode, *patch.list_mode)) return false;
    if (patch.list_start && !same(snapshot.list_start, *patch.list_start)) return false;
    if (patch.hyphenation && !same(snapshot.hyphenation, *patch.hyphenation)) return false;
    if (patch.drop_cap_lines && !same(snapshot.drop_cap_lines, *patch.drop_cap_lines)) return false;
    if (patch.direction && !same(snapshot.direction, *patch.direction)) return false;
    if (patch.writing_mode && !same(snapshot.writing_mode, *patch.writing_mode)) return false;
    if (patch.orientation && !same(snapshot.orientation, *patch.orientation)) return false;
    return true;
}

bool TextStyleController::apply(TextStylePatch const &patch, char const *undo_key,
                                Util::Internal::ContextString undo_label, bool continuous)
{
    if (!_desktop || !_desktop->getDocument() || patch.empty()) return false;
    auto snapshot = query();
    if (patchIsNoOp(patch, snapshot)) {
        cancelPreview();
        return false;
    }
    if (_targets.empty() && !captureTargets()) return false;
    if (!targetsValid()) {
        cancelPreview();
        return false;
    }

    enum class PreparedStyleMode { LogicalRange, Recursive, Direct };
    struct PreparedStyleRange {
        PreparedStyleMode mode = PreparedStyleMode::LogicalRange;
        SPItem *item = nullptr;
        SPObject *object = nullptr;
        unsigned first = 0;
        unsigned last = 0;
        CssAttrPtr css;
        TextStyleLocalSpacing local_spacing;
    };
    std::vector<PreparedStyleRange> prepared;
    auto const per_run = needs_run_specific_css(patch);

    for (auto const &target : _targets) {
        auto item = target.item.get();
        if (!item || !item->style) continue;

        if (target.whole_object && !per_run) {
            // Retain recursive text styling and its document-to-local size
            // conversion, but never style flow regions or referenced artwork.
            // A family-only patch contains no dimensional properties to scale.
            std::vector<SPObject *> pending{item};
            while (!pending.empty()) {
                auto object = pending.back();
                pending.pop_back();
                if (!object->style || object->cloned ||
                    !(is<SPText>(object) || is<SPFlowtext>(object) || is<SPTSpan>(object) ||
                      is<SPTextPath>(object) || is<SPFlowdiv>(object) || is<SPFlowpara>(object) ||
                      is<SPFlowtspan>(object) || is<SPTRef>(object))) continue;
                auto span = cast<SPTSpan>(object);
                auto const inherit_only = !object->getAttribute("style") &&
                    ((span && span->role != SP_TSPAN_ROLE_UNSPECIFIED) ||
                     is<SPFlowdiv>(object) || is<SPFlowpara>(object) || is<SPTextPath>(object));
                if (!inherit_only) {
                    CssAttrPtr css{cssForPatch(patch, *object->style)};
                    auto const scale = cast<SPItem>(object)->i2doc_affine().descrim();
                    if (!std::isfinite(scale) || scale <= 0) {
                        cancelPreview();
                        return false;
                    }
                    if (scale != 1) sp_css_attr_scale(css.get(), 1 / scale);
                    prepared.push_back({PreparedStyleMode::Direct, item, object, 0, 0, std::move(css)});
                }
                if (!is<SPTRef>(object)) {
                    for (auto &child : object->children) pending.push_back(&child);
                }
            }
            continue;
        }

        if (!per_run) {
            prepared.push_back({target.whole_object ? PreparedStyleMode::Recursive
                                                    : PreparedStyleMode::LogicalRange,
                                item, item, target.first_char, target.last_char,
                                CssAttrPtr{cssForPatch(patch, *item->style)}});
            continue;
        }

        struct StyleRun {
            unsigned first = 0;
            unsigned last = 0;
            SPObject *source = nullptr;
            SPStyle const *style = nullptr;
        };
        std::vector<StyleRun> runs;
        auto layout = canonical_layout(item);
        if (layout) {
            auto iter = layout->begin();
            for (unsigned i = 0; i < target.first_char && iter != layout->end(); ++i) {
                iter.nextCharacter();
            }
            for (unsigned index = target.first_char;
                 index < target.last_char && iter != layout->end();
                 ++index, iter.nextCharacter()) {
                SPObject *source = nullptr;
                layout->getSourceOfCharacter(iter, &source);
                source = nearest_style_source(source, item);
                auto const *style = source ? source->style : item->style;
                if (runs.empty() || runs.back().source != source || runs.back().style != style) {
                    runs.push_back({index, index + 1, source, style});
                } else {
                    runs.back().last = index + 1;
                }
            }
        }

        if (runs.empty()) {
            // No layout runs: a partial LogicalRange still reaches
            // sp_te_apply_style(), which inverse-scales document-space lengths.
            // Mark the percent properties as local for that partial range only;
            // the whole-object Recursive path keeps its existing conversion.
            TextStyleLocalSpacing local_spacing;
            if (!target.whole_object) {
                local_spacing.letter_spacing = patch.character_spacing_percent.has_value();
                local_spacing.word_spacing = patch.word_spacing_percent.has_value();
            }
            prepared.push_back({target.whole_object ? PreparedStyleMode::Recursive
                                                    : PreparedStyleMode::LogicalRange,
                                item, item, target.first_char, target.last_char,
                                CssAttrPtr{cssForPatch(patch, *item->style)}, local_spacing});
            continue;
        }

        if (target.whole_object) {
            std::unordered_set<SPObject *> seen;
            for (auto const &run : runs) {
                if (!run.source || !run.style || !seen.insert(run.source).second) continue;
                auto local_patch = patch;
                if (local_patch.font_size_px) {
                    auto const scale = cast<SPItem>(run.source)->i2doc_affine().descrim();
                    if (!std::isfinite(scale) || scale <= 0) {
                        cancelPreview();
                        return false;
                    }
                    local_patch.font_size_px = *local_patch.font_size_px / scale;
                }
                prepared.push_back({PreparedStyleMode::Direct, item, run.source,
                                    run.first, run.last,
                                    CssAttrPtr{cssForPatch(local_patch, *run.style)}});
            }
            continue;
        }

        // sp_te_apply_style() normalizes the XML tree and synchronously rebuilds the
        // authoritative layout. Applying runs from the end keeps logical ranges stable.
        // It also inverse-scales document-space lengths by the run's common ancestor
        // descrim. Percent spacing was computed in local run units, so mark those
        // properties local and let the engine restore them after that one scale.
        // *_px patches stay verbatim because they are already expressed in document
        // units and keep no local flag.
        for (auto const &run : runs | std::views::reverse) {
            TextStyleLocalSpacing local_spacing;
            local_spacing.letter_spacing = patch.character_spacing_percent.has_value();
            local_spacing.word_spacing = patch.word_spacing_percent.has_value();
            prepared.push_back({PreparedStyleMode::LogicalRange, item, run.source,
                                run.first, run.last,
                                CssAttrPtr{cssForPatch(patch, *run.style)}, local_spacing});
        }
    }

    if (prepared.empty()) {
        cancelPreview();
        return false;
    }

    _committing = true;
    for (auto const &operation : prepared) {
        if (operation.mode == PreparedStyleMode::Recursive) {
            sp_desktop_apply_css_recursive(operation.item, operation.css.get(), true);
        } else if (operation.mode == PreparedStyleMode::Direct) {
            if (operation.object) {
                operation.object->changeCSS(operation.css.get(), "style");
            }
        } else {
            auto layout = canonical_layout(operation.item);
            if (!layout) continue;
            auto first = layout->begin();
            auto last = layout->begin();
            for (unsigned i = 0; i < operation.first && first != layout->end(); ++i) {
                first.nextCharacter();
            }
            for (unsigned i = 0; i < operation.last && last != layout->end(); ++i) {
                last.nextCharacter();
            }
            sp_te_apply_style(operation.item, first, last, operation.css.get(),
                              operation.local_spacing);
        }
    }

    auto document = _desktop->getDocument();
    document->ensureUpToDate();
    for (auto const &target : _targets) {
        if (auto item = target.item.get()) item->updateRepr();
    }

    // Keep the transient drawing until canonical relayout above is complete.
    cancelPreview();
    if (continuous && undo_key && *undo_key) {
        DocumentUndo::maybeDone(document, undo_key, undo_label, INKSCAPE_ICON("draw-text"));
    } else {
        DocumentUndo::done(document, undo_label, INKSCAPE_ICON("draw-text"));
    }
    _committing = false;

    if (patch.fontspec) FontLister::get_instance()->set_fontspec(*patch.fontspec, false);
    if (patch.family) RecentlyUsedFonts::get()->prepend_to_list(*patch.family);
    FontLister::get_instance()->update_font_list(document);
    return true;
}

bool TextStyleController::commit(TextStylePatch const &patch, char const *undo_key,
                                 Util::Internal::ContextString undo_label)
{
    return apply(patch, undo_key, undo_label, false);
}

bool TextStyleController::commitContinuous(TextStylePatch const &patch, char const *undo_key,
                                           Util::Internal::ContextString undo_label)
{
    return apply(patch, undo_key, undo_label, true);
}

bool TextStyleController::applyParagraph(TextParagraphPatch const &patch, char const *undo_key,
                                         Util::Internal::ContextString undo_label, bool continuous)
{
    if (!_desktop || !_desktop->getDocument() || patch.empty()) return false;
    auto const snapshot = queryParagraph();
    if (paragraphPatchIsNoOp(patch, snapshot)) return false;

    auto targets = currentParagraphTargets();
    if (targets.empty() || std::ranges::any_of(targets, [](auto const &target) {
            return !target.item;
        })) {
        return false;
    }

    auto const paragraph_objects = paragraphObjects(targets);

    // Alignment is split out so ordinary SVG <text> owners are aligned
    // object-scoped in one helper call per owner regardless of caret position.
    // Wrapped text, flowtext and authored paragraphs keep the paragraph-local
    // path; every other patch property keeps its existing target and scope.
    TextParagraphPatch style_patch = patch;
    style_patch.alignment.reset();
    // Ordinary whole-object line height also takes the native object scope. It
    // is removed from the paragraph style patch for those owners only; other
    // line-height targets keep the existing paragraph path.
    auto const line_height_owners = patch.line_height
        ? whole_object_line_height_owners(_desktop, selectedTextItems())
        : std::vector<SPText *>{};
    std::unordered_set<SPText *> const line_height_owner_set(
        line_height_owners.begin(), line_height_owners.end());
    // Ordinary explicit partial ranges take the native inner operation. Their
    // line-expanded logical indices are captured here, before the root style is
    // mutated, and the patch is removed from the paragraph path for them so the
    // selection is not also written paragraph-wide.
    auto const inner_line_height_owners = patch.line_height
        ? inner_line_height_targets(_desktop)
        : std::vector<InnerLineHeightTarget>{};
    std::unordered_set<SPText *> inner_owner_set;
    for (auto const &target : inner_line_height_owners) {
        if (target.text) inner_owner_set.insert(target.text);
    }
    TextParagraphPatch whole_style_patch = style_patch;
    if (!line_height_owner_set.empty() || !inner_owner_set.empty()) {
        whole_style_patch.line_height.reset();
    }
    auto const has_style_patch = style_patch.line_height ||
        style_patch.first_line_indent_px || style_patch.spacing_before_px ||
        style_patch.spacing_after_px || style_patch.direction ||
        style_patch.writing_mode || style_patch.orientation;

    // Applying a style synchronously rebuilds Layout. Work from the end so the
    // canonical logical indices of every earlier paragraph stay valid.
    _committing = true;
    bool changed = false;
    std::unordered_set<SPText *> updated_line_height_owners;
    if (has_style_patch) {
        std::unordered_set<SPObject *> styled_paragraphs;
        for (auto const &target : targets | std::views::reverse) {
            auto item = target.item.get();
            if (!item || !item->style) continue;

            auto paragraph = paragraphObject(target);
            auto const *base = paragraph && paragraph->style ? paragraph->style : item->style;
            bool const object_line_height =
                line_height_owner_set.contains(cast<SPText>(item)) ||
                inner_owner_set.contains(cast<SPText>(item));
            TextParagraphPatch effective_patch =
                object_line_height ? whole_style_patch : style_patch;
            if (effective_patch.empty()) continue;
            if (paragraph && paragraph != item) {
                // Direct paragraph CSS writes store absolute line-height in the
                // paragraph's local context, so convert the document-unit request
                // here. The whole-object request is scaled by
                // applyDocumentTextStyle. Logical-range writes go through
                // sp_te_apply_style, which inverse-scales document units itself,
                // so they must NOT be divided first.
                if (!object_line_height && effective_patch.line_height) {
                    effective_patch.line_height = from_document_line_height(
                        *effective_patch.line_height, item->i2doc_affine().descrim());
                }
                CssAttrPtr css{cssForParagraphPatch(effective_patch, *base)};
                if (styled_paragraphs.insert(paragraph).second) {
                    paragraph->changeCSS(css.get(), "style");
                    changed = true;
                }
            } else {
                CssAttrPtr css{cssForParagraphPatch(effective_patch, *base)};
                changed |= applyStyleToLogicalRanges(
                    item, {{target.first_char, target.last_char}}, css.get());
            }
        }
    }
    if (patch.alignment) {
        std::unordered_set<SPText *> aligned_owners;
        std::unordered_set<SPObject *> styled_paragraphs;
        for (auto const &target : targets | std::views::reverse) {
            auto item = target.item.get();
            if (!item || !item->style) continue;

            if (auto text = cast<SPText>(item);
                text && is_ordinary_object_scoped_text(item)) {
                if (!aligned_owners.insert(text).second) continue;
                // Start/End are direction-relative; Left/Right are physical.
                auto const direction = text->style->direction.value;
                int mode = 0;
                switch (*patch.alignment) {
                    case TextParagraphAlignment::Start:
                        mode = direction == SP_CSS_DIRECTION_RTL ? 2 : 0;
                        break;
                    case TextParagraphAlignment::End:
                        mode = direction == SP_CSS_DIRECTION_RTL ? 0 : 2;
                        break;
                    case TextParagraphAlignment::Left: mode = 0; break;
                    case TextParagraphAlignment::Right: mode = 2; break;
                    case TextParagraphAlignment::Center: mode = 1; break;
                    case TextParagraphAlignment::Justify: mode = 3; break;
                }
                changed |= applyNativeTextAlignment(*text, mode);
                continue;
            }

            auto paragraph = paragraphObject(target);
            auto const *base = paragraph && paragraph->style ? paragraph->style : item->style;
            TextParagraphPatch alignment_patch;
            alignment_patch.alignment = patch.alignment;
            CssAttrPtr css{cssForParagraphPatch(alignment_patch, *base)};
            if (paragraph && paragraph != item) {
                if (styled_paragraphs.insert(paragraph).second) {
                    paragraph->changeCSS(css.get(), "style");
                    changed = true;
                }
            } else {
                changed |= applyStyleToLogicalRanges(
                    item, {{target.first_char, target.last_char}}, css.get());
            }
        }
    }
    if (patch.list_mode) {
        changed |= setParagraphListMode(*_desktop->getDocument(), paragraph_objects,
                                        *patch.list_mode, patch.list_start.value_or(1));
    }
    if (patch.hyphenation) {
        changed |= setParagraphHyphenation(*_desktop->getDocument(), paragraph_objects,
                                           *patch.hyphenation);
    }
    if (patch.drop_cap_lines) {
        changed |= setParagraphDropCapLines(*_desktop->getDocument(), paragraph_objects,
                                            *patch.drop_cap_lines);
    }

    // Native whole-object line height: the root carries the value and descendant
    // overrides are unset so a previous override cannot survive. Once per
    // compatible owner; an already-equivalent uniform owner is left untouched.
    for (auto text : line_height_owners) {
        TextLineHeightValue current;
        bool mixed = false;
        if (object_line_height_style(*text, current, mixed) && !mixed &&
            equivalent_line_height(current, *patch.line_height)) {
            continue;
        }
        auto css = sp_repr_css_attr_new();
        sp_repr_css_set_property(css, "line-height",
                                 serializeLineHeight(*patch.line_height).c_str());
        applyDocumentTextStyle(*text, css);
        sp_repr_css_attr_unref(css);
        updated_line_height_owners.insert(text);
        changed = true;
    }

    // Native inner line height for ordinary explicit partial ranges: push the
    // root style onto its children, then apply the document-unit value to the
    // line-expanded logical range through the shared sp_te_apply_style engine.
    // No toolbar method and no active-selection swap; the selection is
    // reacquired from logical indices after the synchronous rebuilds below.
    for (auto const &target : inner_line_height_owners) {
        auto text = target.text;
        if (!text || !text->style) continue;
        prepareInnerRootLineHeight(*text);
        TextParagraphPatch line_patch;
        line_patch.line_height = *patch.line_height;
        CssAttrPtr css{cssForParagraphPatch(line_patch, *text->style)};
        applyStyleToLogicalRanges(text, {{target.line_first, target.line_last}}, css.get());
        // prepareInnerRootLineHeight already normalized the owner's root style;
        // the operation is committed even if the range write reported nothing.
        changed = true;
        updated_line_height_owners.insert(text);
    }

    if (!changed) {
        _committing = false;
        return false;
    }

    auto document = _desktop->getDocument();
    document->ensureUpToDate();
    // Whole-object line height follows the native outer path and must persist the
    // recomputed sodipodi:role="line" x/y. Only ordinary owners this operation
    // actually touched are updated; wrapped/authored paragraphs stay on the
    // updateRepr-free paragraph path above.
    for (auto text : updated_line_height_owners) {
        text->updateRepr();
    }
    // Native inner sequence: once the layout is canonical, normalize the
    // wrappers the partial line-height change created. Iterators are rebuilt
    // from the captured line-expanded logical indices because prepare/updateRepr
    // may have rebuilt the layout.
    for (auto const &target : inner_line_height_owners) {
        auto text = target.text;
        if (!text) continue;
        auto layout = canonical_layout(text);
        if (!layout) continue;
        auto start = layout->charIndexToIterator(static_cast<int>(target.line_first));
        auto end = layout->charIndexToIterator(static_cast<int>(target.line_last));
        prepareInnerText(*text, start, end);
    }
    // The active raw selection may point into a layout prepareInnerText rebuilt;
    // re-resolve it from the direction-preserving logical indices so direction
    // and the next panel query survive, then refresh the tool's visual
    // cursor/selection. refreshDisplayGeometry() performs no document or default
    // writes and suppresses our own cursor-moved callback.
    if (!inner_line_height_owners.empty()) {
        if (auto tool = dynamic_cast<Tools::TextTool *>(_desktop->getTool())) {
            if (auto layout = canonical_layout(tool->textItem())) {
                for (auto const &target : inner_line_height_owners) {
                    if (tool->textItem() != target.text) continue;
                    tool->text_sel_start =
                        layout->charIndexToIterator(static_cast<int>(target.raw_start));
                    tool->text_sel_end =
                        layout->charIndexToIterator(static_cast<int>(target.raw_end));
                }
            }
        }
        refreshDisplayGeometry();
    }
    // Paragraph operations already update their XML representation directly.
    // Calling SPText::updateRepr() here serializes SVG2 wrapping into visual-line
    // tspans, which destroys authored paragraph roles and makes list/drop-cap
    // metadata target the generated lines on the next action.
    if (continuous && undo_key && *undo_key) {
        DocumentUndo::maybeDone(document, undo_key, undo_label, INKSCAPE_ICON("draw-text"));
    } else {
        DocumentUndo::done(document, undo_label, INKSCAPE_ICON("draw-text"));
    }
    _committing = false;
    return true;
}

bool TextStyleController::commitParagraph(TextParagraphPatch const &patch, char const *undo_key,
                                          Util::Internal::ContextString undo_label)
{
    return applyParagraph(patch, undo_key, undo_label, false);
}

bool TextStyleController::commitParagraphContinuous(
    TextParagraphPatch const &patch, char const *undo_key,
    Util::Internal::ContextString undo_label)
{
    return applyParagraph(patch, undo_key, undo_label, true);
}

bool TextStyleController::commitFrame(TextFramePatch const &patch, char const *undo_key,
                                      Util::Internal::ContextString undo_label, bool continuous)
{
    if (!_desktop || !_desktop->getDocument() || patch.empty()) return false;
    std::vector<SPText *> texts;
    for (auto item : selectedTextItems()) {
        if (auto text = cast<SPText>(item)) texts.push_back(text);
    }
    if (texts.empty()) return false;

    bool changed = false;
    _committing = true;
    for (auto text : texts) {
        auto settings = textFrameSettings(*text);
        if (settings.width <= 0.0 || settings.height <= 0.0) {
            if (auto bounds = text->geometricBounds()) {
                if (settings.width <= 0.0) settings.width = std::max(1.0, bounds->width());
                if (settings.height <= 0.0) settings.height = std::max(1.0, bounds->height());
            }
        }
        if (patch.width_px) settings.width = *patch.width_px;
        if (patch.height_px) settings.height = *patch.height_px;
        if (patch.columns) settings.columns = *patch.columns;
        if (patch.gap_px) settings.gap = *patch.gap_px;
        if (patch.vertical_alignment) settings.vertical_alignment = *patch.vertical_alignment;
        changed |= setTextFrameSettings(*_desktop->getDocument(), {text}, settings);
    }
    if (!changed) {
        _committing = false;
        return false;
    }
    auto document = _desktop->getDocument();
    document->ensureUpToDate();
    if (continuous && undo_key && *undo_key) {
        DocumentUndo::maybeDone(document, undo_key, undo_label, INKSCAPE_ICON("draw-text"));
    } else {
        DocumentUndo::done(document, undo_label, INKSCAPE_ICON("draw-text"));
    }
    _committing = false;
    return true;
}

void TextStyleController::setPanelFontOnly(bool enabled)
{
    if (_panel_font_only == enabled) return;
    invalidateFontChoices();
    _panel_font_only = enabled;
}

void TextStyleController::invalidateFontChoices() noexcept
{
    ++_font_policy_generation;
    cancelPreview();
}

bool TextStyleController::requestFontChoice(FontChoice const &choice, FontChoicePolicy policy,
                                          std::optional<uint64_t> policy_generation)
{
    if (policy_generation && *policy_generation != _font_policy_generation) return false;
    if (choice.phase == FontChoicePhase::Cancel) {
        if (choice.interaction_id && _font_interaction &&
            choice.interaction_id != _font_interaction) {
            return false;
        }
        cancelPreview();
        return false;
    }
    if (!choice.available || choice.family.empty()) return false;
    if (choice.interaction_id && _font_interaction &&
        choice.interaction_id != _font_interaction) {
        cancelPreview();
    }
    if (choice.interaction_id && choice.interaction_id == _font_interaction &&
        choice.generation && choice.generation <= _font_generation) {
        return false;
    }
    _font_interaction = choice.interaction_id;
    _font_generation = choice.generation;
    TextStylePatch patch;
    patch.family = choice.family;
    if (policy == FontChoicePolicy::NormalizeFace) {
        patch.face = choice.face;
        patch.fontspec = choice.fontspec;
    }
    if (policy == FontChoicePolicy::NormalizeFace && !patch.face) {
        // Family rows are an explicit normalization request. Resolve one
        // deterministic default face instead of preserving every target's
        // old weight/slant/stretch.
        auto lister = FontLister::get_instance();
        auto const parsed = lister->ui_from_fontspec(choice.fontspec);
        if (!parsed.second.empty()) patch.face = parsed.second;
        if (!patch.face) {
            auto const styles = lister->get_font_styles(choice.family);
            auto preferred = std::ranges::find_if(*styles, [](auto const &style) {
                auto const name = (style.display_name.empty() ? style.css_name : style.display_name).lowercase();
                return name.find("regular") != Glib::ustring::npos ||
                       name.find("normal") != Glib::ustring::npos;
            });
            if (preferred == styles->end() && !styles->empty()) preferred = styles->begin();
            if (preferred != styles->end()) patch.face = preferred->css_name;
        }
        if (patch.face && patch.fontspec->empty()) {
            patch.fontspec = Inkscape::get_fontspec(choice.family, *patch.face);
        }
    }
    if (choice.phase == FontChoicePhase::Commit) {
        auto const snapshot = query();
        auto const confirmed_no_op = snapshot.has_text_target && patchIsNoOp(patch, snapshot);
        auto const changed = commit(patch, "text-panel:font", RC_("Undo", "Set text font"));
        // A confirmed no-op choice is still a real use of the family and should
        // move it to the front without manufacturing an undo item. Changed
        // commits are already recorded by apply().
        if (!changed && confirmed_no_op) {
            RecentlyUsedFonts::get()->prepend_to_list(choice.family);
        }
        return changed;
    } else {
        preview(patch, choice.origin == FontChoiceOrigin::Keyboard
                           ? TextStyleOrigin::Keyboard : TextStyleOrigin::Pointer);
    }
    return false;
}

void TextStyleController::refreshDisplayGeometry()
{
    if (!_desktop) return;
    if (auto tool = dynamic_cast<Tools::TextTool *>(_desktop->getTool())) {
        // TextTool emits cursor-moved while repainting the same logical range.
        // Only suppress our own synchronous callback: actual cursor movement
        // must still invalidate the frozen targets and the panel's policy token.
        SignalBlocker blocker(_cursor_moved);
        tool->refreshDisplayGeometry();
    }
}

void TextStyleController::cancelPreview() noexcept
{
    // Refreshing TextTool geometry emits text_cursor_moved, whose handler calls
    // cancelPreview() again. Only a published preview changes display geometry;
    // remembering that state before clearing it makes cancellation idempotent
    // and prevents an event-only cancellation from recursing indefinitely.
    auto const refresh_display_geometry = _visible.has_value();
    _scheduled.disconnect();
    ++_generation;
    if (_desktop) {
        for (auto const &target : _targets) {
            if (auto item = target.item.get()) clear_item_preview(item, _desktop->dkey);
        }
    }
    _targets.clear();
    _pending.reset();
    _visible.reset();
    _font_interaction = 0;
    _font_generation = 0;
    _slow_candidate = false;
    if (_desktop && refresh_display_geometry) {
        refreshDisplayGeometry();
    }
}

} // namespace Inkscape::UI
