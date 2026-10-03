// SPDX-License-Identifier: GPL-2.0-or-later

#include "tiff-output.h"

#include <glibmm.h>
#include <glibmm/i18n.h>

#include "clear-n_.h"
#include "extension/output.h"
#include "extension/system.h"
#include "inkscape.h"
#include "io/tiff-export.h"
#include "path-prefix.h"
#include "ui/interface.h"

namespace Inkscape::Extension::Internal {

std::string TiffOutput::output_profile_path()
{
    auto const override = Glib::getenv("INKSCAPE_VACARDS_TIFF_ICC_PROFILE");
    if (!override.empty()) {
        return override;
    }

    // Release packaging validates this exact profile by SHA-256 before placing
    // it in the bundle. Do not silently use a same-named system profile: that
    // would make exported color depend on the machine doing the export.
    return Glib::build_filename(get_inkscape_datadir(), "inkscape", "color", "icc", "TheBest.icc");
}

void TiffOutput::export_raster(Output *module, SPDocument const *, std::string const &png_file, gchar const *filename)
{
    std::string error;
    Inkscape::IO::TiffExportOptions options;
    options.prevent_white_clipping = module && module->get_param_bool("prevent_white_clipping", false);
    options.include_transparent = options.prevent_white_clipping &&
                                  module->get_param_bool("prevent_white_clipping_transparent", false);
    options.clean_edges = !module || module->get_param_bool("clean_edges", true);
    options.hard_edges = module && module->get_param_bool("hard_edges", false);
    if (!Inkscape::IO::export_png_to_color_managed_tiff(png_file, filename, output_profile_path(), error, nullptr,
                                                        options)) {
        auto const message = Glib::ustring::compose(_("TIFF export failed: %1"), error);
        g_warning("%s", message.c_str());
        if (Inkscape::Application::exists() && INKSCAPE.use_gui()) {
            sp_ui_error_dialog(message.c_str());
        }
        throw Output::save_failed();
    }
}

void TiffOutput::init()
{
    build_from_mem("<inkscape-extension xmlns=\"" INKSCAPE_EXTENSION_URI "\">\n"
                   "  <name>" N_("Export to TIFF") "</name>\n"
                                                   "  <id>org.inkscape.raster.tiff_output</id>\n"
                   "  <param name=\"prevent_white_clipping\" type=\"bool\" gui-text=\"" N_("Prevent white clipping") "\" "
                   "gui-description=\"" N_("Write pure white (255,255,255) as 254,254,254 so printing software (RIP) "
                                            "does not treat it as transparent. Other colours and transparent areas are "
                                            "unchanged.") "\">false</param>\n"
                   "  <param name=\"prevent_white_clipping_transparent\" type=\"bool\" gui-text=\"" N_("Also change transparent white") "\" "
                   "gui-description=\"" N_("With Prevent white clipping on, also write fully transparent white as "
                                            "254,254,254 (still transparent), for printing software that ignores "
                                            "transparency.") "\">false</param>\n"
                   "  <param name=\"clean_edges\" type=\"bool\" gui-text=\"" N_("Clean edges for printing (RIP)") "\" "
                   "gui-description=\"" N_("Give the semi-transparent outline of opaque artwork the colour of the "
                                            "artwork next to it, and remove colour hidden under full transparency. "
                                            "Images made in other programs often carry dark edges that printing "
                                            "software prints as an outline. Translucent areas keep their colour.") "\">true</param>\n"
                   "  <param name=\"hard_edges\" type=\"bool\" gui-text=\"" N_("Hard edges") "\" "
                   "gui-description=\"" N_("No semi-transparent pixels: each pixel is either opaque or transparent "
                                            "(at 50%), for printing software that mishandles soft edges.") "\">false</param>\n"
                                                   "  <output raster=\"true\" priority=\"1\">\n"
                                                   "    <extension>.tiff</extension>\n"
                                                   "    <mimetype>image/tiff</mimetype>\n"
                                                   "    <filetypename>" N_(
                                                       "TIFF (*.tiff)") "</filetypename>\n"
                                                                        "    <filetypetooltip>" N_(
                                                                            "RGB TIFF with embedded VACards output "
                                                                            "profile") "</filetypetooltip>\n"
                                                                                       "  </output>\n"
                                                                                       "</inkscape-extension>",
                   std::make_unique<TiffOutput>());
}

} // namespace Inkscape::Extension::Internal
