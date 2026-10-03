// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Common import dialog for .cdr and .vss files.
 */
#ifndef INKSCAPE_EXTENSION_INTERNAL_RVNGIMPORTDIALOG_H
#define INKSCAPE_EXTENSION_INTERNAL_RVNGIMPORTDIALOG_H

#include <climits>
#include <memory>
#include <vector>
#include <glib.h>
#include <librevenge/librevenge.h>
#include <librevenge-stream/librevenge-stream.h>
#include <gtkmm/adjustment.h>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/dialog.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/grid.h>
#include <gtkmm/label.h>
#include <gtkmm/spinbutton.h>

#include "document.h"
#include "ui/view/svg-view-widget.h"

namespace Inkscape::Extension::Internal {

inline std::span<char const> as_span(librevenge::RVNGString const &str)
{
    return {str.cstr(), str.size()};
}

/**
 * Open a librevenge input stream for a UTF-8 path.
 *
 * RVNGFileStream uses fopen(), which on Windows only understands the ANSI code
 * page. A path such as `Logo <star>.cdr` has no such name: the locale
 * conversion then returns NULL and RVNGFileStream(NULL) ends the process in
 * the UCRT invalid-parameter handler. On Windows that case falls back to
 * reading the file into memory (GLib opens UTF-8 paths through the wide API).
 *
 * Returns null when the file is missing, is not a regular file or cannot be
 * read; callers must treat that as an open failure.
 */
inline std::unique_ptr<librevenge::RVNGInputStream> open_rvng_input_stream(char const *utf8_path)
{
    if (!utf8_path || !*utf8_path || !g_file_test(utf8_path, G_FILE_TEST_IS_REGULAR)) {
        return nullptr;
    }
#ifdef _WIN32
    // Attempt the system code page; even if not possible the alternate short
    // (8.3) file name is used when available.
    if (auto *converted = g_win32_locale_filename_from_utf8(utf8_path)) {
        auto stream = std::make_unique<librevenge::RVNGFileStream>(converted);
        g_free(converted);
        return stream;
    }
    gchar *contents = nullptr;
    gsize length = 0;
    if (!g_file_get_contents(utf8_path, &contents, &length, nullptr)) {
        return nullptr;
    }
    std::unique_ptr<librevenge::RVNGInputStream> stream;
    if (length <= UINT_MAX) {
        stream = std::make_unique<librevenge::RVNGStringStream>(
            reinterpret_cast<unsigned char const *>(contents), static_cast<unsigned>(length));
    }
    g_free(contents);
    return stream;
#else
    return std::make_unique<librevenge::RVNGFileStream>(utf8_path);
#endif
}

class RvngImportDialog : public Gtk::Dialog
{
public:
    RvngImportDialog(std::vector<librevenge::RVNGString> const &pages);

    bool showDialog();
    int getSelectedPage() const { return _current_page; }

private:
    void _setPreviewPage();

    // Signal handlers
    void _onPageNumberChanged();
    void _onSpinButtonClickPressed(int n_press, double x, double y);
    void _onSpinButtonClickReleased(int n_press, double x, double y);

    Gtk::Box *vbox1;
    Gtk::Button *cancelbutton;
    Gtk::Button *okbutton;

    Gtk::Box *_page_selector_box;
    Gtk::Label *_labelSelect;
    Gtk::Label *_labelTotalPages;
    Gtk::SpinButton *_pageNumberSpin;

    std::vector<librevenge::RVNGString> const &_pages; // Document to be imported
    int _current_page = 1; // Current selected page
    bool _spinning = false; // Whether SpinButton is pressed (i.e. we're "spinning")

    std::unique_ptr<SPDocument> _doc;
    Inkscape::UI::View::SVGViewWidget _preview;
};

std::unique_ptr<SPDocument> rvng_open(
    char const *uri,
    bool (*is_supported)(librevenge::RVNGInputStream *),
    bool (*parse)(librevenge::RVNGInputStream *, librevenge::RVNGDrawingInterface *));

} // namespace Inkscape::Extension::Internal

#endif // INKSCAPE_EXTENSION_INTERNAL_RVNGIMPORTDIALOG_H
