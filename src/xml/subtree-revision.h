// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Records whether anything in an XML subtree changed since construction.
 */
#ifndef INKSCAPE_XML_SUBTREE_REVISION_H
#define INKSCAPE_XML_SUBTREE_REVISION_H

#include <vector>

#include "xml/node-observer.h"

namespace Inkscape::XML {

/// Records whether anything in a subtree changed since construction.
class SubtreeRevision final : public NodeObserver
{
public:
    /// With ignored_code 0 every change counts. Otherwise changes to the own
    /// attributes of an element whose name code equals ignored_code are
    /// ignored, and so are changes at or below its children whose name code is
    /// listed in ignored_child_codes (including adding, removing or reordering
    /// such children). Any other content under that element still counts:
    /// arbitrary elements there are real document objects (ids, styles).
    explicit SubtreeRevision(Node &root, int ignored_code = 0, std::vector<int> ignored_child_codes = {});
    ~SubtreeRevision() override;
    SubtreeRevision(SubtreeRevision const &) = delete;
    SubtreeRevision &operator=(SubtreeRevision const &) = delete;

    [[nodiscard]] bool changed() const noexcept { return _changed; }

    void notifyChildAdded(Node &node, Node &child, Node *) override { mark(&child, &node); }
    void notifyChildRemoved(Node &node, Node &child, Node *) override { mark(&child, &node); }
    void notifyChildOrderChanged(Node &node, Node &child, Node *, Node *) override { mark(&child, &node); }
    void notifyContentChanged(Node &node, Util::ptr_shared, Util::ptr_shared) override;
    void notifyAttributeChanged(Node &node, GQuark, Util::ptr_shared, Util::ptr_shared) override;
    void notifyElementNameChanged(Node &node, GQuark, GQuark) override;

private:
    /// `parent` is passed explicitly: a removed child is already detached.
    void mark(Node const *node, Node const *parent);
    [[nodiscard]] bool ignorable(Node const *node, Node const *parent) const;
    Node *_root; // GC-anchored in the constructor, released in the destructor
    int _ignored_code;
    std::vector<int> _ignored_child_codes;
    bool _changed = false;
};

} // namespace Inkscape::XML

#endif // INKSCAPE_XML_SUBTREE_REVISION_H
