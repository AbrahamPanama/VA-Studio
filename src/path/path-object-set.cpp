// SPDX-License-Identifier: GPL-2.0-or-later

/** @file
 *
 * Path related functions for ObjectSet.
 *
 * Copyright (C) 2020 Tavmjong Bah
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 *
 * TODO: Move related code from path-chemistry.cpp
 *
 */

#include <glibmm/i18n.h>
#include <glibmm/markup.h>
#include <2geom/rect.h>
#include <boost/geometry.hpp>
#include <boost/geometry/index/rtree.hpp>
#include <cmath>
#include <exception>
#include <iterator>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "desktop.h"
#include "display/dispatch-pool.h"
#include "document-undo.h"
#include "document.h"
#include "message-stack.h"
#include "path-boolop.h"
#include "path-chemistry.h"     // copy_object_properties()
#include "path-util.h"
#include "preferences.h"

#include "object/object-set.h"
#include "object/sp-flowtext.h"
#include "object/sp-shape.h"
#include "object/sp-text.h"
#include "path/path-outline.h"
#include "path/path-simplify.h"
#include "ui/icon-names.h"
#include "util/operation-targets.h"
#include "xml/repr-sorting.h"
#include "style.h"

namespace Inkscape::detail {
namespace {
std::atomic_size_t union_parallel_pairs_threshold{256};
std::atomic_size_t union_candidate_budget{8 * 1024 * 1024};
std::atomic_uint64_t union_throw_pair{std::numeric_limits<std::uint64_t>::max()};
std::atomic_bool union_fallback_exhaustive{false};
}
void set_union_parallel_pairs_threshold_for_testing(std::size_t threshold)
{
    union_parallel_pairs_threshold.store(threshold, std::memory_order_relaxed);
}
void set_union_candidate_budget_for_testing(std::size_t bytes)
{
    union_candidate_budget.store(bytes, std::memory_order_relaxed);
}
void set_union_fallback_exhaustive_for_testing(bool exhaustive)
{
    union_fallback_exhaustive.store(exhaustive, std::memory_order_relaxed);
}
void set_union_parallel_throw_pair_for_testing(unsigned i, unsigned j)
{
    union_throw_pair.store((static_cast<std::uint64_t>(i) << 32) | j, std::memory_order_relaxed);
}

// Internal to this implementation; declared by the focused test to exercise
// the actual bounded enumerator, including its fallback and touching boxes.
using UnionCandidate = std::pair<unsigned, unsigned>;
bool union_candidates_bounded(std::vector<Geom::Rect> const &bounds,
                              std::vector<UnionCandidate> &candidates,
                              std::size_t budget_bytes)
{
    // Reserve a single fixed allocation: neither vector growth nor remapping
    // may temporarily hold a second full pair list. Bounds/events are O(N).
    std::vector<UnionCandidate>().swap(candidates);
    auto const limit = budget_bytes / sizeof(UnionCandidate);
    auto const n = bounds.size();
    // Divide first and cap before multiplying, also on 32-bit size_t.
    auto const a = n % 2 == 0 ? n / 2 : n;
    auto const b = n < 2 ? 0 : (n % 2 == 0 ? n - 1 : (n - 1) / 2);
    auto const max_pairs = b && a > limit / b ? limit : a * b;
    candidates.reserve(std::min(limit, max_pairs));
    struct Event { double x; unsigned index; bool closing; };
    std::vector<Event> events;
    events.reserve(2 * n);
    for (unsigned i = 0; i < n; ++i) {
        events.push_back({bounds[i].left(), i, false});
        events.push_back({bounds[i].right(), i, true});
    }
    std::sort(events.begin(), events.end(), [](auto const &a, auto const &b) {
        // Opening before closing includes touching and zero-width boxes.
        if (a.x != b.x) return a.x < b.x;
        if (a.closing != b.closing) return a.closing < b.closing;
        return a.index < b.index;
    });
    std::vector<unsigned> active;
    active.reserve(n);
    for (auto const &event : events) {
        auto const i = event.index;
        if (event.closing) {
            active.erase(std::find(active.begin(), active.end(), i));
        } else {
            for (auto j : active) {
                if (bounds[i][Geom::Y].intersects(bounds[j][Geom::Y])) {
                    if (candidates.size() == limit) {
                        // Discard the entire partial index BEFORE the old
                        // on-demand exhaustive pass starts; never omit a pair.
                        std::vector<UnionCandidate>().swap(candidates);
                        return false;
                    }
                    candidates.emplace_back(std::max(i, j), std::min(i, j));
                }
            }
            active.push_back(i);
        }
    }
    // Original exhaustive (i,j) ordering preserves intersection-cut semantics.
    std::sort(candidates.begin(), candidates.end());
    return true;
}
} // namespace Inkscape::detail

using Inkscape::ObjectSet;

void Inkscape::ObjectSet::pathUnion(bool skip_undo, bool silent)
{
    _pathBoolOp(bool_op_union, INKSCAPE_ICON("path-union"), RC_("Undo", "Union"), skip_undo, silent);
}

void Inkscape::ObjectSet::pathIntersect(bool skip_undo, bool silent)
{
    _pathBoolOp(bool_op_inters, INKSCAPE_ICON("path-intersection"), RC_("Undo", "Intersection"), skip_undo, silent);
}

void Inkscape::ObjectSet::pathDiff(bool skip_undo, bool silent)
{
    _pathBoolOp(bool_op_diff, INKSCAPE_ICON("path-difference"), RC_("Undo", "Difference"), skip_undo, silent);
}

void Inkscape::ObjectSet::pathDiffReverse(bool skip_undo, bool silent)
{
    _pathBoolOp(bool_op_diff, INKSCAPE_ICON("path-difference"),
                RC_("Undo", "Reverse Difference"), skip_undo, silent, true);
}

void Inkscape::ObjectSet::pathDiffMany(bool keep_top, bool skip_undo, bool silent)
{
    auto items = items_vector();
    if (items.size() == 2) {
        if (keep_top) {
            pathDiffReverse(skip_undo, silent);
        } else {
            pathDiff(skip_undo, silent);
        }
        return;
    }

    if (items.size() < 2) {
        if (!silent) {
            auto const message = _("Select <b>at least 2 paths</b> to perform a multiple-object difference.");
            if (desktop()) {
                desktop()->messageStack()->flash(ERROR_MESSAGE, message);
            } else {
                g_printerr("%s\n", message);
            }
        }
        return;
    }

    for (auto item : items) {
        if (!is<SPShape>(item) && !is<SPText>(item) && !is<SPFlowtext>(item)) {
            if (!silent) {
                auto const message = _("Booleans need paths or shapes: ungroup first, or use Boolean Assist, which treats a group as one shape");
                if (desktop()) {
                    desktop()->messageStack()->flash(ERROR_MESSAGE, message);
                } else {
                    g_printerr("%s\n", message);
                }
            }
            return;
        }
    }

    // Define the operation by document stacking order, independent of the
    // order in which the user selected the objects. All non-anchor objects are
    // first unioned into one mask, then subtracted in a single logical command.
    std::sort(items.begin(), items.end(), sp_object_compare_position_bool);
    auto const anchor = keep_top ? items.back() : items.front();

    std::vector<SPItem *> subtractors;
    subtractors.reserve(items.size() - 1);
    for (auto item : items) {
        if (item != anchor) {
            subtractors.emplace_back(item);
        }
    }

    setList(subtractors);
    _pathBoolOp(bool_op_union, false);
    auto const mask = singleItem();
    if (!mask) {
        if (!silent && desktop()) {
            desktop()->messageStack()->flash(ERROR_MESSAGE, _("Unable to combine the subtracting objects."));
        }
        return;
    }

    setList(std::vector<SPItem *>{anchor, mask});
    _pathBoolOp(bool_op_diff, keep_top);

    if (!skip_undo) {
        auto const description = keep_top ? RC_("Undo", "Top minus other objects")
                                          : RC_("Undo", "Bottom minus other objects");
        DocumentUndo::done(document(), description, INKSCAPE_ICON("path-difference"));
    }
}

void Inkscape::ObjectSet::pathSymDiff(bool skip_undo, bool silent)
{
    _pathBoolOp(bool_op_symdiff, INKSCAPE_ICON("path-exclusion"), RC_("Undo", "Exclusion"), skip_undo, silent);
}

void Inkscape::ObjectSet::pathCut(bool skip_undo, bool silent)
{
    _pathBoolOp(bool_op_cut, INKSCAPE_ICON("path-division"), RC_("Undo", "Division"), skip_undo, silent);
}

void Inkscape::ObjectSet::pathSlice(bool skip_undo, bool silent)
{
    _pathBoolOp(bool_op_slice, INKSCAPE_ICON("path-cut"), RC_("Undo", "Cut path"), skip_undo, silent);
}

void Inkscape::ObjectSet::_pathBoolOp(BooleanOp bop, char const *icon_name,
                                     Inkscape::Util::Internal::ContextString description,
                                     bool skip_undo, bool silent, bool reverse_difference)
{
    try {
        for (auto item : items()) {
            if (!is<SPShape>(item) && !is<SPText>(item) && !is<SPFlowtext>(item)) {
                throw _("Booleans need paths or shapes: ungroup first, or use Boolean Assist, which treats a group as one shape");
            }
        }
        ObjectSet::_pathBoolOp(bop, reverse_difference);
        if (!skip_undo) {
            DocumentUndo::done(document(), description, icon_name);
        }
    } catch (char const *msg) {
        if (!silent) {
            if (desktop()) {
                desktop()->messageStack()->flash(ERROR_MESSAGE, msg);
            } else {
                g_printerr("%s\n", msg);
            }
        }
    }
}

void Inkscape::ObjectSet::_pathBoolOp(BooleanOp bop, bool reverse_difference)
{
    auto const doc = document();

    // Grab the items list.
    auto il = items_vector();

    // Validate number of items.
    switch (bop) {
        case bool_op_union:
            if (il.size() < 1) { // Allow union of single item --> flatten.
                throw _("Select <b>at least 1 path</b> to perform a boolean union.");
            }
            break;
        case bool_op_inters:
        case bool_op_symdiff:
            if (il.size() < 2) {
                throw _("Select <b>at least 2 paths</b> to perform an intersection or symmetric difference.");
            }
            break;
        case bool_op_diff:
        case bool_op_cut:
        case bool_op_slice:
            if (il.size() != 2) {
                throw _("Select <b>exactly 2 paths</b> to perform difference, division, or path cut.");
            }
            break;
    }
    assert(!il.empty());

    // reverseOrderForOp marks whether the order of the list is the top->down order
    // it's only used when there are 2 objects, and for operations which need to know the
    // topmost object (differences, cuts)
    bool reverseOrderForOp = false;

    if (bop == bool_op_diff || bop == bool_op_cut || bop == bool_op_slice) {
        // check in the tree to find which element of the selection list is topmost (for 2-operand commands only)
        Inkscape::XML::Node *a = il.front()->getRepr();
        Inkscape::XML::Node *b = il.back()->getRepr();

        if (!a || !b) {
            return;
        }

        if (is_descendant_of(a, b)) {
            // a is a child of b, already in the proper order
        } else if (is_descendant_of(b, a)) {
            // reverse order
            reverseOrderForOp = true;
        } else {

            // objects are not in parent/child relationship;
            // find their lowest common ancestor
            Inkscape::XML::Node *parent = lowest_common_ancestor(a, b);
            if (!parent) {
                return;
            }

            // find the children of the LCA that lead from it to the a and b
            Inkscape::XML::Node *as = find_containing_child(a, parent);
            Inkscape::XML::Node *bs = find_containing_child(b, parent);

            // find out which comes first
            for (Inkscape::XML::Node *child = parent->firstChild(); child; child = child->next()) {
                if (child == as) {
                    /* a first, so reverse. */
                    reverseOrderForOp = true;
                    break;
                }
                if (child == bs)
                    break;
            }
        }
    }

    if (bop == bool_op_diff && reverse_difference) {
        reverseOrderForOp = !reverseOrderForOp;
    }

    // first check if all the input objects have shapes
    // otherwise bail out
    for (auto item : il) {
        if (!is<SPShape>(item) && !is<SPText>(item) && !is<SPFlowtext>(item)) {
            return;
        }
    }

    struct Operand
    {
        // From source objects
        FillRule fill_rule{};
        Geom::PathVector pathv;

        // Computed
        std::vector<Geom::PathVectorTime> cuts;
        std::unique_ptr<Path> path;
    };

    std::vector<Operand> operands;
    operands.resize(il.size());

    // Validate every operand BEFORE baking any live path effect: baking is a
    // document mutation, and bailing out after it would leave earlier
    // operands permanently stripped of their effects with no result.
    // Operands with a live path effect are not judged here: their own stored
    // path may be empty while the effect generates the geometry (clone
    // original and similar), so only the post-bake check applies to them.
    // Geometry computed here is reused below unless baking could have changed it.
    std::vector<std::optional<Geom::PathVector>> prepared(il.size());
    for (std::size_t i = 0; i < il.size(); i++) {
        auto const lpeitem = cast<SPLPEItem>(il[i]);
        if (lpeitem && lpeitem->hasPathEffect()) {
            continue;
        }
        auto curve = curve_for_item(il[i]);
        if (!curve || std::none_of(curve->begin(), curve->end(),
                                   [](Geom::Path const &path) { return !path.empty(); })) {
            return; // no geometry: the operation below would bail out anyway
        }
        prepared[i] = *curve * il[i]->i2doc_affine();
    }

    // The first bake takes a fence around the caller's own pending (not yet
    // committed) changes, so that a later failure rolls back only what this
    // call changed. Without a fence (insensitive document, live interaction)
    // nothing can be rolled back here: the failure is reported and the caller
    // decides.
    bool baked_any = false;
    std::optional<DocumentUndo::PendingFence> fence;
    struct FenceRelease
    {
        SPDocument *doc;
        std::optional<DocumentUndo::PendingFence> &fence;
        ~FenceRelease()
        {
            if (fence) {
                DocumentUndo::reattachPendingChanges(doc, *fence);
            }
        }
    } fence_release{doc, fence};
    auto const abandon = [&] {
        if (!baked_any) {
            return;
        }
        if (fence) {
            DocumentUndo::rollbackToDetachedChanges(doc, *fence);
            fence.reset();
        } else {
            g_warning("Boolean operation failed after baking path effects and cannot be rolled back here.");
        }
    };

    // Extract the fill rules and pathvectors from the source objects.
    for (int i = 0; i < il.size(); i++) {
        auto &item = il[i];
        auto &operand = operands[i];

        // apply live path effects prior to performing boolean operation
        if (auto lpeitem = cast<SPLPEItem>(item); lpeitem && lpeitem->hasPathEffect()) {
            // Baking a native shape can replace (and delete) its SPItem. Keep
            // the replacement in the operand list used for style and deletion,
            // not only in a temporary local pointer. Some effects remove their
            // holder altogether; that leaves no operand to combine.
            if (!baked_any) {
                baked_any = true;
                fence = DocumentUndo::detachPendingChanges(doc);
            }
            // Baking can delete the item (an effect whose result has no
            // curves removes its holder) while still returning its pointer.
            // Resolve the operand again by id instead of trusting that pointer.
            std::string const baked_id = lpeitem->getId() ? lpeitem->getId() : "";
            item = lpeitem->removeAllPathEffects(true);
            lpeitem = nullptr;
            item = baked_id.empty() ? nullptr : cast<SPItem>(doc->getObjectById(baked_id));
            if (!item) {
                abandon();
                return;
            }
        }

        // Get the fill rule.
        if (item->style->fill_rule.computed == SP_WIND_RULE_EVENODD) {
            operand.fill_rule = fill_oddEven;
        } else {
            operand.fill_rule = fill_nonZero;
        }

        // Get the pathvector.
        if (prepared[i] && !baked_any) {
            operand.pathv = std::move(*prepared[i]);
            continue;
        }
        auto curve = curve_for_item(item);
        if (!curve) {
            abandon();
            return;
        }

        operand.pathv = *curve * item->i2doc_affine();
    }

    // Compute the intersections and self-intersections, and use this information when converting to livarot paths.
    // Union's curves already live in document space. Cache conservative control-hull
    // bounds once, then sweep boxes (including touching edges) before intersecting
    // curves. Other operations retain their original preprocessing path.
    // 8 MiB is an allocation budget, not a geometry/accuracy threshold.
    // Dense selections retain the original exhaustive on-demand preprocessing.
    auto const candidate_budget = detail::union_candidate_budget.load(std::memory_order_relaxed);
    // Below this size the single merge tree is fast and keeps the legacy contour order.
    // The below-threshold order is pinned by BoolopAttrTest.Union (testfiles/src/boolop-attr-test.cpp:184).
    constexpr std::size_t union_component_min_operands = 256;
    std::vector<detail::UnionCandidate> union_candidates;
    std::vector<Geom::Rect> union_bounds;
    bool indexed_union = bop == bool_op_union;
    bool union_bounds_usable = false;
    bool stream_union_fallback = false;
    if (indexed_union) {
        union_bounds.reserve(operands.size());
        union_bounds_usable = true;
        for (auto const &operand : operands) {
            auto const box = operand.pathv.boundsFast();
            if (!box || !std::isfinite(box->left()) || !std::isfinite(box->right()) ||
                !std::isfinite(box->top()) || !std::isfinite(box->bottom())) {
                // Preserve the old behavior for geometry without usable bounds.
                indexed_union = false;
                union_bounds_usable = false;
                break;
            }
            union_bounds.push_back(*box);
        }
        if (indexed_union) {
            indexed_union = detail::union_candidates_bounded(union_bounds, union_candidates, candidate_budget);
            stream_union_fallback = !indexed_union && union_bounds_usable &&
                                    !detail::union_fallback_exhaustive.load(std::memory_order_relaxed);
        }
    }
    using Pair = std::pair<int, int>;
    std::vector<Pair> pairs;
    if (indexed_union) {
        pairs.reserve(union_candidates.size());
        for (auto const &[i, j] : union_candidates) pairs.emplace_back(i, j);
    }
    auto for_each_streamed_union_pair = [&](auto &&emit) {
        using Point = boost::geometry::model::point<double, 2, boost::geometry::cs::cartesian>;
        using Box = boost::geometry::model::box<Point>;
        using Value = std::pair<Box, unsigned>;
        std::vector<Value> values;
        values.reserve(union_bounds.size());
        for (unsigned i = 0; i < union_bounds.size(); ++i) {
            auto const &bounds = union_bounds[i];
            auto const min_x = bounds.left();
            auto const max_x = bounds.right();
            auto const min_y = std::min(bounds.top(), bounds.bottom());
            auto const max_y = std::max(bounds.top(), bounds.bottom());
            values.emplace_back(Box{Point{min_x, min_y}, Point{max_x, max_y}}, i);
        }
        boost::geometry::index::rtree<Value, boost::geometry::index::quadratic<16>> index(values.begin(), values.end());
        std::vector<Value> row;
        row.reserve(union_bounds.size());
        for (unsigned i = 0; i < union_bounds.size(); ++i) {
            row.clear();
            index.query(boost::geometry::index::intersects(values[i].first), std::back_inserter(row));
            auto const &box = union_bounds[i];
            row.erase(std::remove_if(row.begin(), row.end(), [&](Value const &overlap) {
                auto const j = overlap.second;
                if (j >= i) return true;
                auto const &prior = union_bounds[j];
                // Match the indexed path's closed X and Y interval predicate exactly.
                return !(box.left() <= prior.right() && prior.left() <= box.right() &&
                         box[Geom::Y].intersects(prior[Geom::Y]));
            }), row.end());
            std::sort(row.begin(), row.end(), [](Value const &a, Value const &b) {
                return a.second < b.second;
            });
            for (auto const &overlap : row) emit(Pair{static_cast<int>(i), static_cast<int>(overlap.second)});
        }
    };
    auto process_pair = [&](Pair const &pair) {
        distribute_intersection_times(operands[pair.first].cuts, operands[pair.second].cuts,
                                      operands[pair.first].pathv.intersect(operands[pair.second].pathv));
    };
    auto const threshold = detail::union_parallel_pairs_threshold.load(std::memory_order_relaxed);
#ifdef VA_2GEOM_THREAD_LOCAL_CLIPPING
    auto exhaustive_parallel_enough = [&] {
        auto const n = operands.size();
        auto const a = n % 2 == 0 ? n / 2 : n;
        auto const b = n < 2 ? 0 : (n % 2 == 0 ? n - 1 : (n - 1) / 2);
        return b && a > std::numeric_limits<std::size_t>::max() / b ? true : a * b >= threshold;
    };
    auto const exhaustive_count_nonzero = operands.size() > 1;
    auto const parallel = indexed_union ? (!pairs.empty() && pairs.size() >= threshold)
                                        : (exhaustive_count_nonzero && exhaustive_parallel_enough());
    if (!parallel) {
        if (indexed_union) {
            for (auto const &pair : pairs) process_pair(pair);
        } else if (stream_union_fallback) {
            for_each_streamed_union_pair(process_pair);
        } else {
            for (int i = 0; i < operands.size(); ++i) {
                for (int j = 0; j < i; ++j) process_pair({i, j});
            }
        }
    } else {
        auto const pool_size = std::clamp<int>(std::thread::hardware_concurrency(), 1, 8);
        Inkscape::dispatch_pool pool{pool_size};
        constexpr std::size_t chunk_size = 16384;
        std::vector<Pair> chunk;
        chunk.reserve(chunk_size);
        auto dispatch_chunk = [&](std::vector<Pair> const &work) {
            auto const count = work.size();
            std::vector<std::vector<Geom::PathVectorIntersection>> results(count);
            std::vector<std::exception_ptr> errors(count);
            pool.dispatch(static_cast<int>(count), [&](int index, int) {
                auto const [i, j] = work[index];
                auto const injected_pair = detail::union_throw_pair.load(std::memory_order_relaxed);
                try {
                    if (injected_pair == ((static_cast<std::uint64_t>(i) << 32) | static_cast<unsigned>(j)))
                        throw std::runtime_error("injected Union intersection failure");
                    results[index] = operands[i].pathv.intersect(operands[j].pathv);
                } catch (...) {
                    errors[index] = std::current_exception();
                }
            });
            for (std::size_t k = 0; k < count; ++k) {
                if (errors[k]) std::rethrow_exception(errors[k]);
                auto const [i, j] = work[k];
                distribute_intersection_times(operands[i].cuts, operands[j].cuts, results[k]);
            }
        };
        if (indexed_union) {
            for (std::size_t first = 0; first < pairs.size(); first += chunk_size) {
                auto const count = std::min(chunk_size, pairs.size() - first);
                dispatch_chunk(std::vector<Pair>(pairs.begin() + first, pairs.begin() + first + count));
            }
        } else if (stream_union_fallback) {
            for_each_streamed_union_pair([&](Pair const &pair) {
                chunk.push_back(pair);
                if (chunk.size() == chunk_size) {
                    dispatch_chunk(chunk);
                    chunk.clear();
                }
            });
            if (!chunk.empty()) dispatch_chunk(chunk);
        } else {
            for (int i = 0; i < operands.size(); ++i) {
                for (int j = 0; j < i; ++j) {
                    chunk.emplace_back(i, j);
                    if (chunk.size() == chunk_size) {
                        dispatch_chunk(chunk);
                        chunk.clear();
                    }
                }
            }
            if (!chunk.empty()) dispatch_chunk(chunk);
        }
    }
#else
    if (indexed_union) {
        for (auto const &pair : pairs) process_pair(pair);
    } else if (stream_union_fallback) {
        for_each_streamed_union_pair(process_pair);
    } else {
        for (int i = 0; i < operands.size(); ++i) {
            for (int j = 0; j < i; ++j) process_pair({i, j});
        }
    }
#endif

    std::vector<unsigned> component_of(operands.size());
    std::size_t component_count = 1;
    // Keep the component split off here; enabling it on fallback would reorder output subpaths.
    if (bop == bool_op_union && indexed_union && operands.size() >= union_component_min_operands) {
        std::vector<unsigned> parent(operands.size());
        std::vector<unsigned> size(operands.size(), 1);
        for (unsigned i = 0; i < parent.size(); ++i) parent[i] = i;
        auto find_root = [&](unsigned i) {
            unsigned root = i;
            while (parent[root] != root) root = parent[root];
            while (parent[i] != i) {
                auto next = parent[i];
                parent[i] = root;
                i = next;
            }
            return root;
        };
        for (auto const &[i, j] : union_candidates) {
            auto a = find_root(i);
            auto b = find_root(j);
            if (a != b) {
                if (size[a] < size[b]) std::swap(a, b);
                parent[b] = a;
                size[a] += size[b];
            }
        }
        constexpr auto no_component = static_cast<unsigned>(-1);
        std::vector<unsigned> component_for_root(operands.size(), no_component);
        component_count = 0;
        for (unsigned i = 0; i < operands.size(); ++i) {
            auto root = find_root(i);
            if (component_for_root[root] == no_component) {
                component_for_root[root] = component_count++;
            }
            component_of[i] = component_for_root[root];
        }
    }

    // No candidate storage survives into path normalization or Shape reduction.
    std::vector<detail::UnionCandidate>().swap(union_candidates);

    for (auto &operand : operands) {
        distribute_intersection_times(operand.cuts, operand.cuts, operand.pathv.intersectSelf());
        sort_and_clean_intersection_times(operand.cuts);

        operand.path = std::make_unique<Path>();
        operand.path->LoadPathVector(operand.pathv, operand.cuts);
        operand.path->ConvertWithBackData(RELATIVE_THRESHOLD, true);

        if (operand.path->descr_cmd.size() <= 1) {
            abandon();
            return;
        }
    }

    // reverse if needed
    // note that the selection list keeps its order
    if (reverseOrderForOp) {
        std::swap(operands[0], operands[1]);
    }

    // and work
    // some temporary instances, first
    Shape *theShapeA = new Shape;
    Shape *theShapeB = new Shape;
    Shape *theShape = new Shape;
    Path *res = new Path;
    Path::cut_position  *toCut=nullptr;
    int                  nbToCut=0;

    auto get_path_arr = [&] {
        std::vector<Path *> result;
        result.reserve(operands.size());
        for (auto &operand : operands) result.emplace_back(operand.path.get());
        return result;
    };
    bool union_res_ready = false;
    auto reduce_union = [&](std::vector<std::unique_ptr<Shape>> level) -> std::unique_ptr<Shape> {
        while (level.size() > 1) {
            std::vector<std::unique_ptr<Shape>> next;
            next.reserve((level.size() + 1) / 2);
            for (std::size_t i = 0; i < level.size(); i += 2) {
                if (i + 1 == level.size() || level[i + 1]->numberOfEdges() == 0) {
                    next.push_back(std::move(level[i]));
                } else if (level[i]->numberOfEdges() == 0) {
                    next.push_back(std::move(level[i + 1]));
                } else {
                    auto merged = std::make_unique<Shape>();
                    merged->Booleen(level[i + 1].get(), level[i].get(), bool_op_union);
                    next.push_back(std::move(merged));
                }
                // Moves leave null slots; reset also destroys the consumed
                // nonempty pair and any empty partner immediately, not at the
                // end of the level while all of next is already resident.
                level[i].reset();
                if (i + 1 < level.size()) level[i + 1].reset();
            }
            level = std::move(next);
        }
        return level.empty() ? std::make_unique<Shape>() : std::move(level.front());
    };

    if (bop == bool_op_union) {
        // Normalize each leaf exactly once using its own winding rule. The index
        // passed to Fill remains the global backdata path ID at every tree level;
        // operands (and their original Paths) outlive the final ConvertToForme.
        if (component_count == 1) {
            std::vector<std::unique_ptr<Shape>> leaves;
            leaves.reserve(operands.size());
            for (int i = 0; i < operands.size(); ++i) {
                operands[i].path->Fill(theShape, i);
                auto leaf = std::make_unique<Shape>();
                leaf->ConvertToShape(theShape, operands[i].fill_rule);
                leaves.push_back(std::move(leaf));
            }
            delete theShape;
            theShape = reduce_union(std::move(leaves)).release();
        } else {
            Geom::PathVector combined;
            std::vector<std::vector<unsigned>> members(component_count);
            for (unsigned i = 0; i < operands.size(); ++i) members[component_of[i]].push_back(i);
            auto const path_arr = get_path_arr();
            for (unsigned component = 0; component < component_count; ++component) {
                std::vector<std::unique_ptr<Shape>> leaves;
                for (auto i : members[component]) {
                    operands[i].path->Fill(theShape, i);
                    auto leaf = std::make_unique<Shape>();
                    leaf->ConvertToShape(theShape, operands[i].fill_rule);
                    leaves.push_back(std::move(leaf));
                }
                auto reduced = reduce_union(std::move(leaves));
                if (reduced->numberOfEdges() > 0) {
                    Path component_path;
                    reduced->ConvertToForme(&component_path, operands.size(), path_arr.data());
                    auto paths = component_path.MakePathVector();
                    combined.insert(combined.end(), paths.begin(), paths.end());
                }
            }
            res->LoadPathVector(combined);
            union_res_ready = true;
        }
        // No document objects participate in this reduction. The existing anchor
        // selection, one final replacement, and caller's Undo settlement below
        // are independent of the merge tree.
    } else if (bop == bool_op_inters || bop == bool_op_diff || bop == bool_op_symdiff) {
        // true boolean op
        // get the polygons of each path, with the winding rule specified, and apply the operation iteratively

        operands[0].path->Fill(theShape, 0);

        theShapeA->ConvertToShape(theShape, operands[0].fill_rule);

        for (int i = 1; i < operands.size(); i++) {
            operands[i].path->Fill(theShape, i);

            theShapeB->ConvertToShape(theShape, operands[i].fill_rule);

            /* Due to quantization of the input shape coordinates, we may end up with A or B being empty.
             * If this is a union or symdiff operation, we just use the non-empty shape as the result:
             *   A=0  =>  (0 or B) == B
             *   B=0  =>  (A or 0) == A
             *   A=0  =>  (0 xor B) == B
             *   B=0  =>  (A xor 0) == A
             * If this is an intersection operation, we just use the empty shape as the result:
             *   A=0  =>  (0 and B) == 0 == A
             *   B=0  =>  (A and 0) == 0 == B
             * If this a difference operation, and the upper shape (A) is empty, we keep B.
             * If the lower shape (B) is empty, we still keep B, as it's empty:
             *   A=0  =>  (B - 0) == B
             *   B=0  =>  (0 - A) == 0 == B
             *
             * In any case, the output from this operation is stored in shape A, so we may apply
             * the above rules simply by judicious use of swapping A and B where necessary.
             */
            bool zeroA = theShapeA->numberOfEdges() == 0;
            bool zeroB = theShapeB->numberOfEdges() == 0;
            if (zeroA || zeroB) {
                // We might need to do a swap. Apply the above rules depending on operation type.
                bool resultIsB =   ((bop == bool_op_union || bop == bool_op_symdiff) && zeroA)
                                   || ((bop == bool_op_inters) && zeroB)
                                   ||  (bop == bool_op_diff);
                if (resultIsB) {
                    // Swap A and B to use B as the result
                    std::swap(theShapeA, theShapeB);
                }
            } else {
                // Just do the Boolean operation as usual
                // les elements arrivent en ordre inverse dans la liste
                theShape->Booleen(theShapeB, theShapeA, bop);
                std::swap(theShape, theShapeA);
            }
        }

        std::swap(theShape, theShapeA);

    } else if (bop == bool_op_cut) {
        // cuts= sort of a bastard boolean operation, thus not the axact same modus operandi
        // technically, the cut path is not necessarily a polygon (thus has no winding rule)
        // it is just uncrossed, and cleaned from duplicate edges and points
        // then it's fed to Booleen() which will uncross it against the other path
        // then comes the trick: each edge of the cut path is duplicated (one in each direction),
        // thus making a polygon. the weight of the edges of the cut are all 0, but
        // the Booleen need to invert the ones inside the source polygon (for the subsequent
        // ConvertToForme)

        // the cut path needs to have the highest pathID in the back data
        // that's how the Booleen() function knows it's an edge of the cut
        std::swap(operands[0], operands[1]);

        operands[0].path->Fill(theShape, 0);

        theShapeA->ConvertToShape(theShape, operands[0].fill_rule);

        operands[1].path->Fill(theShape, 1, false, is_line(*operands[1].path), false);

        theShapeB->ConvertToShape(theShape, fill_justDont);

        // les elements arrivent en ordre inverse dans la liste
        theShape->Booleen(theShapeB, theShapeA, bool_op_cut, 1);

    } else if (bop == bool_op_slice) {
        // slice is not really a boolean operation
        // you just put the 2 shapes in a single polygon, uncross it
        // the points where the degree is > 2 are intersections
        // just check it's an intersection on the path you want to cut, and keep it
        // the intersections you have found are then fed to ConvertPositionsToMoveTo() which will
        // make new subpath at each one of these positions
        // inversion pour l'opération
        std::swap(operands[0], operands[1]);

        operands[0].path->Fill(theShapeA, 0, false, false, false); // don't closeIfNeeded

        operands[1].path->Fill(theShapeA, 1, true, false, false); // don't closeIfNeeded and just dump in the shape, don't reset it

        theShape->ConvertToShape(theShapeA, fill_justDont);

        if (theShape->hasBackData()) {
            // should always be the case, but ya never know
            {
                for (int i = 0; i < theShape->numberOfPoints(); i++) {
                    if ( theShape->getPoint(i).totalDegree() > 2 ) {
                        // possibly an intersection
                        // we need to check that at least one edge from the source path is incident to it
                        // before we declare it's an intersection
                        int cb = theShape->getPoint(i).incidentEdge[FIRST];
                        int   nbOrig=0;
                        int   nbOther=0;
                        int   piece=-1;
                        float t=0.0;
                        while ( cb >= 0 && cb < theShape->numberOfEdges() ) {
                            if ( theShape->ebData[cb].pathID == 0 ) {
                                // the source has an edge incident to the point, get its position on the path
                                piece=theShape->ebData[cb].pieceID;
                                if ( theShape->getEdge(cb).st == i ) {
                                    t=theShape->ebData[cb].tSt;
                                } else {
                                    t=theShape->ebData[cb].tEn;
                                }
                                nbOrig++;
                            }
                            if ( theShape->ebData[cb].pathID == 1 ) nbOther++; // the cut is incident to this point
                            cb=theShape->NextAt(i, cb);
                        }
                        if ( nbOrig > 0 && nbOther > 0 ) {
                            // point incident to both path and cut: an intersection
                            // note that you only keep one position on the source; you could have degenerate
                            // cases where the source crosses itself at this point, and you wouyld miss an intersection
                            toCut=(Path::cut_position*)realloc(toCut, (nbToCut+1)*sizeof(Path::cut_position));
                            toCut[nbToCut].piece=piece;
                            toCut[nbToCut].t=t;
                            nbToCut++;
                        }
                    }
                }
            }
            {
                // i think it's useless now
                int i = theShape->numberOfEdges() - 1;
                for (;i>=0;i--) {
                    if ( theShape->ebData[i].pathID == 1 ) {
                        theShape->SubEdge(i);
                    }
                }
            }

        }
    }

    int*    nesting=nullptr;
    int*    conts=nullptr;
    int     nbNest=0;
    // pour compenser le swap juste avant
    if (bop == bool_op_slice) {
        res->Copy(operands[0].path.get());
        res->ConvertPositionsToMoveTo(nbToCut, toCut); // cut where you found intersections
        free(toCut);
    } else if (bop == bool_op_cut) {
        // il faut appeler pour desallouer PointData (pas vital, mais bon)
        // the Booleen() function did not deallocate the point_data array in theShape, because this
        // function needs it.
        // this function uses the point_data to get the winding number of each path (ie: is a hole or not)
        // for later reconstruction in objects, you also need to extract which path is parent of holes (nesting info)
        theShape->ConvertToFormeNested(res, operands.size(), get_path_arr().data(), nbNest, nesting, conts, true);
    } else if (!union_res_ready) {
        theShape->ConvertToForme(res, operands.size(), get_path_arr().data());
    }

    delete theShape;
    delete theShapeA;
    delete theShapeB;

    if (res->descr_cmd.size() <= 1) {
        // only one command, presumably a moveto: it isn't a path
        for (auto l : il){
            l->deleteObject();
        }
        clear();

        delete res;
        return;
    }

    // get the source path object
    SPObject *source;
    if ( bop == bool_op_diff || bop == bool_op_cut || bop == bool_op_slice ) {
        if (reverseOrderForOp) {
            source = il[0];
        } else {
            source = il.back();
        }
    } else {
        // find out the bottom object
        std::vector<Inkscape::XML::Node *> sorted;
        for (auto item : il) sorted.push_back(item->getRepr());
        std::sort(sorted.begin(), sorted.end(), sp_repr_compare_position_bool);
        source = doc->getObjectByRepr(sorted.front());
    }

    // adjust style properties that depend on a possible transform in the source object in order
    // to get a correct style attribute for the new path
    auto item_source = cast<SPItem>(source);
    Geom::Affine i2doc(item_source->i2doc_affine());

    Inkscape::XML::Node *repr_source = source->getRepr();

    // remember important aspects of the source path, to be restored
    gint pos = repr_source->position();
    Inkscape::XML::Node *parent = repr_source->parent();
    // remove source paths
    clear();
    for (auto l : il){
        if (l != item_source) {
            // delete the object for real, so that its clones can take appropriate action
            l->deleteObject();
        }
    }

    auto const source2doc_inverse = i2doc.inverse();
    // The source is deleted before this value is applied to the new paths.
    auto const transform = repr_source->attribute("transform");
    std::string const old_transform_attribute = transform ? transform : "";

    // now that we have the result, add it on the canvas
    if ( bop == bool_op_cut || bop == bool_op_slice ) {
        int    nbRP=0;
        Path** resPath;
        if ( bop == bool_op_slice ) {
            // there are moveto's at each intersection, but it's still one unique path
            // so break it down and add each subpath independently
            // we could call break_apart to do this, but while we have the description...
            resPath=res->SubPaths(nbRP, false);
        } else {
            // cut operation is a bit wicked: you need to keep holes
            // that's why you needed the nesting
            // ConvertToFormeNested() dumped all the subpath in a single Path "res", so we need
            // to get the path for each part of the polygon. that's why you need the nesting info:
            // to know in which subpath to add a subpath
            resPath=res->SubPathsWithNesting(nbRP, true, nbNest, nesting, conts);

            // cleaning
            if ( conts ) free(conts);
            if ( nesting ) free(nesting);
        }

        // add all the pieces resulting from cut or slice
        std::vector <Inkscape::XML::Node*> selection;
        for (int i=0;i<nbRP;i++) {
            resPath[i]->Transform(source2doc_inverse);

            Inkscape::XML::Document *xml_doc = doc->getReprDoc();
            Inkscape::XML::Node *repr = xml_doc->createElement("svg:path");

            Inkscape::copy_object_properties(repr, repr_source);

            // Delete source on last iteration (after we don't need repr_source anymore). As a consequence, the last
            // item will inherit the original's id.
            if (i + 1 == nbRP) {
                item_source->deleteObject(false);
            }

            repr->setAttribute("d", resPath[i]->svg_dump_path().c_str());

            // for slice, remove fill
            if (bop == bool_op_slice) {
                SPCSSAttr *css;

                css = sp_repr_css_attr_new();
                sp_repr_css_set_property(css, "fill", "none");

                sp_repr_css_change(repr, css, "style");

                sp_repr_css_attr_unref(css);
            }

            repr->setAttributeOrRemoveIfEmpty("transform", old_transform_attribute.c_str());

            // add the new repr to the parent
            // move to the saved position
            parent->addChildAtPos(repr, pos);

            selection.push_back(repr);
            Inkscape::GC::release(repr);

            delete resPath[i];
        }
        setReprList(selection);
        if ( resPath ) free(resPath);

    } else {
        res->Transform(source2doc_inverse);

        Inkscape::XML::Document *xml_doc = doc->getReprDoc();
        Inkscape::XML::Node *repr = xml_doc->createElement("svg:path");

        Inkscape::copy_object_properties(repr, repr_source);

        // delete it so that its clones don't get alerted; this object will be restored shortly, with the same id
        item_source->deleteObject(false);

        repr->setAttribute("d", res->svg_dump_path().c_str());

        repr->setAttributeOrRemoveIfEmpty("transform", old_transform_attribute.c_str());

        parent->addChildAtPos(repr, pos);

        set(repr);
        Inkscape::GC::release(repr);
    }

    delete res;
}

bool ObjectSet::strokesToPaths(bool legacy, bool skip_undo)
{
    if (desktop() && isEmpty()) {
        desktop()->messageStack()->flash(Inkscape::WARNING_MESSAGE, _("Select <b>stroked path(s)</b> to convert stroke to path."));
        return false;
    }

    auto doc = document();
    if (!doc || isEmpty()) return false;
    auto prefs = Inkscape::Preferences::get();
    StrokeToPathConversion conversion;
    conversion.unlink_clones = prefs->getBool("/options/pathoperationsunlink/value", true);
    auto selected = items_vector();
    std::vector<std::string> original_ids;
    for (auto item : selected) {
        // Restoration must be possible without assigning IDs during preflight.
        if (!item->getId()) return false;
        original_ids.emplace_back(item->getId());
    }
    auto roots = Util::resolve_composite_targets(selected,
        [](SPItem *) { return Util::TargetAvailability::Eligible; },
        [](SPItem *item) { return cast<SPItem>(item->parent); },
        [](SPItem *) -> SPItem * { return nullptr; }); // Instances are independent of their source.
    std::vector<std::string> eligible;
    for (auto item : roots.items) {
        if (item_to_paths_preflight(item, legacy, conversion)) eligible.emplace_back(item->getId());
    }
    auto report_exclusions = [&] {
        if (desktop() && !conversion.exclusions.empty()) {
            Glib::ustring message = _("Stroke to Path exclusions:");
            for (auto const &reason : conversion.exclusions) {
                message += "\n" + Glib::Markup::escape_text(reason);
            }
            desktop()->messageStack()->flash(Inkscape::WARNING_MESSAGE, message.c_str());
        }
    };
    report_exclusions();
    if (eligible.empty()) return false;
    auto fence = DocumentUndo::detachPendingChanges(doc);
    if (!fence) {
        if (desktop()) desktop()->messageStack()->flash(Inkscape::ERROR_MESSAGE,
            _("Stroke to Path cannot acquire a rollback fence."));
        return false;
    }
    bool did = false;
    bool const scale_stroke = prefs->getBool("/options/transform/stroke", true);
    prefs->setBool("/options/transform/stroke", true);
    try {
        for (auto const &id : eligible) {
            auto item = cast<SPItem>(doc->getObjectById(id));
            if (!item) { conversion.failed = true; break; }
            did = item_to_paths_unlink(item, conversion) || did;
            if (conversion.failed) break;
        }
        for (auto const &id : eligible) {
            if (conversion.failed) break;
            auto item = cast<SPItem>(doc->getObjectById(id));
            if (!item) { conversion.failed = true; break; }
            if (item_to_paths_apply(item, legacy, conversion)) did = true;
        }
    } catch (...) {
        conversion.failed = true;
    }
    prefs->setBool("/options/transform/stroke", scale_stroke);
    if (conversion.failed || !did) {
        DocumentUndo::rollbackToDetachedChanges(doc, *fence);
    } else {
        DocumentUndo::reattachPendingChanges(doc, *fence);
    }
    // Replacements and rollback release objects. Resolve every original ID,
    // preserving the caller's ordering (including covered descendants).
    std::vector<SPItem *> restored;
    for (auto const &id : original_ids) {
        if (auto item = cast<SPItem>(doc->getObjectById(id))) restored.push_back(item);
    }
    setList(restored);
    if (conversion.failed) {
        if (desktop()) desktop()->messageStack()->flash(Inkscape::ERROR_MESSAGE,
            _("Stroke to Path failed; the selection was restored."));
        return false;
    }
    if (desktop() && !did && conversion.exclusions.empty()) {
        desktop()->messageStack()->flash(Inkscape::ERROR_MESSAGE, _("<b>No stroked paths</b> in the selection."));
    }
    if (did && !skip_undo) {
        DocumentUndo::done(doc, RC_("Undo", "Convert stroke to path"), "");
    }
    return did;
}

bool
ObjectSet::simplifyPaths(bool skip_undo)
{
    if (desktop() && isEmpty()) {
        desktop()->messageStack()->flash(Inkscape::WARNING_MESSAGE, _("Select <b>path(s)</b> to simplify."));
        return false;
    }

    Inkscape::Preferences *prefs = Inkscape::Preferences::get();
    double threshold = prefs->getDouble("/options/simplifythreshold/value", 0.003);
    bool justCoalesce = prefs->getBool(  "/options/simplifyjustcoalesce/value", false);

    // Keep track of accelerated simplify
    static gint64 previous_time = 0;
    static gdouble multiply = 1.0;

    // Get the current time
    gint64 current_time = g_get_monotonic_time();

    // Was the previous call to this function recent? (<0.5 sec)
    if (previous_time > 0 && current_time - previous_time < 500000) {

        // add to the threshold 1/2 of its original value
        multiply  += 0.5;
        threshold *= multiply;

    } else {
        // reset to the default
        multiply = 1;
    }

    // Remember time for next call
    previous_time = current_time;

    // set "busy" cursor
    if (desktop()) {
        desktop()->setWaitingCursor();
    }

    Geom::OptRect selectionBbox = visualBounds();
    if (!selectionBbox) {
        std::cerr << "ObjectSet::: selection has no visual bounding box!" << std::endl;
        return false;
    }
    double size = L2(selectionBbox->dimensions());

    int pathsSimplified = 0;
    for (auto item : items_vector()) {
        pathsSimplified += path_simplify(item, threshold, justCoalesce, size);
    }

    if (pathsSimplified > 0 && !skip_undo) {
        DocumentUndo::done(document(), RC_("Undo", "Simplify"), INKSCAPE_ICON("path-simplify"));
    }

    if (desktop()) {
        desktop()->clearWaitingCursor();
        if (pathsSimplified > 0) {
            desktop()->messageStack()->flashF(Inkscape::NORMAL_MESSAGE, _("<b>%d</b> paths simplified."), pathsSimplified);
        } else {
            desktop()->messageStack()->flash(Inkscape::ERROR_MESSAGE, _("<b>No paths</b> to simplify in the selection."));
        }
    }

    return (pathsSimplified > 0);
}

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
