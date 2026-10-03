// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * Long-press Boolean operation previews for the Shape Builder toolbar button.
 */

#include "boolean-assist.h"

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>
#include <glibmm/i18n.h>
#include <glibmm/main.h>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/eventcontrollermotion.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/gesturelongpress.h>
#include <gtkmm/image.h>
#include <gtkmm/label.h>
#include <gtkmm/popover.h>
#include <gtkmm/togglebutton.h>

#include "desktop.h"
#include "display/control/canvas-item-bpath.h"
#include "display/control/canvas-item-ptr.h"
#include "display/drawing-item.h"
#include "document.h"
#include "document-undo.h"
#include "inkscape-window.h"
#include "message-stack.h"
#include "object/object-set.h"
#include "object/sp-flowtext.h"
#include "object/sp-item.h"
#include "object/sp-item-group.h"
#include "object/sp-object.h"
#include "object/sp-path.h"
#include "object/sp-root.h"
#include "object/sp-shape.h"
#include "object/sp-text.h"
#include "path/path-boolop.h"
#include "path/path-util.h"
#include "selection.h"
#include "style.h"
#include "ui/icon-names.h"
#include "ui/popup-menu.h"
#include "xml/node.h"

namespace Inkscape::UI::Toolbar {

std::optional<Geom::PathVector> boolean_operand_path(SPItem *item)
{
    if (!item || (!is<SPShape>(item) && !is<SPText>(item) && !is<SPFlowtext>(item))) {
        return {};
    }

    std::optional<Geom::PathVector> path;
    if (auto shape = cast<SPShape>(item)) {
        if (auto curve = shape->curve()) {
            path = *curve;
        }
    } else {
        path = curve_for_item(item);
    }

    if (!path || path->empty()) {
        return {};
    }
    return path;
}

bool boolean_operand_eligible(SPItem *item)
{
    return boolean_operand_path(item).has_value();
}

namespace {

// A shape/text type whose outline is empty is distinct from a type the operation does not support at all: the
// outline can legitimately be empty, so it is reported as "empty-geometry" rather than "not-a-shape".
bool boolean_item_is_shape_type(SPItem *item)
{
    return item && (is<SPShape>(item) || is<SPText>(item) || is<SPFlowtext>(item));
}

// A group still renders something when it has any descendant that is not itself a group (a path, shape, text, ...).
bool item_has_leaf_content(SPObject *object)
{
    for (auto &child : object->children) {
        auto *item = cast<SPItem>(&child);
        if (!item) {
            continue;
        }
        if (is<SPGroup>(item)) {
            if (item_has_leaf_content(item)) {
                return true;
            }
        } else {
            return true;
        }
    }
    return false;
}

// Candidate groups for cleanup: the selected group roots themselves and the groups nested inside them, each
// only while it still has leaf content before the operation. Nothing outside a selected root's subtree is ever
// considered, so an unselected group elsewhere in the document is never removed.
void collect_group_candidates(SPObject *group_root, std::unordered_set<SPObject *> &out)
{
    auto *item = cast<SPItem>(group_root);
    if (!item) {
        return;
    }
    if (auto *group = cast<SPGroup>(item)) {
        if (item_has_leaf_content(group)) {
            out.insert(group);
        }
    }
    for (auto &child : group_root->children) {
        auto *child_item = cast<SPItem>(&child);
        if (child_item && is<SPGroup>(child_item)) {
            collect_group_candidates(child_item, out);
        }
    }
}

// A group that renders no effect of its own can act as one combined shape. A clip, mask, filter, group opacity
// or non-normal blend mode changes how the group renders, and a combined silhouette cannot reproduce it, so such
// a group must be refused rather than silently dropping the effect.
bool group_has_effect(SPItem *item)
{
    if (!item) {
        return false;
    }
    if (item->getClipObject() || item->getMaskObject() || item->isFiltered()) {
        return true;
    }
    if (auto const *style = item->style) {
        if (style->opacity.set && style->opacity.as_double() < 1.0) {
            return true;
        }
        if (style->mix_blend_mode.value != SP_CSS_BLEND_NORMAL) {
            return true;
        }
    }
    return false;
}

// Post-order sweep over one selected root's subtree: a candidate group the operation emptied loses its last
// content and is removed. Deleting a group also removes its (already processed) descendants, so a snapshot of
// the child list avoids iterator invalidation. Groups that were empty before the operation are not candidates and
// are left untouched. Returns whether @a parent still has content and records every deleted group's id.
bool remove_emptied_groups(SPObject *parent, std::unordered_set<SPObject *> const &candidates,
                           std::vector<std::string> *removed_ids)
{
    std::vector<SPObject *> children;
    for (auto &child : parent->children) {
        children.push_back(&child);
    }

    bool has_content = false;
    for (auto *child : children) {
        auto *item = cast<SPItem>(child);
        if (!item) {
            continue;
        }
        if (auto *group = cast<SPGroup>(item)) {
            if (remove_emptied_groups(group, candidates, removed_ids)) {
                has_content = true;
            }
        } else {
            has_content = true;
        }
    }

    // Only plain groups are cleaned up. A layer the operation emptied (operands taken from two layers,
    // result placed in one) is user structure and is never deleted.
    if (auto *group = cast<SPGroup>(parent);
        group && group->layerMode() == SPGroup::GROUP && !has_content && candidates.count(group)) {
        if (removed_ids) {
            if (char const *id = group->getId()) {
                removed_ids->emplace_back(id);
            }
        }
        group->deleteObject();
        return false;
    }
    return has_content;
}

// Keeps the operands' XML nodes alive for the duration of a boolean operation. The engine deletes the
// non-source operands and then allocates a fresh svg:path for the result; without anchoring, the allocator can
// reuse a freed node's address and make the new result compare equal to a deleted operand. Anchors on demand and
// releases every anchored node on destruction, so no caller path can leak an anchor.
class OperandNodeAnchor
{
public:
    OperandNodeAnchor() = default;
    OperandNodeAnchor(OperandNodeAnchor const &) = delete;
    OperandNodeAnchor &operator=(OperandNodeAnchor const &) = delete;

    ~OperandNodeAnchor()
    {
        for (auto *node : _nodes) {
            Inkscape::GC::release(node);
        }
    }

    void anchor(Inkscape::XML::Node *node)
    {
        if (node) {
            Inkscape::GC::anchor(node);
            _nodes.push_back(node);
        }
    }

    std::vector<Inkscape::XML::Node *> const &nodes() const { return _nodes; }

private:
    std::vector<Inkscape::XML::Node *> _nodes;
};

} // namespace

bool boolean_assist_leaves(SPItem *root, std::function<bool(SPItem *)> const &available,
                           std::vector<SPItem *> &leaves, SPItem **offending, std::string *reason)
{
    auto const set_reason = [reason](char const *value) {
        if (reason) {
            *reason = value;
        }
    };

    leaves.clear();
    if (offending) {
        *offending = nullptr;
    }
    if (reason) {
        reason->clear();
    }
    if (!root) {
        return false;
    }

    if (boolean_operand_eligible(root)) {
        if (!available(root)) {
            if (offending) {
                *offending = root;
            }
            set_reason("unavailable");
            return false;
        }
        leaves.push_back(root);
        return true;
    }
    if (!is<SPGroup>(root)) {
        if (offending) {
            *offending = root;
        }
        set_reason(boolean_item_is_shape_type(root) ? "empty-geometry" : "not-a-shape");
        return false;
    }

    bool ok = true;
    SPItem *bad = nullptr;
    std::string bad_reason;
    auto const fail = [&](SPItem *which, char const *why) {
        ok = false;
        bad = which;
        bad_reason = why;
    };

    // The root group itself is checked first: an effect on the group cannot be reproduced by its silhouette.
    if (group_has_effect(root)) {
        fail(root, "group-effect");
    }

    std::function<void(SPGroup *)> walk = [&](SPGroup *group) {
        for (auto &child : group->children) {
            if (!ok) {
                return;
            }
            auto *item = cast<SPItem>(&child);
            if (!item) {
                continue;
            }
            if (!available(item)) {
                fail(item, "unavailable");
                return;
            }
            if (auto *nested = cast<SPGroup>(item)) {
                if (group_has_effect(nested)) {
                    fail(nested, "group-effect");
                    return;
                }
                auto const before = leaves.size();
                walk(nested);
                if (ok && leaves.size() == before) {
                    fail(nested, "empty-group"); // A group with no leaves cannot act as a combined shape.
                }
            } else if (boolean_operand_eligible(item)) {
                leaves.push_back(item);
            } else {
                fail(item, boolean_item_is_shape_type(item) ? "empty-geometry" : "not-a-shape");
                return;
            }
        }
    };
    walk(cast<SPGroup>(root));

    if (ok && leaves.empty()) {
        fail(root, "empty-group");
    }

    if (!ok) {
        leaves.clear();
        if (offending) {
            *offending = bad;
        }
        set_reason(bad_reason.c_str());
        return false;
    }
    return true;
}

bool boolean_result_is_new(SPItem *result, std::vector<Inkscape::XML::Node *> const &operand_reprs)
{
    if (!result) {
        return false;
    }
    auto *repr = result->getRepr();
    return repr && std::find(operand_reprs.begin(), operand_reprs.end(), repr) == operand_reprs.end();
}

SPItem *apply_boolean_assist(Inkscape::ObjectSet &set, BooleanAssistOp op,
                             std::vector<std::string> *removed_group_ids)
{
    auto *document = set.document();
    if (!document) {
        return nullptr;
    }

    auto const roots = set.items_vector();

    // Cleanup candidates are computed before the operation and limited to the selected group roots and the
    // groups nested inside them. A selection without a group root leaves the candidate set empty, so no group
    // is ever removed for it; the whole document is never scanned. The selected group roots are captured now:
    // the operation deletes the other (non-anchor) operands, so after it runs the original root pointers can no
    // longer be classified.
    std::unordered_set<SPObject *> candidates;
    std::vector<SPItem *> group_roots;
    for (auto *root : roots) {
        if (root && is<SPGroup>(root)) {
            collect_group_candidates(root, candidates);
            group_roots.push_back(root);
        }
    }

    std::vector<SPItem *> operands;
    operands.reserve(roots.size());
    bool expanded_any_group = false;

    for (auto *root : roots) {
        if (!root) {
            continue;
        }
        if (is<SPGroup>(root)) {
            std::vector<SPItem *> leaves;
            SPItem *offending = nullptr;
            if (!boolean_assist_leaves(root, [](SPItem *item) { return item->isVisibleAndUnlocked(); },
                                       leaves, &offending, nullptr)) {
                return nullptr;
            }
            Inkscape::ObjectSet leaf_set(document);
            leaf_set.setList(leaves);
            leaf_set.pathUnion(true, true);
            auto *const combined = leaf_set.singleItem();
            if (!combined || !is<SPPath>(combined)) {
                return nullptr;
            }
            operands.push_back(combined);
            expanded_any_group = true;
        } else if (boolean_operand_eligible(root)) {
            operands.push_back(root);
        } else {
            return nullptr;
        }
    }

    // Hold the operands' XML nodes for the whole operation. Their addresses must stay unique so the freshly
    // created result path cannot land on a freed operand's address and look like an untouched operand below.
    OperandNodeAnchor anchored_operands;
    for (auto *operand : operands) {
        anchored_operands.anchor(operand->getRepr());
    }

    // Selections without groups keep today's exact call sequence: the set already holds the operands.
    if (expanded_any_group) {
        set.setList(operands);
    }

    switch (op) {
        case BooleanAssistOp::Union:
            set.pathUnion(true, true);
            break;
        case BooleanAssistOp::Intersection:
            set.pathIntersect(true, true);
            break;
        case BooleanAssistOp::BottomMinusRest:
            set.pathDiffMany(false, true, true);
            break;
        case BooleanAssistOp::TopMinusRest:
            set.pathDiffMany(true, true, true);
            break;
        case BooleanAssistOp::Exclusion:
            set.pathSymDiff(true, true);
            break;
    }

    if (!candidates.empty()) {
        for (auto *root : group_roots) {
            // Defensive only: the selected roots are disjoint subtrees (ObjectSet::add drops ancestors and
            // descendants), so no selected root is a descendant of another, and the boolean engine never deletes
            // a group root. The parent check guards against a future engine change that would make a root dangle.
            if (root && is<SPGroup>(root) && root->parent) {
                remove_emptied_groups(root, candidates, removed_group_ids);
            }
        }
    }

    auto *result = set.singleItem();
    if (!result || !is<SPPath>(result) || !boolean_result_is_new(result, anchored_operands.nodes())) {
        return nullptr;
    }
    return result;
}

std::pair<Util::Internal::ContextString, char const *> boolean_assist_undo_label(BooleanAssistOp op,
                                                                                 std::size_t operand_count)
{
    switch (op) {
        case BooleanAssistOp::Union:
            return {RC_("Undo", "Union"), INKSCAPE_ICON("path-union")};
        case BooleanAssistOp::Intersection:
            return {RC_("Undo", "Intersection"), INKSCAPE_ICON("path-intersection")};
        case BooleanAssistOp::Exclusion:
            return {RC_("Undo", "Exclusion"), INKSCAPE_ICON("path-exclusion")};
        case BooleanAssistOp::BottomMinusRest:
            if (operand_count == 2) {
                return {RC_("Undo", "Difference"), INKSCAPE_ICON("path-difference")};
            }
            return {RC_("Undo", "Bottom minus other objects"), INKSCAPE_ICON("path-difference")};
        case BooleanAssistOp::TopMinusRest:
            if (operand_count == 2) {
                return {RC_("Undo", "Reverse Difference"), INKSCAPE_ICON("path-difference")};
            }
            return {RC_("Undo", "Top minus other objects"), INKSCAPE_ICON("path-difference")};
    }
    return {RC_("Undo", "Union"), INKSCAPE_ICON("path-union")};
}

namespace {

enum class Operation : unsigned
{
    UNION,
    INTERSECTION,
    A_MINUS_B,
    B_MINUS_A,
    EXCLUSION,
    COUNT
};

constexpr auto operation_count = static_cast<unsigned>(Operation::COUNT);

struct OperationInfo
{
    Operation operation;
    char const *icon;
    char const *label;
    char const *tooltip;
};

constexpr std::array<OperationInfo, operation_count> operation_info = {{
    {Operation::UNION, "path-union", N_("Union"), N_("Union A and B")},
    {Operation::INTERSECTION, "path-intersection", N_("Intersect"), N_("Keep the intersection of A and B")},
    {Operation::A_MINUS_B, "path-difference", N_("A − B"), N_("Subtract upper object B from lower object A")},
    {Operation::B_MINUS_A, "path-difference", N_("B − A"), N_("Subtract lower object A from upper object B")},
    {Operation::EXCLUSION, "path-exclusion", N_("Exclude"), N_("Keep areas belonging to A or B, but not both")},
}};

char const *operation_label(Operation operation)
{
    return _(operation_info[static_cast<unsigned>(operation)].label);
}

BooleanAssistOp boolean_assist_op(Operation operation)
{
    switch (operation) {
        case Operation::UNION:
            return BooleanAssistOp::Union;
        case Operation::INTERSECTION:
            return BooleanAssistOp::Intersection;
        case Operation::A_MINUS_B:
            return BooleanAssistOp::BottomMinusRest;
        case Operation::B_MINUS_A:
            return BooleanAssistOp::TopMinusRest;
        case Operation::EXCLUSION:
            return BooleanAssistOp::Exclusion;
        case Operation::COUNT:
            break;
    }
    return BooleanAssistOp::Union;
}

struct Operand
{
    SPItem *item = nullptr;
    // For a group root: its leaf items (paths, shapes, text), in document order. Empty for a single-item operand.
    std::vector<SPItem *> leaves;
    Geom::PathVector path;
    FillRule fill_rule = fill_nonZero;
    bool path_ready = false;
};

// One operand path exactly as the single-object case has always built it: the object curve, i2dt applied.
std::optional<Geom::PathVector> operand_path(SPItem *item)
{
    auto path = boolean_operand_path(item);
    if (!path) {
        return {};
    }
    *path *= item->i2dt_affine();
    return path;
}

FillRule operand_fill_rule(SPItem *item)
{
    return item->style && item->style->fill_rule.computed == SP_WIND_RULE_EVENODD ? fill_oddEven : fill_nonZero;
}

// Build an operand's silhouette only when the preview first needs it. A single item uses its own path and fill
// rule; a group starts from the first leaf's own fill rule (exactly as that leaf would on its own) and, after
// the first union, the accumulated result is nonzero like any union result. open() therefore only walks a
// group's leaves; the (potentially expensive) union is deferred to the first preview.
bool build_operand_path(Operand &operand)
{
    if (operand.path_ready) {
        return true;
    }
    if (operand.leaves.empty()) {
        auto path = operand_path(operand.item);
        if (!path) {
            return false;
        }
        operand.path = std::move(*path);
        operand.fill_rule = operand_fill_rule(operand.item);
        operand.path_ready = true;
        return true;
    }

    Geom::PathVector combined;
    FillRule combined_fill = fill_nonZero;
    bool initialized = false;
    for (auto *leaf : operand.leaves) {
        auto path = operand_path(leaf);
        if (!path) {
            return false;
        }
        if (!initialized) {
            combined = std::move(*path);
            combined_fill = operand_fill_rule(leaf);
            initialized = true;
        } else {
            combined = sp_pathvector_boolop(combined, *path, bool_op_union, combined_fill, operand_fill_rule(leaf));
            combined_fill = fill_nonZero;
        }
    }
    if (!initialized) {
        return false;
    }
    operand.path = std::move(combined);
    operand.fill_rule = combined_fill;
    operand.path_ready = true;
    return true;
}

// Validate one selected root without building any silhouette. On failure returns nullopt and reports the
// offending object and the reason ("group-effect", "not-a-shape", "unavailable" or "empty-group").
std::optional<Operand> make_operand(SPItem *item, SPItem **offending, std::string *reason)
{
    if (offending) {
        *offending = nullptr;
    }
    if (reason) {
        reason->clear();
    }

    // Reuse the outline computed for the eligibility check instead of recomputing it on the first preview.
    if (auto path = boolean_operand_path(item)) {
        Operand operand;
        operand.item = item;
        *path *= item->i2dt_affine();
        operand.path = std::move(*path);
        operand.fill_rule = operand_fill_rule(item);
        operand.path_ready = true;
        return operand;
    }
    if (is<SPGroup>(item)) {
        Operand operand;
        operand.item = item;
        SPItem *bad = nullptr;
        std::string why;
        if (!boolean_assist_leaves(item, [](SPItem *leaf) { return leaf->isVisibleAndUnlocked(); },
                                   operand.leaves, &bad, &why)) {
            if (offending) {
                *offending = bad;
            }
            if (reason) {
                *reason = why;
            }
            return {};
        }
        return operand;
    }

    if (offending) {
        *offending = item;
    }
    if (reason) {
        *reason = boolean_item_is_shape_type(item) ? "empty-geometry" : "not-a-shape";
    }
    return {};
}

} // namespace

ItemReleaseWatch::ItemReleaseWatch(std::function<void(SPItem *)> on_release)
    : _on_release(std::move(on_release))
{}

ItemReleaseWatch::~ItemReleaseWatch()
{
    clear();
}

void ItemReleaseWatch::watch(SPItem *item)
{
    if (!item) {
        return;
    }
    _connections.emplace_back(item->connectRelease([this](SPObject *object) {
        _released = true;
        // Every pointer the session holds may now be stale: stop listening first, then let the session react
        // while the released object is still alive.
        for (auto &connection : _connections) {
            connection.disconnect();
        }
        _connections.clear();
        if (_on_release) {
            _on_release(cast<SPItem>(object));
        }
    }));
}

void ItemReleaseWatch::clear()
{
    for (auto &connection : _connections) {
        connection.disconnect();
    }
    _connections.clear();
    _released = false;
}

class BooleanAssist::Impl
{
public:
    Impl(Gtk::ToggleButton &button, Gtk::Widget &popover_parent, InkscapeWindow *window)
        : _button(button)
        , _window(window)
        , _popover(Gtk::make_managed<Gtk::Popover>())
        , _operand_watch([this](SPItem *released) { on_operand_released(released); })
    {
        _button.set_tooltip_text(_("Shape Builder Tool\nLong press or right-click for Boolean Assistant"));

        auto box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 2);
        box->set_margin(4);
        box->add_css_class("linked");

        for (auto const &info : operation_info) {
            auto operation_button = Gtk::make_managed<Gtk::Button>();
            operation_button->add_css_class("flat");
            operation_button->set_tooltip_text(_(info.tooltip));

            auto content = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
            auto image = Gtk::make_managed<Gtk::Image>();
            image->set_from_icon_name(info.icon);
            image->set_pixel_size(24);
            auto label = Gtk::make_managed<Gtk::Label>(_(info.label));
            content->append(*image);
            content->append(*label);
            operation_button->set_child(*content);

            auto motion = Gtk::EventControllerMotion::create();
            track(motion->signal_enter().connect(
                [this, operation = info.operation](double, double) { preview(operation); }));
            track(motion->signal_motion().connect([this, operation = info.operation](double, double) {
                // A newly mapped popover may already be under the pointer and GTK
                // does not always synthesize an enter event. Motion is a reliable
                // fallback and the idle check in open() covers a stationary pointer.
                preview(operation);
            }));
            track(motion->signal_leave().connect([this, operation = info.operation] {
                // Entering the next button and leaving the previous one can be
                // reported in either order. Only clear the preview if this is still
                // the operation being shown.
                if (!_pending && _hovered == operation) {
                    _hovered.reset();
                    clear_preview();
                    set_status(_("Boolean Assistant: hover an operation to preview; "
                                 "click to apply or press Esc to cancel."));
                }
            }));
            operation_button->add_controller(motion);
            auto const index = static_cast<unsigned>(info.operation);
            _operation_buttons[index] = operation_button;
            _operation_labels[index] = label;
            _operation_motion[index] = motion.get();
            track(operation_button->signal_clicked().connect(
                [this, operation = info.operation] { choose(operation); }));
            box->append(*operation_button);
        }

        _popover->set_child(*box);
        _popover->set_position(Gtk::PositionType::RIGHT);
        _popover->set_autohide(true);
        _popover->set_has_arrow(true);
        _popover->set_parent(popover_parent);
        track(_popover->signal_closed().connect([this] {
            if (!_pending) {
                reset_session(false);
            }
        }));

        auto long_press = Gtk::GestureLongPress::create();
        _long_press = long_press.get();
        _long_press->set_button(GDK_BUTTON_PRIMARY);
        _long_press->set_touch_only(false);
        _long_press->set_delay_factor(0.85);
        _long_press->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
        track(_long_press->signal_pressed().connect([this](double, double) {
            _long_press->set_state(Gtk::EventSequenceState::CLAIMED);
            _long_press_active = true;
            open();
        }));
        track(_long_press->signal_update().connect([this](Gdk::EventSequence *sequence) {
            if (!_long_press_active || !_valid) {
                return;
            }
            double x = 0.0;
            double y = 0.0;
            if (_long_press->get_point(sequence, x, y)) {
                update_hover_from_button(x, y);
            }
        }));
        track(_long_press->signal_end().connect([this](Gdk::EventSequence *) {
            if (!_long_press_active) {
                return;
            }
            _long_press_active = false;
            if (_valid && _hovered) {
                choose(*_hovered);
            }
        }));
        track(_long_press->signal_cancel().connect([this](Gdk::EventSequence *) {
            if (_long_press_active) {
                _long_press_active = false;
                reset_session(true);
            }
        }));
        _button.add_controller(long_press);

        // A discoverable alternative for mouse users and a deterministic way
        // to open the same flyout without changing the Shape Builder action.
        auto secondary_click = Gtk::GestureClick::create();
        _secondary_click = secondary_click.get();
        _secondary_click->set_button(GDK_BUTTON_SECONDARY);
        _secondary_click->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
        track(_secondary_click->signal_pressed().connect([this](int, double, double) {
            _secondary_click->set_state(Gtk::EventSequenceState::CLAIMED);
            open();
        }));
        _button.add_controller(secondary_click);

        auto key_controller = Gtk::EventControllerKey::create();
        _key_controller = key_controller.get();
        _key_controller->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
        _key_connection = _key_controller->signal_key_pressed().connect(
            [this](guint keyval, guint, Gdk::ModifierType) {
                if (!_valid) {
                    return false;
                }
                if (keyval == GDK_KEY_Escape) {
                    reset_session(true);
                    return true;
                }
                if ((keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) && _hovered) {
                    _pending = _hovered;
                    commit();
                    return true;
                }
                return false;
            },
            false);
        // Keep this controller inside the flyout's widget subtree. Attaching it
        // to the application window creates a cross-widget lifetime dependency:
        // by the time a managed ToolToolbar is destroyed, its parent window can
        // already be inside GtkWidget::dispose().
        _popover->add_controller(key_controller);
    }

    ~Impl()
    {
        _connections.clear();
        _key_connection.disconnect();
        reset_session(false);
        // The widgets own their controllers. We intentionally keep only raw
        // observer pointers: managed ToolToolbar wrappers can be finalized
        // after GtkWidget::dispose(), when retaining the last controller
        // RefPtr would make gtkmm try to detach it from an already-dead widget.
    }

private:
    void track(sigc::connection connection)
    {
        _connections.emplace_back(std::move(connection));
    }

    std::optional<Operation> operation_at_button_point(double x, double y) const
    {
        if (!_popover->get_visible()) {
            return {};
        }

        auto const origin = Gdk::Graphene::Point(static_cast<float>(x), static_cast<float>(y));
        for (auto const &info : operation_info) {
            auto const operation_button = _operation_buttons[static_cast<unsigned>(info.operation)];
            if (!operation_button) {
                continue;
            }
            auto point = _button.compute_point(*operation_button, origin);
            if (point && operation_button->contains(point->get_x(), point->get_y())) {
                return info.operation;
            }
        }
        return {};
    }

    void update_hover_from_button(double x, double y)
    {
        if (auto operation = operation_at_button_point(x, y)) {
            preview(*operation);
        } else if (!_pending && _hovered) {
            _hovered.reset();
            clear_preview();
            set_status(_("Boolean Assistant: hover an operation to preview; "
                         "click or release to apply, or press Esc to cancel."));
        }
    }

    void configure_operation_buttons(std::size_t operand_count)
    {
        if (operand_count == 2) {
            for (auto const &info : operation_info) {
                auto const index = static_cast<unsigned>(info.operation);
                _operation_labels[index]->set_text(_(info.label));
                _operation_buttons[index]->set_tooltip_text(_(info.tooltip));
            }
            return;
        }

        auto configure = [this](Operation operation, char const *label, char const *tooltip) {
            auto const index = static_cast<unsigned>(operation);
            _operation_labels[index]->set_text(_(label));
            _operation_buttons[index]->set_tooltip_text(_(tooltip));
        };
        configure(Operation::UNION, N_("Union all"), N_("Combine every selected object"));
        configure(Operation::INTERSECTION, N_("Intersect all"), N_("Keep the area shared by every selected object"));
        configure(Operation::A_MINUS_B, N_("Bottom − rest"),
                  N_("Keep the bottom object and subtract the union of all objects above it"));
        configure(Operation::B_MINUS_A, N_("Top − rest"),
                  N_("Keep the top object and subtract the union of all objects below it"));
        configure(Operation::EXCLUSION, N_("Exclude all"),
                  N_("Keep areas covered by an odd number of selected objects"));
    }

    Glib::ustring operation_name(Operation operation) const
    {
        auto const label = _operation_labels[static_cast<unsigned>(operation)];
        return label ? label->get_text() : operation_label(operation);
    }

    void open()
    {
        reset_session(false);
        _desktop = _window ? _window->get_desktop() : nullptr;
        if (!_desktop || !_desktop->getSelection()) {
            return;
        }

        auto items = _desktop->getSelection()->items_vector();
        if (items.size() < 2) {
            _desktop->showNotice(_("Select at least two paths or shapes to use the Boolean Assistant."), 5000);
            return;
        }

        // The first operand is always the bottom object and the last is the top
        // object, independent of selection order.
        std::sort(items.begin(), items.end(), sp_object_compare_position_bool);
        _operands.clear();
        _operands.reserve(items.size());
        for (auto item : items) {
            SPItem *offending = nullptr;
            std::string reason;
            auto operand = make_operand(item, &offending, &reason);
            if (!operand) {
                _operands.clear();
                if (is<SPGroup>(item)) {
                    char const *id = offending ? offending->getId() : nullptr;
                    if (reason == "group-effect") {
                        _desktop->showNotice(
                            Glib::ustring::compose(
                                _("Boolean Assistant cannot combine a group that has a clip, mask, filter, opacity "
                                  "or blend mode (see %1)."),
                                id ? id : "?"),
                            5000);
                    } else {
                        _desktop->showNotice(
                            Glib::ustring::compose(
                                _("Boolean Assistant can combine groups only when every object inside is a visible, "
                                  "unlocked path, shape or text (see %1)."),
                                id ? id : "?"),
                            5000);
                    }
                } else {
                    _desktop->showNotice(_("Boolean Assistant currently supports paths, shapes, and text."), 5000);
                }
                return;
            }
            _operands.emplace_back(std::move(*operand));
        }

        _valid = true;
        _hovered.reset();
        _computed.fill(false);
        for (auto &result : _results) {
            result.clear();
        }
        configure_operation_buttons(_operands.size());

        auto selection = _desktop->getSelection();
        // Undo can delete an operand or a member of a previewed group without changing the selection; the session
        // holds raw SPItem pointers, so it must end the moment any of them is released.
        _operand_watch.clear();
        for (auto const &operand : _operands) {
            _operand_watch.watch(operand.item);
            for (auto *leaf : operand.leaves) {
                _operand_watch.watch(leaf);
            }
        }
        _selection_changed = selection->connectChanged([this](Selection *) { reset_session(true); });
        _selection_modified = selection->connectModified([this](Selection *, unsigned) { reset_session(true); });
        _session_watch.disconnect();
        _session_watch = Glib::signal_timeout().connect(
            [this] {
                if (!_valid) {
                    return false;
                }
                if (!_window || _window->get_desktop() != _desktop) {
                    reset_session(true);
                    return false;
                }
                return true;
            },
            100);

        if (_operands.size() == 2) {
            set_status(_("Boolean Assistant: A is the lower object, B is the upper object. "
                         "Hover an operation to preview; click or release to apply, or press Esc to cancel."));
        } else {
            set_status(Glib::ustring::compose(
                _("Boolean Assistant: %1 objects selected. Boolean operations use all objects; "
                  "subtraction keeps the bottom or top object. Hover to preview."),
                _operands.size()));
        }
        UI::popup_at_center(*_popover, _button);

        // If the popover appears underneath a stationary pointer, no crossing
        // event is guaranteed. Check once after GTK has mapped and allocated it.
        _initial_hover.disconnect();
        _initial_hover = Glib::signal_idle().connect([this] {
            if (_valid && !_pending) {
                for (auto const &info : operation_info) {
                    auto const &motion = _operation_motion[static_cast<unsigned>(info.operation)];
                    if (motion && motion->contains_pointer()) {
                        preview(info.operation);
                        break;
                    }
                }
            }
            return false;
        });
    }

    Geom::PathVector const &result_for(Operation operation)
    {
        auto const index = static_cast<unsigned>(operation);
        if (_computed[index]) {
            return _results[index];
        }

        // A group's silhouette is built here, on the first preview that needs it, not while open() validated the
        // selection. A failure can only be a degenerate operand the eligibility walk already rejected.
        for (auto &operand : _operands) {
            if (!build_operand_path(operand)) {
                _results[index].clear();
                _computed[index] = true;
                return _results[index];
            }
        }

        auto reduce = [this](BooleanOp boolean_op, std::optional<std::size_t> omitted = {}) {
            Geom::PathVector result;
            FillRule result_fill = fill_nonZero;
            bool initialized = false;

            for (std::size_t i = 0; i < _operands.size(); ++i) {
                if (omitted && *omitted == i) {
                    continue;
                }
                auto const &operand = _operands[i];
                if (!initialized || (result.empty() && boolean_op != bool_op_inters)) {
                    result = operand.path;
                    result_fill = operand.fill_rule;
                    initialized = true;
                    continue;
                }
                if (result.empty()) {
                    break; // Empty intersect anything is still empty.
                }
                result = sp_pathvector_boolop(result, operand.path, boolean_op,
                                              result_fill, operand.fill_rule);
                result_fill = fill_nonZero;
            }
            return result;
        };

        auto subtract_others_from = [this, &reduce](std::size_t anchor_index) {
            auto const &anchor = _operands[anchor_index];
            auto mask = reduce(bool_op_union, anchor_index);
            if (mask.empty()) {
                return anchor.path;
            }
            // Livarot's two-path helper computes its second path minus its first.
            return sp_pathvector_boolop(mask, anchor.path, bool_op_diff,
                                        fill_nonZero, anchor.fill_rule);
        };

        switch (operation) {
            case Operation::UNION:
                _results[index] = reduce(bool_op_union);
                break;
            case Operation::INTERSECTION:
                _results[index] = reduce(bool_op_inters);
                break;
            case Operation::A_MINUS_B:
                _results[index] = subtract_others_from(0);
                break;
            case Operation::B_MINUS_A:
                _results[index] = subtract_others_from(_operands.size() - 1);
                break;
            case Operation::EXCLUSION:
                _results[index] = reduce(bool_op_symdiff);
                break;
            case Operation::COUNT:
                g_assert_not_reached();
        }
        _computed[index] = true;
        return _results[index];
    }

    void preview(Operation operation)
    {
        if (!_valid || !_desktop || _pending) {
            return;
        }

        if (_hovered == operation) {
            return;
        }

        _hovered = operation;
        _preview.reset();
        dim_operands();
        auto const &result = result_for(operation);
        if (!result.empty()) {
            _preview = make_canvasitem<CanvasItemBpath>(_desktop->getCanvasTemp(), result, false);
            _preview->set_fill(0x2f80eddd, SP_WIND_RULE_NONZERO);
            _preview->set_stroke(0xffffffff);
            _preview->set_stroke_width(1.25);
        }
        set_status(Glib::ustring::compose(_("Previewing %1. Click to apply; move away to restore."),
                                          operation_name(operation)));
    }

    void choose(Operation operation)
    {
        if (!_valid || !_desktop) {
            return;
        }
        preview(operation);
        _pending = operation;
        commit();
    }

    void commit()
    {
        if (!_pending || !_desktop) {
            return;
        }

        auto const operation = *_pending;
        auto selection = _desktop->getSelection();
        // The operation itself deletes operands; that is not a stale-session event.
        _operand_watch.clear();
        _selection_changed.disconnect();
        _selection_modified.disconnect();
        clear_preview();
        clear_status();
        _hovered.reset();
        _valid = false;

        auto const operand_count = selection->items_vector().size();
        auto const assist_op = boolean_assist_op(operation);
        auto *const result = apply_boolean_assist(*selection, assist_op);
        if (result) {
            auto const label = boolean_assist_undo_label(assist_op, operand_count);
            DocumentUndo::done(_desktop->getDocument(), label.first, label.second);
        } else {
            DocumentUndo::cancel(_desktop->getDocument());
            _desktop->showNotice(_("The boolean operation did not produce one path; nothing was changed."), 5000);
        }

        // Keep _pending set while the popover closes so signal_closed() does
        // not tear down the session before the document operation is applied.
        _popover->popdown();
        _pending.reset();
        _desktop = nullptr;
    }

    void dim_operands()
    {
        if (!_dimmed.empty() || !_desktop) {
            return;
        }
        for (auto const &operand : _operands) {
            if (auto drawing_item = operand.item->get_arenaitem(_desktop->dkey)) {
                drawing_item->setOpacityOverride(0.15);
                _dimmed.push_back(operand.item);
            }
        }
    }

    void clear_preview()
    {
        _preview.reset();
        if (_desktop) {
            for (auto item : _dimmed) {
                if (auto drawing_item = item->get_arenaitem(_desktop->dkey)) {
                    drawing_item->setOpacityOverride({});
                }
            }
        }
        _dimmed.clear();
    }

    void set_status(Glib::ustring const &message)
    {
        clear_status();
        if (_desktop) {
            _status = _desktop->messageStack()->push(INFORMATION_MESSAGE, message.c_str());
        }
    }

    void clear_status()
    {
        if (_desktop && _status) {
            _desktop->messageStack()->cancel(*_status);
        }
        _status.reset();
    }

    // A watched operand or group leaf was released (Undo, delete, reconstruction). Forget every raw pointer now,
    // and finish tearing the session down once the document operation that released it has returned.
    void on_operand_released(SPItem *released)
    {
        // Every other operand of this Undo is still alive right now: restore their opacity and drop _dimmed
        // entirely, so the later idle reset cannot touch an operand freed in the same Undo. The released item
        // is still valid during its release signal.
        (void)released;
        clear_preview();
        _operands.clear();
        _valid = false;
        _hovered.reset();
        _pending.reset();
        _results = {};
        _computed.fill(false);
        _release_reset.disconnect();
        _release_reset = Glib::signal_idle().connect([this] {
            reset_session(true);
            return false;
        });
    }

    void reset_session(bool close_popover)
    {
        _operand_watch.clear();
        _operands.clear();
        _release_reset.disconnect();
        _long_press_active = false;
        _initial_hover.disconnect();
        _session_watch.disconnect();
        _selection_changed.disconnect();
        _selection_modified.disconnect();
        clear_preview();
        clear_status();
        _hovered.reset();
        _pending.reset();
        _valid = false;
        if (close_popover && _popover && _popover->get_visible()) {
            _popover->popdown();
        }
        _desktop = nullptr;
    }

    Gtk::ToggleButton &_button;
    InkscapeWindow *_window = nullptr;
    SPDesktop *_desktop = nullptr;
    Gtk::Popover *_popover = nullptr; // managed child of the toolbar
    Gtk::GestureLongPress *_long_press = nullptr;
    Gtk::GestureClick *_secondary_click = nullptr;
    Gtk::EventControllerKey *_key_controller = nullptr;
    std::array<Gtk::Button *, operation_count> _operation_buttons{};
    std::array<Gtk::Label *, operation_count> _operation_labels{};
    std::array<Gtk::EventControllerMotion *, operation_count> _operation_motion{};
    std::vector<sigc::scoped_connection> _connections;
    sigc::scoped_connection _key_connection;
    sigc::scoped_connection _initial_hover;
    sigc::scoped_connection _session_watch;
    sigc::scoped_connection _selection_changed;
    sigc::scoped_connection _selection_modified;
    sigc::scoped_connection _release_reset;
    ItemReleaseWatch _operand_watch;

    std::vector<Operand> _operands;
    std::array<Geom::PathVector, operation_count> _results;
    std::array<bool, operation_count> _computed{};
    std::optional<Operation> _hovered;
    std::optional<Operation> _pending;
    std::optional<MessageId> _status;
    CanvasItemPtr<CanvasItemBpath> _preview;
    std::vector<SPItem *> _dimmed;
    bool _long_press_active = false;
    bool _valid = false;
};

BooleanAssist::BooleanAssist(Gtk::ToggleButton &button, Gtk::Widget &popover_parent, InkscapeWindow *window)
    : _impl(std::make_unique<Impl>(button, popover_parent, window))
{}

BooleanAssist::~BooleanAssist() = default;

} // namespace Inkscape::UI::Toolbar
