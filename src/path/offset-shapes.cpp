// SPDX-License-Identifier: GPL-2.0-or-later

#include "offset-shapes.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <optional>
#include <unordered_set>
#include <2geom/path-sink.h>

#include <glibmm/ustring.h>

#include "document-undo.h"
#include "document.h"
#include "gc.h"
#include "helper/geom-curves.h"
#include "livarot/Path.h"
#include "object/sp-flowtext.h"
#include "object/sp-image.h"
#include "object/sp-item.h"
#include "object/sp-item-group.h"
#include "object/sp-text.h"
#include "object/sp-use.h"
#include "path/path-boolop.h"
#include "path/path-util.h"
#include "style.h"
#include "svg/svg.h"
#include "ui/icon-names.h"
#include "xml/document.h"
#include "xml/node.h"
#include "xml/repr.h"

namespace Inkscape::OffsetShapes {
namespace {

constexpr std::uint64_t FNV_OFFSET = 14695981039346656037ULL;
constexpr std::uint64_t FNV_PRIME = 1099511628211ULL;

void hash_word(std::uint64_t &hash, std::uint64_t word)
{
    for (unsigned byte = 0; byte < 8; ++byte) {
        hash ^= (word >> (byte * 8)) & 0xffU;
        hash *= FNV_PRIME;
    }
}

std::uint64_t fingerprint(Geom::PathVector const &pathvector)
{
    auto hash = FNV_OFFSET;
    hash_word(hash, pathvector.size());
    for (auto const &path : pathvector) {
        hash_word(hash, path.size());
        hash_word(hash, path.closed());
        for (auto const &curve : path) {
            for (double time : {0.0, 0.25, 0.5, 0.75, 1.0}) {
                auto const point = curve.pointAt(time);
                hash_word(hash, std::bit_cast<std::uint64_t>(point.x()));
                hash_word(hash, std::bit_cast<std::uint64_t>(point.y()));
            }
        }
    }
    return hash;
}

Geom::Affine parent_to_document(SPItem *source)
{
    if (source && source->parent) {
        if (auto parent = cast<SPItem>(source->parent)) {
            return parent->i2doc_affine();
        }
    }
    return Geom::identity();
}

std::optional<Geom::PathVector> item_geometry(SPItem *item, Geom::Affine const &to_document)
{
    if (!item || item->isHidden() || is<SPImage>(item) || item->getMaskObject() ||
        (item->style && item->style->getFilter())) {
        return {};
    }
    Geom::PathVector output;
    if (auto group = cast<SPGroup>(item)) {
        for (auto &child_object : group->children) {
            if (auto child = cast<SPItem>(&child_object)) {
                if (auto geometry = item_geometry(child, child->transform * to_document)) {
                    output.insert(output.end(), geometry->begin(), geometry->end());
                }
            }
        }
    } else if (auto use = cast<SPUse>(item)) {
        if (use->child) {
            auto const child_transform = use->child->transform * Geom::Translate(use->x.computed, use->y.computed) *
                                         to_document;
            if (auto geometry = item_geometry(use->child, child_transform)) {
                output = std::move(*geometry);
            }
        }
    } else if (auto geometry = curve_for_item(item); geometry && !geometry->empty()) {
        output = *geometry * to_document;
    }

    if (output.empty()) {
        return {};
    }
    if (auto clip = item->getClipPathVector()) {
        output = sp_pathvector_boolop(output, *clip * to_document, bool_op_inters, fill_nonZero, fill_nonZero);
    }
    return output.empty() ? std::nullopt : std::optional{std::move(output)};
}

bool all_closed(Geom::PathVector const &pathvector)
{
    return !pathvector.empty() &&
           std::all_of(pathvector.begin(), pathvector.end(), [](Geom::Path const &path) { return path.closed(); });
}

double signed_area(Geom::Path const &path)
{
    double area = 0.0;
    Geom::Point centroid;
    Geom::centroid(path.toPwSb(), centroid, area);
    return area;
}

Geom::PathVector remove_holes(Geom::PathVector pathvector)
{
    if (pathvector.size() < 2) {
        return pathvector;
    }
    auto outer = std::max_element(pathvector.begin(), pathvector.end(), [](auto const &a, auto const &b) {
        return std::abs(signed_area(a)) < std::abs(signed_area(b));
    });
    auto const outer_sign = std::signbit(signed_area(*outer));
    Geom::PathVector result;
    for (auto const &path : pathvector) {
        auto const area = signed_area(path);
        if (std::abs(area) >= 1e-9 && std::signbit(area) == outer_sign) result.push_back(path);
    }
    return result;
}

Geom::PathVector closed_only(Geom::PathVector const &geometry, std::size_t &skipped)
{
    Geom::PathVector result;
    for (auto const &path : geometry) {
        if (path.closed() && !path.empty()) {
            result.push_back(path);
        } else {
            ++skipped;
        }
    }
    return result;
}

constexpr double min_simplify_tolerance_px = 0.001;
constexpr double max_simplify_steps = 5e6;

// Coordinates beyond this magnitude are never real drawings and overflow livarot's
// fixed-point/float arithmetic (hang), even though they are finite doubles.
constexpr double max_source_coordinate = 1e9;

bool finite_geometry(Geom::PathVector const &pathvector)
{
    for (auto const &path : pathvector) {
        for (auto const &curve : path) {
            for (double time : {0.0, 0.25, 0.5, 0.75, 1.0}) {
                auto const point = curve.pointAt(time);
                if (!std::isfinite(point.x()) || !std::isfinite(point.y()) ||
                    std::abs(point.x()) > max_source_coordinate || std::abs(point.y()) > max_source_coordinate) {
                    return false;
                }
            }
        }
    }
    return true;
}

bool topology_compatible(Geom::PathVector const &original, Geom::PathVector const &candidate)
{
    if (candidate.size() != original.size() || !all_closed(candidate)) {
        return false;
    }
    for (std::size_t index = 0; index < original.size(); ++index) {
        auto const original_area = signed_area(original[index]);
        auto const candidate_area = signed_area(candidate[index]);
        if (!std::isfinite(original_area) || !std::isfinite(candidate_area) ||
            std::abs(original_area) < 1e-12 || std::abs(candidate_area) < 1e-12 ||
            std::signbit(original_area) != std::signbit(candidate_area)) {
            return false;
        }
    }
    return true;
}

std::optional<double> directed_sample_distance(Geom::PathVector const &from,
                                               Geom::PathVector const &to,
                                               double tolerance)
{
    // Simplification is optional. Refuse it rather than spending unbounded
    // time proving fidelity for pathological paths.
    constexpr std::size_t max_samples = 100'000;
    std::size_t sample_count = 0;
    double maximum = 0.0;
    for (auto const &path : from) {
        for (auto const &curve : path) {
            auto samples = std::size_t{1};
            if (!is_straight_curve(curve)) {
                auto const curve_bounds = curve.boundsFast();
                auto const diagonal = Geom::distance(curve_bounds.min(), curve_bounds.max());
                samples = static_cast<std::size_t>(std::ceil(diagonal / std::max(tolerance * 0.25, 1e-9)));
                samples = std::clamp(samples, std::size_t{8}, std::size_t{256});
            }
            auto const required_samples = samples + 1;
            if (required_samples > max_samples - sample_count) {
                return std::nullopt;
            }
            sample_count += required_samples;
            for (std::size_t step = 0; step <= samples; ++step) {
                auto const point = curve.pointAt(static_cast<double>(step) / samples);
                Geom::Coord distance = std::numeric_limits<Geom::Coord>::infinity();
                if (!to.nearestTime(point, &distance) || !std::isfinite(distance)) {
                    return std::nullopt;
                }
                maximum = std::max(maximum, static_cast<double>(distance));
                if (maximum > tolerance * (1.0 + 1e-6)) {
                    return maximum;
                }
            }
        }
    }
    return maximum;
}

bool fidelity_within_tolerance(Geom::PathVector const &original,
                               Geom::PathVector const &candidate,
                               double tolerance)
{
    auto const forward = directed_sample_distance(original, candidate, tolerance);
    if (!forward || *forward > tolerance * (1.0 + 1e-6)) {
        return false;
    }
    auto const reverse = directed_sample_distance(candidate, original, tolerance);
    return reverse && *reverse <= tolerance * (1.0 + 1e-6);
}

double total_length(Geom::PathVector const &geometry)
{
    double total = 0.0;
    for (auto const &path : geometry) {
        for (auto const &curve : path) {
            total += curve.length(0.1);
        }
    }
    return total;
}

Geom::PathVector simplify_result(Geom::PathVector const &geometry, double tolerance, bool &applied)
{
    applied = false;
    if (!std::isfinite(tolerance) || tolerance <= 0.0) {
        return geometry;
    }
    // ConvertEvenLines subdivides by length / tolerance before any sample cap
    // applies; a tiny tolerance would explode memory and time. Clamp it and
    // refuse (keep the unsimplified outline) when the budget is still exceeded.
    tolerance = std::max(tolerance, min_simplify_tolerance_px);
    auto const length = total_length(geometry);
    if (!std::isfinite(length) || length / tolerance > max_simplify_steps) {
        return geometry;
    }
    auto path = Path_for_pathvector(geometry);
    auto const before = path->descr_cmd.size();
    path->ConvertEvenLines(tolerance);
    path->Simplify(tolerance);
    auto candidate = path->MakePathVector();
    if (path->descr_cmd.size() > before || !finite_geometry(candidate) ||
        !topology_compatible(geometry, candidate) ||
        !fidelity_within_tolerance(geometry, candidate, tolerance)) {
        return geometry;
    }
    applied = true;
    return candidate;
}

LineJoinType join_for(Corner corner)
{
    switch (corner) {
        case Corner::Round: return JOIN_ROUND;
        case Corner::Bevel: return JOIN_BEVEL;
        case Corner::Miter: return JOIN_MITER;
    }
    return JOIN_MITER;
}

void append_offset(Build &build, Geom::PathVector const &geometry, FillRule fill_rule, std::size_t source_index,
                   Direction direction, Options const &options)
{
    auto const signed_distance = direction == Direction::Outward ? options.distance_px : -options.distance_px;
    auto offset = do_offset(geometry, signed_distance, -1.0, options.miter_limit, fill_rule,
                            join_for(options.corner));
    if (options.simplify_results) {
        bool applied = false;
        offset = simplify_result(offset, options.simplify_tolerance_px, applied);
        if (!applied && !offset.empty()) {
            ++build.simplify_skipped;
        }
    }
    if (!offset.empty()) {
        build.results.push_back({source_index, direction, std::move(offset)});
    }
}

} // namespace

Preparation prepare(std::span<SPItem *const> items)
{
    Preparation preparation;
    preparation.selected_count = items.size();
    std::unordered_set<SPItem *> seen;
    for (auto *item : items) {
        if (!item || !seen.emplace(item).second || item->isHidden() || item->isLocked()) {
            ++preparation.skipped_count;
            continue;
        }
        auto extracted = item_geometry(item, item->i2doc_affine());
        if (!extracted) {
            ++preparation.skipped_count;
            continue;
        }
        // An Inf/NaN coordinate makes livarot loop forever; skip such sources.
        if (!finite_geometry(*extracted)) {
            ++preparation.skipped_count;
            continue;
        }
        auto geometry = closed_only(*extracted, preparation.open_subpaths_skipped);
        if (geometry.empty()) {
            ++preparation.skipped_count;
            continue;
        }
        auto const fill_rule = item->style && item->style->fill_rule.computed == SP_WIND_RULE_EVENODD
                                   ? fill_oddEven
                                   : fill_nonZero;
        preparation.sources.push_back({SPWeakPtr<SPItem>(item), std::move(geometry), fill_rule, 0});
        preparation.sources.back().geometry_fingerprint = fingerprint(preparation.sources.back().geometry_document);
    }
    if (preparation.sources.empty()) {
        preparation.error = "selection contains no supported closed vector regions to offset";
    }
    return preparation;
}

Build build(std::span<Source const> sources, Options const &options)
{
    Build result;
    if (sources.empty()) {
        result.error = "offset source list is empty";
        return result;
    }
    if (!std::isfinite(options.distance_px) || options.distance_px <= 0.0) {
        result.error = "offset distance must be finite and greater than zero";
        return result;
    }
    if (!std::isfinite(options.miter_limit) || options.miter_limit < 1.0) {
        result.error = "miter limit must be finite and at least one";
        return result;
    }
    if (options.simplify_results &&
        (!std::isfinite(options.simplify_tolerance_px) || options.simplify_tolerance_px <= 0.0)) {
        result.error = "simplification tolerance must be finite and greater than zero";
        return result;
    }

    if (options.outer_shapes_only) {
        auto combined = flattened(sources.front().geometry_document, sources.front().fill_rule);
        for (std::size_t index = 1; index < sources.size(); ++index) {
            auto next = flattened(sources[index].geometry_document, sources[index].fill_rule);
            combined = sp_pathvector_boolop(combined, next, bool_op_union, fill_nonZero, fill_nonZero);
        }
        combined = remove_holes(flattened(combined, fill_nonZero));
        auto topmost_index = std::size_t{0};
        for (std::size_t index = 1; index < sources.size(); ++index) {
            if (sp_object_compare_position_bool(sources[topmost_index].item.get(), sources[index].item.get())) {
                topmost_index = index;
            }
        }
        if (options.direction != Direction::Inward) {
            append_offset(result, combined, fill_nonZero, topmost_index, Direction::Outward, options);
        }
        if (options.direction != Direction::Outward) {
            append_offset(result, combined, fill_nonZero, topmost_index, Direction::Inward, options);
        }
    } else {
        for (std::size_t index = 0; index < sources.size(); ++index) {
            auto const &source = sources[index];
            if (options.direction != Direction::Inward) {
                append_offset(result, source.geometry_document, source.fill_rule, index, Direction::Outward, options);
            }
            if (options.direction != Direction::Outward) {
                append_offset(result, source.geometry_document, source.fill_rule, index, Direction::Inward, options);
            }
        }
    }
    if (result.results.empty()) {
        result.error = "the requested offset collapsed or produced no valid geometry";
    }
    return result;
}

bool geometry_still_matches(std::span<Source const> sources)
{
    for (auto const &source : sources) {
        auto *item = source.item.get();
        Geom::PathVector geometry;
        auto current = item ? item_geometry(item, item->i2doc_affine()) : std::nullopt;
        if (!item || item->isHidden() || item->isLocked() || !current) {
            return false;
        }
        std::size_t ignored = 0;
        geometry = closed_only(*current, ignored);
        if (fingerprint(geometry) != source.geometry_fingerprint) return false;
    }
    return true;
}

CommitResult commit(SPDocument *document, Preparation const &preparation, Build const &build,
                    Options const &options, CommitProtocol protocol,
                    std::function<void()> const &before_mutation)
{
    struct PendingPath
    {
        Inkscape::XML::Node *parent = nullptr;
        Inkscape::XML::Node *after = nullptr;
        std::string d;
        Glib::ustring style;
    };

    std::vector<PendingPath> pending;
    pending.reserve(build.results.size());
    if (options.outer_shapes_only) {
        Inkscape::XML::Node *combined_parent = nullptr;
        for (auto const &prepared : preparation.sources) {
            auto *source = prepared.item.get();
            auto *parent = source && source->getRepr() ? source->getRepr()->parent() : nullptr;
            if (!source || !parent || std::abs(parent_to_document(source).descrim()) < 1e-12) {
                CommitResult r;
                r.error = "an offset source no longer has a valid parent transform";
                return r;
            }
            if (!combined_parent) combined_parent = parent;
            if (combined_parent != parent) {
                CommitResult r;
                r.error = "Outer only requires eligible sources to share one editable parent";
                return r;
            }
        }
    }
    for (auto const &result : build.results) {
        if (result.source_index >= preparation.sources.size()) {
            CommitResult r;
            r.error = "offset result references an invalid source";
            return r;
        }
        auto *source = preparation.sources[result.source_index].item.get();
        auto *parent = source && source->getRepr() ? source->getRepr()->parent() : nullptr;
        if (!source || !parent || std::abs(parent_to_document(source).descrim()) < 1e-12) {
            CommitResult r;
            r.error = "an offset source no longer has a valid parent transform";
            return r;
        }
        auto d = sp_svg_write_path(result.geometry_document * parent_to_document(source).inverse());
        if (d.empty()) {
            CommitResult r;
            r.error = "an offset result could not be serialized";
            return r;
        }
        auto style = source->style
                         ? source->style->writeIfDiff(source->parent ? source->parent->style : nullptr)
                         : Glib::ustring{"fill:#000000;stroke:none"};
        pending.push_back({parent, source->getRepr(), std::move(d), std::move(style)});
    }

    Util::Internal::ContextString const description{"Offset shapes"};

    // The caller owns the Undo protocol; commit() never guesses it. Interaction keeps the live
    // tool's rollback point for the whole mutation and refuses (no fallback, ever) when another
    // document interaction is active. CommandLine must not take an interaction; it refuses before
    // any change only while the document is closing or another interaction is active, then records
    // the standard single Undo step (pending logged changes are folded into it, as for any action).
    std::optional<DocumentUndo::RollbackableInteraction> interaction;
    if (protocol == CommitProtocol::Interaction) {
        interaction = DocumentUndo::beginRollbackableInteraction(document);
        if (!interaction) {
            CommitResult r;
            r.error = "another document interaction is active";
            return r;
        }
    } else {
        if (DocumentUndo::interactionCloseRequested(document)) {
            CommitResult r;
            r.error = "the document is closing";
            return r;
        }
        // The exact atomic-fence query, DocumentUndo::atomicHistoryConflict(), is private to
        // DocumentUndo (src/document-undo.h) and not ours to expose. Every atomic fence is taken
        // through beginAtomicInteraction() -> beginRollbackableInteraction(), which marks the
        // document as having an active interaction, so the public interactionActive() is the safe
        // superset: a live rollback point owns the history and a single CLI Undo step could not be
        // recorded honestly (done() would be merged into it or silently ignored).
        if (protocol == CommitProtocol::CommandLine && DocumentUndo::interactionActive(document)) {
            CommitResult r;
            r.error = "the document history is protected";
            return r;
        }
    }

    if (protocol == CommitProtocol::CallerOwnedAtomic && !DocumentUndo::interactionActive(document)) {
        CommitResult r;
        r.error = "the caller must own an atomic document interaction";
        return r;
    }

    // Arm the caller's guard exactly once, right before the first document mutation. A refusal
    // above leaves it unarmed, so a tool still reacts to a later selection change.
    if (before_mutation) {
        before_mutation();
    }

    CommitResult result;
    // From here on the first mutation may already have happened, so a caller can tell a pre-mutation
    // refusal (nothing touched, guards still armed) from a post-mutation rollback.
    result.mutation_started = true;
    result.created.reserve(pending.size());
    auto *xml_document = document->getReprDoc();
    for (auto const &path : pending) {
        auto *repr = xml_document->createElement("svg:path");
        repr->setAttribute("d", path.d);
        repr->setAttribute("style", path.style);
        path.parent->addChild(repr, path.after);
        auto *item = cast<SPItem>(document->getObjectByRepr(repr));
        Inkscape::GC::release(repr);
        if (!item) {
            if (interaction) {
                interaction->rollback();
            } else if (protocol == CommitProtocol::CommandLine) {
                DocumentUndo::cancel(document);
            }
            CommitResult r;
            r.error = "a generated offset path could not be attached to the document";
            r.mutation_started = true; // a caller must know its armed guards are already down
            return r;
        }
        result.created.push_back(item);
    }

    if (options.delete_originals) {
        std::unordered_set<SPItem *> deleted;
        std::unordered_set<std::size_t> produced;
        for (auto const &entry : build.results) produced.emplace(entry.source_index);
        // Delete with the delete signal (as Edit > Delete does), so unselected clones of a deleted
        // original follow the user's orphaned-clone preference (unlinked by default) instead of
        // dangling. Unlike Edit > Delete, selected clones go first and the deepest clone of a chain
        // first, so no selected object is unlinked into a surviving copy by deleting its source.
        std::vector<SPItem *> order;
        for (std::size_t index = 0; index < preparation.sources.size(); ++index) {
            if (!options.outer_shapes_only && !produced.contains(index)) continue;
            if (auto *item = preparation.sources[index].item.get(); item && deleted.emplace(item).second) {
                order.push_back(item);
            }
        }
        auto const depth = [](SPItem *item) { auto *use = cast<SPUse>(item); return use ? use->cloneDepth() : 0; };
        std::stable_sort(order.begin(), order.end(),
                         [&depth](SPItem *a, SPItem *b) { return depth(a) > depth(b); });
        // Weak pointers: deleting one source can still unlink or free another selected object only
        // through a reference the ordering above has already removed; skip any that did go away.
        std::vector<SPWeakPtr<SPItem>> pending_delete(order.begin(), order.end());
        for (auto &weak : pending_delete) {
            auto *item = weak.get();
            if (!item) continue;
            auto const id = item->getId();
            result.deleted_ids.emplace_back(id ? id : "");
            result.deleted.push_back(item);
            item->deleteObject();
        }
    }

    if (interaction) {
        interaction->commit(description, INKSCAPE_ICON("path-offset-dynamic"));
    } else if (protocol == CommitProtocol::CommandLine) {
        DocumentUndo::done(document, description, INKSCAPE_ICON("path-offset-dynamic"));
    }
    document->ensureUpToDate();
    return result;
}

} // namespace Inkscape::OffsetShapes
