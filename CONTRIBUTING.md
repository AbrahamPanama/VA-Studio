# Contributing to VA Studio

Thank you for helping. VA Studio is a modified version of Inkscape maintained
by VA Cards; contributions are welcome under the terms below.

## Before you start

- Problems and questions: [SUPPORT.md](SUPPORT.md). Security problems:
  [SECURITY.md](SECURITY.md), never in public issues.
- Changes that are not specific to VA Studio are best made in Inkscape itself
  (<https://gitlab.com/inkscape/inkscape>); VA Studio picks them up from there.
  Inkscape's own developer guide is in
  [doc/CONTRIBUTING-inkscape.md](doc/CONTRIBUTING-inkscape.md).
- Fixes to bundled components (for example lib2geom, CapyPDF, libcroco or the
  libcdr fork in `third_party/libcdr-vacards`) should also go to their upstream
  projects; see [share/doc/THIRD-PARTY-NOTICES.md](share/doc/THIRD-PARTY-NOTICES.md).

## Making a change

1. Build VA Studio as described in [doc/public/BUILDING.md](doc/public/BUILDING.md).
2. Keep each pull request to one change, with tests that check the outcome.
3. Follow the existing code style (`_clang-format`, `.clang-tidy`) and keep the
   `SPDX-License-Identifier` line at the top of every source file. New VA Studio
   files use `GPL-2.0-or-later` unless agreed otherwise.
4. Operations on groups and multiple selections follow the editing contract in
   [doc/vacards/SELECTION_CONTRACT.md](doc/vacards/SELECTION_CONTRACT.md): one
   user action, one Undo step, incompatible objects preserved and reported.
5. This repository does not run hosted CI. Run the relevant tests locally (see
   the Tests section of BUILDING.md) and list the commands and results in the
   pull request, including tests you could not run.

## Developer Certificate of Origin

> LEGAL-CHECK: recommended process, pending the owner's legal review.

Every commit must be signed off to certify the
[Developer Certificate of Origin 1.1](https://developercertificate.org/): that
you wrote the change or otherwise have the right to submit it under the
project's open source license. Add the sign-off with `git commit -s`:

```text
Signed-off-by: Your Name <you@example.org>
```

Use your real name and an address you can be reached at. Pull requests with
commits that are not signed off cannot be merged.

## Translations

User-visible texts are written in English. VA Studio's Spanish translation is
maintained in `po/`; corrections are welcome as pull requests.
