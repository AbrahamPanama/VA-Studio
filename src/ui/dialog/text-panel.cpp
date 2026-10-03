// SPDX-License-Identifier: GPL-2.0-or-later

#include "text-panel.h"

#include <algorithm>
#include <cmath>

#include <glibmm/i18n.h>
#include <gtkmm/adjustment.h>
#include <gtkmm/eventcontrollerfocus.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/eventcontrollermotion.h>
#include <gtkmm/separator.h>
#include <gtkmm/stringlist.h>

#include "colors/color.h"
#include "desktop.h"
#include "document.h"
#include "gradient-chemistry.h"
#include "libnrtype/font-factory.h"
#include "libnrtype/font-lister.h"
#include "object/sp-gradient.h"
#include "object/sp-hatch.h"
#include "object/sp-mesh-gradient.h"
#include "object/sp-pattern.h"
#include "object/sp-text.h"
#include "object/sp-flowtext.h"
#include "selection.h"
#include "style.h"
#include "ui/widget/paint-enums.h"
#include "ui/widget/paint-switch.h"
#include "ui/util.h"
#include "util/units.h"

namespace Inkscape::UI::Dialog {
namespace {

constexpr std::array<char const *, 7> capitalization_labels = {
    N_("None"),
    N_("All Caps"),
    N_("Titling Caps"),
    N_("Small Caps (auto)"),
    N_("All Small Caps"),
    N_("Small Caps from Caps"),
    N_("Small Caps (synthesized)")
};

CapitalizationMode capitalization_mode(unsigned index)
{
    return static_cast<CapitalizationMode>(index);
}

unsigned capitalization_index(CapitalizationMode mode)
{
    return static_cast<unsigned>(mode);
}

void configure_spin(Gtk::SpinButton &spin, double value, double lower, double upper,
                    double step, unsigned digits, char const *tooltip)
{
    spin.set_adjustment(Gtk::Adjustment::create(value, lower, upper, step, step * 10.0));
    spin.set_digits(digits);
    spin.set_numeric();
    spin.set_width_chars(6);
    spin.set_hexpand(false);
    spin.set_halign(Gtk::Align::START);
    spin.set_tooltip_text(tooltip);
}

void attach_spacing_control(Gtk::Grid &grid, int column, char const *label,
                            Gtk::Widget &widget, char const *suffix)
{
    auto text = Gtk::make_managed<Gtk::Label>(label);
    text->set_halign(Gtk::Align::START);
    text->set_mnemonic_widget(widget);
    grid.attach(*text, column, 0, 2, 1);
    grid.attach(widget, column, 1, 1, 1);

    auto unit = Gtk::make_managed<Gtk::Label>(suffix);
    unit->add_css_class("dim-label");
    unit->set_margin_end(8);
    grid.attach(*unit, column + 1, 1, 1, 1);
}

Glib::ustring paint_url(SPObject *object)
{
    return object && object->getId() ? Glib::ustring{object->getUrl()} : Glib::ustring{};
}

Glib::ustring paint_summary(TextStyleValue<Glib::ustring> const &paint, char const *name)
{
    if (!paint.valid) return _(name);
    if (paint.mixed) return Glib::ustring::compose(_("%1 · Mixed"), _(name));
    if (paint.value == "none") return Glib::ustring::compose(_("%1 · None"), _(name));
    return _(name);
}

void set_mixed(Gtk::ToggleButton &button, TextStyleValue<bool> const &value)
{
    button.set_active(value.valid && !value.mixed && value.value);
    if (value.mixed) {
        button.add_css_class("mixed");
    } else {
        button.remove_css_class("mixed");
    }
}

// Paragraph/frame enumeration dropdowns keep their real option indices stable.
// A heterogeneous selection is shown by appending a presentation-only
// "Multiple values" item after the real options: selecting it never reaches a
// commit handler. The sentinel is present only while the selection is mixed, so
// a uniform selection never offers it. When there is no target or the value is
// invalid the control is cleared rather than retaining the previous
// selection's value, which would otherwise look like a real current setting.
//
// The underlying Gtk::StringList identity is kept for the control's whole
// lifetime and mutated in place. Replacing the model would unref the internal
// GtkSingleSelection that is still emitting notify::selected while a commit
// handler synchronously calls this helper (a use-after-free), so set_model must
// not run from a sync. Clearing the items is what makes the native selection
// truly invalid: GtkSingleSelection rejects GTK_INVALID_LIST_POSITION on a
// non-empty model because DropDown uses can-unselect=false/autoselect=true.
void sync_enum_dropdown(Gtk::DropDown &dropdown,
                        std::vector<Glib::ustring> const &real_labels,
                        std::optional<unsigned> real_index,
                        bool mixed, bool has_target)
{
    auto string_list = std::dynamic_pointer_cast<Gtk::StringList>(dropdown.get_model());
    if (!string_list) {
        return;
    }

    bool const clear = !has_target || (!mixed && !real_index);
    std::vector<Glib::ustring> wanted;
    if (!clear) {
        wanted = real_labels;
        if (mixed) wanted.emplace_back(_("Multiple values"));
    }

    bool same = string_list->get_n_items() == wanted.size();
    for (unsigned i = 0; same && i < wanted.size(); ++i) {
        if (string_list->get_string(i) != wanted[i]) same = false;
    }
    if (!same) {
        string_list->splice(0, string_list->get_n_items(), wanted);
    }

    if (clear) {
        dropdown.set_selected(GTK_INVALID_LIST_POSITION);
    } else if (mixed) {
        dropdown.set_selected(static_cast<guint>(real_labels.size()));
    } else {
        dropdown.set_selected(static_cast<guint>(*real_index));
    }
}

} // namespace

TextPanel::TextPanel()
    : DialogBase("/dialogs/textandfont", "Text")
    , _font_list(std::make_unique<Inkscape::UI::Widget::FontList>("/font-selector",
          Inkscape::UI::Widget::FontListMode::CompactPopover))
{
    set_name("TextPanel");
    set_orientation(Gtk::Orientation::VERTICAL);
    buildInterface();
    connectSignals();
    set_defocus_target(this, this);
}

TextPanel::~TextPanel()
{
    cancelPanelPreview();
    _font_choice.disconnect();
    _font_lister_update.disconnect();
    _fill_registration.reset();
    _stroke_registration.reset();
}

void TextPanel::focus_dialog()
{
    DialogBase::focus_dialog();

    // Native menu activation can restore focus to the canvas after the menu
    // callback returns. Reassert the dialog's last (or first) focusable child
    // on the next UI turn so the panel is reachable without a pointer.
    Glib::signal_idle().connect_once(
        sigc::mem_fun(*this, &TextPanel::restoreFocusAfterActivation),
        Glib::PRIORITY_HIGH_IDLE);
}

void TextPanel::restoreFocusAfterActivation()
{
    if (!get_mapped()) {
        return;
    }

    if (_font_button.get_sensitive()) {
        _font_button.grab_focus();
    } else {
        DialogBase::focus_dialog();
    }
}

void TextPanel::buildInterface()
{
    _scroll.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    _scroll.set_propagate_natural_height();
    _scroll.set_child(_content);
    append(_scroll);

    _content.set_spacing(6);
    _content.set_margin_start(8);
    _content.set_margin_end(8);
    _content.set_margin_top(8);
    _content.set_margin_bottom(8);

    _status.set_halign(Gtk::Align::START);
    _status.set_wrap();
    _status.add_css_class("dim-label");
    _content.append(_status);

    _character.add_css_class("card");
    _character.set_spacing(6);
    _character_title.set_text(_("Character"));
    _character_title.set_halign(Gtk::Align::START);
    _character_title.add_css_class("heading");
    _character.append(_character_title);

    _font_grid.set_column_spacing(4);
    _font_grid.set_row_spacing(4);
    _font_grid.set_hexpand();

    _font_button_label.set_ellipsize(Pango::EllipsizeMode::END);
    _font_button_label.set_xalign(0.0f);
    _font_button_label.set_hexpand();
    _font_button.set_child(_font_button_label);
    _font_button.set_hexpand();
    _font_button.set_tooltip_text(_("Search and choose a font family"));
    _font_popover.set_child(*_font_list);
    auto const compact_size = _font_list->compact_size();
    _font_popover.set_size_request(compact_size.width, compact_size.height);
    _font_popover_size = _font_list->signal_compact_size_changed().connect(
        [this](int width, int height) {
            _font_popover.set_size_request(width, height);
        });
    _font_button.set_popover(_font_popover);

    _face_label.set_ellipsize(Pango::EllipsizeMode::END);
    _face_label.set_xalign(0.0f);
    _face_label.set_hexpand();
    _face.set_child(_face_label);
    _face.set_hexpand();
    _face.set_tooltip_text(_("Font style or face"));
    _face_list.set_activate_on_single_click();
    _face_list.set_selection_mode(Gtk::SelectionMode::SINGLE);
    _face_scroll.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    _face_scroll.set_min_content_width(240);
    _face_scroll.set_max_content_height(280);
    _face_scroll.set_propagate_natural_height();
    _face_scroll.set_child(_face_list);
    _face_popover.set_child(_face_scroll);
    _face.set_popover(_face_popover);

    _font_size.set_hexpand(false);
    _font_size.set_halign(Gtk::Align::END);
    _font_only.set_label(_("Font only"));
    _font_only.set_tooltip_text(_("Change only the font family; keep each text fragment's size and formatting."));
    _font_only.set_focusable(true);
    _font_grid.attach(_font_button, 0, 0, 1, 1);
    _font_grid.attach(_font_only, 1, 0, 1, 1);
    _font_grid.attach(_font_size, 2, 0, 1, 1);
    _font_grid.attach(_face, 0, 1, 3, 1);
    _character.append(_font_grid);

    _quick_style.set_homogeneous(false);
    for (auto button : {&_bold, &_italic, &_underline, &_superscript, &_subscript}) {
        button->set_has_frame(false);
        button->set_focus_on_click(false);
    }
    _bold.set_tooltip_text(_("Bold"));
    _italic.set_tooltip_text(_("Italic or oblique"));
    _underline.set_tooltip_text(_("Underline"));
    _superscript.set_tooltip_text(_("Superscript"));
    _subscript.set_tooltip_text(_("Subscript"));
    _quick_style.append(_bold);
    _quick_style.append(_italic);
    _quick_style.append(_underline);
    _quick_style.append(_superscript);
    _quick_style.append(_subscript);

    _fill.set_label(_("Fill"));
    _fill.set_hexpand(false);
    _fill.set_tooltip_text(_("Text fill"));
    _stroke.set_label(_("Stroke"));
    _stroke.set_hexpand(false);
    _stroke.set_tooltip_text(_("Text stroke"));
    _quick_style.append(_fill);
    _quick_style.append(_stroke);
    _character.append(_quick_style);

    _case_label.set_text(_("Capitalization"));
    _case_label.set_halign(Gtk::Align::START);
    _case_label.set_width_chars(13);
    _case_button.set_label(_("None"));
    _case_button.set_size_request(190, -1);
    _case_button.set_hexpand(false);
    _case_button.set_halign(Gtk::Align::START);
    _case_popover.set_child(_case_choices);
    _case_button.set_popover(_case_popover);
    _case_row.append(_case_label);
    _case_row.append(_case_button);
    _character.append(_case_row);

    for (unsigned i = 0; i < _case_choice_buttons.size(); ++i) {
        auto button = Gtk::make_managed<Gtk::Button>(_(capitalization_labels[i]));
        button->set_has_frame(false);
        button->set_halign(Gtk::Align::FILL);
        button->set_hexpand();
        _case_choices.append(*button);
        _case_choice_buttons[i] = button;
    }

    _standard_ligatures.set_label(_("Standard ligatures"));
    _standard_ligatures.set_has_frame(false);
    _standard_ligatures.set_halign(Gtk::Align::START);
    _standard_ligatures.set_tooltip_text(_("Enable common OpenType ligatures without changing other features"));
    _character.append(_standard_ligatures);
    _content.append(_character);

    _spacing.set_label(_("Spacing"));
    _spacing.set_expanded();
    _spacing_grid.set_column_spacing(4);
    _spacing_grid.set_row_spacing(4);
    _spacing_grid.set_margin_top(4);
    _spacing_grid.set_margin_bottom(2);
    configure_spin(_character_spacing, 0, -100, 2000, 1, 1,
                   _("Character spacing relative to the effective space advance"));
    configure_spin(_word_spacing, 100, 0, 2000, 1, 1,
                   _("Word spacing; 100% is normal"));
    configure_spin(_language_spacing, 0, 0, 2000, 1, 1,
                   _("Extra spacing at adjacent Latin/number and East Asian script boundaries"));
    attach_spacing_control(_spacing_grid, 0, _("Characters"), _character_spacing, "%");
    attach_spacing_control(_spacing_grid, 2, _("Words"), _word_spacing, "%");
    attach_spacing_control(_spacing_grid, 4, _("Latin ↔ Asian"), _language_spacing, "%");
    _spacing.set_child(_spacing_grid);
    _content.append(_spacing);

    _paragraph.set_label(_("Paragraph"));
    _paragraph.set_expanded(false);
    _paragraph_grid.set_column_spacing(6);
    _paragraph_grid.set_row_spacing(4);
    _paragraph_grid.set_margin_top(4);
    _paragraph_grid.set_margin_bottom(2);
    _paragraph_grid.set_hexpand();

    constexpr std::array<char const *, 4> alignment_icons = {
        "format-justify-left", "format-justify-center",
        "format-justify-right", "format-justify-fill"
    };
    constexpr std::array<char const *, 4> alignment_tooltips = {
        N_("Align paragraph to start"), N_("Center paragraph"),
        N_("Align paragraph to end"), N_("Justify paragraph except its final line")
    };
    for (unsigned i = 0; i < _paragraph_alignment_buttons.size(); ++i) {
        auto &button = _paragraph_alignment_buttons[i];
        button.set_icon_name(alignment_icons[i]);
        button.set_tooltip_text(_(alignment_tooltips[i]));
        button.set_has_frame(false);
        button.set_focus_on_click(false);
        if (i > 0) button.set_group(_paragraph_alignment_buttons[0]);
        _paragraph_alignment.append(button);
    }
    _paragraph_grid.attach(_paragraph_alignment, 0, 0, 2, 1);

    auto line_height_label = Gtk::make_managed<Gtk::Label>(_("Line height"));
    line_height_label->set_halign(Gtk::Align::END);
    line_height_label->set_mnemonic_widget(_line_height);
    configure_spin(_line_height, 1.25, 0.01, 1000, 0.01, 2,
                   _("Distance between baselines"));
    auto line_height_units = Gtk::StringList::create();
    for (auto const unit : {TextLineHeightUnit::Lines, TextLineHeightUnit::Percent,
                            TextLineHeightUnit::Px, TextLineHeightUnit::Pt,
                            TextLineHeightUnit::Mm, TextLineHeightUnit::Cm,
                            TextLineHeightUnit::In}) {
        line_height_units->append(lineHeightUnitLabel(unit));
    }
    _line_height_unit.set_model(line_height_units);
    _line_height_unit.set_selected(0);
    _line_height_unit.set_tooltip_text(_("Line-height unit"));
    _paragraph_grid.attach(*line_height_label, 2, 0, 1, 1);
    _paragraph_grid.attach(_line_height, 3, 0, 1, 1);
    _paragraph_grid.attach(_line_height_unit, 4, 0, 1, 1);

    auto make_model = [](std::initializer_list<Glib::ustring> labels) {
        auto model = Gtk::StringList::create();
        for (auto const &label : labels) model->append(label);
        return model;
    };
    _paragraph_direction.set_model(make_model({_("LTR"), _("RTL")}));
    _paragraph_writing_mode.set_model(make_model({
        _("Horizontal"), _("Vertical RL"), _("Vertical LR")
    }));
    _paragraph_orientation.set_model(make_model({
        _("Auto glyphs"), _("Upright glyphs"), _("Sideways glyphs")
    }));
    _paragraph_direction.set_tooltip_text(_("Paragraph text direction"));
    _paragraph_writing_mode.set_tooltip_text(_("Text writing mode"));
    _paragraph_orientation.set_tooltip_text(_("Glyph orientation in vertical text"));
    _paragraph_direction.set_hexpand();
    _paragraph_writing_mode.set_hexpand();
    _paragraph_orientation.set_hexpand();
    _paragraph_grid.attach(_paragraph_direction, 0, 1, 1, 1);
    _paragraph_grid.attach(_paragraph_writing_mode, 1, 1, 2, 1);
    _paragraph_grid.attach(_paragraph_orientation, 3, 1, 2, 1);

    auto indent_label = Gtk::make_managed<Gtk::Label>(_("First-line indent"));
    indent_label->set_halign(Gtk::Align::END);
    indent_label->set_mnemonic_widget(_first_line_indent);
    configure_spin(_first_line_indent, 0, -10000, 10000, 1, 2,
                   _("Offset of the first formatted line; negative values create a hanging indent"));
    auto indent_unit = Gtk::make_managed<Gtk::Label>("px");
    indent_unit->add_css_class("dim-label");
    _paragraph_grid.attach(*indent_label, 0, 2, 2, 1);
    _paragraph_grid.attach(_first_line_indent, 2, 2, 2, 1);
    _paragraph_grid.attach(*indent_unit, 4, 2, 1, 1);

    auto before_label = Gtk::make_managed<Gtk::Label>(_("Before (px)"));
    auto after_label = Gtk::make_managed<Gtk::Label>(_("After (px)"));
    before_label->set_halign(Gtk::Align::END);
    after_label->set_halign(Gtk::Align::END);
    before_label->set_mnemonic_widget(_paragraph_spacing_before);
    after_label->set_mnemonic_widget(_paragraph_spacing_after);
    configure_spin(_paragraph_spacing_before, 0, 0, 10000, 1, 2,
                   _("Extra block-axis space before each targeted paragraph"));
    configure_spin(_paragraph_spacing_after, 0, 0, 10000, 1, 2,
                   _("Extra block-axis space after each targeted paragraph"));
    _paragraph_grid.attach(*before_label, 0, 3, 1, 1);
    _paragraph_grid.attach(_paragraph_spacing_before, 1, 3, 1, 1);
    _paragraph_grid.attach(*after_label, 2, 3, 1, 1);
    _paragraph_grid.attach(_paragraph_spacing_after, 3, 3, 2, 1);
    _paragraph_box.append(_paragraph_grid);
    _paragraph.set_child(_paragraph_box);
    _content.append(_paragraph);

    _paragraph_tools.set_label(_("Paragraph tools"));
    constexpr std::array<char const *, 3> list_labels = {
        N_("No list"), N_("Bulleted list"), N_("Numbered list")
    };
    constexpr std::array<char const *, 3> list_icons = {
        "edit-clear", "format-list-unordered", "format-list-ordered"
    };
    for (unsigned i = 0; i < _list_mode_buttons.size(); ++i) {
        auto &button = _list_mode_buttons[i];
        button.set_icon_name(list_icons[i]);
        button.set_tooltip_text(_(list_labels[i]));
        if (i > 0) button.set_group(_list_mode_buttons[0]);
        _paragraph_tools_box.append(button);
    }
    auto start_label = Gtk::make_managed<Gtk::Label>(_("Start"));
    configure_spin(_list_start, 1, 1, 100000, 1, 0,
                   _("Starting number for the selected numbered paragraphs"));
    _list_start.set_width_chars(5);
    _paragraph_tools_box.append(*start_label);
    _paragraph_tools_box.append(_list_start);
    _hyphenation.set_label(_("Hyphenate"));
    _hyphenation.set_tooltip_text(_("Automatically hyphenate using the paragraph language dictionary"));
    _paragraph_tools_box.append(_hyphenation);
    _drop_cap.set_label(_("Drop cap"));
    _drop_cap.set_tooltip_text(_("Enlarge the first character across multiple lines"));
    _paragraph_tools_box.append(_drop_cap);
    configure_spin(_drop_cap_lines, 3, 2, 10, 1, 0,
                   _("Number of lines occupied by the drop cap"));
    _drop_cap_lines.set_width_chars(2);
    _paragraph_tools_box.append(_drop_cap_lines);
    _paragraph_tools.set_child(_paragraph_tools_box);
    _content.append(_paragraph_tools);

    _text_frame.set_label(_("Text frame & columns"));
    _text_frame.set_expanded(false);
    _text_frame_grid.set_column_spacing(6);
    _text_frame_grid.set_row_spacing(4);
    _text_frame_grid.set_margin_top(4);
    auto attach_frame = [this](Gtk::SpinButton &spin, char const *label, int column,
                               double value, double lower, double upper, double step) {
        auto text = Gtk::make_managed<Gtk::Label>(label);
        text->set_halign(Gtk::Align::START);
        text->set_mnemonic_widget(spin);
        configure_spin(spin, value, lower, upper, step, 1, label);
        _text_frame_grid.attach(*text, column, 0, 1, 1);
        _text_frame_grid.attach(spin, column, 1, 1, 1);
    };
    attach_frame(_frame_width, _("Width (px)"), 0, 200, 1, 100000, 1);
    attach_frame(_frame_height, _("Height (px)"), 1, 120, 1, 100000, 1);
    attach_frame(_frame_columns, _("Columns"), 2, 1, 1, 20, 1);
    attach_frame(_frame_gap, _("Gap (px)"), 3, 12, 0, 10000, 1);
    _frame_vertical_alignment.set_model(make_model({_("Top"), _("Middle"), _("Bottom")}));
    _frame_vertical_alignment.set_tooltip_text(_("Vertical alignment within a single text frame"));
    _text_frame_grid.attach(_frame_vertical_alignment, 4, 1, 1, 1);
    _text_frame.set_child(_text_frame_grid);
    _content.append(_text_frame);
}

void TextPanel::connectFontChoices()
{
    // Popdown can defer unmapping. Purge old queued events before connecting
    // a listener that captures the new desktop/policy generation.
    _font_list->cancel_pending_choices();
    _font_choice.disconnect();
    auto const desktop = getDesktop();
    auto const generation = controller() ? controller()->fontPolicyGeneration() : 0;
    auto const policy = controller() && controller()->panelFontOnly()
        ? FontChoicePolicy::FamilyOnly : FontChoicePolicy::NormalizeFace;
    _font_choice = _font_list->signal_font_choice().connect([this, desktop, generation, policy](FontChoice const &choice) {
        if (desktop != getDesktop()) return;
        if (auto style = controller()) {
            if (generation != style->fontPolicyGeneration()) return;
            if (choice.phase == FontChoicePhase::Preview) {
                setPreviewDesktop(getDesktop());
            }
            style->requestFontChoice(choice, policy, generation);
            if (choice.phase == FontChoicePhase::Commit) {
                setPreviewDesktop(nullptr);
                _font_popover.popdown();
                syncFromSelection();
            } else if (choice.phase == FontChoicePhase::Cancel) {
                setPreviewDesktop(nullptr);
                if (choice.origin == FontChoiceOrigin::Keyboard) {
                    _font_popover.popdown();
                }
            }
        }
    });
}

void TextPanel::connectSignals()
{
    connectFontChoices();
    _font_only.signal_toggled().connect([this] {
        if (_updating.pending() || !controller()) return;
        // Unmapping the list also discards its pending pointer-exit callback.
        // Close a face popover before changing policy so a queued face activation
        // cannot turn into a formatting edit under the new mode.
        _font_popover.popdown();
        _face_popover.popdown();
        cancelPanelPreview();
        controller()->setPanelFontOnly(_font_only.get_active());
        connectFontChoices();
        syncFromSelection();
    });
    _font_popover.signal_closed().connect([this] {
        cancelPanelPreview();
    });
    _font_popover.signal_show().connect([this] {
        connectFontChoices();
        // Gtk::MenuButton may recalculate its popover after adopting it. Reapply
        // the persisted compact dimensions at map time so the sidebar width and
        // font-row contents can never collapse the selector.
        auto const compact_size = _font_list->compact_size();
        _font_popover.set_size_request(compact_size.width, compact_size.height);
        _font_popover.queue_resize();
        Glib::signal_idle().connect_once(
            sigc::mem_fun(*_font_list, &Inkscape::UI::Widget::FontList::focus_search),
            Glib::PRIORITY_HIGH_IDLE);
    });

    _face_list.signal_row_activated().connect([this](Gtk::ListBoxRow *row) {
        auto const index = row ? row->get_index() : -1;
        if (index < 0 || static_cast<unsigned>(index) >= _face_specs.size()) return;
        commitFace(_face_specs[index]);
    });
    _face_popover.signal_closed().connect([this] {
        _hovered_face.reset();
        cancelPanelPreview();
    });
    auto face_keys = Gtk::EventControllerKey::create();
    face_keys->signal_key_pressed().connect([this](unsigned keyval, unsigned, Gdk::ModifierType) {
        if (keyval != GDK_KEY_Escape) return false;
        cancelPanelPreview();
        _face_popover.popdown();
        return true;
    }, false);
    _face_list.add_controller(face_keys);

    _font_size.signal_size_changed().connect([this](double size, int unit) {
        // A mixed selection deliberately has no editable numeric value. The
        // underlying adjustment may still emit while focus moves through the
        // control (for example after adding a relative-size drop cap). Never
        // turn that implementation value into a real font-size change. A user
        // edit clears the mixed state before emitting, so it still commits.
        if (_updating.pending() || _font_size.isMixed() || size <= 0 || !controller()) return;
        TextStylePatch patch;
        patch.font_size_px = sp_style_css_size_units_to_px(size, unit);
        controller()->commitContinuous(patch, "text-panel:size", RC_("Undo", "Set font size"));
    });

    _bold.signal_toggled().connect([this] {
        if (_updating.pending() || !controller()) return;
        TextStylePatch patch; patch.bold = _bold.get_active();
        controller()->commit(patch, "text-panel:bold", RC_("Undo", "Toggle bold text"));
        syncFromSelection();
    });
    _italic.signal_toggled().connect([this] {
        if (_updating.pending() || !controller()) return;
        TextStylePatch patch; patch.italic = _italic.get_active();
        controller()->commit(patch, "text-panel:italic", RC_("Undo", "Toggle italic text"));
        syncFromSelection();
    });
    _underline.signal_toggled().connect([this] {
        if (_updating.pending() || !controller()) return;
        TextStylePatch patch; patch.underline = _underline.get_active();
        controller()->commit(patch, "text-panel:underline", RC_("Undo", "Toggle underline"));
        syncFromSelection();
    });
    _superscript.signal_toggled().connect([this] {
        if (_updating.pending() || !controller()) return;
        TextStylePatch patch;
        patch.script_position = _superscript.get_active()
                              ? TextScriptPosition::Superscript : TextScriptPosition::Normal;
        controller()->commit(patch, "text-panel:script", RC_("Undo", "Set superscript"));
        syncFromSelection();
    });
    _subscript.signal_toggled().connect([this] {
        if (_updating.pending() || !controller()) return;
        TextStylePatch patch;
        patch.script_position = _subscript.get_active()
                              ? TextScriptPosition::Subscript : TextScriptPosition::Normal;
        controller()->commit(patch, "text-panel:script", RC_("Undo", "Set subscript"));
        syncFromSelection();
    });
    _standard_ligatures.signal_toggled().connect([this] {
        if (_updating.pending() || !controller()) return;
        TextStylePatch patch; patch.standard_ligatures = _standard_ligatures.get_active();
        controller()->commit(patch, "text-panel:ligatures", RC_("Undo", "Set standard ligatures"));
        syncFromSelection();
    });

    for (unsigned i = 0; i < _case_choice_buttons.size(); ++i) {
        auto button = _case_choice_buttons[i];
        auto const mode = capitalization_mode(i);
        button->signal_clicked().connect([this, mode] { commitCapitalization(mode); });

        auto motion = Gtk::EventControllerMotion::create();
        motion->signal_motion().connect([this, mode](double, double) {
            if (_hovered_capitalization == mode) return;
            _hovered_capitalization = mode;
            requestCapitalizationPreview(mode, TextStyleOrigin::Pointer);
        });
        motion->signal_leave().connect([this] {
            _hovered_capitalization.reset();
            cancelPanelPreview();
        });
        button->add_controller(motion);

        auto focus = Gtk::EventControllerFocus::create();
        focus->signal_enter().connect([this, mode] {
            requestCapitalizationPreview(mode, TextStyleOrigin::Keyboard);
        });
        button->add_controller(focus);
    }
    _case_popover.signal_closed().connect([this] {
        _hovered_capitalization.reset();
        cancelPanelPreview();
    });
    auto case_keys = Gtk::EventControllerKey::create();
    case_keys->signal_key_pressed().connect([this](unsigned keyval, unsigned, Gdk::ModifierType) {
        if (keyval != GDK_KEY_Escape) return false;
        cancelPanelPreview();
        _case_popover.popdown();
        return true;
    }, false);
    _case_choices.add_controller(case_keys);

    auto connect_spacing = [this](Gtk::SpinButton &spin, auto member,
                                  char const *key, Util::Internal::ContextString label) {
        spin.signal_value_changed().connect([this, &spin, member, key, label] {
            if (_updating.pending() || !controller()) return;
            TextStylePatch patch;
            patch.*member = spin.get_value();
            controller()->commitContinuous(patch, key, label);
        });
    };
    connect_spacing(_character_spacing, &TextStylePatch::character_spacing_percent,
                    "text-panel:character-spacing", RC_("Undo", "Set character spacing"));
    connect_spacing(_word_spacing, &TextStylePatch::word_spacing_percent,
                    "text-panel:word-spacing", RC_("Undo", "Set word spacing"));
    connect_spacing(_language_spacing, &TextStylePatch::language_spacing_percent,
                    "text-panel:language-spacing", RC_("Undo", "Set language spacing"));

    constexpr std::array<TextParagraphAlignment, 4> paragraph_alignments = {
        TextParagraphAlignment::Start, TextParagraphAlignment::Center,
        TextParagraphAlignment::End, TextParagraphAlignment::Justify
    };
    for (unsigned i = 0; i < _paragraph_alignment_buttons.size(); ++i) {
        auto const alignment = paragraph_alignments[i];
        _paragraph_alignment_buttons[i].signal_toggled().connect([this, i, alignment] {
            if (_updating.pending() || !_paragraph_alignment_buttons[i].get_active() || !controller()) {
                return;
            }
            TextParagraphPatch patch;
            patch.alignment = alignment;
            controller()->commitParagraph(patch, "text-panel:paragraph-alignment",
                                          RC_("Undo", "Set paragraph alignment"));
            syncFromSelection();
        });
    }
    _line_height.signal_value_changed().connect([this] {
        if (_updating.pending() || !controller()) return;
        TextParagraphPatch patch;
        patch.line_height = TextLineHeightValue{
            _line_height.get_value(), static_cast<TextLineHeightUnit>(_line_height_unit.get_selected())
        };
        controller()->commitParagraphContinuous(patch, "text-panel:line-height",
                                                RC_("Undo", "Set paragraph line height"));
    });
    _line_height_unit.property_selected().signal_changed().connect([this] {
        if (_updating.pending() || !controller()) return;
        auto const paragraph = controller()->queryParagraph();
        if (!paragraph.line_height.valid || paragraph.line_height.mixed) return;
        auto const destination = static_cast<TextLineHeightUnit>(_line_height_unit.get_selected());
        // Line-height snapshots are document units, so convert with the document
        // font size the controller query already reports. Fall back to the
        // existing representative local style only when no font query is
        // available; the query is representative-first, not an average.
        auto const snapshot = controller()->query();
        double font_size = 20.0;
        if (snapshot.font_size_px.valid) {
            font_size = snapshot.font_size_px.value;
        } else if (auto const style = controller()->representativeRunStyle()) {
            font_size = style->font_size.computed;
        }
        auto const converted = convertLineHeight(paragraph.line_height.value, destination, font_size);
        {
            auto updating = _updating.block();
            _line_height.set_value(converted.value);
        }
        TextParagraphPatch patch;
        patch.line_height = converted;
        controller()->commitParagraph(patch, "text-panel:line-height-unit",
                                      RC_("Undo", "Set paragraph line-height unit"));
        syncFromSelection();
    });
    _first_line_indent.signal_value_changed().connect([this] {
        if (_updating.pending() || !controller()) return;
        TextParagraphPatch patch;
        patch.first_line_indent_px = _first_line_indent.get_value();
        controller()->commitParagraphContinuous(patch, "text-panel:first-line-indent",
                                                RC_("Undo", "Set first-line indent"));
    });
    auto connect_paragraph_spacing = [this](Gtk::SpinButton &spin, auto member,
                                            char const *key,
                                            Util::Internal::ContextString label) {
        spin.signal_value_changed().connect([this, &spin, member, key, label] {
            if (_updating.pending() || !controller()) return;
            TextParagraphPatch patch;
            patch.*member = spin.get_value();
            controller()->commitParagraphContinuous(patch, key, label);
        });
    };
    connect_paragraph_spacing(_paragraph_spacing_before,
                              &TextParagraphPatch::spacing_before_px,
                              "text-panel:paragraph-spacing-before",
                              RC_("Undo", "Set space before paragraph"));
    connect_paragraph_spacing(_paragraph_spacing_after,
                              &TextParagraphPatch::spacing_after_px,
                              "text-panel:paragraph-spacing-after",
                              RC_("Undo", "Set space after paragraph"));
    _paragraph_direction.property_selected().signal_changed().connect([this] {
        if (_updating.pending() || !controller()) return;
        // 0/1 are the real LTR/RTL options; the mixed sentinel and cleared
        // selection must never be committed.
        auto const selected = _paragraph_direction.get_selected();
        if (selected > 1) return;
        TextParagraphPatch patch;
        patch.direction = selected == 1 ? TextParagraphDirection::RightToLeft
                                        : TextParagraphDirection::LeftToRight;
        controller()->commitParagraph(patch, "text-panel:paragraph-direction",
                                      RC_("Undo", "Set paragraph direction"));
        syncFromSelection();
    });
    _paragraph_writing_mode.property_selected().signal_changed().connect([this] {
        if (_updating.pending() || !controller()) return;
        // 0/1/2 are the real horizontal/vertical options; reject the sentinel.
        auto const selected = _paragraph_writing_mode.get_selected();
        if (selected > 2) return;
        TextParagraphPatch patch;
        switch (selected) {
            case 1: patch.writing_mode = TextParagraphWritingMode::VerticalRightToLeft; break;
            case 2: patch.writing_mode = TextParagraphWritingMode::VerticalLeftToRight; break;
            default: patch.writing_mode = TextParagraphWritingMode::Horizontal; break;
        }
        controller()->commitParagraph(patch, "text-panel:paragraph-writing-mode",
                                      RC_("Undo", "Set paragraph writing mode"));
        syncFromSelection();
    });
    _paragraph_orientation.property_selected().signal_changed().connect([this] {
        if (_updating.pending() || !controller()) return;
        // Real index 0 is the CSS `mixed` (auto) orientation; 1/2 are upright
        // and sideways. Indices beyond those are the heterogeneity sentinel or
        // a cleared value and are not committed.
        auto const selected = _paragraph_orientation.get_selected();
        if (selected > 2) return;
        TextParagraphPatch patch;
        switch (selected) {
            case 1: patch.orientation = TextParagraphOrientation::Upright; break;
            case 2: patch.orientation = TextParagraphOrientation::Sideways; break;
            default: patch.orientation = TextParagraphOrientation::Mixed; break;
        }
        controller()->commitParagraph(patch, "text-panel:paragraph-orientation",
                                      RC_("Undo", "Set paragraph glyph orientation"));
        syncFromSelection();
    });
    constexpr std::array<TextListMode, 3> list_modes = {
        TextListMode::None, TextListMode::Bulleted, TextListMode::Numbered
    };
    for (unsigned i = 0; i < _list_mode_buttons.size(); ++i) {
        auto const mode = list_modes[i];
        _list_mode_buttons[i].signal_toggled().connect([this, i, mode] {
            if (_updating.pending() || !_list_mode_buttons[i].get_active() || !controller()) return;
            TextParagraphPatch patch;
            patch.list_mode = mode;
            if (mode == TextListMode::Numbered) {
                patch.list_start = static_cast<unsigned>(_list_start.get_value_as_int());
            }
            controller()->commitParagraph(patch, "text-panel:list-mode",
                                          RC_("Undo", "Set paragraph list style"));
            syncFromSelection();
        });
    }
    _list_start.signal_value_changed().connect([this] {
        if (_updating.pending() || !controller() ||
            !_list_mode_buttons[2].get_active()) return;
        TextParagraphPatch patch;
        patch.list_mode = TextListMode::Numbered;
        patch.list_start = static_cast<unsigned>(_list_start.get_value_as_int());
        controller()->commitParagraphContinuous(patch, "text-panel:list-start",
                                                RC_("Undo", "Set list starting number"));
    });
    _hyphenation.signal_toggled().connect([this] {
        if (_updating.pending() || !controller()) return;
        TextParagraphPatch patch;
        patch.hyphenation = _hyphenation.get_active();
        controller()->commitParagraph(patch, "text-panel:hyphenation",
                                      RC_("Undo", "Set paragraph hyphenation"));
        syncFromSelection();
    });
    _drop_cap.signal_toggled().connect([this] {
        if (_updating.pending() || !controller()) return;
        TextParagraphPatch patch;
        patch.drop_cap_lines = _drop_cap.get_active()
            ? static_cast<unsigned>(_drop_cap_lines.get_value_as_int()) : 0;
        controller()->commitParagraph(patch, "text-panel:drop-cap",
                                      RC_("Undo", "Set paragraph drop cap"));
        syncFromSelection();
    });
    _drop_cap_lines.signal_value_changed().connect([this] {
        if (_updating.pending() || !controller() || !_drop_cap.get_active()) return;
        TextParagraphPatch patch;
        patch.drop_cap_lines = static_cast<unsigned>(_drop_cap_lines.get_value_as_int());
        controller()->commitParagraphContinuous(patch, "text-panel:drop-cap-lines",
                                                RC_("Undo", "Set drop cap lines"));
    });
    _frame_width.signal_value_changed().connect([this] {
        if (_updating.pending() || !controller()) return;
        TextFramePatch patch; patch.width_px = _frame_width.get_value();
        controller()->commitFrame(patch, "text-panel:frame-geometry",
                                  RC_("Undo", "Set text frame geometry"), true);
    });
    _frame_height.signal_value_changed().connect([this] {
        if (_updating.pending() || !controller()) return;
        TextFramePatch patch; patch.height_px = _frame_height.get_value();
        controller()->commitFrame(patch, "text-panel:frame-geometry",
                                  RC_("Undo", "Set text frame geometry"), true);
    });
    _frame_columns.signal_value_changed().connect([this] {
        if (_updating.pending() || !controller()) return;
        TextFramePatch patch;
        patch.columns = static_cast<unsigned>(_frame_columns.get_value_as_int());
        controller()->commitFrame(patch, "text-panel:frame-geometry",
                                  RC_("Undo", "Set text frame geometry"), true);
    });
    _frame_gap.signal_value_changed().connect([this] {
        if (_updating.pending() || !controller()) return;
        TextFramePatch patch; patch.gap_px = _frame_gap.get_value();
        controller()->commitFrame(patch, "text-panel:frame-geometry",
                                  RC_("Undo", "Set text frame geometry"), true);
    });
    _frame_vertical_alignment.property_selected().signal_changed().connect([this] {
        if (_updating.pending() || !controller()) return;
        auto const selected = _frame_vertical_alignment.get_selected();
        if (selected > 2) return;
        TextFramePatch patch;
        patch.vertical_alignment = static_cast<TextFrameVerticalAlignment>(selected);
        controller()->commitFrame(patch, "text-panel:frame-align",
                                  RC_("Undo", "Align text in frame"));
        syncFromSelection();
    });

    _fill_registration.emplace(Inkscape::UI::Widget::PaintPopoverManager::get().register_button(
        _fill, true, [this] { setupPaint(true); }, [this] { return connectPaint(true); }));
    _stroke_registration.emplace(Inkscape::UI::Widget::PaintPopoverManager::get().register_button(
        _stroke, false, [this] { setupPaint(false); }, [this] { return connectPaint(false); }));

    _font_lister_update = FontLister::get_instance()->connectUpdate([this] {
        if (_updating.pending()) return;
        // Publishing a preview refreshes the TextTool geometry, and TextTool then
        // emits the desktop-wide text_cursor_moved signal. TextToolbar reacts to
        // that and calls FontLister::selection_update(), which arrives here. That
        // is our own preview echoing back, not a font-list change: cancelling here
        // destroyed the preview the panel had just requested, so a partial
        // selection never showed a candidate font. Leave an active preview alone;
        // a genuine font-list change still resyncs on the next selection change.
        if (auto style = controller(); style && style->previewActive()) return;
        cancelPanelPreview();
        syncFromSelection();
    });
}

TextStyleController *TextPanel::controller() const
{
    return getDesktop() ? &getDesktop()->textStyleController() : nullptr;
}

void TextPanel::rebuildFaces(TextStyleSnapshot const &snapshot)
{
    _hovered_face.reset();
    auto const family_only = controller() && controller()->panelFontOnly();
    _face.set_tooltip_text(family_only
        ? _("Turn off Font only to choose a font style; each fragment's current style is preserved.")
        : _("Font style or face"));
    if (!snapshot.family.valid || snapshot.family.mixed) {
        _face_family.reset();
        _face_styles.clear();
        _face_specs.clear();
        Inkscape::UI::remove_all_children(_face_list);
        _face_label.set_text(snapshot.family.mixed ? _("Mixed styles") : _("Font style"));
        _face.set_sensitive(false);
        return;
    }

    _face.set_sensitive(!family_only);
    auto lister = FontLister::get_instance();
    auto const preferred = snapshot.face.valid && !snapshot.face.mixed
                         ? snapshot.face.value : Glib::ustring{"Normal"};
    auto const styles = lister->get_font_styles(snapshot.family.value);
    std::vector<std::pair<Glib::ustring, Glib::ustring>> face_styles;
    for (auto const &style : *styles) face_styles.emplace_back(style.css_name, style.display_name);
    // Document rows may be replaced on a cursor move. Compare their contents,
    // so an identical family/style list keeps its widgets and event controllers.
    bool const rebuild = _face_family != snapshot.family.value || _face_styles != face_styles;
    if (rebuild) {
        _face_family = snapshot.family.value;
        _face_styles = std::move(face_styles);
        _face_specs.clear();
        Inkscape::UI::remove_all_children(_face_list);
    }
    _face_list.unselect_all();
    int selected = -1;
    unsigned index = 0;
    for (auto const &style : *styles) {
        Glib::ustring const css = style.css_name;
        Glib::ustring const display = style.display_name;
        if (rebuild) {
            _face_specs.push_back(css);
            auto const text = display.empty() ? css : display;
            auto list_row = Gtk::make_managed<Gtk::ListBoxRow>();
            auto label = Gtk::make_managed<Gtk::Label>(text);
            label->set_halign(Gtk::Align::START);
            label->set_margin_start(8);
            label->set_margin_end(8);
            label->set_margin_top(4);
            label->set_margin_bottom(4);
            list_row->set_child(*label);

            auto motion = Gtk::EventControllerMotion::create();
            motion->signal_motion().connect([this, css](double, double) {
                if (_hovered_face == css) return;
                _hovered_face = css;
                requestFacePreview(css, TextStyleOrigin::Pointer);
            });
            motion->signal_leave().connect([this] {
                _hovered_face.reset();
                if (auto style = controller()) style->cancelPreview();
            });
            list_row->add_controller(motion);

            auto focus = Gtk::EventControllerFocus::create();
            focus->signal_enter().connect([this, css] {
                requestFacePreview(css, TextStyleOrigin::Keyboard);
            });
            list_row->add_controller(focus);

            _face_list.append(*list_row);
        }
        if (css == preferred) selected = index;
        ++index;
    }
    if (snapshot.face.mixed) {
        _face_label.set_text(_("Mixed styles"));
        return;
    }
    if (selected < 0 && snapshot.face.valid) {
        // Requested styles can be absent from a family's installed faces.
        // Keep that state visible instead of claiming the first row is active.
        _face_label.set_text(snapshot.face.value);
        _face.set_tooltip_text(family_only
            ? _("Font only preserves the requested style; this family may use a fallback face.")
            : _("The requested style is unavailable; a fallback face may be used."));
        return;
    }
    if (selected < 0 && !_face_specs.empty()) selected = 0;
    if (selected >= 0) {
        if (auto row = _face_list.get_row_at_index(selected)) {
            _face_list.select_row(*row);
            if (auto label = dynamic_cast<Gtk::Label *>(row->get_child())) {
                _face_label.set_text(label->get_text());
            }
        }
    } else {
        _face_label.set_text(_("Font style"));
        _face.set_sensitive(false);
    }
}

void TextPanel::requestFacePreview(Glib::ustring const &face, TextStyleOrigin origin)
{
    auto style = controller();
    auto const snapshot = style ? style->query() : TextStyleSnapshot{};
    if (!style || style->panelFontOnly() || face.empty() || !snapshot.family.valid || snapshot.family.mixed) return;
    TextStylePatch patch;
    patch.family = snapshot.family.value;
    patch.face = face;
    patch.fontspec = Inkscape::get_fontspec(*patch.family, *patch.face);
    setPreviewDesktop(getDesktop());
    style->preview(patch, origin);
    if (origin == TextStyleOrigin::Keyboard) {
        auto const message = Glib::ustring::compose(_("Previewing %1"), face);
        gtk_accessible_announce(GTK_ACCESSIBLE(_face_list.gobj()), message.c_str(),
                                GTK_ACCESSIBLE_ANNOUNCEMENT_PRIORITY_LOW);
    }
}

void TextPanel::commitFace(Glib::ustring const &face)
{
    auto style = controller();
    auto const snapshot = style ? style->query() : TextStyleSnapshot{};
    if (!style || style->panelFontOnly() || face.empty() || !snapshot.family.valid || snapshot.family.mixed) return;
    TextStylePatch patch;
    patch.family = snapshot.family.value;
    patch.face = face;
    patch.fontspec = Inkscape::get_fontspec(*patch.family, *patch.face);
    style->commit(patch, "text-panel:face", RC_("Undo", "Set font style"));
    setPreviewDesktop(nullptr);
    _face_popover.popdown();
    syncFromSelection();
}

void TextPanel::updateCapitalizationMenu(TextStyleSnapshot const &snapshot)
{
    auto supports_titl = controller() && controller()->supportsOpenTypeFeature("titl");
    auto supports_c2sc = controller() && controller()->supportsOpenTypeFeature("c2sc");
    _case_choice_buttons[capitalization_index(CapitalizationMode::TitlingCaps)]->set_sensitive(supports_titl);
    _case_choice_buttons[capitalization_index(CapitalizationMode::SmallCapsFromCaps)]->set_sensitive(supports_c2sc);

    if (snapshot.capitalization.mixed) {
        _case_button.set_label(_("Mixed"));
    } else if (snapshot.capitalization_custom) {
        _case_button.set_label(_("Custom"));
    } else if (snapshot.capitalization.valid) {
        _case_button.set_label(_(capitalization_labels[capitalization_index(snapshot.capitalization.value)]));
    } else {
        _case_button.set_label(_("None"));
    }
}

void TextPanel::syncFromSelection()
{
    auto scoped = _updating.block();
    _font_only.set_active(controller() && controller()->panelFontOnly());
    auto snapshot = controller() ? controller()->query() : TextStyleSnapshot{};
    auto paragraph = controller() ? controller()->queryParagraph() : TextParagraphSnapshot{};
    auto frame = controller() ? controller()->queryFrame() : TextFrameSnapshot{};
    _character.set_sensitive(snapshot.has_text_target);
    _spacing.set_sensitive(snapshot.has_text_target);
    _paragraph.set_sensitive(paragraph.has_text_target);
    _paragraph_tools.set_sensitive(paragraph.has_text_target);
    _text_frame.set_sensitive(frame.has_text_target);
    _status.set_text(snapshot.has_text_target
        ? _("Edits apply to the selected characters or text objects.")
        : _("Select text to edit. New-text defaults are not changed here."));

    _font_button_label.set_text(snapshot.family.valid
        ? (snapshot.family.mixed ? _("Mixed fonts") : snapshot.family.value)
        : _("Font family"));
    if (snapshot.family.valid && !snapshot.family.mixed) {
        auto face = snapshot.face.valid && !snapshot.face.mixed ? snapshot.face.value : Glib::ustring{};
        _font_list->set_current_font(snapshot.family.value, face);
    }
    rebuildFaces(snapshot);

    if (snapshot.font_size_px.valid && !snapshot.font_size_px.mixed) {
        _font_size.setMixed(false);
        _font_size.setSize(sp_style_css_size_px_to_units(snapshot.font_size_px.value,
                                                        _font_size.getUnit()));
    } else {
        _font_size.setMixed(snapshot.font_size_px.mixed);
    }

    set_mixed(_bold, snapshot.bold);
    set_mixed(_italic, snapshot.italic);
    set_mixed(_underline, snapshot.underline);
    auto const script = snapshot.script_position;
    _superscript.set_active(script.valid && !script.mixed &&
                            script.value == TextScriptPosition::Superscript);
    _subscript.set_active(script.valid && !script.mixed &&
                          script.value == TextScriptPosition::Subscript);
    if (script.mixed) {
        _superscript.add_css_class("mixed");
        _subscript.add_css_class("mixed");
    } else {
        _superscript.remove_css_class("mixed");
        _subscript.remove_css_class("mixed");
    }
    set_mixed(_standard_ligatures, snapshot.standard_ligatures);
    updateCapitalizationMenu(snapshot);

    auto sync_spacing = [](Gtk::SpinButton &spin, TextStyleValue<double> const &value,
                           double lower, double upper) {
        if (value.valid && !value.mixed) {
            spin.remove_css_class("mixed");
            spin.set_value(std::clamp(value.value, lower, upper));
        } else if (value.mixed) {
            spin.add_css_class("mixed");
            spin.set_text({});
        } else {
            spin.remove_css_class("mixed");
        }
    };
    sync_spacing(_character_spacing, snapshot.character_spacing_percent, -100.0, 2000.0);
    sync_spacing(_word_spacing, snapshot.word_spacing_percent, 0.0, 2000.0);
    sync_spacing(_language_spacing, snapshot.language_spacing_percent, 0.0, 2000.0);

    for (auto &button : _paragraph_alignment_buttons) {
        button.remove_css_class("mixed");
    }
    if (paragraph.alignment.valid && !paragraph.alignment.mixed) {
        unsigned index = 0;
        switch (paragraph.alignment.value) {
            case TextParagraphAlignment::Center: index = 1; break;
            case TextParagraphAlignment::End:
            case TextParagraphAlignment::Right: index = 2; break;
            case TextParagraphAlignment::Justify: index = 3; break;
            case TextParagraphAlignment::Start:
            case TextParagraphAlignment::Left: index = 0; break;
        }
        _paragraph_alignment_buttons[index].set_active(true);
    } else if (paragraph.alignment.mixed) {
        for (auto &button : _paragraph_alignment_buttons) button.add_css_class("mixed");
    }
    if (paragraph.line_height.valid && !paragraph.line_height.mixed) {
        _line_height.remove_css_class("mixed");
        _line_height.set_value(paragraph.line_height.value.value);
        _line_height_unit.set_selected(static_cast<unsigned>(paragraph.line_height.value.unit));
    } else if (paragraph.line_height.mixed) {
        _line_height.add_css_class("mixed");
        _line_height.set_text({});
    } else {
        _line_height.remove_css_class("mixed");
    }
    sync_spacing(_first_line_indent, paragraph.first_line_indent_px, -10000.0, 10000.0);
    sync_spacing(_paragraph_spacing_before, paragraph.spacing_before_px, 0.0, 10000.0);
    sync_spacing(_paragraph_spacing_after, paragraph.spacing_after_px, 0.0, 10000.0);
    for (auto &button : _list_mode_buttons) button.remove_css_class("mixed");
    if (paragraph.list_mode.valid && !paragraph.list_mode.mixed) {
        _list_mode_buttons[static_cast<unsigned>(paragraph.list_mode.value)].set_active(true);
    } else if (paragraph.list_mode.mixed) {
        for (auto &button : _list_mode_buttons) button.add_css_class("mixed");
    }
    _list_start.set_sensitive(paragraph.list_mode.valid && !paragraph.list_mode.mixed &&
                              paragraph.list_mode.value == TextListMode::Numbered);
    if (paragraph.list_start.valid && !paragraph.list_start.mixed) {
        _list_start.remove_css_class("mixed");
        _list_start.set_value(paragraph.list_start.value);
    }
    _hyphenation.remove_css_class("mixed");
    if (paragraph.hyphenation.valid && !paragraph.hyphenation.mixed) {
        _hyphenation.set_active(paragraph.hyphenation.value);
    } else if (paragraph.hyphenation.mixed) {
        _hyphenation.set_inconsistent(true);
        _hyphenation.add_css_class("mixed");
    }
    if (!paragraph.hyphenation.mixed) _hyphenation.set_inconsistent(false);
    _hyphenation.set_sensitive(paragraph.has_text_target && paragraph.hyphenation_available);
    _hyphenation.set_tooltip_text(paragraph.hyphenation_available
        ? _("Automatically hyphenate using the paragraph language dictionary")
        : _("No hyphenation dictionary is available for the selected paragraph language"));
    _drop_cap.set_inconsistent(paragraph.drop_cap_lines.mixed);
    if (paragraph.drop_cap_lines.valid && !paragraph.drop_cap_lines.mixed) {
        _drop_cap.set_active(paragraph.drop_cap_lines.value >= 2);
        if (paragraph.drop_cap_lines.value >= 2) {
            _drop_cap_lines.set_value(paragraph.drop_cap_lines.value);
        }
    }
    auto const horizontal_drop_cap = paragraph.writing_mode.valid &&
        !paragraph.writing_mode.mixed &&
        paragraph.writing_mode.value == TextParagraphWritingMode::Horizontal;
    _drop_cap.set_sensitive(paragraph.has_text_target && horizontal_drop_cap);
    _drop_cap_lines.set_sensitive(paragraph.has_text_target && horizontal_drop_cap &&
                                  _drop_cap.get_active());
    _drop_cap.set_tooltip_text(horizontal_drop_cap
        ? _("Enlarge the first character across multiple lines")
        : _("Drop caps are currently available for horizontal text"));
    sync_spacing(_frame_width, frame.width_px, 1.0, 100000.0);
    sync_spacing(_frame_height, frame.height_px, 1.0, 100000.0);
    if (frame.columns.valid && !frame.columns.mixed) {
        _frame_columns.set_value(frame.columns.value);
    } else if (frame.columns.mixed) {
        _frame_columns.set_text({});
    }
    sync_spacing(_frame_gap, frame.gap_px, 0.0, 10000.0);
    std::optional<unsigned> vertical_alignment_index;
    if (frame.vertical_alignment.valid && !frame.vertical_alignment.mixed) {
        vertical_alignment_index = static_cast<unsigned>(frame.vertical_alignment.value);
    }
    sync_enum_dropdown(_frame_vertical_alignment, {_("Top"), _("Middle"), _("Bottom")},
                       vertical_alignment_index, frame.vertical_alignment.mixed,
                       frame.has_text_target);
    auto const one_column = frame.columns.valid && !frame.columns.mixed && frame.columns.value == 1;
    _frame_vertical_alignment.set_sensitive(frame.has_text_target && one_column);
    std::optional<unsigned> direction_index;
    if (paragraph.direction.valid && !paragraph.direction.mixed) {
        direction_index = paragraph.direction.value == TextParagraphDirection::RightToLeft ? 1u : 0u;
    }
    sync_enum_dropdown(_paragraph_direction, {_("LTR"), _("RTL")}, direction_index,
                       paragraph.direction.mixed, paragraph.has_text_target);
    std::optional<unsigned> writing_mode_index;
    if (paragraph.writing_mode.valid && !paragraph.writing_mode.mixed) {
        unsigned mode = 0;
        if (paragraph.writing_mode.value == TextParagraphWritingMode::VerticalRightToLeft) mode = 1;
        if (paragraph.writing_mode.value == TextParagraphWritingMode::VerticalLeftToRight) mode = 2;
        writing_mode_index = mode;
    }
    sync_enum_dropdown(_paragraph_writing_mode,
                       {_("Horizontal"), _("Vertical RL"), _("Vertical LR")},
                       writing_mode_index, paragraph.writing_mode.mixed,
                       paragraph.has_text_target);
    std::optional<unsigned> orientation_index;
    if (paragraph.orientation.valid && !paragraph.orientation.mixed) {
        unsigned orientation = 0;
        if (paragraph.orientation.value == TextParagraphOrientation::Upright) orientation = 1;
        if (paragraph.orientation.value == TextParagraphOrientation::Sideways) orientation = 2;
        orientation_index = orientation;
    }
    sync_enum_dropdown(_paragraph_orientation,
                       {_("Auto glyphs"), _("Upright glyphs"), _("Sideways glyphs")},
                       orientation_index, paragraph.orientation.mixed,
                       paragraph.has_text_target);

    _fill.set_label(paint_summary(snapshot.fill, N_("Fill")));
    _stroke.set_label(paint_summary(snapshot.stroke, N_("Stroke")));
}

void TextPanel::requestCapitalizationPreview(CapitalizationMode mode, TextStyleOrigin origin)
{
    if (!controller()) return;
    auto button = _case_choice_buttons[capitalization_index(mode)];
    if (!button->get_sensitive()) return;
    TextStylePatch patch; patch.capitalization = mode;
    setPreviewDesktop(getDesktop());
    controller()->preview(patch, origin);
    if (origin == TextStyleOrigin::Keyboard) {
        auto const message = Glib::ustring::compose(
            _("Previewing %1"), _(capitalization_labels[capitalization_index(mode)]));
        gtk_accessible_announce(GTK_ACCESSIBLE(_case_choices.gobj()), message.c_str(),
                                GTK_ACCESSIBLE_ANNOUNCEMENT_PRIORITY_LOW);
    }
}

void TextPanel::commitCapitalization(CapitalizationMode mode)
{
    if (!controller()) return;
    TextStylePatch patch; patch.capitalization = mode;
    controller()->commit(patch, "text-panel:capitalization", RC_("Undo", "Set text capitalization"));
    setPreviewDesktop(nullptr);
    _case_popover.popdown();
    syncFromSelection();
}

void TextPanel::setupPaint(bool fill)
{
    auto paint_switch = Inkscape::UI::Widget::PaintPopoverManager::get().get_switch(fill);
    paint_switch->set_desktop(getDesktop());
    paint_switch->set_document(getDocument());
    auto style_controller = controller();
    auto const snapshot = style_controller ? style_controller->query() : TextStyleSnapshot{};
    auto const &paint_value = fill ? snapshot.fill : snapshot.stroke;
    auto style = style_controller ? style_controller->representativeRunStyle() : nullptr;
    if (!snapshot.has_text_target || !style) {
        paint_switch->show_placeholder(_("Select text to edit"), true);
        return;
    }
    if (!paint_value.valid || paint_value.mixed) {
        paint_switch->show_placeholder(_("Mixed paint"), true);
        return;
    }
    auto paint = style->getFillOrStroke(fill);
    if (!paint) {
        paint_switch->show_placeholder(_("Mixed paint"), true);
        return;
    }
    paint_switch->set_mode(Inkscape::UI::Widget::get_mode_from_paint(*paint));
    paint_switch->update_from_paint(*paint);
    if (paint->isColor()) {
        auto color = paint->getColor();
        color.setOpacity(fill ? style->fill_opacity.as_double()
                              : style->stroke_opacity.as_double());
        paint_switch->set_color(color);
    }
    if (fill) {
        paint_switch->set_fill_rule(style->fill_rule.computed == SP_WIND_RULE_EVENODD
            ? Inkscape::UI::Widget::FillRule::EvenOdd : Inkscape::UI::Widget::FillRule::NonZero);
    }
}

void TextPanel::commitPaint(bool fill, Glib::ustring const &paint, bool continuous)
{
    if (!controller() || paint.empty()) return;
    TextStylePatch patch;
    if (fill) patch.fill = paint; else patch.stroke = paint;
    if (continuous) {
        controller()->commitContinuous(patch, fill ? "text-panel:fill" : "text-panel:stroke",
                                       fill ? RC_("Undo", "Set text fill")
                                            : RC_("Undo", "Set text stroke"));
    } else {
        controller()->commit(patch, fill ? "text-panel:fill" : "text-panel:stroke",
                             fill ? RC_("Undo", "Set text fill")
                                  : RC_("Undo", "Set text stroke"));
    }
    syncFromSelection();
}

std::vector<sigc::connection> TextPanel::connectPaint(bool fill)
{
    std::vector<sigc::connection> connections;
    auto paint_switch = Inkscape::UI::Widget::PaintPopoverManager::get().get_switch(fill);

    auto apply_gradient = [this, fill](SPGradient *gradient, SPGradientType type) {
        auto style_controller = controller();
        auto document = getDocument();
        if (!style_controller || !document) return;
        auto server = sp_gradient_paint_server_for_object(
            document, getDesktop(), style_controller->representativeTextItem(),
            fill ? Inkscape::FOR_FILL : Inkscape::FOR_STROKE, gradient, type);
        commitPaint(fill, paint_url(server), false);
    };

    connections.push_back(paint_switch->get_flat_color_changed().connect([this, fill](auto const &color) {
        commitPaint(fill, color.toString(true));
    }));
    connections.push_back(paint_switch->get_signal_mode_changed().connect([this, fill, apply_gradient](auto mode) {
        if (mode == Inkscape::UI::Widget::PaintMode::None) commitPaint(fill, "none", false);
        // PaintSwitch's initial gradient notification is suppressed while the
        // compact shared popover is synchronizing. Create the normal linear
        // gradient at the explicit mode transition; later editor changes carry
        // their selected vector and type through get_gradient_changed().
        if (mode == Inkscape::UI::Widget::PaintMode::Gradient) {
            apply_gradient(nullptr, SP_GRADIENT_TYPE_LINEAR);
        }
    }));
    connections.push_back(paint_switch->get_gradient_changed().connect(
        [apply_gradient](SPGradient *gradient, SPGradientType type) {
            if (gradient) apply_gradient(gradient, type);
        }));
    connections.push_back(paint_switch->get_pattern_changed().connect(
        [this, fill](SPPattern *pattern, auto &&...) { commitPaint(fill, paint_url(pattern)); }));
    connections.push_back(paint_switch->get_hatch_changed().connect(
        [this, fill](SPHatch *hatch, auto &&...) { commitPaint(fill, paint_url(hatch)); }));
    connections.push_back(paint_switch->get_mesh_changed().connect(
        [this, fill](SPGradient *mesh) { commitPaint(fill, paint_url(mesh)); }));
    connections.push_back(paint_switch->get_swatch_changed().connect(
        [this, fill](SPGradient *swatch, auto, auto, auto, auto) {
            commitPaint(fill, paint_url(swatch));
        }));
    connections.push_back(paint_switch->get_inherit_mode_changed().connect([this, fill](auto mode) {
        commitPaint(fill, Inkscape::UI::Widget::get_inherited_paint_css_mode(mode), false);
    }));
    return connections;
}

void TextPanel::desktopReplaced()
{
    cancelPanelPreview();
    connectFontChoices();
    syncFromSelection();
}

void TextPanel::documentReplaced()
{
    setPreviewDesktop(nullptr); // The desktop controller owns document-change cancellation.
    syncFromSelection();
}

void TextPanel::selectionChanged(Selection *)
{
    _font_popover.popdown();
    _face_popover.popdown();
    if (_preview_desktop && _preview_desktop != getDesktop()) {
        cancelPanelPreview();
    } else {
        // The controller's selection callback cancels same-desktop previews and
        // deliberately ignores notifications generated by its own commit.
        setPreviewDesktop(nullptr);
    }
    syncFromSelection();
}

void TextPanel::selectionModified(Selection *, guint)
{
    setPreviewDesktop(nullptr); // See selectionChanged(); do not interrupt an active commit.
    syncFromSelection();
}

void TextPanel::update()
{
    syncFromSelection();
}

void TextPanel::on_unmap()
{
    cancelPanelPreview();
    if (auto style = controller()) style->invalidateFontChoices();
    DialogBase::on_unmap();
}

void TextPanel::setPreviewDesktop(SPDesktop *desktop)
{
    if (_preview_desktop == desktop) return;
    _preview_desktop_destroy.disconnect();
    _preview_desktop = desktop;
    if (desktop) {
        _preview_desktop_destroy = desktop->connectDestroy([this](SPDesktop *) { _preview_desktop = nullptr; });
    }
}

void TextPanel::cancelPanelPreview() noexcept
{
    if (_preview_desktop) {
        _preview_desktop->textStyleController().cancelPreview();
    } else if (auto style = controller()) {
        style->cancelPreview();
    }
    setPreviewDesktop(nullptr);
}

} // namespace Inkscape::UI::Dialog
