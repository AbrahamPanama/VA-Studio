# Pinned Sparrow helper

Original Sparrow revision is pinned in the root VACARDS-DEPENDENCIES.env.
The darwin-arm64 executable was rebuilt from the pinned unmodified upstream
source for Build 30 (2026-10-03), with `only_final_svg`; preserve its MIT LICENSE.
It has its own Rust/Jagua runtime and does not replace the application's pinned
Jagua, Cairo, GTK, libcdr, or toolchain.

CMake verifies and copies it next to the development/installed Inkscape binary
as vacards-sparrow (vacards-sparrow.exe on Windows). Runtime discovery never
searches PATH. Both macOS arm64 and Windows x64 include the helper. Unsupported
platforms keep the existing solver.
No network download or global installation takes place at runtime.

## macOS arm64 source build — Build 30 (2026-10-03)

Source: `https://github.com/JeroenGar/sparrow` at
`57c45cd295f5d2ce2a11edf6e765318a51d2b41e`. The fetched archive's commit
metadata and GitHub commit API both matched the pin. The previous DeepnestML
prebuilt remains the comparison baseline.

Built with separate Rust 1.90.0 (`1159e78c4`, LLVM 20.1.8), targeting
`aarch64-apple-darwin`. The existing `windows-x64.Cargo.lock` was reused unchanged
with `--locked`; its SHA-256 is
`cb8f18928b7f6639b45773d6a41cc4b7d3324342fc82908a63764487d0b1db98`.
No Darwin-specific lockfile or dependency-version changes were necessary.

Command: `cargo build --locked --release --features only_final_svg --bin sparrow
--target aarch64-apple-darwin --jobs 2`, with `CARGO_BUILD_JOBS=2` and upstream
release profile `opt-level=3`, `lto="fat"` (no optimization-profile override).
`RUSTFLAGS` remaps the workspace, Cargo home, Rustup home and Sparrow source,
in that order, to `/build`, `/cargo`, `/rust` and `/sparrow` using four
`--remap-path-prefix=ACTUAL_PATH=NEUTRAL_PREFIX` flags. The more specific
mappings follow the workspace mapping. Exact flags, commands, environment,
archive identity, logs, path scan and comparisons are retained with the
internal release evidence.

Executable SHA-256: `a238310dafb39ac998ccd2fa9de731ca17321fd00f3b31f66ff03589c95487a4`.
The linker-produced ad-hoc signature is retained and verified before pinning;
no post-pin signing or stripping was applied. The binary has no user/home,
Cargo/Rustup home or work-directory paths in its `strings` output, and imports
only system libraries. Its minimum macOS version is 11.0 (SDK 26.0).
Packaging must preserve these exact pinned bytes after inner signing, as
specified in `internal note SPARROW_INTEGRATION`.

## Windows x64 source build — Build 30 (2026-10-03)

The Windows executable was rebuilt on a Windows build PC from the same unmodified upstream
commit `57c45cd295f5d2ce2a11edf6e765318a51d2b41e`, with `only_final_svg` for
`x86_64-pc-windows-gnu`. Upstream does not track Cargo.lock; the unchanged
`windows-x64.Cargo.lock` freezes the transitive dependencies, SHA-256
`cb8f18928b7f6639b45773d6a41cc4b7d3324342fc82908a63764487d0b1db98`.

Built with separate Rust 1.90.0 (`1159e78c4`, LLVM 20.1.8), installed into the
separate Rustup home without changing PATH or the application's Rust 1.88.0.
Compiler: MSYS2 UCRT64 GCC 16.2.0. Command: `cargo build --locked --release
--features only_final_svg --bin sparrow --target x86_64-pc-windows-gnu --jobs 2`,
with `CARGO_BUILD_JOBS=2`, upstream `opt-level=3` and `lto="fat"`.

Used a local copy of `packaging/windows/vacards/build-sparrow.sh`, retaining
`-Clink-arg=-Wl,--no-insert-timestamp` and adding four
`--remap-path-prefix=ACTUAL_PATH=NEUTRAL_PREFIX` flags for the build workspace,
Cargo home, Rustup home and Sparrow source, in that order, mapping to `/build`,
`/cargo`, `/rust` and `/sparrow`. Only source-root discovery and the RUSTFLAGS
assignment differ from the maintained script. The recipe's upstream source,
lock validation, toolchain selection, target, features and build flags remain.
The maintained script currently overwrites incoming RUSTFLAGS; merely setting
RUSTFLAGS in its caller does not reproduce this remapped build. Exact expanded
flags, environment, script diff and build logs are retained with the internal
release evidence.

Executable SHA-256:
`80d2901e168dc49176ee890774f55128803704a022fdbae181bda213f63f2908`.
No post-link stripping or signing was applied to the helper. Its strings contain
no account/home, Cargo/Rustup home or work-directory paths. PE imports match the
prior helper and contain Windows system DLLs only; users need neither Rust nor
MSYS2 to run it.

Headless Windows qualification used the normal compiled SHA gate, the full
`test_nesting-sparrow` suite and its corpus fixtures, and `test_nesting-document`.
Seed-0 circles80 and compound_rigid were compared against the prior Windows
helper with matched budgets, three runs per helper. The full Sparrow suite
retains an outline-preparation deadline failure reproduced with the old helper
on this host; the all-green suite gate remains open. This is prepared helper
evidence; the three Sparrow GUI lifecycle cases in `test_nesting-tool` were
not run on that build PC and remain required for broader Windows
GUI/release qualification. No installed application was replaced.
