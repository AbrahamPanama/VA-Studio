// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * A panel for listing objects in a document.
 *
 * Authors:
 *   Martin Owens
 *   Mike Kowalski
 *   Adam Belis (UX/Design)
 *
 * Copyright (C) Authors 2020-2022
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "objects.h"

#include <glibmm/main.h>
#include <gtkmm/dragsource.h>
#include <gtkmm/droptarget.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/eventcontrollermotion.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/scale.h>
#include <gtkmm/searchentry2.h>
#include <gtkmm/separator.h>
#include <gtkmm/treestore.h>

#include "desktop-style.h"
#include "desktop.h"
#include "display/translucency-group.h"
#include "document-undo.h"
#include "document.h"
#include "filter-chemistry.h"
#include "inkscape-window.h"
#include "layer-manager.h"
#include "object/object-set.h"
#include "object/sp-root.h"
#include "style.h"
#include "svg/css-ostringstream.h"
#include "ui/builder-utils.h"
#include "ui/contextmenu.h"
#include "ui/controller.h"
#include "ui/icon-names.h"
#include "ui/shortcuts.h"
#include "ui/util.h"
#include "ui/widget-vfuncs-class-init.h"
#include "ui/widget/canvas.h"
#include "ui/widget/filter-effect-chooser.h"
#include "ui/widget/imagetoggler.h"
#include "ui/widget/objects-dialog-cells.h"
#include "ui/widget/shapeicon.h"
#include "util/numeric/converters.h"
#include "xml/document.h"

// alpha (transparency) multipliers corresponding to item selection state combinations (SelectionState)
// when 0 - do not color item's background
static double const SELECTED_ALPHA[16] = {
    0.00, //00 not selected
    0.90, //01 selected
    0.50, //02 layer focused
    0.20, //03 layer focused & selected
    0.00, //04 child of focused layer
    0.90, //05 selected child of focused layer
    0.50, //06 2 and 4
    0.90, //07 1, 2 and 4
    0.40, //08 child of selected group
    0.90, //09 1 and 8
    0.40, //10 2 and 8
    0.90, //11 1, 2 and 8
    0.40, //12 4 and 8
    0.90, //13 1, 4 and 8
    0.40, //14 2, 4 and 8
    0.90, //15 1, 2 , 4 and 8
};

static double const HOVER_ALPHA = 0.10;

namespace Inkscape::UI::Dialog {

namespace {

void connect_on_window_when_mapped(Glib::RefPtr<Gtk::EventController> controller, Gtk::Widget &widget)
{
    auto const on_map = [controller, &widget] {
        auto& window = dynamic_cast<Gtk::Window &>(*widget.get_root());
        window.add_controller(controller);
    };
    auto const on_unmap = [controller, &widget] {
        auto& window = dynamic_cast<Gtk::Window &>(*widget.get_root());
        window.remove_controller(controller);
    };
    widget.signal_map().connect(on_map);
    widget.signal_unmap().connect(on_unmap);
}

} // namespace

using Inkscape::XML::Node;
using namespace Inkscape::UI::Widget;

// This was the 1 widget where we used signal_style_updated(), so just hack together a replacement!
class ObjectsPanel::TreeViewWithCssChanged final
    : public WidgetVfuncsClassInit
    , public Gtk::TreeView
{
public:
    TreeViewWithCssChanged()
        : Glib::ObjectBase{"TreeViewWithCssChanged"}
        , WidgetVfuncsClassInit{}
        , Gtk::TreeView{}
    {
    }

    auto connect_css_changed(sigc::slot<void (GtkCssStyleChange *)> slot)
    {
        return _signal.connect(std::move(slot));
    }

    bool iter_next(Gtk::TreePath &path)
    {
        auto model = get_model();
        auto c_model = model->gobj();
        auto iter = model->get_iter(path);
        auto c_iter = iter.gobj();
        if (gtk_tree_model_iter_next(c_model, c_iter)) {
            path.next();
            return true;
        }
        return false;
    }

    Gtk::TreePath iter_last_child(Gtk::TreePath &path)
    {
        auto model = get_model();
        auto c_model = model->gobj();
        auto iter = model->get_iter(path);
        auto c_iter = iter.gobj();
        auto count = std::to_string(gtk_tree_model_iter_n_children(c_model, c_iter) - 1);
        auto new_path = path.to_string() + ":" + count;
        return Gtk::TreePath(new_path);
    }

private:
    sigc::signal<void (GtkCssStyleChange *)> _signal;

    void css_changed(GtkCssStyleChange * const change) final
    {
        _signal.emit(change);
    }
};

// A checkpoint is copied before calling GTK. It remains readable even when a
// callback closes the desktop/document (or destroys the panel). Never throw
// through a GTK callback: checks run only after the emitting GTK call returns.
struct ObjectsPanelRefreshState {
    bool valid = true;
    bool alive = true;
};
namespace {
struct RefreshInterrupted {};
struct RefreshCheckpoint {
    std::shared_ptr<ObjectsPanelRefreshState> state;
    void operator()() const { if (state && !state->valid) throw RefreshInterrupted{}; }
};
}

// Membership observation must not depend on whether a row was materialized.
class ObjectsPanelDocumentObserver final : public XML::NodeObserver {
public:
    ObjectsPanelDocumentObserver(ObjectsPanel &panel, Node &root) : panel(panel), root(root) {
        GC::anchor(&root);
        root.addSubtreeObserver(*this);
    }
    ~ObjectsPanelDocumentObserver() override {
        root.removeSubtreeObserver(*this);
        GC::release(&root);
    }
    void changed(bool membership) {
        if (panel._flush_state) panel._flush_state->valid = false;
        if (membership && panel.root_watcher && filtered()) refresh();
    }
    void notifyChildAdded(Node &, Node &, Node *) override { changed(true); }
    void notifyChildRemoved(Node &, Node &, Node *) override { changed(true); }
    void notifyChildOrderChanged(Node &, Node &, Node *, Node *) override { changed(true); }
    void notifyAttributeChanged(Node &, GQuark name, Util::ptr_shared, Util::ptr_shared) override {
        changed(name == g_quark_from_static_string("id") ||
                name == g_quark_from_static_string("inkscape:label") ||
                name == g_quark_from_static_string("inkscape:groupmode"));
    }
    void notifyElementNameChanged(Node &, GQuark, GQuark) override { changed(true); }
    void notifyContentChanged(Node &, Util::ptr_shared, Util::ptr_shared) override { changed(false); }
private:
    bool filtered() const;
    void refresh();
    ObjectsPanel &panel;
    Node &root;
};

class ObjectWatcher : public Inkscape::XML::NodeObserver
{
public:
    ObjectWatcher(ObjectsPanel *panel, SPItem *, Gtk::TreeRow *row, bool is_filtered);
    ~ObjectWatcher() override;

    void stopWatching();
    void initRowInfo();
    void updateRowInfo();
    void updateRowHighlight();
    void updateRowAncestorState(bool invisible, bool locked);
    void updateRowBg(guint32 rgba = 0.0);

    ObjectWatcher *findChild(Node *node);
    void addDummyChild();
    bool addChild(SPItem *, bool dummy = true);
    void addChildren(SPItem *, bool dummy = false);
    void setSelectedBit(SelectionState mask, bool enabled);
    void setSelectedBitRecursive(SelectionState mask, bool enabled);
    void setSelectedBitChildren(SelectionState mask, bool enabled);
    void rememberExtendedItems();
    void rebuildChildren();
    void rememberMaterializedItems();
    void syncSelection(bool inherited = false);
    void rememberExpansion(bool value) { expanded = value; }
    bool isFiltered() const { return is_filtered; }

    Gtk::TreeNodeChildren getChildren() const;
    Gtk::TreeModel::iterator getChildIter(Node *) const;

    void notifyChildRemoved(Node &, Node &, Node *) final;
    void notifyChildOrderChanged(Node &, Node &child, Node *, Node *) final;
    void notifyChildAdded(Node &, Node &, Node *) final;
    void notifyAttributeChanged(Node &, GQuark, Util::ptr_shared, Util::ptr_shared) final;

    // GtkTreeStore guarantees persistent iterators until their row is erased.
    // Unlike RowReference these do not traverse every sibling on each insertion.
    void setRow(Gtk::TreeModel::Row row) { row_iter = row.get_iter(); }
    Gtk::TreeModel::Path getTreePath() const {
        return row_iter ? panel->_store->get_path(row_iter) : Gtk::TreeModel::Path{};
    }
    bool hasRow() const { return bool(row_iter); }

    /// Transfer a child watcher to its new parent
    void transferChild(Node *childnode)
    {
        auto *target = panel->getWatcher(childnode->parent());
        assert(target != this);
        auto nh = child_watchers.extract(childnode);
        assert(nh);
        bool inserted = target->child_watchers.insert(std::move(nh)).inserted;
        assert(inserted);
    }

    /// The XML node associated with this watcher.
    Node *getRepr() const { return node; }
    std::optional<Gtk::TreeRow> getRow() const {
        if (row_iter) return *row_iter;
        return std::nullopt;
    }

    std::unordered_map<Node const *, std::unique_ptr<ObjectWatcher>> child_watchers;

private:
    Node *node;
    Gtk::TreeModel::iterator row_iter;
    ObjectsPanel *panel;
    SelectionState selection_state;
    bool is_filtered;
    bool expanded = false;
    bool children_materialized = false;
    bool variable_height = false;
    bool watching = false;
};

bool ObjectsPanelDocumentObserver::filtered() const { return panel.root_watcher->isFiltered(); }
void ObjectsPanelDocumentObserver::refresh() { panel.queueStructuralRefresh(panel.root_watcher.get()); }

class ObjectsPanel::ModelColumns final : public Gtk::TreeModel::ColumnRecord
{
public:
    ModelColumns()
    {
        add(_colNode);
        add(_colLabel);
        add(_colType);
        add(_colIconColor);
        add(_colClipMask);
        add(_colBgColor);
        add(_colInvisible);
        add(_colLocked);
        add(_colAncestorInvisible);
        add(_colAncestorLocked);
        add(_colHover);
        add(_colItemStateSet);
        add(_colBlendMode);
        add(_colOpacity);
        add(_colItemState);
        add(_colHoverColor);
        add(_colIconsVisible);
    }

    Gtk::TreeModelColumn<Node*> _colNode;
    Gtk::TreeModelColumn<Glib::ustring> _colLabel;
    Gtk::TreeModelColumn<Glib::ustring> _colType;
    Gtk::TreeModelColumn<unsigned int> _colIconColor;
    Gtk::TreeModelColumn<unsigned int> _colClipMask;
    Gtk::TreeModelColumn<Gdk::RGBA> _colBgColor;
    Gtk::TreeModelColumn<bool> _colInvisible;
    Gtk::TreeModelColumn<bool> _colLocked;
    Gtk::TreeModelColumn<bool> _colAncestorInvisible;
    Gtk::TreeModelColumn<bool> _colAncestorLocked;
    Gtk::TreeModelColumn<bool> _colHover;
    Gtk::TreeModelColumn<bool> _colItemStateSet;
    Gtk::TreeModelColumn<SPBlendMode> _colBlendMode;
    Gtk::TreeModelColumn<double> _colOpacity;
    Gtk::TreeModelColumn<Glib::ustring> _colItemState;
    // Set when hovering over the color tag cell
    Gtk::TreeModelColumn<bool> _colHoverColor;
    Gtk::TreeModelColumn<bool> _colIconsVisible;
};

/**
 * Creates a new ObjectWatcher, a gtk TreeView iterated watching device.
 *
 * @param panel The panel to which the object watcher belongs
 * @param obj The SPItem to watch in the document
 * @param row The optional list store tree row for the item,
          if not provided, assumes this is the root 'document' object.
 * @param filtered, if true this watcher will filter all chldren using the panel filtering function on each item to decide if it should be shown.
 */
ObjectWatcher::ObjectWatcher(ObjectsPanel* panel, SPItem* obj, Gtk::TreeRow *row, bool filtered)
    : panel(panel)
    , row_iter()
    , selection_state(0)
    , is_filtered(filtered)
    , node(obj->getRepr())
{
    GC::anchor(node);
    node->addObserver(*this);
    watching = true;
    auto state = panel->_flush_state;
    try {
        if (auto id = obj->getId(); id && panel->_expanded_items.count(id)) obj->setExpanded(true);
        expanded = obj->isExpanded();
        if (auto id = obj->getId(); id && panel->_collapsed_items.count(id)) expanded = false;
        if (auto selection = panel->getSelection()) {
            if (selection->includes(obj)) selection_state |= SELECTED_OBJECT;
            if (selection->includes(obj, true)) selection_state |= GROUP_SELECT_CHILD;
        }
        if(row != nullptr) {
            assert(row->children().empty());
            setRow(*row);
            initRowInfo();
            updateRowInfo();
        }

        // Only show children for groups (and their subclasses like SPAnchor or SPRoot)
        if (!is<SPGroup>(obj)) {
            return;
        }

        // Add children as a dummy row to avoid excensive execution when
        // the tree is really large, but not in layers mode.
        auto id = obj->getId();
        bool remembered = id && panel->_materialized_items.count(id);
        addChildren(obj, (bool)row && !expanded && !remembered);
    } catch (...) {
        if (variable_height && (!state || state->alive)) --panel->_variable_height_rows;
        if (watching) node->removeObserver(*this);
        GC::release(node);
        throw;
    }
}

ObjectWatcher::~ObjectWatcher()
{
    // Destruction unregisters synchronously, but never erases individual rows.
    // The owning parent clears its rows once after all descendants are gone.
    if (!panel->_rebuilding && !panel->_dirty_parents.empty()) {
        if (auto id = node->attribute("id")) {
            if (expanded) panel->_expanded_items.insert(id);
            if (children_materialized) panel->_materialized_items.insert(id);
        }
    }
    if (variable_height) --panel->_variable_height_rows;
    panel->_dirty_parents.erase(this);
    stopWatching();
    child_watchers.clear();
    GC::release(node);
}

void ObjectWatcher::stopWatching()
{
    if (!watching) return;
    node->removeObserver(*this);
    watching = false;
    for (auto const &entry : child_watchers) entry.second->stopWatching();
}

void ObjectWatcher::rememberMaterializedItems()
{
    if (auto id = node->attribute("id")) {
        (expanded ? panel->_expanded_items : panel->_collapsed_items).insert(id);
    }
    if (children_materialized) {
        if (auto id = node->attribute("id")) panel->_materialized_items.insert(id);
    }
    for (auto const &entry : child_watchers) entry.second->rememberMaterializedItems();
}

void ObjectWatcher::rebuildChildren()
{
    RefreshCheckpoint check{panel->_flush_state};
    child_watchers.clear();
    auto children = getChildren();
    while (!children.empty()) { panel->_store->erase(children.begin()); check(); }
    if (auto item = cast<SPItem>(panel->getObject(node)); item && is<SPGroup>(item)) {
        addChildren(item, hasRow() && !expanded && !children_materialized);
    }
}

void ObjectWatcher::initRowInfo()
{
    RefreshCheckpoint check{panel->_flush_state};
    auto const _model = panel->_model.get();
    auto row = *row_iter;
    row[_model->_colHover] = false; check();
    row[_model->_colBgColor] = Gdk::RGBA(); check();
}

/**
 * Update the information in the row from the stored node
 */
void ObjectWatcher::updateRowInfo()
{
    RefreshCheckpoint check{panel->_flush_state};
    if (auto item = cast<SPItem>(panel->getObject(node))) {
        assert(row_iter);

        auto const _model = panel->_model.get();
        auto row = *row_iter;
        row[_model->_colNode] = node; check();

        // show ids without "#"
        char const *id = item->getId();
        auto label = id && !item->label() ? get_synthetic_object_name(item) : item->defaultLabel();
        // Plain single-line ASCII uses one shared font and uniform icon cells.
        // Multiline/control characters and font-fallback text retain GTK's full
        // variable-height layout; do not clip labels to obtain faster timings.
        bool variable = std::any_of(label.begin(), label.end(), [](auto c) { return c < 32 || c >= 127; });
        if (variable != variable_height) {
            if (variable) ++panel->_variable_height_rows;
            else --panel->_variable_height_rows;
            variable_height = variable;
            panel->updateRowHeightMode(); check();
        }
        row[_model->_colLabel] = label; check();

        row[_model->_colType] = item->typeName(); check();
        row[_model->_colClipMask] =
            (item->getClipObject() ? Inkscape::UI::Widget::OVERLAY_CLIP : 0) |
            (item->getMaskObject() ? Inkscape::UI::Widget::OVERLAY_MASK : 0);
        row[_model->_colInvisible] = item->isHidden(); check();
        row[_model->_colLocked] = !item->isSensitive(); check();
        auto blend = item->style && item->style->mix_blend_mode.set ? item->style->mix_blend_mode.value : SP_CSS_BLEND_NORMAL;
        row[_model->_colBlendMode] = blend; check();
        auto opacity = 1.0;
        if (item->style && item->style->opacity.set) {
            opacity = item->style->opacity.as_double();
        }
        row[_model->_colOpacity] = opacity; check();
        std::string item_state;
        if (opacity == 0.0) {
            item_state = "object-transparent";
        }
        else if (blend != SP_CSS_BLEND_NORMAL) {
            item_state = opacity == 1.0 ? "object-blend-mode" : "object-translucent-blend-mode";
        }
        else if (opacity < 1.0) {
            item_state = "object-translucent";
        }
        row[_model->_colItemState] = item_state; check();
        row[_model->_colItemStateSet] = !item_state.empty(); check();

        updateRowHighlight();
        updateRowAncestorState(row[_model->_colAncestorInvisible], row[_model->_colAncestorLocked]);
    }
}

/**
 * Propagate changes to the highlight color to all children.
 */
void ObjectWatcher::updateRowHighlight() {
    RefreshCheckpoint check{panel->_flush_state};

    if (!hasRow()) {
        std::cerr << "ObjectWatcher::updateRowHighlight: no row_iter: " << node->name() << std::endl;
        return;
    }

    if (auto item = cast<SPItem>(panel->getObject(node))) {
        auto row = *row_iter;
        auto new_color = item->highlight_color().toRGBA();
        if (new_color != row[panel->_model->_colIconColor]) {
            row[panel->_model->_colIconColor] = new_color; check();
            updateRowBg(new_color);
            for (auto &watcher : child_watchers) {
                watcher.second->updateRowHighlight();
            }
        }
    }
}

/**
 * Propagate a change in visibility or locked state to all children
 */
void ObjectWatcher::updateRowAncestorState(bool invisible, bool locked) {
    RefreshCheckpoint check{panel->_flush_state};
    auto const _model = panel->_model.get();
    auto row = *row_iter;
    row[_model->_colAncestorInvisible] = invisible; check();
    row[_model->_colAncestorLocked] = locked; check();
    for (auto &watcher : child_watchers) {
        watcher.second->updateRowAncestorState(
            invisible || row[_model->_colInvisible],
            locked || row[_model->_colLocked]);
    }
}

Gdk::RGBA selection_color;

/**
 * Updates the row's background colour as indicated by its selection.
 */
void ObjectWatcher::updateRowBg(guint32 rgba)
{
    RefreshCheckpoint check{panel->_flush_state};
    assert(row_iter);
    if (auto row = *row_iter) {
        auto alpha = SELECTED_ALPHA[selection_state];
        if (alpha == 0.0) {
            if (row[panel->_model->_colBgColor] != Gdk::RGBA()) {
                row[panel->_model->_colBgColor] = Gdk::RGBA(); check();
            }
            return;
        }

        const auto& sel = selection_color;
        const auto gdk_color = change_alpha(sel, sel.get_alpha() * alpha);
        // Multiple selection bits can represent the same visible color. GTK
        // emits row-changed (and computes its path) even for equal values.
        if (row[panel->_model->_colBgColor] != gdk_color) {
            row[panel->_model->_colBgColor] = gdk_color; check();
        }
    }
}

/**
 * Flip the selected state bit on or off as needed, calls updateRowBg if changed.
 *
 * @param mask - The selection bit to set or unset
 * @param enabled - If the bit should be set or unset
 */
void ObjectWatcher::setSelectedBit(SelectionState mask, bool enabled) {
    if (!row_iter) return;
    SelectionState value = selection_state;
    SelectionState original = value;
    if (enabled) {
        value |= mask;
    } else {
        value &= ~mask;
    }
    if (value != original) {
        selection_state = value;
        updateRowBg();
    }
}

/**
 * Flip the selected state bit on or off as needed, on this watcher and all
 * its direct and indirect children.
 */
void ObjectWatcher::setSelectedBitRecursive(SelectionState mask, bool enabled)
{
    RefreshCheckpoint check{panel->_flush_state};
    setSelectedBit(mask, enabled);
    setSelectedBitChildren(mask, enabled);
}
void ObjectWatcher::setSelectedBitChildren(SelectionState mask, bool enabled)
{
    RefreshCheckpoint check{panel->_flush_state};
    for (auto &pair : child_watchers) {
        pair.second->setSelectedBitRecursive(mask, enabled);
    }
}

// Compute the final selection mask in one traversal. Clearing then setting
// bits emits unnecessary row-changed notifications with O(sibling-count) paths.
// Newly rebuilt rows already have their final selection color at insertion time.
void ObjectWatcher::syncSelection(bool inherited)
{
    RefreshCheckpoint check{panel->_flush_state};
    auto object = panel->getObject(node);
    auto selection = panel->getSelection();
    bool selected = object && selection && selection->includes(object);
    bool grouped = inherited || selected;
    auto state = (selection_state & ~(SELECTED_OBJECT | GROUP_SELECT_CHILD)) |
                 (selected ? SELECTED_OBJECT : 0) | (grouped ? GROUP_SELECT_CHILD : 0);
    if (state != selection_state) {
        selection_state = state;
        if (hasRow()) updateRowBg();
    }
    for (auto const &entry : child_watchers) entry.second->syncSelection(grouped);
}

/**
 * Keep expanded rows expanded and recurse through all children.
 */
void ObjectWatcher::rememberExtendedItems()
{
    RefreshCheckpoint check{panel->_flush_state};
    if (auto item = cast<SPItem>(panel->getObject(node))) {
        if (hasRow() && expanded)
            panel->_tree.expand_row(getTreePath(), false);
        check();
    }
    for (auto &pair : child_watchers) {
        pair.second->rememberExtendedItems(); check();
    }
}

/**
 * Find the child watcher for the given node.
 */
ObjectWatcher *ObjectWatcher::findChild(Node *node)
{
    auto it = child_watchers.find(node);
    if (it != child_watchers.end()) {
        return it->second.get();
    }
    return nullptr;
}

/**
 * Add the child object to this node.
 *
 * @param child - SPObject to be added
 * @param dummy - Add a dummy objects (hidden) instead
 *
 * @returns true if child added was a dummy objects
 */
bool ObjectWatcher::addChild(SPItem *child, bool dummy)
{
    RefreshCheckpoint check{panel->_flush_state};
    if (is_filtered && !panel->showChildInTree(child)) {
        return false;
    }

    auto children = getChildren();
    if (!is_filtered && dummy && row_iter) {
        if (children.empty()) {
            auto const iter = panel->_store->append(children); check();
            assert(panel->isDummy(*iter));
            return true;
        } else if (panel->isDummy(children[0])) {
            return false;
        }
    }

    auto *node = child->getRepr();
    assert(node);
    auto iter = panel->_store->prepend(children); check();
    Gtk::TreeModel::Row row = *iter;

    // Ancestor states are handled inside the list store (so we don't have to re-ask every update)
    auto const _model = panel->_model.get();
    if (row_iter) {
        auto parent_row = *row_iter;
        row[_model->_colAncestorInvisible] = parent_row[_model->_colAncestorInvisible] || parent_row[_model->_colInvisible]; check();
        row[_model->_colAncestorLocked] = parent_row[_model->_colAncestorLocked] || parent_row[_model->_colLocked]; check();
    } else {
        row[_model->_colAncestorInvisible] = false; check();
        row[_model->_colAncestorLocked] = false; check();
    }

    // Publish only a fully initialized watcher: GTK callbacks during its
    // constructor must never encounter a null entry in the ownership map.
    auto owned = std::make_unique<ObjectWatcher>(panel, child, &row, is_filtered);
    auto watcher = owned.get();
    auto inserted = child_watchers.emplace(node, std::move(owned)).second;
    assert(inserted);

    // Make sure new children have the right focus set.
    if ((selection_state & LAYER_FOCUSED) != 0) {
        watcher->setSelectedBit(LAYER_FOCUS_CHILD, true);
    }
    return false;
}

/**
 * Add all SPItem children as child rows.
 */
void ObjectWatcher::addChildren(SPItem *obj, bool dummy)
{
    RefreshCheckpoint check{panel->_flush_state};
    assert(child_watchers.empty());
    children_materialized = !dummy || is_filtered;

    // Prepending SVG's forward order produces the final reverse display order.
    // Rows use persistent iterators, not shifting row references, and their
    // initial values are written while their sibling index is zero. Appending
    // would make GTK's row-changed path construction scan every prior sibling.
    for (auto &child : obj->children) {
        if (auto item = cast<SPItem>(&child)) {
            if (addChild(item, dummy) && dummy) break;
        }
    }
}

/**
 * Get the TreeRow's children iterator
 *
 * @returns Gtk Tree Node Children iterator
 */
Gtk::TreeNodeChildren ObjectWatcher::getChildren() const
{
    return row_iter ? row_iter->children() : panel->_store->children();
}

/**
 * Convert SPObject to TreeView Row, assuming the object is a child.
 *
 * @param child - The child object to find in this branch
 * @returns Gtk TreeRow for the child, or end() if not found
 */
Gtk::TreeModel::iterator ObjectWatcher::getChildIter(Node *node) const
{
    auto childrows = getChildren();

    if (!node) {
        return childrows.end();
    }

    for (auto &row : childrows) {
        if (panel->getRepr(row) == node) {
            return row.get_iter();
        }
    }
    // In layer mode, we will come here for all non-layers
    return childrows.begin();
}

void ObjectWatcher::notifyChildAdded(Node &parent, Node &, Node *)
{
    assert(node == &parent);
    panel->queueStructuralRefresh(this);
}
void ObjectWatcher::notifyChildRemoved(Node &parent, Node &child, Node *)
{
    assert(node == &parent);
    panel->queueStructuralRefresh(this);
    // No removed XML node survives in the queue or in registered observers.
    if (panel->_flushing) {
        // Keep an executing watcher alive until the stack has unwound, but stop
        // observing removed XML immediately. The cancelled flush resets the tree.
        if (auto watcher = findChild(&child)) watcher->stopWatching();
    } else {
        child_watchers.erase(&child);
    }
}
void ObjectWatcher::notifyChildOrderChanged(Node &parent, Node &, Node *, Node *)
{
    assert(node == &parent);
    panel->queueStructuralRefresh(this);
}
void ObjectWatcher::notifyAttributeChanged( Node &node, GQuark name, Util::ptr_shared /*old_value*/, Util::ptr_shared /*new_value*/ )
{
    assert(this->node == &node);
    if (panel->_flushing) {
        panel->queueStructuralRefresh(this);
        return;
    }
    if (is_filtered && (name == g_quark_from_static_string("id") ||
                        name == g_quark_from_static_string("inkscape:label") ||
                        name == g_quark_from_static_string("inkscape:groupmode"))) {
        panel->queueStructuralRefresh(this);
        return;
    }

    // The root <svg> node doesn't have a row
    if (this == panel->getRootWatcher()) {
        return;
    }

    // Almost anything could change the icon, so update upon any change, defer for lots of updates.

    // examples of not-so-obvious cases:
    // - width/height: Can change type "circle" to an "ellipse"

    static std::set<GQuark> const excluded{
        g_quark_from_static_string("transform"),
        g_quark_from_static_string("x"),
        g_quark_from_static_string("y"),
        g_quark_from_static_string("d"),
        g_quark_from_static_string("sodipodi:nodetypes"),
    };

    if (excluded.count(name)) {
        return;
    }

    updateRowInfo();
}

/**
 * Get the object from the node.
 *
 * @param node - XML Node involved in the signal.
 * @returns SPObject matching the node, returns nullptr if not found.
 */
SPObject *ObjectsPanel::getObject(Node *node) {
    if (node != nullptr && getDocument())
        return getDocument()->getObjectByRepr(node);
    return nullptr;
}

/**
 * Get the object watcher from the xml node (reverse lookup), it uses a ancesstor
 * recursive pattern to match up with the root_watcher.
 *
 * @param node - The node to look up.
 * @return the ObjectWatcher object if it's possible to find.
 */
ObjectWatcher* ObjectsPanel::getWatcher(Node *node)
{
    assert(node);

    if (!root_watcher) return nullptr;

    if (root_watcher->getRepr() == node) {
        return root_watcher.get();
    }

    if (node->parent()) {
        if (auto parent_watcher = getWatcher(node->parent())) {
            return parent_watcher->findChild(node);
        }
    }

    return nullptr;
}

/**
 * Constructor
 */
ObjectsPanel::ObjectsPanel()
    : DialogBase("/dialogs/objects", "Objects")
    , _model{std::make_unique<ModelColumns>()}
    , _layer(nullptr)
    , _is_editing(false)
    , _page(Gtk::Orientation::VERTICAL)
    , _builder(create_builder("dialog-objects.glade"))
    , _settings_menu(get_widget<Gtk::Popover>(_builder, "settings-menu"))
    , _object_menu(get_widget<Gtk::Popover>(_builder, "object-menu"))
    , _colors(std::make_shared<Colors::ColorSet>(nullptr, false))
    , _searchBox(get_widget<Gtk::SearchEntry2>(_builder, "search"))
    , _opacity_slider(get_widget<Gtk::Scale>(_builder, "opacity-slider"))
    , _setting_layers(get_derived_widget<PrefCheckButton, Glib::ustring, bool>(_builder, "setting-layers", "/dialogs/objects/layers_only", false))
    , _setting_track(get_derived_widget<PrefCheckButton, Glib::ustring, bool>(_builder, "setting-track", "/dialogs/objects/expand_to_layer", true))
    , _tree{*Gtk::make_managed<TreeViewWithCssChanged>()}
{
    _store = Gtk::TreeStore::create(*_model);

    //Set up the tree
    _tree.set_model(_store);
    _tree.set_headers_visible(false);
    _tree.set_name("ObjectsTreeView");

    auto& header = get_widget<Gtk::Box>(_builder, "header");
    // Search
    _searchBox.signal_search_changed().connect(sigc::mem_fun(*this, &ObjectsPanel::_searchActivated));

    // Buttons
    auto& _move_up_button = get_widget<Gtk::Button>(_builder, "move-up");
    auto& _move_down_button = get_widget<Gtk::Button>(_builder, "move-down");
    auto& _object_delete_button = get_widget<Gtk::Button>(_builder, "remove-object");

    // Connect with modifier state support, so we can move items to the top/bottom with an Alt modifier depressed
    Inkscape::UI::connect_click_with_state(_move_up_button, [this](Gdk::ModifierType state) {
        if (Controller::has_flag(state, Gdk::ModifierType::ALT_MASK)) {
            _activateAction("win.layer-top", "selection-top");
        } else {
            _activateAction("win.layer-raise", "selection-stack-up");
        }
    });
    Inkscape::UI::connect_click_with_state(_move_down_button, [this](Gdk::ModifierType state) {
        if (Controller::has_flag(state, Gdk::ModifierType::ALT_MASK)) {
            _activateAction("win.layer-bottom", "selection-bottom");
        } else {
            _activateAction("win.layer-lower", "selection-stack-down");
        }
    });

    _object_delete_button.signal_clicked().connect([this]() {
        _activateAction("win.layer-delete", "delete-selection");
    });

    //Label
    _name_column = Gtk::make_managed<Gtk::TreeViewColumn>();
    _text_renderer = Gtk::make_managed<Gtk::CellRendererText>();
    _text_renderer->property_editable() = true;
    _text_renderer->property_ellipsize().set_value(Pango::EllipsizeMode::END);
    _text_renderer->signal_editing_started().connect([this](Gtk::CellEditable*,const Glib::ustring&){
        _is_editing = true;
    });
    _text_renderer->signal_editing_canceled().connect([this](){
        _is_editing = false;
    });
    _text_renderer->signal_edited().connect([this](const Glib::ustring&,const Glib::ustring&){
        _is_editing = false;
    });

    const int icon_col_width = 24;
    auto const icon_renderer = Gtk::make_managed<Inkscape::UI::Widget::CellRendererItemIcon>();
    icon_renderer->property_xpad() = 2;
    icon_renderer->property_width() = icon_col_width;
    _tree.append_column(*_name_column);
    _name_column->set_expand(true);
    _name_column->pack_start(*icon_renderer, false);
    _name_column->pack_start(*_text_renderer, true);
    _name_column->add_attribute(_text_renderer->property_text(), _model->_colLabel);
    _name_column->add_attribute(_text_renderer->property_cell_background_rgba(), _model->_colBgColor);
    _name_column->add_attribute(icon_renderer->property_shape_type(), _model->_colType);
    _name_column->add_attribute(icon_renderer->property_color(), _model->_colIconColor);
    _name_column->add_attribute(icon_renderer->property_clipmask(), _model->_colClipMask);
    _name_column->add_attribute(icon_renderer->property_cell_background_rgba(), _model->_colBgColor);

    // blend mode and opacity icon(s)
    _item_state_toggler = Gtk::make_managed<UI::Widget::ImageToggler>(
        INKSCAPE_ICON("object-blend-mode"), INKSCAPE_ICON("object-opaque"));
    int modeColNum = _tree.append_column("mode", *_item_state_toggler) - 1;
    if (auto col = _tree.get_column(modeColNum)) {
        col->add_attribute(_item_state_toggler->property_active(), _model->_colItemStateSet);
        col->add_attribute(_item_state_toggler->property_active_icon(), _model->_colItemState);
        col->add_attribute(_item_state_toggler->property_cell_background_rgba(), _model->_colBgColor);
        col->add_attribute(_item_state_toggler->property_activatable(), _model->_colIconsVisible);
        col->set_fixed_width(icon_col_width);
        _blend_mode_column = col;
    }

    _tree.set_has_tooltip(true);
    _tree.signal_query_tooltip().connect([this](int x, int y, bool kbd, const Glib::RefPtr<Gtk::Tooltip>& tooltip){
        Gtk::TreeModel::iterator iter;
        if (!_tree.get_tooltip_context_iter(x, y, kbd, iter) || !iter) {
            return false;
        }

        // Get column we are hovering over (would be nice if the above method gave that to us)
        int rel_x, rel_y, cell_x, cell_y;
        Gtk::TreeModel::Path path;
        Gtk::TreeViewColumn *column;
        _tree.convert_widget_to_bin_window_coords(x, y, rel_x, rel_y);
        if (!_tree.get_path_at_pos(rel_x, rel_y, path, column, cell_x, cell_y)) {
            return false;
        }

        if (column == _blend_mode_column) {
            auto blend = (*iter)[_model->_colBlendMode];
            auto opacity = (*iter)[_model->_colOpacity];
            auto templt = !pango_version_check(1, 50, 0) ?
                "<span>%1 %2%%\n</span><span line_height=\"0.5\">\n</span><span>%3\n<i>%4</i></span>" :
                "<span>%1 %2%%\n</span><span>\n</span><span>%3\n<i>%4</i></span>";
            auto label = Glib::ustring::compose(templt,
                _("Opacity:"), Util::format_number(opacity * 100.0, 1),
                _("Blend mode:"), _blend_mode_names[blend]
            );
            tooltip->set_markup(label);
        }
        else if (column == _eye_column) {
            auto invisible = (*iter)[_model->_colInvisible];
            auto label = invisible ? _("Hidden") : _("Visible");
            tooltip->set_text(label);
        }
        else if (column == _lock_column) {
            auto locked = (*iter)[_model->_colLocked];
            auto label = locked ? _("Locked") :  _("Unlocked");
            tooltip->set_text(label);
        }
        else {
            return false;
        }

        _tree.set_tooltip_cell(tooltip, nullptr, column, nullptr);
        return true;
    }, false); // before

    _object_menu.signal_closed().connect([this]{
        _item_state_toggler->set_force_visible(false);
        _tree.queue_draw();
    });

    auto& modes = get_widget<Gtk::Grid>(_builder, "modes");
    _opacity_slider.set_format_value_func([](double const val){
        return Util::format_number(val, 1) + "%";
    });
    const int min = 0, max = 100;
    for (int i = min; i <= max; i += 50) {
        _opacity_slider.add_mark(i, Gtk::PositionType::BOTTOM, "");
    }
    _opacity_slider.signal_value_changed().connect([this](){
        if (current_item.get()) {
            auto value = _opacity_slider.get_value() / 100.0;
            Inkscape::CSSOStringStream os;
            os << CLAMP(value, 0.0, 1.0);
            auto css = sp_repr_css_attr_new();
            sp_repr_css_set_property(css, "opacity", os.str().c_str());

            // Apply CSS through the desktop to ensure that "last style used" is
            // set correctly.
            auto obj_set = Inkscape::ObjectSet();
            obj_set.set(current_item.get());
            sp_desktop_set_style(&obj_set, getDesktop(), css);

            sp_repr_css_attr_unref(css);
            DocumentUndo::maybeDone(current_item.get()->document, ":opacity", RC_("Undo", "Change opacity"), INKSCAPE_ICON("dialog-object-properties"));
        }
    });

    // object blend mode and opacity popup
    Gtk::CheckButton *group = nullptr;
    int top = 0;
    int left = 0;
    int width = 2;
    for (size_t i = 0; i < Inkscape::SPBlendModeConverter._length; ++i) {
        auto& data = Inkscape::SPBlendModeConverter.data(i);
        auto label = _blend_mode_names[data.id] = g_dpgettext2(nullptr, "BlendMode", data.label.c_str());
        if (Inkscape::SPBlendModeConverter.get_key(data.id) == "-") {
            if (top >= (Inkscape::SPBlendModeConverter._length + 1) / 2) {
                ++left;
                top = 2;
            } else if (!left) {
                auto const sep = Gtk::make_managed<Gtk::Separator>();
                sep->set_visible(true);
                modes.attach(*sep, left, top, 2, 1);
            }
        } else {
            // Manual correction that indicates this should all be done in glade
            if (left == 1 && top == 9)
                top++;

            auto const check = Gtk::make_managed<Gtk::CheckButton>(label);
            if (!group) group = check;
            else check->set_group(*group);
            check->set_halign(Gtk::Align::START);
            check->signal_toggled().connect([=, this]{
                if (!check->get_active()) return;
                // set blending mode
                if (set_blend_mode(current_item.get(), data.id)) {
                    for (auto const &btn : _blend_items) {
                        btn.second->property_active().set_value(btn.first == data.id);
                    }
                    DocumentUndo::done(getDocument(), RC_("Undo", "Change blend mode"), "");
                }
            });
            _blend_items[data.id] = check;
            _blend_mode_names[data.id] = label;
            check->set_visible(true);
            modes.attach(*check, left, top, width, 1);
            width = 1; // First element takes whole width
        }
        top++;
    }

    // Visible icon
    auto const eyeRenderer = Gtk::make_managed<UI::Widget::ImageToggler>(
            INKSCAPE_ICON("object-hidden"), INKSCAPE_ICON("object-visible"));
    int visibleColNum = _tree.append_column("vis", *eyeRenderer) - 1;
    if (auto eye = _tree.get_column(visibleColNum)) {
        eye->add_attribute(eyeRenderer->property_active(), _model->_colInvisible);
        eye->add_attribute(eyeRenderer->property_cell_background_rgba(), _model->_colBgColor);
        eye->add_attribute(eyeRenderer->property_activatable(), _model->_colIconsVisible);
        eye->add_attribute(eyeRenderer->property_gossamer(), _model->_colAncestorInvisible);
        eye->set_fixed_width(icon_col_width);
        _eye_column = eye;
    }

    // Unlocked icon
    auto const lockRenderer = Gtk::make_managed<UI::Widget::ImageToggler>(
        INKSCAPE_ICON("object-locked"), INKSCAPE_ICON("object-unlocked"));
    int lockedColNum = _tree.append_column("lock", *lockRenderer) - 1;
    if (auto lock = _tree.get_column(lockedColNum)) {
        lock->add_attribute(lockRenderer->property_active(), _model->_colLocked);
        lock->add_attribute(lockRenderer->property_cell_background_rgba(), _model->_colBgColor);
        lock->add_attribute(lockRenderer->property_activatable(), _model->_colIconsVisible);
        lock->add_attribute(lockRenderer->property_gossamer(), _model->_colAncestorLocked);
        lock->set_fixed_width(icon_col_width);
        _lock_column = lock;
    }

    // hierarchy indicator - using item's layer highlight color
    auto const tag_renderer = Gtk::make_managed<Inkscape::UI::Widget::ColorTagRenderer>();
    int tag_column = _tree.append_column("tag", *tag_renderer) - 1;
    if (auto tag = _tree.get_column(tag_column)) {
        tag->add_attribute(tag_renderer->property_color(), _model->_colIconColor);
        tag->add_attribute(tag_renderer->property_hover(), _model->_colHoverColor);
        tag->set_fixed_width(tag_renderer->get_width());
        _color_tag_column = tag;
    }

    // All icon columns already have fixed widths; the name column expands to
    // the available space. Uniform rows need only visible-cell measurement.
    for (auto column : _tree.get_columns()) column->set_sizing(Gtk::TreeViewColumn::Sizing::FIXED);
    updateRowHeightMode();

    //Set the expander columns and search columns
    _tree.set_expander_column(*_name_column);
    _tree.set_search_column(-1);
    _tree.set_enable_search(false);
    _tree.get_selection()->set_mode(Gtk::SelectionMode::NONE);

    //Set up tree signals
    auto const click = Gtk::GestureClick::create();
    click->set_button(0); // any
    click->set_propagation_phase(Gtk::PropagationPhase::TARGET);
    click->signal_pressed().connect(Controller::use_state([this](auto &&...args) { return on_click(args..., EventType::pressed); }, *click));
    click->signal_released().connect(Controller::use_state([this](auto &&...args) { return on_click(args..., EventType::released); }, *click));
    _tree.add_controller(click);

    auto const key = Gtk::EventControllerKey::create();
    key->signal_key_pressed().connect([this, &key = *key](auto &&...args) { return on_tree_key_pressed(key, args...); }, true);
    _tree.add_controller(key);

    auto const motion = Gtk::EventControllerMotion::create();
    motion->set_propagation_phase(Gtk::PropagationPhase::TARGET);
    motion->signal_enter().connect(sigc::mem_fun(*this, &ObjectsPanel::on_motion_enter));
    motion->signal_leave().connect(sigc::mem_fun(*this, &ObjectsPanel::on_motion_leave));
    motion->signal_motion().connect([this, &motion = *motion](auto &&...args) { on_motion_motion(&motion, args...); });
    _tree.add_controller(motion);

    // Track Alt key on parent window so we don't need to have key focus to work
    auto const window_key = Gtk::EventControllerKey::create();
    window_key->signal_key_pressed().connect([this, &window_key = *window_key](auto &&...args) { return on_window_key(window_key, args..., EventType::pressed); }, true);
    window_key->signal_key_released().connect([this, &window_key = *window_key](auto &&...args) { on_window_key(window_key, args..., EventType::released); });
    connect_on_window_when_mapped(window_key, _tree);

    // Before expanding a row, replace the dummy child with the actual children
    _tree.signal_test_expand_row().connect([this](const Gtk::TreeModel::iterator &iter, const Gtk::TreeModel::Path &) {
        if (!_flushing && !_rebuilding && !_root_reset_pending && _dirty_parents.empty() && cleanDummyChildren(*iter)) {
            if (getSelection()) {
                _selectionChanged();
            }
        }
        return false;
    }, false); // before
    _tree.signal_row_expanded().connect([this](const Gtk::TreeModel::iterator &iter, const Gtk::TreeModel::Path &) {
        if (!_rebuilding && !_root_reset_pending && _dirty_parents.empty()) if (auto item = getItem(*iter)) {
            item->setExpanded(true);
            if (auto watcher = getWatcher(item->getRepr())) watcher->rememberExpansion(true);
        }
    });
    _tree.signal_row_collapsed().connect([this](const Gtk::TreeModel::iterator &iter, const Gtk::TreeModel::Path &) {
        if (!_rebuilding && !_root_reset_pending && _dirty_parents.empty()) if (auto item = getItem(*iter)) {
            item->setExpanded(false);
            if (auto watcher = getWatcher(item->getRepr())) watcher->rememberExpansion(false);
        }
    });
    _tree.signal_cursor_changed().connect([this] {
        if (GTK_IS_TREE_MODEL(_store->gobj())) { // avoid calling during destroy
            _updateIconVisibility();
        }
    });

    auto const drag = Gtk::DragSource::create();
    drag->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
    drag->set_actions(Gdk::DragAction::MOVE);
    drag->signal_prepare().connect([this, &drag = *drag](auto &&...args) { return on_prepare(drag, args...); }, false); // before
    drag->signal_drag_begin().connect(sigc::mem_fun(*this, &ObjectsPanel::on_drag_begin));
    drag->signal_drag_end().connect(sigc::mem_fun(*this, &ObjectsPanel::on_drag_end));
    _tree.add_controller(drag);

    auto const drop = Gtk::DropTarget::create(Glib::Value<Glib::ustring>::value_type(), Gdk::DragAction::MOVE);
    drop->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
    drop->signal_motion().connect(sigc::mem_fun(*this, &ObjectsPanel::on_drag_motion), false); // before
    drop->signal_drop().connect(sigc::mem_fun(*this, &ObjectsPanel::on_drag_drop), false); // before
    _tree.add_controller(drop);

    // Set the treeview up as a drag destination for some arbitrary format. This sets up enough
    // internal gtk machinery such that rows get the correct css markings, but we don't actually
    // use this format. Our drags are handled by the above controllers, because there's a lot of
    // edge cases (including column drags on visibility or locks).
    auto formats = Gdk::ContentFormats::create("application/x-inkscape-objects-panel-drag");
    _tree.enable_model_drag_dest(formats, Gdk::DragAction::COPY);

    //Set up the label editing signals
    _text_renderer->signal_edited().connect(sigc::mem_fun(*this, &ObjectsPanel::_handleEdited));

    //Set up the scroller window and pack the page
    // turn off overlay scrollbars - they block access to the 'lock' icon
    _scroller.set_overlay_scrolling(false);
    _scroller.set_child(_tree);
    _scroller.set_policy( Gtk::PolicyType::AUTOMATIC, Gtk::PolicyType::AUTOMATIC );
    _scroller.set_has_frame(true);
    _scroller.set_vexpand();
    Gtk::Requisition sreq;
    Gtk::Requisition sreq_natural;
    _scroller.get_preferred_size(sreq_natural, sreq);
    int minHeight = 70;
    if (sreq.get_height() < minHeight) {
        // Set a min height to see the layers when used with Ubuntu liboverlay-scrollbar
        _scroller.set_size_request(sreq.get_width(), minHeight);
    }

    _page.append(header);
    _page.append(_scroller);
    _popoverbin.setChild(&_page);
    _popoverbin.set_expand();
    append(_popoverbin);

    auto const set_selection_color = [&] {
        selection_color = get_color_with_class(_tree, "theme_selected_bg_color");
    };
    set_selection_color();

    auto enter_layer_label_editing_mode = [this]() {
        layerChanged(getDesktop()->layerManager().currentLayer());
        if (auto watcher = getWatcher(_layer->getRepr())) {
            _tree.set_cursor(watcher->getTreePath(), *_tree.get_column(0), true);
            _is_editing = true;
        }
    };
    auto& add_layer_btn = get_widget<Gtk::Button>(_builder, "insert-layer");
    add_layer_btn.signal_clicked().connect(enter_layer_label_editing_mode);

    _tree_style = _tree.connect_css_changed([=, this] (GtkCssStyleChange *change) {
        set_selection_color();

        if (!root_watcher) return;
        for (auto&& kv : root_watcher->child_watchers) {
            if (kv.second) {
                kv.second->updateRowHighlight();
            }
        }
    });

    // Clear and update entire tree (do not use this in changed/modified signals)
    auto prefs = Inkscape::Preferences::get();
    _watch_object_mode = prefs->createObserver("/dialogs/objects/layers_only", [this]() { setRootWatcher(); });

    update();
}

ObjectsPanel::~ObjectsPanel()
{
    if (_flush_state) { _flush_state->valid = false; _flush_state->alive = false; }
    _mutation_finished.disconnect();
    _document_observer.reset();
    _structural_idle.disconnect();
    _idle_connection.disconnect();
    _rebuilding = true;
    root_watcher.reset();
    _tree.unset_model();
    _store->clear();
}

void ObjectsPanel::desktopReplaced()
{
    layer_changed.disconnect();

    auto desktop = getDesktop();
    if (desktop) {
        layer_changed = desktop->layerManager().connectCurrentLayerChanged([this](SPObject *layer) {
            if (_flushing) { _flush_state->valid = false; return; }
            layerChanged(layer);
        });
    }
}

void ObjectsPanel::documentReplaced()
{
    setRootWatcher();
}

void ObjectsPanel::setRootWatcher()
{
    if (_flushing) {
        _flush_state->valid = false;
        ++_document_generation;
        _root_reset_pending = true;
        _mutation_finished.disconnect();
        _document_observer.reset();
        if (root_watcher) root_watcher->stopWatching();
        return;
    }
    // Filter changes and document attachment also rebuild the whole model.
    // They must obey the same fence as a queued structural refresh.
    if (auto document = getDocument(); document && document->getReprDoc()->mutationActive()) {
        ++_document_generation;
        _root_reset_pending = true;
        _structural_idle.disconnect();
        _mutation_finished.disconnect();
        _document_observer.reset();
        if (root_watcher) root_watcher->stopWatching();
        _tree.set_sensitive(false);
        scheduleStructuralRefresh();
        return;
    }
    auto state = std::make_shared<ObjectsPanelRefreshState>();
    _flush_state = state;
    RefreshCheckpoint check{state};
    _flushing = true;
    try {
        _root_reset_pending = false;
        _mutation_finished.disconnect();
        _document_observer.reset();
        _structural_idle.disconnect();
        ++_document_generation;
        _expanded_items.clear();
        _collapsed_items.clear();
        _materialized_items.clear();
        _dirty_parents.clear();
        _pending_current.reset();
        _scroll_anchor.reset();
        _scroll_anchor_id.clear();
        _cursor_id.clear();
        _cursor_column_index = -1;
        _rebuilding = true;
        _hovered_row_ref = {};
        _clicked_item_row.reset();
        current_item = nullptr;
        _layer = nullptr;
        root_watcher.reset();
        _store->clear(); check();
        _rebuilding = false;
        _tree.set_sensitive(true); check();
        _idle_connection.disconnect();

        auto const document = getDocument();
        if (document) {
            _mutation_finished = document->getReprDoc()->signalMutationFinished().connect([this] {
                if (!_dirty_parents.empty() || _root_reset_pending) scheduleStructuralRefresh();
            });
            _document_observer = std::make_unique<ObjectsPanelDocumentObserver>(*this, *document->getRoot()->getRepr());

            auto const prefs = Inkscape::Preferences::get();
            bool const filtered = prefs->getBool("/dialogs/objects/layers_only", false) || _searchBox.get_text().length();

            // A filtered object watcher behaves differently to an unfiltered one.
            // Filtering disables creating dummy children and instead processes entire trees.
            root_watcher = std::make_unique<ObjectWatcher>(this, document->getRoot(), nullptr, filtered);
            updateRowHeightMode(); check();
            root_watcher->rememberExtendedItems(); check();
            layerChanged(getDesktop()->layerManager().currentLayer()); check();
            _selectionChanged(); check();
        }
    } catch (RefreshInterrupted const &) {
        if (!state->alive) return;
        _root_reset_pending = true;
    }
    _flush_state.reset();
    _flushing = false;
    _rebuilding = false;
    if (_root_reset_pending || !_dirty_parents.empty()) scheduleStructuralRefresh();
}

void ObjectsPanel::updateRowHeightMode()
{
    _tree.set_fixed_height_mode(_variable_height_rows == 0);
}

void ObjectsPanel::queueStructuralRefresh(ObjectWatcher *watcher)
{
    if (_flush_state) _flush_state->valid = false;
    // A recursive filter can change the visibility of ancestors as well.
    if (watcher->isFiltered()) watcher = root_watcher.get();
    if (!watcher) return; // Root construction has not published its watcher yet.
    if (_dirty_parents.empty() && !_flushing) {
        // Capture only weak document identities; a deletion may release any of them
        // before the idle runs. Disable input against the temporarily stale model.
        _pending_current = current_item.get();
        current_item = nullptr;
        _scroll_anchor.reset();
        _scroll_anchor_id.clear();
        Gtk::TreeModel::Path first, last;
        if (_tree.get_visible_range(first, last)) {
            if (auto row = _store->get_iter(first)) {
                _scroll_anchor = getItem(*row);
                // Native removal may already have released the SPObject, while
                // this notification still owns the removed XML subtree.
                if (auto node = getRepr(*row)) {
                    if (auto id = node->attribute("id")) _scroll_anchor_id = id;
                }
                Gdk::Rectangle rect;
                _tree.get_background_area(first, *_tree.get_column(0), rect);
                _anchor_y = rect.get_y();
            }
        }
        _scroll_value = _scroller.get_vadjustment()->get_value();
        _cursor_id.clear();
        _cursor_column_index = -1;
        Gtk::TreeModel::Path cursor_path;
        Gtk::TreeViewColumn *cursor_column = nullptr;
        _tree.get_cursor(cursor_path, cursor_column);
        if (cursor_path) {
            if (auto row = _store->get_iter(cursor_path)) {
                if (auto node = getRepr(*row)) {
                    if (auto id = node->attribute("id")) _cursor_id = id;
                }
            }
        }
        auto columns = _tree.get_columns();
        for (unsigned i = 0; i < columns.size(); ++i) {
            if (columns[i] == cursor_column) _cursor_column_index = i;
        }
        on_motion_motion(nullptr, 0, 0);
        _hovered_row_ref = {};
        _clicked_item_row.reset();
        _initial_path = Gtk::TreeModel::Path();
        _prev_range.clear();
        gtk_tree_view_set_drag_dest_row(_tree.gobj(), nullptr, GTK_TREE_VIEW_DROP_BEFORE);
        _tree.set_sensitive(false);
    }
    _dirty_parents.insert(watcher);
    scheduleStructuralRefresh();
}

void ObjectsPanel::scheduleStructuralRefresh()
{
    if (_flushing || _structural_idle.connected()) return;
    // Do not poll an open fence in a nested loop. Quiescence only schedules;
    // it never rebuilds synchronously once per XML child notification.
    if (auto document = getDocument(); document && document->getReprDoc()->mutationActive()) {
        if (!_mutation_finished.connected()) {
            _mutation_finished = document->getReprDoc()->signalMutationFinished().connect([this] {
                if (_root_reset_pending || !_dirty_parents.empty()) scheduleStructuralRefresh();
            });
        }
        return;
    }
    _structural_idle = Glib::signal_idle().connect([this] {
        _structural_idle.disconnect(); // Work queued by callbacks gets a new source.
        if (auto document = getDocument(); document && document->getReprDoc()->mutationActive()) return false;
        if (_root_reset_pending) setRootWatcher();
        else flushStructuralRefresh();
        return false;
    }, G_PRIORITY_HIGH_IDLE - 1);
}

void ObjectsPanel::flushStructuralRefresh()
{
    if (_flushing || !root_watcher || _dirty_parents.empty()) return;
    if (auto document = getDocument(); !document || document->getReprDoc()->mutationActive()) return;
    auto state = std::make_shared<ObjectsPanelRefreshState>();
    _flush_state = state;
    RefreshCheckpoint check{state};
    _flushing = true;
    auto generation = _document_generation;
    try {
        _rebuilding = true;
        // Consume one owned dirty entry at a time. No vector of raw roots survives
        // a model callback. Destruction still removes entries from the live set.
        while (!_dirty_parents.empty()) {
            auto watcher = *_dirty_parents.begin();
            for (auto parent = watcher->getRepr()->parent(); parent; parent = parent->parent()) {
                if (auto ancestor = getWatcher(parent); ancestor && _dirty_parents.count(ancestor)) watcher = ancestor;
            }
            watcher->rememberMaterializedItems();
            _dirty_parents.erase(watcher);
            watcher->rebuildChildren(); check();
            ++_structural_flushes;
        }
        updateRowHeightMode(); check();
        _rebuilding = false;
        current_item = cast<SPItem>(_pending_current.get());
        _pending_current.reset();
        _expanded_items.clear();
        _collapsed_items.clear();
        layerChanged(getDesktop()->layerManager().currentLayer()); check();
        _scroll_lock = true;
        _preserve_expansion = true;
        _selectionChanged(); check();
        _preserve_expansion = false;
        _materialized_items.clear();
        _idle_connection.disconnect();
        _rebuilding = true;
        root_watcher->rememberExtendedItems(); check();
        if (!_cursor_id.empty()) {
            if (auto object = getDocument()->getObjectById(_cursor_id)) {
                if (auto watcher = getWatcher(object->getRepr()); watcher && watcher->hasRow()) {
                    auto path = watcher->getTreePath();
                    auto column = _cursor_column_index >= 0 ? _tree.get_column(_cursor_column_index) : nullptr;
                    gtk_tree_view_set_cursor(_tree.gobj(), path.gobj(), column ? column->gobj() : nullptr, false); check();
                    if (auto row = watcher->getRow()) (*row)[_model->_colIconsVisible] = true;
                    check();
                }
            }
        }
        _cursor_id.clear();
        _rebuilding = false;
        _scroller.get_vadjustment()->set_value(_scroll_value); check();
        if (!_scroll_anchor && !_scroll_anchor_id.empty()) {
            _scroll_anchor = getDocument()->getObjectById(_scroll_anchor_id);
        }
        if (_scroll_anchor) {
            if (auto watcher = getWatcher(_scroll_anchor->getRepr()); watcher && watcher->hasRow()) {
                Gdk::Rectangle rect;
                auto path = watcher->getTreePath();
                _tree.get_background_area(path, *_tree.get_column(0), rect);
                auto adjustment = _scroller.get_vadjustment();
                adjustment->set_value(adjustment->get_value() + rect.get_y() - _anchor_y); check();
            }
        }
        _scroll_anchor.reset();
        _scroll_anchor_id.clear();
        _tree.set_sensitive(true); check();
    } catch (RefreshInterrupted const &) {
        if (!state->alive) return;
        // A callback invalidated either content or document identity. Do not
        // restore state through stale objects; rebuild on a fresh idle instead.
        _root_reset_pending = true;
    }
    if (!state->alive) return;
    _flush_state.reset();
    _flushing = false;
    _rebuilding = false;
    _preserve_expansion = false;
    if (generation != _document_generation) _root_reset_pending = true;
    if (_root_reset_pending || !_dirty_parents.empty()) scheduleStructuralRefresh();
}

/**
 * Apply any ongoing filters to the items.
 */
bool ObjectsPanel::showChildInTree(SPItem *item) {
    Inkscape::Preferences *prefs = Inkscape::Preferences::get();

    bool show_child = true;

    // Filter by object type, the layers dialog here.
    if (prefs->getBool("/dialogs/objects/layers_only", false)) {
        auto group = cast<SPGroup>(item);
        if (!group || group->layerMode() != SPGroup::LAYER) {
            show_child = false;
        }
    }

    // Filter by text search, if the search text box has any contents
    auto term = _searchBox.get_text().lowercase();
    if (show_child && term.length()) {
        // A source document allows search for different pieces of metadata
        std::stringstream source;
        if (char const *id = item->getId()) {
            source << "#" << id;
        }
        if (auto label = item->label())
            source << " " << label;
        source << " @" << item->getTagName();
        // Might want to add class names here as ".class"

        auto doc = source.str();
        transform(doc.begin(), doc.end(), doc.begin(), ::tolower);
        show_child = doc.find(term) != std::string::npos;
    }

    // Now the terrible bit, searching all the children causing a
    // duplication of work as it must re-scan up the tree multiple times
    // when the tree is very deep.
    for (auto child_obj : item->childList(false)) {
        if (show_child)
            break;
        if (auto child = cast<SPItem>(child_obj)) {
            show_child = showChildInTree(child);
        }
    }

    return show_child;
}

/**
 * This both unpacks the tree, and populates lazy loading
 */
ObjectWatcher *ObjectsPanel::unpackToObject(SPObject *item)
{
    ObjectWatcher *watcher = nullptr;

    for (auto &parent : item->ancestorList(true)) {
        if (parent->getRepr() == root_watcher->getRepr()) {
            watcher = root_watcher.get();
        } else if (watcher &&
                   (watcher = watcher->findChild(parent->getRepr())))
        {
            if (auto const row = watcher->getRow()) {
                cleanDummyChildren(*row);
            }
        }
    }

    return watcher;
}

// Same definition as in 'document.cpp'
#define SP_DOCUMENT_UPDATE_PRIORITY (G_PRIORITY_HIGH_IDLE - 2)

void ObjectsPanel::selectionChanged(Selection *selected /* not used */)
{
    if (_flushing) { _flush_state->valid = false; return; }
    if (!_idle_connection.connected()) {
        auto handler = [this] { return _flushing ? false : _selectionChanged(); };
        int priority = SP_DOCUMENT_UPDATE_PRIORITY + 1;
        _idle_connection = Glib::signal_idle().connect(handler, priority);
    }
}

bool ObjectsPanel::_selectionChanged()
{
    RefreshCheckpoint check{_flush_state};
    if (!root_watcher || !getSelection() || _rebuilding || _root_reset_pending || !_dirty_parents.empty()) return false;
    Inkscape::Preferences *prefs = Inkscape::Preferences::get();
    bool keep_current_item = current_item && getSelection()->includes(current_item.get());
    std::unordered_set<SPObject *> unpacked_parents;

    for (auto item : getSelection()->items()) {
        // During a structural flush, unpack each selected parent just once.
        // Every child watcher is initialized from the final native selection.
        if (_preserve_expansion && !unpacked_parents.insert(item->parent).second) continue;
        if (auto watcher = unpackToObject(item)) {
            // Expand layers themselves, but do not expand groups.
            auto focus_watcher = watcher;

            // Failing to find the child watcher here means the object is filtered out
            // of the current object view and we expand to the closest sublayer instead.
            if (auto child_watcher = watcher->findChild(item->getRepr())) {
                watcher = child_watcher;
            }

            {
                if (!_preserve_expansion && prefs->getBool("/dialogs/objects/expand_to_layer", true)) {
                    if (auto path = focus_watcher->getTreePath()) { _tree.expand_to_path(path); check(); }
                    if (!_scroll_lock) {
                        if (auto path = watcher->getTreePath()) { _tree.scroll_to_row(path, 0.5); check(); }
                    }
                }
            }
        }
    }
    root_watcher->syncSelection(); check();
    if (!keep_current_item) {
        current_item = nullptr;
    }
    _scroll_lock = false;

    // Returning 'false' disconnects idle signal handler
    return false;
}

/**
 * Happens when the layer selected is changed.
 *
 * @param layer - The layer now selected
 */
void ObjectsPanel::layerChanged(SPObject *layer)
{
    RefreshCheckpoint check{_flush_state};
    if (!root_watcher || _rebuilding || _root_reset_pending || !_dirty_parents.empty()) return;
    root_watcher->setSelectedBitRecursive(LAYER_FOCUS_CHILD | LAYER_FOCUSED, false); check();

    if (!layer || !layer->getRepr()) return;

    auto const watcher = getWatcher(layer->getRepr());
    if (watcher && watcher != root_watcher.get()) {
        watcher->setSelectedBitChildren(LAYER_FOCUS_CHILD, true); check();
        watcher->setSelectedBit(LAYER_FOCUSED, true); check();
    }

    _layer = layer;
}

/**
 * Special context-aware functions - If nothing is selected
 * or layers-only mode is active, move/delete layers.
 */
void ObjectsPanel::_activateAction(const std::string& layerAction, const std::string& selectionAction)
{
    auto selection = getSelection();
    auto *prefs = Inkscape::Preferences::get();
    if (selection->isEmpty() || prefs->getBool("/dialogs/objects/layers_only", false)) {
        InkscapeWindow* win = InkscapeApplication::instance()->get_active_window();
        win->activate_action(layerAction);
    } else {
        Glib::RefPtr<Gio::Application> app = Gio::Application::get_default();
        app->activate_action(selectionAction);
    }
}

/**
 * Sets visibility of items in the tree
 * @param iter Current item in the tree
 */
bool ObjectsPanel::toggleVisible(Gdk::ModifierType const state, Gtk::TreeModel::Row row)
{
    auto desktop = getDesktop();
    auto selection = getSelection();

    if (SPItem* item = getItem(row)) {
        if (Controller::has_flag(state, Gdk::ModifierType::SHIFT_MASK)) {
            // Toggle Visible for layers (hide all other layers)
            if (desktop->layerManager().isLayer(item)) {
                desktop->layerManager().toggleLayerSolo(item);
                DocumentUndo::done(getDocument(), RC_("Undo", "Hide other layers"), "");
            }
            return true;
        }
        bool visible = !row[_model->_colInvisible];
        if (Controller::has_flag(state, Gdk::ModifierType::CONTROL_MASK) ||
            !selection->includes(item))
        {
            item->setHidden(visible);
        } else {
            for (auto sitem : selection->items()) {
                sitem->setHidden(visible);
            }
        }
        // Use maybeDone so user can flip back and forth without making loads of undo items
        DocumentUndo::maybeDone(getDocument(), "toggle-vis", RC_("Undo", "Toggle item visibility"), INKSCAPE_ICON("dialog-object-properties"));
        return visible;
    }
    return false;
}

// show blend mode popup menu for current item
bool ObjectsPanel::blendModePopup(int const x, int const y, Gtk::TreeModel::Row row)
{
    auto const item = getItem(row);
    if (item == nullptr) {
        return false;
    }

    current_item = nullptr;

    auto blend = SP_CSS_BLEND_NORMAL;
    if (item->style && item->style->mix_blend_mode.set) {
        blend = item->style->mix_blend_mode.value;
    }

    auto opacity = 1.0;
    if (item->style && item->style->opacity.set) {
        opacity = item->style->opacity.as_double();
    }

    for (auto const &btn : _blend_items) {
        btn.second->property_active().set_value(btn.first == blend);
    }

    _opacity_slider.set_value(opacity * 100);
    current_item = item;

    _item_state_toggler->set_force_visible(true);

    _popoverbin.setPopover(&_object_menu);
    UI::popup_at(_object_menu, _tree, x, y);
    return true;
}

bool ObjectsPanel::colorTagPopup(int const x, int const y, Gtk::TreeModel::Row row)
{
    auto const item = getItem(row);
    if (item == nullptr) {
        return false;
    }
    _colors->set(item->highlight_color());
    auto color_popup = Gtk::make_managed<Gtk::Popover>();
    _color_selector = Gtk::make_managed<ColorNotebook>(_colors);
    _color_selector->set_label(_("Highlight Color"));
    _color_selector->set_margin(4);
    color_popup->set_child(*_color_selector);
    _colors->signal_changed.connect([this]() {
        if (_clicked_item_row) if (auto item = getItem(*_clicked_item_row)) {
            item->setHighlight(_colors->get().value());
            DocumentUndo::maybeDone(getDocument(), "highlight-color", RC_("Undo", "Set item highlight color"), INKSCAPE_ICON("dialog-object-properties"));
        }
    });
    _popoverbin.setPopover(&*color_popup);
    UI::popup_at(*color_popup, _tree, x, y);

    return true;
}

/**
 * Sets sensitivity of items in the tree
 * @param iter Current item in the tree
 * @param locked Whether the item should be locked
 */
bool ObjectsPanel::toggleLocked(Gdk::ModifierType const state, Gtk::TreeModel::Row row)
{
    auto desktop = getDesktop();
    auto selection = getSelection();

    if (SPItem* item = getItem(row)) {
        if (Controller::has_flag(state, Gdk::ModifierType::SHIFT_MASK)) {
            // Toggle lock for layers (lock all other layers)
            if (desktop->layerManager().isLayer(item)) {
                desktop->layerManager().toggleLockOtherLayers(item);
                DocumentUndo::done(getDocument(), RC_("Undo", "Lock other layers"), "");
            }
            return true;
        }
        bool locked = !row[_model->_colLocked];
        if (Controller::has_flag(state, Gdk::ModifierType::CONTROL_MASK) ||
            !selection->includes(item))
        {
            item->setLocked(locked);
        } else {
            for (auto sitem : selection->items()) {
                sitem->setLocked(locked);
            }
        }
        // Use maybeDone so user can flip back and forth without making loads of undo items
        DocumentUndo::maybeDone(getDocument(), "toggle-lock", RC_("Undo", "Toggle item locking"), "");
        return locked;
    }
    return false;
}

/**
 * Handles keyboard events on the TreeView
 * @return Whether the event should be eaten (om nom nom)
 */
bool ObjectsPanel::on_tree_key_pressed(Gtk::EventControllerKey const &controller,
                                       unsigned keyval, unsigned keycode, Gdk::ModifierType state)
{
    auto desktop = getDesktop();
    if (!desktop)
        return false;

    Gtk::TreeModel::Path path;
    Gtk::TreeViewColumn *column;
    _tree.get_cursor(path, column);

    auto const shift = Controller::has_flag(state, Gdk::ModifierType::SHIFT_MASK);
    auto const ctrl = Controller::has_flag(state, Gdk::ModifierType::CONTROL_MASK);
    auto const shortcut = Inkscape::Shortcuts::get_from(controller, keyval, keycode, state);
    switch (shortcut.get_key()) {
        case GDK_KEY_Escape:
            if (desktop->getCanvas()) {
                desktop->getCanvas()->grab_focus();
                return true;
            }
            break;
        case GDK_KEY_space:
            selectCursorItem(Gdk::ModifierType(state));

            if (path && column == _name_column) {
                // Toggle expansion of row
                if (_tree.row_expanded(path)) {
                    _tree.collapse_row(path);
                } else {
                    _tree.expand_row(path, false);
                }
            }

            return true;
        // Depending on the action to cover this causes it's special
        // text and node handling to block deletion of objects. DIY
        case GDK_KEY_Delete:
        case GDK_KEY_KP_Delete:
        case GDK_KEY_BackSpace:
            _activateAction("win.layer-delete", "delete-selection");
            // NOTE: We could select a sibling object here to make deleting many objects easier.
            return true;
        case GDK_KEY_Page_Up:
        case GDK_KEY_KP_Page_Up:
            if (shift) {
                _activateAction("win.layer-top", "selection-top");
                return true;
            }
            break;
        case GDK_KEY_Page_Down:
        case GDK_KEY_KP_Page_Down:
            if (shift) {
                _activateAction("win.layer-bottom", "selection-bottom");
                return true;
            }
            break;
        case GDK_KEY_Up:
        case GDK_KEY_KP_Up:
            if (ctrl) {
                _activateAction("win.layer-raise", "selection-stack-up");
                return true;
            }
            break;
        case GDK_KEY_Down:
        case GDK_KEY_KP_Down:
            if (ctrl) {
                _activateAction("win.layer-lower", "selection-stack-down");
                return true;
            }
            break;
        case GDK_KEY_Return:
            if (path) {
                _tree.set_cursor(path, *_tree.get_column(0), true /* start_editing */);
                _is_editing = true;
                return true;
            }
            break;
    }

    return false;
}

bool ObjectsPanel::on_window_key(Gtk::EventControllerKey const &controller,
                                 unsigned keyval, unsigned keycode,
                                 Gdk::ModifierType state, EventType event_type)
{
    auto desktop = getDesktop();
    if (!desktop)
        return false;

    auto const shortcut = Inkscape::Shortcuts::get_from(controller, keyval, keycode, state);
    switch (shortcut.get_key()) {
        case GDK_KEY_Alt_L:
        case GDK_KEY_Alt_R:
            _handleTransparentHover(event_type == EventType::pressed);
            return false;
    }

    return false;
}

/**
 * Handles mouse movements
 */

// Set a status bar text when entering the widget
void ObjectsPanel::on_motion_enter(double /*ex*/, double /*ey*/)
{
    _msg_id = getDesktop()->messageStack()->push(Inkscape::NORMAL_MESSAGE,
         _("<b>Hold ALT</b> while hovering over item to highlight, "
           "<b>hold SHIFT</b> and click to hide/lock all."));
    _translucency_key =  getDesktop()->getTranslucencyGroups().createGroupKey();
}
// watch mouse leave too to clear any state.
void ObjectsPanel::on_motion_leave()
{
    getDesktop()->getTranslucencyGroups().removeGroupKey(_translucency_key);
    _translucency_key = 0;
    getDesktop()->messageStack()->cancel(_msg_id);
    on_motion_motion(nullptr, 0, 0);
}

void ObjectsPanel::on_motion_motion(Gtk::EventControllerMotion const *controller,
                                    double ex, double ey)
{
    if (_is_editing || _flushing || _rebuilding || _root_reset_pending || !_dirty_parents.empty()) return;

    // Unhover any existing hovered row.
    if (_hovered_row_ref) {
        if (auto row = *_store->get_iter(_hovered_row_ref.get_path())) {
            row[_model->_colHover] = false;
            row[_model->_colHoverColor] = false;
            // selection etc. might change _colBgColor. Erase hover
            // highlight only if it hasn't changed
            if (row[_model->_colBgColor] == _hovered_row_color) {
                row[_model->_colBgColor] = _hovered_row_old_color;
            }
            else { // update row's slection color if it has changed
                _hovered_row_old_color = row[_model->_colBgColor];
            }
        }
    }

    // Allow this function to be called by LEAVE motion
    if (controller == nullptr) {
        _hovered_row_ref = Gtk::TreeModel::RowReference();
        _handleTransparentHover(false);
        _updateIconVisibility();
        return;
    }


    Gtk::TreeModel::Path path;
    Gtk::TreeViewColumn* col = nullptr;
    int cell_x, cell_y;
    if (_tree.get_path_at_pos(ex, ey, path, col, cell_x, cell_y)) {
        // Only allow drag and drop from the name column, not any others
        if (col == _name_column) {
            _drag_column = nullptr;
        }

        // Only allow drag and drop when not filtering. Otherwise bad things happen
        // _tree.set_reorderable(col == _name_column);

        if (auto row = *_store->get_iter(path)) {
            row[_model->_colHover] = true;
            _hovered_row_ref = Gtk::TreeModel::RowReference(_store, path);
            // update color for hovered row
            const Gdk::RGBA color = row[_model->_colBgColor]; // current color
            _hovered_row_old_color = color; // store old color
            _hovered_row_color = change_alpha(color, color.get_alpha() + HOVER_ALPHA);
            row[_model->_colBgColor] = _hovered_row_color;

            if (col == _color_tag_column) {
                row[_model->_colHoverColor] = true;
            }

            // Dragging over the eye or locks will set them all
            auto item = getItem(row);
            if (item && _drag_column && col == _drag_column) {
                if (col == _eye_column) {
                    // Defer visibility to th idle thread (it's expensive)
                    auto const id = std::string(item->getId() ? item->getId() : "");
                    auto const destination = getDesktop();
                    auto const binding = destination ? destination->documentBindingGeneration() : std::nullopt;
                    Glib::signal_idle().connect_once(sigc::track_object([this, id, destination, binding]() {
                        auto desktop = getDesktop();
                        if (desktop != destination || !desktop || !binding || desktop->documentBindingGeneration() != binding) return;
                        auto document = getDocument();
                        if (!document) return;
                        auto resolved = cast<SPItem>(document->getObjectById(id.c_str()));
                        if (!resolved) return;
                        resolved->setHidden(_drag_flip);
                        DocumentUndo::maybeDone(document, "toggle-vis", RC_("Undo", "Toggle item visibility"), "");
                    }, *this), Glib::PRIORITY_DEFAULT_IDLE);
                } else if (col == _lock_column) {
                    item->setLocked(_drag_flip);
                    DocumentUndo::maybeDone(getDocument(), "toggle-lock", RC_("Undo", "Toggle item locking"), "");
                }
            }
        }
    }

    auto const state = controller->get_current_event_state();
    _handleTransparentHover(Controller::has_flag(state, Gdk::ModifierType::ALT_MASK));
    _updateIconVisibility();
}

void ObjectsPanel::_handleTransparentHover(bool enabled)
{
    SPItem *item = nullptr;
    if (enabled && _hovered_row_ref) {
        if (auto row = *_store->get_iter(_hovered_row_ref.get_path())) {
            item = getItem(row);
        }
    }
    // Hovered item is solid, everything else is translucent
    getDesktop()->getTranslucencyGroups().setSolidItem(_translucency_key, item);
}

void ObjectsPanel::_updateIconVisibility()
{
    if (_is_editing || _flushing || _rebuilding || _root_reset_pending || !_dirty_parents.empty()) {
        return; // modifying store confuses editor
    }

    Gtk::TreeModel::Path focus_path;
    Gtk::TreeViewColumn *focus_column;
    _tree.get_cursor(focus_path, focus_column);

    // Iterate all rows because some might need to be marked invisible now (no focus or hover).
    _store->foreach_path([this, focus_path](const Gtk::TreeModel::Path &path) {
        auto iter = _store->get_iter(path);
        auto row = *iter;
        auto has_focus = focus_path && focus_path == path;
        row[_model->_colIconsVisible] = row[_model->_colHover] || has_focus;
        return false; // keep walking tree
    });
}

[[nodiscard]] static auto get_cell_area(Gtk::TreeView const &tree_view,
                                        Gtk::TreeModel::Path const &path, Gtk::TreeViewColumn &column)
{
    auto area = Gdk::Rectangle{};
    tree_view.get_cell_area(path, column, area);
    return area;
}

[[nodiscard]] static auto get_cell_center(Gtk::TreeView const &tree_view,
                                          Gtk::TreeModel::Path const &path, Gtk::TreeViewColumn &column)
{
    auto const area = get_cell_area(tree_view, path, column);
    return std::pair{std::lround(area.get_x() + area.get_width () / 2.0),
                     std::lround(area.get_y() + area.get_height() / 2.0)};
}

/**
 * Handles mouse button click events
 * @return whether to eat the event (om nom nom)
 */
Gtk::EventSequenceState ObjectsPanel::on_click(Gtk::GestureClick const &gesture,
                                               int const n_press, double const ex, double const ey,
                                               EventType const event_type)
{
    auto selection = getSelection();
    if (!selection) {
        return Gtk::EventSequenceState::NONE;
    }

    if (event_type == EventType::released) {
        _drag_column = nullptr;
    }

    Gtk::TreeModel::Path path;
    Gtk::TreeViewColumn* col = nullptr;
    int x, y;
    if (!_tree.get_path_at_pos(ex, ey, path, col, x, y)) {
        // Over background (below list or between list items).
        return Gtk::EventSequenceState::NONE;
    }

    // Setting the cursor on the clicked row so that later calls to selectCursorItem knows which
    // item to select (via get_cursor).
    // This used to be done in on_motion_motion but was moved here because of issue #5156.
    _tree.set_cursor(path);

    if (auto row = *_store->get_iter(path)) {
        if (event_type == EventType::pressed) {
            auto const state = gesture.get_current_event_state();
            // Remember column for dragging feature
            _drag_column = col;
            if (col == _eye_column) {
                _drag_flip = toggleVisible(state, row);
            } else if (col == _lock_column) {
                _drag_flip = toggleLocked(state, row);
            } else if (col == _blend_mode_column) {
                auto const [cx, cy] = get_cell_center(_tree, path, *_blend_mode_column);
                return blendModePopup(cx, cy, row) ? Gtk::EventSequenceState::CLAIMED
                                                   : Gtk::EventSequenceState::NONE;
            } else if (col == _color_tag_column) {
                _clicked_item_row = *_store->get_iter(path);
                auto const [cx, cy] = get_cell_center(_tree, path, *_color_tag_column);
                return colorTagPopup(cx, cy, row) ? Gtk::EventSequenceState::CLAIMED
                                                   : Gtk::EventSequenceState::NONE;
            }
        }
    }

    // Block D&D via controllers if over icons.
    if (col != _name_column) {
        return Gtk::EventSequenceState::CLAIMED;
    }

    // Gtk lacks the ability to detect if the user is clicking on the
    // expander icon. So we must detect it using the cell_area check.
    auto const is_expander = x < get_cell_area(_tree, path, *_name_column).get_x();
    if (is_expander) {
        return Gtk::EventSequenceState::NONE; // Or else expander won't work.
    }

    // Rename row item.
    if (n_press == 2) {
        _tree.set_cursor(path, *col, true); // true -> Start editing.
        _is_editing = true;
        return Gtk::EventSequenceState::CLAIMED;
    }

    _is_editing &= event_type == EventType::released;

    auto row = *_store->get_iter(path);
    if (!row) {
        // Already handled above by get_path_at_pos()...
        return Gtk::EventSequenceState::NONE;
    }

    SPItem *item = getItem(row);
    assert(item);

    auto layer = Inkscape::LayerManager::asLayer(item);
    auto const state = gesture.get_current_event_state();
    // returns true if layer has to be set as active but not selected
    auto const should_set_current_layer = [&] {
        if (!layer) {
            return false;
        }

        // Modifier keys force selection mode.
        if (Controller::has_flag(state, Gdk::ModifierType::SHIFT_MASK |
                                        Gdk::ModifierType::CONTROL_MASK)) {
            return false;
        }

        return _layer.get() != layer || selection->includes(layer);
    };

    // Load the right click menu?
    auto const button = gesture.get_current_button();
    auto const context_menu = event_type == EventType::pressed && button == 3;

    // Select items on button release to not confuse drag (unless it's a right-click which selects
    // item for use by context menu).
    if (!_is_editing && (event_type == EventType::released || context_menu)) {
        if (context_menu) {
            // If right-clicking on a layer, make it current for context menu actions to work correctly.
            if (layer && !selection->includes(layer)) {
                getDesktop()->layerManager().setCurrentLayer(item, true);
            }

            // If the item under cursor is not selected, we select it before opening the
            // contextmenu. Otherwise, if the item hasn't been selected with left-click
            // beforehand, ContextMenu's constructor may select the item and cause the list
            // to scroll to it. Also, if the item is the parent group of a selected object,
            // it won't get selected by ContextMenu's constructor.
            // See https://gitlab.com/inkscape/inkscape/-/issues/5243
            // Layers are also not selected, and layer specific contextmenus are used instead.
            if (!selection->includes(item) && !layer ) {
                selectCursorItem(state);
            }

            // true == hide menu item for opening this dialog!
            std::vector<SPItem *> items = {item};
            auto menu = Gtk::make_managed<ContextMenu>(getDesktop(), item, items, true);
            // popup context menu pointing to the clicked tree row:
            _popoverbin.setPopover(menu);
            UI::popup_at(*menu, _tree, ex, ey);
        } else if (should_set_current_layer()) {
            getDesktop()->layerManager().setCurrentLayer(item, true);
            _initial_path = path;
        } else {
            selectCursorItem(state);
        }

        return Gtk::EventSequenceState::CLAIMED;
    } else {
        // Remember the item for we are about to drag it!
        current_item = item;
    }

    return Gtk::EventSequenceState::NONE;
}

/**
 * Handle a successful item label edit
 * @param path Tree path of the item currently being edited
 * @param new_text New label text
 */
void ObjectsPanel::_handleEdited(const Glib::ustring& path, const Glib::ustring& new_text)
{
    _is_editing = false;
    if (auto row = *_store->get_iter(path)) {
        if (auto item = getItem(row)) {
            if (!new_text.empty() && (!item->label() || new_text != item->label())) {
                auto obj = cast<SPGroup>(item);
                if (obj && obj->layerMode() == SPGroup::LAYER && !obj->isHighlightSet()) {
                    obj->setHighlight(obj->highlight_color());
                }
                item->setLabel(new_text.c_str());
                DocumentUndo::done(getDocument(), RC_("Undo", "Rename object"), "");
            }
        }
    }
}

/**
 * Take over the select row functionality from the TreeView, this is because
 * we have two selections (layer and object selection) and require a custom
 * method of rendering the result to the treeview.
 */
bool ObjectsPanel::select_row( Glib::RefPtr<Gtk::TreeModel> const & /*model*/, Gtk::TreeModel::Path const &path, bool /*sel*/ )
{
    return true;
}

/**
 * Get the XML node which is associated with a row. Can be NULL for dummy children.
 */
Node *ObjectsPanel::getRepr(Gtk::TreeModel::ConstRow const &row) const
{
    if (_rebuilding || _root_reset_pending || !_dirty_parents.empty()) return nullptr;
    return row[_model->_colNode];
}

/**
 * Get the item which is associated with a row. If getRepr(row) is not NULL,
 * then this call is expected to also not be NULL.
 */
SPItem *ObjectsPanel::getItem(Gtk::TreeModel::ConstRow const &row) const
{
    auto const this_const = const_cast<ObjectsPanel *>(this);
    return cast<SPItem>(this_const->getObject(getRepr(row)));
}

/**
 * If the given row has dummy children, remove them.
 * @pre Either all, or no children are dummies
 * @post If the function returns true, the row has no children
 * @return False if there are children and they are not dummies
 */
bool ObjectsPanel::removeDummyChildren(Gtk::TreeModel::Row row)
{
    RefreshCheckpoint check{_flush_state};
    auto &children = row.children();
    if (!children.empty()) {
        auto const iter = row.get_iter();
        Gtk::TreeStore::iterator child = children.begin();
        if (!isDummy(*child)) {
            return false;
        }

        do {
            assert(child->parent() == iter);
            assert(isDummy(*child));
            child = _store->erase(child); check();
        } while (child && child->parent() == iter);
    }
    return true;
}

bool ObjectsPanel::cleanDummyChildren(Gtk::TreeModel::Row row)
{
    if (removeDummyChildren(row)) {
        assert(row);
        if (auto watcher = getWatcher(getRepr(row))) {
            watcher->addChildren(getItem(row));
            return true;
        }
    }
    return false;
}

/**
 * Signal handler for "drag-motion"
 *
 * Refuses drops onto self.
 */
Gdk::DragAction ObjectsPanel::on_drag_motion(double x, double y)
{
    if (_rebuilding || _flushing || _root_reset_pending || !_dirty_parents.empty()) return {};
    // Clear dest row (will be set again at end, if we survive the guantlet of early exits)
    _tree.set_drag_dest_row(Gtk::TreeModel::Path(), Gtk::TreeView::DropPosition::BEFORE);

    auto selection = getSelection();
    auto document = getDocument();
    if (!selection || !document) {
        return Gdk::DragAction{}; // not supported
    }

    Gtk::TreeModel::Path path;
    Gtk::TreeView::DropPosition pos;
    _tree.get_dest_row_at_pos(x, y, path, pos);
    if (path) {
        auto item = getItem(*_store->get_iter(path));
        if (!item) {
            std::cerr << "ObjectsPanel::on_drag_motion: path doesn't correspond to an item!" << std::endl;
            return Gdk::DragAction{}; // not supported
        }

        // Don't drop on self. This causes disturbing flickering so maybe remove this and
        // rely on code in "on_drag_drop" to reject dropping on self.
        if (selection->includes(item)) {
            return Gdk::DragAction{}; // not supported
        }

        // Don't drop on descendent.
        if (selection->includesAncestor(item)) {
            return Gdk::DragAction{}; // not supported
        }

        // Only allow dragging rows from name column.
        int cell_x, cell_y;
        Gtk::TreeViewColumn* col = nullptr;
        _tree.get_path_at_pos(x, y, path, col, cell_x, cell_y);
        if (col != _name_column) {
            return Gdk::DragAction{}; // not supported
        }

        // Setting CSS class here is useless as we can't set CSS on CellRenderer.
    } else {
        if (_tree.is_blank_at_pos(x, y)) {
            // Dropping on background.
            path = --_store->children().end();
            pos = Gtk::TreeView::DropPosition::AFTER;
            auto item = getItem(*_store->get_iter(path));
            if (selection->includes(item)) {
                // Don't drop after self.
                return Gdk::DragAction{}; // not supported
            }
        } else {
            std::cerr << "ObjectsPanel::on_drag_motion: invalid drop area!" << std::endl;
            return Gdk::DragAction{}; // not supported
        }
    }

    _tree.set_drag_dest_row(path, pos);

    // need to cater scenarios where we got no selection/empty bottom space
    return Gdk::DragAction::MOVE;
}

/**
 * Signal handler for "drag-drop".
 *
 * Do the actual work of drag-and-drop.
 */
bool ObjectsPanel::on_drag_drop(Glib::ValueBase const &/*value*/, double x, double y)
{
    if (_rebuilding || _flushing || _root_reset_pending || !_dirty_parents.empty()) return false;
    Gtk::TreeModel::Path path;
    Gtk::TreeView::DropPosition pos;
    _tree.get_dest_row_at_pos(x, y, path, pos);

    if (!path) {
        if (_tree.is_blank_at_pos(x, y)){
            // We are in background/bottom empty space. Hence, need to drop the item at end.
            // We will move to the last node/path and set drop position accordingly.
            path = --_store->children().end();
            pos = Gtk::TreeView::DropPosition::AFTER;
        } else {
            std::cerr << "ObjectsPanel::on_drag_drop: invalid drop area!" << std::endl;
            return true;
        }
    }

    auto drop_repr = getRepr(*_store->get_iter(path));
    if (!drop_repr) return false;
    bool const drop_into = pos != Gtk::TreeView::DropPosition::BEFORE && //
                           pos != Gtk::TreeView::DropPosition::AFTER;

    auto selection = getSelection();
    auto document = getDocument();
    if (selection && document) {
        auto item = document->getObjectByRepr(drop_repr);

        // We always try to drop the item, even if we end up dropping it after the non-group item.
        if (drop_into && is<SPGroup>(item)) {
            selection->toLayer(item);
        } else {
            // Note: Object dialog order opposite of XML order.
            Node *after = (pos == Gtk::TreeView::DropPosition::BEFORE ||
                           pos == Gtk::TreeView::DropPosition::INTO_OR_BEFORE)
                ? drop_repr : drop_repr->prev();
            selection->toLayer(item->parent, after);
        }
        DocumentUndo::done(document, RC_("Undo", "Move items"), INKSCAPE_ICON("selection-move-to-layer"));
    }

    drag_end_impl();
    return true;
}

Glib::RefPtr<Gdk::ContentProvider> ObjectsPanel::on_prepare(Gtk::DragSource &controller, double x, double y)
{
    if (_rebuilding || _flushing || _root_reset_pending || !_dirty_parents.empty()) return {};
    Gtk::TreeModel::Path path;
    Gtk::TreeView::DropPosition pos;
    _tree.get_dest_row_at_pos(x, y, path, pos);

    if (path) {
        // Set icon (or else icon is determined by provider value).
        auto surface = _tree.create_row_drag_icon(path);
        controller.set_icon(surface, x, 12);
    }

    // We must have some kind of value which matches DropTarget type! Use a string for now.
    Glib::Value<Glib::ustring> value;
    value.init(G_TYPE_STRING);
    value.set("ObjectsPanelDrag");
    auto provider = Gdk::ContentProvider::create(value);
    return provider;
}

void ObjectsPanel::on_drag_begin(Glib::RefPtr<Gdk::Drag> const &/*drag*/)
{
    _scroll_lock = true;

    auto selection = _tree.get_selection();
    selection->set_mode(Gtk::SelectionMode::MULTIPLE);
    selection->unselect_all();

    auto obj_selection = getSelection();
    if (!obj_selection)
        return;

    if (current_item.get() && !obj_selection->includes(current_item.get())) {
        // This means the item the user started to drag is not one that is selected
        // So we'll deselect everything and start dragging this item instead.
        auto watcher = getWatcher(current_item.get()->getRepr());
        if (watcher) {
            auto path = watcher->getTreePath();
            selection->select(path);
            obj_selection->set(current_item.get());
        }
    } else {
        // Drag all the items currently selected (multi-row)
        for (auto item : obj_selection->items()) {
            auto watcher = getWatcher(item->getRepr());
            if (watcher) {
                auto path = watcher->getTreePath();
                selection->select(path);
            }
        }
    }
    // auto content = controller.get_content();  Can't modify content! Can't modify controller!
}

void ObjectsPanel::drag_end_impl()
{
    auto selection = _tree.get_selection();
    selection->unselect_all();
    selection->set_mode(Gtk::SelectionMode::NONE);
    current_item = nullptr;
    _tree.set_drag_dest_row(Gtk::TreeModel::Path(), Gtk::TreeView::DropPosition::BEFORE);
}

void ObjectsPanel::on_drag_end(Glib::RefPtr<Gdk::Drag> const &/*drag*/, bool /*delete_data*/)
{
    drag_end_impl();
}

void ObjectsPanel::selectRange(Gtk::TreeModel::Path start, Gtk::TreeModel::Path end)
{
    if (!start || !end) {
        return;
    }

    if (gtk_tree_path_compare(start.gobj(), end.gobj()) > 0) {
        std::swap(start, end);
    }

    auto selection = getSelection();
    std::vector<SPObject*> _temp_range;

    if (!_start_new_range) {
        // Deselect previous selection of this range first and then proceed.
        for (auto const &obj : _prev_range) {
            if (obj) {
                _temp_range.push_back(obj.get());
            }
        }
        selection->remove(_temp_range.begin(), _temp_range.end());
    }

    _prev_range.clear();
    _temp_range.clear();

    // Select everything between the initial selection and currently selected item.
    _store->foreach ([&](Gtk::TreeModel::Path const &p, Gtk::TreeModel::const_iterator const &it) {
        if ((gtk_tree_path_compare(start.gobj(), p.gobj()) <= 0) &&
            (gtk_tree_path_compare(end.gobj(), p.gobj()) >= 0)) {
            auto obj = getItem(*it);
            if (obj) {
                _prev_range.emplace_back(obj);
                _temp_range.push_back(obj);
            }
        }
        return false;
    });
    selection->add(_temp_range.begin(), _temp_range.end());

    _start_new_range = false;
}

/**
 * Select the object currently under the list-cursor (keyboard or mouse)
 */
bool ObjectsPanel::selectCursorItem(Gdk::ModifierType const state)
{
    auto &layers = getDesktop()->layerManager();
    auto selection = getSelection();
    if (!selection)
        return false;

    Gtk::TreeModel::Path path;
    Gtk::TreeViewColumn *column;
    _tree.get_cursor(path, column);
    if (!path || !column)
        return false;

    auto row = *_store->get_iter(path);
    if (!row)
        return false;

    if (column == _eye_column) {
        toggleVisible(state, row);
    } else if (column == _lock_column) {
        toggleLocked(state, row);
    } else if (column == _name_column) {
        auto item = getItem(row);
        auto group = cast<SPGroup>(item);
        _scroll_lock = true; // Clicking to select shouldn't scroll the treeview.

        if (Controller::has_flag(state, Gdk::ModifierType::SHIFT_MASK) && !selection->isEmpty()) {
            // Shift + Click or Shift + Ctrl + Click
            // TODO: Fix layers expand unexpectedly on range selection.
            selectRange(_initial_path, path);
        } else if (Controller::has_flag(state, Gdk::ModifierType::CONTROL_MASK)) {
            if (selection->includes(item, true)) {
                selection->remove(item);
            } else {
                selection->add(item, false);
                _initial_path = path;
                _start_new_range = true;
            }
        } else if (group && selection->includes(item) && !group->isLayer()) {
            // Clicking off a group (second click) will enter the group
            layers.setCurrentLayer(item, true);
        } else {
            // Just Click
            if (layers.currentLayer() == item || group) {
                layers.setCurrentLayer(item->parent);
            }

            selection->set(item);
            _initial_path = path;
            _start_new_range = true;
        }

        return true;
    }
    return false;
}

/**
 * User pressed return in search box, process search query.
 */
void ObjectsPanel::_searchActivated()
{
    // The root watcher and watcher tree handles the search operations
    setRootWatcher();
}

} // namespace Inkscape::UI::Dialog

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
