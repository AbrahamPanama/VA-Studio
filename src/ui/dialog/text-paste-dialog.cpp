// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * @brief Ask how to format text that is being pasted
 */
/* Authors:
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "text-paste-dialog.h"

#include <cstddef>
#include <vector>
#include <glibmm/i18n.h>
#include <gtk/gtk.h> // GTK_ACCESSIBLE_RELATION_* / gtk_accessible_update_relation (must precede gtkaccessible.h)
#include <gtkmm/checkbutton.h>
#include <gtkmm/dialog.h>
#include <gtkmm/label.h>
#include <gtkmm/window.h>

#include "desktop.h"
#include "ui/dialog-events.h"
#include "ui/dialog-run.h"
#include "ui/widget/desktop-widget.h"

namespace Inkscape::UI::Dialog {
namespace {

struct PasteModeOption {
    TextPasteMode mode;
    char const *label;
    char const *detail;
    char const *widget_name; ///< Stable widget name for GUI test automation (not an AT name).
};

struct PasteModeQuestion {
    char const *title;
    char const *question;
    char const *window_name;
    char const *remember_label;
    char const *remember_tooltip;
    char const *remember_name;
    PasteModeOption const *options;
    std::size_t option_count;
};

/**
 * Run one formatting question and return the decision.
 *
 * This is the only place a paste-mode dialog is built. It deliberately does not
 * touch Preferences: persistence belongs to the caller and only after a
 * successful insertion. Cancel, Escape and the window close button return
 * std::nullopt and change nothing.
 */
std::optional<TextPasteChoice> run_paste_mode_dialog(SPDesktop *desktop, PasteModeQuestion const &question,
                                                    TextPasteMode initial)
{
    if (question.option_count == 0 || !question.options) {
        return std::nullopt;
    }

    Gtk::Dialog dialog(_(question.title), true);
    dialog.set_resizable(false);
    dialog.set_name(question.window_name);

    Gtk::Window *parent = nullptr;
    if (desktop) {
        if (auto *widget = desktop->getDesktopWidget()) {
            parent = dynamic_cast<Gtk::Window *>(widget->get_root());
        }
    }
    if (parent) {
        dialog.set_transient_for(*parent);
    } else {
        sp_transientize(dialog);
    }

    auto &content = *dialog.get_content_area();
    content.set_spacing(6);

    auto *heading = Gtk::make_managed<Gtk::Label>(_(question.question), Gtk::Align::START, Gtk::Align::CENTER, false);
    heading->set_wrap(true);
    heading->set_xalign(0.0);
    content.append(*heading);

    Gtk::CheckButton *group = nullptr;
    Gtk::CheckButton *selected = nullptr;
    // RAII: any exception between here and the return (dialog_run and widget
    // construction can both throw) must not leak the option array.
    std::vector<Gtk::CheckButton *> radios(question.option_count, nullptr);

    for (std::size_t i = 0; i < question.option_count; ++i) {
        auto const &option = question.options[i];

        auto *radio = Gtk::make_managed<Gtk::CheckButton>(_(option.label), false);
        radio->set_halign(Gtk::Align::START);
        radio->set_name(option.widget_name);
        if (group) {
            radio->set_group(*group);
        } else {
            group = radio;
        }
        content.append(*radio);
        radios[i] = radio;

        if (option.mode == initial) {
            selected = radio;
        }

        auto *detail = Gtk::make_managed<Gtk::Label>(_(option.detail), Gtk::Align::START, Gtk::Align::CENTER, false);
        detail->set_wrap(true);
        detail->set_xalign(0.0);
        detail->set_margin_start(24);
        detail->add_css_class("dim-label");
        content.append(*detail);

        // The detail label is a separate widget; associate it with its radio so
        // assistive technology can read the option description. Widget names
        // above are a CSS/test hook and are NOT accessible names.
        // GTK_ACCESSIBLE_RELATION_DESCRIBED_BY is a reference LIST: the varargs
        // are a NULL-terminated list of GtkAccessible*, then -1 ends the
        // relation/value pairs (gtkaccessiblevalue.c, GTK_ACCESSIBLE_COLLECT_REFERENCE_LIST).
        gtk_accessible_update_relation(GTK_ACCESSIBLE(radio->gobj()), GTK_ACCESSIBLE_RELATION_DESCRIBED_BY,
                                       detail->gobj(), nullptr, -1);
    }

    // The caller resolves `initial` before the call (Automatic is not offered in
    // the two-option external question); index 0 remains the documented fallback.
    (selected ? selected : radios[0])->set_active(true);

    auto *remember = Gtk::make_managed<Gtk::CheckButton>(_(question.remember_label), false);
    remember->set_name(question.remember_name);
    remember->set_margin_top(6);
    remember->set_tooltip_text(_(question.remember_tooltip));
    content.append(*remember);

    auto *cancel = dialog.add_button(_("Cancel"), int(Gtk::ResponseType::CANCEL));
    cancel->set_name("text-paste-cancel");
    auto *paste = dialog.add_button(_("Paste"), int(Gtk::ResponseType::OK));
    paste->set_name("text-paste-accept");
    paste->add_css_class("suggested-action");
    dialog.set_default_response(int(Gtk::ResponseType::OK));

    auto const response = UI::dialog_run(dialog);
    if (response != int(Gtk::ResponseType::OK)) {
        // Cancelling or closing the dialog must not change any preference.
        return std::nullopt;
    }

    TextPasteChoice choice;
    choice.mode = TextPasteMode::Automatic;
    for (std::size_t i = 0; i < question.option_count; ++i) {
        if (radios[i] && radios[i]->get_active()) {
            choice.mode = question.options[i].mode;
            break;
        }
    }
    choice.remember = remember->get_active();
    return choice;
}

} // namespace

std::optional<TextPasteChoice> choose_text_paste_mode(SPDesktop *desktop, TextPasteMode initial)
{
    static constexpr PasteModeOption options[] = {
        {TextPasteMode::Automatic, N_("Automatic"),
         N_("Use the formatting of the copied text on the canvas, and the formatting of the text you paste into"),
         "text-paste-mode-automatic"},
        {TextPasteMode::Source, N_("Keep source formatting"),
         N_("Always keep the formatting of the copied text, even when pasting into existing text"),
         "text-paste-mode-source"},
        {TextPasteMode::Destination, N_("Paste without formatting"),
         N_("Always use the formatting of the text you paste into, or the Text tool's default style on the canvas"),
         "text-paste-mode-destination"},
    };
    static constexpr PasteModeQuestion question{
        N_("Paste Text"),
        N_("How should the pasted text be formatted?"),
        "text-paste-dialog",
        N_("Remember this choice"),
        N_("Use this formatting for future pastes and stop asking"),
        "text-paste-remember",
        options,
        G_N_ELEMENTS(options),
    };
    return run_paste_mode_dialog(desktop, question, initial);
}

std::optional<TextPasteChoice> choose_external_text_paste_mode(SPDesktop *desktop, TextPasteMode initial)
{
    static constexpr PasteModeOption options[] = {
        {TextPasteMode::Source, N_("Keep source formatting"),
         N_("Keep the font, size and supported formatting of the text copied from the other application"),
         "text-paste-external-mode-source"},
        {TextPasteMode::Destination, N_("Use destination formatting"),
         N_("Paste the text only, using the formatting of the text you paste into or the Text tool's default style"),
         "text-paste-external-mode-destination"},
    };
    static constexpr PasteModeQuestion question{
        N_("Paste Text from Another Application"),
        N_("This text has formatting from another application. How should it be pasted?"),
        "text-paste-external-dialog",
        N_("Remember this choice for external text and don't ask again"),
        N_("Use this formatting for future external text pastes and stop asking"),
        "text-paste-external-remember",
        options,
        G_N_ELEMENTS(options),
    };
    // The caller must have resolved Automatic from the destination context; the
    // two-option list has no Automatic entry, so an unresolved value would
    // silently mean "Keep source formatting".
    if (initial == TextPasteMode::Automatic) {
        initial = TextPasteMode::Source;
    }
    return run_paste_mode_dialog(desktop, question, initial);
}

} // namespace Inkscape::UI::Dialog

/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0)(case-label . +))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:fileencoding=utf-8:textwidth=99 :
