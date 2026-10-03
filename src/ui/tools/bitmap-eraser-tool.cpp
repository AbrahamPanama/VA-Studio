// SPDX-License-Identifier: GPL-2.0-or-later

#include "bitmap-eraser-tool.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>
#include <glibmm/i18n.h>
#include <2geom/circle.h>

#include "desktop.h"
#include "display/control/canvas-item-bpath.h"
#include "document-undo.h"
#include "document.h"
#include "message-context.h"
#include "object/sp-image.h"
#include "object/sp-use.h"
#include "selection.h"
#include "ui/icon-names.h"
#include "ui/widget/events/canvas-event.h"

using Inkscape::DocumentUndo;

namespace Inkscape::UI::Tools {
namespace {

bool usable_bitmap(SPDesktop const *desktop, SPImage const *image)
{
    return desktop && image && image->pixbuf && !image->missing && image->getRepr() &&
           image->isVisibleAndUnlocked(desktop->dkey) && image->get_arenaitem(desktop->dkey);
}

void collect_bitmap_descendants(SPDesktop const *desktop, SPObject *object, std::vector<SPImage *> &result,
                                std::unordered_set<SPImage *> &seen)
{
    if (!object || is<SPUse>(object))
        return;
    if (auto *image = cast<SPImage>(object)) {
        if (usable_bitmap(desktop, image) && seen.insert(image).second)
            result.push_back(image);
        return;
    }
    for (auto &child : object->children) {
        collect_bitmap_descendants(desktop, &child, result, seen);
    }
}

bool just_control(unsigned modifiers)
{
    return (modifiers & GDK_CONTROL_MASK) && !(modifiers & (GDK_ALT_MASK | GDK_SHIFT_MASK));
}

} // namespace

BitmapEraserTool::BitmapEraserTool(SPDesktop *desktop)
    : ToolBase(desktop, "/tools/bitmaperaser", "eraser.svg", false)
{
    auto *prefs = Preferences::get();
    _size = std::clamp(prefs->getDouble("/tools/bitmaperaser/size", 40.0), 1.0, 2000.0);
    _hardness = std::clamp(prefs->getDouble("/tools/bitmaperaser/hardness", 100.0) / 100.0, 0.0, 1.0);
    _spacing = std::clamp(prefs->getDouble("/tools/bitmaperaser/spacing", 0.18), 0.05, 0.50);
    _shape = prefs->getInt("/tools/bitmaperaser/shape", 0) == 1 ? DrawingImageEraseShape::Square
                                                                : DrawingImageEraseShape::Round;
    _use_pressure = prefs->getBool("/tools/bitmaperaser/usepressure", false);

    _tip_cursor = make_canvasitem<CanvasItemBpath>(desktop->getCanvasControls());
    _tip_cursor->set_fill(0x0, SP_WIND_RULE_EVENODD);
    _tip_cursor->set_stroke(0xffffffff);
    _tip_cursor->set_stroke_width(1.0);
    _tip_cursor->set_outline(0x000000ff);
    _tip_cursor->set_outline_width(1.0);
    _tip_cursor->set_pickable(false);
    _tip_cursor->set_visible(false);

    if (auto *selection = desktop->getSelection()) {
        _selection_changed = selection->connectChanged([this](auto *) {
            if (_drawing)
                _cancelStroke();
        });
    }
    enableSelectionCue();
}

BitmapEraserTool::~BitmapEraserTool()
{
    _cancelStroke();
}

void BitmapEraserTool::set(Preferences::Entry const &value)
{
    auto const name = value.getEntryName();
    if (name == "size") {
        _size = std::clamp(value.getDouble(40.0), 1.0, 2000.0);
    } else if (name == "hardness") {
        _hardness = std::clamp(value.getDouble(100.0) / 100.0, 0.0, 1.0);
    } else if (name == "spacing") {
        _spacing = std::clamp(value.getDouble(0.18), 0.05, 0.50);
    } else if (name == "shape") {
        _shape = value.getInt(0) == 1 ? DrawingImageEraseShape::Square : DrawingImageEraseShape::Round;
    } else if (name == "usepressure") {
        _use_pressure = value.getBool(false);
    } else {
        ToolBase::set(value);
    }

    if (_last_pointer)
        _updateCursor(*_last_pointer, _last_pressure);
}

std::vector<SPImage *> BitmapEraserTool::_findTargets(Geom::Point const &world_point) const
{
    std::vector<SPImage *> result;
    std::unordered_set<SPImage *> seen;
    auto *selection = _desktop ? _desktop->getSelection() : nullptr;
    if (!_desktop || !selection)
        return result;

    if (!selection->isEmpty()) {
        for (auto *item : selection->items()) {
            collect_bitmap_descendants(_desktop, item, result, seen);
        }
        return result;
    }

    SPItem *below = nullptr;
    std::unordered_set<SPItem *> visited;
    while (auto *item = _desktop->getItemAtPoint(world_point, true, below)) {
        if (!visited.insert(item).second)
            break;
        below = item;
        if (auto *image = cast<SPImage>(item); usable_bitmap(_desktop, image)) {
            result.push_back(image);
            break;
        }
    }
    return result;
}

double BitmapEraserTool::_readPressure(ExtendedInput const &input) const
{
    if (!_use_pressure || !input.pressure)
        return 1.0;
    // A tiny floor prevents event-to-event gaps when tablet drivers report a
    // transient zero immediately after contact.
    return std::clamp(*input.pressure, 0.05, 1.0);
}

double BitmapEraserTool::_effectiveDiameter(double pressure) const
{
    return std::max(0.25, _size * (_use_pressure ? std::clamp(pressure, 0.05, 1.0) : 1.0));
}

BitmapBrushStamp BitmapEraserTool::_makeStamp(Geom::Point const &desktop_point, double pressure) const
{
    return {.center_desktop = desktop_point,
            .diameter = _effectiveDiameter(pressure),
            .hardness = _hardness,
            .opacity = 1.0,
            .shape = _shape};
}

void BitmapEraserTool::_appendSample(Geom::Point const &desktop_point, double pressure,
                                     std::vector<BitmapBrushStamp> &output)
{
    if (!_last_input) {
        output.emplace_back(_makeStamp(desktop_point, pressure));
        _last_input = desktop_point;
        _last_stamp = desktop_point;
        _last_pressure = pressure;
        _distance_since_stamp = 0.0;
        return;
    }

    auto const start = *_last_input;
    auto const delta = desktop_point - start;
    auto const length = Geom::L2(delta);
    if (!(length > 1e-9) || !std::isfinite(length)) {
        _last_input = desktop_point;
        _last_pressure = pressure;
        return;
    }

    auto const previous_pressure = _last_pressure;
    auto const minimum_diameter = std::min(_effectiveDiameter(previous_pressure), _effectiveDiameter(pressure));
    auto const step = std::max(0.5, minimum_diameter * _spacing);
    double traveled = 0.0;

    while (_distance_since_stamp + (length - traveled) >= step) {
        auto const advance = step - _distance_since_stamp;
        traveled += advance;
        auto const t = std::clamp(traveled / length, 0.0, 1.0);
        auto const point = start + delta * t;
        auto const interpolated_pressure = std::lerp(previous_pressure, pressure, t);
        output.emplace_back(_makeStamp(point, interpolated_pressure));
        _last_stamp = point;
        _distance_since_stamp = 0.0;
    }

    _distance_since_stamp += length - traveled;
    _last_input = desktop_point;
    _last_pressure = pressure;
}

void BitmapEraserTool::_appendFinalSample(Geom::Point const &desktop_point, std::vector<BitmapBrushStamp> &output)
{
    _appendSample(desktop_point, _last_pressure, output);
    if (!_last_stamp || Geom::L2(desktop_point - *_last_stamp) > 1e-6) {
        output.emplace_back(_makeStamp(desktop_point, _last_pressure));
        _last_stamp = desktop_point;
        _distance_since_stamp = 0.0;
    }
}

void BitmapEraserTool::_publish(std::vector<BitmapBrushStamp> const &stamps)
{
    if (_session && !stamps.empty())
        _session->preview(stamps);
}

void BitmapEraserTool::_cancelStroke() noexcept
{
    if (_session) {
        _session->cancel();
        _session.reset();
    }
    if (_drawing) {
        ungrabCanvasEvents();
        set_high_motion_precision(false);
    }
    _drawing = false;
    dragging = false;
    _last_input.reset();
    _last_stamp.reset();
    _distance_since_stamp = 0.0;
    if (message_context)
        message_context->clear();
}

void BitmapEraserTool::_updateCursor(Geom::Point const &desktop_point, double pressure)
{
    _last_pointer = desktop_point;
    _last_pressure = pressure;
    auto const radius = _effectiveDiameter(pressure) * 0.5;
    Geom::PathVector path;
    if (_shape == DrawingImageEraseShape::Round) {
        path.push_back(Geom::Path(Geom::Circle(desktop_point, radius)));
    } else {
        path.push_back(Geom::Path(
            Geom::Rect(desktop_point - Geom::Point(radius, radius), desktop_point + Geom::Point(radius, radius))));
    }
    _tip_cursor->set_bpath(std::move(path));
    _setCursorVisible(true);
}

void BitmapEraserTool::_setCursorVisible(bool visible)
{
    if (_tip_cursor)
        _tip_cursor->set_visible(visible);
}

void BitmapEraserTool::_changeSize(double delta)
{
    auto const value = std::clamp(_size + delta, 1.0, 2000.0);
    Preferences::get()->setDouble("/tools/bitmaperaser/size", value);
    _desktop->setToolboxAdjustmentValue("bitmap-eraser-size", value);
}

bool BitmapEraserTool::_handleKeyPress(KeyPressEvent const &event)
{
    switch (get_latin_keyval(event)) {
        case GDK_KEY_Escape:
            if (_drawing) {
                _cancelStroke();
                return true;
            }
            break;
        case GDK_KEY_z:
        case GDK_KEY_Z:
            if (_drawing && just_control(event.modifiers)) {
                _cancelStroke();
                return true;
            }
            break;
        case GDK_KEY_bracketleft:
            _changeSize(_size <= 20.0 ? -1.0 : -5.0);
            return true;
        case GDK_KEY_bracketright:
            _changeSize(_size < 20.0 ? 1.0 : 5.0);
            return true;
        default:
            break;
    }
    return false;
}

bool BitmapEraserTool::root_handler(CanvasEvent const &event)
{
    bool handled = false;

    inspect_event(
        event, [&](EnterEvent const &event) { _updateCursor(_desktop->w2d(event.pos), 1.0); },
        [&](LeaveEvent const &) {
            if (!_drawing)
                _setCursorVisible(false);
        },
        [&](ButtonPressEvent const &event) {
            if (event.num_press != 1 || event.button != 1)
                return;

            auto targets = _findTargets(event.pos);
            auto session = std::make_unique<BitmapEraseSession>(_desktop);
            if (targets.empty() || !session->begin(targets)) {
                message_context->set(Inkscape::WARNING_MESSAGE,
                                     _("Select a usable bitmap, or begin the stroke over one."));
                handled = true;
                return;
            }

            _session = std::move(session);
            _drawing = true;
            dragging = true;
            _last_input.reset();
            _last_stamp.reset();
            _distance_since_stamp = 0.0;
            auto const pressure = _readPressure(event.extinput);
            auto const point = _desktop->w2d(event.pos);
            std::vector<BitmapBrushStamp> stamps;
            _appendSample(point, pressure, stamps);
            _publish(stamps);
            _updateCursor(point, pressure);
            set_high_motion_precision();
            grabCanvasEvents();
            message_context->set(Inkscape::NORMAL_MESSAGE,
                                 _("<b>Erasing bitmap pixels</b> — release to commit, Escape to cancel"));
            handled = true;
        },
        [&](MotionEvent const &event) {
            auto const point = _desktop->w2d(event.pos);
            auto const pressure = _readPressure(event.extinput);
            _updateCursor(point, pressure);
            if (!_drawing || !(event.modifiers & GDK_BUTTON1_MASK))
                return;

            std::vector<BitmapBrushStamp> stamps;
            stamps.reserve(event.history.size() + 4);
            for (auto const &history : event.history) {
                _appendSample(_desktop->w2d(history.pos), _readPressure(history.extinput), stamps);
            }
            _appendSample(point, pressure, stamps);
            _publish(stamps);
            handled = true;
        },
        [&](ButtonReleaseEvent const &event) {
            if (event.button != 1 || !_drawing)
                return;

            auto const point = _desktop->w2d(event.pos);
            std::vector<BitmapBrushStamp> stamps;
            _appendFinalSample(point, stamps);
            _publish(stamps);

            ungrabCanvasEvents();
            set_high_motion_precision(false);
            _drawing = false;
            dragging = false;

            auto *document = _desktop->getDocument();
            if (_session && _session->commit()) {
                DocumentUndo::done(document, RC_("Undo", "Erase bitmap pixels"), INKSCAPE_ICON("draw-eraser-bitmap"));
            }
            _session.reset();
            _last_input.reset();
            _last_stamp.reset();
            _distance_since_stamp = 0.0;
            message_context->clear();
            _updateCursor(point, _last_pressure);
            handled = true;
        },
        [&](KeyPressEvent const &event) { handled = _handleKeyPress(event); }, [&](CanvasEvent const &) {});

    return handled || ToolBase::root_handler(event);
}

} // namespace Inkscape::UI::Tools
