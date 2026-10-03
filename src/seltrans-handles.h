// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef SEEN_SP_SELTRANS_HANDLES_H
#define SEEN_SP_SELTRANS_HANDLES_H

/*
 * Seltrans knots
 *
 * Authors:
 *   Lauris Kaplinski <lauris@kaplinski.com>
 *
 * Copyright (C) 1999-2002 authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include <vector>
#include <cmath>

#include <2geom/point.h>

#include <glibmm/ustring.h>

#include "enums.h"  // SPAnchorType

typedef unsigned int guint32;

namespace Inkscape {
    class SelTrans;
}

enum SPSelTransType {
    HANDLE_STRETCH,
    HANDLE_SCALE,
    HANDLE_SKEW,
    HANDLE_ROTATE,
    HANDLE_CENTER,
    HANDLE_SIDE_ALIGN,
    HANDLE_CORNER_ALIGN,
    HANDLE_CENTER_ALIGN
};

// Offset for moving from Left click to Shift Click
const int ALIGN_SHIFT_OFFSET = 9;

// Offset for handle number (handles used by alignment don't start at zero, see seltrans-handles.cpp).
const int ALIGN_OFFSET = -13;

// Which handle does what in the alignment (clicking)
// clang-format off
const std::vector<Glib::ustring> AlignArguments = {
    // Left Click
    "selection top",
    "selection right",
    "selection bottom",
    "selection left",
    "selection vcenter",
    "selection top left",
    "selection top right",
    "selection bottom right",
    "selection bottom left",

    // Shift click
    "selection anchor bottom",
    "selection anchor left",
    "selection anchor top",
    "selection anchor right",
    "selection hcenter",
    "selection anchor bottom right",
    "selection anchor bottom left",
    "selection anchor top left",
    "selection anchor top right"
};
// clang-format on

struct SPSelTransHandle;

struct SPSelTransHandle {
    SPSelTransType type;
    SPAnchorType anchor;
    unsigned int control;
    gdouble x, y;
};

// Handle-table coordinates are screen-relative before the desktop Y mapping:
// y=0 is the lower row. Keep midpoint skew and alignment handles unchanged.
constexpr bool selection_handle_uses_steps(SPSelTransType type, double x, double y)
{
    return ((type == HANDLE_SCALE || type == HANDLE_ROTATE) && y == 0.0) ||
           (type == HANDLE_STRETCH && (y == 0.0 || x == 1.0));
}
// Choose directions in window coordinates; degenerate bounds retain table directions.
inline char const *selection_handle_cursor(SPSelTransType type, Geom::Point screen_dir, double x, double y)
{
    auto const dx = screen_dir.x();
    auto const dy = screen_dir.y();
    auto const degenerate = std::abs(dx) < 1e-9 && std::abs(dy) < 1e-9;
    switch (type) {
        case HANDLE_SCALE:
            return (degenerate || std::abs(dx * dy) < 1e-9 ? x == 1.0 - y : dx * dy > 0)
                ? "select-scale-nwse.svg" : "select-scale-nesw.svg";
        case HANDLE_STRETCH:
            return (degenerate ? x == 0.5 : std::abs(dx) < std::abs(dy))
                ? "select-stretch-vertical.svg" : "select-stretch-horizontal.svg";
        case HANDLE_ROTATE:
            return "rotate.svg";
        case HANDLE_SKEW:
            return (degenerate ? x == 0.5 : std::abs(dy) > std::abs(dx))
                ? "select-skew-horizontal.svg" : "select-skew-vertical.svg";
        case HANDLE_CENTER:
            return "select-pivot.svg";
        default:
            return "select.svg";
    }
}
constexpr guint32 SELECTION_STEP_HANDLE_COLOR = 0x169447ff;
// These are 4 * each handle type + 1 for center
int const NUMHANDS = 26;
extern SPSelTransHandle const hands[NUMHANDS];

#endif // SEEN_SP_SELTRANS_HANDLES_H

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
