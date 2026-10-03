// SPDX-License-Identifier: GPL-2.0-or-later

#include "bitmap-adjustment-chemistry.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string_view>

#include <glib.h>

#include "display/nr-filter-component-transfer.h"
#include "display/nr-filter-clipping-warning.h"
#include "display/nr-filter.h"
#include "filter-chemistry.h"
#include "object/filters/sp-filter-primitive.h"
#include "object/sp-defs.h"
#include "object/sp-filter.h"
#include "object/sp-image.h"
#include "object/sp-item.h"
#include "object/sp-use.h"
#include "style.h"
#include "svg/stringstream.h"
#include "xml/node.h"
#include "xml/repr.h"

namespace Inkscape::BitmapAdjustments {
namespace {

constexpr std::array<std::pair<char const *, double Filters::BitmapToneSettings::*>, 6> parameters = {{
    {"inkscape:brightness", &Filters::BitmapToneSettings::brightness},
    {"inkscape:contrast", &Filters::BitmapToneSettings::contrast},
    {"inkscape:intensity", &Filters::BitmapToneSettings::intensity},
    {"inkscape:highlights", &Filters::BitmapToneSettings::highlights},
    {"inkscape:shadows", &Filters::BitmapToneSettings::shadows},
    {"inkscape:midtones", &Filters::BitmapToneSettings::midtones}
}};

std::string serialize_table(Filters::BitmapToneTable const &table)
{
    Inkscape::SVGOStringStream output;
    output.precision(8);
    bool first = true;
    for (auto value : table) {
        if (!first) output << ' ';
        first = false;
        output << value;
    }
    return output.str();
}

double read_number(Inkscape::XML::Node const *repr, char const *name, bool &valid)
{
    auto const value = repr ? repr->attribute(name) : nullptr;
    if (!value) {
        valid = false;
        return 0.0;
    }
    char *end = nullptr;
    auto const number = g_ascii_strtod(value, &end);
    if (end == value || (end && *end) || !std::isfinite(number)) {
        valid = false;
        return 0.0;
    }
    return number;
}

Inkscape::XML::Node *create_function(Inkscape::XML::Document *document, char const *name,
                                     char const *table)
{
    auto function = document->createElement(name);
    function->setAttribute("type", "table");
    function->setAttribute("tableValues", table);
    return function;
}

void replace_transfer_children(Inkscape::XML::Node *repr, char const *table)
{
    while (auto child = repr->firstChild()) {
        sp_repr_unparent(child);
    }

    auto document = repr->document();
    for (auto name : {"svg:feFuncR", "svg:feFuncG", "svg:feFuncB"}) {
        auto function = create_function(document, name, table);
        repr->appendChild(function);
        Inkscape::GC::release(function);
    }
    auto alpha = document->createElement("svg:feFuncA");
    alpha->setAttribute("type", "identity");
    repr->appendChild(alpha);
    Inkscape::GC::release(alpha);
}

bool settings_equal(Filters::BitmapToneSettings const &a, Filters::BitmapToneSettings const &b)
{
    return bitmap_tone_settings_equal(a, b);
}

void configure_tone_renderer(Filters::FilterComponentTransfer &primitive,
                             Filters::BitmapToneSettings const &settings)
{
    auto const table = build_bitmap_tone_table(settings);
    for (int channel = 0; channel < 3; ++channel) {
        primitive.type[channel] = Filters::COMPONENTTRANSFER_TYPE_TABLE;
        primitive.tableValues[channel].assign(table.begin(), table.end());
    }
    primitive.type[3] = Filters::COMPONENTTRANSFER_TYPE_IDENTITY;
    primitive.set_color_interpolation(SP_CSS_COLOR_INTERPOLATION_SRGB);
}

} // namespace

bool is_managed_tone_primitive(SPFilterPrimitive const *primitive)
{
    if (!primitive || !primitive->getRepr()) return false;
    auto const marker = primitive->getRepr()->attribute(TONE_MARKER_ATTRIBUTE);
    return marker && std::string_view(marker) == TONE_MARKER_VALUE;
}

SPFilterPrimitive *find_managed_tone_primitive(SPFilter *filter)
{
    if (!filter) return nullptr;
    for (auto &child : filter->children) {
        auto primitive = cast<SPFilterPrimitive>(&child);
        if (is_managed_tone_primitive(primitive)) return primitive;
    }
    return nullptr;
}

std::optional<Filters::BitmapToneSettings> query_tone(SPItem const *item)
{
    if (!item || !item->style || !item->style->filter.set) return std::nullopt;
    auto filter = item->style->getFilter();
    auto primitive = find_managed_tone_primitive(filter);
    if (!primitive) return std::nullopt;

    Filters::BitmapToneSettings settings;
    bool valid = true;
    for (auto const &[name, member] : parameters) {
        settings.*member = read_number(primitive->getRepr(), name, valid);
    }
    if (!valid) return std::nullopt;
    return settings.clamped();
}

bool reset_tone(SPItem *item)
{
    if (!item || !item->style || !item->style->filter.set) return false;
    auto filter = item->style->getFilter();
    auto primitive = find_managed_tone_primitive(filter);
    if (!filter || !primitive) return false;

    filter = ensure_private_filter_for_item(item);
    primitive = find_managed_tone_primitive(filter);
    if (!primitive) return false;
    sp_repr_unparent(primitive->getRepr());

    if (filter->primitive_count() == 0) {
        remove_filter(item, false);
        // ensure_private_filter_for_item() guarantees that this item owns the
        // filter being edited. Remove the now-empty definition as well, but
        // only after the style reference has been detached and only when no
        // other reference remains. This keeps repeated adjust/reset cycles
        // from accumulating empty filters in <defs> while remaining safe if a
        // reference was added concurrently by another document operation.
        filter->collectOrphan();
    } else {
        filter->invalidate_slots();
        filter->update_filter_region(item);
    }
    item->requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG | SP_OBJECT_STYLE_MODIFIED_FLAG);
    return true;
}

bool usable_bitmap(SPImage const *image)
{
    return image && image->pixbuf && !image->missing;
}

bool is_embedded(SPImage const *image)
{
    return image && image->href && !std::strncmp(image->href, "data:", 5);
}

Filters::BitmapToneSettings canonical_tone(SPItem const *item)
{
    return query_tone(item).value_or(Filters::BitmapToneSettings{});
}

Util::OperationTargets<SPItem>
resolve_tone_targets(std::vector<SPItem *> const &selected, std::function<bool(SPItem *)> const &available)
{
    return Util::resolve_composite_targets(selected,
        [&available](SPItem *item) {
            if (!available(item)) return Util::TargetAvailability::Unavailable;
            if (auto image = cast<SPImage>(item); image && !usable_bitmap(image)) {
                return Util::TargetAvailability::MissingSource;
            }
            return Util::TargetAvailability::Eligible;
        },
        [](SPItem *item) -> SPItem * {
            for (auto parent = item->parent; parent; parent = parent->parent) {
                if (auto result = cast<SPItem>(parent)) return result;
            }
            return nullptr;
        },
        [](SPItem *item) -> SPItem * {
            if (auto clone = cast<SPUse>(item)) return clone->trueOriginal();
            return nullptr;
        });
}

std::array<Filters::BitmapToneAggregateValue, Filters::BITMAP_TONE_PROPERTY_COUNT>
aggregate_tone(std::vector<SPItem *> const &targets)
{
    std::array<Filters::BitmapToneAggregateValue, Filters::BITMAP_TONE_PROPERTY_COUNT> tone{};
    if (targets.empty()) return tone;
    auto const first = canonical_tone(targets.front());
    for (std::size_t i = 0; i < Filters::BITMAP_TONE_PROPERTY_COUNT; ++i) {
        auto const property = static_cast<Filters::BitmapToneProperty>(i);
        tone[i].value = Filters::get_bitmap_tone_property(first, property);
        for (std::size_t j = 1; j < targets.size(); ++j) {
            auto const value = Filters::get_bitmap_tone_property(canonical_tone(targets[j]), property);
            auto const scale = std::max({1.0, std::abs(tone[i].value), std::abs(value)});
            if (std::abs(tone[i].value - value) > 1e-9 * scale) {
                tone[i].mixed = true;
                break;
            }
        }
    }
    return tone;
}

bool apply_tone(SPItem *item, Filters::BitmapToneSettings const &raw_settings)
{
    if (!item) return false;
    auto const settings = raw_settings.clamped();
    if (settings.is_neutral()) return reset_tone(item);

    if (auto existing = query_tone(item); existing && settings_equal(*existing, settings)) {
        return false;
    }

    auto filter = ensure_private_filter_for_item(item);
    if (!filter) return false;
    auto primitive = find_managed_tone_primitive(filter);
    if (!primitive) {
        primitive = filter_add_primitive(filter, Filters::NR_FILTER_COMPONENTTRANSFER);
        primitive->getRepr()->setAttribute(TONE_MARKER_ATTRIBUTE, TONE_MARKER_VALUE);
    }

    auto repr = primitive->getRepr();
    // Filter primitives are owned by their parent filter, not referenced
    // independently. Marking one as collectible makes collectOrphans() remove
    // it when an Undo checkpoint is created, leaving an empty, invisible filter.
    // Also repair documents produced by early development builds.
    repr->removeAttribute("inkscape:collect");
    for (auto const &[name, member] : parameters) {
        repr->setAttributeSvgDouble(name, settings.*member);
    }
    auto css = sp_repr_css_attr(repr, "style");
    sp_repr_css_set_property(css, "color-interpolation-filters", "sRGB");
    sp_repr_css_change(repr, css, "style");
    sp_repr_css_attr_unref(css);

    auto const table = build_bitmap_tone_table(settings);
    auto const serialized = serialize_table(table);
    replace_transfer_children(repr, serialized.c_str());

    filter->invalidate_slots();
    filter->update_filter_region(item);
    sp_style_set_property_url(item, "filter", filter, false);
    item->requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG | SP_OBJECT_STYLE_MODIFIED_FLAG);
    return true;
}

std::unique_ptr<Filters::FilterPrimitive>
build_tone_renderer_primitive(Filters::BitmapToneSettings const &settings)
{
    auto primitive = std::make_unique<Filters::FilterComponentTransfer>();
    configure_tone_renderer(*primitive, settings);
    return primitive;
}

std::unique_ptr<Filters::Filter>
build_tone_preview_renderer(SPItem const *item, DrawingItem *drawing_item,
                            std::optional<Filters::BitmapToneSettings> const &settings,
                            bool show_clipping)
{
    SPFilter *filter = nullptr;
    SPFilterPrimitive *managed = nullptr;
    if (item && item->style && item->style->filter.set) {
        filter = item->style->getFilter();
        managed = find_managed_tone_primitive(filter);
    }

    auto const transient = settings && !settings->clamped().is_neutral();
    if (!filter && !transient && !show_clipping) return nullptr;

    std::unique_ptr<Filters::Filter> renderer;
    if (filter) {
        // Replace the managed primitive in-place. Keeping its canonical input,
        // result slot, subregion and position is required for preview/commit
        // parity when later primitives consume its named result.
        renderer = filter->build_renderer(drawing_item, {}, [drawing_item, &settings](
            SPFilterPrimitive const *primitive) -> std::unique_ptr<Filters::FilterPrimitive> {
            if (!is_managed_tone_primitive(primitive)) return nullptr;
            auto replacement = primitive->build_renderer(drawing_item);
            if (auto transfer = dynamic_cast<Filters::FilterComponentTransfer *>(
                    replacement.get())) {
                configure_tone_renderer(*transfer,
                    settings.value_or(Filters::BitmapToneSettings{}));
            }
            return replacement;
        });
    } else {
        renderer = std::make_unique<Filters::Filter>((transient ? 1 : 0) +
                                                     (show_clipping ? 1 : 0));
    }
    if (!managed && transient) renderer->add_primitive(build_tone_renderer_primitive(*settings));
    if (show_clipping) {
        renderer->add_primitive(std::make_unique<Filters::FilterClippingWarning>());
    }
    return renderer;
}

} // namespace Inkscape::BitmapAdjustments
