// SPDX-License-Identifier: GPL-2.0-or-later
/** \file
 *
 *  Actions for Editing an object which require desktop
 *
 * Authors:
 *   Sushant A A <sushant.co19@gmail.com>
 *
 * Copyright (C) 2021 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include <giomm.h>
#include <glibmm/i18n.h>

#include "actions-edit-window.h"
#include "actions-helper.h"
#include "document-undo.h"
#include "inkscape-application.h"
#include "inkscape-window.h"
#include "desktop.h"
#include "document.h"
#include "selection-chemistry.h"
#include "ui/clipboard.h"
#include "ui/icon-names.h"

void
paste(InkscapeWindow* win)
{
    SPDesktop* dt = win->get_desktop();

    // Paste
    sp_selection_paste(dt, false);
}

/**
 * Paste clipboard text with an explicit formatting mode, bypassing the
 * "ask when pasting formatted text" question.
 *
 * Undo protocol (documented in the shared interface): ClipboardManager::pasteText
 * inserts without committing Undo, so the caller owns the single Undo step, as
 * in sp_selection_paste() for the normal paste action.
 */
static void
paste_text_with_mode(InkscapeWindow* win, Inkscape::UI::TextPasteMode mode)
{
    SPDesktop* dt = win->get_desktop();
    auto *clipboard = Inkscape::UI::ClipboardManager::get();

    if (clipboard && clipboard->pasteText(dt, mode)) {
        Inkscape::DocumentUndo::done(dt->getDocument(), RC_("Undo", "Paste"), INKSCAPE_ICON("edit-paste"));
    }
}

void
paste_keep_source_formatting(InkscapeWindow* win)
{
    paste_text_with_mode(win, Inkscape::UI::TextPasteMode::Source);
}

void
paste_without_formatting(InkscapeWindow* win)
{
    paste_text_with_mode(win, Inkscape::UI::TextPasteMode::Destination);
}

void
paste_in_place(InkscapeWindow* win)
{
    SPDesktop* dt = win->get_desktop();

    // Paste In Place
    sp_selection_paste(dt, true);
}

void
paste_on_page(InkscapeWindow* win)
{
    SPDesktop* dt = win->get_desktop();

    // Paste In Place
    sp_selection_paste(dt, true, true);
}

void
path_effect_parameter_next(InkscapeWindow* win)
{
    SPDesktop* dt = win->get_desktop();

    // Next path effect parameter
    sp_selection_next_patheffect_param(dt);
}

const Glib::ustring SECTION = NC_("Action Section", "Edit");

std::vector<std::vector<Glib::ustring>> raw_data_edit_window =
{
    // clang-format off
    {"win.paste",                        N_("Paste"),                        SECTION, N_("Paste objects from clipboard to mouse point, or paste text")},
    {"win.paste-in-place",               N_("Paste In Place"),               SECTION, N_("Paste objects from clipboard to the original position of the copied objects")},
    {"win.paste-on-page",                N_("Paste On Page"),                 SECTION, N_("Paste objects from clipboard into the same place on the selected page.")},
    {"win.paste-keep-source-formatting", N_("Paste with Source Formatting"), SECTION, N_("Paste text keeping the formatting of the copied text")},
    {"win.paste-without-formatting",     N_("Paste Without Formatting"),     SECTION, N_("Paste text using the destination's formatting, or the Text tool's default style on the canvas")},
    {"win.path-effect-parameter-next",   N_("Next path effect parameter"),   SECTION, N_("Show next editable path effect parameter")}
    // clang-format on
};

void
add_actions_edit_window(InkscapeWindow* win)
{
    // clang-format off
    win->add_action(        "paste",                           sigc::bind(sigc::ptr_fun(&paste), win));
    win->add_action(        "paste-in-place",                  sigc::bind(sigc::ptr_fun(&paste_in_place), win));
    win->add_action(        "paste-on-page",                   sigc::bind(sigc::ptr_fun(&paste_on_page), win));
    win->add_action(        "paste-keep-source-formatting",    sigc::bind(sigc::ptr_fun(&paste_keep_source_formatting), win));
    win->add_action(        "paste-without-formatting",        sigc::bind(sigc::ptr_fun(&paste_without_formatting), win));
    win->add_action(        "path-effect-parameter-next",      sigc::bind(sigc::ptr_fun(&path_effect_parameter_next), win));
    // clang-format on

    auto app = InkscapeApplication::instance();
    if (!app) {
        show_output("add_actions_edit_window: no app!");
        return;
    }
    app->get_action_extra_data().add_data(raw_data_edit_window);
}
