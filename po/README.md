# Inkscape translations

This repository contains Inkscape translations.

It is synchronized with a [Weblate](https://weblate.org) instance, hosted here :

<div align="center">

---
 
# https://translate.inkscape.org

--- 
</div>


This website provides an overview of translatable strings, translation statistics, and an easy-to-use web interface to contribute to translations!


## Contacts and help
 - Chat: [#translators](https://chat.inkscape.org/channel/team_translation) 
 - Mailing-list : [inkscape-translators](https://lists.inkscape.org/postorius/lists/inkscape-translator.lists.inkscape.org/)


## Update process

### 0. Introduce yourselves to fellow translators

To coordinate with other translators for your language, please introduce yourself to the ML and join the chat mentioned above :) In some cases, we did not have a translator for your language for a long time, for other languages we have an active translator community happy to help you started!

### 1. Create a Gitlab account

You will need a Gitlab account to login into Weblate. If you are unable create one for some reason, please contact us on the chat.

### 2a. Translate: Option 1: from Weblate

Just login on https://translate.inkscape.org with your gitlab account, select your language, and start translating right there online!

### 2b. Translate: Option 2: With a translation software (poedit, etc)

You can download the latest po file from Weblate and use it in your favorite translation software. You can then upload it into Weblate (`File → Upload translation` menu). 

> [!caution] MRs
> The preferred way of contribution is to upload the file in Weblate.
> You can still submit it directly in the git repository through a MR, but the behavior will be to forcibly replace the current set of translated strings, while the default behavior when uploading to weblate is to merge the newly translated strings into the already translated weblate strings (preventing accidental loss of work) - please only go that route in the absence of alternatives.

### 3. Testing your translation

 1. When you have a `.po` file (directly with a translation software, or you can download it from weblate), you can ask a translation software to produce a `.mo` file. Rename it to `inkscape.mo`

 1. Replace that file in your installation of Inkscape, usually under `<install path>/locale/$YOURLANGUAGE/LC_MESSAGES/inkscape.mo` where the install path is listed in `Edit > Preferences > System > Inkscape Data` within Inkscape.

 1. You should see your strings when launching Inkscape !

> [!note] Notes
> You cannot easily edit/replace this file in dmg, Appimage, or flatpak versions of Inkscape.
> 
> You might need admin privileges for versions installed with msi, exe, or a package manager.
> 
> Make a backup of the previous file if you want to be able to cancel that change.



### Branches

 - Release branches are named after their main version, e.g. `1.4.x`
 - The `master` branch contains the latest development code, and may contain strings that will change before release.
 - The translation of different branches is an independent process, and not automated. You can import the work done in a branch into a different branch. So if you want to translate the next release (`Inkscape 1.4.3`), make sure your translations are in the `1.4.x` branch!

## LICENCE: GPL-2.0-or-later
Just like the rest of the Inkscape source code, this repository is available under the GNU General Public License, version 2 or later.

