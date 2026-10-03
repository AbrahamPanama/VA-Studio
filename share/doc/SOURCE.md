# Source code of VA Studio

> LEGAL-CHECK: draft wording for the owner's legal review; not final.

VA Studio is free software: you can redistribute it and/or modify it under the
terms of the GNU General Public License as published by the Free Software
Foundation, either version 3 of the License, or (at your option) any later
version. It is a modified version of Inkscape. The third-party components it
contains or is distributed with, and their licenses, are listed in
`THIRD-PARTY-NOTICES.md`.

## Where to get the source of a release

Every VA Studio release is built from one commit of the public source repository:

- Source repository: @VA_PUBLIC_REPO_URL@
- Source of each release, as one archive: @VA_SOURCE_URL@

The release notes of each version name its source commit. The repository is
self-contained: the former Git submodules (lib2geom, libcroco, libdepixelize,
libUEMF, CapyPDF, the translations, the GTK themes and the Inkscape extensions,
including their nested modules) are ordinary directories at their pinned
commits, the VA Studio fork of libcdr is in `third_party/libcdr-vacards`, and the
Rust crates of the nesting engine are vendored. The modified components and
their upstream bases are listed in `THIRD-PARTY-NOTICES.md`.

The release source archive additionally contains the upstream release archives
that the build scripts download and verify by SHA-256 (Cairo 1.18.4, GTK 4.22.4
and librevenge 0.0.6), the patches and scripts used to build and package the
release, and a manifest of every file with its SHA-256.

## Libraries from MSYS2 and Homebrew

The Windows package bundles libraries and data from MSYS2 packages, and the
macOS package bundles libraries from Homebrew formulae. Their exact versions
are listed in `THIRD-PARTY-NOTICES.md`. Their source code is available from
those projects (https://packages.msys2.org, https://formulae.brew.sh) and is
also covered by the offer below.

## Written offer

For at least three years after we last distribute a given VA Studio binary,
and for as long as we offer spare parts or customer support for it,
@VA_COPYRIGHT_HOLDER@ will give anyone who has that binary, for a charge no
more than our reasonable cost of physically performing the distribution, a
complete machine-readable copy of the corresponding source code of that binary,
including the source of the GPL-, LGPL- and MPL-licensed libraries distributed
with it, on a durable physical medium customarily used for software
interchange, or by download from a network server at no charge.

To request it, contact @VA_SUPPORT_URL@ and state the VA Studio version and
build number (Help > About VA Studio) and your platform.

## Building

`doc/public/BUILDING.md` in the source tree describes the Windows and macOS
builds, the pinned dependencies and the verification steps.
