// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_UI_TOOLS_BITMAP_ERASER_TOOL_H
#define INKSCAPE_UI_TOOLS_BITMAP_ERASER_TOOL_H

#include <memory>
#include <optional>
#include <vector>
#include <sigc++/scoped_connection.h>

#include "display/control/canvas-item-ptr.h"
#include "ui/tools/bitmap-eraser.h"
#include "ui/tools/tool-base.h"

class SPImage;

namespace Inkscape {
class CanvasItemBpath;
struct ExtendedInput;
struct KeyPressEvent;
} // namespace Inkscape

namespace Inkscape::UI::Tools {

/**
 * Direct destructive bitmap eraser.
 *
 * Unlike the vector EraserTool, this tool samples the pointer directly and
 * applies raster brush stamps. The document is changed only when a complete
 * stroke commits; cancellation and live preview remain view-local.
 */
class BitmapEraserTool final : public ToolBase
{
public:
    explicit BitmapEraserTool(SPDesktop *desktop);
    ~BitmapEraserTool() override;

    void set(Preferences::Entry const &value) override;
    bool root_handler(CanvasEvent const &event) override;

    double size() const { return _size; }
    double hardness() const { return _hardness; }
    DrawingImageEraseShape shape() const { return _shape; }
    bool usePressure() const { return _use_pressure; }
    bool isDrawing() const { return _drawing; }

private:
    std::vector<SPImage *> _findTargets(Geom::Point const &world_point) const;
    double _readPressure(ExtendedInput const &input) const;
    double _effectiveDiameter(double pressure) const;
    BitmapBrushStamp _makeStamp(Geom::Point const &desktop_point, double pressure) const;
    void _appendSample(Geom::Point const &desktop_point, double pressure, std::vector<BitmapBrushStamp> &output);
    void _appendFinalSample(Geom::Point const &desktop_point, std::vector<BitmapBrushStamp> &output);
    void _publish(std::vector<BitmapBrushStamp> const &stamps);
    void _cancelStroke() noexcept;
    bool _handleKeyPress(KeyPressEvent const &event);
    void _updateCursor(Geom::Point const &desktop_point, double pressure = 1.0);
    void _setCursorVisible(bool visible);
    void _changeSize(double delta);

    std::unique_ptr<BitmapEraseSession> _session;
    CanvasItemPtr<CanvasItemBpath> _tip_cursor;
    sigc::scoped_connection _selection_changed;

    std::optional<Geom::Point> _last_input;
    std::optional<Geom::Point> _last_stamp;
    std::optional<Geom::Point> _last_pointer;
    double _last_pressure = 1.0;
    double _distance_since_stamp = 0.0;

    double _size = 40.0;
    double _hardness = 1.0;
    double _spacing = 0.18;
    DrawingImageEraseShape _shape = DrawingImageEraseShape::Round;
    bool _use_pressure = false;
    bool _drawing = false;
};

} // namespace Inkscape::UI::Tools

#endif // INKSCAPE_UI_TOOLS_BITMAP_ERASER_TOOL_H
