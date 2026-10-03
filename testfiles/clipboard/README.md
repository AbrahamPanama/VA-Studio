# External-process clipboard paste harness (milestone A / E-series)

This directory owns the **separate-process clipboard protocol** for external text
paste. It exists because the in-process `test_text-paste` suite publishes through
its own GDK connection: on macOS that clears and replaces the user's real
clipboard and leaves GDK reporting `is_local() == true`, so the application's
remote-paste branch is never exercised. Here the clipboard owner is a genuinely
different OS process.

| File | Role |
| --- | --- |
| `macos/clipboard-publisher.mm` | test-only native `NSPasteboard` owner (`vacards-clipboard-publisher`) |
| `fixtures/manifest.json` + `fixtures/bytes/*` | synthetic fixtures and externally authored expected values |
| `run-external-paste.py` | driver: lease, snapshot/restore, publication, receiver launch, evidence, `--protocol-self-test` |
| `run-receiver-skip-aware.py` | CTest wrapper for the standalone receiver: all-skipped exit 0 → 77, never a false pass |
| `../../src/text-paste-external-test.cpp` | per-case receiver GTest binary (`test_text-paste-external`) |
| `CMakeLists.txt` | publisher target, `vacards-clipboard-external` and `vacards-clipboard-protocol-self-test` registrations |

## Running it

```sh
# Build (integration owner wires testfiles/CMakeLists.txt first; see below)
cmake --build <build> --target test_text-paste-external vacards-clipboard-publisher -j 2

# Default ctest: SKIPS. It never touches the clipboard and never reports pass.
ctest --test-dir <build> -R vacards-clipboard-external -V

# Pure protocol self-test: no clipboard, no publisher, no receiver. Safe anytime.
python3 testfiles/clipboard/run-external-paste.py --protocol-self-test

# Qualification run: explicit consent to replace the user's clipboard.
python3 testfiles/clipboard/run-external-paste.py \
    --publisher <build>/bin/vacards-clipboard-publisher \
    --receiver  <build>/bin/test_text-paste-external \
    --fixtures  testfiles/clipboard/fixtures \
    --session-root <build>/testfiles/clipboard/sessions \
    --evidence  <build>/testfiles/clipboard/clipboard-evidence \
    --allow-clipboard-takeover

# Safe planning/validation path: no clipboard access, prints the resolved plan.
python3 testfiles/clipboard/run-external-paste.py --dry-run ...same paths...
```

### Opt-in semantics and exit codes

* `--allow-clipboard-takeover` or `VACARDS_CLIP_ALLOW=1` is required before any
  clipboard read or write. Without it every selected case is recorded as
  `env-blocked:not-opted-in`, `summary.json` records `"opt_in": false`, no session
  directory is created, and the driver exits **77** (`SKIP_RETURN_CODE 77` on the
  CTest test → "Skipped", never green).
* With opt-in: **0** only when every selected case passed. Any `fail`,
  `env-blocked` or `not-run` outcome exits **1**. A qualification run can never
  turn "could not test" into a pass.
* Publisher exit codes: `2` usage, `3` lease not owned by the launching driver,
  `4` publication failed/not exact or restore write failed twice/unverified,
  `5` snapshot incomplete or unusable, `6` restore refused (ownership changed),
  `7` hold timeout.
* `run-receiver-skip-aware.py` exit codes: `0` at least one test executed and the
  receiver exited 0, `77` the receiver exited 0 but nothing executed (all tests
  skipped, or zero tests), `2` the wrapper could not verify a gtest report
  (never reported as a pass), otherwise the receiver's own exit code.
* `--protocol-self-test` exits 0 only when all seven pure protocol checks pass.

## Protocol

1. **Session + lease.** `<session-root>/<run-id>/` is mode 0700. The flock lease
   is taken on the **run-independent** `<session-root>/lease.lock` (0600) with
   `flock(LOCK_EX|LOCK_NB)` for the whole run, so two drivers with different run
   ids contend for the same file; `<session-root>/lease.holder.json` names the
   current holder. The publisher and receiver read the per-run
   `<session-root>/<run-id>/lease.json` (pid/nonce) from their `--session`
   argument, unchanged. A leftover `lease.json` from a reused `--run-id` is
   rotated to `lease.stale.<epoch>.json`. Publisher and receiver verify
   `lease.pid == getppid()` (`VACARDS_CLIP_REQUIRE_LEASE=0` disables this for
   manual probing).
2. **Snapshot before publish.** `vacards-clipboard-publisher --snapshot` copies
   every pasteboard type into `<session>/snapshot/` (blobs 0600, directory
   0700), recording per-type length and SHA-256. A type whose provider does not
   answer, or that exceeds 32 MiB (64 MiB total), marks the snapshot
   `complete:false`; the run then stops as an `env-blocked:snapshot-*` case whose
   reason keeps the publisher exit code and stderr tail (for example
   `snapshot-failed:publisher-exit-5:snapshot-incomplete:<stderr>`, or
   `snapshot-timeout` / `snapshot-spawn-failed:<error>`). There is **no flag that
   publishes after an incomplete snapshot**: `--allow-partial-snapshot` is
   rejected with exit 2 before any clipboard access, and the publisher's own
   `--allow-partial-snapshot` is never passed by the driver. Before the
   pasteboard is replaced, the driver also re-reads every snapshot blob and
   verifies its length and SHA-256 (`validate_snapshot_restore_data`): a
   captured-but-unrestorable snapshot refuses the run as
   `snapshot-unrestorable:<problem>,...` while the user's clipboard is still
   intact. `summary.json` records the refusal plus `snapshot_captured` /
   `snapshot_restorable`.
3. **Publication handshake.** The driver writes `pub/request.json`, spawns the
   publisher with `--publish --nonce N --hold-until pub/release`, and waits for
   the publisher's atomic `pub/ready.json` (tmp+rename) containing nonce, pid,
   requested UTIs, observed `[pasteboard types]`, `changeCount` and fixture
   SHA-256. Readiness is that artifact plus a live pid — never a fixed sleep.
4. **Receiver per case.** A fresh `test_text-paste-external` process reads
   `VACARDS_CLIP_SESSION/CASE/EXPECT/RESULT`, asserts `!clipboard->is_local()`,
   that the live formats contain the fixture `required_formats`, that the
   publisher pid differs from its own, and that the `ready.json` nonce matches.
   It then drives the real `win.paste` action (selector or Text-tool route) and
   writes `cases/<id>/result.json` atomically with the observed text hash,
   object count, Undo/Redo counts and every named check. It never writes the
   clipboard. A missing or unexecutable receiver is recorded explicitly as
   `env-blocked:receiver-missing:<path>` or
   `receiver-spawn-failed:<error>`, never as an uncaught traceback.
5. **Ownership-checked restore.** After the last case the driver restores only
   when the current `changeCount` still equals the last harness publication's.
   The publisher also re-checks `--expect-change-count` immediately before
   `clearContents`. **This is not race-free**: `NSPasteboard` offers no
   compare-and-swap, so a third party can still write in the small window between
   the final check and the write. That residual window is a documented
   limitation, not a guarantee. The restore prepares and hash-validates every
   blob before touching the pasteboard, retries a failed `writeObjects` once, and
   verifies that every requested type is present before reporting success. If the
   changeCount no longer matches, the driver records
   `restore-skipped:clipboard-ownership-lost` and never overwrites a newer user
   copy. If the user writes mid-run, remaining cases are
   `env-blocked:clipboard-changed` and the run stops.
6. **Snapshot retention on failed restore.** Snapshot blobs are deleted only
   after a **verified** restore, or when the pasteboard was never replaced /
   still holds a newer user copy. An **incomplete or unrestorable** captured
   snapshot is never treated as safe to delete: when no publication happened
   (`last_change_count is None`), the driver reports
   `restore-skipped:snapshot-incomplete` / `restore-skipped:snapshot-unrestorable`
   and keeps the session. On a failed, timed-out or unverifiable restore the
   driver keeps the 0700 session directory (blobs 0600), records
   `snapshot_retained: true` and `snapshot_dir` in `summary.json`, and prints the
   path to stderr so the user can recover manually. A `changeCount` change seen
   only *after* a failed write is not classified as a safe ownership loss (it may
   be the failed write itself): the driver stops without writing again and
   retains the snapshot. Treat a retained directory as sensitive and remove it
   after recovery.
7. **Cleanup on every path.** Publisher SIGTERM then SIGKILL; lease released;
   unless the snapshot is retained (point 6), session directory and blobs are
   unlinked. Logs and evidence contain lengths and SHA-256 only — never clipboard
   bytes.

Evidence: `<evidence>/<run-id>/results.jsonl` (one record per case with
requirement ID, source SHA, binary paths/hashes, OS/app version, publisher
formats, fixture hash, expected/actual, duration, artifact links, outcome),
`cases/<id>/{expect.json,publication.json,result.json,gtest.json,receiver.log,
publisher.log}` and `summary.json` (with `opt_in`, `counts` — `total`, `pass`,
`fail`, `env-blocked`, `not-run`, `executed`, `skipped` — `snapshot_complete`,
`snapshot_retained`, `snapshot_dir`, `restore`, `restore_attempts` and
`outcomes`). `<evidence>/LATEST` points at the newest run.

## Fixtures

Synthetic only; no clipboard content is ever committed. `manifest.json` carries,
per fixture: file + SHA-256 (the **primary** payload recorded in evidence),
macOS `publish`, `required_formats` (GTK MIME names the receiver must observe),
`transport_bytes`, `failure_class`, and `expect`. The macOS publication has two
mutually exclusive forms:

* legacy `publish.macos { "utis": [...], "as": "string"|"data" }` — every UTI
  carries the fixture's own file/sha256/bytes (E01–E07 keep exactly this form);
* extended `publish.macos.representations: [{ "uti", "file", "sha256", "bytes",
  "as", "stall"? }]` — each representation carries **its own** payload file,
  hash, byte count and `as` mode. `stall: true` declares a representation whose
  data is advertised through an `NSPasteboardItemDataProvider` that never calls
  the completion handler.

The publisher writes every declared representation with its own bytes
(`setData:forType:` / `setString:forType:`) and records the representation list
(uti, file, sha256, bytes, as, stall) in `ready.json`. The driver's
publication-exactness check requires every declared UTI to be observed, a
raw-data payload not to appear under the string spelling behind the manifest's
back, and every `stall` flag to be reported honestly. `transport_bytes` must equal
the sum of the representations' bytes plus one terminal NUL per `string`
representation (the macOS string path appends exactly one), and the fixture-level
file/sha256/bytes must be one of the representations.

`expect` fields:

| Field | Meaning |
| --- | --- |
| `text_sha256`, `text_bytes` | hash/length of the document text the case must observe |
| `text_utf8` | that text literally, when small enough (must match `text_bytes`) |
| `object_count`, `undo_steps` | new **text** objects and Undo entries the paste must add |
| `document_unchanged` | true for every non-pasting class |
| `require_nonempty_style` | pasted text must carry a real family and positive size |
| `import_object_id` | `import-object` only: element id that must appear and exists **only** in the SVG/object payload |
| `forbidden_text_utf8` | distinctive string (or list) from the **losing** representation that must never become document text |
| `max_elapsed_ms` | upper bound on one paste command's wall time; required when a representation stalls |

`expect.text_sha256`/`text_bytes` are bound to what the case must observe, and
the driver refuses to run a manifest that violates the binding:

* `success` — the document text is exactly the fixture bytes, so the oracle hash
  and length must equal the fixture `sha256`/`bytes` (and `text_utf8`, when
  present, must match `text_bytes`).
* `fallback-plain` — the fixture is the malformed/rich representation and the
  oracle describes the **complete plain alternative**, so a document change, at
  least one object and non-empty text are required; the receiver asserts the
  pasted text equals the complete declared alternative. `forbidden_text_utf8` is
  required and must not occur in the expected text.
* `import-object` — the SVG/object representation wins: a document change and at
  least one Undo step, but **zero** new text objects and empty expected text.
  `import_object_id` and `forbidden_text_utf8` are required; the id must occur in
  a published payload and the forbidden marker must not.
* `reject-no-mutation` / `abort-no-mutation` / `no-op` — the document must not
  change, so the oracle must be the unchanged (empty) document text: hash
  `e3b0c442…b855`, `text_bytes`/`object_count`/`undo_steps` zero and
  `document_unchanged: true`. A stalled representation must declare
  `max_elapsed_ms`, and a deadline oracle must declare at least one
  `stall: true` representation.

Discriminating markers are additionally bound to real bytes: every
`forbidden_text_utf8` must occur in one of the fixture's published payloads
(otherwise absence proves nothing) and must not occur in the expected text, and
`import_object_id` must occur in one of the payloads.

| Case | Fixture | Publication | Oracle |
| --- | --- | --- | --- |
| E01 | `E01_utf8_only` | only `public.utf8-plain-text` (string) | selector paste creates one editable text, exact bytes, 1 Undo |
| E02 | `E01_utf8_only` | same | Text tool with no edited object creates text, no silent no-op |
| E04 | `E04_terminated` | `public.utf8-plain-text` (string) | one OS-appended NUL accepted once, not in the document |
| E05 | `E05_plain` | `public.plain-text` (raw data) | same text without NUL gives the identical result |
| E06 | `E06_embedded_nul` | `public.plain-text` (raw data) | embedded NUL rejected, zero mutation, zero Undo |
| E07 | `E07_at_cap_with_terminator` | string | 65,536 bytes + NUL (65,537 transport) accepted, no truncation |
| E07 | `E07_at_cap_multibyte_end` | string | multibyte ending at the cap is not split |
| E07 | `E07_over_cap` | `public.plain-text` (raw data) | 65,537 bytes abort whole, no partial insert |
| E13 | `E13_object_priority_svg_plain` | `public.svg-image` (data) + `public.utf8-plain-text` (string) | SVG import wins: `e13-object-priority-marker` appears, no text object, `PLAIN-ALTERNATIVE-E13` never becomes text, 1 Undo |
| B15 | `B15_object_priority_svg_html` | `public.html` (data) + `public.svg-image` (data) | SVG wins over the rich route: `b15-object-priority-marker` appears, `HTML-RICH-ALTERNATIVE-B15` never becomes text |
| F13 | `F13_plain_svg_object_path` | `public.utf8-plain-text` (string) whose bytes are an SVG document | takes the object/SVG path (`f13-plain-svg-object-marker`), never a literal insert of the markup (`F13-PLAIN-SVG-SOURCE-TEXT` must not become text) |
| E08 | `E08_object_target_stall` | `public.svg-image` (data, `stall: true`) | lazy type never delivers: `win.paste` returns false, document unchanged, 0 Undo, `actual.elapsed_ms <= 8000` |
| R04 | `R04_fallback_plain_distinct` | malformed under-cap `public.html` + `public.rtf` + `public.utf8-plain-text` (string) | the complete plain alternative is pasted; neither `R04-MALFORMED-HTML-MARKER` nor `R04-MALFORMED-RTF-MARKER` is spliced in |

`public.plain-text` is used only for byte-exact raw fixtures: GDK then takes the
`dataForType:` path instead of `stringForType:`. The driver rejects a publication
whose observed UTIs include `public.utf8-plain-text` when raw data was requested
and no string representation was declared (GDK's requested-order match would
silently choose the string path).

## Protocol self-test (X01–X04 scope)

`python3 testfiles/clipboard/run-external-paste.py --protocol-self-test` runs
seven pure checks with no clipboard access at all: it never starts the publisher
or the receiver and never reads or writes `NSPasteboard`. It is registered as the
CTest test `vacards-clipboard-protocol-self-test` (no `RESOURCE_LOCK`, safe
alongside a real run; `fcntl` excludes Windows, hence `if(NOT WIN32)`).

| Check | Covers |
| --- | --- |
| `paths.session_and_run_independent_lease` | run-id session/evidence paths; the lease file is `<session-root>/lease.lock`, independent of the run id |
| `lease.two_processes_contend_on_one_lock` | two real subprocesses with different run ids contend for the same lock file; the second is refused |
| `wrapper.skip_exit_code_mapping` | `run-receiver-skip-aware.py` spawned with trivial Python children: all-skipped/zero-executed → 77, `[  SKIPPED ]` passed through, executed → 0, non-zero receiver exit propagated, unverifiable gtest report → 2 (never 0) |
| `manifest.representations_and_expect_hash_binding` | the shipped manifest validates; tampered fixture/representation bytes, mixed legacy+representations forms, bad transport sums, inconsistent `expect` bindings, missing discriminators and a stall without `max_elapsed_ms` are rejected; a legacy fixture converted to `representations` is accepted; the publication-exactness oracle reads the normalized representation list (including stall reporting) |
| `summary.accounting_and_truthful_opt_in` | `counts.executed`/`skipped` accounting; a refusal summary records `opt_in: false` and `env-blocked` |
| `restore.decision_with_injected_failures` | restore precheck/outcome state machine with injected publisher exits (write failure, retry, ownership change between attempts) and snapshot-retention decisions |
| `snapshot.refusal_and_retention` | an incomplete or unrestorable snapshot refuses publication; restore data is validated before publication (missing/size/hash mismatch, type without a blob); `last_change_count is None` keeps the incomplete/unrestorable status and retains the session |

The self-test covers only protocol logic; it cannot verify real pasteboard
behavior, which requires an opted-in qualification run.

## Implemented vs manual

* **Automated here:** E01, E02, E04, E05, E06, E07 (at cap, multibyte at cap,
  over cap) — all against a real native macOS publisher in a separate process.
  The `--protocol-self-test` checks above run without any clipboard access.
* **Authored, compiled, manifest-validated, NOT yet executed (no live clipboard
  in the round that added them):** E13 (SVG over plain), B15 (SVG over HTML), F13
  (plain-text SVG takes the object path), E08 (lazy object target stall with a
  bounded deadline) and R04 (malformed HTML+RTF → complete distinct plain
  fallback). They are registered in `CMakeLists.txt` and in the driver's
  `DEFAULT_CASES`, their fixtures/hashes/expectations validate in the protocol
  self-test, and both binaries compile — but no opted-in qualification run has
  exercised the real pasteboard for them yet. Treat them as unproven until such a
  run is archived.
* **Not implemented in this harness:** E03, E09, E12, E14, E15 (in-text caret
  routes, Unicode/paragraph matrix, save/reopen, action-route equivalence,
  concurrent-change aborts) and chunked (as opposed to stalled) producers. They
  need additional fixtures/routes.
* **Manual acceptance only (no safe automation in this checkout):** E10 (real
  Safari/Chrome copy needs Accessibility/Automation permission, a controlled
  page and browser versions), E11 (TextEdit/Pages/Word availability is
  machine-specific; absent apps must be recorded `not-tested`), the installed-app
  halves of E12/E14, and all Windows cells (no Windows host here). Record
  screenshots, app versions and the exact build identity; do not fabricate
  automation.
* **Baseline-red expectation:** on the pre-A2/A3 source, E01/E02 (UTF-8-only
  selector routing) and E07-at-cap (transport budget) are expected to fail.
  Archive the red log before product edits; a green run on the unfixed baseline
  is itself a harness defect.

## Limitations

1. `GdkClipboard` exposes no `changeCount`; the receiver therefore verifies
   locality, formats and publisher pid, while the driver (which can call the
   native publisher) enforces changeCount ownership before and after each case.
2. The receiver needs the recorded build flags of the existing suite; it only
   runs in a configured GUI build on macOS.
3. `testfiles/CMakeLists.txt` cannot be edited by this harness: the integration
   owner must add `text-paste-external-test` to `TEST_SOURCES` (target
   `test_text-paste-external`), `add_subdirectory(clipboard)` next to
   `add_subdirectory(selection-contract)`, and (optionally, already done from
   inside the subdirectory) `add_dependencies(tests vacards-clipboard-publisher)`.
   The subdirectory fails configuration with a clear message when the
   `TEST_SOURCES` entry is missing. Exact lines:

   ```cmake
   # in TEST_SOURCES, alphabetically next to text-paste-test (line ~153)
       text-paste-external-test

   # next to add_subdirectory(selection-contract) (line ~562)
   add_subdirectory(clipboard)
   ```

   **Required: the receiver must not be registered by the generic
   `TEST_SOURCES` loop** (the loop's plain `add_test(NAME ${testname} COMMAND
   ${testname})` at line ~242 lets a `GTEST_SKIP()`-only run exit 0 and be
   reported as Passed). Keep the loop's shared `ENVIRONMENT` line, but for this
   one target replace the command with the skip-aware wrapper and add the
   properties below (do not duplicate the test name):

   ```cmake
   # in the TEST_SOURCES loop (~line 242), instead of add_test(NAME ${testname} COMMAND ${testname}):
   if(test_source STREQUAL "text-paste-external-test")
       add_test(NAME ${testname}
                COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/clipboard/run-receiver-skip-aware.py"
                        --receiver "$<TARGET_FILE:${testname}>"
                        --gtest-output "${CMAKE_CURRENT_BINARY_DIR}/${testname}-gtest.json")
       set_tests_properties(${testname} PROPERTIES
           SKIP_RETURN_CODE 77
           FAIL_REGULAR_EXPRESSION "\\[ +SKIPPED +\\]" "\\[==========\\] Running 0 tests"
           RESOURCE_LOCK os_clipboard
           RUN_SERIAL TRUE
           TIMEOUT 300)
   else()
       add_test(NAME ${testname} COMMAND ${testname})
   endif()
   ```

   `SKIP_RETURN_CODE 77` makes the wrapper's all-skipped mapping a CTest
   "Skipped" (never Passed). `FAIL_REGULAR_EXPRESSION` is the secondary
   `[  SKIPPED ]` guard: if the wrapper is bypassed and the receiver exits 0
   while gtest printed a skip (or ran zero tests), CTest reports Failed, never
   Passed. `RESOURCE_LOCK os_clipboard` + `RUN_SERIAL` keep the standalone
   receiver from overlapping an opted-in clipboard run. The loop's
   `set_tests_properties(${testname} PROPERTIES ENVIRONMENT ...)` line (~244,
   `INKSCAPE_PROFILE_DIR=${INKSCAPE_TEST_PROFILE_DIR}/${testname}` plus
   `${CMAKE_CTEST_ENV}`) stays in place for both branches.

   No `set_tests_properties`/environment block for the receiver target is
   otherwise required at the parent level: this directory registers
   `vacards-clipboard-external` with its own `RESOURCE_LOCK os_clipboard`,
   `RUN_SERIAL`, `LABELS "clipboard-external"`, `TIMEOUT 1800`,
   `SKIP_RETURN_CODE 77` and the `INKSCAPE_TEST_GUI`/fontconfig/profile
   environment (and creates `clipboard-profile/` at configure time). The
   standalone `test_text-paste-external` test skips when
   `VACARDS_CLIP_SESSION` is unset, but only the wrapper + properties above make
   that skip visible to CTest instead of a false pass.
4. **Do not run `test_text-paste` while the user is active.** That existing suite
   still writes the user's clipboard in-process (`setPlainClipboard` /
   `setRichClipboard`) without snapshot or restore. This harness does not modify
   it; `RESOURCE_LOCK os_clipboard` is also still missing from its registration.
5. Only macOS is implemented. The Windows publisher and the GDK-only publisher
   described in the discovery report are not part of this checkout, so Windows
   and Linux cells are `not-tested` by definition.
6. `--allow-partial-snapshot` was **removed as a publication path**. An incomplete
   or unrestorable snapshot always refuses publication (exit non-zero, refusal
   recorded in `results.jsonl` and `summary.json`) and the session is retained;
   passing the old flag now exits 2 before any clipboard access. There is no way
   to trade the user's clipboard restoration for test coverage.
7. The restore is **not race-free**. `NSPasteboard` has no compare-and-swap, so a
   third party can write between the publisher's final `changeCount` check and
   `clearContents`; the check is repeated immediately before the destructive call
   and the driver never writes after the count changed, but that residual window
   cannot be closed with this API.
8. A restore that fails after `clearContents` can leave the pasteboard empty:
   the publisher retries the write once and verifies the restored types, the
   driver retries once when the pasteboard is unchanged, and on final failure it
   **retains** the snapshot (`snapshot_dir` in `summary.json`, path also on
   stderr) instead of deleting the only copy. Manual recovery from that directory
   is the documented fallback.
9. The reject/abort cases assert the document/Undo/Redo oracle and that
   `ClipboardManager::paste` reports no paste. The user-visible warning text
   (message stack) is not asserted; that belongs to a UI-level manual case.
10. The stalled-representation mechanism (`stall: true`) is compiled and
    manifest-validated but has not been exercised against a live pasteboard in
    this round. Residual risk to confirm on the first opted-in run: that
    `[NSPasteboard types]` lists a provider-declared type without the publisher
    itself triggering a data request, so readiness is never blocked by the stall.
    If it is, the publisher times out (`env-blocked`, not a false pass) and the
    fixture must be revisited.
