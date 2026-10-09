// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_UI_TOOLS_SELECT_TOOl_H
#define INKSCAPE_UI_TOOLS_SELECT_TOOl_H

/*
 * Select tool
 *
 * Authors:
 *   Lauris Kaplinski <lauris@kaplinski.com>
 *
 * Copyright (C) 1999-2002 authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include <cstdint>

#include "rubberband.h"
#include "ui/tools/selector-interaction.h"
#include "ui/tools/tool-base.h"
#include "ui/widget/events/canvas-event.h"

namespace Inkscape {
struct ScrollEvent;
class SelTrans;
class SelectionDescriber;
class Selection;
class DrawingItem;
} // namespace Inkscape

namespace Inkscape::UI::Tools {

class SelectTool : public ToolBase
{
public:
    SelectTool(SPDesktop *desktop);
    ~SelectTool() override;

    bool moved = false;
    unsigned button_press_state = 0;

    std::vector<SPItem *> cycling_items;
    std::vector<SPItem *> cycling_items_cmp;
    SPItem *cycling_cur_item = nullptr;
    bool cycling_wrap = true;

    SPItem *item = nullptr;
    CanvasItem *grabbed = nullptr;
    SelTrans *_seltrans = nullptr;
    SelectionDescriber *_describer = nullptr;
    char *no_selection_msg = nullptr;

    void set(Preferences::Entry const &val) override;
    bool root_handler(CanvasEvent const &event) override;
    bool item_handler(SPItem *item, CanvasEvent const &event) override;
    std::string const &cursor_filename() const { return _cursor_filename; }
    std::optional<Geom::Rect> hover_outline_rect_for_testing() const;

    void updateDescriber(Selection *sel);

protected:
    /// For tools that add behaviour on top of selection (Nesting). The
    /// preference path and default cursor are the derived tool's.
    SelectTool(SPDesktop *desktop, std::string prefs_path, std::string cursor_filename);
    /// A press, drag, rubber band or handle transform is in progress; a derived
    /// tool must pass its events on to the Select tool until it ends.
    [[nodiscard]] bool pointer_gesture_active() const;

private:

    // Hover outline of the object a plain click would select (display only, never pickable).
    CanvasItemPtr<CanvasItemRect> _hover_outline;
    sigc::scoped_connection _hover_selection_changed;
    sigc::scoped_connection _hover_item_deleted;
    Inkscape::DrawingItem *_hover_item = nullptr;
    unsigned _hover_item_key = 0;
    uint32_t _hover_tint_rgba = 0x277fff4d;
    std::optional<Geom::Rect> _hover_rect;
    void _updateHoverOutline(Geom::Point const &window_point, unsigned modifiers);
    void _clearHoverOutline();

    // aborts selection interaction
    bool sp_select_context_abort();

    void sp_select_context_cycle_through_items(Selection *selection, ScrollEvent const &scroll_event);

    // resets the opacities of all selected items to their original values
    void sp_select_context_reset_opacities();

    static std::pair<Rubberband::Mode, CanvasItemCtrlType> get_default_rubberband_state();

    void _duplicate_drag(Geom::Point const &p);
    bool _duplicate_drag_state(unsigned int state) const;
    void _duplicate_drag_reset();
    bool _duplicate_drag_on_press = false;
    bool _duplicate_down_on_selected = false;

    void _begin_pointer_interaction(Geom::Point const &screen_point);
    void _finish_pointer_interaction();
    void _update_directional_marquee(Geom::Point const &screen_point, Rubberband &rubberband);
    bool _consume_canceled_release(ButtonReleaseEvent const &event);

    SelectorInteractionState _interaction_state = SelectorInteractionState::Idle;
    MarqueeBehavior _marquee_behavior = MarqueeBehavior::Undetermined;
    Geom::Point _marquee_press_screen;
    uint64_t _interaction_generation = 0;

    void handleClick(ButtonReleaseEvent const &event, Selection *selection);

    bool _alt_on = false;
    bool _force_dragging = false;

    Geom::Point _live_point;

    std::string _default_cursor;
    void onHideSelectionChanged(bool hide) override;

    Util::ActionAccel _acc_st_grab;
    Util::ActionAccel _acc_st_scale;
    Util::ActionAccel _acc_st_rotate;

    Modifiers::Modifier *mod_select_add_to;
    Modifiers::Modifier *mod_select_always_box;
    Modifiers::Modifier *mod_select_cycle;
    Modifiers::Modifier *mod_select_duplicate;
    Modifiers::Modifier *mod_select_force_drag;
    Modifiers::Modifier *mod_select_in_groups;
    Modifiers::Modifier *mod_select_remove_from;
    Modifiers::Modifier *mod_select_remove_snap;
    Modifiers::Modifier *mod_select_touch_path;
};

} // namespace Inkscape::UI::Tools

#endif // INKSCAPE_UI_TOOLS_SELECT_TOOl_H

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
