// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * @brief System-wide clipboard management - class declaration
 *//*
 * Authors: see git history
 *   Krzysztof Kosiński <tweenk@o2.pl>
 *   Jon A. Cruz <jon@joncruz.org>
 *
 *
 * Copyright (C) 2018 Authors
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef INKSCAPE_UI_CLIPBOARD_H
#define INKSCAPE_UI_CLIPBOARD_H

#include <glibmm/ustring.h>
#include <cstddef>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace Geom {
class Rect;
class Point;
}

class SPDesktop;
class SPDocument;

namespace Inkscape {

class ObjectSet;
namespace XML { class Node; }
namespace LivePathEffect { class PathParam; }

namespace UI {

/**
 * How a rich text fragment is pasted. Automatic applies the context rules
 * (destination typing style inside an edited text object, copied character and
 * paragraph format when creating a new text object); Source always keeps the
 * copied character format; Destination strips it.
 */
enum class TextPasteMode {
    Automatic = 0,
    Source = 1,
    Destination = 2,
};

struct SelectionCopyLimits {
    std::size_t nodes = 100000;
    std::size_t references = 100000;
    std::size_t depth = 128; // Values above the safe recursive ceiling (256) are rejected.
    std::size_t svg_bytes = 32u * 1024 * 1024; // Source/work/output budgets; expanded refs are charged conservatively.
    std::size_t image_pixels = 16u * 1024 * 1024;
    std::size_t total_image_pixels = 64u * 1024 * 1024;
};

struct DetachedSelection {
    std::unique_ptr<SPDocument> document;
    std::vector<std::string> warnings;
    DetachedSelection();
    ~DetachedSelection();
    DetachedSelection(DetachedSelection &&) noexcept;
    DetachedSelection &operator=(DetachedSelection &&) noexcept;
    DetachedSelection(DetachedSelection const &) = delete;
    DetachedSelection &operator=(DetachedSelection const &) = delete;
};

class SelectionCopyCancelled final : public std::runtime_error {
public:
    SelectionCopyCancelled();
};

// Synchronous native-document staging, NOT an untrusted SVG importer. The caller
// keeps source and its up-to-date document alive and unchanged on their owning
// thread, including during cancellation callbacks. Future UI jobs must retain a
// source-operation lease until return. No desktop/system clipboard is accessed.
// Unsupported input/limits throw runtime_error; cancellation throws the subtype
// above. Failure discards private work; no partial document is returned.
DetachedSelection copy_selection_detached(ObjectSet &source,
    SelectionCopyLimits const &limits = {}, std::function<bool()> cancelled = {});

/**
 * @brief System-wide clipboard manager
 *
 * ClipboardManager takes care of manipulating the system clipboard in response
 * to user actions. It holds a complete SPDocument as the contents. This document
 * is exported using output extensions when other applications request data.
 * Copying to another instance of Inkscape is special-cased, because of the extra
 * data required (i.e. style, size, Live Path Effects parameters, etc.)
 */

class ClipboardManager
{
public:
    // Reports clipboard-unavailable and returns false before any mutation.
    // Cut must check this before invoking its existing copy/delete behavior.
    virtual bool ensureClipboard(SPDesktop *desktop = nullptr) = 0;
    virtual void copy(ObjectSet *set) = 0;
    virtual void copyPathParameter(Inkscape::LivePathEffect::PathParam *) = 0;
    virtual bool copyString(Glib::ustring str) = 0;
    virtual void copySymbol(Inkscape::XML::Node* symbol, gchar const* style, SPDocument *source, const char* symbol_set, Geom::Rect const &bbox, bool set_clipboard) = 0;
    virtual void insertSymbol(SPDesktop *desktop, Geom::Point const &shift_dt, bool read_clipboard) = 0;
    virtual bool paste(SPDesktop *desktop, bool in_place = false, bool on_page = false) = 0;
    /**
     * Paste the clipboard text with an explicit formatting policy. The existing
     * paste() signature keeps its behavior; this entry point is for the explicit
     * "paste source formatting" / "paste without formatting" actions.
     */
    virtual bool pasteText(SPDesktop *desktop, TextPasteMode mode) = 0;
    virtual bool pasteStyle(ObjectSet *set) = 0;
    virtual bool pasteSize(ObjectSet *set, bool separately, bool apply_x, bool apply_y) = 0;
    virtual bool pastePathEffect(ObjectSet *set) = 0;
    virtual Glib::ustring getPathParameter(SPDesktop* desktop) = 0;
    virtual Glib::ustring getShapeOrTextObjectId(SPDesktop *desktop) = 0;
    virtual std::vector<Glib::ustring> getElementsOfType(SPDesktop *desktop, gchar const* type = "*", gint maxdepth = -1) = 0;
    virtual Glib::ustring getFirstObjectID() = 0;

    static ClipboardManager *get();
    // Native regression seam: refuses access even when the singleton cached a clipboard.
    static void setClipboardUnavailableForTesting(bool unavailable);

protected:
    ClipboardManager() = default;
    ClipboardManager(ClipboardManager const &) = delete;
    ClipboardManager &operator=(ClipboardManager const &) = delete;
};

} // namespace UI
} // namespace Inkscape

#endif // INKSCAPE_UI_CLIPBOARD_H
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
