// SPDX-License-Identifier: GPL-2.0-or-later
#include "bitmap-tone-member-targets.h"
#include <unordered_set>
#include <algorithm>
#include "bitmap-adjustment-chemistry.h"
#include "actions/actions-vacards-cli.h"
#include "actions/vacards-cli-fault.h"
#include "document.h"
#include "object/sp-image.h"
#include "object/sp-item-group.h"
#include "object/sp-use.h"
#include "util/operation-targets.h"
#include "xml/node.h"
namespace Inkscape::VACardsCli {
ToneMemberTargets resolve_tone_members(SPDocument &doc, std::vector<std::string> const &ids) {
    ToneMemberTargets result;
    std::vector<SPItem *> selected;
    for (auto const &id : ids) {
        auto item = cast<SPItem>(doc.getObjectById(id));
        if (!item) { result.error = ParseError{"unknown-id", "ids", "Explicit tone root does not exist."}; return result; }
        selected.push_back(item);
    }
    auto parent = [](SPItem *item) { return cast<SPItem>(item->parent); };
    auto roots = Util::resolve_composite_targets<SPItem>(selected,
        [](SPItem *) { return Util::TargetAvailability::Eligible; }, parent,
        [](SPItem *) -> SPItem * { return nullptr; });
    std::unordered_set<SPItem *> visited;
    auto idOf = [](SPItem *item) { return std::string(item->getId() ? item->getId() : ""); };
    for (auto item : selected) {
        if (std::find(roots.items.begin(), roots.items.end(), item) == roots.items.end())
            result.targets.covered_ids.push_back(idOf(item));
    }
    auto visit = [&](auto const &self, SPItem *item) -> void {
        if (!visited.insert(item).second) return;
        auto exclude = [&](char const *reason) { result.targets.exclusions.push_back({idOf(item), reason}); };
        if (!document_available(item)) { exclude("protected"); return; }
        // Never descend clone shadow trees or edit a clone's source through it.
        if (is<SPUse>(item)) { exclude("clone"); return; }
        if (is<SPGroup>(item)) {
            for (auto &child : item->children) if (auto member = cast<SPItem>(&child)) self(self, member);
            return;
        }
        auto image = cast<SPImage>(item);
        if (!image) { exclude("unsupported-target"); return; }
        if (!BitmapAdjustments::usable_bitmap(image)) { exclude("missing-source"); return; }
        for (auto p = item->parent; p; p = p->parent) {
            auto name = std::string_view(p->getRepr()->name());
            if (name == "svg:defs" || name == "svg:clipPath" || name == "svg:mask" || name == "svg:pattern") {
                exclude("resource-target"); return;
            }
            if (auto ancestor = cast<SPItem>(p); ancestor && BitmapAdjustments::query_tone(ancestor)) {
                exclude("ancestor-tone"); return;
            }
        }
        result.targets.ordered_roots.push_back(idOf(item));
    };
    for (auto root : roots.items) visit(visit, root);
    if (result.targets.ordered_roots.size() > cli_counted_limit("bitmap.tone-set.members", 100000))
        result.error = ParseError{"engine-limit", "ids", "Resolved tone member count exceeds 100000."};
    return result;
}
}
