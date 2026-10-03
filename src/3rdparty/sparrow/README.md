# Pinned Sparrow helper

Original Sparrow revision is pinned in the root VACARDS-DEPENDENCIES.env.
The darwin-arm64 executable is the exact DeepnestML bundled artifact tested by
the VACards sandbox, built with only_final_svg; preserve its MIT LICENSE.
It has its own Rust/Jagua runtime and does not replace the application's pinned
Jagua, Cairo, GTK, libcdr, or toolchain.

CMake verifies and copies it next to the development/installed Inkscape binary
as vacards-sparrow (vacards-sparrow.exe on Windows). Runtime discovery never
searches PATH. Both macOS arm64 and Windows x64 include the helper. Unsupported
platforms keep the existing solver.
No network download or global installation takes place at runtime.

## Windows x64 build

The Windows executable uses the same unmodified upstream commit, built with
`only_final_svg` for `x86_64-pc-windows-gnu`. Upstream does not track Cargo.lock;
`windows-x64.Cargo.lock` freezes this build's transitive dependencies. Both its
checksum and the executable checksum are pinned in the root manifest.

This helper needs Rust 1.90.0 (ordered-float 5.5 requires it). Install it alongside
the app's unchanged Rust 1.88.0, never as an app toolchain replacement. The initial
artifact was built with MSYS2 UCRT64 GCC 16.2.0, release LTO, two build workers and
`RUSTFLAGS=-Clink-arg=-Wl,--no-insert-timestamp`. Its PE imports are Windows system
DLLs only; users do not need Rust or MSYS2 installed to run this helper.

To build a new candidate in MSYS2 UCRT64:

```sh
bash packaging/windows/vacards/build-sparrow.sh /c/vacards/deps/sparrow-new-candidate
```

The script uses the pinned source, lock and separate toolchain, and retains the
build log, hash receipt and PE imports. It does not automatically replace or
approve the committed executable. Run `test_nesting-sparrow` and the three
Sparrow GUI lifecycle cases in `test_nesting-tool` on Windows before updating a
pin. A different compiler or build path is not guaranteed bit-reproducible.
