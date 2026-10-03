// SPDX-License-Identifier: GPL-2.0-or-later

#include "config.h" // Needed for WITH_LIBSPELLING

#include <algorithm>
#include <vector>

#include "ui/dialog/dialog-data.h"
#include "ui/explode-bitmap-feature.h"
#include "ui/icon-names.h"  // INKSCAPE_ICON macro

/*
 * In an ideal world, this information would be in .ui files for each
 * dialog (the .ui file would describe a dialog wrapped by a notebook
 * tab). At the moment we create each dialog notebook tab on the fly
 * so we need a place to keep this information.
 */

static std::span<const DialogData> all_dialog_data() {
    // Note the "AttrDialog" is now part of the "XMLDialog" and the "Style" dialog is part of the
    // "Selectors" dialog. Also note that the "AttrDialog" does not correspond to SP_VERB_DIALOG_ATTR!!!
    // (That would be the "ObjectAttributes" dialog.)

    // This is a list of all dialogs arranged by their categories and then by their importance.
    // They will be presented in this order in the UI (dialogs menu).
    // Popup menu arranges them in two columns, left to right, top down.
    // Keep that in mind rearranging them or adding a new one.
    static const std::vector<DialogData> dialog_data_list = {
        // clang-format off
    // BASICs ----------------------------------------
    {"FillStroke",         _("_Fill and Stroke"),      INKSCAPE_ICON("dialog-fill-and-stroke"),      DialogData::Basic,          ScrollProvider::NOPROVIDE },
    {"Objects",            _("Layers and Object_s"),   INKSCAPE_ICON("dialog-objects"),              DialogData::Basic,          ScrollProvider::PROVIDE   },
    {"AlignDistribute",    _("_Align and Distribute"), INKSCAPE_ICON("dialog-align-and-distribute"), DialogData::Basic,          ScrollProvider::NOPROVIDE },
    {"Transform",          _("Transfor_m"),            INKSCAPE_ICON("dialog-transform"),            DialogData::Basic,          ScrollProvider::NOPROVIDE },
    {"ObjectProperties",   _("_Object Properties"),    INKSCAPE_ICON("dialog-object-properties"),    DialogData::Basic,          ScrollProvider::NOPROVIDE },
    {"Export",             _("_Export"),               INKSCAPE_ICON("document-export"),             DialogData::Basic,          ScrollProvider::PROVIDE   },
    {"Swatches",           _("S_watches"),             INKSCAPE_ICON("swatches"),                    DialogData::Basic,          ScrollProvider::PROVIDE   },

    // TEXT ------------------------------------------
    {"Text",               _("_Text…"),                INKSCAPE_ICON("dialog-text-and-font"),        DialogData::Typography,     ScrollProvider::NOPROVIDE },
    {"FontBrowser",        _("_Font Browser"),         INKSCAPE_ICON("dialog-text-and-font"),        DialogData::Typography,     ScrollProvider::NOPROVIDE },
    {"FontCollections",    _("_Font Collections"),     INKSCAPE_ICON("font_collections"),            DialogData::Typography,     ScrollProvider::NOPROVIDE },
#if WITH_LIBSPELLING
    {"Spellcheck",         _("Check Spellin_g"),       INKSCAPE_ICON("tools-check-spelling"),        DialogData::Typography,     ScrollProvider::NOPROVIDE },
#endif
    {"Find",               _("_Find/Replace"),         INKSCAPE_ICON("edit-find"),                   DialogData::Typography,     ScrollProvider::NOPROVIDE },
    {"SVGFonts",           _("SVG Font Editor"),       INKSCAPE_ICON("dialog-svg-font"),             DialogData::Typography,     ScrollProvider::NOPROVIDE },

    // EFFECTS ---------------------------------------
    {"ExplodeBitmap",      _("Explode Bitmap"),         INKSCAPE_ICON("shape-image"),                 DialogData::EffectsActions, ScrollProvider::NOPROVIDE },
    {"BitmapAdjustments", _("_Bitmap Adjustments"),  INKSCAPE_ICON("shape-image"),                 DialogData::EffectsActions, ScrollProvider::NOPROVIDE },
    {"LivePathEffect",     _("Path E_ffects"),         INKSCAPE_ICON("dialog-path-effects"),         DialogData::EffectsActions, ScrollProvider::NOPROVIDE },
    {"Trace",              _("_Trace Bitmap"),         INKSCAPE_ICON("bitmap-trace"),                DialogData::EffectsActions, ScrollProvider::NOPROVIDE },
    {"FilterGallery",      _("Filter Gallery"),        INKSCAPE_ICON("color-filters"),               DialogData::EffectsActions, ScrollProvider::NOPROVIDE },
    {"FilterEffects",      _("Filter _Editor"),        INKSCAPE_ICON("dialog-filters"),              DialogData::EffectsActions, ScrollProvider::NOPROVIDE },
    {"ExtensionsGallery",  _("_Extension Gallery"),    INKSCAPE_ICON("dialog-extensions"),           DialogData::EffectsActions, ScrollProvider::NOPROVIDE },
    {"CloneTiler",         _("Tiled Clones"),          INKSCAPE_ICON("dialog-tile-clones"),          DialogData::EffectsActions, ScrollProvider::NOPROVIDE },

    // ASSETS ----------------------------------------
    {"Symbols",            _("S_ymbols"),              INKSCAPE_ICON("symbols"),                     DialogData::Assets,         ScrollProvider::PROVIDE   },
    {"ArtworkLibrary",     _("_Artwork Library"),      INKSCAPE_ICON("symbols"),                     DialogData::Assets,         ScrollProvider::PROVIDE   },
    {"DocumentResources",  _("_Document Resources"),   INKSCAPE_ICON("document-resources"),          DialogData::Assets,         ScrollProvider::NOPROVIDE },

    // ADVANCED --------------------------------------
    {"Selectors",          _("_Selectors and CSS"),    INKSCAPE_ICON("dialog-selectors"),            DialogData::Advanced,       ScrollProvider::PROVIDE   },
    {"XMLEditor",          _("_XML Editor"),           INKSCAPE_ICON("dialog-xml-editor"),           DialogData::Advanced,       ScrollProvider::NOPROVIDE },
    {"UndoHistory",        _("Undo _History"),         INKSCAPE_ICON("edit-undo-history"),           DialogData::Advanced,       ScrollProvider::NOPROVIDE },
    {"IconPreview",        _("Icon Preview"),          INKSCAPE_ICON("dialog-icon-preview"),         DialogData::Advanced,       ScrollProvider::NOPROVIDE },

    // SETTINGS --------------------------------------
    {"DocumentProperties", _("_Document Properties"),  INKSCAPE_ICON("document-properties"),         DialogData::Settings,       ScrollProvider::PROVIDE   },
    {"Preferences",        _("P_references"),          INKSCAPE_ICON("preferences-system"),          DialogData::Settings,       ScrollProvider::PROVIDE   },

    // All others (hidden) ---------------------------
    {"DebugWindow",        _("_Debugger"),             INKSCAPE_ICON("dialog-debug"),                DialogData::Diagnostics,    ScrollProvider::NOPROVIDE },
#if DEBUG
    {"Prototype",          _("Prototype"),             INKSCAPE_ICON("document-properties"),         DialogData::Other,          ScrollProvider::NOPROVIDE },
#endif
        // clang-format on
    };
    return dialog_data_list;
}

std::span<const DialogData> get_dialog_data_list() {
    auto const all = all_dialog_data();
    // Keep both lists alive: callers may retain a span while Preferences changes.
    static const auto disabled = [all] {
        std::vector<DialogData> list(all.begin(), all.end());
        std::erase_if(list, [](auto const &entry) { return entry.key == "ExplodeBitmap"; });
        return list;
    }();
    return Inkscape::Bitmap::explodeBitmapEnabled() ? all : std::span<const DialogData>(disabled);
}

// Metadata is also read by already-open notebooks when scrolling/reparenting.
// Keep it stable when opt-in is switched off. Discovery uses the gated list,
// and action/factory/restore entry points check the live flag before opening.
const std::map<std::string, DialogData>& get_dialog_data() {
    static auto const data = [] {
        std::map<std::string, DialogData> result;
        for (auto const &dlg : all_dialog_data()) result.emplace(dlg.key, dlg);
        return result;
    }();
    return data;
}

/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0)(case-label . +))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:fileencoding=utf-8:textwidth=99 :
