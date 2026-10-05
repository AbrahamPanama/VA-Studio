// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_ACTIONS_VACARDS_CLI_REGISTRY_H
#define INKSCAPE_ACTIONS_VACARDS_CLI_REGISTRY_H
#include <boost/json.hpp>

#include "vacards-cli-params.h"
namespace Inkscape::VACardsCli {
// Recursive JSON Schema descriptor, shared by validation, discovery and adapters.
// Only the supported 2020-12 vocabulary emitted by the combinators is used.
struct TypeDescriptor
{
    boost::json::object schema;
};
inline constexpr std::string_view catalog_version = "va-studio.cli-catalog/1";
std::vector<ActionSpec const *> command_specs();
ActionSpec const *find_command(std::string_view name);
boost::json::object request_schema(ActionSpec const &spec);
boost::json::object result_schema_descriptor(ActionSpec const &spec);
boost::json::object production_result_schema(boost::json::object const &data);
boost::json::object command_catalog();
std::string catalog_hash(boost::json::value const &descriptor);
boost::json::array mcp_descriptors();
std::optional<ParseError> validate_schema(boost::json::value const &value, boost::json::object const &schema,
                                          std::string path = "");
// Validate exclusive raw route before recursive defaults; output is canonical CSS px.
std::optional<ParseError> normalize_schema_params(boost::json::object const &raw, boost::json::object const &schema,
                                                 boost::json::object &normalized);
/// Package-owned command tables, appended by the registry after its own commands (session.*,
/// query.*). Views and spans must have static storage. An empty result_data keeps the registry's
/// default data schema; errors and warnings are added to the shared codes.
struct PackageCommand
{
    ActionSpec spec;
    std::string_view id, effects, policy;
    boost::json::object example;
    boost::json::object result_data;
    std::vector<std::string_view> errors, warnings;
};
std::vector<PackageCommand> session_commands(); ///< vacards-cli-session.cpp
std::vector<PackageCommand> query_commands();   ///< actions-vacards-query.cpp
// Service exports: lifted original bodies, never an action-string trampoline.
ActionSpec boolean_command();
ActionSpec transform_resize_command();
ActionSpec offset_command();
ActionSpec corners_command();
ActionSpec tone_query_command();
ActionSpec histogram_command();
ActionSpec options_command();
ActionSpec describe_command();
ActionSpec undo_command();
ActionSpec redo_command();
ActionSpec result_file_command();
} // namespace Inkscape::VACardsCli
#endif
