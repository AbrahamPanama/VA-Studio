// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_UTIL_OPERATION_TARGETS_H
#define INKSCAPE_UTIL_OPERATION_TARGETS_H

#include <cstddef>
#include <unordered_set>
#include <vector>

namespace Inkscape::Util {

// Availability is separate from coverage: a descendant already affected through
// its selected ancestor is not an incompatible/skipped object.
enum class TargetAvailability { Eligible, Unavailable, MissingSource };

template <typename Item> struct OperationTargets {
    std::vector<Item *> items;
    std::size_t unavailable = 0;
    std::size_t missing_sources = 0;
    std::size_t covered = 0;
};

// Resolve independent roots for operations that affect a group's composited
// appearance. Do NOT use this to discover editable text/bitmap descendants or
// to infer a collective effect over unrelated roots. See SELECTION_CONTRACT.md.
// Callers supply an acyclic parent tree and a canonical referenced source (or
// null); resolving targets is read-only and must be repeated before mutation.
template <typename Item, typename Classify, typename Parent, typename Source>
OperationTargets<Item> resolve_composite_targets(std::vector<Item *> const &selected,
                                                Classify classify, Parent parent, Source source)
{
    OperationTargets<Item> result;
    std::vector<Item *> candidates;
    std::unordered_set<Item *> seen;
    for (auto item : selected) {
        if (!seen.insert(item).second) { ++result.covered; continue; }
        auto availability = item ? classify(item) : TargetAvailability::Unavailable;
        if (availability == TargetAvailability::MissingSource) { ++result.missing_sources; continue; }
        if (availability == TargetAvailability::Unavailable) { ++result.unavailable; continue; }
        candidates.push_back(item);
    }
    std::unordered_set<Item *> eligible(candidates.begin(), candidates.end());
    auto covered = [&](Item *item) {
        for (; item; item = parent(item)) {
            if (eligible.count(item)) return true;
        }
        return false;
    };
    std::vector<Item *> roots;
    for (auto item : candidates) {
        if (covered(parent(item))) ++result.covered;
        else roots.push_back(item);
    }
    std::unordered_set<Item *> root_set(roots.begin(), roots.end());
    for (auto item : roots) {
        // A use instance inherits its referenced source's own filter, but not
        // an outside ancestor's filter. Only a directly targeted source covers
        // the instance; a targeted source ancestor must not suppress it.
        if (auto original = source(item); original && original != item && root_set.count(original)) {
            ++result.covered;
        } else {
            result.items.push_back(item);
        }
    }
    return result;
}

} // namespace Inkscape::Util
#endif
