// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * VACards agent command line: channel/option/describe actions and the shared
 * runner every VACards CLI action uses (parse, run, finish and emit one record).
 */

#include "actions-vacards-cli.h"

#include <exception>
#include <string>

#include <glibmm/i18n.h>

#include "config.h"
#include "desktop.h"
#include "document.h"
#include "document-undo.h"
#include "message-stack.h"
#include "inkscape-application.h"
#include "inkscape-version.h"
#include "object/sp-item.h"
#include "selection.h"

#ifdef WITH_VACARDS_NESTING
#include "nesting/sparrow-adapter.h"
#endif

namespace Inkscape::VACardsCli {

std::string string_parameter(Glib::VariantBase const &value)
{
    if (!value.gobj()) {
        return "";
    }
    if (value.get_type_string() != "s") {
        return "";
    }
    return Glib::VariantBase::cast_dynamic<Glib::Variant<Glib::ustring>>(value).get().raw();
}

bool records_enabled(InkscapeApplication *app)
{
    // Decided by how the action was invoked, not by whether a desktop exists:
    // --batch-process and --with-gui --actions create one but are still
    // command-line runs that need records, halting and the exit status.
    return !app || !app->get_active_desktop() || command_line_chain_active();
}

bool document_available(SPItem const *item)
{
    if (!item || item->isLocked()) {
        return false;
    }
    for (SPObject const *object = item; object; object = object->parent) {
        if (auto const *ancestor = cast<SPItem>(object); ancestor && ancestor->isHidden()) {
            return false;
        }
    }
    return true;
}

namespace {

// Command line (no desktop): one result record, exit latch and halt state.
// GUI (command palette, menus): the message goes to the status bar instead, so
// an interactive session never prints records or changes the exit status.
void report(InkscapeApplication *app, Record const &record)
{
    if (records_enabled(app)) {
        emit(record);
        return;
    }
    auto *desktop = app ? app->get_active_desktop() : nullptr;
    if (!desktop || !desktop->messageStack() || record.message.empty()) {
        return;
    }
    auto const type = record.status == Status::Rejected || record.status == Status::Failed
                              || record.status == Status::Uncertain
                          ? Inkscape::ERROR_MESSAGE
                          : Inkscape::NORMAL_MESSAGE;
    desktop->messageStack()->flash(type, record.message);
}

} // namespace

void run_action(ActionSpec const &spec, Glib::VariantBase const &value, InkscapeApplication *app,
                std::function<void(ActionContext &)> const &body, bool needs_document)
{
    register_action_spec(spec);

    auto const text = string_parameter(value);

    Record record;
    record.action = std::string(spec.name);
    record.mode = std::string(spec.mode);
    record.params_text = text;

    auto const parsed = parse_params(spec, text);

    SPDocument *document = app ? app->get_active_document() : nullptr;
    Inkscape::Selection *selection = app ? app->get_active_selection() : nullptr;

    if (document) {
        char const *f = document->getDocumentFilename();
        record.document_path = f ? std::string(f) : std::string();
    }

    if (!parsed.ok()) {
        record.status = Status::Rejected;
        record.reason = "invalid-argument";
        record.message = parsed.error->message;
        record.error = parsed.error;
        report(app, record);
        return;
    }

    record.params = parsed.given;
    if (parsed.has("dry-run")) {
        record.dry_run = parsed.at("dry-run").boolean;
    }

    if (needs_document && !document) {
        record.status = Status::Rejected;
        record.reason = "no-document";
        record.message = "This action needs an open document.";
        report(app, record);
        return;
    }

    ActionContext context{app, parsed, document, selection, record};
    try {
        body(context);
    } catch (std::exception const &e) { // includes Glib::Error (glibmm 2.68+)
        record.status = Status::Failed;
        record.reason = "internal-error";
        record.message = e.what();
    } catch (...) {
        record.status = Status::Failed;
        record.reason = "internal-error";
        record.message = "Unexpected internal error.";
    }

    if (selection) {
        record.selection_after.clear();
        for (auto *item : selection->items()) {
            if (char const *id = item->getId()) {
                record.selection_after.emplace_back(id);
            }
        }
    }

    report(app, record);
}

namespace {

constexpr ParamSpec options_params[] = {
    {.key = "halt-on-error", .type = ParamType::Boolean,
     .help = "Stop the remaining actions of a chain after a rejected, failed or uncertain VACards action."}};

constexpr ActionSpec options_spec{.name = "vacards-options", .mode = "session",
    .summary = "Change VACards command-line session options.", .params = options_params};

constexpr ActionSpec describe_spec{.name = "vacards-describe", .mode = "read-only",
    .summary = "Describe VA Studio and every VACards command-line action."};

constexpr ActionSpec result_file_spec{.name = "vacards-result-file", .mode = "session",
    .summary = "Write result records to a JSON Lines file, or to stdout with '-'."};

void result_file(Glib::VariantBase const &value, InkscapeApplication *app)
{
    register_action_spec(result_file_spec);

    auto const target = string_parameter(value);

    Record record;
    record.action = "vacards-result-file";
    record.mode = "session";
    record.params_text = target;

    if (target.empty()) {
        record.status = Status::Rejected;
        record.reason = "invalid-argument";
        record.message = "vacards-result-file needs a file path or '-'.";
        emit(record);
        return;
    }

    std::string error;
    if (!open_result_channel(target, error)) {
        record.status = Status::Rejected;
        record.reason = "cannot-open-result-file";
        record.message = error;
        emit(record); // goes to the previous channel
        return;
    }
    record.data["target"] = target;
    emit(record); // first line of the new channel
}

void options(Glib::VariantBase const &value, InkscapeApplication *app)
{
    run_action(options_spec, value, app, [](ActionContext &c) {
        if (c.params.has("halt-on-error")) {
            set_halt_on_error(c.params.at("halt-on-error").boolean);
        }
        c.record.data["halt-on-error"] = halt_on_error();
    }, false);
}

constexpr ActionSpec undo_spec{.name = "vacards-undo", .mode = "session",
    .summary = "Undo the last Undo step of the active document (command-line counterpart of Edit > Undo)."};
constexpr ActionSpec redo_spec{.name = "vacards-redo", .mode = "session",
    .summary = "Redo the last undone step of the active document (command-line counterpart of Edit > Redo)."};

// Upstream undo/redo exist only as document/window actions, which --actions
// cannot reach. These make "one action is one Undo step" observable and usable.
void undo_redo(Glib::VariantBase const &value, InkscapeApplication *app, bool redo)
{
    run_action(redo ? redo_spec : undo_spec, value, app, [redo](ActionContext &c) {
        bool const done = redo ? DocumentUndo::redo(c.document) : DocumentUndo::undo(c.document);
        if (done) {
            c.document->ensureUpToDate();
            c.record.status = Status::Changed;
            c.record.message = redo ? "Redid one step." : "Undid one step.";
        } else {
            c.record.status = Status::Unchanged;
            c.record.reason = redo ? "nothing-to-redo" : "nothing-to-undo";
            c.record.message = redo ? "There is nothing to redo." : "There is nothing to undo.";
        }
    });
}

void undo(Glib::VariantBase const &value, InkscapeApplication *app) { undo_redo(value, app, false); }
void redo(Glib::VariantBase const &value, InkscapeApplication *app) { undo_redo(value, app, true); }

void describe(Glib::VariantBase const &value, InkscapeApplication *app)
{
    run_action(describe_spec, value, app, [](ActionContext &c) {
        register_action_spec(result_file_spec);
        register_action_spec(options_spec);
        auto &d = c.record.data;
        d["product"] = VACARDS_PRODUCT_NAME;
        d["display_version"] = VACARDS_PRODUCT_DISPLAY_VERSION;
        d["version"] = VACARDS_PRODUCT_VERSION;
        d["inkscape_version"] = Inkscape::version_string;
        d["schema"] = std::string(result_schema);
        boost::json::object features;
#ifdef WITH_VACARDS_NESTING
        features["nesting"] = true;
        features["sparrow_helper"] = Inkscape::Nesting::sparrowAvailable();
#else
        features["nesting"] = false;
        features["sparrow_helper"] = false;
#endif
#ifdef WITH_LIBCDR
        features["libcdr"] = true;
#else
        features["libcdr"] = false;
#endif
        d["features"] = features;
        boost::json::object codes;
        codes["0"] = "success";
        codes["1"] = "export-failure";
        codes["3"] = "rejected";
        codes["4"] = "failed-or-uncertain";
        d["exit_codes"] = codes;
        boost::json::array actions;
        for (auto const *s : registered_action_specs()) {
            actions.push_back(describe_action(*s));
        }
        d["actions"] = actions;
    }, false);
}

} // namespace

} // namespace Inkscape::VACardsCli

std::vector<std::vector<Glib::ustring>> raw_data_vacards_cli = {
    {"app.vacards-result-file", N_("VACards Result File"), N_("VACards"), N_("Write VACards command-line results to a file")},
    {"app.vacards-options", N_("VACards CLI Options"), N_("VACards"), N_("Change VACards command-line options")},
    {"app.vacards-describe", N_("VACards Describe"), N_("VACards"), N_("Describe VACards command-line actions")},
    {"app.vacards-undo", N_("VACards Undo"), N_("VACards"), N_("Undo the last step (command line)")},
    {"app.vacards-redo", N_("VACards Redo"), N_("VACards"), N_("Redo the last undone step (command line)")}};

void add_actions_vacards_cli(InkscapeApplication *app)
{
    auto *gapp = app->gio_app();
    Glib::VariantType String(Glib::VARIANT_TYPE_STRING);
    gapp->add_action_with_parameter("vacards-result-file", String, sigc::bind(sigc::ptr_fun(&Inkscape::VACardsCli::result_file), app));
    gapp->add_action_with_parameter("vacards-options", String, sigc::bind(sigc::ptr_fun(&Inkscape::VACardsCli::options), app));
    gapp->add_action_with_parameter("vacards-describe", String, sigc::bind(sigc::ptr_fun(&Inkscape::VACardsCli::describe), app));
    gapp->add_action_with_parameter("vacards-undo", String, sigc::bind(sigc::ptr_fun(&Inkscape::VACardsCli::undo), app));
    gapp->add_action_with_parameter("vacards-redo", String, sigc::bind(sigc::ptr_fun(&Inkscape::VACardsCli::redo), app));

    Inkscape::VACardsCli::register_action_spec(Inkscape::VACardsCli::result_file_spec);
    Inkscape::VACardsCli::register_action_spec(Inkscape::VACardsCli::options_spec);
    Inkscape::VACardsCli::register_action_spec(Inkscape::VACardsCli::describe_spec);
    Inkscape::VACardsCli::register_action_spec(Inkscape::VACardsCli::undo_spec);
    Inkscape::VACardsCli::register_action_spec(Inkscape::VACardsCli::redo_spec);

    app->get_action_extra_data().add_data(raw_data_vacards_cli);
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
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4 :
