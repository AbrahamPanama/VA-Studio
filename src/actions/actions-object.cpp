// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Gio::Actions for working with objects without GUI.
 *
 * Copyright (C) 2020 Tavmjong Bah
 *
 * The contents of this file may be used under the GNU General Public License Version 2 or later.
 *
 */

#include <giomm.h>  // Not <gtkmm.h>! To eventually allow a headless version!
#include <glibmm/i18n.h>

#include <optional>

#include "actions-object.h"
#include "actions-helper.h"
#include "actions/actions-vacards-cli.h"
#include "actions/vacards-cli-result.h"
#include "desktop.h"
#include "document-undo.h"
#include "inkscape-application.h"
#include "message-stack.h"
#include "object/sp-star.h"
#include "object/sp-image.h"
#include "preferences.h"
#include "selection.h"

#include "live_effects/effect.h"
#include "live_effects/lpe-powerclip.h"
#include "live_effects/lpe-powermask.h"
#include "object/sp-lpe-item.h"
#include "trace/potrace/inkscape-potrace.h"
#include "trace/trace.h"
#include "ui/icon-names.h"
#include "ui/tools/destructive-bitmap-clip-chemistry.h"
#include "ui/tools/destructive-bitmap-coverage.h"
#include "util/cast.h"

namespace {

double stod_finite(std::string const &str)
{
    double const result = std::stod(str);
    if (!std::isfinite(result)) {
        throw std::out_of_range{"stod: Inf or NaN"};
    }
    return result;
}

void object_trace(Glib::VariantBase const &value, InkscapeApplication *app)
{
    auto selection = app->get_active_selection();
    if (!selection || selection->isEmpty()) {
        show_output("action:object_trace: selection empty!", true);
        return;
    }

    auto const str = Glib::VariantBase::cast_dynamic<Glib::Variant<Glib::ustring>>(value);
    std::vector<Glib::ustring> const settings = Glib::Regex::split_simple(",", str.get());
    if (settings.size() != 7) {
        show_output("action:object_trace: expected argument format: {scans},{smooth[false|true]},{stack[false|true]},{remove_background[false|true],{speckles},{smooth_corners},{optimize}}", true);
        return;
    }

    int scans;
    bool smooth;
    bool stack;
    bool remove_background;
    int speckles;
    double smooth_corners;
    double optimize;
    try {
        scans = std::stoi(settings[0]);
        smooth = settings[1] == "true";
        stack = settings[2] == "true";
        remove_background = settings[3] == "true";
        speckles = std::stoi(settings[4]);
        smooth_corners = stod_finite(settings[5]);
        optimize = stod_finite(settings[6]);
    } catch (std::logic_error const &e) {
        show_output(std::string{"action:object_trace: parsing arguments failed: "} + e.what(), true);
        return;
    }

    auto tracer = std::make_unique<Inkscape::Trace::Potrace::PotraceTracingEngine>(Inkscape::Trace::Potrace::TraceType::QUANT_COLOR, false, 64,
                                                                                   0.45, 0.0, 0.65, scans, stack, smooth, remove_background);
    tracer->setOptiCurve(true);
    tracer->setTurdSize(speckles);
    tracer->setAlphaMax(smooth_corners);
    tracer->setOptTolerance(optimize);

    auto mainloop = Glib::MainLoop::create();

    auto future = Inkscape::Trace::trace(
        std::move(tracer),
        false,
        [] (double progress) {
            std::cout << "Tracing... " << std::round(100 * progress) << '%' << std::endl;
        },
        [&] {
            show_output("Tracing done.");
            mainloop->quit();
        }
    );

    if (!future) {
        show_output("Tracing failed.", true);
        return;
    }

    mainloop->run();
}


void
object_get_attribute(const Glib::VariantBase& value, InkscapeApplication *app)
{
    SPDocument* document = nullptr;
    Inkscape::Selection* selection = nullptr;
    if (!get_document_and_selection(app, &document, &selection)) {
        return;
    }
    auto const attribute = Glib::VariantBase::cast_dynamic<Glib::Variant<Glib::ustring>>(value).get();

    for (auto obj : selection->objects()) {
        Inkscape::XML::Node *repr = obj->getRepr();
        auto value = repr->attribute(attribute.c_str());
        show_output(value ? Glib::strescape(value) : "", false);
    }
}


void
object_get_property(const Glib::VariantBase& value, InkscapeApplication *app)
{
    SPDocument* document = nullptr;
    Inkscape::Selection* selection = nullptr;
    if (!get_document_and_selection(app, &document, &selection)) {
        return;
    }
    auto const attribute = Glib::VariantBase::cast_dynamic<Glib::Variant<Glib::ustring>>(value).get();

    for (auto obj : selection->objects()) {
        Inkscape::XML::Node *repr = obj->getRepr();
        SPCSSAttr *css = sp_repr_css_attr(repr, "style");
        auto value = sp_repr_css_property(css, attribute.c_str(), "");
        show_output(value ? Glib::strescape(value) : "", false);
        sp_repr_css_attr_unref(css);
    }
}

void
object_remove_attribute(Glib::VariantBase const &value, InkscapeApplication *app)
{
    Inkscape::Selection *selection = app->get_active_selection();
    if (!selection || selection->isEmpty()) {
        show_output("action:object_remove_attribute: selection empty!");
        return;
    }
    auto const attribute = Glib::VariantBase::cast_dynamic<Glib::Variant<Glib::ustring>>(value).get();

    for (auto obj : selection->objects()) {
        Inkscape::XML::Node *repr = obj->getRepr();
        repr->removeAttribute(attribute);
    }
    Inkscape::DocumentUndo::done(app->get_active_document(), RC_("Undo", "Action remove attribute from objects"), "");
}

void
object_remove_property(Glib::VariantBase const &value, InkscapeApplication *app)
{
    Inkscape::Selection *selection = app->get_active_selection();
    if (!selection || selection->isEmpty()) {
        show_output("action:object_remove_property: selection empty!");
        return;
    }
    auto const property = Glib::VariantBase::cast_dynamic<Glib::Variant<Glib::ustring>>(value).get();

    for (auto obj : selection->objects()) {
        Inkscape::XML::Node *repr = obj->getRepr();
        SPCSSAttr *css = sp_repr_css_attr(repr, "style");
        sp_repr_css_set_property(css, property.c_str(), nullptr);
        sp_repr_css_set(repr, css, "style");
        sp_repr_css_attr_unref(css);
    }
    Inkscape::DocumentUndo::done(app->get_active_document(), RC_("Undo", "Action remove property from objects"), "");
}

// No sanity checking is done... should probably add.
void
object_set_attribute(const Glib::VariantBase& value, InkscapeApplication *app)
{
    auto const argument = Glib::VariantBase::cast_dynamic<Glib::Variant<Glib::ustring>>(value).get();
    auto const comma_position = argument.find_first_of(',');
    if (comma_position == 0 || comma_position == Glib::ustring::npos) {
        show_output("action:object_set_attribute: requires 'attribute name, attribute value'");
        return;
    }
    auto const attribute = argument.substr(0, comma_position);
    auto const new_value = argument.substr(comma_position + 1);

    auto selection = app->get_active_selection();
    if (!selection || selection->isEmpty()) {
        show_output("action:object_set_attribute: selection empty!");
        return;
    }

    // Should this be a selection member function?
    for (auto obj : selection->objects()) {
        Inkscape::XML::Node *repr = obj->getRepr();
        repr->setAttribute(attribute, new_value);
    }

    // TODO: Needed to update repr (is this the best way?).
    Inkscape::DocumentUndo::done(app->get_active_document(), Inkscape::Util::Internal::ContextString("ActionObjectSetAttribute"), "");
}


// No sanity checking is done... should probably add.
void
object_set_property(const Glib::VariantBase& value, InkscapeApplication *app)
{
    Glib::Variant<Glib::ustring> s = Glib::VariantBase::cast_dynamic<Glib::Variant<Glib::ustring> >(value);

    std::vector<Glib::ustring> tokens = Glib::Regex::split_simple(",", s.get());
    if (tokens.size() != 2) {
        show_output("action:object_set_property: requires 'property name, property value'");
        return;
    }

    auto selection = app->get_active_selection();
    if (!selection || selection->isEmpty()) {
        show_output("action:object_set_property: selection empty!");
        return;
    }

    // Should this be a selection member function?
    for (auto obj : selection->objects()) {
        Inkscape::XML::Node *repr = obj->getRepr();
        SPCSSAttr *css = sp_repr_css_attr(repr, "style");
        sp_repr_css_set_property(css, tokens[0].c_str(), tokens[1].c_str());
        sp_repr_css_set(repr, css, "style");
        sp_repr_css_attr_unref(css);
    }

    // Needed to update repr (is this the best way?).
    Inkscape::DocumentUndo::done(app->get_active_document(), Inkscape::Util::Internal::ContextString("ActionObjectSetProperty"), "");
}


void
object_unlink_clones(InkscapeApplication *app)
{
    auto selection = app->get_active_selection();
    if (!selection) {
        return;
    }

    selection->unlink();
}

bool
should_remove_original()
{
    return Inkscape::Preferences::get()->getBool("/options/maskobject/remove", true);
}

void
object_clip_set(InkscapeApplication *app)
{
    Inkscape::Selection *selection = app->get_active_selection();
    if (!selection) {
        return;
    }

    // Object Clip Set
    selection->setMask(true, false, should_remove_original());
    Inkscape::DocumentUndo::done(selection->document(), RC_("Undo", "Set clipping path"), "");
}

void
object_clip_set_inverse(InkscapeApplication *app)
{
    Inkscape::Selection *selection = app->get_active_selection();
    if (!selection) {
        return;
    }

    // Object Clip Set Inverse
    selection->setMask(true, false, should_remove_original());
    Inkscape::LivePathEffect::sp_inverse_powerclip(app->get_active_selection());
    Inkscape::DocumentUndo::done(app->get_active_document(), RC_("Undo", "Set Inverse Clip(LPE)"), "");
}

void
object_clip_release(InkscapeApplication *app)
{
    Inkscape::Selection *selection = app->get_active_selection();
    if (!selection) {
        return;
    }

    // Object Clip Release
    Inkscape::LivePathEffect::sp_remove_powerclip(app->get_active_selection());
    selection->unsetMask(true, true, should_remove_original());
    Inkscape::DocumentUndo::done(app->get_active_document(), RC_("Undo", "Release clipping path"), "");
}

void
object_clip_set_group(InkscapeApplication *app)
{
    Inkscape::Selection *selection = app->get_active_selection();
    if (!selection) {
        return;
    }

    selection->setClipGroup();
    // Undo added in setClipGroup().
}

} // namespace

namespace Inkscape::UI::Tools::DestructiveBitmapClip {

namespace {

// Stable, locale-independent reason identifiers. They are the testable contract
// between a B04 transaction status and the visible feedback produced here.
constexpr char const *ACTION_REASON_SUCCESS = "success";
constexpr char const *ACTION_REASON_SUCCESS_FULLY_TRANSPARENT = "success-fully-transparent";
constexpr char const *ACTION_REASON_SUCCESS_STRAIGHTENED = "success-straightened";
constexpr char const *ACTION_REASON_SUCCESS_STRAIGHTENED_FULLY_TRANSPARENT = "success-straightened-fully-transparent";
constexpr char const *ACTION_REASON_STRAIGHTENING_EMPTY_REGION = "straightening-empty-region";
constexpr char const *ACTION_REASON_STRAIGHTENING_TOO_LARGE = "straightening-too-large";
constexpr char const *ACTION_REASON_NO_CHANGE = "no-change";
constexpr char const *ACTION_REASON_INVALID_SELECTION = "invalid-selection";
constexpr char const *ACTION_REASON_MISSING_SOURCE = "missing-source";
constexpr char const *ACTION_REASON_INVALID_CUTTER = "invalid-cutter";
constexpr char const *ACTION_REASON_UNSUPPORTED_EFFECTS = "unsupported-effects-or-references";
constexpr char const *ACTION_REASON_UNSUPPORTED_TRIM = "unsupported-trim";
constexpr char const *ACTION_REASON_PROTECTED_OBJECT = "protected-object";
constexpr char const *ACTION_REASON_RASTERIZATION_FAILED = "rasterization-failed";
constexpr char const *ACTION_REASON_ENCODING_FAILED = "encoding-failed";
constexpr char const *ACTION_REASON_CANCELLED = "cancelled";

} // namespace

/**
 * Map the B04 transaction status (refined by the read-only resolver when it
 * rejected the pair) to a stable user-visible reason.
 *
 * The transaction status alone collapses several causes; in particular
 * `InvalidGeometry` covers both a singular image mapping and a cutter with no
 * supported closed region. A failed transaction never mutates the document, so
 * the caller can re-run `resolve_targets` and pass its status here to keep the
 * missing-source and invalid-cutter reasons distinct.
 */
char const *
action_outcome_reason(CommitStatus status, TargetStatus targets) noexcept
{
    // Success/no-op statuses are unambiguous and never need target refinement.
    switch (status) {
        case CommitStatus::Committed:
            return ACTION_REASON_SUCCESS;
        case CommitStatus::CommittedAllTransparent:
            return ACTION_REASON_SUCCESS_FULLY_TRANSPARENT;
        case CommitStatus::CommittedStraightened:
            return ACTION_REASON_SUCCESS_STRAIGHTENED;
        case CommitStatus::CommittedStraightenedAllTransparent:
            return ACTION_REASON_SUCCESS_STRAIGHTENED_FULLY_TRANSPARENT;
        case CommitStatus::NoChange:
            return ACTION_REASON_NO_CHANGE;
        default:
            break;
    }

    // A rejected pair supplies the most precise reason available.
    switch (targets) {
        case TargetStatus::MissingImage:
        case TargetStatus::InvalidGeometry:
            return ACTION_REASON_MISSING_SOURCE;
        case TargetStatus::UnsupportedCutter:
            return ACTION_REASON_INVALID_CUTTER;
        case TargetStatus::UnsupportedBranch:
            return ACTION_REASON_UNSUPPORTED_EFFECTS;
        case TargetStatus::Resolved:
            break;
        case TargetStatus::NotPair:
        case TargetStatus::NestedOrDuplicateRoots:
        case TargetStatus::AmbiguousImageBranch:
        case TargetStatus::MultipleImages:
            return ACTION_REASON_INVALID_SELECTION;
        case TargetStatus::ProtectedObject:
            // Structurally a valid pair, but a hidden/locked member must be
            // revealed/unlocked first; this is not a "select exactly one" error.
            return ACTION_REASON_PROTECTED_OBJECT;
    }

    // The pair resolved, so the failure happened during coverage/encode/commit.
    switch (status) {
        case CommitStatus::Cancelled:
            return ACTION_REASON_CANCELLED;
        case CommitStatus::EncodingFailed:
            return ACTION_REASON_ENCODING_FAILED;
        case CommitStatus::UnsupportedTrim:
            return ACTION_REASON_UNSUPPORTED_TRIM;
        case CommitStatus::StraighteningEmptyRegion:
            return ACTION_REASON_STRAIGHTENING_EMPTY_REGION;
        case CommitStatus::StraighteningTooLarge:
            return ACTION_REASON_STRAIGHTENING_TOO_LARGE;
        case CommitStatus::InvalidGeometry:
            // Coverage/apply rejected the image source pixels or mapping.
            return ACTION_REASON_MISSING_SOURCE;
        case CommitStatus::InvalidSelection:
            // The read-only resolver still reports the pair as resolved, so the
            // commit-time rejection is an unsupported effect/reference (wrapper
            // branch, href/clone, shared compositing context), not a bad pair.
            return ACTION_REASON_UNSUPPORTED_EFFECTS;
        case CommitStatus::RasterizationFailed:
            return ACTION_REASON_RASTERIZATION_FAILED;
        case CommitStatus::Committed:
        case CommitStatus::CommittedAllTransparent:
        case CommitStatus::CommittedStraightened:
        case CommitStatus::CommittedStraightenedAllTransparent:
        case CommitStatus::NoChange:
            break;
    }
    return ACTION_REASON_RASTERIZATION_FAILED;
}

/** Severity used for the visible desktop status message of a reason. */
Inkscape::MessageType
action_outcome_message_type(char const *reason) noexcept
{
    if (!reason) {
        return Inkscape::ERROR_MESSAGE;
    }
    if (g_strcmp0(reason, ACTION_REASON_SUCCESS) == 0 ||
        g_strcmp0(reason, ACTION_REASON_NO_CHANGE) == 0 ||
        g_strcmp0(reason, ACTION_REASON_SUCCESS_STRAIGHTENED) == 0) {
        return Inkscape::NORMAL_MESSAGE;
    }
    if (g_strcmp0(reason, ACTION_REASON_SUCCESS_STRAIGHTENED_FULLY_TRANSPARENT) == 0 ||
        g_strcmp0(reason, ACTION_REASON_STRAIGHTENING_EMPTY_REGION) == 0 ||
        g_strcmp0(reason, ACTION_REASON_STRAIGHTENING_TOO_LARGE) == 0 ||
        g_strcmp0(reason, ACTION_REASON_SUCCESS_FULLY_TRANSPARENT) == 0 ||
        g_strcmp0(reason, ACTION_REASON_UNSUPPORTED_EFFECTS) == 0 ||
        g_strcmp0(reason, ACTION_REASON_UNSUPPORTED_TRIM) == 0 ||
        g_strcmp0(reason, ACTION_REASON_CANCELLED) == 0) {
        return Inkscape::WARNING_MESSAGE;
    }
    return Inkscape::ERROR_MESSAGE;
}

/** Translated, user-facing message for a reason. Never claims a bare "no selection". */
char const *
action_outcome_message(char const *reason, bool inverse) noexcept
{
    if (!reason) {
        reason = ACTION_REASON_RASTERIZATION_FAILED;
    }
    if (g_strcmp0(reason, ACTION_REASON_SUCCESS_STRAIGHTENED_FULLY_TRANSPARENT) == 0) {
        return inverse ? _("Bitmap straightened; the inverse clip made it fully transparent.")
                       : _("Bitmap straightened; the clip made it fully transparent.");
    }
    if (g_strcmp0(reason, ACTION_REASON_SUCCESS_STRAIGHTENED) == 0) {
        return inverse ? _("Bitmap straightened and destructively inverse-clipped.")
                       : _("Bitmap straightened and destructively clipped.");
    }
    if (g_strcmp0(reason, ACTION_REASON_STRAIGHTENING_EMPTY_REGION) == 0) {
        return _("The cutter does not overlap the visible part of the bitmap; no changes were made.");
    }
    if (g_strcmp0(reason, ACTION_REASON_STRAIGHTENING_TOO_LARGE) == 0) {
        return _("The straightened bitmap would exceed 16384 pixels per side or 100 megapixels; no changes were made.");
    }
    if (g_strcmp0(reason, ACTION_REASON_SUCCESS) == 0) {
        return inverse ? _("Bitmap destructively inverse-clipped.")
                       : _("Bitmap destructively clipped.");
    }
    if (g_strcmp0(reason, ACTION_REASON_SUCCESS_FULLY_TRANSPARENT) == 0) {
        return inverse ? _("Destructive inverse clip made the bitmap fully transparent.")
                       : _("Destructive clip made the bitmap fully transparent.");
    }
    if (g_strcmp0(reason, ACTION_REASON_NO_CHANGE) == 0) {
        return _("No destructive clip was needed; the selection is unchanged.");
    }
    if (g_strcmp0(reason, ACTION_REASON_MISSING_SOURCE) == 0) {
        return _("The selected bitmap has no usable source pixels or mapping; no changes were made.");
    }
    if (g_strcmp0(reason, ACTION_REASON_INVALID_CUTTER) == 0) {
        return _("The selected cutter does not provide a supported closed vector region; no changes were made.");
    }
    if (g_strcmp0(reason, ACTION_REASON_UNSUPPORTED_EFFECTS) == 0) {
        return _("The bitmap cannot be destructively clipped with its current effects or references; no changes were made.");
    }
    if (g_strcmp0(reason, ACTION_REASON_UNSUPPORTED_TRIM) == 0) {
        return _("The bitmap cannot be trimmed with its current viewport, effects, or references; no changes were made.");
    }
    if (g_strcmp0(reason, ACTION_REASON_ENCODING_FAILED) == 0) {
        return _("The bitmap could not be clipped because its replacement PNG could not be encoded; no changes were made.");
    }
    if (g_strcmp0(reason, ACTION_REASON_CANCELLED) == 0) {
        return _("The destructive clip was cancelled; no changes were made.");
    }
    if (g_strcmp0(reason, ACTION_REASON_RASTERIZATION_FAILED) == 0) {
        return _("The bitmap could not be clipped because raster coverage failed; no changes were made.");
    }
    if (g_strcmp0(reason, ACTION_REASON_PROTECTED_OBJECT) == 0) {
        return _("A hidden or locked object must be made visible and unlocked before it can be destructively clipped; no changes were made.");
    }
    // invalid-selection and any unknown reason.
    return _("Select exactly one bitmap and one closed vector cutter; no changes were made.");
}

} // namespace Inkscape::UI::Tools::DestructiveBitmapClip

namespace {

// Report an outcome through both the existing CLI diagnostic and, when a GUI
// desktop is attached, the existing visible status-message stack. No modal
// dialog is shown per action.
void
present_destructive_clip_feedback(InkscapeApplication *app, char const *reason, bool inverse)
{
    using namespace Inkscape::UI::Tools::DestructiveBitmapClip;

    auto const *message = action_outcome_message(reason, inverse);
    show_output(message);

    if (!app) {
        return;
    }
    if (auto *desktop = app->get_active_desktop()) {
        if (auto *stack = desktop->messageStack()) {
            stack->flash(action_outcome_message_type(reason), message);
        }
    }
}

/**
 * Map an outcome reason to the agent-CLI record status. Any unrecognized reason
 * (including a null pointer) is a rejection: the action did not change anything.
 */
Inkscape::VACardsCli::Status clip_status_for_reason(char const *reason)
{
    namespace DBC = Inkscape::UI::Tools::DestructiveBitmapClip;

    if (g_strcmp0(reason, DBC::ACTION_REASON_SUCCESS) == 0 ||
        g_strcmp0(reason, DBC::ACTION_REASON_SUCCESS_FULLY_TRANSPARENT) == 0 ||
        g_strcmp0(reason, DBC::ACTION_REASON_SUCCESS_STRAIGHTENED) == 0 ||
        g_strcmp0(reason, DBC::ACTION_REASON_SUCCESS_STRAIGHTENED_FULLY_TRANSPARENT) == 0) {
        return Inkscape::VACardsCli::Status::Changed;
    }
    if (g_strcmp0(reason, DBC::ACTION_REASON_NO_CHANGE) == 0) {
        return Inkscape::VACardsCli::Status::Unchanged;
    }
    if (g_strcmp0(reason, DBC::ACTION_REASON_CANCELLED) == 0) {
        return Inkscape::VACardsCli::Status::Cancelled;
    }
    if (g_strcmp0(reason, DBC::ACTION_REASON_RASTERIZATION_FAILED) == 0 ||
        g_strcmp0(reason, DBC::ACTION_REASON_ENCODING_FAILED) == 0) {
        return Inkscape::VACardsCli::Status::Failed;
    }
    return Inkscape::VACardsCli::Status::Rejected;
}

/** Fill and emit one agent-CLI record for an existing destructive-clip action. */
void emit_clip_record(InkscapeApplication *app, char const *action, bool inverse, char const *reason,
                      std::size_t selected, int eligible, std::string const &image_id,
                      std::string const &cutter_id, bool dry_run, std::string const &params_text)
{
    using namespace Inkscape::UI::Tools::DestructiveBitmapClip;

    Inkscape::VACardsCli::Record r;
    r.action = action;
    r.mode = "paired";
    r.params_text = params_text;
    r.dry_run = dry_run;
    if (auto *document = app->get_active_document()) {
        char const *filename = document->getDocumentFilename();
        r.document_path = filename ? std::string(filename) : std::string();
    }
    r.selected = static_cast<int>(selected);
    r.eligible = eligible;
    r.status = clip_status_for_reason(reason);
    r.reason = reason ? reason : "rasterization-failed";
    r.message = action_outcome_message(reason, inverse);
    if (r.status == Inkscape::VACardsCli::Status::Changed) {
        r.one_undo_step = true;
        if (!image_id.empty()) {
            r.modified = {image_id};
        }
    }
    if (!image_id.empty()) {
        r.data["image"] = image_id;
    }
    if (!cutter_id.empty()) {
        r.data["cutter"] = cutter_id;
    }
    if (auto *selection = app->get_active_selection()) {
        for (auto *item : selection->items()) {
            if (char const *id = item->getId()) {
                r.selection_after.emplace_back(id);
            }
        }
    }
    Inkscape::VACardsCli::emit(r);
}

void
object_destructive_clip(InkscapeApplication *app,
                        Inkscape::UI::Tools::DestructiveBitmapClip::Mode mode)
{
    auto *selection = app->get_active_selection();
    auto *document = app->get_active_document();
    if (!selection || !document || selection->document() != document) {
        if (Inkscape::VACardsCli::records_enabled(app)) {
            Inkscape::VACardsCli::Record r;
            r.action = mode == Inkscape::UI::Tools::DestructiveBitmapClip::Mode::KeepOutside
                           ? "object-destructive-inverse-clip"
                           : "object-destructive-clip";
            r.mode = "paired";
            r.status = Inkscape::VACardsCli::Status::Rejected;
            r.reason = "no-document";
            r.message = "This action needs an open document.";
            if (document) {
                char const *filename = document->getDocumentFilename();
                r.document_path = filename ? std::string(filename) : std::string();
            }
            Inkscape::VACardsCli::emit(r);
        }
        update_destructive_bitmap_clip_actions(app);
        return;
    }

    using namespace Inkscape::UI::Tools::DestructiveBitmapClip;
    auto const inverse = mode == Mode::KeepOutside;

    // The record-only target resolution must run before the commit (which
    // replaces the bitmap), but only when a record will actually be emitted, so
    // the interactive GUI path performs no extra work.
    bool const report = Inkscape::VACardsCli::records_enabled(app);
    std::size_t selected_count = 0;
    std::string image_id;
    std::string cutter_id;
    std::optional<ResolvedTargets> before;
    if (report) {
        selected_count = selection->items_vector().size();
        before = resolve_targets(*selection);
        image_id = before->image && before->image->getId() ? before->image->getId() : "";
        cutter_id = before->cutter && before->cutter->getId() ? before->cutter->getId() : "";
    }
    auto const status = commit_selection(*selection, mode);

    // A failed transaction is an atomic no-op, so the read-only resolver still
    // describes the original pair and can keep a missing bitmap source distinct
    // from an invalid cutter. Committed statuses changed the selection to the
    // bitmap alone, so they must not be re-resolved.
    auto const targets = status == CommitStatus::Committed ||
                                 status == CommitStatus::CommittedAllTransparent ||
                                 status == CommitStatus::CommittedStraightened ||
                                 status == CommitStatus::CommittedStraightenedAllTransparent ||
                                 status == CommitStatus::NoChange
                             ? TargetStatus::Resolved
                             : resolve_targets(*selection).status;
    auto const *reason = action_outcome_reason(status, targets);
    present_destructive_clip_feedback(app, reason, inverse);
    if (report) {
        int const eligible = before && before->status == TargetStatus::Resolved ? 2 : 0;
        emit_clip_record(app, inverse ? "object-destructive-inverse-clip" : "object-destructive-clip",
                         inverse, reason, selected_count, eligible, image_id, cutter_id, false, "");
    }
}

constexpr Inkscape::VACardsCli::ParamSpec destructive_clip_params[] = {
    {.key = "inverse", .type = Inkscape::VACardsCli::ParamType::Boolean, .default_value = "false",
     .help = "Keep the pixels outside the cutter instead of inside."},
    {.key = "dry-run", .type = Inkscape::VACardsCli::ParamType::Boolean, .default_value = "false",
     .help = "Resolve and report the bitmap and cutter without changing the document."}};

constexpr Inkscape::VACardsCli::ActionSpec destructive_clip_spec{
    .name = "vacards-destructive-clip", .mode = "paired",
    .summary = "Destructively clip one bitmap with one closed vector cutter.",
    .params = destructive_clip_params};

void
vacards_destructive_clip(Glib::VariantBase const &value, InkscapeApplication *app)
{
    Inkscape::VACardsCli::run_action(destructive_clip_spec, value, app, [](Inkscape::VACardsCli::ActionContext &c) {
        using namespace Inkscape::UI::Tools::DestructiveBitmapClip;

        bool const inverse = c.params.at("inverse").boolean;
        auto &r = c.record;
        if (!c.selection) {
            r.status = Inkscape::VACardsCli::Status::Rejected;
            r.reason = "invalid-selection";
            r.message = action_outcome_message("invalid-selection", inverse);
            return;
        }
        r.selected = static_cast<int>(c.selection->items_vector().size());
        auto const targets = resolve_targets(*c.selection);
        if (targets.status != TargetStatus::Resolved) {
            char const *reason = action_outcome_reason(CommitStatus::InvalidSelection, targets.status);
            r.status = Inkscape::VACardsCli::Status::Rejected;
            r.reason = reason;
            r.message = action_outcome_message(reason, inverse);
            return;
        }
        std::string const image_id = targets.image && targets.image->getId() ? targets.image->getId() : "";
        std::string const cutter_id = targets.cutter && targets.cutter->getId() ? targets.cutter->getId() : "";
        r.eligible = 2;
        if (!image_id.empty()) {
            r.data["image"] = image_id;
        }
        if (!cutter_id.empty()) {
            r.data["cutter"] = cutter_id;
        }
        if (targets.branch && targets.branch->getId()) {
            r.data["branch"] = targets.branch->getId();
        }
        // A resolved pair is not enough: the commit also refuses a wrapped image
        // and referenced/cloned objects. A dry run must predict that refusal
        // rather than report a success the real run cannot deliver.
        if (!commit_preconditions_hold(targets.image, targets.branch, targets.cutter)) {
            r.status = Inkscape::VACardsCli::Status::Rejected;
            r.reason = "unsupported-effects-or-references";
            r.message = action_outcome_message("unsupported-effects-or-references", inverse);
            return;
        }
        // The commit's coverage step also refuses cheaply before rendering:
        // a missing/unsupported source, an invalid chain, a shared compositing
        // ancestor or an unsupported effect. Predict those from the one shared
        // implementation so the dry run cannot report a success the real run
        // will not deliver. commit_status_for() is the same mapping
        // commit_selection() uses.
        using CoverageStatus = Inkscape::UI::Tools::DestructiveBitmapCoverage::Status;
        auto const coverage =
            Inkscape::UI::Tools::DestructiveBitmapCoverage::preflight(*targets.branch, *targets.image);
        if (coverage != CoverageStatus::Completed) {
            char const *reason = action_outcome_reason(commit_status_for(coverage), TargetStatus::Resolved);
            r.status = Inkscape::VACardsCli::Status::Rejected;
            r.reason = reason;
            r.message = action_outcome_message(reason, inverse);
            return;
        }
        if (r.dry_run) {
            r.status = Inkscape::VACardsCli::Status::Ok;
            r.reason = "success";
            r.message = "Dry run: the bitmap and cutter pass every check that can be made without clipping; the clip "
                        "can still fail during rasterization, encoding or trimming.";
            return;
        }
        auto const status = commit_selection(*c.selection, inverse ? Mode::KeepOutside : Mode::KeepInside);
        auto const refined = (status == CommitStatus::Committed || status == CommitStatus::CommittedAllTransparent ||
                              status == CommitStatus::CommittedStraightened ||
                              status == CommitStatus::CommittedStraightenedAllTransparent ||
                              status == CommitStatus::NoChange)
                                 ? TargetStatus::Resolved
                                 : resolve_targets(*c.selection).status;
        char const *reason = action_outcome_reason(status, refined);
        r.status = clip_status_for_reason(reason);
        r.reason = reason;
        r.message = action_outcome_message(reason, inverse);
        if (r.status == Inkscape::VACardsCli::Status::Changed) {
            r.one_undo_step = true;
            if (!image_id.empty()) {
                r.modified = {image_id};
            }
        }
    });
}

void
object_mask_set(InkscapeApplication *app)
{
    Inkscape::Selection *selection = app->get_active_selection();
    if (!selection) {
        return;
    }

    // Object Mask Set
    selection->setMask(false, false, should_remove_original());
    Inkscape::DocumentUndo::done(selection->document(), RC_("Undo", "Set mask"), "");
}

void
object_mask_set_inverse(InkscapeApplication *app)
{
    Inkscape::Selection *selection = app->get_active_selection();
    if (!selection) {
        return;
    }

    // Object Mask Set Inverse
    selection->setMask(false, false, should_remove_original());
    Inkscape::LivePathEffect::sp_inverse_powermask(app->get_active_selection());
    Inkscape::DocumentUndo::done(app->get_active_document(), RC_("Undo", "Set Inverse Mask (LPE)"), "");
}

void
object_mask_release(InkscapeApplication *app)
{
    Inkscape::Selection *selection = app->get_active_selection();
    if (!selection) {
        return;
    }

    // Object Mask Release
    Inkscape::LivePathEffect::sp_remove_powermask(app->get_active_selection());
    selection->unsetMask(false, true, should_remove_original());
    Inkscape::DocumentUndo::done(app->get_active_document(), RC_("Undo", "Release mask"), "");
}

void
object_rotate_90_cw(InkscapeApplication *app)
{
    Inkscape::Selection *selection = app->get_active_selection();
    if (!selection) {
        return;
    }

    // Object Rotate 90
    auto doc = selection->document();
    selection->rotateAnchored((!doc || doc->yaxisdown()) ? 90 : -90);
}

void
object_rotate_90_ccw(InkscapeApplication *app)
{
    Inkscape::Selection *selection = app->get_active_selection();
    if (!selection) {
        return;
    }

    // Object Rotate 90 CCW
    auto doc = selection->document();
    selection->rotateAnchored((!doc || doc->yaxisdown()) ? -90 : 90);
}

void
object_flip_horizontal(InkscapeApplication *app)
{
    Inkscape::Selection *selection = app->get_active_selection();
    if (!selection) {
        return;
    }

    Geom::OptRect bbox = selection->visualBounds();
    if (!bbox) {
        return;
    }

    // Get center
    Geom::Point center;
    if (selection->center()) {
        center = *selection->center();
    } else {
        center = bbox->midpoint();
    }

    // Object Flip Horizontal
    selection->scaleRelative(center, Geom::Scale(-1.0, 1.0));
    Inkscape::DocumentUndo::done(app->get_active_document(), RC_("Undo", "Flip horizontally"), INKSCAPE_ICON("object-flip-horizontal"));
}

void
object_flip_vertical(InkscapeApplication *app)
{
    Inkscape::Selection *selection = app->get_active_selection();
    if (!selection) {
        return;
    }

    Geom::OptRect bbox = selection->visualBounds();
    if (!bbox) {
        return;
    }

    // Get center
    Geom::Point center;
    if (selection->center()) {
        center = *selection->center();
    } else {
        center = bbox->midpoint();
    }

    // Object Flip Vertical
    selection->scaleRelative(center, Geom::Scale(1.0, -1.0));
    Inkscape::DocumentUndo::done(app->get_active_document(), RC_("Undo", "Flip vertically"), INKSCAPE_ICON("object-flip-vertical"));
}

void
object_star_turn_upright(InkscapeApplication *app)
{
    auto selection = app->get_active_selection();
    if (!selection || selection->isEmpty()) {
        show_output("action:object_star_turn_upright: selection empty!");
        return;
    }

    bool has_stars = false;
    for (auto obj : selection->objects()) {
        if (auto star = cast<SPStar>(obj)) {
            has_stars = true;
            star->turn_upright();
        }
    }

    if (!has_stars) {
        show_output("action:objects_star_turn_upright: no SPStar in selection!");
        return;
    }

    Inkscape::DocumentUndo::done(app->get_active_document(), RC_("Undo", "Turn stars upright"), INKSCAPE_ICON("object-level"));
}

void
object_to_path(InkscapeApplication *app)
{
    auto selection = app->get_active_selection();
    if (!selection) {
        return;
    }

    selection->toCurves(false, Inkscape::Preferences::get()->getBool("/options/clonestocurvesjustunlink/value", true));
}

void
object_add_corners_lpe(InkscapeApplication *app) {
    auto selection = app->get_active_selection();
    if (!selection) {
        return;
    }

    // We should not have to do this!
    auto document  = app->get_active_document();
    if (!document) {
        return;
    }

    auto items = selection->items_vector();
    selection->clear();
    for (auto i : items) {
        if (auto lpeitem = cast<SPLPEItem>(i)) {
            if (auto lpe = lpeitem->getFirstPathEffectOfType(Inkscape::LivePathEffect::FILLET_CHAMFER)) {
                lpeitem->removePathEffect(lpe, false);
                Inkscape::DocumentUndo::done(document, RC_("Undo", "Remove Live Path Effect"), INKSCAPE_ICON("dialog-path-effects"));
            } else {
                Inkscape::LivePathEffect::Effect::createAndApply("fillet_chamfer", document, lpeitem);
                Inkscape::DocumentUndo::done(document, RC_("Undo", "Create and apply path effect"), INKSCAPE_ICON("dialog-path-effects"));
            }
            if (auto lpe = lpeitem->getCurrentLPE()) {
                lpe->refresh_widgets = true;
            }
        }
        selection->add(i);
    }
}

void
object_stroke_to_path(InkscapeApplication *app)
{
    auto selection = app->get_active_selection();
    if (!selection) {
        return;
    }

    selection->strokesToPaths();
}

const Glib::ustring SECTION = NC_("Action Section", "Object");

std::vector<std::vector<Glib::ustring>> raw_data_object =
{
    // clang-format off
    {"app.object-set-attribute",      N_("Set Attribute"),           SECTION, N_("Set or update an attribute of selected objects; usage: object-set-attribute:attribute name, attribute value;")},
    {"app.object-set-property",       N_("Set Property"),            SECTION, N_("Set or update a property on selected objects; usage: object-set-property:property name, property value;")},
    {"app.object-get-attribute",      N_("Get Attribute"),           SECTION, N_("Get the value of an attribute of selected objects; usage: object-get-attribute:attribute name;")},
    {"app.object-get-property",       N_("Get Property"),            SECTION, N_("Get the value of a property on selected objects; usage: object-get-property:property name;")},
    {"app.object-remove-attribute",   N_("Remove Attribute"),        SECTION, N_("Remove an attribute on selected objects; usage: object-remove-attribute:property name;")},
    {"app.object-remove-property",    N_("Remove Property"),         SECTION, N_("Remove a property on selected objects; usage: object-remove-property:property name;")},

    {"app.object-unlink-clones",      N_("Unlink Clones"),           SECTION, N_("Unlink clones and symbols")},
    {"app.object-to-path",            N_("Object to Path"),          SECTION, N_("Convert shapes to paths")},
    {"app.object-add-corners-lpe",    N_("Add Corners LPE"),         SECTION, N_("Add Corners Live Path Effect to path")},
    {"app.object-stroke-to-path",     N_("Stroke to Path"),          SECTION, N_("Convert strokes to paths")},

    {"app.object-set-clip",           N_("Set Object Clipping"),         SECTION, N_("Apply clipping path to selection (using the topmost object as clipping path)")},
    {"app.object-set-inverse-clip",   N_("Set Object Inverse Clipping"), SECTION, N_("Apply inverse clipping path to selection (Power Clip LPE)")},
    {"app.object-release-clip",       N_("Release Object Clipping"),     SECTION, N_("Remove clipping path from selection")},
    {"app.object-set-clip-group",     N_("Set Object Clipping Group"),   SECTION, N_("Create a self-clipping group to which objects (not contributing to the clip-path) can be added")},
    {"app.object-destructive-clip",   N_("Destructive Clip Bitmap"),     SECTION, N_("Permanently remove bitmap pixels outside exactly one selected closed vector cutter; the cutter is kept and Undo restores the original bitmap")},
    {"app.object-destructive-inverse-clip", N_("Destructive Inverse Clip Bitmap"), SECTION, N_("Permanently remove bitmap pixels inside exactly one selected closed vector cutter; the cutter is kept and Undo restores the original bitmap")},
    {"app.vacards-destructive-clip", N_("VACards Destructive Clip"), SECTION, N_("Destructively clip one bitmap with one closed vector cutter and report the outcome (command line)")},
    {"app.object-set-mask",           N_("Set Object Mask"),         SECTION, N_("Apply mask to selection (using the topmost object as mask)")},
    {"app.object-set-inverse-mask",   N_("Set Object Inverse Mask"), SECTION, N_("Apply inverse mask to selection (Power Mask LPE)")},
    {"app.object-release-mask",       N_("Release Object Mask"),     SECTION, N_("Remove mask from selection")},

    {"app.object-rotate-90-cw",       N_("Rotate Object 90°"),        SECTION, N_("Rotate selected objects 90° clockwise")},
    {"app.object-rotate-90-ccw",      N_("Rotate Object 90° CCW"),    SECTION, N_("Rotate selected objects 90° counter-clockwise")},
    {"app.object-flip-horizontal",    N_("Flip Object Horizontally"),  SECTION, N_("Flip selected objects horizontally")},
    {"app.object-flip-vertical",      N_("Flip Object Vertically"),    SECTION, N_("Flip selected objects vertically")},
    {"app.object-star-turn-upright",  N_("Turn Stars/Polygons Upright"), SECTION, N_("Turn stars and polygons upright")}
    // clang-format on
};

std::vector<std::vector<Glib::ustring>> hint_data_object =
{
    // clang-format off
    {"app.object-set-attribute",        N_("Enter comma-separated string for attribute name, attribute value") },
    {"app.object-set-property",         N_("Enter comma-separated string for property name, property value")  }
    // clang-format on
};

} // namespace

void update_destructive_bitmap_clip_actions(InkscapeApplication *app)
{
    if (!app) {
        return;
    }
    auto *selection = app->get_active_selection();
    auto *document = app->get_active_document();
    auto const enabled = selection && document && selection->document() == document &&
                         Inkscape::UI::Tools::DestructiveBitmapClip::selection_is_eligible(*selection);
    for (auto const *name : {"object-destructive-clip", "object-destructive-inverse-clip"}) {
        auto action = app->gio_app()->lookup_action(name);
        if (auto simple = std::dynamic_pointer_cast<Gio::SimpleAction>(action)) {
            simple->set_enabled(enabled);
        }
    }
}

void
add_actions_object(InkscapeApplication* app)
{
    Glib::VariantType Bool(  Glib::VARIANT_TYPE_BOOL);
    Glib::VariantType Int(   Glib::VARIANT_TYPE_INT32);
    Glib::VariantType Double(Glib::VARIANT_TYPE_DOUBLE);
    Glib::VariantType String(Glib::VARIANT_TYPE_STRING);

    auto *gapp = app->gio_app();

    // clang-format off
    gapp->add_action_with_parameter( "object-set-attribute",            String, sigc::bind(sigc::ptr_fun(&object_set_attribute),  app));
    gapp->add_action_with_parameter( "object-set-property",             String, sigc::bind(sigc::ptr_fun(&object_set_property),   app));
    gapp->add_action_with_parameter( "object-get-attribute",            String, sigc::bind(sigc::ptr_fun(&object_get_attribute),  app));
    gapp->add_action_with_parameter( "object-get-property",             String, sigc::bind(sigc::ptr_fun(&object_get_property),   app));
    gapp->add_action_with_parameter( "object-remove-attribute",         String, sigc::bind(sigc::ptr_fun(&object_remove_attribute),  app));
    gapp->add_action_with_parameter( "object-remove-property",          String, sigc::bind(sigc::ptr_fun(&object_remove_property),   app));
    gapp->add_action_with_parameter( "object-trace",                    String, sigc::bind(sigc::ptr_fun(&object_trace),          app));

    gapp->add_action(                "object-unlink-clones",            sigc::bind(sigc::ptr_fun(&object_unlink_clones),          app));
    gapp->add_action(                "object-to-path",                  sigc::bind(sigc::ptr_fun(&object_to_path),                app));
    gapp->add_action(                "object-add-corners-lpe",          sigc::bind(sigc::ptr_fun(&object_add_corners_lpe),        app));
    gapp->add_action(                "object-stroke-to-path",           sigc::bind(sigc::ptr_fun(&object_stroke_to_path),         app));

    gapp->add_action(                "object-set-clip",                 sigc::bind(sigc::ptr_fun(&object_clip_set),               app));
    gapp->add_action(                "object-set-inverse-clip",         sigc::bind(sigc::ptr_fun(&object_clip_set_inverse),       app));
    gapp->add_action(                "object-release-clip",             sigc::bind(sigc::ptr_fun(&object_clip_release),           app));
    gapp->add_action(                "object-set-clip-group",           sigc::bind(sigc::ptr_fun(&object_clip_set_group),         app));
    gapp->add_action(                "object-destructive-clip",         sigc::bind(sigc::ptr_fun(&object_destructive_clip),        app, Inkscape::UI::Tools::DestructiveBitmapClip::Mode::KeepInside));
    gapp->add_action(                "object-destructive-inverse-clip", sigc::bind(sigc::ptr_fun(&object_destructive_clip),        app, Inkscape::UI::Tools::DestructiveBitmapClip::Mode::KeepOutside));
    gapp->add_action_with_parameter( "vacards-destructive-clip",        String, sigc::bind(sigc::ptr_fun(&vacards_destructive_clip), app));
    gapp->add_action(                "object-set-mask",                 sigc::bind(sigc::ptr_fun(&object_mask_set),               app));
    gapp->add_action(                "object-set-inverse-mask",         sigc::bind(sigc::ptr_fun(&object_mask_set_inverse),       app));
    gapp->add_action(                "object-release-mask",             sigc::bind(sigc::ptr_fun(&object_mask_release),           app));

    // Deprecated, see app.transform-rotate(90)
    gapp->add_action(                "object-rotate-90-cw",             sigc::bind(sigc::ptr_fun(&object_rotate_90_cw),           app));
    gapp->add_action(                "object-rotate-90-ccw",            sigc::bind(sigc::ptr_fun(&object_rotate_90_ccw),          app));
    gapp->add_action(                "object-flip-horizontal",          sigc::bind(sigc::ptr_fun(&object_flip_horizontal),        app));
    gapp->add_action(                "object-flip-vertical",            sigc::bind(sigc::ptr_fun(&object_flip_vertical),          app));
    gapp->add_action(                "object-star-turn-upright",        sigc::bind(sigc::ptr_fun(&object_star_turn_upright),      app));
    // clang-format on

    app->get_action_extra_data().add_data(raw_data_object);
    app->get_action_hint_data().add_data(hint_data_object);
    update_destructive_bitmap_clip_actions(app);
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
