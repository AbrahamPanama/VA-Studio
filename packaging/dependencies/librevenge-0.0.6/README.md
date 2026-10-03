# VACards patched librevenge 0.0.6 (bundle)

This directory is the tracked, license-preserving patch bundle for the pinned
librevenge **0.0.6** source archive. It carries the reviewed C02-FIX + T01-FIX +
T01-FIX2 generator changes that give the CDR import path its versioned transport
and native SVG crop/text serialization (see
`doc/vacards/cdr-clipping-work/CDR_EVENT_CONTRACT.md`).

The bundle is **provenance and build input only**. It does not change the
application, its CMake feature flags or any installed dependency prefix.

## Contents

| File | Purpose |
| --- | --- |
| `vacards-librevenge-0.0.6.patch` | Single `patch -p1` patch from the stock 0.0.6 release tree to the reviewed combined state. |
| `VACARDS-LIBREVENGE-BUNDLE.env` | Machine-readable manifest: archive hash, patch hash, source identity, capability marker, component provenance, final file hashes and exact test counts. |
| `TEST-RECIPE.md` | Exact dependency build/test recipe and the expected serial results. |
| `README.md` | This notice. |

## Source identity (no invented Git commit)

The source is the upstream **librevenge 0.0.6 release tarball**
`librevenge-0.0.6.tar.xz`, SHA256:

```
19eacf5ce55d7fe6a990a45142589cdf7da0c7b68701797f133482cb44f189fa
```

It is a release tarball extraction, not a Git checkout: there is **no** branch
or commit to point at. The archive SHA is the source identity. The combined
patch hash is recorded separately. Because the tarball is not committed to this
repository, the build scripts require the operator to supply the archive path
and re-verify the archive SHA before extracting.

## License

librevenge 0.0.6 is distributed under the **MPL 2.0** and **LGPL v2.1 or later**
(see `COPYING.MPL` and `COPYING.LGPL` inside the archive). This patch only
modifies program source files; the archive's license and copyright notices are
retained unchanged in the extracted tree that the build recipe produces. The
patch adds no new third-party code and no new license terms; contributors retain
their existing rights. Do not redistribute a built prefix without the archive's
license files.

## Capability

The patched generator emits the versioned CDR-only marker
`vacards:cdr-fidelity-version=1` and the diagnostic comments
`<!-- librevenge:cdr-crop ... -->` / `<!-- librevenge:cdr-text ... -->`. The
presence of the `librevenge:cdr-crop` string in the installed shared library is
the capability marker used by `verify-vacards-librevenge.sh`; a stock 0.0.6
library does not contain it.

## Reproduce

See `TEST-RECIPE.md` and
`packaging/macos/vacards/build-patched-librevenge.sh` /
`packaging/windows/vacards/build-patched-librevenge.sh`. Both scripts refuse to
overwrite an existing output directory, cap compilation at two jobs and run
`make check` serially.
