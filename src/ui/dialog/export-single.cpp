// SPDX-License-Identifier: GPL-2.0-or-later
/* Authors:
 *   Lauris Kaplinski <lauris@kaplinski.com>
 *   bulia byak <buliabyak@users.sf.net>
 *   Johan Engelen <j.b.c.engelen@ewi.utwente.nl>
 *   Anshudhar Kumar Singh <anshudhar2001@gmail.com>
 *
 * Copyright (C) 1999-2007, 2021 Authors
 * Copyright (C) 2001-2002 Ximian, Inc.
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "export-single.h"

#include <glibmm/convert.h>
#include <glibmm/i18n.h>
#include <glibmm/main.h>
#include <glibmm/miscutils.h>
#include <gtkmm/filefilter.h>
#include <gtkmm/flowbox.h>
#include <gtkmm/progressbar.h>
#include <gtkmm/recentmanager.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/spinbutton.h>
#include <png.h>
#include <array>

#include "desktop.h"
#include "document-undo.h"
#include "extension/output.h"
#include "inkscape.h"
#include "inkscape-window.h"
#include "io/sandbox.h"
#include "io/fix-broken-links.h"
#include "object/sp-page.h"
#include "object/sp-root.h"
#include "object/weakptr.h"
#include "selection.h"
#include "svg/stringstream.h"
#include "ui/builder-utils.h"
#include "ui/dialog/choose-file-utils.h"
#include "ui/dialog/choose-file.h"
#include "ui/dialog/export.h"
#include "ui/icon-names.h"
#include "ui/util.h"
#include "ui/widget/color-picker.h"
#include "ui/widget/export-lists.h"
#include "ui/widget/spinbutton.h"
#include "ui/widget/unit-menu.h"

using Inkscape::Util::UnitTable;
using Inkscape::UI::Widget::UnitMenu;

namespace Inkscape::UI::Dialog {

SingleExport::SingleExport(BaseObjectType *cobject, const Glib::RefPtr<Gtk::Builder> &builder)
    : Gtk::Box(cobject)
    , pages_list        (get_widget<Gtk::FlowBox>         (builder, "si_pages"))
    , pages_list_box    (get_widget<Gtk::ScrolledWindow>  (builder, "si_pages_box"))
    , size_box          (get_widget<Gtk::Grid>            (builder, "si_sizes"))
    , units             (get_derived_widget<UnitMenu>     (builder, "si_units"))
    , si_units_row      (get_widget<Gtk::Box>             (builder, "si_units_row"))
    , si_hide_all       (get_widget<Gtk::CheckButton>     (builder, "si_hide_all"))
    , si_show_preview   (get_widget<Gtk::CheckButton>     (builder, "si_show_preview"))
    , preview           (get_derived_widget<ExportPreview>(builder, "si_preview"))
    , preview_box       (get_widget<Gtk::Box>             (builder, "si_preview_box"))

    , si_extension_cb   (get_derived_widget<ExtensionList>(builder, "si_extention"))
    , si_filename_entry (get_widget<Gtk::Entry>           (builder, "si_filename"))
    , si_filename_button(get_widget<Gtk::Button>          (builder, "si_filename_button"))
    , si_export         (get_widget<Gtk::Button>          (builder, "si_export"))

    , progress_bar      (get_widget<Gtk::ProgressBar>     (builder, "si_progress"))
    , cancel_button     (get_widget<Gtk::Button>          (builder, "si_cancel"))
    , progress_box      (get_widget<Gtk::Box>             (builder, "si_inprogress"))
    , _background_color (get_derived_widget<UI::Widget::ColorPicker>(builder, "si_backgnd", _("Background color"), true))
{
    prefs = Inkscape::Preferences::get();

    selection_names[SELECTION_DRAWING] = "drawing";
    selection_names[SELECTION_PAGE] = "page";
    selection_names[SELECTION_SELECTION] = "selection";
    selection_names[SELECTION_CUSTOM] = "custom";

    selection_buttons[SELECTION_DRAWING]   = &get_widget<Gtk::ToggleButton>(builder, "si_s_document");
    selection_buttons[SELECTION_PAGE]      = &get_widget<Gtk::ToggleButton>(builder, "si_s_page");
    selection_buttons[SELECTION_SELECTION] = &get_widget<Gtk::ToggleButton>(builder, "si_s_selection");
    selection_buttons[SELECTION_CUSTOM]    = &get_widget<Gtk::ToggleButton>(builder, "si_s_custom");

    spin_buttons[SPIN_X0]       = &get_derived_widget<UI::Widget::SpinButton>(builder, "si_left_sb");
    spin_buttons[SPIN_X1]       = &get_derived_widget<UI::Widget::SpinButton>(builder, "si_right_sb");
    spin_buttons[SPIN_Y0]       = &get_derived_widget<UI::Widget::SpinButton>(builder, "si_top_sb");
    spin_buttons[SPIN_Y1]       = &get_derived_widget<UI::Widget::SpinButton>(builder, "si_bottom_sb");
    spin_buttons[SPIN_HEIGHT]   = &get_derived_widget<UI::Widget::SpinButton>(builder, "si_height_sb");
    spin_buttons[SPIN_WIDTH]    = &get_derived_widget<UI::Widget::SpinButton>(builder, "si_width_sb");
    spin_buttons[SPIN_BMHEIGHT] = &get_derived_widget<UI::Widget::SpinButton>(builder, "si_img_height_sb");
    spin_buttons[SPIN_BMWIDTH]  = &get_derived_widget<UI::Widget::SpinButton>(builder, "si_img_width_sb");
    spin_buttons[SPIN_DPI]      = &get_derived_widget<UI::Widget::SpinButton>(builder, "si_dpi_sb");

    spin_labels[SPIN_X0]        = &get_widget<Gtk::Label>(builder, "si_label_left");
    spin_labels[SPIN_X1]        = &get_widget<Gtk::Label>(builder, "si_label_right");
    spin_labels[SPIN_Y0]        = &get_widget<Gtk::Label>(builder, "si_label_top");
    spin_labels[SPIN_Y1]        = &get_widget<Gtk::Label>(builder, "si_label_bottom");
    spin_labels[SPIN_HEIGHT]    = &get_widget<Gtk::Label>(builder, "si_label_height");
    spin_labels[SPIN_WIDTH]     = &get_widget<Gtk::Label>(builder, "si_label_width");

    auto &pref_button_box = get_widget<Gtk::Box>(builder, "si_prefs");
    auto &pref_button = *si_extension_cb.getPrefButton();
    pref_button_box.append(pref_button);
    attach_tiff_profile_picker(si_extension_cb);
    pref_button.set_expand(false);
    pref_button_box.set_expand(false);
    pref_button.set_valign(Gtk::Align::BASELINE_CENTER);
    pref_button_box.set_valign(Gtk::Align::BASELINE_CENTER);

    setup();
}

// Inkscape Selection Modified CallBack
void SingleExport::selectionModified(Inkscape::Selection *selection, guint flags)
{
    if (!_desktop || _desktop->getSelection() != selection) {
        return;
    }
    if (!(flags & (SP_OBJECT_MODIFIED_FLAG | SP_OBJECT_PARENT_MODIFIED_FLAG | SP_OBJECT_CHILD_MODIFIED_FLAG))) {
        return;
    }
    refreshArea();
    // Do not load export hits for modifications
}

void SingleExport::selectionChanged(Inkscape::Selection *selection)
{
    if (!_desktop || _desktop->getSelection() != selection) {
        return;
    }

    Glib::ustring pref_key_name = prefs->getString("/dialogs/export/exportarea/value");
    for (auto [key, name] : selection_names) {
        if (name == pref_key_name && current_key != key && key != SELECTION_SELECTION) {
            selection_buttons[key]->set_active(true);
            current_key = key;
            break;
        }
    }
    if (selection->isEmpty()) {
        selection_buttons[SELECTION_SELECTION]->set_sensitive(false);
        if (current_key == SELECTION_SELECTION) {
            selection_buttons[(selection_mode)0]->set_active(true); // This causes refresh area
            // even though we are at default key, selection is the one which was original key.
            prefs->setString("/dialogs/export/exportarea/value", selection_names[SELECTION_SELECTION]);
            // return otherwise refreshArea will be called again
            return;
        }
    } else {
        selection_buttons[SELECTION_SELECTION]->set_sensitive(true);
        if (selection_names[SELECTION_SELECTION] == pref_key_name && current_key != SELECTION_SELECTION) {
            selection_buttons[SELECTION_SELECTION]->set_active();
            return;
        }
    }

    refreshArea();
    loadExportHints();
}

// Setup Single Export.Called by export on realize
void SingleExport::setup()
{
    if (setupDone) {
        // We need to setup only once
        return;
    }
    setupDone = true;

    si_extension_cb.setup();

    setupUnits();
    setupSpinButtons();

    // set them before connecting to signals
    setDefaultSelectionMode();
    setPagesMode(false);
    setExporting(false);

    // Make the filename entry box read-only when the filesystem is sandboxed.
    // "Sandboxed" means that the user can't access arbitrary paths but only those
    // that come from the file chooser.
    if (Inkscape::IO::Sandbox::filesystem_is_sandboxed()) {
        si_filename_entry.set_editable(false);
        si_filename_entry.set_can_focus(false);
        si_filename_entry.set_has_frame(false);
    }

    // Refresh the filename when the user selects a different page
    _pages_list_changed = pages_list.signal_selected_children_changed().connect([this]() {
        loadExportHints();
        refreshArea();
    });

    // Connect Signals Here
    for (auto [key, button] : selection_buttons) {
        button->signal_toggled().connect(sigc::bind(sigc::mem_fun(*this, &SingleExport::onAreaTypeToggle), key));
    }
    units.signal_changed().connect(sigc::mem_fun(*this, &SingleExport::onUnitChanged));
    extensionConn = si_extension_cb.signal_changed().connect(sigc::mem_fun(*this, &SingleExport::onExtensionChanged));
    exportConn = si_export.signal_clicked().connect(sigc::mem_fun(*this, &SingleExport::onExport));
    filenameConn = si_filename_entry.signal_changed().connect(sigc::mem_fun(*this, &SingleExport::onFilenameModified));
    cancelConn = cancel_button.signal_clicked().connect(sigc::mem_fun(*this, &SingleExport::onCancel));
    si_filename_entry.signal_activate().connect(sigc::mem_fun(*this, &SingleExport::onExport));
    browseConn = si_filename_button.signal_clicked().connect(sigc::mem_fun(*this, &SingleExport::onBrowse));
    si_show_preview.signal_toggled().connect(sigc::mem_fun(*this, &SingleExport::refreshPreview));
    si_hide_all.signal_toggled().connect(sigc::mem_fun(*this, &SingleExport::refreshPreview));
    _background_color.connectChanged([=, this](Colors::Color const &color){
        if (_desktop) {
            Inkscape::UI::Dialog::set_export_bg_color(_desktop->getNamedView(), color);
        }
        refreshPreview();
    });
}

// Setup units combobox
void SingleExport::setupUnits()
{
    units.setUnitType(Inkscape::Util::UNIT_TYPE_LINEAR);
    if (_desktop) {
        units.setUnit(_desktop->getNamedView()->display_units->abbr);
    }
}

// Create all spin buttons
void SingleExport::setupSpinButtons()
{
    setupSpinButton<sb_type>(spin_buttons[SPIN_X0], 0.0, -1000000.0, 1000000.0, 0.1, 1.0, EXPORT_COORD_PRECISION, true,
                             &SingleExport::onAreaXChange, SPIN_X0);
    setupSpinButton<sb_type>(spin_buttons[SPIN_X1], 0.0, -1000000.0, 1000000.0, 0.1, 1.0, EXPORT_COORD_PRECISION, true,
                             &SingleExport::onAreaXChange, SPIN_X1);
    setupSpinButton<sb_type>(spin_buttons[SPIN_Y0], 0.0, -1000000.0, 1000000.0, 0.1, 1.0, EXPORT_COORD_PRECISION, true,
                             &SingleExport::onAreaYChange, SPIN_Y0);
    setupSpinButton<sb_type>(spin_buttons[SPIN_Y1], 0.0, -1000000.0, 1000000.0, 0.1, 1.0, EXPORT_COORD_PRECISION, true,
                             &SingleExport::onAreaYChange, SPIN_Y1);

    setupSpinButton<sb_type>(spin_buttons[SPIN_HEIGHT], 0.0, 0.0, PNG_UINT_31_MAX, 0.1, 1.0, EXPORT_COORD_PRECISION,
                             true, &SingleExport::onAreaYChange, SPIN_HEIGHT);
    setupSpinButton<sb_type>(spin_buttons[SPIN_WIDTH], 0.0, 0.0, PNG_UINT_31_MAX, 0.1, 1.0, EXPORT_COORD_PRECISION,
                             true, &SingleExport::onAreaXChange, SPIN_WIDTH);

    setupSpinButton<sb_type>(spin_buttons[SPIN_BMHEIGHT], 1.0, 1.0, 1000000.0, 1.0, 10.0, 0, true,
                             &SingleExport::onDpiChange, SPIN_BMHEIGHT);
    setupSpinButton<sb_type>(spin_buttons[SPIN_BMWIDTH], 1.0, 1.0, 1000000.0, 1.0, 10.0, 0, true,
                             &SingleExport::onDpiChange, SPIN_BMWIDTH);
    setupSpinButton<sb_type>(spin_buttons[SPIN_DPI], prefs->getDouble("/dialogs/export/defaultxdpi/value", DPI_BASE),
                             1.0, 100000.0, 0.1, 1.0, 2, true, &SingleExport::onDpiChange, SPIN_DPI);
}

template <typename T>
void SingleExport::setupSpinButton(UI::Widget::SpinButton *sb, double val, double min, double max, double step, double page,
                                   int digits, bool sensitive, void (SingleExport::*cb)(T), T param)
{
    if (sb) {
        sb->set_digits(digits);
        sb->set_increments(step, page);
        sb->set_range(min, max);
        sb->set_value(val);
        sb->set_sensitive(sensitive);
        if (cb) {
            auto signal = sb->signal_value_changed().connect(sigc::bind(sigc::mem_fun(*this, cb), param));
            // add signals to list to block all easily
            spinButtonConns.push_back(signal);
        }
    }
}

void SingleExport::refreshArea()
{
    if (_document) {
        Geom::OptRect bbox;
        auto sel = getSelectedPages();

        switch (current_key) {
            case SELECTION_SELECTION:
                if ((_desktop->getSelection())->isEmpty() == false) {
                    bbox = _desktop->getSelection()->visualBounds();
                    break;
                }
            case SELECTION_DRAWING:
                bbox = _document->getRoot()->desktopVisualBounds();
                if (bbox) {
                    break;
                }
            case SELECTION_PAGE:
                // If the page is set in the multi-selection use that.
                if (sel.size() == 1) {
                    bbox = sel[0]->getDesktopRect();
                } else {
                    bbox = _document->getPageManager().getSelectedPageRect();
                }
                break;
            case SELECTION_CUSTOM:
                break;
            default:
                break;
        }
        if (current_key != SELECTION_CUSTOM && bbox) {
            setArea(bbox->min()[Geom::X], bbox->min()[Geom::Y], bbox->max()[Geom::X], bbox->max()[Geom::Y]);
        }
    }
    refreshPreview();
}

void SingleExport::refreshPage()
{
    if (!_document)
        return;

    bool multi = pages_list.get_selection_mode() == Gtk::SelectionMode::MULTIPLE;
    auto &pm = _document->getPageManager();
    bool has_pages = current_key == SELECTION_PAGE && pm.getPageCount() > 1;
    pages_list_box.set_visible(has_pages);
    preview_box.set_visible(!has_pages);
    size_box.set_visible(!has_pages || !multi);
}

void SingleExport::setPagesMode(bool multi)
{
    // Set set the internal mode to NONE to preserve selections while changing
    for (auto &widget : children(pages_list)) {
        if (auto item = dynamic_cast<BatchItem *>(&widget)) {
            item->on_mode_changed(Gtk::SelectionMode::NONE);
        }
    }
    pages_list.set_selection_mode(multi ? Gtk::SelectionMode::MULTIPLE : Gtk::SelectionMode::SINGLE);
    // A second call is needed in its own loop because of how updates happen in the FlowBox
    for (auto &widget : children(pages_list)) {
        if (auto item = dynamic_cast<BatchItem *>(&widget)) {
            item->update_selected();
        }
    }
    refreshPage();
}

void SingleExport::selectPage(SPPage *page)
{
    for (auto &widget : children(pages_list)) {
        if (auto item = dynamic_cast<BatchItem *>(&widget)) {
            if (item->getPage() == page) {
                item->set_selected(true);
            }
        }
    }
}

std::vector<SPPage const *> SingleExport::getSelectedPages() const
{
    std::vector<SPPage const *> pages;
    pages_list.selected_foreach([&pages](Gtk::FlowBox *box, Gtk::FlowBoxChild *child) {
        if (auto item = dynamic_cast<BatchItem *>(child))
            pages.push_back(item->getPage());
    });
    return pages;
}

void SingleExport::onPagesChanged(SPPage *new_page)
{
    std::map<std::string, SPObject*> itemsList;

    if (_document) {
        auto &pm = _document->getPageManager();
        if (pm.getPageCount() > 1) {
            for (auto page : pm.getPages()) {
                if (auto id = page->getId()) {
                    itemsList[id] = page;
                }
            }
        }
    }

    _pages_list_changed.block();
    BatchItem::syncItems(current_items, itemsList, pages_list, _preview_drawing, false);
    refreshPage();
    if (auto ext = si_extension_cb.getExtension()) {
        setPagesMode(!ext->is_raster());
    }
    _pages_list_changed.unblock();
}

void SingleExport::onPagesModified(SPPage *page)
{
    refreshArea();
}

void SingleExport::onPagesSelected(SPPage *page) {
    if (pages_list.get_selection_mode() != Gtk::SelectionMode::MULTIPLE) {
        selectPage(page);
    }
    refreshArea();
}

/**
 * Update suggested DPI and filename when the selection has changed.
 */
void SingleExport::loadExportHints()
{
    auto const lifetime = _callback_lifetime;
    loadResolutionHints();
    if (*lifetime) refreshDestination();
}

void SingleExport::loadResolutionHints()
{
    if (!_document || !_desktop)
        return;

    // Keep area/DPI hints. Filename hints remain available to explicit CLI
    // re-export, but are not defaults for a fresh interactive session.
    Geom::Point dpi;
    switch (current_key) {
        case SELECTION_PAGE:
        {
            auto pages = getSelectedPages();
            if (pages.size() == 1) {
                dpi = pages[0]->getExportDpi();

                break;
            }
            // No or many pages means output is drawing, continue.
        }
        case SELECTION_CUSTOM:
        case SELECTION_DRAWING:
        {
            dpi = _document->getRoot()->getExportDpi();
            break;
        }
        case SELECTION_SELECTION:
        {
            auto selection = _desktop->getSelection();
            if (selection->isEmpty()) break;

            // Get filename and dpi from selected items
            for (auto item : selection->items()) {
                if (!dpi.x()) {
                    dpi = item->getExportDpi();
                }
            }
            break;
        }
        default:
            break;
    }
    if (dpi.x() != 0.0) { // XXX Should this deal with dpi.y() ?
        spin_buttons[SPIN_DPI]->set_value(dpi.x());
    }
}

void SingleExport::refreshDestination()
{
    if (!_document || !_desktop) return;
    auto const lifetime = _callback_lifetime;
    auto ext = si_extension_cb.getExtension();
    if (!ext) return;
    auto source = std::string(_document->getDocumentFilename() ? _document->getDocumentFilename() : "");
    auto key = IO::ExportDestination::format_key(ext->get_mimetype(), ext->get_extension());
    _destination.refresh(source, Glib::filename_from_utf8(_("bitmap")), ext->get_extension());
    if (_destination.user_folder) {
        _directory_request.reset();
        _destination_ready = true;
        si_export.set_sensitive(true);
        if (!*lifetime) return;
        setFilename(_destination.path(), true);
        return;
    }
    if (IO::Sandbox::filesystem_is_sandboxed()) {
        _destination_ready = true;
        setFilename("", false); // The chooser must grant access.
        return;
    }
    auto context = source + '\n' + key;
    if (_destination_context == context) {
        if (_destination_ready) setFilename(_destination.folder.empty() ? "" : _destination.path(), false);
        return;
    }
    _destination_context = std::move(context);
    _directory_request.reset();
    _destination_ready = false;
    si_export.set_sensitive(false);
    if (!*lifetime) return;
    auto remembered = Glib::filename_from_utf8(prefs->getString(IO::ExportDestination::preference_key(key)));
    auto candidates = IO::ExportDestination::directories(remembered, source, Glib::get_home_dir());
    _directory_request = std::make_unique<IO::ExportDestination::DirectoryRequest>(std::move(candidates),
        [lifetime](std::string folder) {
            auto panel = *lifetime;
            if (!panel) return;
            panel->_destination.folder = std::move(folder);
            panel->_destination_ready = true;
            panel->setFilename(panel->_destination.folder.empty() ? "" : panel->_destination.path(), false);
            if (*lifetime) (*lifetime)->si_export.set_sensitive(true);
        });
}

/**
 * Set filename and update filename entry box.
 *
 * @param filename
 *     Raw file path.
 *     Value is in platform-native encoding (see Glib::filename_to_utf8).
 * @param is_user_input
 *     True if the new filename comes from user input (by file chooser, editing the text box, or by pushing the Export
 * button). False if the new value is auto-generated.
 */
void SingleExport::setFilename(std::string filename, bool is_user_input)
{
    auto const lifetime = _callback_lifetime;
    // Update filename:
    if (!is_user_input && Inkscape::IO::Sandbox::filesystem_is_sandboxed()) {
        // With a sandboxed filesystem, autogenerated filenames don't work.
        // We can only use files coming from the file chooser dialog.
        // Therefore, if the filename changed and it is not from user input,
        // make the filename invalid to force that the user opens the file dialog again.
        if (filepath_native != filename) {
            filename = "";
        }
    }
    filepath_native = filename;

    // Determine filename label for filename entry box:
    Glib::ustring filename_label = Glib::filename_to_utf8(filename);
    if (Inkscape::IO::Sandbox::filesystem_is_sandboxed()) {
        // In a sandboxed filesystem, the file entry box is not editable.
        // We show a nice label instead of the raw path.
        filename_label = Inkscape::IO::Sandbox::filesystem_get_display_path(Gio::File::create_for_path(filename));
    }

    // Set value of filename entry box:
    if (si_filename_entry.get_text().raw() != filename_label.raw()) {
        auto old_block_state = filenameConn.blocked();
        filenameConn.block();
        si_filename_entry.set_text(filename_label);
        if (!*lifetime) return;
        si_filename_entry.set_position(filename_label.length());
        if (!*lifetime) return;
        filenameConn.block(old_block_state);
    }

    // Unconditionally update the original value to prevent it from becoming stale.
    filename_entry_original_value = filename_label;
}

void SingleExport::setArea(double x0, double y0, double x1, double y1)
{
    blockSpinConns(true);

    Unit const *unit = units.getUnit();
    auto px = UnitTable::get().getUnit("px");
    spin_buttons[SPIN_X0]->get_adjustment()->set_value(px->convert(x0, unit));
    spin_buttons[SPIN_X1]->get_adjustment()->set_value(px->convert(x1, unit));
    spin_buttons[SPIN_Y0]->get_adjustment()->set_value(px->convert(y0, unit));
    spin_buttons[SPIN_Y1]->get_adjustment()->set_value(px->convert(y1, unit));

    areaXChange(SPIN_X1);
    areaYChange(SPIN_Y1);

    blockSpinConns(false);
}

// Signals CallBack

void SingleExport::onUnitChanged()
{
    refreshArea();
}

void SingleExport::onAreaTypeToggle(selection_mode key)
{
    // Prevent executing function twice
    if (!selection_buttons[key]->get_active()) {
        return;
    }
    // If you have reached here means the current key is active one ( not sure if multiple transitions happen but
    // last call will change values)
    current_key = key;
    prefs->setString("/dialogs/export/exportarea/value", selection_names[current_key]);

    refreshArea();
    loadExportHints();
    toggleSpinButtonVisibility();
    refreshPage();
}

void SingleExport::toggleSpinButtonVisibility()
{
    bool show = current_key == SELECTION_CUSTOM;
    spin_buttons[SPIN_X0]->set_visible(show);
    spin_buttons[SPIN_X1]->set_visible(show);
    spin_buttons[SPIN_Y0]->set_visible(show);
    spin_buttons[SPIN_Y1]->set_visible(show);
    spin_buttons[SPIN_WIDTH]->set_visible(show);
    spin_buttons[SPIN_HEIGHT]->set_visible(show);

    spin_labels[SPIN_X0]->set_visible(show);
    spin_labels[SPIN_X1]->set_visible(show);
    spin_labels[SPIN_Y0]->set_visible(show);
    spin_labels[SPIN_Y1]->set_visible(show);
    spin_labels[SPIN_WIDTH]->set_visible(show);
    spin_labels[SPIN_HEIGHT]->set_visible(show);

    si_units_row.set_visible(show);
}

void SingleExport::onAreaXChange(sb_type type)
{
    blockSpinConns(true);
    areaXChange(type);
    selection_buttons[SELECTION_CUSTOM]->set_active(true);
    refreshPreview();
    blockSpinConns(false);
}
void SingleExport::onAreaYChange(sb_type type)
{
    blockSpinConns(true);
    areaYChange(type);
    selection_buttons[SELECTION_CUSTOM]->set_active(true);
    refreshPreview();
    blockSpinConns(false);
}
void SingleExport::onDpiChange(sb_type type)
{
    blockSpinConns(true);
    dpiChange(type);
    blockSpinConns(false);
}

/**
 * Filename in filename entry field was changed.
 */
void SingleExport::onFilenameModified()
{
    auto const lifetime = _callback_lifetime;
    extensionConn.block();

    Glib::ustring filename = si_filename_entry.get_text();
    if (filename_entry_original_value.raw() != filename.raw()) {
        // Textbox entry was changed
        auto native = Glib::filename_from_utf8(filename);
        si_extension_cb.setExtensionFromFilename(native);
        if (!*lifetime) return;
        if (auto ext = si_extension_cb.getExtension()) {
            _destination.edit(native, ext->get_extension());
        }
        _directory_request.reset();
        _destination_context.clear();
        _destination_ready = true;
        setFilename(native, true);
        if (!*lifetime) return;
        si_export.set_sensitive(true);
        if (!*lifetime) return;
    }

    // This will not change the output extension if filename extension is same as previously
    // selected extension's filename extension.  In otherwords, selecting "SVG" in file dialog
    // won't override "Plain SVG" in export dialog.
    si_extension_cb.setExtensionFromFilename(Glib::filename_from_utf8(filename));
    if (!*lifetime) return;
    extensionConn.unblock();
}

void SingleExport::onExtensionChanged()
{
    auto const lifetime = _callback_lifetime;
    if (auto ext = si_extension_cb.getExtension()) {
        // Changing FlowBox selection mode can emit page-selection notifications.
        // A format change is not a request to reload the old resolution hint.
        _pages_list_changed.block();
        setPagesMode(!ext->is_raster());
        if (!*lifetime) return;
        _pages_list_changed.unblock();
        refreshDestination();
    }
}

void SingleExport::onCancel()
{
    interrupted = true;
    // The active call owns its final UI reset. Do not re-enable other mutations
    // or pump a nested loop while that call is still encoding/finalizing.
    if (!_exporting) setExporting(false);
}

struct SingleExport::ExportProgress {
    std::weak_ptr<SingleExport *> view;
    SPDocument *document;
    SPWeakPtr<SPObject> root;
    unsigned generation;
    bool cancelled = false;

    bool sourceAlive() const {
        return root && root->document == document;
    }

    SingleExport *panel() const {
        auto alive = view.lock();
        return alive ? *alive : nullptr;
    }
    bool current() const {
        auto si = panel();
        return si && sourceAlive() &&
            si->_document == document && si->_document_generation == generation &&
            si->_desktop == SP_ACTIVE_DESKTOP && !DocumentUndo::interactionCloseRequested(document);
    }
};

void SingleExport::onExport()
{
    if (_exporting || !_desktop || !_document)
        return;

    if (!_destination_ready) return;

    if (filepath_native.empty()) {
        // No file selected yet - call file chooser first
        onBrowse();
        return; // The chooser exports once on acceptance; cancellation does nothing.
    }

    auto const source = _document;
    if (DocumentUndo::interactionCloseRequested(source)) return;
    // Registered owners retain their document throughout the synchronous call;
    // private owners must do so themselves. This does not lease the desktop
    // retained by the existing shared encoder across its own callbacks.
    auto source_operation = DocumentUndo::holdInteractionOperation(source);
    ExportProgress progress{_callback_lifetime, source, SPWeakPtr<SPObject>(source->getRoot()),
                            _document_generation};
    if (!progress.current()) return;
    auto &page_manager = source->getPageManager();
    auto selection = _desktop->getSelection();
    bool exportSuccessful = false;
    auto omod = si_extension_cb.getExtension();
    if (!omod) {
        std::cerr << "SingleExport::onExport(): Cannot find export extension!" << std::endl;
        return;
    }

    bool selected_only = si_hide_all.get_active();
    Unit const *unit = units.getUnit();
    auto const completed_path = Glib::canonicalize_filename(Export::absolutizePath(_document, filepath_native));
    auto const completed_key = IO::ExportDestination::format_key(omod->get_mimetype(), omod->get_extension());
    auto const completed_dpi = spin_buttons[SPIN_DPI]->get_value();
    auto const completed_mode = current_key;
    auto pages = getSelectedPages();
    SPObject *target = source->getRoot();
    if (completed_mode == SELECTION_SELECTION) {
        target = selection->firstItem();
    } else if (completed_mode == SELECTION_PAGE && pages.size() == 1) {
        target = const_cast<SPPage *>(pages.front());
    }
    SPWeakPtr<SPObject> hint_target(target);

    // Do not synchronously probe or recreate the parent. Automatic folders were
    // checked asynchronously; a removed or manually typed nonexistent folder
    // fails through the existing encoder. Folder creation belongs to the chooser.

    /// Label for displaying to user
    Glib::ustring filename_label =
        Inkscape::IO::Sandbox::filesystem_get_display_path(Gio::File::create_for_path(filepath_native));

    /// File path converted to UTF8
    Glib::ustring filename_utf8 = Glib::filename_to_utf8(completed_path);

    float x0 = unit->convert(spin_buttons[SPIN_X0]->get_value(), "px");
    float x1 = unit->convert(spin_buttons[SPIN_X1]->get_value(), "px");
    float y0 = unit->convert(spin_buttons[SPIN_Y0]->get_value(), "px");
    float y1 = unit->convert(spin_buttons[SPIN_Y1]->get_value(), "px");
    auto area = Geom::Rect(Geom::Point(x0, y0), Geom::Point(x1, y1)) * _desktop->dt2doc();
    unsigned long int width = int(spin_buttons[SPIN_BMWIDTH]->get_value() + 0.5);
    unsigned long int height = int(spin_buttons[SPIN_BMHEIGHT]->get_value() + 0.5);
    auto const background = _background_color.get_current_color();
    std::vector<SPItem const *> selected;
    std::vector<SPWeakPtr<SPObject>> submitted_objects;
    for (auto item : selection->items()) {
        selected.push_back(item);
        submitted_objects.emplace_back(item);
    }
    if (completed_mode == SELECTION_PAGE && page_manager.getPageCount() == 1) {
        pages = {page_manager.getPage(0)};
    }
    for (auto page : pages) submitted_objects.emplace_back(const_cast<SPPage *>(page));
    auto inputs_current = [&] {
        if (!progress.current()) return false;
        for (auto const &object : submitted_objects) {
            if (!object || object->document != source) return false;
        }
        return true;
    };
    auto finish = [&] {
        if (auto si = progress.panel()) {
            si->setExporting(false); // Keep admission closed through property signals and the event pump.
        }
        if (auto si = progress.panel()) {
            si->interrupted = false;
            si->_exporting = false;
        }
    };
    _exporting = true;
    interrupted = false;
    auto const label = omod->is_raster()
        ? Glib::ustring::compose(_("Exporting %1 (%2 x %3)"), filename_label, width, height)
        : Glib::ustring::compose(_("Exporting %1"), filename_label);
    setExporting(true, label);
    if (!inputs_current() || progress.cancelled || progress.panel()->interrupted) { finish(); return; }

    if (omod->is_raster()) {
        exportSuccessful = Export::exportRaster(area, width, height, completed_dpi,
                                                background, filename_utf8, false,
                                                onProgressCallback, &progress, omod, selected_only ? &selected : nullptr);

    } else {
        auto copy_doc = source->copy();
        auto items = selected_only ? selected : std::vector<SPItem const *>{};

        if (completed_mode == SELECTION_PAGE && page_manager.hasPages()) {
            exportSuccessful = Export::exportVector(omod, copy_doc.get(), filename_utf8, false, items, pages);
        } else {
            // To get the right kind of export, we're going to make a page
            // This allows all the same raster options to work for vectors
            auto const page = copy_doc->getPageManager().newDocumentPage(area);
            exportSuccessful = Export::exportVector(omod, copy_doc.get(), filename_utf8, false, items, page);
        }
    }
    if (!progress.current()) { finish(); return; }
    // Cancellation is latched ONCE, before metadata admission. Once admitted,
    // view/focus changes and late Cancel must not strand a half-written tuple.
    bool const admitted = exportSuccessful && !progress.cancelled && !progress.panel()->interrupted;
    // Explicit re-export hints are actual document metadata, not directory history.
    if (admitted &&
        hint_target && hint_target->document == source) {
        constexpr std::array names{"inkscape:export-filename", "inkscape:export-xdpi", "inkscape:export-ydpi"};
        auto attributes = [&] {
            std::array<std::string, 3> result;
            for (std::size_t i = 0; i < names.size(); ++i) {
                auto value = hint_target->getRepr()->attribute(names[i]);
                result[i] = value ? value : "";
            }
            return result;
        };
        auto const before = attributes();
        // Match the existing setters' normalization/serialization without calling
        // any XML setter: equal tuples produce no mutation-quiescence callbacks.
        auto filename = Glib::filename_to_utf8(completed_path);
        auto doc_filename = source->getDocumentFilename();
        auto base = Glib::path_get_dirname(doc_filename ? doc_filename : filename.c_str());
        SVGOStringStream dpi_text;
        dpi_text << completed_dpi;
        std::array<std::string, 3> const desired{
            Inkscape::optimizePath(filename, base), dpi_text.str(), dpi_text.str()};
        if (before != desired) {
            // Record only: never replay/undo across somebody else's done(), and
            // never append the rest of our tuple to its fresh XML transaction.
            bool external_commit = false;
            bool own_commit = false;
            sigc::scoped_connection commit_started = source->connectBeforeCommit([&] {
                if (!own_commit) external_commit = true;
            });
            // Publish and log the complete tuple before the first observer can
            // commit, close the view or rebind the representation to a new object.
            // The source operation lease retains its document across callbacks.
            std::vector<XML::Node::AttributeUpdate> updates;
            for (std::size_t i = 0; i < names.size(); ++i) updates.push_back({names[i], desired[i]});
            bool const changed = hint_target->getRepr()->setAttributesAtomically(std::move(updates));
            if (changed && progress.sourceAlive() && !external_commit) {
                own_commit = true;
                DocumentUndo::done(source, RC_("Undo", "Set Export Options"), INKSCAPE_ICON("export"));
            }
        }
    }
    // Interactive directory history is profile state, never an SVG edit or Undo command.
    if (admitted && progress.sourceAlive()) {
        auto const &path = completed_path;
        auto recentmanager = Gtk::RecentManager::get_default();
        if (recentmanager && Glib::path_is_absolute(path)) {
            Glib::ustring uri = Glib::filename_to_uri(path);
            recentmanager->add_item(uri);
        }

        IO::ExportDestination::record_result(completed_key, path, true, false,
            [](auto const &key, auto const &folder) {
                Inkscape::Preferences::get()->setString(key, Glib::filename_to_utf8(folder));
            });
    }
    finish();
}

void SingleExport::onBrowse()
{
    if (_exporting || browseConn.blocked() || !_app || !_app->get_active_window() || !_document) {
        return;
    }

    auto const lifetime = _callback_lifetime;
    auto const source = _document;
    auto const generation = _document_generation;
    auto operation = DocumentUndo::holdInteractionOperation(source);
    auto current = [&] {
        auto panel = *lifetime;
        return panel && panel->_document == source && panel->_document_generation == generation &&
               !DocumentUndo::interactionCloseRequested(source);
    };
    auto unblock = [&] { if (*lifetime) (*lifetime)->browseConn.unblock(); };

    Gtk::Window *window = _app->get_active_window();

    browseConn.block();
    auto omod = si_extension_cb.getExtension();
    assert(omod);

    std::string filename = Glib::filename_from_utf8(si_filename_entry.get_text());

    if (filename.empty()) {
        filename = _destination.path();
    }

    // Note, there are currently multiple modules per filename extension (.svg, .dxf, .zip).
    // We cannot distinguish between them.
    std::string basename = Glib::path_get_basename(filename);
    std::string dirname = Glib::path_get_dirname(filename);
    auto file = choose_file_save( _("Select a filename for exporting"), window,
                                  create_export_filters(), // {}, // mimetype
                                  basename,
                                  dirname);

    if (!current()) { unblock(); return; }

    if (file) {
        auto native = file->get_path();
        if (native.empty()) { browseConn.unblock(); return; }
        extensionConn.block();
        si_extension_cb.setExtensionFromFilename(native);
        if (!*lifetime) return;
        extensionConn.unblock();
        if (!current()) { unblock(); return; }
        auto ext = si_extension_cb.getExtension();
        if (!ext) { unblock(); return; }
        _directory_request.reset();
        _destination.edit(native, ext->get_extension(), true);
        _destination_ready = true;
        setFilename(native, true);
        if (!current()) { unblock(); return; }
        onExport();
    }

    unblock();
}

// Utils Functions

void SingleExport::blockSpinConns(bool status = true)
{
    for (auto &signal : spinButtonConns) {
        signal.block(status);
    }
}

void SingleExport::areaXChange(sb_type type)
{
    auto x0_adj = spin_buttons[SPIN_X0]->get_adjustment();
    auto x1_adj = spin_buttons[SPIN_X1]->get_adjustment();
    auto width_adj = spin_buttons[SPIN_WIDTH]->get_adjustment();

    float x0, x1, dpi, width, bmwidth;

    // Get all values in px
    Unit const *unit = units.getUnit();
    x0 = unit->convert(x0_adj->get_value(), "px");
    x1 = unit->convert(x1_adj->get_value(), "px");
    width = unit->convert(width_adj->get_value(), "px");
    bmwidth = spin_buttons[SPIN_BMWIDTH]->get_value();
    dpi = spin_buttons[SPIN_DPI]->get_value();

    switch (type) {
        case SPIN_X0:
            bmwidth = (x1 - x0) * dpi / DPI_BASE;
            if (bmwidth < SP_EXPORT_MIN_SIZE) {
                x0 = x1 - (SP_EXPORT_MIN_SIZE * DPI_BASE) / dpi;
            }
            break;
        case SPIN_X1:
            bmwidth = (x1 - x0) * dpi / DPI_BASE;
            if (bmwidth < SP_EXPORT_MIN_SIZE) {
                x1 = x0 + (SP_EXPORT_MIN_SIZE * DPI_BASE) / dpi;
            }
            break;
        case SPIN_WIDTH:
            bmwidth = width * dpi / DPI_BASE;
            if (bmwidth < SP_EXPORT_MIN_SIZE) {
                width = (SP_EXPORT_MIN_SIZE * DPI_BASE) / dpi;
            }
            x1 = x0 + width;
            break;
        default:
            break;
    }

    width = x1 - x0;
    bmwidth = floor(width * dpi / DPI_BASE + 0.5);

    auto px = UnitTable::get().getUnit("px");
    x0_adj->set_value(px->convert(x0, unit));
    x1_adj->set_value(px->convert(x1, unit));
    width_adj->set_value(px->convert(width, unit));
    spin_buttons[SPIN_BMWIDTH]->set_value(bmwidth);
}

void SingleExport::areaYChange(sb_type type)
{
    auto y0_adj = spin_buttons[SPIN_Y0]->get_adjustment();
    auto y1_adj = spin_buttons[SPIN_Y1]->get_adjustment();
    auto height_adj = spin_buttons[SPIN_HEIGHT]->get_adjustment();

    float y0, y1, dpi, height, bmheight;

    // Get all values in px
    Unit const *unit = units.getUnit();
    y0 = unit->convert(y0_adj->get_value(), "px");
    y1 = unit->convert(y1_adj->get_value(), "px");
    height = unit->convert(height_adj->get_value(), "px");
    bmheight = spin_buttons[SPIN_BMHEIGHT]->get_value();
    dpi = spin_buttons[SPIN_DPI]->get_value();

    switch (type) {
        case SPIN_Y0:
            bmheight = (y1 - y0) * dpi / DPI_BASE;
            if (bmheight < SP_EXPORT_MIN_SIZE) {
                y0 = y1 - (SP_EXPORT_MIN_SIZE * DPI_BASE) / dpi;
            }
            break;
        case SPIN_Y1:
            bmheight = (y1 - y0) * dpi / DPI_BASE;
            if (bmheight < SP_EXPORT_MIN_SIZE) {
                y1 = y0 + (SP_EXPORT_MIN_SIZE * DPI_BASE) / dpi;
            }
            break;
        case SPIN_HEIGHT:
            bmheight = height * dpi / DPI_BASE;
            if (bmheight < SP_EXPORT_MIN_SIZE) {
                height = (SP_EXPORT_MIN_SIZE * DPI_BASE) / dpi;
            }
            y1 = y0 + height;
            break;
        default:
            break;
    }

    height = y1 - y0;
    bmheight = floor(height * dpi / DPI_BASE + 0.5);

    auto px = UnitTable::get().getUnit("px");
    y0_adj->set_value(px->convert(y0, unit));
    y1_adj->set_value(px->convert(y1, unit));
    height_adj->set_value(px->convert(height, unit));
    spin_buttons[SPIN_BMHEIGHT]->set_value(bmheight);
}

void SingleExport::dpiChange(sb_type type)
{
    float dpi, height, width, bmheight, bmwidth;

    // Get all values in px
    Unit const *unit = units.getUnit();
    height = unit->convert(spin_buttons[SPIN_HEIGHT]->get_value(), "px");
    width = unit->convert(spin_buttons[SPIN_WIDTH]->get_value(), "px");
    bmheight = spin_buttons[SPIN_BMHEIGHT]->get_value();
    bmwidth = spin_buttons[SPIN_BMWIDTH]->get_value();
    dpi = spin_buttons[SPIN_DPI]->get_value();

    switch (type) {
        case SPIN_BMHEIGHT:
            if (bmheight < SP_EXPORT_MIN_SIZE) {
                bmheight = SP_EXPORT_MIN_SIZE;
            }
            dpi = bmheight * DPI_BASE / height;
            break;
        case SPIN_BMWIDTH:
            if (bmwidth < SP_EXPORT_MIN_SIZE) {
                bmwidth = SP_EXPORT_MIN_SIZE;
            }
            dpi = bmwidth * DPI_BASE / width;
            break;
        case SPIN_DPI:
            prefs->setDouble("/dialogs/export/defaultdpi/value", dpi);
            break;
        default:
            break;
    }

    bmwidth = floor(width * dpi / DPI_BASE + 0.5);
    bmheight = floor(height * dpi / DPI_BASE + 0.5);

    spin_buttons[SPIN_BMHEIGHT]->set_value(bmheight);
    spin_buttons[SPIN_BMWIDTH]->set_value(bmwidth);
    spin_buttons[SPIN_DPI]->set_value(dpi);
}

void SingleExport::setDefaultSelectionMode()
{
    current_key = (selection_mode)0; // default key
    bool found = false;
    Glib::ustring pref_key_name = prefs->getString("/dialogs/export/exportarea/value");
    for (auto [key, name] : selection_names) {
        if (pref_key_name == name) {
            current_key = key;
            found = true;
            break;
        }
    }
    if (!found) {
        pref_key_name = selection_names[current_key];
    }

    if (_desktop) {
        if (current_key == SELECTION_SELECTION && (_desktop->getSelection())->isEmpty()) {
            current_key = (selection_mode)0;
        }
        if ((_desktop->getSelection())->isEmpty()) {
            selection_buttons[SELECTION_SELECTION]->set_sensitive(false);
        }
        if (current_key == SELECTION_CUSTOM &&
            (spin_buttons[SPIN_HEIGHT]->get_value() == 0 || spin_buttons[SPIN_WIDTH]->get_value() == 0)) {
            Geom::OptRect bbox = _document->preferredBounds();
            setArea(bbox->min()[Geom::X], bbox->min()[Geom::Y], bbox->max()[Geom::X], bbox->max()[Geom::Y]);
        }
    } else {
        current_key = (selection_mode)0;
    }
    selection_buttons[current_key]->set_active(true);
    prefs->setString("/dialogs/export/exportarea/value", pref_key_name);

    toggleSpinButtonVisibility();
    refreshPage();
}

void SingleExport::setExporting(bool exporting, Glib::ustring const &text)
{
    // Property notifications may synchronously destroy this panel, even before
    // the explicit event-loop iteration below. Do not touch its widgets again.
    auto const lifetime = _callback_lifetime;
    set_sensitive(!exporting);
    if (!*lifetime) return;
    set_opacity(exporting ? 0.2 : 1.0);
    if (!*lifetime) return;
    progress_box.set_visible(exporting);
    if (!*lifetime) return;
    progress_bar.set_text(exporting ? text : "");
    if (!*lifetime) return;
    progress_bar.set_fraction(0.0);
    if (!*lifetime) return;
    auto main_context = Glib::MainContext::get_default();
    main_context->iteration(false);
}

// Called for every progress iteration
unsigned int SingleExport::onProgressCallback(float value, void *data)
{
    auto progress = static_cast<ExportProgress *>(data);
    if (progress && progress->current() && !progress->cancelled) {
        auto si = progress->panel();
        si->progress_bar.set_fraction(value);
        auto main_context = Glib::MainContext::get_default();
        main_context->iteration(false);
        progress->cancelled = !progress->current() || progress->panel()->interrupted;
        return !progress->cancelled;
    }
    return false;
}

void SingleExport::refreshPreview()
{
    if (!_desktop) {
        preview.resetPixels();
        return;
    }

    std::vector<SPItem const *> selected;
    if (si_hide_all.get_active()) {
        // This is because selection items is not a std::vector yet. FIXME.
        auto sel_range = _desktop->getSelection()->items();
        selected = {sel_range.begin(), sel_range.end()};
    }
    _preview_drawing->set_shown_items(std::move(selected));

    bool show = si_show_preview.get_active();
    if (!show || current_key == SELECTION_PAGE) {
        bool have_pages = false;
        for (auto &child : UI::children(pages_list)) {
            if (auto bi = dynamic_cast<BatchItem *>(&child)) {
                bi->refresh(!show, _background_color.get_current_color().toRGBA());
                have_pages = true;
            }
        }
        if (have_pages) {
            // We don't want to update the main preview for pages, it's hidden
            preview.resetPixels();
            return;
        }
    }

    Unit const *unit = units.getUnit();
    float x0 = unit->convert(spin_buttons[SPIN_X0]->get_value(), "px");
    float x1 = unit->convert(spin_buttons[SPIN_X1]->get_value(), "px");
    float y0 = unit->convert(spin_buttons[SPIN_Y0]->get_value(), "px");
    float y1 = unit->convert(spin_buttons[SPIN_Y1]->get_value(), "px");
    preview.setBox(Geom::Rect(x0, y0, x1, y1) * _document->dt2doc());
    preview.setBackgroundColor(_background_color.get_current_color().toRGBA());
    preview.queueRefresh();
}

void SingleExport::setDesktop(SPDesktop *desktop)
{
    if (desktop != _desktop) {
        _page_selected_connection.disconnect();
        _desktop = desktop;
    }
}

void SingleExport::setDocument(SPDocument *document)
{
    if (_document == document)
        return;

    _document = document;
    ++_document_generation;
    _directory_request.reset();
    _destination = {};
    _destination_context.clear();
    _destination_ready = false;
    filepath_native.clear();
    _filename_set_connection.disconnect();

    _page_selected_connection.disconnect();
    _page_modified_connection.disconnect();
    _page_changed_connection.disconnect();

    if (document) {
        _filename_set_connection = document->connectFilenameSet([this](char const *) { refreshDestination(); });
        auto &pm = document->getPageManager();
        _page_selected_connection = pm.connectPageSelected(sigc::mem_fun(*this, &SingleExport::onPagesSelected));
        _page_modified_connection = pm.connectPageModified(sigc::mem_fun(*this, &SingleExport::onPagesModified));
        _page_changed_connection = pm.connectPagesChanged(sigc::mem_fun(*this, &SingleExport::onPagesChanged));
        _background_color.setColor(get_export_bg_color(document->getNamedView(), Colors::Color(0xffffff00)));
        _preview_drawing = std::make_shared<PreviewDrawing>(document);
        preview.setDrawing(_preview_drawing);

        // Refresh values to sync them with defaults.
        onPagesChanged(nullptr);
        refreshArea();
        loadExportHints();
    } else {
        preview.setDrawing({});
        _preview_drawing.reset();
        onPagesChanged(nullptr);
    }
}

SingleExport::~SingleExport()
{
    *_callback_lifetime = nullptr;
    _directory_request.reset();
}

} // namespace Inkscape::UI::Dialog

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
