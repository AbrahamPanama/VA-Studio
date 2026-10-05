// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_ACTIONS_VACARDS_CLI_DISPATCH_H
#define INKSCAPE_ACTIONS_VACARDS_CLI_DISPATCH_H
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <boost/json/object.hpp>
#include "vacards-cli-registry.h"
#include "vacards-cli-result.h"
#include "vacards-cli-transaction.h"
class InkscapeApplication;
namespace Inkscape::VACardsCli {
struct Request
{
    std::string id, command;
    std::optional<std::string> document;
    std::optional<std::uint64_t> if_revision;
    boost::json::object params;
    bool dry_run = false;
};
struct RequestParse
{
    std::optional<Request> request;
    std::optional<ParseError> error;
    // Validated correlation only; an error never provides an executable request.
    std::string error_id, error_command;
};
struct DispatchContext
{
    InkscapeApplication *app = nullptr;
    SPDocument *document = nullptr;
    Selection *selection = nullptr;
    std::string preferred_unit = "mm";
    std::uint64_t session_revision = 0;
    std::function<bool()> cancelled;
    std::shared_ptr<void> operation_lease;
    // Optional private agent session state; legacy action dispatch leaves this empty.
    std::function<Record(Request const &)> session_handler;
    // Agent controls can mutate the revision from the reader thread.
    std::function<std::uint64_t()> session_revision_snapshot;
    std::function<void()> session_state_changed; // Increment once under Engine state lock.
    std::function<Record(Request const &)> file_handler;
    // Integration: validate exclusive route before defaults (selected branch only), exact
    // uint64 and recursively converted CSS-px bounds; then document identity,
    // descriptor guard_domain/guard_required, read-only and cancellation.
    // Selection requires document identity + SESSION revision; history.query's
    // document guard is optional, but a supplied identity/guard is always checked.
    // Invoke before generic disposable-edit dispatch: each adapter owns its
    // preview route and settlement. This callback remains unconnected for now.
    std::function<Record(Request const &)> production_handler;
    // Evaluated only after schema, identity/revision and cancellation checks.
    std::function<std::optional<Record>(Request const &)> command_admission;
};
// Process action-chain session; the agent session owns a separate persistent DispatchContext.
DispatchContext &action_session_context();
// Publishes only the actual caller lease; the transaction still validates its identity.
class ActionOperationScope
{
public:
    ActionOperationScope(DispatchContext &context, std::shared_ptr<void> const &lease)
        : _context(context) { _context.operation_lease = lease; }
    ~ActionOperationScope() { _context.operation_lease.reset(); }
    ActionOperationScope(ActionOperationScope const &) = delete;
    ActionOperationScope &operator=(ActionOperationScope const &) = delete;
private:
    DispatchContext &_context;
};
RequestParse parse_request(std::string_view json);
ParseResult typed_params(ActionSpec const &spec, boost::json::object const &params);
Record dispatch(Request const &request, DispatchContext &context);
// Legacy parsing stays at the adapter edge; both adapters call this typed executor.
void dispatch_validated(ActionSpec const &spec, ParseResult const &params, DispatchContext &context, Record &record,
                        std::function<void(ActionContext &)> const &handler = {}, bool needs_document = true);
boost::json::object typed_result(Record const &record, std::string_view id, unsigned seq = 0);
// Common typed error construction; retryability is classified by the native error code.
boost::json::object cli_error(std::string code, std::string message, std::string hint);
int typed_exit_status(Status status);
boost::json::object report_length(double px, std::string_view preferred_unit = "mm");
} // namespace Inkscape::VACardsCli
#endif
