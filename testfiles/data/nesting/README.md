# VACards nesting benchmark corpus

These synthetic, non-customer fixtures exercise stable geometry classes used by
the nesting release gate. Every document has one `container` and sequential
`part-N` top-level payloads. Tests must preserve the files unchanged, validate
all returned placements through Jagua, and report preparation/solve metrics.

- `small-rectangles.svg`: deterministic baseline and dense identical parts.
- `concave-mixed.svg`: irregular container plus concave, large, and small parts.
- `compound-print-cut.svg`: rigid multi-island groups with explicit cut contours.
- `transparent-bitmap.svg`: an embedded hard-alpha bitmap whose occupied pixels
  are smaller than its image rectangle.

Add reduced synthetic fixtures here rather than customer-identifying artwork.
Performance baselines belong in release evidence, not as machine-specific
absolute timing assertions in the source tree.

## Portable differential gate

The Phase 2 C++ contract executable also provides a backend-neutral report and
comparison interface. A runner is the same test executable linked to the
backend under evaluation; therefore the harness does not require a permanent
user-facing backend switch or exchange solver input through JSON.

Generate one report from each separately built runner with an equal budget:

```sh
vacards_nesting_cpp_contract_test \
  --benchmark-report baseline.tsv --backend-label baseline --budget-ms 1000
vacards_nesting_cpp_contract_test \
  --benchmark-report experimental.tsv --backend-label experimental --budget-ms 1000
vacards_nesting_cpp_contract_test \
  --compare-reports baseline.tsv experimental.tsv
```

`--budget-ms 0` is a deterministic first-complete-incumbent mode: the harness
copies and authoritatively validates the first complete ABI progress snapshot,
then cancels the unlimited job. It is used by
`vacards-nesting-differential-selfcheck` with `--require-identical` to verify
report compatibility and seeded placement repeatability without making a
wall-clock quality claim. Positive budgets exercise the real finite deadline.

The versioned TSV schema records immutable-input and placement fingerprints,
selected/placed counts and areas, usable container area, utilization, unplaced
count, first-incumbent and total time, combined exploration/refinement time,
iterations, callback count, occupied bounds, deadline overrun, and contour
source/fidelity. Every reported placement is independently checked for result
cardinality and IDs, finite/allowed transforms, containment, holes, spacing,
margin, and overlap before a report is accepted. The comparator rejects input
mismatches, invalid output, missing/extra cases, and any regression in placed
area, placed count, or occupied-bounds area.

Fields the current ABI cannot measure are present as `NA`: application geometry
preparation time, peak resident memory, per-part unplaced reason, and
cancellation latency. Cancellation latency has a separate C++ contract gate.
The current ABI also does not expose the engine's stable tie-break key;
`placement_signature` supports deterministic same-backend checks but is not
invented as a cross-backend ordering rule. Engine/host integration must fill
those measurements before NEST-009 release evidence is complete.

For a portable orchestration entry point (including Windows path handling), run
`src/nesting/tests/run-nesting-differential.cmake` with
`BASELINE_RUNNER`, `EXPERIMENTAL_RUNNER`, `COMPARATOR_RUNNER`, `OUTPUT_DIR`,
and optional `BUDGET_MS`. Set `REQUIRE_IDENTICAL=ON` only for deterministic
same-backend checks; a new backend is required to be non-worse, not byte-for-byte
identical.
