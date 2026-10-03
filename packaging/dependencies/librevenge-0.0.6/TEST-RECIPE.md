# librevenge 0.0.6 dependency test recipe

This recipe is the reviewed dependency-test record for the patch bundle. It is a
**dependency** test, not an application test: it never registers in an
application CMake target, because it requires the private patched prefix and
must not replace the app's dependency resolution.

## Inputs

| Item | Value |
| --- | --- |
| Archive | `librevenge-0.0.6.tar.xz` |
| Archive SHA256 | `19eacf5ce55d7fe6a990a45142589cdf7da0c7b68701797f133482cb44f189fa` |
| Patch | `vacards-librevenge-0.0.6.patch` |
| Patch SHA256 | `66353626b00f13a226a00bee347af5911e831d6bab42a0dbc5a43cff175ee4f7` |
| Pinned final generator | `4c6d65e5cd792185552d174d8d2d235f53851883dffcdd359f441afe2ffb633d` |
| Capability | `vacards:cdr-fidelity-version=1`; binary marker `librevenge:cdr-crop` |

## Commands (macOS, verified 2026-09-19)

```sh
# 1. Apply the tracked patch to a fresh stock extraction.
tar -C "$WORK" -xf librevenge-0.0.6.tar.xz
cd "$WORK/librevenge-0.0.6"
patch -p1 --dry-run < .../vacards-librevenge-0.0.6.patch     # exit 0
patch -p1          < .../vacards-librevenge-0.0.6.patch      # exit 0

# 2. Out-of-tree autotools build, at most two compile jobs.
mkdir "$WORK/build" && cd "$WORK/build"
CPPFLAGS=-I/opt/homebrew/include LDFLAGS=-L/opt/homebrew/lib \
  "$WORK/librevenge-0.0.6/configure" --enable-tests --disable-werror --disable-docs   # exit 0
make -j2                                                                              # exit 0
make check                                                                            # exit 0

# 3. Focused CppUnit suite (run from its build directory).
make -C src/test test                                                                 # exit 0
( cd src/test && DYLD_LIBRARY_PATH=../lib/.libs ./test )                              # exit 0
```

## Expected results

| Check | Result |
| --- | --- |
| `patch -p1 --dry-run` | exit 0, all five files apply |
| `configure` | exit 0, `tests: yes` |
| `make -j2` | exit 0 |
| `make check` | exit 0, `TOTAL 1 PASS 1 FAIL 0` |
| focused suite | exit 0, `OK (59)` = 17 crop + 21 text + 21 other |
| capability marker | `librevenge:cdr-crop` present in installed `librevenge-0.0.0.dylib` |

The scripted form is
`packaging/macos/vacards/build-patched-librevenge.sh OUTPUT_DIR ARCHIVE`; it
performs the same steps, verifies the archive and patch hashes, enforces the
two-job cap, runs `make check` serially, verifies the capability marker and
writes `VACARDS-LIBREVENGE.env` in the prefix.

## Platform note (Windows)

The Windows recipe is
`packaging/windows/vacards/build-patched-librevenge.sh`. It runs under MSYS2
UCRT64 and mirrors the macOS steps; autotools plus the UCRT64 toolchain is an
external prerequisite and was **not executed** in this integration (no Windows
host available). See `packaging/windows/vacards/verify-vacards-librevenge.sh`.
