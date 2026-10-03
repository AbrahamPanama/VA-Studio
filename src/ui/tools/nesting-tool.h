// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_UI_TOOLS_NESTING_TOOL_H
#define INKSCAPE_UI_TOOLS_NESTING_TOOL_H

#include <array>
#include <memory>
#include <optional>
#include <span>
#include <thread>
#include <vector>
#include <glibmm/ustring.h>
#include <2geom/affine.h>
#include <2geom/rect.h>

#include "async/channel.h"
#include "display/control/canvas-item-ptr.h"
#include "nesting/nesting-document.h"
#include "object/weakptr.h"
#include "ui/tools/nesting-preview.h"
#include "ui/tools/select-tool.h"

class SPItem;

namespace Inkscape { class CanvasItemBpath; class CanvasItemText; }

namespace Inkscape::UI::Tools {

/// Add places the selection around what is already on the sheet (M13);
/// Renest nests the sheet's tracked parts together with the selection.
enum class NestMode
{
    Add,
    Renest,
};

/// What the tool remembers about the last sheet until it is switched away.
struct SheetMemory
{
    SPWeakPtr<SPItem> sheet;
    std::vector<SPWeakPtr<SPItem>> tracked; // placed on sheet by this tool
    std::vector<SPWeakPtr<SPItem>> staged;  // leftovers moved beside sheet
};

/// One run (add-to-sheet work order 7.1).
struct RunContext
{
    NestMode mode = NestMode::Add;
    SPWeakPtr<SPItem> sheet;
    std::vector<SPWeakPtr<SPItem>> parts;
    std::size_t tracked_count = 0;    // parts that came from tracking
    std::size_t obstacle_count = 0;
    std::size_t background_count = 0;
};

/**
 * Persistent nesting tool built on the Select tool (tool work order 4).
 *
 * Parts are picked with the Select tool's own behaviour. The platform's primary
 * modifier (Command on macOS, Control elsewhere) plus a click picks the sheet
 * and nests the current selection into it; Enter nests into the last sheet.
 * Parts are captured at that moment. Geometry is frozen before the worker
 * starts; the worker only consumes copied polygons; document validation and
 * the single Undoable commit happen back on the GTK thread. The tool stays
 * active after a run and the selection becomes the leftovers.
 */
class NestingTool final : public SelectTool
{
public:
    /// Canvas labels (add-to-sheet work order 8.2), highest priority first:
    /// when two would overlap, the later one is hidden.
    enum class Label
    {
        Hover,    // L2 action / L3 reason
        Progress, // L4
        Hint,     // L7 empty-sheet hint
        Sheet,    // L1 sheet summary
        Leftover, // L6 leftover tag
        Result,   // L5 result badge
        Count,
    };

    explicit NestingTool(SPDesktop *desktop);
    ~NestingTool() override;

    bool root_handler(CanvasEvent const &event) override;
    bool item_handler(SPItem *item, CanvasEvent const &event) override;
    void switching_away(std::string const &new_tool) override;

    /// Nest the current selection into sheet (Add). Returns false if nothing started.
    bool nest_into(SPItem *sheet);
    /// Nest into sheet in the given mode. Returns false if nothing started.
    bool nest(SPItem *sheet, NestMode mode);
    /// Nest into the last sheet (Enter, and the controls bar's Nest button).
    bool nest_last_sheet(NestMode mode);
    /// Emitted when a run starts or ends and when the last sheet changes.
    sigc::connection connect_state_changed(sigc::slot<void()> slot) { return _state_changed.connect(std::move(slot)); }

    [[nodiscard]] bool is_solving() const noexcept { return _solving; }
    [[nodiscard]] SPItem *last_sheet() const { return _memory.sheet.get(); }
    [[nodiscard]] SheetMemory const &memory() const noexcept { return _memory; }
    /// Outlines currently shown (test and accessibility queries).
    [[nodiscard]] bool has_sheet_outline() const noexcept { return static_cast<bool>(_sheet_outline); }
    [[nodiscard]] bool has_hover_outline() const noexcept { return static_cast<bool>(_hover_outline); }
    /// Measurement accessors (R0/R5a): the longest single preview rebuild of the
    /// current run, and the latest progress report.
    [[nodiscard]] double longest_preview_update_seconds() const noexcept { return _longest_preview_update_seconds; }
    [[nodiscard]] std::optional<Nesting::Progress> const &last_progress() const noexcept { return _last_progress; }
    /// Label and outline state (tests and accessibility queries).
    [[nodiscard]] bool label_visible(Label label) const;
    [[nodiscard]] Glib::ustring label_text(Label label) const;
    [[nodiscard]] std::optional<Geom::Rect> label_screen_rect(Label label) const;
    [[nodiscard]] std::size_t keepout_outline_count() const noexcept { return _keepout_outlines.size(); }
    [[nodiscard]] std::size_t move_outline_count() const noexcept { return _move_outlines.size(); }
    [[nodiscard]] bool has_leftover_outline() const noexcept { return static_cast<bool>(_leftover_outline); }
    [[nodiscard]] bool has_margin_outline() const noexcept { return static_cast<bool>(_margin_outline); }
    [[nodiscard]] std::size_t obstacle_cache_builds() const noexcept { return _obstacle_cache_builds; }
    /// Union of the visible preview outlines, in document and in desktop
    /// coordinates (the latter is what the canvas items were given; C4).
    [[nodiscard]] Geom::OptRect preview_document_bounds() const;
    [[nodiscard]] Geom::OptRect preview_desktop_bounds() const;

private:
    enum class Pointing
    {
        Parts,
        Sheet,
        Invalid,
        Nothing,
    };

    Nesting::Options read_options() const;
    SPItem *top_level_item(SPItem *item) const;
    SPItem *item_under_pointer() const;
    std::optional<Glib::ustring> sheet_problem(SPItem *item) const;
    /// may_reuse: a motion over the same object keeps the last state.
    void update_pointing(unsigned modifiers, SPItem *under_pointer, bool may_reuse = false);
    void apply_pointing_feedback();
    void end_pointing();
    void refresh_sheet_outline();
    // Labels and outlines (add-to-sheet work order 8).
    struct LabelState
    {
        CanvasItemPtr<CanvasItemText> item;
        Geom::Point desktop_point;
        Geom::Point anchor;
        Geom::Point adjust;
        Glib::ustring text;
    };
    [[nodiscard]] bool labels_enabled() const;
    void set_label(Label label, Geom::Point const &desktop_point, Glib::ustring const &text, std::uint32_t background,
                   std::uint32_t foreground, Geom::Point const &anchor, Geom::Point const &adjust);
    void clear_label(Label label);
    void clear_result_label();
    void arrange_labels();
    [[nodiscard]] std::optional<Geom::Rect> screen_rect(LabelState const &state) const;
    CanvasItemPtr<CanvasItemBpath> make_outline(Geom::PathVector const &desktop_path, std::uint32_t stroke, double width,
                                                std::vector<double> dashes, std::uint32_t fill = 0);
    void clear_hover_items();
    void show_hover_for_sheet(SPItem *sheet, bool renest);
    void refresh_sheet_items();
    void show_run_result(Nesting::ApplyResult const &applied, std::vector<SPItem *> const &unplaced);
    std::vector<SPItem *> obstacles_for(SPItem *sheet, bool renest);
    void show_idle_message();
    void selection_changed();
    void selection_modified();
    void bind_document(SPDocument *document);
    void update_progress(Nesting::Progress progress);
    void update_preview(std::span<Nesting::Placement const> placements);
    void clear_preview();
    void preparation_progress(std::size_t done, std::size_t total);
    void geometry_ready(Nesting::CapturedGeometry geometry);
    void abort_run(std::string const &error);
    void finish_solve(Nesting::SolveResult result);
    void end_run();
    void cancel(Glib::ustring const &reason = {});
    void report_error(std::string const &message);

    SheetMemory _memory;
    std::optional<RunContext> _run;
    double _run_leftover_gap = 0.0;
    /// R1: the capture (GTK-thread identities) while the worker prepares the
    /// geometry; replaced by the assembled snapshot when the geometry arrives.
    std::optional<Nesting::CaptureResult> _capture;
    std::optional<Nesting::PreparedDocumentNesting> _snapshot;
    std::optional<Nesting::Progress> _last_progress;
    std::optional<NestingPreviewModel> _preview_model;
    Geom::Affine _preview_doc2dt; // the mapping the preview items were built with
    /// One item per snapshot part, created on first need and reused (R3).
    std::vector<CanvasItemPtr<CanvasItemBpath>> _preview_items;
    std::vector<Geom::OptRect> _preview_document_bounds;
    std::vector<Geom::OptRect> _preview_desktop_bounds;
    CanvasItemPtr<CanvasItemBpath> _sheet_outline;    // O1, persistent, dashed
    CanvasItemPtr<CanvasItemBpath> _hover_outline;    // O3/O4, while the modifier is held
    CanvasItemPtr<CanvasItemBpath> _margin_outline;   // O2
    CanvasItemPtr<CanvasItemBpath> _leftover_outline; // O7
    std::vector<CanvasItemPtr<CanvasItemBpath>> _keepout_outlines; // O5
    std::vector<CanvasItemPtr<CanvasItemBpath>> _move_outlines;    // O6
    std::array<LabelState, static_cast<std::size_t>(Label::Count)> _labels;
    sigc::scoped_connection _result_timeout; // 6 s L5 lifetime
    std::unique_ptr<Preferences::PreferencesObserver> _labels_observer;
    /// Keep-out outlines are built once per sheet and mode (O5 performance
    /// rule) and dropped on any document or selection change.
    std::optional<std::vector<SPWeakPtr<SPItem>>> _obstacle_cache;
    SPWeakPtr<SPItem> _obstacle_cache_sheet;
    sigc::scoped_connection _refresh_idle; // coalesced sheet-item refresh after edits
    sigc::scoped_connection _zoom_changed; // labels re-arranged on zoom/rotate
    bool _obstacle_cache_renest = false;
    std::size_t _obstacle_cache_builds = 0;
    double _sheet_used_fraction = 0.0; // L1, from the last run
    Pointing _pointing = Pointing::Parts;
    SPWeakPtr<SPItem> _pointed;   // object the current pointing state describes
    bool _pointing_valid = false; // _pointing/_pointed/message still describe the document
    Glib::ustring _pointing_message;
    std::string _pointing_cursor;
    bool _consumed_press = false; // a sheet-picking press this tool consumed
    Async::Channel::Dest _completion_dest;
    std::jthread _worker;
    sigc::scoped_connection _selection_changed;
    sigc::scoped_connection _selection_modified;
    sigc::scoped_connection _document_modified;
    sigc::scoped_connection _document_replaced;
    sigc::signal<void()> _state_changed;
    bool _solving = false;
    bool _leaving = false;
    bool _setting_selection = false; // suppress the tool's own selection signals
    double _longest_preview_update_seconds = 0.0;
};

} // namespace Inkscape::UI::Tools

#endif // INKSCAPE_UI_TOOLS_NESTING_TOOL_H
