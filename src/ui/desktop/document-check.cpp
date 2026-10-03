// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Check for data loss when closing a document window.
 *
 * Copyright (C) 2004-2021 Authors
 *
 * The contents of this file may be used under the GNU General Public License Version 2 or later.
 *
 */

/* Authors:
 *   MenTaLguY
 *   Link Mauve
 *   Thomas Holder
 *   Tavmjong Bah
 */

#include "document-check.h"

#include <glibmm/i18n.h>  // Internationalization
#include <gtkmm/messagedialog.h>
#include <optional>

#include "desktop.h"
#include "document.h"
#include "document-undo.h"
#include "file.h"
#include "inkscape-application.h"
#include "inkscape-window.h"
#include "io/stream/bufferstream.h"
#include "object/sp-namedview.h"
#include "ui/dialog-run.h"
#include "ui/util.h"

static std::optional<int> document_check_test_response;
static unsigned document_check_prompt_count = 0;
unsigned document_check_prompt_count_for_testing() { return document_check_prompt_count; }
void reset_document_check_prompt_count_for_testing() { document_check_prompt_count = 0; }

void set_document_check_response_for_testing(int response)
{
    if (Inkscape::IO::file_io_test_hooks_enabled()) document_check_test_response = response;
}

static int run_dialog(Gtk::Window &window, char const * const save_text,
                      char const * const format, char const * const document_name)
{
    if (Inkscape::IO::file_io_test_hooks_enabled()) ++document_check_prompt_count;
    if (Inkscape::IO::file_io_test_hooks_enabled() && document_check_test_response) {
        auto response = *document_check_test_response;
        document_check_test_response.reset();
        return response;
    }
    auto fmt = '\n' + Glib::ustring(format);
    auto const message = g_markup_printf_escaped(fmt.c_str(), document_name);
    auto dialog = Gtk::MessageDialog{window, message, true, Gtk::MessageType::WARNING,
                                     Gtk::ButtonsType::NONE};
    g_free(message);

    dialog.property_destroy_with_parent() = true;

    // Don't allow text to be selected (via tabbing).
    auto const ma = dialog.get_message_area();
    g_assert(ma);
    ma->get_first_child()->set_focusable(false);

    dialog.set_title(_("Save Document"));
    dialog.add_button(_("Close _without saving"), Gtk::ResponseType::NO);
    dialog.add_button(_("_Cancel"),               Gtk::ResponseType::CANCEL);
    dialog.add_button(_(save_text),               Gtk::ResponseType::YES);
    dialog.set_default_response(Gtk::ResponseType::YES);

    return Inkscape::UI::dialog_run(dialog);
}

bool document_check_save_for_close(Gtk::Window &window, SPDocument *document)
{
    return sp_file_save_document(window, document, true);
}

static SPDocument *document_check_test_document = nullptr;
static Gtk::Window *document_check_test_window = nullptr;

void set_document_check_context_for_testing(SPDocument *document, Gtk::Window *window)
{
    if (Inkscape::IO::file_io_test_hooks_enabled()) {
        document_check_test_document = document;
        document_check_test_window = window;
        if (!document) document_check_test_response.reset();
    }
}

/** Check if closing document associated with window will cause data loss, and if so opens a dialog
 *  that gives user options to save or ignore.
 *
 *  Returns true if document should remain open.
 */
bool document_check_for_data_loss(SPDesktop *desktop)
{
    g_assert(desktop || (Inkscape::IO::file_io_test_hooks_enabled()
                         && document_check_test_document && document_check_test_window));
    auto document = document_check_test_document ? document_check_test_document : desktop->getDocument();
    auto window = document_check_test_window ? document_check_test_window : desktop->getInkscapeWindow();

    // An admitted publication must resolve before another loss decision is offered.
    if (auto *app = InkscapeApplication::instance()) {
        if (app->deferCloseForPublication(document, desktop)) return true;
        app->flushSettledPublication(document);
    }

    if (document->isModifiedSinceSave() || Inkscape::DocumentUndo::publicationPending(document) ||
        (InkscapeApplication::instance() && InkscapeApplication::instance()->publicationInFlight(document))) {
        // Document has been modified!

        int const response = run_dialog(*window, _("_Save"),
            _("<span weight=\"bold\" size=\"larger\">Save changes to document \"%s\" before closing?</span>\n\n"
              "If you close without saving, your changes will be discarded."),
            document->getDocumentName());

        switch (response) {
            case GTK_RESPONSE_YES:
            {
                // Save document
                if (desktop) sp_namedview_document_from_window(desktop); // Save window geometry in document.
                if (!document->isModifiedSinceSave()) break;
                if (!document_check_save_for_close(*window, document)) {
                    if (auto *app = InkscapeApplication::instance(); app &&
                        app->deferCloseForPublication(document, desktop)) return true;
                    // Save dialog cancelled or save failed.
                    return true;
                }
                break;
            }
            case GTK_RESPONSE_NO:
                break;
            default: // cancel pressed, or dialog was closed
                return true;
                break;
        }
    }

    // Check for data loss due to saving in lossy format.
    bool allow_data_loss = false;
    while (document->getReprRoot()->attribute("inkscape:dataloss") != nullptr && allow_data_loss == false) {
        // This loop catches if the user saves to a lossy format when in the loop. 

        int const response = run_dialog(*window, _("_Save as VA Studio SVG"),
            _("<span weight=\"bold\" size=\"larger\">The file \"%s\" was saved with a format that may cause data loss!</span>\n\n"
              "Do you want to save this file as VA Studio SVG (Inkscape SVG format)?"),
            document->getDocumentName() ? document->getDocumentName() : "Unnamed");

        switch (response) {
            case GTK_RESPONSE_YES:
            {
                if (!sp_file_save_dialog(*window, document, Inkscape::Extension::FILE_SAVE_METHOD_INKSCAPE_SVG)) {
                    // Save dialog cancelled or save failed.
                    return TRUE;
                }

                break;
            }
            case GTK_RESPONSE_NO:
                allow_data_loss = true;
                break;
            default: // cancel pressed, or dialog was closed
                return true;
                break;
        }
    }

    return false;
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
