# Security policy

## Supported versions

Security fixes are made in the latest VA Studio release. Older releases are
not updated; please install the latest one.

## Reporting a vulnerability

Please do not report security problems in public issues or discussions.

- Report them privately through the VA Studio support address (published
  with the first public release) and mark the report as a security issue,
  or use the private vulnerability reporting of the VA Studio repository
  (Security > Report a vulnerability) when it is enabled.
- Include the VA Studio version and build (Help > About VA Studio), your
  operating system, the steps to reproduce, and a file that triggers the
  problem if you can share it.

We will confirm that we received the report, keep you informed while we work
on a fix, and credit you in the release notes unless you prefer otherwise.
Please give us reasonable time to release a fix before you disclose details.

## Scope

In scope: the VA Studio application, its installers and disk images, the
Windows File Explorer thumbnail and preview handler, and the bundled helper
programs and libraries as shipped with VA Studio.

Problems in Inkscape or in a third-party library that also affect VA Studio
are welcome too; we coordinate with the upstream project.

## Things that are expected behavior

- Extensions are programs. VA Studio runs the Python extensions it ships and
  those you install yourself (for example with Extensions > Manage
  Extensions...) with your user rights. Install only extensions you trust.
- Opening a document can read the local files that the document links to, such
  as linked images.
