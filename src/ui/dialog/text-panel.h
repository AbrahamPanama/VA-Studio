// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_UI_DIALOG_TEXT_PANEL_H
#define INKSCAPE_UI_DIALOG_TEXT_PANEL_H

#include <array>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/dropdown.h>
#include <gtkmm/expander.h>
#include <gtkmm/grid.h>
#include <gtkmm/label.h>
#include <gtkmm/listbox.h>
#include <gtkmm/menubutton.h>
#include <gtkmm/popover.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/spinbutton.h>
#include <gtkmm/togglebutton.h>

#include "ui/dialog/dialog-base.h"
#include "ui/operation-blocker.h"
#include "ui/text-style-controller.h"
#include "ui/widget/font-list.h"
#include "ui/widget/font-size-selector.h"
#include "ui/widget/paint-popover-manager.h"

namespace Inkscape::UI::Dialog {

/** Compact, canvas-first text styling panel. */
class TextPanel final : public DialogBase
{
public:
    TextPanel();
    ~TextPanel() final;

    void focus_dialog() final;
    void update() final;

private:
    void desktopReplaced() final;
    void documentReplaced() final;
    void selectionChanged(Selection *) final;
    void selectionModified(Selection *, guint) final;
    void on_unmap() override;

    void buildInterface();
    void connectSignals();
    void connectFontChoices();
    void syncFromSelection();
    void rebuildFaces(TextStyleSnapshot const &snapshot);
    void requestFacePreview(Glib::ustring const &face, TextStyleOrigin origin);
    void commitFace(Glib::ustring const &face);
    void updateCapitalizationMenu(TextStyleSnapshot const &snapshot);
    void requestCapitalizationPreview(CapitalizationMode mode, TextStyleOrigin origin);
    void commitCapitalization(CapitalizationMode mode);
    void setupPaint(bool fill);
    std::vector<sigc::connection> connectPaint(bool fill);
    void commitPaint(bool fill, Glib::ustring const &paint, bool continuous = true);
    void cancelPanelPreview() noexcept;
    void restoreFocusAfterActivation();

    TextStyleController *controller() const;

    Gtk::ScrolledWindow _scroll;
    Gtk::Box _content{Gtk::Orientation::VERTICAL, 6};
    Gtk::Label _status;

    Gtk::Box _character{Gtk::Orientation::VERTICAL, 6};
    Gtk::Label _character_title;
    Gtk::Grid _font_grid;
    Gtk::MenuButton _font_button;
    Gtk::Label _font_button_label;
    Gtk::ToggleButton _font_only;
    std::unique_ptr<Inkscape::UI::Widget::FontList> _font_list;
    Gtk::Popover _font_popover;
    Inkscape::UI::Widget::FontSizeSelector _font_size;
    Gtk::MenuButton _face;
    Gtk::Label _face_label;
    Gtk::Popover _face_popover;
    Gtk::ScrolledWindow _face_scroll;
    Gtk::ListBox _face_list;
    std::vector<Glib::ustring> _face_specs;
    std::optional<Glib::ustring> _face_family;
    std::vector<std::pair<Glib::ustring, Glib::ustring>> _face_styles;

    Gtk::Box _quick_style{Gtk::Orientation::HORIZONTAL, 4};
    Gtk::ToggleButton _bold{"B"};
    Gtk::ToggleButton _italic{"I"};
    Gtk::ToggleButton _underline{"U"};
    Gtk::ToggleButton _superscript{"x²"};
    Gtk::ToggleButton _subscript{"x₂"};
    Gtk::MenuButton _fill;
    Gtk::MenuButton _stroke;

    Gtk::Box _case_row{Gtk::Orientation::HORIZONTAL, 6};
    Gtk::Label _case_label;
    Gtk::MenuButton _case_button;
    Gtk::Popover _case_popover;
    Gtk::Box _case_choices{Gtk::Orientation::VERTICAL, 0};
    std::array<Gtk::Button *, 7> _case_choice_buttons{};
    Gtk::ToggleButton _standard_ligatures;

    Gtk::Expander _spacing;
    Gtk::Grid _spacing_grid;
    Gtk::SpinButton _character_spacing;
    Gtk::SpinButton _word_spacing;
    Gtk::SpinButton _language_spacing;

    Gtk::Expander _paragraph;
    Gtk::Box _paragraph_box{Gtk::Orientation::VERTICAL, 6};
    Gtk::Grid _paragraph_grid;
    Gtk::Box _paragraph_alignment{Gtk::Orientation::HORIZONTAL, 2};
    std::array<Gtk::ToggleButton, 4> _paragraph_alignment_buttons;
    Gtk::SpinButton _line_height;
    Gtk::DropDown _line_height_unit;
    Gtk::SpinButton _first_line_indent;
    Gtk::SpinButton _paragraph_spacing_before;
    Gtk::SpinButton _paragraph_spacing_after;
    Gtk::DropDown _paragraph_direction;
    Gtk::DropDown _paragraph_writing_mode;
    Gtk::DropDown _paragraph_orientation;
    Gtk::Expander _paragraph_tools;
    Gtk::Box _paragraph_tools_box{Gtk::Orientation::HORIZONTAL, 4};
    std::array<Gtk::ToggleButton, 3> _list_mode_buttons;
    Gtk::SpinButton _list_start;
    Gtk::CheckButton _hyphenation;
    Gtk::CheckButton _drop_cap;
    Gtk::SpinButton _drop_cap_lines;
    Gtk::Expander _text_frame;
    Gtk::Grid _text_frame_grid;
    Gtk::SpinButton _frame_width;
    Gtk::SpinButton _frame_height;
    Gtk::SpinButton _frame_columns;
    Gtk::SpinButton _frame_gap;
    Gtk::DropDown _frame_vertical_alignment;

    std::optional<Inkscape::UI::Widget::PaintPopoverManager::Registration> _fill_registration;
    std::optional<Inkscape::UI::Widget::PaintPopoverManager::Registration> _stroke_registration;
    sigc::scoped_connection _font_choice;
    sigc::scoped_connection _font_popover_size;
    sigc::scoped_connection _font_lister_update;
    std::optional<Glib::ustring> _hovered_face;
    std::optional<CapitalizationMode> _hovered_capitalization;
    void setPreviewDesktop(SPDesktop *desktop);
    SPDesktop *_preview_desktop = nullptr;
    sigc::scoped_connection _preview_desktop_destroy;
    OperationBlocker _updating;
};

} // namespace Inkscape::UI::Dialog

#endif // INKSCAPE_UI_DIALOG_TEXT_PANEL_H
