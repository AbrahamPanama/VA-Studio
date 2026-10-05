// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_OBJECT_CLIP_DOCUMENT_SERVICE_H
#define INKSCAPE_OBJECT_CLIP_DOCUMENT_SERVICE_H

#include <string>
#include <vector>

class SPDocument;
class SPItem;

namespace Inkscape::ClipDocumentService {

struct SetOptions { bool inverse = false; bool keep_cutter = true; };
struct ReleaseOptions { bool keep_cutter = true; bool ungroup_helpers = false; };

enum class Status { Prepared, Applied, Unchanged, Refused };
enum class Reason {
    None, InvalidDocument, InvalidItem, MissingSource, OverlappingRoles, CloneWithSource,
    ProtectedItem, ReferencedCutter, SingularTransform, UnsupportedInverseTarget,
    UnsupportedInverseCutter, ExistingPowerClip, NotClipped, UnsupportedHelperUngroup
};
struct Exclusion { std::string id; Reason reason; };
struct Result {
    Status status = Status::Refused;
    Reason reason = Reason::None;
    std::vector<std::string> affected_ids;
    std::vector<std::string> restored_cutter_ids;
    std::vector<Exclusion> excluded;
    std::vector<std::string> covered_ids;
};

// Read-only admission. No retained pointer ticket: apply repeats preparation.
Result prepareSetClip(SPDocument *document, SPItem *target, SPItem *cutter, SetOptions options = {});
Result prepareReleaseClip(SPDocument *document, std::vector<SPItem *> const &roots, ReleaseOptions options = {});

// Explicit role pair; clips the target root as a whole, without regrouping.
// keep_cutter retains the original artwork; the clip always owns a duplicate.
Result setClip(SPDocument *document, SPItem *target, SPItem *cutter, SetOptions options = {});
// ungroup_helpers=true is refused: native ungrouping changes GUI preferences.
// Explicit clipped roots, no recursive leaf traversal; compatible roots apply
// once, incompatible roots are preserved and reported. keep_cutter restores
// copies from the clip (it does not delete any already retained artwork).
Result releaseClip(SPDocument *document, std::vector<SPItem *> const &roots, ReleaseOptions options = {});

// These helpers never settle history. The caller owns commit/rollback and one
// Undo transaction for the entire request. Prepare/refusal creates no history.
} // namespace Inkscape::ClipDocumentService
#endif
