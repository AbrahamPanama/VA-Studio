# Installing VA Studio on Windows

## Requirements

- 64-bit Windows 10 or Windows 11 on an x64 processor.
- For SVG thumbnails and the preview pane in File Explorer: the Microsoft Edge
  WebView2 Runtime, which Windows 11 and up-to-date Windows 10 systems include.

## Choose an installer

Each release offers two installers. Both install a complete, self-contained
copy of VA Studio; you do not need to install Inkscape, Python or any other
runtime first.

| Installer | Installs for | Location | Needs administrator rights |
| --- | --- | --- | --- |
| `VA-Studio-<version>-windows-x64.exe` | your user account | a folder in your local application data | no |
| `VA-Studio-<version>-windows-x64-programfiles.exe` | all users | `C:\Program Files\VA Studio <version>` | yes |

Different versions install side by side, each in its own folder with its own
Start menu entry. An installer refuses to install over an existing installation
of the same version; uninstall that one first.

## Check the download

The release notes list the SHA-256 checksum of every file. In PowerShell:

```powershell
Get-FileHash .\VA-Studio-<version>-windows-x64.exe -Algorithm SHA256
```

Compare the result with the release notes before you run the installer.

## Install

1. Run the installer and follow its steps.
2. If Windows SmartScreen shows "Windows protected your PC", the installer is
   not yet digitally signed (see the release notes). After checking the
   checksum, choose **More info** and then **Run anyway**.
3. Start VA Studio from the Start menu (the Program Files installer also
   creates a desktop shortcut).

## File Explorer thumbnails and preview pane

The installer registers VA Studio's SVG thumbnail and preview handlers, which
show the drawing itself rather than the page. When the last VA Studio version is
uninstalled, the previous handlers are restored.

If Microsoft PowerToys shows SVG thumbnails or previews for your user, those take
precedence. VA Studio detects this and explains how to turn off the PowerToys
SVG add-ons; it does not change PowerToys settings itself.

## Uninstall

Open **Settings > Apps > Installed apps**, find **VA Studio <version>** and
choose **Uninstall**. Your documents are not touched. Your VA Studio preferences
stay in your user profile so that a later version can use them; delete that
folder yourself if you no longer want them.

## Troubleshooting

- VA Studio closed unexpectedly: it offers recovery copies of unsaved documents
  at the next start when they exist, and keeps a local crash record in its
  profile folder. See [SUPPORT.md](../../SUPPORT.md) to report the problem.
- More known problems: [KNOWN-ISSUES.md](KNOWN-ISSUES.md).
