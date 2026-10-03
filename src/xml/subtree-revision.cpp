// SPDX-License-Identifier: GPL-2.0-or-later

#include "xml/subtree-revision.h"

#include <algorithm>
#include <utility>

#include "gc-anchored.h"
#include "xml/node.h"

namespace Inkscape::XML {

SubtreeRevision::SubtreeRevision(Node &root, int ignored_code, std::vector<int> ignored_child_codes)
    : _root(&root)
    , _ignored_code(ignored_code)
    , _ignored_child_codes(std::move(ignored_child_codes))
{
    GC::anchor(_root);
    _root->addSubtreeObserver(*this);
}

SubtreeRevision::~SubtreeRevision()
{
    _root->removeSubtreeObserver(*this);
    GC::release(_root);
}

void SubtreeRevision::notifyAttributeChanged(Node &node, GQuark, Util::ptr_shared, Util::ptr_shared)
{
    if (_ignored_code != 0 && node.code() == _ignored_code) {
        return; // the ignored element's own attributes
    }
    mark(&node, node.parent());
}

void SubtreeRevision::notifyContentChanged(Node &node, Util::ptr_shared, Util::ptr_shared)
{
    mark(&node, node.parent());
}

void SubtreeRevision::notifyElementNameChanged(Node &node, GQuark old_name, GQuark)
{
    // The node already carries its new name: a direct child of the ignored
    // element renamed into an ignored kind was real content before.
    auto const *parent = node.parent();
    if (_ignored_code != 0 && parent && parent->code() == _ignored_code &&
        std::find(_ignored_child_codes.begin(), _ignored_child_codes.end(), static_cast<int>(old_name)) ==
            _ignored_child_codes.end()) {
        _changed = true;
        return;
    }
    mark(&node, parent);
}

bool SubtreeRevision::ignorable(Node const *node, Node const *parent) const
{
    if (_ignored_code == 0) {
        return false;
    }
    // Find the ancestor-or-self whose parent is the ignored element; it must be
    // one of the ignored child kinds.
    while (node && parent) {
        if (parent->code() == _ignored_code) {
            return std::find(_ignored_child_codes.begin(), _ignored_child_codes.end(), node->code()) !=
                   _ignored_child_codes.end();
        }
        node = parent;
        parent = parent->parent();
    }
    return false;
}

void SubtreeRevision::mark(Node const *node, Node const *parent)
{
    if (!_changed && !ignorable(node, parent)) {
        _changed = true;
    }
}

} // namespace Inkscape::XML
