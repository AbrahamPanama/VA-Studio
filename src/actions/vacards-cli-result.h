// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * VACards agent command line: result records, result channel, outcome latch
 * and the action specification registry (design 5.3-5.5).
 *
 * Every VACards CLI action emits exactly one JSON Lines record, also when it
 * rejects or fails. Records go to stderr (prefixed) until vacards-result-file
 * selects stdout (prefixed) or a file (bare lines). Process exit status:
 * 0 success, 3 at least one rejection, 4 at least one failed/uncertain outcome
 * (the highest applies; export failure 1 is handled in inkscape-main.cpp).
 */

#ifndef SEEN_VACARDS_CLI_RESULT_H
#define SEEN_VACARDS_CLI_RESULT_H

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/json/object.hpp>

#include "vacards-cli-params.h"

namespace Inkscape::VACardsCli {

inline constexpr std::string_view result_schema = "va-studio.cli-result/1";
/// Prefix of records written to stdout or stderr (never to a result file).
inline constexpr std::string_view record_prefix = "VASTUDIO-RESULT ";

enum class Status
{
    Ok,        ///< read-only query succeeded
    Changed,   ///< committed, one Undo step
    Unchanged, ///< legitimate no-op
    Rejected,  ///< invalid argument, no eligible target or refused precondition; atomic no-op
    Cancelled, ///< stopped by deadline or cancellation before commit
    Failed,    ///< runtime failure after validation; atomic no-op
    Uncertain, ///< file publication outcome unknown
    Skipped,   ///< not run because an earlier action rejected or failed
};

/// "ok", "changed", "unchanged", "rejected", "cancelled", "failed", "uncertain", "skipped".
std::string_view status_name(Status status);

struct Exclusion
{
    std::string id;
    std::string reason;
};

struct Record
{
    std::string action;
    std::string params_text;                                 ///< the raw parameter string as received
    std::vector<std::pair<std::string, std::string>> params; ///< given (decoded) parameters, input order
    Status status = Status::Ok;
    std::string reason = "success";
    std::string message;
    std::string mode;
    bool dry_run = false;
    std::optional<std::string> document_path; ///< nullopt: no document (JSON null)
    int selected = 0;
    int eligible = 0;
    int covered = 0;
    std::vector<Exclusion> excluded;
    std::vector<std::string> created;
    std::vector<std::string> modified;
    std::vector<std::string> deleted;
    std::vector<std::string> selection_after;
    bool one_undo_step = false; ///< JSON "undo": "one-step" or "none"
    boost::json::object metrics;
    std::vector<std::string> warnings;
    std::optional<ParseError> error; ///< parameter error details; JSON "error": {"code","key"} when set
    boost::json::object typed_extensions; ///< Only canonical action adapters populate this.
    std::string preferred_unit = "mm";
    boost::json::object normalized_params;
    std::string document_id;
    std::uint64_t revision_before = 0, revision_after = 0;
    std::string undo_effect = "none";
    std::string publication = "not-published";
    boost::json::object data;        ///< action-specific payload; JSON "data" only when non-empty
    bool publication_persisted = false; // M2; legacy serializer deliberately ignores this.
    // Native adapter receipt, copied to typed error.details (including reason and mutation_state).
    // Set at the actual typed failure branch; never reconstruct from diagnostic text.
    boost::json::object error_details;
    std::optional<bool> error_retryable;
};

/// Serialize one record as a single JSON line (no prefix, no trailing newline) with this key order:
/// schema, seq, action, params_text, params, status, reason, message, mode, dry_run, document,
/// targets{selected, eligible, covered, excluded[{id, reason}]}, created, modified, deleted,
/// selection_after, undo, metrics, warnings, [error], [data].
std::string to_json_line(Record const &record, unsigned seq);

/// A record for an action that was not run after an earlier rejection or failure.
Record make_skipped_record(std::string action, std::string params_text);

/// Select the result channel: "-" = stdout (prefixed lines); anything else = a file path that is
/// created or truncated (bare lines). On failure returns false, sets @a error and keeps the
/// previous channel.
bool open_result_channel(std::string const &target, std::string &error);

/// Assign the next sequence number (1, 2, ...), write the record as one line to the current
/// channel and flush, then update the outcome latch and the halt state.
void emit(Record const &record);

/// Record a rejection that has no result record (unknown, disabled or unparsable upstream
/// action). Affects the exit status only, never halting.
void note_rejection();

/// 0, 3 or 4 as described in the file comment.
int exit_status();

/// Halt-on-error (default true). When true, a Rejected/Failed/Uncertain record makes
/// halt_pending() true until the next begin_chain().
void set_halt_on_error(bool halt);
bool halt_on_error();
bool halt_pending();
/// Start of one action chain (one --actions string for one document, or one shell line).
void begin_chain();

/// Start of one document's command-line processing (all chains and shell lines for it).
/// Clears the export veto.
void begin_document();
/// True when, since begin_document(), a VACards action ended rejected, failed or uncertain while
/// halt-on-error was on. The automatic --export-filename/batch export must then not run.
bool export_vetoed();

/// RAII marker for actions run from a command-line chain (--actions, --shell, batch), whether or
/// not a desktop exists. Actions started interactively (menus, command palette) run outside it.
class CommandLineChainScope
{
public:
    CommandLineChainScope();
    ~CommandLineChainScope();
    CommandLineChainScope(CommandLineChainScope const &) = delete;
    CommandLineChainScope &operator=(CommandLineChainScope const &) = delete;
};
bool command_line_chain_active();

/// Restore the initial state: stderr channel (closing any file), seq 0, latch 0,
/// halt-on-error true, no halt pending, no export veto, no active chain. Registered specs are kept.
void reset_for_testing();

/// Register a VACards CLI action specification (idempotent by name). The spec must outlive the process.
void register_action_spec(ActionSpec const &spec);
/// All registered specs sorted by name.
std::vector<ActionSpec const *> registered_action_specs();
ActionSpec const *find_action_spec(std::string_view name);

/// {"name", "mode", "summary", "params": [{"key", "type", ["unit"], "required", ["default"],
///  ["min"], ["max"], ["choices"], ["help"]}]}. Integral limits are JSON integers.
boost::json::object describe_action(ActionSpec const &spec);

} // namespace Inkscape::VACardsCli

#endif // SEEN_VACARDS_CLI_RESULT_H

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
