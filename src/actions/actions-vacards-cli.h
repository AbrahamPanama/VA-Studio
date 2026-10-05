// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * VACards agent command line: channel/option/describe actions and the shared
 * runner every VACards CLI action uses (parse, run, finish and emit one record).
 */

#ifndef SEEN_ACTIONS_VACARDS_CLI_H
#define SEEN_ACTIONS_VACARDS_CLI_H

#include <functional>
#include <memory>

#include <glibmm/variant.h>

#include "vacards-cli-params.h"
#include "vacards-cli-result.h"

class InkscapeApplication;
class SPDocument;
class SPItem;

namespace Inkscape {
class Selection;
} // namespace Inkscape

namespace Inkscape::VACardsCli {

struct ActionContext
{
    InkscapeApplication *app = nullptr;
    ParseResult const &params;
    SPDocument *document = nullptr;             ///< active document; never null when the runner calls a body
                                                ///< that needs a document
    Inkscape::Selection *selection = nullptr;   ///< active selection (headless: the document's selection)
    Record &record;                             ///< pre-filled; the body sets status, reason, message, ...
    std::shared_ptr<void> operation_lease;
};

/**
 * Run one VACards CLI action and report exactly one outcome (a record when records_enabled(),
 * otherwise a status-bar message):
 * 1. register @a spec; read the string parameter from @a value ("" when absent);
 * 2. parse it; a parse error emits Rejected / "invalid-argument" without calling @a body;
 * 3. when @a needs_document and there is no active document, emit Rejected / "no-document";
 * 4. call @a body; any escaping exception becomes Failed / "internal-error";
 * 5. fill document path and selection_after, then report.
 * The record starts as Status::Ok / "success" with action, mode, params_text, params and dry_run
 * ("dry-run" parameter, when the spec has one) already set.
 */
void run_action(ActionSpec const &spec, Glib::VariantBase const &value, InkscapeApplication *app,
                std::function<void(ActionContext &)> const &body, bool needs_document = true);

/// The string of a string-typed action parameter; "" for an empty/absent value.
std::string string_parameter(Glib::VariantBase const &value);

/// True for command-line use: no desktop, or the action runs inside a command-line chain (--actions,
/// --shell, batch). Interactive use (menus, command palette) reports to the status bar instead, so it
/// never prints records, halts chains or changes the exit status.
bool records_enabled(InkscapeApplication *app);

/// Document-based availability for headless actions (the GUI uses the desktop's display state instead):
/// the item and every ancestor item are displayed (not display:none) and none is locked.
bool document_available(SPItem const *item);

} // namespace Inkscape::VACardsCli

/// Registers app.vacards-result-file, app.vacards-options and app.vacards-describe.
void add_actions_vacards_cli(InkscapeApplication *app);

#endif // SEEN_ACTIONS_VACARDS_CLI_H

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
