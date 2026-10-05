# VACards agent CLI cases

One JSON file per case, one directory per case group. `vacards-agent-cli-<group>`
in ctest runs `testfiles/vacards-agent-cli-test.py <inkcape> <group dir>`; a group
directory with no `*.json` prints `SKIP: no cases in <dir>` and exits 77 (Skipped).

## Placeholders

Replaced by plain string substitution in every string of a case (actions, args,
input and recursively every string value inside expectations):

| Token         | Value                                                     |
|---------------|-----------------------------------------------------------|
| `{WORK}`      | the case's temporary, absolute work directory             |
| `{RESULT}`    | `{WORK}/result.jsonl`                                     |
| `{TESTCASES}` | `testfiles/cli_tests/testcases`                           |
| `{FIXTURES}`  | `testfiles/cli_tests/vacards-agent/fixtures`              |

## Case keys

All are optional except `actions`.

| Key                            | Meaning                                                                                  |
|--------------------------------|------------------------------------------------------------------------------------------|
| `description`                  | free text                                                                                |
| `input`                        | file copied into `{WORK}`; the copy is the first positional argument                       |
| `actions`                      | passed as `--actions=<actions>`                                                          |
| `args`                         | extra arguments appended after `--actions`                                               |
| `timeout`                      | seconds, default 120; a timeout is a case failure                                        |
| `records_from`                 | `file` \| `stdout` \| `stderr`; defaults to `file` when `{RESULT}` appears in `actions`, else `stderr` |
| `expect_exit`                  | expected process exit status, default 0                                                   |
| `expect_records`               | list of expected record objects (subset match, see below); absent means "not checked"      |
| `expect_records_text_contains` | substrings that must occur in the concatenated raw record lines                            |
| `expect_stdout_contains`       | substrings that must occur in stdout                                                      |
| `expect_stderr_contains`       | substrings that must occur in stderr                                                      |
| `expect_stderr_not_contains`   | substrings that must not occur in stderr                                                  |
| `expect_files`                 | `{ "relative/path": {"exists": bool, "contains": [..], "not_contains": [..]} }` vs `{WORK}` |
| `expect_same_files`            | list of `[A, B]` relative paths that must both exist under `{WORK}` with identical bytes      |

## Record matching

Records are the bare lines of `{WORK}/result.jsonl`, or the
`VASTUDIO-RESULT `-prefixed stdout/stderr lines with the prefix removed. Each line
must parse as JSON; every record's `schema` must be `va-studio.cli-result/1` and
its `seq` values must be `1, 2, 3, ...` in order. `expect_records` requires the
same length, and element `i` matches when:

* expected is an object: every expected key exists and matches (extra actual keys are fine);
* expected is a list: same length and elementwise match;
* a bool matches only a bool with the same type and value (`true != 1`, `1 != true`);
* otherwise: exact equality, except that two numbers (int/float, never a bool) match when
  `abs(expected - actual) <= 1e-6 * max(1, abs(expected), abs(actual))`.

- `"stdin"`: text fed to the app on standard input (e.g. `--shell` lines); placeholders apply. Omit `"actions"` to run without `--actions`.

## Boolean failure semantics

`vacards-boolean` runs the shared Boolean Assist body (one Undo step). When the operation is refused (an
incompatible operand) or fails (it does not leave exactly one path, or the only remaining path is an untouched
operand), it cancels its changes with `DocumentUndo::cancel` and reports `rejected`/`failed`; the canceled
transaction also clears the Redo history. An empty result (for example an intersection of disjoint shapes, or a
bottom-minus-rest fully covered by the rest) is reverted and reported as a failure, unlike stock
  Path > Intersection, which commits an empty path. Cases assert this with `vacards-options:halt-on-error=false`
and a following export that still contains every operand. Plain objects with an empty outline (a path with no
`d`, or text with no characters) are excluded with reason `empty-geometry`, distinct from the `not-a-shape`
reason for a type the operation does not support.


## M2 file contract and independent oracles

The `vacards-agent-session/files` group adds session and one-shot cases while
preserving every existing legacy and M1 case. Cases are authored from
the M2 plan and the descriptors; application output never
supplies expected geometry, pixels, profile bytes, or success/refusal status.
The authoritative case ledger is `files/support/inventory.json`. Missing six
commands, cases, enum values, registrations, skipped native tests or missing
service evidence keep the gate failing. `native-required` means unqualified
until a real service-path test proves that cell; it is not an N/A or a pass.

All M2 fixtures are hash-checked against `fixtures/m2/manifest.json` before each
case and copied to `{WORK}/inputs`. `files/support/fixtures.py` documents exact
literal geometry and native fixture authoring. Native cjpeg and Little CMS are
only needed to regenerate fixtures; checked-in hashes pin delivered bytes.
The CDR and its rendered reference come from the existing upstream CLI fixtures.
PNG uses an independent bounded standard-library decoder with CRC/filter checks.
TIFF reopens through native libtiff; PDF reopens through Poppler (`pdfinfo`,
`pdftoppm`, `pdffonts`). Missing readers fail, including on Windows. ICC pixels
use a separate Little CMS transform and exact embedded-profile byte comparison.
Python requires no third-party packages.

M2 case additions:

| Key | Purpose |
| --- | --- |
| `setup`, step `before` | Controlled copy/write/mkdir/symlink/unlink/replacement/same-size mutation, only inside the disposable root |
| `{"$ref":"open.document_id"}` | Resolve a prior result's opaque identity/revision; never use it to invent expected geometry or status |
| `expect_not_equal` | Assert a fresh identity differs from the previous incarnation |
| `file_oracles` | Check absence, pinned hashes, bytes reported by the CLI, SVG payload/embedding, raster dimensions/pixels/ICC, PDF pages/fonts |
| `profile_override` | A per-case disposable ICC override, with explicit grants, for deterministic default-profile checks |

The harness separately enforces publication status/state/persisted truth and
checks published destination hashes/versions against disk. Dry-run/lifecycle
requests never publish; save must confirm its current revision and clear dirty.
Cases isolate profiles and reject preferences/recent/autosave writes, preserve
inputs, compare source queries before/after export, and reopen saved SVG.

Run the acceptance gate (development repository only) directly; it owns `build/.vacards-build-lock` for each CTest step.
Never wrap it in the same lock. The gate budgets all adapter/native groups and
records commands, timings, binary hashes, JUnit and JSON. Every legacy group
remains required; existing empty groups still fail acceptance.

Real native fault/cancellation/history/network and installed-platform domains
are listed in the ledger. A reviewed required-service
evidence file must use schema
`p9-m2-service-evidence/1`, identify the exact platform/source, and map each
applicable domain to an executed JUnit testcase, command/exit, measured time,
log/JUnit/binary SHA-256 and independently declared oracle provenance. Boolean
claims, placeholders, duplicates, skips and mismatched identities are refused.
This is a qualification input, not permission to add production fault backdoors.
Windows mapped-network/reparse/ACL/sharing evidence and a Mac mounted-network
probe remain required on their corresponding hosts. A Mac gate cannot establish
Windows or installed-package acceptance.

### M3 outcomes

`vacards-cli-m3-cases.py --author --evidence PATH` authors independent analytic
fixtures and the exact command cases. It never consumes production expected
outputs. The `m3-*.json` session bundles have schema `p9-m3-cases/1`, and contain
nonempty independently executed `cases`, each with a unique `id`. The existing
session runner reports every inner `M3-PASS`/`M3-FAIL` and only passes a bundle if
all its children pass. The eight registered groups are selection, history,
geometry, bitmap, explode, clip, nest and workflow. Missing/zero/skipped cases
fail the M3 gate. Authoring/enum coverage is not execution coverage.

For individual JSON/JUnit receipts run `vacards-cli-m3-cases.py --run BINARY
--evidence FRESH_PATH` under the shared build lock and pinned test environment.
The acceptance gate's `--milestone M3` mode adds those eight wire groups and the eight
native M3 targets; M4 additionally requires MCP/docs and client/install evidence.
Build-30 deferrals remain historical and cannot authorize an M3 passing gate.
Use the gate's `--packet-evidence` option for the platform ledger directory.

The development repository's debt reconciler covers the immutable original 147 IDs and every
new M3 error branch exactly once. Pending rows fail. Executed rows need actual
branch observations, child case/JUnit/log identity and preservation evidence;
reviewed removed-inapplicable rows need hashed before/after descriptors and
independent reachability rationale, and never count as passes. Defensive wire
ungroup, empty release, and production Completed obligations have explicit
reviewed dispositions. Parser admission/source audits never count as actual
filesystem-open interception. Native C++ tests
remain with their native owners; `native-oracles.json` is a specification, not a
receipt. Preview/cancel includes XML, ordered selection, both revisions, Undo,
Redo, tokens and settled outcome; the wire checks observable history/session
state, while exact XML and native settlement require owner case evidence.
