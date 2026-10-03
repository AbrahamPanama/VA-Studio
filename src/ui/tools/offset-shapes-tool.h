// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_UI_TOOLS_OFFSET_SHAPES_TOOL_H
#define INKSCAPE_UI_TOOLS_OFFSET_SHAPES_TOOL_H

#include <cstdint>
#include <optional>
#include <vector>

#include "display/control/canvas-item-ptr.h"
#include "path/offset-shapes.h"
#include "ui/tools/tool-base.h"

namespace Inkscape { class CanvasItemBpath; }

namespace Inkscape::UI::Tools {

/** Interactive, non-mutating preview and one-shot commit for ordinary SVG offsets. */
class OffsetShapesTool final : public ToolBase
{
public:
    explicit OffsetShapesTool(SPDesktop *desktop);
    ~OffsetShapesTool() override;

    bool is_ready() const override;
    bool root_handler(CanvasEvent const &event) override;
    void switching_away(std::string const &new_tool) override;

    void set_options(OffsetShapes::Options const &options);
    [[nodiscard]] OffsetShapes::Options const &options() const noexcept { return _options; }
    [[nodiscard]] bool has_preview() const noexcept { return !_preview_items.empty(); }
    sigc::signal<void(bool)> &signal_preview_changed() noexcept { return _preview_changed; }
    bool apply();
    void cancel(Glib::ustring const &reason = {});

private:
    OffsetShapes::Options read_options() const;
    void rebuild_preview();
    void clear_preview();
    void selection_changed();
    void return_to_selector();
    void report_error(std::string const &message);

    OffsetShapes::Preparation _preparation;
    OffsetShapes::Build _build;
    OffsetShapes::Options _options;
    std::vector<SPWeakPtr<SPItem>> _original_selection;
    std::vector<CanvasItemPtr<CanvasItemBpath>> _preview_items;
    sigc::connection _preview_timeout;
    sigc::signal<void(bool)> _preview_changed;
    sigc::scoped_connection _selection_changed;
    sigc::scoped_connection _selection_modified;
    bool _leaving = false;
    bool _committing = false;
    bool _preview_dirty = false;
    std::uint64_t _generation = 0;
};

} // namespace Inkscape::UI::Tools

#endif // INKSCAPE_UI_TOOLS_OFFSET_SHAPES_TOOL_H
