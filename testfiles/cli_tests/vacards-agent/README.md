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
