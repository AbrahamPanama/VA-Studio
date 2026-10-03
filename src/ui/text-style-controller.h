// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_UI_TEXT_STYLE_CONTROLLER_H
#define INKSCAPE_UI_TEXT_STYLE_CONTROLLER_H

#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include <glibmm/ustring.h>
#include <sigc++/scoped_connection.h>

#include "object/weakptr.h"
#include "ui/text-paragraph-tools.h"
#include "ui/text-frame-tools.h"
#include "ui/text-style-units.h"
#include "ui/widget/font-selector-interface.h"
#include "util-string/context-string.h"

class SPCSSAttr;
class SPDesktop;
class SPItem;
class SPObject;
class SPStyle;

namespace Inkscape::UI {

enum class TextStylePhase { Preview, Commit, Cancel };
enum class TextStyleOrigin { Pointer, Keyboard, Direct };
enum class FontChoicePolicy { NormalizeFace, FamilyOnly };

enum class CapitalizationMode {
    None,
    AllCaps,
    TitlingCaps,
    SmallCapsAuto,
    AllSmallCaps,
    SmallCapsFromCaps,
    SmallCapsSynthesized
};

enum class TextScriptPosition { Normal, Superscript, Subscript };

enum class TextParagraphAlignment { Start, Center, End, Justify, Left, Right };
enum class TextParagraphDirection { LeftToRight, RightToLeft };
enum class TextParagraphWritingMode { Horizontal, VerticalRightToLeft, VerticalLeftToRight };
enum class TextParagraphOrientation { Mixed, Upright, Sideways };

template <typename T>
struct TextStyleValue {
    T value{};
    bool valid = false;
    bool mixed = false;
};

struct TextStyleTarget {
    SPWeakPtr<SPItem> item;
    unsigned first_char = 0;
    unsigned last_char = 0;
    bool whole_object = false;
};

struct TextStylePatch {
    std::optional<Glib::ustring> family;
    std::optional<Glib::ustring> face;
    std::optional<Glib::ustring> fontspec;
    std::optional<double> font_size_px;
    std::optional<bool> bold;
    std::optional<bool> italic;
    std::optional<bool> underline;
    std::optional<TextScriptPosition> script_position;
    std::optional<CapitalizationMode> capitalization;
    std::optional<bool> standard_ligatures;
    std::optional<double> character_spacing_percent;
    std::optional<double> word_spacing_percent;
    std::optional<double> character_spacing_px;
    std::optional<double> word_spacing_px;
    std::optional<double> language_spacing_percent;
    std::optional<Glib::ustring> fill;
    std::optional<Glib::ustring> stroke;

    bool empty() const;
    bool operator==(TextStylePatch const &) const = default;
};

struct TextStyleSnapshot {
    bool has_text_target = false;
    TextStyleValue<Glib::ustring> family;
    TextStyleValue<Glib::ustring> face;
    TextStyleValue<Glib::ustring> fontspec;
    TextStyleValue<double> font_size_px;
    TextStyleValue<bool> bold;
    TextStyleValue<bool> italic;
    TextStyleValue<bool> underline;
    TextStyleValue<TextScriptPosition> script_position;
    TextStyleValue<CapitalizationMode> capitalization;
    bool capitalization_custom = false;
    TextStyleValue<bool> standard_ligatures;
    TextStyleValue<double> character_spacing_percent;
    TextStyleValue<double> word_spacing_percent;
    TextStyleValue<double> language_spacing_percent;
    TextStyleValue<Glib::ustring> fill;
    TextStyleValue<Glib::ustring> stroke;
};

struct TextParagraphPatch {
    std::optional<TextParagraphAlignment> alignment;
    std::optional<TextLineHeightValue> line_height;
    std::optional<double> first_line_indent_px;
    std::optional<double> spacing_before_px;
    std::optional<double> spacing_after_px;
    std::optional<TextListMode> list_mode;
    std::optional<unsigned> list_start;
    std::optional<bool> hyphenation;
    std::optional<unsigned> drop_cap_lines;
    std::optional<TextParagraphDirection> direction;
    std::optional<TextParagraphWritingMode> writing_mode;
    std::optional<TextParagraphOrientation> orientation;

    bool empty() const;
    bool operator==(TextParagraphPatch const &) const = default;
};

struct TextParagraphSnapshot {
    bool has_text_target = false;
    TextStyleValue<TextParagraphAlignment> alignment;
    TextStyleValue<TextLineHeightValue> line_height;
    TextStyleValue<double> first_line_indent_px;
    TextStyleValue<double> spacing_before_px;
    TextStyleValue<double> spacing_after_px;
    TextStyleValue<TextListMode> list_mode;
    TextStyleValue<unsigned> list_start;
    TextStyleValue<bool> hyphenation;
    bool hyphenation_available = false;
    TextStyleValue<unsigned> drop_cap_lines;
    TextStyleValue<TextParagraphDirection> direction;
    TextStyleValue<TextParagraphWritingMode> writing_mode;
    TextStyleValue<TextParagraphOrientation> orientation;
};

struct TextFramePatch {
    std::optional<double> width_px;
    std::optional<double> height_px;
    std::optional<unsigned> columns;
    std::optional<double> gap_px;
    std::optional<TextFrameVerticalAlignment> vertical_alignment;
    bool empty() const;
};

struct TextFrameSnapshot {
    bool has_text_target = false;
    TextStyleValue<double> width_px;
    TextStyleValue<double> height_px;
    TextStyleValue<unsigned> columns;
    TextStyleValue<double> gap_px;
    TextStyleValue<TextFrameVerticalAlignment> vertical_alignment;
};

/** Shared text styling and transient-preview authority for one desktop. */
class TextStyleController final {
public:
    explicit TextStyleController(SPDesktop *desktop);
    ~TextStyleController();

    TextStyleSnapshot query() const;
    TextParagraphSnapshot queryParagraph() const;
    TextFrameSnapshot queryFrame() const;
    void begin(std::vector<TextStyleTarget> targets);
    void preview(TextStylePatch const &patch, TextStyleOrigin origin);
    bool commit(TextStylePatch const &patch, char const *undo_key,
                Util::Internal::ContextString undo_label);
    bool commitContinuous(TextStylePatch const &patch, char const *undo_key,
                          Util::Internal::ContextString undo_label);
    bool commitParagraph(TextParagraphPatch const &patch, char const *undo_key,
                         Util::Internal::ContextString undo_label);
    bool commitParagraphContinuous(TextParagraphPatch const &patch, char const *undo_key,
                                   Util::Internal::ContextString undo_label);
    bool commitFrame(TextFramePatch const &patch, char const *undo_key,
                     Util::Internal::ContextString undo_label, bool continuous = false);
    void cancelPreview() noexcept;
    void invalidateFontChoices() noexcept;

    bool requestFontChoice(FontChoice const &choice,
                           FontChoicePolicy policy = FontChoicePolicy::NormalizeFace,
                           std::optional<uint64_t> policy_generation = std::nullopt);
    // Panel UI state belongs to the desktop, not to a panel instance or SVG.
    bool panelFontOnly() const { return _panel_font_only; }
    void setPanelFontOnly(bool enabled);
    uint64_t fontPolicyGeneration() const { return _font_policy_generation; }
    /** True while a transient preview is published under this desktop's key. */
    bool previewActive() const { return _visible.has_value(); }
    bool hasTargets() const;
    SPItem *representativeTextItem() const;
    SPStyle const *representativeRunStyle() const;
    bool supportsOpenTypeFeature(std::string_view tag) const;

private:
    bool captureTargets();
    bool targetsValid() const;
    bool publish(TextStylePatch const &patch, uint64_t generation);
    bool apply(TextStylePatch const &patch, char const *undo_key,
               Util::Internal::ContextString undo_label, bool continuous);
    bool patchIsNoOp(TextStylePatch const &patch, TextStyleSnapshot const &snapshot) const;
    bool containsUnsupportedTref(SPItem *item, unsigned first, unsigned last) const;
    SPCSSAttr *cssForPatch(TextStylePatch const &patch, SPStyle const &base) const;
    std::vector<SPItem *> selectedTextItems() const;
    std::vector<SPStyle const *> currentRunStyles() const;
    std::vector<TextStyleTarget> currentParagraphTargets() const;
    SPObject *paragraphObject(TextStyleTarget const &target) const;
    std::vector<SPStyle const *> paragraphStyles(std::vector<TextStyleTarget> const &targets) const;
    std::vector<SPObject *> paragraphObjects(std::vector<TextStyleTarget> const &targets) const;
    SPCSSAttr *cssForParagraphPatch(TextParagraphPatch const &patch, SPStyle const &base) const;
    bool paragraphPatchIsNoOp(TextParagraphPatch const &patch,
                              TextParagraphSnapshot const &snapshot) const;
    bool applyParagraph(TextParagraphPatch const &patch, char const *undo_key,
                        Util::Internal::ContextString undo_label, bool continuous);
    void reconnectSelection();
    void refreshDisplayGeometry();

    SPDesktop *_desktop = nullptr;
    std::vector<TextStyleTarget> _targets;
    std::optional<TextStylePatch> _pending;
    std::optional<TextStylePatch> _visible;
    uint64_t _generation = 0;
    uint64_t _font_interaction = 0;
    uint64_t _font_generation = 0;
    bool _slow_candidate = false;
    bool _committing = false;
    bool _panel_font_only = false;
    uint64_t _font_policy_generation = 0;

    sigc::scoped_connection _scheduled;
    sigc::scoped_connection _selection_changed;
    sigc::scoped_connection _selection_modified;
    sigc::scoped_connection _tool_changed;
    sigc::scoped_connection _document_replaced;
    sigc::scoped_connection _cursor_moved;
    sigc::scoped_connection _document_modified;
};

} // namespace Inkscape::UI

#endif // INKSCAPE_UI_TEXT_STYLE_CONTROLLER_H
