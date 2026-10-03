// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_UI_DIALOG_BITMAP_COPY_DIALOG_H
#define INKSCAPE_UI_DIALOG_BITMAP_COPY_DIALOG_H
#include <cstdint>
#include <optional>
#include <glibmm/ustring.h>
#include <2geom/rect.h>
#include "object/object-set.h"
namespace Gtk { class Window; }
namespace Inkscape::UI::Dialog {
/// Pixel size and uncompressed RGBA size of a bitmap copy of `bounds_px`
/// (document px) at `dpi`, computed as ObjectSet::createBitmapCopy does.
struct BitmapCopySize { int width = 0; int height = 0; std::uint64_t bytes = 0; };
BitmapCopySize bitmap_copy_size(Geom::Rect const &bounds_px, int dpi);
/// For example "300 × 150 px · 180.0 kB" (sizes as g_format_size prints them).
Glib::ustring bitmap_copy_size_text(BitmapCopySize const &size);
/// Run the modal dialog; returns the chosen options, or nullopt on Cancel.
std::optional<Inkscape::BitmapCopyOptions> run_bitmap_copy_dialog(Gtk::Window &parent, Geom::Rect const &bounds_px,
                                                                  Inkscape::BitmapCopyOptions const &initial);

/**
 * CDR-1: ask whether to convert \a count clipped bitmaps to images, with the
 * Bitmap Copy options (no Keep original: each image replaces its clip). The
 * size readout is for \a largest_px, the largest item. \a with_vectors
 * clips that also hold vectors or text and \a in_effect_groups clips inside
 * groups with effects are mentioned as left unchanged.
 * \a on_open: asked while opening a CorelDRAW file, where it cannot be undone.
 */
std::optional<Inkscape::BitmapCopyOptions> run_clipped_bitmaps_dialog(Gtk::Window &parent, int count, int with_vectors,
                                                                      int in_effect_groups,
                                                                      Geom::Rect const &largest_px,
                                                                      Inkscape::BitmapCopyOptions const &initial,
                                                                      bool on_open);
} // namespace Inkscape::UI::Dialog
#endif // INKSCAPE_UI_DIALOG_BITMAP_COPY_DIALOG_H
