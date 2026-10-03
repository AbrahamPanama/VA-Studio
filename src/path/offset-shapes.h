// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_PATH_OFFSET_SHAPES_H
#define INKSCAPE_PATH_OFFSET_SHAPES_H

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include <2geom/pathvector.h>

#include "helper/geom-pathstroke.h"
#include "object/weakptr.h"

class SPDocument;
class SPItem;

namespace Inkscape::OffsetShapes {

enum class Direction { Outward, Inward, Both };
enum class Corner { Round, Bevel, Miter };

/**
 * Who owns the Undo protocol for a commit. The caller decides, and commit()
 * never guesses:
 *
 * - Interaction: the live Offset Shapes tool. commit() obtains a rollbackable
 *   interaction token for the whole mutation and refuses (nothing changed) when
 *   another document interaction is active. That refusal is never overridden by
 *   a fallback.
 * - CommandLine: a VACards command-line action. commit() must not take a
 *   rollbackable interaction; it follows the standard single-step protocol. It
 *   refuses before any change while the document is closing or another document
 *   interaction is active, then mutates and records exactly one Undo step (or
 *   DocumentUndo::cancel on a failure after the mutation started). Like every
 *   Inkscape action, pending logged changes are folded into that Undo step.
 */
enum class CommitProtocol { Interaction, CommandLine };

struct Options
{
    double distance_px = 1.0;
    Direction direction = Direction::Outward;
    Corner corner = Corner::Miter;
    double miter_limit = 4.0;
    bool outer_shapes_only = false;
    bool select_results = true;
    bool delete_originals = false;
    bool simplify_results = false;
    double simplify_tolerance_px = 0.05;
};

struct Source
{
    SPWeakPtr<SPItem> item;
    Geom::PathVector geometry_document;
    FillRule fill_rule = fill_nonZero;
    std::uint64_t geometry_fingerprint = 0;
};

struct Result
{
    std::size_t source_index = 0;
    Direction generated_direction = Direction::Outward;
    Geom::PathVector geometry_document;
};

struct Preparation
{
    std::vector<Source> sources;
    std::size_t selected_count = 0;
    std::size_t skipped_count = 0;
    std::size_t open_subpaths_skipped = 0;
    std::string error;

    explicit operator bool() const noexcept { return error.empty() && !sources.empty(); }
};

struct Build
{
    std::vector<Result> results;
    /// Outlines for which simplification was requested but could not be applied (over the work budget, or the
    /// simplified shape failed the topology/tolerance checks); they were kept as generated.
    std::size_t simplify_skipped = 0;
    std::string error;

    explicit operator bool() const noexcept { return error.empty() && !results.empty(); }
};

struct CommitResult
{
    std::vector<SPItem *> created;   ///< new paths, in result order
    std::vector<SPItem *> deleted;   ///< originals deleted (delete_originals), deepest clones first, then source
                                     ///< order; pointers are dangling
                                     ///< after return: use only for identity comparison (e.g. against a saved id list)
    std::vector<std::string> deleted_ids; ///< ids of `deleted`, captured before deletion
    std::string error;               ///< non-empty: the offset was not applied (refused before changes, or its
                                     ///< changes rolled back; a CommandLine cancel also reverts pending unrecorded
                                     ///< changes, as DocumentUndo::cancel does for every caller)
    bool mutation_started = false;   ///< true once before_mutation ran (the first document mutation may have happened)
    explicit operator bool() const noexcept { return error.empty(); }
};

[[nodiscard]] Preparation prepare(std::span<SPItem *const> items);
[[nodiscard]] Build build(std::span<Source const> sources, Options const &options);
[[nodiscard]] bool geometry_still_matches(std::span<Source const> sources);
/// Commit a build as one Undo step (label "Offset shapes"): the same checks, parent/order placement, styles,
/// outer-only parent rule, deletion of originals and rollback as the Offset Shapes tool. Selection is not touched.
/// @a protocol selects the Undo protocol and must be given by the caller. @a before_mutation, when set, runs
/// exactly once immediately before the first document mutation (for Interaction: right after the token was
/// obtained, which is where the tool arms its own guards); it is never called on a refusal.
[[nodiscard]] CommitResult commit(SPDocument *document, Preparation const &preparation, Build const &build,
                                  Options const &options, CommitProtocol protocol,
                                  std::function<void()> const &before_mutation = {});

} // namespace Inkscape::OffsetShapes

#endif // INKSCAPE_PATH_OFFSET_SHAPES_H
