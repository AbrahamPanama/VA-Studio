// SPDX-License-Identifier: GPL-2.0-or-later

#include "offset-shapes-tool.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>
#include <utility>

#include <glibmm/i18n.h>
#include <glibmm/main.h>

#include "actions/actions-tools.h"
#include "desktop.h"
#include "display/control/canvas-item-bpath.h"
#include "document-undo.h"
#include "document.h"
#include "gc.h"
#include "message-context.h"
#include "object/sp-item.h"
#include "preferences.h"
#include "selection.h"
#include "style.h"
#include "svg/svg.h"
#include "ui/widget/events/canvas-event.h"
#include "ui/icon-names.h"
#include "xml/node.h"
#include "xml/repr.h"

namespace Inkscape::UI::Tools {
namespace {

constexpr auto PREVIEW_FILL = 0x1683d82c;
constexpr auto PREVIEW_STROKE = 0x1683d8ff;

} // namespace

OffsetShapesTool::OffsetShapesTool(SPDesktop *desktop)
    : ToolBase(desktop, "/tools/offsetshapes", "select.svg", false)
    , _options(read_options())
{
    auto *selection = desktop->getSelection();
    if (!selection) {
        return;
    }

    std::vector<SPItem *> items;
    for (auto *item : selection->items()) {
        items.push_back(item);
        _original_selection.emplace_back(item);
    }
    _preparation = OffsetShapes::prepare(items);
    _selection_changed = selection->connectChanged([this](Selection *) { selection_changed(); });
    _selection_modified = selection->connectModified([this](Selection *, int) { selection_changed(); });
    enableSelectionCue();

    if (_preparation) {
        rebuild_preview();
        if (_preparation.skipped_count || _preparation.open_subpaths_skipped) {
            message_context->setF(WARNING_MESSAGE,
                                  _("Offset preview ready; skipped %zu unsupported objects and %zu open subpaths. "
                                    "Press <b>Enter</b> to apply or <b>Esc</b> to cancel."),
                                  _preparation.skipped_count, _preparation.open_subpaths_skipped);
        } else {
            message_context->set(INFORMATION_MESSAGE,
                                 _("Adjust the offset options, then press <b>Enter</b> or <b>Apply</b>. "
                                   "Press <b>Esc</b> to cancel."));
        }
    } else {
        message_context->set(WARNING_MESSAGE,
                             _("Select one or more supported closed vector shapes to offset."));
    }
}

OffsetShapesTool::~OffsetShapesTool()
{
    _selection_changed.disconnect();
    _selection_modified.disconnect();
    _preview_timeout.disconnect();
    clear_preview();
    enableSelectionCue(false);
}

bool OffsetShapesTool::is_ready() const
{
    return true;
}

OffsetShapes::Options OffsetShapesTool::read_options() const
{
    auto *prefs = Preferences::get();
    OffsetShapes::Options options;
    auto const legacy_distance = prefs->getDouble("/options/defaultoffsetwidth/value", 3.7795275591);
    options.distance_px = std::max(0.001, prefs->getDouble("/tools/offsetshapes/distance", legacy_distance));
    options.direction = static_cast<OffsetShapes::Direction>(
        std::clamp(prefs->getInt("/tools/offsetshapes/direction", 0), 0, 2));
    options.corner = static_cast<OffsetShapes::Corner>(
        std::clamp(prefs->getInt("/tools/offsetshapes/corner", 2), 0, 2));
    options.miter_limit = std::max(1.0, prefs->getDouble("/tools/offsetshapes/miter_limit", 4.0));
    options.outer_shapes_only = prefs->getBool("/tools/offsetshapes/outer_shapes_only", false);
    options.select_results = prefs->getBool("/tools/offsetshapes/select_results", true);
    options.delete_originals = prefs->getBool("/tools/offsetshapes/delete_originals", false);
    options.simplify_results = prefs->getBool("/tools/offsetshapes/simplify_results", false);
    options.simplify_tolerance_px =
        std::max(0.001, prefs->getDouble("/tools/offsetshapes/simplify_tolerance", 0.05));
    return options;
}

void OffsetShapesTool::set_options(OffsetShapes::Options const &options)
{
    if (_leaving || _committing) {
        return;
    }
    _options = options;
    _preview_dirty = true;
    auto const generation = ++_generation;
    _preview_timeout.disconnect();
    _preview_timeout = Glib::signal_timeout().connect(
        [this, generation] {
            if (!_leaving && generation == _generation) rebuild_preview();
            return false;
        },
        50);
}

void OffsetShapesTool::rebuild_preview()
{
    _preview_timeout.disconnect();
    _preview_dirty = false;
    clear_preview();
    if (!_preparation || !OffsetShapes::geometry_still_matches(_preparation.sources)) {
        return;
    }

    _build = OffsetShapes::build(_preparation.sources, _options);
    if (!_build) {
        message_context->setF(WARNING_MESSAGE, _("Offset preview unavailable: %s"), _build.error.c_str());
        return;
    }

    std::vector<CanvasItemPtr<CanvasItemBpath>> preview;
    preview.reserve(_build.results.size());
    for (auto const &result : _build.results) {
        auto item = make_canvasitem<CanvasItemBpath>(_desktop->getCanvasTemp(), result.geometry_document, false);
        item->set_fill(PREVIEW_FILL, SP_WIND_RULE_EVENODD);
        item->set_stroke(PREVIEW_STROKE);
        item->set_stroke_width(1.5);
        preview.emplace_back(std::move(item));
    }
    _preview_items = std::move(preview);
    _preview_changed.emit(!_preview_items.empty());
    if (_build.simplify_skipped) {
        message_context->setF(WARNING_MESSAGE,
                              _("Offset preview ready; %zu outline(s) could not be simplified and were kept as "
                                "generated."),
                              _build.simplify_skipped);
    }
}

void OffsetShapesTool::clear_preview()
{
    _preview_timeout.disconnect();
    _preview_items.clear();
    _preview_changed.emit(false);
}

bool OffsetShapesTool::apply()
{
    if (_leaving || _committing || !_preparation) {
        return false;
    }
    if (_preview_dirty) rebuild_preview();
    if (!_build) return false;
    if (!OffsetShapes::geometry_still_matches(_preparation.sources)) {
        cancel(_("Offset Shapes was cancelled because a source object changed."));
        return false;
    }

    auto *document = _desktop->getDocument();
    if (!document) {
        return false;
    }

    // commit() arms this guard itself, exactly once and only right before the first document
    // mutation. If commit() refuses before mutating, the tool stays connected to the selection so a
    // later selection change still cancels. Never disconnect earlier.
    auto const result = OffsetShapes::commit(
        document, _preparation, _build, _options, OffsetShapes::CommitProtocol::Interaction, [this] {
            _committing = true;
            _selection_changed.disconnect();
            _selection_modified.disconnect();
        });
    if (!result) {
        if (result.mutation_started) {
            // commit() had already armed the guard (the selection signals are disconnected) and
            // mutated before it failed and rolled back. The tool can no longer observe selection
            // changes, so leave for the selector instead of staying active but inert.
            cancel(Glib::ustring::compose(_("Offset Shapes could not continue: %1"), result.error));
            return false;
        }
        _committing = false;
        report_error(result.error);
        return false;
    }

    clear_preview();
    if (_options.select_results) {
        _desktop->getSelection()->setList(result.created);
    } else if (_options.delete_originals) {
        std::unordered_set<SPItem *> const deleted(result.deleted.begin(), result.deleted.end());
        std::vector<SPItem *> surviving;
        for (auto const &weak : _original_selection) {
            if (auto *item = weak.get(); item && !deleted.contains(item)) surviving.push_back(item);
        }
        _desktop->getSelection()->setList(surviving);
    }
    _committing = false;
    message_context->flashF(INFORMATION_MESSAGE, ngettext("Created %zu offset path.",
                                                          "Created %zu offset paths.", result.created.size()),
                            result.created.size());
    return_to_selector();
    return true;
}

void OffsetShapesTool::selection_changed()
{
    if (!_leaving && !_committing) {
        cancel(_("Offset Shapes was cancelled because the selection changed."));
    }
}

void OffsetShapesTool::report_error(std::string const &message)
{
    auto const text = Glib::ustring::compose(_("Offset Shapes could not continue: %1"), message);
    message_context->flash(ERROR_MESSAGE, text.c_str());
}

void OffsetShapesTool::cancel(Glib::ustring const &reason)
{
    if (_leaving) {
        return;
    }
    clear_preview();
    if (reason.empty()) {
        message_context->flash(INFORMATION_MESSAGE, _("Offset Shapes cancelled; no objects were changed."));
    } else {
        message_context->flash(WARNING_MESSAGE, reason.c_str());
    }
    return_to_selector();
}

void OffsetShapesTool::return_to_selector()
{
    if (_leaving) {
        return;
    }
    _leaving = true;
    auto *desktop = _desktop;
    if (desktop->getDesktopWidget()) {
        set_active_tool(desktop, "Select");
    } else {
        desktop->setTool("/tools/select");
    }
}

void OffsetShapesTool::switching_away(std::string const &)
{
    _leaving = true;
    clear_preview();
}

bool OffsetShapesTool::root_handler(CanvasEvent const &event)
{
    bool handled = false;
    inspect_event(
        event,
        [&](KeyPressEvent const &key) {
            switch (get_latin_keyval(key)) {
                case GDK_KEY_Escape:
                    cancel();
                    handled = true;
                    break;
                case GDK_KEY_Return:
                case GDK_KEY_KP_Enter:
                    apply();
                    handled = true;
                    break;
                default:
                    break;
            }
        },
        [&](CanvasEvent const &) {});
    return handled || ToolBase::root_handler(event);
}

} // namespace Inkscape::UI::Tools
