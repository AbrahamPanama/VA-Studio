// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * Long-press Boolean operation previews for the Shape Builder toolbar button.
 */

#ifndef INKSCAPE_UI_TOOLBAR_BOOLEAN_ASSIST_H
#define INKSCAPE_UI_TOOLBAR_BOOLEAN_ASSIST_H

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <2geom/pathvector.h>
#include <sigc++/connection.h>

#include "util-string/context-string.h" // Util::Internal::ContextString (RC_)

class InkscapeWindow;
class SPItem;
class SPDocument;

namespace Inkscape {
class ObjectSet;
namespace XML {
class Node;
} // namespace XML
} // namespace Inkscape

namespace Gtk {
class ToggleButton;
class Widget;
} // namespace Gtk

namespace Inkscape::UI::Toolbar {

/// The untransformed outline of a Boolean Assist operand: the object curve of a path, shape or text (no
/// i2dt applied), or nullopt when the item is not one of those types or its outline is empty.
[[nodiscard]] std::optional<Geom::PathVector> boolean_operand_path(SPItem *item);

/// True for objects Boolean Assist accepts as operands: a path, shape or text with non-empty geometry.
[[nodiscard]] bool boolean_operand_eligible(SPItem *item);

/// The Boolean Assistant operations, mapped one-to-one onto the ObjectSet boolean entry points.
enum class BooleanAssistOp { Union, Intersection, BottomMinusRest, TopMinusRest, Exclusion };

/// Leaf operands of a selected root: the root itself when boolean_operand_eligible(root); for an SPGroup, every
/// descendant SPItem that is not an SPGroup, in document order (groups nest). Returns false and sets *offending to
/// the first descendant that is neither an eligible operand nor a group, or that is unavailable(item) (hidden or
/// locked, as decided by the caller-supplied predicate), or when a group has no leaves at all (*offending = group).
/// When a group in the subtree (including the root group) carries a clip, mask, filter, group opacity or a
/// non-normal blend mode, fails with *offending = that group. If @a reason is non-null it receives one of
/// "group-effect", "not-a-shape", "empty-geometry", "unavailable" or "empty-group".
bool boolean_assist_leaves(SPItem *root, std::function<bool(SPItem *)> const &available,
                           std::vector<SPItem *> &leaves, SPItem **offending, std::string *reason = nullptr);

/// True when @a result is a freshly created object rather than an untouched operand of the boolean.
/// @a operand_reprs are the XML nodes of the operands, anchored before the operation (which keeps their addresses
/// from being reused by the freshly created result path). A null result is never new.
[[nodiscard]] bool boolean_result_is_new(SPItem *result,
                                         std::vector<Inkscape::XML::Node *> const &operand_reprs);

/// Apply op to the selected roots of @a set like Boolean Assist. Every group root is first replaced by the union of
/// its leaves (ObjectSet with those leaves -> pathUnion(true, true); the result must be exactly one SPPath).
/// Then the operation runs on the resulting roots with the Boolean Assist mapping (pathUnion / pathIntersect /
/// pathDiffMany(false) / pathDiffMany(true) / pathSymDiff), all with skip_undo=true, silent=true. Afterwards every
/// group this operation emptied is deleted (deleteObject). Records NO Undo step; the caller commits
/// (DocumentUndo::done with label boolean_assist_undo_label(op, operand_count)) or cancels.
/// Cleanup is limited to the selected group roots and the groups nested inside them; a selection without a group
/// root removes nothing. When @a removed_group_ids is non-null it receives the ids of the deleted groups.
/// Returns the resulting single SPPath, or nullptr when the result is not exactly one SPPath or when the only
/// remaining path is an untouched operand (the operands' XML nodes are anchored across the operation so a freed
/// operand's address cannot be reused by the fresh result). The caller must cancel on nullptr.
SPItem *apply_boolean_assist(Inkscape::ObjectSet &set, BooleanAssistOp op,
                             std::vector<std::string> *removed_group_ids = nullptr);

/// Explicit roles for the document service; no stacking-order or preference lookup.
enum class BooleanOperandOp { Union, Difference, Intersection, Exclusion, Division };
enum class BooleanEmptyPolicy { Refuse, Allow };
enum class BooleanOperandStatus { Applied, Refused };
enum class BooleanOperandReason {
    None, MissingDocument, InvalidOperation, InvalidCount, InvalidOperand, OverlappingOperands,
    Unavailable, GroupEffect, NotAShape, EmptyGeometry, EmptyGroup, UnsafeTransform, EmptyResult
};

struct BooleanOperandResult {
    BooleanOperandStatus status = BooleanOperandStatus::Refused;
    BooleanOperandReason reason = BooleanOperandReason::None;
    std::vector<std::string> output_ids;
    // Original root geometry consumed, in request order. Layers retain their structural IDs.
    std::vector<std::string> consumed_ids;
    std::string offending_id;
    std::string detail;
};

/// Collective geometry with whole-operand compatibility: every explicit root must be eligible,
/// visible/unlocked and distinct, with no ancestor/descendant overlap. Groups are unioned as one shape
/// using boolean_assist_leaves eligibility; incompatibility refuses the entire request before writing.
/// The first root is the subject and the source of output properties, parent, transform and position.
/// Difference subtracts each subsequent root in request order. Division requires exactly two roots;
/// union permits one, and the other operations require at least two. Empty-policy Refuse writes nothing;
/// Allow consumes all roots and can return zero paths. Output IDs identify replacements, not retained
/// operands (the last output reuses the subject ID, as in native division). A layer root retains its
/// container; its artwork is consumed and outputs with fresh IDs are placed inside that layer.
/// Caller supplies a current document and owns settlement: no Undo commit/cancel or preferences here.
/// Refusals preserve the document and pending caller changes. No preview is published by this service.
BooleanOperandResult apply_boolean_assist(SPDocument *document, std::vector<SPItem *> const &ordered_roots,
                                         BooleanOperandOp op, BooleanEmptyPolicy empty_policy);

/// The label/icon today's GUI records: Union "Union"/path-union; Intersection "Intersection"/path-intersection;
/// Exclusion "Exclusion"/path-exclusion; BottomMinusRest: 2 operands "Difference", more "Bottom minus other objects";
/// TopMinusRest: 2 operands "Reverse Difference", more "Top minus other objects"; both path-difference.
/// Uses RC_("Undo", ...) exactly as path-object-set.cpp does.
std::pair<Util::Internal::ContextString, char const *> boolean_assist_undo_label(BooleanAssistOp op,
                                                                                 std::size_t operand_count);

/// Watches raw SPItem pointers that a long-lived session keeps (Boolean Assist operands and group leaves). When
/// any watched item is released (for example Undo removing a member of a previewed group), @a on_release is
/// called with that item while it is still valid, and the watch forgets every item, so the session can drop its
/// pointers before anything dereferences them. Not copyable; destroying the watch disconnects it.
class ItemReleaseWatch final
{
public:
    explicit ItemReleaseWatch(std::function<void(SPItem *)> on_release);
    ~ItemReleaseWatch();

    ItemReleaseWatch(ItemReleaseWatch const &) = delete;
    ItemReleaseWatch &operator=(ItemReleaseWatch const &) = delete;

    /// Start watching @a item (null is ignored).
    void watch(SPItem *item);
    /// Stop watching everything; the callback is not called.
    void clear();
    /// True once a watched item has been released since the last clear().
    [[nodiscard]] bool released() const { return _released; }
    /// Number of items currently watched.
    [[nodiscard]] std::size_t size() const { return _connections.size(); }

private:
    std::function<void(SPItem *)> _on_release;
    std::vector<sigc::connection> _connections;
    bool _released = false;
};

class BooleanAssist final
{
public:
    BooleanAssist(Gtk::ToggleButton &button, Gtk::Widget &popover_parent, InkscapeWindow *window);
    ~BooleanAssist();

    BooleanAssist(BooleanAssist const &) = delete;
    BooleanAssist &operator=(BooleanAssist const &) = delete;

private:
    class Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace Inkscape::UI::Toolbar

#endif // INKSCAPE_UI_TOOLBAR_BOOLEAN_ASSIST_H
