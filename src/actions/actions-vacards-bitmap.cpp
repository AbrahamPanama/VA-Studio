// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * VACards agent command line: bitmap tone queries and adjustments actions.
 *
 * Two read-only actions:
 *   vacards-bitmap-tone-query -- collective-compositing report of the tone
 *     settings of the selection's composite targets (legacy per-composite-root
 *     policy, the documented SELECTION_CONTRACT exception, resolved exactly as
 *     the Bitmap Adjustments panel via resolve_tone_targets);
 *   vacards-bitmap-histogram  -- luminance histogram of exactly one usable
 *     bitmap, optionally tone-remapped.
 * Neither action changes the document, the selection, Undo history or
 * preferences. Availability is document-based (hidden or locked ancestors),
 * which differs from the GUI's desktop-based test on purpose (see
 * actions-vacards-cli.h).
 */

#define BOOST_JSON_NO_LIB

#include "actions-vacards-bitmap.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <boost/json.hpp>

#include <glibmm/i18n.h>

#include "actions-vacards-cli.h"
#include "vacards-cli-result.h"
#include "bitmap-adjustment-chemistry.h"
#include "display/bitmap-histogram.h"
#include "display/bitmap-tone.h"
#include "display/cairo-utils.h"
#include "inkscape-application.h"
#include "object/sp-image.h"
#include "object/sp-item.h"
#include "selection.h"

namespace Inkscape::VACardsCli {
namespace {

/// Property names in BitmapToneProperty order; one table drives both actions.
struct TonePropertyName
{
    std::string_view name;
    Filters::BitmapToneProperty property;
};

constexpr TonePropertyName tone_properties[] = {
    {"brightness", Filters::BitmapToneProperty::Brightness},
    {"contrast", Filters::BitmapToneProperty::Contrast},
    {"intensity", Filters::BitmapToneProperty::Intensity},
    {"highlights", Filters::BitmapToneProperty::Highlights},
    {"shadows", Filters::BitmapToneProperty::Shadows},
    {"midtones", Filters::BitmapToneProperty::Midtones},
};

std::string item_id(SPItem const *item)
{
    if (item) {
        if (char const *id = item->getId()) {
            return id;
        }
    }
    return "";
}

double tone_value(Filters::BitmapToneSettings const &settings, Filters::BitmapToneProperty property)
{
    return Filters::get_bitmap_tone_property(settings, property);
}

constexpr ActionSpec tone_query_spec{.name = "vacards-bitmap-tone-query", .mode = "read-only",
    .summary = "Report the bitmap tone settings of the selection's composite targets without changing anything."};

void tone_query(Glib::VariantBase const &value, InkscapeApplication *app)
{
    run_action(tone_query_spec, value, app, [](ActionContext &c) {
        Record &r = c.record;

        if (!c.selection) {
            r.status = Status::Rejected;
            r.reason = "empty-selection";
            r.message = "Select at least one object.";
            return;
        }

        std::vector<SPItem *> selected;
        for (auto *item : c.selection->items()) {
            selected.push_back(item);
        }
        r.selected = static_cast<int>(selected.size());

        auto const resolved = BitmapAdjustments::resolve_tone_targets(
            selected, [](SPItem *item) { return Inkscape::VACardsCli::document_available(item); });
        r.eligible = static_cast<int>(resolved.items.size());
        r.covered = static_cast<int>(resolved.covered);

        // Distinct selected items, in selection order: unavailable, or an image without usable pixels.
        std::vector<SPItem *> seen;
        for (auto *item : selected) {
            bool duplicate = false;
            for (auto *previous : seen) {
                if (previous == item) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) {
                continue;
            }
            seen.push_back(item);
            if (!Inkscape::VACardsCli::document_available(item)) {
                r.excluded.push_back({item_id(item), "unavailable"});
            } else if (auto *image = cast<SPImage>(item); image && !BitmapAdjustments::usable_bitmap(image)) {
                r.excluded.push_back({item_id(item), "missing-source"});
            }
        }

        if (selected.empty()) {
            r.status = Status::Rejected;
            r.reason = "empty-selection";
            r.message = "Select at least one object.";
            return;
        }
        if (resolved.items.empty()) {
            r.status = Status::Rejected;
            r.reason = "no-eligible-targets";
            r.message = "No selected object can take a bitmap tone adjustment.";
            return;
        }

        r.status = Status::Ok;
        r.reason = "success";
        r.message = "Reported tone settings of " + std::to_string(resolved.items.size()) + " target(s).";

        boost::json::array targets;
        targets.reserve(resolved.items.size());
        for (auto *item : resolved.items) {
            auto const settings = BitmapAdjustments::canonical_tone(item);
            boost::json::object target;
            target["id"] = item_id(item);
            target["type"] = cast<SPImage>(item) ? "image" : "item";
            target["managed"] = BitmapAdjustments::query_tone(item).has_value();
            for (auto const &[name, property] : tone_properties) {
                target[boost::json::string_view(name.data(), name.size())] = tone_value(settings, property);
            }
            targets.push_back(std::move(target));
        }
        r.data["targets"] = std::move(targets);

        auto const aggregate = BitmapAdjustments::aggregate_tone(resolved.items);
        boost::json::object aggregate_object;
        for (auto const &[name, property] : tone_properties) {
            auto const index = static_cast<std::size_t>(property);
            boost::json::object entry;
            entry["value"] = aggregate[index].value;
            entry["mixed"] = aggregate[index].mixed;
            aggregate_object[boost::json::string_view(name.data(), name.size())] = std::move(entry);
        }
        r.data["aggregate"] = std::move(aggregate_object);
    });
}

constexpr std::string_view remap_choices[] = {"none", "current"};

constexpr ParamSpec histogram_params[] = {
    {.key = "remap", .type = ParamType::Choice, .default_value = "none", .choices = remap_choices,
     .help = "none: source pixels; current: apply the bitmap's current tone before counting."},
    {.key = "brightness", .type = ParamType::Number, .min = -100, .max = 100,
     .help = "Override this tone value for the remapped histogram."},
    {.key = "contrast", .type = ParamType::Number, .min = -100, .max = 100,
     .help = "Override this tone value for the remapped histogram."},
    {.key = "intensity", .type = ParamType::Number, .min = -100, .max = 100,
     .help = "Override this tone value for the remapped histogram."},
    {.key = "highlights", .type = ParamType::Number, .min = -100, .max = 100,
     .help = "Override this tone value for the remapped histogram."},
    {.key = "shadows", .type = ParamType::Number, .min = -100, .max = 100,
     .help = "Override this tone value for the remapped histogram."},
    {.key = "midtones", .type = ParamType::Number, .min = -100, .max = 100,
     .help = "Override this tone value for the remapped histogram."},
};

constexpr ActionSpec histogram_spec{.name = "vacards-bitmap-histogram", .mode = "read-only",
    .summary = "Report the luminance histogram of exactly one usable bitmap, optionally tone-remapped.",
    .params = histogram_params};

void histogram(Glib::VariantBase const &value, InkscapeApplication *app)
{
    run_action(histogram_spec, value, app, [](ActionContext &c) {
        Record &r = c.record;

        if (!c.selection) {
            r.status = Status::Rejected;
            r.reason = "empty-selection";
            r.message = "Select at least one object.";
            return;
        }

        std::vector<SPItem *> selected;
        for (auto *item : c.selection->items()) {
            selected.push_back(item);
        }
        r.selected = static_cast<int>(selected.size());

        if (selected.size() != 1) {
            r.status = Status::Rejected;
            r.reason = "requires-single-bitmap";
            r.message = "Select exactly one bitmap.";
            return;
        }

        SPItem *item = selected.front();
        if (!Inkscape::VACardsCli::document_available(item)) {
            r.status = Status::Rejected;
            r.reason = "unavailable";
            r.message = "The bitmap is hidden or locked.";
            r.excluded.push_back({item_id(item), "unavailable"});
            return;
        }

        auto *image = cast<SPImage>(item);
        if (!image) {
            r.status = Status::Rejected;
            r.reason = "requires-single-bitmap";
            r.message = "Select exactly one bitmap.";
            return;
        }

        if (!BitmapAdjustments::usable_bitmap(image)) {
            r.status = Status::Rejected;
            r.reason = "missing-source";
            r.message = "The bitmap has no usable source pixels.";
            r.excluded.push_back({item_id(item), "missing-source"});
            return;
        }

        r.eligible = 1;

        auto const source = Filters::build_bitmap_histogram(*image->pixbuf);

        std::string const remap = c.params.at("remap").text;
        Filters::BitmapToneSettings settings{};
        if (remap == "current") {
            settings = BitmapAdjustments::canonical_tone(image);
        }
        bool remapped = (remap == "current");
        for (auto const &[name, property] : tone_properties) {
            if (c.params.has(name)) {
                Filters::set_bitmap_tone_property(settings, property, c.params.at(name).number);
                remapped = true;
            }
        }

        auto const result = remapped ? Filters::remap_bitmap_histogram(source, settings) : source;

        r.status = Status::Ok;
        r.reason = "success";
        r.message = "Histogram of " + item_id(item) + ".";

        auto &d = r.data;
        d["image"] = item_id(item);
        d["embedded"] = BitmapAdjustments::is_embedded(image);
        d["pixel_width"] = static_cast<std::uint64_t>(image->pixbuf->width());
        d["pixel_height"] = static_cast<std::uint64_t>(image->pixbuf->height());
        d["remapped"] = remapped;

        boost::json::object settings_object;
        for (auto const &[name, property] : tone_properties) {
            settings_object[boost::json::string_view(name.data(), name.size())] = tone_value(settings, property);
        }
        d["settings"] = std::move(settings_object);

        boost::json::array luminance;
        luminance.reserve(result.luminance.size());
        for (auto const count : result.luminance) {
            luminance.push_back(static_cast<std::uint64_t>(count));
        }
        d["luminance"] = std::move(luminance);
        d["sampled_pixels"] = static_cast<std::uint64_t>(result.sampled_pixels);
        d["transparent_pixels"] = static_cast<std::uint64_t>(result.transparent_pixels);
        d["shadow_clipped"] = static_cast<std::uint64_t>(result.shadow_clipped);
        d["highlight_clipped"] = static_cast<std::uint64_t>(result.highlight_clipped);
    });
}

} // namespace
} // namespace Inkscape::VACardsCli

std::vector<std::vector<Glib::ustring>> raw_data_vacards_bitmap = {
    {"app.vacards-bitmap-tone-query", N_("VACards Bitmap Tone Query"), N_("VACards"),
     N_("Report the bitmap tone settings of the selection")},
    {"app.vacards-bitmap-histogram", N_("VACards Bitmap Histogram"), N_("VACards"),
     N_("Report the luminance histogram of one bitmap")}};

void add_actions_vacards_bitmap(InkscapeApplication *app)
{
    auto *gapp = app->gio_app();
    Glib::VariantType String(Glib::VARIANT_TYPE_STRING);
    gapp->add_action_with_parameter("vacards-bitmap-tone-query", String,
        sigc::bind(sigc::ptr_fun(&Inkscape::VACardsCli::tone_query), app));
    gapp->add_action_with_parameter("vacards-bitmap-histogram", String,
        sigc::bind(sigc::ptr_fun(&Inkscape::VACardsCli::histogram), app));

    Inkscape::VACardsCli::register_action_spec(Inkscape::VACardsCli::tone_query_spec);
    Inkscape::VACardsCli::register_action_spec(Inkscape::VACardsCli::histogram_spec);

    app->get_action_extra_data().add_data(raw_data_vacards_bitmap);
}

/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0)(case-label . +))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4 :
