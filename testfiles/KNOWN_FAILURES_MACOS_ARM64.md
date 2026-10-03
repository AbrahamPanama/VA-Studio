# Known upstream test failures on macOS ARM64

This file records tests which fail in both this branch and an unmodified
Inkscape `origin/master` worktree on the same machine. They remain enabled in
the complete suite. On macOS ARM64 only, they carry the CTest label
`upstream-platform-sensitive` so that feature regression runs can distinguish
new failures from this known baseline.

## Reproduction baseline

- Upstream commit: `d2b9be1c534e1c742fb5bbcce6e4c30a0833232d`
- Platform: macOS 26.6, Apple Silicon (`arm64`)
- Build type: `RelWithDebInfo`
- Baseline command:

  ```sh
  ctest --output-on-failure \
    -R '^(test_geom-pathstroke|test_lpe|test_lpe64)$'
  ```

All three tests fail in the clean worktree as well as the VACards worktree.
The complete output remains available by running the command above.

## Tracked tests

### `test_geom-pathstroke`

`GeomPathstrokeTest.BoundedHausdorffDistance`, fixture case 7, consistently
reports a distance of `0.30249962611032621` against a tolerance of `0.1`.
The other three tests in the executable pass. Rebuilding the fixture at `-O0`
produces the same result, so this is not an optimizer-only failure.

Do not raise the global tolerance or replace the expected path without first
establishing which extrapolated join is mathematically correct.

### `test_lpe`

The 0.92 and 1.0 fixture groups pass. The 1.1, 1.2, and 1.3 groups contain
architecture-sensitive generated paths. The comparator now correctly handles
reversed and cyclically re-indexed equivalent paths, but material geometry and
segment-count differences remain.

### `test_lpe64`

The 1.0 64-bit fixture group passes. The 0.92 64-bit group differs in five
generated paths on this platform.

Upstream Inkscape issue
<https://gitlab.com/inkscape/inkscape/-/issues/3554> documents the LPE fixture
suite producing architecture-dependent failures and false positives on ARM64
and other platforms.

## Test commands

Run the complete suite, including the known failures:

```sh
ctest --output-on-failure
```

Run the actionable regression gate for this platform:

```sh
cmake --build . --target check-regressions
```

The gate excludes only tests carrying the
`upstream-platform-sensitive` label. It does not mark them passed, disable them,
or change their tolerances.

## Removal criteria

Remove a label as soon as one of the following is available:

1. upstream publishes architecture-independent fixture references;
2. the underlying 2Geom/LPE implementation is corrected and the clean
   upstream test passes on ARM64; or
3. the test is redesigned to compare geometric equivalence without depending
   on unstable generated path serialization.
