# Public release tooling

Tools that prepare a VA Studio release for public distribution. They need only
Python 3 (standard library), Bash and the platform build tools already used by
the release pipeline. None of them builds, signs, uploads or publishes anything.

## Public identity

`public-release.env` holds the owner's decisions: product name, copyright
holder, support URL, source URL, repository URL, forks URL and the status of
the TIFF ICC profile. While a value is empty, the build uses neutral fallbacks
(`CMakeScripts/VACardsPublicIdentity.cmake`): the crash dialog points to the
installed `SUPPORT.md`, the Windows version resource names "the VA Studio
authors", and installed documents say "(not yet published)".

## Complete source archive

```sh
packaging/public-release/make-source-archive.sh --public-repo . --commit RELEASE_COMMIT \
    --output va-studio-source-RELEASE.tar.xz --downloads ~/va-downloads [--fetch] \
    --icc PROFILE.icc|none --hyphenation DICTIONARY_DIR|none
```

It packs the source tree of the release commit, the upstream Cairo, GTK and
librevenge archives and the MSYS2 GTK recipe files (each verified against the
SHA-256 pinned in the tree), the external inputs given as parameters,
`SOURCE-ARCHIVE.json` and `SHA256SUMS`. It fails if anything is missing or does
not match. Publish the archive with each release (see `share/doc/SOURCE.md`).

## Third-party notices

```sh
python3 packaging/public-release/generate-third-party-notices.py          # rewrite share/doc/THIRD-PARTY-NOTICES.md
python3 packaging/public-release/generate-third-party-notices.py --check  # fail if the committed file is stale
```

Optional inputs, used at release time:

- `--sbom FILE.tsv` (repeatable): the platform inventories captured below.
- `--sparrow-vendor DIR`: the output of `cargo vendor` for
  `src/3rdparty/sparrow/windows-x64.Cargo.lock` (run in a Sparrow checkout at the
  pinned commit with that lockfile). It supplies the crates' own license files.
- `--refresh-crates-io`: refresh `crates-io-sparrow.json` with read-only crates.io
  API requests (one per second). Each entry records its URL, retrieval time and
  whether the registry checksum equals the lockfile checksum.

`notices-status.json` lists what is still incomplete; a public release needs it
to report no open item.

## Platform library SBOM

Run these on the build hosts after staging a package, then regenerate the
notices with the resulting TSV files. Keep the outputs with the release evidence.

Windows (MSYS2 UCRT64 shell, after `create-internal-test-installer.py stage`):

```sh
bash packaging/public-release/capture-sbom-msys2.sh OUTPUT/payload EVIDENCE/sbom-windows
```

macOS (after the application bundle is built):

```sh
packaging/public-release/capture-sbom-homebrew.sh "VA Studio.app" EVIDENCE/sbom-macos \
    --va-prefix ../deps/cairo/install --va-prefix ../deps/libcdr/install \
    --va-prefix ../deps/librevenge/install --va-prefix ../deps/gtk/install
```

Both write `PREFIX.tsv` (one row per bundled file: package, version, license,
license files, project URL) and `PREFIX.unattributed.txt`. They exit 1 when an
executable module cannot be attributed to a package or to a VA Studio build;
review the list rather than deleting entries. Then:

```sh
python3 packaging/public-release/generate-third-party-notices.py \
    --sbom EVIDENCE/sbom-windows.tsv --sbom EVIDENCE/sbom-macos.tsv
```

The SBOM records the exact MSYS2/Homebrew versions that the Windows installer
and the macOS bundle contain. Together with the `pe-imports.json` written by the
Windows staging step it is also the evidence for reviewing which bundled codec
DLLs (x264, x265, openh264, libde265, kvazaar) the package actually needs.
