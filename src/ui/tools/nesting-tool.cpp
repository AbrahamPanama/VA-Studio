// SPDX-License-Identifier: GPL-2.0-or-later

#include "nesting-tool.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <2geom/path.h>
#include <glibmm/i18n.h>
#include <glibmm/main.h>
#include <glibmm/markup.h>

#include "desktop.h"
#include "display/control/canvas-item-bpath.h"
#include "display/control/canvas-item-text.h"
#include "document.h"
#include "layer-manager.h"
#include "message-context.h"
#include "nesting/nesting-settings.h"
#include "object/sp-rect.h"
#include "object/sp-shape.h"
#include "preferences.h"
#include "selection.h"
#include "ui/widget/events/canvas-event.h"
#include "util/scope_exit.h"
#include "xml/subtree-revision.h"

namespace Inkscape::UI::Tools {

namespace {

// Command on macOS, Control elsewhere. Control stays free on macOS, where
// Control-click is commonly a secondary click.
bool target_modifier(unsigned modifiers)
{
#ifdef __APPLE__
    return modifiers & GDK_META_MASK;
#else
    return modifiers & GDK_CONTROL_MASK;
#endif
}

bool is_target_modifier_key(unsigned keyval)
{
#ifdef __APPLE__
    return keyval == GDK_KEY_Meta_L || keyval == GDK_KEY_Meta_R;
#else
    return keyval == GDK_KEY_Control_L || keyval == GDK_KEY_Control_R;
#endif
}

bool is_shift_key(unsigned keyval)
{
    return keyval == GDK_KEY_Shift_L || keyval == GDK_KEY_Shift_R;
}

bool is_enter_key(unsigned keyval)
{
    return keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter || keyval == GDK_KEY_ISO_Enter;
}

Glib::ustring sheet_click()
{
#ifdef __APPLE__
    return _("Cmd-click");
#else
    return _("Ctrl-click");
#endif
}

/// A library reason ("a group cannot be used…") as a status bar sentence.
Glib::ustring sentence(std::string const &reason)
{
    Glib::ustring text{reason};
    if (!text.empty()) {
        text = text.substr(0, 1).uppercase() + text.substr(1);
        if (text[text.size() - 1] != '.') {
            text += ".";
        }
    }
    return text;
}

Glib::ustring markup_label(SPItem *item)
{
    auto const *label = item ? item->defaultLabel() : nullptr;
    return Glib::Markup::escape_text(label ? label : _("the sheet"));
}

/// The item's outline in desktop coordinates: its shape, or its visual bounds.
Geom::PathVector desktop_outline(SPItem *item)
{
    if (auto *shape = cast<SPShape>(item); shape && shape->curve() && !shape->curve()->empty()) {
        return *shape->curve() * item->i2dt_affine();
    }
    if (auto bounds = item->desktopVisualBounds()) {
        return Geom::PathVector(Geom::Path(*bounds));
    }
    return {};
}

Geom::PathVector rect_path(Geom::Rect const &rect)
{
    return Geom::PathVector(Geom::Path(rect));
}

/// The short L3 phrase for a reason the tool or sheetIneligibility gave.
Glib::ustring short_reason(Glib::ustring const &reason, bool selected, bool hidden_or_locked)
{
    if (selected) {
        return _("This is one of the parts");
    }
    if (hidden_or_locked) {
        return _("Hidden or locked objects cannot be the sheet");
    }
    auto const lower = reason.lowercase();
    if (lower.find("group") != Glib::ustring::npos) {
        return _("A group cannot be the sheet");
    }
    if (lower.find("clipped") != Glib::ustring::npos || lower.find("masked") != Glib::ustring::npos) {
        return _("Clipped or masked shapes cannot be the sheet");
    }
    if (lower.find("filter") != Glib::ustring::npos) {
        return _("Filtered shapes cannot be the sheet");
    }
    return _("This shape cannot be the sheet");
}

constexpr std::uint32_t BLUE = 0x258beae6;
constexpr std::uint32_t RED = 0xd0342ce6;
constexpr std::uint32_t GREEN = 0x2e7d32e6;
constexpr std::uint32_t ORANGE = 0xf5a623ff;
constexpr std::uint32_t DARK = 0x333333e6;
constexpr std::uint32_t WHITE = 0xffffffff;
constexpr std::uint32_t DARK_TEXT = 0x222222ff;

std::vector<SPWeakPtr<SPItem>> weak_list(std::vector<SPItem *> const &items)
{
    std::vector<SPWeakPtr<SPItem>> result;
    result.reserve(items.size());
    for (auto *item : items) {
        result.emplace_back(item);
    }
    return result;
}

} // namespace

NestingTool::NestingTool(SPDesktop *desktop)
    : SelectTool(desktop, "/tools/nesting", "cursor-nesting.svg")
{
    auto *selection = desktop->getSelection();
    _selection_changed = selection->connectChanged([this](Selection *) { selection_changed(); });
    _selection_modified = selection->connectModified([this](Selection *, int) { selection_modified(); });
    _document_replaced = desktop->connectDocumentReplaced([this](SPDesktop *, SPDocument *document) {
        // Hover outlines and labels belong to the outgoing document.
        clear_hover_items();
        bind_document(document);
    });
    bind_document(desktop->getDocument());
    _labels_observer = Preferences::get()->createObserver(Nesting::SHOW_LABELS_PREF_PATH, [this] { arrange_labels(); });
    // Screen-size labels keep their size while the drawing zooms or rotates,
    // so overlaps change: arrange them again.
    _zoom_changed = desktop->signal_zoom_changed.connect([this](double) { arrange_labels(); });
    show_idle_message();
}

void NestingTool::bind_document(SPDocument *document)
{
    _document_modified.disconnect();
    _refresh_idle.disconnect();
    _pointing_valid = false;
    _obstacle_cache.reset();
    if (document) {
        _document_modified = document->connectModified([this](unsigned) {
            // An edit (Undo included) can move, hide or lock the pointed object
            // or the remembered sheet.
            _pointing_valid = false;
            _obstacle_cache.reset();
            // Coalesce: a Select drag modifies the document on every motion.
            if (!_refresh_idle.connected()) {
                _refresh_idle = Glib::signal_idle().connect([this] {
                    refresh_sheet_outline();
                    _state_changed.emit();
                    return false;
                });
            }
        });
    }
}

NestingTool::~NestingTool()
{
    _completion_dest.close();
    if (_worker.joinable()) {
        _worker.request_stop();
        _worker.join();
    }
    clear_preview();
    _desktop->clearWaitingCursor();
}

Nesting::Options NestingTool::read_options() const
{
    auto *preferences = Preferences::get();
    Nesting::Options options;
    options.part_spacing = Nesting::readLengthPreferencePx(*preferences, Nesting::PART_SPACING_PREF_PATH);
    options.container_margin = Nesting::readLengthPreferencePx(*preferences, Nesting::CONTAINER_MARGIN_PREF_PATH);
    options.rotation_step_degrees =
        std::clamp(preferences->getDouble("/tools/nesting/rotation_step", 15.0), 0.1, 360.0);
    options.random_seed = static_cast<std::uint64_t>(std::max(0, preferences->getInt("/tools/nesting/random_seed", 0)));
    options.time_limit_ms = static_cast<std::uint64_t>(std::max(
        0, preferences->getInt("/tools/nesting/time_limit_ms",
                               static_cast<int>(Nesting::DEFAULT_OPTIMIZATION_TIME_MS))));
    options.rotation_mode = static_cast<Nesting::RotationMode>(
        std::clamp(preferences->getInt("/tools/nesting/rotation_mode", static_cast<int>(Nesting::RotationMode::Free)),
                   static_cast<int>(Nesting::RotationMode::None), static_cast<int>(Nesting::RotationMode::Free)));
    options.quality = static_cast<Nesting::Quality>(
        std::clamp(preferences->getInt("/tools/nesting/quality", static_cast<int>(Nesting::Quality::Balanced)),
                   static_cast<int>(Nesting::Quality::Draft), static_cast<int>(Nesting::Quality::High)));
    return options;
}


SPItem *NestingTool::top_level_item(SPItem *item) const
{
    // The object a plain Select click picks: the ancestor directly inside a
    // layer (or the entered group). Parts and sheets are whole objects.
    auto const &layers = _desktop->layerManager();
    while (item) {
        auto *parent = item->parent;
        if (!parent || !is<SPItem>(parent) || layers.isLayer(parent) || parent == layers.currentLayer() ||
            parent == layers.currentRoot()) {
            return item;
        }
        item = cast<SPItem>(parent);
    }
    return nullptr;
}

SPItem *NestingTool::item_under_pointer() const
{
    // Key events carry no position: use the last known pointer position.
    return top_level_item(sp_event_context_find_item(_desktop, _desktop->d2w(_desktop->point()), false, false));
}

std::optional<Glib::ustring> NestingTool::sheet_problem(SPItem *item) const
{
    if (_desktop->getSelection()->includes(item)) {
        return Glib::ustring{_("This object is one of the selected parts.")};
    }
    if (item->isHidden() || item->isLocked()) {
        return Glib::ustring{_("Hidden or locked objects cannot be used as the sheet.")};
    }
    if (auto reason = Nesting::sheetIneligibility(item)) {
        return sentence(*reason);
    }
    return std::nullopt;
}

void NestingTool::show_idle_message()
{
    if (auto *sheet = _memory.sheet.get()) {
        message_context->setF(NORMAL_MESSAGE,
                              _("Select the parts to nest. <b>%s</b> a sheet to add them to it; <b>Enter</b> adds "
                                "them to <b>%s</b>, <b>Shift+Enter</b> re-nests it."),
                              sheet_click().c_str(), markup_label(sheet).c_str());
    } else {
        message_context->setF(NORMAL_MESSAGE,
                              _("Select the parts to nest. <b>%s</b> a sheet to add them to it (with Shift: "
                                "re-nest it)."),
                              sheet_click().c_str());
    }
}

void NestingTool::update_pointing(unsigned modifiers, SPItem *under_pointer, bool may_reuse)
{
    if (_solving || _leaving) {
        return;
    }
    if (!target_modifier(modifiers)) {
        end_pointing();
        return;
    }
    // Motion repeats over the same object keep the state (sheetIneligibility
    // converts curves); key events, selection and document changes recompute.
    if (may_reuse && _pointing_valid && _pointing != Pointing::Parts && under_pointer == _pointed.get()) {
        apply_pointing_feedback(); // the Select tool may have replaced cursor or message
        if (auto &hover = _labels[static_cast<std::size_t>(Label::Hover)]; hover.item) {
            hover.desktop_point = _desktop->point(); // L2/L3 follow the pointer
            hover.item->set_coord(hover.desktop_point);
            arrange_labels();
        }
        return;
    }
    _pointed = SPWeakPtr<SPItem>(under_pointer);
    _pointing_valid = true;
    clear_hover_items();
    if (!under_pointer) {
        _pointing = Pointing::Nothing;
        _pointing_cursor = "cursor-nesting-target.svg";
        _pointing_message = _("Point at a closed shape to use as the sheet.");
    } else if (auto problem = sheet_problem(under_pointer)) {
        _pointing = Pointing::Invalid;
        _pointing_cursor = "cursor-nesting-invalid.svg";
        _pointing_message = *problem;
        // O4 and L3.
        if (auto bounds = under_pointer->desktopVisualBounds()) {
            _hover_outline = make_outline(rect_path(*bounds), 0xd0342cff, 1.5, {4.0, 2.0});
        }
        set_label(Label::Hover, _desktop->point(),
                  short_reason(*problem, _desktop->getSelection()->includes(under_pointer),
                               under_pointer->isHidden() || under_pointer->isLocked()),
                  RED, WHITE, {0, 0}, {16, 16});
    } else {
        _pointing = Pointing::Sheet;
        _pointing_cursor = "cursor-nesting-target.svg";
        auto const count = static_cast<unsigned long>(_desktop->getSelection()->size());
        auto const renest = mod_shift(modifiers);
        if (!renest) {
            _pointing_message = Glib::ustring::compose(
                ngettext("Click to add %1 object to <b>%2</b>.", "Click to add %1 objects to <b>%2</b>.", count),
                count, markup_label(under_pointer));
        } else if (under_pointer == _memory.sheet.get()) {
            _pointing_message = Glib::ustring::compose(
                _("Click to re-nest <b>%1</b>: its tracked parts and %2 selected objects."),
                markup_label(under_pointer), count);
        } else {
            _pointing_message = Glib::ustring::compose(
                _("Click to re-nest <b>%1</b> with %2 selected objects (no parts tracked here)."),
                markup_label(under_pointer), count);
        }
        show_hover_for_sheet(under_pointer, renest);
    }
    apply_pointing_feedback();
    refresh_sheet_items(); // L7 hides while pointing
}

void NestingTool::apply_pointing_feedback()
{
    set_cursor(_pointing_cursor);
    message_context->set(_pointing == Pointing::Invalid ? WARNING_MESSAGE : NORMAL_MESSAGE,
                         _pointing_message.c_str());
}

void NestingTool::end_pointing()
{
    clear_hover_items();
    _pointed.reset();
    _pointing_valid = false;
    if (_pointing == Pointing::Parts) {
        return;
    }
    _pointing = Pointing::Parts;
    set_cursor("cursor-nesting.svg");
    show_idle_message();
    refresh_sheet_items();
}

void NestingTool::clear_hover_items()
{
    _hover_outline.reset();
    _keepout_outlines.clear();
    _move_outlines.clear();
    clear_label(Label::Hover);
}

std::vector<SPItem *> NestingTool::obstacles_for(SPItem *sheet, bool renest)
{
    // Built once per sheet and mode; dropped on any document or selection
    // change (O5 performance rule). The same query the click would make.
    renest = renest && sheet == _memory.sheet.get();
    if (!_obstacle_cache || _obstacle_cache_sheet.get() != sheet || _obstacle_cache_renest != renest) {
        std::vector<SPItem *> parts;
        for (auto *item : _desktop->getSelection()->items()) {
            parts.push_back(item);
        }
        if (renest) {
            for (auto const &weak : _memory.tracked) {
                if (auto *item = weak.get()) {
                    parts.push_back(item);
                }
            }
        }
        auto const found = Nesting::collectSheetObstacles(sheet, _desktop->dkey, parts);
        _obstacle_cache.emplace();
        for (auto *item : found.items) {
            _obstacle_cache->emplace_back(item);
        }
        _obstacle_cache_sheet = SPWeakPtr<SPItem>(sheet);
        _obstacle_cache_renest = renest;
        ++_obstacle_cache_builds;
    }
    std::vector<SPItem *> alive;
    for (auto const &weak : *_obstacle_cache) {
        if (auto *item = weak.get()) {
            alive.push_back(item);
        }
    }
    return alive;
}

void NestingTool::show_hover_for_sheet(SPItem *sheet, bool renest)
{
    // O3: a halo over the sheet outline.
    _hover_outline = make_outline(desktop_outline(sheet), 0x258bea59, 3.0, {});
    // O5 (amendment C3): each obstacle's visual bounds until preparation runs
    // off the GTK thread (R1); capped so a crowded sheet stays responsive.
    auto const obstacles = obstacles_for(sheet, renest);
    constexpr std::size_t KEEPOUT_CAP = 200;
    for (auto *item : obstacles) {
        if (_keepout_outlines.size() >= KEEPOUT_CAP) {
            break;
        }
        if (auto bounds = item->desktopVisualBounds()) {
            _keepout_outlines.push_back(make_outline(rect_path(*bounds), 0x777777ff, 1.0, {3.0, 2.0}, 0x80808033));
        }
    }
    // O6 and L2.
    auto const selected = _desktop->getSelection()->size();
    Glib::ustring text;
    if (!renest) {
        text = Glib::ustring::compose(_("Add %1 · %2 fixed"), selected, obstacles.size());
    } else if (sheet == _memory.sheet.get()) {
        auto const sheet_bounds = sheet->documentVisualBounds();
        std::size_t tracked = 0;
        for (auto const &weak : _memory.tracked) {
            auto *item = weak.get();
            auto const bounds = item ? item->documentVisualBounds() : Geom::OptRect{};
            if (item && !item->isHidden() && bounds && sheet_bounds && bounds->intersects(*sheet_bounds) &&
                !_desktop->getSelection()->includes(item)) {
                ++tracked;
                if (auto dt = item->desktopVisualBounds()) {
                    _move_outlines.push_back(make_outline(rect_path(*dt), 0x258beaff, 1.0, {4.0, 2.0}, 0x258bea1a));
                }
            }
        }
        text = Glib::ustring::compose(_("Re-nest %1 (%2 + %3 new) · %4 fixed"), tracked + selected, tracked, selected,
                                      obstacles.size());
    } else {
        text = Glib::ustring::compose(_("Re-nest %1 (no parts tracked here)"), selected);
    }
    set_label(Label::Hover, _desktop->point(), text, BLUE, WHITE, {0, 0}, {16, 16});
}

void NestingTool::refresh_sheet_outline()
{
    refresh_sheet_items();
}

void NestingTool::refresh_sheet_items()
{
    auto *sheet = _memory.sheet.get();
    if (_leaving || !sheet || sheet->isHidden()) {
        _sheet_outline.reset();
        _margin_outline.reset();
        clear_label(Label::Sheet);
        clear_label(Label::Hint);
        return;
    }
    // O1.
    auto outline = desktop_outline(sheet);
    if (!_sheet_outline) {
        _sheet_outline = make_outline(outline, 0x258beaff, 1.5, {5.0, 3.0});
    } else {
        _sheet_outline->set_bpath(std::move(outline), false);
    }
    // O2: the margin line of an axis-aligned rectangular sheet.
    _margin_outline.reset();
    auto const margin = Nesting::readLengthPreferencePx(*Preferences::get(), Nesting::CONTAINER_MARGIN_PREF_PATH);
    auto const affine = sheet->i2doc_affine();
    if (margin > 0 && is<SPRect>(sheet) && std::abs(affine[1]) < 1e-9 && std::abs(affine[2]) < 1e-9) {
        if (auto bounds = sheet->documentGeometricBounds();
            bounds && bounds->width() > 2 * margin && bounds->height() > 2 * margin) {
            auto inset = *bounds;
            inset.expandBy(-margin);
            _margin_outline = make_outline(rect_path(inset) * _desktop->doc2dt(), 0x258beab3, 0.8, {2.0, 3.0});
        }
    }
    auto const window = sheet->desktopVisualBounds();
    if (!window) {
        return;
    }
    auto const screen = *window * _desktop->d2w();
    // L1: above the top-left corner.
    auto const sheet_bounds = sheet->documentVisualBounds();
    std::size_t tracked = 0;
    for (auto const &weak : _memory.tracked) {
        auto *item = weak.get();
        auto const bounds = item ? item->documentVisualBounds() : Geom::OptRect{};
        if (item && !item->isHidden() && bounds && sheet_bounds && bounds->intersects(*sheet_bounds)) {
            ++tracked;
        }
    }
    auto const *name = sheet->defaultLabel();
    Glib::ustring const label = name ? name : "";
    auto const summary = tracked ? Glib::ustring::compose(ngettext("%1 · %2 part · ~%3%% used",
                                                                   "%1 · %2 parts · ~%3%% used",
                                                                   static_cast<unsigned long>(tracked)),
                                                          label, tracked,
                                                          static_cast<int>(std::lround(_sheet_used_fraction * 100)))
                                 : Glib::ustring::compose(_("%1 · no parts yet"), label);
    set_label(Label::Sheet, _desktop->w2d(screen.corner(0)), summary, BLUE, WHITE, {0, 1}, {0, -4});
    // L7: an empty remembered sheet, a selection, no modifier.
    auto const selected = _desktop->getSelection()->size();
    if (!tracked && selected && _pointing == Pointing::Parts && !_solving) {
#ifdef __APPLE__
        auto const hint = Glib::ustring::compose(_("Cmd-click to nest %1 parts"), selected);
#else
        auto const hint = Glib::ustring::compose(_("Ctrl-click to nest %1 parts"), selected);
#endif
        set_label(Label::Hint, _desktop->w2d(screen.midpoint()), hint, 0x258beacc, WHITE, {0.5, 0.5}, {0, 0});
    } else {
        clear_label(Label::Hint);
    }
}

bool NestingTool::labels_enabled() const
{
    return Preferences::get()->getBool(Nesting::SHOW_LABELS_PREF_PATH, true);
}

void NestingTool::set_label(Label label, Geom::Point const &desktop_point, Glib::ustring const &text,
                            std::uint32_t background, std::uint32_t foreground, Geom::Point const &anchor,
                            Geom::Point const &adjust)
{
    auto &state = _labels[static_cast<std::size_t>(label)];
    if (!state.item) {
        state.item = make_canvasitem<CanvasItemText>(_desktop->getCanvasTemp(), desktop_point, text);
        state.item->set_fontsize(11);
        state.item->set_border(4);
        state.item->set_bg_radius(0.35); // fraction of half the height, as Inkscape's selection feedback
    } else {
        state.item->set_coord(desktop_point);
        state.item->set_text(text);
    }
    state.item->set_fill(foreground);
    state.item->set_background(background);
    state.item->set_anchor(anchor);
    state.item->set_adjust(adjust);
    state.desktop_point = desktop_point;
    state.anchor = anchor;
    state.adjust = adjust;
    state.text = text;
    arrange_labels();
}

void NestingTool::clear_label(Label label)
{
    auto &state = _labels[static_cast<std::size_t>(label)];
    if (state.item) {
        state = {};
        arrange_labels();
    }
}

void NestingTool::clear_result_label()
{
    _result_timeout.disconnect();
    clear_label(Label::Result);
}

std::optional<Geom::Rect> NestingTool::screen_rect(LabelState const &state) const
{
    if (!state.item) {
        return {};
    }
    auto const size = state.item->get_text_size();
    auto const point = _desktop->d2w(state.desktop_point);
    return Geom::Rect::from_xywh(point.x() - state.anchor.x() * size.width() + state.adjust.x(),
                                 point.y() - state.anchor.y() * size.height() + state.adjust.y(), size.width(),
                                 size.height());
}

void NestingTool::arrange_labels()
{
    // Labels never overlap: in priority order, a label that would collide with
    // one already shown is hidden (8.1). The Labels toggle hides them all.
    auto const enabled = labels_enabled();
    std::vector<Geom::Rect> shown;
    for (auto &state : _labels) {
        if (!state.item) {
            continue;
        }
        auto visible = enabled;
        if (visible) {
            if (auto const rect = screen_rect(state)) {
                visible = std::none_of(shown.begin(), shown.end(),
                                       [&](Geom::Rect const &other) { return other.interiorIntersects(*rect); });
                if (visible) {
                    shown.push_back(*rect);
                }
            }
        }
        state.item->set_visible(visible);
    }
}

CanvasItemPtr<CanvasItemBpath> NestingTool::make_outline(Geom::PathVector const &desktop_path, std::uint32_t stroke,
                                                         double width, std::vector<double> dashes, std::uint32_t fill)
{
    auto outline = make_canvasitem<CanvasItemBpath>(_desktop->getCanvasTemp(), desktop_path, false);
    outline->set_stroke(stroke);
    outline->set_stroke_width(width);
    if (!dashes.empty()) {
        outline->set_dashes(std::move(dashes));
    }
    outline->set_fill(fill, SP_WIND_RULE_NONZERO);
    return outline;
}

void NestingTool::show_run_result(Nesting::ApplyResult const &applied, std::vector<SPItem *> const &unplaced)
{
    auto *sheet = _memory.sheet.get();
    auto const window = sheet ? sheet->desktopVisualBounds() : Geom::OptRect{};
    // L5: inside the sheet's bottom-left corner; 6 s or the next interaction.
    if (window) {
        auto const screen = *window * _desktop->d2w();
        auto const mode = _run ? _run->mode : NestMode::Add;
        auto const backgrounds = _run ? _run->background_count : 0;
        Glib::ustring text;
        auto background = GREEN;
        if (applied.placed_count == 0) {
            text = _("No parts fit");
            background = RED;
        } else {
            text = Glib::ustring::compose(mode == NestMode::Renest ? _("✓ %1 re-nested") : _("✓ %1 added"),
                                          applied.placed_count);
            if (applied.unplaced_count) {
                text += Glib::ustring::compose(_(" · %1 did not fit"), applied.unplaced_count);
            }
        }
        if (backgrounds) {
            text += Glib::ustring::compose(_(" · %1 background ignored"), backgrounds);
        }
        set_label(Label::Result, _desktop->w2d(Geom::Point(screen.left(), screen.bottom())), text, background, WHITE,
                  {0, 1}, {8, -8});
        _result_timeout = Glib::signal_timeout().connect(
            [this] {
                clear_label(Label::Result);
                return false;
            },
            6000);
    }
    // L6 and O7: the leftovers.
    Geom::OptRect leftovers;
    for (auto *item : unplaced) {
        if (auto bounds = item->desktopVisualBounds()) {
            leftovers.unionWith(*bounds * _desktop->d2w());
        }
    }
    if (leftovers) {
        auto box = *leftovers;
        box.expandBy(4.0);
        _leftover_outline = make_outline(rect_path(box * _desktop->w2d()), 0xf5a623ff, 2.0, {});
        set_label(Label::Leftover, _desktop->w2d(Geom::Point(leftovers->left(), leftovers->bottom())),
                  Glib::ustring::compose(_("Did not fit (%1)"), unplaced.size()), ORANGE, DARK_TEXT, {0, 0}, {0, 6});
    }
}

bool NestingTool::nest_into(SPItem *sheet)
{
    return nest(sheet, NestMode::Add);
}

bool NestingTool::nest_last_sheet(NestMode mode)
{
    if (auto *sheet = _memory.sheet.get()) {
        return nest(sheet, mode);
    }
    message_context->flashF(WARNING_MESSAGE, _("<b>%s</b> a sheet first."), sheet_click().c_str());
    return false;
}

bool NestingTool::nest(SPItem *sheet, NestMode mode)
{
    if (_solving || _leaving) {
        return false;
    }
    end_pointing();
    if (!sheet) {
        message_context->flash(WARNING_MESSAGE, _("Point at a closed shape to use as the sheet."));
        return false;
    }
    if (auto problem = sheet_problem(sheet)) {
        message_context->flash(WARNING_MESSAGE, problem->c_str());
        return false;
    }

    // Parts are captured now: for a re-nest the sheet's tracked parts first
    // (still on it), then the selection in selection order. Add places the
    // selection around everything already on the sheet (obstacles).
    std::vector<SPItem *> parts;
    std::unordered_set<SPItem *> unique;
    std::size_t tracked_count = 0;
    auto const sheet_bounds = sheet->documentVisualBounds();
    if (mode == NestMode::Renest && sheet == _memory.sheet.get() && sheet_bounds) {
        for (auto const &weak : _memory.tracked) {
            auto *item = weak.get();
            auto const bounds = item ? item->documentVisualBounds() : Geom::OptRect{};
            // A locked tracked part stays where it is, as a fixed object.
            if (item && !item->isHidden() && !item->isLocked() && bounds && bounds->intersects(*sheet_bounds) &&
                unique.insert(item).second) {
                parts.push_back(item);
                ++tracked_count;
            }
        }
    }
    for (auto *item : _desktop->getSelection()->items()) {
        if (item != sheet && !item->isLocked() && unique.insert(item).second) {
            parts.push_back(item);
        }
    }
    if (parts.empty()) {
        message_context->flash(WARNING_MESSAGE, mode == NestMode::Add ? _("Select the parts to nest first.")
                                                                      : _("Nothing to re-nest on this sheet."));
        return false;
    }

    // A cancelled run's worker may still be finishing; it references the old
    // snapshot, so it must be gone before the snapshot is replaced.
    _completion_dest.close();
    if (_worker.joinable()) {
        _worker.request_stop();
        _worker.join();
    }

    _longest_preview_update_seconds = 0.0;
    auto const tolerance = std::max(1.0e-6, Preferences::get()->getDouble("/tools/nesting/flatten_tolerance", 0.05));
    // Keep the same precise hotspot, but show the static blue chick for the
    // entire operation, including geometry preparation and final validation.
    set_cursor("cursor-nesting-busy.svg");
    auto const obstacles = Nesting::collectSheetObstacles(sheet, _desktop->dkey, parts);
    // C5/R1: only the capture runs here; the geometry is prepared on the worker.
    auto capture = Nesting::captureDocumentNesting(sheet, parts, tolerance, obstacles.items);
    if (!capture) {
        set_cursor("cursor-nesting.svg");
        report_error(capture.error);
        return false;
    }

    clear_preview(); // the preview model is sized for one snapshot
    _snapshot.reset();
    auto const input = capture.input;
    auto *document = capture.skeleton.document;
    _capture = std::move(capture);
    _run = RunContext{.mode = mode,
                      .sheet = SPWeakPtr<SPItem>(sheet),
                      .parts = weak_list(parts),
                      .tracked_count = tracked_count,
                      .obstacle_count = 0, // known when the geometry arrives
                      .background_count = obstacles.ignored_backgrounds.size()};
    _solving = true;
    message_context->setF(INFORMATION_MESSAGE, _("Preparing %zu objects… Press <b>Esc</b> to cancel."), parts.size());
    // Run start: the previous result and leftover cues go; L4 appears.
    clear_result_label();
    clear_label(Label::Leftover);
    clear_label(Label::Hint);
    _leftover_outline.reset();
    if (auto window = sheet->desktopVisualBounds()) {
        set_label(Label::Progress, window->midpoint(),
                  Glib::ustring::compose(_("Preparing… %1 of %2 · Esc cancels"), 0, parts.size()), DARK, WHITE,
                  {0.5, 0.5}, {0, 0});
    }
    _state_changed.emit();

    auto [completion_source, completion_dest] = Async::Channel::create();
    _completion_dest = std::move(completion_dest);
    auto const options = read_options();
    // Leftovers keep at least the part spacing, and never look nested (5 mm).
    _run_leftover_gap = std::max(options.part_spacing, 18.9);
    _worker = std::jthread([this, source = std::move(completion_source), options, input,
                            document](std::stop_token stop) mutable {
        // Phase B: geometry from plain data only; progress with the same throttle.
        auto last_update = std::chrono::steady_clock::now() - std::chrono::milliseconds(100);
        Nesting::CapturedGeometry geometry;
        try {
            geometry = Nesting::prepareCapturedGeometry(*input, stop, [&](std::size_t done, std::size_t total) {
                auto const now = std::chrono::steady_clock::now();
                if (done < total && now - last_update < std::chrono::milliseconds(100)) {
                    return;
                }
                last_update = now;
                source.run([this, done, total] { preparation_progress(done, total); });
            });
        } catch (std::exception const &error) {
            // On the GTK thread glibmm used to catch these; a worker must not
            // let them reach std::terminate.
            geometry.error = std::string{"geometry preparation failed: "} + error.what();
        } catch (...) {
            geometry.error = "geometry preparation failed unexpectedly";
        }
        if (!geometry) {
            source.run([this, error = geometry.error] { abort_run(error); });
            return;
        }
        // The worker solves its own copy; the GTK thread assembles the snapshot
        // (with the document references) for the preview and the commit.
        auto solving = Nesting::solvingSnapshot(document, *input, geometry);
        source.run([this, geometry = std::move(geometry)]() mutable { geometry_ready(std::move(geometry)); });

        last_update = std::chrono::steady_clock::now() - std::chrono::milliseconds(100);
        // The engine attaches a layout only to the report that improves it. A
        // throttled report must not lose that layout: it rides on the next
        // report that is forwarded, or the live preview would rarely update.
        std::optional<Nesting::Progress> pending_layout;
        auto result = Nesting::solvePreparedNesting(
            solving, options,
            [this, &source, &last_update, &pending_layout](Nesting::Progress const &progress) {
                auto const now = std::chrono::steady_clock::now();
                auto const terminal = progress.stage != 1;
                if (!progress.placements.empty()) {
                    pending_layout = progress;
                }
                if (!terminal && now - last_update < std::chrono::milliseconds(100)) {
                    return;
                }
                last_update = now;
                auto forwarded = progress;
                carry_pending_layout(forwarded, pending_layout);
                source.run([this, forwarded = std::move(forwarded)] { update_progress(forwarded); });
            },
            stop);
        source.run([this, result = std::move(result)]() mutable { finish_solve(std::move(result)); });
    });
    return true;
}

void NestingTool::preparation_progress(std::size_t done, std::size_t total)
{
    if (!_solving || _leaving || _snapshot) {
        return;
    }
    message_context->setF(INFORMATION_MESSAGE, _("Preparing geometry… %zu of %zu. Press <b>Esc</b> to cancel."), done,
                          total);
    if (auto &label = _labels[static_cast<std::size_t>(Label::Progress)]; label.item) {
        set_label(Label::Progress, label.desktop_point,
                  Glib::ustring::compose(_("Preparing… %1 of %2 · Esc cancels"), done, total), DARK, WHITE,
                  {0.5, 0.5}, {0, 0});
    }
}

void NestingTool::geometry_ready(Nesting::CapturedGeometry geometry)
{
    if (!_solving || _leaving || !_capture) {
        return;
    }
    auto *sheet = _run ? _run->sheet.get() : nullptr;
    auto prepared = Nesting::assemblePreparedNesting(std::move(*_capture), std::move(geometry));
    _capture.reset();
    if (!prepared) {
        abort_run(prepared.error);
        return;
    }
    // A part that preparation skips is neither moved nor an obstacle: if it
    // sits on the sheet, new parts could overlap it. Stop instead.
    auto const sheet_bounds = sheet ? sheet->documentVisualBounds() : Geom::OptRect{};
    for (auto const &skipped : prepared.snapshot->skipped_parts) {
        using Reason = Nesting::SkippedPartReason;
        auto *item = skipped.item.get();
        if (!item || item == sheet || item->isHidden() || skipped.reason == Reason::DuplicateItem ||
            skipped.reason == Reason::ContainerSelectedAsPart || skipped.reason == Reason::DifferentDocument) {
            continue;
        }
        auto const bounds = item->documentVisualBounds();
        if (bounds && sheet_bounds && bounds->intersects(*sheet_bounds)) {
            auto const *label = item->defaultLabel();
            abort_run(Glib::ustring::compose(_("a selected object on the sheet cannot be nested, so parts could "
                                               "overlap it: %1 (%2). Move it off the sheet or hide it."),
                                             label ? label : "", skipped.detail));
            return;
        }
    }
    _snapshot = std::move(*prepared.snapshot);
    if (_run) {
        _snapshot->metrics.ignored_background_count = _run->background_count;
        _run->obstacle_count = _snapshot->obstacles.size();
    }
    message_context->setF(INFORMATION_MESSAGE, _("Nesting %zu objects… Press <b>Esc</b> to cancel."),
                          _snapshot->parts.size());
    if (auto &label = _labels[static_cast<std::size_t>(Label::Progress)]; label.item) {
        set_label(Label::Progress, label.desktop_point,
                  Glib::ustring::compose(_("Nesting… %1 of %2 · %3%% · Esc cancels"), 0, _snapshot->parts.size(), 0),
                  DARK, WHITE, {0.5, 0.5}, {0, 0});
    }
}

void NestingTool::abort_run(std::string const &error)
{
    // A preparation error (or a part that cannot be protected): nothing was
    // written; the tool stays active.
    if (!_solving || _leaving) {
        return;
    }
    _solving = false;
    _completion_dest.close();
    if (_worker.joinable()) {
        _worker.request_stop();
        _worker.join();
    }
    clear_preview();
    _capture.reset();
    _snapshot.reset();
    end_run();
    report_error(error);
}

void NestingTool::update_progress(Nesting::Progress progress)
{
    if (!_solving || _leaving || !_snapshot) {
        return;
    }

    if (_last_progress) {
        progress.iteration = std::max(progress.iteration, _last_progress->iteration);
        if (progress.stage == 1 && progress.best_score < _last_progress->best_score) {
            progress.best_score = _last_progress->best_score;
            progress.placed_count = _last_progress->placed_count;
            progress.placements.clear();
        }
    }
    if (!progress.placements.empty()) {
        update_preview(progress.placements);
    }
    _last_progress = progress;

    if (progress.stage == 0) {
        message_context->set(INFORMATION_MESSAGE, _("Validating nesting geometry… Press <b>Esc</b> to cancel."));
    } else if (progress.stage == 1) {
        auto const utilization = _snapshot->metrics.container_usable_area > 0.0
                                     ? progress.best_score / _snapshot->metrics.container_usable_area * 100.0
                                     : 0.0;
        if (auto &label = _labels[static_cast<std::size_t>(Label::Progress)]; label.item) {
            set_label(Label::Progress, label.desktop_point,
                      Glib::ustring::compose(_("Nesting… %1 of %2 · %3%% · Esc cancels"), progress.placed_count,
                                             progress.total_count, static_cast<int>(std::lround(utilization))),
                      DARK, WHITE, {0.5, 0.5}, {0, 0});
        }
        message_context->setF(INFORMATION_MESSAGE,
                              _("Nesting: best layout fits %u of %u objects, %.1f%% utilization (%.1f s)… "
                                "Press <b>Esc</b> to cancel."),
                              progress.placed_count, progress.total_count, utilization, progress.elapsed_seconds);
    } else {
        message_context->setF(INFORMATION_MESSAGE, _("Finalizing nesting of %u objects…"), progress.placed_count);
    }
}

void NestingTool::update_preview(std::span<Nesting::Placement const> placements)
{
    if (!_snapshot) {
        return;
    }
    auto const started = std::chrono::steady_clock::now();
    auto record = scope_exit([&] {
        _longest_preview_update_seconds = std::max(
            _longest_preview_update_seconds,
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
    });

    if (_preview_model && _preview_doc2dt != _desktop->doc2dt()) {
        // The document-to-desktop mapping changed (y axis toggled, page height
        // changed with the y axis up): rebuild every part on this layout.
        _preview_model.reset();
    }
    if (!_preview_model) {
        _preview_model.emplace(*_snapshot);
        _preview_doc2dt = _desktop->doc2dt();
        _preview_items.clear();
        _preview_items.resize(_preview_model->size());
        _preview_document_bounds.assign(_preview_model->size(), {});
        _preview_desktop_bounds.assign(_preview_model->size(), {});
    }
    // Only parts whose placement changed are rebuilt; their items are reused.
    for (auto &change : _preview_model->update(placements)) {
        auto &canvas_item = _preview_items[change.index];
        if (!change.visible) {
            if (canvas_item) {
                canvas_item->set_visible(false);
            }
            _preview_document_bounds[change.index] = {};
            _preview_desktop_bounds[change.index] = {};
            continue;
        }
        // The canvas draws in desktop coordinates (C4): with the y axis
        // pointing up, document coordinates are mirrored.
        auto desktop_path = change.path * _desktop->doc2dt();
        _preview_document_bounds[change.index] = change.path.boundsFast();
        _preview_desktop_bounds[change.index] = desktop_path.boundsFast();
        if (!canvas_item) {
            canvas_item = make_canvasitem<CanvasItemBpath>(_desktop->getCanvasTemp(), std::move(desktop_path), false);
            canvas_item->set_fill(0x1683d82c, SP_WIND_RULE_EVENODD);
            canvas_item->set_stroke(0x1683d8ff);
            canvas_item->set_stroke_width(1.5);
        } else {
            canvas_item->set_bpath(std::move(desktop_path), false);
        }
        canvas_item->set_visible(true);
    }
}

bool NestingTool::label_visible(Label label) const
{
    auto const &state = _labels[static_cast<std::size_t>(label)];
    return state.item && state.item->is_visible();
}

Glib::ustring NestingTool::label_text(Label label) const
{
    return _labels[static_cast<std::size_t>(label)].text;
}

std::optional<Geom::Rect> NestingTool::label_screen_rect(Label label) const
{
    return screen_rect(_labels[static_cast<std::size_t>(label)]);
}

Geom::OptRect NestingTool::preview_document_bounds() const
{
    Geom::OptRect bounds;
    for (auto const &part : _preview_document_bounds) {
        bounds.unionWith(part);
    }
    return bounds;
}

Geom::OptRect NestingTool::preview_desktop_bounds() const
{
    Geom::OptRect bounds;
    for (auto const &part : _preview_desktop_bounds) {
        bounds.unionWith(part);
    }
    return bounds;
}

void NestingTool::clear_preview()
{
    _preview_items.clear();
    _preview_model.reset();
    _preview_document_bounds.clear();
    _preview_desktop_bounds.clear();
    _last_progress.reset();
}

void NestingTool::finish_solve(Nesting::SolveResult result)
{
    if (!_solving || _leaving || !_snapshot) {
        return;
    }
    _solving = false;

    if (!result) {
        clear_preview();
        _snapshot.reset();
        end_run();
        report_error(result.error.empty() ? Nesting::statusMessage(result.status) : result.error);
        return;
    }

    // A successful worker result can already be queued when Escape arrives.
    if (_worker.get_stop_token().stop_requested()) {
        clear_preview();
        _snapshot.reset();
        end_run();
        return;
    }
    auto *sheet = _run->sheet.get();
    auto const sheet_bounds = sheet ? sheet->documentVisualBounds() : Geom::OptRect{};
    std::unordered_set<SPItem *> in_run;
    for (auto const &weak : _run->parts) {
        in_run.insert(weak.get());
    }
    // Earlier leftovers still staged beside this sheet (not in this run).
    auto const staged_here = [&](SPItem *item) {
        auto const bounds = item ? item->documentVisualBounds() : Geom::OptRect{};
        return item && !in_run.contains(item) && bounds && sheet_bounds && bounds->left() >= sheet_bounds->right();
    };
    // New leftovers start right of them, so repeated runs do not pile up.
    auto start_x = std::numeric_limits<double>::quiet_NaN();
    for (auto const &weak : _memory.staged) {
        if (auto *item = weak.get(); staged_here(item)) {
            auto const right = item->documentVisualBounds()->right() + _run_leftover_gap;
            start_x = std::isnan(start_x) ? right : std::max(start_x, right);
        }
    }
    auto applied = Nesting::applyNestingPlacements(
        *_snapshot, result.placements,
        Nesting::LeftoverPlacement{.move_beside_container = true, .gap = _run_leftover_gap, .start_x = start_x});
    clear_preview();
    if (applied.status != Nesting::ApplyStatus::Applied && applied.status != Nesting::ApplyStatus::NoChange) {
        _snapshot.reset();
        end_run();
        report_error(applied.error);
        return;
    }

    std::unordered_map<std::uint64_t, bool> placed_by_id;
    for (auto const &placement : result.placements) {
        placed_by_id[placement.part_id] = placement.placed;
    }
    std::vector<SPItem *> placed;
    std::vector<SPItem *> unplaced;
    for (auto const &part : _snapshot->parts) {
        if (auto *item = part.item.get()) {
            (placed_by_id[part.id] ? placed : unplaced).push_back(item);
        }
    }

    // Tracking (add-to-sheet 3.4): Add keeps the tracked parts still on the
    // sheet and adds the parts placed now; Re-nest tracks the parts placed
    // now; another sheet starts over.
    if (sheet != _memory.sheet.get()) {
        _memory = SheetMemory{.sheet = SPWeakPtr<SPItem>(sheet)};
    }
    std::vector<SPWeakPtr<SPItem>> tracked;
    if (_run->mode == NestMode::Add) {
        for (auto const &weak : _memory.tracked) {
            auto *item = weak.get();
            auto const bounds = item ? item->documentVisualBounds() : Geom::OptRect{};
            if (item && !item->isHidden() && bounds && sheet_bounds && bounds->intersects(*sheet_bounds) &&
                !in_run.contains(item)) {
                tracked.push_back(weak);
            }
        }
    }
    for (auto *item : placed) {
        tracked.emplace_back(item);
    }
    _memory.tracked = std::move(tracked);
    std::vector<SPWeakPtr<SPItem>> staged;
    for (auto const &weak : _memory.staged) {
        if (staged_here(weak.get())) {
            staged.push_back(weak);
        }
    }
    for (auto *item : unplaced) {
        staged.emplace_back(item);
    }
    _memory.staged = std::move(staged);

    {
        _setting_selection = true;
        auto restore = scope_exit([this] { _setting_selection = false; });
        _desktop->getSelection()->setList(unplaced);
    }

    auto const &metrics = _snapshot->metrics;
    auto const substituted = metrics.repaired_count + metrics.fallback_count;
    Glib::ustring recovery_note;
    if (substituted > 0) {
        recovery_note = Glib::ustring::compose(
            _(" %1 part(s) used rectangular or inferred outlines because their geometry was invalid."), substituted);
        if (!metrics.recovery_details.empty()) {
            auto const &detail = metrics.recovery_details.front();
            recovery_note +=
                Glib::ustring::compose(_(" First: %1 (%2)."), detail.label, detail.reason);
        }
    }
    // Bitmaps in vector groups do not collide (owner rule); warn when the
    // vector outline is much smaller than the bitmaps it carries.
    if (metrics.sparse_vector_count > 0) {
        recovery_note += Glib::ustring::compose(
            _(" %1 group(s) were nested by vector artwork much smaller than their bitmaps (first: %2), so "
              "their bitmaps may overlap. Add a cut line or a nesting contour to keep them apart."),
            metrics.sparse_vector_count,
            metrics.sparse_vector_details.empty() ? Glib::ustring{}
                                                  : Glib::ustring{metrics.sparse_vector_details.front().label});
    }
    auto const skipped_count = applied.skipped_count;
    // L1: (fixed objects + parts placed now) over the usable sheet area.
    _sheet_used_fraction = metrics.container_usable_area > 0.0
                               ? std::clamp((metrics.obstacle_area + result.metrics.placed_contour_area) /
                                                metrics.container_usable_area,
                                            0.0, 1.0)
                               : 0.0;
    end_run();
    if (skipped_count) {
        recovery_note += Glib::ustring::compose(_(" %1 unsupported objects were left unchanged."), skipped_count);
    }
    // Say "moved" only for leftovers that actually moved; a repeated run can
    // find them already beside the sheet (NoChange, no Undo step).
    auto const moved = applied.leftover_moved_count;
    auto const mode = _run ? _run->mode : NestMode::Add;
    auto const obstacle_count = _run ? _run->obstacle_count : 0;
    auto const background_count = _run ? _run->background_count : 0;
    auto const unplaced_count = static_cast<unsigned long>(applied.unplaced_count);
    Glib::ustring leftover_note;
    if (unplaced_count) {
        if (moved == applied.unplaced_count) {
            leftover_note = Glib::ustring::compose(
                ngettext(" %1 did not fit; it was moved beside the sheet and is selected.",
                         " %1 did not fit; they were moved beside the sheet and are selected.", unplaced_count),
                unplaced_count);
        } else if (moved) {
            leftover_note = Glib::ustring::compose(
                ngettext(" %1 did not fit; %2 of them was moved beside the sheet and all are selected.",
                         " %1 did not fit; %2 of them were moved beside the sheet and all are selected.", moved),
                unplaced_count, moved);
        } else {
            leftover_note = Glib::ustring::compose(
                ngettext(" %1 did not fit and is selected.", " %1 did not fit and are selected.", unplaced_count),
                unplaced_count);
        }
        if (mode == NestMode::Add) {
            leftover_note += _(" Shift+Enter re-nests the whole sheet and may fit more.");
        }
    }
    if (background_count) {
        recovery_note = Glib::ustring::compose(
                            ngettext(" %1 background object covering the whole sheet was ignored.",
                                     " %1 background objects covering the whole sheet were ignored.",
                                     static_cast<unsigned long>(background_count)),
                            background_count) +
                        recovery_note;
    }
    Glib::ustring text;
    auto type = WARNING_MESSAGE;
    if (applied.placed_count == 0) {
        text = Glib::ustring::compose(applied.status == Nesting::ApplyStatus::NoChange
                                          ? _("None of the selected objects fit; the document was not changed.%1%2")
                                          : _("None of the selected objects fit.%1%2"),
                                      leftover_note, recovery_note);
    } else {
        auto const placed_count = static_cast<unsigned long>(applied.placed_count);
        if (mode == NestMode::Renest) {
            text = Glib::ustring::compose(ngettext("Re-nested %1 part.", "Re-nested %1 parts.", placed_count),
                                          placed_count);
        } else if (obstacle_count > 0) {
            text = Glib::ustring::compose(
                ngettext("Added %1 part around %2 fixed objects.", "Added %1 parts around %2 fixed objects.",
                         placed_count),
                placed_count, obstacle_count);
        } else {
            text = Glib::ustring::compose(ngettext("Nested %1 object.", "Nested %1 objects.", placed_count),
                                          placed_count);
        }
        if (!applied.unplaced_count && applied.status == Nesting::ApplyStatus::NoChange) {
            text += _(" They were already in place.");
        }
        text += leftover_note + recovery_note;
        if (!applied.unplaced_count && !skipped_count && metrics.sparse_vector_count == 0 && !background_count) {
            type = INFORMATION_MESSAGE;
        }
    }
    show_run_result(applied, unplaced);
    _run.reset();
    message_context->flash(type, text.c_str());
    _snapshot.reset(); // releases the document observer until the next run
}

void NestingTool::end_run()
{
    // The worker has finished or been joined; nothing references the snapshot.
    // _run stays until finish_solve has composed its message.
    _solving = false;
    clear_label(Label::Progress);
    set_cursor("cursor-nesting.svg");
    _desktop->clearWaitingCursor();
    refresh_sheet_outline();
    show_idle_message();
    _state_changed.emit();
}

void NestingTool::selection_changed()
{
    // Idle selection changes are ordinary part picking; only a running solve,
    // whose parts are frozen, is cancelled.
    _pointing_valid = false; // "a selected part" may have become valid or invalid
    if (_leaving || _setting_selection) {
        return;
    }
    if (!_solving) {
        _obstacle_cache.reset(); // the parts, and so the obstacles, changed
        clear_result_label();    // L5 ends on the next interaction
        refresh_sheet_items();   // L7 follows the selection
        return;
    }
    cancel(_("Nesting was cancelled because the selection changed."));
}

void NestingTool::selection_modified()
{
    // Selection "modified" is emitted on idle, so one queued by an edit made
    // before the run can arrive after it started. Cancel only when the
    // document really changed after the parts were captured; the apply-time
    // freshness check stays the backstop.
    auto const &revision = _snapshot ? _snapshot->revision : _capture ? _capture->skeleton.revision : nullptr;
    if (_leaving || _setting_selection || !_solving || !revision || !revision->changed()) {
        return;
    }
    cancel(_("Nesting was cancelled because the selected objects changed."));
}

void NestingTool::report_error(std::string const &message)
{
    auto const text = Glib::ustring::compose(_("Nesting could not continue: %1"), message);
    message_context->flash(ERROR_MESSAGE, text.c_str());
}

void NestingTool::cancel(Glib::ustring const &reason)
{
    if (_leaving || !_solving) {
        return;
    }
    _solving = false;
    _completion_dest.close();
    if (_worker.joinable()) {
        _worker.request_stop();
        _worker.join(); // the worker references the snapshot
    }
    clear_preview();
    _capture.reset();
    _snapshot.reset();
    end_run();
    if (reason.empty()) {
        message_context->flash(INFORMATION_MESSAGE, _("Nesting cancelled; no objects were changed."));
    } else {
        message_context->flash(WARNING_MESSAGE, reason.c_str());
    }
}

void NestingTool::switching_away(std::string const &new_tool)
{
    _leaving = true;
    _completion_dest.close();
    if (_worker.joinable()) {
        _worker.request_stop();
    }
    clear_preview();
    _hover_outline.reset();
    _sheet_outline.reset();
    _margin_outline.reset();
    _leftover_outline.reset();
    _keepout_outlines.clear();
    _move_outlines.clear();
    _result_timeout.disconnect();
    for (auto &label : _labels) {
        label = {};
    }
    _memory = {};
    SelectTool::switching_away(new_tool);
}

bool NestingTool::root_handler(CanvasEvent const &event)
{
    if (_solving) {
        // Only Escape acts while a run is in progress. Zoom and middle-button
        // pan stay available to inspect the live preview; the rest is consumed.
        if (auto key = dynamic_cast<KeyPressEvent const *>(&event); key && get_latin_keyval(*key) == GDK_KEY_Escape) {
            cancel();
            return true;
        }
        auto const button = dynamic_cast<ButtonEvent const *>(&event);
        auto const motion = dynamic_cast<MotionEvent const *>(&event);
        if (dynamic_cast<ScrollEvent const *>(&event) || (button && button->button == 2) ||
            (motion && (motion->modifiers & GDK_BUTTON2_MASK))) {
            return ToolBase::root_handler(event);
        }
        return true;
    }
    if (event.type() == EventType::BUTTON_PRESS || event.type() == EventType::KEY_PRESS) {
        clear_result_label(); // L5 ends on the next click or key press
    }
    if (pointer_gesture_active()) {
        // A Select gesture (drag, rubber band, handle) owns every event until
        // it ends, even when the nesting modifier is pressed part-way.
        _hover_outline.reset();
        _pointed.reset();
        _pointing_valid = false;
        return SelectTool::root_handler(event);
    }
    bool handled = false;
    std::optional<unsigned> modifiers_changed;
    auto const pointing_key = [&](unsigned keyval, unsigned modifiers) {
        return is_target_modifier_key(keyval) || (is_shift_key(keyval) && target_modifier(modifiers));
    };
    inspect_event(
        event,
        [&](KeyPressEvent const &key) {
            if (pointing_key(key.keyval, key.modifiersAfter())) {
                modifiers_changed = key.modifiersAfter();
            } else if (is_enter_key(key.keyval)) {
                nest_last_sheet(mod_shift(key.modifiers) ? NestMode::Renest : NestMode::Add);
                handled = true;
            }
        },
        [&](KeyReleaseEvent const &key) {
            if (pointing_key(key.keyval, key.modifiers)) {
                modifiers_changed = key.modifiersAfter();
            }
        },
        [&](MotionEvent const &motion) {
            if (target_modifier(motion.modifiers)) {
                update_pointing(motion.modifiers,
                                top_level_item(sp_event_context_find_item(_desktop, motion.pos, false, false)), true);
                handled = true; // no rubber-band or hover-select while pointing at a sheet
            } else if (_pointing != Pointing::Parts) {
                end_pointing();
            }
        },
        [&](ButtonPressEvent const &button) {
            if (button.button == 1 && target_modifier(button.modifiers)) {
                // Empty canvas: nothing to nest into.
                message_context->flash(WARNING_MESSAGE, _("Point at a closed shape to use as the sheet."));
                _consumed_press = true;
                handled = true;
            }
        },
        [&](ButtonReleaseEvent const &button) {
            // Only the release of a press this tool consumed.
            if (button.button == 1 && _consumed_press) {
                _consumed_press = false;
                handled = true;
            }
        },
        [&](CanvasEvent const &) {});
    auto const result = handled || SelectTool::root_handler(event);
    // After the Select tool, so the pointing cursor wins; no motion is needed.
    if (modifiers_changed) {
        update_pointing(*modifiers_changed, item_under_pointer());
    }
    return result;
}

bool NestingTool::item_handler(SPItem *item, CanvasEvent const &event)
{
    if (_solving) {
        return true;
    }
    if (event.type() == EventType::BUTTON_PRESS) {
        clear_result_label();
    }
    if (pointer_gesture_active()) {
        return SelectTool::item_handler(item, event);
    }
    if (auto button = dynamic_cast<ButtonPressEvent const *>(&event);
        button && button->button == 1 && target_modifier(button->modifiers)) {
        _consumed_press = true;
        if (button->num_press == 1) {
            // The object under the pointer, never a group member.
            nest(top_level_item(item), mod_shift(button->modifiers) ? NestMode::Renest : NestMode::Add);
        }
        return true;
    }
    if (auto release = dynamic_cast<ButtonReleaseEvent const *>(&event);
        release && release->button == 1 && _consumed_press) {
        _consumed_press = false;
        return true;
    }
    if (auto motion = dynamic_cast<MotionEvent const *>(&event); motion && target_modifier(motion->modifiers)) {
        update_pointing(motion->modifiers, top_level_item(item), true);
        return true;
    }
    return SelectTool::item_handler(item, event);
}

} // namespace Inkscape::UI::Tools
