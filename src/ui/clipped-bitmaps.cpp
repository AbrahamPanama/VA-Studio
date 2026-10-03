// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * CDR-1: convert bitmaps held in clip masks (CorelDRAW PowerClips) to plain
 * images.
 *
 * Copyright 2026 VA Studio authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "clipped-bitmaps.h"

#include <glibmm/i18n.h>
#include <glibmm/main.h>
#include <gtkmm/box.h>
#include <gtkmm/label.h>
#include <gtkmm/progressbar.h>
#include <gtkmm/window.h>
#include <memory>
#include <optional>
#include <set>

#include "desktop.h"
#include "document-undo.h"
#include "document.h"
#include "inkscape-window.h"
#include "message-stack.h"
#include "object/sp-clippath.h"
#include "object/sp-defs.h"
#include "object/sp-item-group.h"
#include "object/sp-image.h"
#include "object/sp-item.h"
#include "object/sp-lpe-item.h"
#include "object/sp-mask.h"
#include "object/sp-root.h"
#include "style.h"
#include "ui/dialog/bitmap-copy-dialog.h"
#include "util/units.h"

namespace Inkscape::UI::ClippedBitmaps {

namespace {

bool is_clipped(SPItem const &item)
{
    return item.getClipObject() || item.getMaskObject();
}

bool is_layer(SPItem const &item)
{
    auto const *group = cast<SPGroup>(&item);
    return group && group->isLayer();
}

// Not drawn: display:none or visibility hidden/collapse.
bool invisible(SPItem const &item)
{
    return item.isHidden() || (item.style && item.style->visibility.computed != SP_CSS_VISIBILITY_VISIBLE);
}

// An effect Make a Bitmap Copy renders into the image and then places the
// image outside of (has_rendering_effect in selection-chemistry.cpp). A clip
// under such an ancestor would move above that ancestor's other content.
bool has_rendering_effect(SPItem const &item)
{
    if (item.isFiltered() || item.getClipObject() || item.getMaskObject()) {
        return true;
    }
    if (auto const *lpe_item = cast<SPLPEItem>(&item); lpe_item && lpe_item->hasPathEffect()) {
        return true;
    }
    return item.style && item.style->opacity.value < SP_SCALE24_MAX;
}

// Visible content of \a item is bitmaps only (at least one); groups are walked.
bool bitmaps_only(SPItem const &item, bool &has_bitmap)
{
    if (is<SPImage>(&item)) {
        has_bitmap = true;
        return true;
    }
    if (auto const *group = cast<SPGroup>(&item)) {
        for (auto const &child : group->children) {
            auto const *child_item = cast<SPItem>(&child);
            if (!child_item || invisible(*child_item)) {
                continue;
            }
            if (!bitmaps_only(*child_item, has_bitmap)) {
                return false;
            }
        }
        return true;
    }
    return false; // shapes, text, clones: vectors
}

bool holds_bitmap(SPItem const &item)
{
    if (invisible(item)) {
        return false;
    }
    if (is<SPImage>(&item)) {
        return true;
    }
    for (auto const &child : item.children) {
        if (auto const *child_item = cast<SPItem>(&child); child_item && holds_bitmap(*child_item)) {
            return true;
        }
    }
    return false;
}

void walk(SPObject &parent, bool under_effect, Scan &result)
{
    for (auto &child : parent.children) {
        auto *item = cast<SPItem>(&child);
        if (!item || is<SPDefs>(&child) || invisible(*item) || item->isLocked()) {
            continue;
        }
        if (is_clipped(*item) && !is_layer(*item)) {
            bool has_bitmap = false;
            if (bitmaps_only(*item, has_bitmap) && has_bitmap) {
                if (under_effect) {
                    ++result.in_effect_groups;
                } else {
                    result.convertible.push_back(item);
                }
            } else if (holds_bitmap(*item)) {
                ++result.with_vectors;
            }
            continue; // the outermost clip decides; nested clips render inside it
        }
        if (is<SPGroup>(item)) {
            walk(*item, under_effect || has_rendering_effect(*item), result);
        }
    }
}

// Clip paths and masks used inside \a item, by id.
void collect_clips(SPItem const &item, std::set<std::string> &ids)
{
    for (auto const *object : {static_cast<SPObject const *>(item.getClipObject()),
                               static_cast<SPObject const *>(item.getMaskObject())}) {
        if (object && object->getId()) {
            ids.emplace(object->getId());
        }
    }
    for (auto const &child : item.children) {
        if (auto const *child_item = cast<SPItem>(&child)) {
            collect_clips(*child_item, ids);
        }
    }
}

Geom::Rect largest_bounds_px(std::vector<SPItem *> const &items)
{
    Geom::Rect largest(0, 0, 1, 1);
    double area = 0.0;
    for (auto *item : items) {
        if (auto bounds = item->documentVisualBounds(); bounds && bounds->area() > area) {
            area = bounds->area();
            largest = *bounds;
        }
    }
    return largest;
}

std::optional<BitmapCopyOptions> ask(SPDesktop *desktop, Scan const &found, bool on_open)
{
    auto *window = desktop ? desktop->getInkscapeWindow() : nullptr;
    if (!window) {
        return std::nullopt;
    }
    auto options = Dialog::run_clipped_bitmaps_dialog(*window, static_cast<int>(found.convertible.size()),
                                                      found.with_vectors, found.in_effect_groups,
                                                      largest_bounds_px(found.convertible),
                                                      BitmapCopyOptions::from_preferences(), on_open);
    if (options) {
        // The next Make a Bitmap Copy starts from these settings too; Keep
        // original is not part of this choice and keeps its stored value.
        auto stored = BitmapCopyOptions::from_preferences();
        stored.dpi = options->dpi;
        stored.antialias = options->antialias;
        stored.transparent = options->transparent;
        stored.save_to_preferences();
    }
    return options;
}

Glib::ustring unchanged_text(Scan const &found)
{
    Glib::ustring text;
    if (found.with_vectors > 0) {
        text += " " + Glib::ustring::compose(ngettext("%1 clip with vectors or text was left unchanged.",
                                                      "%1 clips with vectors or text were left unchanged.",
                                                      found.with_vectors),
                                             found.with_vectors);
    }
    if (found.in_effect_groups > 0) {
        text += " " + Glib::ustring::compose(
                          ngettext("%1 clipped bitmap inside a group with transparency or effects was left unchanged.",
                                   "%1 clipped bitmaps inside groups with transparency or effects were left unchanged.",
                                   found.in_effect_groups),
                          found.in_effect_groups);
    }
    return text;
}

void report(SPDesktop *desktop, int converted, Scan const &found)
{
    if (!desktop || !desktop->messageStack()) {
        return;
    }
    auto text = Glib::ustring::compose(
        ngettext("Converted %1 clipped bitmap to an image.", "Converted %1 clipped bitmaps to images.", converted),
        converted);
    desktop->messageStack()->flash(Inkscape::NORMAL_MESSAGE, text + unchanged_text(found));
}

/// A small modal window over the document: "Converting clipped bitmaps to
/// images…" and a bar counting the images done, "12 / 39".
class ProgressWindow : public Gtk::Window
{
public:
    ProgressWindow(Gtk::Window &parent, int total)
        : _box(Gtk::Orientation::VERTICAL, 10)
    {
        set_title(_("Converting Clipped Bitmaps"));
        set_transient_for(parent);
        set_modal(true);
        set_deletable(false);
        set_resizable(false);
        _label.set_text(_("Converting clipped bitmaps to images…"));
        _label.set_xalign(0);
        _bar.set_show_text(true);
        _bar.set_size_request(360, -1);
        _box.set_margin(18);
        _box.append(_label);
        _box.append(_bar);
        set_child(_box);
        show_progress(0, total);
        // Closing it would end the modality while the conversion goes on;
        // it closes itself when the conversion ends.
        signal_close_request().connect([] { return true; }, false);
    }

    void show_progress(int done, int total)
    {
        _bar.set_fraction(total > 0 ? static_cast<double>(done) / total : 0.0);
        _bar.set_text(Glib::ustring::compose(C_("Conversion progress, images done / total", "%1 / %2"), done, total));
    }

private:
    Gtk::Box _box;
    Gtk::Label _label;
    Gtk::ProgressBar _bar;
};

struct ProgressRun
{
    std::unique_ptr<Conversion> conversion;
    std::unique_ptr<ProgressWindow> window;
    SPDesktop *desktop = nullptr;
    Scan counts; ///< only the counts of unchanged clips; no item pointers
    sigc::scoped_connection desktop_gone;
};

// One image per main-loop turn. The short pause lets the progress window
// draw its new state before the next render, which can block the main loop
// for seconds, starts.
void next_step(std::shared_ptr<ProgressRun> const &run, unsigned delay_ms = 30)
{
    Glib::signal_timeout().connect_once([run] {
        if (run->conversion->step()) {
            if (run->window) {
                run->window->show_progress(run->conversion->processed(), run->conversion->total());
            }
            next_step(run);
            return;
        }
        int const converted = run->conversion->finish();
        run->window.reset();
        if (run->desktop) {
            report(run->desktop, converted, run->counts);
        }
    }, delay_ms);
}

void convert_with_progress(SPDesktop *desktop, SPDocument &document, std::vector<std::string> const &ids,
                           BitmapCopyOptions const &options, bool undoable, Scan const &found)
{
    auto run = std::make_shared<ProgressRun>();
    run->conversion = std::make_unique<Conversion>(document, ids, options, undoable);
    if (!run->conversion->started()) {
        // Something owns the history; never fold into it.
        if (undoable && desktop->messageStack()) {
            desktop->messageStack()->flash(Inkscape::WARNING_MESSAGE,
                                           _("Finish the current operation first, then convert the clipped bitmaps."));
        }
        return;
    }
    run->desktop = desktop;
    run->counts.with_vectors = found.with_vectors;
    run->counts.in_effect_groups = found.in_effect_groups;
    run->desktop_gone = desktop->connectDestroy([weak = std::weak_ptr<ProgressRun>(run)](SPDesktop *) {
        if (auto live = weak.lock()) {
            live->desktop = nullptr;
            live->window.reset();
        }
    });
    if (auto *parent = desktop->getInkscapeWindow()) {
        run->window = std::make_unique<ProgressWindow>(*parent, run->conversion->total());
        run->window->present();
    }
    next_step(run, 200); // the window maps and draws before the first render
}

// Asks and converts in \a desktop's document; the dialog runs a nested main
// loop, so the desktop and the document may close meanwhile.
void ask_and_convert(SPDesktop *desktop, bool on_open)
{
    auto *document = desktop->getDocument();
    if (DocumentUndo::interactionActive(document)) {
        if (!on_open && desktop->messageStack()) {
            desktop->messageStack()->flash(Inkscape::WARNING_MESSAGE,
                                           _("Finish the current operation first, then convert the clipped bitmaps."));
        }
        return;
    }
    auto const found = scan(*document);
    if (found.convertible.empty()) {
        if (!on_open && desktop->messageStack()) {
            desktop->messageStack()->flash(Inkscape::WARNING_MESSAGE,
                                           _("No clipped bitmaps can be converted in this document.") +
                                               unchanged_text(found));
        }
        return;
    }
    auto const ids = found.ids();
    auto alive = std::make_shared<bool>(true);
    sigc::scoped_connection desktop_gone = desktop->connectDestroy([alive](SPDesktop *) { *alive = false; });
    sigc::scoped_connection document_gone = document->connectDestroy([alive] { *alive = false; });
    auto options = ask(desktop, found, on_open); // nested main loop
    if (!*alive || desktop->getDocument() != document || !options) {
        return;
    }
    if (DocumentUndo::interactionActive(document)) {
        return; // something started meanwhile; never fold into it
    }
    convert_with_progress(desktop, *document, ids, *options, !on_open, found);
}

} // namespace

Scan scan(SPDocument &document)
{
    Scan result;
    if (auto *root = document.getRoot()) {
        document.ensureUpToDate();
        walk(*root, false, result);
    }
    return result;
}

std::vector<std::string> Scan::ids() const
{
    std::vector<std::string> result;
    for (auto *item : convertible) {
        if (item && item->getId()) {
            result.emplace_back(item->getId());
        }
    }
    return result;
}

int convert(SPDocument &document, std::vector<std::string> const &ids, BitmapCopyOptions options, bool undoable)
{
    Conversion conversion(document, ids, options, undoable);
    while (conversion.step()) {
    }
    return conversion.finish();
}

Conversion::Conversion(SPDocument &document, std::vector<std::string> ids, BitmapCopyOptions options, bool undoable)
    // Topmost first: an image that lands after a shared effect-carrying
    // ancestor (Make a Bitmap Copy placement) then keeps the stacking order.
    : _document(&document)
    , _ids(ids.rbegin(), ids.rend())
    , _options(options)
    , _undoable(undoable)
{
    _options.keep_original = false;
    _options.commit_undo = false;
    if (DocumentUndo::interactionActive(&document)) {
        return; // a live preview or drag owns the history
    }
    _interaction = DocumentUndo::beginRollbackableInteraction(&document);
    if (_interaction) {
        _document_gone = document.connectDestroy([this] {
            _document = nullptr;
            _interaction.reset(); // closing already rolled the conversion back
        });
    }
}

Conversion::~Conversion()
{
    // Stopped before finish(): nothing may stay half converted.
    if (_interaction && _interaction->active()) {
        _interaction->rollback();
    }
}

bool Conversion::usable() const
{
    return _document && _interaction && _interaction->active();
}

bool Conversion::step()
{
    if (_result || !usable() || _next >= total()) {
        return false;
    }
    auto const &id = _ids[_next++];
    auto *item = cast<SPItem>(_document->getObjectById(id));
    if (!item) {
        return true;
    }
    std::set<std::string> used;
    collect_clips(*item, used);
    Inkscape::ObjectSet set(_document);
    set.add(item);
    set.createBitmapCopy(_options);
    if (set.size() == 1 && is<SPImage>(set.singleItem()) && !_document->getObjectById(id)) {
        ++_converted;
        _clips.merge(used);
        if (auto const *image_id = set.singleItem()->getId()) {
            _images.emplace_back(image_id);
        }
    }
    return true;
}

void Conversion::remove_unused_clips()
{
    // CorelDRAW clip paths have no inkscape:collect; remove the ones nothing
    // uses any more (masks can hold large bitmaps).
    for (auto const &id : _clips) {
        auto *object = _document->getObjectById(id);
        if (object && object->hrefcount == 0 && is<SPDefs>(object->parent)) {
            object->deleteObject(false);
        }
    }
}

int Conversion::images_present() const
{
    int count = 0;
    for (auto const &id : _images) {
        if (is<SPImage>(_document->getObjectById(id))) {
            ++count;
        }
    }
    return count;
}

int Conversion::finish()
{
    if (_result) {
        return *_result;
    }
    if (!_document || !_interaction) {
        _result = 0; // never started, or the document closed (closing rolled it back)
        return 0;
    }
    if (!_interaction->active()) {
        // Ended from outside between two steps. Closing rolls the work back;
        // another action's Undo step keeps the images made so far. Count
        // what is really there, tidy it and, when opening, keep it out of the
        // history like the rest of the conversion.
        _interaction.reset();
        if (DocumentUndo::interactionCloseRequested(_document)) {
            _result = 0;
            return 0;
        }
        int const kept = images_present();
        if (kept > 0) {
            remove_unused_clips();
            DocumentUndo::done(_document, RC_("Undo", "Convert clipped bitmaps to images"), "selection-make-bitmap-copy");
            if (!_undoable) {
                DocumentUndo::clearUndo(_document);
            }
        }
        _result = kept;
        return kept;
    }
    remove_unused_clips();
    if (_converted > 0) {
        _interaction->commit(RC_("Undo", "Convert clipped bitmaps to images"), "selection-make-bitmap-copy");
    } else {
        _interaction->rollback();
    }
    _interaction.reset();
    if (_converted > 0 && !_undoable) {
        // Part of opening: record the edits pending before it as usual, so
        // they stay consistent, then drop the history. Right after opening it
        // holds at most the automatic fixes made while opening.
        DocumentUndo::done(_document, RC_("Undo", "Convert clipped bitmaps to images"), "selection-make-bitmap-copy");
        DocumentUndo::clearUndo(_document);
    }
    _result = _converted;
    return _converted;
}

bool is_coreldraw_file(std::string const &path)
{
    auto const lower = Glib::ustring(path).lowercase();
    for (auto const *suffix : {".cdr", ".cdt", ".ccx", ".cmx"}) {
        if (lower.size() >= 4 && lower.compare(lower.size() - 4, 4, suffix) == 0) {
            return true;
        }
    }
    return false;
}

void offer_after_open(SPDesktop *desktop)
{
    if (!desktop) {
        return;
    }
    // Once the window is up; the desktop may close before that.
    auto alive = std::make_shared<bool>(true);
    auto connection = std::make_shared<sigc::connection>(desktop->connectDestroy([alive](SPDesktop *) { *alive = false; }));
    Glib::signal_timeout().connect_once([desktop, alive, connection] {
        connection->disconnect();
        if (*alive && desktop->getDocument()) {
            ask_and_convert(desktop, true);
        }
    }, 300);
}

void convert_in_desktop(SPDesktop *desktop)
{
    if (desktop && desktop->getDocument()) {
        ask_and_convert(desktop, false);
    }
}

} // namespace Inkscape::UI::ClippedBitmaps
