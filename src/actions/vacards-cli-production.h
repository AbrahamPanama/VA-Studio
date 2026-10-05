// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "vacards-cli-dispatch.h"
#include "vacards-cli-tokens.h"
namespace Inkscape::Bitmap { class CliSession; }
namespace Inkscape::Nesting { class CliSession; }
namespace Inkscape::VACardsCli {
struct FileState;
struct Grants;
struct EditServices;
// Main-context references only, all owned by the same Engine. On close: cancel,
// settle/join jobs, close delivery, retire tokens/context, then destroy document.
// No SPDocument/Selection/native pointer crosses a worker boundary.
struct ProductionContext {
    FileState &files;
    Grants const &grants;
    EditServices &edits;
    TokenStore &tokens;
    std::function<void()> cancel_and_settle;
    // Owner-supplied token identity. One-shot has an empty session and cannot retain.
    std::string session_id, catalog_identity;
    std::uint64_t incarnation = 0, target_generation = 0;
    Nesting::CliSession *nest = nullptr; // Persistent Engine-owned native capture.
    Bitmap::CliSession *bitmap = nullptr; // Persistent Engine-owned native capability, never a token payload.
};
inline constexpr bool m3_accepted = false, m4_accepted = false;
Record production_unavailable(Request const &);
// Development overlay admission; legacy native registry remains unchanged.
Record dispatch_production(Request const &, DispatchContext &);
ActionSpec const *find_production_command(std::string_view);
RequestParse parse_production_request(std::string_view);
Record execute_production(Request const &, DispatchContext &, ProductionContext &);
void production_unavailable_action(ActionContext &);
// Authoritative M3 metadata lives in each input descriptor's x-m3-contract.
// The agent development catalog overlays canonical IDs without changing legacy aliases.
// Static descriptor backing storage; canonical IDs replace (never duplicate)
// legacy typed IDs only when integration explicitly adopts version 2.
std::vector<PackageCommand> production_commands();
boost::json::object planned_production_catalog();
// Executable development catalog: all 31 overlay rows are experimental and unaccepted.
boost::json::object production_catalog();
}
