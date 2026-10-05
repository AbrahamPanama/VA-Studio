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

void tone_query_body(ActionContext &c)
{
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
}

void tone_query(Glib::VariantBase const &value, InkscapeApplication *app)
{
    run_action(tone_query_spec, value, app, tone_query_body);
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

void histogram_body(ActionContext &c)
{
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
}

void histogram(Glib::VariantBase const &value, InkscapeApplication *app)
{
    run_action(histogram_spec, value, app, histogram_body);
}

} // namespace
ActionSpec tone_query_command() { auto s = tone_query_spec; s.handler = tone_query_body; return s; }
ActionSpec histogram_command() { auto s = histogram_spec; s.handler = histogram_body; return s; }

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

#include "vacards-cli-production.h"
#include <boost/json.hpp>
namespace Inkscape::VACardsCli {
namespace {
// | `bitmap.tone-query` | ids; `mode=compatible-members|legacy-roots`, default legacy-roots for existing canonical compatibility; pagination | per-target six values, mixed flags/counts/exclusions, declared mode | T | Q / none / same query / Q; explicit mode prevents changing the existing root query silently |
TypeDescriptor const m3_0{boost::json::parse(R"m3({
  "type": "object",
  "properties": {
    "ids": {
      "type": "array",
      "items": {
        "type": "string",
        "minLength": 1,
        "maxLength": 128
      },
      "minItems": 1,
      "maxItems": 100000,
      "uniqueItems": true
    },
    "mode": {
      "type": "string",
      "enum": [
        "compatible-members",
        "legacy-roots"
      ],
      "default": "legacy-roots"
    },
    "limit": {
      "type": "integer",
      "minimum": 1,
      "maximum": 1000,
      "default": 100
    },
    "cursor": {
      "type": "string",
      "minLength": 1,
      "maxLength": 128
    }
  },
  "required": [
    "ids"
  ],
  "additionalProperties": false,
  "x-m3-contract": {
    "guard_domain": "document",
    "guard_required": false,
    "needs_document": true,
    "selection_mode": "request-selected-compatible-members-or-legacy-roots",
    "target_cardinality": {
      "min": 1,
      "max": 100000
    },
    "normalization": "deduplicate roots then recursive eligible image members once; legacy-roots query does not recurse",
    "partial_policy": "preserve-and-report-exclusions",
    "role_order": "input-order; each normalized root once",
    "normalization_order": [
      "validate raw schema and exclusive route",
      "select route",
      "fill defaults recursively only in selected branch; pivot suppresses anchor, size suppresses dpi, token suppresses recipe/analyze/solve",
      "parse exact integers",
      "convert all lengths including nested recipes to CSS px and enforce bounds",
      "resolve and normalize targets",
      "canonicalize normalized params for token binding"
    ],
    "native_service": "`resolve_tone_targets/canonical_tone/aggregate_tone`, `src/bitmap-adjustment-chemistry.cpp:193,187,216`; new member resolver is unavailable",
    "warnings": [
      {
        "code": "exclusions",
        "emit_site": "`resolve_tone_targets/canonical_tone/aggregate_tone`, `src/bitmap-adjustment-chemistry.cpp:193,187,216`; new member resolver is unavailable; after target resolution, nonempty exclusions"
      }
    ],
    "error_rows": [
      {
        "layer": "transport/schema",
        "native_branch": "parse_request: request-too-large",
        "code": "request-too-large",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "parse_request: request-too-large",
        "native_evidence": "`resolve_tone_targets/canonical_tone/aggregate_tone`, `src/bitmap-adjustment-chemistry.cpp:193,187,216`; new member resolver is unavailable",
        "oracle": "P9:bitmap.tone-query:transport/schema:request-too-large:parse_request: request-too-large",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "transport/schema",
        "native_branch": "parse_request: repeated-key",
        "code": "repeated-key",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "parse_request: repeated-key",
        "native_evidence": "`resolve_tone_targets/canonical_tone/aggregate_tone`, `src/bitmap-adjustment-chemistry.cpp:193,187,216`; new member resolver is unavailable",
        "oracle": "P9:bitmap.tone-query:transport/schema:repeated-key:parse_request: repeated-key",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "transport/schema",
        "native_branch": "parse_request: invalid-utf8",
        "code": "invalid-utf8",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "parse_request: invalid-utf8",
        "native_evidence": "`resolve_tone_targets/canonical_tone/aggregate_tone`, `src/bitmap-adjustment-chemistry.cpp:193,187,216`; new member resolver is unavailable",
        "oracle": "P9:bitmap.tone-query:transport/schema:invalid-utf8:parse_request: invalid-utf8",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "transport/schema",
        "native_branch": "parse_request: malformed-json",
        "code": "malformed-json",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "parse_request: malformed-json",
        "native_evidence": "`resolve_tone_targets/canonical_tone/aggregate_tone`, `src/bitmap-adjustment-chemistry.cpp:193,187,216`; new member resolver is unavailable",
        "oracle": "P9:bitmap.tone-query:transport/schema:malformed-json:parse_request: malformed-json",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "transport/schema",
        "native_branch": "raw schema/type/range/route/uniqueItems violation",
        "code": "invalid-argument",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "raw schema/type/range/route/uniqueItems violation",
        "native_evidence": "`resolve_tone_targets/canonical_tone/aggregate_tone`, `src/bitmap-adjustment-chemistry.cpp:193,187,216`; new member resolver is unavailable",
        "oracle": "P9:bitmap.tone-query:transport/schema:invalid-argument:raw schema/type/range/route/uniqueItems violation",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "missing current document",
        "code": "no-document",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "missing current document",
        "native_evidence": "`resolve_tone_targets/canonical_tone/aggregate_tone`, `src/bitmap-adjustment-chemistry.cpp:193,187,216`; new member resolver is unavailable",
        "oracle": "P9:bitmap.tone-query:identity/admission:no-document:missing current document",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "document identity mismatch",
        "code": "stale-document",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "document identity mismatch",
        "native_evidence": "`resolve_tone_targets/canonical_tone/aggregate_tone`, `src/bitmap-adjustment-chemistry.cpp:193,187,216`; new member resolver is unavailable",
        "oracle": "P9:bitmap.tone-query:identity/admission:stale-document:document identity mismatch",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "supplied guard differs from document revision",
        "code": "stale-revision",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "supplied guard differs from document revision",
        "native_evidence": "`resolve_tone_targets/canonical_tone/aggregate_tone`, `src/bitmap-adjustment-chemistry.cpp:193,187,216`; new member resolver is unavailable",
        "oracle": "P9:bitmap.tone-query:identity/admission:stale-revision:supplied guard differs from document revision",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "cancellation observed before commit",
        "code": "cancelled",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "cancellation observed before commit",
        "native_evidence": "`resolve_tone_targets/canonical_tone/aggregate_tone`, `src/bitmap-adjustment-chemistry.cpp:193,187,216`; new member resolver is unavailable",
        "oracle": "P9:bitmap.tone-query:identity/admission:cancelled:cancellation observed before commit",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "explicit ID cannot resolve",
        "code": "unknown-id",
        "retryable": false,
        "mutation_state": "none",
        "route": "explicit-target route",
        "detail.reason": "explicit ID cannot resolve",
        "native_evidence": "`resolve_tone_targets/canonical_tone/aggregate_tone`, `src/bitmap-adjustment-chemistry.cpp:193,187,216`; new member resolver is unavailable",
        "oracle": "P9:bitmap.tone-query:identity/admission:unknown-id:explicit ID cannot resolve",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "resolved member set empty",
        "code": "no-eligible-targets",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "resolved member set empty",
        "native_evidence": "`resolve_tone_targets/canonical_tone/aggregate_tone`, `src/bitmap-adjustment-chemistry.cpp:193,187,216`; new member resolver is unavailable",
        "oracle": "P9:bitmap.tone-query:command-service:no-eligible-targets:resolved member set empty",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "cursor binding differs",
        "code": "invalid-cursor",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "cursor binding differs",
        "native_evidence": "`resolve_tone_targets/canonical_tone/aggregate_tone`, `src/bitmap-adjustment-chemistry.cpp:193,187,216`; new member resolver is unavailable",
        "oracle": "P9:bitmap.tone-query:command-service:invalid-cursor:cursor binding differs",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "unexpected service exception; rollback before return",
        "code": "internal-error",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "unexpected service exception; rollback before return",
        "native_evidence": "`resolve_tone_targets/canonical_tone/aggregate_tone`, `src/bitmap-adjustment-chemistry.cpp:193,187,216`; new member resolver is unavailable",
        "oracle": "P9:bitmap.tone-query:command-service:internal-error:unexpected service exception; rollback before return",
        "evidence_status": "required-unimplemented"
      }
    ],
    "success_native_branches": [
      "native successful outcome",
      "native unchanged outcome when applicable",
      "read-only computed preparation"
    ],
    "variant_applicability": {
      "success": "ok or changed",
      "unchanged": "native no-op only; query/preparation/history empty are not successful unchanged",
      "computed-dry-run": "successful read-only preflight; no retention, publication, Undo or Redo change"
    },
    "success_rows": [
      {
        "native_branch": "native successful outcome",
        "code": null,
        "retryable": false,
        "mutation_state": "none",
        "data_variant": "success",
        "evidence_route": "P9:bitmap.tone-query:native:native successful outcome",
        "evidence_status": "required-unimplemented"
      },
      {
        "native_branch": "native unchanged outcome when applicable",
        "code": null,
        "retryable": false,
        "mutation_state": "none",
        "data_variant": "unchanged",
        "evidence_route": "P9:bitmap.tone-query:native:native unchanged outcome when applicable",
        "evidence_status": "required-unimplemented"
      },
      {
        "native_branch": "read-only computed preparation",
        "code": null,
        "retryable": false,
        "mutation_state": "none",
        "data_variant": "computed-dry-run",
        "evidence_route": "P9:bitmap.tone-query:native:read-only computed preparation",
        "evidence_status": "required-unimplemented"
      }
    ]
  }
})m3").as_object()};
// | `bitmap.histogram` | exactly one image id; `channel=luminance`, `bins=256` (both singleton supported enums); `remap=none|current` default none; existing six optional tone override values | 256 counts, total samples, image-pixel dimensions, remap | T, requires-single-bitmap | Q / none / same query / Q single image; no invented RGB-channel engine |
TypeDescriptor const m3_1{boost::json::parse(R"m3({
  "type": "object",
  "properties": {
    "ids": {
      "type": "array",
      "items": {
        "type": "string",
        "minLength": 1,
        "maxLength": 128
      },
      "minItems": 1,
      "maxItems": 1,
      "uniqueItems": true
    },
    "channel": {
      "type": "string",
      "enum": [
        "luminance"
      ],
      "default": "luminance"
    },
    "bins": {
      "type": "integer",
      "enum": [
        256
      ],
      "default": 256
    },
    "remap": {
      "type": "string",
      "enum": [
        "none",
        "current"
      ],
      "default": "none",
      "description": "none = identity baseline; current = current-tone baseline; any supplied channel override triggers remapping, including none. Counts sum to sampled total, not source pixel count."
    },
    "brightness": {
      "type": "number",
      "minimum": -100,
      "maximum": 100
    },
    "contrast": {
      "type": "number",
      "minimum": -100,
      "maximum": 100
    },
    "intensity": {
      "type": "number",
      "minimum": -100,
      "maximum": 100
    },
    "highlights": {
      "type": "number",
      "minimum": -100,
      "maximum": 100
    },
    "shadows": {
      "type": "number",
      "minimum": -100,
      "maximum": 100
    },
    "midtones": {
      "type": "number",
      "minimum": -100,
      "maximum": 100
    }
  },
  "required": [
    "ids"
  ],
  "additionalProperties": false,
  "x-m3-contract": {
    "guard_domain": "document",
    "guard_required": false,
    "needs_document": true,
    "selection_mode": "single-object",
    "target_cardinality": {
      "min": 1,
      "max": 1
    },
    "normalization": "one explicit root or retained target; no member expansion",
    "partial_policy": "reject-any-incompatible",
    "role_order": "input-order; each normalized root once",
    "normalization_order": [
      "validate raw schema and exclusive route",
      "select route",
      "fill defaults recursively only in selected branch; pivot suppresses anchor, size suppresses dpi, token suppresses recipe/analyze/solve",
      "parse exact integers",
      "convert all lengths including nested recipes to CSS px and enforce bounds",
      "resolve and normalize targets",
      "canonicalize normalized params for token binding"
    ],
    "native_service": "`build_bitmap_histogram/remap_bitmap_histogram`, `src/display/bitmap-histogram.cpp:43,81`",
    "warnings": [],
    "error_rows": [
      {
        "layer": "transport/schema",
        "native_branch": "parse_request: request-too-large",
        "code": "request-too-large",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "parse_request: request-too-large",
        "native_evidence": "`build_bitmap_histogram/remap_bitmap_histogram`, `src/display/bitmap-histogram.cpp:43,81`",
        "oracle": "P9:bitmap.histogram:transport/schema:request-too-large:parse_request: request-too-large",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "transport/schema",
        "native_branch": "parse_request: repeated-key",
        "code": "repeated-key",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "parse_request: repeated-key",
        "native_evidence": "`build_bitmap_histogram/remap_bitmap_histogram`, `src/display/bitmap-histogram.cpp:43,81`",
        "oracle": "P9:bitmap.histogram:transport/schema:repeated-key:parse_request: repeated-key",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "transport/schema",
        "native_branch": "parse_request: invalid-utf8",
        "code": "invalid-utf8",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "parse_request: invalid-utf8",
        "native_evidence": "`build_bitmap_histogram/remap_bitmap_histogram`, `src/display/bitmap-histogram.cpp:43,81`",
        "oracle": "P9:bitmap.histogram:transport/schema:invalid-utf8:parse_request: invalid-utf8",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "transport/schema",
        "native_branch": "parse_request: malformed-json",
        "code": "malformed-json",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "parse_request: malformed-json",
        "native_evidence": "`build_bitmap_histogram/remap_bitmap_histogram`, `src/display/bitmap-histogram.cpp:43,81`",
        "oracle": "P9:bitmap.histogram:transport/schema:malformed-json:parse_request: malformed-json",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "transport/schema",
        "native_branch": "raw schema/type/range/route/uniqueItems violation",
        "code": "invalid-argument",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "raw schema/type/range/route/uniqueItems violation",
        "native_evidence": "`build_bitmap_histogram/remap_bitmap_histogram`, `src/display/bitmap-histogram.cpp:43,81`",
        "oracle": "P9:bitmap.histogram:transport/schema:invalid-argument:raw schema/type/range/route/uniqueItems violation",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "missing current document",
        "code": "no-document",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "missing current document",
        "native_evidence": "`build_bitmap_histogram/remap_bitmap_histogram`, `src/display/bitmap-histogram.cpp:43,81`",
        "oracle": "P9:bitmap.histogram:identity/admission:no-document:missing current document",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "document identity mismatch",
        "code": "stale-document",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "document identity mismatch",
        "native_evidence": "`build_bitmap_histogram/remap_bitmap_histogram`, `src/display/bitmap-histogram.cpp:43,81`",
        "oracle": "P9:bitmap.histogram:identity/admission:stale-document:document identity mismatch",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "supplied guard differs from document revision",
        "code": "stale-revision",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "supplied guard differs from document revision",
        "native_evidence": "`build_bitmap_histogram/remap_bitmap_histogram`, `src/display/bitmap-histogram.cpp:43,81`",
        "oracle": "P9:bitmap.histogram:identity/admission:stale-revision:supplied guard differs from document revision",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "cancellation observed before commit",
        "code": "cancelled",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "cancellation observed before commit",
        "native_evidence": "`build_bitmap_histogram/remap_bitmap_histogram`, `src/display/bitmap-histogram.cpp:43,81`",
        "oracle": "P9:bitmap.histogram:identity/admission:cancelled:cancellation observed before commit",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "explicit ID cannot resolve",
        "code": "unknown-id",
        "retryable": false,
        "mutation_state": "none",
        "route": "explicit-target route",
        "detail.reason": "explicit ID cannot resolve",
        "native_evidence": "`build_bitmap_histogram/remap_bitmap_histogram`, `src/display/bitmap-histogram.cpp:43,81`",
        "oracle": "P9:bitmap.histogram:identity/admission:unknown-id:explicit ID cannot resolve",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "single usable image admission",
        "code": "requires-single-bitmap",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "single usable image admission",
        "native_evidence": "`build_bitmap_histogram/remap_bitmap_histogram`, `src/display/bitmap-histogram.cpp:43,81`",
        "oracle": "P9:bitmap.histogram:command-service:requires-single-bitmap:single usable image admission",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "build_bitmap_histogram: decode unavailable",
        "code": "missing-source",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "build_bitmap_histogram",
        "native_evidence": "`build_bitmap_histogram/remap_bitmap_histogram`, `src/display/bitmap-histogram.cpp:43,81`",
        "oracle": "P9:bitmap.histogram:command-service:missing-source:build_bitmap_histogram: decode unavailable",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "unexpected service exception; rollback before return",
        "code": "internal-error",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "unexpected service exception; rollback before return",
        "native_evidence": "`build_bitmap_histogram/remap_bitmap_histogram`, `src/display/bitmap-histogram.cpp:43,81`",
        "oracle": "P9:bitmap.histogram:command-service:internal-error:unexpected service exception; rollback before return",
        "evidence_status": "required-unimplemented"
      }
    ],
    "success_native_branches": [
      "native successful outcome",
      "native unchanged outcome when applicable",
      "read-only computed preparation"
    ],
    "variant_applicability": {
      "success": "ok or changed",
      "unchanged": "native no-op only; query/preparation/history empty are not successful unchanged",
      "computed-dry-run": "successful read-only preflight; no retention, publication, Undo or Redo change"
    },
    "success_rows": [
      {
        "native_branch": "native successful outcome",
        "code": null,
        "retryable": false,
        "mutation_state": "none",
        "data_variant": "success",
        "evidence_route": "P9:bitmap.histogram:native:native successful outcome",
        "evidence_status": "required-unimplemented"
      },
      {
        "native_branch": "native unchanged outcome when applicable",
        "code": null,
        "retryable": false,
        "mutation_state": "none",
        "data_variant": "unchanged",
        "evidence_route": "P9:bitmap.histogram:native:native unchanged outcome when applicable",
        "evidence_status": "required-unimplemented"
      },
      {
        "native_branch": "read-only computed preparation",
        "code": null,
        "retryable": false,
        "mutation_state": "none",
        "data_variant": "computed-dry-run",
        "evidence_route": "P9:bitmap.histogram:native:read-only computed preparation",
        "evidence_status": "required-unimplemented"
      }
    ]
  }
})m3").as_object()};
// | `bitmap.tone-set` | ids; nonempty `patch` of brightness/contrast/intensity/highlights/shadows/midtones, each finite -100..100 | per-member old/new patch values, changed/unchanged/excluded counts | T, incompatible-tone-context, tone-apply-failed | E / one / computed / M; shared chemistry/controller, unrelated settings retained |
TypeDescriptor const m3_2{boost::json::parse(R"m3({
  "type": "object",
  "properties": {
    "ids": {
      "type": "array",
      "items": {
        "type": "string",
        "minLength": 1,
        "maxLength": 128
      },
      "minItems": 1,
      "maxItems": 100000,
      "uniqueItems": true
    },
    "patch": {
      "type": "object",
      "properties": {
        "brightness": {
          "type": "number",
          "minimum": -100,
          "maximum": 100
        },
        "contrast": {
          "type": "number",
          "minimum": -100,
          "maximum": 100
        },
        "intensity": {
          "type": "number",
          "minimum": -100,
          "maximum": 100
        },
        "highlights": {
          "type": "number",
          "minimum": -100,
          "maximum": 100
        },
        "shadows": {
          "type": "number",
          "minimum": -100,
          "maximum": 100
        },
        "midtones": {
          "type": "number",
          "minimum": -100,
          "maximum": 100
        }
      },
      "required": [],
      "additionalProperties": false,
      "minProperties": 1
    }
  },
  "required": [
    "ids",
    "patch"
  ],
  "additionalProperties": false,
  "x-m3-contract": {
    "guard_domain": "document",
    "guard_required": true,
    "needs_document": true,
    "selection_mode": "compatible-members",
    "target_cardinality": {
      "min": 1,
      "max": 100000
    },
    "normalization": "deduplicate roots then recursive eligible image members once; legacy-roots query does not recurse",
    "partial_policy": "preserve-and-report-exclusions",
    "role_order": "input-order; each normalized root once",
    "normalization_order": [
      "validate raw schema and exclusive route",
      "select route",
      "fill defaults recursively only in selected branch; pivot suppresses anchor, size suppresses dpi, token suppresses recipe/analyze/solve",
      "parse exact integers",
      "convert all lengths including nested recipes to CSS px and enforce bounds",
      "resolve and normalize targets",
      "canonicalize normalized params for token binding"
    ],
    "native_service": "`canonical_tone/apply_tone`, `src/bitmap-adjustment-chemistry.cpp:187,236`; controller patch/commit reference `src/ui/bitmap-adjustments-controller.cpp:298-339`",
    "warnings": [
      {
        "code": "exclusions",
        "emit_site": "`canonical_tone/apply_tone`, `src/bitmap-adjustment-chemistry.cpp:187,236`; controller patch/commit reference `src/ui/bitmap-adjustments-controller.cpp:298-339`; after target resolution, nonempty exclusions"
      }
    ],
    "error_rows": [
      {
        "layer": "transport/schema",
        "native_branch": "parse_request: request-too-large",
        "code": "request-too-large",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "parse_request: request-too-large",
        "native_evidence": "`canonical_tone/apply_tone`, `src/bitmap-adjustment-chemistry.cpp:187,236`; controller patch/commit reference `src/ui/bitmap-adjustments-controller.cpp:298-339`",
        "oracle": "P9:bitmap.tone-set:transport/schema:request-too-large:parse_request: request-too-large",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "transport/schema",
        "native_branch": "parse_request: repeated-key",
        "code": "repeated-key",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "parse_request: repeated-key",
        "native_evidence": "`canonical_tone/apply_tone`, `src/bitmap-adjustment-chemistry.cpp:187,236`; controller patch/commit reference `src/ui/bitmap-adjustments-controller.cpp:298-339`",
        "oracle": "P9:bitmap.tone-set:transport/schema:repeated-key:parse_request: repeated-key",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "transport/schema",
        "native_branch": "parse_request: invalid-utf8",
        "code": "invalid-utf8",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "parse_request: invalid-utf8",
        "native_evidence": "`canonical_tone/apply_tone`, `src/bitmap-adjustment-chemistry.cpp:187,236`; controller patch/commit reference `src/ui/bitmap-adjustments-controller.cpp:298-339`",
        "oracle": "P9:bitmap.tone-set:transport/schema:invalid-utf8:parse_request: invalid-utf8",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "transport/schema",
        "native_branch": "parse_request: malformed-json",
        "code": "malformed-json",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "parse_request: malformed-json",
        "native_evidence": "`canonical_tone/apply_tone`, `src/bitmap-adjustment-chemistry.cpp:187,236`; controller patch/commit reference `src/ui/bitmap-adjustments-controller.cpp:298-339`",
        "oracle": "P9:bitmap.tone-set:transport/schema:malformed-json:parse_request: malformed-json",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "transport/schema",
        "native_branch": "raw schema/type/range/route/uniqueItems violation",
        "code": "invalid-argument",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "raw schema/type/range/route/uniqueItems violation",
        "native_evidence": "`canonical_tone/apply_tone`, `src/bitmap-adjustment-chemistry.cpp:187,236`; controller patch/commit reference `src/ui/bitmap-adjustments-controller.cpp:298-339`",
        "oracle": "P9:bitmap.tone-set:transport/schema:invalid-argument:raw schema/type/range/route/uniqueItems violation",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "missing current document",
        "code": "no-document",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "missing current document",
        "native_evidence": "`canonical_tone/apply_tone`, `src/bitmap-adjustment-chemistry.cpp:187,236`; controller patch/commit reference `src/ui/bitmap-adjustments-controller.cpp:298-339`",
        "oracle": "P9:bitmap.tone-set:identity/admission:no-document:missing current document",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "document identity mismatch",
        "code": "stale-document",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "document identity mismatch",
        "native_evidence": "`canonical_tone/apply_tone`, `src/bitmap-adjustment-chemistry.cpp:187,236`; controller patch/commit reference `src/ui/bitmap-adjustments-controller.cpp:298-339`",
        "oracle": "P9:bitmap.tone-set:identity/admission:stale-document:document identity mismatch",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "supplied guard differs from document revision",
        "code": "stale-revision",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "supplied guard differs from document revision",
        "native_evidence": "`canonical_tone/apply_tone`, `src/bitmap-adjustment-chemistry.cpp:187,236`; controller patch/commit reference `src/ui/bitmap-adjustments-controller.cpp:298-339`",
        "oracle": "P9:bitmap.tone-set:identity/admission:stale-revision:supplied guard differs from document revision",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "cancellation observed before commit",
        "code": "cancelled",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "cancellation observed before commit",
        "native_evidence": "`canonical_tone/apply_tone`, `src/bitmap-adjustment-chemistry.cpp:187,236`; controller patch/commit reference `src/ui/bitmap-adjustments-controller.cpp:298-339`",
        "oracle": "P9:bitmap.tone-set:identity/admission:cancelled:cancellation observed before commit",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "explicit ID cannot resolve",
        "code": "unknown-id",
        "retryable": false,
        "mutation_state": "none",
        "route": "explicit-target route",
        "detail.reason": "explicit ID cannot resolve",
        "native_evidence": "`canonical_tone/apply_tone`, `src/bitmap-adjustment-chemistry.cpp:187,236`; controller patch/commit reference `src/ui/bitmap-adjustments-controller.cpp:298-339`",
        "oracle": "P9:bitmap.tone-set:identity/admission:unknown-id:explicit ID cannot resolve",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "EditTransaction inactive",
        "code": "transaction-unavailable",
        "retryable": true,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "EditTransaction inactive",
        "native_evidence": "`canonical_tone/apply_tone`, `src/bitmap-adjustment-chemistry.cpp:187,236`; controller patch/commit reference `src/ui/bitmap-adjustments-controller.cpp:298-339`",
        "oracle": "P9:bitmap.tone-set:identity/admission:transaction-unavailable:EditTransaction inactive",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "resolved member set empty",
        "code": "no-eligible-targets",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "resolved member set empty",
        "native_evidence": "`canonical_tone/apply_tone`, `src/bitmap-adjustment-chemistry.cpp:187,236`; controller patch/commit reference `src/ui/bitmap-adjustments-controller.cpp:298-339`",
        "oracle": "P9:bitmap.tone-set:command-service:no-eligible-targets:resolved member set empty",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "resolved member count exceeds 100000 before mutation",
        "code": "engine-limit",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "resolved member count exceeds 100000 before mutation",
        "native_evidence": "`canonical_tone/apply_tone`, `src/bitmap-adjustment-chemistry.cpp:187,236`; controller patch/commit reference `src/ui/bitmap-adjustments-controller.cpp:298-339`",
        "oracle": "P9:bitmap.tone-set:command-service:engine-limit:resolved member count exceeds 100000 before mutation",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "read-only session rejects requested document mutation",
        "code": "document-read-only",
        "retryable": false,
        "mutation_state": "none",
        "route": "document-edit or history mutation only",
        "detail.reason": "read-only session rejects requested document mutation",
        "native_evidence": "`canonical_tone/apply_tone`, `src/bitmap-adjustment-chemistry.cpp:187,236`; controller patch/commit reference `src/ui/bitmap-adjustments-controller.cpp:298-339`",
        "oracle": "P9:bitmap.tone-set:identity/admission:document-read-only:read-only session rejects requested document mutation",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "unexpected service exception; rollback before return",
        "code": "internal-error",
        "retryable": false,
        "mutation_state": "rolled-back",
        "route": "all",
        "detail.reason": "unexpected service exception; rollback before return",
        "native_evidence": "`canonical_tone/apply_tone`, `src/bitmap-adjustment-chemistry.cpp:187,236`; controller patch/commit reference `src/ui/bitmap-adjustments-controller.cpp:298-339`",
        "oracle": "P9:bitmap.tone-set:command-service:internal-error:unexpected service exception; rollback before return",
        "evidence_status": "required-unimplemented"
      }
    ],
    "success_native_branches": [
      "native successful outcome",
      "native unchanged outcome when applicable",
      "read-only computed preparation"
    ],
    "variant_applicability": {
      "success": "ok or changed",
      "unchanged": "native no-op only; query/preparation/history empty are not successful unchanged",
      "computed-dry-run": "successful read-only preflight; no retention, publication, Undo or Redo change"
    },
    "success_rows": [
      {
        "native_branch": "native successful outcome",
        "code": null,
        "retryable": false,
        "mutation_state": "committed",
        "data_variant": "success",
        "evidence_route": "P9:bitmap.tone-set:native:native successful outcome",
        "evidence_status": "required-unimplemented"
      },
      {
        "native_branch": "native unchanged outcome when applicable",
        "code": null,
        "retryable": false,
        "mutation_state": "none",
        "data_variant": "unchanged",
        "evidence_route": "P9:bitmap.tone-set:native:native unchanged outcome when applicable",
        "evidence_status": "required-unimplemented"
      },
      {
        "native_branch": "read-only computed preparation",
        "code": null,
        "retryable": false,
        "mutation_state": "none",
        "data_variant": "computed-dry-run",
        "evidence_route": "P9:bitmap.tone-set:native:read-only computed preparation",
        "evidence_status": "required-unimplemented"
      }
    ]
  }
})m3").as_object()};
// | `bitmap.copy` | ids; `dpi=96` positive; OR explicit positive integer image-pixel `size:{width,height}` (exclusive); `background=[0,0,0,0]` normalized RGBA; `bbox=visual|geometric` default visual; `replace=false` | embedded image ID, pixel/profile hashes, dimensions, root/pixel-to-parent affine, source-retained | T, no-bounds, incompatible-operands, rasterization-failed, encoding-failed | E / one / computed / C; one combined render, explicit conversion |
TypeDescriptor const m3_3{boost::json::parse(R"m3({
  "type": "object",
  "properties": {
    "ids": {
      "type": "array",
      "items": {
        "type": "string",
        "minLength": 1,
        "maxLength": 128
      },
      "minItems": 1,
      "maxItems": 100000,
      "uniqueItems": true
    },
    "dpi": {
      "type": "number",
      "default": 96,
      "exclusiveMinimum": 0
    },
    "size": {
      "type": "object",
      "properties": {
        "width": {
          "type": "integer",
          "minimum": 1,
          "maximum": 32000000
        },
        "height": {
          "type": "integer",
          "minimum": 1,
          "maximum": 32000000
        }
      },
      "required": [
        "width",
        "height"
      ],
      "additionalProperties": false,
      "description": "Exact integer dimensions; preserve document rectangle and change sampling density. Refuse unsupported dimensions, never clamp or round."
    },
    "background": {
      "type": "array",
      "items": {
        "type": "number",
        "minimum": 0,
        "maximum": 1
      },
      "minItems": 4,
      "maxItems": 4,
      "default": [
        0,
        0,
        0,
        0
      ]
    },
    "bbox": {
      "type": "string",
      "enum": [
        "visual",
        "geometric"
      ],
      "default": "visual"
    },
    "replace": {
      "type": "boolean",
      "default": false
    }
  },
  "required": [
    "ids"
  ],
  "additionalProperties": false,
  "not": {
    "required": [
      "dpi",
      "size"
    ]
  },
  "x-m3-contract": {
    "guard_domain": "document",
    "guard_required": true,
    "needs_document": true,
    "selection_mode": "collective-compositing",
    "target_cardinality": {
      "min": 1,
      "max": 100000
    },
    "normalization": "ordered-composite-roots; ancestor covers descendant, preserving surviving input order",
    "partial_policy": "reject-any-incompatible",
    "role_order": "input-order; each normalized root once",
    "normalization_order": [
      "validate raw schema and exclusive route",
      "select route",
      "fill defaults recursively only in selected branch; pivot suppresses anchor, size suppresses dpi, token suppresses recipe/analyze/solve",
      "parse exact integers",
      "convert all lengths including nested recipes to CSS px and enforce bounds",
      "resolve and normalize targets",
      "canonicalize normalized params for token binding"
    ],
    "native_service": "`prepareBitmapCopy/publishBitmapCopy`, `src/bitmap-copy-outcome.cpp:160,360`",
    "warnings": [],
    "error_rows": [
      {
        "layer": "transport/schema",
        "native_branch": "parse_request: request-too-large",
        "code": "request-too-large",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "parse_request: request-too-large",
        "native_evidence": "`prepareBitmapCopy/publishBitmapCopy`, `src/bitmap-copy-outcome.cpp:160,360`",
        "oracle": "P9:bitmap.copy:transport/schema:request-too-large:parse_request: request-too-large",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "transport/schema",
        "native_branch": "parse_request: repeated-key",
        "code": "repeated-key",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "parse_request: repeated-key",
        "native_evidence": "`prepareBitmapCopy/publishBitmapCopy`, `src/bitmap-copy-outcome.cpp:160,360`",
        "oracle": "P9:bitmap.copy:transport/schema:repeated-key:parse_request: repeated-key",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "transport/schema",
        "native_branch": "parse_request: invalid-utf8",
        "code": "invalid-utf8",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "parse_request: invalid-utf8",
        "native_evidence": "`prepareBitmapCopy/publishBitmapCopy`, `src/bitmap-copy-outcome.cpp:160,360`",
        "oracle": "P9:bitmap.copy:transport/schema:invalid-utf8:parse_request: invalid-utf8",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "transport/schema",
        "native_branch": "parse_request: malformed-json",
        "code": "malformed-json",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "parse_request: malformed-json",
        "native_evidence": "`prepareBitmapCopy/publishBitmapCopy`, `src/bitmap-copy-outcome.cpp:160,360`",
        "oracle": "P9:bitmap.copy:transport/schema:malformed-json:parse_request: malformed-json",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "transport/schema",
        "native_branch": "raw schema/type/range/route/uniqueItems violation",
        "code": "invalid-argument",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "raw schema/type/range/route/uniqueItems violation",
        "native_evidence": "`prepareBitmapCopy/publishBitmapCopy`, `src/bitmap-copy-outcome.cpp:160,360`",
        "oracle": "P9:bitmap.copy:transport/schema:invalid-argument:raw schema/type/range/route/uniqueItems violation",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "missing current document",
        "code": "no-document",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "missing current document",
        "native_evidence": "`prepareBitmapCopy/publishBitmapCopy`, `src/bitmap-copy-outcome.cpp:160,360`",
        "oracle": "P9:bitmap.copy:identity/admission:no-document:missing current document",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "document identity mismatch",
        "code": "stale-document",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "document identity mismatch",
        "native_evidence": "`prepareBitmapCopy/publishBitmapCopy`, `src/bitmap-copy-outcome.cpp:160,360`",
        "oracle": "P9:bitmap.copy:identity/admission:stale-document:document identity mismatch",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "supplied guard differs from document revision",
        "code": "stale-revision",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "supplied guard differs from document revision",
        "native_evidence": "`prepareBitmapCopy/publishBitmapCopy`, `src/bitmap-copy-outcome.cpp:160,360`",
        "oracle": "P9:bitmap.copy:identity/admission:stale-revision:supplied guard differs from document revision",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "cancellation observed before commit",
        "code": "cancelled",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "cancellation observed before commit",
        "native_evidence": "`prepareBitmapCopy/publishBitmapCopy`, `src/bitmap-copy-outcome.cpp:160,360`",
        "oracle": "P9:bitmap.copy:identity/admission:cancelled:cancellation observed before commit",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "explicit ID cannot resolve",
        "code": "unknown-id",
        "retryable": false,
        "mutation_state": "none",
        "route": "explicit-target route",
        "detail.reason": "explicit ID cannot resolve",
        "native_evidence": "`prepareBitmapCopy/publishBitmapCopy`, `src/bitmap-copy-outcome.cpp:160,360`",
        "oracle": "P9:bitmap.copy:identity/admission:unknown-id:explicit ID cannot resolve",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "EditTransaction inactive",
        "code": "transaction-unavailable",
        "retryable": true,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "EditTransaction inactive",
        "native_evidence": "`prepareBitmapCopy/publishBitmapCopy`, `src/bitmap-copy-outcome.cpp:160,360`",
        "oracle": "P9:bitmap.copy:identity/admission:transaction-unavailable:EditTransaction inactive",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "prepare: empty visual/geometric rectangle",
        "code": "no-bounds",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "prepare",
        "native_evidence": "`prepareBitmapCopy/publishBitmapCopy`, `src/bitmap-copy-outcome.cpp:160,360`",
        "oracle": "P9:bitmap.copy:command-service:no-bounds:prepare: empty visual/geometric rectangle",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "prepare: decode missing",
        "code": "missing-source",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "prepare",
        "native_evidence": "`prepareBitmapCopy/publishBitmapCopy`, `src/bitmap-copy-outcome.cpp:160,360`",
        "oracle": "P9:bitmap.copy:command-service:missing-source:prepare: decode missing",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "prepare: raster budget exceeded",
        "code": "engine-limit",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "prepare",
        "native_evidence": "`prepareBitmapCopy/publishBitmapCopy`, `src/bitmap-copy-outcome.cpp:160,360`",
        "oracle": "P9:bitmap.copy:command-service:engine-limit:prepare: raster budget exceeded",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "prepare: renderer failed",
        "code": "rasterization-failed",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "prepare",
        "native_evidence": "`prepareBitmapCopy/publishBitmapCopy`, `src/bitmap-copy-outcome.cpp:160,360`",
        "oracle": "P9:bitmap.copy:command-service:rasterization-failed:prepare: renderer failed",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "publish: encode failed",
        "code": "encoding-failed",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "publish",
        "native_evidence": "`prepareBitmapCopy/publishBitmapCopy`, `src/bitmap-copy-outcome.cpp:160,360`",
        "oracle": "P9:bitmap.copy:command-service:encoding-failed:publish: encode failed",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "publish failed; caller rollback",
        "code": "publication-failed",
        "retryable": false,
        "mutation_state": "rolled-back",
        "route": "all",
        "detail.reason": "publish failed; caller rollback",
        "native_evidence": "`prepareBitmapCopy/publishBitmapCopy`, `src/bitmap-copy-outcome.cpp:160,360`",
        "oracle": "P9:bitmap.copy:command-service:publication-failed:publish failed; caller rollback",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "read-only session rejects requested document mutation",
        "code": "document-read-only",
        "retryable": false,
        "mutation_state": "none",
        "route": "document-edit or history mutation only",
        "detail.reason": "read-only session rejects requested document mutation",
        "native_evidence": "`prepareBitmapCopy/publishBitmapCopy`, `src/bitmap-copy-outcome.cpp:160,360`",
        "oracle": "P9:bitmap.copy:identity/admission:document-read-only:read-only session rejects requested document mutation",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "unexpected service exception; rollback before return",
        "code": "internal-error",
        "retryable": false,
        "mutation_state": "rolled-back",
        "route": "all",
        "detail.reason": "unexpected service exception; rollback before return",
        "native_evidence": "`prepareBitmapCopy/publishBitmapCopy`, `src/bitmap-copy-outcome.cpp:160,360`",
        "oracle": "P9:bitmap.copy:command-service:internal-error:unexpected service exception; rollback before return",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "context reason UnsupportedTarget",
        "code": "unsupported-target",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "context reason UnsupportedTarget",
        "native_evidence": "request-only prepareBitmapCopy and native publication capture branches",
        "oracle": "P9:bitmap.copy:command-service:unsupported-target:context reason UnsupportedTarget",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "context reason StaleCapture",
        "code": "stale-dependency",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "context reason StaleCapture",
        "native_evidence": "request-only prepareBitmapCopy and native publication capture branches",
        "oracle": "P9:bitmap.copy:command-service:stale-dependency:context reason StaleCapture",
        "evidence_status": "required-unimplemented"
      }
    ],
    "success_native_branches": [
      "native successful outcome",
      "native unchanged outcome when applicable",
      "read-only computed preparation"
    ],
    "variant_applicability": {
      "success": "ok or changed",
      "unchanged": "native no-op only; query/preparation/history empty are not successful unchanged",
      "computed-dry-run": "successful read-only preflight; no retention, publication, Undo or Redo change"
    },
    "success_rows": [
      {
        "native_branch": "native successful outcome",
        "code": null,
        "retryable": false,
        "mutation_state": "committed",
        "data_variant": "success",
        "evidence_route": "P9:bitmap.copy:native:native successful outcome",
        "evidence_status": "required-unimplemented"
      },
      {
        "native_branch": "native unchanged outcome when applicable",
        "code": null,
        "retryable": false,
        "mutation_state": "none",
        "data_variant": "unchanged",
        "evidence_route": "P9:bitmap.copy:native:native unchanged outcome when applicable",
        "evidence_status": "required-unimplemented"
      },
      {
        "native_branch": "read-only computed preparation",
        "code": null,
        "retryable": false,
        "mutation_state": "none",
        "data_variant": "computed-dry-run",
        "evidence_route": "P9:bitmap.copy:native:read-only computed preparation",
        "evidence_status": "required-unimplemented"
      }
    ]
  }
})m3").as_object()};
// | `bitmap.explode.analyze` | `id` exactly one embedded image; `recipe` below; `retain=true` | analysis-token or null, pieces/counts/lost-alpha/budget metrics, sampling/warnings | T, K, requires-single-bitmap, unsupported-target, memory-admission-failed, analysis-failed | Q+R / none / computed transient / C single bitmap; PanelPreparation::calculate |
TypeDescriptor const m3_4{boost::json::parse(R"m3({
  "type": "object",
  "properties": {
    "id": {
      "type": "string",
      "minLength": 1,
      "maxLength": 128
    },
    "recipe": {
      "type": "object",
      "properties": {
        "threshold": {
          "type": "integer",
          "minimum": 0,
          "maximum": 255,
          "default": 128
        },
        "softness": {
          "type": "integer",
          "minimum": 0,
          "maximum": 127,
          "default": 40
        },
        "faint-floor": {
          "type": "integer",
          "minimum": 0,
          "maximum": 25,
          "default": 5
        },
        "refine": {
          "type": "boolean",
          "default": true
        }
      },
      "required": [],
      "additionalProperties": false
    },
    "retain": {
      "type": "boolean",
      "default": true
    }
  },
  "required": [
    "id",
    "recipe"
  ],
  "additionalProperties": false,
  "x-m3-contract": {
    "guard_domain": "document",
    "guard_required": false,
    "needs_document": true,
    "selection_mode": "single-object",
    "target_cardinality": {
      "min": 1,
      "max": 1
    },
    "normalization": "one explicit root or retained target; no member expansion",
    "partial_policy": "reject-any-incompatible",
    "role_order": "input-order; each normalized root once",
    "normalization_order": [
      "validate raw schema and exclusive route",
      "select route",
      "fill defaults recursively only in selected branch; pivot suppresses anchor, size suppresses dpi, token suppresses recipe/analyze/solve",
      "parse exact integers",
      "convert all lengths including nested recipes to CSS px and enforce bounds",
      "resolve and normalize targets",
      "canonicalize normalized params for token binding"
    ],
    "native_service": "context `resolve` replacing desktop service `src/ui/explode-bitmap-target.cpp:352`; `PanelPreparation::calculate`, `src/ui/explode-bitmap-panel-preparation.cpp:226`",
    "warnings": [],
    "error_rows": [
      {
        "layer": "transport/schema",
        "native_branch": "parse_request: request-too-large",
        "code": "request-too-large",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "parse_request: request-too-large",
        "native_evidence": "context `resolve` replacing desktop service `src/ui/explode-bitmap-target.cpp:352`; `PanelPreparation::calculate`, `src/ui/explode-bitmap-panel-preparation.cpp:226`",
        "oracle": "P9:bitmap.explode.analyze:transport/schema:request-too-large:parse_request: request-too-large",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "transport/schema",
        "native_branch": "parse_request: repeated-key",
        "code": "repeated-key",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "parse_request: repeated-key",
        "native_evidence": "context `resolve` replacing desktop service `src/ui/explode-bitmap-target.cpp:352`; `PanelPreparation::calculate`, `src/ui/explode-bitmap-panel-preparation.cpp:226`",
        "oracle": "P9:bitmap.explode.analyze:transport/schema:repeated-key:parse_request: repeated-key",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "transport/schema",
        "native_branch": "parse_request: invalid-utf8",
        "code": "invalid-utf8",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "parse_request: invalid-utf8",
        "native_evidence": "context `resolve` replacing desktop service `src/ui/explode-bitmap-target.cpp:352`; `PanelPreparation::calculate`, `src/ui/explode-bitmap-panel-preparation.cpp:226`",
        "oracle": "P9:bitmap.explode.analyze:transport/schema:invalid-utf8:parse_request: invalid-utf8",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "transport/schema",
        "native_branch": "parse_request: malformed-json",
        "code": "malformed-json",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "parse_request: malformed-json",
        "native_evidence": "context `resolve` replacing desktop service `src/ui/explode-bitmap-target.cpp:352`; `PanelPreparation::calculate`, `src/ui/explode-bitmap-panel-preparation.cpp:226`",
        "oracle": "P9:bitmap.explode.analyze:transport/schema:malformed-json:parse_request: malformed-json",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "transport/schema",
        "native_branch": "raw schema/type/range/route/uniqueItems violation",
        "code": "invalid-argument",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "raw schema/type/range/route/uniqueItems violation",
        "native_evidence": "context `resolve` replacing desktop service `src/ui/explode-bitmap-target.cpp:352`; `PanelPreparation::calculate`, `src/ui/explode-bitmap-panel-preparation.cpp:226`",
        "oracle": "P9:bitmap.explode.analyze:transport/schema:invalid-argument:raw schema/type/range/route/uniqueItems violation",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "missing current document",
        "code": "no-document",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "missing current document",
        "native_evidence": "context `resolve` replacing desktop service `src/ui/explode-bitmap-target.cpp:352`; `PanelPreparation::calculate`, `src/ui/explode-bitmap-panel-preparation.cpp:226`",
        "oracle": "P9:bitmap.explode.analyze:identity/admission:no-document:missing current document",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "document identity mismatch",
        "code": "stale-document",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "document identity mismatch",
        "native_evidence": "context `resolve` replacing desktop service `src/ui/explode-bitmap-target.cpp:352`; `PanelPreparation::calculate`, `src/ui/explode-bitmap-panel-preparation.cpp:226`",
        "oracle": "P9:bitmap.explode.analyze:identity/admission:stale-document:document identity mismatch",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "supplied guard differs from document revision",
        "code": "stale-revision",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "supplied guard differs from document revision",
        "native_evidence": "context `resolve` replacing desktop service `src/ui/explode-bitmap-target.cpp:352`; `PanelPreparation::calculate`, `src/ui/explode-bitmap-panel-preparation.cpp:226`",
        "oracle": "P9:bitmap.explode.analyze:identity/admission:stale-revision:supplied guard differs from document revision",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "cancellation observed before commit",
        "code": "cancelled",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "cancellation observed before commit",
        "native_evidence": "context `resolve` replacing desktop service `src/ui/explode-bitmap-target.cpp:352`; `PanelPreparation::calculate`, `src/ui/explode-bitmap-panel-preparation.cpp:226`",
        "oracle": "P9:bitmap.explode.analyze:identity/admission:cancelled:cancellation observed before commit",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "explicit ID cannot resolve",
        "code": "unknown-id",
        "retryable": false,
        "mutation_state": "none",
        "route": "explicit-target route",
        "detail.reason": "explicit ID cannot resolve",
        "native_evidence": "context `resolve` replacing desktop service `src/ui/explode-bitmap-target.cpp:352`; `PanelPreparation::calculate`, `src/ui/explode-bitmap-panel-preparation.cpp:226`",
        "oracle": "P9:bitmap.explode.analyze:identity/admission:unknown-id:explicit ID cannot resolve",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "retention allocation/root budget exceeded",
        "code": "token-capacity",
        "retryable": false,
        "mutation_state": "none",
        "route": "retain=true and not dry-run",
        "detail.reason": "retention allocation/root budget exceeded",
        "native_evidence": "context `resolve` replacing desktop service `src/ui/explode-bitmap-target.cpp:352`; `PanelPreparation::calculate`, `src/ui/explode-bitmap-panel-preparation.cpp:226`",
        "oracle": "P9:bitmap.explode.analyze:identity/admission:token-capacity:retention allocation/root budget exceeded",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "retention requested without session",
        "code": "session-required",
        "retryable": false,
        "mutation_state": "none",
        "route": "retain=true and not dry-run",
        "detail.reason": "retention requested without session",
        "native_evidence": "context `resolve` replacing desktop service `src/ui/explode-bitmap-target.cpp:352`; `PanelPreparation::calculate`, `src/ui/explode-bitmap-panel-preparation.cpp:226`",
        "oracle": "P9:bitmap.explode.analyze:identity/admission:session-required:retention requested without session",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "context resolve: not one embedded bitmap",
        "code": "requires-single-bitmap",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "context resolve",
        "native_evidence": "context `resolve` replacing desktop service `src/ui/explode-bitmap-target.cpp:352`; `PanelPreparation::calculate`, `src/ui/explode-bitmap-panel-preparation.cpp:226`",
        "oracle": "P9:bitmap.explode.analyze:command-service:requires-single-bitmap:context resolve: not one embedded bitmap",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "context reason UnsupportedTarget",
        "code": "unsupported-target",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "context reason UnsupportedTarget",
        "native_evidence": "context `resolve` replacing desktop service `src/ui/explode-bitmap-target.cpp:352`; `PanelPreparation::calculate`, `src/ui/explode-bitmap-panel-preparation.cpp:226`",
        "oracle": "P9:bitmap.explode.analyze:command-service:unsupported-target:context reason UnsupportedTarget",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "context reason AnalysisFailed (full recipe only for publication)",
        "code": "analysis-failed",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "context reason AnalysisFailed (full recipe only for publication)",
        "native_evidence": "context `resolve` replacing desktop service `src/ui/explode-bitmap-target.cpp:352`; `PanelPreparation::calculate`, `src/ui/explode-bitmap-panel-preparation.cpp:226`",
        "oracle": "P9:bitmap.explode.analyze:command-service:analysis-failed:context reason AnalysisFailed (full recipe only for publication)",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "context reason EncodingFailed during analysis encoding",
        "code": "analysis-failed",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "context reason EncodingFailed during analysis encoding",
        "native_evidence": "PanelPreparation::calculate whole-image and piece encoder branches",
        "oracle": "P9:bitmap.explode.analyze:command-service:analysis-failed:context reason EncodingFailed during analysis encoding",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "context reason MemoryAdmissionFailed",
        "code": "memory-admission-failed",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "context reason MemoryAdmissionFailed",
        "native_evidence": "context `resolve` replacing desktop service `src/ui/explode-bitmap-target.cpp:352`; `PanelPreparation::calculate`, `src/ui/explode-bitmap-panel-preparation.cpp:226`",
        "oracle": "P9:bitmap.explode.analyze:command-service:memory-admission-failed:context reason MemoryAdmissionFailed",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "unexpected service exception; rollback before return",
        "code": "internal-error",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "unexpected service exception; rollback before return",
        "native_evidence": "context `resolve` replacing desktop service `src/ui/explode-bitmap-target.cpp:352`; `PanelPreparation::calculate`, `src/ui/explode-bitmap-panel-preparation.cpp:226`",
        "oracle": "P9:bitmap.explode.analyze:command-service:internal-error:unexpected service exception; rollback before return",
        "evidence_status": "required-unimplemented"
      }
    ],
    "success_native_branches": [
      "Bitmap::Status::changed",
      "Bitmap::Status::unchanged"
    ],
    "native_reason_requirement": "P6 must expose typed stage+reason; broad Outcome/unavailable or PublicationRefusal::nativePublication cannot be classified by diagnostic text.",
    "variant_applicability": {
      "success": "ok or changed",
      "unchanged": "native no-op only; query/preparation/history empty are not successful unchanged",
      "computed-dry-run": "successful read-only preflight; no retention, publication, Undo or Redo change"
    },
    "success_rows": [
      {
        "native_branch": "Bitmap::Status::changed",
        "code": null,
        "retryable": false,
        "mutation_state": "none",
        "data_variant": "success",
        "evidence_route": "P9:bitmap.explode.analyze:native:Bitmap::Status::changed",
        "evidence_status": "required-unimplemented"
      },
      {
        "native_branch": "Bitmap::Status::unchanged",
        "code": null,
        "retryable": false,
        "mutation_state": "none",
        "data_variant": "unchanged",
        "evidence_route": "P9:bitmap.explode.analyze:native:Bitmap::Status::unchanged",
        "evidence_status": "required-unimplemented"
      }
    ]
  }
})m3").as_object()};
// | `bitmap.explode.contour` | `analysis-token`; `contour` recipe; `retain=true` | contour-token or null, parent token, ring/topology/noContour counts, bounds and recipe | K, contour-failed | Q+R / none / computed transient / C single bitmap; calculateContours on retained AnalysisState |
TypeDescriptor const m3_5{boost::json::parse(R"m3({
  "type": "object",
  "properties": {
    "analysis-token": {
      "type": "string",
      "minLength": 1,
      "maxLength": 128
    },
    "contour": {
      "type": "object",
      "properties": {
        "offset": {
          "type": "object",
          "properties": {
            "value": {
              "type": "number",
              "minimum": -1000000.0,
              "maximum": 1000000.0
            },
            "unit": {
              "type": "string",
              "enum": [
                "px",
                "mm",
                "cm",
                "in",
                "pt",
                "pc"
              ]
            }
          },
          "required": [
            "value",
            "unit"
          ],
          "additionalProperties": false,
          "x-css-px-absolute-maximum": 1000000,
          "default": {
            "value": 0,
            "unit": "mm"
          },
          "description": "Convert recursively to CSS px at 96 dpi, then enforce absolute <=1000000 and sign bound. No rounding or clamping.",
          "allOf": [
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "px"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": -1000000.0,
                    "maximum": 1000000.0
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "mm"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": -264583.3333333333,
                    "maximum": 264583.3333333333
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "cm"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": -26458.333333333336,
                    "maximum": 26458.333333333336
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "in"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": -10416.666666666666,
                    "maximum": 10416.666666666666
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "pt"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": -750000.0,
                    "maximum": 750000.0
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "pc"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": -62500.0,
                    "maximum": 62500.0
                  }
                }
              }
            }
          ]
        },
        "gap-tolerance": {
          "type": "object",
          "properties": {
            "value": {
              "type": "number",
              "minimum": 0,
              "maximum": 1000000.0
            },
            "unit": {
              "type": "string",
              "enum": [
                "px",
                "mm",
                "cm",
                "in",
                "pt",
                "pc"
              ]
            }
          },
          "required": [
            "value",
            "unit"
          ],
          "additionalProperties": false,
          "x-css-px-absolute-maximum": 1000000,
          "default": {
            "value": 0.5,
            "unit": "mm"
          },
          "description": "Convert recursively to CSS px at 96 dpi, then enforce absolute <=1000000 and sign bound. No rounding or clamping.",
          "allOf": [
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "px"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": 0,
                    "maximum": 1000000.0
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "mm"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": 0,
                    "maximum": 264583.3333333333
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "cm"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": 0,
                    "maximum": 26458.333333333336
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "in"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": 0,
                    "maximum": 10416.666666666666
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "pt"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": 0,
                    "maximum": 750000.0
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "pc"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": 0,
                    "maximum": 62500.0
                  }
                }
              }
            }
          ]
        },
        "smoothing": {
          "type": "number",
          "minimum": 0,
          "maximum": 100,
          "default": 50
        }
      },
      "required": [],
      "additionalProperties": false
    },
    "retain": {
      "type": "boolean",
      "default": true
    }
  },
  "required": [
    "analysis-token",
    "contour"
  ],
  "additionalProperties": false,
  "x-m3-contract": {
    "guard_domain": "document",
    "guard_required": false,
    "needs_document": true,
    "selection_mode": "single-object",
    "target_cardinality": {
      "min": 1,
      "max": 1
    },
    "normalization": "one explicit root or retained target; no member expansion",
    "partial_policy": "reject-any-incompatible",
    "role_order": "input-order; each normalized root once",
    "normalization_order": [
      "validate raw schema and exclusive route",
      "select route",
      "fill defaults recursively only in selected branch; pivot suppresses anchor, size suppresses dpi, token suppresses recipe/analyze/solve",
      "parse exact integers",
      "convert all lengths including nested recipes to CSS px and enforce bounds",
      "resolve and normalize targets",
      "canonicalize normalized params for token binding"
    ],
    "native_service": "`PanelPreparation::calculateContours`, `src/ui/explode-bitmap-panel-preparation.cpp:198`; `analysisIdentity`, header `:43`",
    "warnings": [
      {
        "code": "optional-contour-unavailable",
        "emit_site": "prepared piece noContour; image-only piece retained and counted"
      }
    ],
    "error_rows": [
      {
        "layer": "transport/schema",
        "native_branch": "parse_request: request-too-large",
        "code": "request-too-large",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "parse_request: request-too-large",
        "native_evidence": "`PanelPreparation::calculateContours`, `src/ui/explode-bitmap-panel-preparation.cpp:198`; `analysisIdentity`, header `:43`",
        "oracle": "P9:bitmap.explode.contour:transport/schema:request-too-large:parse_request: request-too-large",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "transport/schema",
        "native_branch": "parse_request: repeated-key",
        "code": "repeated-key",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "parse_request: repeated-key",
        "native_evidence": "`PanelPreparation::calculateContours`, `src/ui/explode-bitmap-panel-preparation.cpp:198`; `analysisIdentity`, header `:43`",
        "oracle": "P9:bitmap.explode.contour:transport/schema:repeated-key:parse_request: repeated-key",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "transport/schema",
        "native_branch": "parse_request: invalid-utf8",
        "code": "invalid-utf8",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "parse_request: invalid-utf8",
        "native_evidence": "`PanelPreparation::calculateContours`, `src/ui/explode-bitmap-panel-preparation.cpp:198`; `analysisIdentity`, header `:43`",
        "oracle": "P9:bitmap.explode.contour:transport/schema:invalid-utf8:parse_request: invalid-utf8",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "transport/schema",
        "native_branch": "parse_request: malformed-json",
        "code": "malformed-json",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "parse_request: malformed-json",
        "native_evidence": "`PanelPreparation::calculateContours`, `src/ui/explode-bitmap-panel-preparation.cpp:198`; `analysisIdentity`, header `:43`",
        "oracle": "P9:bitmap.explode.contour:transport/schema:malformed-json:parse_request: malformed-json",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "transport/schema",
        "native_branch": "raw schema/type/range/route/uniqueItems violation",
        "code": "invalid-argument",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "raw schema/type/range/route/uniqueItems violation",
        "native_evidence": "`PanelPreparation::calculateContours`, `src/ui/explode-bitmap-panel-preparation.cpp:198`; `analysisIdentity`, header `:43`",
        "oracle": "P9:bitmap.explode.contour:transport/schema:invalid-argument:raw schema/type/range/route/uniqueItems violation",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "missing current document",
        "code": "no-document",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "missing current document",
        "native_evidence": "`PanelPreparation::calculateContours`, `src/ui/explode-bitmap-panel-preparation.cpp:198`; `analysisIdentity`, header `:43`",
        "oracle": "P9:bitmap.explode.contour:identity/admission:no-document:missing current document",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "document identity mismatch",
        "code": "stale-document",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "document identity mismatch",
        "native_evidence": "`PanelPreparation::calculateContours`, `src/ui/explode-bitmap-panel-preparation.cpp:198`; `analysisIdentity`, header `:43`",
        "oracle": "P9:bitmap.explode.contour:identity/admission:stale-document:document identity mismatch",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "supplied guard differs from document revision",
        "code": "stale-revision",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "supplied guard differs from document revision",
        "native_evidence": "`PanelPreparation::calculateContours`, `src/ui/explode-bitmap-panel-preparation.cpp:198`; `analysisIdentity`, header `:43`",
        "oracle": "P9:bitmap.explode.contour:identity/admission:stale-revision:supplied guard differs from document revision",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "cancellation observed before commit",
        "code": "cancelled",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "cancellation observed before commit",
        "native_evidence": "`PanelPreparation::calculateContours`, `src/ui/explode-bitmap-panel-preparation.cpp:198`; `analysisIdentity`, header `:43`",
        "oracle": "P9:bitmap.explode.contour:identity/admission:cancelled:cancellation observed before commit",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "token missing or wrong kind/parent",
        "code": "invalid-token",
        "retryable": false,
        "mutation_state": "none",
        "route": "token route",
        "detail.reason": "token missing or wrong kind/parent",
        "native_evidence": "`PanelPreparation::calculateContours`, `src/ui/explode-bitmap-panel-preparation.cpp:198`; `analysisIdentity`, header `:43`",
        "oracle": "P9:bitmap.explode.contour:identity/admission:invalid-token:token missing or wrong kind/parent",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "stored document generation/revision differs",
        "code": "stale-plan",
        "retryable": false,
        "mutation_state": "none",
        "route": "token route",
        "detail.reason": "stored document generation/revision differs",
        "native_evidence": "`PanelPreparation::calculateContours`, `src/ui/explode-bitmap-panel-preparation.cpp:198`; `analysisIdentity`, header `:43`",
        "oracle": "P9:bitmap.explode.contour:identity/admission:stale-plan:stored document generation/revision differs",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "stored dependency revalidation failed",
        "code": "stale-dependency",
        "retryable": false,
        "mutation_state": "none",
        "route": "token route",
        "detail.reason": "stored dependency revalidation failed",
        "native_evidence": "`PanelPreparation::calculateContours`, `src/ui/explode-bitmap-panel-preparation.cpp:198`; `analysisIdentity`, header `:43`",
        "oracle": "P9:bitmap.explode.contour:identity/admission:stale-dependency:stored dependency revalidation failed",
        "evidence_status": "required-unimplemented"
      },
{
        "layer": "identity/admission",
        "native_branch": "retention allocation/root budget exceeded",
        "code": "token-capacity",
        "retryable": false,
        "mutation_state": "none",
        "route": "retain=true and not dry-run",
        "detail.reason": "retention allocation/root budget exceeded",
        "native_evidence": "`PanelPreparation::calculateContours`, `src/ui/explode-bitmap-panel-preparation.cpp:198`; `analysisIdentity`, header `:43`",
        "oracle": "P9:bitmap.explode.contour:identity/admission:token-capacity:retention allocation/root budget exceeded",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "retention requested without session",
        "code": "session-required",
        "retryable": false,
        "mutation_state": "none",
        "route": "retain=true and not dry-run",
        "detail.reason": "retention requested without session",
        "native_evidence": "`PanelPreparation::calculateContours`, `src/ui/explode-bitmap-panel-preparation.cpp:198`; `analysisIdentity`, header `:43`",
        "oracle": "P9:bitmap.explode.contour:identity/admission:session-required:retention requested without session",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "context reason MemoryAdmissionFailed",
        "code": "memory-admission-failed",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "context reason MemoryAdmissionFailed",
        "native_evidence": "`PanelPreparation::calculateContours`, `src/ui/explode-bitmap-panel-preparation.cpp:198`; `analysisIdentity`, header `:43`",
        "oracle": "P9:bitmap.explode.contour:command-service:memory-admission-failed:context reason MemoryAdmissionFailed",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "context reason ContourFailed (requested contour stage)",
        "code": "contour-failed",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "context reason ContourFailed (requested contour stage)",
        "native_evidence": "`PanelPreparation::calculateContours`, `src/ui/explode-bitmap-panel-preparation.cpp:198`; `analysisIdentity`, header `:43`",
        "oracle": "P9:bitmap.explode.contour:command-service:contour-failed:context reason ContourFailed (requested contour stage)",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "unexpected service exception; rollback before return",
        "code": "internal-error",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "unexpected service exception; rollback before return",
        "native_evidence": "`PanelPreparation::calculateContours`, `src/ui/explode-bitmap-panel-preparation.cpp:198`; `analysisIdentity`, header `:43`",
        "oracle": "P9:bitmap.explode.contour:command-service:internal-error:unexpected service exception; rollback before return",
        "evidence_status": "required-unimplemented"
      }
    ],
    "success_native_branches": [
      "Bitmap::Status::changed",
      "Bitmap::Status::unchanged"
    ],
    "native_reason_requirement": "P6 must expose typed stage+reason; broad Outcome/unavailable or PublicationRefusal::nativePublication cannot be classified by diagnostic text.",
    "variant_applicability": {
      "success": "ok or changed",
      "unchanged": "native no-op only; query/preparation/history empty are not successful unchanged",
      "computed-dry-run": "successful read-only preflight; no retention, publication, Undo or Redo change"
    },
    "success_rows": [
      {
        "native_branch": "Bitmap::Status::changed",
        "code": null,
        "retryable": false,
        "mutation_state": "none",
        "data_variant": "success",
        "evidence_route": "P9:bitmap.explode.contour:native:Bitmap::Status::changed",
        "evidence_status": "required-unimplemented"
      },
      {
        "native_branch": "Bitmap::Status::unchanged",
        "code": null,
        "retryable": false,
        "mutation_state": "none",
        "data_variant": "unchanged",
        "evidence_route": "P9:bitmap.explode.contour:native:Bitmap::Status::unchanged",
        "evidence_status": "required-unimplemented"
      }
    ]
  }
})m3").as_object()};
// | `bitmap.explode.explode` | analysis-token and matching contour-token when `contours=true`; OR `id+recipe` and contour recipe for one-shot; `contours=false`, `contour-style` | piece/image/group/cut-path mapping, counts/noContour, pixel/profile hashes, selection-after | K, T for recipe route, unsupported-target, publication-failed | E / one / computed / C single bitmap; publishExplode |
TypeDescriptor const m3_6{boost::json::parse(R"m3({
  "type": "object",
  "properties": {
    "analysis-token": {
      "type": "string",
      "minLength": 1,
      "maxLength": 128
    },
    "id": {
      "type": "string",
      "minLength": 1,
      "maxLength": 128
    },
    "recipe": {
      "type": "object",
      "properties": {
        "threshold": {
          "type": "integer",
          "minimum": 0,
          "maximum": 255,
          "default": 128
        },
        "softness": {
          "type": "integer",
          "minimum": 0,
          "maximum": 127,
          "default": 40
        },
        "faint-floor": {
          "type": "integer",
          "minimum": 0,
          "maximum": 25,
          "default": 5
        },
        "refine": {
          "type": "boolean",
          "default": true
        }
      },
      "required": [],
      "additionalProperties": false
    },
    "contour": {
      "type": "object",
      "properties": {
        "offset": {
          "type": "object",
          "properties": {
            "value": {
              "type": "number",
              "minimum": -1000000.0,
              "maximum": 1000000.0
            },
            "unit": {
              "type": "string",
              "enum": [
                "px",
                "mm",
                "cm",
                "in",
                "pt",
                "pc"
              ]
            }
          },
          "required": [
            "value",
            "unit"
          ],
          "additionalProperties": false,
          "x-css-px-absolute-maximum": 1000000,
          "default": {
            "value": 0,
            "unit": "mm"
          },
          "description": "Convert recursively to CSS px at 96 dpi, then enforce absolute <=1000000 and sign bound. No rounding or clamping.",
          "allOf": [
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "px"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": -1000000.0,
                    "maximum": 1000000.0
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "mm"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": -264583.3333333333,
                    "maximum": 264583.3333333333
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "cm"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": -26458.333333333336,
                    "maximum": 26458.333333333336
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "in"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": -10416.666666666666,
                    "maximum": 10416.666666666666
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "pt"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": -750000.0,
                    "maximum": 750000.0
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "pc"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": -62500.0,
                    "maximum": 62500.0
                  }
                }
              }
            }
          ]
        },
        "gap-tolerance": {
          "type": "object",
          "properties": {
            "value": {
              "type": "number",
              "minimum": 0,
              "maximum": 1000000.0
            },
            "unit": {
              "type": "string",
              "enum": [
                "px",
                "mm",
                "cm",
                "in",
                "pt",
                "pc"
              ]
            }
          },
          "required": [
            "value",
            "unit"
          ],
          "additionalProperties": false,
          "x-css-px-absolute-maximum": 1000000,
          "default": {
            "value": 0.5,
            "unit": "mm"
          },
          "description": "Convert recursively to CSS px at 96 dpi, then enforce absolute <=1000000 and sign bound. No rounding or clamping.",
          "allOf": [
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "px"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": 0,
                    "maximum": 1000000.0
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "mm"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": 0,
                    "maximum": 264583.3333333333
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "cm"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": 0,
                    "maximum": 26458.333333333336
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "in"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": 0,
                    "maximum": 10416.666666666666
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "pt"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": 0,
                    "maximum": 750000.0
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "pc"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": 0,
                    "maximum": 62500.0
                  }
                }
              }
            }
          ]
        },
        "smoothing": {
          "type": "number",
          "minimum": 0,
          "maximum": 100,
          "default": 50
        }
      },
      "required": [],
      "additionalProperties": false
    },
    "contour-style": {
      "type": "object",
      "properties": {
        "stroke": {
          "type": "string",
          "default": "#ff00ff",
          "pattern": "^#[0-9a-fA-F]{6}$"
        },
        "stroke-width": {
          "type": "object",
          "properties": {
            "value": {
              "type": "number",
              "minimum": 0,
              "maximum": 1000000.0,
              "exclusiveMinimum": 0
            },
            "unit": {
              "type": "string",
              "enum": [
                "px",
                "mm",
                "cm",
                "in",
                "pt",
                "pc"
              ]
            }
          },
          "required": [
            "value",
            "unit"
          ],
          "additionalProperties": false,
          "x-css-px-absolute-maximum": 1000000,
          "default": {
            "value": 0.1,
            "unit": "mm"
          },
          "description": "Convert recursively to CSS px at 96 dpi, then enforce absolute <=1000000 and sign bound. No rounding or clamping.",
          "allOf": [
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "px"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": 0,
                    "maximum": 1000000.0
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "mm"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": 0,
                    "maximum": 264583.3333333333
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "cm"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": 0,
                    "maximum": 26458.333333333336
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "in"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": 0,
                    "maximum": 10416.666666666666
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "pt"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": 0,
                    "maximum": 750000.0
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "pc"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": 0,
                    "maximum": 62500.0
                  }
                }
              }
            }
          ]
        }
      },
      "required": [],
      "additionalProperties": false
    },
    "contour-token": {
      "type": "string",
      "minLength": 1,
      "maxLength": 128
    },
    "contours": {
      "type": "boolean",
      "default": false
    }
  },
  "required": [],
  "additionalProperties": false,
  "oneOf": [
    {
      "required": [
        "analysis-token"
      ],
      "not": {
        "anyOf": [
          {
            "required": [
              "id"
            ]
          },
          {
            "required": [
              "recipe"
            ]
          },
          {
            "required": [
              "contour"
            ]
          }
        ]
      }
    },
    {
      "required": [
        "id",
        "recipe"
      ],
      "not": {
        "anyOf": [
          {
            "required": [
              "analysis-token"
            ]
          },
          {
            "required": [
              "contour-token"
            ]
          }
        ]
      }
    }
  ],
  "allOf": [
    {
      "if": {
        "properties": {
          "contours": {
            "const": true
          }
        },
        "required": [
          "contours",
          "analysis-token"
        ]
      },
      "then": {
        "required": [
          "contour-token"
        ]
      }
    },
    {
      "if": {
        "properties": {
          "contours": {
            "const": true
          }
        },
        "required": [
          "contours",
          "id"
        ]
      },
      "then": {
        "required": [
          "contour"
        ]
      }
    }
  ],
  "x-m3-contract": {
    "guard_domain": "document",
    "guard_required": true,
    "needs_document": true,
    "selection_mode": "single-object",
    "target_cardinality": {
      "min": 1,
      "max": 1
    },
    "normalization": "one explicit root or retained target; no member expansion",
    "partial_policy": "reject-any-incompatible",
    "role_order": "input-order; each normalized root once",
    "normalization_order": [
      "validate raw schema and exclusive route",
      "select route",
      "fill defaults recursively only in selected branch; pivot suppresses anchor, size suppresses dpi, token suppresses recipe/analyze/solve",
      "parse exact integers",
      "convert all lengths including nested recipes to CSS px and enforce bounds",
      "resolve and normalize targets",
      "canonicalize normalized params for token binding"
    ],
    "native_service": "`publishExplode` -> `publishPieces`, `src/bitmap-explode-chemistry.cpp:683,266`",
    "warnings": [
      {
        "code": "optional-contour-unavailable",
        "emit_site": "prepared piece noContour; image-only piece retained and counted"
      }
    ],
    "error_rows": [
      {
        "layer": "transport/schema",
        "native_branch": "parse_request: request-too-large",
        "code": "request-too-large",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "parse_request: request-too-large",
        "native_evidence": "`publishExplode` -> `publishPieces`, `src/bitmap-explode-chemistry.cpp:683,266`",
        "oracle": "P9:bitmap.explode.explode:transport/schema:request-too-large:parse_request: request-too-large",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "transport/schema",
        "native_branch": "parse_request: repeated-key",
        "code": "repeated-key",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "parse_request: repeated-key",
        "native_evidence": "`publishExplode` -> `publishPieces`, `src/bitmap-explode-chemistry.cpp:683,266`",
        "oracle": "P9:bitmap.explode.explode:transport/schema:repeated-key:parse_request: repeated-key",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "transport/schema",
        "native_branch": "parse_request: invalid-utf8",
        "code": "invalid-utf8",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "parse_request: invalid-utf8",
        "native_evidence": "`publishExplode` -> `publishPieces`, `src/bitmap-explode-chemistry.cpp:683,266`",
        "oracle": "P9:bitmap.explode.explode:transport/schema:invalid-utf8:parse_request: invalid-utf8",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "transport/schema",
        "native_branch": "parse_request: malformed-json",
        "code": "malformed-json",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "parse_request: malformed-json",
        "native_evidence": "`publishExplode` -> `publishPieces`, `src/bitmap-explode-chemistry.cpp:683,266`",
        "oracle": "P9:bitmap.explode.explode:transport/schema:malformed-json:parse_request: malformed-json",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "transport/schema",
        "native_branch": "raw schema/type/range/route/uniqueItems violation",
        "code": "invalid-argument",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "raw schema/type/range/route/uniqueItems violation",
        "native_evidence": "`publishExplode` -> `publishPieces`, `src/bitmap-explode-chemistry.cpp:683,266`",
        "oracle": "P9:bitmap.explode.explode:transport/schema:invalid-argument:raw schema/type/range/route/uniqueItems violation",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "missing current document",
        "code": "no-document",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "missing current document",
        "native_evidence": "`publishExplode` -> `publishPieces`, `src/bitmap-explode-chemistry.cpp:683,266`",
        "oracle": "P9:bitmap.explode.explode:identity/admission:no-document:missing current document",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "document identity mismatch",
        "code": "stale-document",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "document identity mismatch",
        "native_evidence": "`publishExplode` -> `publishPieces`, `src/bitmap-explode-chemistry.cpp:683,266`",
        "oracle": "P9:bitmap.explode.explode:identity/admission:stale-document:document identity mismatch",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "supplied guard differs from document revision",
        "code": "stale-revision",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "supplied guard differs from document revision",
        "native_evidence": "`publishExplode` -> `publishPieces`, `src/bitmap-explode-chemistry.cpp:683,266`",
        "oracle": "P9:bitmap.explode.explode:identity/admission:stale-revision:supplied guard differs from document revision",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "cancellation observed before commit",
        "code": "cancelled",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "cancellation observed before commit",
        "native_evidence": "`publishExplode` -> `publishPieces`, `src/bitmap-explode-chemistry.cpp:683,266`",
        "oracle": "P9:bitmap.explode.explode:identity/admission:cancelled:cancellation observed before commit",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "explicit ID cannot resolve",
        "code": "unknown-id",
        "retryable": false,
        "mutation_state": "none",
        "route": "explicit-target route",
        "detail.reason": "explicit ID cannot resolve",
        "native_evidence": "`publishExplode` -> `publishPieces`, `src/bitmap-explode-chemistry.cpp:683,266`",
        "oracle": "P9:bitmap.explode.explode:identity/admission:unknown-id:explicit ID cannot resolve",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "token missing or wrong kind/parent",
        "code": "invalid-token",
        "retryable": false,
        "mutation_state": "none",
        "route": "token route",
        "detail.reason": "token missing or wrong kind/parent",
        "native_evidence": "`publishExplode` -> `publishPieces`, `src/bitmap-explode-chemistry.cpp:683,266`",
        "oracle": "P9:bitmap.explode.explode:identity/admission:invalid-token:token missing or wrong kind/parent",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "stored document generation/revision differs",
        "code": "stale-plan",
        "retryable": false,
        "mutation_state": "none",
        "route": "token route",
        "detail.reason": "stored document generation/revision differs",
        "native_evidence": "`publishExplode` -> `publishPieces`, `src/bitmap-explode-chemistry.cpp:683,266`",
        "oracle": "P9:bitmap.explode.explode:identity/admission:stale-plan:stored document generation/revision differs",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "stored dependency revalidation failed",
        "code": "stale-dependency",
        "retryable": false,
        "mutation_state": "none",
        "route": "token route",
        "detail.reason": "stored dependency revalidation failed",
        "native_evidence": "`publishExplode` -> `publishPieces`, `src/bitmap-explode-chemistry.cpp:683,266`",
        "oracle": "P9:bitmap.explode.explode:identity/admission:stale-dependency:stored dependency revalidation failed",
        "evidence_status": "required-unimplemented"
      },
{
        "layer": "command-service",
        "native_branch": "context resolve: not one embedded bitmap",
        "code": "requires-single-bitmap",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "context resolve",
        "native_evidence": "`publishExplode` -> `publishPieces`, `src/bitmap-explode-chemistry.cpp:683,266`",
        "oracle": "P9:bitmap.explode.explode:command-service:requires-single-bitmap:context resolve: not one embedded bitmap",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "context reason UnsupportedTarget",
        "code": "unsupported-target",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "context reason UnsupportedTarget",
        "native_evidence": "`publishExplode` -> `publishPieces`, `src/bitmap-explode-chemistry.cpp:683,266`",
        "oracle": "P9:bitmap.explode.explode:command-service:unsupported-target:context reason UnsupportedTarget",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "context reason AnalysisFailed (full recipe only for publication)",
        "code": "analysis-failed",
        "retryable": false,
        "mutation_state": "none",
        "route": "full recipe preparation; contour stage only if requested",
        "detail.reason": "context reason AnalysisFailed (full recipe only for publication)",
        "native_evidence": "`publishExplode` -> `publishPieces`, `src/bitmap-explode-chemistry.cpp:683,266`",
        "oracle": "P9:bitmap.explode.explode:command-service:analysis-failed:context reason AnalysisFailed (full recipe only for publication)",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "context reason MemoryAdmissionFailed",
        "code": "memory-admission-failed",
        "retryable": false,
        "mutation_state": "none",
        "route": "preparation or publication on either route",
        "detail.reason": "context reason MemoryAdmissionFailed",
        "native_evidence": "`publishExplode` -> `publishPieces`, `src/bitmap-explode-chemistry.cpp:683,266`",
        "oracle": "P9:bitmap.explode.explode:command-service:memory-admission-failed:context reason MemoryAdmissionFailed",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "context reason ContourFailed (requested contour stage)",
        "code": "contour-failed",
        "retryable": false,
        "mutation_state": "none",
        "route": "full recipe preparation; contour stage only if requested",
        "detail.reason": "context reason ContourFailed (requested contour stage)",
        "native_evidence": "`publishExplode` -> `publishPieces`, `src/bitmap-explode-chemistry.cpp:683,266`",
        "oracle": "P9:bitmap.explode.explode:command-service:contour-failed:context reason ContourFailed (requested contour stage)",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "context reason EncodingFailed",
        "code": "encoding-failed",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "context reason EncodingFailed",
        "native_evidence": "`publishExplode` -> `publishPieces`, `src/bitmap-explode-chemistry.cpp:683,266`",
        "oracle": "P9:bitmap.explode.explode:command-service:encoding-failed:context reason EncodingFailed",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "context reason PublicationFailed; native rollback",
        "code": "publication-failed",
        "retryable": false,
        "mutation_state": "rolled-back",
        "route": "all",
        "detail.reason": "context reason PublicationFailed; native rollback",
        "native_evidence": "`publishExplode` -> `publishPieces`, `src/bitmap-explode-chemistry.cpp:683,266`",
        "oracle": "P9:bitmap.explode.explode:command-service:publication-failed:context reason PublicationFailed; native rollback",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "context reason DocumentBusy",
        "code": "document-busy",
        "retryable": true,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "context reason DocumentBusy",
        "native_evidence": "`publishExplode` -> `publishPieces`, `src/bitmap-explode-chemistry.cpp:683,266`",
        "oracle": "P9:bitmap.explode.explode:command-service:document-busy:context reason DocumentBusy",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "context reason StaleCapture",
        "code": "stale-dependency",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "context reason StaleCapture",
        "native_evidence": "`publishExplode` -> `publishPieces`, `src/bitmap-explode-chemistry.cpp:683,266`",
        "oracle": "P9:bitmap.explode.explode:command-service:stale-dependency:context reason StaleCapture",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "read-only session rejects requested document mutation",
        "code": "document-read-only",
        "retryable": false,
        "mutation_state": "none",
        "route": "document-edit or history mutation only",
        "detail.reason": "read-only session rejects requested document mutation",
        "native_evidence": "`publishExplode` -> `publishPieces`, `src/bitmap-explode-chemistry.cpp:683,266`",
        "oracle": "P9:bitmap.explode.explode:identity/admission:document-read-only:read-only session rejects requested document mutation",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "unexpected service exception; rollback before return",
        "code": "internal-error",
        "retryable": false,
        "mutation_state": "rolled-back",
        "route": "all",
        "detail.reason": "unexpected service exception; rollback before return",
        "native_evidence": "`publishExplode` -> `publishPieces`, `src/bitmap-explode-chemistry.cpp:683,266`",
        "oracle": "P9:bitmap.explode.explode:command-service:internal-error:unexpected service exception; rollback before return",
        "evidence_status": "required-unimplemented"
      }
    ],
    "success_native_branches": [
      "Bitmap::Status::changed",
      "Bitmap::Status::unchanged"
    ],
    "native_reason_requirement": "P6 must expose typed stage+reason; broad Outcome/unavailable or PublicationRefusal::nativePublication cannot be classified by diagnostic text.",
    "variant_applicability": {
      "success": "ok or changed",
      "unchanged": "native no-op only; query/preparation/history empty are not successful unchanged",
      "computed-dry-run": "successful read-only preflight; no retention, publication, Undo or Redo change"
    },
    "success_rows": [
      {
        "native_branch": "Bitmap::Status::changed",
        "code": null,
        "retryable": false,
        "mutation_state": "committed",
        "data_variant": "success",
        "evidence_route": "P9:bitmap.explode.explode:native:Bitmap::Status::changed",
        "evidence_status": "required-unimplemented"
      },
      {
        "native_branch": "Bitmap::Status::unchanged",
        "code": null,
        "retryable": false,
        "mutation_state": "none",
        "data_variant": "unchanged",
        "evidence_route": "P9:bitmap.explode.explode:native:Bitmap::Status::unchanged",
        "evidence_status": "required-unimplemented"
      }
    ]
  }
})m3").as_object()};
// | `bitmap.explode.create-contour-only` | matching contour-token; OR id+recipe+contour for one-shot; contour-style | group/path IDs, same bitmap element/pixel hash, placement, noContour report | K, T for recipe route, contour-failed, publication-failed | E / one / computed / C single bitmap; publishContourOnly; does not split bitmap |
TypeDescriptor const m3_7{boost::json::parse(R"m3({
  "type": "object",
  "properties": {
    "contour-token": {
      "type": "string",
      "minLength": 1,
      "maxLength": 128
    },
    "id": {
      "type": "string",
      "minLength": 1,
      "maxLength": 128
    },
    "recipe": {
      "type": "object",
      "properties": {
        "threshold": {
          "type": "integer",
          "minimum": 0,
          "maximum": 255,
          "default": 128
        },
        "softness": {
          "type": "integer",
          "minimum": 0,
          "maximum": 127,
          "default": 40
        },
        "faint-floor": {
          "type": "integer",
          "minimum": 0,
          "maximum": 25,
          "default": 5
        },
        "refine": {
          "type": "boolean",
          "default": true
        }
      },
      "required": [],
      "additionalProperties": false
    },
    "contour": {
      "type": "object",
      "properties": {
        "offset": {
          "type": "object",
          "properties": {
            "value": {
              "type": "number",
              "minimum": -1000000.0,
              "maximum": 1000000.0
            },
            "unit": {
              "type": "string",
              "enum": [
                "px",
                "mm",
                "cm",
                "in",
                "pt",
                "pc"
              ]
            }
          },
          "required": [
            "value",
            "unit"
          ],
          "additionalProperties": false,
          "x-css-px-absolute-maximum": 1000000,
          "default": {
            "value": 0,
            "unit": "mm"
          },
          "description": "Convert recursively to CSS px at 96 dpi, then enforce absolute <=1000000 and sign bound. No rounding or clamping.",
          "allOf": [
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "px"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": -1000000.0,
                    "maximum": 1000000.0
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "mm"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": -264583.3333333333,
                    "maximum": 264583.3333333333
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "cm"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": -26458.333333333336,
                    "maximum": 26458.333333333336
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "in"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": -10416.666666666666,
                    "maximum": 10416.666666666666
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "pt"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": -750000.0,
                    "maximum": 750000.0
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "pc"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": -62500.0,
                    "maximum": 62500.0
                  }
                }
              }
            }
          ]
        },
        "gap-tolerance": {
          "type": "object",
          "properties": {
            "value": {
              "type": "number",
              "minimum": 0,
              "maximum": 1000000.0
            },
            "unit": {
              "type": "string",
              "enum": [
                "px",
                "mm",
                "cm",
                "in",
                "pt",
                "pc"
              ]
            }
          },
          "required": [
            "value",
            "unit"
          ],
          "additionalProperties": false,
          "x-css-px-absolute-maximum": 1000000,
          "default": {
            "value": 0.5,
            "unit": "mm"
          },
          "description": "Convert recursively to CSS px at 96 dpi, then enforce absolute <=1000000 and sign bound. No rounding or clamping.",
          "allOf": [
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "px"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": 0,
                    "maximum": 1000000.0
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "mm"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": 0,
                    "maximum": 264583.3333333333
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "cm"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": 0,
                    "maximum": 26458.333333333336
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "in"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": 0,
                    "maximum": 10416.666666666666
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "pt"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": 0,
                    "maximum": 750000.0
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "pc"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": 0,
                    "maximum": 62500.0
                  }
                }
              }
            }
          ]
        },
        "smoothing": {
          "type": "number",
          "minimum": 0,
          "maximum": 100,
          "default": 50
        }
      },
      "required": [],
      "additionalProperties": false
    },
    "contour-style": {
      "type": "object",
      "properties": {
        "stroke": {
          "type": "string",
          "default": "#ff00ff",
          "pattern": "^#[0-9a-fA-F]{6}$"
        },
        "stroke-width": {
          "type": "object",
          "properties": {
            "value": {
              "type": "number",
              "minimum": 0,
              "maximum": 1000000.0,
              "exclusiveMinimum": 0
            },
            "unit": {
              "type": "string",
              "enum": [
                "px",
                "mm",
                "cm",
                "in",
                "pt",
                "pc"
              ]
            }
          },
          "required": [
            "value",
            "unit"
          ],
          "additionalProperties": false,
          "x-css-px-absolute-maximum": 1000000,
          "default": {
            "value": 0.1,
            "unit": "mm"
          },
          "description": "Convert recursively to CSS px at 96 dpi, then enforce absolute <=1000000 and sign bound. No rounding or clamping.",
          "allOf": [
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "px"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": 0,
                    "maximum": 1000000.0
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "mm"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": 0,
                    "maximum": 264583.3333333333
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "cm"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": 0,
                    "maximum": 26458.333333333336
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "in"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": 0,
                    "maximum": 10416.666666666666
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "pt"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": 0,
                    "maximum": 750000.0
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "pc"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": 0,
                    "maximum": 62500.0
                  }
                }
              }
            }
          ]
        }
      },
      "required": [],
      "additionalProperties": false
    }
  },
  "required": [],
  "additionalProperties": false,
  "oneOf": [
    {
      "required": [
        "contour-token"
      ],
      "not": {
        "anyOf": [
          {
            "required": [
              "id"
            ]
          },
          {
            "required": [
              "recipe"
            ]
          },
          {
            "required": [
              "contour"
            ]
          }
        ]
      }
    },
    {
      "required": [
        "id",
        "recipe",
        "contour"
      ],
      "not": {
        "anyOf": [
          {
            "required": [
              "analysis-token"
            ]
          },
          {
            "required": [
              "contour-token"
            ]
          }
        ]
      }
    }
  ],
  "x-m3-contract": {
    "guard_domain": "document",
    "guard_required": true,
    "needs_document": true,
    "selection_mode": "single-object",
    "target_cardinality": {
      "min": 1,
      "max": 1
    },
    "normalization": "one explicit root or retained target; no member expansion",
    "partial_policy": "reject-any-incompatible",
    "role_order": "input-order; each normalized root once",
    "normalization_order": [
      "validate raw schema and exclusive route",
      "select route",
      "fill defaults recursively only in selected branch; pivot suppresses anchor, size suppresses dpi, token suppresses recipe/analyze/solve",
      "parse exact integers",
      "convert all lengths including nested recipes to CSS px and enforce bounds",
      "resolve and normalize targets",
      "canonicalize normalized params for token binding"
    ],
    "native_service": "`publishContourOnly` -> `publishPieces`, same file `:686,266`",
    "warnings": [
      {
        "code": "optional-contour-unavailable",
        "emit_site": "prepared piece noContour; image-only piece retained and counted"
      }
    ],
    "error_rows": [
      {
        "layer": "transport/schema",
        "native_branch": "parse_request: request-too-large",
        "code": "request-too-large",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "parse_request: request-too-large",
        "native_evidence": "`publishContourOnly` -> `publishPieces`, same file `:686,266`",
        "oracle": "P9:bitmap.explode.create-contour-only:transport/schema:request-too-large:parse_request: request-too-large",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "transport/schema",
        "native_branch": "parse_request: repeated-key",
        "code": "repeated-key",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "parse_request: repeated-key",
        "native_evidence": "`publishContourOnly` -> `publishPieces`, same file `:686,266`",
        "oracle": "P9:bitmap.explode.create-contour-only:transport/schema:repeated-key:parse_request: repeated-key",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "transport/schema",
        "native_branch": "parse_request: invalid-utf8",
        "code": "invalid-utf8",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "parse_request: invalid-utf8",
        "native_evidence": "`publishContourOnly` -> `publishPieces`, same file `:686,266`",
        "oracle": "P9:bitmap.explode.create-contour-only:transport/schema:invalid-utf8:parse_request: invalid-utf8",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "transport/schema",
        "native_branch": "parse_request: malformed-json",
        "code": "malformed-json",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "parse_request: malformed-json",
        "native_evidence": "`publishContourOnly` -> `publishPieces`, same file `:686,266`",
        "oracle": "P9:bitmap.explode.create-contour-only:transport/schema:malformed-json:parse_request: malformed-json",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "transport/schema",
        "native_branch": "raw schema/type/range/route/uniqueItems violation",
        "code": "invalid-argument",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "raw schema/type/range/route/uniqueItems violation",
        "native_evidence": "`publishContourOnly` -> `publishPieces`, same file `:686,266`",
        "oracle": "P9:bitmap.explode.create-contour-only:transport/schema:invalid-argument:raw schema/type/range/route/uniqueItems violation",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "missing current document",
        "code": "no-document",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "missing current document",
        "native_evidence": "`publishContourOnly` -> `publishPieces`, same file `:686,266`",
        "oracle": "P9:bitmap.explode.create-contour-only:identity/admission:no-document:missing current document",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "document identity mismatch",
        "code": "stale-document",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "document identity mismatch",
        "native_evidence": "`publishContourOnly` -> `publishPieces`, same file `:686,266`",
        "oracle": "P9:bitmap.explode.create-contour-only:identity/admission:stale-document:document identity mismatch",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "supplied guard differs from document revision",
        "code": "stale-revision",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "supplied guard differs from document revision",
        "native_evidence": "`publishContourOnly` -> `publishPieces`, same file `:686,266`",
        "oracle": "P9:bitmap.explode.create-contour-only:identity/admission:stale-revision:supplied guard differs from document revision",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "cancellation observed before commit",
        "code": "cancelled",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "cancellation observed before commit",
        "native_evidence": "`publishContourOnly` -> `publishPieces`, same file `:686,266`",
        "oracle": "P9:bitmap.explode.create-contour-only:identity/admission:cancelled:cancellation observed before commit",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "explicit ID cannot resolve",
        "code": "unknown-id",
        "retryable": false,
        "mutation_state": "none",
        "route": "explicit-target route",
        "detail.reason": "explicit ID cannot resolve",
        "native_evidence": "`publishContourOnly` -> `publishPieces`, same file `:686,266`",
        "oracle": "P9:bitmap.explode.create-contour-only:identity/admission:unknown-id:explicit ID cannot resolve",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "token missing or wrong kind/parent",
        "code": "invalid-token",
        "retryable": false,
        "mutation_state": "none",
        "route": "token route",
        "detail.reason": "token missing or wrong kind/parent",
        "native_evidence": "`publishContourOnly` -> `publishPieces`, same file `:686,266`",
        "oracle": "P9:bitmap.explode.create-contour-only:identity/admission:invalid-token:token missing or wrong kind/parent",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "stored document generation/revision differs",
        "code": "stale-plan",
        "retryable": false,
        "mutation_state": "none",
        "route": "token route",
        "detail.reason": "stored document generation/revision differs",
        "native_evidence": "`publishContourOnly` -> `publishPieces`, same file `:686,266`",
        "oracle": "P9:bitmap.explode.create-contour-only:identity/admission:stale-plan:stored document generation/revision differs",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "stored dependency revalidation failed",
        "code": "stale-dependency",
        "retryable": false,
        "mutation_state": "none",
        "route": "token route",
        "detail.reason": "stored dependency revalidation failed",
        "native_evidence": "`publishContourOnly` -> `publishPieces`, same file `:686,266`",
        "oracle": "P9:bitmap.explode.create-contour-only:identity/admission:stale-dependency:stored dependency revalidation failed",
        "evidence_status": "required-unimplemented"
      },
{
        "layer": "command-service",
        "native_branch": "context resolve: not one embedded bitmap",
        "code": "requires-single-bitmap",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "context resolve",
        "native_evidence": "`publishContourOnly` -> `publishPieces`, same file `:686,266`",
        "oracle": "P9:bitmap.explode.create-contour-only:command-service:requires-single-bitmap:context resolve: not one embedded bitmap",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "context reason UnsupportedTarget",
        "code": "unsupported-target",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "context reason UnsupportedTarget",
        "native_evidence": "`publishContourOnly` -> `publishPieces`, same file `:686,266`",
        "oracle": "P9:bitmap.explode.create-contour-only:command-service:unsupported-target:context reason UnsupportedTarget",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "context reason AnalysisFailed (full recipe only for publication)",
        "code": "analysis-failed",
        "retryable": false,
        "mutation_state": "none",
        "route": "full recipe preparation; contour stage only if requested",
        "detail.reason": "context reason AnalysisFailed (full recipe only for publication)",
        "native_evidence": "`publishContourOnly` -> `publishPieces`, same file `:686,266`",
        "oracle": "P9:bitmap.explode.create-contour-only:command-service:analysis-failed:context reason AnalysisFailed (full recipe only for publication)",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "context reason MemoryAdmissionFailed",
        "code": "memory-admission-failed",
        "retryable": false,
        "mutation_state": "none",
        "route": "preparation or publication on either route",
        "detail.reason": "context reason MemoryAdmissionFailed",
        "native_evidence": "`publishContourOnly` -> `publishPieces`, same file `:686,266`",
        "oracle": "P9:bitmap.explode.create-contour-only:command-service:memory-admission-failed:context reason MemoryAdmissionFailed",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "context reason ContourFailed (requested contour stage)",
        "code": "contour-failed",
        "retryable": false,
        "mutation_state": "none",
        "route": "full recipe preparation; contour stage only if requested",
        "detail.reason": "context reason ContourFailed (requested contour stage)",
        "native_evidence": "`publishContourOnly` -> `publishPieces`, same file `:686,266`",
        "oracle": "P9:bitmap.explode.create-contour-only:command-service:contour-failed:context reason ContourFailed (requested contour stage)",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "context reason EncodingFailed",
        "code": "encoding-failed",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "context reason EncodingFailed",
        "native_evidence": "`publishContourOnly` -> `publishPieces`, same file `:686,266`",
        "oracle": "P9:bitmap.explode.create-contour-only:command-service:encoding-failed:context reason EncodingFailed",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "context reason PublicationFailed; native rollback",
        "code": "publication-failed",
        "retryable": false,
        "mutation_state": "rolled-back",
        "route": "all",
        "detail.reason": "context reason PublicationFailed; native rollback",
        "native_evidence": "`publishContourOnly` -> `publishPieces`, same file `:686,266`",
        "oracle": "P9:bitmap.explode.create-contour-only:command-service:publication-failed:context reason PublicationFailed; native rollback",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "context reason DocumentBusy",
        "code": "document-busy",
        "retryable": true,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "context reason DocumentBusy",
        "native_evidence": "`publishContourOnly` -> `publishPieces`, same file `:686,266`",
        "oracle": "P9:bitmap.explode.create-contour-only:command-service:document-busy:context reason DocumentBusy",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "context reason StaleCapture",
        "code": "stale-dependency",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "context reason StaleCapture",
        "native_evidence": "`publishContourOnly` -> `publishPieces`, same file `:686,266`",
        "oracle": "P9:bitmap.explode.create-contour-only:command-service:stale-dependency:context reason StaleCapture",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "read-only session rejects requested document mutation",
        "code": "document-read-only",
        "retryable": false,
        "mutation_state": "none",
        "route": "document-edit or history mutation only",
        "detail.reason": "read-only session rejects requested document mutation",
        "native_evidence": "`publishContourOnly` -> `publishPieces`, same file `:686,266`",
        "oracle": "P9:bitmap.explode.create-contour-only:identity/admission:document-read-only:read-only session rejects requested document mutation",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "unexpected service exception; rollback before return",
        "code": "internal-error",
        "retryable": false,
        "mutation_state": "rolled-back",
        "route": "all",
        "detail.reason": "unexpected service exception; rollback before return",
        "native_evidence": "`publishContourOnly` -> `publishPieces`, same file `:686,266`",
        "oracle": "P9:bitmap.explode.create-contour-only:command-service:internal-error:unexpected service exception; rollback before return",
        "evidence_status": "required-unimplemented"
      }
    ],
    "success_native_branches": [
      "Bitmap::Status::changed",
      "Bitmap::Status::unchanged"
    ],
    "native_reason_requirement": "P6 must expose typed stage+reason; broad Outcome/unavailable or PublicationRefusal::nativePublication cannot be classified by diagnostic text.",
    "variant_applicability": {
      "success": "ok or changed",
      "unchanged": "native no-op only; query/preparation/history empty are not successful unchanged",
      "computed-dry-run": "successful read-only preflight; no retention, publication, Undo or Redo change"
    },
    "success_rows": [
      {
        "native_branch": "Bitmap::Status::changed",
        "code": null,
        "retryable": false,
        "mutation_state": "committed",
        "data_variant": "success",
        "evidence_route": "P9:bitmap.explode.create-contour-only:native:Bitmap::Status::changed",
        "evidence_status": "required-unimplemented"
      },
      {
        "native_branch": "Bitmap::Status::unchanged",
        "code": null,
        "retryable": false,
        "mutation_state": "none",
        "data_variant": "unchanged",
        "evidence_route": "P9:bitmap.explode.create-contour-only:native:Bitmap::Status::unchanged",
        "evidence_status": "required-unimplemented"
      }
    ]
  }
})m3").as_object()};
// | `bitmap.explode.apply-adjustment` | analysis-token; OR id+recipe for one-shot | image ID, alpha-only hash/metrics, baked marker, unchanged tone/geometry/profile | K, T for recipe route, unsupported-target, encoding-failed, publication-failed | E / one / computed / M one bitmap; publishAlpha; no second alpha refinement |
TypeDescriptor const m3_8{boost::json::parse(R"m3({
  "type": "object",
  "properties": {
    "analysis-token": {
      "type": "string",
      "minLength": 1,
      "maxLength": 128
    },
    "id": {
      "type": "string",
      "minLength": 1,
      "maxLength": 128
    },
    "recipe": {
      "type": "object",
      "properties": {
        "threshold": {
          "type": "integer",
          "minimum": 0,
          "maximum": 255,
          "default": 128
        },
        "softness": {
          "type": "integer",
          "minimum": 0,
          "maximum": 127,
          "default": 40
        },
        "faint-floor": {
          "type": "integer",
          "minimum": 0,
          "maximum": 25,
          "default": 5
        },
        "refine": {
          "type": "boolean",
          "default": true
        }
      },
      "required": [],
      "additionalProperties": false
    }
  },
  "required": [],
  "additionalProperties": false,
  "oneOf": [
    {
      "required": [
        "analysis-token"
      ],
      "not": {
        "anyOf": [
          {
            "required": [
              "id"
            ]
          },
          {
            "required": [
              "recipe"
            ]
          },
          {
            "required": [
              "contour"
            ]
          }
        ]
      }
    },
    {
      "required": [
        "id",
        "recipe"
      ],
      "not": {
        "anyOf": [
          {
            "required": [
              "analysis-token"
            ]
          },
          {
            "required": [
              "contour-token"
            ]
          }
        ]
      }
    }
  ],
  "x-m3-contract": {
    "guard_domain": "document",
    "guard_required": true,
    "needs_document": true,
    "selection_mode": "single-object",
    "target_cardinality": {
      "min": 1,
      "max": 1
    },
    "normalization": "one explicit root or retained target; no member expansion",
    "partial_policy": "reject-any-incompatible",
    "role_order": "input-order; each normalized root once",
    "normalization_order": [
      "validate raw schema and exclusive route",
      "select route",
      "fill defaults recursively only in selected branch; pivot suppresses anchor, size suppresses dpi, token suppresses recipe/analyze/solve",
      "parse exact integers",
      "convert all lengths including nested recipes to CSS px and enforce bounds",
      "resolve and normalize targets",
      "canonicalize normalized params for token binding"
    ],
    "native_service": "`prepareAlpha/publishAlpha`, same file `:121,134`",
    "warnings": [],
    "error_rows": [
      {
        "layer": "transport/schema",
        "native_branch": "parse_request: request-too-large",
        "code": "request-too-large",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "parse_request: request-too-large",
        "native_evidence": "`prepareAlpha/publishAlpha`, same file `:121,134`",
        "oracle": "P9:bitmap.explode.apply-adjustment:transport/schema:request-too-large:parse_request: request-too-large",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "transport/schema",
        "native_branch": "parse_request: repeated-key",
        "code": "repeated-key",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "parse_request: repeated-key",
        "native_evidence": "`prepareAlpha/publishAlpha`, same file `:121,134`",
        "oracle": "P9:bitmap.explode.apply-adjustment:transport/schema:repeated-key:parse_request: repeated-key",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "transport/schema",
        "native_branch": "parse_request: invalid-utf8",
        "code": "invalid-utf8",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "parse_request: invalid-utf8",
        "native_evidence": "`prepareAlpha/publishAlpha`, same file `:121,134`",
        "oracle": "P9:bitmap.explode.apply-adjustment:transport/schema:invalid-utf8:parse_request: invalid-utf8",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "transport/schema",
        "native_branch": "parse_request: malformed-json",
        "code": "malformed-json",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "parse_request: malformed-json",
        "native_evidence": "`prepareAlpha/publishAlpha`, same file `:121,134`",
        "oracle": "P9:bitmap.explode.apply-adjustment:transport/schema:malformed-json:parse_request: malformed-json",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "transport/schema",
        "native_branch": "raw schema/type/range/route/uniqueItems violation",
        "code": "invalid-argument",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "raw schema/type/range/route/uniqueItems violation",
        "native_evidence": "`prepareAlpha/publishAlpha`, same file `:121,134`",
        "oracle": "P9:bitmap.explode.apply-adjustment:transport/schema:invalid-argument:raw schema/type/range/route/uniqueItems violation",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "missing current document",
        "code": "no-document",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "missing current document",
        "native_evidence": "`prepareAlpha/publishAlpha`, same file `:121,134`",
        "oracle": "P9:bitmap.explode.apply-adjustment:identity/admission:no-document:missing current document",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "document identity mismatch",
        "code": "stale-document",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "document identity mismatch",
        "native_evidence": "`prepareAlpha/publishAlpha`, same file `:121,134`",
        "oracle": "P9:bitmap.explode.apply-adjustment:identity/admission:stale-document:document identity mismatch",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "supplied guard differs from document revision",
        "code": "stale-revision",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "supplied guard differs from document revision",
        "native_evidence": "`prepareAlpha/publishAlpha`, same file `:121,134`",
        "oracle": "P9:bitmap.explode.apply-adjustment:identity/admission:stale-revision:supplied guard differs from document revision",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "cancellation observed before commit",
        "code": "cancelled",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "cancellation observed before commit",
        "native_evidence": "`prepareAlpha/publishAlpha`, same file `:121,134`",
        "oracle": "P9:bitmap.explode.apply-adjustment:identity/admission:cancelled:cancellation observed before commit",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "explicit ID cannot resolve",
        "code": "unknown-id",
        "retryable": false,
        "mutation_state": "none",
        "route": "explicit-target route",
        "detail.reason": "explicit ID cannot resolve",
        "native_evidence": "`prepareAlpha/publishAlpha`, same file `:121,134`",
        "oracle": "P9:bitmap.explode.apply-adjustment:identity/admission:unknown-id:explicit ID cannot resolve",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "token missing or wrong kind/parent",
        "code": "invalid-token",
        "retryable": false,
        "mutation_state": "none",
        "route": "token route",
        "detail.reason": "token missing or wrong kind/parent",
        "native_evidence": "`prepareAlpha/publishAlpha`, same file `:121,134`",
        "oracle": "P9:bitmap.explode.apply-adjustment:identity/admission:invalid-token:token missing or wrong kind/parent",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "stored document generation/revision differs",
        "code": "stale-plan",
        "retryable": false,
        "mutation_state": "none",
        "route": "token route",
        "detail.reason": "stored document generation/revision differs",
        "native_evidence": "`prepareAlpha/publishAlpha`, same file `:121,134`",
        "oracle": "P9:bitmap.explode.apply-adjustment:identity/admission:stale-plan:stored document generation/revision differs",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "stored dependency revalidation failed",
        "code": "stale-dependency",
        "retryable": false,
        "mutation_state": "none",
        "route": "token route",
        "detail.reason": "stored dependency revalidation failed",
        "native_evidence": "`prepareAlpha/publishAlpha`, same file `:121,134`",
        "oracle": "P9:bitmap.explode.apply-adjustment:identity/admission:stale-dependency:stored dependency revalidation failed",
        "evidence_status": "required-unimplemented"
      },
{
        "layer": "command-service",
        "native_branch": "context resolve: not one embedded bitmap",
        "code": "requires-single-bitmap",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "context resolve",
        "native_evidence": "`prepareAlpha/publishAlpha`, same file `:121,134`",
        "oracle": "P9:bitmap.explode.apply-adjustment:command-service:requires-single-bitmap:context resolve: not one embedded bitmap",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "context reason UnsupportedTarget",
        "code": "unsupported-target",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "context reason UnsupportedTarget",
        "native_evidence": "`prepareAlpha/publishAlpha`, same file `:121,134`",
        "oracle": "P9:bitmap.explode.apply-adjustment:command-service:unsupported-target:context reason UnsupportedTarget",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "context reason AnalysisFailed (full recipe only for publication)",
        "code": "analysis-failed",
        "retryable": false,
        "mutation_state": "none",
        "route": "full recipe preparation; contour stage only if requested",
        "detail.reason": "context reason AnalysisFailed (full recipe only for publication)",
        "native_evidence": "`prepareAlpha/publishAlpha`, same file `:121,134`",
        "oracle": "P9:bitmap.explode.apply-adjustment:command-service:analysis-failed:context reason AnalysisFailed (full recipe only for publication)",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "context reason MemoryAdmissionFailed",
        "code": "memory-admission-failed",
        "retryable": false,
        "mutation_state": "none",
        "route": "preparation or publication on either route",
        "detail.reason": "context reason MemoryAdmissionFailed",
        "native_evidence": "`prepareAlpha/publishAlpha`, same file `:121,134`",
        "oracle": "P9:bitmap.explode.apply-adjustment:command-service:memory-admission-failed:context reason MemoryAdmissionFailed",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "context reason EncodingFailed",
        "code": "encoding-failed",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "context reason EncodingFailed",
        "native_evidence": "`prepareAlpha/publishAlpha`, same file `:121,134`",
        "oracle": "P9:bitmap.explode.apply-adjustment:command-service:encoding-failed:context reason EncodingFailed",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "context reason PublicationFailed; native rollback",
        "code": "publication-failed",
        "retryable": false,
        "mutation_state": "rolled-back",
        "route": "all",
        "detail.reason": "context reason PublicationFailed; native rollback",
        "native_evidence": "`prepareAlpha/publishAlpha`, same file `:121,134`",
        "oracle": "P9:bitmap.explode.apply-adjustment:command-service:publication-failed:context reason PublicationFailed; native rollback",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "context reason DocumentBusy",
        "code": "document-busy",
        "retryable": true,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "context reason DocumentBusy",
        "native_evidence": "`prepareAlpha/publishAlpha`, same file `:121,134`",
        "oracle": "P9:bitmap.explode.apply-adjustment:command-service:document-busy:context reason DocumentBusy",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "context reason StaleCapture",
        "code": "stale-dependency",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "context reason StaleCapture",
        "native_evidence": "`prepareAlpha/publishAlpha`, same file `:121,134`",
        "oracle": "P9:bitmap.explode.apply-adjustment:command-service:stale-dependency:context reason StaleCapture",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "read-only session rejects requested document mutation",
        "code": "document-read-only",
        "retryable": false,
        "mutation_state": "none",
        "route": "document-edit or history mutation only",
        "detail.reason": "read-only session rejects requested document mutation",
        "native_evidence": "`prepareAlpha/publishAlpha`, same file `:121,134`",
        "oracle": "P9:bitmap.explode.apply-adjustment:identity/admission:document-read-only:read-only session rejects requested document mutation",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "unexpected service exception; rollback before return",
        "code": "internal-error",
        "retryable": false,
        "mutation_state": "rolled-back",
        "route": "all",
        "detail.reason": "unexpected service exception; rollback before return",
        "native_evidence": "`prepareAlpha/publishAlpha`, same file `:121,134`",
        "oracle": "P9:bitmap.explode.apply-adjustment:command-service:internal-error:unexpected service exception; rollback before return",
        "evidence_status": "required-unimplemented"
      }
    ],
    "success_native_branches": [
      "Bitmap::Status::changed",
      "Bitmap::Status::unchanged"
    ],
    "native_reason_requirement": "P6 must expose typed stage+reason; broad Outcome/unavailable or PublicationRefusal::nativePublication cannot be classified by diagnostic text.",
    "variant_applicability": {
      "success": "ok or changed",
      "unchanged": "native no-op only; query/preparation/history empty are not successful unchanged",
      "computed-dry-run": "successful read-only preflight; no retention, publication, Undo or Redo change"
    },
    "success_rows": [
      {
        "native_branch": "Bitmap::Status::changed",
        "code": null,
        "retryable": false,
        "mutation_state": "committed",
        "data_variant": "success",
        "evidence_route": "P9:bitmap.explode.apply-adjustment:native:Bitmap::Status::changed",
        "evidence_status": "required-unimplemented"
      },
      {
        "native_branch": "Bitmap::Status::unchanged",
        "code": null,
        "retryable": false,
        "mutation_state": "none",
        "data_variant": "unchanged",
        "evidence_route": "P9:bitmap.explode.apply-adjustment:native:Bitmap::Status::unchanged",
        "evidence_status": "required-unimplemented"
      }
    ]
  }
})m3").as_object()};
}
std::vector<PackageCommand> bitmap_commands()
{
    std::vector<PackageCommand> out;
    out.push_back({ActionSpec{.name="bitmap.tone-query", .mode="Q", .summary="Query tone settings for the selected bitmaps.", .canonical_id="bitmap.tone-query", .handler=production_unavailable_action, .version=2, .effects="read-only", .target_policy="Q", .undo_policy="none", .dry_run_grade="computed", .cancellation_boundary="before-native-publication", .input=&m3_0}, "bitmap.tone-query", "read-only", "Q", {}, {{"type","object"},{"properties",boost::json::object{}},{"additionalProperties",false}}, {"slice-unavailable"}, {}});
    out.push_back({ActionSpec{.name="bitmap.histogram", .mode="Q", .summary="Compute the luminance histogram of the target bitmap, optionally remapped by tone settings.", .canonical_id="bitmap.histogram", .handler=production_unavailable_action, .version=2, .effects="read-only", .target_policy="Q", .undo_policy="none", .dry_run_grade="computed", .cancellation_boundary="before-native-publication", .input=&m3_1}, "bitmap.histogram", "read-only", "Q", {}, {{"type","object"},{"properties",boost::json::object{}},{"additionalProperties",false}}, {"slice-unavailable"}, {}});
    out.push_back({ActionSpec{.name="bitmap.tone-set", .mode="M", .summary="Set tone channels on the selected bitmaps.", .canonical_id="bitmap.tone-set", .handler=production_unavailable_action, .version=2, .effects="document-edit", .target_policy="M", .undo_policy="one-step", .dry_run_grade="computed", .cancellation_boundary="before-native-publication", .input=&m3_2}, "bitmap.tone-set", "document-edit", "M", {}, {{"type","object"},{"properties",boost::json::object{}},{"additionalProperties",false}}, {"slice-unavailable"}, {}});
    out.push_back({ActionSpec{.name="bitmap.copy", .mode="C", .summary="Render the selected objects into a new embedded bitmap copy.", .canonical_id="bitmap.copy", .handler=production_unavailable_action, .version=2, .effects="document-edit", .target_policy="C", .undo_policy="one-step", .dry_run_grade="computed", .cancellation_boundary="before-native-publication", .input=&m3_3}, "bitmap.copy", "document-edit", "C", {}, {{"type","object"},{"properties",boost::json::object{}},{"additionalProperties",false}}, {"slice-unavailable"}, {}});
    out.push_back({ActionSpec{.name="bitmap.explode.analyze", .mode="Q", .summary="Analyze a bitmap and return its stable piece indices.", .canonical_id="bitmap.explode.analyze", .handler=production_unavailable_action, .version=2, .effects="read-only", .target_policy="Q", .undo_policy="none", .dry_run_grade="computed", .cancellation_boundary="before-native-publication", .input=&m3_4}, "bitmap.explode.analyze", "read-only", "Q", {}, {{"type","object"},{"properties",boost::json::object{}},{"additionalProperties",false}}, {"slice-unavailable"}, {}});
    out.push_back({ActionSpec{.name="bitmap.explode.contour", .mode="Q", .summary="Create contours from a retained bitmap analysis.", .canonical_id="bitmap.explode.contour", .handler=production_unavailable_action, .version=2, .effects="read-only", .target_policy="Q", .undo_policy="none", .dry_run_grade="computed", .cancellation_boundary="before-native-publication", .input=&m3_5}, "bitmap.explode.contour", "read-only", "Q", {}, {{"type","object"},{"properties",boost::json::object{}},{"additionalProperties",false}}, {"slice-unavailable"}, {}});
    out.push_back({ActionSpec{.name="bitmap.explode.explode", .mode="C", .summary="Publish pieces from a retained bitmap analysis.", .canonical_id="bitmap.explode.explode", .handler=production_unavailable_action, .version=2, .effects="document-edit", .target_policy="C", .undo_policy="one-step", .dry_run_grade="computed", .cancellation_boundary="before-native-publication", .input=&m3_6}, "bitmap.explode.explode", "document-edit", "C", {}, {{"type","object"},{"properties",boost::json::object{}},{"additionalProperties",false}}, {"slice-unavailable"}, {}});
    out.push_back({ActionSpec{.name="bitmap.explode.create-contour-only", .mode="C", .summary="Create contours while preserving the source bitmap.", .canonical_id="bitmap.explode.create-contour-only", .handler=production_unavailable_action, .version=2, .effects="document-edit", .target_policy="C", .undo_policy="one-step", .dry_run_grade="computed", .cancellation_boundary="before-native-publication", .input=&m3_7}, "bitmap.explode.create-contour-only", "document-edit", "C", {}, {{"type","object"},{"properties",boost::json::object{}},{"additionalProperties",false}}, {"slice-unavailable"}, {}});
    out.push_back({ActionSpec{.name="bitmap.explode.apply-adjustment", .mode="M", .summary="Apply alpha adjustments from a retained bitmap analysis.", .canonical_id="bitmap.explode.apply-adjustment", .handler=production_unavailable_action, .version=2, .effects="document-edit", .target_policy="M", .undo_policy="one-step", .dry_run_grade="computed", .cancellation_boundary="before-native-publication", .input=&m3_8}, "bitmap.explode.apply-adjustment", "document-edit", "M", {}, {{"type","object"},{"properties",boost::json::object{}},{"additionalProperties",false}}, {"slice-unavailable"}, {}});
    out[0].example = boost::json::parse(R"m3({
  "ids": [
    "image1"
  ]
})m3").as_object();
    out[0].result_data = boost::json::parse(R"m3({
  "oneOf": [
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "success"
        },
        "members": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "id": {
                "type": "string"
              },
              "values": {
                "type": "object",
                "properties": {
                  "brightness": {
                    "type": "number"
                  },
                  "contrast": {
                    "type": "number"
                  },
                  "intensity": {
                    "type": "number"
                  },
                  "highlights": {
                    "type": "number"
                  },
                  "shadows": {
                    "type": "number"
                  },
                  "midtones": {
                    "type": "number"
                  }
                },
                "required": [
                  "brightness",
                  "contrast",
                  "intensity",
                  "highlights",
                  "shadows",
                  "midtones"
                ],
                "additionalProperties": false
              }
            },
            "required": [
              "id",
              "values"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 1000
        },
        "mixed": {
          "type": "object",
          "properties": {
            "brightness": {
              "type": "boolean"
            },
            "contrast": {
              "type": "boolean"
            },
            "intensity": {
              "type": "boolean"
            },
            "highlights": {
              "type": "boolean"
            },
            "shadows": {
              "type": "boolean"
            },
            "midtones": {
              "type": "boolean"
            }
          },
          "required": [
            "brightness",
            "contrast",
            "intensity",
            "highlights",
            "shadows",
            "midtones"
          ],
          "additionalProperties": false
        },
        "mode": {
          "enum": [
            "compatible-members",
            "legacy-roots"
          ]
        },
        "total-count": {
          "type": "integer",
          "minimum": 0
        },
        "excluded-count": {
          "type": "integer",
          "minimum": 0
        },
        "next-cursor": {
          "type": [
            "string",
            "null"
          ]
        }
      },
      "required": [
        "variant",
        "members",
        "mixed",
        "mode",
        "total-count",
        "excluded-count",
        "next-cursor"
      ],
      "additionalProperties": false
    },
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "unchanged"
        },
        "members": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "id": {
                "type": "string"
              },
              "values": {
                "type": "object",
                "properties": {
                  "brightness": {
                    "type": "number"
                  },
                  "contrast": {
                    "type": "number"
                  },
                  "intensity": {
                    "type": "number"
                  },
                  "highlights": {
                    "type": "number"
                  },
                  "shadows": {
                    "type": "number"
                  },
                  "midtones": {
                    "type": "number"
                  }
                },
                "required": [
                  "brightness",
                  "contrast",
                  "intensity",
                  "highlights",
                  "shadows",
                  "midtones"
                ],
                "additionalProperties": false
              }
            },
            "required": [
              "id",
              "values"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 1000
        },
        "mixed": {
          "type": "object",
          "properties": {
            "brightness": {
              "type": "boolean"
            },
            "contrast": {
              "type": "boolean"
            },
            "intensity": {
              "type": "boolean"
            },
            "highlights": {
              "type": "boolean"
            },
            "shadows": {
              "type": "boolean"
            },
            "midtones": {
              "type": "boolean"
            }
          },
          "required": [
            "brightness",
            "contrast",
            "intensity",
            "highlights",
            "shadows",
            "midtones"
          ],
          "additionalProperties": false
        },
        "mode": {
          "enum": [
            "compatible-members",
            "legacy-roots"
          ]
        },
        "total-count": {
          "type": "integer",
          "minimum": 0
        },
        "excluded-count": {
          "type": "integer",
          "minimum": 0
        },
        "next-cursor": {
          "type": [
            "string",
            "null"
          ]
        }
      },
      "required": [
        "variant",
        "members",
        "mixed",
        "mode",
        "total-count",
        "excluded-count",
        "next-cursor"
      ],
      "additionalProperties": false
    },
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "computed-dry-run"
        },
        "members": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "id": {
                "type": [
                  "string",
                  "null"
                ],
                "description": "null for prospective output; existing source IDs remain strings"
              },
              "values": {
                "type": "object",
                "properties": {
                  "brightness": {
                    "type": "number"
                  },
                  "contrast": {
                    "type": "number"
                  },
                  "intensity": {
                    "type": "number"
                  },
                  "highlights": {
                    "type": "number"
                  },
                  "shadows": {
                    "type": "number"
                  },
                  "midtones": {
                    "type": "number"
                  }
                },
                "required": [
                  "brightness",
                  "contrast",
                  "intensity",
                  "highlights",
                  "shadows",
                  "midtones"
                ],
                "additionalProperties": false
              }
            },
            "required": [
              "id",
              "values"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 1000
        },
        "mixed": {
          "type": "object",
          "properties": {
            "brightness": {
              "type": "boolean"
            },
            "contrast": {
              "type": "boolean"
            },
            "intensity": {
              "type": "boolean"
            },
            "highlights": {
              "type": "boolean"
            },
            "shadows": {
              "type": "boolean"
            },
            "midtones": {
              "type": "boolean"
            }
          },
          "required": [
            "brightness",
            "contrast",
            "intensity",
            "highlights",
            "shadows",
            "midtones"
          ],
          "additionalProperties": false
        },
        "mode": {
          "enum": [
            "compatible-members",
            "legacy-roots"
          ]
        },
        "total-count": {
          "type": "integer",
          "minimum": 0
        },
        "excluded-count": {
          "type": "integer",
          "minimum": 0
        },
        "next-cursor": {
          "type": [
            "string",
            "null"
          ]
        }
      },
      "required": [
        "variant",
        "members",
        "mixed",
        "mode",
        "total-count",
        "excluded-count",
        "next-cursor"
      ],
      "additionalProperties": false
    }
  ],
  "description": "Data only; common va-studio.cli-result/1 envelope authoritative. Variant selected by status/dry_run; any duplicated fields must equal envelope. Preview IDs never claim live publication."
})m3").as_object();
    out[0].errors = {"request-too-large","repeated-key","invalid-utf8","malformed-json","invalid-argument","no-document","stale-document","stale-revision","cancelled","unknown-id","no-eligible-targets","invalid-cursor","internal-error"};
    out[1].example = boost::json::parse(R"m3({
  "ids": [
    "image1"
  ]
})m3").as_object();
    out[1].result_data = boost::json::parse(R"m3({
  "oneOf": [
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "success"
        },
        "counts": {
          "type": "array",
          "items": {
            "type": "integer",
            "minimum": 0
          },
          "minItems": 256,
          "maxItems": 256
        },
        "total-samples": {
          "type": "integer",
          "minimum": 0
        },
        "width": {
          "type": "integer",
          "minimum": 0
        },
        "height": {
          "type": "integer",
          "minimum": 0
        },
        "remap": {
          "enum": [
            "none",
            "current"
          ]
        },
        "remapped": {
          "type": "boolean"
        }
      },
      "required": [
        "variant",
        "counts",
        "total-samples",
        "width",
        "height",
        "remap",
        "remapped"
      ],
      "additionalProperties": false
    },
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "unchanged"
        },
        "counts": {
          "type": "array",
          "items": {
            "type": "integer",
            "minimum": 0
          },
          "minItems": 256,
          "maxItems": 256
        },
        "total-samples": {
          "type": "integer",
          "minimum": 0
        },
        "width": {
          "type": "integer",
          "minimum": 0
        },
        "height": {
          "type": "integer",
          "minimum": 0
        },
        "remap": {
          "enum": [
            "none",
            "current"
          ]
        },
        "remapped": {
          "type": "boolean"
        }
      },
      "required": [
        "variant",
        "counts",
        "total-samples",
        "width",
        "height",
        "remap",
        "remapped"
      ],
      "additionalProperties": false
    },
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "computed-dry-run"
        },
        "counts": {
          "type": "array",
          "items": {
            "type": "integer",
            "minimum": 0
          },
          "minItems": 256,
          "maxItems": 256
        },
        "total-samples": {
          "type": "integer",
          "minimum": 0
        },
        "width": {
          "type": "integer",
          "minimum": 0
        },
        "height": {
          "type": "integer",
          "minimum": 0
        },
        "remap": {
          "enum": [
            "none",
            "current"
          ]
        },
        "remapped": {
          "type": "boolean"
        }
      },
      "required": [
        "variant",
        "counts",
        "total-samples",
        "width",
        "height",
        "remap",
        "remapped"
      ],
      "additionalProperties": false
    }
  ],
  "description": "Data only; common va-studio.cli-result/1 envelope authoritative. Variant selected by status/dry_run; any duplicated fields must equal envelope. Preview IDs never claim live publication."
})m3").as_object();
    out[1].errors = {"request-too-large","repeated-key","invalid-utf8","malformed-json","invalid-argument","no-document","stale-document","stale-revision","cancelled","unknown-id","requires-single-bitmap","missing-source","internal-error"};
    out[2].example = boost::json::parse(R"m3({
  "ids": [
    "image1"
  ],
  "patch": {
    "brightness": 10
  }
})m3").as_object();
    out[2].result_data = boost::json::parse(R"m3({
  "oneOf": [
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "success"
        },
        "members": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "id": {
                "type": "string"
              },
              "before": {
                "type": "object",
                "properties": {
                  "brightness": {
                    "type": "number"
                  },
                  "contrast": {
                    "type": "number"
                  },
                  "intensity": {
                    "type": "number"
                  },
                  "highlights": {
                    "type": "number"
                  },
                  "shadows": {
                    "type": "number"
                  },
                  "midtones": {
                    "type": "number"
                  }
                },
                "required": [
                  "brightness",
                  "contrast",
                  "intensity",
                  "highlights",
                  "shadows",
                  "midtones"
                ],
                "additionalProperties": false
              },
              "after": {
                "type": "object",
                "properties": {
                  "brightness": {
                    "type": "number"
                  },
                  "contrast": {
                    "type": "number"
                  },
                  "intensity": {
                    "type": "number"
                  },
                  "highlights": {
                    "type": "number"
                  },
                  "shadows": {
                    "type": "number"
                  },
                  "midtones": {
                    "type": "number"
                  }
                },
                "required": [
                  "brightness",
                  "contrast",
                  "intensity",
                  "highlights",
                  "shadows",
                  "midtones"
                ],
                "additionalProperties": false
              }
            },
            "required": [
              "id",
              "before",
              "after"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "changed-count": {
          "type": "integer",
          "minimum": 0
        },
        "unchanged-count": {
          "type": "integer",
          "minimum": 0
        },
        "excluded-count": {
          "type": "integer",
          "minimum": 0
        },
        "total-count": {
          "type": "integer",
          "minimum": 0
        }
      },
      "required": [
        "variant",
        "members",
        "changed-count",
        "unchanged-count",
        "excluded-count",
        "total-count"
      ],
      "additionalProperties": false
    },
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "unchanged"
        },
        "members": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "id": {
                "type": "string"
              },
              "before": {
                "type": "object",
                "properties": {
                  "brightness": {
                    "type": "number"
                  },
                  "contrast": {
                    "type": "number"
                  },
                  "intensity": {
                    "type": "number"
                  },
                  "highlights": {
                    "type": "number"
                  },
                  "shadows": {
                    "type": "number"
                  },
                  "midtones": {
                    "type": "number"
                  }
                },
                "required": [
                  "brightness",
                  "contrast",
                  "intensity",
                  "highlights",
                  "shadows",
                  "midtones"
                ],
                "additionalProperties": false
              },
              "after": {
                "type": "object",
                "properties": {
                  "brightness": {
                    "type": "number"
                  },
                  "contrast": {
                    "type": "number"
                  },
                  "intensity": {
                    "type": "number"
                  },
                  "highlights": {
                    "type": "number"
                  },
                  "shadows": {
                    "type": "number"
                  },
                  "midtones": {
                    "type": "number"
                  }
                },
                "required": [
                  "brightness",
                  "contrast",
                  "intensity",
                  "highlights",
                  "shadows",
                  "midtones"
                ],
                "additionalProperties": false
              }
            },
            "required": [
              "id",
              "before",
              "after"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "changed-count": {
          "type": "integer",
          "minimum": 0
        },
        "unchanged-count": {
          "type": "integer",
          "minimum": 0
        },
        "excluded-count": {
          "type": "integer",
          "minimum": 0
        },
        "total-count": {
          "type": "integer",
          "minimum": 0
        }
      },
      "required": [
        "variant",
        "members",
        "changed-count",
        "unchanged-count",
        "excluded-count",
        "total-count"
      ],
      "additionalProperties": false
    },
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "computed-dry-run"
        },
        "members": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "id": {
                "type": [
                  "string",
                  "null"
                ],
                "description": "null for prospective output; existing source IDs remain strings"
              },
              "before": {
                "type": "object",
                "properties": {
                  "brightness": {
                    "type": "number"
                  },
                  "contrast": {
                    "type": "number"
                  },
                  "intensity": {
                    "type": "number"
                  },
                  "highlights": {
                    "type": "number"
                  },
                  "shadows": {
                    "type": "number"
                  },
                  "midtones": {
                    "type": "number"
                  }
                },
                "required": [
                  "brightness",
                  "contrast",
                  "intensity",
                  "highlights",
                  "shadows",
                  "midtones"
                ],
                "additionalProperties": false
              },
              "after": {
                "type": "object",
                "properties": {
                  "brightness": {
                    "type": "number"
                  },
                  "contrast": {
                    "type": "number"
                  },
                  "intensity": {
                    "type": "number"
                  },
                  "highlights": {
                    "type": "number"
                  },
                  "shadows": {
                    "type": "number"
                  },
                  "midtones": {
                    "type": "number"
                  }
                },
                "required": [
                  "brightness",
                  "contrast",
                  "intensity",
                  "highlights",
                  "shadows",
                  "midtones"
                ],
                "additionalProperties": false
              }
            },
            "required": [
              "id",
              "before",
              "after"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "changed-count": {
          "type": "integer",
          "minimum": 0
        },
        "unchanged-count": {
          "type": "integer",
          "minimum": 0
        },
        "excluded-count": {
          "type": "integer",
          "minimum": 0
        },
        "total-count": {
          "type": "integer",
          "minimum": 0
        }
      },
      "required": [
        "variant",
        "members",
        "changed-count",
        "unchanged-count",
        "excluded-count",
        "total-count"
      ],
      "additionalProperties": false
    }
  ],
  "description": "Data only; common va-studio.cli-result/1 envelope authoritative. Variant selected by status/dry_run; any duplicated fields must equal envelope. Preview IDs never claim live publication."
})m3").as_object();
    out[2].errors = {"request-too-large","repeated-key","invalid-utf8","malformed-json","invalid-argument","no-document","stale-document","stale-revision","cancelled","unknown-id","transaction-unavailable","no-eligible-targets","engine-limit","document-read-only","internal-error"};
    out[3].example = boost::json::parse(R"m3({
  "ids": [
    "image1"
  ]
})m3").as_object();
    out[3].result_data = boost::json::parse(R"m3({
  "oneOf": [
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "success"
        },
        "image-id": {
          "type": "string"
        },
        "width": {
          "type": "integer",
          "minimum": 0
        },
        "height": {
          "type": "integer",
          "minimum": 0
        },
        "dpi-x": {
          "type": "number",
          "exclusiveMinimum": 0
        },
        "dpi-y": {
          "type": "number",
          "exclusiveMinimum": 0
        },
        "pixel-sha256": {
          "type": "string",
          "pattern": "^[0-9a-f]{64}$"
        },
        "profile-sha256": {
          "type": "string",
          "pattern": "^[0-9a-f]{64}$"
        },
        "root-affine": {
          "type": "array",
          "items": {
            "type": "number"
          },
          "minItems": 6,
          "maxItems": 6
        },
        "pixel-to-parent": {
          "type": "array",
          "items": {
            "type": "number"
          },
          "minItems": 6,
          "maxItems": 6
        },
        "source-retained": {
          "type": "boolean"
        }
      },
      "required": [
        "variant",
        "image-id",
        "width",
        "height",
        "dpi-x",
        "dpi-y",
        "pixel-sha256",
        "profile-sha256",
        "root-affine",
        "pixel-to-parent",
        "source-retained"
      ],
      "additionalProperties": false
    },
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "unchanged"
        },
        "image-id": {
          "type": "string"
        },
        "width": {
          "type": "integer",
          "minimum": 0
        },
        "height": {
          "type": "integer",
          "minimum": 0
        },
        "dpi-x": {
          "type": "number",
          "exclusiveMinimum": 0
        },
        "dpi-y": {
          "type": "number",
          "exclusiveMinimum": 0
        },
        "pixel-sha256": {
          "type": "string",
          "pattern": "^[0-9a-f]{64}$"
        },
        "profile-sha256": {
          "type": "string",
          "pattern": "^[0-9a-f]{64}$"
        },
        "root-affine": {
          "type": "array",
          "items": {
            "type": "number"
          },
          "minItems": 6,
          "maxItems": 6
        },
        "pixel-to-parent": {
          "type": "array",
          "items": {
            "type": "number"
          },
          "minItems": 6,
          "maxItems": 6
        },
        "source-retained": {
          "type": "boolean"
        }
      },
      "required": [
        "variant",
        "image-id",
        "width",
        "height",
        "dpi-x",
        "dpi-y",
        "pixel-sha256",
        "profile-sha256",
        "root-affine",
        "pixel-to-parent",
        "source-retained"
      ],
      "additionalProperties": false
    },
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "computed-dry-run"
        },
        "image-id": {
          "type": [
            "string",
            "null"
          ],
          "description": "null for prospective output; existing source IDs remain strings"
        },
        "width": {
          "type": "integer",
          "minimum": 0
        },
        "height": {
          "type": "integer",
          "minimum": 0
        },
        "dpi-x": {
          "type": "number",
          "exclusiveMinimum": 0
        },
        "dpi-y": {
          "type": "number",
          "exclusiveMinimum": 0
        },
        "pixel-sha256": {
          "type": "string",
          "pattern": "^[0-9a-f]{64}$"
        },
        "profile-sha256": {
          "type": "string",
          "pattern": "^[0-9a-f]{64}$"
        },
        "root-affine": {
          "type": "array",
          "items": {
            "type": "number"
          },
          "minItems": 6,
          "maxItems": 6
        },
        "pixel-to-parent": {
          "type": "array",
          "items": {
            "type": "number"
          },
          "minItems": 6,
          "maxItems": 6
        },
        "source-retained": {
          "type": "boolean"
        }
      },
      "required": [
        "variant",
        "image-id",
        "width",
        "height",
        "dpi-x",
        "dpi-y",
        "pixel-sha256",
        "profile-sha256",
        "root-affine",
        "pixel-to-parent",
        "source-retained"
      ],
      "additionalProperties": false
    }
  ],
  "description": "Data only; common va-studio.cli-result/1 envelope authoritative. Variant selected by status/dry_run; any duplicated fields must equal envelope. Preview IDs never claim live publication."
})m3").as_object();
    out[3].errors = {"unsupported-target","stale-dependency","request-too-large","repeated-key","invalid-utf8","malformed-json","invalid-argument","no-document","stale-document","stale-revision","cancelled","unknown-id","transaction-unavailable","no-bounds","missing-source","engine-limit","rasterization-failed","encoding-failed","publication-failed","document-read-only","internal-error"};
    out[4].example = boost::json::parse(R"m3({
  "id": "image1",
  "recipe": {}
})m3").as_object();
    out[4].result_data = boost::json::parse(R"m3({
  "oneOf": [
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "success"
        },
        "pieces": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "index": {
                "type": "integer",
                "minimum": 0
              },
              "bounds": {
                "oneOf": [
                  {
                    "type": "array",
                    "items": {
                      "type": "number"
                    },
                    "minItems": 4,
                    "maxItems": 4
                  },
                  {
                    "type": "null"
                  }
                ]
              },
              "pixel-count": {
                "type": "integer",
                "minimum": 0
              },
              "alpha-sum": {
                "type": "integer",
                "minimum": 0
              }
            },
            "required": [
              "index",
              "bounds",
              "pixel-count",
              "alpha-sum"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "piece-count": {
          "type": "integer",
          "minimum": 0
        },
        "analysis-token": {
          "type": [
            "string",
            "null"
          ]
        },
        "recipe": {
          "type": "object",
          "properties": {
            "threshold": {
              "type": "integer",
              "minimum": 0,
              "maximum": 255
            },
            "softness": {
              "type": "integer",
              "minimum": 0,
              "maximum": 127
            },
            "faint-floor": {
              "type": "integer",
              "minimum": 0,
              "maximum": 25
            },
            "refine": {
              "type": "boolean"
            }
          },
          "required": [
            "threshold",
            "softness",
            "faint-floor",
            "refine"
          ],
          "additionalProperties": false
        },
        "budget-bytes": {
          "type": "integer",
          "minimum": 0
        },
        "lost-alpha": {
          "type": "number"
        }
      },
      "required": [
        "variant",
        "pieces",
        "piece-count",
        "analysis-token",
        "recipe",
        "budget-bytes",
        "lost-alpha"
      ],
      "additionalProperties": false
    },
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "unchanged"
        },
        "pieces": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "index": {
                "type": "integer",
                "minimum": 0
              },
              "bounds": {
                "oneOf": [
                  {
                    "type": "array",
                    "items": {
                      "type": "number"
                    },
                    "minItems": 4,
                    "maxItems": 4
                  },
                  {
                    "type": "null"
                  }
                ]
              },
              "pixel-count": {
                "type": "integer",
                "minimum": 0
              },
              "alpha-sum": {
                "type": "integer",
                "minimum": 0
              }
            },
            "required": [
              "index",
              "bounds",
              "pixel-count",
              "alpha-sum"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "piece-count": {
          "type": "integer",
          "minimum": 0
        },
        "analysis-token": {
          "type": [
            "string",
            "null"
          ]
        },
        "recipe": {
          "type": "object",
          "properties": {
            "threshold": {
              "type": "integer",
              "minimum": 0,
              "maximum": 255
            },
            "softness": {
              "type": "integer",
              "minimum": 0,
              "maximum": 127
            },
            "faint-floor": {
              "type": "integer",
              "minimum": 0,
              "maximum": 25
            },
            "refine": {
              "type": "boolean"
            }
          },
          "required": [
            "threshold",
            "softness",
            "faint-floor",
            "refine"
          ],
          "additionalProperties": false
        },
        "budget-bytes": {
          "type": "integer",
          "minimum": 0
        },
        "lost-alpha": {
          "type": "number"
        }
      },
      "required": [
        "variant",
        "pieces",
        "piece-count",
        "analysis-token",
        "recipe",
        "budget-bytes",
        "lost-alpha"
      ],
      "additionalProperties": false
    },
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "computed-dry-run"
        },
        "pieces": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "index": {
                "type": "integer",
                "minimum": 0
              },
              "bounds": {
                "oneOf": [
                  {
                    "type": "array",
                    "items": {
                      "type": "number"
                    },
                    "minItems": 4,
                    "maxItems": 4
                  },
                  {
                    "type": "null"
                  }
                ]
              },
              "pixel-count": {
                "type": "integer",
                "minimum": 0
              },
              "alpha-sum": {
                "type": "integer",
                "minimum": 0
              }
            },
            "required": [
              "index",
              "bounds",
              "pixel-count",
              "alpha-sum"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "piece-count": {
          "type": "integer",
          "minimum": 0
        },
        "analysis-token": {
          "type": "null"
        },
        "recipe": {
          "type": "object",
          "properties": {
            "threshold": {
              "type": "integer",
              "minimum": 0,
              "maximum": 255
            },
            "softness": {
              "type": "integer",
              "minimum": 0,
              "maximum": 127
            },
            "faint-floor": {
              "type": "integer",
              "minimum": 0,
              "maximum": 25
            },
            "refine": {
              "type": "boolean"
            }
          },
          "required": [
            "threshold",
            "softness",
            "faint-floor",
            "refine"
          ],
          "additionalProperties": false
        },
        "budget-bytes": {
          "type": "integer",
          "minimum": 0
        },
        "lost-alpha": {
          "type": "number"
        }
      },
      "required": [
        "variant",
        "pieces",
        "piece-count",
        "analysis-token",
        "recipe",
        "budget-bytes",
        "lost-alpha"
      ],
      "additionalProperties": false
    }
  ],
  "description": "Data only; common va-studio.cli-result/1 envelope authoritative. Variant selected by status/dry_run; any duplicated fields must equal envelope. Preview IDs never claim live publication."
})m3").as_object();
    out[4].errors = {"request-too-large","repeated-key","invalid-utf8","malformed-json","invalid-argument","no-document","stale-document","stale-revision","cancelled","unknown-id","token-capacity","session-required","requires-single-bitmap","unsupported-target","analysis-failed","memory-admission-failed","internal-error"};
    out[5].example = boost::json::parse(R"m3({
  "analysis-token": "token-example",
  "contour": {}
})m3").as_object();
    out[5].result_data = boost::json::parse(R"m3({
  "oneOf": [
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "success"
        },
        "pieces": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "index": {
                "type": "integer",
                "minimum": 0
              },
              "noContour": {
                "type": "boolean"
              },
              "topology-count": {
                "type": "integer",
                "minimum": 0
              },
              "bounds": {
                "oneOf": [
                  {
                    "type": "array",
                    "items": {
                      "type": "number"
                    },
                    "minItems": 4,
                    "maxItems": 4
                  },
                  {
                    "type": "null"
                  }
                ]
              },
              "path-data": {
                "type": "string"
              }
            },
            "required": [
              "index",
              "noContour",
              "topology-count",
              "bounds",
              "path-data"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "contour-token": {
          "type": [
            "string",
            "null"
          ]
        },
        "parent-token": {
          "type": "string"
        },
        "recipe": {
          "type": "object",
          "properties": {
            "threshold": {
              "type": "integer",
              "minimum": 0,
              "maximum": 255
            },
            "softness": {
              "type": "integer",
              "minimum": 0,
              "maximum": 127
            },
            "faint-floor": {
              "type": "integer",
              "minimum": 0,
              "maximum": 25
            },
            "refine": {
              "type": "boolean"
            }
          },
          "required": [
            "threshold",
            "softness",
            "faint-floor",
            "refine"
          ],
          "additionalProperties": false
        },
        "contour": {
          "type": "object",
          "properties": {
            "offset": {
              "type": "number"
            },
            "gap-tolerance": {
              "type": "number",
              "minimum": 0
            },
            "smoothing": {
              "type": "number",
              "minimum": 0,
              "maximum": 100
            }
          },
          "required": [
            "offset",
            "gap-tolerance",
            "smoothing"
          ],
          "additionalProperties": false
        },
        "no-contour-count": {
          "type": "integer",
          "minimum": 0
        }
      },
      "required": [
        "variant",
        "pieces",
        "contour-token",
        "parent-token",
        "recipe",
        "contour",
        "no-contour-count"
      ],
      "additionalProperties": false
    },
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "unchanged"
        },
        "pieces": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "index": {
                "type": "integer",
                "minimum": 0
              },
              "noContour": {
                "type": "boolean"
              },
              "topology-count": {
                "type": "integer",
                "minimum": 0
              },
              "bounds": {
                "oneOf": [
                  {
                    "type": "array",
                    "items": {
                      "type": "number"
                    },
                    "minItems": 4,
                    "maxItems": 4
                  },
                  {
                    "type": "null"
                  }
                ]
              },
              "path-data": {
                "type": "string"
              }
            },
            "required": [
              "index",
              "noContour",
              "topology-count",
              "bounds",
              "path-data"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "contour-token": {
          "type": [
            "string",
            "null"
          ]
        },
        "parent-token": {
          "type": "string"
        },
        "recipe": {
          "type": "object",
          "properties": {
            "threshold": {
              "type": "integer",
              "minimum": 0,
              "maximum": 255
            },
            "softness": {
              "type": "integer",
              "minimum": 0,
              "maximum": 127
            },
            "faint-floor": {
              "type": "integer",
              "minimum": 0,
              "maximum": 25
            },
            "refine": {
              "type": "boolean"
            }
          },
          "required": [
            "threshold",
            "softness",
            "faint-floor",
            "refine"
          ],
          "additionalProperties": false
        },
        "contour": {
          "type": "object",
          "properties": {
            "offset": {
              "type": "number"
            },
            "gap-tolerance": {
              "type": "number",
              "minimum": 0
            },
            "smoothing": {
              "type": "number",
              "minimum": 0,
              "maximum": 100
            }
          },
          "required": [
            "offset",
            "gap-tolerance",
            "smoothing"
          ],
          "additionalProperties": false
        },
        "no-contour-count": {
          "type": "integer",
          "minimum": 0
        }
      },
      "required": [
        "variant",
        "pieces",
        "contour-token",
        "parent-token",
        "recipe",
        "contour",
        "no-contour-count"
      ],
      "additionalProperties": false
    },
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "computed-dry-run"
        },
        "pieces": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "index": {
                "type": "integer",
                "minimum": 0
              },
              "noContour": {
                "type": "boolean"
              },
              "topology-count": {
                "type": "integer",
                "minimum": 0
              },
              "bounds": {
                "oneOf": [
                  {
                    "type": "array",
                    "items": {
                      "type": "number"
                    },
                    "minItems": 4,
                    "maxItems": 4
                  },
                  {
                    "type": "null"
                  }
                ]
              },
              "path-data": {
                "type": "string"
              }
            },
            "required": [
              "index",
              "noContour",
              "topology-count",
              "bounds",
              "path-data"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "contour-token": {
          "type": "null"
        },
        "parent-token": {
          "type": "string"
        },
        "recipe": {
          "type": "object",
          "properties": {
            "threshold": {
              "type": "integer",
              "minimum": 0,
              "maximum": 255
            },
            "softness": {
              "type": "integer",
              "minimum": 0,
              "maximum": 127
            },
            "faint-floor": {
              "type": "integer",
              "minimum": 0,
              "maximum": 25
            },
            "refine": {
              "type": "boolean"
            }
          },
          "required": [
            "threshold",
            "softness",
            "faint-floor",
            "refine"
          ],
          "additionalProperties": false
        },
        "contour": {
          "type": "object",
          "properties": {
            "offset": {
              "type": "number"
            },
            "gap-tolerance": {
              "type": "number",
              "minimum": 0
            },
            "smoothing": {
              "type": "number",
              "minimum": 0,
              "maximum": 100
            }
          },
          "required": [
            "offset",
            "gap-tolerance",
            "smoothing"
          ],
          "additionalProperties": false
        },
        "no-contour-count": {
          "type": "integer",
          "minimum": 0
        }
      },
      "required": [
        "variant",
        "pieces",
        "contour-token",
        "parent-token",
        "recipe",
        "contour",
        "no-contour-count"
      ],
      "additionalProperties": false
    }
  ],
  "description": "Data only; common va-studio.cli-result/1 envelope authoritative. Variant selected by status/dry_run; any duplicated fields must equal envelope. Preview IDs never claim live publication."
})m3").as_object();
    out[5].errors = {"request-too-large","repeated-key","invalid-utf8","malformed-json","invalid-argument","no-document","stale-document","stale-revision","cancelled","invalid-token","stale-plan","stale-dependency","session-required","token-capacity","memory-admission-failed","contour-failed","internal-error"};
    out[6].example = boost::json::parse(R"m3({
  "id": "image1",
  "recipe": {}
})m3").as_object();
    out[6].result_data = boost::json::parse(R"m3({
  "oneOf": [
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "success"
        },
        "publication-map": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "index": {
                "type": "integer",
                "minimum": 0
              },
              "image-id": {
                "type": "string"
              },
              "group-id": {
                "type": [
                  "string",
                  "null"
                ]
              },
              "cut-path-ids": {
                "type": "array",
                "items": {
                  "type": "string"
                },
                "minItems": 0,
                "maxItems": 100000
              },
              "pixel-sha256": {
                "type": "string",
                "pattern": "^[0-9a-f]{64}$"
              },
              "profile-sha256": {
                "type": "string",
                "pattern": "^[0-9a-f]{64}$"
              },
              "root-affine": {
                "type": "array",
                "items": {
                  "type": "number"
                },
                "minItems": 6,
                "maxItems": 6
              },
              "pixel-to-parent": {
                "type": "array",
                "items": {
                  "type": "number"
                },
                "minItems": 6,
                "maxItems": 6
              }
            },
            "required": [
              "index",
              "image-id",
              "group-id",
              "cut-path-ids",
              "pixel-sha256",
              "profile-sha256",
              "root-affine",
              "pixel-to-parent"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "source-retained": {
          "type": "boolean"
        },
        "source-pixel-sha256": {
          "type": "string",
          "pattern": "^[0-9a-f]{64}$"
        },
        "source-profile-sha256": {
          "type": "string",
          "pattern": "^[0-9a-f]{64}$"
        },
        "recipe": {
          "type": "object",
          "properties": {
            "threshold": {
              "type": "integer",
              "minimum": 0,
              "maximum": 255
            },
            "softness": {
              "type": "integer",
              "minimum": 0,
              "maximum": 127
            },
            "faint-floor": {
              "type": "integer",
              "minimum": 0,
              "maximum": 25
            },
            "refine": {
              "type": "boolean"
            }
          },
          "required": [
            "threshold",
            "softness",
            "faint-floor",
            "refine"
          ],
          "additionalProperties": false
        },
        "piece-count": {
          "type": "integer",
          "minimum": 0
        },
        "no-contour-count": {
          "type": "integer",
          "minimum": 0
        }
      },
      "required": [
        "variant",
        "publication-map",
        "source-retained",
        "source-pixel-sha256",
        "source-profile-sha256",
        "recipe",
        "piece-count",
        "no-contour-count"
      ],
      "additionalProperties": false
    },
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "unchanged"
        },
        "publication-map": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "index": {
                "type": "integer",
                "minimum": 0
              },
              "image-id": {
                "type": "string"
              },
              "group-id": {
                "type": [
                  "string",
                  "null"
                ]
              },
              "cut-path-ids": {
                "type": "array",
                "items": {
                  "type": "string"
                },
                "minItems": 0,
                "maxItems": 100000
              },
              "pixel-sha256": {
                "type": "string",
                "pattern": "^[0-9a-f]{64}$"
              },
              "profile-sha256": {
                "type": "string",
                "pattern": "^[0-9a-f]{64}$"
              },
              "root-affine": {
                "type": "array",
                "items": {
                  "type": "number"
                },
                "minItems": 6,
                "maxItems": 6
              },
              "pixel-to-parent": {
                "type": "array",
                "items": {
                  "type": "number"
                },
                "minItems": 6,
                "maxItems": 6
              }
            },
            "required": [
              "index",
              "image-id",
              "group-id",
              "cut-path-ids",
              "pixel-sha256",
              "profile-sha256",
              "root-affine",
              "pixel-to-parent"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 0
        },
        "source-retained": {
          "const": true
        },
        "source-pixel-sha256": {
          "type": "string",
          "pattern": "^[0-9a-f]{64}$"
        },
        "source-profile-sha256": {
          "type": "string",
          "pattern": "^[0-9a-f]{64}$"
        },
        "recipe": {
          "type": "object",
          "properties": {
            "threshold": {
              "type": "integer",
              "minimum": 0,
              "maximum": 255
            },
            "softness": {
              "type": "integer",
              "minimum": 0,
              "maximum": 127
            },
            "faint-floor": {
              "type": "integer",
              "minimum": 0,
              "maximum": 25
            },
            "refine": {
              "type": "boolean"
            }
          },
          "required": [
            "threshold",
            "softness",
            "faint-floor",
            "refine"
          ],
          "additionalProperties": false
        },
        "unchanged-reason": {
          "enum": [
            "zero-pieces",
            "no-contour",
            "already-applied",
            "identity-adjustment"
          ]
        }
      },
      "required": [
        "variant",
        "publication-map",
        "source-retained",
        "source-pixel-sha256",
        "source-profile-sha256",
        "recipe",
        "unchanged-reason"
      ],
      "additionalProperties": false
    },
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "computed-dry-run"
        },
        "publication-map": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "index": {
                "type": "integer",
                "minimum": 0
              },
              "image-id": {
                "type": [
                  "string",
                  "null"
                ],
                "description": "null for prospective output; existing source IDs remain strings"
              },
              "group-id": {
                "type": [
                  "string",
                  "null"
                ]
              },
              "cut-path-ids": {
                "type": "array",
                "items": {
                  "type": [
                    "string",
                    "null"
                  ],
                  "description": "null for prospective output; existing source IDs remain strings"
                },
                "minItems": 0,
                "maxItems": 100000
              },
              "pixel-sha256": {
                "type": "string",
                "pattern": "^[0-9a-f]{64}$"
              },
              "profile-sha256": {
                "type": "string",
                "pattern": "^[0-9a-f]{64}$"
              },
              "root-affine": {
                "type": "array",
                "items": {
                  "type": "number"
                },
                "minItems": 6,
                "maxItems": 6
              },
              "pixel-to-parent": {
                "type": "array",
                "items": {
                  "type": "number"
                },
                "minItems": 6,
                "maxItems": 6
              }
            },
            "required": [
              "index",
              "image-id",
              "group-id",
              "cut-path-ids",
              "pixel-sha256",
              "profile-sha256",
              "root-affine",
              "pixel-to-parent"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "source-retained": {
          "type": "boolean"
        },
        "source-pixel-sha256": {
          "type": "string",
          "pattern": "^[0-9a-f]{64}$"
        },
        "source-profile-sha256": {
          "type": "string",
          "pattern": "^[0-9a-f]{64}$"
        },
        "recipe": {
          "type": "object",
          "properties": {
            "threshold": {
              "type": "integer",
              "minimum": 0,
              "maximum": 255
            },
            "softness": {
              "type": "integer",
              "minimum": 0,
              "maximum": 127
            },
            "faint-floor": {
              "type": "integer",
              "minimum": 0,
              "maximum": 25
            },
            "refine": {
              "type": "boolean"
            }
          },
          "required": [
            "threshold",
            "softness",
            "faint-floor",
            "refine"
          ],
          "additionalProperties": false
        },
        "piece-count": {
          "type": "integer",
          "minimum": 0
        },
        "no-contour-count": {
          "type": "integer",
          "minimum": 0
        }
      },
      "required": [
        "variant",
        "publication-map",
        "source-retained",
        "source-pixel-sha256",
        "source-profile-sha256",
        "recipe",
        "piece-count",
        "no-contour-count"
      ],
      "additionalProperties": false
    }
  ],
  "description": "Data only; common va-studio.cli-result/1 envelope authoritative. Variant selected by status/dry_run; any duplicated fields must equal envelope. Preview IDs never claim live publication."
})m3").as_object();
    out[6].errors = {"request-too-large","repeated-key","invalid-utf8","malformed-json","invalid-argument","no-document","stale-document","stale-revision","cancelled","unknown-id","invalid-token","stale-plan","stale-dependency","session-required","requires-single-bitmap","unsupported-target","analysis-failed","memory-admission-failed","contour-failed","encoding-failed","publication-failed","document-busy","document-read-only","internal-error"};
    out[7].example = boost::json::parse(R"m3({
  "id": "image1",
  "recipe": {},
  "contour": {}
})m3").as_object();
    out[7].result_data = boost::json::parse(R"m3({
  "oneOf": [
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "success"
        },
        "publication-map": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "index": {
                "type": "integer",
                "minimum": 0
              },
              "image-id": {
                "type": "string"
              },
              "group-id": {
                "type": [
                  "string",
                  "null"
                ]
              },
              "cut-path-ids": {
                "type": "array",
                "items": {
                  "type": "string"
                },
                "minItems": 0,
                "maxItems": 100000
              },
              "pixel-sha256": {
                "type": "string",
                "pattern": "^[0-9a-f]{64}$"
              },
              "profile-sha256": {
                "type": "string",
                "pattern": "^[0-9a-f]{64}$"
              },
              "root-affine": {
                "type": "array",
                "items": {
                  "type": "number"
                },
                "minItems": 6,
                "maxItems": 6
              },
              "pixel-to-parent": {
                "type": "array",
                "items": {
                  "type": "number"
                },
                "minItems": 6,
                "maxItems": 6
              }
            },
            "required": [
              "index",
              "image-id",
              "group-id",
              "cut-path-ids",
              "pixel-sha256",
              "profile-sha256",
              "root-affine",
              "pixel-to-parent"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "source-retained": {
          "type": "boolean"
        },
        "source-pixel-sha256": {
          "type": "string",
          "pattern": "^[0-9a-f]{64}$"
        },
        "source-profile-sha256": {
          "type": "string",
          "pattern": "^[0-9a-f]{64}$"
        },
        "recipe": {
          "type": "object",
          "properties": {
            "threshold": {
              "type": "integer",
              "minimum": 0,
              "maximum": 255
            },
            "softness": {
              "type": "integer",
              "minimum": 0,
              "maximum": 127
            },
            "faint-floor": {
              "type": "integer",
              "minimum": 0,
              "maximum": 25
            },
            "refine": {
              "type": "boolean"
            }
          },
          "required": [
            "threshold",
            "softness",
            "faint-floor",
            "refine"
          ],
          "additionalProperties": false
        },
        "piece-count": {
          "type": "integer",
          "minimum": 0
        },
        "no-contour-count": {
          "type": "integer",
          "minimum": 0
        }
      },
      "required": [
        "variant",
        "publication-map",
        "source-retained",
        "source-pixel-sha256",
        "source-profile-sha256",
        "recipe",
        "piece-count",
        "no-contour-count"
      ],
      "additionalProperties": false
    },
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "unchanged"
        },
        "publication-map": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "index": {
                "type": "integer",
                "minimum": 0
              },
              "image-id": {
                "type": "string"
              },
              "group-id": {
                "type": [
                  "string",
                  "null"
                ]
              },
              "cut-path-ids": {
                "type": "array",
                "items": {
                  "type": "string"
                },
                "minItems": 0,
                "maxItems": 100000
              },
              "pixel-sha256": {
                "type": "string",
                "pattern": "^[0-9a-f]{64}$"
              },
              "profile-sha256": {
                "type": "string",
                "pattern": "^[0-9a-f]{64}$"
              },
              "root-affine": {
                "type": "array",
                "items": {
                  "type": "number"
                },
                "minItems": 6,
                "maxItems": 6
              },
              "pixel-to-parent": {
                "type": "array",
                "items": {
                  "type": "number"
                },
                "minItems": 6,
                "maxItems": 6
              }
            },
            "required": [
              "index",
              "image-id",
              "group-id",
              "cut-path-ids",
              "pixel-sha256",
              "profile-sha256",
              "root-affine",
              "pixel-to-parent"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 0
        },
        "source-retained": {
          "const": true
        },
        "source-pixel-sha256": {
          "type": "string",
          "pattern": "^[0-9a-f]{64}$"
        },
        "source-profile-sha256": {
          "type": "string",
          "pattern": "^[0-9a-f]{64}$"
        },
        "recipe": {
          "type": "object",
          "properties": {
            "threshold": {
              "type": "integer",
              "minimum": 0,
              "maximum": 255
            },
            "softness": {
              "type": "integer",
              "minimum": 0,
              "maximum": 127
            },
            "faint-floor": {
              "type": "integer",
              "minimum": 0,
              "maximum": 25
            },
            "refine": {
              "type": "boolean"
            }
          },
          "required": [
            "threshold",
            "softness",
            "faint-floor",
            "refine"
          ],
          "additionalProperties": false
        },
        "unchanged-reason": {
          "enum": [
            "zero-pieces",
            "no-contour",
            "already-applied",
            "identity-adjustment"
          ]
        }
      },
      "required": [
        "variant",
        "publication-map",
        "source-retained",
        "source-pixel-sha256",
        "source-profile-sha256",
        "recipe",
        "unchanged-reason"
      ],
      "additionalProperties": false
    },
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "computed-dry-run"
        },
        "publication-map": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "index": {
                "type": "integer",
                "minimum": 0
              },
              "image-id": {
                "type": [
                  "string",
                  "null"
                ],
                "description": "null for prospective output; existing source IDs remain strings"
              },
              "group-id": {
                "type": [
                  "string",
                  "null"
                ]
              },
              "cut-path-ids": {
                "type": "array",
                "items": {
                  "type": [
                    "string",
                    "null"
                  ],
                  "description": "null for prospective output; existing source IDs remain strings"
                },
                "minItems": 0,
                "maxItems": 100000
              },
              "pixel-sha256": {
                "type": "string",
                "pattern": "^[0-9a-f]{64}$"
              },
              "profile-sha256": {
                "type": "string",
                "pattern": "^[0-9a-f]{64}$"
              },
              "root-affine": {
                "type": "array",
                "items": {
                  "type": "number"
                },
                "minItems": 6,
                "maxItems": 6
              },
              "pixel-to-parent": {
                "type": "array",
                "items": {
                  "type": "number"
                },
                "minItems": 6,
                "maxItems": 6
              }
            },
            "required": [
              "index",
              "image-id",
              "group-id",
              "cut-path-ids",
              "pixel-sha256",
              "profile-sha256",
              "root-affine",
              "pixel-to-parent"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "source-retained": {
          "type": "boolean"
        },
        "source-pixel-sha256": {
          "type": "string",
          "pattern": "^[0-9a-f]{64}$"
        },
        "source-profile-sha256": {
          "type": "string",
          "pattern": "^[0-9a-f]{64}$"
        },
        "recipe": {
          "type": "object",
          "properties": {
            "threshold": {
              "type": "integer",
              "minimum": 0,
              "maximum": 255
            },
            "softness": {
              "type": "integer",
              "minimum": 0,
              "maximum": 127
            },
            "faint-floor": {
              "type": "integer",
              "minimum": 0,
              "maximum": 25
            },
            "refine": {
              "type": "boolean"
            }
          },
          "required": [
            "threshold",
            "softness",
            "faint-floor",
            "refine"
          ],
          "additionalProperties": false
        },
        "piece-count": {
          "type": "integer",
          "minimum": 0
        },
        "no-contour-count": {
          "type": "integer",
          "minimum": 0
        }
      },
      "required": [
        "variant",
        "publication-map",
        "source-retained",
        "source-pixel-sha256",
        "source-profile-sha256",
        "recipe",
        "piece-count",
        "no-contour-count"
      ],
      "additionalProperties": false
    }
  ],
  "description": "Data only; common va-studio.cli-result/1 envelope authoritative. Variant selected by status/dry_run; any duplicated fields must equal envelope. Preview IDs never claim live publication."
})m3").as_object();
    out[7].errors = {"request-too-large","repeated-key","invalid-utf8","malformed-json","invalid-argument","no-document","stale-document","stale-revision","cancelled","unknown-id","invalid-token","stale-plan","stale-dependency","session-required","requires-single-bitmap","unsupported-target","analysis-failed","memory-admission-failed","contour-failed","encoding-failed","publication-failed","document-busy","document-read-only","internal-error"};
    out[8].example = boost::json::parse(R"m3({
  "id": "image1",
  "recipe": {}
})m3").as_object();
    out[8].result_data = boost::json::parse(R"m3({
  "oneOf": [
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "success"
        },
        "publication-map": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "index": {
                "type": "integer",
                "minimum": 0
              },
              "image-id": {
                "type": "string"
              },
              "group-id": {
                "type": [
                  "string",
                  "null"
                ]
              },
              "cut-path-ids": {
                "type": "array",
                "items": {
                  "type": "string"
                },
                "minItems": 0,
                "maxItems": 100000
              },
              "pixel-sha256": {
                "type": "string",
                "pattern": "^[0-9a-f]{64}$"
              },
              "profile-sha256": {
                "type": "string",
                "pattern": "^[0-9a-f]{64}$"
              },
              "root-affine": {
                "type": "array",
                "items": {
                  "type": "number"
                },
                "minItems": 6,
                "maxItems": 6
              },
              "pixel-to-parent": {
                "type": "array",
                "items": {
                  "type": "number"
                },
                "minItems": 6,
                "maxItems": 6
              }
            },
            "required": [
              "index",
              "image-id",
              "group-id",
              "cut-path-ids",
              "pixel-sha256",
              "profile-sha256",
              "root-affine",
              "pixel-to-parent"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "source-retained": {
          "type": "boolean"
        },
        "source-pixel-sha256": {
          "type": "string",
          "pattern": "^[0-9a-f]{64}$"
        },
        "source-profile-sha256": {
          "type": "string",
          "pattern": "^[0-9a-f]{64}$"
        },
        "recipe": {
          "type": "object",
          "properties": {
            "threshold": {
              "type": "integer",
              "minimum": 0,
              "maximum": 255
            },
            "softness": {
              "type": "integer",
              "minimum": 0,
              "maximum": 127
            },
            "faint-floor": {
              "type": "integer",
              "minimum": 0,
              "maximum": 25
            },
            "refine": {
              "type": "boolean"
            }
          },
          "required": [
            "threshold",
            "softness",
            "faint-floor",
            "refine"
          ],
          "additionalProperties": false
        },
        "pixel-sha256": {
          "type": "string",
          "pattern": "^[0-9a-f]{64}$"
        },
        "profile-sha256": {
          "type": "string",
          "pattern": "^[0-9a-f]{64}$"
        },
        "alpha-sha256": {
          "type": "string",
          "pattern": "^[0-9a-f]{64}$"
        },
        "baked": {
          "type": "boolean"
        },
        "root-affine": {
          "type": "array",
          "items": {
            "type": "number"
          },
          "minItems": 6,
          "maxItems": 6
        }
      },
      "required": [
        "variant",
        "publication-map",
        "source-retained",
        "source-pixel-sha256",
        "source-profile-sha256",
        "recipe",
        "pixel-sha256",
        "profile-sha256",
        "alpha-sha256",
        "baked",
        "root-affine"
      ],
      "additionalProperties": false
    },
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "unchanged"
        },
        "publication-map": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "index": {
                "type": "integer",
                "minimum": 0
              },
              "image-id": {
                "type": "string"
              },
              "group-id": {
                "type": [
                  "string",
                  "null"
                ]
              },
              "cut-path-ids": {
                "type": "array",
                "items": {
                  "type": "string"
                },
                "minItems": 0,
                "maxItems": 100000
              },
              "pixel-sha256": {
                "type": "string",
                "pattern": "^[0-9a-f]{64}$"
              },
              "profile-sha256": {
                "type": "string",
                "pattern": "^[0-9a-f]{64}$"
              },
              "root-affine": {
                "type": "array",
                "items": {
                  "type": "number"
                },
                "minItems": 6,
                "maxItems": 6
              },
              "pixel-to-parent": {
                "type": "array",
                "items": {
                  "type": "number"
                },
                "minItems": 6,
                "maxItems": 6
              }
            },
            "required": [
              "index",
              "image-id",
              "group-id",
              "cut-path-ids",
              "pixel-sha256",
              "profile-sha256",
              "root-affine",
              "pixel-to-parent"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 0
        },
        "source-retained": {
          "const": true
        },
        "source-pixel-sha256": {
          "type": "string",
          "pattern": "^[0-9a-f]{64}$"
        },
        "source-profile-sha256": {
          "type": "string",
          "pattern": "^[0-9a-f]{64}$"
        },
        "recipe": {
          "type": "object",
          "properties": {
            "threshold": {
              "type": "integer",
              "minimum": 0,
              "maximum": 255
            },
            "softness": {
              "type": "integer",
              "minimum": 0,
              "maximum": 127
            },
            "faint-floor": {
              "type": "integer",
              "minimum": 0,
              "maximum": 25
            },
            "refine": {
              "type": "boolean"
            }
          },
          "required": [
            "threshold",
            "softness",
            "faint-floor",
            "refine"
          ],
          "additionalProperties": false
        },
        "unchanged-reason": {
          "enum": [
            "zero-pieces",
            "no-contour",
            "already-applied",
            "identity-adjustment"
          ]
        }
      },
      "required": [
        "variant",
        "publication-map",
        "source-retained",
        "source-pixel-sha256",
        "source-profile-sha256",
        "recipe",
        "unchanged-reason"
      ],
      "additionalProperties": false
    },
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "computed-dry-run"
        },
        "publication-map": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "index": {
                "type": "integer",
                "minimum": 0
              },
              "image-id": {
                "type": [
                  "string",
                  "null"
                ],
                "description": "null for prospective output; existing source IDs remain strings"
              },
              "group-id": {
                "type": [
                  "string",
                  "null"
                ]
              },
              "cut-path-ids": {
                "type": "array",
                "items": {
                  "type": [
                    "string",
                    "null"
                  ],
                  "description": "null for prospective output; existing source IDs remain strings"
                },
                "minItems": 0,
                "maxItems": 100000
              },
              "pixel-sha256": {
                "type": "string",
                "pattern": "^[0-9a-f]{64}$"
              },
              "profile-sha256": {
                "type": "string",
                "pattern": "^[0-9a-f]{64}$"
              },
              "root-affine": {
                "type": "array",
                "items": {
                  "type": "number"
                },
                "minItems": 6,
                "maxItems": 6
              },
              "pixel-to-parent": {
                "type": "array",
                "items": {
                  "type": "number"
                },
                "minItems": 6,
                "maxItems": 6
              }
            },
            "required": [
              "index",
              "image-id",
              "group-id",
              "cut-path-ids",
              "pixel-sha256",
              "profile-sha256",
              "root-affine",
              "pixel-to-parent"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "source-retained": {
          "type": "boolean"
        },
        "source-pixel-sha256": {
          "type": "string",
          "pattern": "^[0-9a-f]{64}$"
        },
        "source-profile-sha256": {
          "type": "string",
          "pattern": "^[0-9a-f]{64}$"
        },
        "recipe": {
          "type": "object",
          "properties": {
            "threshold": {
              "type": "integer",
              "minimum": 0,
              "maximum": 255
            },
            "softness": {
              "type": "integer",
              "minimum": 0,
              "maximum": 127
            },
            "faint-floor": {
              "type": "integer",
              "minimum": 0,
              "maximum": 25
            },
            "refine": {
              "type": "boolean"
            }
          },
          "required": [
            "threshold",
            "softness",
            "faint-floor",
            "refine"
          ],
          "additionalProperties": false
        },
        "pixel-sha256": {
          "type": "string",
          "pattern": "^[0-9a-f]{64}$"
        },
        "profile-sha256": {
          "type": "string",
          "pattern": "^[0-9a-f]{64}$"
        },
        "alpha-sha256": {
          "type": "string",
          "pattern": "^[0-9a-f]{64}$"
        },
        "baked": {
          "type": "boolean"
        },
        "root-affine": {
          "type": "array",
          "items": {
            "type": "number"
          },
          "minItems": 6,
          "maxItems": 6
        }
      },
      "required": [
        "variant",
        "publication-map",
        "source-retained",
        "source-pixel-sha256",
        "source-profile-sha256",
        "recipe",
        "pixel-sha256",
        "profile-sha256",
        "alpha-sha256",
        "baked",
        "root-affine"
      ],
      "additionalProperties": false
    }
  ],
  "description": "Data only; common va-studio.cli-result/1 envelope authoritative. Variant selected by status/dry_run; any duplicated fields must equal envelope. Preview IDs never claim live publication."
})m3").as_object();
    out[8].errors = {"request-too-large","repeated-key","invalid-utf8","malformed-json","invalid-argument","no-document","stale-document","stale-revision","cancelled","unknown-id","invalid-token","stale-plan","stale-dependency","session-required","requires-single-bitmap","unsupported-target","analysis-failed","memory-admission-failed","encoding-failed","publication-failed","document-busy","document-read-only","internal-error"};
    out[0].policy = "request-selected-compatible-members-or-legacy-roots"; out[0].spec.target_policy = "request-selected-compatible-members-or-legacy-roots";
    out[1].policy = "single-object"; out[1].spec.target_policy = "single-object";
    out[2].policy = "compatible-members"; out[2].spec.target_policy = "compatible-members";
    out[3].policy = "collective-compositing"; out[3].spec.target_policy = "collective-compositing";
    out[4].policy = "single-object"; out[4].spec.target_policy = "single-object";
    out[5].policy = "single-object"; out[5].spec.target_policy = "single-object";
    out[6].policy = "single-object"; out[6].spec.target_policy = "single-object";
    out[7].policy = "single-object"; out[7].spec.target_policy = "single-object";
    out[8].policy = "single-object"; out[8].spec.target_policy = "single-object";
    out[0].warnings = {"exclusions"};
    out[1].warnings = {};
    out[2].warnings = {"exclusions"};
    out[3].warnings = {};
    out[4].warnings = {};
    out[5].warnings = {"optional-contour-unavailable"};
    out[6].warnings = {"optional-contour-unavailable"};
    out[7].warnings = {"optional-contour-unavailable"};
    out[8].warnings = {};
    return out;
}
}

#include <charconv>
#include "document.h"
#include "ui/bitmap-tone-member-targets.h"
#include "actions/vacards-cli-edit-services.h"
#include "actions/vacards-cli-fault.h"

namespace Inkscape::VACardsCli {
namespace {
boost::json::object tone_values(Filters::BitmapToneSettings const &settings) {
    boost::json::object values;
    for (auto const &[name, property] : tone_properties) values[name] = tone_value(settings, property);
    return values;
}
Record bitmap_refuse(Record r, std::string code, std::string message) {
    r.data.clear();r.created.clear();r.deleted.clear();r.modified.clear();
    r.status = code == "cancelled" ? Status::Cancelled : Status::Rejected;
    r.reason = code; r.message = message; r.error = ParseError{std::move(code), {}, std::move(message)};
    return r;
}
Record bitmap_record(Request const &request, DispatchContext const &context, ProductionContext &p) {
    Record r; r.action = request.command; r.dry_run = request.dry_run;
    r.normalized_params = request.params; r.preferred_unit = context.preferred_unit;
    auto stamp = document_stamp(&p.edits.document);
    r.document_id = stamp.id; r.revision_before = r.revision_after = stamp.revision;
    for (auto item : p.edits.selection.items()) r.selection_after.push_back(item_id(item));
    return r;
}
}
Record execute_bitmap_copy(Request const &, DispatchContext &, ProductionContext &);
Record execute_bitmap(Request const &request, DispatchContext &context, ProductionContext &p) {
    auto r = bitmap_record(request, context, p);
    auto canceled = [&] { return context.cancelled && context.cancelled(); };
    if (canceled()) return bitmap_refuse(std::move(r), "cancelled", "Canceled before bitmap preparation.");
    auto &doc = p.edits.document;
    if (request.command=="bitmap.copy") return execute_bitmap_copy(request,context,p);
    if (request.command.starts_with("bitmap.explode.")) {
        if (!p.bitmap) return bitmap_refuse(std::move(r), "internal-error", "Persistent bitmap capability unavailable.");
        return p.bitmap->execute(request, context, p);
    }
    if (request.command != "bitmap.tone-query" && request.command != "bitmap.tone-set" && request.command != "bitmap.histogram")
        return production_unavailable(request);
    std::vector<std::string> ids;
    std::vector<SPItem *> roots;
    for (auto const &value : request.params.at("ids").as_array()) {
        auto id = std::string(value.as_string()); auto item = cast<SPItem>(doc.getObjectById(id));
        if (!item) return bitmap_refuse(std::move(r), "unknown-id", "Explicit bitmap root does not exist.");
        ids.push_back(std::move(id)); roots.push_back(item);
    }
    r.selected = ids.size();
    auto variant = request.dry_run ? "computed-dry-run" : "success";
    if (request.command == "bitmap.histogram") {
        r.mode = "single-object";
        if (roots.size() != 1 || !is<SPImage>(roots.front()) || !document_available(roots.front()))
            return bitmap_refuse(std::move(r), "requires-single-bitmap", "One available bitmap is required.");
        auto image = cast<SPImage>(roots.front());
        if (!BitmapAdjustments::usable_bitmap(image)) return bitmap_refuse(std::move(r), "missing-source", "No decoded bitmap pixels.");
        auto remap = request.params.if_contains("remap");
        bool current = remap && remap->as_string() == "current", remapped = current;
        auto settings = current ? BitmapAdjustments::canonical_tone(image) : Filters::BitmapToneSettings{};
        for (auto const &[name, property] : tone_properties) if (auto v = request.params.if_contains(name)) {
            Filters::set_bitmap_tone_property(settings, property, v->to_number<double>()); remapped = true;
        }
        cli_fault_throw("bitmap.histogram.before-histogram");
        auto histogram = Filters::build_bitmap_histogram(*image->pixbuf);
        if (remapped) histogram = Filters::remap_bitmap_histogram(histogram, settings);
        boost::json::array counts; for (auto count : histogram.luminance) counts.emplace_back(count);
        r.eligible = 1;
        r.data = {{"variant", variant}, {"counts", std::move(counts)}, {"total-samples", histogram.sampled_pixels},
                  {"width", image->pixbuf->width()}, {"height", image->pixbuf->height()},
                  {"remap", current ? "current" : "none"}, {"remapped", remapped}};
        return r;
    }
    auto modeValue = request.params.if_contains("mode");
    bool members = request.command == "bitmap.tone-set" || (modeValue && modeValue->as_string() == "compatible-members");
    r.mode = members ? "compatible-members" : "legacy-roots";
    std::vector<SPItem *> targets;
    if (members) {
        auto resolved = resolve_tone_members(doc, ids);
        if (resolved.error) return bitmap_refuse(std::move(r), resolved.error->code, resolved.error->message);
        r.excluded = std::move(resolved.targets.exclusions); r.covered = resolved.targets.covered_ids.size();
        for (auto const &id : resolved.targets.ordered_roots) targets.push_back(cast<SPItem>(doc.getObjectById(id)));
    } else {
        auto resolved = BitmapAdjustments::resolve_tone_targets(roots, [](SPItem *i) { return document_available(i); });
        targets = std::move(resolved.items); r.covered = resolved.covered;
        for (auto item : roots) {
            if (!document_available(item)) r.excluded.push_back({item_id(item), "unavailable"});
            else if (auto image = cast<SPImage>(item); image && !BitmapAdjustments::usable_bitmap(image))
                r.excluded.push_back({item_id(item), "missing-source"});
        }
    }
    r.eligible = targets.size(); if (!r.excluded.empty()) r.warnings.push_back("exclusions");
    if (request.command == "bitmap.tone-query") {
        if (targets.empty()) {
            boost::json::array exclusions;
            for (auto const &excluded : r.excluded)
                exclusions.emplace_back(boost::json::object{{"id", excluded.id}, {"reason", excluded.reason}});
            r = bitmap_refuse(std::move(r), "no-eligible-targets", "No compatible bitmap members.");
            r.error_details = {{"reason", "resolved member set empty"}, {"mutation_state", "none"},
                               {"exclusions", std::move(exclusions)}};
            return r;
        }
        cli_fault_throw("bitmap.tone-query.before-aggregate");
        auto aggregate = BitmapAdjustments::aggregate_tone(targets);
        boost::json::object mixed; for (auto const &[name, property] : tone_properties)
            mixed[name] = aggregate[static_cast<std::size_t>(property)].mixed;
        auto binding = request.params; binding.erase("cursor"); binding.erase("limit");
        binding["document"] = r.document_id; binding["revision"] = r.revision_before;
        binding["session-revision"] = context.session_revision; binding["mode"] = r.mode;
        auto hash = catalog_hash(binding); std::size_t offset = 0;
        if (auto cursor = request.params.if_contains("cursor")) {
            auto text = std::string(cursor->as_string()); auto prefix = hash + ":";
            if (!text.starts_with(prefix)) return bitmap_refuse(std::move(r), "invalid-cursor", "Tone cursor binding differs.");
            auto begin = text.data() + prefix.size(); auto end = text.data() + text.size();
            auto parsed = std::from_chars(begin, end, offset);
            if (parsed.ec != std::errc{} || parsed.ptr != end || offset > targets.size())
                return bitmap_refuse(std::move(r), "invalid-cursor", "Invalid tone cursor offset.");
        }
        auto limit = request.params.if_contains("limit");
        auto end = std::min(targets.size(), offset + (limit ? limit->to_number<std::size_t>() : 100));
        boost::json::array values;
        for (auto i = offset; i < end; ++i) values.emplace_back(boost::json::object{
            {"id", item_id(targets[i])}, {"values", tone_values(BitmapAdjustments::canonical_tone(targets[i]))}});
        r.data = {{"variant", variant}, {"members", std::move(values)}, {"mixed", std::move(mixed)}, {"mode", r.mode},
                  {"total-count", targets.size()}, {"excluded-count", r.excluded.size()},
                  {"next-cursor", end < targets.size() ? boost::json::value(hash + ":" + std::to_string(end)) : boost::json::value(nullptr)}};
        return r;
    }
    if (targets.empty()) return bitmap_refuse(std::move(r), "no-eligible-targets", "No compatible bitmap members.");
    Filters::BitmapTonePatch patch;
    for (auto const &[name, property] : tone_properties) if (auto v = request.params.at("patch").as_object().if_contains(name))
        Filters::set_bitmap_tone_patch_property(patch, property, v->to_number<double>());
    std::vector<Filters::BitmapToneSettings> after; std::vector<SPItem *> changed;
    boost::json::array values;
    for (auto target : targets) {
        auto before = BitmapAdjustments::canonical_tone(target);
        auto next = Filters::apply_bitmap_tone_patch(before, patch);
        values.emplace_back(boost::json::object{{"id", item_id(target)}, {"before", tone_values(before)}, {"after", tone_values(next)}});
        if (!Filters::bitmap_tone_settings_equal(before, next)) { changed.push_back(target); after.push_back(next); }
    }
    r.data = {{"variant", request.dry_run ? "computed-dry-run" : changed.empty() ? "unchanged" : "success"},
              {"members", std::move(values)}, {"changed-count", changed.size()}, {"unchanged-count", targets.size()-changed.size()},
              {"excluded-count", r.excluded.size()}, {"total-count", targets.size()}};
    if (canceled()) return bitmap_refuse(std::move(r), "cancelled", "Canceled before tone publication.");
    if (request.dry_run) return r;
    if (changed.empty()) { r.status = Status::Unchanged; return r; }
    // Shared chemistry is the panel's exact sparse-patch operation. One native
    // rollbackable settlement encloses every member; preview never enters it.
    auto guard = DocumentUndo::beginAtomicInteraction(&doc);
    if (!guard) {
        r = bitmap_refuse(std::move(r), "transaction-unavailable", "Tone transaction admission refused.");
        r.error_details = {{"reason", "EditTransaction inactive"}, {"mutation_state", "none"}};
        r.error_retryable = true;
        return r;
    }
    try {
        for (std::size_t i = 0; i < changed.size(); ++i) {
            if (canceled()) { guard->rollback(); return bitmap_refuse(std::move(r), "cancelled", "Canceled before tone settlement."); }
            if (!BitmapAdjustments::apply_tone(changed[i], after[i])) throw std::runtime_error("Tone member publication failed.");
            r.modified.push_back(item_id(changed[i]));
        }
        if (!doc.ensureUpToDate() || !guard->commitAtomically(Util::Internal::ContextString("Adjust object tone"), "", [&] { return !canceled(); }))
            throw std::runtime_error("Tone settlement refused.");
    } catch (...) {
        guard->rollback(); r.modified.clear(); r.data.clear();
        return bitmap_refuse(std::move(r), "internal-error", "Tone operation rolled back.");
    }
    r.status = Status::Changed; r.one_undo_step = true; r.undo_effect = "one-step"; r.publication = "committed";
    r.revision_after = document_stamp(&doc).revision;
    return r;
}
}

#include <thread>
#include <map>
#include <limits>
#include "bitmap-explode-chemistry.h"
#include "ui/explode-bitmap-context.h"
#include "ui/explode-bitmap-panel-preparation.h"
#include "util/bitmap-input-header.h"
#include "object/sp-item-group.h"
#include "object/sp-path.h"
#include "style.h"
#include "xml/href-attribute-helper.h"
#include "xml/node.h"
namespace Inkscape::Bitmap {
namespace {
namespace Cli = VACardsCli;
std::string allocationIdentity() {
    auto uuid=g_uuid_string_random();std::string id=uuid;g_free(uuid);return id;
}
struct AnalysisPayload final : Cli::TokenPayload {
    std::shared_ptr<PanelPreparation::Output const> output;
    SessionRecipe recipe;
    std::string sourcePixels, sourceProfile, sourceAlpha;
    std::string sourceId;
    std::string allocationId=allocationIdentity();
    Cli::TokenKind kind() const noexcept override { return Cli::TokenKind::ExplodeAnalysis; }
};
struct ContourPayload final : Cli::TokenPayload {
    std::shared_ptr<AnalysisPayload const> analysis;
    std::shared_ptr<PanelPreparation::ContourResult const> output;
    PanelPreparation::ContourRecipe recipe;
    std::string allocationId=allocationIdentity();
    Cli::TokenKind kind() const noexcept override { return Cli::TokenKind::ExplodeContour; }
};
std::string digest(void const *data, std::size_t size) {
    auto sum = g_checksum_new(G_CHECKSUM_SHA256);
    if(size) g_checksum_update(sum, static_cast<guchar const *>(data), size);
    std::string result = g_checksum_get_string(sum); g_checksum_free(sum); return result;
}
struct PixelHashes { std::string pixels, profile, alpha; };
PixelHashes pixelHashes(Pixbuf const &source) {
    Pixbuf copy(source); copy.ensurePixelFormat(Pixbuf::PF_GDK); auto raw = copy.getPixbufRaw();
    auto pixels = gdk_pixbuf_get_pixels(raw); auto stride = gdk_pixbuf_get_rowstride(raw);
    auto rgba = g_checksum_new(G_CHECKSUM_SHA256), alpha = g_checksum_new(G_CHECKSUM_SHA256);
    // Frozen M3 representation: uint32 little-endian dimensions precede rows.
    guchar dimensions[8];
    for (unsigned i = 0; i < 4; ++i) {
        dimensions[i] = (static_cast<std::uint32_t>(copy.width()) >> (8*i)) & 0xff;
        dimensions[4+i] = (static_cast<std::uint32_t>(copy.height()) >> (8*i)) & 0xff;
    }
    g_checksum_update(rgba, dimensions, sizeof(dimensions));
    g_checksum_update(alpha, dimensions, sizeof(dimensions));
    for (int y = 0; y < copy.height(); ++y) {
        auto row = pixels + y*stride; g_checksum_update(rgba, row, copy.width()*4);
        for (int x = 0; x < copy.width(); ++x) g_checksum_update(alpha, row+x*4+3, 1);
    }
    PixelHashes result{g_checksum_get_string(rgba), digest(nullptr, 0), g_checksum_get_string(alpha)};
    g_checksum_free(rgba); g_checksum_free(alpha);
    if (auto profile = gdk_pixbuf_get_option(raw, "icc-profile")) {
        gsize size = 0; auto bytes = g_base64_decode(profile, &size); result.profile = digest(bytes, size); g_free(bytes);
    }
    return result;
}
boost::json::object recipeJSON(SessionRecipe const &r) {
    return {{"threshold", r.threshold}, {"softness", r.softness}, {"faint-floor", r.faintFloor}, {"refine", r.refine}};
}
SessionRecipe parseRecipe(boost::json::object const &o) {
    SessionRecipe r; r.threshold=128; r.softness=40;
    if (auto v=o.if_contains("threshold")) r.threshold=v->to_number<unsigned>();
    if (auto v=o.if_contains("softness")) r.softness=v->to_number<unsigned>();
    if (auto v=o.if_contains("faint-floor")) r.faintFloor=v->to_number<unsigned>();
    if (auto v=o.if_contains("refine")) r.refine=v->as_bool();
    return r;
}
double lengthMm(boost::json::value const &v) {
    // The integration layer normalizes length objects to CSS px before adapter admission.
    auto const &o=v.as_object(); auto value=o.at("value").to_number<double>(); auto unit=o.at("unit").as_string();
    if (unit=="mm") return value;
    if (unit=="cm") return value*10;
    if (unit=="in") return value*25.4;
    if (unit=="pt") return value*25.4/72;
    if (unit=="pc") return value*25.4/6;
    return value*25.4/96;
}
PanelPreparation::ContourRecipe parseContour(boost::json::object const &o) {
    PanelPreparation::ContourRecipe r; r.enabled=true;
    if(auto v=o.if_contains("offset")) r.offsetMm=lengthMm(*v);
    if(auto v=o.if_contains("gap-tolerance")) r.gapToleranceMm=lengthMm(*v);
    if(auto v=o.if_contains("smoothing")) r.smoothing=v->to_number<double>();
    return r;
}
char const *failureCode(CliBitmapFailure f) {
    switch(f.reason) {
        case CliBitmapReason::UnsupportedTarget:return "unsupported-target";
        case CliBitmapReason::MemoryAdmissionFailed:return "memory-admission-failed";
        case CliBitmapReason::AnalysisFailed:return "analysis-failed";
        case CliBitmapReason::ContourFailed:return "contour-failed";
        case CliBitmapReason::EncodingFailed:return "encoding-failed";
        case CliBitmapReason::PublicationFailed:return "publication-failed";
        case CliBitmapReason::DocumentBusy:return "document-busy";
        case CliBitmapReason::StaleCapture:return "stale-dependency";
        case CliBitmapReason::Canceled:return "cancelled";
        default:return "internal-error";
    }
}
Cli::Record failureRecord(Cli::Record r, Outcome const &o, std::optional<CliBitmapFailure> failure, bool rolledBack = false) {
    auto code=failure ? failureCode(*failure) : "internal-error";
    // Analyze includes native PNG preparation; its declared service error is
    // analysis-failed, while the receipt still identifies the Encode branch.
    if(failure && r.action=="bitmap.explode.analyze" && failure->reason==CliBitmapReason::EncodingFailed)
        code="analysis-failed";
    r.data.clear(); r=Cli::bitmap_refuse(std::move(r), code, o.diagnostic);
    if(failure) {
        static char const *stages[]={"Resolve","Analyze","Contour","Encode","Publish"};
        static char const *reasons[]={"None","UnsupportedTarget","MemoryAdmissionFailed","AnalysisFailed","ContourFailed",
            "EncodingFailed","PublicationFailed","DocumentBusy","StaleCapture","InternalError","Canceled","MissingSource","EngineLimit"};
        r.error_details={{"reason",std::string("context reason ")+reasons[static_cast<unsigned>(failure->reason)]},
            {"stage",stages[static_cast<unsigned>(failure->stage)]},{"mutation_state",rolledBack ? "rolled-back":"none"}};
        if(r.action=="bitmap.explode.analyze" && failure->reason==CliBitmapReason::EncodingFailed)
            r.error_details["reason"]="context reason EncodingFailed during analysis encoding";
        r.error_retryable=failure->reason==CliBitmapReason::DocumentBusy;
    }
    return r;
}
JobResult runBitmapJob(JobInput input, Cli::DispatchContext &context) {
    auto stage=input.stage;
    recordBitmapMainThread(); bool delivered=false, canceled=false; JobResult result;
    BitmapJobs jobs([&](Ticket,JobResult r) { result=std::move(r); delivered=true; },{},JobClock::now,false);
    auto ticket=jobs.request(std::move(input));
    for (;;) {
        if (!canceled && context.cancelled && context.cancelled()) { canceled=true; jobs.cancel(ticket); }
        jobs.poll();
        if ((delivered || canceled) && !jobs.active()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    jobs.close();
    if (canceled) return {{Status::canceled,"Canceled before bitmap delivery."},{},0,CliBitmapFailure{stage,CliBitmapReason::Canceled}};
    return result;
}
boost::json::array affineJSON(auto const &affine) {
    boost::json::array result; for (unsigned i=0;i<6;++i) result.emplace_back(affine[i]); return result;
}
boost::json::array boundsJSON(double x,double y,double ex,double ey,GridTransform const &m) {
    double minX=std::numeric_limits<double>::infinity(),minY=minX,maxX=-minX,maxY=-minX;
    for (double xx:{x,ex}) for(double yy:{y,ey}) { auto a=xx*m[0]+yy*m[2]+m[4],b=xx*m[1]+yy*m[3]+m[5];
        minX=std::min(minX,a);minY=std::min(minY,b);maxX=std::max(maxX,a);maxY=std::max(maxY,b); }
    return {minX,minY,maxX,maxY};
}
boost::json::array analysisPieces(PanelPreparation::Output const &out) {
    boost::json::array result;
    if (!out.analysis) return result;
    auto const &p=out.analysis->partition; auto grid=out.analysis->grid;
    for(unsigned i=0;i<p.pieceCount;++i) {
        auto const &piece=p.pieces()[i]; std::uint64_t alpha=0;
        for(auto at=p.pieceOffsets()[i];at<p.pieceOffsets()[i+1];++at) {
            auto run=p.run(p.pieceRuns()[at]); if(!run.foreground)continue;
            for(auto x=run.x;x<run.end;++x)alpha+=grid.data[run.y*grid.stride+x*4+3];
        }
        result.emplace_back(boost::json::object{{"index",i},{"bounds",boundsJSON(piece.x,piece.y,piece.endX,piece.endY,out.grid.pixelToDocument)},
            {"pixel-count",piece.area},{"alpha-sum",alpha}});
    }
    return result;
}
}
struct CliSession::Impl {
    struct Owner {
        std::unique_ptr<DocumentPublicationContext> context;
        Prepared prepared;
        std::weak_ptr<AnalysisPayload const> payload;
    };
    std::map<std::string,std::unique_ptr<Owner>> retained;
};
CliSession::CliSession():_impl(std::make_unique<Impl>()) {}
CliSession::~CliSession() { retire(); }
void CliSession::retire() noexcept {
    for(auto &[id,owner]:_impl->retained) { try { owner->context->cancel(); } catch(...) {} }
    _impl->retained.clear();
}
void CliSession::prune(Cli::TokenSnapshot const &snapshot) {
    std::erase_if(_impl->retained,[&](auto const &entry) {
        return std::find(snapshot.active_tokens.begin(),snapshot.active_tokens.end(),entry.first)==snapshot.active_tokens.end();
    });
}
Cli::Record CliSession::execute(Cli::Request const &request,Cli::DispatchContext &dispatch,Cli::ProductionContext &production) {
    auto r=Cli::bitmap_record(request,dispatch,production); auto &doc=production.edits.document;
    auto canceled=[&] { return dispatch.cancelled && dispatch.cancelled(); };
    if(canceled())return Cli::bitmap_refuse(std::move(r),"cancelled","Canceled before bitmap preparation.");
    auto analyze=request.command=="bitmap.explode.analyze", contour=request.command=="bitmap.explode.contour";
    auto alpha=request.command=="bitmap.explode.apply-adjustment", only=request.command=="bitmap.explode.create-contour-only";
    bool retain=(analyze || contour) && (!request.params.if_contains("retain") || request.params.at("retain").as_bool()) && !request.dry_run;
    if(retain && production.session_id.empty())return Cli::bitmap_refuse(std::move(r),"session-required","Retained analysis requires an agent session.");
    Cli::TokenValidationContext validation{production.session_id,r.document_id,"native",production.catalog_identity,
        production.incarnation,r.revision_before,production.target_generation};
    std::shared_ptr<AnalysisPayload const> analysis;
    std::shared_ptr<ContourPayload const> contours;
    std::unique_ptr<Impl::Owner> local;
    Impl::Owner *owner=nullptr;
    std::string rootToken,consumeToken;
    auto analysisId=request.params.if_contains("analysis-token"),contourId=request.params.if_contains("contour-token");
    if(analysisId || contourId) {
        if(contourId) {
            consumeToken=std::string(contourId->as_string());
            auto found=production.tokens.lookup(consumeToken,Cli::TokenKind::ExplodeContour,validation,
                analysisId ? std::optional<Cli::TokenParent>{{std::string(analysisId->as_string()),Cli::TokenKind::ExplodeAnalysis}} : std::nullopt);
            if(found.error)return Cli::bitmap_refuse(std::move(r),found.error->code,found.error->message);
            contours=std::dynamic_pointer_cast<ContourPayload const>(found.value->payload);
            if(!contours || !found.value->parent)return Cli::bitmap_refuse(std::move(r),"invalid-token","Invalid contour payload.");
            rootToken=found.value->parent->id; analysis=contours->analysis;
        } else {
            rootToken=consumeToken=std::string(analysisId->as_string());
            auto found=production.tokens.lookup(rootToken,Cli::TokenKind::ExplodeAnalysis,validation);
            if(found.error)return Cli::bitmap_refuse(std::move(r),found.error->code,found.error->message);
            analysis=std::dynamic_pointer_cast<AnalysisPayload const>(found.value->payload);
        }
        auto found=_impl->retained.find(rootToken);
        if(!analysis || found==_impl->retained.end() || found->second->payload.lock()!=analysis)
            return Cli::bitmap_refuse(std::move(r),"invalid-token","Analysis owner has retired.");
        owner=found->second.get();
        if(!valid(owner->prepared.target,doc) || !valid(owner->prepared.dependencies))
            return Cli::bitmap_refuse(std::move(r),"stale-dependency","Bitmap capture has retired.");
    } else {
        auto id=std::string(request.params.at("id").as_string());
        auto object=doc.getObjectById(id);
        if(!object)return Cli::bitmap_refuse(std::move(r),"unknown-id","Explicit bitmap root does not exist.");
        if(!is<SPImage>(object))return Cli::bitmap_refuse(std::move(r),"requires-single-bitmap","Explode requires one direct embedded bitmap; convert explicitly first.");
        auto made=DocumentPublicationContext::headless(doc,{id},production.incarnation,production.target_generation);
        if(!made.ok())return failureRecord(std::move(r),made.outcome,made.outcome.failure);
        local=std::make_unique<Impl::Owner>(); local->context=std::move(made.value);owner=local.get();
        auto target=resolve(*owner->context,Intent::Explode);
        if(!target.ok())return failureRecord(std::move(r),target.outcome,bitmapFailure(target.outcome,CliBitmapStage::Resolve,CliBitmapReason::UnsupportedTarget));
        auto image=cast<SPImage>(doc.getObjectById(id));
        auto payload=std::make_shared<AnalysisPayload>();payload->sourceId=id;
        payload->recipe=requestRecipe(logicalImageIdentity(*image),parseRecipe(request.params.at("recipe").as_object()));
        auto &p=owner->prepared;p.target=target.value;p.dependencies=capture(target.value);p.activation=owner->context->activation();
        p.requestRecipe=payload->recipe;p.session=sessionJobIdentity(logicalImageIdentity(*image),target.value);
        p.limits=measuredLimits(macDependencyEvidence());
        auto budget=std::make_shared<Budget>(std::numeric_limits<std::uint64_t>::max());
        if(Cli::cli_fault(request.command+".before-memory-admission",Cli::CliFaultKind::MemoryAdmission))
            return failureRecord(std::move(r),{Status::failed,"Injected memory admission denial."},CliBitmapFailure{CliBitmapStage::Analyze,CliBitmapReason::MemoryAdmissionFailed});
        auto memory=sampleMemory();if(!memory.ok())return failureRecord(std::move(r),memory.outcome,CliBitmapFailure{CliBitmapStage::Analyze,CliBitmapReason::MemoryAdmissionFailed});
        auto check=budget->recheckMeasured(memory.value);if(!check.ok())return failureRecord(std::move(r),check,CliBitmapFailure{CliBitmapStage::Analyze,CliBitmapReason::MemoryAdmissionFailed});
        {
            Budget::Token hashStorage;
            check=budget->acquire(Stage::prepared,std::uint64_t(image->pixbuf->width())*image->pixbuf->height()*4+32*MiB,hashStorage);
            if(!check.ok())return failureRecord(std::move(r),check,CliBitmapFailure{CliBitmapStage::Analyze,CliBitmapReason::MemoryAdmissionFailed});
            auto hashes=pixelHashes(*image->pixbuf);payload->sourcePixels=hashes.pixels;payload->sourceProfile=hashes.profile;payload->sourceAlpha=hashes.alpha;
        }
        auto href=getHrefAttribute(*image->getRepr()).second;
        auto uri=inspectUri(href ? href : "",{});if(!uri.ok())return failureRecord(std::move(r),uri.outcome,CliBitmapFailure{CliBitmapStage::Resolve,CliBitmapReason::UnsupportedTarget});
        auto header=inspectHref(href,{},*budget);if(!header.ok())return failureRecord(std::move(r),header.outcome,bitmapFailure(header.outcome,CliBitmapStage::Analyze,CliBitmapReason::AnalysisFailed));
        auto plan=PanelPreparation::resources(header.value.width,header.value.height,uri.value.decodedBytes,budget->reserved(),{true},false);
        auto admitted=admit(plan,memory.value);if(!admitted.ok())return failureRecord(std::move(r),admitted.outcome,CliBitmapFailure{CliBitmapStage::Analyze,CliBitmapReason::MemoryAdmissionFailed});
        auto input=std::make_shared<PanelPreparation::Input>();input->target=target.value;input->recipe=payload->recipe;input->retainAnalysis=true;
        input->recipe.alphaPrepared=payload->recipe.bypassAlpha;input->recipe.bypassAlpha=!payload->recipe.refine || payload->recipe.bypassAlpha;
        input->decodedBytes=uri.value.decodedBytes;
        if(image->getClipObject()) {auto coverage=captureGridCoverage(*image,target.value,*budget);
            if(!coverage.ok())return failureRecord(std::move(r),coverage.outcome,bitmapFailure(coverage.outcome,CliBitmapStage::Analyze,CliBitmapReason::AnalysisFailed));input->coverage=std::move(coverage.value);}
        JobInput job;job.work=PanelPreparation::calculate;job.pixels=std::uint64_t(header.value.width)*header.value.height;job.storage.budget=budget;
        check=budget->acquire(Stage::input,uri.value.payloadLength+sizeof(*input)+4096,job.storage.reservation);
        if(!check.ok())return failureRecord(std::move(r),check,CliBitmapFailure{CliBitmapStage::Analyze,CliBitmapReason::MemoryAdmissionFailed});
        job.storage.bytes.assign(href+uri.value.payloadOffset,href+uri.value.payloadOffset+uri.value.payloadLength);job.storage.payload=input;
        if(Cli::cli_fault(request.command+".before-analysis",Cli::CliFaultKind::Analysis))
            return failureRecord(std::move(r),{Status::failed,"Injected analysis failure."},CliBitmapFailure{CliBitmapStage::Analyze,CliBitmapReason::AnalysisFailed});
        auto result=runBitmapJob(std::move(job),dispatch);if(!result.ok())return failureRecord(std::move(r),result.outcome,result.failure);
        payload->output=std::dynamic_pointer_cast<PanelPreparation::Output const>(result.value.payload);
        if(!payload->output)return Cli::bitmap_refuse(std::move(r),"internal-error","Missing native analysis output.");
        auto const &out=*payload->output;
        if(!alpha && !out.explodeOutcome.ok())return failureRecord(std::move(r),out.explodeOutcome,out.explodeFailure);
        if(alpha && !out.adjustmentOutcome.ok())return failureRecord(std::move(r),out.adjustmentOutcome,out.adjustmentFailure);
        if(!alpha && out.count>MaxExplodePieces)return Cli::bitmap_refuse(std::move(r),"analysis-failed","Native piece limit exceeded.");
        p.grid=&out.grid;p.pieces=&out.pieces;p.budget=budget.get();analysis=payload;owner->payload=analysis;
    }
    auto const &out=*analysis->output;
    if(analyze)Cli::cli_fault_throw(request.command+".after-analysis");
    if(alpha && Cli::cli_fault(request.command+".after-analysis-before-encoding-receipt",Cli::CliFaultKind::Encoding))
        return failureRecord(std::move(r),{Status::failed,"Injected encoding failure."},CliBitmapFailure{CliBitmapStage::Encode,CliBitmapReason::EncodingFailed});
    if(canceled())return Cli::bitmap_refuse(std::move(r),"cancelled","Canceled after bitmap preparation.");
    if(analyze) {
        if(!out.analysis)return failureRecord(std::move(r),out.contours.outcome,out.contours.outcome.failure);
        boost::json::value token=nullptr;
        if(retain) {
            Cli::TokenBinding binding{production.session_id,r.document_id,"native",production.catalog_identity,r.revision_before,
                {analysis->sourceId},{},boost::json::serialize(recipeJSON(analysis->recipe)),production.incarnation,production.target_generation,dispatch.session_revision};
            auto kept=production.tokens.retain(Cli::TokenKind::ExplodeAnalysis,binding,analysis,
                {{"bitmap-analysis-"+analysis->allocationId,out.budget->reserved(),analysis}});
            if(kept.error)return Cli::bitmap_refuse(std::move(r),kept.error->code,kept.error->message);
            token=kept.value->id;_impl->retained.emplace(kept.value->id,std::move(local));
        }
        r.data={{"variant",request.dry_run ? "computed-dry-run":"success"},{"pieces",analysisPieces(out)},{"piece-count",out.count},
            {"analysis-token",std::move(token)},{"recipe",recipeJSON(analysis->recipe)},{"budget-bytes",out.budget->reserved()},{"lost-alpha",out.lostArea}};
        return r;
    }
    bool needContour=contour || only || (request.params.if_contains("contours") && request.params.at("contours").as_bool());
    if(needContour && !contours) {
        if(!out.analysis)return Cli::bitmap_refuse(std::move(r),"contour-failed","Retained topology unavailable.");
        auto cp=std::make_shared<ContourPayload>();cp->analysis=analysis;
        cp->recipe=parseContour(request.params.at("contour").as_object());
        auto input=std::make_shared<PanelPreparation::ContourInput>();input->analysis=out.analysis;
        input->expectedIdentity=PanelPreparation::analysisIdentity(out.grid,owner->prepared.target);input->contour=cp->recipe;
        JobInput job;job.stage=CliBitmapStage::Contour;job.work=PanelPreparation::calculateContours;job.pixels=std::uint64_t(out.grid.width)*out.grid.height;
        job.storage.budget=out.budget;job.storage.payload=input;
        if(Cli::cli_fault(request.command+".before-contour-memory-admission",Cli::CliFaultKind::MemoryAdmission))
            return failureRecord(std::move(r),{Status::failed,"Injected contour admission denial."},CliBitmapFailure{CliBitmapStage::Contour,CliBitmapReason::MemoryAdmissionFailed});
        if(Cli::cli_fault(request.command+".before-contour",Cli::CliFaultKind::Contour))
            return failureRecord(std::move(r),{Status::failed,"Injected contour failure."},CliBitmapFailure{CliBitmapStage::Contour,CliBitmapReason::ContourFailed});
        auto result=runBitmapJob(std::move(job),dispatch);if(!result.ok())return failureRecord(std::move(r),result.outcome,result.failure);
        cp->output=std::dynamic_pointer_cast<PanelPreparation::ContourResult const>(result.value.payload);
        if(!cp->output)return Cli::bitmap_refuse(std::move(r),"internal-error","Missing contour output.");
        if(!cp->output->outcome.ok())return failureRecord(std::move(r),cp->output->outcome,cp->output->outcome.failure);
        contours=cp;
    }
    unsigned absent=0; auto fitted=contours && contours->output->product ? &contours->output->product->fitted:nullptr;
    if(fitted)for(unsigned i=0;i<fitted->pieceCount;++i)absent+=fitted->pieces()[i].noContour;
    if(absent && !alpha)r.warnings.push_back("optional-contour-unavailable");
    if(contour) {
        Cli::cli_fault_throw(request.command+".after-contour");
        boost::json::array pieces;
        if(fitted)for(unsigned i=0;i<fitted->pieceCount;++i) {
            auto piece=fitted->pieces()[i];boost::json::value bounds=nullptr;
            if(piece.ringBegin!=piece.ringEnd) {
                auto const &first=fitted->rings()[piece.ringBegin];double x=first.minX,y=first.minY,ex=first.maxX,ey=first.maxY;
                for(auto j=piece.ringBegin+1;j<piece.ringEnd;++j) {auto const &ring=fitted->rings()[j];x=std::min(x,ring.minX);y=std::min(y,ring.minY);ex=std::max(ex,ring.maxX);ey=std::max(ey,ring.maxY);}
                bounds=boundsJSON(x,y,ex,ey,out.grid.pixelToDocument);
            }
            pieces.emplace_back(boost::json::object{{"index",i},{"noContour",piece.noContour},{"topology-count",piece.ringEnd-piece.ringBegin},
                {"bounds",std::move(bounds)},{"path-data",serializeContours(*fitted,piece.ringBegin,piece.ringEnd)}});
        }
        boost::json::value token=nullptr;
        if(retain) {
            auto root=production.tokens.lookup(rootToken,Cli::TokenKind::ExplodeAnalysis,validation);
            if(root.error)return Cli::bitmap_refuse(std::move(r),root.error->code,root.error->message);
            auto binding=root.value->binding;binding.normalized_params=boost::json::serialize(request.params);
            auto allocations=root.value->allocations;
            auto bytes=contours->output->product ? contours->output->product->reservation.bytes() : 0;
            allocations.push_back({"bitmap-contour-"+contours->allocationId,bytes,contours});
            auto kept=production.tokens.retain(Cli::TokenKind::ExplodeContour,binding,contours,std::move(allocations),Cli::TokenParent{rootToken,Cli::TokenKind::ExplodeAnalysis});
            if(kept.error)return Cli::bitmap_refuse(std::move(r),kept.error->code,kept.error->message);token=kept.value->id;
        }
        r.data={{"variant",request.dry_run ? "computed-dry-run":"success"},{"pieces",std::move(pieces)},
            {"contour-token",std::move(token)},{"parent-token",rootToken},{"recipe",recipeJSON(analysis->recipe)},
            {"contour",boost::json::object{{"offset",contours->recipe.offsetMm*96/25.4},{"gap-tolerance",contours->recipe.gapToleranceMm*96/25.4},{"smoothing",contours->recipe.smoothing}}},
            {"no-contour-count",absent}};return r;
    }
    auto p=owner->prepared;p.contours=needContour ? fitted:nullptr;
    if(auto style=request.params.if_contains("contour-style")) {
        auto const &o=style->as_object();if(auto stroke=o.if_contains("stroke"))p.contourStyle.stroke=std::string(stroke->as_string());
        if(auto width=o.if_contains("stroke-width"))p.contourStyle.strokeWidthMm=lengthMm(*width);
    }
    auto unchanged=alpha ? (!out.adjustment || !analysis->recipe.refine && !analysis->recipe.faintFloor || analysis->recipe.bypassAlpha)
                         : only ? !fitted || !fitted->ringCount : !out.count;
    if(unchanged) {
        r.status=request.dry_run ? Cli::Status::Ok : Cli::Status::Unchanged;
        r.data={{"variant","unchanged"},{"publication-map",boost::json::array{}},{"source-retained",true},
            {"source-pixel-sha256",analysis->sourcePixels},{"source-profile-sha256",analysis->sourceProfile},
            {"recipe",recipeJSON(analysis->recipe)},{"unchanged-reason",alpha ? (analysis->recipe.bypassAlpha ? "already-applied":"identity-adjustment") : only ? "no-contour":"zero-pieces"}};
        if(request.dry_run) {
            r.data["variant"]="computed-dry-run";r.data.erase("unchanged-reason");
            if(alpha) {
                r.data["pixel-sha256"]=analysis->sourcePixels;r.data["profile-sha256"]=analysis->sourceProfile;
                r.data["alpha-sha256"]=analysis->sourceAlpha;r.data["baked"]=analysis->recipe.bypassAlpha;
                r.data["root-affine"]=affineJSON(cast<SPItem>(doc.getObjectById(analysis->sourceId))->i2doc_affine());
            } else {r.data["piece-count"]=out.count;r.data["no-contour-count"]=absent;}
        }
        return r;
    }
    // Compute prospective hashes from native encoded products, never synthesize IDs.
    boost::json::array map;std::vector<PixelHashes> hashes;unsigned count=alpha || only ? 1:out.count;
    for(unsigned i=0;i<count;++i) {
        PixelHashes h{analysis->sourcePixels,analysis->sourceProfile,analysis->sourceAlpha};
        if(!only) {
            auto const &piece=alpha ? out.adjustment->image.piece(0):out.pieces.piece(i);
            Budget::Token hashStorage;
            auto admitted=out.budget->acquire(Stage::prepared,std::uint64_t(piece.width)*piece.height*8+32*MiB,hashStorage);
            if(!admitted.ok())return failureRecord(std::move(r),admitted,CliBitmapFailure{CliBitmapStage::Encode,CliBitmapReason::MemoryAdmissionFailed});
            std::unique_ptr<Pixbuf> pix(Pixbuf::create_from_buffer(std::string(reinterpret_cast<char const *>(piece.data),piece.size)));
            if(!pix)return Cli::bitmap_refuse(std::move(r),"encoding-failed","Prepared PNG could not be decoded for receipt.");h=pixelHashes(*pix);
        }
        hashes.push_back(h);
        auto source=cast<SPImage>(doc.getObjectById(analysis->sourceId));
        auto root=only ? cast<SPItem>(source->parent)->i2doc_affine() : alpha ? source->i2doc_affine() :
            Geom::Affine(p.grid->pixelToDocument[0],p.grid->pixelToDocument[1],p.grid->pixelToDocument[2],p.grid->pixelToDocument[3],p.grid->pixelToDocument[4],p.grid->pixelToDocument[5]);
        auto pixelsToParent=source->c2p*source->transform;
        if(!only && !alpha) {
            auto const &piece=out.pieces.piece(i);pixelsToParent=Geom::Translate(piece.x,piece.y);
            if(!needContour)pixelsToParent*=Geom::Affine(p.grid->pixelToParent[0],p.grid->pixelToParent[1],p.grid->pixelToParent[2],p.grid->pixelToParent[3],p.grid->pixelToParent[4],p.grid->pixelToParent[5]);
        }
        boost::json::array paths;
        map.emplace_back(boost::json::object{{"index",i},{"image-id",only || alpha ? boost::json::value(analysis->sourceId):boost::json::value(nullptr)},
            {"group-id",nullptr},{"cut-path-ids",std::move(paths)},{"pixel-sha256",h.pixels},{"profile-sha256",h.profile},
            {"root-affine",affineJSON(root)},{"pixel-to-parent",affineJSON(pixelsToParent)}});
    }
    PublicationResult published;
    if(!request.dry_run) {
        if(canceled())return Cli::bitmap_refuse(std::move(r),"cancelled","Canceled before native bitmap publication.");
        if(Cli::cli_fault(request.command+".before-publication-validation",Cli::CliFaultKind::StaleCapture))
            p.target.desktop=0; // invalid caller-local snapshot; native admission remains authoritative
        bool serviceException=false;
        struct FaultPublication { std::string command; bool *exception; } fault{request.command,&serviceException};
        p.hooks={[](PublishStage stage,unsigned,void *data) {
            auto &f=*static_cast<FaultPublication *>(data);
            if(stage!=PublishStage::Native)return; // after XML binding and native update, before settlement
            if(Cli::cli_fault(f.command+".after-native-before-settlement",Cli::CliFaultKind::Publication))
                throw std::runtime_error("Injected native publication failure");
            try {Cli::cli_fault_throw(f.command+".after-native-before-settlement");}
            catch(...) {*f.exception=true;throw;}
        },&fault};
        static Ticket nextTicket=1;auto ticket=nextTicket++;
        if(alpha) {
            // Borrow immutable alpha pixels with the separate request-local publication snapshot.
            published=publishAlpha(*owner->context,*out.adjustment,p,ticket);
        } else if(only)published=publishContourOnly(*owner->context,{p},ticket);
        else published=publishExplode(*owner->context,p,ticket);
        if(serviceException && published.rolledBack)throw std::runtime_error("Bitmap service exception after native rollback");
        if(!published.ok())return failureRecord(std::move(r),published,published.failure,published.rolledBack);
        if(published.status==Status::unchanged) {
            r.status=Cli::Status::Unchanged;
            r.data={{"variant","unchanged"},{"publication-map",boost::json::array{}},{"source-retained",true},
                {"source-pixel-sha256",analysis->sourcePixels},{"source-profile-sha256",analysis->sourceProfile},
                {"recipe",recipeJSON(analysis->recipe)},{"unchanged-reason",alpha ? "identity-adjustment" : only ? "no-contour":"zero-pieces"}};
            return r;
        }
        for(unsigned i=0;i<count;++i) {
            auto &row=map[i].as_object();auto item=cast<SPItem>(doc.getObjectById(published.selectionAfter.at(i)));
            if(!item)throw std::runtime_error("Committed bitmap output missing");
            row["root-affine"]=affineJSON(item->i2doc_affine());boost::json::array paths;
            if(auto group=cast<SPGroup>(item)) {
                row["group-id"]=std::string(group->getId());
                for(auto &child:group->children) {if(auto image=cast<SPImage>(&child)) {row["image-id"]=std::string(image->getId());row["pixel-to-parent"]=affineJSON(image->c2p*image->transform);}
                    else if(auto path=cast<SPPath>(&child))paths.emplace_back(path->getId());}
            } else {row["image-id"]=std::string(item->getId());if(auto image=cast<SPImage>(item))row["pixel-to-parent"]=affineJSON(image->c2p*image->transform);}row["cut-path-ids"]=std::move(paths);
        }
        r.status=Cli::Status::Changed;r.one_undo_step=true;r.undo_effect="one-step";r.publication="committed";
        r.selection_after=published.selectionAfter;r.revision_after=Cli::document_stamp(&doc).revision;
        if(alpha)r.modified={analysis->sourceId};else {r.created=published.selectionAfter;r.deleted={analysis->sourceId};}
        if(!consumeToken.empty()) {auto consumed=production.tokens.consume_after_commit(consumeToken,validation);
            if(consumed.error)r.warnings.push_back("Token consumption failed after native commit.");prune(production.tokens.snapshot());}
    }
    r.data={{"variant",request.dry_run ? "computed-dry-run":"success"},{"publication-map",std::move(map)},
        {"source-retained",alpha || only},{"source-pixel-sha256",analysis->sourcePixels},{"source-profile-sha256",analysis->sourceProfile},{"recipe",recipeJSON(analysis->recipe)}};
    if(alpha) {r.data["pixel-sha256"]=hashes[0].pixels;r.data["profile-sha256"]=hashes[0].profile;r.data["alpha-sha256"]=hashes[0].alpha;r.data["baked"]=true;
        r.data["root-affine"]=r.data.at("publication-map").as_array()[0].as_object().at("root-affine");}
    else {r.data["piece-count"]=out.count;r.data["no-contour-count"]=absent;}
    return r;
}
}

#include "bitmap-copy-outcome.h"
namespace Inkscape::VACardsCli {
Record execute_bitmap_copy(Request const &request, DispatchContext &dispatch, ProductionContext &production) {
    using namespace Bitmap;
    auto r=bitmap_record(request,dispatch,production);auto &doc=production.edits.document;
    auto canceled=[&] {return dispatch.cancelled && dispatch.cancelled();};
    if(canceled())return bitmap_refuse(std::move(r),"cancelled","Canceled before Bitmap Copy preparation.");
    std::vector<std::string> ids;for(auto const &v:request.params.at("ids").as_array())ids.emplace_back(v.as_string());
    for(auto const &id:ids)if(!cast<SPItem>(doc.getObjectById(id)))return bitmap_refuse(std::move(r),"unknown-id","Explicit copy root does not exist.");
    auto made=DocumentPublicationContext::headlessBitmapCopy(doc,ids,production.incarnation,production.target_generation);
    if(!made.ok())return failureRecord(std::move(r),made.outcome,made.outcome.failure);
    BitmapCopyRequestOptions options;
    if(auto size=request.params.if_contains("size"))options.sizing=ExactPixels{size->as_object().at("width").to_number<std::uint32_t>(),size->as_object().at("height").to_number<std::uint32_t>()};
    else options.sizing=Dpi{request.params.if_contains("dpi") ? request.params.at("dpi").to_number<double>() : 96};
    if(auto bbox=request.params.if_contains("bbox"))options.bounds=bbox->as_string()=="geometric" ? CopyBounds::Geometric:CopyBounds::Visual;
    if(auto bg=request.params.if_contains("background"))for(unsigned i=0;i<4;++i)options.background[i]=bg->as_array()[i].to_number<double>();
    options.keep_original=!request.params.if_contains("replace") || !request.params.at("replace").as_bool();
    options.placement=options.keep_original ? PlacementPolicy::NativeCopy:PlacementPolicy::ContiguousReplacement;
    Budget budget(std::numeric_limits<std::uint64_t>::max());
    CandidateHooks hooks;
    hooks.createSurface=[](int width,int height) -> cairo_surface_t * {
        if(cli_fault("bitmap.copy.before-render-surface",CliFaultKind::Renderer))return nullptr;
        return cairo_image_surface_create(CAIRO_FORMAT_ARGB32,width,height);
    };
    auto prepared=prepareBitmapCopy(*made.value,options,budget,hooks);
    auto refuseCopy=[&](BitmapCopyOutcome const &outcome,bool rollback=false) {
        auto reason=outcome.failure ? outcome.failure->reason:CliBitmapReason::InternalError;
        auto code=reason==CliBitmapReason::EncodingFailed ? "encoding-failed" :
            reason==CliBitmapReason::MemoryAdmissionFailed || reason==CliBitmapReason::EngineLimit ? "engine-limit" :
            reason==CliBitmapReason::MissingSource ? "missing-source" : reason==CliBitmapReason::DocumentBusy ? "transaction-unavailable" :
            reason==CliBitmapReason::AnalysisFailed ? "rasterization-failed" : failureCode({CliBitmapStage::Publish,reason});
        auto refused=failureRecord(r,outcome.outcome,outcome.failure,rollback || outcome.rolledBack);refused.reason=code;if(refused.error)refused.error->code=code;
        char const *branch=nullptr;
        switch(reason) {
            case CliBitmapReason::EncodingFailed:branch="publish: encode failed";break;
            case CliBitmapReason::MemoryAdmissionFailed:case CliBitmapReason::EngineLimit:branch="prepare: raster budget exceeded";break;
            case CliBitmapReason::MissingSource:branch="prepare: decode missing";break;
            case CliBitmapReason::DocumentBusy:branch="EditTransaction inactive";break;
            case CliBitmapReason::AnalysisFailed:branch="prepare: renderer failed";break;
            case CliBitmapReason::PublicationFailed:branch="publish failed; caller rollback";break;
            default:break;
        }
        if(branch)refused.error_details["reason"]=branch;
        return refused;
    };
    if(!prepared.ok())return refuseCopy(prepared);
    if(cli_fault("bitmap.copy.after-prepare-before-publication",CliFaultKind::Encoding)) {
        BitmapCopyOutcome failed;failed.outcome={Bitmap::Status::failed,"Injected copy encoding failure."};
        failed.failure=CliBitmapFailure{CliBitmapStage::Encode,CliBitmapReason::EncodingFailed};return refuseCopy(failed);
    }
    if(!prepared.candidate.pixels())return bitmap_refuse(std::move(r),"no-bounds","Copy has no measurable rectangle.");
    auto const metadata=prepared.candidate.metadata();
    Budget::Token receipt;
    auto admission=budget.acquire(Stage::prepared,std::uint64_t(metadata.width)*metadata.height*4+32*MiB,receipt);
    if(!admission.ok()) {BitmapCopyOutcome out;out.outcome=admission;out.failure=CliBitmapFailure{CliBitmapStage::Encode,CliBitmapReason::MemoryAdmissionFailed};return refuseCopy(out);}
    auto hashes=pixelHashes(*prepared.candidate.pixels());receipt.release();
    auto selection=made.value->getSelection();auto bounds=selection->documentBounds(options.bounds==CopyBounds::Visual ? SPItem::VISUAL_BBOX:SPItem::GEOMETRIC_BBOX);
    auto parent=cast<SPItem>(reinterpret_cast<SPObject *>(metadata.parent));Geom::Affine root=Geom::Translate(bounds->min());
    auto pixelToParent=Geom::Scale(bounds->width()/metadata.width,bounds->height()/metadata.height)*root*parent->i2doc_affine().inverse();
    r.data={{"variant",request.dry_run ? "computed-dry-run":"success"},{"image-id",nullptr},{"width",metadata.width},{"height",metadata.height},
        {"dpi-x",metadata.dpiX},{"dpi-y",metadata.dpiY},{"pixel-sha256",hashes.pixels},{"profile-sha256",hashes.profile},
        {"root-affine",affineJSON(root)},{"pixel-to-parent",affineJSON(pixelToParent)},{"source-retained",options.keep_original}};
    r.mode="collective-compositing";r.selected=ids.size();r.eligible=ids.size();
    if(canceled()) {r.data.clear();return bitmap_refuse(std::move(r),"cancelled","Canceled before Bitmap Copy publication.");}
    if(request.dry_run)return r;
    auto guard=DocumentUndo::beginAtomicInteraction(&doc);
    if(!guard) {
        BitmapCopyOutcome failed;
        failed.outcome={Bitmap::Status::failed,"Bitmap Copy atomic admission refused."};
        failed.failure=CliBitmapFailure{CliBitmapStage::Publish,CliBitmapReason::DocumentBusy};
        return refuseCopy(failed);
    }
    if(cli_fault("bitmap.copy.before-publication-validation",CliFaultKind::StaleCapture)) {
        guard->rollback();BitmapCopyOutcome failed;failed.outcome={Bitmap::Status::unavailable,"Injected stale copy capture."};
        failed.failure=CliBitmapFailure{CliBitmapStage::Publish,CliBitmapReason::StaleCapture};return refuseCopy(failed);
    }
    auto result=publishBitmapCopy(*selection,prepared.candidate,&*guard);
    if(!result.ok()) {guard->rollback();return refuseCopy(result);}
    if(cli_fault("bitmap.copy.after-publication-before-settlement",CliFaultKind::ServiceException)) {
        guard->rollback();throw std::runtime_error("Copy service exception after rollback");
    }
    auto image=cast<SPImage>(doc.getObjectById(result.imageId));
    bool settled=guard->commitAtomically(Util::Internal::ContextString("Create bitmap"),"selection-make-bitmap-copy",[&] {
        return !canceled() && image && image->pixbuf && image->pixbuf->width()==int(metadata.width) && image->pixbuf->height()==int(metadata.height);
    });
    if(!settled) {
        guard->rollback();BitmapCopyOutcome failed;failed.outcome={Bitmap::Status::failed,"Bitmap Copy settlement rolled back."};
        failed.failure=CliBitmapFailure{CliBitmapStage::Publish,CliBitmapReason::PublicationFailed};failed.rolledBack=true;
        return refuseCopy(failed);
    }
    r.data["image-id"]=result.imageId;r.data["root-affine"]=affineJSON(image->i2doc_affine());
    r.status=VACardsCli::Status::Changed;r.created={result.imageId};r.selection_after={result.imageId};if(!options.keep_original)r.deleted=ids;
    r.one_undo_step=true;r.undo_effect="one-step";r.publication="committed";r.revision_after=document_stamp(&doc).revision;
    return r;
}
}
