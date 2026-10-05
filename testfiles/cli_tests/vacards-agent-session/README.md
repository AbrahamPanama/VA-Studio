# Agent command line session cases

The no-flag `vacards-agent-cli-test.py INKSCAPE CASE_DIR` path is the legacy
compatibility oracle. Its functions and output remain unchanged.

```
python3 -B testfiles/vacards-agent-cli-test.py --transport session VASTUDIO_CLI GROUP
python3 -B testfiles/vacards-agent-cli-test.py --transport one-shot VASTUDIO_CLI GROUP
python3 -B testfiles/vacards-cli-inventory.py VASTUDIO_CLI testfiles/cli_tests
```

Python standard library only. The new runner exits 1 on empty groups, timeouts,
invalid cases or any failed oracle. Exit 77 is reserved for a missing CLI binary.
An existing stub executable is a failure. No test is marked skipped to satisfy a
gate. The standalone acceptance gate (development repository only) obtains/releases `.vacards-build-lock` for each CTest
step; use `--lock` for a shared lock outside its build directory. Do not run it
inside an already-held lock. Default whole-gate budget is 1,200 seconds and can
only be tightened.

## Isolation and execution

One JSON file is one required case. Each case explicitly resets by restarting
its process; no cross-case state reuse is assumed. `startup_args` passes the
inspection file and grants to `--agent-session`. The legacy placeholders
`{WORK}`, `{FIXTURES}`, `{TESTCASES}`, `{RESULT}` apply recursively. A case has:

- `requests`: ordered steps (must be nonempty); each step has `request` containing
  the full typed request, or `raw`/`raw_hex`/`raw_repeat:{text,count}` for transport
  faults. Raw steps declare `id` and `command` for result correlation/validation.
- `concurrent:true`: send this step before waiting for the previous terminal;
  recorded arrival/send times additionally enforce that the previous terminal
  did not arrive first. Ordinary steps wait for the preceding batch to settle.
- `fragment_bytes`: write fragment size; case `line_ending` defaults to LF and
  can be CRLF. Readers drain stdout/stderr independently. Writes and reads have
  the same case deadline. The runner kills only its own exact child PID on failure.
- `expect_accepted`: defaults true for typed requests, false for raw faults.
  Every request requires a terminal; accepted cardinality is checked independently.
- `expect`: recursive subset comparison, with legacy numeric tolerance.
  `expect_exact:{"dotted.path":value}` uses exact JSON equality (Booleans distinct
  from numbers). `expect_numeric:{"path":{value,abs,rel}}` gives explicit tolerance.
  `expect_absent:["path"]`, `expect_nonempty:["path"]` check key absence/content.
  `expect_error` pins an error code and requires a nonempty actionable hint.
- `timeout` seconds (default 30), `expect_exit` (default 0), `expect_stderr` (exact),
  `expect_stderr_contains` and `expect_stderr_not_contains` are case-level oracles.
- `expect_hello` subset and `expect_hello_nonempty` paths assert inspection startup.
- `transport:"one-shot"` selects a single request-file case within a CTest group.
  One-shot cases must contain exactly one step. A separate catalog probe supplies
  schemas; operation stdout must contain exactly one bare typed result.
- `command` and `cells` on a step (or case/`coverage` entries) declare inventory
  coverage. Each declaration must reference an actual request in that case.
  Cells are success, invalid (boundary + invalid input), no-op, refusal, dry-run,
  undo, group, save, cancel. Advertised input enum values also require successful
  requests; the inventory checks supplied values and registry defaults.

The session runner obtains `system.catalog` immediately after hello, using the
reserved ID `__p9_catalog`, which cases must not use. It validates *every* catalog
example/request schema and result schema independently, then validates each
dispatched terminal against its command's result schema, including catalog results.
Anonymous admission refusals use the closed common envelope from system.catalog,
plus explicit refusal-code, no-acceptance, no-publication and no-mutation checks. Unsupported schema vocabulary is an ERROR even in an unused
conditional/alternative. The catalog is never substituted with a fixture in live
tests. Strict JSON rejects duplicate keys, nonfinite values and invalid UTF-8.

## Wire integration

The M1 transport is implemented. `internal note cli/session` records the
integrated wire contract; the strict hello check includes the full compiled SHA:

- Every stdout line: `{schema:"va-studio.cli-session/1",event,event_seq,...}`;
  event_seq is a strictly increasing positive integer; hello is first and unique.
- Hello fields: `session_id`, `protocol:"va-studio.cli-session/1"`,
  `catalog_version:"va-studio.cli-catalog/1"`, `catalog_hash` (64 lowercase hex),
  `product`, `build`, `source_sha` (40 lowercase hex),
  `capabilities:{enabled_slices:["M1",...]}`, `limits:{request_bytes,response_bytes,...}`,
  `document` (summary or null). Optional `startup_error` needs code/message/hint.
- accepted/progress/result carry `id`. A result carries `result` (cli-result/1)
  whose id matches its envelope and whose seq counts all terminals from 1.
  A framing failure uses empty-string id. Rejected/unaccepted requests still
  produce one terminal. No extra terminal, unknown event or stray stdout is allowed.
- Progress contains `phase`, `phase_label`, `phase_percent` (0–100), directly or
  inside `progress`. It requires an outstanding accepted request.
- status data: `active_request` (null while idle), `retention:{tokens:[],roots:0,bytes:0}` in M1.
  cancel unknown: unchanged, `found:false,cancellation_requested:false`; input is `params.id`. release unknown: unchanged,
  `released:[]`, `not_found:[...]`. close success: ok, `closed:true`.
- Pinned transport refusal codes: malformed-json, invalid-utf8, request-too-large,
  repeated-key, unknown-key, out-of-range, duplicate-request-id, session-busy,
  session-output-fixed.

The busy case pipelines catalog requests. If the first request finishes before
the second is sent, it fails instead of claiming a busy test happened. A bounded
slow-work seam may be needed for deterministic platform qualification. Dirty
close, actual retained tokens and cooperative long-operation cancel need later
slices. Inspection/grants cases additionally require the inspection slice.

## Inventory and release evidence

`inventory-na.json` records reviewed exceptions and named legacy case coverage.
The inventory computes all nine cells from effects/target/cancellation policy,
prints every N/A reason, and refuses absent M1 commands, undeclared commands,
unknown policy/effects, missing reviewed legacy files and uncovered cells.
`--catalog FILE --json FILE` is offline review only and is never used by CTest.

The gate discovers both labels, requires inventory + system/session/query + all
nine legacy CTest groups, executes them serially, and checks JUnit status plus
exact case filenames/counts against disk. Missing, zero, skipped, failed and
timed-out tests fail. Existing empty legacy groups remain skipped in the legacy
runner and therefore block this stricter gate. This does not weaken that oracle.
The gate writes per-group logs/JUnit, inventory/catalog JSON, `summary.json` and
`summary.xml`, with elapsed time, commands, host/environment and binary hashes.

`query/` contains the 77 required session cases. Mac execution does
not qualify Windows or the staged/installed package. Release scripts run the gate
before packaging and retain their original `.b29` copies.
