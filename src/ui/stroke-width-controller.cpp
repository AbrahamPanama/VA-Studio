// SPDX-License-Identifier: GPL-2.0-or-later

#include "stroke-width-controller.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <glib.h>
#include <glibmm/i18n.h>

#include <2geom/affine.h>

#include "colors/color.h"
#include "colors/spaces/base.h"
#include "document.h"
#include "document-undo.h"
#include "util-string/context-string.h"
#include "libnrtype/Layout-TNG.h"
#include "libnrtype/font-instance.h"
#include "object/sp-flowregion.h"
#include "object/sp-flowtext.h"
#include "object/sp-image.h"
#include "object/sp-item-group.h"
#include "object/sp-object.h"
#include "object/sp-shape.h"
#include "object/sp-string.h"
#include "object/sp-text.h"
#include "object/sp-textpath.h"
#include "object/sp-tref.h"
#include "object/sp-tspan.h"
#include "object/sp-root.h"
#include "object/sp-use.h"
#include "style.h"
#include "svg/svg.h"
#include "text-editing.h"
#include "util/cast.h"
#include "util/delete-with.h"
#include "util/units.h"
#include "xml/attribute-record.h"
#include "xml/node.h"
#include "xml/node-observer.h"
#include "xml/repr.h"
#include "xml/sp-css-attr.h"

namespace Inkscape::UI {

double stroke_width_step(Util::Unit const &unit)
{
    if (unit.type != Util::UNIT_TYPE_LINEAR) return 0.0;
    auto const &a = unit.abbr;
    if (a == "px" || a == "pt") return 0.1;
    if (a == "pc") return 0.01;
    if (a == "mm") return 0.05;
    if (a == "cm") return 0.005;
    if (a == "in") return 0.001;
    if (a == "m") return 0.00005;
    if (a == "ft") return 0.0001;
    return 0.0;
}

unsigned stroke_width_digits(Util::Unit const &unit)
{
    return unit.abbr == "m" ? 5 : unit.abbr == "ft" ? 6 : 3;
}

Util::Unit const *stroke_width_list_unit(Util::Unit const &unit)
{
    auto const &a = unit.abbr;
    return Util::UnitTable::get().getUnit(a == "mm" || a == "cm" || a == "m" ? "mm" : "pt");
}

std::vector<StrokeWidthPreset> stroke_width_presets(Util::Unit const &list_unit)
{
    bool const metric = list_unit.abbr == "mm";
    auto const *unit = Util::UnitTable::get().getUnit(metric ? "mm" : "pt");
    std::vector<StrokeWidthPreset> result{{_("Hairline"), unit, 0, StrokeWidthPresetKind::Hairline}};
    auto add = [&](char const *label, double value) {
        result.push_back({label, unit, value, StrokeWidthPresetKind::Absolute});
    };
    if (metric) {
        add("0.1", 0.1); add("0.2", 0.2); add("0.25", 0.25); add("0.35", 0.35);
        add("0.5", 0.5); add("0.75", 0.75); add("1", 1); add("1.5", 1.5);
        add("2", 2); add("2.5", 2.5); add("3", 3);
    } else {
        add("0.25", 0.25); add("0.5", 0.5); add("0.75", 0.75); add("1", 1);
        add("1.5", 1.5); add("2", 2); add("3", 3); add("4", 4);
        add("6", 6); add("8", 8); add("10", 10); add("12", 12);
    }
    return result;
}

double stroke_width_preset_px(StrokeWidthPreset const &preset)
{
    return preset.kind == StrokeWidthPresetKind::Hairline ? 0.0
        : Util::Quantity::convert(preset.value, preset.unit, Util::UnitTable::get().getUnit("px"));
}

bool stroke_width_can_decrease(double w_px, double step_px)
{
    return std::isfinite(w_px) && w_px >= 0.0 && std::isfinite(step_px) && step_px > 0.0 &&
           w_px - step_px >= step_px - 1e-9 * std::max(1.0, w_px);
}

namespace {

constexpr double query_tolerance = 1e-9;

bool nearly_equal(double a, double b)
{
    double const diff = std::fabs(a - b);
    double const scale = std::max({1.0, std::fabs(a), std::fabs(b)});
    return diff <= query_tolerance * scale;
}

/// Same displayed width: the selection's widths differ only by how they are
/// stored (text styles keep 6 decimals, paths the full value), at most 0.001
/// CSS px, far below the 0.001 mm the width field shows.
bool same_displayed_width(double a, double b)
{
    return std::fabs(a - b) <= 1e-3 || nearly_equal(a, b);
}

bool affine_is_finite(Geom::Affine const &affine)
{
    for (unsigned i = 0; i < 6; ++i) {
        if (!std::isfinite(affine[i])) return false;
    }
    return true;
}

enum class TransformCheck { Usable, NonFinite, Singular };

/**
 * Classify a transform for stroke-width use. `scale` receives descrim() so the
 * caller can report the observed magnitude. Usable requires all six entries
 * finite, a finite descrim (an overflowing determinant is rejected) and
 * descrim > 0.
 */
TransformCheck classify_transform(Geom::Affine const &affine, double &scale)
{
    scale = affine.descrim();
    if (!affine_is_finite(affine) || !std::isfinite(scale)) return TransformCheck::NonFinite;
    if (!(scale > 0.0)) return TransformCheck::Singular;
    return TransformCheck::Usable;
}

bool repr_name_is(SPObject const *object, char const *name)
{
    auto repr = object ? object->getRepr() : nullptr;
    return repr && std::strcmp(repr->name(), name) == 0;
}

/// Containers that must never be traversed or treated as editable artwork.
bool is_reference_container(SPObject const *object)
{
    static char const *const names[] = {
        "svg:defs", "svg:clipPath", "svg:mask", "svg:symbol", "svg:use",
        "svg:marker", "svg:pattern", "svg:linearGradient", "svg:radialGradient",
        "svg:filter",
    };
    for (auto const *name : names) {
        if (repr_name_is(object, name)) return true;
    }
    return false;
}

enum class ProtectionKind { None, Hidden, Locked, Reference, TextDescendant };

StrokeWidthExclusion exclusion_for(ProtectionKind kind)
{
    switch (kind) {
    case ProtectionKind::Hidden:
        return StrokeWidthExclusion::HiddenAncestor;
    case ProtectionKind::Locked:
        return StrokeWidthExclusion::LockedAncestor;
    case ProtectionKind::Reference:
        return StrokeWidthExclusion::DefinitionOrReference;
    case ProtectionKind::TextDescendant:
        return StrokeWidthExclusion::TextDescendant;
    case ProtectionKind::None:
        break;
    }
    return StrokeWidthExclusion::None;
}

/**
 * Walk a node and its ancestors. `allowed_text_owner` is the one text container
 * that may appear (the candidate itself for a text owner, or the owner when
 * walking a text run's style source). A textPath is an interior style scope only
 * while an explicit owner scope is supplied, so it is permitted when resolving a
 * legitimate owner run but never when directly selected; its referenced path
 * geometry is never followed from here. `stop` is an exclusive ancestor bound.
 */
ProtectionKind node_protection(SPObject *node, SPObject *allowed_text_owner, SPObject *stop)
{
    for (auto *o = node; o && o != stop; o = o->parent) {
        // A clone owner is a reference container for its descendants, but the
        // selected SPUse itself is dispatched as a clone instance.
        if (o != node && is_reference_container(o)) return ProtectionKind::Reference;

        bool const is_owner = o == allowed_text_owner;
        bool const allowed_textpath = is<SPTextPath>(o) && allowed_text_owner != nullptr;
        bool const is_text_container =
            is<SPText>(o) || is<SPFlowtext>(o) || is<SPTextPath>(o) || is<SPFlowregion>(o);
        if (is_text_container && !is_owner && !allowed_textpath) {
            return ProtectionKind::TextDescendant;
        }

        if (auto *item = cast<SPItem>(o)) {
            if (!item->isSensitive()) return ProtectionKind::Locked;
            if (item->isHidden()) return ProtectionKind::Hidden;
            if (item->style && item->style->visibility.computed != SP_CSS_VISIBILITY_VISIBLE) {
                return ProtectionKind::Hidden;
            }
        }
    }
    return ProtectionKind::None;
}

StrokeWidthTargetKind kind_for(SPItem const *item)
{
    if (is<SPText>(item) || is<SPFlowtext>(item)) return StrokeWidthTargetKind::TextOwner;
    if (is<SPUse>(item)) return StrokeWidthTargetKind::CloneInstance;
    if (is<SPShape>(item)) return StrokeWidthTargetKind::Shape;
    if (is<SPGroup>(item)) return StrokeWidthTargetKind::Container;
    return StrokeWidthTargetKind::Other;
}

StrokeWidthStyle snapshot_style(SPStyle const &style, double descrim)
{
    StrokeWidthStyle out;
    out.local_computed = style.stroke_width.computed;
    out.paint_none = style.stroke.isNone();
    out.paint_important = style.stroke.important;
    out.paint_style_src = static_cast<unsigned char>(style.stroke.style_src);
    out.hairline_important = style.stroke_extensions.important;
    out.hairline_style_src = static_cast<unsigned char>(style.stroke_extensions.style_src);
    out.dash_set = style.stroke_dasharray.set;
    out.dash_computed = style.stroke_dasharray.get_computed();
    out.dash_offset_computed = style.stroke_dashoffset.computed;
    out.width_set = style.stroke_width.set;
    out.width_inherit = style.stroke_width.inherit;
    out.width_important = style.stroke_width.important;
    out.width_style_src = static_cast<unsigned char>(style.stroke_width.style_src);
    out.dash_style_src = static_cast<unsigned char>(style.stroke_dasharray.style_src);
    out.dashoffset_style_src = static_cast<unsigned char>(style.stroke_dashoffset.style_src);
    out.dasharray_important = style.stroke_dasharray.important;
    out.dashoffset_important = style.stroke_dashoffset.important;
    out.non_scaling = style.vector_effect.stroke;
    // Exact native vector-effect payload and its independent source/priority.
    // `non_scaling` remains the settled convention input above; these fields are
    // read-only metadata so distinct logical runs never collapse on the bit.
    out.vector_effect_value = style.vector_effect.get_value().raw();
    out.vector_effect_set = style.vector_effect.set;
    out.vector_effect_inherit = style.vector_effect.inherit;
    out.vector_effect_important = style.vector_effect.important;
    out.vector_effect_style_src = static_cast<unsigned char>(style.vector_effect.style_src);

    // The hairline extension always wins over the vector-effect fallback: a
    // native hairline is a device-space stroke with no numeric px value.
    if (style.stroke_extensions.hairline) {
        out.convention = StrokeWidthConvention::Hairline;
    } else if (out.non_scaling) {
        out.convention = StrokeWidthConvention::NonScaling;
        // Measured native oracle: a non-scaling stroke keeps its local computed
        // value in document px, independent of item/root scale. The same renderer
        // convention is shared by shapes, clones and text.
        out.effective_px = out.local_computed;
    } else {
        out.convention = StrokeWidthConvention::Ordinary;
        out.effective_px = out.local_computed * descrim;
    }
    return out;
}

bool styles_match(StrokeWidthStyle const &a, StrokeWidthStyle const &b)
{
    if (a.convention != b.convention || a.paint_none != b.paint_none || a.non_scaling != b.non_scaling ||
        a.dash_set != b.dash_set || a.width_set != b.width_set || a.width_inherit != b.width_inherit ||
        a.width_important != b.width_important) {
        return false;
    }
    if (a.paint_important != b.paint_important || a.paint_style_src != b.paint_style_src ||
        a.hairline_important != b.hairline_important ||
        a.hairline_style_src != b.hairline_style_src) return false;
    // Full native vector-effect value and its independent source/priority are
    // categorical run identity, so two runs with the same numeric width but a
    // different value or `!important` never merge.
    if (a.vector_effect_value != b.vector_effect_value ||
        a.vector_effect_set != b.vector_effect_set ||
        a.vector_effect_inherit != b.vector_effect_inherit ||
        a.vector_effect_important != b.vector_effect_important ||
        a.vector_effect_style_src != b.vector_effect_style_src) {
        return false;
    }
    if (!nearly_equal(a.local_computed, b.local_computed) ||
        !nearly_equal(a.dash_offset_computed, b.dash_offset_computed)) {
        return false;
    }
    if (a.dash_computed.size() != b.dash_computed.size()) return false;
    for (std::size_t i = 0; i < a.dash_computed.size(); ++i) {
        if (!nearly_equal(a.dash_computed[i], b.dash_computed[i])) return false;
    }
    if (a.effective_px.has_value() != b.effective_px.has_value()) return false;
    if (a.effective_px && !nearly_equal(*a.effective_px, *b.effective_px)) return false;
    return true;
}

/// Every numeric width we snapshot must be finite and nonnegative. This also
/// catches an effective result that overflowed after multiplying by descrim.
bool stroke_style_is_valid(StrokeWidthStyle const &style)
{
    if (!std::isfinite(style.local_computed) || style.local_computed < 0.0) return false;
    if (style.effective_px && (!std::isfinite(*style.effective_px) || *style.effective_px < 0.0)) {
        return false;
    }
    return true;
}

/// Shared run/target eligibility decision for text runs and shape targets.
void classify_style(StrokeWidthStyle const &style, ProtectionKind protection,
                    StrokeWidthEligibility &eligibility, StrokeWidthExclusion &exclusion)
{
    eligibility = StrokeWidthEligibility::Eligible;
    exclusion = StrokeWidthExclusion::None;
    if (protection != ProtectionKind::None) {
        eligibility = StrokeWidthEligibility::Unavailable;
        exclusion = exclusion_for(protection);
    } else if (!stroke_style_is_valid(style)) {
        eligibility = StrokeWidthEligibility::Unavailable;
        exclusion = StrokeWidthExclusion::InvalidStyle;
    }
}

StrokeWidthRun make_run(SPStyle const &style, double descrim, SPObject *source, unsigned first,
                        unsigned last, ProtectionKind protection)
{
    StrokeWidthRun run;
    run.first_char = first;
    run.last_char = last;
    run.style_source = source;
    run.style = snapshot_style(style, descrim);
    classify_style(run.style, protection, run.eligibility, run.exclusion);
    return run;
}

SPObject *nearest_style_source(SPObject *source, SPItem *owner)
{
    while (source && source != owner && (!source->style || !is<SPItem>(source))) {
        source = source->parent;
    }
    if (source && source->style && is<SPItem>(source)) return source;
    return owner && owner->style ? static_cast<SPObject *>(owner) : nullptr;
}

/// Work counters of the apply in progress (see `StrokeWidthWorkCounters`). Reset by
/// every public apply entry point and copied into its result on every return.
thread_local StrokeWidthWorkCounters g_work;

// Public input provenance, captured once per layout scan. Empty TEXT_SOURCE
// items produce no logical characters (line wrappers can also own controls).
// Ambiguous nonempty text/control source identity is unavailable, never guessed.
struct LayoutSources {
    Text::Layout const &layout;
    std::unordered_map<SPObject *, unsigned> kinds;
    explicit LayoutSources(Text::Layout const &value) : layout(value)
    {
        for (auto const *input : layout.input_stream()) {
            if (input->Type() == Text::Layout::TEXT_SOURCE) {
                if (static_cast<Text::Layout::InputStreamTextSource const *>(input)->text_length == 0) continue;
                kinds[input->source] |= 1;
            } else if (input->Type() == Text::Layout::CONTROL_CODE) {
                kinds[input->source] |= 2;
            }
        }
    }
    mutable std::unordered_map<SPObject *, StrokeWidthStyle> styles;
    StrokeWidthStyle const &style(SPObject *source, double scale) const
    {
        auto [entry, inserted] = styles.try_emplace(source);
        if (inserted) entry->second = snapshot_style(*source->style, scale);
        return entry->second;
    }
    unsigned kind(unsigned index, SPObject **source) const
    {
        layout.getSourceOfCharacter(layout.charIndexToIterator(index), source);
        auto const found = kinds.find(*source);
        return found == kinds.end() ? 0 : found->second;
    }
};

bool authored_character(LayoutSources const &origins, unsigned index, SPObject **source)
{
    return origins.kind(index, source) == 1;
}

auto authored_tree(SPItem *owner, std::vector<StrokeWidthSourceBaseline> const &sources)
{
    std::unordered_set<XML::Node const *> styled;
    for (auto const &entry : sources) if (entry.source.get()) styled.insert(entry.source.get()->getRepr());
    std::vector<std::string> tree;
    std::function<void(XML::Node const *)> capture = [&](XML::Node const *node) {
        tree.emplace_back("(");
        tree.emplace_back(node->name() ? node->name() : "");
        tree.emplace_back(node->content() ? node->content() : "");
        std::map<std::string, std::string> attributes;
        for (auto const &attr : node->attributeList()) {
            auto const *name = g_quark_to_string(attr.key);
            if (std::strcmp(name, "style") != 0 || !styled.count(node)) attributes[name] = attr.value.pointer();
        }
        for (auto const &[name, value] : attributes) { tree.push_back(name); tree.push_back(value); }
        for (auto const *child = node->firstChild(); child; child = child->next()) capture(child);
        tree.emplace_back(")");
    };
    capture(owner->getRepr());
    return tree;
}

// Lossless, per-property signatures: flags and exact numbers are binary, never
// rounded or hashed. Only nonnumeric values use the native property serializer.
// Each source is visited once; unchanged property comparisons need no copies.
auto computed_declarations(SPStyle const &style, std::vector<bool> const *patched = nullptr)
{
    std::vector<std::string> values;
    values.reserve(style.properties().size() + style.extended_properties.size());
    for (auto const *property : style.properties()) {
        if (patched && (*patched)[values.size()]) { values.emplace_back(); continue; }
        std::string value;
        value.push_back(property->set);
        value.push_back(property->important);
        value.push_back(property->inherit);
        auto number = [&](double n) { value.append(reinterpret_cast<char const *>(&n), sizeof(n)); };
        auto color = [&](Colors::Color const &paint) {
            auto const &name = paint.getSpace()->getName();
            value.append(name); value.push_back('\0');
            for (double n : paint.getValues()) number(n);
        };
        // SPStyle's TypedSPI IDs fix these native classes. Dispatch once instead
        // of trying seven RTTI casts per property; the captured bytes are identical.
        switch (property->id()) {
            case SPAttr::FONT_SIZE: number(static_cast<SPIFontSize const *>(property)->computed); break;
            case SPAttr::BASELINE_SHIFT: number(static_cast<SPIBaselineShift const *>(property)->computed); break;
            case SPAttr::LINE_HEIGHT: case SPAttr::TEXT_INDENT: case SPAttr::LETTER_SPACING:
            case SPAttr::WORD_SPACING: case SPAttr::INKSCAPE_LANGUAGE_SPACING:
            case SPAttr::INKSCAPE_PARAGRAPH_SPACING_BEFORE: case SPAttr::INKSCAPE_PARAGRAPH_SPACING_AFTER:
            case SPAttr::SHAPE_PADDING: case SPAttr::SHAPE_MARGIN: case SPAttr::INLINE_SIZE:
            case SPAttr::STROKE_WIDTH: case SPAttr::STROKE_DASHOFFSET:
                number(static_cast<SPILength const *>(property)->computed); break;
            case SPAttr::STROKE_MITERLIMIT: number(static_cast<SPIFloat const *>(property)->value); break;
            case SPAttr::OPACITY: case SPAttr::SOLID_OPACITY: case SPAttr::FILL_OPACITY:
            case SPAttr::STROKE_OPACITY: case SPAttr::STOP_OPACITY:
                number(static_cast<SPIScale24 const *>(property)->value); break;
            case SPAttr::TEXT_DECORATION_COLOR: case SPAttr::COLOR: case SPAttr::SOLID_COLOR: case SPAttr::STOP_COLOR: {
                auto const *paint = static_cast<SPIColor const *>(property);
                number(paint->currentcolor); color(paint->getColor()); break;
            }
            case SPAttr::TEXT_DECORATION_FILL: case SPAttr::TEXT_DECORATION_STROKE:
            case SPAttr::FILL: case SPAttr::STROKE: {
                auto const *paint = static_cast<SPIPaint const *>(property);
                number(paint->paintOrigin); number(paint->isNone());
                auto const href = reinterpret_cast<std::uintptr_t>(paint->href ? paint->href->getObject() : nullptr);
                value.append(reinterpret_cast<char const *>(&href), sizeof(href));
                if (paint->isColor()) color(paint->getColor());
                break;
            }
            default:
                if (property->set || property->inherit || property->id() == SPAttr::INVALID) value += property->get_value().raw();
        }
        values.push_back(std::move(value));
    }
    for (auto const &[name, value] : style.extended_properties) values.push_back(name + '\0' + value);
    return values;
}

auto glyph_signature(Text::Layout const &layout)
{
    std::vector<std::pair<std::string, std::array<double, 12>>> signature;
    std::unordered_map<FontInstance *, std::string> fonts;
    signature.reserve(layout.glyphs().size());
    for (auto const &glyph : layout.glyphs()) {
        auto const transform = glyph.transform(layout);
        auto *font = glyph.span(&layout).font.get();
        auto [identity, inserted] = fonts.try_emplace(font);
        if (inserted && font) {
            auto *description = pango_font_description_to_string(font->get_descr());
            identity->second = description;
            g_free(description);
        }
        signature.push_back({identity->second,
            {double(glyph.glyph), double(glyph.in_character), double(glyph.hidden),
             double(glyph.orientation), glyph.advance, glyph.vertical_scale,
             transform[0], transform[1], transform[2], transform[3], transform[4], transform[5]}});
    }
    return signature;
}

// Only a flat text-bearing rendering element can be written without affecting
// unrepresented/protected descendants by inheritance. Wrappers are not targets.
bool flat_text_source(SPObject *source, SPItem *owner)
{
    if (!source || !source->firstChild()) return false;
    if (!(source == owner || repr_name_is(source, "svg:tspan") ||
          repr_name_is(source, "svg:flowSpan") || repr_name_is(source, "svg:flowPara") ||
          repr_name_is(source, "svg:textPath"))) return false;
    for (auto *child = source->firstChild(); child; child = child->getNext()) {
        if (!is<SPString>(child)) return false;
    }
    return true;
}

// Preservation snapshots belong to the prepared action, not each display or
// postwrite query. Capture once per owner. Untouched selected owners still need
// these checks when another member changes: selector side effects may reach them.
void capture_source_preservation(StrokeWidthMemberPlan &member);

/// Capture one logical character's rendered relevant-stroke snapshot through the
/// same source resolution the read-only query uses: the nearest style-bearing
/// SPItem source of the character. This deliberately does not use
/// `sp_te_style_at_position`, whose own SPString/nearest style can lose a
/// non-inheriting vector-effect value and its independent priority that the
/// renderer reads from the parent element. Returns false when no rendering source
/// is available; it never invents a style. Stores plain values only.
bool capture_rendered_char(Text::Layout const *layout, LayoutSources const &origins, SPItem *owner, unsigned index,
                           double transform_scale, StrokeWidthCharBaseline &out,
                           SPObject **source_out = nullptr)
{
    ++g_work.char_captures;
    if (source_out) *source_out = nullptr;
    if (!layout) return false;
    auto it = layout->charIndexToIterator(static_cast<int>(index));
    out.glyph = it.hasGlyph();
    SPObject *raw_source = nullptr;
    auto const kind = origins.kind(index, &raw_source);
    out.authored = kind == 1;
    out.glyph_index = it.glyphIndex();
    auto const anchor = layout->characterAnchorPoint(it);
    out.anchor = {anchor.x(), anchor.y()};
    if (!raw_source || (kind != 1 && kind != 2)) return false;
    auto *source = nearest_style_source(raw_source, owner);
    if (!source || !source->style) return false;
    out.style = origins.style(source, transform_scale);
    if (source_out) *source_out = source;
    return true;
}

/// Any tref within the owner scope (the style source or the raw character
/// source, and their ancestors up to but excluding the owner) is unsupported.
bool has_tref_within(SPObject *node, SPObject *owner)
{
    for (auto *o = node; o && o != owner; o = o->parent) {
        if (is<SPTRef>(o)) return true;
    }
    return false;
}

bool subtree_has_unsupported_clone_child(SPObject *object)
{
    for (auto &child : object->children) {
        auto *child_object = &child;
        if (is<SPUse>(child_object) || is<SPGroup>(child_object) || is<SPText>(child_object) ||
            is<SPFlowtext>(child_object) || is<SPTextPath>(child_object)) {
            return true;
        }
        if (subtree_has_unsupported_clone_child(child_object)) return true;
    }
    return false;
}

/// A referenced clone source owns its stroke width (explicitly or `!important`), so its
/// instances no longer follow the width of their own use. An explicit/important `inherit`
/// is compatible. The single predicate behind `CloneSourceOverrides`, used by the query and
/// by the post-write clone expectation.
bool source_overrides_width(SPObject const *source)
{
    return source && source->style && !source->style->stroke_width.inherit &&
           (source->style->stroke_width.important || source->style->stroke_width.set);
}

class Resolver
{
public:
    explicit Resolver(SPDocument &document)
        : _document(document)
    {}

    StrokeWidthResult run(std::vector<SPItem *> const &roots)
    {
        reset();
        for (auto *root : roots) {
            collect(root);
        }
        finalize();
        return _result;
    }

    StrokeWidthResult run(StrokeWidthTextRange const &range, std::vector<SPItem *> const &roots,
                          bool combined = false)
    {
        reset();
        if (!validate_range(range, roots)) return _result;
        if (combined) {
            _range_owner = range.owner.get();
            for (auto *root : roots) collect(root);
            _range_owner = nullptr;
            finalize();
        }
        return _result;
    }

private:
    SPDocument &_document;
    StrokeWidthResult _result;
    std::unordered_set<SPItem *> _seen;
    std::unordered_set<SPItem *> _eligible_seen;
    SPItem *_range_owner = nullptr;

    void reset()
    {
        _result = StrokeWidthResult{};
        _seen.clear();
        _eligible_seen.clear();
    }

    void submit(StrokeWidthTarget target)
    {
        bool any_eligible = false;
        for (auto const &run : target.runs) {
            if (run.eligibility == StrokeWidthEligibility::Eligible) {
                any_eligible = true;
                break;
            }
        }
        if (any_eligible && target.exclusion != StrokeWidthExclusion::EmptyText) {
            target.eligibility = StrokeWidthEligibility::Eligible;
            target.exclusion = StrokeWidthExclusion::None;
            if (auto *owner = target.owner.get()) _eligible_seen.insert(owner);
            _result.targets.push_back(std::move(target));
        } else {
            if (target.eligibility == StrokeWidthEligibility::Eligible) {
                target.eligibility = StrokeWidthEligibility::Unavailable;
                for (auto const &run : target.runs) {
                    if (run.eligibility != StrokeWidthEligibility::Eligible) {
                        target.eligibility = run.eligibility;
                        target.exclusion = run.exclusion;
                        break;
                    }
                }
                if (target.exclusion == StrokeWidthExclusion::None) {
                    target.exclusion = StrokeWidthExclusion::UnsupportedType;
                }
            }
            _result.excluded.push_back(std::move(target));
        }
    }

    void emit_excluded(SPItem *item, StrokeWidthTargetKind kind, StrokeWidthEligibility eligibility,
                       StrokeWidthExclusion exclusion)
    {
        StrokeWidthTarget target;
        target.owner = item;
        target.kind = kind;
        target.eligibility = eligibility;
        target.exclusion = exclusion;
        submit(std::move(target));
    }

    void emit_covered(SPItem *item)
    {
        emit_excluded(item, kind_for(item), StrokeWidthEligibility::Covered, StrokeWidthExclusion::None);
    }

    void collect(SPItem *item)
    {
        if (!item) return;
        // The explicit range owns this text member even when a selected group
        // also contains it. Never replace it with a whole-owner record.
        if (item == _range_owner) return;
        // Explicit scope validation: a root from another document is never
        // traversed or resolved, regardless of its type or position.
        if (item->document != &_document) {
            emit_excluded(item, kind_for(item), StrokeWidthEligibility::Unavailable,
                          StrokeWidthExclusion::WrongDocument);
            return;
        }
        if (!_seen.insert(item).second) {
            if (_eligible_seen.count(item)) emit_covered(item);
            return;
        }

        SPObject *allowed_text_owner =
            (is<SPText>(item) || is<SPFlowtext>(item)) ? static_cast<SPObject *>(item) : nullptr;
        auto const protection = node_protection(item, allowed_text_owner, nullptr);
        if (protection != ProtectionKind::None) {
            emit_excluded(item, kind_for(item), StrokeWidthEligibility::Unavailable,
                          exclusion_for(protection));
            return;
        }

        if (auto *use = cast<SPUse>(item)) {
            emit_clone(use);
        } else if (is_reference_container(item)) {
            emit_excluded(item, kind_for(item), StrokeWidthEligibility::Unavailable,
                          StrokeWidthExclusion::DefinitionOrReference);
        } else if (is<SPText>(item) || is<SPFlowtext>(item)) {
            emit_text_owner(item, true, false, 0, 0, 0, 0, false);
        } else if (is<SPShape>(item)) {
            emit_shape(item);
        } else if (is<SPImage>(item)) {
            emit_excluded(item, StrokeWidthTargetKind::Other, StrokeWidthEligibility::Incompatible,
                          StrokeWidthExclusion::Bitmap);
        } else if (auto *group = cast<SPGroup>(item)) {
            if (group->layerMode() == SPGroup::GROUP && repr_name_is(group, "svg:g")) {
                for (auto &child : group->children) {
                    if (auto *child_item = cast<SPItem>(&child)) collect(child_item);
                }
            } else {
                emit_excluded(item, StrokeWidthTargetKind::Container,
                              StrokeWidthEligibility::Incompatible,
                              StrokeWidthExclusion::UnsupportedContainer);
            }
        } else {
            emit_excluded(item, StrokeWidthTargetKind::Other, StrokeWidthEligibility::Incompatible,
                          StrokeWidthExclusion::UnsupportedType);
        }
    }

    void emit_shape(SPItem *item)
    {
        StrokeWidthTarget target;
        target.owner = item;
        target.kind = StrokeWidthTargetKind::Shape;

        auto const affine = item->i2doc_affine();
        double scale = 1.0;
        auto const transform_check = classify_transform(affine, scale);
        if (transform_check != TransformCheck::Usable) {
            target.safe_transform = false;
            target.transform_scale = scale;
            target.eligibility = StrokeWidthEligibility::Unavailable;
            target.exclusion = transform_check == TransformCheck::NonFinite
                                   ? StrokeWidthExclusion::NonFiniteTransform
                                   : StrokeWidthExclusion::SingularTransform;
            submit(std::move(target));
            return;
        }
        target.transform_scale = scale;

        if (!item->style) {
            emit_excluded(item, StrokeWidthTargetKind::Shape, StrokeWidthEligibility::Unavailable,
                          StrokeWidthExclusion::UnsupportedType);
            return;
        }
        target.runs.push_back(
            make_run(*item->style, scale, item, 0, 0, ProtectionKind::None));
        submit(std::move(target));
    }

    void emit_clone(SPUse *use)
    {
        StrokeWidthTarget target;
        target.owner = use;
        target.kind = StrokeWidthTargetKind::CloneInstance;
        target.clone_source = use->get_original();

        auto fail = [&](StrokeWidthEligibility eligibility, StrokeWidthExclusion exclusion) {
            target.eligibility = eligibility;
            target.exclusion = exclusion;
            submit(std::move(target));
        };

        auto *child = use->child;
        if (!child) {
            fail(StrokeWidthEligibility::MissingSource, StrokeWidthExclusion::MissingSource);
            return;
        }
        if (!is<SPShape>(child) || subtree_has_unsupported_clone_child(child) || !child->style) {
            fail(StrokeWidthEligibility::Unavailable, StrokeWidthExclusion::CloneUnsupportedChild);
            return;
        }

        auto *source = use->get_original();
        if (!source || !source->getRepr()) {
            fail(StrokeWidthEligibility::MissingSource, StrokeWidthExclusion::MissingSource);
            return;
        }
        // The narrow supported boundary: the referenced source must not own the
        // width explicitly (or must inherit it), and must not mark it important.
        // An explicit/important `inherit` is compatible: `important` alone must
        // not exclude when inherit is true.
        if (source_overrides_width(source)) {
            fail(StrokeWidthEligibility::Unavailable, StrokeWidthExclusion::CloneSourceOverrides);
            return;
        }

        auto const affine = child->i2doc_affine();
        double scale = 1.0;
        auto const transform_check = classify_transform(affine, scale);
        if (transform_check != TransformCheck::Usable) {
            target.safe_transform = false;
            target.transform_scale = scale;
            target.eligibility = StrokeWidthEligibility::Unavailable;
            target.exclusion = transform_check == TransformCheck::NonFinite
                                   ? StrokeWidthExclusion::NonFiniteTransform
                                   : StrokeWidthExclusion::SingularTransform;
            submit(std::move(target));
            return;
        }
        target.transform_scale = scale;

        // The use and its ancestors are checked first. Then the actual instance
        // child is checked narrowly, stopping before its use ancestor (already
        // checked) so an unselected referenced source's defs/ancestry is never
        // walked. This catches a hidden/locked/visibility source child.
        ProtectionKind protection = node_protection(use, nullptr, nullptr);
        if (protection == ProtectionKind::None) {
            protection = node_protection(child, nullptr, use);
        }
        target.runs.push_back(
            make_run(*child->style, scale, child, 0, 0, protection));
        submit(std::move(target));
    }

    void emit_text_owner(SPItem *item, bool whole_object, bool caret_scope, unsigned lo, unsigned hi,
                         unsigned raw_first, unsigned raw_last, bool reversed)
    {
        StrokeWidthTarget target;
        target.owner = item;
        target.kind = StrokeWidthTargetKind::TextOwner;
        target.whole_object = whole_object;
        target.caret_scope = caret_scope;
        target.reversed = reversed;

        auto const affine = item->i2doc_affine();
        double scale = 1.0;
        auto const transform_check = classify_transform(affine, scale);
        if (transform_check != TransformCheck::Usable) {
            target.safe_transform = false;
            target.transform_scale = scale;
            target.eligibility = StrokeWidthEligibility::Unavailable;
            target.exclusion = transform_check == TransformCheck::NonFinite
                                   ? StrokeWidthExclusion::NonFiniteTransform
                                   : StrokeWidthExclusion::SingularTransform;
            submit(std::move(target));
            return;
        }
        target.transform_scale = scale;

        auto const *layout = te_get_layout(item);
        unsigned const count = layout
            ? static_cast<unsigned>(layout->iteratorToCharIndex(layout->end())) : 0;

        // Freeze the whole-owner semantic content and logical count at query
        // time, independent of XML span structure. Presence is explicit: a
        // missing layout is never equivalent to a valid empty owner. This is a
        // read-only native child walk; no layout pointer/iterator is retained and
        // no ensureUpToDate is called.
        target.text_content_available = layout != nullptr;
        target.char_count = count;
        if (layout) {
            target.text_content = sp_te_get_string_multiline(item).raw();
        }

        target.raw_first_char = raw_first;
        target.raw_last_char = raw_last;
        if (count == 0) {
            // No layout/glyphs: exclude before planning. Keep the owner style
            // only as a read-only exclusion baseline, never a planned run.
            target.empty_text = true;
            target.eligibility = StrokeWidthEligibility::Unavailable;
            target.exclusion = StrokeWidthExclusion::EmptyText;
            if (item->style) {
                target.runs.push_back(make_run(*item->style, scale, item, 0, 0, ProtectionKind::None));
            }
            submit(std::move(target));
            return;
        }

        if (whole_object) {
            lo = 0;
            hi = count;
            // A caret keeps the caller's raw collapsed index as metadata; only
            // an object-selected whole owner normalizes raw to [0,count).
            if (!caret_scope) {
                raw_first = 0;
                raw_last = count;
            }
        }
        target.first_char = lo;
        target.last_char = hi;
        target.raw_first_char = raw_first;
        target.raw_last_char = raw_last;

        LayoutSources const origins(*layout);
        bool has_authored = false;
        struct PendingRun {
            SPObject *source = nullptr;
            StrokeWidthStyle style;
            StrokeWidthEligibility eligibility = StrokeWidthEligibility::Eligible;
            StrokeWidthExclusion exclusion = StrokeWidthExclusion::None;
            unsigned first = 0;
            unsigned last = 0;
        };
        PendingRun pending;
        bool have_pending = false;

        auto flush = [&]() {
            if (!have_pending) return;
            StrokeWidthRun run;
            run.first_char = pending.first;
            run.last_char = pending.last;
            run.style_source = pending.source;
            run.style = pending.style;
            run.eligibility = pending.eligibility;
            run.exclusion = pending.exclusion;
            target.runs.push_back(std::move(run));
            have_pending = false;
        };

        target.glyph_signature = glyph_signature(*layout);
        target.char_baseline.reserve(count);
        bool baseline_available = true;
        std::unordered_map<SPObject *, StrokeWidthRun> source_runs;
        for (unsigned index = 0; index < count; ++index) {
            StrokeWidthCharBaseline baseline;
            SPObject *source = nullptr;
            if (!capture_rendered_char(layout, origins, item, index, scale, baseline, &source)) {
                baseline_available = false;
                break;
            }
            target.char_baseline.push_back(std::move(baseline));
            if (index < lo || index >= hi) continue;
            if (!target.char_baseline.back().authored) {
                flush(); // controls keep their logical indices, but split writable runs
                continue;
            }
            has_authored = true;
            SPObject *raw_source = nullptr;
            origins.kind(index, &raw_source);
            auto [facts, inserted] = source_runs.try_emplace(source);
            if (inserted) {
                auto const protection = node_protection(source, item, item);
                auto const &style = origins.style(source, scale);

                StrokeWidthEligibility eligibility = StrokeWidthEligibility::Eligible;
                StrokeWidthExclusion exclusion = StrokeWidthExclusion::None;
                if (has_tref_within(raw_source, item) || has_tref_within(source, item)) {
                    // A whole-owner tref run can never be edited through the tref
                    // itself; explicit ranges already reject the whole query.
                    eligibility = StrokeWidthEligibility::Unavailable;
                    exclusion = StrokeWidthExclusion::UnsupportedTref;
                } else {
                    classify_style(style, protection, eligibility, exclusion);
                }
                facts->second.style = style;
                facts->second.eligibility = eligibility;
                facts->second.exclusion = exclusion;
            }
            auto const &style = facts->second.style;
            auto const eligibility = facts->second.eligibility;
            auto const exclusion = facts->second.exclusion;
            if (have_pending && pending.source == source && pending.eligibility == eligibility &&
                pending.exclusion == exclusion) {
                pending.last = index + 1;
            } else {
                flush();
                pending.source = source;
                pending.style = style;
                pending.eligibility = eligibility;
                pending.exclusion = exclusion;
                pending.first = index;
                pending.last = index + 1;
                have_pending = true;
            }
        }
        flush();

        if (!has_authored) {
            target.empty_text = true;
            target.eligibility = StrokeWidthEligibility::Unavailable;
            target.exclusion = StrokeWidthExclusion::EmptyText;
        }

        if (!baseline_available || target.char_baseline.size() != count) {
            target.char_baseline.clear();
            target.runs.clear();
            target.eligibility = StrokeWidthEligibility::Unavailable;
            target.exclusion = StrokeWidthExclusion::InvalidStyle;
            submit(std::move(target));
            return;
        }

        submit(std::move(target));
    }

    bool contains_unsupported_tref(SPItem *item, unsigned lo, unsigned hi)
    {
        auto const *layout = te_get_layout(item);
        if (!layout || lo >= hi) return false;
        auto it = layout->charIndexToIterator(static_cast<int>(lo));
        for (unsigned index = lo; index < hi && it != layout->end(); ++index, it.nextCharacter()) {
            SPObject *source = nullptr;
            layout->getSourceOfCharacter(it, &source);
            if (has_tref_within(source, item)) return true;
        }
        return false;
    }

    bool validate_range(StrokeWidthTextRange const &range, std::vector<SPItem *> const &roots)
    {
        auto *owner = range.owner.get();
        if (!owner || (!is<SPText>(owner) && !is<SPFlowtext>(owner)) || owner->document != &_document) {
            return reject(StrokeWidthExclusion::InvalidTextRange), false;
        }

        // The scope owner must lie inside at least one explicit root (or equal
        // it). It is never broadened to the bound root.
        bool bound = false;
        for (auto *root : roots) {
            // Scope validation: only roots belonging to this document can bind
            // the range. A foreign root never broadens or satisfies the scope.
            if (root && root->document == &_document &&
                (root == owner || root->isAncestorOf(owner))) {
                bound = true;
                break;
            }
        }
        if (!bound) {
            return reject(StrokeWidthExclusion::NotBoundToRoot), false;
        }

        auto const *layout = te_get_layout(owner);
        if (!layout) {
            collect_range(owner, range.caret, range.caret, 0, 0,
                          range.first_char, range.last_char, range.first_char > range.last_char);
            finalize();
            return true;
        }
        unsigned const count = static_cast<unsigned>(layout->iteratorToCharIndex(layout->end()));

        if (range.caret) {
            // Validate the caller's caret before whole-owner resolution: a
            // non-collapsed or out-of-range caret is rejected, never broadened.
            if (range.first_char != range.last_char) {
                return reject(StrokeWidthExclusion::InvalidTextRange), false;
            }
            if (range.first_char > count) {
                return reject(StrokeWidthExclusion::TextRangeOutOfBounds), false;
            }
            collect_range(owner, true, true, 0, count, range.first_char, range.last_char, false);
            finalize();
            return true;
        }

        if (range.first_char == range.last_char) {
            return reject(StrokeWidthExclusion::InvalidTextRange), false;
        }
        unsigned const lo = std::min(range.first_char, range.last_char);
        unsigned const hi = std::max(range.first_char, range.last_char);
        if (hi > count) {
            return reject(StrokeWidthExclusion::TextRangeOutOfBounds), false;
        }
        if (contains_unsupported_tref(owner, lo, hi)) {
            return reject(StrokeWidthExclusion::UnsupportedTref), false;
        }
        collect_range(owner, false, false, lo, hi, range.first_char, range.last_char,
                      range.first_char > range.last_char);
        finalize();
        return true;
    }

    bool reject(StrokeWidthExclusion exclusion)
    {
        _result = StrokeWidthResult{};
        _result.range_rejected = true;
        _result.range_exclusion = exclusion;
        return false;
    }

    void collect_range(SPItem *owner, bool whole_object, bool caret_scope, unsigned lo, unsigned hi,
                       unsigned raw_first, unsigned raw_last, bool reversed)
    {
        auto const protection =
            node_protection(owner, static_cast<SPObject *>(owner), nullptr);
        if (protection != ProtectionKind::None) {
            emit_excluded(owner, StrokeWidthTargetKind::TextOwner, StrokeWidthEligibility::Unavailable,
                          exclusion_for(protection));
            return;
        }
        emit_text_owner(owner, whole_object, caret_scope, lo, hi, raw_first, raw_last, reversed);
    }

    void compute_state()
    {
        bool any = false;
        bool any_ordinary = false;
        bool any_hairline = false;
        bool uniform = true;
        std::optional<double> first;

        for (auto const &target : _result.targets) {
            for (auto const &run : target.runs) {
                if (run.eligibility != StrokeWidthEligibility::Eligible) continue;
                any = true;
                if (run.style.convention == StrokeWidthConvention::Hairline) {
                    any_hairline = true;
                    continue;
                }
                any_ordinary = true;
                double const value = run.style.effective_px.value_or(run.style.local_computed);
                if (!first) {
                    first = value;
                } else if (!same_displayed_width(*first, value)) {
                    uniform = false;
                }
            }
        }

        if (!any) {
            _result.state = StrokeWidthQuery::Empty;
            _result.uniform_px.reset();
        } else if (!any_ordinary) {
            // All eligible members are hairlines: uniform, with no numeric value.
            _result.state = StrokeWidthQuery::Uniform;
            _result.uniform_px.reset();
        } else if (any_hairline || !uniform) {
            _result.state = StrokeWidthQuery::Mixed;
            _result.uniform_px.reset();
        } else {
            _result.state = StrokeWidthQuery::Uniform;
            _result.uniform_px = first;
        }
    }

    void count_flags()
    {
        auto scan = [this](StrokeWidthTarget const &target) {
            bool paint_none = false;
            bool hairline = false;
            bool non_scaling = false;
            for (auto const &run : target.runs) {
                if (run.eligibility != StrokeWidthEligibility::Eligible) continue;
                paint_none = paint_none || run.style.paint_none;
                hairline = hairline || run.style.convention == StrokeWidthConvention::Hairline;
                // Aggregate by the settled convention so a hairline fallback is
                // never double-reported as ordinary non-scaling.
                non_scaling =
                    non_scaling || run.style.convention == StrokeWidthConvention::NonScaling;
            }
            if (paint_none) ++_result.paint_none;
            if (hairline) ++_result.hairline;
            if (non_scaling) ++_result.non_scaling;
        };
        for (auto const &target : _result.targets) scan(target);
    }

    void finalize()
    {
        _result.eligible = _result.targets.size();
        // A combined range is finalized once for its owner and again after
        // collecting the other roots. Recompute counters from the records.
        _result.incompatible = _result.unavailable = _result.covered = 0;
        _result.missing_sources = 0;
        _result.paint_none = _result.hairline = _result.non_scaling = 0;
        for (auto const &target : _result.excluded) {
            switch (target.eligibility) {
            case StrokeWidthEligibility::Incompatible:
                ++_result.incompatible;
                break;
            case StrokeWidthEligibility::Unavailable:
                ++_result.unavailable;
                break;
            case StrokeWidthEligibility::Covered:
                ++_result.covered;
                break;
            case StrokeWidthEligibility::MissingSource:
                ++_result.missing_sources;
                break;
            case StrokeWidthEligibility::Eligible:
                break;
            }
        }
        count_flags();
        compute_state();
        sort_by_document_order();
    }

    void sort_by_document_order()
    {
        // Compare owners directly with the established tree-position helper.
        // Same-document valid owners come first; null and foreign owners (for
        // example a rejected WrongDocument root) sort last and keep their stable
        // relative order. Equal owners compare false.
        auto by_position = [this](StrokeWidthTarget const &a, StrokeWidthTarget const &b) {
            SPItem *owner_a = a.owner.get();
            SPItem *owner_b = b.owner.get();
            if (owner_a == owner_b) return false;
            bool const valid_a = owner_a && owner_a->document == &_document;
            bool const valid_b = owner_b && owner_b->document == &_document;
            if (valid_a != valid_b) return valid_a;
            if (!valid_a && !valid_b) return false;
            return sp_object_compare_position(owner_a, owner_b) < 0;
        };
        std::stable_sort(_result.targets.begin(), _result.targets.end(), by_position);
        std::stable_sort(_result.excluded.begin(), _result.excluded.end(), by_position);
    }
};

} // namespace

bool stroke_width_same_width(double a_px, double b_px)
{
    return same_displayed_width(a_px, b_px);
}

StrokeWidthResult query_stroke_widths(SPDocument &document, std::vector<SPItem *> const &roots)
{
    Resolver resolver(document);
    return resolver.run(roots);
}

StrokeWidthResult query_stroke_widths(SPDocument &document, StrokeWidthTextRange const &text_range,
                                      std::vector<SPItem *> const &roots)
{
    Resolver resolver(document);
    return resolver.run(text_range, roots);
}

namespace {
StrokeWidthResult query_combined_stroke_widths(SPDocument &document,
                                               StrokeWidthTextRange const &text_range,
                                               std::vector<SPItem *> const &roots)
{
    Resolver resolver(document);
    return resolver.run(text_range, roots, true);
}
}

namespace {

/// Frozen SPStyleSrc ordinals (style-internal.h) kept as plain integers so the
/// public header stays independent of style-internal.h.
bool stylesheet_important(unsigned char style_src, bool important)
{
    return important && static_cast<SPStyleSrc>(style_src) == SPStyleSrc::STYLE_SHEET;
}

std::optional<std::string> inline_style_of(SPItem *item)
{
    if (!item || !item->getRepr()) return std::nullopt;
    char const *style = item->getRepr()->attribute("style");
    if (!style) return std::nullopt;
    return std::string(style);
}

/// Exact authored repr attributes of `item`, excluding only `style`, copied and
/// then sorted by name. The repr itself is never reordered or mutated. This is
/// an authored-XML snapshot for a later preflight/output check, not proof of a
/// rendered bound or a universal computed style.
std::vector<std::pair<std::string, std::string>> non_style_attributes_of(SPItem *item)
{
    std::vector<std::pair<std::string, std::string>> attributes;
    if (!item || !item->getRepr()) return attributes;
    for (auto const &attr : item->getRepr()->attributeList()) {
        char const *name = g_quark_to_string(attr.key);
        if (!name || std::strcmp(name, "style") == 0) continue;
        attributes.emplace_back(name, static_cast<char const *>(attr.value));
    }
    std::sort(attributes.begin(), attributes.end());
    return attributes;
}

/// Accept only native serialization of an existing text-owner position or
/// transform. The plan's exact XML checks remain the authority for all other
/// changes. A float-sized tolerance covers length serialization; SVG transform
/// output uses eight significant digits.
bool same_text_owner_length_list(std::string const &before, char const *after)
{
    auto parse = [](char const *value) -> std::optional<std::vector<SVGLength>> {
        std::vector<SVGLength> lengths;
        if (!value || !*value) return std::nullopt;
        char const *cursor = value;
        while (*cursor) {
            SVGLength::Unit unit;
            double number = 0;
            double computed = 0;
            char *next = nullptr;
            if (!parse_number_with_unit(cursor, unit, number, computed, false, &next) || next == cursor) {
                return std::nullopt;
            }
            SVGLength length;
            length.set(unit, number, computed);
            lengths.push_back(length);
            cursor = next;
            if (*cursor == ',') {
                ++cursor;
                if (!*cursor) return std::nullopt;
            }
        }
        return lengths;
    };
    auto const old_values = parse(before.c_str());
    auto const new_values = parse(after);
    if (!old_values || !new_values || old_values->size() != new_values->size()) return false;
    for (std::size_t i = 0; i < old_values->size(); ++i) {
        auto const &a = (*old_values)[i];
        auto const &b = (*new_values)[i];
        if (a.unit != b.unit || !std::isfinite(a.value) || !std::isfinite(b.value)) return false;
        double const scale = std::max({1.0, std::fabs(a.value), std::fabs(b.value)});
        if (std::fabs(a.value - b.value) > std::numeric_limits<float>::epsilon() * scale) return false;
    }
    return true;
}

bool same_text_owner_transform(std::string const &before, char const *after)
{
    Geom::Affine a, b;
    if (!after || !sp_svg_transform_read(before.c_str(), &a) || !sp_svg_transform_read(after, &b)) {
        return false;
    }
    for (unsigned i = 0; i < 6; ++i) {
        if (!std::isfinite(a[i]) || !std::isfinite(b[i])) return false;
        double const scale = std::max({1.0, std::fabs(a[i]), std::fabs(b[i])});
        if (std::fabs(a[i] - b[i]) > 1e-7 * scale) return false;
    }
    return true;
}

void restore_reserialized_text_owner_attributes(
    SPItem *owner, std::vector<std::pair<std::string, std::string>> const &before)
{
    auto *repr = owner->getRepr();
    for (auto const &[name, authored] : before) {
        char const *current = repr->attribute(name.c_str());
        if (!current || authored == current) continue;
        bool const equivalent = name == "transform" ? same_text_owner_transform(authored, current) :
            (name == "x" || name == "y" || name == "dx" || name == "dy" ||
             name == "rotate" || name == "textLength") && same_text_owner_length_list(authored, current);
        if (equivalent) repr->setAttribute(name.c_str(), authored.c_str());
    }
}

constexpr std::array<char const *, 5> text_position_names{"x", "y", "dx", "dy", "rotate"};

std::array<std::optional<std::string>, 5> text_positions(SPObject *source)
{
    std::array<std::optional<std::string>, 5> result;
    for (unsigned i = 0; i < result.size(); ++i) {
        if (auto const *value = source->getRepr()->attribute(text_position_names[i])) result[i] = value;
    }
    return result;
}

// Native tidy retains line wrappers and spans with parsed positioning, but may
// join dx/dy/rotate-only siblings. Refuse uncertain identities before any write;
// a distinct class is a native, style-write-independent barrier to that join.
bool restorable_text_positions(StrokeWidthMemberPlan const &member)
{
    for (auto const &baseline : member.target.source_baseline) {
        if (!baseline.positions) continue;
        auto *span = cast<SPTSpan>(baseline.source.get());
        if (!span || !span->getRepr()) return false;
        if (span->role == SP_TSPAN_ROLE_LINE) continue;
        if (!span->attributes.anyAttributesSet() || !span->hasChildren()) return false;
        auto *parent = cast<SPTSpan>(span->parent);
        for (auto &sibling : span->parent->children) {
            if (&sibling == span || !is<SPTSpan>(&sibling)) continue;
            auto *repr = sibling.getRepr();
            if (g_strcmp0(span->getRepr()->attribute("class"), repr->attribute("class")) != 0) continue;
            // A later x/y or line span cannot be joined; rotate on the first
            // span also prevents joining. Otherwise the identity is uncertain.
            if (sp_object_compare_position(span, &sibling) < 0) {
                if (!cast<SPTSpan>(&sibling)->attributes.anyAttributesSet() ||
                    repr->attribute("x") || repr->attribute("y") ||
                    cast<SPTSpan>(&sibling)->role == SP_TSPAN_ROLE_LINE ||
                    span->getRepr()->attribute("rotate")) continue;
            } else if (span->getRepr()->attribute("x") || span->getRepr()->attribute("y") ||
                       repr->attribute("rotate")) continue;
            return false;
        }
        // Positionless enclosing spans can be unwrapped, exposing new siblings.
        if (!span->getRepr()->attribute("x") && !span->getRepr()->attribute("y") &&
            span->parent != member.target.owner.get() && (!parent || parent->role != SP_TSPAN_ROLE_LINE)) return false;
    }
    return true;
}

bool restore_text_positions(StrokeWidthMemberPlan const &member)
{
    for (auto const &baseline : member.target.source_baseline) {
        if (!baseline.positions) continue;
        auto *source = baseline.source.get();
        auto *owner = member.target.owner.get();
        if (!source || !owner || !owner->isAncestorOf(source)) return false;
        for (unsigned i = 0; i < text_position_names.size(); ++i) {
            auto const &value = (*baseline.positions)[i];
            if (g_strcmp0(source->getRepr()->attribute(text_position_names[i]),
                          value ? value->c_str() : nullptr) != 0) {
                source->getRepr()->setAttribute(text_position_names[i], value ? value->c_str() : nullptr);
            }
        }
    }
    return true;
}

/// Serialize intended local dash numbers with GLib's locale-independent
/// round-trip formatter and comma separators. Returns nullopt for any non-finite
/// entry. This is deliberately not a CSS formatter; the native parser owns all
/// value interpretation and normalization.
std::optional<std::string> serialize_dash_numbers(std::vector<double> const &values)
{
    std::string out;
    for (double value : values) {
        if (!std::isfinite(value)) return std::nullopt;
        char buffer[G_ASCII_DTOSTR_BUF_SIZE];
        g_ascii_dtostr(buffer, sizeof(buffer), value);
        if (!out.empty()) out += ", ";
        out += buffer;
    }
    return out;
}

/// The value the native CSS reader returns for the width text the writer
/// emits. The reader can return a different double for g_ascii_dtostr's
/// round-trip text (0.52916666666666656 reads back as 0.52916666666666701),
/// and every postcondition compares exact values, so a plan must carry the
/// value that reads back as itself. nullopt when none is reached.
std::optional<double> native_length(double value, char const *property = "stroke-width", bool allow_negative = false)
{
    double current = value;
    for (int attempt = 0; attempt < 4; ++attempt) {
        char buffer[G_ASCII_DTOSTR_BUF_SIZE];
        g_ascii_dtostr(buffer, sizeof(buffer), current);
        // Through the same path as an element's style attribute (libcroco
        // declaration parsing), which is where the digits drift.
        SPStyle native;
        native.mergeString((std::string(property) + ":" + buffer).c_str());
        double const parsed = std::string(property) == "stroke-dashoffset" ? native.stroke_dashoffset.computed
                                                                           : native.stroke_width.computed;
        if (!std::isfinite(parsed) || (!allow_negative && parsed < 0.0)) return std::nullopt;
        if (parsed == current) return current;
        // Only the reader's last-digit drift, never a different width.
        if ((value != 0.0 && parsed == 0.0) || std::fabs(parsed - value) > 1e-12 * std::fabs(value)) {
            return std::nullopt;
        }
        current = parsed;
    }
    return std::nullopt;
}

/// Text runs reach the element through several paths: some copy the CSS text
/// as given, others (sp_te_apply_style's re-serialization) rewrite it with the
/// CSS number precision (0.52916666666666701 becomes 0.529167). So a text run
/// writes the precision-limited text itself, which every path then keeps, and
/// is planned as the values that text reads back as (the text postconditions
/// compare exact values). The text must be stable: written again it is the
/// same, else the run keeps its full-precision plan.
void plan_as_text_writes(StrokeWidthTextRunPlan &run)
{
    auto number = [](double value) {
        char buffer[G_ASCII_DTOSTR_BUF_SIZE];
        g_ascii_dtostr(buffer, sizeof(buffer), value);
        return std::string(buffer);
    };
    bool const width = run.local_width && run.intent_kind != StrokeWidthIntentKind::Hairline;
    std::string declarations;
    if (width) declarations += "stroke-width:" + number(*run.local_width) + ";";
    if (run.native_dasharray_css) declarations += "stroke-dasharray:" + *run.native_dasharray_css + ";";
    if (run.local_dashoffset) declarations += "stroke-dashoffset:" + number(*run.local_dashoffset) + ";";
    if (declarations.empty()) return;

    SPStyle merged;
    merged.mergeString(declarations.c_str());
    std::string const written = merged.write(SP_STYLE_FLAG_IFSET).raw();
    SPStyle reread;
    reread.mergeString(written.c_str());
    if (reread.write(SP_STYLE_FLAG_IFSET).raw() != written) return; // not stable

    std::map<std::string, std::string> texts;
    std::size_t start = 0;
    while (start < written.size()) {
        auto const end = std::min(written.find(';', start), written.size());
        auto const declaration = written.substr(start, end - start);
        if (auto const colon = declaration.find(':'); colon != std::string::npos) {
            texts[declaration.substr(0, colon)] = declaration.substr(colon + 1);
        }
        start = end + 1;
    }
    if (width) {
        if (!texts.count("stroke-width")) return;
        run.width_css = texts["stroke-width"];
        run.local_width = reread.stroke_width.computed;
    }
    if (run.native_dasharray_css) {
        if (!texts.count("stroke-dasharray")) return;
        run.native_dasharray_css = texts["stroke-dasharray"];
        run.native_dasharray_computed = reread.stroke_dasharray.get_computed();
    }
    if (run.local_dashoffset) {
        if (!texts.count("stroke-dashoffset")) return;
        run.dashoffset_css = texts["stroke-dashoffset"];
        run.local_dashoffset = reread.stroke_dashoffset.computed;
    }
}

/// Canonicalize an intended local dash array through the native standalone
/// parser. Returns false when the native result cannot faithfully represent the
/// requested pattern: a nonzero raw vector normalized by the native parser to
/// empty, a length mismatch, a non-finite/non-round-tripping computed value, or
/// an invalid native result. On failure `css`/`computed` are left untouched. The
/// native parser's own zero boundary is used; it is never duplicated here.
bool canonicalize_dash_array(std::vector<double> const &raw, std::optional<std::string> &css,
                             std::optional<std::vector<double>> &computed)
{
    auto serialized = serialize_dash_numbers(raw);
    if (!serialized) return false;

    // Read back the way an element's style attribute is read (see native_length).
    SPStyle style;
    style.mergeString(("stroke-dasharray:" + *serialized).c_str());
    auto const &native = style.stroke_dasharray;
    if (!native.is_valid()) return false;

    auto parsed = native.get_computed();
    if (parsed.empty()) {
        // An exactly zero raw pattern is legitimately normalized by the native
        // parser to the empty (solid) array; any raw nonzero entry that becomes
        // empty is unrepresentable and must not be silently downgraded.
        bool const all_zero = std::all_of(raw.begin(), raw.end(),
                                          [](double value) { return value == 0.0; });
        if (!all_zero) return false;
    } else {
        if (parsed.size() != raw.size()) return false;
        for (std::size_t i = 0; i < parsed.size(); ++i) {
            // The native reader may drift in the last digits (see
            // native_length); the verifier compares against `parsed`, the
            // values this exact text reads back as.
            if (!std::isfinite(parsed[i]) || (raw[i] != 0.0 && parsed[i] == 0.0) ||
                std::fabs(parsed[i] - raw[i]) > 1e-12 * std::fabs(raw[i])) {
                return false;
            }
        }
    }

    css = std::move(serialized);
    computed = std::move(parsed);
    return true;
}

bool intent_value_is_valid(StrokeWidthIntent const &intent, StrokeWidthMemberReason &reason)
{
    // Exhaustive boundary validation: a cast to an out-of-range enum ordinal
    // must be rejected, never silently fall through the numeric switch below.
    switch (intent.kind) {
    case StrokeWidthIntentKind::AbsoluteCssPx:
    case StrokeWidthIntentKind::RelativePercent:
    case StrokeWidthIntentKind::Hairline:
    case StrokeWidthIntentKind::RemoveStroke:
        break;
    case StrokeWidthIntentKind::AdditiveCssPx:
        if (std::isfinite(intent.value) && intent.value != 0.0 && std::fabs(intent.value) <= 1e6) {
            return true;
        }
        reason = StrokeWidthMemberReason::InvalidIntent;
        return false;
    default:
        reason = StrokeWidthMemberReason::InvalidIntent;
        return false;
    }
    if (!std::isfinite(intent.value) || intent.value < 0.0) {
        reason = StrokeWidthMemberReason::InvalidIntent;
        return false;
    }
    return true;
}

StrokeWidthMemberPlan make_member(SPItem *owner, StrokeWidthTarget const &target)
{
    StrokeWidthMemberPlan member;
    member.target = target;
    if (owner) {
        Geom::Affine const affine = owner->i2doc_affine();
        for (unsigned i = 0; i < 6; ++i) member.i2doc_affine[i] = affine[i];
    }
    member.inline_style = inline_style_of(owner);
    member.original_parent = owner ? owner->parent : nullptr;
    member.non_style_attributes = non_style_attributes_of(owner);
    return member;
}

/// Concrete numeric result of one style candidate's width and per-property dash
/// preparation. Plain values only: no member, ownership or document state, so a
/// later text-run adapter can reuse the identical arithmetic. Patch fields are
/// populated only for `Change`, matching the existing shape plan.
struct NumericWidthPatch {
    StrokeWidthMemberOutcome outcome = StrokeWidthMemberOutcome::Unchanged;
    StrokeWidthMemberReason reason = StrokeWidthMemberReason::None;
    std::optional<double> local_width;
    std::optional<std::vector<double>> local_dasharray;
    std::optional<double> local_dashoffset;
    std::optional<std::string> native_dasharray_css;
    std::optional<std::vector<double>> native_dasharray_computed;
};

/// The single numeric width/dash calculation shared by the numeric shape plan
/// and the deferred text-run adapter. It is read-only and touches no member or
/// document state; callers supply the frozen style and transform scale. Every
/// existing outcome/reason, exact effective comparison, stylesheet !important
/// gate and native dash canonicalization is preserved here unchanged.
NumericWidthPatch plan_numeric_width(StrokeWidthStyle const &style, double transform_scale,
                                     StrokeWidthIntent const &intent)
{
    NumericWidthPatch patch;
    double const old_local = style.local_computed;

    // Relative and additive input have no numeric baseline on an existing hairline.
    if (style.convention == StrokeWidthConvention::Hairline) {
        if (intent.kind == StrokeWidthIntentKind::RelativePercent ||
            intent.kind == StrokeWidthIntentKind::AdditiveCssPx) {
            patch.outcome = StrokeWidthMemberOutcome::Excluded;
            patch.reason = StrokeWidthMemberReason::UnsupportedIntent;
            return patch;
        }
    }

    if (intent.kind == StrokeWidthIntentKind::Hairline) {
        if (stylesheet_important(style.width_style_src, style.width_important) ||
            stylesheet_important(style.vector_effect_style_src, style.vector_effect_important) ||
            stylesheet_important(style.hairline_style_src, style.hairline_important)) {
            patch.outcome = StrokeWidthMemberOutcome::Excluded;
            patch.reason = StrokeWidthMemberReason::StylePriority;
            return patch;
        }
        if (style.convention == StrokeWidthConvention::Hairline) {
            patch.outcome = StrokeWidthMemberOutcome::Unchanged;
            return patch;
        }
        // Reuse the numeric member's own dash policy for the 1px fallback;
        // the mode still changes when its old width was already 1px.
        StrokeWidthIntent fallback = intent;
        fallback.kind = StrokeWidthIntentKind::AbsoluteCssPx;
        fallback.value = style.non_scaling ? 1.0 : transform_scale;
        patch = plan_numeric_width(style, transform_scale, fallback);
        if (patch.outcome == StrokeWidthMemberOutcome::Excluded) return patch;
        patch.outcome = StrokeWidthMemberOutcome::Change;
        patch.local_width = 1.0;
        return patch;
    }
    if (intent.kind == StrokeWidthIntentKind::RemoveStroke) {
        if (stylesheet_important(style.paint_style_src, style.paint_important)) {
            patch.outcome = StrokeWidthMemberOutcome::Excluded;
            patch.reason = StrokeWidthMemberReason::StylePriority;
            return patch;
        }
        patch.outcome = style.paint_none ? StrokeWidthMemberOutcome::Unchanged
                                        : StrokeWidthMemberOutcome::Change;
        if (patch.outcome == StrokeWidthMemberOutcome::Change) {
            patch.local_width = style.local_computed; // structural write marker
        }
        return patch;
    }

    double new_local = old_local;
    switch (intent.kind) {
    case StrokeWidthIntentKind::AbsoluteCssPx:
        if (style.non_scaling && style.convention != StrokeWidthConvention::Hairline) {
            // An ordinary non-scaling stroke keeps its vector effect. A
            // hairline converts to an ordinary scaling stroke below.
            new_local = intent.value;
        } else {
            double const descrim = transform_scale;
            if (!std::isfinite(descrim) || descrim <= 0.0) {
                patch.outcome = StrokeWidthMemberOutcome::Excluded;
                patch.reason = StrokeWidthMemberReason::InvalidIntent;
                return patch;
            }
            new_local = intent.value / descrim;
        }
        break;
    case StrokeWidthIntentKind::RelativePercent:
        new_local = old_local * (intent.value / 100.0);
        break;
    case StrokeWidthIntentKind::AdditiveCssPx: {
        double const w = style.effective_px.value_or(old_local);
        if (intent.value < 0.0 && !stroke_width_can_decrease(w, -intent.value)) return patch;
        double const effective = w + intent.value;
        if (effective == w) return patch;
        new_local = style.non_scaling ? effective : effective / transform_scale;
        if (!std::isfinite(effective) || effective < 0.0 ||
            (effective > 0.0 && (new_local == 0.0 || std::fpclassify(new_local) == FP_SUBNORMAL))) {
            patch.outcome = StrokeWidthMemberOutcome::Excluded;
            patch.reason = StrokeWidthMemberReason::InvalidIntent;
            return patch;
        }
        break;
    }
    case StrokeWidthIntentKind::Hairline:
    case StrokeWidthIntentKind::RemoveStroke:
        break;
    }

    // A non-finite/negative/overflowing local width cannot be applied. The
    // fixed write-reason list has no separate arithmetic value; InvalidIntent
    // is the specific rejection used for every such computation.
    if (!std::isfinite(new_local) || new_local < 0.0) {
        patch.outcome = StrokeWidthMemberOutcome::Excluded;
        patch.reason = StrokeWidthMemberReason::InvalidIntent;
        return patch;
    }
    // Plan the width the written text reads back as (exact postconditions).
    // An unchanged request stays a no-op even when the authored value is not
    // itself a fixed point of the reader (checked again below).
    bool const requested_unchanged =
        style.convention != StrokeWidthConvention::Hairline &&
        (style.non_scaling ? new_local : new_local * transform_scale) ==
            (style.effective_px ? *style.effective_px : old_local);
    if (requested_unchanged) {
        patch.outcome = StrokeWidthMemberOutcome::Unchanged;
        return patch;
    }
    if (intent.kind == StrokeWidthIntentKind::AbsoluteCssPx ||
        intent.kind == StrokeWidthIntentKind::RelativePercent ||
        intent.kind == StrokeWidthIntentKind::AdditiveCssPx) {
        auto const native = native_length(new_local);
        if (!native) {
            patch.outcome = StrokeWidthMemberOutcome::Excluded;
            patch.reason = StrokeWidthMemberReason::InvalidIntent;
            return patch;
        }
        new_local = *native;
    }

    // Compare intended against frozen effective widths, not raw local values:
    // a tiny local delta under a large transform scale (e.g. old 0 -> 1e-10 at
    // scale 1e10) is a real effective change, while an ordinary relative value
    // that only overflows after the descrim multiply is an explicit rejection.
    bool const non_scaling = style.non_scaling && style.convention != StrokeWidthConvention::Hairline;
    double const old_effective = style.effective_px ? *style.effective_px : old_local;
    double const new_effective = non_scaling ? new_local : new_local * transform_scale;
    if (!std::isfinite(new_effective) || new_effective < 0.0) {
        patch.outcome = StrokeWidthMemberOutcome::Excluded;
        patch.reason = StrokeWidthMemberReason::InvalidIntent;
        return patch;
    }

    // Same effective result is a genuine no-op: retain no width/dash patch so
    // authored style is never normalized. Compare exact doubles, not the
    // query/display nearly_equal floor: the effective value is already in the
    // intended document-px space, so a representable change (old 0 -> 1e-20 at
    // ordinary scale 2) must still be planned and written. Signed zero still
    // compares equal; ordinary local*descrim and NonScaling effective==local are
    // both preserved unchanged.
    if (new_effective == old_effective &&
        style.convention != StrokeWidthConvention::Hairline) {
        patch.outcome = StrokeWidthMemberOutcome::Unchanged;
        return patch;
    }

    // A demanded change that a stylesheet !important rule controls cannot be
    // knowingly overridden by a normal inline patch. An inline !important is
    // retained through width_important instead (handled by the caller later).
    if (stylesheet_important(style.width_style_src, style.width_important)) {
        patch.outcome = StrokeWidthMemberOutcome::Excluded;
        patch.reason = StrokeWidthMemberReason::StylePriority;
        return patch;
    }
    if (style.convention == StrokeWidthConvention::Hairline &&
        stylesheet_important(style.hairline_style_src, style.hairline_important)) {
        patch.outcome = StrokeWidthMemberOutcome::Excluded;
        patch.reason = StrokeWidthMemberReason::StylePriority;
        return patch;
    }
    if (style.convention == StrokeWidthConvention::Hairline &&
        stylesheet_important(style.vector_effect_style_src, style.vector_effect_important)) {
        patch.outcome = StrokeWidthMemberOutcome::Excluded;
        patch.reason = StrokeWidthMemberReason::StylePriority;
        return patch;
    }

    // Dash scaling uses this member's own old/new ratio, only when enabled and
    // the old local width is positive; old width 0 and disabled scaling retain
    // no dash patch. The array and the offset are validated independently and
    // each is blocked only by its own stylesheet !important rule.
    std::optional<std::vector<double>> dash_array;
    std::optional<double> dash_offset;
    if (intent.scale_dashes && old_local > 0.0) {
        double const ratio = new_local / old_local;
        if (!std::isfinite(ratio) || ratio < 0.0) {
            patch.outcome = StrokeWidthMemberOutcome::Excluded;
            patch.reason = StrokeWidthMemberReason::InvalidDash;
            return patch;
        }

        if (!style.dash_computed.empty()) {
            // Validate the frozen originals before multiplying, then every
            // product, so a malformed snapshot is InvalidDash and never leaks a
            // non-finite or negative entry into the patch.
            for (double value : style.dash_computed) {
                if (!std::isfinite(value) || value < 0.0) {
                    patch.outcome = StrokeWidthMemberOutcome::Excluded;
                    patch.reason = StrokeWidthMemberReason::InvalidDash;
                    return patch;
                }
            }
            std::vector<double> scaled;
            scaled.reserve(style.dash_computed.size());
            bool changed = false;
            for (double value : style.dash_computed) {
                double const next = value * ratio;
                if (!std::isfinite(next) || next < 0.0) {
                    patch.outcome = StrokeWidthMemberOutcome::Excluded;
                    patch.reason = StrokeWidthMemberReason::InvalidDash;
                    return patch;
                }
                scaled.push_back(next);
                // Exact local inequality: a finite dash delta below the absolute
                // nearly_equal floor is still a real change when the effective
                // width moved (e.g. 1e-10 at scale 1e10).
                if (next != value) changed = true;
            }
            if (changed) {
                if (stylesheet_important(style.dash_style_src, style.dasharray_important)) {
                    patch.outcome = StrokeWidthMemberOutcome::Excluded;
                    patch.reason = StrokeWidthMemberReason::StylePriority;
                    return patch;
                }
                dash_array = std::move(scaled);
            }
        }

        // A changed offset is included even when it scales to 0 or is negative
        // finite, and is never gated on the array property's priority.
        // stroke-dasharray:none is never inserted.
        double const old_offset = style.dash_offset_computed;
        if (!std::isfinite(old_offset)) {
            patch.outcome = StrokeWidthMemberOutcome::Excluded;
            patch.reason = StrokeWidthMemberReason::InvalidDash;
            return patch;
        }
        double const next_offset = old_offset * ratio;
        if (!std::isfinite(next_offset)) {
            patch.outcome = StrokeWidthMemberOutcome::Excluded;
            patch.reason = StrokeWidthMemberReason::InvalidDash;
            return patch;
        }
        // Exact local inequality, matching the array rule above.
        if (next_offset != old_offset) {
            if (stylesheet_important(style.dashoffset_style_src, style.dashoffset_important)) {
                patch.outcome = StrokeWidthMemberOutcome::Excluded;
                patch.reason = StrokeWidthMemberReason::StylePriority;
                return patch;
            }
            auto const native = native_length(next_offset, "stroke-dashoffset", true);
            if (!native) {
                patch.outcome = StrokeWidthMemberOutcome::Excluded;
                patch.reason = StrokeWidthMemberReason::InvalidDash;
                return patch;
            }
            dash_offset = *native;
        }
    }

    // Canonicalize the intended dash array through the native parser before any
    // patch field is committed. A requested nonzero pattern the native parser
    // cannot round-trip is an explicit InvalidDash with no width/dash/css/
    // canonical patch fields, never a silently faked or downgraded patch.
    if (dash_array) {
        std::optional<std::string> native_css;
        std::optional<std::vector<double>> native_computed;
        if (!canonicalize_dash_array(*dash_array, native_css, native_computed)) {
            patch.outcome = StrokeWidthMemberOutcome::Excluded;
            patch.reason = StrokeWidthMemberReason::InvalidDash;
            return patch;
        }
        patch.native_dasharray_css = std::move(native_css);
        patch.native_dasharray_computed = std::move(native_computed);
    }

    patch.local_width = new_local;
    patch.local_dasharray = std::move(dash_array);
    patch.local_dashoffset = dash_offset;
    patch.outcome = StrokeWidthMemberOutcome::Change;
    return patch;
}

/// Plan one eligible numeric shape target. Read-only: the shared helper computes
/// intended local-unit values and the member receives its outcome/reason and
/// patch fields unchanged.
void plan_shape(StrokeWidthMemberPlan &member, StrokeWidthIntent const &intent)
{
    member.intent_kind = intent.kind;
    auto const &target = member.target;
    if (target.runs.size() != 1) {
        member.outcome = StrokeWidthMemberOutcome::Excluded;
        member.reason = StrokeWidthMemberReason::UnsupportedIntent;
        return;
    }
    auto const &style = target.runs[0].style;
    member.width_important = style.width_important;
    member.dasharray_important = style.dasharray_important;
    member.dashoffset_important = style.dashoffset_important;

    NumericWidthPatch patch = plan_numeric_width(style, target.transform_scale, intent);
    member.outcome = patch.outcome;
    member.reason = patch.reason;
    member.local_width = std::move(patch.local_width);
    member.local_dasharray = std::move(patch.local_dasharray);
    member.local_dashoffset = std::move(patch.local_dashoffset);
    member.native_dasharray_css = std::move(patch.native_dasharray_css);
    member.native_dasharray_computed = std::move(patch.native_dasharray_computed);
}

/// Text and clone adapters are explicitly deferred: never the numeric path.
void plan_adapter(StrokeWidthMemberPlan &member)
{
    switch (member.target.kind) {
    case StrokeWidthTargetKind::TextOwner:
        member.outcome = StrokeWidthMemberOutcome::Excluded;
        member.reason = StrokeWidthMemberReason::TextAdapterPending;
        break;
    case StrokeWidthTargetKind::CloneInstance:
        member.outcome = StrokeWidthMemberOutcome::Excluded;
        member.reason = StrokeWidthMemberReason::CloneAdapterPending;
        break;
    default:
        member.outcome = StrokeWidthMemberOutcome::Excluded;
        member.reason = StrokeWidthMemberReason::UnsupportedIntent;
        break;
    }
}

// F5: native splitting serializes SPString::string, not raw XML. Even when
// those match, collapsing whitespace can change at a newly created boundary.
// Prove only lossless, whitespace-free collapsing strings; preserved strings
// still require raw equality. Tidy touches the entire owner, including outside.
bool lossless_native_text(SPObject *node, bool nested_rule = false)
{
    // Tidy may unwrap a nested whitespace override. Do not infer its future
    // sibling context; whitespace-free strings are independent of that rule.
    if (!is<SPString>(node) && !is<SPText>(node) && !is<SPFlowtext>(node)) {
        nested_rule |= (node->getRepr() && node->getRepr()->attribute("xml:space")) ||
            (node->style && node->style->white_space.set);
    }
    if (auto *text = cast<SPString>(node)) {
        auto const *raw = text->getRepr()->content();
        if (!raw || text->string.raw() != raw) return false;
        if (!text->parent || !text->parent->style) return false;
        auto const white = text->parent->style->white_space.computed;
        bool const preserved = white == SP_CSS_WHITE_SPACE_PRE || white == SP_CSS_WHITE_SPACE_PREWRAP ||
            (white == SP_CSS_WHITE_SPACE_NORMAL && text->xml_space.value == SP_XML_SPACE_PRESERVE);
        return (!nested_rule && preserved) || text->string.raw().find_first_of(" \t\r\n") == std::string::npos;
    }
    for (auto &child : node->children) if (!lossless_native_text(&child, nested_rule)) return false;
    return true;
}

// A complete existing flat element needs no raw/logical substring mapping.
bool complete_range_source(StrokeWidthTarget const &target, StrokeWidthTextRunPlan const &run)
{
    auto *owner = target.owner.get();
    auto *source = run.style_source.get();
    if (!source || !flat_text_source(source, owner)) return false;
    if (source == owner) return is<SPText>(owner) && run.first_char == 0 && run.last_char == target.char_count;
    if (!(repr_name_is(source, "svg:tspan") || repr_name_is(source, "svg:flowSpan"))) return false;
    auto const *layout = te_get_layout(owner);
    if (!layout) return false;
    LayoutSources const origins(*layout);
    for (unsigned i = 0; i < target.char_count; ++i) {
        SPObject *raw = nullptr;
        bool const belongs = authored_character(origins, i, &raw) && nearest_style_source(raw, owner) == source;
        if (belongs != (i >= run.first_char && i < run.last_char)) return false;
    }
    return true;
}

/// Build the prospective per-run numeric records for one TextOwner member. Every
/// frozen target.runs entry yields exactly one record in logical order. An
/// eligible run reuses the identical shared numeric helper; an ineligible run
/// keeps its exclusion and no patch. The member is left Excluded/
/// TextAdapterPending by the caller, no parent-level patch is set, no
/// planned-change count is touched and no document state is read or written.
/// Read-only: no layout iterator or SPStyle pointer is retained.
void plan_text_runs(StrokeWidthMemberPlan &member, StrokeWidthIntent const &intent)
{
    auto const &target = member.target;
    member.text_runs.reserve(target.runs.size());
    for (auto const &run : target.runs) {
        StrokeWidthTextRunPlan run_plan;
        run_plan.intent_kind = intent.kind;
        run_plan.first_char = run.first_char;
        run_plan.last_char = run.last_char;
        run_plan.style_source = run.style_source;
        run_plan.style = run.style;
        run_plan.eligibility = run.eligibility;
        run_plan.exclusion = run.exclusion;

        if (run.eligibility == StrokeWidthEligibility::Eligible) {
            // Independent inline !important retention mirrors the shape member;
            // paint_none and zero width are not inspected here.
            run_plan.width_important = run.style.width_important;
            run_plan.dasharray_important = run.style.dasharray_important;
            run_plan.dashoffset_important = run.style.dashoffset_important;
            NumericWidthPatch patch = plan_numeric_width(run.style, target.transform_scale, intent);
            run_plan.outcome = patch.outcome;
            run_plan.reason = patch.reason;
            run_plan.local_width = std::move(patch.local_width);
            run_plan.local_dasharray = std::move(patch.local_dasharray);
            run_plan.local_dashoffset = std::move(patch.local_dashoffset);
            run_plan.native_dasharray_css = std::move(patch.native_dasharray_css);
            run_plan.native_dasharray_computed = std::move(patch.native_dasharray_computed);
            if (run_plan.outcome == StrokeWidthMemberOutcome::Change) {
                double const intended_local = run_plan.local_width.value_or(0.0);
                plan_as_text_writes(run_plan);
                // Additive classification uses the value the text writer stores,
                // including its precision limit. No patch for an unrepresentable delta.
                if (intent.kind == StrokeWidthIntentKind::AdditiveCssPx && run_plan.local_width) {
                    double const local = *run_plan.local_width;
                    double const effective = run.style.non_scaling ? local : local * target.transform_scale;
                    bool const unsafe = !std::isfinite(local) || !std::isfinite(effective) || local < 0.0 ||
                        (intended_local > 0.0 && (local == 0.0 || std::fpclassify(local) == FP_SUBNORMAL));
                    bool const unchanged = effective == run.style.effective_px.value_or(run.style.local_computed);
                    if (unsafe || unchanged) {
                        run_plan.outcome = unsafe ? StrokeWidthMemberOutcome::Excluded
                                                  : StrokeWidthMemberOutcome::Unchanged;
                        run_plan.reason = unsafe ? StrokeWidthMemberReason::InvalidIntent : StrokeWidthMemberReason::None;
                        run_plan.local_width.reset();
                        run_plan.local_dasharray.reset();
                        run_plan.local_dashoffset.reset();
                        run_plan.native_dasharray_css.reset();
                        run_plan.native_dasharray_computed.reset();
                        run_plan.width_css.reset();
                        run_plan.dashoffset_css.reset();
                    }
                }
            }
            // Exclusions are only the shared numeric planner's unsafe runs:
            // invalid width/dash/transform, stylesheet !important overrides,
            // and relative/additive hairlines (no numeric baseline). Ordinary,
            // non-scaling, absolute-converted hairline, and dashed text runs
            // all have native write_text_run patches and remain writable.
        } else {
            // Ineligible run: retain the query exclusion, no patch, no proposal.
            run_plan.outcome = StrokeWidthMemberOutcome::Excluded;
            run_plan.reason = StrokeWidthMemberReason::None;
        }
        member.text_runs.push_back(std::move(run_plan));
    }
    if (target.whole_object) {
        std::unordered_map<SPObject *, StrokeWidthTextRunPlan const *> changing;
        for (auto const &run : member.text_runs) if (run.outcome == StrokeWidthMemberOutcome::Change) changing.emplace(run.style_source.get(), &run);
        bool const coverage = std::all_of(member.text_runs.begin(), member.text_runs.end(), [&](auto const &run) {
            if (run.outcome != StrokeWidthMemberOutcome::Change) return true;
            auto *source = run.style_source.get();
            if (flat_text_source(source, target.owner.get())) return true;
            // Bounded one-level rendering parent: every element child is a
            // complete flat source that receives its own changing frozen patch.
            // Frozen child payloads must override every changed inherited family.
            if (!source || !(source == target.owner.get() || repr_name_is(source, "svg:tspan") ||
                repr_name_is(source, "svg:flowSpan") || repr_name_is(source, "svg:flowPara") ||
                repr_name_is(source, "svg:flowDiv") || repr_name_is(source, "svg:textPath"))) return false;
            for (auto &child : source->children) {
                if (is<SPString>(&child)) continue;
                if (!flat_text_source(&child, target.owner.get()) || !changing.count(&child) ||
                    node_protection(&child, target.owner.get(), target.owner.get()) != ProtectionKind::None ||
                    has_tref_within(&child, target.owner.get())) return false;
                auto const &patch = *changing.at(&child);
                if ((run.native_dasharray_css && !patch.native_dasharray_css) ||
                    (run.local_dashoffset && !patch.local_dashoffset)) return false;
            }
            return true;
        });
        if (!coverage) for (auto &run : member.text_runs) {
            run.outcome = StrokeWidthMemberOutcome::Excluded;
            run.reason = StrokeWidthMemberReason::UnsupportedTextCoverage;
        }
    } else if (!lossless_native_text(target.owner.get()) &&
        std::any_of(member.text_runs.begin(), member.text_runs.end(), [&](auto const &run) {
            return run.outcome == StrokeWidthMemberOutcome::Change && !complete_range_source(target, run);
        })) {
        // Exclude the owner before settlement/mutation, retaining independent
        // compatible members and genuine no-ops. Never normalize customer text.
        for (auto &run : member.text_runs) if (run.outcome == StrokeWidthMemberOutcome::Change) {
            run.outcome = StrokeWidthMemberOutcome::Excluded;
            run.reason = StrokeWidthMemberReason::UnsafeTextWhitespace;
        }
    }

}

/**
 * True when `item` belongs to the document and its ownership ancestry reaches
 * the weak plan root captured at prepare time. Wrong-document/dead/unbound
 * items are not dependency scope and are never inspected.
 */
bool owned_by_document_root(SPItem *item, SPDocument &document, SPObject *document_root)
{
    if (!item || item->document != &document) return false;
    for (SPObject *o = item; o; o = o->parent) {
        if (o == document_root) return true;
    }
    return false;
}

/**
 * Record the selected descendants of one bound explicit root. Ownership children
 * are followed, references are not. A selected SPUse is recorded and its
 * instantiated shadow child is never descended.
 */
void collect_selected_clones(SPItem *item, std::unordered_set<SPUse *> &out)
{
    if (!item) return;
    if (auto *use = cast<SPUse>(item)) {
        out.insert(use);
        return;
    }
    for (auto &child : item->children) {
        if (auto *child_item = cast<SPItem>(&child)) collect_selected_clones(child_item, out);
    }
}

enum class DependencyOutcome { NoConflict, Conflict, Uncertain };

/**
 * Read-only source-graph inspection for the bounded dependent-selection guard.
 *
 * It follows native SPUse original references (skipping the instantiated shadow
 * child) and inspects the ownership descendants of a referenced group/source so
 * nested clones and group-contained use references are reached. Object identity
 * is compared only against planned changed owners (shape members and text owners
 * with a prospective text-run change). It resolves no style or geometry, mutates
 * nothing, and treats a cycle, missing original or foreign node as Uncertain;
 * Uncertain anywhere in a graph wins over a Conflict in it.
 * Cycles use an in-progress set; completed nodes are memoized tri-state
 * (safe/uncertain).
 */
class DependencyScanner
{
public:
    DependencyScanner(SPDocument &document, std::unordered_set<SPItem *> const &changed_owners)
        : _document(document), _changed_owners(changed_owners)
    {}

    /// Report one selected clone's graph. `target` names that clone when the
    /// result is Conflict or Uncertain.
    DependencyOutcome follow(SPUse *clone, SPItem *&target)
    {
        auto const outcome = visit(clone);
        if (outcome != DependencyOutcome::NoConflict) target = clone;
        return outcome;
    }

private:
    SPDocument &_document;
    std::unordered_set<SPItem *> const &_changed_owners;
    std::unordered_set<SPObject *> _in_progress;
    std::unordered_set<SPObject *> _safe;
    std::unordered_set<SPObject *> _uncertain;
    /// A completed Conflict met no in-progress node (that would be Uncertain,
    /// which wins), so it does not depend on the path and is cached too:
    /// nested clone grids stay linear.
    std::unordered_set<SPObject *> _conflict;

    DependencyOutcome visit(SPObject *node)
    {
        if (!node) return DependencyOutcome::Uncertain;
        if (node->document != &_document) return DependencyOutcome::Uncertain;
        if (auto *item = cast<SPItem>(node)) {
            if (_changed_owners.count(item)) return DependencyOutcome::Conflict;
        }
        if (_safe.count(node)) return DependencyOutcome::NoConflict;
        if (_uncertain.count(node)) return DependencyOutcome::Uncertain;
        if (_conflict.count(node)) return DependencyOutcome::Conflict;
        if (!_in_progress.insert(node).second) return DependencyOutcome::Uncertain;

        DependencyOutcome result = DependencyOutcome::NoConflict;
        if (auto *use = cast<SPUse>(node)) {
            // Follow the referenced original; never the instantiated shadow child.
            result = visit(use->get_original());
        } else {
            // Every child: a missing or cyclic reference anywhere in the graph
            // makes it Uncertain even when it also holds a changed owner.
            for (auto &child : node->children) {
                auto const child_result = visit(&child);
                if (child_result == DependencyOutcome::Uncertain) {
                    result = DependencyOutcome::Uncertain;
                } else if (child_result == DependencyOutcome::Conflict &&
                           result == DependencyOutcome::NoConflict) {
                    result = DependencyOutcome::Conflict;
                }
            }
        }

        _in_progress.erase(node);
        if (result == DependencyOutcome::NoConflict) {
            _safe.insert(node);
        } else if (result == DependencyOutcome::Uncertain) {
            _uncertain.insert(node);
        } else {
            _conflict.insert(node);
        }
        return result;
    }
};

struct DependencyDecision {
    /// DependentCloneUncertain, or None.
    StrokeWidthMemberReason reason = StrokeWidthMemberReason::None;
    SPItem *target = nullptr;
    /// Selected clones whose source graph holds a planned change, sorted:
    /// they are never written and follow their original (owner decision
    /// 2026-09-28).
    std::vector<SPItem *> following;
};

/**
 * Bounded dependent-selection guard. Selected clones are the reported SPUse
 * query targets/excluded records plus the owned descendants of the bound
 * explicit roots, so protected/unsupported group descendants are inspected
 * read-only. With no planned changed owner there is no dependency conflict. A
 * changed shape owner, or a text owner whose prospective text runs include a
 * Change, inside a selected clone's source graph makes that clone follow its
 * original (counted, never written); an unresolvable/cyclic/foreign graph is
 * Uncertain and rejects the plan. Unselected clones are never inspected and
 * never block.
 */
DependencyDecision dependency_decision(StrokeWidthPlan const &plan, SPDocument &document)
{
    std::unordered_set<SPItem *> changed_owners;
    for (auto const &member : plan.members) {
        auto *owner = member.target.owner.get();
        if (!owner) continue;
        if (member.target.kind == StrokeWidthTargetKind::Shape) {
            // Unchanged: a planned shape write names its own owner.
            if (member.outcome == StrokeWidthMemberOutcome::Change) {
                changed_owners.insert(owner);
            }
            continue;
        }
        if (member.target.kind == StrokeWidthTargetKind::TextOwner) {
            // The text member itself stays Excluded/TextAdapterPending, so its
            // prospective writes exist only as text-run metadata. An eligible
            // run with a Change is an actual potential text write and therefore
            // a real dependency, exactly like a changed shape owner.
            for (auto const &run : member.text_runs) {
                if (run.eligibility == StrokeWidthEligibility::Eligible &&
                    run.outcome == StrokeWidthMemberOutcome::Change) {
                    changed_owners.insert(owner);
                    break;
                }
            }
        }
    }
    if (changed_owners.empty()) return {};

    auto *document_root = plan.document_root.get();

    std::unordered_set<SPUse *> selected;
    auto add_reported_clone = [&](StrokeWidthTarget const &target) {
        if (target.kind != StrokeWidthTargetKind::CloneInstance) return;
        auto *use = cast<SPUse>(target.owner.get());
        if (use && owned_by_document_root(use, document, document_root)) selected.insert(use);
    };
    for (auto const &target : plan.query.targets) add_reported_clone(target);
    for (auto const &target : plan.query.excluded) add_reported_clone(target);

    for (auto const &root : plan.roots) {
        auto *root_item = root.get();
        if (owned_by_document_root(root_item, document, document_root)) {
            collect_selected_clones(root_item, selected);
        }
    }

    if (selected.empty()) return {};

    DependencyScanner scanner(document, changed_owners);
    DependencyDecision decision;
    for (auto *clone : selected) {
        SPItem *target = nullptr;
        auto const outcome = scanner.follow(clone, target);
        if (outcome == DependencyOutcome::Conflict) {
            decision.following.push_back(clone);
            continue;
        }
        if (outcome == DependencyOutcome::Uncertain &&
            decision.reason == StrokeWidthMemberReason::None) {
            decision.reason = StrokeWidthMemberReason::DependentCloneUncertain;
            decision.target = target;
        }
    }
    std::sort(decision.following.begin(), decision.following.end(), std::less<SPItem *>());
    return decision;
}

/// The live dependency guard still gives the planned outcome: no uncertain
/// graph and the same selected clones following a changed original.
bool dependency_as_planned(StrokeWidthPlan const &plan, SPDocument &document)
{
    auto const live = dependency_decision(plan, document);
    if (live.reason != StrokeWidthMemberReason::None) return false;
    if (live.following.size() != plan.following.size()) return false;
    for (std::size_t i = 0; i < live.following.size(); ++i) {
        if (live.following[i] != plan.following[i].get()) return false;
    }
    return true;
}

StrokeWidthPlan build_plan(SPDocument &document, StrokeWidthResult result,
                           std::vector<SPItem *> const &roots, StrokeWidthIntent const &intent,
                           std::uint64_t scope_generation,
                           std::optional<StrokeWidthTextRange> text_scope,
                           bool combined_text_scope = false)
{
    StrokeWidthPlan plan;
    plan.intent = intent;
    plan.query = std::move(result);
    plan.scope_generation = scope_generation;
    plan.document_root = document.getRoot();
    for (auto *root : roots) {
        if (root) plan.roots.emplace_back(root);
    }
    plan.text_scope = std::move(text_scope);
    plan.combined_text_scope = combined_text_scope;

    // An invalid explicit range already rejected atomically in the query; no
    // fallback and no member planning.
    StrokeWidthMemberReason intent_reason = StrokeWidthMemberReason::None;
    if (intent.kind == StrokeWidthIntentKind::AdditiveCssPx && !intent_value_is_valid(intent, intent_reason)) {
        plan.state = StrokeWidthPlanState::Rejected;
        plan.rejection = intent_reason;
        return plan;
    }
    if (plan.query.range_rejected) {
        plan.state = StrokeWidthPlanState::Rejected;
        return plan;
    }

    if (!intent_value_is_valid(intent, intent_reason)) {
        plan.state = StrokeWidthPlanState::Rejected;
        plan.rejection = intent_reason;
        return plan;
    }

    // Query-level incompatible/unavailable/missing records count as excluded;
    // Covered duplicates are tracked separately in the query and never here.
    plan.excluded = plan.query.incompatible + plan.query.unavailable + plan.query.missing_sources;

    for (auto const &target : plan.query.targets) {
        StrokeWidthMemberPlan member = make_member(target.owner.get(), target);
        if (target.kind == StrokeWidthTargetKind::Shape) {
            plan_shape(member, intent);
        } else {
            plan_adapter(member);
            // Text stays member-level Excluded/TextAdapterPending; eligible runs
            // additionally carry prospective numeric proposals as pending
            // metadata only, with no parent patch and no planned-change count.
            if (target.kind == StrokeWidthTargetKind::TextOwner) {
                plan_text_runs(member, intent);
                for (auto const &run : member.text_runs) {
                    if (run.outcome == StrokeWidthMemberOutcome::Excluded) ++plan.skipped_runs;
                }
            }
        }
        plan.members.push_back(std::move(member));
    }

    // Selected clones the query excluded are never written: keep their bindings to prove it.
    for (auto const &target : plan.query.excluded) {
        if (target.kind != StrokeWidthTargetKind::CloneInstance ||
            target.eligibility == StrokeWidthEligibility::Covered) {
            continue;
        }
        if (auto *clone = target.owner.get()) plan.preserved_clones.push_back(make_member(clone, target));
    }

    auto const dependency = dependency_decision(plan, document);
    plan.following_clones = dependency.following.size();
    for (auto *clone : dependency.following) {
        plan.following.emplace_back(clone);
    }
    if (dependency.reason != StrokeWidthMemberReason::None) {
        plan.following_clones = 0;
        plan.following.clear();
        plan.state = StrokeWidthPlanState::Rejected;
        plan.rejection = dependency.reason;
        plan.dependent_target = dependency.target;
        plan.members.clear();
        plan.preserved_clones.clear();
        plan.planned_changes = 0;
        plan.unchanged = 0;
        plan.excluded = 0;
        plan.skipped_runs = 0;
        return plan;
    }

    bool const changing_action = std::any_of(plan.members.begin(), plan.members.end(), [](auto const &member) {
        return member.outcome == StrokeWidthMemberOutcome::Change ||
            std::any_of(member.text_runs.begin(), member.text_runs.end(), [](auto const &run) {
                return run.outcome == StrokeWidthMemberOutcome::Change;
            });
    });
    if (changing_action) for (auto &member : plan.members) {
        if (member.target.kind == StrokeWidthTargetKind::TextOwner) capture_source_preservation(member);
    }

    for (auto const &member : plan.members) {
        switch (member.outcome) {
        case StrokeWidthMemberOutcome::Change:
            ++plan.planned_changes;
            break;
        case StrokeWidthMemberOutcome::Unchanged:
            ++plan.unchanged;
            break;
        case StrokeWidthMemberOutcome::Excluded:
            ++plan.excluded;
            break;
        }
    }
    return plan;
}

} // namespace

StrokeWidthPlan prepare_stroke_widths(SPDocument &document, std::vector<SPItem *> const &roots,
                                      StrokeWidthIntent const &intent,
                                      std::uint64_t scope_generation)
{
    return build_plan(document, query_stroke_widths(document, roots), roots, intent,
                      scope_generation, std::nullopt);
}

StrokeWidthPlan prepare_stroke_widths(SPDocument &document, StrokeWidthTextRange const &text_range,
                                      std::vector<SPItem *> const &roots,
                                      StrokeWidthIntent const &intent,
                                      std::uint64_t scope_generation)
{
    return build_plan(document, query_stroke_widths(document, text_range, roots), roots, intent,
                      scope_generation, std::optional<StrokeWidthTextRange>{text_range});
}

StrokeWidthPlan prepare_stroke_widths_combined(SPDocument &document,
                                               StrokeWidthTextRange const &text_range,
                                               std::vector<SPItem *> const &roots,
                                               StrokeWidthIntent const &intent,
                                               std::uint64_t scope_generation)
{
    return build_plan(document, query_combined_stroke_widths(document, text_range, roots),
                      roots, intent, scope_generation,
                      std::optional<StrokeWidthTextRange>{text_range}, true);
}

namespace {

// ---------------------------------------------------------------------------
// SW2B writer internals.
//
// The preflight comparison is intentionally exact: it re-derives the frozen
// plan from the current document and rejects any difference. The only double
// tolerance is that two NaNs compare equal, used solely to retain an unchanged
// excluded malformed record. NaNs are never treated as a supported value and no
// NaN is ever written.
// ---------------------------------------------------------------------------

bool same_double_frozen(double a, double b)
{
    if (std::isnan(a) && std::isnan(b)) return true;
    return a == b;
}

bool same_optional_double(std::optional<double> const &a, std::optional<double> const &b)
{
    if (a.has_value() != b.has_value()) return false;
    if (!a) return true;
    return same_double_frozen(*a, *b);
}

bool same_vector_frozen(std::vector<double> const &a, std::vector<double> const &b)
{
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (!same_double_frozen(a[i], b[i])) return false;
    }
    return true;
}

bool exact_vector(std::vector<double> const &a, std::vector<double> const &b)
{
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i] != b[i]) return false;
    }
    return true;
}

bool same_style_frozen(StrokeWidthStyle const &a, StrokeWidthStyle const &b)
{
    if (a.convention != b.convention || a.paint_none != b.paint_none || a.non_scaling != b.non_scaling) {
        return false;
    }
    if (!same_double_frozen(a.local_computed, b.local_computed)) return false;
    if (a.effective_px.has_value() != b.effective_px.has_value()) return false;
    if (a.effective_px && !same_double_frozen(*a.effective_px, *b.effective_px)) return false;
    if (a.dash_set != b.dash_set || !same_vector_frozen(a.dash_computed, b.dash_computed)) return false;
    if (a.paint_important != b.paint_important || a.paint_style_src != b.paint_style_src ||
        a.hairline_important != b.hairline_important ||
        a.hairline_style_src != b.hairline_style_src) return false;
    if (!same_double_frozen(a.dash_offset_computed, b.dash_offset_computed)) return false;
    if (a.width_set != b.width_set || a.width_inherit != b.width_inherit ||
        a.width_important != b.width_important) {
        return false;
    }
    if (a.width_style_src != b.width_style_src || a.dash_style_src != b.dash_style_src ||
        a.dashoffset_style_src != b.dashoffset_style_src) {
        return false;
    }
    if (a.dasharray_important != b.dasharray_important || a.dashoffset_important != b.dashoffset_important) {
        return false;
    }
    if (a.vector_effect_value != b.vector_effect_value ||
        a.vector_effect_set != b.vector_effect_set ||
        a.vector_effect_inherit != b.vector_effect_inherit ||
        a.vector_effect_important != b.vector_effect_important ||
        a.vector_effect_style_src != b.vector_effect_style_src) {
        return false;
    }
    return true;
}

bool same_run_frozen(StrokeWidthRun const &a, StrokeWidthRun const &b)
{
    return a.first_char == b.first_char && a.last_char == b.last_char &&
           a.style_source.get() == b.style_source.get() && a.eligibility == b.eligibility &&
           a.exclusion == b.exclusion && same_style_frozen(a.style, b.style);
}

/// Exact whole-owner baseline equality: size, glyph presence and every rendered
/// relevant-stroke field/priority. No tolerance and no relaxed element.
bool same_char_baseline_frozen(std::vector<StrokeWidthCharBaseline> const &a,
                               std::vector<StrokeWidthCharBaseline> const &b)
{
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].glyph != b[i].glyph || a[i].authored != b[i].authored ||
            a[i].glyph_index != b[i].glyph_index || a[i].anchor != b[i].anchor) return false;
        if (!same_style_frozen(a[i].style, b[i].style)) return false;
    }
    return true;
}

bool same_target_frozen(StrokeWidthTarget const &a, StrokeWidthTarget const &b)
{
    if (a.owner.get() != b.owner.get() || a.kind != b.kind) return false;
    if (a.whole_object != b.whole_object || a.caret_scope != b.caret_scope ||
        a.empty_text != b.empty_text) {
        return false;
    }
    if (a.first_char != b.first_char || a.last_char != b.last_char) return false;
    if (a.raw_first_char != b.raw_first_char || a.raw_last_char != b.raw_last_char ||
        a.reversed != b.reversed) {
        return false;
    }
    if (a.eligibility != b.eligibility || a.exclusion != b.exclusion) return false;
    if (a.safe_transform != b.safe_transform) return false;
    if (!same_double_frozen(a.transform_scale, b.transform_scale)) return false;
    if (a.clone_source.get() != b.clone_source.get()) return false;
    if (a.text_content != b.text_content || a.text_content_available != b.text_content_available ||
        a.char_count != b.char_count) {
        return false;
    }
    if (a.runs.size() != b.runs.size()) return false;
    for (std::size_t i = 0; i < a.runs.size(); ++i) {
        if (!same_run_frozen(a.runs[i], b.runs[i])) return false;
    }
    if (!same_char_baseline_frozen(a.char_baseline, b.char_baseline) ||
        a.glyph_signature != b.glyph_signature || a.authored_tree != b.authored_tree || a.source_baseline.size() != b.source_baseline.size()) return false;
    for (std::size_t i = 0; i < a.source_baseline.size(); ++i) {
        if (a.source_baseline[i].source.get() != b.source_baseline[i].source.get() ||
            a.source_baseline[i].inline_style != b.source_baseline[i].inline_style ||
            a.source_baseline[i].computed_style != b.source_baseline[i].computed_style ||
            a.source_baseline[i].inline_declarations != b.source_baseline[i].inline_declarations ||
            a.source_baseline[i].changed_properties != b.source_baseline[i].changed_properties ||
            a.source_baseline[i].computed_patched != b.source_baseline[i].computed_patched ||
            a.source_baseline[i].positions != b.source_baseline[i].positions) return false;
    }
    return true;
}

/// Run scope without the style payload: range, style source and run-level
/// eligibility are structural output scope, so they must survive a write.
bool same_run_scope(StrokeWidthRun const &a, StrokeWidthRun const &b)
{
    return a.first_char == b.first_char && a.last_char == b.last_char &&
           a.style_source.get() == b.style_source.get() && a.eligibility == b.eligibility &&
           a.exclusion == b.exclusion;
}

/// Full target metadata except the per-run style payload: whole/caret/empty,
/// normalized and raw range, direction, eligibility/exclusion, transform safety
/// and scale, clone source, and every run's structural scope. Styles are handled
/// separately because a changed shape legitimately has a different run style.
bool same_target_scope(StrokeWidthTarget const &a, StrokeWidthTarget const &b)
{
    if (a.owner.get() != b.owner.get() || a.kind != b.kind) return false;
    if (a.whole_object != b.whole_object || a.caret_scope != b.caret_scope ||
        a.empty_text != b.empty_text) {
        return false;
    }
    if (a.first_char != b.first_char || a.last_char != b.last_char) return false;
    if (a.raw_first_char != b.raw_first_char || a.raw_last_char != b.raw_last_char ||
        a.reversed != b.reversed) {
        return false;
    }
    if (a.eligibility != b.eligibility || a.exclusion != b.exclusion) return false;
    if (a.safe_transform != b.safe_transform) return false;
    if (!same_double_frozen(a.transform_scale, b.transform_scale)) return false;
    if (a.clone_source.get() != b.clone_source.get()) return false;
    if (a.text_content != b.text_content || a.text_content_available != b.text_content_available ||
        a.char_count != b.char_count) {
        return false;
    }
    if (a.runs.size() != b.runs.size()) return false;
    for (std::size_t i = 0; i < a.runs.size(); ++i) {
        if (!same_run_scope(a.runs[i], b.runs[i])) return false;
    }
    return true;
}

/// Target metadata without any per-run content at all. Native text span
/// rewriting legitimately changes run grouping and style-source identity while
/// preserving the owner, scope, transform, semantic content and logical count, so
/// a changed TextOwner's fresh query target is compared with this rather than
/// `same_target_scope` (whose run loop would fail on a legitimate split).
bool same_target_metadata(StrokeWidthTarget const &a, StrokeWidthTarget const &b)
{
    if (a.owner.get() != b.owner.get() || a.kind != b.kind) return false;
    if (a.whole_object != b.whole_object || a.caret_scope != b.caret_scope ||
        a.empty_text != b.empty_text) {
        return false;
    }
    if (a.first_char != b.first_char || a.last_char != b.last_char) return false;
    if (a.raw_first_char != b.raw_first_char || a.raw_last_char != b.raw_last_char ||
        a.reversed != b.reversed) {
        return false;
    }
    if (a.eligibility != b.eligibility || a.exclusion != b.exclusion) return false;
    if (a.safe_transform != b.safe_transform) return false;
    if (!same_double_frozen(a.transform_scale, b.transform_scale)) return false;
    if (a.clone_source.get() != b.clone_source.get()) return false;
    if (a.text_content != b.text_content || a.text_content_available != b.text_content_available ||
        a.char_count != b.char_count) {
        return false;
    }
    return true;
}

/// Full run comparison including style, used for eligible members whose outcome
/// is not Change (their style is still the frozen input style).
bool same_runs_frozen(std::vector<StrokeWidthRun> const &a, std::vector<StrokeWidthRun> const &b)
{
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (!same_run_frozen(a[i], b[i])) return false;
    }
    return true;
}

/// Exact frozen comparison of one prospective text-run record: range, weak
/// source identity, full style snapshot, run eligibility/exclusion, every
/// proposed patch field and canonical native dash result, the three independent
/// importance bits and the outcome/reason. No relaxed or tolerance comparison.
bool same_text_run_frozen(StrokeWidthTextRunPlan const &a, StrokeWidthTextRunPlan const &b)
{
    if (a.intent_kind != b.intent_kind) return false;
    if (a.first_char != b.first_char || a.last_char != b.last_char) return false;
    if (a.style_source.get() != b.style_source.get()) return false;
    if (!same_style_frozen(a.style, b.style)) return false;
    if (a.eligibility != b.eligibility || a.exclusion != b.exclusion) return false;
    if (!same_optional_double(a.local_width, b.local_width)) return false;
    if (a.local_dasharray.has_value() != b.local_dasharray.has_value()) return false;
    if (a.local_dasharray && !same_vector_frozen(*a.local_dasharray, *b.local_dasharray)) return false;
    if (!same_optional_double(a.local_dashoffset, b.local_dashoffset)) return false;
    if (a.native_dasharray_css != b.native_dasharray_css) return false;
    if (a.width_css != b.width_css || a.dashoffset_css != b.dashoffset_css) return false;
    if (a.native_dasharray_computed.has_value() != b.native_dasharray_computed.has_value()) return false;
    if (a.native_dasharray_computed &&
        !same_vector_frozen(*a.native_dasharray_computed, *b.native_dasharray_computed)) {
        return false;
    }
    if (a.width_important != b.width_important || a.dasharray_important != b.dasharray_important ||
        a.dashoffset_important != b.dashoffset_important) {
        return false;
    }
    return a.outcome == b.outcome && a.reason == b.reason;
}

bool same_member_frozen(StrokeWidthMemberPlan const &a, StrokeWidthMemberPlan const &b)
{
    if (a.intent_kind != b.intent_kind) return false;
    if (!same_target_frozen(a.target, b.target)) return false;
    for (unsigned i = 0; i < 6; ++i) {
        if (!same_double_frozen(a.i2doc_affine[i], b.i2doc_affine[i])) return false;
    }
    if (a.inline_style != b.inline_style) return false;
    if (a.original_parent.get() != b.original_parent.get()) return false;
    if (a.non_style_attributes != b.non_style_attributes) return false;
    if (a.text_runs.size() != b.text_runs.size()) return false;
    for (std::size_t i = 0; i < a.text_runs.size(); ++i) {
        if (!same_text_run_frozen(a.text_runs[i], b.text_runs[i])) return false;
    }
    if (!same_optional_double(a.local_width, b.local_width)) return false;
    if (a.local_dasharray.has_value() != b.local_dasharray.has_value()) return false;
    if (a.local_dasharray && !same_vector_frozen(*a.local_dasharray, *b.local_dasharray)) return false;
    if (!same_optional_double(a.local_dashoffset, b.local_dashoffset)) return false;
    if (a.native_dasharray_css != b.native_dasharray_css) return false;
    if (a.native_dasharray_computed.has_value() != b.native_dasharray_computed.has_value()) return false;
    if (a.native_dasharray_computed &&
        !same_vector_frozen(*a.native_dasharray_computed, *b.native_dasharray_computed)) {
        return false;
    }
    if (a.width_important != b.width_important || a.dasharray_important != b.dasharray_important ||
        a.dashoffset_important != b.dashoffset_important) {
        return false;
    }
    return a.outcome == b.outcome && a.reason == b.reason;
}

bool same_query_frozen(StrokeWidthResult const &a, StrokeWidthResult const &b)
{
    if (a.state != b.state || a.range_rejected != b.range_rejected ||
        a.range_exclusion != b.range_exclusion) {
        return false;
    }
    if (a.eligible != b.eligible || a.incompatible != b.incompatible ||
        a.unavailable != b.unavailable || a.covered != b.covered ||
        a.missing_sources != b.missing_sources) {
        return false;
    }
    if (a.paint_none != b.paint_none || a.hairline != b.hairline || a.non_scaling != b.non_scaling) {
        return false;
    }
    if (!same_optional_double(a.uniform_px, b.uniform_px)) return false;
    if (a.targets.size() != b.targets.size() || a.excluded.size() != b.excluded.size()) return false;
    for (std::size_t i = 0; i < a.targets.size(); ++i) {
        if (!same_target_frozen(a.targets[i], b.targets[i])) return false;
    }
    for (std::size_t i = 0; i < a.excluded.size(); ++i) {
        if (!same_target_frozen(a.excluded[i], b.excluded[i])) return false;
    }
    return true;
}

bool same_plan_frozen(StrokeWidthPlan const &a, StrokeWidthPlan const &b)
{
    if (a.state != b.state || a.rejection != b.rejection) return false;
    if (a.dependent_target.get() != b.dependent_target.get()) return false;
    if (a.document_root.get() != b.document_root.get()) return false;
    if (a.roots.size() != b.roots.size()) return false;
    for (std::size_t i = 0; i < a.roots.size(); ++i) {
        if (a.roots[i].get() != b.roots[i].get()) return false;
    }
    if (a.text_scope.has_value() != b.text_scope.has_value()) return false;
    if (a.combined_text_scope != b.combined_text_scope) return false;
    if (a.text_scope) {
        if (a.text_scope->owner.get() != b.text_scope->owner.get()) return false;
        if (a.text_scope->caret != b.text_scope->caret) return false;
        if (a.text_scope->first_char != b.text_scope->first_char ||
            a.text_scope->last_char != b.text_scope->last_char) {
            return false;
        }
    }
    if (a.scope_generation != b.scope_generation) return false;
    if (a.intent.kind != b.intent.kind || a.intent.scale_dashes != b.intent.scale_dashes) return false;
    if (!same_double_frozen(a.intent.value, b.intent.value)) return false;
    if (!same_query_frozen(a.query, b.query)) return false;
    if (a.planned_changes != b.planned_changes || a.unchanged != b.unchanged ||
        a.excluded != b.excluded || a.skipped_runs != b.skipped_runs) {
        return false;
    }
    if (a.following.size() != b.following.size()) return false;
    for (std::size_t i = 0; i < a.following.size(); ++i) {
        if (a.following[i].get() != b.following[i].get()) return false;
    }
    if (a.members.size() != b.members.size()) return false;
    for (std::size_t i = 0; i < a.members.size(); ++i) {
        if (!same_member_frozen(a.members[i], b.members[i])) return false;
    }
    // The excluded clones' binding snapshots: a replacement of one (authored style or
    // attributes) is a different plan even when its query record is unchanged.
    if (a.preserved_clones.size() != b.preserved_clones.size()) return false;
    for (std::size_t i = 0; i < a.preserved_clones.size(); ++i) {
        if (!same_member_frozen(a.preserved_clones[i], b.preserved_clones[i])) return false;
    }
    return true;
}

/// The selected clones the query excluded are never written and never change class silently:
/// their binding, exact affine, authored attributes and inline style equal the prepare-time
/// snapshot whatever a source write does to their query record.
bool verify_preserved_clones(SPDocument &document, StrokeWidthPlan const &plan)
{
    for (auto const &member : plan.preserved_clones) {
        SPItem *owner = member.target.owner.get();
        if (!owner || owner->document != &document || !owner->getRepr()) return false;
        if (owner->parent != member.original_parent.get()) return false;
        Geom::Affine const affine = owner->i2doc_affine();
        for (unsigned i = 0; i < 6; ++i) {
            // An unchanged malformed (NaN) component is unchanged, as for excluded records.
            if (!same_double_frozen(affine[i], member.i2doc_affine[i])) return false;
        }
        if (non_style_attributes_of(owner) != member.non_style_attributes) return false;
        if (inline_style_of(owner) != member.inline_style) return false;
    }
    return true;
}

/**
 * What a source write legitimately does to the selected clones' query records. A clone that
 * follows an UNSET source width is an eligible (or protected/invalid) record; once the
 * source gets its own width it is `CloneSourceOverrides`, by the same rule the query applies.
 * A record may change class only when ALL hold: the clone is in `plan.following`; its frozen
 * record is not one of the classes that outrank the source check (missing source, unsupported
 * child, already overriding); its `clone_source` is a shape this plan changes; and the LIVE
 * source now overrides the width. Every other record must be unchanged. The clones' own
 * attributes and inline style are verified by the member checks as before.
 */
struct CloneExpectation {
    std::unordered_set<SPItem *> following;
    /// Clones expected to be `Unavailable/CloneSourceOverrides` after the write, with the
    /// ORIGINAL source of their frozen record: the fresh record must name the same source.
    std::unordered_map<SPItem *, SPItem *> flipped_source;
    std::unordered_set<SPItem *> flipped;
    std::ptrdiff_t eligible = 0;
    std::ptrdiff_t incompatible = 0;
    std::ptrdiff_t unavailable = 0;
    std::ptrdiff_t covered = 0;
    std::ptrdiff_t missing_sources = 0;
};

CloneExpectation expect_clone_classes(StrokeWidthPlan const &plan)
{
    CloneExpectation expectation;
    for (auto const &weak : plan.following) {
        if (auto *clone = weak.get()) expectation.following.insert(clone);
    }
    if (expectation.following.empty()) return expectation;

    std::unordered_set<SPItem *> changed_shapes;
    for (auto const &member : plan.members) {
        if (member.target.kind == StrokeWidthTargetKind::Shape &&
            member.outcome == StrokeWidthMemberOutcome::Change && member.target.owner.get()) {
            changed_shapes.insert(member.target.owner.get());
        }
    }
    if (changed_shapes.empty()) return expectation;

    // One primary record per clone (an eligible target or a classified exclusion).
    auto const consider = [&](StrokeWidthTarget const &target, bool eligible_record) {
        if (target.kind != StrokeWidthTargetKind::CloneInstance ||
            target.eligibility == StrokeWidthEligibility::Covered) {
            return;
        }
        SPItem *clone = target.owner.get();
        if (!clone || !expectation.following.count(clone)) return;
        if (target.exclusion == StrokeWidthExclusion::MissingSource ||
            target.exclusion == StrokeWidthExclusion::CloneUnsupportedChild ||
            target.exclusion == StrokeWidthExclusion::CloneSourceOverrides) {
            return;
        }
        SPItem *source = target.clone_source.get();
        if (!source || !changed_shapes.count(source) || !source_overrides_width(source)) return;
        expectation.flipped.insert(clone);
        expectation.flipped_source[clone] = source;
        if (eligible_record) {
            --expectation.eligible;
        } else {
            switch (target.eligibility) {
            case StrokeWidthEligibility::Incompatible: --expectation.incompatible; break;
            case StrokeWidthEligibility::Unavailable: --expectation.unavailable; break;
            case StrokeWidthEligibility::Covered: break;
            case StrokeWidthEligibility::MissingSource: --expectation.missing_sources; break;
            case StrokeWidthEligibility::Eligible: break;
            }
        }
        ++expectation.unavailable;
    };
    for (auto const &target : plan.query.targets) consider(target, true);
    for (auto const &target : plan.query.excluded) consider(target, false);
    // A clone selected more than once has one Covered duplicate per repeat while it is
    // eligible. Once it is not, the query records no duplicate at all: they disappear.
    for (auto const &target : plan.query.excluded) {
        if (target.kind == StrokeWidthTargetKind::CloneInstance &&
            target.eligibility == StrokeWidthEligibility::Covered && expectation.flipped.count(target.owner.get())) {
            --expectation.covered;
        }
    }
    return expectation;
}

/// The fresh post-write query against the frozen one with `expectation` applied: aggregate
/// counts, target order/identity (`same_target` compares each surviving pair), and every
/// excluded record exactly, except that each flipped clone must appear once as a bare
/// `Unavailable/CloneSourceOverrides` record of its own source. Nothing else may differ.
bool query_matches_frozen(StrokeWidthPlan const &plan, StrokeWidthResult const &fresh,
                          CloneExpectation const &expectation,
                          std::function<bool(StrokeWidthTarget const &, StrokeWidthTarget const &)> const &same_target)
{
    auto const equal = [](std::size_t live, std::size_t frozen, std::ptrdiff_t delta) {
        return static_cast<std::ptrdiff_t>(live) == static_cast<std::ptrdiff_t>(frozen) + delta;
    };
    auto const &frozen = plan.query;
    if (!equal(fresh.eligible, frozen.eligible, expectation.eligible) ||
        !equal(fresh.incompatible, frozen.incompatible, expectation.incompatible) ||
        !equal(fresh.unavailable, frozen.unavailable, expectation.unavailable) ||
        !equal(fresh.covered, frozen.covered, expectation.covered) ||
        !equal(fresh.missing_sources, frozen.missing_sources, expectation.missing_sources)) {
        return false;
    }

    std::vector<StrokeWidthTarget const *> frozen_targets;
    for (auto const &target : frozen.targets) {
        if (!expectation.flipped.count(target.owner.get())) frozen_targets.push_back(&target);
    }
    if (fresh.targets.size() != frozen_targets.size()) return false;
    for (std::size_t i = 0; i < fresh.targets.size(); ++i) {
        if (expectation.flipped.count(fresh.targets[i].owner.get())) return false;
        if (!same_target(fresh.targets[i], *frozen_targets[i])) return false;
    }

    std::vector<StrokeWidthTarget const *> frozen_excluded;
    for (auto const &target : frozen.excluded) {
        if (!expectation.flipped.count(target.owner.get())) frozen_excluded.push_back(&target);
    }
    std::size_t flipped_seen = 0;
    std::size_t next = 0;
    for (auto const &target : fresh.excluded) {
        if (expectation.flipped.count(target.owner.get())) {
            if (target.kind != StrokeWidthTargetKind::CloneInstance || !target.runs.empty() ||
                target.eligibility != StrokeWidthEligibility::Unavailable ||
                target.exclusion != StrokeWidthExclusion::CloneSourceOverrides ||
                !target.clone_source.get() ||
                target.clone_source.get() != expectation.flipped_source.at(target.owner.get())) {
                return false;
            }
            ++flipped_seen;
            continue;
        }
        if (next >= frozen_excluded.size() || !same_target_frozen(target, *frozen_excluded[next])) return false;
        ++next;
    }
    return next == frozen_excluded.size() && flipped_seen == expectation.flipped.size();
}

/// A caller-supplied live-scope predicate (empty: always live).
bool scope_still_live(std::function<bool()> const &live_scope)
{
    return !live_scope || live_scope();
}

/// Re-derive the frozen plan read-only. `roots` must be the resolved live roots.
bool reprepare_matches(SPDocument &document, StrokeWidthPlan const &plan,
                       std::vector<SPItem *> const &roots)
{
    StrokeWidthPlan fresh = plan.combined_text_scope
        ? prepare_stroke_widths_combined(document, *plan.text_scope, roots, plan.intent,
                                         plan.scope_generation)
        : plan.text_scope
            ? prepare_stroke_widths(document, *plan.text_scope, roots, plan.intent,
                                    plan.scope_generation)
            : prepare_stroke_widths(document, roots, plan.intent, plan.scope_generation);
    return same_plan_frozen(plan, fresh);
}

/**
 * Shared caller-identity/scope preconditions for a bounded apply. The check
 * order matches the original shape writer exactly; on success `roots` receives
 * the resolved live roots. On failure `reason` is the same reason the shape
 * writer returned. No document state is mutated and no layout is refreshed.
 */
bool validate_apply_preconditions(SPDocument &document, StrokeWidthPlan const &plan,
                                  std::uint64_t current_scope_generation,
                                  DocumentUndo::RollbackableInteraction &interaction,
                                  std::vector<SPItem *> &roots, StrokeWidthMemberReason &reason)
{
    // Identity validation FIRST, before any weak root or live document access.
    if (!interaction.validFor(&document)) {
        reason = StrokeWidthMemberReason::InvalidTransaction;
        return false;
    }
    if (plan.state != StrokeWidthPlanState::Prepared) {
        reason = plan.rejection;
        return false;
    }
    if (current_scope_generation != plan.scope_generation) {
        reason = StrokeWidthMemberReason::StalePlan;
        return false;
    }
    if (plan.document_root.get() != document.getRoot()) {
        reason = StrokeWidthMemberReason::StalePlan;
        return false;
    }
    for (auto const &weak : plan.roots) {
        auto *root = weak.get();
        if (!root || root->document != &document) {
            reason = StrokeWidthMemberReason::StalePlan;
            return false;
        }
        roots.push_back(root);
    }
    if (plan.text_scope) {
        auto *owner = plan.text_scope->owner.get();
        if (!owner || owner->document != &document) {
            reason = StrokeWidthMemberReason::StalePlan;
            return false;
        }
    }
    return true;
}

void set_number_property(SPCSSAttr *css, char const *name, double value, bool important)
{
    char buffer[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_dtostr(buffer, sizeof(buffer), value);
    std::string text(buffer);
    if (important) text += " !important";
    sp_repr_css_set_property(css, name, text.c_str());
}

void set_string_property(SPCSSAttr *css, char const *name, std::string const &value, bool important)
{
    std::string text = value;
    if (important) text += " !important";
    sp_repr_css_set_property(css, name, text.c_str());
}

/// Unrelated inline declarations of the original style must survive a changed
/// member's native merge byte-for-value; only the actually patched property
/// names are excluded. The native libcroco-backed SPCSSAttr map is the only CSS
/// parser used; no second declaration engine is introduced.
bool unrelated_declarations_preserved(SPItem *owner, std::optional<std::string> const &original,
                                      std::vector<std::string> const &patched)
{
    if (!owner || !owner->getRepr()) return false;

    auto before = Util::delete_with<sp_repr_css_attr_unref>(sp_repr_css_attr_new());
    if (original && !original->empty()) {
        sp_repr_css_attr_add_from_string(before.get(), original->c_str());
    }
    auto current = Util::delete_with<sp_repr_css_attr_unref>(sp_repr_css_attr(owner->getRepr(), "style"));

    auto collect = [&patched](SPCSSAttr *css) {
        std::map<std::string, std::string> out;
        for (auto const &attr : css->attributeList()) {
            char const *name = g_quark_to_string(attr.key);
            if (!name || !attr.value) continue;
            bool skip = false;
            for (auto const &property : patched) {
                if (g_ascii_strcasecmp(name, property.c_str()) == 0) {
                    skip = true;
                    break;
                }
            }
            if (!skip) out.emplace(name, static_cast<char const *>(attr.value));
        }
        return out;
    };

    return collect(before.get()) == collect(current.get());
}

/// Shared member binding/affine/authored-attribute invariants for the postwrite
/// verifiers. Returns false on any dead/wrong-document/rebound/transformed member
/// or changed non-style authored attributes. The owner inline `style` is
/// deliberately not compared here: the text verifier must tolerate native span
/// rewriting normalizing it, and the shape/text verifiers compare it explicitly
/// where their contracts require it.
bool verify_member_bindings(SPDocument &document, StrokeWidthPlan const &plan)
{
    for (auto const &member : plan.members) {
        SPItem *owner = member.target.owner.get();
        if (!owner || owner->document != &document || !owner->getRepr()) return false;
        if (owner->parent != member.original_parent.get()) return false;

        Geom::Affine const affine = owner->i2doc_affine();
        for (unsigned i = 0; i < 6; ++i) {
            if (affine[i] != member.i2doc_affine[i]) return false;
        }
        if (non_style_attributes_of(owner) != member.non_style_attributes) return false;
    }
    return true;
}

bool verify_changed_member(SPItem *owner, StrokeWidthMemberPlan const &member)
{
    if (!owner || !owner->style || !member.local_width) return false;
    double const local = *member.local_width;
    if (!std::isfinite(local)) return false;

    SPStyle const &style = *owner->style;
    auto const &frozen = member.target.runs[0].style;
    auto const current = snapshot_style(style, member.target.transform_scale);
    if (member.intent_kind == StrokeWidthIntentKind::RemoveStroke) {
        return current.paint_none &&
               current.paint_important == frozen.paint_important &&
               current.paint_style_src == static_cast<unsigned char>(SPStyleSrc::STYLE_PROP) &&
               current.local_computed == frozen.local_computed &&
               current.convention == frozen.convention &&
               same_vector_frozen(current.dash_computed, frozen.dash_computed) &&
               current.dash_offset_computed == frozen.dash_offset_computed &&
               current.vector_effect_value == frozen.vector_effect_value &&
               unrelated_declarations_preserved(owner, member.inline_style, {"stroke"});
    }
    if (member.intent_kind == StrokeWidthIntentKind::Hairline) {
        auto const &expected_dash = member.native_dasharray_computed
                                        ? *member.native_dasharray_computed : frozen.dash_computed;
        std::vector<std::string> patched{"stroke-width", "vector-effect", "-inkscape-stroke"};
        if (member.native_dasharray_css) patched.emplace_back("stroke-dasharray");
        if (member.local_dashoffset) patched.emplace_back("stroke-dashoffset");
        return current.convention == StrokeWidthConvention::Hairline &&
               current.local_computed == 1.0 && current.non_scaling &&
               current.width_important == frozen.width_important &&
               current.hairline_important == frozen.hairline_important &&
               current.vector_effect_important == frozen.vector_effect_important &&
               current.paint_none == frozen.paint_none &&
               same_vector_frozen(current.dash_computed, expected_dash) &&
               current.dash_offset_computed ==
                   member.local_dashoffset.value_or(frozen.dash_offset_computed) &&
               unrelated_declarations_preserved(owner, member.inline_style, patched);
    }
    // Exact intended local double: no tolerance that could equate a tiny
    // requested width to zero.
    if (style.stroke_width.computed != local) return false;
    if (!style.stroke_width.set || style.stroke_width.inherit) return false;
    if (style.stroke_width.style_src != SPStyleSrc::STYLE_PROP) return false;
    if (style.stroke_width.important != member.width_important) return false;

    if (current.convention != (frozen.convention == StrokeWidthConvention::Hairline
                                   ? StrokeWidthConvention::Ordinary : frozen.convention)) return false;
    if (current.paint_none != frozen.paint_none) return false;
    if (current.non_scaling !=
        (frozen.convention == StrokeWidthConvention::Hairline ? false : frozen.non_scaling)) return false;
    if (frozen.convention == StrokeWidthConvention::Hairline &&
        current.vector_effect_value != "none") return false;

    bool const non_scaling = current.convention == StrokeWidthConvention::NonScaling;
    double const effective = non_scaling ? style.stroke_width.computed
                                         : style.stroke_width.computed * member.target.transform_scale;
    double const intended_effective =
        non_scaling ? local : local * member.target.transform_scale;
    if (!std::isfinite(effective) || effective != intended_effective) return false;

    bool const array_patched = member.local_dasharray.has_value();
    if (array_patched) {
        if (!member.native_dasharray_computed) return false;
        if (!exact_vector(style.stroke_dasharray.get_computed(), *member.native_dasharray_computed)) {
            return false;
        }
        if (style.stroke_dasharray.style_src != SPStyleSrc::STYLE_PROP) return false;
        if (!style.stroke_dasharray.set) return false;
        if (style.stroke_dasharray.important != member.dasharray_important) return false;
    } else {
        if (!same_vector_frozen(style.stroke_dasharray.get_computed(), frozen.dash_computed)) return false;
        if (style.stroke_dasharray.set != frozen.dash_set) return false;
        if (static_cast<unsigned char>(style.stroke_dasharray.style_src) != frozen.dash_style_src) {
            return false;
        }
        if (style.stroke_dasharray.important != frozen.dasharray_important) return false;
    }

    bool const offset_patched = member.local_dashoffset.has_value();
    if (offset_patched) {
        if (style.stroke_dashoffset.computed != *member.local_dashoffset) return false;
        if (style.stroke_dashoffset.style_src != SPStyleSrc::STYLE_PROP) return false;
        if (!style.stroke_dashoffset.set) return false;
        if (style.stroke_dashoffset.important != member.dashoffset_important) return false;
    } else {
        if (!same_double_frozen(style.stroke_dashoffset.computed, frozen.dash_offset_computed)) return false;
        if (static_cast<unsigned char>(style.stroke_dashoffset.style_src) != frozen.dashoffset_style_src) {
            return false;
        }
        if (style.stroke_dashoffset.important != frozen.dashoffset_important) return false;
    }

    std::vector<std::string> patched{"stroke-width"};
    if (frozen.convention == StrokeWidthConvention::Hairline) {
        patched.emplace_back("-inkscape-stroke");
        patched.emplace_back("vector-effect");
    }
    if (array_patched) patched.emplace_back("stroke-dasharray");
    if (offset_patched) patched.emplace_back("stroke-dashoffset");
    return unrelated_declarations_preserved(owner, member.inline_style, patched);
}

/// Read-only intended-output verification. Assumes the caller has already
/// validated the token/document/scope identity. It never reprepares against old
/// widths and never writes.
bool verify_intended_output(SPDocument &document, StrokeWidthPlan const &plan)
{
    if (!verify_member_bindings(document, plan) || !verify_preserved_clones(document, plan)) return false;

    for (auto const &member : plan.members) {
        SPItem *owner = member.target.owner.get();

        switch (member.outcome) {
        case StrokeWidthMemberOutcome::Change:
            if (member.target.runs.size() != 1) return false;
            if (!verify_changed_member(owner, member)) return false;
            break;
        case StrokeWidthMemberOutcome::Unchanged:
        case StrokeWidthMemberOutcome::Excluded:
            if (inline_style_of(owner) != member.inline_style) return false;
            break;
        }
    }

    std::vector<SPItem *> roots;
    for (auto const &weak : plan.roots) {
        auto *root = weak.get();
        if (!root || root->document != &document) return false;
        roots.push_back(root);
    }
    StrokeWidthResult fresh = plan.text_scope
        ? query_stroke_widths(document, *plan.text_scope, roots)
        : query_stroke_widths(document, roots);

    if (fresh.range_rejected != plan.query.range_rejected ||
        fresh.range_exclusion != plan.query.range_exclusion) {
        return false;
    }
    // Target order/identity/scope must match the prepared query. Every target
    // metadata field and run structural scope is compared; the per-run style
    // payload is only compared for eligible members whose outcome is not Change
    // (a changed shape legitimately has a different intended run style), and
    // never for a clone that follows a changed source. Selected clones of an unset
    // source may only move to CloneSourceOverrides (see `expect_clone_classes`).
    // Query-excluded records keep full frozen target/style equality. A Covered
    // record is a duplicate of an eligible owner and legitimately carries no runs,
    // so its full equality stays meaningful even when that owner changed.
    std::unordered_set<SPItem *> changed_owners;
    for (auto const &member : plan.members) {
        if (member.outcome == StrokeWidthMemberOutcome::Change && member.target.owner.get()) {
            changed_owners.insert(member.target.owner.get());
        }
    }
    auto const clones = expect_clone_classes(plan);
    if (!query_matches_frozen(plan, fresh, clones,
            [&](StrokeWidthTarget const &fresh_target, StrokeWidthTarget const &frozen_target) {
                if (!same_target_scope(fresh_target, frozen_target)) return false;
                SPItem *owner = fresh_target.owner.get();
                if (changed_owners.count(owner) || clones.following.count(owner)) return true;
                return same_runs_frozen(fresh_target.runs, frozen_target.runs);
            })) {
        return false;
    }

    // Re-evaluate the read-only dependency guard against the original changed
    // owner set: a post-write callback must not silently bind another selected
    // clone to a changed owner (or leave its source graph uncertain).
    if (!dependency_as_planned(plan, document)) {
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// SW3-B1 text writer verification.
//
// Native span rewriting legitimately changes style-source identity and run
// grouping, so verification is by rendered per-character relevant stroke style
// and whole-owner semantic content/count, never by a retained SPString,
// SPStyle pointer or layout iterator.
// ---------------------------------------------------------------------------

/// Dash/paint/vector-effect convention equality. Width and width priority are
/// deliberately excluded: a changed run is allowed to move them, and they are
/// checked separately against the intended value.
bool same_preserved_stroke(StrokeWidthStyle const &a, StrokeWidthStyle const &b)
{
    if (a.dash_set != b.dash_set || !same_vector_frozen(a.dash_computed, b.dash_computed)) return false;
    if (!same_double_frozen(a.dash_offset_computed, b.dash_offset_computed)) return false;
    if (a.dasharray_important != b.dasharray_important || a.dashoffset_important != b.dashoffset_important) {
        return false;
    }
    if (a.paint_none != b.paint_none) return false;
    if (a.convention != b.convention || a.non_scaling != b.non_scaling) return false;
    if (a.vector_effect_important != b.vector_effect_important) return false;
    // Compare the full native vector-effect payload unconditionally, not only
    // when the frozen run authored one: a bit newly added to a frozen-unset
    // glyph (`fixed-position`/`non-rotation`) is a real rendered change. Both
    // sides come from `snapshot_style`, whose
    // `SPStyle::vector_effect.get_value()` normalizes an ordinary default to
    // "none" (and an inherit state to "inherit"), so an unchanged default
    // compares equal. `vector_effect_set`/`_inherit` need no separate term: the
    // value already carries every rendered distinction, so this adds no raw CSS
    // string and no broader normalization.
    if (a.vector_effect_value != b.vector_effect_value) return false;
    return true;
}

/// Untouched-run equality: the preserved fields plus the frozen width and
/// width priority.
bool same_untouched_stroke(StrokeWidthStyle const &a, StrokeWidthStyle const &b)
{
    if (!same_double_frozen(a.local_computed, b.local_computed)) return false;
    if (a.width_important != b.width_important) return false;
    return same_preserved_stroke(a, b);
}

/// Owner-level outcome of one TextOwner member. The plan deliberately keeps the
/// member itself Excluded/TextAdapterPending, so the writable outcome is derived
/// from the prospective per-run records: any eligible `Change` run makes the
/// owner changed; otherwise an owner whose eligible runs are all genuine no-ops
/// is unchanged; anything else (only ineligible/excluded runs, or a genuine
/// eligible exclusion) is excluded.
StrokeWidthMemberOutcome text_owner_outcome(StrokeWidthMemberPlan const &member)
{
    bool any_unchanged = false;
    bool any_excluded = false;
    for (auto const &run : member.text_runs) {
        if (run.eligibility != StrokeWidthEligibility::Eligible) {
            any_excluded = true;
            continue;
        }
        switch (run.outcome) {
        case StrokeWidthMemberOutcome::Change:
            return StrokeWidthMemberOutcome::Change;
        case StrokeWidthMemberOutcome::Unchanged:
            any_unchanged = true;
            break;
        case StrokeWidthMemberOutcome::Excluded:
            any_excluded = true;
            break;
        }
    }
    return any_unchanged && !any_excluded ? StrokeWidthMemberOutcome::Unchanged
                                          : StrokeWidthMemberOutcome::Excluded;
}

/// Count the TextOwner members by owner-level writable outcome. `plan.unchanged`
/// and `plan.excluded` cannot be used directly for text members because the plan
/// keeps every TextOwner member Excluded/TextAdapterPending even when its runs
/// are real changes or genuine no-ops.
void count_text_owner_outcomes(StrokeWidthPlan const &plan, std::size_t &text_changed,
                               std::size_t &text_unchanged)
{
    text_changed = 0;
    text_unchanged = 0;
    for (auto const &member : plan.members) {
        if (member.target.kind != StrokeWidthTargetKind::TextOwner) continue;
        switch (text_owner_outcome(member)) {
        case StrokeWidthMemberOutcome::Change:
            ++text_changed;
            break;
        case StrokeWidthMemberOutcome::Unchanged:
            ++text_unchanged;
            break;
        case StrokeWidthMemberOutcome::Excluded:
            break;
        }
    }
}

/**
 * Structural gate for one TextOwner member's prospective runs. A changed run
 * must carry an intended width and its canonical dash patch. Shared by the text-only writer and the
 * compatible writer; empty/glyph-only range validation still needs a live layout
 * and is done before the first write.
 */
bool text_runs_supported(StrokeWidthMemberPlan const &member, StrokeWidthMemberReason &reason)
{
    for (auto const &run : member.text_runs) {
        if (run.eligibility != StrokeWidthEligibility::Eligible ||
            run.outcome == StrokeWidthMemberOutcome::Excluded) continue;
        if (run.local_dasharray && (!run.native_dasharray_css || !run.native_dasharray_computed)) {
            reason = StrokeWidthMemberReason::UnsupportedIntent;
            return false;
        }
        if (run.outcome == StrokeWidthMemberOutcome::Change && !run.local_width.has_value()) {
            reason = StrokeWidthMemberReason::UnsupportedIntent;
            return false;
        }
    }
    return true;
}

/**
 * Whole-plan structural gate for the text-only writer. Rejects any non-text
 * `Change` in this text-only entry point, and for every planned TextOwner member
 * rejects an eligible `Change` run without an intended width or canonical dash patch. Empty/
 * glyph-only range validation needs a live layout and is done separately before
 * the first write.
 */
bool text_plan_supported(StrokeWidthPlan const &plan, StrokeWidthMemberReason &reason)
{
    for (auto const &member : plan.members) {
        if (member.outcome == StrokeWidthMemberOutcome::Change &&
            member.target.kind != StrokeWidthTargetKind::TextOwner) {
            reason = StrokeWidthMemberReason::UnsupportedIntent;
            return false;
        }
    }
    for (auto const &member : plan.members) {
        if (member.target.kind != StrokeWidthTargetKind::TextOwner) continue;
        if (!text_runs_supported(member, reason)) return false;
    }
    return true;
}

/// One TextOwner member's pending native writer. `runs` holds that owner's
/// eligible Change records strictly descending by logical start index. `member`
/// points into the frozen plan for the whole operation; weak owner lookup still
/// happens per call.
/// Consecutive runs (in write order) of one owner that one native range write covers: adjacent
/// in logical order with an identical patch (`TextRunCss` equal).
struct TextWriteGroup {
    std::size_t first_run = 0;
    std::size_t run_count = 0;
    unsigned first_char = 0;
    unsigned last_char = 0;
};

struct TextOwnerWritePlan {
    StrokeWidthMemberPlan const *member = nullptr;
    std::vector<StrokeWidthTextRunPlan const *> runs;
    /// The write units over `runs`, in write order (`runs` stays strictly descending).
    std::vector<TextWriteGroup> groups;
    /// Some character's width comes from a stylesheet: such an owner keeps one write per run.
    bool stylesheet_width = false;
};

/// Owners with at least this many write units use direct complete-span writes (no per-write XML
/// tidy and layout rebuild); smaller owners keep the native range write per unit unchanged.
std::size_t g_bulk_text_threshold = 16;

using RunCssPtr = decltype(Util::delete_with<sp_repr_css_attr_unref>(sp_repr_css_attr_new()));

/// The native CSS one text run writes, and the opt-in local-unit flags of the native range write.
struct TextRunCss {
    RunCssPtr css;
    TextStyleLocalStroke local_stroke;
};

TextRunCss build_text_run_css(StrokeWidthTextRunPlan const &run, bool range_adapter = true)
{
    TextRunCss out{Util::delete_with<sp_repr_css_attr_unref>(sp_repr_css_attr_new()), {}};
    auto &css = out.css;
    if (run.intent_kind == StrokeWidthIntentKind::RemoveStroke) {
        set_string_property(css.get(), "stroke", "none", run.style.paint_important);
    } else if (run.intent_kind == StrokeWidthIntentKind::Hairline) {
        set_number_property(css.get(), "stroke-width", 1.0, run.width_important);
        set_string_property(css.get(), "vector-effect", "non-scaling-stroke",
                            run.style.vector_effect_important);
        set_string_property(css.get(), "-inkscape-stroke", "hairline",
                            run.style.hairline_important);
        if (run.native_dasharray_css) {
            set_string_property(css.get(), "stroke-dasharray", *run.native_dasharray_css,
                                run.dasharray_important);
        }
        if (run.dashoffset_css) {
            set_string_property(css.get(), "stroke-dashoffset", *run.dashoffset_css, run.dashoffset_important);
        } else if (run.local_dashoffset) {
            set_number_property(css.get(), "stroke-dashoffset", *run.local_dashoffset,
                                run.dashoffset_important);
        }
    } else if (run.local_width) {
        if (run.width_css) {
            set_string_property(css.get(), "stroke-width", *run.width_css, run.width_important);
        } else {
            set_number_property(css.get(), "stroke-width", *run.local_width, run.width_important);
        }
        if (run.style.convention == StrokeWidthConvention::Hairline) {
            set_string_property(css.get(), "-inkscape-stroke", "none",
                                run.style.hairline_important);
            set_string_property(css.get(), "vector-effect", "none",
                                run.style.vector_effect_important);
        }
        if (run.native_dasharray_css) {
            set_string_property(css.get(), "stroke-dasharray", *run.native_dasharray_css,
                                run.dasharray_important);
        }
        if (run.dashoffset_css) {
            set_string_property(css.get(), "stroke-dashoffset", *run.dashoffset_css, run.dashoffset_important);
        } else if (run.local_dashoffset) {
            set_number_property(css.get(), "stroke-dashoffset", *run.local_dashoffset,
                                run.dashoffset_important);
        }
    }
    // Preserve an authored vector-effect (value and priority) on the changed run;
    // the engine does not infer this non-inheriting property itself. Only a
    // hairline converted to numeric width deliberately clears it above.
    if (range_adapter && run.style.vector_effect_set && run.intent_kind != StrokeWidthIntentKind::Hairline &&
        !(run.style.convention == StrokeWidthConvention::Hairline &&
          run.intent_kind != StrokeWidthIntentKind::RemoveStroke)) {
        set_string_property(css.get(), "vector-effect", run.style.vector_effect_value,
                            run.style.vector_effect_important);
    }
    out.local_stroke.stroke_width = run.intent_kind != StrokeWidthIntentKind::RemoveStroke;
    out.local_stroke.stroke_dasharray = run.native_dasharray_css.has_value();
    out.local_stroke.stroke_dashoffset = run.local_dashoffset.has_value();
    out.local_stroke.exclude_end_line_break = true;
    return out;
}

/// Two runs are written by exactly the same native call (same declarations, priorities and
/// local-unit flags), so one range write over both is equivalent to two.
bool same_text_run_write(StrokeWidthTextRunPlan const &a, StrokeWidthTextRunPlan const &b)
{
    if (a.intent_kind != b.intent_kind) return false;
    auto const x = build_text_run_css(a);
    auto const y = build_text_run_css(b);
    if (x.local_stroke.stroke_width != y.local_stroke.stroke_width ||
        x.local_stroke.stroke_dasharray != y.local_stroke.stroke_dasharray ||
        x.local_stroke.stroke_dashoffset != y.local_stroke.stroke_dashoffset) {
        return false;
    }
    std::map<std::string, std::string> left;
    std::map<std::string, std::string> right;
    for (auto const &attr : x.css->attributeList()) left[g_quark_to_string(attr.key)] = attr.value.pointer();
    for (auto const &attr : y.css->attributeList()) right[g_quark_to_string(attr.key)] = attr.value.pointer();
    return left == right;
}

/// Build the deterministic multi-owner write order: owners follow the frozen plan
/// member order; each owner's changed runs are strictly descending by logical
/// start index (a later native call may split/reparent spans, so earlier indices
/// are written last). Owners with no eligible Change run are omitted.
std::vector<TextOwnerWritePlan> collect_text_owner_writes(StrokeWidthPlan const &plan)
{
    std::vector<TextOwnerWritePlan> owners;
    for (auto const &member : plan.members) {
        if (member.target.kind != StrokeWidthTargetKind::TextOwner) continue;
        TextOwnerWritePlan write;
        write.member = &member;
        std::unordered_set<SPObject *> written_sources;
        for (auto const &run : member.text_runs) {
            if (run.eligibility == StrokeWidthEligibility::Eligible &&
                run.outcome == StrokeWidthMemberOutcome::Change) {
                if (member.target.whole_object && !written_sources.insert(run.style_source.get()).second) continue;
                write.runs.push_back(&run);
            }
        }
        if (write.runs.empty()) continue;
        std::sort(write.runs.begin(), write.runs.end(),
                  [](StrokeWidthTextRunPlan const *a, StrokeWidthTextRunPlan const *b) {
                      if (a->first_char != b->first_char) return a->first_char > b->first_char;
                      return a->last_char > b->last_char;
                  });
        write.stylesheet_width = std::any_of(member.target.char_baseline.begin(),
            member.target.char_baseline.end(), [](auto const &character) {
                return static_cast<SPStyleSrc>(character.style.width_style_src) == SPStyleSrc::STYLE_SHEET;
            });
        // One write unit per run, except that a run directly adjacent (in logical order) to the
        // next lower one with the identical write is covered by the same range write. Never for
        // an owner with a stylesheet width: its complete spans are written one by one.
        bool const native_lossless = member.target.whole_object || lossless_native_text(member.target.owner.get());
        for (std::size_t index = 0; index < write.runs.size(); ++index) {
            auto const *run = write.runs[index];
            if (!member.target.whole_object && !write.groups.empty() && !write.stylesheet_width &&
                native_lossless) {
                auto &group = write.groups.back();
                auto const *previous = write.runs[group.first_run + group.run_count - 1];
                if (run->last_char == previous->first_char && same_text_run_write(*run, *previous)) {
                    ++group.run_count;
                    group.first_char = run->first_char;
                    continue;
                }
            }
            write.groups.push_back({index, 1, run->first_char, run->last_char});
        }
        owners.push_back(std::move(write));
    }
    return owners;
}

/**
 * Read-only whole-plan preflight before the first native text write. Validates
 * every member's owner binding, original parent, exact i2doc affine and authored
 * non-style attributes, and for every TextOwner member with a changed run further
 * validates document-root descent, protection, explicit scope identity, a live
 * layout, the live logical count against the frozen count, every changed run as a
 * non-empty glyph-only half-open range, and the complete immutable whole-owner
 * baseline. Returns None on success and never mutates the document.
 */
StrokeWidthMemberReason preflight_text_plan(SPDocument &document, StrokeWidthPlan const &plan)
{
    for (auto const &member : plan.members) {
        SPItem *owner = member.target.owner.get();
        if (!owner || owner->document != &document || !owner->getRepr()) {
            return StrokeWidthMemberReason::StalePlan;
        }
        if (owner->parent != member.original_parent.get()) {
            return StrokeWidthMemberReason::StalePlan;
        }
        Geom::Affine const affine = owner->i2doc_affine();
        for (unsigned i = 0; i < 6; ++i) {
            if (affine[i] != member.i2doc_affine[i]) {
                return StrokeWidthMemberReason::StalePlan;
            }
        }
        if (non_style_attributes_of(owner) != member.non_style_attributes) {
            return StrokeWidthMemberReason::StalePlan;
        }
    }

    // Every planned TextOwner, changed or not, is verified post-write by
    // `verify_text_owner_output`, which requires the frozen whole-owner content
    // to be available and the frozen baseline to cover the frozen count. Enforce
    // that before any native write rather than discovering a missing snapshot
    // only after a write has landed.
    for (auto const &member : plan.members) {
        if (member.target.kind != StrokeWidthTargetKind::TextOwner) continue;
        if (!member.target.text_content_available) return StrokeWidthMemberReason::StalePlan;
        if (member.target.char_baseline.size() != member.target.char_count) {
            return StrokeWidthMemberReason::StalePlan;
        }
    }

    for (auto const &member : plan.members) {
        if (member.target.kind != StrokeWidthTargetKind::TextOwner) continue;
        bool has_change = false;
        for (auto const &run : member.text_runs) {
            if (run.eligibility == StrokeWidthEligibility::Eligible &&
                run.outcome == StrokeWidthMemberOutcome::Change) {
                has_change = true;
                break;
            }
        }
        if (!has_change) continue;

        SPItem *owner = member.target.owner.get();
        if (!owned_by_document_root(owner, document, plan.document_root.get())) {
            return StrokeWidthMemberReason::StalePlan;
        }
        if (node_protection(owner, owner, nullptr) != ProtectionKind::None) {
            return StrokeWidthMemberReason::StalePlan;
        }
        if (plan.text_scope && !plan.combined_text_scope &&
            plan.text_scope->owner.get() != owner) {
            return StrokeWidthMemberReason::StalePlan;
        }

        auto const *layout = te_get_layout(owner);
        if (!layout) return StrokeWidthMemberReason::StalePlan;
        unsigned const count = static_cast<unsigned>(layout->iteratorToCharIndex(layout->end()));
        if (count != member.target.char_count) {
            return StrokeWidthMemberReason::StalePlan;
        }
        if (!restorable_text_positions(member)) return StrokeWidthMemberReason::UnrestorableTextPosition;
        LayoutSources const origins(*layout);
        for (auto const &run : member.text_runs) {
            if (run.eligibility != StrokeWidthEligibility::Eligible ||
                run.outcome != StrokeWidthMemberOutcome::Change) {
                continue;
            }
            if (run.first_char >= run.last_char || run.last_char > count) {
                return StrokeWidthMemberReason::UnsupportedIntent;
            }
            for (unsigned index = run.first_char; index < run.last_char; ++index) {
                SPObject *raw = nullptr;
                if (!authored_character(origins, index, &raw) || !raw) {
                    return StrokeWidthMemberReason::UnsupportedIntent;
                }
            }
        }
        if (member.target.char_baseline.size() != member.target.char_count) {
            return StrokeWidthMemberReason::StalePlan;
        }
    }
    return StrokeWidthMemberReason::None;
}

/**
 * Between native calls: interaction currency plus, for every run not yet written,
 * its owner's binding/protection/transform/authored attributes and the frozen
 * prewrite rendered run style. An observer that mutates the current owner or a
 * still-pending owner is refused before the next write. Rendered sources, not
 * pointer identity, are compared, so native span splitting with equal semantics
 * is tolerated. The caller has already validated the interaction, but this
 * re-reads no cached iterator.
 */
bool pending_text_runs_intact(SPDocument &document, StrokeWidthPlan const &plan,
                              std::vector<TextOwnerWritePlan> const &owners,
                              std::size_t current_owner, std::size_t current_run,
                              bool current_owner_only = false)
{
    for (std::size_t oi = current_owner; oi < owners.size(); ++oi) {
        if (current_owner_only && oi != current_owner) break;
        auto const &write = owners[oi];
        ++g_work.pending_owner_checks;
        StrokeWidthMemberPlan const &member = *write.member;
        SPItem *owner = member.target.owner.get();
        if (!owner || owner->document != &document || !owner->getRepr()) return false;
        if (owner->parent != member.original_parent.get()) return false;
        Geom::Affine const affine = owner->i2doc_affine();
        for (unsigned i = 0; i < 6; ++i) {
            if (affine[i] != member.i2doc_affine[i]) return false;
        }
        if (non_style_attributes_of(owner) != member.non_style_attributes) return false;
        if (!owned_by_document_root(owner, document, plan.document_root.get())) return false;
        if (node_protection(owner, owner, nullptr) != ProtectionKind::None) return false;
        if (plan.text_scope && !plan.combined_text_scope &&
            plan.text_scope->owner.get() != owner) return false;

        // Fresh layout and iterators for every check/call; nothing is retained.
        auto const *layout = te_get_layout(owner);
        if (!layout) return false;
        unsigned const count = static_cast<unsigned>(layout->iteratorToCharIndex(layout->end()));
        // A callback must not insert, delete or reorder same-style text in a
        // still-pending whole-owner scope and still reach another native write:
        // the live logical count and whole-owner semantic content must stay equal
        // to the frozen snapshot.
        if (count != member.target.char_count) return false;
        if (sp_te_get_string_multiline(owner).raw() != member.target.text_content) return false;

        LayoutSources const origins(*layout);
        for (std::size_t ri = 0; ri < write.runs.size(); ++ri) {
            // Runs already written by this operation are intentionally different
            // from the frozen prewrite style and are skipped.
            if (oi == current_owner && ri < current_run) continue;
            auto const *run = write.runs[ri];
            if (run->first_char >= run->last_char || run->last_char > count) return false;
            for (unsigned index = run->first_char; index < run->last_char; ++index) {
                StrokeWidthCharBaseline current;
                SPObject *source = nullptr;
                if (!capture_rendered_char(layout, origins, owner, index, member.target.transform_scale, current,
                                           &source)) {
                    return false;
                }
                if (!current.authored) return false;
                if (node_protection(source, owner, nullptr) != ProtectionKind::None) return false;
                if (!same_untouched_stroke(current.style, run->style)) return false;
            }
        }
    }
    return true;
}

auto style_declarations(char const *style)
{
    auto css = Util::delete_with<sp_repr_css_attr_unref>(sp_repr_css_attr_new());
    if (style) sp_repr_css_attr_add_from_string(css.get(), style);
    std::map<std::string, std::string> declarations;
    for (auto const &attr : css->attributeList()) declarations[g_quark_to_string(attr.key)] = attr.value.pointer();
    return declarations;
}

void capture_source_preservation(StrokeWidthMemberPlan &member)
{
    auto &target = member.target;
    std::unordered_map<SPObject *, std::vector<std::string>> changed_properties;
    for (auto const &run : member.text_runs) {
        if (run.outcome != StrokeWidthMemberOutcome::Change || changed_properties.count(run.style_source.get())) continue;
        auto const built = build_text_run_css(run, false);
        auto &properties = changed_properties[run.style_source.get()];
        for (auto const &attr : built.css->attributeList()) properties.emplace_back(g_quark_to_string(attr.key));
    }
    // Inline parsing is context-free. Exact CSS bytes share a parsed map, while
    // every source still freezes its own computed cascade. SPStyle has one fixed
    // native property order; reuse a mask only for the identical emitted key list.
    std::unordered_map<std::optional<std::string>, std::map<std::string, std::string>> declarations;
    std::map<std::vector<std::string>, std::vector<bool>> masks;
    std::function<void(SPObject *)> capture = [&](SPObject *source) {
        if (source->style && !is<SPString>(source)) {
            StrokeWidthSourceBaseline snapshot;
            snapshot.source = source;
            if (!target.whole_object && is<SPTSpan>(source)) {
                auto positions = text_positions(source);
                if (cast<SPTSpan>(source)->role == SP_TSPAN_ROLE_LINE ||
                    std::any_of(positions.begin(), positions.end(), [](auto const &v) { return v.has_value(); })) {
                    snapshot.positions = std::move(positions);
                }
            }
            if (auto const *style = source->getRepr()->attribute("style")) snapshot.inline_style = style;
            auto [parsed, new_css] = declarations.try_emplace(snapshot.inline_style);
            if (new_css) parsed->second = style_declarations(source->getRepr()->attribute("style"));
            snapshot.inline_declarations = parsed->second;
            snapshot.changed_properties = changed_properties[source];
            snapshot.computed_style = computed_declarations(*source->style);
            auto [mask, new_keys] = masks.try_emplace(snapshot.changed_properties);
            if (new_keys) for (auto const *property : source->style->properties()) {
                mask->second.push_back(std::find(snapshot.changed_properties.begin(),
                    snapshot.changed_properties.end(), property->name().raw()) != snapshot.changed_properties.end());
            }
            snapshot.computed_patched = mask->second;
            target.source_baseline.push_back(std::move(snapshot));
        }
        for (auto &child : source->children) capture(&child);
    };
    capture(target.owner.get());
    if (target.whole_object) target.authored_tree = authored_tree(target.owner.get(), target.source_baseline);
}

bool verify_source_declarations(StrokeWidthMemberPlan const &member)
{
    // This cache lives only for this read-only verification. Equal inline bytes
    // parse equally; computed cascade and the whitelist remain source-specific.
    std::unordered_map<std::optional<std::string>, std::map<std::string, std::string>> declarations;
    for (auto const &baseline : member.target.source_baseline) {
        auto *source = baseline.source.get();
        if (!source || !source->getRepr() || !source->style) return false;
        auto const *style = source->getRepr()->attribute("style");
        std::optional<std::string> const css = style ? std::optional<std::string>(style) : std::nullopt;
        auto [parsed, new_css] = declarations.try_emplace(css);
        if (new_css) parsed->second = style_declarations(style);
        auto const &after = parsed->second;
        auto const &changed = baseline.changed_properties;
        std::size_t unchanged = 0;
        for (auto const &[name, value] : baseline.inline_declarations) {
            if (std::find(changed.begin(), changed.end(), name) != changed.end()) continue;
            auto const found = after.find(name);
            if (found == after.end() || found->second != value) return false;
            ++unchanged;
        }
        std::size_t remaining = 0;
        for (auto const &[name, value] : after) {
            if (std::find(changed.begin(), changed.end(), name) == changed.end()) ++remaining;
        }
        if (unchanged != remaining || baseline.computed_patched.size() != source->style->properties().size()) return false;
        auto const computed_after = computed_declarations(*source->style, &baseline.computed_patched);
        if (baseline.computed_style.size() != computed_after.size()) return false;
        for (std::size_t i = 0; i < computed_after.size(); ++i) {
            if (i < baseline.computed_patched.size() && baseline.computed_patched[i]) continue;
            if (baseline.computed_style[i] != computed_after[i]) return false;
        }
    }
    return true;
}

/// Post-write intended-output verification of one TextOwner member. Never
/// reprepares and never writes. It uses the immutable whole-owner baseline frozen
/// in the plan at query time, so every glyph not covered by a frozen run is
/// proven unchanged through the same `nearest_style_source` rendering resolution
/// the query used.
bool verify_text_owner_output(SPDocument &document, StrokeWidthMemberPlan const &member)
{
    SPItem *owner = member.target.owner.get();
    if (!owner) return false;
    auto const *layout = te_get_layout(owner);
    if (!layout) return false;
    unsigned const count = static_cast<unsigned>(layout->iteratorToCharIndex(layout->end()));
    // Whole-owner semantic content and logical count are span-structure
    // independent; compare them exactly to the frozen snapshot.
    if (!member.target.text_content_available) return false;
    if (count != member.target.char_count) return false;
    if (sp_te_get_string_multiline(owner).raw() != member.target.text_content) return false;
    // The immutable whole-owner baseline must be complete before any covered or
    // uncovered glyph can be verified. A missing/short baseline is never treated
    // as "nothing to compare": reject safely instead.
    if (member.target.char_baseline.size() != count) return false;

    LayoutSources const origins(*layout);
    if (glyph_signature(*layout) != member.target.glyph_signature) return false;
    std::vector<SPObject *> rendered_sources(count, nullptr);
    for (unsigned index = 0; index < count; ++index) {
        auto const it = layout->charIndexToIterator(index);
        auto const anchor = layout->characterAnchorPoint(it);
        SPObject *raw = nullptr;
        auto const &baseline = member.target.char_baseline[index];
        if (authored_character(origins, index, &raw) != baseline.authored ||
            it.glyphIndex() != baseline.glyph_index ||
            std::array<double, 2>{anchor.x(), anchor.y()} != baseline.anchor) return false;
        rendered_sources[index] = nearest_style_source(raw, owner);
    }
    for (auto const &baseline : member.target.source_baseline) {
        if (baseline.positions && (!baseline.source || !owner->isAncestorOf(baseline.source.get()) ||
            text_positions(baseline.source.get()) != *baseline.positions)) return false;
    }
    if (member.target.whole_object &&
        (authored_tree(owner, member.target.source_baseline) != member.target.authored_tree ||
         !verify_source_declarations(member))) return false;

    std::vector<bool> covered(count, false);
    for (auto const &run : member.text_runs) {
        bool const changed = run.eligibility == StrokeWidthEligibility::Eligible &&
                             run.outcome == StrokeWidthMemberOutcome::Change;
        std::unordered_set<SPObject *> checked_sources;
        for (unsigned index = run.first_char; index < run.last_char; ++index) {
            if (index >= count) return false;
            covered[index] = true;
            if (!member.target.char_baseline[index].authored) return false;
            auto *rendered_source = rendered_sources[index];
            if (!rendered_source || !rendered_source->style) return false;
            if (!checked_sources.insert(rendered_source).second) continue;
            StrokeWidthStyle const &current = origins.style(rendered_source, member.target.transform_scale);
            if (changed) {
                if (!run.local_width) return false;
                if (run.intent_kind == StrokeWidthIntentKind::RemoveStroke) {
                    if (!current.paint_none ||
                        current.paint_important != run.style.paint_important ||
                        current.local_computed != run.style.local_computed ||
                        current.convention != run.style.convention ||
                        !same_vector_frozen(current.dash_computed, run.style.dash_computed) ||
                        current.dash_offset_computed != run.style.dash_offset_computed) return false;
                } else if (run.intent_kind == StrokeWidthIntentKind::Hairline) {
                    if (current.convention != StrokeWidthConvention::Hairline ||
                        current.local_computed != 1.0 || !current.non_scaling ||
                        current.width_important != run.width_important ||
                        current.paint_none != run.style.paint_none) return false;
                    auto const &expected_dash = run.native_dasharray_computed
                                                    ? *run.native_dasharray_computed
                                                    : run.style.dash_computed;
                    if (!same_vector_frozen(current.dash_computed, expected_dash) ||
                        current.dash_offset_computed !=
                            run.local_dashoffset.value_or(run.style.dash_offset_computed)) return false;
                } else {
                    if (current.local_computed != *run.local_width) return false;
                    if (current.width_important != run.width_important) return false;
                    auto const expected_convention = run.style.convention == StrokeWidthConvention::Hairline
                        ? StrokeWidthConvention::Ordinary : run.style.convention;
                    if (current.convention != expected_convention) return false;
                    if (current.non_scaling != (run.style.convention == StrokeWidthConvention::Hairline
                                                    ? false : run.style.non_scaling)) return false;
                    if (run.style.convention == StrokeWidthConvention::Hairline &&
                        current.vector_effect_value != "none") return false;
                    if (run.style.convention != StrokeWidthConvention::Hairline &&
                        current.vector_effect_value != run.style.vector_effect_value) return false;
                    if (current.paint_none != run.style.paint_none) return false;
                    auto const &expected_dash = run.native_dasharray_computed
                        ? *run.native_dasharray_computed : run.style.dash_computed;
                    if (!same_vector_frozen(current.dash_computed, expected_dash) ||
                        current.dash_offset_computed !=
                            run.local_dashoffset.value_or(run.style.dash_offset_computed)) return false;
                }
                // Dash priorities: a patched dash property carries the intended
                // priority, an unpatched one keeps its frozen priority. A rewrite that keeps
                // the numbers but drops or adds `!important` is a different result.
                if (current.dasharray_important !=
                        (run.native_dasharray_css ? run.dasharray_important : run.style.dasharray_important) ||
                    current.dashoffset_important !=
                        (run.local_dashoffset ? run.dashoffset_important : run.style.dashoffset_important)) {
                    return false;
                }
                // A callback after the last write must not protect the changed
                // glyph's rendered source (or an ancestor) and still be reported
                // as an unsafe success.
                if (node_protection(rendered_source, owner, nullptr) != ProtectionKind::None) {
                    return false;
                }
            } else if (!same_untouched_stroke(current, run.style)) {
                return false;
            }
        }
    }

    // Every glyph not covered by a frozen run is checked against the immutable
    // query-time baseline. A changed glyph is never required to equal the
    // baseline (it intentionally moved), so only uncovered indices are compared.
    for (unsigned index = 0; index < count; ++index) {
        if (covered[index]) continue;
        if (!member.target.char_baseline[index].authored) continue;
        auto *source = rendered_sources[index];
        if (!source || !source->style) return false;
        if (!same_untouched_stroke(origins.style(source, member.target.transform_scale), member.target.char_baseline[index].style)) {
            return false;
        }
    }
    return true;
}

/**
 * Post-write intended-output verification for the text-only writer across every
 * planned TextOwner member. Never reprepares (native span splitting legitimately
 * changes the plan) and never writes.
 */
bool verify_text_intended_output(SPDocument &document, StrokeWidthPlan const &plan)
{
    StrokeWidthMemberReason gate_reason = StrokeWidthMemberReason::None;
    if (!text_plan_supported(plan, gate_reason)) return false;

    // Owner binding/affine/non-style authored attributes are output invariants
    // for every member. The owner inline `style` is deliberately not compared:
    // native span rewriting may normalize it without changing rendered output.
    if (!verify_member_bindings(document, plan) || !verify_preserved_clones(document, plan)) return false;

    // Re-query the same original roots and optional text scope. Native span
    // splitting legitimately changes eligible text-run structure/style, so only
    // the frozen aggregate outcome and every excluded record are compared. An
    // excluded record that changed classification during a write (for example a
    // selected bitmap newly carrying `display:none`) must invalidate the whole
    // result before the caller commits it.
    std::vector<SPItem *> roots;
    for (auto const &weak : plan.roots) {
        auto *root = weak.get();
        if (!root || root->document != &document) return false;
        roots.push_back(root);
    }
    StrokeWidthResult const fresh = plan.text_scope
        ? query_stroke_widths(document, *plan.text_scope, roots)
        : query_stroke_widths(document, roots);
    if (fresh.range_rejected != plan.query.range_rejected ||
        fresh.range_exclusion != plan.query.range_exclusion) {
        return false;
    }
    if (fresh.eligible != plan.query.eligible || fresh.incompatible != plan.query.incompatible ||
        fresh.unavailable != plan.query.unavailable || fresh.covered != plan.query.covered ||
        fresh.missing_sources != plan.query.missing_sources) {
        return false;
    }
    if (fresh.excluded.size() != plan.query.excluded.size()) return false;
    for (std::size_t i = 0; i < fresh.excluded.size(); ++i) {
        if (!same_target_frozen(fresh.excluded[i], plan.query.excluded[i])) return false;
    }

    // Re-evaluate the same bounded live dependency guard the shape verifier uses:
    // a selected clone retargeted to a changed text owner during/between writes
    // (or left uncertain) must never be reported as ready. `dependency_decision`
    // already counts planned text-run changes as changed owners; only reported or
    // owned-descendant selected clones are inspected, never unselected ones.
    if (!dependency_as_planned(plan, document)) {
        return false;
    }

    for (auto const &member : plan.members) {
        if (member.target.kind != StrokeWidthTargetKind::TextOwner) continue;
        if (!verify_text_owner_output(document, member)) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// SW3-C compatible writer internals.
// ---------------------------------------------------------------------------

/// One native text-run write with fresh CSS and local-unit opt-in. Plain
/// whole-owner text and complete spans needing stylesheet preservation use
/// local CSS; other text retains native range writing and normalization. `watch`
/// (optional) is told which part of the tree this write may legitimately change; false is
/// returned when a direct write saw anything but its one expected style notification.
bool write_text_run(SPItem *owner, Text::Layout const *layout, StrokeWidthTextRunPlan const &run,
                    StrokeWidthMemberPlan const &member, PendingOwnerWatch *watch = nullptr)
{
    auto built = build_text_run_css(run);
    auto &css = built.css;
    auto const &local_stroke = built.local_stroke;
    // A plain <text> with only direct character data has no nested style to
    // protect. Apply a whole-owner width through the native item CSS operation:
    // sp_te_apply_style would create a tspan and serialize unrelated authored
    // presentation attributes on the owner (for example #111 -> #111111).
    // Keep the span writer for every other text structure or partial range.
    bool plain_whole_owner = is<SPText>(owner) && run.first_char == 0 &&
        run.last_char == static_cast<unsigned>(layout->iteratorToCharIndex(layout->end())) &&
        owner->firstChild();
    if (plain_whole_owner) {
        for (auto *child = owner->firstChild(); child; child = child->getNext()) {
            if (!is<SPString>(child)) {
                plain_whole_owner = false;
                break;
            }
        }
    }
    if (plain_whole_owner) {
        ++g_work.direct_source_writes;
        if (watch) watch->begin_style_write(owner->getRepr());
        owner->changeCSS(css.get(), "style");
        return !watch || watch->end_write();
    }
    // A whole plain span can use the same native local CSS write for every intent.
    // Avoid global text normalization here: it can unwrap an untouched span
    // whose width is supplied by a CSS class, losing that protected style.
    // A partial source always retains the native range/splitting path below.
    auto *source = run.style_source.get();
    if (source && source != owner &&
        source->firstChild() && (repr_name_is(source, "svg:tspan") || repr_name_is(source, "svg:flowSpan"))) {
        bool complete_plain_source = true;
        for (auto *child = source->firstChild(); child; child = child->getNext()) {
            if (!is<SPString>(child)) complete_plain_source = false;
        }
        unsigned const count = static_cast<unsigned>(layout->iteratorToCharIndex(layout->end()));
        for (unsigned index = 0; complete_plain_source && index < count; ++index) {
            SPObject *raw = nullptr;
            layout->getSourceOfCharacter(layout->charIndexToIterator(static_cast<int>(index)), &raw);
            bool const in_run = index >= run.first_char && index < run.last_char;
            if ((nearest_style_source(raw, owner) == source) != in_run) complete_plain_source = false;
        }
        // Native normalization must still merge identical authored spans.
        // Bypass it only when it could dissolve a stylesheet-width span,
        // including unchanged characters outside an explicit range. This
        // preservation decision is independent of the requested intent.
        bool const stylesheet_width = std::any_of(member.target.char_baseline.begin(),
            member.target.char_baseline.end(), [](auto const &character) {
                return static_cast<SPStyleSrc>(character.style.width_style_src) == SPStyleSrc::STYLE_SHEET;
            });
        // F1 exposes role=line ranges to native tidy. Preserve an authored
        // class on a line wrapper when a complete flat leaf can be written;
        // partial leaves still use the existing native splitter.
        bool class_line = false;
        for (auto *node = source; node && node != owner; node = node->parent) {
            auto *repr = node->getRepr();
            if (repr && repr->attribute("class") && repr->attribute("sodipodi:role") &&
                std::strcmp(repr->attribute("sodipodi:role"), "line") == 0) class_line = true;
        }
        if (complete_plain_source && (stylesheet_width || class_line || !lossless_native_text(owner))) {
            ++g_work.direct_source_writes;
            if (watch) watch->begin_style_write(source->getRepr());
            source->changeCSS(css.get(), "style");
            return !watch || watch->end_write();
        }
    }
    auto const owner_attributes = non_style_attributes_of(owner);
    ++g_work.native_range_writes;
    ++g_work.layout_rebuilds; // sp_te_apply_style rebuilds the layout before returning
    if (watch) watch->begin_subtree_write(owner->getRepr());
    sp_te_apply_style(owner, layout->charIndexToIterator(static_cast<int>(run.first_char)),
                      layout->charIndexToIterator(static_cast<int>(run.last_char)), css.get(),
                      TextStyleLocalSpacing{}, local_stroke);
    restore_reserialized_text_owner_attributes(owner, owner_attributes);
    bool const restored = restore_text_positions(member);
    if (watch) watch->end_write();
    return restored;
}

/// Whitespace that the layout collapses across span boundaries (no `xml:space="preserve"`, or an
/// `xml:space` set below the owner). The native range write re-tidies the spans between its writes
/// and the collapsed string can change. Partial ranges keep that native route;
/// proven whole owners preserve raw whitespace through their direct source writes.
bool has_collapsible_whitespace(SPObject *owner)
{
    if (!owner) return true;
    bool preserve = false;
    for (SPObject *o = owner; o; o = o->parent) {
        if (auto const *space = o->getRepr() ? o->getRepr()->attribute("xml:space") : nullptr) {
            preserve = std::strcmp(space, "preserve") == 0;
            break;
        }
    }
    std::function<bool(SPObject *)> below = [&](SPObject *node) {
        for (auto &child : node->children) {
            if (child.getRepr() && child.getRepr()->attribute("xml:space")) return true; // conservative
            if (auto const *string = cast<SPString>(&child)) {
                if (!preserve && string->string.raw().find_first_of(" \t\r\n") != std::string::npos) return true;
            } else if (below(&child)) {
                return true;
            }
        }
        return false;
    };
    return below(owner);
}

/// Per-owner facts for the direct complete-span writes of a large owner, built once from one
/// pass over the layout and rebuilt only after a native write (which may split/merge spans).
struct BulkOwnerState {
    bool ready = false;
    /// A change we did not make happened: the layout may describe an older tree, so refresh it
    /// before the map is rebuilt.
    bool layout_stale = false;
    std::vector<SPObject *> sources;
    std::unordered_map<SPObject *, unsigned> characters_of;

    /// Drop the map after a change we did not make (a callback may have split or merged spans).
    void invalidate_after_foreign_change()
    {
        ready = false;
        layout_stale = true;
    }

    void refresh(SPItem *owner)
    {
        if (layout_stale) {
            if (auto *text = cast<SPText>(owner)) text->rebuildLayout();
            else if (auto *flow = cast<SPFlowtext>(owner)) flow->rebuildLayout();
            ++g_work.layout_rebuilds;
            layout_stale = false;
        }
        auto const &layout = *te_get_layout(owner);
        unsigned const count = static_cast<unsigned>(layout.iteratorToCharIndex(layout.end()));
        LayoutSources const origins(layout);
        sources.assign(count, nullptr);
        characters_of.clear();
        for (unsigned index = 0; index < count; ++index) {
            SPObject *raw = nullptr;
            if (!authored_character(origins, index, &raw)) continue;
            sources[index] = nearest_style_source(raw, owner);
            ++characters_of[sources[index]];
        }
        g_work.source_lookups += count;
        ready = true;
    }

    /// The run is exactly one complete plain tspan/flowSpan (only character data inside, no
    /// authored position/rotation attributes of its own): the element the existing stylesheet-width
    /// branch writes directly.
    SPObject *direct_target(SPItem *owner, StrokeWidthTextRunPlan const &run,
                            bool whole, unsigned expected) const
    {
        auto *source = run.style_source.get();
        if (!ready || !source || (!whole && (source == owner || !flat_text_source(source, owner) ||
            !(repr_name_is(source, "svg:tspan") || repr_name_is(source, "svg:flowSpan"))))) {
            return nullptr;
        }
        if (run.first_char >= run.last_char || run.last_char > sources.size()) return nullptr;
        auto const found = characters_of.find(source);
        if (found == characters_of.end() || found->second != expected) return nullptr;
        g_work.source_lookups += run.last_char - run.first_char;
        for (unsigned index = run.first_char; index < run.last_char; ++index) {
            if (sources[index] != source) return nullptr;
        }
        return source;
    }
};

/**
 * Phase 1 of the text writers: every owner's changed runs, owners in plan order, write units
 * strictly descending by logical start index (a later native call may split/reparent spans, so
 * earlier indices are written last). Before each write the interaction and the caller's scope
 * are live, and every still-pending run keeps its frozen provenance and rendered style: the
 * full pending scan runs only when the watch saw a change it cannot attribute to our own
 * write (always before the first write), the current owner is re-checked after a native range
 * write that leaves more of its runs pending. Owners with many units write complete plain
 * spans directly. Returns false on any refusal; `written` counts the native calls.
 */
// Bound None must not round-trip unrelated declarations through the CSS parser:
// its normal/important passes reorder them. Last declaration at the frozen
// priority wins; the ordinary output verifier still proves the computed result.
void write_bound_remove(SPObject *source, SPCSSAttr *css)
{
    auto *repr = source->getRepr();
    std::string style = repr->attribute("style") ? repr->attribute("style") : "";
    if (!style.empty()) style += ';';
    style += "stroke:";
    style += sp_repr_css_property(css, "stroke", "none");
    repr->setAttribute("style", style.c_str());
}

bool write_text_owners(SPDocument &document, StrokeWidthPlan const &plan,
                       std::vector<TextOwnerWritePlan> const &owners,
                       DocumentUndo::RollbackableInteraction &interaction,
                       std::function<bool()> const &live_scope, std::size_t &written,
                       bool bound_remove = false)
{
    if (owners.empty()) return true;
    PendingOwnerWatch watch(document);
    bool watch_started = false; // the first full scan is the baseline, not a foreign change
    for (std::size_t oi = 0; oi < owners.size(); ++oi) {
        TextOwnerWritePlan const &write = owners[oi];
        StrokeWidthMemberPlan const &member = *write.member;
        bool const whole = member.target.whole_object;
        bool const bulk = whole || (write.groups.size() >= g_bulk_text_threshold && !write.stylesheet_width &&
                          !has_collapsible_whitespace(member.target.owner.get()));
        BulkOwnerState bulk_state;
        std::unordered_map<SPObject *, unsigned> whole_counts;
        if (whole) for (auto const &run : member.text_runs) whole_counts[run.style_source.get()] += run.last_char - run.first_char;
        bool recheck_owner = false;
        for (std::size_t gi = 0; gi < write.groups.size(); ++gi) {
            TextWriteGroup const &group = write.groups[gi];
            // After any callback-bearing operation, no live access before this check.
            if (!interaction.validFor(&document) || !scope_still_live(live_scope)) return false;
            // Between calls: the owner and every still-pending owner keep their frozen
            // provenance/protection/transform and prewrite run style. An observer mutating a
            // pending owner refuses the next write.
            if (watch.foreign()) {
                if (!pending_text_runs_intact(document, plan, owners, oi, group.first_run, false)) return false;
                watch.acknowledge();
                recheck_owner = false;
                // The scan passes when text, count and rendered styles are equal, but the span
                // structure may not be (a split or merged span): never trust the cached map.
                if (watch_started) bulk_state.invalidate_after_foreign_change();
                watch_started = true;
            } else if (recheck_owner) {
                if (!pending_text_runs_intact(document, plan, owners, oi, group.first_run, true)) return false;
                recheck_owner = false;
            }

            SPItem *owner = member.target.owner.get();
            // Fresh layout for every call; nothing is retained across native writes.
            auto const *layout = te_get_layout(owner);
            if (!layout) return false;
            unsigned const count = static_cast<unsigned>(layout->iteratorToCharIndex(layout->end()));
            if (group.first_char >= group.last_char || group.last_char > count) return false;

            auto const *first = write.runs[group.first_run];
            if (bulk && group.run_count == 1) {
                if (!bulk_state.ready) bulk_state.refresh(owner);
                if (auto *target = bulk_state.direct_target(owner, *first, whole, whole ? whole_counts[first->style_source.get()] : first->last_char - first->first_char)) {
                    auto built = build_text_run_css(*first, !whole);
                    ++g_work.direct_source_writes;
                    watch.begin_style_write(target->getRepr());
                    if (bound_remove && first->intent_kind == StrokeWidthIntentKind::RemoveStroke)
                        write_bound_remove(target, built.css.get());
                    else target->changeCSS(built.css.get(), "style");
                    ++written;
                    // Exactly the one expected style notification, or the write is refused.
                    if (!watch.end_write()) return false;
                    if (!interaction.validFor(&document) || !scope_still_live(live_scope)) return false;
                    continue;
                }
            }
            if (whole) return false; // proven whole owners never enter the native splitter
            bool exact = true;
            if (group.run_count == 1) {
                exact = write_text_run(owner, layout, *first, member, &watch);
            } else {
                StrokeWidthTextRunPlan merged = *first;
                merged.first_char = group.first_char;
                merged.last_char = group.last_char;
                merged.style_source.reset();
                exact = write_text_run(owner, layout, merged, member, &watch);
            }
            ++written;
            if (!exact) return false;
            bulk_state.ready = false;
            recheck_owner = gi + 1 < write.groups.size();

            if (!interaction.validFor(&document) || !scope_still_live(live_scope)) return false;
        }
    }
    return true;
}

/// One native shape-member write via fresh `SPCSSAttr` + `changeCSS`. Extracted
/// from the shape writer so both writers issue the byte-identical native call.
void write_shape_member(SPItem *owner, StrokeWidthMemberPlan const &member, bool bound_remove = false)
{
    auto css = Util::delete_with<sp_repr_css_attr_unref>(sp_repr_css_attr_new());
    if (member.intent_kind == StrokeWidthIntentKind::RemoveStroke) {
        set_string_property(css.get(), "stroke", "none",
                            member.target.runs[0].style.paint_important);
    } else if (member.intent_kind == StrokeWidthIntentKind::Hairline) {
        set_number_property(css.get(), "stroke-width", 1.0, member.width_important);
        set_string_property(css.get(), "vector-effect", "non-scaling-stroke",
                            member.target.runs[0].style.vector_effect_important);
        set_string_property(css.get(), "-inkscape-stroke", "hairline",
                            member.target.runs[0].style.hairline_important);
        if (member.native_dasharray_css) {
            set_string_property(css.get(), "stroke-dasharray", *member.native_dasharray_css,
                                member.dasharray_important);
        }
        if (member.local_dashoffset) {
            set_number_property(css.get(), "stroke-dashoffset", *member.local_dashoffset,
                                member.dashoffset_important);
        }
    } else if (member.local_width) {
        set_number_property(css.get(), "stroke-width", *member.local_width, member.width_important);
        if (member.target.runs[0].style.convention == StrokeWidthConvention::Hairline) {
            set_string_property(css.get(), "-inkscape-stroke", "none",
                                member.target.runs[0].style.hairline_important);
            set_string_property(css.get(), "vector-effect", "none",
                                member.target.runs[0].style.vector_effect_important);
        }
    }
    if (member.native_dasharray_css) {
        set_string_property(css.get(), "stroke-dasharray", *member.native_dasharray_css,
                            member.dasharray_important);
    }
    if (member.local_dashoffset) {
        set_number_property(css.get(), "stroke-dashoffset", *member.local_dashoffset,
                            member.dashoffset_important);
    }
    if (bound_remove && member.intent_kind == StrokeWidthIntentKind::RemoveStroke)
        write_bound_remove(owner, css.get());
    else owner->changeCSS(css.get(), "style");
}

/// Per-shape-member write readiness, extracted verbatim from the shape writer's
/// write loop: live owner/repr, original parent, exact i2doc affine, authored
/// non-style and inline-style attributes, document-root descent, and a fresh
/// single-owner query that must resolve to exactly this frozen target. Never
/// writes and never refreshes layout.
bool shape_member_write_ready(SPDocument &document, StrokeWidthPlan const &plan,
                              StrokeWidthMemberPlan const &member)
{
    SPItem *owner = member.target.owner.get();
    if (!owner || owner->document != &document || !owner->getRepr()) return false;
    if (owner->parent != member.original_parent.get()) return false;

    Geom::Affine const affine = owner->i2doc_affine();
    for (unsigned i = 0; i < 6; ++i) {
        if (affine[i] != member.i2doc_affine[i]) return false;
    }
    if (non_style_attributes_of(owner) != member.non_style_attributes) return false;
    if (inline_style_of(owner) != member.inline_style) return false;

    // A callback earlier in the operation can newly lock/hide an ancestor or
    // detach a live retained subtree without changing this leaf's authored XML,
    // parent or affine. Re-resolve the owner read-only through the existing
    // native query before writing: it must still descend from the frozen
    // document root and resolve to exactly one eligible target whose complete
    // frozen shape (binding, runs and style) still matches the plan. No new
    // protection policy and no layout call.
    if (!owned_by_document_root(owner, document, plan.document_root.get())) return false;
    StrokeWidthResult const current = query_stroke_widths(document, std::vector<SPItem *>{owner});
    if (current.eligible != 1 || current.targets.size() != 1 ||
        !same_target_frozen(current.targets[0], member.target)) {
        return false;
    }
    return true;
}

/**
 * Whole-plan structural gate for the compatible writer. Only invariants reject
 * the whole plan: a legacy text-only scope would broaden a character range;
 * an unsupported changed member/shape patch has no safe writer; or a supposedly
 * writable text run has no valid width/patch. Unsupported numeric text runs are
 * classified Excluded during planning, so they do not veto other members.
 */
bool compatible_plan_supported(StrokeWidthPlan const &plan, StrokeWidthMemberReason &reason)
{
    if (plan.text_scope && !plan.combined_text_scope) {
        reason = StrokeWidthMemberReason::UnsupportedIntent;
        return false;
    }
    for (auto const &member : plan.members) {
        if (member.target.kind == StrokeWidthTargetKind::TextOwner) {
            if (!text_runs_supported(member, reason)) return false;
            continue;
        }
        if (member.outcome != StrokeWidthMemberOutcome::Change) continue;
        if (member.target.kind != StrokeWidthTargetKind::Shape || member.target.runs.size() != 1 ||
            !member.local_width) {
            reason = StrokeWidthMemberReason::UnsupportedIntent;
            return false;
        }
    }
    return true;
}

/**
 * Postwrite intended-output verification for the compatible writer. Never
 * reprepares and never writes.
 *
 *  - every member's binding/affine/non-style authored attributes are equal to the
 *    frozen plan (the owner inline `style` is not a shared invariant: text span
 *    rewriting may normalize it, while an unchanged non-text member is checked
 *    against its exact frozen inline style);
 *  - every changed Shape member is verified with `verify_changed_member`;
 *  - every TextOwner member is verified with `verify_text_owner_output` against
 *    the immutable query-time baseline, because native span splitting may change
 *    source/run grouping;
 *  - a fresh read-only query re-checks the aggregate counts, target order and
 *    identity, every excluded record's captured fields, and each target's stable
 *    metadata. A changed TextOwner is compared with `same_target_metadata` (runs
 *    excluded) because its run partition may legitimately change; a changed shape
 *    and every unchanged target use `same_target_scope`, and unchanged targets
 *    additionally use `same_runs_frozen`;
 *  - the bounded selected-clone `dependency_decision` is re-evaluated once.
 */
bool verify_compatible_output(SPDocument &document, StrokeWidthPlan const &plan)
{
    // Unsupported scope and changed kinds/runs fail the same structural gate in
    // apply and output readiness. Re-applying it keeps the
    // output-readiness path (which does not run the apply-time gate) from ever
    // reporting an unsupported plan as ready.
    StrokeWidthMemberReason gate_reason = StrokeWidthMemberReason::None;
    if (!compatible_plan_supported(plan, gate_reason)) return false;

    if (!verify_member_bindings(document, plan) || !verify_preserved_clones(document, plan)) return false;

    // Changed Shape members: exact intended local width/patch and preserved
    // unrelated declarations, exactly as the shape writer verifies.
    for (auto const &member : plan.members) {
        if (member.target.kind != StrokeWidthTargetKind::Shape) continue;
        if (member.outcome != StrokeWidthMemberOutcome::Change) continue;
        if (member.target.runs.size() != 1) return false;
        if (!verify_changed_member(member.target.owner.get(), member)) return false;
    }

    // Untouched/excluded non-text members keep their exact frozen inline style.
    // TextOwner owner-level inline style is deliberately excluded (native span
    // rewriting may normalize it without changing rendered output).
    for (auto const &member : plan.members) {
        if (member.target.kind == StrokeWidthTargetKind::TextOwner) continue;
        if (member.outcome == StrokeWidthMemberOutcome::Change) continue;
        if (inline_style_of(member.target.owner.get()) != member.inline_style) return false;
    }

    // Every TextOwner member, changed or not, is verified by rendered output,
    // semantic content/count and the immutable whole-owner baseline. Never by
    // retained run structure or owner inline style.
    for (auto const &member : plan.members) {
        if (member.target.kind != StrokeWidthTargetKind::TextOwner) continue;
        if (!verify_text_owner_output(document, member)) return false;
    }

    // Fresh read-only query (not a reprepare): aggregate scope, target order and
    // identity, and every excluded record must equal the frozen query.
    std::vector<SPItem *> roots;
    for (auto const &weak : plan.roots) {
        auto *root = weak.get();
        if (!root || root->document != &document) return false;
        roots.push_back(root);
    }
    StrokeWidthResult const fresh = plan.combined_text_scope
        ? query_combined_stroke_widths(document, *plan.text_scope, roots)
        : query_stroke_widths(document, roots);

    if (fresh.range_rejected != plan.query.range_rejected ||
        fresh.range_exclusion != plan.query.range_exclusion) {
        return false;
    }
    // Which eligible owners actually changed in this operation: a shape `Change`
    // member, or a TextOwner owner whose prospective runs include a `Change`.
    std::unordered_set<SPItem *> changed_owners;
    std::unordered_set<SPItem *> changed_text_owners;
    for (auto const &member : plan.members) {
        SPItem *owner = member.target.owner.get();
        if (!owner) continue;
        if (member.target.kind == StrokeWidthTargetKind::Shape &&
            member.outcome == StrokeWidthMemberOutcome::Change) {
            changed_owners.insert(owner);
        } else if (member.target.kind == StrokeWidthTargetKind::TextOwner &&
                   text_owner_outcome(member) == StrokeWidthMemberOutcome::Change) {
            changed_owners.insert(owner);
            changed_text_owners.insert(owner);
        }
    }

    // Aggregate counts, target order/identity and every excluded record equal the frozen
    // query, with one exception: selected clones of an unset source may only move to
    // `CloneSourceOverrides` (`expect_clone_classes`). Query-excluded records keep equality
    // for their captured classification and target fields. Bitmap geometry/payload is not
    // represented in these records; the writer never targets bitmaps, and that separate
    // preservation assertion is covered by the outcome test.
    auto const clones = expect_clone_classes(plan);
    if (!query_matches_frozen(plan, fresh, clones,
            [&](StrokeWidthTarget const &fresh_target, StrokeWidthTarget const &frozen_target) {
                if (changed_text_owners.count(fresh_target.owner.get())) {
                    // Span splitting legitimately changes run grouping/style-source
                    // identity, so only the stable target metadata is compared; the
                    // rendered output and baseline are already checked above.
                    return same_target_metadata(fresh_target, frozen_target);
                }
                if (!same_target_scope(fresh_target, frozen_target)) return false;
                SPItem *owner = fresh_target.owner.get();
                // A clone that follows a changed source renders the source's new style.
                if (changed_owners.count(owner) || clones.following.count(owner)) return true;
                return same_runs_frozen(fresh_target.runs, frozen_target.runs);
            })) {
        return false;
    }

    // Re-evaluate the read-only dependency guard against the original changed
    // owner set: a post-write callback must not silently bind another selected
    // clone to a changed owner (or leave its source graph uncertain).
    if (!dependency_as_planned(plan, document)) {
        return false;
    }
    return true;
}

// Count only owners accepted by the writer, from their live postwrite paint.
// Apply callers just completed the full verifier with no intervening callbacks;
// note-only callers still require verification here. Commit rechecks separately.
// Text paint can live on spans; never infer it from the owner style alone.
std::size_t changed_paint_none(StrokeWidthPlan const &plan, bool shapes, bool text, bool text_verified = false)
{
    std::unordered_set<SPItem *> none;
    for (auto const &member : plan.members) {
        auto *owner = member.target.owner.get();
        if (!owner) continue;
        if (shapes && member.target.kind == StrokeWidthTargetKind::Shape &&
            member.outcome == StrokeWidthMemberOutcome::Change && owner->style && owner->style->stroke.isNone() &&
            verify_changed_member(owner, member)) {
            none.insert(owner);
        }
        if (!text || member.target.kind != StrokeWidthTargetKind::TextOwner ||
            text_owner_outcome(member) != StrokeWidthMemberOutcome::Change ||
            (!text_verified && !verify_text_owner_output(*owner->document, member))) continue;
        auto const *layout = te_get_layout(owner);
        LayoutSources const origins(*layout);
        for (auto const &run : member.text_runs) {
            if (run.outcome != StrokeWidthMemberOutcome::Change) continue;
            for (unsigned index = run.first_char; index < run.last_char; ++index) {
                StrokeWidthCharBaseline current;
                if (capture_rendered_char(layout, origins, owner, index, member.target.transform_scale, current) &&
                    current.authored && current.style.paint_none) none.insert(owner);
            }
        }
    }
    return none.size();
}
} // namespace

PendingOwnerWatch::PendingOwnerWatch(SPDocument &document)
    : _root(document.getReprRoot())
    , _style(g_quark_from_static_string("style"))
{
    // The document's XML may outlive it (a retained node): the observer must leave the XML before
    // the root pointer is dropped.
    _destroyed = document.connectDestroy([this] { detach(); });
    if (_root) _root->addSubtreeObserver(*this);
}

PendingOwnerWatch::~PendingOwnerWatch()
{
    _destroyed.disconnect();
    detach();
}

void PendingOwnerWatch::detach()
{
    if (!_root) return;
    _root->removeSubtreeObserver(*this);
    _root = nullptr;
}

void PendingOwnerWatch::begin_subtree_write(XML::Node const *root)
{
    _subtree = root;
    _element = nullptr;
    _expected_style_writes = 0;
    _violated = false;
}

void PendingOwnerWatch::begin_style_write(XML::Node const *element)
{
    _element = element;
    _subtree = nullptr;
    _expected_style_writes = 1;
    _violated = false;
}

bool PendingOwnerWatch::end_write()
{
    // A direct write must have produced its one style notification and nothing on its element.
    bool const exact = !_element || (!_violated && _expected_style_writes == 0);
    _subtree = _element = nullptr;
    _expected_style_writes = 0;
    _violated = false;
    return exact;
}

void PendingOwnerWatch::changed(XML::Node &node, GQuark attribute)
{
    if (_subtree) {
        for (XML::Node const *n = &node; n; n = n->parent()) {
            if (n == _subtree) return;
        }
    }
    if (_element && &node == _element) {
        if (attribute == _style && _expected_style_writes > 0) {
            --_expected_style_writes;
            return;
        }
        // Any other change to the active element, including a second style rewrite.
        _violated = true;
        return;
    }
    _foreign = true;
}

std::size_t stroke_width_set_bulk_text_threshold(std::size_t units)
{
    return std::exchange(g_bulk_text_threshold, units);
}

/// Authored non-style attributes are equal, except the path effect's OUTPUT: only `d`
/// may differ, and only when both snapshots carry the same `inkscape:path-effect` and
/// the same authored source geometry `inkscape:original-d`. That is verified
/// regeneration (an automatic update recomputing the effect); a changed source, effect
/// reference or any other attribute is a different plan.
bool same_attributes_but_lpe_path(std::vector<std::pair<std::string, std::string>> const &a,
                                  std::vector<std::pair<std::string, std::string>> const &b)
{
    auto const find = [](auto const &list, char const *name) -> std::string const * {
        for (auto const &[key, value] : list) {
            if (key == name) return &value;
        }
        return nullptr;
    };
    auto const *effect_a = find(a, "inkscape:path-effect");
    auto const *effect_b = find(b, "inkscape:path-effect");
    auto const *source_a = find(a, "inkscape:original-d");
    auto const *source_b = find(b, "inkscape:original-d");
    bool const lpe = effect_a && effect_b && *effect_a == *effect_b && source_a && source_b && *source_a == *source_b;
    auto const path_data = [&](std::string const &key) { return lpe && key == "d"; };
    for (auto const &[key, value] : a) {
        if (path_data(key)) continue;
        auto const *other = find(b, key.c_str());
        if (!other || *other != value) return false;
    }
    for (auto const &[key, value] : b) {
        if (path_data(key)) continue;
        if (!find(a, key.c_str())) return false;
    }
    return true;
}

bool stroke_width_plans_match(StrokeWidthPlan const &a, StrokeWidthPlan const &b)
{
    // Everything that decides WHAT is written, strictly: intent, counts, text scope,
    // every target's frozen style (hairline, vector-effect, dash, priority) and run
    // scope, every member and text-run patch (width, dash array and offset, native dash
    // text, importance bits, outcome, reason), and provenance: parent, exact affine,
    // inline style and every authored attribute. The single permitted difference is `d`
    // when an unchanged path effect (same reference, same `inkscape:original-d`)
    // regenerated it during an automatic update.
    if (a.state != b.state || a.rejection != b.rejection) return false;
    if (a.text_scope.has_value() != b.text_scope.has_value() || a.combined_text_scope != b.combined_text_scope) {
        return false;
    }
    if (a.text_scope) {
        if (a.text_scope->owner.get() != b.text_scope->owner.get() || a.text_scope->caret != b.text_scope->caret ||
            a.text_scope->first_char != b.text_scope->first_char ||
            a.text_scope->last_char != b.text_scope->last_char) {
            return false;
        }
    }
    if (a.intent.kind != b.intent.kind || a.intent.scale_dashes != b.intent.scale_dashes ||
        !same_double_frozen(a.intent.value, b.intent.value)) {
        return false;
    }
    if (a.planned_changes != b.planned_changes || a.unchanged != b.unchanged || a.excluded != b.excluded ||
        a.skipped_runs != b.skipped_runs || a.following_clones != b.following_clones ||
        a.following.size() != b.following.size() || a.members.size() != b.members.size() ||
        a.preserved_clones.size() != b.preserved_clones.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.following.size(); ++i) {
        if (a.following[i].get() != b.following[i].get()) return false;
    }
    for (std::size_t i = 0; i < a.members.size(); ++i) {
        auto const &x = a.members[i];
        auto const &y = b.members[i];
        if (x.intent_kind != y.intent_kind || !same_target_frozen(x.target, y.target)) return false;
        // Provenance: parent, exact affine and authored style text never change.
        if (x.original_parent.get() != y.original_parent.get() || x.inline_style != y.inline_style) return false;
        for (unsigned k = 0; k < 6; ++k) {
            if (!same_double_frozen(x.i2doc_affine[k], y.i2doc_affine[k])) return false;
        }
        if (!same_attributes_but_lpe_path(x.non_style_attributes, y.non_style_attributes)) return false;
        if (x.text_runs.size() != y.text_runs.size()) return false;
        for (std::size_t j = 0; j < x.text_runs.size(); ++j) {
            if (!same_text_run_frozen(x.text_runs[j], y.text_runs[j])) return false;
        }
        if (!same_optional_double(x.local_width, y.local_width)) return false;
        if (x.local_dasharray.has_value() != y.local_dasharray.has_value()) return false;
        if (x.local_dasharray && !same_vector_frozen(*x.local_dasharray, *y.local_dasharray)) return false;
        if (!same_optional_double(x.local_dashoffset, y.local_dashoffset)) return false;
        if (x.native_dasharray_css != y.native_dasharray_css) return false;
        if (x.native_dasharray_computed.has_value() != y.native_dasharray_computed.has_value()) return false;
        if (x.native_dasharray_computed &&
            !same_vector_frozen(*x.native_dasharray_computed, *y.native_dasharray_computed)) {
            return false;
        }
        if (x.width_important != y.width_important || x.dasharray_important != y.dasharray_important ||
            x.dashoffset_important != y.dashoffset_important || x.outcome != y.outcome || x.reason != y.reason) {
            return false;
        }
    }
    for (std::size_t i = 0; i < a.preserved_clones.size(); ++i) {
        auto const &x = a.preserved_clones[i];
        auto const &y = b.preserved_clones[i];
        if (!same_target_frozen(x.target, y.target) || x.original_parent.get() != y.original_parent.get() ||
            x.inline_style != y.inline_style || !same_attributes_but_lpe_path(x.non_style_attributes, y.non_style_attributes)) {
            return false;
        }
        for (unsigned k = 0; k < 6; ++k) {
            if (!same_double_frozen(x.i2doc_affine[k], y.i2doc_affine[k])) return false;
        }
    }
    return true;
}

static StrokeWidthApplyResult apply_stroke_widths_impl(SPDocument &document, StrokeWidthPlan const &plan,
                                           std::uint64_t current_scope_generation,
                                           DocumentUndo::RollbackableInteraction &interaction,
                                           std::function<bool()> const &live_scope)
{
    StrokeWidthApplyResult result;
    result.skipped_runs = plan.skipped_runs;

    std::vector<SPItem *> roots;
    StrokeWidthMemberReason precondition_reason = StrokeWidthMemberReason::None;
    if (!validate_apply_preconditions(document, plan, current_scope_generation, interaction, roots,
                                      precondition_reason)) {
        result.state = StrokeWidthApplyState::Rejected;
        result.reason = precondition_reason;
        return result;
    }

    // Read-only re-derivation of the frozen plan FIRST, for unchanged/excluded
    // plans too: a stale or tampered no-op must be rejected without writes. No
    // ensureUpToDate before the first write; the caller owns native layout
    // currency, and this path performs no layout refresh.
    if (!reprepare_matches(document, plan, roots)) {
        result.state = StrokeWidthApplyState::Rejected;
        result.reason = StrokeWidthMemberReason::StalePlan;
        return result;
    }

    // A genuinely all-unchanged/excluded plan is a no-op: no layout refresh and
    // no history.
    if (plan.planned_changes == 0) {
        result.state = StrokeWidthApplyState::Unchanged;
        result.reason = StrokeWidthMemberReason::None;
        result.changed = 0;
        result.unchanged = plan.unchanged;
        result.excluded = plan.excluded;
        result.attempted_writes = 0;
        return result;
    }

    std::size_t written = 0;
    bool failed = false;
    for (auto const &member : plan.members) {
        if (member.outcome != StrokeWidthMemberOutcome::Change) continue;

        // After any callback-bearing operation, no live access before this check.
        if (!interaction.validFor(&document) || !scope_still_live(live_scope)) {
            failed = true;
            break;
        }

        // Reacquire and revalidate this member's live owner/repr, original
        // parent, exact affine, authored attributes, document-root descent and
        // frozen single-owner query result before writing it.
        if (!shape_member_write_ready(document, plan, member)) {
            failed = true;
            break;
        }

        write_shape_member(member.target.owner.get(), member);
        ++written;

        if (!interaction.validFor(&document) || !scope_still_live(live_scope)) {
            failed = true;
            break;
        }
    }

    if (failed) {
        result.state = StrokeWidthApplyState::Failed;
        result.reason = StrokeWidthMemberReason::PostconditionMismatch;
        // Pending native writes only; no verified change is claimed.
        result.changed = 0;
        result.unchanged = plan.unchanged;
        result.excluded = plan.excluded;
        result.attempted_writes = written;
        return result;
    }

    // Native layout currency only after actual writes, inside the caller's
    // transaction, then intended-output verification.
    document.ensureUpToDate();
    if (!interaction.validFor(&document) || !verify_intended_output(document, plan)) {
        result.state = StrokeWidthApplyState::Failed;
        result.reason = StrokeWidthMemberReason::PostconditionMismatch;
        result.changed = 0;
        result.unchanged = plan.unchanged;
        result.excluded = plan.excluded;
        result.attempted_writes = written;
        return result;
    }

    result.paint_none = changed_paint_none(plan, true, false);
    result.state = StrokeWidthApplyState::Applied;
    result.reason = StrokeWidthMemberReason::None;
    result.changed = written;
    result.unchanged = plan.unchanged;
    result.excluded = plan.excluded;
    result.attempted_writes = written;
    return result;
}

bool stroke_widths_output_ready(SPDocument &document, StrokeWidthPlan const &plan,
                                std::uint64_t current_scope_generation,
                                DocumentUndo::RollbackableInteraction &interaction)
{
    if (!interaction.validFor(&document)) return false;
    if (plan.state != StrokeWidthPlanState::Prepared) return false;
    if (current_scope_generation != plan.scope_generation) return false;
    if (plan.document_root.get() != document.getRoot()) return false;
    for (auto const &weak : plan.roots) {
        auto *root = weak.get();
        if (!root || root->document != &document) return false;
    }
    if (plan.text_scope) {
        auto *owner = plan.text_scope->owner.get();
        if (!owner || owner->document != &document) return false;
    }
    return verify_intended_output(document, plan);
}

static StrokeWidthApplyResult apply_stroke_widths_text_impl(SPDocument &document, StrokeWidthPlan const &plan,
                                                std::uint64_t current_scope_generation,
                                                DocumentUndo::RollbackableInteraction &interaction,
                                                std::function<bool()> const &live_scope)
{
    StrokeWidthApplyResult result;
    result.skipped_runs = plan.skipped_runs;

    std::vector<SPItem *> roots;
    StrokeWidthMemberReason precondition_reason = StrokeWidthMemberReason::None;
    if (!validate_apply_preconditions(document, plan, current_scope_generation, interaction, roots,
                                      precondition_reason)) {
        result.state = StrokeWidthApplyState::Rejected;
        result.reason = precondition_reason;
        return result;
    }

    // Exact whole-plan re-derivation before any write, for no-op plans too: a
    // stale or tampered plan is rejected without a single native call.
    if (!reprepare_matches(document, plan, roots)) {
        result.state = StrokeWidthApplyState::Rejected;
        result.reason = StrokeWidthMemberReason::StalePlan;
        return result;
    }

    StrokeWidthMemberReason gate_reason = StrokeWidthMemberReason::None;
    if (!text_plan_supported(plan, gate_reason)) {
        result.state = StrokeWidthApplyState::Rejected;
        result.reason = gate_reason;
        return result;
    }

    // Observed owner-level classification. The plan keeps every TextOwner member
    // Excluded/TextAdapterPending, so a changed text owner must not be reported
    // as excluded and an all-unchanged text owner must not be reported as
    // excluded either. `plan.excluded` includes every text member once, so
    // subtracting the classified text owners cannot underflow.
    std::size_t text_changed = 0;
    std::size_t text_unchanged = 0;
    count_text_owner_outcomes(plan, text_changed, text_unchanged);
    std::size_t const result_unchanged = plan.unchanged + text_unchanged;
    std::size_t const result_excluded = plan.excluded - text_changed - text_unchanged;

    // Owners in deterministic plan order; within each owner, changed runs
    // strictly descending by logical start index: a later native call may
    // split/reparent spans, so earlier indices are written last.
    std::vector<TextOwnerWritePlan> owners = collect_text_owner_writes(plan);

    if (owners.empty()) {
        result.state = StrokeWidthApplyState::Unchanged;
        result.reason = StrokeWidthMemberReason::None;
        result.changed = 0;
        result.unchanged = result_unchanged;
        result.excluded = result_excluded;
        result.attempted_writes = 0;
        return result;
    }

    // Freeze and check the entire plan before any write: every changed text run
    // glyph range, the complete whole-owner baseline and every owner's
    // ownership/protection/transform/non-style attributes. A stale or unsupported
    // plan rejects atomically here; nothing is reprepared or rebaselined after a
    // write begins.
    StrokeWidthMemberReason const preflight = preflight_text_plan(document, plan);
    if (preflight != StrokeWidthMemberReason::None) {
        result.state = StrokeWidthApplyState::Rejected;
        result.reason = preflight;
        return result;
    }

    std::size_t written = 0;
    bool failed = false;
    if (!write_text_owners(document, plan, owners, interaction, live_scope, written)) failed = true;

    if (failed) {
        result.state = StrokeWidthApplyState::Failed;
        result.reason = StrokeWidthMemberReason::PostconditionMismatch;
        // Pending native writes only; no verified change is claimed.
        result.changed = 0;
        result.unchanged = result_unchanged;
        result.excluded = result_excluded;
        result.attempted_writes = written;
        return result;
    }

    // Native layout currency only after actual writes, inside the caller's
    // transaction, then intended-output verification across every TextOwner.
    document.ensureUpToDate();
    if (!interaction.validFor(&document) || !verify_text_intended_output(document, plan)) {
        result.state = StrokeWidthApplyState::Failed;
        result.reason = StrokeWidthMemberReason::PostconditionMismatch;
        result.changed = 0;
        result.unchanged = result_unchanged;
        result.excluded = result_excluded;
        result.attempted_writes = written;
        return result;
    }

    result.paint_none = changed_paint_none(plan, false, true, true);
    result.state = StrokeWidthApplyState::Applied;
    result.reason = StrokeWidthMemberReason::None;
    // Verified changed TextOwner owners; attempted_writes is the native call
    // count.
    result.changed = text_changed;
    result.unchanged = result_unchanged;
    result.excluded = result_excluded;
    result.attempted_writes = written;
    return result;
}

bool stroke_widths_text_output_ready(SPDocument &document, StrokeWidthPlan const &plan,
                                     std::uint64_t current_scope_generation,
                                     DocumentUndo::RollbackableInteraction &interaction)
{
    std::vector<SPItem *> roots;
    StrokeWidthMemberReason reason = StrokeWidthMemberReason::None;
    if (!validate_apply_preconditions(document, plan, current_scope_generation, interaction, roots,
                                      reason)) {
        return false;
    }
    return verify_text_intended_output(document, plan);
}

static StrokeWidthApplyResult apply_stroke_widths_compatible_impl(SPDocument &document, StrokeWidthPlan const &plan,
                                                      std::uint64_t current_scope_generation,
                                                      DocumentUndo::RollbackableInteraction &interaction,
                                                      std::function<bool()> const &live_scope,
                                                      bool bound_remove = false)
{
    StrokeWidthApplyResult result;
    result.skipped_runs = plan.skipped_runs;

    std::vector<SPItem *> roots;
    StrokeWidthMemberReason precondition_reason = StrokeWidthMemberReason::None;
    if (!validate_apply_preconditions(document, plan, current_scope_generation, interaction, roots,
                                      precondition_reason)) {
        result.state = StrokeWidthApplyState::Rejected;
        result.reason = precondition_reason;
        return result;
    }

    // The ONE whole-plan freeze: exactly one read-only re-derivation before any
    // native write, for no-op plans too. No writer in this operation reprepares
    // afterwards; a sequential `apply_stroke_widths` + `apply_stroke_widths_text`
    // pair cannot do this because the second entry's reprepare would see the
    // first writer's changes as stale.
    if (!reprepare_matches(document, plan, roots)) {
        result.state = StrokeWidthApplyState::Rejected;
        result.reason = StrokeWidthMemberReason::StalePlan;
        return result;
    }

    // Structural gate before any write: legacy range plans, unsupported changed
    // kinds, unsupported text runs and unsupported single-run shape patches all
    // reject atomically here.
    StrokeWidthMemberReason gate_reason = StrokeWidthMemberReason::None;
    if (!compatible_plan_supported(plan, gate_reason)) {
        result.state = StrokeWidthApplyState::Rejected;
        result.reason = gate_reason;
        return result;
    }

    // Observed owner-level classification. The plan keeps every TextOwner member
    // Excluded/TextAdapterPending, so a changed text owner is never reported as
    // excluded. `plan.excluded` includes every text member once, so subtracting
    // the classified text owners cannot underflow.
    std::size_t text_changed = 0;
    std::size_t text_unchanged = 0;
    count_text_owner_outcomes(plan, text_changed, text_unchanged);
    std::size_t const result_unchanged = plan.unchanged + text_unchanged;
    std::size_t const result_excluded = plan.excluded - text_changed - text_unchanged;
    std::size_t const result_changed = plan.planned_changes + text_changed;

    // A genuinely all-unchanged/excluded plan is a no-op: no preflight layout
    // access, no native write, no history.
    if (result_changed == 0) {
        result.state = StrokeWidthApplyState::Unchanged;
        result.reason = StrokeWidthMemberReason::None;
        result.changed = 0;
        result.unchanged = result_unchanged;
        result.excluded = result_excluded;
        result.attempted_writes = 0;
        return result;
    }

    // Whole-plan preflight before ANY write. `preflight_text_plan` validates every
    // member's binding/affine/authored attributes, every TextOwner baseline and
    // every changed text run's live layout/glyph range; the shape write-ready
    // checks add the document-root/protection/frozen-target re-query for every
    // changed shape. A stale or unsupported plan rejects with zero native calls.
    StrokeWidthMemberReason const preflight = preflight_text_plan(document, plan);
    if (preflight != StrokeWidthMemberReason::None) {
        result.state = StrokeWidthApplyState::Rejected;
        result.reason = preflight;
        return result;
    }
    for (auto const &member : plan.members) {
        if (member.target.kind != StrokeWidthTargetKind::Shape ||
            member.outcome != StrokeWidthMemberOutcome::Change) {
            continue;
        }
        if (!shape_member_write_ready(document, plan, member)) {
            result.state = StrokeWidthApplyState::Rejected;
            result.reason = StrokeWidthMemberReason::StalePlan;
            return result;
        }
    }

    std::size_t written = 0;
    bool failed = false;

    // Phase 1: text runs. Owners follow frozen plan order; each owner's changed
    // runs are strictly descending by logical start index because a later native
    // call may split/reparent spans, so earlier indices are written last. Layout
    // and iterators are reacquired before every call and no iterator is retained.
    std::vector<TextOwnerWritePlan> owners = collect_text_owner_writes(plan);
    if (!write_text_owners(document, plan, owners, interaction, live_scope, written, bound_remove)) failed = true;

    // Phase 2: shape members in frozen plan order. Each is rechecked immediately
    // before its write, so a callback in the text phase or an earlier shape write
    // that invalidated a still-pending shape is refused before it is overwritten.
    for (auto const &member : plan.members) {
        if (failed) break;
        if (member.target.kind != StrokeWidthTargetKind::Shape ||
            member.outcome != StrokeWidthMemberOutcome::Change) {
            continue;
        }
        if (!interaction.validFor(&document) || !scope_still_live(live_scope)) {
            failed = true;
            break;
        }
        if (!shape_member_write_ready(document, plan, member)) {
            failed = true;
            break;
        }
        write_shape_member(member.target.owner.get(), member, bound_remove);
        ++written;

        if (!interaction.validFor(&document) || !scope_still_live(live_scope)) {
            failed = true;
            break;
        }
    }

    if (failed) {
        result.state = StrokeWidthApplyState::Failed;
        result.reason = StrokeWidthMemberReason::PostconditionMismatch;
        // Pending native writes only; no verified change is claimed.
        result.changed = 0;
        result.unchanged = result_unchanged;
        result.excluded = result_excluded;
        result.attempted_writes = written;
        return result;
    }

    // Native layout currency only after all actual writes, inside the caller's
    // transaction, then ONE combined read-only verifier. Nothing is reprepared
    // and no fresh baseline is inferred after a write.
    document.ensureUpToDate();
    if (!interaction.validFor(&document) || !verify_compatible_output(document, plan)) {
        result.state = StrokeWidthApplyState::Failed;
        result.reason = StrokeWidthMemberReason::PostconditionMismatch;
        result.changed = 0;
        result.unchanged = result_unchanged;
        result.excluded = result_excluded;
        result.attempted_writes = written;
        return result;
    }

    result.paint_none = changed_paint_none(plan, true, true, true);
    result.state = StrokeWidthApplyState::Applied;
    result.reason = StrokeWidthMemberReason::None;
    // Verified changed owners (shape changes + TextOwner owners with changed
    // runs); attempted_writes is the native call count.
    result.changed = result_changed;
    result.unchanged = result_unchanged;
    result.excluded = result_excluded;
    result.attempted_writes = written;
    return result;
}

bool stroke_widths_compatible_output_ready(SPDocument &document, StrokeWidthPlan const &plan,
                                           std::uint64_t current_scope_generation,
                                           DocumentUndo::RollbackableInteraction &interaction)
{
    std::vector<SPItem *> roots;
    StrokeWidthMemberReason reason = StrokeWidthMemberReason::None;
    if (!validate_apply_preconditions(document, plan, current_scope_generation, interaction, roots,
                                      reason)) {
        return false;
    }
    return verify_compatible_output(document, plan);
}

StrokeWidthApplyResult apply_stroke_widths(SPDocument &document, StrokeWidthPlan const &plan,
                          std::uint64_t current_scope_generation,
                          DocumentUndo::RollbackableInteraction &interaction,
                          std::function<bool()> const &live_scope)
{
    g_work = {};
    auto result = apply_stroke_widths_impl(document, plan, current_scope_generation, interaction, live_scope);
    result.work = g_work;
    return result;
}

StrokeWidthApplyResult apply_stroke_widths_text(SPDocument &document, StrokeWidthPlan const &plan,
                          std::uint64_t current_scope_generation,
                          DocumentUndo::RollbackableInteraction &interaction,
                          std::function<bool()> const &live_scope)
{
    g_work = {};
    auto result = apply_stroke_widths_text_impl(document, plan, current_scope_generation, interaction, live_scope);
    result.work = g_work;
    return result;
}

StrokeWidthApplyResult apply_stroke_widths_compatible(SPDocument &document, StrokeWidthPlan const &plan,
                          std::uint64_t current_scope_generation,
                          DocumentUndo::RollbackableInteraction &interaction,
                          std::function<bool()> const &live_scope)
{
    g_work = {};
    auto result = apply_stroke_widths_compatible_impl(document, plan, current_scope_generation, interaction, live_scope);
    result.work = g_work;
    return result;
}

StrokeWidthApplyResult apply_stroke_widths_bound(SPDocument &document, StrokeWidthPlan const &plan,
                          std::uint64_t current_scope_generation,
                          DocumentUndo::RollbackableInteraction &interaction,
                          std::function<bool()> const &live_scope)
{
    if (plan.text_scope) {
        StrokeWidthApplyResult result;
        result.state = StrokeWidthApplyState::Rejected;
        result.reason = StrokeWidthMemberReason::UnsupportedIntent;
        return result;
    }
    g_work = {};
    auto result = apply_stroke_widths_compatible_impl(document, plan, current_scope_generation,
                                                    interaction, live_scope, true);
    result.work = g_work;
    return result;
}

std::optional<DocumentUndo::RollbackableInteraction> begin_stroke_width_interaction(
    SPDocument &document, StrokeWidthPlan const &plan, std::function<bool()> const &proceed)
{
    auto token = DocumentUndo::beginAtomicInteraction(&document);
    if (token || DocumentUndo::interactionActive(&document) || !DocumentUndo::interactionIsQuiescent(&document) ||
        DocumentUndo::interactionCloseRequested(&document)) {
        return token;
    }
    bool const changes = plan.planned_changes > 0 ||
                         std::any_of(plan.members.begin(), plan.members.end(), [](auto const &member) {
                             return std::any_of(member.text_runs.begin(), member.text_runs.end(), [](auto const &run) {
                                 return run.outcome == StrokeWidthMemberOutcome::Change;
                             });
                         });
    if (!changes) {
        return token;
    }
    // The settlement's commit callbacks may destroy the document or end the caller's
    // scope: nothing may touch the document again before both are checked.
    bool document_gone = false;
    sigc::scoped_connection const on_destroy = document.connectDestroy([&] { document_gone = true; });
    DocumentUndo::done(&document, RC_("Undo", "Automatic update"), "");
    if (document_gone || (proceed && !proceed())) return std::nullopt;
    return DocumentUndo::beginAtomicInteraction(&document);
}

namespace {
std::string applied_note(StrokeWidthPlan const &plan, std::size_t excluded, std::size_t paint_none,
                         bool applied)
{
    auto const following = plan.following_clones;
    // Following clones that are themselves counted in `excluded`: excluded
    // members and the query's incompatible/unavailable/missing records (a
    // clone whose source sets its own width is one). A clone found only
    // inside a selected locked group is not one; a clone selected twice is two.
    auto const is_following = [&](SPItem *owner) {
        if (!owner) return false;
        for (auto const &clone : plan.following) {
            if (clone.get() == owner) return true;
        }
        return false;
    };
    std::size_t following_records = 0;
    for (auto const &member : plan.members) {
        if (member.outcome == StrokeWidthMemberOutcome::Excluded && is_following(member.target.owner.get())) {
            ++following_records;
        }
    }
    for (auto const &target : plan.query.excluded) {
        if (target.eligibility != StrokeWidthEligibility::Covered && is_following(target.owner.get())) {
            ++following_records;
        }
    }
    auto const others = excluded > following_records ? excluded - following_records : 0;
    auto exclusion_note = [&](StrokeWidthMemberReason reason) {
        std::size_t count = 0;
        std::string names;
        for (auto const &member : plan.members) {
            if (!std::any_of(member.text_runs.begin(), member.text_runs.end(), [&](auto const &run) {
                return run.reason == reason;
            })) continue;
            ++count;
            auto *owner = member.target.owner.get();
            auto const *label = owner && owner->getRepr() ? owner->getRepr()->attribute("inkscape:label") : nullptr;
            auto const *id = owner ? owner->getId() : nullptr;
            if (!names.empty()) names += ", ";
            names += label ? label : id ? id : _("unnamed text");
        }
        if (!count) return std::string{};
        if (reason == StrokeWidthMemberReason::UnsafeTextWhitespace) {
            auto *text = g_strdup_printf(ngettext(
                "%zu text owner excluded: partial range could rewrite raw text or change collapsed whitespace (%s)",
                "%zu text owners excluded: partial range could rewrite raw text or change collapsed whitespace (%s)", count), count, names.c_str());
            std::string note(text); g_free(text); return note;
        }
        auto *text = g_strdup_printf(ngettext(
            "%zu text owner excluded: direct source coverage cannot be proved (%s)",
            "%zu text owners excluded: direct source coverage cannot be proved (%s)", count), count, names.c_str());
        std::string note(text); g_free(text);
        return note;
    };
    auto coverage_note = [&]() {
        auto note = exclusion_note(StrokeWidthMemberReason::UnsupportedTextCoverage);
        auto whitespace = exclusion_note(StrokeWidthMemberReason::UnsafeTextWhitespace);
        if (!note.empty() && !whitespace.empty()) note += "; ";
        return note + whitespace;
    };
    if (!applied) {
        if (plan.intent.kind == StrokeWidthIntentKind::AdditiveCssPx && plan.intent.value < 0.0) {
            auto const at_floor = [&](StrokeWidthStyle const &style) {
                return !stroke_width_can_decrease(style.effective_px.value_or(style.local_computed),
                                                 -plan.intent.value);
            };
            for (auto const &member : plan.members) {
                if (member.target.kind == StrokeWidthTargetKind::Shape &&
                    member.outcome == StrokeWidthMemberOutcome::Unchanged &&
                    !member.target.runs.empty() && at_floor(member.target.runs[0].style)) {
                    return _("Already at the minimum step");
                }
                for (auto const &run : member.text_runs) {
                    if (run.eligibility == StrokeWidthEligibility::Eligible &&
                        run.outcome == StrokeWidthMemberOutcome::Unchanged && at_floor(run.style)) {
                        return _("Already at the minimum step");
                    }
                }
            }
        }
        auto note = coverage_note();
        if (others || plan.skipped_runs) {
            if (!note.empty()) note += "; ";
            note += _("Stroke width: incompatible or protected items were skipped");
        }
        return note;
    }
    // Removal leaves every changed member without stroke paint by design, so
    // that outcome is not reported as a warning and the prefix names the removal.
    bool const removal = plan.intent.kind == StrokeWidthIntentKind::RemoveStroke;
    std::vector<std::string> parts;
    if (paint_none && !removal) {
        char *text = g_strdup_printf(
            ngettext("%zu object has no stroke colour", "%zu objects have no stroke colour", paint_none),
            paint_none);
        parts.emplace_back(text);
        g_free(text);
    }
    if (following) {
        char *text = g_strdup_printf(
            ngettext("%zu linked clone follows its original", "%zu linked clones follow their original", following),
            following);
        parts.emplace_back(text);
        g_free(text);
    }
    auto const coverage = coverage_note();
    if (!coverage.empty()) parts.push_back(coverage);
    if (others || plan.skipped_runs) parts.emplace_back(_("incompatible or protected items were skipped"));
    if (parts.empty()) return {};
    std::string note = removal ? _("Stroke removed") : _("Stroke width applied");
    for (auto const &part : parts) note += "; " + part;
    return note;
}

} // namespace

std::string stroke_width_applied_note(StrokeWidthPlan const &plan, std::size_t excluded)
{
    bool const changes = plan.planned_changes || std::any_of(plan.members.begin(), plan.members.end(),
        [](auto const &member) { return member.target.kind == StrokeWidthTargetKind::TextOwner &&
            text_owner_outcome(member) == StrokeWidthMemberOutcome::Change; });
    return applied_note(plan, excluded, changed_paint_none(plan, true, true), changes);
}

std::string stroke_width_applied_note(StrokeWidthPlan const &plan, StrokeWidthApplyResult const &result)
{
    if (result.state == StrokeWidthApplyState::Failed || result.state == StrokeWidthApplyState::Rejected) return {};
    return applied_note(plan, result.excluded, result.paint_none, result.state == StrokeWidthApplyState::Applied);
}

} // namespace Inkscape::UI
