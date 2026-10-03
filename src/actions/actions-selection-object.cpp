// SPDX-License-Identifier: GPL-2.0-or-later
/** \file
 *
 * Actions related to manipulation a selection of objects which don't require desktop.
 *
 * Authors:
 *   Sushant A A <sushant.co19@gmail.com>
 *
 * Copyright (C) 2021 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

/*
 * Note: Actions must be app level as different windows can have different selections
 *       and selections must also work from the command line (without GUI).
 */

#include "config.h" // WITH_VACARDS_NESTING

#include <algorithm>
#include <vector>

#include <giomm.h>
#include <glibmm/i18n.h>

#include "actions-selection-object.h"
#include "actions-helper.h"
#include "actions/actions-vacards-cli.h"
#include "actions/vacards-cli-result.h"
#include "document-undo.h"
#include "inkscape-application.h"
#include "inkscape-window.h"
#include "page-manager.h"
#include "selection.h"
#include "object/sp-item.h"
#include "object/sp-item-group.h"
#include "object/sp-shape.h"
#include "object/sp-use.h"

#include "ui/dialog/dialog-container.h" // Used by select_object_link() to open dialog to add hyperlink.
#include "ui/clipped-bitmaps.h"
#include "ui/dialog/bitmap-copy-dialog.h"
#include "ui/icon-names.h"

void
select_object_group(InkscapeApplication* app)
{
    Inkscape::Selection *selection = app->get_active_selection();
    selection->group();
    Inkscape::DocumentUndo::done(selection->document(), RC_("Undo", "Group"), INKSCAPE_ICON("object-group"));
}

void
select_object_ungroup(InkscapeApplication* app)
{
    Inkscape::Selection *selection = app->get_active_selection();

    selection->ungroup();
    Inkscape::DocumentUndo::done(selection->document(), RC_("Undo", "Ungroup"), INKSCAPE_ICON("object-ungroup"));
}

void
select_object_ungroup_all(InkscapeApplication* app)
{
    Inkscape::Selection *selection = app->get_active_selection();
    selection->ungroup_all();
}

void
select_object_ungroup_pop(InkscapeApplication* app)
{
    Inkscape::Selection *selection = app->get_active_selection();

    // Pop Selected Objects out of Group
    selection->popFromGroup();
}

void
select_object_link(InkscapeApplication* app)
{
    Inkscape::Selection *selection = app->get_active_selection();

    // Group with <a>
    // group() warns and returns null when nothing is selected; there is nothing to link.
    auto anchor = selection->group(true);
    if (!anchor) {
        return;
    }
    selection->set(anchor);

    // Open dialog to set link (command line runs have no desktop and no dialogs).
    if (auto desktop = selection->desktop()) {
        desktop->getContainer()->new_dialog("ObjectProperties");
    }
    Inkscape::DocumentUndo::done(selection->document(), RC_("Undo", "Anchor"), INKSCAPE_ICON("object-group"));
}

void
selection_top(InkscapeApplication* app)
{
    Inkscape::Selection *selection = app->get_active_selection();

    // Raise to Top
    selection->raiseToTop();
}

void
selection_raise(InkscapeApplication* app)
{
    Inkscape::Selection *selection = app->get_active_selection();

    // Raise
    selection->raise();
}

void
selection_lower(InkscapeApplication* app)
{
    Inkscape::Selection *selection = app->get_active_selection();

    // Lower
    selection->lower();
}

void
selection_bottom(InkscapeApplication* app)
{
    Inkscape::Selection *selection = app->get_active_selection();

    // Lower to Bottom
    selection->lowerToBottom();
}

void
selection_stack_up(InkscapeApplication *app)
{
    auto selection = app->get_active_selection();
    selection->stackUp();
}

void
selection_stack_down(InkscapeApplication *app)
{
    auto selection = app->get_active_selection();
    selection->stackDown();
}

void
selection_make_bitmap_copy(InkscapeApplication *app)
{
    auto selection = app->get_active_selection();
    if (!selection) {
        return;
    }
    // Make a Bitmap Copy (no dialog: command line and scripts)
    selection->createBitmapCopy();
}

void
selection_make_bitmap_copy_dialog(InkscapeApplication *app)
{
    auto selection = app->get_active_selection();
    if (!selection) {
        return;
    }
    auto *document = selection->document();
    auto *window = app->get_active_window();
    auto const bounds = selection->documentBounds(SPItem::VISUAL_BBOX);
    if (!window || selection->isEmpty() || !bounds) {
        // Command line, or nothing to render: the previous behaviour (and its messages).
        selection->createBitmapCopy();
        return;
    }
    auto options = Inkscape::UI::Dialog::run_bitmap_copy_dialog(*window, *bounds,
                                                                 Inkscape::BitmapCopyOptions::from_preferences());
    // The dialog runs a nested main loop: the window may have closed or the
    // document changed meanwhile.
    if (!options || app->get_active_window() != window || app->get_active_selection() != selection ||
        selection->document() != document) {
        return;
    }
    options->save_to_preferences();
    selection->createBitmapCopy(*options);
}

void
selection_make_bitmap_copy_repeat(InkscapeApplication *app)
{
    // Repeat Bitmap Copy: the last options used, without a dialog.
    if (auto *selection = app->get_active_selection()) {
        selection->createBitmapCopy(Inkscape::BitmapCopyOptions::from_preferences());
    }
}

void
convert_clipped_bitmaps(InkscapeApplication *app)
{
    // CDR-1: every clipped bitmap of the document, one Undo step; the selection is not used.
    Inkscape::UI::ClippedBitmaps::convert_in_desktop(app->get_active_desktop());
}

void
page_fit_to_selection(InkscapeApplication *app)
{
    SPDocument* document = nullptr;
    Inkscape::Selection* selection = nullptr;
    if (!get_document_and_selection(app, &document, &selection)) {
        return;
    }

    document->getPageManager().fitToSelection(selection);
    Inkscape::DocumentUndo::done(document, RC_("Undo", "Resize page to fit"), INKSCAPE_ICON("tool-pages"));
}

#ifdef WITH_VACARDS_NESTING
/**
 * Fill and emit one nesting-contour agent-CLI record. Existing VACards actions
 * predate the agent CLI and report only when no desktop is attached, so GUI use
 * keeps its current behavior and prints no record.
 */
void emit_contour_record(InkscapeApplication *app, char const *action, Inkscape::VACardsCli::Status status,
                         char const *reason, std::string message, std::size_t selected,
                         std::vector<std::string> created, std::vector<std::string> modified, bool one_step,
                         int eligible = 0, boost::json::object data = {})
{
    if (!Inkscape::VACardsCli::records_enabled(app)) {
        return;
    }

    Inkscape::VACardsCli::Record r;
    r.action = action;
    r.mode = "collective-geometry";
    r.params_text = "";
    if (auto *document = app->get_active_document()) {
        char const *filename = document->getDocumentFilename();
        r.document_path = filename ? std::string(filename) : std::string();
    }
    r.selected = static_cast<int>(selected);
    r.eligible = eligible;
    r.status = status;
    r.reason = reason ? reason : "internal-error";
    r.message = std::move(message);
    r.created = std::move(created);
    r.modified = std::move(modified);
    r.one_undo_step = one_step;
    if (!data.empty()) {
        r.data = std::move(data);
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

void selection_set_nesting_contour(InkscapeApplication *app)
{
    auto *selection = app->get_active_selection();
    if (!selection) {
        emit_contour_record(app, "selection-set-nesting-contour", Inkscape::VACardsCli::Status::Rejected,
                            "no-document", "This action needs an open document.", 0, {}, {}, false);
        return;
    }
    auto range = selection->items();
    std::vector<SPItem *> items(range.begin(), range.end());
    auto const selected = items.size();
    if (items.size() < 2) {
        show_output("Select the artwork and one topmost vector cut contour.", true);
        emit_contour_record(app, "selection-set-nesting-contour", Inkscape::VACardsCli::Status::Rejected,
                            "needs-two-items", "Select the artwork and one topmost vector cut contour.",
                            selected, {}, {}, false);
        return;
    }

    // "Topmost" is stacking order, not selection order.
    std::sort(items.begin(), items.end(), sp_object_compare_position_bool);
    // group() re-creates every member from a copy of its XML, so capture all
    // selected ids in stacking order before the group is created.
    std::vector<std::string> member_ids;
    for (auto *item : items) {
        if (char const *id = item->getId()) {
            member_ids.emplace_back(id);
        }
    }
    SPItem *contour = nullptr;
    for (auto iterator = items.rbegin(); iterator != items.rend(); ++iterator) {
        if (is<SPShape>(*iterator)) {
            contour = *iterator;
            break;
        }
    }
    if (!contour) {
        show_output("The selection needs a vector shape to use as its nesting contour.", true);
        emit_contour_record(app, "selection-set-nesting-contour", Inkscape::VACardsCli::Status::Rejected,
                            "no-vector-shape", "The selection needs a vector shape to use as its nesting contour.",
                            selected, {}, {}, false);
        return;
    }

    // Mark before grouping: group() re-creates the members from copies of
    // their XML, so the marker travels with the contour.
    contour->setAttribute("inkscape:nesting-contour", "true");
    std::string const contour_id = contour->getId() ? contour->getId() : "";
    auto *payload = selection->group(); // selects the new group; no Undo step of its own
    if (!payload) {
        show_output("The selected artwork could not be grouped.", true);
        emit_contour_record(app, "selection-set-nesting-contour", Inkscape::VACardsCli::Status::Failed,
                            "group-failed", "The selected artwork could not be grouped.",
                            selected, {}, {}, false);
        Inkscape::DocumentUndo::cancel(selection->document());
        return;
    }
    payload->setAttribute("inkscape:nesting-contour-version", "1");
    Inkscape::DocumentUndo::done(selection->document(), RC_("Undo", "Set nesting contour"),
                                 INKSCAPE_ICON("object-group"));
    std::vector<std::string> created;
    if (char const *payload_id = payload->attribute("id")) {
        created.emplace_back(payload_id);
    }
    boost::json::object data;
    data["contour"] = contour_id;
    emit_contour_record(app, "selection-set-nesting-contour", Inkscape::VACardsCli::Status::Changed,
                        "success", "Nesting contour set.", selected, std::move(created), std::move(member_ids),
                        true, static_cast<int>(selected), std::move(data));
}

void selection_release_nesting_contour(InkscapeApplication *app)
{
    auto *selection = app->get_active_selection();
    if (!selection) {
        emit_contour_record(app, "selection-release-nesting-contour", Inkscape::VACardsCli::Status::Rejected,
                            "no-document", "This action needs an open document.", 0, {}, {}, false);
        return;
    }
    if (selection->isEmpty()) {
        emit_contour_record(app, "selection-release-nesting-contour", Inkscape::VACardsCli::Status::Rejected,
                            "empty-selection", "Select the nesting contour groups to release.", 0, {}, {}, false);
        return;
    }

    bool changed = false;
    std::vector<std::string> modified;
    auto clear_markers = [&](auto const &self, SPObject *object) -> void {
        // A clone's internal children share the original's XML nodes, so
        // clearing markers through them would strip the original too. The
        // <use> element's own repr is still cleared below.
        if (object->cloned) {
            return;
        }
        auto *repr = object->getRepr();
        bool removed = false;
        if (repr->attribute("inkscape:nesting-contour")) {
            repr->removeAttribute("inkscape:nesting-contour");
            removed = true;
        }
        if (repr->attribute("inkscape:nesting-contour-version")) {
            repr->removeAttribute("inkscape:nesting-contour-version");
            removed = true;
        }
        if (removed) {
            changed = true;
            // Report only the objects an attribute was actually removed from.
            if (char const *id = object->getId()) {
                modified.emplace_back(id);
            }
        }
        // Do not descend into an SPUse: its children are clones of another
        // element's subtree, not content this selection owns.
        if (!is<SPUse>(object)) {
            for (auto &child : object->children)
                self(self, &child);
        }
    };
    for (auto *object : selection->objects())
        clear_markers(clear_markers, object);

    if (changed) {
        Inkscape::DocumentUndo::done(selection->document(), RC_("Undo", "Release nesting contour"),
                                     INKSCAPE_ICON("object-group"));
        int const eligible = static_cast<int>(modified.size());
        emit_contour_record(app, "selection-release-nesting-contour", Inkscape::VACardsCli::Status::Changed,
                            "success", "Nesting contour released.", selection->size(), {}, std::move(modified), true,
                            eligible);
    } else {
        emit_contour_record(app, "selection-release-nesting-contour", Inkscape::VACardsCli::Status::Unchanged,
                            "no-markers", "The selection has no nesting contour markers.", selection->size(),
                            {}, {}, false);
    }
}
#endif

const Glib::ustring SECTION_SELECT = NC_("Action Section", "Select");
const Glib::ustring SECTION_PAGE = NC_("Action Section", "Page");

std::vector<std::vector<Glib::ustring>> raw_data_selection_object =
{
    // clang-format off
    { "app.selection-group",                NC_("Verb", "Group"),                     SECTION_SELECT, N_("Group selected objects")},
    { "app.selection-ungroup",              N_("Ungroup"),                            SECTION_SELECT, N_("Ungroup selected objects")},
    { "app.selection-ungroup-all",          N_("Ungroup All"),                        SECTION_SELECT, N_("Ungroup selected groups and every group inside them")},
    { "app.selection-ungroup-pop",          N_("Pop Selected Objects out of Group"),  SECTION_SELECT, N_("Pop selected objects out of group")},
    { "app.selection-link",                 NC_("Hyperlink|Verb", "Link"),            SECTION_SELECT, N_("Add an anchor to selected objects")},

    { "app.selection-top",                  N_("Raise to Top"),                       SECTION_SELECT, N_("Raise selection to top")},
    { "app.selection-raise",                N_("Raise"),                              SECTION_SELECT, N_("Raise selection one step")},
    { "app.selection-lower",                N_("Lower"),                              SECTION_SELECT, N_("Lower selection one step")},
    { "app.selection-bottom",               N_("Lower to Bottom"),                    SECTION_SELECT, N_("Lower selection to bottom")},

    { "app.selection-stack-up",             N_("Move up the Stack"),                  SECTION_SELECT, N_("Move the selection up in the stack order")},
    { "app.selection-stack-down",           N_("Move down the Stack"),                SECTION_SELECT, N_("Move the selection down in the stack order")},

    { "app.selection-make-bitmap-copy",     N_("Make a Bitmap Copy"),                 SECTION_SELECT, N_("Export selection to a bitmap and insert it into document")},
    { "app.selection-make-bitmap-copy-dialog", N_("Make a Bitmap Copy..."),            SECTION_SELECT, N_("Render the selection to a bitmap with chosen options and insert it into the document")},
    { "app.selection-make-bitmap-copy-repeat", N_("Repeat Bitmap Copy"),               SECTION_SELECT, N_("Make a bitmap copy with the last options used")},
    { "app.vacards-convert-clipped-bitmaps", N_("Convert Clipped Bitmaps to Images..."), SECTION_SELECT, N_("Replace each clip that holds only bitmaps with one image that looks the same")},
#ifdef WITH_VACARDS_NESTING
    { "app.selection-set-nesting-contour",  N_("Set as Nesting Contour"),             SECTION_SELECT, N_("Group the selection and use its topmost vector shape for nesting")},
    { "app.selection-release-nesting-contour", N_("Release Nesting Contour"),         SECTION_SELECT, N_("Stop using the marked vector shape as a nesting contour")},
#endif
    { "app.page-fit-to-selection",          N_("Resize Page to Selection"),           SECTION_PAGE,   N_("Fit the page to the current selection or the drawing if there is no selection")}
    // clang-format on
};

void update_ungroup_actions(InkscapeApplication *app)
{
    if (!app) {
        return;
    }
    auto *selection = app->get_active_selection();
    bool enabled = false;
    if (selection) {
        for (auto *group : selection->groups()) {
            if (!group->isLayer()) {
                enabled = true;
                break;
            }
        }
    }
    for (auto const *name : {"selection-ungroup", "selection-ungroup-all"}) {
        if (auto simple = std::dynamic_pointer_cast<Gio::SimpleAction>(app->gio_app()->lookup_action(name))) {
            simple->set_enabled(enabled);
        }
    }
}

void
add_actions_selection_object(InkscapeApplication* app)
{
    auto *gapp = app->gio_app();

    // clang-format off
    // See actions-layer.cpp for "enter-group" and "exit-group".
    gapp->add_action( "selection-group",              sigc::bind(sigc::ptr_fun(&select_object_group),           app));
    gapp->add_action( "selection-ungroup",            sigc::bind(sigc::ptr_fun(&select_object_ungroup),         app));
    gapp->add_action( "selection-ungroup-all",        sigc::bind(sigc::ptr_fun(&select_object_ungroup_all),     app));
    gapp->add_action( "selection-ungroup-pop",        sigc::bind(sigc::ptr_fun(&select_object_ungroup_pop),     app));
    gapp->add_action( "selection-link",               sigc::bind(sigc::ptr_fun(&select_object_link),            app));

    gapp->add_action( "selection-top",                sigc::bind(sigc::ptr_fun(&selection_top),                 app));
    gapp->add_action( "selection-raise",              sigc::bind(sigc::ptr_fun(&selection_raise),               app));
    gapp->add_action( "selection-lower",              sigc::bind(sigc::ptr_fun(&selection_lower),               app));
    gapp->add_action( "selection-bottom",             sigc::bind(sigc::ptr_fun(&selection_bottom),              app));

    gapp->add_action( "selection-stack-up",           sigc::bind(sigc::ptr_fun(&selection_stack_up),            app));
    gapp->add_action( "selection-stack-down",         sigc::bind(sigc::ptr_fun(&selection_stack_down),          app));

    gapp->add_action( "selection-make-bitmap-copy",   sigc::bind(sigc::ptr_fun(&selection_make_bitmap_copy),    app));
    gapp->add_action( "selection-make-bitmap-copy-dialog", sigc::bind(sigc::ptr_fun(&selection_make_bitmap_copy_dialog), app));
    gapp->add_action( "selection-make-bitmap-copy-repeat", sigc::bind(sigc::ptr_fun(&selection_make_bitmap_copy_repeat), app));
    gapp->add_action( "vacards-convert-clipped-bitmaps", sigc::bind(sigc::ptr_fun(&convert_clipped_bitmaps), app));
#ifdef WITH_VACARDS_NESTING
    gapp->add_action( "selection-set-nesting-contour", sigc::bind(sigc::ptr_fun(&selection_set_nesting_contour), app));
    gapp->add_action( "selection-release-nesting-contour", sigc::bind(sigc::ptr_fun(&selection_release_nesting_contour), app));
#endif
    gapp->add_action( "page-fit-to-selection",        sigc::bind(sigc::ptr_fun(&page_fit_to_selection),         app));
    // clang-format on

    app->get_action_extra_data().add_data(raw_data_selection_object);
    update_ungroup_actions(app);
}
