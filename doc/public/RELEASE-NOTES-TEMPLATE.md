# VA Studio <display version> (build <build number>)

<!-- Template. Replace every <...> and remove sections that do not apply.
     Product version and build come from packaging/vacards/VERSION.env. -->

Released <YYYY-MM-DD>. Based on Inkscape <base version> (development snapshot).

## Downloads

| File | Platform | SHA-256 |
| --- | --- | --- |
| `VA-Studio-<release>-windows-x64.exe` | Windows x64, installs for the current user | `<sha256>` |
| `VA-Studio-<release>-windows-x64-programfiles.exe` | Windows x64, installs for all users | `<sha256>` |
| `VA-Studio-<release>-arm64.dmg` | macOS 26 or later, Apple silicon | `<sha256>` |
| `va-studio-source-<commit>.tar.xz` | complete corresponding source | `<sha256>` |

- Windows installers: <signed by "<publisher>" | not yet code-signed>.
- macOS app: <signed with Developer ID and notarized | not yet notarized>.
- How to check a download: [INSTALL-windows.md](INSTALL-windows.md),
  [INSTALL-macos.md](INSTALL-macos.md).

## Source

- Public repository commit: `<commit>` (tag `<tag>`)
- Source archive: the `va-studio-source-<commit>.tar.xz` above; see
  [SOURCE.md](../../share/doc/SOURCE.md).

## What is new

- <feature, one line each, with its menu or tool>

## Fixed

- <problem that is fixed, as the user saw it>

## Changed

- <behavior that differs from the previous release>

## Known issues

- <issue specific to this release>
- See also [KNOWN-ISSUES.md](KNOWN-ISSUES.md).

## Upgrading

Releases install side by side. Uninstall the previous version when you no
longer need it; your preferences are kept.

## Third-party components

Updated components in this release: <component old version -> new version>.
The full list is in [THIRD-PARTY-NOTICES.md](../../share/doc/THIRD-PARTY-NOTICES.md).
