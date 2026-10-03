// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef SEEN_IMAGE_PROPERTIES_H
#define SEEN_IMAGE_PROPERTIES_H

#include <functional>
#include <giomm/file.h>
#include <glibmm/refptr.h>
#include <gtkmm/box.h>
#include <gtkmm/sizegroup.h>
#include <memory>

#include "object/sp-image.h"
#include "ui/operation-blocker.h"
#include "ui/widget-vfuncs-class-init.h"

namespace Cairo {
class Surface;
} // namespace Cairo

namespace Gtk {
class Grid;
class DropDown;
class Builder;
class Button;
class CheckButton;
class ComboBoxText;
class DrawingArea;
class Entry;
class Window;
} // namespace Gtk

namespace Inkscape::UI::Widget {
class InkSpinButton;
class TextEntry;

class ImageProperties final
    : public WidgetVfuncsClassInit
    , public Gtk::Box
{
public:
    ImageProperties();
    ~ImageProperties() override;

    void update(SPImage* image);

    Gtk::Grid& get_main() { return _main; }

    /// "Change Image" button handler. The chooser runs without a nested loop; the chosen file is
    /// applied only if the image it was started for is still the panel's image.
    void link_image();

    /// Test seam: replaces the native chooser. Receives the parent window and a completion to
    /// call, once, with the chosen file (null when dismissed). Pass an empty function to restore.
    using ChooserHook = std::function<void(Gtk::Window &, std::function<void(Glib::RefPtr<Gio::File>)>)>;
    static void set_chooser_hook_for_testing(ChooserHook hook);

private:
    void css_changed(GtkCssStyleChange *change) final;
    void update_bg_color();
    void reset_preview();
    void set_href(Glib::ustring const &href);

    Glib::RefPtr<Gtk::Builder> _builder;

    Gtk::Grid& _main;
    Gtk::DrawingArea& _preview;
    Gtk::CheckButton &_aspect;
    Gtk::CheckButton &_stretch;
    Gtk::DropDown& _rendering;
    InkSpinButton& _resolution;
    TextEntry& _url;
    Gtk::Button& _embed;
    int _preview_max_height;
    int _preview_max_width;
    SPImage* _image = nullptr;
    std::shared_ptr<bool> _alive = std::make_shared<bool>(true); ///< outlives callbacks; weakly held by them
    OperationBlocker _update;
    Cairo::RefPtr<Cairo::Surface> _preview_image;
    uint32_t _background_color = 0;
};

} // namespace Inkscape::UI::Widget

#endif // SEEN_IMAGE_PROPERTIES_H

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
