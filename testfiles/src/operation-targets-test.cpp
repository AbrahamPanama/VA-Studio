// SPDX-License-Identifier: GPL-2.0-or-later
#include "util/operation-targets.h"
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace Inkscape::Util;
struct Node {
    Node *parent = nullptr;
    Node *source = nullptr;
    TargetAvailability availability = TargetAvailability::Eligible;
};
auto resolve(std::vector<Node *> const &items) {
    return resolve_composite_targets(items,
        [](Node *n) { return n->availability; },
        [](Node *n) { return n->parent; },
        [](Node *n) { return n->source; });
}
void require(bool condition, char const *name) {
    if (!condition) throw std::runtime_error(name);
}
int main() {
    try {
        Node group, child{&group}, nested{&child}, other;
        Node missing{nullptr, nullptr, TargetAvailability::MissingSource};
        Node locked{nullptr, nullptr, TargetAvailability::Unavailable};
        Node clone{nullptr, &child}, direct_clone{nullptr, &other};
        require(resolve({}).items.empty(), "empty selection");
        require(resolve({&other}).items == std::vector<Node*>{&other}, "single item");
        require(resolve({&other, &child}).items == std::vector<Node*>{&other, &child}, "multiple items");
        auto r = resolve({&nested, &group, &child, &group});
        require(r.items == std::vector<Node*>{&group} && r.covered == 3, "ancestor/dedup independent of order");
        require(r.unavailable == 0 && r.missing_sources == 0, "covered is not skipped");
        r = resolve({&other, &missing, &locked, nullptr});
        require(r.items == std::vector<Node*>{&other} && r.missing_sources == 1 && r.unavailable == 2, "partial eligibility");
        require(resolve({&missing, &locked}).items.empty(), "unsupported only");
        require(resolve({&direct_clone, &other}).items == std::vector<Node*>{&other}, "source and instance once");
        require(resolve({&clone}).items == std::vector<Node*>{&clone}, "clone alone does not edit source");
        r = resolve({&group, &child, &clone});
        require(r.items == std::vector<Node*>{&group, &clone}, "outside clone does not inherit source ancestor filter");
        Node unavailable_parent{nullptr, nullptr, TargetAvailability::Unavailable};
        Node eligible_child{&unavailable_parent};
        require(resolve({&unavailable_parent, &eligible_child}).items == std::vector<Node*>{&eligible_child}, "ineligible parent does not suppress compatible child");
        require(group.parent == nullptr && child.parent == &group && clone.source == &child, "resolution is read only");
        std::cout << "PASS: 12 operation-target contract checks\n";
        return 0;
    } catch (std::exception const &e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
