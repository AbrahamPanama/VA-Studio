# Privacy

> LEGAL-CHECK: draft for the owner's legal review; not final.

VA Studio is a desktop application. It has no telemetry: it does not collect
usage statistics, does not report crashes over the network, does not check for
updates, and does not require an account. Your documents and settings stay on
your computer unless you send them somewhere yourself.

## What VA Studio stores on your computer

- Your documents, where you save them. On macOS, a saved SVG file also gets a
  Finder icon of its drawing, stored in the file's Finder metadata.
- Preferences, recently used files, custom palettes, extensions you install and
  similar settings, in VA Studio's profile folder in your user account.
- Automatic backup copies of open documents (autosave, see Edit > Preferences >
  Input/Output > Autosave) and emergency copies of unsaved documents after a
  crash.
- On Windows, after a crash: a short crash record and, when Windows provides
  one, a memory dump, in the CrashReports folder of the VA Studio profile. A
  memory dump can contain parts of the documents that were open. They stay on
  your computer unless you choose to send them with a problem report.
- Artwork libraries (`.valib`) where you save them.

## Optional features that use the internet

VA Studio connects to the internet only when you use one of these features.
The services named below receive your IP address and the requests needed for
the feature, and their own privacy policies apply.

| Feature | What is sent | Services |
| --- | --- | --- |
| File > Import Web Image... | your search terms; requests for the images you choose | Wikimedia Commons, Openclipart, Bioicons, Reactome, the Inkscape website gallery |
| Extensions > Manage Extensions... | requests for the list of extensions and for the ones you install; installing or updating some extensions downloads Python packages | the Inkscape website (inkscape.org), the Python Package Index (pypi.org) |
| Help menu links | the page address | your web browser opens the page (Inkscape website, W3C) |

Images you import from these sources come with their own licenses, which the
Import Web Image dialog shows.

## Windows File Explorer thumbnails and preview pane

The SVG thumbnails and preview pane that VA Studio installs are drawn on your
computer with the Microsoft Edge WebView2 Runtime. VA Studio's handler blocks
every network request that an SVG file could make while it is drawn, and it
never sends the file anywhere. The WebView2 Runtime itself is a Microsoft
component that may communicate with Microsoft, for example for its updates,
under Microsoft's privacy statement.

The handler can write a diagnostic log (timings and error codes, no file
names) only when the folder `C:\Users\Public\Documents\VAStudio-view1` exists
or the `VA_THUMB_LOG` environment variable names a log file.

## Contact

Questions about privacy: @VA_SUPPORT_URL@
