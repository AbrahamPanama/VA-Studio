// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef SEEN_SELTRANS_H
#define SEEN_SELTRANS_H

/*
 * Helper object for transforming selected items
 *
 * Authors:
 *   Lauris Kaplinski <lauris@kaplinski.com>
 *   Carl Hetherington <inkscape@carlh.net>
 *   Diederik van Lierop <mail@diedenrezi.nl>
 *
 * Copyright (C) 2006      Johan Engelen <johan@shouraizou.nl>
 * Copyright (C) 1999-2002 Lauris Kaplinski
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <2geom/point.h>
#include <2geom/affine.h>
#include <2geom/rect.h>
#include <cstddef>
#include <sigc++/sigc++.h>

#include "message-context.h"
#include "document-undo.h"
#include "seltrans-handles.h"
#include "selcue.h"
#include "selection-dimensional-detents.h"
#include "selection-resize-feedback.h"
#include "selection-rotation-feedback.h"

#include "object/sp-item.h"
#include "ui/knot/knot.h"

class  SPDesktop;
struct SPCanvasItem;
struct SPSelTransHandle;

namespace Inkscape {

class CanvasItemCtrl;
class CanvasItemCurve;
class CanvasItemPicture;

namespace Util {
class Unit;
}

Geom::Scale calcScaleFactors(Geom::Point const &initial_point, Geom::Point const &new_point, Geom::Point const &origin, bool const skew = false);

namespace XML {
    class Node;
}

class SelTrans
{
public:
    SelTrans(SPDesktop *desktop);
    ~SelTrans();

    enum State {
        STATE_SCALE, //scale or stretch
        STATE_ROTATE, //rotate or skew
        STATE_ALIGN  //on canvas align
    };

    void increaseState();
    void resetState(State state = STATE_SCALE);
    void setCenter(Geom::Point const &p);
    bool beginInteraction();
    void grab(Geom::Point const &p, double x, double y, bool show_handles, bool translating,
              std::optional<SPSelTransType> resize_type = std::nullopt);
    void transform(Geom::Affine const &rel_affine, Geom::Point const &norm);
    void ungrab();
    bool cancel() noexcept;
    void stamp(bool clone = false);
    bool moveTo(Geom::Point const &xy, unsigned int state);
    void commitAbsoluteAffine();
    void commitRelativeAffine();
    void align(guint state, SPSelTransHandle const &handle);
    int request(SPSelTransHandle const &handle, Geom::Point &pt, unsigned int state);
    int scaleRequest(Geom::Point &pt, unsigned int state);
    int stretchRequest(Geom::Point &pt, unsigned int state, bool is_horz);
    int skewRequest(Geom::Point &pt, unsigned int state, bool is_horz);
    int rotateRequest(Geom::Point &pt, unsigned int state);
    int centerRequest(Geom::Point &pt, unsigned int state);
    int originRequest(Geom::Point &pt, unsigned int state);

    // StKey transforms functionality
    enum class StickyTransform
    {
        None,
        Grab,
        Scale,
        Rotate
    };
private:
    StickyTransform _stkey = StickyTransform::None;
    bool _stkey_transformed = false;
    bool _stkey_paused = false;

public:
    void grab_stkey(Geom::Point const &p, StickyTransform type);
    void pause_stkey() { _stkey_paused = true; }
    bool is_stkey() const { return _stkey != StickyTransform::None; }
    bool request_stkey(Geom::Point const &p, int state);
    bool ungrab_stkey(bool cancel = false);

    int handleRequest(SPKnot *knot, Geom::Point *position, unsigned int state, SPSelTransHandle const &handle);
    void handleGrab(SPKnot *knot, unsigned int state, SPSelTransHandle const &handle);
    void handleClick(SPKnot *knot, unsigned int state, SPSelTransHandle const &handle);
    void handleNewEvent(Geom::Point *position, unsigned int state, SPSelTransHandle const &handle);

    enum Show
    {
        SHOW_CONTENT,
        SHOW_OUTLINE
    };

    void setShow(Show s) {
        _show = s;
    }
    bool isEmpty() {
        return _empty;
    }
    bool isGrabbed() {
        return _grabbed;
    }
    /// True while a move shows a picture of the objects instead of moving
    /// them on every motion (Selector preference "Fast preview when moving").
    bool movesPicture() const { return _picture_move; }
    CanvasItemPicture const *movePicture() const { return _picture.get(); }
    bool centerIsVisible() {
        return ( knots[0]->is_visible());
    }
    SPKnot *handleKnot(unsigned index) const { return index < NUMHANDS ? knots[index] : nullptr; }

    void getNextClosestPoint(bool reverse);
    SelCue &getSelCue() { return _selcue; }

    // Read-only resize feedback state, used by focused interaction tests.
    bool hasResizeDimensionOverlay() const noexcept;
    std::optional<SelectionDimensionLabels> resizeDimensionLabels() const;
    std::optional<Geom::Rect> resizeDimensionBounds() const;
    std::size_t resizeDimensionCanvasItemCount() const noexcept;
    std::array<bool, 2> resizeDimensionAccents() const noexcept;

    // Gesture-local rotation feedback, never document state.
    std::optional<double> rotationAngleDegrees() const noexcept { return _rotation_degrees; }
    SelectionRotationOverlay const *rotationOverlay() const noexcept { return _rotation_overlay.get(); }

private:
    class BoundingBoxPrefsObserver: public Preferences::Observer
    {
    public:
        BoundingBoxPrefsObserver(SelTrans &sel_trans);

        void notify(Preferences::Entry const &val) override;

    private:
        SelTrans &_sel_trans;
    };

    friend class Inkscape::SelTrans::BoundingBoxPrefsObserver;
    void _clear_stamp();
    void _updateHandles();
    void _updateVolatileState();
    void _selChanged(Inkscape::Selection *selection);
    void _selModified(Inkscape::Selection *selection, unsigned int flags);
    void _boundingBoxPrefsChanged(int prefs_bbox);
    void _makeHandles();
    void _showHandles(SPSelTransType type);
    void _updateHandleCursor(int i);
    Geom::Point _getGeomHandlePos(Geom::Point const &visual_handle_pos);
    Geom::Point _calcAbsAffineDefault(Geom::Scale const default_scale);
    Geom::Point _calcAbsAffineGeom(Geom::Scale const geom_scale);
    void _keepClosestPointOnly(Geom::Point const &p);
    void _captureSelectionBaseline();
    void _clearSelectionBaseline();
    void _restoreSelectionBaseline();
    void _clearGrabbedItems();
    void _finishInteraction(Util::Internal::ContextString label);
    std::optional<Geom::Rect> _resizeSourceBounds() const;
    std::optional<Geom::Rect> _currentResizeBounds() const;
    void _setResizeTargetBounds(Geom::Scale const &scale);
    void _ensureResizeDimensionOverlay();
    void _updateResizeDimensionOverlay();
    int _finishResizeRequest();
    void _clearResizeDimensionOverlay() noexcept;
    void _clearRotationFeedback() noexcept;
    struct PendingCancel;
    enum class CancelState { None, Pending, Settling, Closed };
    bool _cancelPending() const noexcept { return _cancel_state != CancelState::None; }
    void _queueCancel(bool immediate_if_quiescent = false);
    // Recorded when WE apply preview, not when a later external commit cancels it.
    std::vector<Geom::Affine> _items_preview_affines;
    std::vector<Geom::Point> _items_baseline_center_offsets, _items_preview_center_offsets;
    std::vector<std::string> _items_preview_repr;
    void _resetResizeDimensionalDetents() noexcept;
    bool _applyResizeDimensionalDetents(Geom::Point &pt, Geom::Scale &scale,
                                        unsigned int state, bool aspect_locked,
                                        std::optional<Geom::Dim2> single_axis,
                                        std::optional<Geom::Dim2> aspect_control_axis,
                                        std::array<bool, 2> const &higher_priority_snap);

    SPDesktop *_desktop;

    std::vector<SPItem *> _items;
    std::vector<SPObject const *> _objects_const;
    std::vector<Geom::Affine> _items_affines;
    std::vector<Geom::Point> _items_centers;
    std::vector<bool> _items_centers_set;
    std::vector<std::string> _selection_before_interaction;
    std::optional<DocumentUndo::RollbackableInteraction> _interaction;
    SPDocument *_interaction_document = nullptr;

    std::vector<Inkscape::SnapCandidatePoint> _snap_points;
    std::vector<Inkscape::SnapCandidatePoint> _bbox_points;
    std::vector<Inkscape::SnapCandidatePoint> _all_snap_sources_sorted;
    std::vector<Inkscape::SnapCandidatePoint>::iterator _all_snap_sources_iter;
    Inkscape::SelCue _selcue;

    Inkscape::Selection *_selection;
    State _state;
    Show _show;

    bool _grabbed = false;
    bool _canceling = false;
    bool _show_handles = true;
    bool _empty;
    bool _changed;

    SPItem::BBoxType _snap_bbox_type;

    Geom::OptRect _bbox;
    Geom::OptRect _stroked_bbox;
    Geom::OptRect _geometric_bbox;
    double _strokewidth;

    Geom::Affine _current_relative_affine;
    Geom::Affine _absolute_affine;
    Geom::Affine _relative_affine;
    /* According to Merriam - Webster's online dictionary
     * Affine: a transformation (as a translation, a rotation, or a uniform stretching) that carries straight
     * lines into straight lines and parallel lines into parallel lines but may alter distance between points
     * and angles between lines <affine geometry>
     */

    Geom::Point _opposite; ///< opposite point to where a scale is taking place
    Geom::Point _opposite_for_specpoints;
    Geom::Point _opposite_for_bboxpoints;
    Geom::Point _origin_for_specpoints;
    Geom::Point _origin_for_bboxpoints;

    double _handle_x;
    double _handle_y;
    bool _stepped_handle = false; ///< Frozen policy of the current transform handle.

    std::optional<Geom::Point> _center;
    bool _center_is_set; ///< we've already set _center, no need to reread it from items

    SPKnot *knots[NUMHANDS];
    CanvasItemPtr<CanvasItemCtrl> _norm;
    CanvasItemPtr<CanvasItemCtrl> _grip;
    std::array<CanvasItemPtr<CanvasItemCurve>, 4> _l;

    // Fast move preview: a picture of the objects follows the pointer and the
    // objects themselves (hidden on this canvas) move once, on release.
    bool _liveContent() const { return _show == SHOW_CONTENT && !_picture_move; }
    bool _beginMovePicture();
    void _endMovePicture();
    CanvasItemPtr<CanvasItemPicture> _picture;
    std::vector<std::pair<SPItem *, bool>> _picture_hidden; ///< item and its visibility before the move
    bool _picture_move = false;
    std::vector<SPItem*> _stamp_cache;
    bool _stamped = false;
    Geom::Point _origin; ///< position of origin for transforms
    Geom::Point _point; ///< original position of the knot being used for the current transform
    Geom::Point _point_geom; ///< original position of the knot being used for the current transform
    Inkscape::MessageContext _message_context;
    sigc::connection _sel_changed_connection;
    sigc::connection _sel_display_changed_connection;
    bool _selection_refresh_pending = false;
    sigc::connection _sel_modified_connection;
    sigc::connection _document_replaced_connection;
    sigc::connection _event_context_changed_connection;
    sigc::connection _view_transform_connection;
    sigc::connection _interaction_commit_connection;
    sigc::connection _interaction_destroy_connection;
    CancelState _cancel_state = CancelState::None;
    std::shared_ptr<SelTrans *> _callback_lifetime = std::make_shared<SelTrans *>(this);
    std::unique_ptr<Util::Unit> _resize_dimension_unit;
    std::unique_ptr<SelectionDimensionOverlay> _resize_dimension_overlay;
    Geom::OptRect _resize_target_bounds;
    DimensionalDetentLatch _resize_width_detent;
    DimensionalDetentLatch _resize_height_detent;
    bool _resize_width_active = false;
    bool _resize_height_active = false;
    bool _rotation_feedback_active = false;
    ContinuousRotationAngle _rotation_angle;
    RotationDetentLatch _rotation_detent;
    std::optional<double> _rotation_degrees;
    std::optional<Geom::Point> _rotation_origin;
    std::unique_ptr<SelectionRotationOverlay> _rotation_overlay;
    BoundingBoxPrefsObserver _bounding_box_prefs_observer;
};

}

#endif // SEEN_SELTRANS_H


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
