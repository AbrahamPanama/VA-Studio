// SPDX-License-Identifier: GPL-2.0-or-later
#include "actions-vacards-selection.h"

#include "vacards-cli-production.h"
#include <boost/json.hpp>
namespace Inkscape::VACardsCli {
namespace {
// | `selection.set` | `ids` | normalized roots, covered IDs, selection-before/after, session_revision | T; non-rendering IDs refuse | S / none / computed normalization / S, composite-root normalization only |
TypeDescriptor const m3_0{boost::json::parse(R"m3({
  "type": "object",
  "properties": {
    "ids": {
      "type": "array",
      "items": {
        "type": "string",
        "minLength": 1,
        "maxLength": 128
      },
      "minItems": 0,
      "maxItems": 100000,
      "uniqueItems": true
    }
  },
  "required": [
    "ids"
  ],
  "additionalProperties": false,
  "x-m3-contract": {
    "guard_domain": "session",
    "guard_required": true,
    "needs_document": true,
    "selection_mode": "explicit-root-selection",
    "target_cardinality": {
      "min": 0,
      "max": 100000
    },
    "normalization": "ordered-composite-roots; ancestor covers descendant, preserving surviving input order",
    "partial_policy": "reject-any-incompatible",
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
    "native_service": "`Util::resolve_composite_targets`, `src/util/operation-targets.h:28`; `ObjectSet::setList`, `src/object/object-set.h:304`",
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
        "native_evidence": "`Util::resolve_composite_targets`, `src/util/operation-targets.h:28`; `ObjectSet::setList`, `src/object/object-set.h:304`",
        "oracle": "P9:selection.set:transport/schema:request-too-large:parse_request: request-too-large",
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
        "native_evidence": "`Util::resolve_composite_targets`, `src/util/operation-targets.h:28`; `ObjectSet::setList`, `src/object/object-set.h:304`",
        "oracle": "P9:selection.set:transport/schema:repeated-key:parse_request: repeated-key",
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
        "native_evidence": "`Util::resolve_composite_targets`, `src/util/operation-targets.h:28`; `ObjectSet::setList`, `src/object/object-set.h:304`",
        "oracle": "P9:selection.set:transport/schema:invalid-utf8:parse_request: invalid-utf8",
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
        "native_evidence": "`Util::resolve_composite_targets`, `src/util/operation-targets.h:28`; `ObjectSet::setList`, `src/object/object-set.h:304`",
        "oracle": "P9:selection.set:transport/schema:malformed-json:parse_request: malformed-json",
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
        "native_evidence": "`Util::resolve_composite_targets`, `src/util/operation-targets.h:28`; `ObjectSet::setList`, `src/object/object-set.h:304`",
        "oracle": "P9:selection.set:transport/schema:invalid-argument:raw schema/type/range/route/uniqueItems violation",
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
        "native_evidence": "`Util::resolve_composite_targets`, `src/util/operation-targets.h:28`; `ObjectSet::setList`, `src/object/object-set.h:304`",
        "oracle": "P9:selection.set:identity/admission:no-document:missing current document",
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
        "native_evidence": "`Util::resolve_composite_targets`, `src/util/operation-targets.h:28`; `ObjectSet::setList`, `src/object/object-set.h:304`",
        "oracle": "P9:selection.set:identity/admission:stale-document:document identity mismatch",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "supplied guard differs from session revision",
        "code": "stale-revision",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "supplied guard differs from session revision",
        "native_evidence": "`Util::resolve_composite_targets`, `src/util/operation-targets.h:28`; `ObjectSet::setList`, `src/object/object-set.h:304`",
        "oracle": "P9:selection.set:identity/admission:stale-revision:supplied guard differs from session revision",
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
        "native_evidence": "`Util::resolve_composite_targets`, `src/util/operation-targets.h:28`; `ObjectSet::setList`, `src/object/object-set.h:304`",
        "oracle": "P9:selection.set:identity/admission:cancelled:cancellation observed before commit",
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
        "native_evidence": "`Util::resolve_composite_targets`, `src/util/operation-targets.h:28`; `ObjectSet::setList`, `src/object/object-set.h:304`",
        "oracle": "P9:selection.set:identity/admission:unknown-id:explicit ID cannot resolve",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "resolve_composite_targets: unknown root",
        "code": "unknown-id",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "resolve_composite_targets",
        "native_evidence": "`Util::resolve_composite_targets`, `src/util/operation-targets.h:28`; `ObjectSet::setList`, `src/object/object-set.h:304`",
        "oracle": "P9:selection.set:command-service:unknown-id:resolve_composite_targets: unknown root",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "resolve_composite_targets: protected root",
        "code": "unavailable",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "resolve_composite_targets",
        "native_evidence": "`Util::resolve_composite_targets`, `src/util/operation-targets.h:28`; `ObjectSet::setList`, `src/object/object-set.h:304`",
        "oracle": "P9:selection.set:command-service:unavailable:resolve_composite_targets: protected root",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "unexpected service exception; rollback before return",
        "code": "internal-error",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "unexpected service exception; rollback before return",
        "native_evidence": "`Util::resolve_composite_targets`, `src/util/operation-targets.h:28`; `ObjectSet::setList`, `src/object/object-set.h:304`",
        "oracle": "P9:selection.set:command-service:internal-error:unexpected service exception; rollback before return",
        "evidence_status": "required-unimplemented"
      }
    ],
    "success_native_branches": [
      "native successful outcome",
      "native unchanged outcome when applicable",
      "read-only computed preparation"
    ],
    "variant_applicability": {
      "success": "ok or changed",
      "unchanged": "native no-op only; query/preparation/history empty are not successful unchanged",
      "computed-dry-run": "successful read-only preflight; no retention, publication, Undo or Redo change"
    },
    "success_rows": [
      {
        "native_branch": "native successful outcome",
        "code": null,
        "retryable": false,
        "mutation_state": "session-only",
        "data_variant": "success",
        "evidence_route": "P9:selection.set:native:native successful outcome",
        "evidence_status": "required-unimplemented"
      },
      {
        "native_branch": "native unchanged outcome when applicable",
        "code": null,
        "retryable": false,
        "mutation_state": "none",
        "data_variant": "unchanged",
        "evidence_route": "P9:selection.set:native:native unchanged outcome when applicable",
        "evidence_status": "required-unimplemented"
      },
      {
        "native_branch": "read-only computed preparation",
        "code": null,
        "retryable": false,
        "mutation_state": "none",
        "data_variant": "computed-dry-run",
        "evidence_route": "P9:selection.set:native:read-only computed preparation",
        "evidence_status": "required-unimplemented"
      }
    ]
  }
})m3").as_object()};
// | `selection.clear` | none | cleared count, selection-after=[], session_revision | none | S / none / preflight / S; already empty unchanged |
TypeDescriptor const m3_1{boost::json::parse(R"m3({
  "type": "object",
  "properties": {},
  "required": [],
  "additionalProperties": false,
  "x-m3-contract": {
    "guard_domain": "session",
    "guard_required": true,
    "needs_document": true,
    "selection_mode": "explicit-root-selection",
    "target_cardinality": {
      "min": 0,
      "max": 100000
    },
    "normalization": "ordered-composite-roots; ancestor covers descendant, preserving surviving input order",
    "partial_policy": "reject-any-incompatible",
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
    "native_service": "`ObjectSet::clear`, `src/object/object-set.h:192`",
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
        "native_evidence": "`ObjectSet::clear`, `src/object/object-set.h:192`",
        "oracle": "P9:selection.clear:transport/schema:request-too-large:parse_request: request-too-large",
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
        "native_evidence": "`ObjectSet::clear`, `src/object/object-set.h:192`",
        "oracle": "P9:selection.clear:transport/schema:repeated-key:parse_request: repeated-key",
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
        "native_evidence": "`ObjectSet::clear`, `src/object/object-set.h:192`",
        "oracle": "P9:selection.clear:transport/schema:invalid-utf8:parse_request: invalid-utf8",
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
        "native_evidence": "`ObjectSet::clear`, `src/object/object-set.h:192`",
        "oracle": "P9:selection.clear:transport/schema:malformed-json:parse_request: malformed-json",
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
        "native_evidence": "`ObjectSet::clear`, `src/object/object-set.h:192`",
        "oracle": "P9:selection.clear:transport/schema:invalid-argument:raw schema/type/range/route/uniqueItems violation",
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
        "native_evidence": "`ObjectSet::clear`, `src/object/object-set.h:192`",
        "oracle": "P9:selection.clear:identity/admission:no-document:missing current document",
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
        "native_evidence": "`ObjectSet::clear`, `src/object/object-set.h:192`",
        "oracle": "P9:selection.clear:identity/admission:stale-document:document identity mismatch",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "supplied guard differs from session revision",
        "code": "stale-revision",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "supplied guard differs from session revision",
        "native_evidence": "`ObjectSet::clear`, `src/object/object-set.h:192`",
        "oracle": "P9:selection.clear:identity/admission:stale-revision:supplied guard differs from session revision",
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
        "native_evidence": "`ObjectSet::clear`, `src/object/object-set.h:192`",
        "oracle": "P9:selection.clear:identity/admission:cancelled:cancellation observed before commit",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "unexpected service exception; rollback before return",
        "code": "internal-error",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "unexpected service exception; rollback before return",
        "native_evidence": "`ObjectSet::clear`, `src/object/object-set.h:192`",
        "oracle": "P9:selection.clear:command-service:internal-error:unexpected service exception; rollback before return",
        "evidence_status": "required-unimplemented"
      }
    ],
    "success_native_branches": [
      "native successful outcome",
      "native unchanged outcome when applicable",
      "read-only computed preparation"
    ],
    "variant_applicability": {
      "success": "ok or changed",
      "unchanged": "native no-op only; query/preparation/history empty are not successful unchanged",
      "computed-dry-run": "successful read-only preflight; no retention, publication, Undo or Redo change"
    },
    "success_rows": [
      {
        "native_branch": "native successful outcome",
        "code": null,
        "retryable": false,
        "mutation_state": "session-only",
        "data_variant": "success",
        "evidence_route": "P9:selection.clear:native:native successful outcome",
        "evidence_status": "required-unimplemented"
      },
      {
        "native_branch": "native unchanged outcome when applicable",
        "code": null,
        "retryable": false,
        "mutation_state": "none",
        "data_variant": "unchanged",
        "evidence_route": "P9:selection.clear:native:native unchanged outcome when applicable",
        "evidence_status": "required-unimplemented"
      },
      {
        "native_branch": "read-only computed preparation",
        "code": null,
        "retryable": false,
        "mutation_state": "none",
        "data_variant": "computed-dry-run",
        "evidence_route": "P9:selection.clear:native:read-only computed preparation",
        "evidence_status": "required-unimplemented"
      }
    ]
  }
})m3").as_object()};
// | `history.query` | none | can-undo/can-redo, next labels, document revision; no mutable history pointers | none | Q / none / same query / Q |
TypeDescriptor const m3_2{boost::json::parse(R"m3({
  "type": "object",
  "properties": {},
  "required": [],
  "additionalProperties": false,
  "x-m3-contract": {
    "guard_domain": "document",
    "guard_required": false,
    "needs_document": true,
    "selection_mode": "history-state",
    "target_cardinality": {
      "min": 0,
      "max": 0
    },
    "normalization": "none",
    "partial_policy": "not-applicable",
    "role_order": "none",
    "normalization_order": [
      "validate raw schema and exclusive route",
      "select route",
      "fill defaults recursively only in selected branch; pivot suppresses anchor, size suppresses dpi, token suppresses recipe/analyze/solve",
      "parse exact integers",
      "convert all lengths including nested recipes to CSS px and enforce bounds",
      "resolve and normalize targets",
      "canonicalize normalized params for token binding"
    ],
    "native_service": "`SPDocument::get_event_log`, `src/document.h:195`; EventLog accessors `src/event-log.h:90-96`",
    "history_snapshot": "Read-only EventLog value snapshot; one coalesced row is one next Undo/Redo operation; labels nullable at empty ends; busy is refusal; never execute Undo to query.",
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
        "native_evidence": "`SPDocument::get_event_log`, `src/document.h:195`; EventLog accessors `src/event-log.h:90-96`",
        "oracle": "P9:history.query:transport/schema:request-too-large:parse_request: request-too-large",
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
        "native_evidence": "`SPDocument::get_event_log`, `src/document.h:195`; EventLog accessors `src/event-log.h:90-96`",
        "oracle": "P9:history.query:transport/schema:repeated-key:parse_request: repeated-key",
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
        "native_evidence": "`SPDocument::get_event_log`, `src/document.h:195`; EventLog accessors `src/event-log.h:90-96`",
        "oracle": "P9:history.query:transport/schema:invalid-utf8:parse_request: invalid-utf8",
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
        "native_evidence": "`SPDocument::get_event_log`, `src/document.h:195`; EventLog accessors `src/event-log.h:90-96`",
        "oracle": "P9:history.query:transport/schema:malformed-json:parse_request: malformed-json",
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
        "native_evidence": "`SPDocument::get_event_log`, `src/document.h:195`; EventLog accessors `src/event-log.h:90-96`",
        "oracle": "P9:history.query:transport/schema:invalid-argument:raw schema/type/range/route/uniqueItems violation",
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
        "native_evidence": "`SPDocument::get_event_log`, `src/document.h:195`; EventLog accessors `src/event-log.h:90-96`",
        "oracle": "P9:history.query:identity/admission:no-document:missing current document",
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
        "native_evidence": "`SPDocument::get_event_log`, `src/document.h:195`; EventLog accessors `src/event-log.h:90-96`",
        "oracle": "P9:history.query:identity/admission:stale-document:document identity mismatch",
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
        "native_evidence": "`SPDocument::get_event_log`, `src/document.h:195`; EventLog accessors `src/event-log.h:90-96`",
        "oracle": "P9:history.query:identity/admission:stale-revision:supplied guard differs from document revision",
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
        "native_evidence": "`SPDocument::get_event_log`, `src/document.h:195`; EventLog accessors `src/event-log.h:90-96`",
        "oracle": "P9:history.query:identity/admission:cancelled:cancellation observed before commit",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "EventLog snapshot: closing or interaction busy",
        "code": "document-busy",
        "retryable": true,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "EventLog snapshot",
        "native_evidence": "`SPDocument::get_event_log`, `src/document.h:195`; EventLog accessors `src/event-log.h:90-96`",
        "oracle": "P9:history.query:command-service:document-busy:EventLog snapshot: closing or interaction busy",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "unexpected service exception; rollback before return",
        "code": "internal-error",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "unexpected service exception; rollback before return",
        "native_evidence": "`SPDocument::get_event_log`, `src/document.h:195`; EventLog accessors `src/event-log.h:90-96`",
        "oracle": "P9:history.query:command-service:internal-error:unexpected service exception; rollback before return",
        "evidence_status": "required-unimplemented"
      }
    ],
    "success_native_branches": [
      "native successful outcome",
      "native unchanged outcome when applicable",
      "read-only computed preparation"
    ],
    "variant_applicability": {
      "success": "ok or changed",
      "unchanged": "native no-op only; query/preparation/history empty are not successful unchanged",
      "computed-dry-run": "successful read-only preflight; no retention, publication, Undo or Redo change"
    },
    "success_rows": [
      {
        "native_branch": "native successful outcome",
        "code": null,
        "retryable": false,
        "mutation_state": "none",
        "data_variant": "success",
        "evidence_route": "P9:history.query:native:native successful outcome",
        "evidence_status": "required-unimplemented"
      },
      {
        "native_branch": "native unchanged outcome when applicable",
        "code": null,
        "retryable": false,
        "mutation_state": "none",
        "data_variant": "unchanged",
        "evidence_route": "P9:history.query:native:native unchanged outcome when applicable",
        "evidence_status": "required-unimplemented"
      },
      {
        "native_branch": "read-only computed preparation",
        "code": null,
        "retryable": false,
        "mutation_state": "none",
        "data_variant": "computed-dry-run",
        "evidence_route": "P9:history.query:native:read-only computed preparation",
        "evidence_status": "required-unimplemented"
      }
    ]
  }
})m3").as_object()};
// | `history.undo` | none; exactly one step | consumed label, availability, revision, selection-after | history-empty | H / undo / preflight, never executes live Undo / S(history) |
TypeDescriptor const m3_3{boost::json::parse(R"m3({
  "type": "object",
  "properties": {},
  "required": [],
  "additionalProperties": false,
  "x-m3-contract": {
    "guard_domain": "document",
    "guard_required": true,
    "needs_document": true,
    "selection_mode": "history-state",
    "target_cardinality": {
      "min": 0,
      "max": 0
    },
    "normalization": "none",
    "partial_policy": "not-applicable",
    "role_order": "none",
    "normalization_order": [
      "validate raw schema and exclusive route",
      "select route",
      "fill defaults recursively only in selected branch; pivot suppresses anchor, size suppresses dpi, token suppresses recipe/analyze/solve",
      "parse exact integers",
      "convert all lengths including nested recipes to CSS px and enforce bounds",
      "resolve and normalize targets",
      "canonicalize normalized params for token binding"
    ],
    "native_service": "`DocumentUndo::undo`, `src/document-undo.h:223`; existing wrapper `actions-vacards-cli.cpp:208`",
    "history_snapshot": "Read-only EventLog value snapshot; one coalesced row is one next Undo/Redo operation; labels nullable at empty ends; busy is refusal; never execute Undo to query.",
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
        "native_evidence": "`DocumentUndo::undo`, `src/document-undo.h:223`; existing wrapper `actions-vacards-cli.cpp:208`",
        "oracle": "P9:history.undo:transport/schema:request-too-large:parse_request: request-too-large",
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
        "native_evidence": "`DocumentUndo::undo`, `src/document-undo.h:223`; existing wrapper `actions-vacards-cli.cpp:208`",
        "oracle": "P9:history.undo:transport/schema:repeated-key:parse_request: repeated-key",
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
        "native_evidence": "`DocumentUndo::undo`, `src/document-undo.h:223`; existing wrapper `actions-vacards-cli.cpp:208`",
        "oracle": "P9:history.undo:transport/schema:invalid-utf8:parse_request: invalid-utf8",
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
        "native_evidence": "`DocumentUndo::undo`, `src/document-undo.h:223`; existing wrapper `actions-vacards-cli.cpp:208`",
        "oracle": "P9:history.undo:transport/schema:malformed-json:parse_request: malformed-json",
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
        "native_evidence": "`DocumentUndo::undo`, `src/document-undo.h:223`; existing wrapper `actions-vacards-cli.cpp:208`",
        "oracle": "P9:history.undo:transport/schema:invalid-argument:raw schema/type/range/route/uniqueItems violation",
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
        "native_evidence": "`DocumentUndo::undo`, `src/document-undo.h:223`; existing wrapper `actions-vacards-cli.cpp:208`",
        "oracle": "P9:history.undo:identity/admission:no-document:missing current document",
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
        "native_evidence": "`DocumentUndo::undo`, `src/document-undo.h:223`; existing wrapper `actions-vacards-cli.cpp:208`",
        "oracle": "P9:history.undo:identity/admission:stale-document:document identity mismatch",
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
        "native_evidence": "`DocumentUndo::undo`, `src/document-undo.h:223`; existing wrapper `actions-vacards-cli.cpp:208`",
        "oracle": "P9:history.undo:identity/admission:stale-revision:supplied guard differs from document revision",
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
        "native_evidence": "`DocumentUndo::undo`, `src/document-undo.h:223`; existing wrapper `actions-vacards-cli.cpp:208`",
        "oracle": "P9:history.undo:identity/admission:cancelled:cancellation observed before commit",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "EventLog: no undo row",
        "code": "history-empty",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "EventLog",
        "native_evidence": "`DocumentUndo::undo`, `src/document-undo.h:223`; existing wrapper `actions-vacards-cli.cpp:208`",
        "oracle": "P9:history.undo:command-service:history-empty:EventLog: no undo row",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "DocumentUndo admission busy",
        "code": "document-busy",
        "retryable": true,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "DocumentUndo admission busy",
        "native_evidence": "`DocumentUndo::undo`, `src/document-undo.h:223`; existing wrapper `actions-vacards-cli.cpp:208`",
        "oracle": "P9:history.undo:command-service:document-busy:DocumentUndo admission busy",
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
        "native_evidence": "`DocumentUndo::undo`, `src/document-undo.h:223`; existing wrapper `actions-vacards-cli.cpp:208`",
        "oracle": "P9:history.undo:identity/admission:document-read-only:read-only session rejects requested document mutation",
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
        "native_evidence": "`DocumentUndo::undo`, `src/document-undo.h:223`; existing wrapper `actions-vacards-cli.cpp:208`",
        "oracle": "P9:history.undo:command-service:internal-error:unexpected service exception; rollback before return",
        "evidence_status": "required-unimplemented"
      }
    ],
    "success_native_branches": [
      "native successful outcome",
      "native unchanged outcome when applicable",
      "read-only computed preparation"
    ],
    "variant_applicability": {
      "success": "ok or changed",
      "unchanged": "native no-op only; query/preparation/history empty are not successful unchanged",
      "computed-dry-run": "successful read-only preflight; no retention, publication, Undo or Redo change"
    },
    "success_rows": [
      {
        "native_branch": "native successful outcome",
        "code": null,
        "retryable": false,
        "mutation_state": "committed",
        "data_variant": "success",
        "evidence_route": "P9:history.undo:native:native successful outcome",
        "evidence_status": "required-unimplemented"
      },
      {
        "native_branch": "native unchanged outcome when applicable",
        "code": null,
        "retryable": false,
        "mutation_state": "none",
        "data_variant": "unchanged",
        "evidence_route": "P9:history.undo:native:native unchanged outcome when applicable",
        "evidence_status": "required-unimplemented"
      },
      {
        "native_branch": "read-only computed preparation",
        "code": null,
        "retryable": false,
        "mutation_state": "none",
        "data_variant": "computed-dry-run",
        "evidence_route": "P9:history.undo:native:read-only computed preparation",
        "evidence_status": "required-unimplemented"
      }
    ]
  }
})m3").as_object()};
// | `history.redo` | none; exactly one step | consumed label, availability, revision, selection-after | history-empty | H / redo / preflight / S(history) |
TypeDescriptor const m3_4{boost::json::parse(R"m3({
  "type": "object",
  "properties": {},
  "required": [],
  "additionalProperties": false,
  "x-m3-contract": {
    "guard_domain": "document",
    "guard_required": true,
    "needs_document": true,
    "selection_mode": "history-state",
    "target_cardinality": {
      "min": 0,
      "max": 0
    },
    "normalization": "none",
    "partial_policy": "not-applicable",
    "role_order": "none",
    "normalization_order": [
      "validate raw schema and exclusive route",
      "select route",
      "fill defaults recursively only in selected branch; pivot suppresses anchor, size suppresses dpi, token suppresses recipe/analyze/solve",
      "parse exact integers",
      "convert all lengths including nested recipes to CSS px and enforce bounds",
      "resolve and normalize targets",
      "canonicalize normalized params for token binding"
    ],
    "native_service": "`DocumentUndo::redo`, `src/document-undo.h:240`; same wrapper",
    "history_snapshot": "Read-only EventLog value snapshot; one coalesced row is one next Undo/Redo operation; labels nullable at empty ends; busy is refusal; never execute Undo to query.",
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
        "native_evidence": "`DocumentUndo::redo`, `src/document-undo.h:240`; same wrapper",
        "oracle": "P9:history.redo:transport/schema:request-too-large:parse_request: request-too-large",
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
        "native_evidence": "`DocumentUndo::redo`, `src/document-undo.h:240`; same wrapper",
        "oracle": "P9:history.redo:transport/schema:repeated-key:parse_request: repeated-key",
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
        "native_evidence": "`DocumentUndo::redo`, `src/document-undo.h:240`; same wrapper",
        "oracle": "P9:history.redo:transport/schema:invalid-utf8:parse_request: invalid-utf8",
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
        "native_evidence": "`DocumentUndo::redo`, `src/document-undo.h:240`; same wrapper",
        "oracle": "P9:history.redo:transport/schema:malformed-json:parse_request: malformed-json",
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
        "native_evidence": "`DocumentUndo::redo`, `src/document-undo.h:240`; same wrapper",
        "oracle": "P9:history.redo:transport/schema:invalid-argument:raw schema/type/range/route/uniqueItems violation",
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
        "native_evidence": "`DocumentUndo::redo`, `src/document-undo.h:240`; same wrapper",
        "oracle": "P9:history.redo:identity/admission:no-document:missing current document",
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
        "native_evidence": "`DocumentUndo::redo`, `src/document-undo.h:240`; same wrapper",
        "oracle": "P9:history.redo:identity/admission:stale-document:document identity mismatch",
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
        "native_evidence": "`DocumentUndo::redo`, `src/document-undo.h:240`; same wrapper",
        "oracle": "P9:history.redo:identity/admission:stale-revision:supplied guard differs from document revision",
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
        "native_evidence": "`DocumentUndo::redo`, `src/document-undo.h:240`; same wrapper",
        "oracle": "P9:history.redo:identity/admission:cancelled:cancellation observed before commit",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "EventLog: no redo row",
        "code": "history-empty",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "EventLog",
        "native_evidence": "`DocumentUndo::redo`, `src/document-undo.h:240`; same wrapper",
        "oracle": "P9:history.redo:command-service:history-empty:EventLog: no redo row",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "DocumentUndo admission busy",
        "code": "document-busy",
        "retryable": true,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "DocumentUndo admission busy",
        "native_evidence": "`DocumentUndo::redo`, `src/document-undo.h:240`; same wrapper",
        "oracle": "P9:history.redo:command-service:document-busy:DocumentUndo admission busy",
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
        "native_evidence": "`DocumentUndo::redo`, `src/document-undo.h:240`; same wrapper",
        "oracle": "P9:history.redo:identity/admission:document-read-only:read-only session rejects requested document mutation",
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
        "native_evidence": "`DocumentUndo::redo`, `src/document-undo.h:240`; same wrapper",
        "oracle": "P9:history.redo:command-service:internal-error:unexpected service exception; rollback before return",
        "evidence_status": "required-unimplemented"
      }
    ],
    "success_native_branches": [
      "native successful outcome",
      "native unchanged outcome when applicable",
      "read-only computed preparation"
    ],
    "variant_applicability": {
      "success": "ok or changed",
      "unchanged": "native no-op only; query/preparation/history empty are not successful unchanged",
      "computed-dry-run": "successful read-only preflight; no retention, publication, Undo or Redo change"
    },
    "success_rows": [
      {
        "native_branch": "native successful outcome",
        "code": null,
        "retryable": false,
        "mutation_state": "committed",
        "data_variant": "success",
        "evidence_route": "P9:history.redo:native:native successful outcome",
        "evidence_status": "required-unimplemented"
      },
      {
        "native_branch": "native unchanged outcome when applicable",
        "code": null,
        "retryable": false,
        "mutation_state": "none",
        "data_variant": "unchanged",
        "evidence_route": "P9:history.redo:native:native unchanged outcome when applicable",
        "evidence_status": "required-unimplemented"
      },
      {
        "native_branch": "read-only computed preparation",
        "code": null,
        "retryable": false,
        "mutation_state": "none",
        "data_variant": "computed-dry-run",
        "evidence_route": "P9:history.redo:native:read-only computed preparation",
        "evidence_status": "required-unimplemented"
      }
    ]
  }
})m3").as_object()};
}
std::vector<PackageCommand> selection_commands()
{
    std::vector<PackageCommand> out;
    out.push_back({ActionSpec{.name="selection.set", .mode="S", .summary="Set the current selection to the requested objects.", .canonical_id="selection.set", .handler=production_unavailable_action, .version=2, .effects="session-state", .target_policy="S", .undo_policy="none", .dry_run_grade="computed", .cancellation_boundary="before-native-publication", .input=&m3_0}, "selection.set", "session-state", "S", {}, {{"type","object"},{"properties",boost::json::object{}},{"additionalProperties",false}}, {"slice-unavailable"}, {}});
    out.push_back({ActionSpec{.name="selection.clear", .mode="S", .summary="Clear the current selection.", .canonical_id="selection.clear", .handler=production_unavailable_action, .version=2, .effects="session-state", .target_policy="S", .undo_policy="none", .dry_run_grade="preflight", .cancellation_boundary="before-native-publication", .input=&m3_1}, "selection.clear", "session-state", "S", {}, {{"type","object"},{"properties",boost::json::object{}},{"additionalProperties",false}}, {"slice-unavailable"}, {}});
    out.push_back({ActionSpec{.name="history.query", .mode="Q", .summary="Query the document undo and redo history.", .canonical_id="history.query", .handler=production_unavailable_action, .version=2, .effects="read-only", .target_policy="Q", .undo_policy="none", .dry_run_grade="computed", .cancellation_boundary="before-native-publication", .input=&m3_2}, "history.query", "read-only", "Q", {}, {{"type","object"},{"properties",boost::json::object{}},{"additionalProperties",false}}, {"slice-unavailable"}, {}});
    out.push_back({ActionSpec{.name="history.undo", .mode="S", .summary="Undo the most recent document change.", .canonical_id="history.undo", .handler=production_unavailable_action, .version=2, .effects="history", .target_policy="S", .undo_policy="undo", .dry_run_grade="preflight", .cancellation_boundary="before-native-publication", .input=&m3_3}, "history.undo", "history", "S", {}, {{"type","object"},{"properties",boost::json::object{}},{"additionalProperties",false}}, {"slice-unavailable"}, {}});
    out.push_back({ActionSpec{.name="history.redo", .mode="S", .summary="Redo the most recently undone document change.", .canonical_id="history.redo", .handler=production_unavailable_action, .version=2, .effects="history", .target_policy="S", .undo_policy="redo", .dry_run_grade="preflight", .cancellation_boundary="before-native-publication", .input=&m3_4}, "history.redo", "history", "S", {}, {{"type","object"},{"properties",boost::json::object{}},{"additionalProperties",false}}, {"slice-unavailable"}, {}});
    out[0].example = boost::json::parse(R"m3({
  "ids": [
    "shape1"
  ]
})m3").as_object();
    out[0].result_data = boost::json::parse(R"m3({
  "oneOf": [
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "success"
        },
        "selection-before": {
          "type": "array",
          "items": {
            "type": "string"
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "selection-after": {
          "type": "array",
          "items": {
            "type": "string"
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "session-revision": {
          "type": "integer",
          "minimum": 0
        },
        "normalized-ids": {
          "type": "array",
          "items": {
            "type": "string"
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "covered-ids": {
          "type": "array",
          "items": {
            "type": "string"
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "exclusions": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "id": {
                "type": "string"
              },
              "reason": {
                "type": "string"
              }
            },
            "required": [
              "id",
              "reason"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 100000
        }
      },
      "required": [
        "variant",
        "selection-before",
        "selection-after",
        "session-revision",
        "normalized-ids",
        "covered-ids",
        "exclusions"
      ],
      "additionalProperties": false
    },
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "unchanged"
        },
        "selection-before": {
          "type": "array",
          "items": {
            "type": "string"
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "selection-after": {
          "type": "array",
          "items": {
            "type": "string"
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "session-revision": {
          "type": "integer",
          "minimum": 0
        },
        "normalized-ids": {
          "type": "array",
          "items": {
            "type": "string"
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "covered-ids": {
          "type": "array",
          "items": {
            "type": "string"
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "exclusions": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "id": {
                "type": "string"
              },
              "reason": {
                "type": "string"
              }
            },
            "required": [
              "id",
              "reason"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 100000
        }
      },
      "required": [
        "variant",
        "selection-before",
        "selection-after",
        "session-revision",
        "normalized-ids",
        "covered-ids",
        "exclusions"
      ],
      "additionalProperties": false
    },
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "computed-dry-run"
        },
        "selection-before": {
          "type": "array",
          "items": {
            "type": [
              "string",
              "null"
            ],
            "description": "null for prospective output; existing source IDs remain strings"
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "selection-after": {
          "type": "array",
          "items": {
            "type": [
              "string",
              "null"
            ],
            "description": "null for prospective output; existing source IDs remain strings"
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "session-revision": {
          "type": "integer",
          "minimum": 0
        },
        "normalized-ids": {
          "type": "array",
          "items": {
            "type": [
              "string",
              "null"
            ],
            "description": "null for prospective output; existing source IDs remain strings"
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "covered-ids": {
          "type": "array",
          "items": {
            "type": [
              "string",
              "null"
            ],
            "description": "null for prospective output; existing source IDs remain strings"
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "exclusions": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "id": {
                "type": [
                  "string",
                  "null"
                ],
                "description": "null for prospective output; existing source IDs remain strings"
              },
              "reason": {
                "type": [
                  "string",
                  "null"
                ],
                "description": "null for prospective output; existing source IDs remain strings"
              }
            },
            "required": [
              "id",
              "reason"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 100000
        }
      },
      "required": [
        "variant",
        "selection-before",
        "selection-after",
        "session-revision",
        "normalized-ids",
        "covered-ids",
        "exclusions"
      ],
      "additionalProperties": false
    }
  ],
  "description": "Data only; common va-studio.cli-result/1 envelope authoritative. Variant selected by status/dry_run; any duplicated fields must equal envelope. Preview IDs never claim live publication."
})m3").as_object();
    out[0].errors = {"request-too-large","repeated-key","invalid-utf8","malformed-json","invalid-argument","no-document","stale-document","stale-revision","cancelled","unknown-id","unavailable","internal-error"};
    out[1].example = boost::json::parse(R"m3({})m3").as_object();
    out[1].result_data = boost::json::parse(R"m3({
  "oneOf": [
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "success"
        },
        "selection-before": {
          "type": "array",
          "items": {
            "type": "string"
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "selection-after": {
          "type": "array",
          "items": {
            "type": "string"
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "session-revision": {
          "type": "integer",
          "minimum": 0
        },
        "cleared-count": {
          "type": "integer",
          "minimum": 0
        }
      },
      "required": [
        "variant",
        "selection-before",
        "selection-after",
        "session-revision",
        "cleared-count"
      ],
      "additionalProperties": false
    },
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "unchanged"
        },
        "selection-before": {
          "type": "array",
          "items": {
            "type": "string"
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "selection-after": {
          "type": "array",
          "items": {
            "type": "string"
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "session-revision": {
          "type": "integer",
          "minimum": 0
        },
        "cleared-count": {
          "type": "integer",
          "minimum": 0
        }
      },
      "required": [
        "variant",
        "selection-before",
        "selection-after",
        "session-revision",
        "cleared-count"
      ],
      "additionalProperties": false
    },
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "computed-dry-run"
        },
        "selection-before": {
          "type": "array",
          "items": {
            "type": "string"
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "selection-after": {
          "type": "array",
          "items": {
            "type": "string"
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "session-revision": {
          "type": "integer",
          "minimum": 0
        },
        "cleared-count": {
          "type": "integer",
          "minimum": 0
        }
      },
      "required": [
        "variant",
        "selection-before",
        "selection-after",
        "session-revision",
        "cleared-count"
      ],
      "additionalProperties": false
    }
  ],
  "description": "Data only; common va-studio.cli-result/1 envelope authoritative. Variant selected by status/dry_run; any duplicated fields must equal envelope. Preview IDs never claim live publication."
})m3").as_object();
    out[1].errors = {"request-too-large","repeated-key","invalid-utf8","malformed-json","invalid-argument","no-document","stale-document","stale-revision","cancelled","internal-error"};
    out[2].example = boost::json::parse(R"m3({})m3").as_object();
    out[2].result_data = boost::json::parse(R"m3({
  "oneOf": [
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "success"
        },
        "can-undo": {
          "type": "boolean"
        },
        "can-redo": {
          "type": "boolean"
        },
        "next-undo-label": {
          "type": [
            "string",
            "null"
          ]
        },
        "next-redo-label": {
          "type": [
            "string",
            "null"
          ]
        }
      },
      "required": [
        "variant",
        "can-undo",
        "can-redo",
        "next-undo-label",
        "next-redo-label"
      ],
      "additionalProperties": false
    },
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "unchanged"
        },
        "can-undo": {
          "type": "boolean"
        },
        "can-redo": {
          "type": "boolean"
        },
        "next-undo-label": {
          "type": [
            "string",
            "null"
          ]
        },
        "next-redo-label": {
          "type": [
            "string",
            "null"
          ]
        }
      },
      "required": [
        "variant",
        "can-undo",
        "can-redo",
        "next-undo-label",
        "next-redo-label"
      ],
      "additionalProperties": false
    },
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "computed-dry-run"
        },
        "can-undo": {
          "type": "boolean"
        },
        "can-redo": {
          "type": "boolean"
        },
        "next-undo-label": {
          "type": [
            "string",
            "null"
          ]
        },
        "next-redo-label": {
          "type": [
            "string",
            "null"
          ]
        }
      },
      "required": [
        "variant",
        "can-undo",
        "can-redo",
        "next-undo-label",
        "next-redo-label"
      ],
      "additionalProperties": false
    }
  ],
  "description": "Data only; common va-studio.cli-result/1 envelope authoritative. Variant selected by status/dry_run; any duplicated fields must equal envelope. Preview IDs never claim live publication."
})m3").as_object();
    out[2].errors = {"request-too-large","repeated-key","invalid-utf8","malformed-json","invalid-argument","no-document","stale-document","stale-revision","cancelled","document-busy","internal-error"};
    out[3].example = boost::json::parse(R"m3({})m3").as_object();
    out[3].result_data = boost::json::parse(R"m3({
  "oneOf": [
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "success"
        },
        "can-undo": {
          "type": "boolean"
        },
        "can-redo": {
          "type": "boolean"
        },
        "next-undo-label": {
          "type": [
            "string",
            "null"
          ]
        },
        "next-redo-label": {
          "type": [
            "string",
            "null"
          ]
        },
        "consumed-label": {
          "type": "string"
        },
        "selection-after": {
          "type": "array",
          "items": {
            "type": "string"
          },
          "minItems": 0,
          "maxItems": 100000
        }
      },
      "required": [
        "variant",
        "can-undo",
        "can-redo",
        "next-undo-label",
        "next-redo-label",
        "consumed-label",
        "selection-after"
      ],
      "additionalProperties": false
    },
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "unchanged"
        },
        "can-undo": {
          "type": "boolean"
        },
        "can-redo": {
          "type": "boolean"
        },
        "next-undo-label": {
          "type": [
            "string",
            "null"
          ]
        },
        "next-redo-label": {
          "type": [
            "string",
            "null"
          ]
        },
        "selection-after": {
          "type": "array",
          "items": {
            "type": "string"
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "would-undo": {
          "type": "boolean"
        }
      },
      "required": [
        "variant",
        "can-undo",
        "can-redo",
        "next-undo-label",
        "next-redo-label",
        "selection-after",
        "would-undo"
      ],
      "additionalProperties": false
    },
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "computed-dry-run"
        },
        "can-undo": {
          "type": "boolean"
        },
        "can-redo": {
          "type": "boolean"
        },
        "next-undo-label": {
          "type": [
            "string",
            "null"
          ]
        },
        "next-redo-label": {
          "type": [
            "string",
            "null"
          ]
        },
        "selection-after": {
          "type": "array",
          "items": {
            "type": "string"
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "would-undo": {
          "type": "boolean"
        }
      },
      "required": [
        "variant",
        "can-undo",
        "can-redo",
        "next-undo-label",
        "next-redo-label",
        "selection-after",
        "would-undo"
      ],
      "additionalProperties": false
    }
  ],
  "description": "Data only; common va-studio.cli-result/1 envelope authoritative. Variant selected by status/dry_run; any duplicated fields must equal envelope. Preview IDs never claim live publication."
})m3").as_object();
    out[3].errors = {"request-too-large","repeated-key","invalid-utf8","malformed-json","invalid-argument","no-document","stale-document","stale-revision","cancelled","history-empty","document-busy","document-read-only","internal-error"};
    out[4].example = boost::json::parse(R"m3({})m3").as_object();
    out[4].result_data = boost::json::parse(R"m3({
  "oneOf": [
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "success"
        },
        "can-undo": {
          "type": "boolean"
        },
        "can-redo": {
          "type": "boolean"
        },
        "next-undo-label": {
          "type": [
            "string",
            "null"
          ]
        },
        "next-redo-label": {
          "type": [
            "string",
            "null"
          ]
        },
        "consumed-label": {
          "type": "string"
        },
        "selection-after": {
          "type": "array",
          "items": {
            "type": "string"
          },
          "minItems": 0,
          "maxItems": 100000
        }
      },
      "required": [
        "variant",
        "can-undo",
        "can-redo",
        "next-undo-label",
        "next-redo-label",
        "consumed-label",
        "selection-after"
      ],
      "additionalProperties": false
    },
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "unchanged"
        },
        "can-undo": {
          "type": "boolean"
        },
        "can-redo": {
          "type": "boolean"
        },
        "next-undo-label": {
          "type": [
            "string",
            "null"
          ]
        },
        "next-redo-label": {
          "type": [
            "string",
            "null"
          ]
        },
        "selection-after": {
          "type": "array",
          "items": {
            "type": "string"
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "would-redo": {
          "type": "boolean"
        }
      },
      "required": [
        "variant",
        "can-undo",
        "can-redo",
        "next-undo-label",
        "next-redo-label",
        "selection-after",
        "would-redo"
      ],
      "additionalProperties": false
    },
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "computed-dry-run"
        },
        "can-undo": {
          "type": "boolean"
        },
        "can-redo": {
          "type": "boolean"
        },
        "next-undo-label": {
          "type": [
            "string",
            "null"
          ]
        },
        "next-redo-label": {
          "type": [
            "string",
            "null"
          ]
        },
        "selection-after": {
          "type": "array",
          "items": {
            "type": "string"
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "would-redo": {
          "type": "boolean"
        }
      },
      "required": [
        "variant",
        "can-undo",
        "can-redo",
        "next-undo-label",
        "next-redo-label",
        "selection-after",
        "would-redo"
      ],
      "additionalProperties": false
    }
  ],
  "description": "Data only; common va-studio.cli-result/1 envelope authoritative. Variant selected by status/dry_run; any duplicated fields must equal envelope. Preview IDs never claim live publication."
})m3").as_object();
    out[4].errors = {"request-too-large","repeated-key","invalid-utf8","malformed-json","invalid-argument","no-document","stale-document","stale-revision","cancelled","history-empty","document-busy","document-read-only","internal-error"};
    out[0].policy = "explicit-root-selection"; out[0].spec.target_policy = "explicit-root-selection";
    out[1].policy = "explicit-root-selection"; out[1].spec.target_policy = "explicit-root-selection";
    out[2].policy = "history-state"; out[2].spec.target_policy = "history-state";
    out[3].policy = "history-state"; out[3].spec.target_policy = "history-state";
    out[4].policy = "history-state"; out[4].spec.target_policy = "history-state";
    out[0].warnings = {};
    out[1].warnings = {};
    out[2].warnings = {};
    out[3].warnings = {};
    out[4].warnings = {};
    return out;
}
}
