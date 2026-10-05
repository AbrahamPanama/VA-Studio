// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * VACards agent command line: nesting actions.
 */

#include "actions-vacards-nest.h"

#include "inkscape-application.h"

void add_actions_vacards_nest(InkscapeApplication *app)
{
    (void)app;
}

#include "vacards-cli-production.h"
#include <boost/json.hpp>
namespace Inkscape::VACardsCli {
namespace {
// | `nest.contour-set` | `payload-id,contour-id` distinct, same safe parent/context | binding/group IDs, contour role, root transforms | T, invalid-contour, incompatible-operands, nest-binding-failed | E / one / computed / G explicit binding; preserve payload/relative placement |
TypeDescriptor const m3_0{boost::json::parse(R"m3({
  "type": "object",
  "properties": {
    "payload-id": {
      "type": "string",
      "minLength": 1,
      "maxLength": 128
    },
    "contour-id": {
      "type": "string",
      "minLength": 1,
      "maxLength": 128
    }
  },
  "required": [
    "payload-id",
    "contour-id"
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
    "role_order": "payload then contour",
    "normalization_order": [
      "validate raw schema and exclusive route",
      "select route",
      "fill defaults recursively only in selected branch; pivot suppresses anchor, size suppresses dpi, token suppresses recipe/analyze/solve",
      "parse exact integers",
      "convert all lengths including nested recipes to CSS px and enforce bounds",
      "resolve and normalize targets",
      "canonicalize normalized params for token binding"
    ],
    "native_service": "native binding logic `selection_set_nesting_contour`, `src/actions/actions-selection-object.cpp:260`",
    "copy_policy": "zero-based; copy 0 moves source; >=1 duplicated only when placed, inside one native transaction; validate expanded count <=100000 and keys against normalized roots before allocation",
    "sheet_policy": "request-local page-index/rectangle plus page revision; immutable worker geometry separate from owner-thread weak capture; no temporary live sheet; fallback reject forbids conservative capture; Job::validate then freshness before one native commit",
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
        "native_evidence": "native binding logic `selection_set_nesting_contour`, `src/actions/actions-selection-object.cpp:260`",
        "oracle": "P9:nest.contour-set:transport/schema:request-too-large:parse_request: request-too-large",
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
        "native_evidence": "native binding logic `selection_set_nesting_contour`, `src/actions/actions-selection-object.cpp:260`",
        "oracle": "P9:nest.contour-set:transport/schema:repeated-key:parse_request: repeated-key",
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
        "native_evidence": "native binding logic `selection_set_nesting_contour`, `src/actions/actions-selection-object.cpp:260`",
        "oracle": "P9:nest.contour-set:transport/schema:invalid-utf8:parse_request: invalid-utf8",
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
        "native_evidence": "native binding logic `selection_set_nesting_contour`, `src/actions/actions-selection-object.cpp:260`",
        "oracle": "P9:nest.contour-set:transport/schema:malformed-json:parse_request: malformed-json",
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
        "native_evidence": "native binding logic `selection_set_nesting_contour`, `src/actions/actions-selection-object.cpp:260`",
        "oracle": "P9:nest.contour-set:transport/schema:invalid-argument:raw schema/type/range/route/uniqueItems violation",
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
        "native_evidence": "native binding logic `selection_set_nesting_contour`, `src/actions/actions-selection-object.cpp:260`",
        "oracle": "P9:nest.contour-set:identity/admission:no-document:missing current document",
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
        "native_evidence": "native binding logic `selection_set_nesting_contour`, `src/actions/actions-selection-object.cpp:260`",
        "oracle": "P9:nest.contour-set:identity/admission:stale-document:document identity mismatch",
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
        "native_evidence": "native binding logic `selection_set_nesting_contour`, `src/actions/actions-selection-object.cpp:260`",
        "oracle": "P9:nest.contour-set:identity/admission:stale-revision:supplied guard differs from document revision",
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
        "native_evidence": "native binding logic `selection_set_nesting_contour`, `src/actions/actions-selection-object.cpp:260`",
        "oracle": "P9:nest.contour-set:identity/admission:cancelled:cancellation observed before commit",
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
        "native_evidence": "native binding logic `selection_set_nesting_contour`, `src/actions/actions-selection-object.cpp:260`",
        "oracle": "P9:nest.contour-set:identity/admission:unknown-id:explicit ID cannot resolve",
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
        "native_evidence": "native binding logic `selection_set_nesting_contour`, `src/actions/actions-selection-object.cpp:260`",
        "oracle": "P9:nest.contour-set:identity/admission:transaction-unavailable:EditTransaction inactive",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "payload and contour roles equal",
        "code": "duplicate-id",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "payload and contour roles equal",
        "native_evidence": "native binding logic `selection_set_nesting_contour`, `src/actions/actions-selection-object.cpp:260`",
        "oracle": "P9:nest.contour-set:command-service:duplicate-id:payload and contour roles equal",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "binding geometry unsupported",
        "code": "unsupported-target",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "binding geometry unsupported",
        "native_evidence": "native binding logic `selection_set_nesting_contour`, `src/actions/actions-selection-object.cpp:260`",
        "oracle": "P9:nest.contour-set:command-service:unsupported-target:binding geometry unsupported",
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
        "native_evidence": "native binding logic `selection_set_nesting_contour`, `src/actions/actions-selection-object.cpp:260`",
        "oracle": "P9:nest.contour-set:identity/admission:document-read-only:read-only session rejects requested document mutation",
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
        "native_evidence": "native binding logic `selection_set_nesting_contour`, `src/actions/actions-selection-object.cpp:260`",
        "oracle": "P9:nest.contour-set:command-service:internal-error:unexpected service exception; rollback before return",
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
        "evidence_route": "P9:nest.contour-set:native:native successful outcome",
        "evidence_status": "required-unimplemented"
      },
      {
        "native_branch": "native unchanged outcome when applicable",
        "code": null,
        "retryable": false,
        "mutation_state": "none",
        "data_variant": "unchanged",
        "evidence_route": "P9:nest.contour-set:native:native unchanged outcome when applicable",
        "evidence_status": "required-unimplemented"
      },
      {
        "native_branch": "read-only computed preparation",
        "code": null,
        "retryable": false,
        "mutation_state": "none",
        "data_variant": "computed-dry-run",
        "evidence_route": "P9:nest.contour-set:native:read-only computed preparation",
        "evidence_status": "required-unimplemented"
      }
    ]
  }
})m3").as_object()};
// | `nest.contour-release` | ids of bound roots | removed binding IDs, retained payload/contour IDs, exclusions | T, invalid-contour, nest-binding-failed | E / one / computed / G per bound root; no deletion/flattening of art |
TypeDescriptor const m3_1{boost::json::parse(R"m3({
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
    "selection_mode": "collective-geometry",
    "target_cardinality": {
      "min": 1,
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
    "native_service": "`selection_release_nesting_contour`, same file `:331`",
    "copy_policy": "zero-based; copy 0 moves source; >=1 duplicated only when placed, inside one native transaction; validate expanded count <=100000 and keys against normalized roots before allocation",
    "sheet_policy": "request-local page-index/rectangle plus page revision; immutable worker geometry separate from owner-thread weak capture; no temporary live sheet; fallback reject forbids conservative capture; Job::validate then freshness before one native commit",
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
        "native_evidence": "`selection_release_nesting_contour`, same file `:331`",
        "oracle": "P9:nest.contour-release:transport/schema:request-too-large:parse_request: request-too-large",
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
        "native_evidence": "`selection_release_nesting_contour`, same file `:331`",
        "oracle": "P9:nest.contour-release:transport/schema:repeated-key:parse_request: repeated-key",
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
        "native_evidence": "`selection_release_nesting_contour`, same file `:331`",
        "oracle": "P9:nest.contour-release:transport/schema:invalid-utf8:parse_request: invalid-utf8",
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
        "native_evidence": "`selection_release_nesting_contour`, same file `:331`",
        "oracle": "P9:nest.contour-release:transport/schema:malformed-json:parse_request: malformed-json",
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
        "native_evidence": "`selection_release_nesting_contour`, same file `:331`",
        "oracle": "P9:nest.contour-release:transport/schema:invalid-argument:raw schema/type/range/route/uniqueItems violation",
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
        "native_evidence": "`selection_release_nesting_contour`, same file `:331`",
        "oracle": "P9:nest.contour-release:identity/admission:no-document:missing current document",
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
        "native_evidence": "`selection_release_nesting_contour`, same file `:331`",
        "oracle": "P9:nest.contour-release:identity/admission:stale-document:document identity mismatch",
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
        "native_evidence": "`selection_release_nesting_contour`, same file `:331`",
        "oracle": "P9:nest.contour-release:identity/admission:stale-revision:supplied guard differs from document revision",
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
        "native_evidence": "`selection_release_nesting_contour`, same file `:331`",
        "oracle": "P9:nest.contour-release:identity/admission:cancelled:cancellation observed before commit",
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
        "native_evidence": "`selection_release_nesting_contour`, same file `:331`",
        "oracle": "P9:nest.contour-release:identity/admission:unknown-id:explicit ID cannot resolve",
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
        "native_evidence": "`selection_release_nesting_contour`, same file `:331`",
        "oracle": "P9:nest.contour-release:identity/admission:transaction-unavailable:EditTransaction inactive",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "no bound root",
        "code": "no-eligible-targets",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "no bound root",
        "native_evidence": "`selection_release_nesting_contour`, same file `:331`",
        "oracle": "P9:nest.contour-release:command-service:no-eligible-targets:no bound root",
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
        "native_evidence": "`selection_release_nesting_contour`, same file `:331`",
        "oracle": "P9:nest.contour-release:identity/admission:document-read-only:read-only session rejects requested document mutation",
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
        "native_evidence": "`selection_release_nesting_contour`, same file `:331`",
        "oracle": "P9:nest.contour-release:command-service:internal-error:unexpected service exception; rollback before return",
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
        "evidence_route": "P9:nest.contour-release:native:native successful outcome",
        "evidence_status": "required-unimplemented"
      },
      {
        "native_branch": "native unchanged outcome when applicable",
        "code": null,
        "retryable": false,
        "mutation_state": "none",
        "data_variant": "unchanged",
        "evidence_route": "P9:nest.contour-release:native:native unchanged outcome when applicable",
        "evidence_status": "required-unimplemented"
      },
      {
        "native_branch": "read-only computed preparation",
        "code": null,
        "retryable": false,
        "mutation_state": "none",
        "data_variant": "computed-dry-run",
        "evidence_route": "P9:nest.contour-release:native:read-only computed preparation",
        "evidence_status": "required-unimplemented"
      }
    ]
  }
})m3").as_object()};
// | `nest.analyze` | ids; exactly one `page` (1-based) or `sheet:{x:L,y:L,width:L,height:L}`; `gap=0mm,margin=0mm`; `rotations={mode:none|right-angles|discrete|free,step-degrees?}` default none, step >0..360 only for discrete; `copies` closed per-ID integer map (1..100000 each, expanded total <=100000), omitted IDs default 1; `obstacles` ID list default []; `fallback=reject|conservative-hull|conservative-bounds` default reject; retain=true | analysis-token, source/recovery/exclusion report, requested copy counts, collision bounds, resource estimate | T, K, invalid-contour, invalid-sheet, unsupported-fallback | Q+R / none / computed transient / Q capture normalized G targets; never silent fallback |
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
    "page": {
      "type": "integer",
      "minimum": 1,
      "maximum": 10000
    },
    "sheet": {
      "type": "object",
      "properties": {
        "x": {
          "type": "object",
          "properties": {
            "value": {
              "type": "number",
              "minimum": -1000000.0,
              "maximum": 1000000.0
            },
            "unit": {
              "type": "string",
              "enum": [
                "px",
                "mm",
                "cm",
                "in",
                "pt",
                "pc"
              ]
            }
          },
          "required": [
            "value",
            "unit"
          ],
          "additionalProperties": false,
          "x-css-px-absolute-maximum": 1000000,
          "description": "Convert recursively to CSS px at 96 dpi, then enforce absolute <=1000000 and sign bound. No rounding or clamping.",
          "allOf": [
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "px"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": -1000000.0,
                    "maximum": 1000000.0
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "mm"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": -264583.3333333333,
                    "maximum": 264583.3333333333
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "cm"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": -26458.333333333336,
                    "maximum": 26458.333333333336
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "in"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": -10416.666666666666,
                    "maximum": 10416.666666666666
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "pt"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": -750000.0,
                    "maximum": 750000.0
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "pc"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": -62500.0,
                    "maximum": 62500.0
                  }
                }
              }
            }
          ]
        },
        "y": {
          "type": "object",
          "properties": {
            "value": {
              "type": "number",
              "minimum": -1000000.0,
              "maximum": 1000000.0
            },
            "unit": {
              "type": "string",
              "enum": [
                "px",
                "mm",
                "cm",
                "in",
                "pt",
                "pc"
              ]
            }
          },
          "required": [
            "value",
            "unit"
          ],
          "additionalProperties": false,
          "x-css-px-absolute-maximum": 1000000,
          "description": "Convert recursively to CSS px at 96 dpi, then enforce absolute <=1000000 and sign bound. No rounding or clamping.",
          "allOf": [
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "px"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": -1000000.0,
                    "maximum": 1000000.0
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "mm"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": -264583.3333333333,
                    "maximum": 264583.3333333333
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "cm"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": -26458.333333333336,
                    "maximum": 26458.333333333336
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "in"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": -10416.666666666666,
                    "maximum": 10416.666666666666
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "pt"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": -750000.0,
                    "maximum": 750000.0
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "pc"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": -62500.0,
                    "maximum": 62500.0
                  }
                }
              }
            }
          ]
        },
        "width": {
          "type": "object",
          "properties": {
            "value": {
              "type": "number",
              "minimum": 0,
              "maximum": 1000000.0,
              "exclusiveMinimum": 0
            },
            "unit": {
              "type": "string",
              "enum": [
                "px",
                "mm",
                "cm",
                "in",
                "pt",
                "pc"
              ]
            }
          },
          "required": [
            "value",
            "unit"
          ],
          "additionalProperties": false,
          "x-css-px-absolute-maximum": 1000000,
          "description": "Convert recursively to CSS px at 96 dpi, then enforce absolute <=1000000 and sign bound. No rounding or clamping.",
          "allOf": [
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "px"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": 0,
                    "maximum": 1000000.0
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "mm"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": 0,
                    "maximum": 264583.3333333333
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "cm"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": 0,
                    "maximum": 26458.333333333336
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "in"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": 0,
                    "maximum": 10416.666666666666
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "pt"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": 0,
                    "maximum": 750000.0
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "pc"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": 0,
                    "maximum": 62500.0
                  }
                }
              }
            }
          ]
        },
        "height": {
          "type": "object",
          "properties": {
            "value": {
              "type": "number",
              "minimum": 0,
              "maximum": 1000000.0,
              "exclusiveMinimum": 0
            },
            "unit": {
              "type": "string",
              "enum": [
                "px",
                "mm",
                "cm",
                "in",
                "pt",
                "pc"
              ]
            }
          },
          "required": [
            "value",
            "unit"
          ],
          "additionalProperties": false,
          "x-css-px-absolute-maximum": 1000000,
          "description": "Convert recursively to CSS px at 96 dpi, then enforce absolute <=1000000 and sign bound. No rounding or clamping.",
          "allOf": [
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "px"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": 0,
                    "maximum": 1000000.0
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "mm"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": 0,
                    "maximum": 264583.3333333333
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "cm"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": 0,
                    "maximum": 26458.333333333336
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "in"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": 0,
                    "maximum": 10416.666666666666
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "pt"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": 0,
                    "maximum": 750000.0
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "pc"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": 0,
                    "maximum": 62500.0
                  }
                }
              }
            }
          ]
        }
      },
      "required": [
        "x",
        "y",
        "width",
        "height"
      ],
      "additionalProperties": false
    },
    "gap": {
      "type": "object",
      "properties": {
        "value": {
          "type": "number",
          "minimum": 0,
          "maximum": 1000000.0
        },
        "unit": {
          "type": "string",
          "enum": [
            "px",
            "mm",
            "cm",
            "in",
            "pt",
            "pc"
          ]
        }
      },
      "required": [
        "value",
        "unit"
      ],
      "additionalProperties": false,
      "x-css-px-absolute-maximum": 1000000,
      "default": {
        "value": 0,
        "unit": "mm"
      },
      "description": "Convert recursively to CSS px at 96 dpi, then enforce absolute <=1000000 and sign bound. No rounding or clamping.",
      "allOf": [
        {
          "if": {
            "properties": {
              "unit": {
                "const": "px"
              }
            },
            "required": [
              "unit"
            ]
          },
          "then": {
            "properties": {
              "value": {
                "minimum": 0,
                "maximum": 1000000.0
              }
            }
          }
        },
        {
          "if": {
            "properties": {
              "unit": {
                "const": "mm"
              }
            },
            "required": [
              "unit"
            ]
          },
          "then": {
            "properties": {
              "value": {
                "minimum": 0,
                "maximum": 264583.3333333333
              }
            }
          }
        },
        {
          "if": {
            "properties": {
              "unit": {
                "const": "cm"
              }
            },
            "required": [
              "unit"
            ]
          },
          "then": {
            "properties": {
              "value": {
                "minimum": 0,
                "maximum": 26458.333333333336
              }
            }
          }
        },
        {
          "if": {
            "properties": {
              "unit": {
                "const": "in"
              }
            },
            "required": [
              "unit"
            ]
          },
          "then": {
            "properties": {
              "value": {
                "minimum": 0,
                "maximum": 10416.666666666666
              }
            }
          }
        },
        {
          "if": {
            "properties": {
              "unit": {
                "const": "pt"
              }
            },
            "required": [
              "unit"
            ]
          },
          "then": {
            "properties": {
              "value": {
                "minimum": 0,
                "maximum": 750000.0
              }
            }
          }
        },
        {
          "if": {
            "properties": {
              "unit": {
                "const": "pc"
              }
            },
            "required": [
              "unit"
            ]
          },
          "then": {
            "properties": {
              "value": {
                "minimum": 0,
                "maximum": 62500.0
              }
            }
          }
        }
      ]
    },
    "margin": {
      "type": "object",
      "properties": {
        "value": {
          "type": "number",
          "minimum": 0,
          "maximum": 1000000.0
        },
        "unit": {
          "type": "string",
          "enum": [
            "px",
            "mm",
            "cm",
            "in",
            "pt",
            "pc"
          ]
        }
      },
      "required": [
        "value",
        "unit"
      ],
      "additionalProperties": false,
      "x-css-px-absolute-maximum": 1000000,
      "default": {
        "value": 0,
        "unit": "mm"
      },
      "description": "Convert recursively to CSS px at 96 dpi, then enforce absolute <=1000000 and sign bound. No rounding or clamping.",
      "allOf": [
        {
          "if": {
            "properties": {
              "unit": {
                "const": "px"
              }
            },
            "required": [
              "unit"
            ]
          },
          "then": {
            "properties": {
              "value": {
                "minimum": 0,
                "maximum": 1000000.0
              }
            }
          }
        },
        {
          "if": {
            "properties": {
              "unit": {
                "const": "mm"
              }
            },
            "required": [
              "unit"
            ]
          },
          "then": {
            "properties": {
              "value": {
                "minimum": 0,
                "maximum": 264583.3333333333
              }
            }
          }
        },
        {
          "if": {
            "properties": {
              "unit": {
                "const": "cm"
              }
            },
            "required": [
              "unit"
            ]
          },
          "then": {
            "properties": {
              "value": {
                "minimum": 0,
                "maximum": 26458.333333333336
              }
            }
          }
        },
        {
          "if": {
            "properties": {
              "unit": {
                "const": "in"
              }
            },
            "required": [
              "unit"
            ]
          },
          "then": {
            "properties": {
              "value": {
                "minimum": 0,
                "maximum": 10416.666666666666
              }
            }
          }
        },
        {
          "if": {
            "properties": {
              "unit": {
                "const": "pt"
              }
            },
            "required": [
              "unit"
            ]
          },
          "then": {
            "properties": {
              "value": {
                "minimum": 0,
                "maximum": 750000.0
              }
            }
          }
        },
        {
          "if": {
            "properties": {
              "unit": {
                "const": "pc"
              }
            },
            "required": [
              "unit"
            ]
          },
          "then": {
            "properties": {
              "value": {
                "minimum": 0,
                "maximum": 62500.0
              }
            }
          }
        }
      ]
    },
    "rotations": {
      "type": "object",
      "properties": {
        "mode": {
          "type": "string",
          "enum": [
            "none",
            "right-angles",
            "discrete",
            "free"
          ],
          "default": "none"
        },
        "step-degrees": {
          "type": "number",
          "maximum": 360,
          "exclusiveMinimum": 0
        }
      },
      "required": [],
      "additionalProperties": false,
      "default": {
        "mode": "none"
      },
      "oneOf": [
        {
          "properties": {
            "mode": {
              "const": "discrete"
            }
          },
          "required": [
            "mode",
            "step-degrees"
          ]
        },
        {
          "properties": {
            "mode": {
              "enum": [
                "none",
                "right-angles",
                "free"
              ]
            }
          },
          "not": {
            "required": [
              "step-degrees"
            ]
          }
        }
      ]
    },
    "copies": {
      "type": "object",
      "propertyNames": {
        "type": "string",
        "minLength": 1,
        "maxLength": 128
      },
      "additionalProperties": {
        "type": "integer",
        "minimum": 1,
        "maximum": 100000
      },
      "maxProperties": 100000
    },
    "obstacles": {
      "type": "array",
      "items": {
        "type": "string",
        "minLength": 1,
        "maxLength": 128
      },
      "minItems": 0,
      "maxItems": 100000,
      "uniqueItems": true,
      "default": []
    },
    "fallback": {
      "type": "string",
      "enum": [
        "reject",
        "conservative-hull",
        "conservative-bounds"
      ],
      "default": "reject"
    },
    "retain": {
      "type": "boolean",
      "default": true
    }
  },
  "required": [
    "ids"
  ],
  "additionalProperties": false,
  "oneOf": [
    {
      "required": [
        "page"
      ],
      "not": {
        "required": [
          "sheet"
        ]
      }
    },
    {
      "required": [
        "sheet"
      ],
      "not": {
        "required": [
          "page"
        ]
      }
    }
  ],
  "x-m3-contract": {
    "guard_domain": "document",
    "guard_required": false,
    "needs_document": true,
    "selection_mode": "collective-geometry",
    "target_cardinality": {
      "min": 1,
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
    "native_service": "`captureDocumentNesting/prepareCapturedGeometry/assemblePreparedNesting`, `src/nesting/nesting-document.cpp:2709,2833,2976`; `solvingSnapshot`, `:3030`",
    "copy_policy": "zero-based; copy 0 moves source; >=1 duplicated only when placed, inside one native transaction; validate expanded count <=100000 and keys against normalized roots before allocation",
    "sheet_policy": "request-local page-index/rectangle plus page revision; immutable worker geometry separate from owner-thread weak capture; no temporary live sheet; fallback reject forbids conservative capture; Job::validate then freshness before one native commit",
    "warnings": [
      {
        "code": "geometry-repaired",
        "emit_site": "capture/prepare reports geometry-repaired; only when permitted by fallback"
      },
      {
        "code": "conservative-recovery",
        "emit_site": "capture/prepare reports conservative-recovery; only when permitted by fallback"
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
        "native_evidence": "`captureDocumentNesting/prepareCapturedGeometry/assemblePreparedNesting`, `src/nesting/nesting-document.cpp:2709,2833,2976`; `solvingSnapshot`, `:3030`",
        "oracle": "P9:nest.analyze:transport/schema:request-too-large:parse_request: request-too-large",
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
        "native_evidence": "`captureDocumentNesting/prepareCapturedGeometry/assemblePreparedNesting`, `src/nesting/nesting-document.cpp:2709,2833,2976`; `solvingSnapshot`, `:3030`",
        "oracle": "P9:nest.analyze:transport/schema:repeated-key:parse_request: repeated-key",
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
        "native_evidence": "`captureDocumentNesting/prepareCapturedGeometry/assemblePreparedNesting`, `src/nesting/nesting-document.cpp:2709,2833,2976`; `solvingSnapshot`, `:3030`",
        "oracle": "P9:nest.analyze:transport/schema:invalid-utf8:parse_request: invalid-utf8",
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
        "native_evidence": "`captureDocumentNesting/prepareCapturedGeometry/assemblePreparedNesting`, `src/nesting/nesting-document.cpp:2709,2833,2976`; `solvingSnapshot`, `:3030`",
        "oracle": "P9:nest.analyze:transport/schema:malformed-json:parse_request: malformed-json",
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
        "native_evidence": "`captureDocumentNesting/prepareCapturedGeometry/assemblePreparedNesting`, `src/nesting/nesting-document.cpp:2709,2833,2976`; `solvingSnapshot`, `:3030`",
        "oracle": "P9:nest.analyze:transport/schema:invalid-argument:raw schema/type/range/route/uniqueItems violation",
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
        "native_evidence": "`captureDocumentNesting/prepareCapturedGeometry/assemblePreparedNesting`, `src/nesting/nesting-document.cpp:2709,2833,2976`; `solvingSnapshot`, `:3030`",
        "oracle": "P9:nest.analyze:identity/admission:no-document:missing current document",
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
        "native_evidence": "`captureDocumentNesting/prepareCapturedGeometry/assemblePreparedNesting`, `src/nesting/nesting-document.cpp:2709,2833,2976`; `solvingSnapshot`, `:3030`",
        "oracle": "P9:nest.analyze:identity/admission:stale-document:document identity mismatch",
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
        "native_evidence": "`captureDocumentNesting/prepareCapturedGeometry/assemblePreparedNesting`, `src/nesting/nesting-document.cpp:2709,2833,2976`; `solvingSnapshot`, `:3030`",
        "oracle": "P9:nest.analyze:identity/admission:stale-revision:supplied guard differs from document revision",
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
        "native_evidence": "`captureDocumentNesting/prepareCapturedGeometry/assemblePreparedNesting`, `src/nesting/nesting-document.cpp:2709,2833,2976`; `solvingSnapshot`, `:3030`",
        "oracle": "P9:nest.analyze:identity/admission:cancelled:cancellation observed before commit",
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
        "native_evidence": "`captureDocumentNesting/prepareCapturedGeometry/assemblePreparedNesting`, `src/nesting/nesting-document.cpp:2709,2833,2976`; `solvingSnapshot`, `:3030`",
        "oracle": "P9:nest.analyze:identity/admission:unknown-id:explicit ID cannot resolve",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "retention allocation/root budget exceeded",
        "code": "token-capacity",
        "retryable": false,
        "mutation_state": "none",
        "route": "retain=true and not dry-run",
        "detail.reason": "retention allocation/root budget exceeded",
        "native_evidence": "`captureDocumentNesting/prepareCapturedGeometry/assemblePreparedNesting`, `src/nesting/nesting-document.cpp:2709,2833,2976`; `solvingSnapshot`, `:3030`",
        "oracle": "P9:nest.analyze:identity/admission:token-capacity:retention allocation/root budget exceeded",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "retention requested without session",
        "code": "session-required",
        "retryable": false,
        "mutation_state": "none",
        "route": "retain=true and not dry-run",
        "detail.reason": "retention requested without session",
        "native_evidence": "`captureDocumentNesting/prepareCapturedGeometry/assemblePreparedNesting`, `src/nesting/nesting-document.cpp:2709,2833,2976`; `solvingSnapshot`, `:3030`",
        "oracle": "P9:nest.analyze:identity/admission:session-required:retention requested without session",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "capture: page not found",
        "code": "invalid-input",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "capture",
        "native_evidence": "`captureDocumentNesting/prepareCapturedGeometry/assemblePreparedNesting`, `src/nesting/nesting-document.cpp:2709,2833,2976`; `solvingSnapshot`, `:3030`",
        "oracle": "P9:nest.analyze:command-service:invalid-input:capture: page not found",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "capture: unusable part",
        "code": "unsupported-target",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "capture: unusable part",
        "native_evidence": "D16: only no usable parts refuses; mixed usable/unusable selection succeeds with exclusions. `captureDocumentNesting/prepareCapturedGeometry/assemblePreparedNesting`, `src/nesting/nesting-document.cpp:2709,2833,2976`; `solvingSnapshot`, `:3030`",
        "oracle": "P9:nest.analyze:command-service:unsupported-target:capture: unusable part",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "capture: conservative and fallback reject",
        "code": "unsupported-target",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "capture",
        "native_evidence": "`captureDocumentNesting/prepareCapturedGeometry/assemblePreparedNesting`, `src/nesting/nesting-document.cpp:2709,2833,2976`; `solvingSnapshot`, `:3030`",
        "oracle": "P9:nest.analyze:command-service:unsupported-target:capture: conservative and fallback reject",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "expanded copies >100000 or resource budget",
        "code": "engine-limit",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "expanded copies >100000 or resource budget",
        "native_evidence": "`captureDocumentNesting/prepareCapturedGeometry/assemblePreparedNesting`, `src/nesting/nesting-document.cpp:2709,2833,2976`; `solvingSnapshot`, `:3030`",
        "oracle": "P9:nest.analyze:command-service:engine-limit:expanded copies >100000 or resource budget",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "prepare/assemble geometry failed",
        "code": "analysis-failed",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "prepare/assemble geometry failed",
        "native_evidence": "`captureDocumentNesting/prepareCapturedGeometry/assemblePreparedNesting`, `src/nesting/nesting-document.cpp:2709,2833,2976`; `solvingSnapshot`, `:3030`",
        "oracle": "P9:nest.analyze:command-service:analysis-failed:prepare/assemble geometry failed",
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
        "native_evidence": "`captureDocumentNesting/prepareCapturedGeometry/assemblePreparedNesting`, `src/nesting/nesting-document.cpp:2709,2833,2976`; `solvingSnapshot`, `:3030`",
        "oracle": "P9:nest.analyze:command-service:internal-error:unexpected service exception; rollback before return",
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
        "evidence_route": "P9:nest.analyze:native:native successful outcome",
        "evidence_status": "required-unimplemented"
      },
      {
        "native_branch": "native unchanged outcome when applicable",
        "code": null,
        "retryable": false,
        "mutation_state": "none",
        "data_variant": "unchanged",
        "evidence_route": "P9:nest.analyze:native:native unchanged outcome when applicable",
        "evidence_status": "required-unimplemented"
      },
      {
        "native_branch": "read-only computed preparation",
        "code": null,
        "retryable": false,
        "mutation_state": "none",
        "data_variant": "computed-dry-run",
        "evidence_route": "P9:nest.analyze:native:read-only computed preparation",
        "evidence_status": "required-unimplemented"
      }
    ]
  }
})m3").as_object()};
// | `nest.solve` | analysis-token; `engine=native` (singleton M3 enum); `seed=0` uint64; exactly one `iterations` integer 1..1000000 or `timeout-ms` 100..600000; retain=true | solution-token, placements/unplaced, utilization, iterations, stop-reason, deterministic flag | K, solver-unavailable, solve-failed, unsupported-work-limit | Q+R / none / computed transient / Q immutable solve; timeout results disclose nondeterminism |
TypeDescriptor const m3_3{boost::json::parse(R"m3({
  "type": "object",
  "properties": {
    "analysis-token": {
      "type": "string",
      "minLength": 1,
      "maxLength": 128
    },
    "engine": {
      "type": "string",
      "enum": [
        "native"
      ],
      "default": "native"
    },
    "seed": {
      "type": "integer",
      "minimum": 0,
      "maximum": 18446744073709551615,
      "default": 0,
      "x-exact-uint64": true,
      "description": "JSON integer token 0..18446744073709551615, parsed losslessly; floating/exponent forms rejected before double conversion."
    },
    "iterations": {
      "type": "integer",
      "minimum": 1,
      "maximum": 1000000
    },
    "timeout-ms": {
      "type": "integer",
      "minimum": 100,
      "maximum": 600000
    },
    "retain": {
      "type": "boolean",
      "default": true
    }
  },
  "required": [
    "analysis-token"
  ],
  "additionalProperties": false,
  "oneOf": [
    {
      "required": [
        "iterations"
      ],
      "not": {
        "required": [
          "timeout-ms"
        ]
      }
    },
    {
      "required": [
        "timeout-ms"
      ],
      "not": {
        "required": [
          "iterations"
        ]
      }
    }
  ],
  "x-m3-contract": {
    "guard_domain": "document",
    "guard_required": false,
    "needs_document": true,
    "selection_mode": "collective-geometry",
    "target_cardinality": {
      "min": 1,
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
    "native_service": "`solveNativeNesting`, same file `:3068` (currently private), `Job::run`, `src/nesting/nesting-ffi.cpp:285`",
    "copy_policy": "zero-based; copy 0 moves source; >=1 duplicated only when placed, inside one native transaction; validate expanded count <=100000 and keys against normalized roots before allocation",
    "sheet_policy": "request-local page-index/rectangle plus page revision; immutable worker geometry separate from owner-thread weak capture; no temporary live sheet; fallback reject forbids conservative capture; Job::validate then freshness before one native commit",
    "work_policy": "one existing native solver progress iteration; preparation excluded; limit checked wherever deadline is checked; native-only, one worker, exact supplied seed; iterations=completed_work; fixed-work deterministic true, timeout deterministic false; cancellation is cancelled, never success",
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
        "native_evidence": "`solveNativeNesting`, same file `:3068` (currently private), `Job::run`, `src/nesting/nesting-ffi.cpp:285`",
        "oracle": "P9:nest.solve:transport/schema:request-too-large:parse_request: request-too-large",
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
        "native_evidence": "`solveNativeNesting`, same file `:3068` (currently private), `Job::run`, `src/nesting/nesting-ffi.cpp:285`",
        "oracle": "P9:nest.solve:transport/schema:repeated-key:parse_request: repeated-key",
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
        "native_evidence": "`solveNativeNesting`, same file `:3068` (currently private), `Job::run`, `src/nesting/nesting-ffi.cpp:285`",
        "oracle": "P9:nest.solve:transport/schema:invalid-utf8:parse_request: invalid-utf8",
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
        "native_evidence": "`solveNativeNesting`, same file `:3068` (currently private), `Job::run`, `src/nesting/nesting-ffi.cpp:285`",
        "oracle": "P9:nest.solve:transport/schema:malformed-json:parse_request: malformed-json",
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
        "native_evidence": "`solveNativeNesting`, same file `:3068` (currently private), `Job::run`, `src/nesting/nesting-ffi.cpp:285`",
        "oracle": "P9:nest.solve:transport/schema:invalid-argument:raw schema/type/range/route/uniqueItems violation",
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
        "native_evidence": "`solveNativeNesting`, same file `:3068` (currently private), `Job::run`, `src/nesting/nesting-ffi.cpp:285`",
        "oracle": "P9:nest.solve:identity/admission:no-document:missing current document",
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
        "native_evidence": "`solveNativeNesting`, same file `:3068` (currently private), `Job::run`, `src/nesting/nesting-ffi.cpp:285`",
        "oracle": "P9:nest.solve:identity/admission:stale-document:document identity mismatch",
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
        "native_evidence": "`solveNativeNesting`, same file `:3068` (currently private), `Job::run`, `src/nesting/nesting-ffi.cpp:285`",
        "oracle": "P9:nest.solve:identity/admission:stale-revision:supplied guard differs from document revision",
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
        "native_evidence": "`solveNativeNesting`, same file `:3068` (currently private), `Job::run`, `src/nesting/nesting-ffi.cpp:285`",
        "oracle": "P9:nest.solve:identity/admission:cancelled:cancellation observed before commit",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "token missing or wrong kind/parent",
        "code": "invalid-token",
        "retryable": false,
        "mutation_state": "none",
        "route": "token route",
        "detail.reason": "token missing or wrong kind/parent",
        "native_evidence": "`solveNativeNesting`, same file `:3068` (currently private), `Job::run`, `src/nesting/nesting-ffi.cpp:285`",
        "oracle": "P9:nest.solve:identity/admission:invalid-token:token missing or wrong kind/parent",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "stored document generation/revision differs",
        "code": "stale-plan",
        "retryable": false,
        "mutation_state": "none",
        "route": "token route",
        "detail.reason": "stored document generation/revision differs",
        "native_evidence": "`solveNativeNesting`, same file `:3068` (currently private), `Job::run`, `src/nesting/nesting-ffi.cpp:285`",
        "oracle": "P9:nest.solve:identity/admission:stale-plan:stored document generation/revision differs",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "stored dependency revalidation failed",
        "code": "stale-dependency",
        "retryable": false,
        "mutation_state": "none",
        "route": "token route",
        "detail.reason": "stored dependency revalidation failed",
        "native_evidence": "`solveNativeNesting`, same file `:3068` (currently private), `Job::run`, `src/nesting/nesting-ffi.cpp:285`",
        "oracle": "P9:nest.solve:identity/admission:stale-dependency:stored dependency revalidation failed",
        "evidence_status": "required-unimplemented"
      },
{
        "layer": "identity/admission",
        "native_branch": "retention allocation/root budget exceeded",
        "code": "token-capacity",
        "retryable": false,
        "mutation_state": "none",
        "route": "retain=true and not dry-run",
        "detail.reason": "retention allocation/root budget exceeded",
        "native_evidence": "`solveNativeNesting`, same file `:3068` (currently private), `Job::run`, `src/nesting/nesting-ffi.cpp:285`",
        "oracle": "P9:nest.solve:identity/admission:token-capacity:retention allocation/root budget exceeded",
        "evidence_status": "required-unimplemented"
      },
{
        "layer": "command-service",
        "native_branch": "native worker failed",
        "code": "solver-failed",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "native worker failed",
        "native_evidence": "`solveNativeNesting`, same file `:3068` (currently private), `Job::run`, `src/nesting/nesting-ffi.cpp:285`",
        "oracle": "P9:nest.solve:command-service:solver-failed:native worker failed",
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
        "native_evidence": "`solveNativeNesting`, same file `:3068` (currently private), `Job::run`, `src/nesting/nesting-ffi.cpp:285`",
        "oracle": "P9:nest.solve:command-service:internal-error:unexpected service exception; rollback before return",
        "evidence_status": "required-unimplemented"
      }
    ],
    "success_native_branches": [
      "WorkLimit",
      "Completed",
      "TimeLimit",
      "Cancelled"
    ],
    "variant_applicability": {
      "success": "ok or changed",
      "unchanged": "native no-op only; query/preparation/history empty are not successful unchanged",
      "computed-dry-run": "successful read-only preflight; no retention, publication, Undo or Redo change"
    },
    "success_rows": [
      {
        "native_branch": "WorkLimit",
        "code": null,
        "retryable": false,
        "mutation_state": "none",
        "data_variant": "success",
        "evidence_route": "P9:nest.solve:native:WorkLimit",
        "evidence_status": "required-unimplemented"
      },
      {
        "native_branch": "Completed",
        "code": null,
        "retryable": false,
        "mutation_state": "none",
        "data_variant": "success",
        "evidence_route": "P9:nest.solve:native:Completed",
        "evidence_status": "required-unimplemented"
      },
      {
        "native_branch": "TimeLimit",
        "code": null,
        "retryable": false,
        "mutation_state": "none",
        "data_variant": "success",
        "evidence_route": "P9:nest.solve:native:TimeLimit",
        "evidence_status": "required-unimplemented"
      },
      {
        "native_branch": "Cancelled",
        "code": "cancelled",
        "retryable": false,
        "mutation_state": "none",
        "data_variant": null,
        "evidence_route": "P9:nest.solve:native:Cancelled",
        "evidence_status": "required-unimplemented"
      }
    ]
  }
})m3").as_object()};
// | `nest.apply` | solution-token and `partial=reject|allow` default reject; OR full analyze+solve recipe with same partial policy for one-shot | transformed/copy IDs, source→copy map, unplaced IDs and selection-after | K, T for recipe route, partial-solution, invalid-placement, nest-apply-failed | E / one / computed / G; placement validation immediately before commit |
TypeDescriptor const m3_4{boost::json::parse(R"m3({
  "type": "object",
  "properties": {
    "solution-token": {
      "type": "string",
      "minLength": 1,
      "maxLength": 128
    },
    "analyze": {
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
        "page": {
          "type": "integer",
          "minimum": 1,
          "maximum": 10000
        },
        "sheet": {
          "type": "object",
          "properties": {
            "x": {
              "type": "object",
              "properties": {
                "value": {
                  "type": "number",
                  "minimum": -1000000.0,
                  "maximum": 1000000.0
                },
                "unit": {
                  "type": "string",
                  "enum": [
                    "px",
                    "mm",
                    "cm",
                    "in",
                    "pt",
                    "pc"
                  ]
                }
              },
              "required": [
                "value",
                "unit"
              ],
              "additionalProperties": false,
              "x-css-px-absolute-maximum": 1000000,
              "description": "Convert recursively to CSS px at 96 dpi, then enforce absolute <=1000000 and sign bound. No rounding or clamping.",
              "allOf": [
                {
                  "if": {
                    "properties": {
                      "unit": {
                        "const": "px"
                      }
                    },
                    "required": [
                      "unit"
                    ]
                  },
                  "then": {
                    "properties": {
                      "value": {
                        "minimum": -1000000.0,
                        "maximum": 1000000.0
                      }
                    }
                  }
                },
                {
                  "if": {
                    "properties": {
                      "unit": {
                        "const": "mm"
                      }
                    },
                    "required": [
                      "unit"
                    ]
                  },
                  "then": {
                    "properties": {
                      "value": {
                        "minimum": -264583.3333333333,
                        "maximum": 264583.3333333333
                      }
                    }
                  }
                },
                {
                  "if": {
                    "properties": {
                      "unit": {
                        "const": "cm"
                      }
                    },
                    "required": [
                      "unit"
                    ]
                  },
                  "then": {
                    "properties": {
                      "value": {
                        "minimum": -26458.333333333336,
                        "maximum": 26458.333333333336
                      }
                    }
                  }
                },
                {
                  "if": {
                    "properties": {
                      "unit": {
                        "const": "in"
                      }
                    },
                    "required": [
                      "unit"
                    ]
                  },
                  "then": {
                    "properties": {
                      "value": {
                        "minimum": -10416.666666666666,
                        "maximum": 10416.666666666666
                      }
                    }
                  }
                },
                {
                  "if": {
                    "properties": {
                      "unit": {
                        "const": "pt"
                      }
                    },
                    "required": [
                      "unit"
                    ]
                  },
                  "then": {
                    "properties": {
                      "value": {
                        "minimum": -750000.0,
                        "maximum": 750000.0
                      }
                    }
                  }
                },
                {
                  "if": {
                    "properties": {
                      "unit": {
                        "const": "pc"
                      }
                    },
                    "required": [
                      "unit"
                    ]
                  },
                  "then": {
                    "properties": {
                      "value": {
                        "minimum": -62500.0,
                        "maximum": 62500.0
                      }
                    }
                  }
                }
              ]
            },
            "y": {
              "type": "object",
              "properties": {
                "value": {
                  "type": "number",
                  "minimum": -1000000.0,
                  "maximum": 1000000.0
                },
                "unit": {
                  "type": "string",
                  "enum": [
                    "px",
                    "mm",
                    "cm",
                    "in",
                    "pt",
                    "pc"
                  ]
                }
              },
              "required": [
                "value",
                "unit"
              ],
              "additionalProperties": false,
              "x-css-px-absolute-maximum": 1000000,
              "description": "Convert recursively to CSS px at 96 dpi, then enforce absolute <=1000000 and sign bound. No rounding or clamping.",
              "allOf": [
                {
                  "if": {
                    "properties": {
                      "unit": {
                        "const": "px"
                      }
                    },
                    "required": [
                      "unit"
                    ]
                  },
                  "then": {
                    "properties": {
                      "value": {
                        "minimum": -1000000.0,
                        "maximum": 1000000.0
                      }
                    }
                  }
                },
                {
                  "if": {
                    "properties": {
                      "unit": {
                        "const": "mm"
                      }
                    },
                    "required": [
                      "unit"
                    ]
                  },
                  "then": {
                    "properties": {
                      "value": {
                        "minimum": -264583.3333333333,
                        "maximum": 264583.3333333333
                      }
                    }
                  }
                },
                {
                  "if": {
                    "properties": {
                      "unit": {
                        "const": "cm"
                      }
                    },
                    "required": [
                      "unit"
                    ]
                  },
                  "then": {
                    "properties": {
                      "value": {
                        "minimum": -26458.333333333336,
                        "maximum": 26458.333333333336
                      }
                    }
                  }
                },
                {
                  "if": {
                    "properties": {
                      "unit": {
                        "const": "in"
                      }
                    },
                    "required": [
                      "unit"
                    ]
                  },
                  "then": {
                    "properties": {
                      "value": {
                        "minimum": -10416.666666666666,
                        "maximum": 10416.666666666666
                      }
                    }
                  }
                },
                {
                  "if": {
                    "properties": {
                      "unit": {
                        "const": "pt"
                      }
                    },
                    "required": [
                      "unit"
                    ]
                  },
                  "then": {
                    "properties": {
                      "value": {
                        "minimum": -750000.0,
                        "maximum": 750000.0
                      }
                    }
                  }
                },
                {
                  "if": {
                    "properties": {
                      "unit": {
                        "const": "pc"
                      }
                    },
                    "required": [
                      "unit"
                    ]
                  },
                  "then": {
                    "properties": {
                      "value": {
                        "minimum": -62500.0,
                        "maximum": 62500.0
                      }
                    }
                  }
                }
              ]
            },
            "width": {
              "type": "object",
              "properties": {
                "value": {
                  "type": "number",
                  "minimum": 0,
                  "maximum": 1000000.0,
                  "exclusiveMinimum": 0
                },
                "unit": {
                  "type": "string",
                  "enum": [
                    "px",
                    "mm",
                    "cm",
                    "in",
                    "pt",
                    "pc"
                  ]
                }
              },
              "required": [
                "value",
                "unit"
              ],
              "additionalProperties": false,
              "x-css-px-absolute-maximum": 1000000,
              "description": "Convert recursively to CSS px at 96 dpi, then enforce absolute <=1000000 and sign bound. No rounding or clamping.",
              "allOf": [
                {
                  "if": {
                    "properties": {
                      "unit": {
                        "const": "px"
                      }
                    },
                    "required": [
                      "unit"
                    ]
                  },
                  "then": {
                    "properties": {
                      "value": {
                        "minimum": 0,
                        "maximum": 1000000.0
                      }
                    }
                  }
                },
                {
                  "if": {
                    "properties": {
                      "unit": {
                        "const": "mm"
                      }
                    },
                    "required": [
                      "unit"
                    ]
                  },
                  "then": {
                    "properties": {
                      "value": {
                        "minimum": 0,
                        "maximum": 264583.3333333333
                      }
                    }
                  }
                },
                {
                  "if": {
                    "properties": {
                      "unit": {
                        "const": "cm"
                      }
                    },
                    "required": [
                      "unit"
                    ]
                  },
                  "then": {
                    "properties": {
                      "value": {
                        "minimum": 0,
                        "maximum": 26458.333333333336
                      }
                    }
                  }
                },
                {
                  "if": {
                    "properties": {
                      "unit": {
                        "const": "in"
                      }
                    },
                    "required": [
                      "unit"
                    ]
                  },
                  "then": {
                    "properties": {
                      "value": {
                        "minimum": 0,
                        "maximum": 10416.666666666666
                      }
                    }
                  }
                },
                {
                  "if": {
                    "properties": {
                      "unit": {
                        "const": "pt"
                      }
                    },
                    "required": [
                      "unit"
                    ]
                  },
                  "then": {
                    "properties": {
                      "value": {
                        "minimum": 0,
                        "maximum": 750000.0
                      }
                    }
                  }
                },
                {
                  "if": {
                    "properties": {
                      "unit": {
                        "const": "pc"
                      }
                    },
                    "required": [
                      "unit"
                    ]
                  },
                  "then": {
                    "properties": {
                      "value": {
                        "minimum": 0,
                        "maximum": 62500.0
                      }
                    }
                  }
                }
              ]
            },
            "height": {
              "type": "object",
              "properties": {
                "value": {
                  "type": "number",
                  "minimum": 0,
                  "maximum": 1000000.0,
                  "exclusiveMinimum": 0
                },
                "unit": {
                  "type": "string",
                  "enum": [
                    "px",
                    "mm",
                    "cm",
                    "in",
                    "pt",
                    "pc"
                  ]
                }
              },
              "required": [
                "value",
                "unit"
              ],
              "additionalProperties": false,
              "x-css-px-absolute-maximum": 1000000,
              "description": "Convert recursively to CSS px at 96 dpi, then enforce absolute <=1000000 and sign bound. No rounding or clamping.",
              "allOf": [
                {
                  "if": {
                    "properties": {
                      "unit": {
                        "const": "px"
                      }
                    },
                    "required": [
                      "unit"
                    ]
                  },
                  "then": {
                    "properties": {
                      "value": {
                        "minimum": 0,
                        "maximum": 1000000.0
                      }
                    }
                  }
                },
                {
                  "if": {
                    "properties": {
                      "unit": {
                        "const": "mm"
                      }
                    },
                    "required": [
                      "unit"
                    ]
                  },
                  "then": {
                    "properties": {
                      "value": {
                        "minimum": 0,
                        "maximum": 264583.3333333333
                      }
                    }
                  }
                },
                {
                  "if": {
                    "properties": {
                      "unit": {
                        "const": "cm"
                      }
                    },
                    "required": [
                      "unit"
                    ]
                  },
                  "then": {
                    "properties": {
                      "value": {
                        "minimum": 0,
                        "maximum": 26458.333333333336
                      }
                    }
                  }
                },
                {
                  "if": {
                    "properties": {
                      "unit": {
                        "const": "in"
                      }
                    },
                    "required": [
                      "unit"
                    ]
                  },
                  "then": {
                    "properties": {
                      "value": {
                        "minimum": 0,
                        "maximum": 10416.666666666666
                      }
                    }
                  }
                },
                {
                  "if": {
                    "properties": {
                      "unit": {
                        "const": "pt"
                      }
                    },
                    "required": [
                      "unit"
                    ]
                  },
                  "then": {
                    "properties": {
                      "value": {
                        "minimum": 0,
                        "maximum": 750000.0
                      }
                    }
                  }
                },
                {
                  "if": {
                    "properties": {
                      "unit": {
                        "const": "pc"
                      }
                    },
                    "required": [
                      "unit"
                    ]
                  },
                  "then": {
                    "properties": {
                      "value": {
                        "minimum": 0,
                        "maximum": 62500.0
                      }
                    }
                  }
                }
              ]
            }
          },
          "required": [
            "x",
            "y",
            "width",
            "height"
          ],
          "additionalProperties": false
        },
        "gap": {
          "type": "object",
          "properties": {
            "value": {
              "type": "number",
              "minimum": 0,
              "maximum": 1000000.0
            },
            "unit": {
              "type": "string",
              "enum": [
                "px",
                "mm",
                "cm",
                "in",
                "pt",
                "pc"
              ]
            }
          },
          "required": [
            "value",
            "unit"
          ],
          "additionalProperties": false,
          "x-css-px-absolute-maximum": 1000000,
          "default": {
            "value": 0,
            "unit": "mm"
          },
          "description": "Convert recursively to CSS px at 96 dpi, then enforce absolute <=1000000 and sign bound. No rounding or clamping.",
          "allOf": [
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "px"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": 0,
                    "maximum": 1000000.0
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "mm"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": 0,
                    "maximum": 264583.3333333333
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "cm"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": 0,
                    "maximum": 26458.333333333336
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "in"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": 0,
                    "maximum": 10416.666666666666
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "pt"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": 0,
                    "maximum": 750000.0
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "pc"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": 0,
                    "maximum": 62500.0
                  }
                }
              }
            }
          ]
        },
        "margin": {
          "type": "object",
          "properties": {
            "value": {
              "type": "number",
              "minimum": 0,
              "maximum": 1000000.0
            },
            "unit": {
              "type": "string",
              "enum": [
                "px",
                "mm",
                "cm",
                "in",
                "pt",
                "pc"
              ]
            }
          },
          "required": [
            "value",
            "unit"
          ],
          "additionalProperties": false,
          "x-css-px-absolute-maximum": 1000000,
          "default": {
            "value": 0,
            "unit": "mm"
          },
          "description": "Convert recursively to CSS px at 96 dpi, then enforce absolute <=1000000 and sign bound. No rounding or clamping.",
          "allOf": [
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "px"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": 0,
                    "maximum": 1000000.0
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "mm"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": 0,
                    "maximum": 264583.3333333333
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "cm"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": 0,
                    "maximum": 26458.333333333336
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "in"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": 0,
                    "maximum": 10416.666666666666
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "pt"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": 0,
                    "maximum": 750000.0
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "pc"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": 0,
                    "maximum": 62500.0
                  }
                }
              }
            }
          ]
        },
        "rotations": {
          "type": "object",
          "properties": {
            "mode": {
              "type": "string",
              "enum": [
                "none",
                "right-angles",
                "discrete",
                "free"
              ],
              "default": "none"
            },
            "step-degrees": {
              "type": "number",
              "maximum": 360,
              "exclusiveMinimum": 0
            }
          },
          "required": [],
          "additionalProperties": false,
          "default": {
            "mode": "none"
          },
          "oneOf": [
            {
              "properties": {
                "mode": {
                  "const": "discrete"
                }
              },
              "required": [
                "mode",
                "step-degrees"
              ]
            },
            {
              "properties": {
                "mode": {
                  "enum": [
                    "none",
                    "right-angles",
                    "free"
                  ]
                }
              },
              "not": {
                "required": [
                  "step-degrees"
                ]
              }
            }
          ]
        },
        "copies": {
          "type": "object",
          "propertyNames": {
            "type": "string",
            "minLength": 1,
            "maxLength": 128
          },
          "additionalProperties": {
            "type": "integer",
            "minimum": 1,
            "maximum": 100000
          },
          "maxProperties": 100000
        },
        "obstacles": {
          "type": "array",
          "items": {
            "type": "string",
            "minLength": 1,
            "maxLength": 128
          },
          "minItems": 0,
          "maxItems": 100000,
          "uniqueItems": true,
          "default": []
        },
        "fallback": {
          "type": "string",
          "enum": [
            "reject",
            "conservative-hull",
            "conservative-bounds"
          ],
          "default": "reject"
        }
      },
      "required": [
        "ids"
      ],
      "additionalProperties": false,
      "oneOf": [
        {
          "required": [
            "page"
          ],
          "not": {
            "required": [
              "sheet"
            ]
          }
        },
        {
          "required": [
            "sheet"
          ],
          "not": {
            "required": [
              "page"
            ]
          }
        }
      ]
    },
    "solve": {
      "type": "object",
      "properties": {
        "engine": {
          "type": "string",
          "enum": [
            "native"
          ],
          "default": "native"
        },
        "seed": {
          "type": "integer",
          "minimum": 0,
          "maximum": 18446744073709551615,
          "default": 0,
          "x-exact-uint64": true,
          "description": "JSON integer token 0..18446744073709551615, parsed losslessly; floating/exponent forms rejected before double conversion."
        },
        "iterations": {
          "type": "integer",
          "minimum": 1,
          "maximum": 1000000
        },
        "timeout-ms": {
          "type": "integer",
          "minimum": 100,
          "maximum": 600000
        }
      },
      "required": [],
      "additionalProperties": false,
      "oneOf": [
        {
          "required": [
            "iterations"
          ],
          "not": {
            "required": [
              "timeout-ms"
            ]
          }
        },
        {
          "required": [
            "timeout-ms"
          ],
          "not": {
            "required": [
              "iterations"
            ]
          }
        }
      ]
    },
    "partial": {
      "type": "string",
      "enum": [
        "reject",
        "allow"
      ],
      "default": "reject"
    }
  },
  "required": [],
  "additionalProperties": false,
  "oneOf": [
    {
      "required": [
        "solution-token"
      ],
      "not": {
        "anyOf": [
          {
            "required": [
              "analyze"
            ]
          },
          {
            "required": [
              "solve"
            ]
          }
        ]
      }
    },
    {
      "required": [
        "analyze",
        "solve"
      ],
      "not": {
        "required": [
          "solution-token"
        ]
      }
    }
  ],
  "x-m3-contract": {
    "guard_domain": "document",
    "guard_required": true,
    "needs_document": true,
    "selection_mode": "collective-geometry",
    "target_cardinality": {
      "min": 1,
      "max": 100000
    },
    "normalization": "ordered-composite-roots; ancestor covers descendant, preserving surviving input order",
    "partial_policy": "request partial reject or allow; preserve unplaced source copy 0; omit unplaced duplicates",
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
    "native_service": "`Job::validate`, `src/nesting/nesting-ffi.cpp:299`; `applyNestingPlacements`, `src/nesting/nesting-document.cpp:3232`",
    "copy_policy": "zero-based; copy 0 moves source; >=1 duplicated only when placed, inside one native transaction; validate expanded count <=100000 and keys against normalized roots before allocation",
    "sheet_policy": "request-local page-index/rectangle plus page revision; immutable worker geometry separate from owner-thread weak capture; no temporary live sheet; fallback reject forbids conservative capture; Job::validate then freshness before one native commit",
    "warnings": [
      {
        "code": "geometry-repaired",
        "emit_site": "capture/prepare reports geometry-repaired; only when permitted by fallback"
      },
      {
        "code": "conservative-recovery",
        "emit_site": "capture/prepare reports conservative-recovery; only when permitted by fallback"
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
        "native_evidence": "`Job::validate`, `src/nesting/nesting-ffi.cpp:299`; `applyNestingPlacements`, `src/nesting/nesting-document.cpp:3232`",
        "oracle": "P9:nest.apply:transport/schema:request-too-large:parse_request: request-too-large",
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
        "native_evidence": "`Job::validate`, `src/nesting/nesting-ffi.cpp:299`; `applyNestingPlacements`, `src/nesting/nesting-document.cpp:3232`",
        "oracle": "P9:nest.apply:transport/schema:repeated-key:parse_request: repeated-key",
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
        "native_evidence": "`Job::validate`, `src/nesting/nesting-ffi.cpp:299`; `applyNestingPlacements`, `src/nesting/nesting-document.cpp:3232`",
        "oracle": "P9:nest.apply:transport/schema:invalid-utf8:parse_request: invalid-utf8",
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
        "native_evidence": "`Job::validate`, `src/nesting/nesting-ffi.cpp:299`; `applyNestingPlacements`, `src/nesting/nesting-document.cpp:3232`",
        "oracle": "P9:nest.apply:transport/schema:malformed-json:parse_request: malformed-json",
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
        "native_evidence": "`Job::validate`, `src/nesting/nesting-ffi.cpp:299`; `applyNestingPlacements`, `src/nesting/nesting-document.cpp:3232`",
        "oracle": "P9:nest.apply:transport/schema:invalid-argument:raw schema/type/range/route/uniqueItems violation",
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
        "native_evidence": "`Job::validate`, `src/nesting/nesting-ffi.cpp:299`; `applyNestingPlacements`, `src/nesting/nesting-document.cpp:3232`",
        "oracle": "P9:nest.apply:identity/admission:no-document:missing current document",
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
        "native_evidence": "`Job::validate`, `src/nesting/nesting-ffi.cpp:299`; `applyNestingPlacements`, `src/nesting/nesting-document.cpp:3232`",
        "oracle": "P9:nest.apply:identity/admission:stale-document:document identity mismatch",
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
        "native_evidence": "`Job::validate`, `src/nesting/nesting-ffi.cpp:299`; `applyNestingPlacements`, `src/nesting/nesting-document.cpp:3232`",
        "oracle": "P9:nest.apply:identity/admission:stale-revision:supplied guard differs from document revision",
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
        "native_evidence": "`Job::validate`, `src/nesting/nesting-ffi.cpp:299`; `applyNestingPlacements`, `src/nesting/nesting-document.cpp:3232`",
        "oracle": "P9:nest.apply:identity/admission:cancelled:cancellation observed before commit",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "token missing or wrong kind/parent",
        "code": "invalid-token",
        "retryable": false,
        "mutation_state": "none",
        "route": "token route",
        "detail.reason": "token missing or wrong kind/parent",
        "native_evidence": "`Job::validate`, `src/nesting/nesting-ffi.cpp:299`; `applyNestingPlacements`, `src/nesting/nesting-document.cpp:3232`",
        "oracle": "P9:nest.apply:identity/admission:invalid-token:token missing or wrong kind/parent",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "stored document generation/revision differs",
        "code": "stale-plan",
        "retryable": false,
        "mutation_state": "none",
        "route": "token route",
        "detail.reason": "stored document generation/revision differs",
        "native_evidence": "`Job::validate`, `src/nesting/nesting-ffi.cpp:299`; `applyNestingPlacements`, `src/nesting/nesting-document.cpp:3232`",
        "oracle": "P9:nest.apply:identity/admission:stale-plan:stored document generation/revision differs",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "stored dependency revalidation failed",
        "code": "stale-dependency",
        "retryable": false,
        "mutation_state": "none",
        "route": "token route",
        "detail.reason": "stored dependency revalidation failed",
        "native_evidence": "`Job::validate`, `src/nesting/nesting-ffi.cpp:299`; `applyNestingPlacements`, `src/nesting/nesting-document.cpp:3232`",
        "oracle": "P9:nest.apply:identity/admission:stale-dependency:stored dependency revalidation failed",
        "evidence_status": "required-unimplemented"
      },
{
        "layer": "command-service",
        "native_branch": "Job::validate: overlap/outside/invalid pose",
        "code": "invalid-placement",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "Job",
        "native_evidence": "`Job::validate`, `src/nesting/nesting-ffi.cpp:299`; `applyNestingPlacements`, `src/nesting/nesting-document.cpp:3232`",
        "oracle": "P9:nest.apply:command-service:invalid-placement:Job::validate: overlap/outside/invalid pose",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "unplaced copies and partial reject",
        "code": "partial-result",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "unplaced copies and partial reject",
        "native_evidence": "`Job::validate`, `src/nesting/nesting-ffi.cpp:299`; `applyNestingPlacements`, `src/nesting/nesting-document.cpp:3232`",
        "oracle": "P9:nest.apply:command-service:partial-result:unplaced copies and partial reject",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "native freshness check",
        "code": "stale-dependency",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "native freshness check",
        "native_evidence": "`Job::validate`, `src/nesting/nesting-ffi.cpp:299`; `applyNestingPlacements`, `src/nesting/nesting-document.cpp:3232`",
        "oracle": "P9:nest.apply:command-service:stale-dependency:native freshness check",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "copy insertion/apply failed; native rollback",
        "code": "publication-failed",
        "retryable": false,
        "mutation_state": "rolled-back",
        "route": "all",
        "detail.reason": "copy insertion/apply failed; native rollback",
        "native_evidence": "`Job::validate`, `src/nesting/nesting-ffi.cpp:299`; `applyNestingPlacements`, `src/nesting/nesting-document.cpp:3232`",
        "oracle": "P9:nest.apply:command-service:publication-failed:copy insertion/apply failed; native rollback",
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
        "native_evidence": "`Job::validate`, `src/nesting/nesting-ffi.cpp:299`; `applyNestingPlacements`, `src/nesting/nesting-document.cpp:3232`",
        "oracle": "P9:nest.apply:identity/admission:document-read-only:read-only session rejects requested document mutation",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "capture/assemble: invalid page or copy keys",
        "code": "invalid-input",
        "retryable": false,
        "mutation_state": "none",
        "route": "full analyze+solve route",
        "detail.reason": "capture/assemble: invalid page or copy keys",
        "native_evidence": "`Job::validate`, `src/nesting/nesting-ffi.cpp:299`; `applyNestingPlacements`, `src/nesting/nesting-document.cpp:3232`",
        "oracle": "P9:nest.apply:command-service:invalid-input:capture/assemble: invalid page or copy keys",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "capture: conservative geometry forbidden",
        "code": "unsupported-target",
        "retryable": false,
        "mutation_state": "none",
        "route": "full analyze+solve route",
        "detail.reason": "capture: conservative geometry forbidden",
        "native_evidence": "`Job::validate`, `src/nesting/nesting-ffi.cpp:299`; `applyNestingPlacements`, `src/nesting/nesting-document.cpp:3232`",
        "oracle": "P9:nest.apply:command-service:unsupported-target:capture: conservative geometry forbidden",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "expanded copies or resource budget exceeded",
        "code": "engine-limit",
        "retryable": false,
        "mutation_state": "none",
        "route": "full analyze+solve route",
        "detail.reason": "expanded copies or resource budget exceeded",
        "native_evidence": "`Job::validate`, `src/nesting/nesting-ffi.cpp:299`; `applyNestingPlacements`, `src/nesting/nesting-document.cpp:3232`",
        "oracle": "P9:nest.apply:command-service:engine-limit:expanded copies or resource budget exceeded",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "prepare: failed geometry",
        "code": "analysis-failed",
        "retryable": false,
        "mutation_state": "none",
        "route": "full analyze+solve route",
        "detail.reason": "prepare: failed geometry",
        "native_evidence": "`Job::validate`, `src/nesting/nesting-ffi.cpp:299`; `applyNestingPlacements`, `src/nesting/nesting-document.cpp:3232`",
        "oracle": "P9:nest.apply:command-service:analysis-failed:prepare: failed geometry",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "native worker failed",
        "code": "solver-failed",
        "retryable": false,
        "mutation_state": "none",
        "route": "full analyze+solve route",
        "detail.reason": "native worker failed",
        "native_evidence": "`Job::validate`, `src/nesting/nesting-ffi.cpp:299`; `applyNestingPlacements`, `src/nesting/nesting-document.cpp:3232`",
        "oracle": "P9:nest.apply:command-service:solver-failed:native worker failed",
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
        "native_evidence": "`Job::validate`, `src/nesting/nesting-ffi.cpp:299`; `applyNestingPlacements`, `src/nesting/nesting-document.cpp:3232`",
        "oracle": "P9:nest.apply:command-service:internal-error:unexpected service exception; rollback before return",
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
        "evidence_route": "P9:nest.apply:native:native successful outcome",
        "evidence_status": "required-unimplemented"
      },
      {
        "native_branch": "native unchanged outcome when applicable",
        "code": null,
        "retryable": false,
        "mutation_state": "none",
        "data_variant": "unchanged",
        "evidence_route": "P9:nest.apply:native:native unchanged outcome when applicable",
        "evidence_status": "required-unimplemented"
      },
      {
        "native_branch": "read-only computed preparation",
        "code": null,
        "retryable": false,
        "mutation_state": "none",
        "data_variant": "computed-dry-run",
        "evidence_route": "P9:nest.apply:native:read-only computed preparation",
        "evidence_status": "required-unimplemented"
      }
    ]
  }
})m3").as_object()};
}
std::vector<PackageCommand> nest_commands()
{
    std::vector<PackageCommand> out;
    out.push_back({ActionSpec{.name="nest.contour-set", .mode="G", .summary="Set contour bindings for the selected nesting targets.", .canonical_id="nest.contour-set", .handler=production_unavailable_action, .version=2, .effects="document-edit", .target_policy="G", .undo_policy="one-step", .dry_run_grade="computed", .cancellation_boundary="before-native-publication", .input=&m3_0}, "nest.contour-set", "document-edit", "G", {}, {{"type","object"},{"properties",boost::json::object{}},{"additionalProperties",false}}, {"slice-unavailable"}, {}});
    out.push_back({ActionSpec{.name="nest.contour-release", .mode="G", .summary="Release contour bindings for the selected nesting targets.", .canonical_id="nest.contour-release", .handler=production_unavailable_action, .version=2, .effects="document-edit", .target_policy="G", .undo_policy="one-step", .dry_run_grade="computed", .cancellation_boundary="before-native-publication", .input=&m3_1}, "nest.contour-release", "document-edit", "G", {}, {{"type","object"},{"properties",boost::json::object{}},{"additionalProperties",false}}, {"slice-unavailable"}, {}});
    out.push_back({ActionSpec{.name="nest.analyze", .mode="Q", .summary="Analyze selected objects for nesting.", .canonical_id="nest.analyze", .handler=production_unavailable_action, .version=2, .effects="read-only", .target_policy="Q", .undo_policy="none", .dry_run_grade="computed", .cancellation_boundary="before-native-publication", .input=&m3_2}, "nest.analyze", "read-only", "Q", {}, {{"type","object"},{"properties",boost::json::object{}},{"additionalProperties",false}}, {"slice-unavailable"}, {}});
    out.push_back({ActionSpec{.name="nest.solve", .mode="Q", .summary="Solve a nesting layout for the selected objects.", .canonical_id="nest.solve", .handler=production_unavailable_action, .version=2, .effects="read-only", .target_policy="Q", .undo_policy="none", .dry_run_grade="computed", .cancellation_boundary="before-native-publication", .input=&m3_3}, "nest.solve", "read-only", "Q", {}, {{"type","object"},{"properties",boost::json::object{}},{"additionalProperties",false}}, {"slice-unavailable"}, {}});
    out.push_back({ActionSpec{.name="nest.apply", .mode="G", .summary="Apply a nesting solution to the document.", .canonical_id="nest.apply", .handler=production_unavailable_action, .version=2, .effects="document-edit", .target_policy="G", .undo_policy="one-step", .dry_run_grade="computed", .cancellation_boundary="before-native-publication", .input=&m3_4}, "nest.apply", "document-edit", "G", {}, {{"type","object"},{"properties",boost::json::object{}},{"additionalProperties",false}}, {"slice-unavailable"}, {}});
    out[0].example = boost::json::parse(R"m3({
  "payload-id": "shape1",
  "contour-id": "contour1"
})m3").as_object();
    out[0].result_data = boost::json::parse(R"m3({
  "oneOf": [
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "success"
        },
        "binding-ids": {
          "type": "array",
          "items": {
            "type": "string"
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "payload-ids": {
          "type": "array",
          "items": {
            "type": "string"
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "contour-ids": {
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
        "binding-ids",
        "payload-ids",
        "contour-ids"
      ],
      "additionalProperties": false
    },
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "unchanged"
        },
        "binding-ids": {
          "type": "array",
          "items": {
            "type": "string"
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "payload-ids": {
          "type": "array",
          "items": {
            "type": "string"
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "contour-ids": {
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
        "binding-ids",
        "payload-ids",
        "contour-ids"
      ],
      "additionalProperties": false
    },
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "computed-dry-run"
        },
        "binding-ids": {
          "type": "array",
          "items": {
            "type": "string"
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "payload-ids": {
          "type": "array",
          "items": {
            "type": "string"
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "contour-ids": {
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
        "binding-ids",
        "payload-ids",
        "contour-ids"
      ],
      "additionalProperties": false
    }
  ],
  "description": "Data only; common va-studio.cli-result/1 envelope authoritative. Variant selected by status/dry_run; any duplicated fields must equal envelope. Preview IDs never claim live publication."
})m3").as_object();
    out[0].errors = {"request-too-large","repeated-key","invalid-utf8","malformed-json","invalid-argument","no-document","stale-document","stale-revision","cancelled","unknown-id","transaction-unavailable","duplicate-id","unsupported-target","document-read-only","internal-error"};
    out[1].example = boost::json::parse(R"m3({
  "ids": [
    "shape1"
  ]
})m3").as_object();
    out[1].result_data = boost::json::parse(R"m3({
  "oneOf": [
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "success"
        },
        "binding-ids": {
          "type": "array",
          "items": {
            "type": "string"
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "payload-ids": {
          "type": "array",
          "items": {
            "type": "string"
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "contour-ids": {
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
        "binding-ids",
        "payload-ids",
        "contour-ids"
      ],
      "additionalProperties": false
    },
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "unchanged"
        },
        "binding-ids": {
          "type": "array",
          "items": {
            "type": "string"
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "payload-ids": {
          "type": "array",
          "items": {
            "type": "string"
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "contour-ids": {
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
        "binding-ids",
        "payload-ids",
        "contour-ids"
      ],
      "additionalProperties": false
    },
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "computed-dry-run"
        },
        "binding-ids": {
          "type": "array",
          "items": {
            "type": "string"
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "payload-ids": {
          "type": "array",
          "items": {
            "type": "string"
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "contour-ids": {
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
        "binding-ids",
        "payload-ids",
        "contour-ids"
      ],
      "additionalProperties": false
    }
  ],
  "description": "Data only; common va-studio.cli-result/1 envelope authoritative. Variant selected by status/dry_run; any duplicated fields must equal envelope. Preview IDs never claim live publication."
})m3").as_object();
    out[1].errors = {"request-too-large","repeated-key","invalid-utf8","malformed-json","invalid-argument","no-document","stale-document","stale-revision","cancelled","unknown-id","transaction-unavailable","no-eligible-targets","document-read-only","internal-error"};
    out[2].example = boost::json::parse(R"m3({
  "ids": [
    "shape1"
  ],
  "page": 1
})m3").as_object();
    out[2].result_data = boost::json::parse(R"m3({
  "oneOf": [
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "success"
        },
        "analysis-token": {
          "type": [
            "string",
            "null"
          ]
        },
        "requested-copies": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "id": {
                "type": "string"
              },
              "count": {
                "type": "integer",
                "minimum": 1
              }
            },
            "required": [
              "id",
              "count"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "recovery": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "id": {
                "type": "string"
              },
              "mode": {
                "enum": [
                  "native",
                  "repaired",
                  "conservative-hull",
                  "conservative-bounds"
                ]
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
              "source-sha256": {
                "type": "string",
                "pattern": "^[0-9a-f]{64}$"
              },
              "collision-sha256": {
                "type": "string",
                "pattern": "^[0-9a-f]{64}$"
              }
            },
            "required": [
              "id",
              "mode",
              "bounds",
              "source-sha256",
              "collision-sha256"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "resource-bytes": {
          "type": "integer",
          "minimum": 0
        },
        "sheet-bounds": {
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
        }
      },
      "required": [
        "variant",
        "analysis-token",
        "requested-copies",
        "recovery",
        "resource-bytes",
        "sheet-bounds"
      ],
      "additionalProperties": false
    },
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "unchanged"
        },
        "analysis-token": {
          "type": [
            "string",
            "null"
          ]
        },
        "requested-copies": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "id": {
                "type": "string"
              },
              "count": {
                "type": "integer",
                "minimum": 1
              }
            },
            "required": [
              "id",
              "count"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "recovery": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "id": {
                "type": "string"
              },
              "mode": {
                "enum": [
                  "native",
                  "repaired",
                  "conservative-hull",
                  "conservative-bounds"
                ]
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
              "source-sha256": {
                "type": "string",
                "pattern": "^[0-9a-f]{64}$"
              },
              "collision-sha256": {
                "type": "string",
                "pattern": "^[0-9a-f]{64}$"
              }
            },
            "required": [
              "id",
              "mode",
              "bounds",
              "source-sha256",
              "collision-sha256"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "resource-bytes": {
          "type": "integer",
          "minimum": 0
        },
        "sheet-bounds": {
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
        }
      },
      "required": [
        "variant",
        "analysis-token",
        "requested-copies",
        "recovery",
        "resource-bytes",
        "sheet-bounds"
      ],
      "additionalProperties": false
    },
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "computed-dry-run"
        },
        "analysis-token": {
          "type": "null"
        },
        "requested-copies": {
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
              "count": {
                "type": "integer",
                "minimum": 1
              }
            },
            "required": [
              "id",
              "count"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "recovery": {
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
              "mode": {
                "enum": [
                  "native",
                  "repaired",
                  "conservative-hull",
                  "conservative-bounds"
                ]
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
              "source-sha256": {
                "type": "string",
                "pattern": "^[0-9a-f]{64}$"
              },
              "collision-sha256": {
                "type": "string",
                "pattern": "^[0-9a-f]{64}$"
              }
            },
            "required": [
              "id",
              "mode",
              "bounds",
              "source-sha256",
              "collision-sha256"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "resource-bytes": {
          "type": "integer",
          "minimum": 0
        },
        "sheet-bounds": {
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
        }
      },
      "required": [
        "variant",
        "analysis-token",
        "requested-copies",
        "recovery",
        "resource-bytes",
        "sheet-bounds"
      ],
      "additionalProperties": false
    }
  ],
  "description": "Data only; common va-studio.cli-result/1 envelope authoritative. Variant selected by status/dry_run; any duplicated fields must equal envelope. Preview IDs never claim live publication."
})m3").as_object();
    out[2].errors = {"request-too-large","repeated-key","invalid-utf8","malformed-json","invalid-argument","no-document","stale-document","stale-revision","cancelled","unknown-id","token-capacity","session-required","invalid-input","unsupported-target","engine-limit","analysis-failed","internal-error"};
    out[3].example = boost::json::parse(R"m3({
  "analysis-token": "token-example",
  "iterations": 1000
})m3").as_object();
    out[3].result_data = boost::json::parse(R"m3({
  "oneOf": [
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "success"
        },
        "solution-token": {
          "type": [
            "string",
            "null"
          ]
        },
        "engine": {
          "const": "native"
        },
        "iterations": {
          "type": "integer",
          "minimum": 0
        },
        "stop-reason": {
          "enum": [
            "work-limit",
            "completed",
            "time-limit"
          ]
        },
        "deterministic": {
          "type": "boolean"
        },
        "placements": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "id": {
                "type": "string"
              },
              "copy": {
                "type": "integer",
                "minimum": 0
              },
              "affine": {
                "type": "array",
                "items": {
                  "type": "number"
                },
                "minItems": 6,
                "maxItems": 6
              }
            },
            "required": [
              "id",
              "copy",
              "affine"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "unplaced": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "id": {
                "type": "string"
              },
              "copy": {
                "type": "integer",
                "minimum": 0
              }
            },
            "required": [
              "id",
              "copy"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "utilization": {
          "type": "number",
          "minimum": 0,
          "maximum": 100,
          "description": "Percent 0..100; native SolveMetrics::utilization_percent."
        }
      },
      "required": [
        "variant",
        "solution-token",
        "engine",
        "iterations",
        "stop-reason",
        "deterministic",
        "placements",
        "unplaced",
        "utilization"
      ],
      "additionalProperties": false,
      "allOf": [
        {
          "if": {
            "properties": {
              "stop-reason": {
                "const": "time-limit"
              }
            },
            "required": [
              "stop-reason"
            ]
          },
          "then": {
            "properties": {
              "deterministic": {
                "const": false
              }
            }
          }
        },
        {
          "if": {
            "properties": {
              "stop-reason": {
                "const": "work-limit"
              }
            },
            "required": [
              "stop-reason"
            ]
          },
          "then": {
            "properties": {
              "deterministic": {
                "const": true
              }
            }
          }
        }
      ]
    },
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "unchanged"
        },
        "solution-token": {
          "type": [
            "string",
            "null"
          ]
        },
        "engine": {
          "const": "native"
        },
        "iterations": {
          "type": "integer",
          "minimum": 0
        },
        "stop-reason": {
          "enum": [
            "work-limit",
            "completed",
            "time-limit"
          ]
        },
        "deterministic": {
          "type": "boolean"
        },
        "placements": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "id": {
                "type": "string"
              },
              "copy": {
                "type": "integer",
                "minimum": 0
              },
              "affine": {
                "type": "array",
                "items": {
                  "type": "number"
                },
                "minItems": 6,
                "maxItems": 6
              }
            },
            "required": [
              "id",
              "copy",
              "affine"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "unplaced": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "id": {
                "type": "string"
              },
              "copy": {
                "type": "integer",
                "minimum": 0
              }
            },
            "required": [
              "id",
              "copy"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "utilization": {
          "type": "number",
          "minimum": 0,
          "maximum": 100,
          "description": "Percent 0..100; native SolveMetrics::utilization_percent."
        }
      },
      "required": [
        "variant",
        "solution-token",
        "engine",
        "iterations",
        "stop-reason",
        "deterministic",
        "placements",
        "unplaced",
        "utilization"
      ],
      "additionalProperties": false,
      "allOf": [
        {
          "if": {
            "properties": {
              "stop-reason": {
                "const": "time-limit"
              }
            },
            "required": [
              "stop-reason"
            ]
          },
          "then": {
            "properties": {
              "deterministic": {
                "const": false
              }
            }
          }
        },
        {
          "if": {
            "properties": {
              "stop-reason": {
                "const": "work-limit"
              }
            },
            "required": [
              "stop-reason"
            ]
          },
          "then": {
            "properties": {
              "deterministic": {
                "const": true
              }
            }
          }
        }
      ]
    },
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "computed-dry-run"
        },
        "solution-token": {
          "type": "null"
        },
        "engine": {
          "const": "native"
        },
        "iterations": {
          "type": "integer",
          "minimum": 0
        },
        "stop-reason": {
          "enum": [
            "work-limit",
            "completed",
            "time-limit"
          ]
        },
        "deterministic": {
          "type": "boolean"
        },
        "placements": {
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
              "copy": {
                "type": "integer",
                "minimum": 0
              },
              "affine": {
                "type": "array",
                "items": {
                  "type": "number"
                },
                "minItems": 6,
                "maxItems": 6
              }
            },
            "required": [
              "id",
              "copy",
              "affine"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "unplaced": {
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
              "copy": {
                "type": "integer",
                "minimum": 0
              }
            },
            "required": [
              "id",
              "copy"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "utilization": {
          "type": "number",
          "minimum": 0,
          "maximum": 100,
          "description": "Percent 0..100; native SolveMetrics::utilization_percent."
        }
      },
      "required": [
        "variant",
        "solution-token",
        "engine",
        "iterations",
        "stop-reason",
        "deterministic",
        "placements",
        "unplaced",
        "utilization"
      ],
      "additionalProperties": false,
      "allOf": [
        {
          "if": {
            "properties": {
              "stop-reason": {
                "const": "time-limit"
              }
            },
            "required": [
              "stop-reason"
            ]
          },
          "then": {
            "properties": {
              "deterministic": {
                "const": false
              }
            }
          }
        },
        {
          "if": {
            "properties": {
              "stop-reason": {
                "const": "work-limit"
              }
            },
            "required": [
              "stop-reason"
            ]
          },
          "then": {
            "properties": {
              "deterministic": {
                "const": true
              }
            }
          }
        }
      ]
    }
  ],
  "description": "Data only; common va-studio.cli-result/1 envelope authoritative. Variant selected by status/dry_run; any duplicated fields must equal envelope. Preview IDs never claim live publication."
})m3").as_object();
    out[3].errors = {"request-too-large","repeated-key","invalid-utf8","malformed-json","invalid-argument","no-document","stale-document","stale-revision","cancelled","invalid-token","stale-plan","stale-dependency","session-required","token-capacity","solver-failed","engine-limit","internal-error"};
    out[4].example = boost::json::parse(R"m3({
  "solution-token": "solution-example"
})m3").as_object();
    out[4].result_data = boost::json::parse(R"m3({
  "oneOf": [
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "success"
        },
        "source-copy": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "source": {
                "type": "string"
              },
              "copy": {
                "type": "integer",
                "minimum": 0
              },
              "output-id": {
                "type": "string"
              },
              "affine": {
                "type": "array",
                "items": {
                  "type": "number"
                },
                "minItems": 6,
                "maxItems": 6
              }
            },
            "required": [
              "source",
              "copy",
              "output-id",
              "affine"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "unplaced": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "id": {
                "type": "string"
              },
              "copy": {
                "type": "integer",
                "minimum": 0
              }
            },
            "required": [
              "id",
              "copy"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "placements": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "id": {
                "type": "string"
              },
              "copy": {
                "type": "integer",
                "minimum": 0
              },
              "affine": {
                "type": "array",
                "items": {
                  "type": "number"
                },
                "minItems": 6,
                "maxItems": 6
              }
            },
            "required": [
              "id",
              "copy",
              "affine"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 100000
        }
      },
      "required": [
        "variant",
        "source-copy",
        "unplaced",
        "placements"
      ],
      "additionalProperties": false
    },
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "unchanged"
        },
        "source-copy": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "source": {
                "type": "string"
              },
              "copy": {
                "type": "integer",
                "minimum": 0
              },
              "output-id": {
                "type": "string"
              },
              "affine": {
                "type": "array",
                "items": {
                  "type": "number"
                },
                "minItems": 6,
                "maxItems": 6
              }
            },
            "required": [
              "source",
              "copy",
              "output-id",
              "affine"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "unplaced": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "id": {
                "type": "string"
              },
              "copy": {
                "type": "integer",
                "minimum": 0
              }
            },
            "required": [
              "id",
              "copy"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "placements": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "id": {
                "type": "string"
              },
              "copy": {
                "type": "integer",
                "minimum": 0
              },
              "affine": {
                "type": "array",
                "items": {
                  "type": "number"
                },
                "minItems": 6,
                "maxItems": 6
              }
            },
            "required": [
              "id",
              "copy",
              "affine"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 100000
        }
      },
      "required": [
        "variant",
        "source-copy",
        "unplaced",
        "placements"
      ],
      "additionalProperties": false
    },
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "computed-dry-run"
        },
        "source-copy": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "source": {
                "type": [
                  "string",
                  "null"
                ],
                "description": "null for prospective output; existing source IDs remain strings"
              },
              "copy": {
                "type": "integer",
                "minimum": 0
              },
              "output-id": {
                "type": [
                  "string",
                  "null"
                ],
                "description": "null for prospective output; existing source IDs remain strings"
              },
              "affine": {
                "type": "array",
                "items": {
                  "type": "number"
                },
                "minItems": 6,
                "maxItems": 6
              }
            },
            "required": [
              "source",
              "copy",
              "output-id",
              "affine"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "unplaced": {
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
              "copy": {
                "type": "integer",
                "minimum": 0
              }
            },
            "required": [
              "id",
              "copy"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "placements": {
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
              "copy": {
                "type": "integer",
                "minimum": 0
              },
              "affine": {
                "type": "array",
                "items": {
                  "type": "number"
                },
                "minItems": 6,
                "maxItems": 6
              }
            },
            "required": [
              "id",
              "copy",
              "affine"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 100000
        }
      },
      "required": [
        "variant",
        "source-copy",
        "unplaced",
        "placements"
      ],
      "additionalProperties": false
    }
  ],
  "description": "Data only; common va-studio.cli-result/1 envelope authoritative. Variant selected by status/dry_run; any duplicated fields must equal envelope. Preview IDs never claim live publication."
})m3").as_object();
    out[4].errors = {"request-too-large","repeated-key","invalid-utf8","malformed-json","invalid-argument","no-document","stale-document","stale-revision","cancelled","invalid-token","stale-plan","stale-dependency","session-required","invalid-placement","partial-result","publication-failed","document-read-only","invalid-input","unsupported-target","engine-limit","analysis-failed","solver-failed","internal-error"};
    out[0].policy = "explicit-role-pair"; out[0].spec.target_policy = "explicit-role-pair";
    out[1].policy = "collective-geometry"; out[1].spec.target_policy = "collective-geometry";
    out[2].policy = "collective-geometry"; out[2].spec.target_policy = "collective-geometry";
    out[3].policy = "collective-geometry"; out[3].spec.target_policy = "collective-geometry";
    out[4].policy = "collective-geometry"; out[4].spec.target_policy = "collective-geometry";
    out[0].warnings = {};
    out[1].warnings = {};
    out[2].warnings = {"geometry-repaired","conservative-recovery"};
    out[3].warnings = {};
    out[4].warnings = {"geometry-repaired","conservative-recovery"};
    return out;
}
}
