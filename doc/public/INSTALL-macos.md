# Installing VA Studio on macOS

## Requirements

- A Mac with Apple silicon (arm64).
- macOS 26 or later. The bundled libraries are built for macOS 26, so earlier
  macOS versions are not supported.

## Check the download

The release notes list the SHA-256 checksum of every file. In Terminal:

```sh
shasum -a 256 ~/Downloads/VA-Studio-<version>-arm64.dmg
```

Compare the result with the release notes before you open the disk image.

## Install

1. Open the `.dmg` file.
2. Drag **VA Studio** to the **Applications** folder.
3. Eject the disk image and start VA Studio from Applications or Launchpad.

VA Studio can be installed next to Inkscape; they keep separate settings.

If macOS says that it cannot verify VA Studio, the release you downloaded is
not yet notarized by Apple (see the release notes). After checking the
checksum, open **System Settings > Privacy & Security** and choose
**Open Anyway** for VA Studio, then confirm. You only need to do this once.

## Permissions

When you open or save files on your Desktop, in Documents or Downloads, or on
removable volumes for the first time, macOS asks whether VA Studio may access
that location. VA Studio needs this only to read and write the files you
choose.

## Finder icons

When you save an SVG file, VA Studio gives it a Finder icon that shows the
drawing. The icon is written in the background into the file's Finder
metadata; the SVG content itself is not changed.

## Uninstall

Drag **VA Studio** from Applications to the Trash. Your documents are not
touched. Your preferences stay in VA Studio's folder under
`~/Library/Application Support`; delete that folder yourself if you no longer
want them.

## Troubleshooting

- VA Studio closed unexpectedly: it tries to save copies of unsaved documents
  and shows where it saved them. See [SUPPORT.md](../../SUPPORT.md) to report
  the problem.
- More known problems: [KNOWN-ISSUES.md](KNOWN-ISSUES.md).
