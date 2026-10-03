// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file Node toolbar
 */
/* Authors:
 *   MenTaLguY <mental@rydia.net>
 *   Lauris Kaplinski <lauris@kaplinski.com>
 *   bulia byak <buliabyak@users.sf.net>
 *   Frank Felfe <innerspace@iname.com>
 *   John Cliff <simarilius@yahoo.com>
 *   David Turner <novalis@gnu.org>
 *   Josh Andler <scislac@scislac.com>
 *   Jon A. Cruz <jon@joncruz.org>
 *   Maximilian Albert <maximilian.albert@gmail.com>
 *   Tavmjong Bah <tavmjong@free.fr>
 *   Abhishek Sharma
 *   Kris De Gussem <Kris.DeGussem@gmail.com>
 *   Vaibhav Malik <vaibhavmalik2018@gmail.com>
 *
 * Copyright (C) 2004 David Turner
 * Copyright (C) 2003 MenTaLguY
 * Copyright (C) 1999-2011 authors
 * Copyright (C) 2001-2002 Ximian, Inc.
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "node-toolbar.h"

#include <glibmm/i18n.h>
#include <glibmm/main.h>
#include "actions/actions-tools.h"
#include "message-context.h"
#include "object/sp-path.h"
#include "ui/tool/node.h"
#include <gtkmm/adjustment.h>
#include <gtkmm/togglebutton.h>
#include <gtkmm/menubutton.h>

#include "desktop.h"
#include "page-manager.h"
#include "selection.h"
#include "ui/builder-utils.h"
#include "ui/simple-pref-pusher.h"
#include "ui/tool/control-point-selection.h"
#include "ui/tool/multi-path-manipulator.h"
#include "ui/tool/path-manipulator.h"
#include "ui/tools/node-tool.h"
#include "ui/tools/corner-rounding-controller.h"
#include "ui/widget/corner-rounding-popover.h"
#include "ui/widget/spinbutton.h"
#include "ui/widget/unit-tracker.h"

using Inkscape::UI::Widget::UnitTracker;
using Inkscape::Util::Unit;
using Inkscape::Util::Quantity;
using Inkscape::DocumentUndo;
using Inkscape::UI::Tools::NodeTool;

namespace Inkscape::UI::Toolbar {

NodeToolbar::NodeToolbar()
    : NodeToolbar{create_builder("toolbar-node.ui")}
{}

NodeToolbar::NodeToolbar(Glib::RefPtr<Gtk::Builder> const &builder)
    : Toolbar{get_widget<Gtk::Box>(builder, "node-toolbar")}
    , _tracker{std::make_unique<UnitTracker>(Util::UNIT_TYPE_LINEAR)}
    , _nodes_lpeedit_btn{get_widget<Gtk::Button>(builder, "_nodes_lpeedit_btn")}
    , _show_helper_path_btn{&get_widget<Gtk::ToggleButton>(builder, "_show_helper_path_btn")}
    , _show_handles_btn{&get_widget<Gtk::ToggleButton>(builder, "_show_handles_btn")}
    , _show_transform_handles_btn{&get_widget<Gtk::ToggleButton>(builder, "_show_transform_handles_btn")}
    , _object_edit_mask_path_btn{&get_widget<Gtk::ToggleButton>(builder, "_object_edit_mask_path_btn")}
    , _object_edit_clip_path_btn{&get_widget<Gtk::ToggleButton>(builder, "_object_edit_clip_path_btn")}
    , _add_corners_btn{get_widget<Gtk::Button>(builder, "_add_corners_btn")}
    , _corner_button{get_widget<Gtk::MenuButton>(builder, "_corner_rounding_btn")}
    , _corner_popover{std::make_unique<UI::Widget::CornerRoundingPopover>()}
    , _nodes_x_item{get_derived_widget<UI::Widget::SpinButton>(builder, "_nodes_x_item")}
    , _nodes_y_item{get_derived_widget<UI::Widget::SpinButton>(builder, "_nodes_y_item")}
    , _nodes_d_item{get_derived_widget<UI::Widget::SpinButton>(builder, "_nodes_d_item")}
    , _nodes_d_box{get_widget<Gtk::Box>(builder, "_nodes_d_box")}
{
    // Setup the derived spin buttons.
    setup_derived_spin_button(_nodes_x_item, "x");
    setup_derived_spin_button(_nodes_y_item, "y");
    setup_derived_spin_button(_nodes_d_item, "d");

    auto unit_menu = _tracker->create_unit_dropdown();
    get_widget<Gtk::Box>(builder, "unit_menu_box").append(*unit_menu);

    // Attach the signals.
    struct ButtonMapping
    {
        char const *button_id;
        void (NodeToolbar::*callback)();
    };

    static constexpr ButtonMapping button_mapping[] = {
        {"insert_node_btn", &NodeToolbar::edit_add},
        {"delete_btn", &NodeToolbar::edit_delete},
        {"join_btn", &NodeToolbar::edit_join},
        {"break_btn", &NodeToolbar::edit_break},
        {"join_segment_btn", &NodeToolbar::edit_join_segment},
        {"delete_segment_btn", &NodeToolbar::edit_delete_segment},
        {"cusp_btn", &NodeToolbar::edit_cusp},
        {"smooth_btn", &NodeToolbar::edit_smooth},
        {"symmetric_btn", &NodeToolbar::edit_symmetrical},
        {"auto_btn", &NodeToolbar::edit_auto},
        {"line_btn", &NodeToolbar::edit_toline},
        {"curve_btn", &NodeToolbar::edit_tocurve},
    };

    for (auto const &button_info : button_mapping) {
        get_widget<Gtk::Button>(builder, button_info.button_id)
            .signal_clicked()
            .connect(sigc::mem_fun(*this, button_info.callback));
    }

    setup_insert_node_menu();
    _corner_button.set_popover(*_corner_popover);
    _corner_popover->signal_show().connect(sigc::mem_fun(*this, &NodeToolbar::refresh_corner_controls));
    _corner_popover->scope_changed.connect([this](auto) { refresh_corner_controls(); });
    _corner_popover->exit_requested.connect([this] {
        if (auto tool = get_node_tool(); tool && tool->corner_mode()) set_active_tool(_desktop, "Select");
    });
    _corner_popover->apply_requested.connect([this](auto const &request, auto generation) {
        auto lifetime = _corner_lifetime;
        auto controller = _corner_controller;
        if (!controller) return;
        // Keep a click radius independently of the readout of the next corner.
        if (auto path = cast<SPShape>(_desktop->getSelection()->singleItem())) {
            if (auto scale = LivePathEffect::CornerEdit::document_scale(path->i2doc_affine())) {
                if (request.radius) _corner_radius_px = *request.radius * *scale;
            }
        }
        if (request.mode) _corner_inverse = *request.mode == LivePathEffect::CornerEdit::Mode::InverseRound;
        auto tool = get_node_tool();
        if (request.scope == LivePathEffect::CornerEdit::Scope::Selected && tool &&
            tool->_selected_nodes->empty() && !_corner_primary_selection) {
            return; // Configure the click radius without changing the document.
        }
        auto result = controller->apply(request, generation);
        if (!*lifetime) return;
        if (result.outcome == Tools::CornerRoundingController::Outcome::NoChange) return;
        (*lifetime)->refresh_corner_controls();
        if (!*lifetime) return;
        if (result.outcome == Tools::CornerRoundingController::Outcome::Rejected ||
            result.outcome == Tools::CornerRoundingController::Outcome::Superseded) {
            (*lifetime)->_corner_popover->show_error(result.reason);
        }
    });

    _pusher_show_outline = std::make_unique<SimplePrefPusher>(_show_helper_path_btn, "/tools/nodes/show_outline");
    _show_helper_path_btn->signal_toggled().connect(sigc::bind(sigc::mem_fun(*this, &NodeToolbar::on_pref_toggled),
                                                               _show_helper_path_btn, "/tools/nodes/show_outline"));

    _pusher_show_handles = std::make_unique<SimplePrefPusher>(_show_handles_btn, "/tools/nodes/show_handles");
    _show_handles_btn->signal_toggled().connect(sigc::bind(sigc::mem_fun(*this, &NodeToolbar::on_pref_toggled),
                                                           _show_handles_btn, "/tools/nodes/show_handles"));

    _pusher_show_transform_handles =
        std::make_unique<SimplePrefPusher>(_show_transform_handles_btn, "/tools/nodes/show_transform_handles");
    _show_transform_handles_btn->signal_toggled().connect(
        sigc::bind(sigc::mem_fun(*this, &NodeToolbar::on_pref_toggled), _show_transform_handles_btn,
                   "/tools/nodes/show_transform_handles"));

    _pusher_edit_masks = std::make_unique<SimplePrefPusher>(_object_edit_mask_path_btn, "/tools/nodes/edit_masks");
    _object_edit_mask_path_btn->signal_toggled().connect(sigc::bind(
        sigc::mem_fun(*this, &NodeToolbar::on_pref_toggled), _object_edit_mask_path_btn, "/tools/nodes/edit_masks"));

    _pusher_edit_clipping_paths =
        std::make_unique<SimplePrefPusher>(_object_edit_clip_path_btn, "/tools/nodes/edit_clipping_paths");
    _object_edit_clip_path_btn->signal_toggled().connect(sigc::bind(sigc::mem_fun(*this, &NodeToolbar::on_pref_toggled),
                                                                    _object_edit_clip_path_btn,
                                                                    "/tools/nodes/edit_clipping_paths"));

    _initMenuBtns();
}

NodeToolbar::~NodeToolbar()
{
    *_corner_lifetime = nullptr;
    _corner_open_idle.disconnect(); _corner_edit_idle.disconnect();
    _corner_click.disconnect(); _corner_hover.disconnect();
    _shape_corner_click.disconnect(); _shape_corner_hover.disconnect();
    _corner_invalidated.disconnect();
    _corner_controller.reset();
    _corner_button.unset_popover();
}

void NodeToolbar::refresh_corner_controls()
{
    auto controller = _corner_controller;
    if (!controller) return;
    std::optional<LivePathEffect::CornerEdit::Address> primary;
    if (_corner_primary_selection) primary = {_corner_primary_selection->first, _corner_primary_selection->second};
    auto view = controller->inspect(_corner_popover->scope(), primary);
    auto unit = _tracker->getActiveUnit();
    auto conversion = view.context.input_to_document_scale
        ? Quantity::convert(1, unit, "px") / *view.context.input_to_document_scale : 1;
    _corner_popover->synchronize(view.summary, conversion, unit->abbr,
                               view.context.reason, view.generation);
    if (!view.summary.count) {
        _corner_popover->set_brush(Quantity::convert(_corner_radius_px, "px", unit), _corner_inverse);
    }
}

void NodeToolbar::show_corner_controls()
{
    refresh_corner_controls();
    _corner_button.popup();
}

void NodeToolbar::click_corner(UI::Node &node)
{
    namespace CE = LivePathEffect::CornerEdit;
    auto tool = get_node_tool();
    if (!tool || !_corner_controller || node.nodeList().subpathList().pm().item() !=
        _desktop->getSelection()->singleItem()) return;
    auto address = Tools::corner_rounding_address(node);
    tool->_selected_nodes->clear();
    tool->_selected_nodes->insert(&node);
    apply_clicked_corner(address.path, address.node, false);
}

void NodeToolbar::click_shape_corner(std::size_t path, std::size_t node)
{
    _corner_primary_selection = {path, node};
    apply_clicked_corner(path, node, true);
}

void NodeToolbar::apply_clicked_corner(std::size_t path, std::size_t node, bool primary_shape)
{
    namespace CE = LivePathEffect::CornerEdit;
    if (!_corner_controller) return;
    CE::Address address{path, node};
    auto view = _corner_controller->inspect(_corner_popover->scope(),
        primary_shape ? std::optional(address) : std::nullopt);
    if (!view.context.snapshot || !view.context.input_to_document_scale) {
        _corner_popover->show_error(view.context.reason); return;
    }
    auto const &data = view.context.snapshot->satellites;
    if (address.path >= data.size() || address.node >= data[address.path].size()) return;
    bool remove = data[address.path][address.node].amount > 0;
    CE::Request request{_corner_popover->scope(),
        _corner_inverse ? CE::Mode::InverseRound : CE::Mode::Round,
        remove ? 0 : _corner_radius_px / *view.context.input_to_document_scale};
    _corner_edit_idle.disconnect();
    _corner_edit_idle = Glib::signal_idle().connect(
        [controller = _corner_controller, lifetime = _corner_lifetime, request, generation = view.generation] {
            auto result = controller->apply(request, generation);
            if (!*lifetime) return false;
            (*lifetime)->refresh_corner_controls();
            if (!result.reason.empty()) (*lifetime)->_corner_popover->show_error(result.reason);
            return false;
        });
}

void NodeToolbar::hover_corner(UI::ControlPoint *point)
{
    namespace CE = LivePathEffect::CornerEdit;
    auto tool = get_node_tool();
    if (!tool) return;
    auto node = dynamic_cast<UI::Node *>(point);
    auto path = cast<SPPath>(_desktop->getSelection()->singleItem());
    if (!node || !path || node->nodeList().subpathList().pm().item() != path) {
        tool->set_cursor("node.svg"); return;
    }
    auto address = Tools::corner_rounding_address(*node);
    hover_shape_corner(address.path, address.node);
}

void NodeToolbar::hover_shape_corner(std::size_t p, std::size_t n)
{
    namespace CE = LivePathEffect::CornerEdit;
    auto tool = get_node_tool();
    auto path = cast<SPShape>(_desktop->getSelection()->singleItem());
    if (!tool || !path) return;
    auto context = Tools::capture_corner_rounding(*path, *tool->_selected_nodes);
    bool valid = false, remove = false;
    if (context.snapshot && context.input_to_document_scale) {
        CE::Address address{p, n};
        auto &snapshot = *context.snapshot;
        snapshot.selected = {address};
        if (address.path < snapshot.satellites.size() && address.node < snapshot.satellites[address.path].size()) {
            remove = snapshot.satellites[address.path][address.node].amount > 0;
            auto plan = CE::prepare(snapshot, {CE::Scope::Selected,
                _corner_inverse ? CE::Mode::InverseRound : CE::Mode::Round,
                remove ? 0 : _corner_radius_px / *context.input_to_document_scale});
            valid = plan.status == CE::Status::Ready || plan.status == CE::Status::NoChange;
        }
    }
    tool->use_cursor(Gdk::Cursor::create(valid ? "crosshair" : "not-allowed"));
    _desktop->tipsMessageContext()->set(NORMAL_MESSAGE, valid
        ? remove ? _("Click to remove rounding.") : _("Click to round this corner.")
        : _("This corner cannot accommodate the current radius."));
}

void NodeToolbar::setDesktop(SPDesktop *desktop)
{
    _corner_primary_selection.reset();
    _corner_invalidated.disconnect();
    _corner_open_idle.disconnect(); _corner_edit_idle.disconnect();
    _corner_click.disconnect(); _corner_hover.disconnect();
    _shape_corner_click.disconnect(); _shape_corner_hover.disconnect();
    _corner_controller.reset();
    _corner_popover->popdown();
    if (_desktop) {
        c_selection_changed.disconnect();
        c_selection_modified.disconnect();
        c_subselection_changed.disconnect();
    }

    Toolbar::setDesktop(desktop);

    if (_desktop) {
        _corner_controller = std::make_shared<Tools::CornerRoundingController>(*_desktop);
        _corner_invalidated = _corner_controller->connectInvalidated([this] {
            _corner_edit_idle.disconnect();
            refresh_corner_controls(); // Cancels pending input; never applies it to a new selection.
        });
        // watch selection
        c_selection_changed = desktop->getSelection()->connectChanged([this](Selection *selection) {
            _corner_primary_selection.reset();
            sel_changed(selection);
            refresh_corner_controls();
        });
        c_selection_modified = desktop->getSelection()->connectModified(sigc::mem_fun(*this, &NodeToolbar::sel_modified));
        c_subselection_changed = desktop->connect_control_point_selected([this] (ControlPointSelection *selection) {
            coord_changed(selection);
        });

        sel_changed(desktop->getSelection());
        coord_changed(get_node_tool()->_selected_nodes);
        auto tool = get_node_tool();
        if (tool->corner_mode()) {
            _corner_click = tool->corner_clicked.connect(sigc::mem_fun(*this, &NodeToolbar::click_corner));
            _corner_hover = tool->corner_hovered.connect(sigc::mem_fun(*this, &NodeToolbar::hover_corner));
            _shape_corner_click = tool->shape_corner_clicked.connect(sigc::mem_fun(*this, &NodeToolbar::click_shape_corner));
            _shape_corner_hover = tool->shape_corner_hovered.connect(sigc::mem_fun(*this, &NodeToolbar::hover_shape_corner));
            _corner_open_idle = Glib::signal_idle().connect([this] { show_corner_controls(); return false; });
        }
    }
}

void NodeToolbar::setActiveUnit(Util::Unit const *unit)
{
    _tracker->setActiveUnit(unit);
    if (_corner_controller) _corner_controller->invalidate();
}

void NodeToolbar::setup_derived_spin_button(UI::Widget::SpinButton &btn, Glib::ustring const &name)
{
    auto adj = btn.get_adjustment();
    adj->set_value(0);
    adj->signal_value_changed().connect(sigc::bind(sigc::mem_fun(*this, &NodeToolbar::value_changed), name, adj));

    _tracker->addAdjustment(adj->gobj());
    btn.addUnitTracker(_tracker.get());

    btn.setDefocusTarget(this);
}

void NodeToolbar::setup_insert_node_menu()
{
    // insert_node_menu
    auto const actions = Gio::SimpleActionGroup::create();
    actions->add_action("insert-leftmost", sigc::mem_fun(*this, &NodeToolbar::edit_add_leftmost));
    actions->add_action("insert-rightmost", sigc::mem_fun(*this, &NodeToolbar::edit_add_rightmost));
    actions->add_action("insert-topmost", sigc::mem_fun(*this, &NodeToolbar::edit_add_topmost));
    actions->add_action("insert-bottommost", sigc::mem_fun(*this, &NodeToolbar::edit_add_bottommost));
    insert_action_group("node-toolbar", actions);
}

void NodeToolbar::value_changed(Glib::ustring const &name, Glib::RefPtr<Gtk::Adjustment> const &adj)
{
    if (_tracker->isUpdating() && _corner_controller) _corner_controller->invalidate();
    // quit if run by the XML listener or a unit change
    if (_blocker.pending() || _tracker->isUpdating()) {
        return;
    }

    // in turn, prevent XML listener from responding
    auto guard = _blocker.block();

    auto const unit = _tracker->getActiveUnit();

    auto nt = get_node_tool();
    double val = Quantity::convert(adj->get_value(), unit, "px");
    auto pwb = nt->_selected_nodes->pointwiseBounds();
    auto fsp = nt->_selected_nodes->firstSelectedPoint();

    if (name == "d") {
        // Length has changed, not a coordinate...
        double delta = val / pwb->diameter();

        if (delta > 0) {
            auto center = fsp ? *fsp : pwb->midpoint();
            nt->_multipath->scale(center, {delta, delta});
        }

    } else if (nt && !nt->_selected_nodes->empty()) {
        // Coordinate
        auto d = name == "x" ? Geom::X : Geom::Y;
        double oldval = pwb->midpoint()[d];

        // Adjust the coordinate to the current page, if needed
        auto &pm = _desktop->getDocument()->getPageManager();
        if (_desktop->getDocument()->get_origin_follows_page()) {
            auto page = pm.getSelectedPageRect();
            oldval -= page.corner(0)[d];
        }

        Geom::Point delta;
        delta[d] = val - oldval;
        nt->_multipath->move(delta);
    }
}

void NodeToolbar::sel_changed(Selection *selection)
{
    if (is<SPGroup>(selection->singleItem())) {
        _add_corners_btn.set_sensitive(false);
    } else {
        _add_corners_btn.set_sensitive(true);
    }
    if (auto lpeitem = cast<SPLPEItem>(selection->singleItem())) {
        _nodes_lpeedit_btn.set_sensitive(lpeitem->hasPathEffect());
    } else {
        _nodes_lpeedit_btn.set_sensitive(false);
    }
}

void NodeToolbar::sel_modified(Selection *selection, guint /*flags*/)
{
    sel_changed(selection);
}

// is called when the node selection is modified
void NodeToolbar::coord_changed(ControlPointSelection *selected_nodes)
{
    // quit if run by the attr_changed listener
    if (_blocker.pending()) {
        return;
    }

    // in turn, prevent listener from responding
    auto guard = _blocker.block();

    if (!_tracker) {
        return;
    }
    auto const unit = _tracker->getActiveUnit();

    if (!selected_nodes || selected_nodes->empty()) {
        // no path selected
        _nodes_x_item.set_sensitive(false);
        _nodes_y_item.set_sensitive(false);
    } else {
        _nodes_x_item.set_sensitive(true);
        _nodes_y_item.set_sensitive(true);
        auto adj_x = _nodes_x_item.get_adjustment();
        auto adj_y = _nodes_y_item.get_adjustment();
        Geom::Coord oldx = Quantity::convert(adj_x->get_value(), unit, "px");
        Geom::Coord oldy = Quantity::convert(adj_y->get_value(), unit, "px");
        Geom::Point mid = selected_nodes->pointwiseBounds()->midpoint();

        // Adjust shown coordinate according to the selected page
        if (_desktop->getDocument()->get_origin_follows_page()) {
            auto &pm = _desktop->getDocument()->getPageManager();
            mid *= pm.getSelectedPageAffine().inverse();
        }

        if (oldx != mid.x()) {
            adj_x->set_value(Quantity::convert(mid.x(), "px", unit));
        }
        if (oldy != mid.y()) {
            adj_y->set_value(Quantity::convert(mid.y(), "px", unit));
        }
    }

    if (selected_nodes->size() == 2) {
        // Length is only visible when exactly two nodes are selected
        _nodes_d_box.set_visible(true);
        auto adj_l = _nodes_d_item.get_adjustment();
        Geom::Coord oldl = Quantity::convert(adj_l->get_value(), unit, "px");

        Geom::Coord length = selected_nodes->pointwiseBounds()->diameter();
        if (oldl != length) {
            adj_l->set_value(Quantity::convert(length, "px", unit));
        }
    } else {
        _nodes_d_box.set_visible(false);
    }
}

void NodeToolbar::edit_add()
{
    if (auto nt = get_node_tool()) {
        nt->_multipath->insertNodes();
    }
}

/* add a node at the left-most point on selected path(s)*/
void NodeToolbar::edit_add_leftmost()
{
    if (auto nt = get_node_tool()) {
        nt->_multipath->insertNodesAtExtrema(PointManipulator::EXTR_MIN_X);
    }
}

/* add a node at the right-most point on selected path(s)*/
void NodeToolbar::edit_add_rightmost()
{
    if (auto nt = get_node_tool()) {
        nt->_multipath->insertNodesAtExtrema(PointManipulator::EXTR_MAX_X);
    }
}

/* add a node at the top-most point on selected path(s)*/
void NodeToolbar::edit_add_topmost()
{
    const auto extrema = _desktop->yaxisdown()
                         ? PointManipulator::EXTR_MIN_Y
                         : PointManipulator::EXTR_MAX_Y;
    if (auto nt = get_node_tool()) {
        nt->_multipath->insertNodesAtExtrema(extrema);
    }
}

/* add a node at the bottom-most point on selected path(s)*/
void NodeToolbar::edit_add_bottommost()
{
    const auto extrema = _desktop->yaxisdown()
                         ? PointManipulator::EXTR_MAX_Y
                         : PointManipulator::EXTR_MIN_Y;
    if (auto nt = get_node_tool()) {
        nt->_multipath->insertNodesAtExtrema(extrema);
    }
}

void NodeToolbar::edit_delete()
{
    if (auto nt = get_node_tool()) {
        auto prefs = Preferences::get();
        nt->_multipath->deleteNodes((NodeDeleteMode)prefs->getInt("/tools/node/delete-mode-default", (int)NodeDeleteMode::automatic));
    }
}

void NodeToolbar::edit_join()
{
    if (auto nt = get_node_tool()) {
        nt->_multipath->joinNodes();
    }
}

void NodeToolbar::edit_break()
{
    if (auto nt = get_node_tool()) {
        nt->_multipath->breakNodes();
    }
}

void NodeToolbar::edit_delete_segment()
{
    if (auto nt = get_node_tool()) {
        nt->_multipath->deleteSegments();
    }
}

void NodeToolbar::edit_join_segment()
{

    if (auto nt = get_node_tool()) {
        nt->_multipath->joinSegments();
    }
}

void NodeToolbar::edit_cusp()
{
    if (auto nt = get_node_tool()) {
        nt->_multipath->setNodeType(NODE_CUSP);
    }
}

void NodeToolbar::edit_smooth()
{
    if (auto nt = get_node_tool()) {
        nt->_multipath->setNodeType(NODE_SMOOTH);
    }
}

void NodeToolbar::edit_symmetrical()
{   
    if (auto nt = get_node_tool()) {
        nt->_multipath->setNodeType(NODE_SYMMETRIC);
    }
}

void NodeToolbar::edit_auto()
{
    if (auto nt = get_node_tool()) {
        nt->_multipath->setNodeType(NODE_AUTO);
    }
}

void NodeToolbar::edit_toline()
{
    if (auto nt = get_node_tool()) {
        nt->_multipath->setSegmentType(SEGMENT_STRAIGHT);
    }
}

void NodeToolbar::edit_tocurve()
{
    if (auto nt = get_node_tool()) {
        nt->_multipath->setSegmentType(SEGMENT_CUBIC_BEZIER);
    }
}

void NodeToolbar::on_pref_toggled(Gtk::ToggleButton *item, Glib::ustring const &path)
{
    Preferences::get()->setBool(path, item->get_active());
}

NodeTool *NodeToolbar::get_node_tool() const
{
    return _desktop ? dynamic_cast<NodeTool *>(_desktop->getTool()) : nullptr;
}

} // namespace Inkscape::UI::Toolbar

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
