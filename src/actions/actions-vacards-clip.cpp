// SPDX-License-Identifier: GPL-2.0-or-later
#include "actions-vacards-clip.h"

#include "vacards-cli-production.h"
#include <boost/json.hpp>
namespace Inkscape::VACardsCli {
namespace {
// | `clip.set` | `target-id,cutter-id`; `inverse=false`, `keep-cutter=true` | clip relation/resource IDs, target/cutter retention, bounds | T, incompatible-operands, unsupported-clip, clip-failed | E / one / computed / P; explicit native clipping pair, no masks |
TypeDescriptor const m3_0{boost::json::parse(R"m3({
  "type": "object",
  "properties": {
    "target-id": {
      "type": "string",
      "minLength": 1,
      "maxLength": 128
    },
    "cutter-id": {
      "type": "string",
      "minLength": 1,
      "maxLength": 128
    },
    "inverse": {
      "type": "boolean",
      "default": false
    },
    "keep-cutter": {
      "type": "boolean",
      "default": true
    }
  },
  "required": [
    "target-id",
    "cutter-id"
  ],
  "additionalProperties": false,
  "x-m3-contract": {
    "guard_domain": "document",
    "guard_required": true,
    "needs_document": true,
    "selection_mode": "explicit-role-pair",
    "target_cardinality": {
      "min": 2,
      "max": 2
    },
    "normalization": "explicit distinct roles; reject ancestor overlap",
    "partial_policy": "reject-any-incompatible",
    "role_order": "target then cutter",
    "normalization_order": [
      "validate raw schema and exclusive route",
      "select route",
      "fill defaults recursively only in selected branch; pivot suppresses anchor, size suppresses dpi, token suppresses recipe/analyze/solve",
      "parse exact integers",
      "convert all lengths including nested recipes to CSS px and enforce bounds",
      "resolve and normalize targets",
      "canonicalize normalized params for token binding"
    ],
    "native_service": "`ObjectSet::setMask`, `src/selection-chemistry.cpp:3927`; inverse wrapper `src/actions/actions-object.cpp:283`",
    "warnings": [],
    "error_rows": [
      {
        "layer": "transport/schema",
        "native_branch": "parse_request: request-too-large",
        "code": "request-too-large",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "parse_request: request-too-large",
        "native_evidence": "`ObjectSet::setMask`, `src/selection-chemistry.cpp:3927`; inverse wrapper `src/actions/actions-object.cpp:283`",
        "oracle": "P9:clip.set:transport/schema:request-too-large:parse_request: request-too-large",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "transport/schema",
        "native_branch": "parse_request: repeated-key",
        "code": "repeated-key",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "parse_request: repeated-key",
        "native_evidence": "`ObjectSet::setMask`, `src/selection-chemistry.cpp:3927`; inverse wrapper `src/actions/actions-object.cpp:283`",
        "oracle": "P9:clip.set:transport/schema:repeated-key:parse_request: repeated-key",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "transport/schema",
        "native_branch": "parse_request: invalid-utf8",
        "code": "invalid-utf8",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "parse_request: invalid-utf8",
        "native_evidence": "`ObjectSet::setMask`, `src/selection-chemistry.cpp:3927`; inverse wrapper `src/actions/actions-object.cpp:283`",
        "oracle": "P9:clip.set:transport/schema:invalid-utf8:parse_request: invalid-utf8",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "transport/schema",
        "native_branch": "parse_request: malformed-json",
        "code": "malformed-json",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "parse_request: malformed-json",
        "native_evidence": "`ObjectSet::setMask`, `src/selection-chemistry.cpp:3927`; inverse wrapper `src/actions/actions-object.cpp:283`",
        "oracle": "P9:clip.set:transport/schema:malformed-json:parse_request: malformed-json",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "transport/schema",
        "native_branch": "raw schema/type/range/route/uniqueItems violation",
        "code": "invalid-argument",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "raw schema/type/range/route/uniqueItems violation",
        "native_evidence": "`ObjectSet::setMask`, `src/selection-chemistry.cpp:3927`; inverse wrapper `src/actions/actions-object.cpp:283`",
        "oracle": "P9:clip.set:transport/schema:invalid-argument:raw schema/type/range/route/uniqueItems violation",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "missing current document",
        "code": "no-document",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "missing current document",
        "native_evidence": "`ObjectSet::setMask`, `src/selection-chemistry.cpp:3927`; inverse wrapper `src/actions/actions-object.cpp:283`",
        "oracle": "P9:clip.set:identity/admission:no-document:missing current document",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "document identity mismatch",
        "code": "stale-document",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "document identity mismatch",
        "native_evidence": "`ObjectSet::setMask`, `src/selection-chemistry.cpp:3927`; inverse wrapper `src/actions/actions-object.cpp:283`",
        "oracle": "P9:clip.set:identity/admission:stale-document:document identity mismatch",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "supplied guard differs from document revision",
        "code": "stale-revision",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "supplied guard differs from document revision",
        "native_evidence": "`ObjectSet::setMask`, `src/selection-chemistry.cpp:3927`; inverse wrapper `src/actions/actions-object.cpp:283`",
        "oracle": "P9:clip.set:identity/admission:stale-revision:supplied guard differs from document revision",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "cancellation observed before commit",
        "code": "cancelled",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "cancellation observed before commit",
        "native_evidence": "`ObjectSet::setMask`, `src/selection-chemistry.cpp:3927`; inverse wrapper `src/actions/actions-object.cpp:283`",
        "oracle": "P9:clip.set:identity/admission:cancelled:cancellation observed before commit",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "explicit ID cannot resolve",
        "code": "unknown-id",
        "retryable": false,
        "mutation_state": "none",
        "route": "explicit-target route",
        "detail.reason": "explicit ID cannot resolve",
        "native_evidence": "`ObjectSet::setMask`, `src/selection-chemistry.cpp:3927`; inverse wrapper `src/actions/actions-object.cpp:283`",
        "oracle": "P9:clip.set:identity/admission:unknown-id:explicit ID cannot resolve",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "EditTransaction inactive",
        "code": "transaction-unavailable",
        "retryable": true,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "EditTransaction inactive",
        "native_evidence": "`ObjectSet::setMask`, `src/selection-chemistry.cpp:3927`; inverse wrapper `src/actions/actions-object.cpp:283`",
        "oracle": "P9:clip.set:identity/admission:transaction-unavailable:EditTransaction inactive",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "read-only session rejects requested document mutation",
        "code": "document-read-only",
        "retryable": false,
        "mutation_state": "none",
        "route": "document-edit or history mutation only",
        "detail.reason": "read-only session rejects requested document mutation",
        "native_evidence": "`ObjectSet::setMask`, `src/selection-chemistry.cpp:3927`; inverse wrapper `src/actions/actions-object.cpp:283`",
        "oracle": "P9:clip.set:identity/admission:document-read-only:read-only session rejects requested document mutation",
        "evidence_status": "required-unimplemented"
      },
{
        "layer": "command-service",
        "native_branch": "ClipDocumentService::Reason::InvalidItem",
        "code": "unknown-id",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "ClipDocumentService::Reason::InvalidItem",
        "native_evidence": "`ObjectSet::setMask`, `src/selection-chemistry.cpp:3927`; inverse wrapper `src/actions/actions-object.cpp:283`",
        "oracle": "P9:clip.set:command-service:unknown-id:ClipDocumentService::Reason::InvalidItem",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "ClipDocumentService::Reason::ProtectedItem",
        "code": "unavailable",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "ClipDocumentService::Reason::ProtectedItem",
        "native_evidence": "`ObjectSet::setMask`, `src/selection-chemistry.cpp:3927`; inverse wrapper `src/actions/actions-object.cpp:283`",
        "oracle": "P9:clip.set:command-service:unavailable:ClipDocumentService::Reason::ProtectedItem",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "ClipDocumentService::Reason::SingularTransform",
        "code": "invalid-transform",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "ClipDocumentService::Reason::SingularTransform",
        "native_evidence": "`ObjectSet::setMask`, `src/selection-chemistry.cpp:3927`; inverse wrapper `src/actions/actions-object.cpp:283`",
        "oracle": "P9:clip.set:command-service:invalid-transform:ClipDocumentService::Reason::SingularTransform",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "ClipDocumentService::Reason::OverlappingRoles",
        "code": "duplicate-id",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "ClipDocumentService::Reason::OverlappingRoles",
        "native_evidence": "`ObjectSet::setMask`, `src/selection-chemistry.cpp:3927`; inverse wrapper `src/actions/actions-object.cpp:283`",
        "oracle": "P9:clip.set:command-service:duplicate-id:ClipDocumentService::Reason::OverlappingRoles",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "ClipDocumentService::Reason::MissingSource",
        "code": "unsupported-target",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "ClipDocumentService::Reason::MissingSource",
        "native_evidence": "src/object/clip-document-service.h; src/selection-chemistry.cpp",
        "oracle": "P9:clip.set:command-service:unsupported-target:ClipDocumentService::Reason::MissingSource",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "ClipDocumentService::Reason::ReferencedCutter",
        "code": "unsupported-target",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "ClipDocumentService::Reason::ReferencedCutter",
        "native_evidence": "src/object/clip-document-service.h; src/selection-chemistry.cpp",
        "oracle": "P9:clip.set:command-service:unsupported-target:ClipDocumentService::Reason::ReferencedCutter",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "ClipDocumentService::Reason::CloneWithSource",
        "code": "unsupported-target",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "ClipDocumentService::Reason::CloneWithSource",
        "native_evidence": "`ObjectSet::setMask`, `src/selection-chemistry.cpp:3927`; inverse wrapper `src/actions/actions-object.cpp:283`",
        "oracle": "P9:clip.set:command-service:unsupported-target:ClipDocumentService::Reason::CloneWithSource",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "ClipDocumentService::Reason::UnsupportedInverseTarget",
        "code": "unsupported-target",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "ClipDocumentService::Reason::UnsupportedInverseTarget",
        "native_evidence": "`ObjectSet::setMask`, `src/selection-chemistry.cpp:3927`; inverse wrapper `src/actions/actions-object.cpp:283`",
        "oracle": "P9:clip.set:command-service:unsupported-target:ClipDocumentService::Reason::UnsupportedInverseTarget",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "ClipDocumentService::Reason::UnsupportedInverseCutter",
        "code": "unsupported-target",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "ClipDocumentService::Reason::UnsupportedInverseCutter",
        "native_evidence": "`ObjectSet::setMask`, `src/selection-chemistry.cpp:3927`; inverse wrapper `src/actions/actions-object.cpp:283`",
        "oracle": "P9:clip.set:command-service:unsupported-target:ClipDocumentService::Reason::UnsupportedInverseCutter",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "ClipDocumentService::Reason::ExistingPowerClip",
        "code": "unsupported-target",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "ClipDocumentService::Reason::ExistingPowerClip",
        "native_evidence": "`ObjectSet::setMask`, `src/selection-chemistry.cpp:3927`; inverse wrapper `src/actions/actions-object.cpp:283`",
        "oracle": "P9:clip.set:command-service:unsupported-target:ClipDocumentService::Reason::ExistingPowerClip",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "unexpected service exception; rollback before return",
        "code": "internal-error",
        "retryable": false,
        "mutation_state": "rolled-back",
        "route": "all",
        "detail.reason": "unexpected service exception; rollback before return",
        "native_evidence": "`ObjectSet::setMask`, `src/selection-chemistry.cpp:3927`; inverse wrapper `src/actions/actions-object.cpp:283`",
        "oracle": "P9:clip.set:command-service:internal-error:unexpected service exception; rollback before return",
        "evidence_status": "required-unimplemented"
      }
    ],
    "success_native_branches": [
      "ClipDocumentService::Status::Prepared",
      "ClipDocumentService::Status::Applied",
      "ClipDocumentService::Status::Unchanged"
    ],
    "variant_applicability": {
      "success": "ok or changed",
      "unchanged": "native no-op only; query/preparation/history empty are not successful unchanged",
      "computed-dry-run": "successful read-only preflight; no retention, publication, Undo or Redo change"
    },
    "success_rows": [
      {
        "native_branch": "ClipDocumentService::Status::Prepared",
        "code": null,
        "retryable": false,
        "mutation_state": "none",
        "data_variant": "computed-dry-run",
        "evidence_route": "P9:clip.set:native:ClipDocumentService::Status::Prepared",
        "evidence_status": "required-unimplemented"
      },
      {
        "native_branch": "ClipDocumentService::Status::Applied",
        "code": null,
        "retryable": false,
        "mutation_state": "committed",
        "data_variant": "success",
        "evidence_route": "P9:clip.set:native:ClipDocumentService::Status::Applied",
        "evidence_status": "required-unimplemented"
      },
      {
        "native_branch": "ClipDocumentService::Status::Unchanged",
        "code": null,
        "retryable": false,
        "mutation_state": "none",
        "data_variant": "unchanged",
        "evidence_route": "P9:clip.set:native:ClipDocumentService::Status::Unchanged",
        "evidence_status": "required-unimplemented"
      }
    ]
  }
})m3").as_object()};
// | `clip.destructive` | target-id direct bitmap, cutter-id closed vector; `inverse=false`, `keep-cutter=true` is **const true** | pixel/profile hash, image ID, crop/root bounds, sampling/straightening status | T, incompatible-operands, invalid-geometry, unsupported-trim, rasterization-failed, encoding-failed | E / one / computed / P; CLIP-1 cutter preserved exactly; wrappers refuse |
TypeDescriptor const m3_1{boost::json::parse(R"m3({
  "type": "object",
  "properties": {
    "target-id": {
      "type": "string",
      "minLength": 1,
      "maxLength": 128
    },
    "cutter-id": {
      "type": "string",
      "minLength": 1,
      "maxLength": 128
    },
    "inverse": {
      "type": "boolean",
      "default": false
    },
    "keep-cutter": {
      "const": true,
      "default": true
    }
  },
  "required": [
    "target-id",
    "cutter-id"
  ],
  "additionalProperties": false,
  "x-m3-contract": {
    "guard_domain": "document",
    "guard_required": true,
    "needs_document": true,
    "selection_mode": "explicit-role-pair",
    "target_cardinality": {
      "min": 2,
      "max": 2
    },
    "normalization": "explicit distinct roles; reject ancestor overlap",
    "partial_policy": "reject-any-incompatible",
    "role_order": "target then cutter",
    "normalization_order": [
      "validate raw schema and exclusive route",
      "select route",
      "fill defaults recursively only in selected branch; pivot suppresses anchor, size suppresses dpi, token suppresses recipe/analyze/solve",
      "parse exact integers",
      "convert all lengths including nested recipes to CSS px and enforce bounds",
      "resolve and normalize targets",
      "canonicalize normalized params for token binding"
    ],
    "native_service": "`resolve_targets(vector,document)`, `src/ui/tools/destructive-bitmap-clip-chemistry.cpp:750`; `commit_selection`, `:793`",
    "warnings": [],
    "error_rows": [
      {
        "layer": "transport/schema",
        "native_branch": "parse_request: request-too-large",
        "code": "request-too-large",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "parse_request: request-too-large",
        "native_evidence": "`resolve_targets(vector,document)`, `src/ui/tools/destructive-bitmap-clip-chemistry.cpp:750`; `commit_selection`, `:793`",
        "oracle": "P9:clip.destructive:transport/schema:request-too-large:parse_request: request-too-large",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "transport/schema",
        "native_branch": "parse_request: repeated-key",
        "code": "repeated-key",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "parse_request: repeated-key",
        "native_evidence": "`resolve_targets(vector,document)`, `src/ui/tools/destructive-bitmap-clip-chemistry.cpp:750`; `commit_selection`, `:793`",
        "oracle": "P9:clip.destructive:transport/schema:repeated-key:parse_request: repeated-key",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "transport/schema",
        "native_branch": "parse_request: invalid-utf8",
        "code": "invalid-utf8",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "parse_request: invalid-utf8",
        "native_evidence": "`resolve_targets(vector,document)`, `src/ui/tools/destructive-bitmap-clip-chemistry.cpp:750`; `commit_selection`, `:793`",
        "oracle": "P9:clip.destructive:transport/schema:invalid-utf8:parse_request: invalid-utf8",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "transport/schema",
        "native_branch": "parse_request: malformed-json",
        "code": "malformed-json",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "parse_request: malformed-json",
        "native_evidence": "`resolve_targets(vector,document)`, `src/ui/tools/destructive-bitmap-clip-chemistry.cpp:750`; `commit_selection`, `:793`",
        "oracle": "P9:clip.destructive:transport/schema:malformed-json:parse_request: malformed-json",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "transport/schema",
        "native_branch": "raw schema/type/range/route/uniqueItems violation",
        "code": "invalid-argument",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "raw schema/type/range/route/uniqueItems violation",
        "native_evidence": "`resolve_targets(vector,document)`, `src/ui/tools/destructive-bitmap-clip-chemistry.cpp:750`; `commit_selection`, `:793`",
        "oracle": "P9:clip.destructive:transport/schema:invalid-argument:raw schema/type/range/route/uniqueItems violation",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "missing current document",
        "code": "no-document",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "missing current document",
        "native_evidence": "`resolve_targets(vector,document)`, `src/ui/tools/destructive-bitmap-clip-chemistry.cpp:750`; `commit_selection`, `:793`",
        "oracle": "P9:clip.destructive:identity/admission:no-document:missing current document",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "document identity mismatch",
        "code": "stale-document",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "document identity mismatch",
        "native_evidence": "`resolve_targets(vector,document)`, `src/ui/tools/destructive-bitmap-clip-chemistry.cpp:750`; `commit_selection`, `:793`",
        "oracle": "P9:clip.destructive:identity/admission:stale-document:document identity mismatch",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "supplied guard differs from document revision",
        "code": "stale-revision",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "supplied guard differs from document revision",
        "native_evidence": "`resolve_targets(vector,document)`, `src/ui/tools/destructive-bitmap-clip-chemistry.cpp:750`; `commit_selection`, `:793`",
        "oracle": "P9:clip.destructive:identity/admission:stale-revision:supplied guard differs from document revision",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "cancellation observed before commit",
        "code": "cancelled",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "cancellation observed before commit",
        "native_evidence": "`resolve_targets(vector,document)`, `src/ui/tools/destructive-bitmap-clip-chemistry.cpp:750`; `commit_selection`, `:793`",
        "oracle": "P9:clip.destructive:identity/admission:cancelled:cancellation observed before commit",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "explicit ID cannot resolve",
        "code": "unknown-id",
        "retryable": false,
        "mutation_state": "none",
        "route": "explicit-target route",
        "detail.reason": "explicit ID cannot resolve",
        "native_evidence": "`resolve_targets(vector,document)`, `src/ui/tools/destructive-bitmap-clip-chemistry.cpp:750`; `commit_selection`, `:793`",
        "oracle": "P9:clip.destructive:identity/admission:unknown-id:explicit ID cannot resolve",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "InvalidSelection",
        "code": "unsupported-target",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "InvalidSelection",
        "native_evidence": "`resolve_targets(vector,document)`, `src/ui/tools/destructive-bitmap-clip-chemistry.cpp:750`; `commit_selection`, `:793`",
        "oracle": "P9:clip.destructive:command-service:unsupported-target:InvalidSelection",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "InvalidGeometry",
        "code": "invalid-input",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "InvalidGeometry",
        "native_evidence": "`resolve_targets(vector,document)`, `src/ui/tools/destructive-bitmap-clip-chemistry.cpp:750`; `commit_selection`, `:793`",
        "oracle": "P9:clip.destructive:command-service:invalid-input:InvalidGeometry",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "UnsupportedTrim",
        "code": "unsupported-target",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "UnsupportedTrim",
        "native_evidence": "`resolve_targets(vector,document)`, `src/ui/tools/destructive-bitmap-clip-chemistry.cpp:750`; `commit_selection`, `:793`",
        "oracle": "P9:clip.destructive:command-service:unsupported-target:UnsupportedTrim",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "StraighteningEmptyRegion",
        "code": "straightening-empty-region",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "StraighteningEmptyRegion",
        "native_evidence": "`resolve_targets(vector,document)`, `src/ui/tools/destructive-bitmap-clip-chemistry.cpp:750`; `commit_selection`, `:793`",
        "oracle": "P9:clip.destructive:command-service:straightening-empty-region:StraighteningEmptyRegion",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "StraighteningTooLarge",
        "code": "engine-limit",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "StraighteningTooLarge",
        "native_evidence": "`resolve_targets(vector,document)`, `src/ui/tools/destructive-bitmap-clip-chemistry.cpp:750`; `commit_selection`, `:793`",
        "oracle": "P9:clip.destructive:command-service:engine-limit:StraighteningTooLarge",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "RasterizationFailed",
        "code": "rasterization-failed",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "RasterizationFailed",
        "native_evidence": "`resolve_targets(vector,document)`, `src/ui/tools/destructive-bitmap-clip-chemistry.cpp:750`; `commit_selection`, `:793`",
        "oracle": "P9:clip.destructive:command-service:rasterization-failed:RasterizationFailed",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "EncodingFailed",
        "code": "encoding-failed",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "EncodingFailed",
        "native_evidence": "`resolve_targets(vector,document)`, `src/ui/tools/destructive-bitmap-clip-chemistry.cpp:750`; `commit_selection`, `:793`",
        "oracle": "P9:clip.destructive:command-service:encoding-failed:EncodingFailed",
        "evidence_status": "required-unimplemented"
      },
{
        "layer": "identity/admission",
        "native_branch": "read-only session rejects requested document mutation",
        "code": "document-read-only",
        "retryable": false,
        "mutation_state": "none",
        "route": "document-edit or history mutation only",
        "detail.reason": "read-only session rejects requested document mutation",
        "native_evidence": "`resolve_targets(vector,document)`, `src/ui/tools/destructive-bitmap-clip-chemistry.cpp:750`; `commit_selection`, `:793`",
        "oracle": "P9:clip.destructive:identity/admission:document-read-only:read-only session rejects requested document mutation",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "unexpected service exception; rollback before return",
        "code": "internal-error",
        "retryable": false,
        "mutation_state": "rolled-back",
        "route": "all",
        "detail.reason": "unexpected service exception; rollback before return",
        "native_evidence": "`resolve_targets(vector,document)`, `src/ui/tools/destructive-bitmap-clip-chemistry.cpp:750`; `commit_selection`, `:793`",
        "oracle": "P9:clip.destructive:command-service:internal-error:unexpected service exception; rollback before return",
        "evidence_status": "required-unimplemented"
      }
    ],
    "success_native_branches": [
      "Committed",
      "CommittedAllTransparent",
      "CommittedStraightened",
      "CommittedStraightenedAllTransparent",
      "NoChange"
    ],
    "variant_applicability": {
      "success": "ok or changed",
      "unchanged": "native no-op only; query/preparation/history empty are not successful unchanged",
      "computed-dry-run": "successful read-only preflight; no retention, publication, Undo or Redo change"
    },
    "success_rows": [
      {
        "native_branch": "Committed",
        "code": null,
        "retryable": false,
        "mutation_state": "committed",
        "data_variant": "success",
        "evidence_route": "P9:clip.destructive:native:Committed",
        "evidence_status": "required-unimplemented"
      },
      {
        "native_branch": "CommittedAllTransparent",
        "code": null,
        "retryable": false,
        "mutation_state": "committed",
        "data_variant": "success",
        "evidence_route": "P9:clip.destructive:native:CommittedAllTransparent",
        "evidence_status": "required-unimplemented"
      },
      {
        "native_branch": "CommittedStraightened",
        "code": null,
        "retryable": false,
        "mutation_state": "committed",
        "data_variant": "success",
        "evidence_route": "P9:clip.destructive:native:CommittedStraightened",
        "evidence_status": "required-unimplemented"
      },
      {
        "native_branch": "CommittedStraightenedAllTransparent",
        "code": null,
        "retryable": false,
        "mutation_state": "committed",
        "data_variant": "success",
        "evidence_route": "P9:clip.destructive:native:CommittedStraightenedAllTransparent",
        "evidence_status": "required-unimplemented"
      },
      {
        "native_branch": "NoChange",
        "code": null,
        "retryable": false,
        "mutation_state": "none",
        "data_variant": "unchanged",
        "evidence_route": "P9:clip.destructive:native:NoChange",
        "evidence_status": "required-unimplemented"
      }
    ]
  }
})m3").as_object()};
// | `clip.release` | ids of clipped roots; `keep-cutter=true` means retain released cutter artwork; default true | released relation/cutter IDs, roots, exclusions | T, unsupported-clip, clip-failed | E / one / computed / M over explicit roots only (no recursive release of descendant clips); documented bounded release policy |
TypeDescriptor const m3_2{boost::json::parse(R"m3({
  "type": "object",
  "properties": {
    "ids": {
      "type": "array",
      "items": {
        "type": "string",
        "minLength": 1,
        "maxLength": 128
      },
      "minItems": 1,
      "maxItems": 100000,
      "uniqueItems": true
    },
    "keep-cutter": {
      "type": "boolean",
      "default": true
    }
  },
  "required": [
    "ids"
  ],
  "additionalProperties": false,
  "x-m3-contract": {
    "guard_domain": "document",
    "guard_required": true,
    "needs_document": true,
    "selection_mode": "compatible-selected-roots",
    "target_cardinality": {
      "min": 1,
      "max": 100000
    },
    "normalization": "composite roots only; never recursively release",
    "partial_policy": "preserve-and-report-exclusions",
    "role_order": "input-order; each normalized root once",
    "normalization_order": [
      "validate raw schema and exclusive route",
      "select route",
      "fill defaults recursively only in selected branch; pivot suppresses anchor, size suppresses dpi, token suppresses recipe/analyze/solve",
      "parse exact integers",
      "convert all lengths including nested recipes to CSS px and enforce bounds",
      "resolve and normalize targets",
      "canonicalize normalized params for token binding"
    ],
    "native_service": "`ObjectSet::unsetMask`, `src/selection-chemistry.cpp:4086`; PowerClip release wrapper `actions-object.cpp:297`",
    "warnings": [
      {
        "code": "exclusions",
        "emit_site": "`ObjectSet::unsetMask`, `src/selection-chemistry.cpp:4086`; PowerClip release wrapper `actions-object.cpp:297`; after target resolution, nonempty exclusions"
      }
    ],
    "error_rows": [
      {
        "layer": "transport/schema",
        "native_branch": "parse_request: request-too-large",
        "code": "request-too-large",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "parse_request: request-too-large",
        "native_evidence": "`ObjectSet::unsetMask`, `src/selection-chemistry.cpp:4086`; PowerClip release wrapper `actions-object.cpp:297`",
        "oracle": "P9:clip.release:transport/schema:request-too-large:parse_request: request-too-large",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "transport/schema",
        "native_branch": "parse_request: repeated-key",
        "code": "repeated-key",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "parse_request: repeated-key",
        "native_evidence": "`ObjectSet::unsetMask`, `src/selection-chemistry.cpp:4086`; PowerClip release wrapper `actions-object.cpp:297`",
        "oracle": "P9:clip.release:transport/schema:repeated-key:parse_request: repeated-key",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "transport/schema",
        "native_branch": "parse_request: invalid-utf8",
        "code": "invalid-utf8",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "parse_request: invalid-utf8",
        "native_evidence": "`ObjectSet::unsetMask`, `src/selection-chemistry.cpp:4086`; PowerClip release wrapper `actions-object.cpp:297`",
        "oracle": "P9:clip.release:transport/schema:invalid-utf8:parse_request: invalid-utf8",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "transport/schema",
        "native_branch": "parse_request: malformed-json",
        "code": "malformed-json",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "parse_request: malformed-json",
        "native_evidence": "`ObjectSet::unsetMask`, `src/selection-chemistry.cpp:4086`; PowerClip release wrapper `actions-object.cpp:297`",
        "oracle": "P9:clip.release:transport/schema:malformed-json:parse_request: malformed-json",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "transport/schema",
        "native_branch": "raw schema/type/range/route/uniqueItems violation",
        "code": "invalid-argument",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "raw schema/type/range/route/uniqueItems violation",
        "native_evidence": "`ObjectSet::unsetMask`, `src/selection-chemistry.cpp:4086`; PowerClip release wrapper `actions-object.cpp:297`",
        "oracle": "P9:clip.release:transport/schema:invalid-argument:raw schema/type/range/route/uniqueItems violation",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "missing current document",
        "code": "no-document",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "missing current document",
        "native_evidence": "`ObjectSet::unsetMask`, `src/selection-chemistry.cpp:4086`; PowerClip release wrapper `actions-object.cpp:297`",
        "oracle": "P9:clip.release:identity/admission:no-document:missing current document",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "document identity mismatch",
        "code": "stale-document",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "document identity mismatch",
        "native_evidence": "`ObjectSet::unsetMask`, `src/selection-chemistry.cpp:4086`; PowerClip release wrapper `actions-object.cpp:297`",
        "oracle": "P9:clip.release:identity/admission:stale-document:document identity mismatch",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "supplied guard differs from document revision",
        "code": "stale-revision",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "supplied guard differs from document revision",
        "native_evidence": "`ObjectSet::unsetMask`, `src/selection-chemistry.cpp:4086`; PowerClip release wrapper `actions-object.cpp:297`",
        "oracle": "P9:clip.release:identity/admission:stale-revision:supplied guard differs from document revision",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "cancellation observed before commit",
        "code": "cancelled",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "cancellation observed before commit",
        "native_evidence": "`ObjectSet::unsetMask`, `src/selection-chemistry.cpp:4086`; PowerClip release wrapper `actions-object.cpp:297`",
        "oracle": "P9:clip.release:identity/admission:cancelled:cancellation observed before commit",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "explicit ID cannot resolve",
        "code": "unknown-id",
        "retryable": false,
        "mutation_state": "none",
        "route": "explicit-target route",
        "detail.reason": "explicit ID cannot resolve",
        "native_evidence": "`ObjectSet::unsetMask`, `src/selection-chemistry.cpp:4086`; PowerClip release wrapper `actions-object.cpp:297`",
        "oracle": "P9:clip.release:identity/admission:unknown-id:explicit ID cannot resolve",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "EditTransaction inactive",
        "code": "transaction-unavailable",
        "retryable": true,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "EditTransaction inactive",
        "native_evidence": "`ObjectSet::unsetMask`, `src/selection-chemistry.cpp:4086`; PowerClip release wrapper `actions-object.cpp:297`",
        "oracle": "P9:clip.release:identity/admission:transaction-unavailable:EditTransaction inactive",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "read-only session rejects requested document mutation",
        "code": "document-read-only",
        "retryable": false,
        "mutation_state": "none",
        "route": "document-edit or history mutation only",
        "detail.reason": "read-only session rejects requested document mutation",
        "native_evidence": "`ObjectSet::unsetMask`, `src/selection-chemistry.cpp:4086`; PowerClip release wrapper `actions-object.cpp:297`",
        "oracle": "P9:clip.release:identity/admission:document-read-only:read-only session rejects requested document mutation",
        "evidence_status": "required-unimplemented"
      },
{
        "layer": "command-service",
        "native_branch": "ClipDocumentService::Reason::InvalidItem",
        "code": "unknown-id",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "ClipDocumentService::Reason::InvalidItem",
        "native_evidence": "`ObjectSet::unsetMask`, `src/selection-chemistry.cpp:4086`; PowerClip release wrapper `actions-object.cpp:297`",
        "oracle": "P9:clip.release:command-service:unknown-id:ClipDocumentService::Reason::InvalidItem",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "ClipDocumentService::Reason::ProtectedItem",
        "code": "unavailable",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "ClipDocumentService::Reason::ProtectedItem",
        "native_evidence": "`ObjectSet::unsetMask`, `src/selection-chemistry.cpp:4086`; PowerClip release wrapper `actions-object.cpp:297`",
        "oracle": "P9:clip.release:command-service:unavailable:ClipDocumentService::Reason::ProtectedItem",
        "evidence_status": "required-unimplemented"
      },
{
        "layer": "command-service",
        "native_branch": "ClipDocumentService::Reason::NotClipped",
        "code": "no-eligible-targets",
        "retryable": false,
        "mutation_state": "none",
        "route": "all requested roots not clipped; otherwise exclusions warning and compatible roots processed",
        "detail.reason": "ClipDocumentService::Reason::NotClipped",
        "native_evidence": "`ObjectSet::unsetMask`, `src/selection-chemistry.cpp:4086`; PowerClip release wrapper `actions-object.cpp:297`",
        "oracle": "P9:clip.release:command-service:no-eligible-targets:ClipDocumentService::Reason::NotClipped",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "ClipDocumentService::Reason::UnsupportedHelperUngroup",
        "code": "invalid-argument",
        "retryable": false,
        "mutation_state": "none",
        "route": "defensive native-only status; wire always ungroup_helpers=false; reviewed removed-inapplicable required, never fake execution",
        "detail.reason": "ClipDocumentService::Reason::UnsupportedHelperUngroup",
        "native_evidence": "`ObjectSet::unsetMask`, `src/selection-chemistry.cpp:4086`; PowerClip release wrapper `actions-object.cpp:297`",
        "oracle": "P9:clip.release:command-service:invalid-argument:ClipDocumentService::Reason::UnsupportedHelperUngroup",
        "evidence_status": "requires-reviewed-removal-not-a-pass"
      },
      {
        "layer": "command-service",
        "native_branch": "unexpected service exception; rollback before return",
        "code": "internal-error",
        "retryable": false,
        "mutation_state": "rolled-back",
        "route": "all",
        "detail.reason": "unexpected service exception; rollback before return",
        "native_evidence": "`ObjectSet::unsetMask`, `src/selection-chemistry.cpp:4086`; PowerClip release wrapper `actions-object.cpp:297`",
        "oracle": "P9:clip.release:command-service:internal-error:unexpected service exception; rollback before return",
        "evidence_status": "required-unimplemented"
      }
    ],
    "success_native_branches": [
      "ClipDocumentService::Status::Prepared",
      "ClipDocumentService::Status::Applied",
      "ClipDocumentService::Status::Unchanged"
    ],
    "variant_applicability": {
      "success": "ok or changed",
      "unchanged": "native no-op only; query/preparation/history empty are not successful unchanged",
      "computed-dry-run": "successful read-only preflight; no retention, publication, Undo or Redo change"
    },
    "success_rows": [
      {
        "native_branch": "ClipDocumentService::Status::Prepared",
        "code": null,
        "retryable": false,
        "mutation_state": "none",
        "data_variant": "computed-dry-run",
        "evidence_route": "P9:clip.release:native:ClipDocumentService::Status::Prepared",
        "evidence_status": "required-unimplemented"
      },
      {
        "native_branch": "ClipDocumentService::Status::Applied",
        "code": null,
        "retryable": false,
        "mutation_state": "committed",
        "data_variant": "success",
        "evidence_route": "P9:clip.release:native:ClipDocumentService::Status::Applied",
        "evidence_status": "required-unimplemented"
      },
      {
        "native_branch": "ClipDocumentService::Status::Unchanged",
        "code": null,
        "retryable": false,
        "mutation_state": "none",
        "data_variant": "unchanged",
        "evidence_route": "P9:clip.release:native:ClipDocumentService::Status::Unchanged",
        "evidence_status": "required-unimplemented"
      }
    ]
  }
})m3").as_object()};
}
std::vector<PackageCommand> clip_commands()
{
    std::vector<PackageCommand> out;
    out.push_back({ActionSpec{.name="clip.set", .mode="P", .summary="Set a clip on the selected targets using the requested cutter.", .canonical_id="clip.set", .handler=production_unavailable_action, .version=2, .effects="document-edit", .target_policy="P", .undo_policy="one-step", .dry_run_grade="computed", .cancellation_boundary="before-native-publication", .input=&m3_0}, "clip.set", "document-edit", "P", {}, {{"type","object"},{"properties",boost::json::object{}},{"additionalProperties",false}}, {"slice-unavailable"}, {}});
    out.push_back({ActionSpec{.name="clip.destructive", .mode="P", .summary="Apply a destructive clip while keeping the cutter.", .canonical_id="clip.destructive", .handler=production_unavailable_action, .version=2, .effects="document-edit", .target_policy="P", .undo_policy="one-step", .dry_run_grade="computed", .cancellation_boundary="before-native-publication", .input=&m3_1}, "clip.destructive", "document-edit", "P", {}, {{"type","object"},{"properties",boost::json::object{}},{"additionalProperties",false}}, {"slice-unavailable"}, {}});
    out.push_back({ActionSpec{.name="clip.release", .mode="M", .summary="Release the clip from the selected targets.", .canonical_id="clip.release", .handler=production_unavailable_action, .version=2, .effects="document-edit", .target_policy="M", .undo_policy="one-step", .dry_run_grade="computed", .cancellation_boundary="before-native-publication", .input=&m3_2}, "clip.release", "document-edit", "M", {}, {{"type","object"},{"properties",boost::json::object{}},{"additionalProperties",false}}, {"slice-unavailable"}, {}});
    out[0].example = boost::json::parse(R"m3({
  "target-id": "image1",
  "cutter-id": "cutter1"
})m3").as_object();
    out[0].result_data = boost::json::parse(R"m3({
  "oneOf": [
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "success"
        },
        "relations": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "target-id": {
                "type": "string"
              },
              "cutter-id": {
                "type": [
                  "string",
                  "null"
                ]
              },
              "resource-id": {
                "type": [
                  "string",
                  "null"
                ]
              },
              "target-retained": {
                "type": "boolean"
              },
              "cutter-retained": {
                "type": "boolean"
              }
            },
            "required": [
              "target-id",
              "cutter-id",
              "resource-id",
              "target-retained",
              "cutter-retained"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 100000
        }
      },
      "required": [
        "variant",
        "relations"
      ],
      "additionalProperties": false
    },
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "unchanged"
        },
        "relations": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "target-id": {
                "type": "string"
              },
              "cutter-id": {
                "type": [
                  "string",
                  "null"
                ]
              },
              "resource-id": {
                "type": [
                  "string",
                  "null"
                ]
              },
              "target-retained": {
                "type": "boolean"
              },
              "cutter-retained": {
                "type": "boolean"
              }
            },
            "required": [
              "target-id",
              "cutter-id",
              "resource-id",
              "target-retained",
              "cutter-retained"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 100000
        }
      },
      "required": [
        "variant",
        "relations"
      ],
      "additionalProperties": false
    },
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "computed-dry-run"
        },
        "relations": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "target-id": {
                "type": "string"
              },
              "cutter-id": {
                "type": [
                  "string",
                  "null"
                ]
              },
              "resource-id": {
                "type": [
                  "string",
                  "null"
                ]
              },
              "target-retained": {
                "type": "boolean"
              },
              "cutter-retained": {
                "type": "boolean"
              }
            },
            "required": [
              "target-id",
              "cutter-id",
              "resource-id",
              "target-retained",
              "cutter-retained"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 100000
        }
      },
      "required": [
        "variant",
        "relations"
      ],
      "additionalProperties": false
    }
  ],
  "description": "Data only; common va-studio.cli-result/1 envelope authoritative. Variant selected by status/dry_run; any duplicated fields must equal envelope. Preview IDs never claim live publication."
})m3").as_object();
    out[0].errors = {"request-too-large","repeated-key","invalid-utf8","malformed-json","invalid-argument","no-document","stale-document","stale-revision","cancelled","unknown-id","transaction-unavailable","document-read-only","unavailable","invalid-transform","duplicate-id","unsupported-target","internal-error"};
    out[1].example = boost::json::parse(R"m3({
  "target-id": "image1",
  "cutter-id": "cutter1"
})m3").as_object();
    out[1].result_data = boost::json::parse(R"m3({
  "oneOf": [
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "success"
        },
        "image-id": {
          "type": "string"
        },
        "target-retained": {
          "type": "boolean"
        },
        "cutter-retained": {
          "const": true
        },
        "pixel-sha256": {
          "type": "string",
          "pattern": "^[0-9a-f]{64}$"
        },
        "profile-sha256": {
          "type": "string",
          "pattern": "^[0-9a-f]{64}$"
        },
        "bounds": {
          "description": "Bounds of the published image in document coordinates after the native crop and transparent-border trim.",
          "oneOf": [
            {
              "type": "array",
              "items": {
                "type": "number"
              },
              "minItems": 4,
              "maxItems": 4
            },
            {
              "type": "null"
            }
          ]
        },
        "crop-bounds": {
          "oneOf": [
            {
              "type": "array",
              "items": {
                "type": "number"
              },
              "minItems": 4,
              "maxItems": 4
            },
            {
              "type": "null"
            }
          ]
        },
        "sampling": {
          "type": "string"
        },
        "straightened": {
          "type": "boolean"
        }
      },
      "required": [
        "variant",
        "image-id",
        "target-retained",
        "cutter-retained",
        "pixel-sha256",
        "profile-sha256",
        "bounds",
        "crop-bounds",
        "sampling",
        "straightened"
      ],
      "additionalProperties": false
    },
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "unchanged"
        },
        "image-id": {
          "type": "string"
        },
        "target-retained": {
          "type": "boolean"
        },
        "cutter-retained": {
          "const": true
        },
        "pixel-sha256": {
          "type": "string",
          "pattern": "^[0-9a-f]{64}$"
        },
        "profile-sha256": {
          "type": "string",
          "pattern": "^[0-9a-f]{64}$"
        },
        "bounds": {
          "oneOf": [
            {
              "type": "array",
              "items": {
                "type": "number"
              },
              "minItems": 4,
              "maxItems": 4
            },
            {
              "type": "null"
            }
          ]
        },
        "crop-bounds": {
          "oneOf": [
            {
              "type": "array",
              "items": {
                "type": "number"
              },
              "minItems": 4,
              "maxItems": 4
            },
            {
              "type": "null"
            }
          ]
        },
        "sampling": {
          "type": "string"
        },
        "straightened": {
          "type": "boolean"
        }
      },
      "required": [
        "variant",
        "image-id",
        "target-retained",
        "cutter-retained",
        "pixel-sha256",
        "profile-sha256",
        "bounds",
        "crop-bounds",
        "sampling",
        "straightened"
      ],
      "additionalProperties": false
    },
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "computed-dry-run"
        },
        "image-id": {
          "type": [
            "string",
            "null"
          ],
          "description": "null for prospective output; existing source IDs remain strings"
        },
        "target-retained": {
          "type": "boolean"
        },
        "cutter-retained": {
          "const": true
        },
        "pixel-sha256": {
          "type": "string",
          "pattern": "^[0-9a-f]{64}$"
        },
        "profile-sha256": {
          "type": "string",
          "pattern": "^[0-9a-f]{64}$"
        },
        "bounds": {
          "oneOf": [
            {
              "type": "array",
              "items": {
                "type": "number"
              },
              "minItems": 4,
              "maxItems": 4
            },
            {
              "type": "null"
            }
          ]
        },
        "crop-bounds": {
          "oneOf": [
            {
              "type": "array",
              "items": {
                "type": "number"
              },
              "minItems": 4,
              "maxItems": 4
            },
            {
              "type": "null"
            }
          ]
        },
        "sampling": {
          "type": [
            "string",
            "null"
          ],
          "description": "null for prospective output; existing source IDs remain strings"
        },
        "straightened": {
          "type": "boolean"
        }
      },
      "required": [
        "variant",
        "image-id",
        "target-retained",
        "cutter-retained",
        "pixel-sha256",
        "profile-sha256",
        "bounds",
        "crop-bounds",
        "sampling",
        "straightened"
      ],
      "additionalProperties": false
    }
  ],
  "description": "Data only; common va-studio.cli-result/1 envelope authoritative. Variant selected by status/dry_run; any duplicated fields must equal envelope. Preview IDs never claim live publication."
})m3").as_object();
    out[1].errors = {"request-too-large","repeated-key","invalid-utf8","malformed-json","invalid-argument","no-document","stale-document","stale-revision","cancelled","unknown-id","unsupported-target","invalid-input","straightening-empty-region","engine-limit","rasterization-failed","encoding-failed","document-read-only","internal-error"};
    out[2].example = boost::json::parse(R"m3({
  "ids": [
    "shape1"
  ]
})m3").as_object();
    out[2].result_data = boost::json::parse(R"m3({
  "oneOf": [
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "success"
        },
        "relations": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "target-id": {
                "type": "string"
              },
              "cutter-id": {
                "type": [
                  "string",
                  "null"
                ]
              },
              "resource-id": {
                "type": [
                  "string",
                  "null"
                ]
              },
              "target-retained": {
                "type": "boolean"
              },
              "cutter-retained": {
                "type": "boolean"
              }
            },
            "required": [
              "target-id",
              "cutter-id",
              "resource-id",
              "target-retained",
              "cutter-retained"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 100000
        }
      },
      "required": [
        "variant",
        "relations"
      ],
      "additionalProperties": false
    },
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "unchanged"
        },
        "relations": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "target-id": {
                "type": "string"
              },
              "cutter-id": {
                "type": [
                  "string",
                  "null"
                ]
              },
              "resource-id": {
                "type": [
                  "string",
                  "null"
                ]
              },
              "target-retained": {
                "type": "boolean"
              },
              "cutter-retained": {
                "type": "boolean"
              }
            },
            "required": [
              "target-id",
              "cutter-id",
              "resource-id",
              "target-retained",
              "cutter-retained"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 100000
        }
      },
      "required": [
        "variant",
        "relations"
      ],
      "additionalProperties": false
    },
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "computed-dry-run"
        },
        "relations": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "target-id": {
                "type": "string"
              },
              "cutter-id": {
                "type": [
                  "string",
                  "null"
                ]
              },
              "resource-id": {
                "type": [
                  "string",
                  "null"
                ]
              },
              "target-retained": {
                "type": "boolean"
              },
              "cutter-retained": {
                "type": "boolean"
              }
            },
            "required": [
              "target-id",
              "cutter-id",
              "resource-id",
              "target-retained",
              "cutter-retained"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 100000
        }
      },
      "required": [
        "variant",
        "relations"
      ],
      "additionalProperties": false
    }
  ],
  "description": "Data only; common va-studio.cli-result/1 envelope authoritative. Variant selected by status/dry_run; any duplicated fields must equal envelope. Preview IDs never claim live publication."
})m3").as_object();
    out[2].errors = {"request-too-large","repeated-key","invalid-utf8","malformed-json","invalid-argument","no-document","stale-document","stale-revision","cancelled","unknown-id","transaction-unavailable","document-read-only","unavailable","invalid-transform","no-eligible-targets","internal-error"};
    out[0].policy = "explicit-role-pair"; out[0].spec.target_policy = "explicit-role-pair";
    out[1].policy = "explicit-role-pair"; out[1].spec.target_policy = "explicit-role-pair";
    out[2].policy = "compatible-selected-roots"; out[2].spec.target_policy = "compatible-selected-roots";
    out[0].warnings = {};
    out[1].warnings = {};
    out[2].warnings = {"exclusions"};
    return out;
}
}

// Explicit-role adapters; native services own geometry, this caller owns settlement.
#include "vacards-cli-edit-services.h"
#include "document.h"
#include "selection.h"
#include "object/clip-document-service.h"
#include "object/sp-item.h"
#include "object/sp-image.h"
#include "object/sp-clippath.h"
#include "display/cairo-utils.h"
#include "ui/tools/destructive-bitmap-clip-chemistry.h"
#include "xml/repr.h"
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <map>
namespace Inkscape::VACardsCli {
namespace {
char const *clip_reason(ClipDocumentService::Reason reason) {
    using R=ClipDocumentService::Reason;
    switch(reason) {
#define P7_REASON(x) case R::x: return "ClipDocumentService::Reason::" #x
    P7_REASON(None); P7_REASON(InvalidDocument); P7_REASON(InvalidItem); P7_REASON(MissingSource);
    P7_REASON(OverlappingRoles); P7_REASON(CloneWithSource); P7_REASON(ProtectedItem);
    P7_REASON(ReferencedCutter); P7_REASON(SingularTransform); P7_REASON(UnsupportedInverseTarget);
    P7_REASON(UnsupportedInverseCutter); P7_REASON(ExistingPowerClip); P7_REASON(NotClipped); P7_REASON(UnsupportedHelperUngroup);
#undef P7_REASON
    }
    return "unknown-native-reason";
}
boost::json::value clip_bounds(SPItem *item) {
    auto b=item ? item->documentVisualBounds() : Geom::OptRect{};
    return b ? boost::json::value(boost::json::array{b->left(),b->top(),b->right(),b->bottom()}) : boost::json::value(nullptr);
}
std::string clip_digest(void const *data, std::size_t size) {
    auto sum=g_checksum_new(G_CHECKSUM_SHA256);
    if(size) g_checksum_update(sum,static_cast<guchar const *>(data),size);
    std::string value=g_checksum_get_string(sum);g_checksum_free(sum);return value;
}
}
Record execute_clip(Request const &request, DispatchContext &dispatch, ProductionContext &context)
{
    Record r; r.action=request.command;r.dry_run=request.dry_run;r.mode="collective-compositing";
    r.normalized_params=request.params;r.preferred_unit=dispatch.preferred_unit;
    auto &live=context.edits.document; auto stamp=document_stamp(&live);
    r.document_id=stamp.id;r.revision_before=r.revision_after=stamp.revision;
    for(auto *item:context.edits.selection.items()) if(item->getId())r.selection_after.emplace_back(item->getId());
    auto refusal=[&](std::string code,std::string reason,bool retry=false) {
        r.status=code=="cancelled"?Status::Cancelled:Status::Rejected;r.reason=code;r.message=reason;
        r.error=ParseError{code,{},reason};r.error_details={{"reason",reason},{"mutation_state","none"}};r.error_retryable=retry;
        r.data.clear();r.created.clear();r.modified.clear();r.deleted.clear();return r;
    };
    auto cancelled=[&] {return dispatch.cancelled && dispatch.cancelled();};
    if(cancelled())return refusal("cancelled","cancellation observed before commit");
    std::unique_ptr<SPDocument> projected;
    SPDocument *doc=&live;
    if(request.dry_run) {
        auto xml=sp_repr_save_buf(live.getReprDoc()).raw();
        projected=SPDocument::createNewDocFromMem(std::span<char const>(xml.data(),xml.size()));
        if(!projected)return refusal("internal-error","could not create preview document");
        projected->ensureUpToDate();doc=projected.get();
    }
    auto resolve=[&](boost::json::value const &v){return cast<SPItem>(doc->getObjectById(std::string(v.as_string())));};
    bool release=request.command=="clip.release", destructive=request.command=="clip.destructive";
    std::vector<SPItem *> roots;SPItem *target=nullptr,*cutter=nullptr;
    std::string cutter_id;
    if(release) {
        for(auto const &id:request.params.at("ids").as_array()) {auto *item=resolve(id);if(!item)return refusal("unknown-id","explicit ID cannot resolve");roots.push_back(item);}
    } else {
        target=resolve(request.params.at("target-id"));cutter=resolve(request.params.at("cutter-id"));
        if(!target||!cutter)return refusal("unknown-id","explicit ID cannot resolve");
        roots={target};cutter_id=cutter->getId();
    }
    bool inverse=request.params.if_contains("inverse") && request.params.at("inverse").as_bool();
    bool keep=!request.params.if_contains("keep-cutter") || request.params.at("keep-cutter").as_bool();
    r.selected=release?roots.size():2;
    namespace C=ClipDocumentService;
    C::Result prepared;
    if(!destructive) {
        prepared=release?C::prepareReleaseClip(doc,roots,{keep,false}):C::prepareSetClip(doc,target,cutter,{inverse,keep});
        if(prepared.status==C::Status::Refused) {
            std::string code="unsupported-target";
            switch(prepared.reason) {
            case C::Reason::InvalidDocument: code="no-document";break;
            case C::Reason::InvalidItem:code="unknown-id";break;
            case C::Reason::ProtectedItem:code="unavailable";break;
            case C::Reason::OverlappingRoles:code="duplicate-id";break;
            case C::Reason::SingularTransform:code="invalid-transform";break;
            case C::Reason::NotClipped:code="no-eligible-targets";break;
            default:break;
            }
            return refusal(code,clip_reason(prepared.reason));
        }
    }
    std::unique_ptr<EditTransaction> transaction;
    if(!request.dry_run) {
        transaction=std::make_unique<EditTransaction>(doc,&context.edits.selection,context.edits.operation_lease);
        if(!transaction->active())return refusal("transaction-unavailable","EditTransaction inactive",true);
    }
    try {
        bool changed=false;
        if(destructive) {
            namespace D=UI::Tools::DestructiveBitmapClip;
            if(!is<SPImage>(target)||target==cutter||target->isAncestorOf(cutter)||cutter->isAncestorOf(target))
                return refusal("unsupported-target","InvalidSelection");
            Selection pair(doc);pair.add(target);pair.add(cutter);
            auto status=D::commit_selection(pair,inverse?D::Mode::KeepOutside:D::Mode::KeepInside,{},!request.dry_run);
            bool straight=status==D::CommitStatus::CommittedStraightened||status==D::CommitStatus::CommittedStraightenedAllTransparent;
            changed=status==D::CommitStatus::Committed||status==D::CommitStatus::CommittedAllTransparent||straight;
            if(!changed && status!=D::CommitStatus::NoChange) {
                std::string code="unsupported-target",reason="InvalidSelection";
                switch(status) {
                case D::CommitStatus::InvalidGeometry:code="invalid-input";reason="InvalidGeometry";break;
                case D::CommitStatus::UnsupportedTrim:reason="UnsupportedTrim";break;
                case D::CommitStatus::StraighteningEmptyRegion:code="straightening-empty-region";reason="StraighteningEmptyRegion";break;
                case D::CommitStatus::StraighteningTooLarge:code="engine-limit";reason="StraighteningTooLarge";break;
                case D::CommitStatus::RasterizationFailed:code="rasterization-failed";reason="RasterizationFailed";break;
                case D::CommitStatus::EncodingFailed:code="encoding-failed";reason="EncodingFailed";break;
                case D::CommitStatus::Cancelled:code="cancelled";reason="Cancelled";break;
                default:break;
                }
                return refusal(code,reason);
            }
            auto *image=cast<SPImage>(target);Pixbuf pixels(*image->pixbuf);pixels.ensurePixelFormat(Pixbuf::PF_GDK);
            auto raw=pixels.getPixbufRaw();auto sum=g_checksum_new(G_CHECKSUM_SHA256);
            for(int y=0;y<pixels.height();++y)g_checksum_update(sum,gdk_pixbuf_get_pixels(raw)+y*gdk_pixbuf_get_rowstride(raw),pixels.width()*4);
            std::string hash=g_checksum_get_string(sum);g_checksum_free(sum);
            std::string profile=clip_digest(nullptr,0);
            if(auto encoded=gdk_pixbuf_get_option(raw,"icc-profile")){gsize n=0;auto bytes=g_base64_decode(encoded,&n);profile=clip_digest(bytes,n);g_free(bytes);}
            r.data={{"image-id",target->getId()},{"target-retained",true},{"cutter-retained",true},{"pixel-sha256",hash},{"profile-sha256",profile},
                {"bounds",clip_bounds(target)},{"crop-bounds",clip_bounds(target)},{"sampling",straight?"document-aligned-source-density":"source-grid"},{"straightened",straight}};
            if(changed)r.modified.emplace_back(target->getId());
        } else {
            std::map<std::string,std::string> old_resources;
            for(auto *root:roots)if(auto *clip=root->getClipObject())old_resources[root->getId()]=clip->getId();
            auto native=release?C::releaseClip(doc,roots,{keep,false}):C::setClip(doc,target,cutter,{inverse,keep});
            if(native.status==C::Status::Refused)return refusal("internal-error",clip_reason(native.reason));
            changed=native.status==C::Status::Applied;r.covered=native.covered_ids.size();
            for(auto const &e:native.excluded)r.excluded.push_back({e.id,clip_reason(e.reason)});
            if(!r.excluded.empty())r.warnings.emplace_back("exclusions");
            r.modified=native.affected_ids;r.created=native.restored_cutter_ids;
            if(!release && !keep)r.deleted.push_back(cutter_id);
            boost::json::array relations;
            for(auto const &id:native.affected_ids) {
                // Restored cutters are outputs, not released target relations.
                if(release && !old_resources.contains(id))continue;
                if(!release && id != target->getId())continue;
                auto *item=cast<SPItem>(doc->getObjectById(id));
                auto *clip=item?item->getClipObject():nullptr;
                boost::json::value resource=clip?boost::json::value(clip->getId()):boost::json::value(nullptr);
                if(release && old_resources.contains(id))resource=old_resources[id];
                if(request.dry_run && !release && changed)resource=nullptr;
                relations.emplace_back(boost::json::object{{"target-id",id},{"cutter-id",release?boost::json::value(nullptr):boost::json::value(cutter_id)},
                    {"resource-id",resource},{"target-retained",item!=nullptr},{"cutter-retained",keep}});
            }
            r.data={{"relations",std::move(relations)}};
        }
        r.data["variant"]=request.dry_run?"computed-dry-run":changed?"success":"unchanged";
        r.eligible=r.selected-static_cast<int>(r.excluded.size())-r.covered;
        if(request.dry_run){r.created.clear();r.modified.clear();r.deleted.clear();return r;}
        if(cancelled()){transaction->rollback();return refusal("cancelled","cancellation observed before commit");}
        if(changed){transaction->commit(Util::Internal::ContextString("Clip objects"),"object-set-clip");
            std::erase_if(r.selection_after,[&](auto const &id){return !doc->getObjectById(id);});r.status=Status::Changed;r.one_undo_step=true;r.undo_effect="one-step";r.revision_after=document_stamp(doc).revision;}
        else {transaction->rollback();r.status=Status::Unchanged;}
        return r;
    } catch(...) {
        if(transaction)transaction->rollback();auto failed=refusal("internal-error","unexpected service exception; rollback before return");
        failed.status=Status::Failed;failed.error_details["mutation_state"]=request.dry_run?"none":"rolled-back";return failed;
    }
}
}
