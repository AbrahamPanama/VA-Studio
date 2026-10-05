// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * VACards agent command line: nesting actions.
 */

#ifndef SEEN_ACTIONS_VACARDS_NEST_H
#define SEEN_ACTIONS_VACARDS_NEST_H

class InkscapeApplication;

/// Registers the app.vacards-* nesting actions.
void add_actions_vacards_nest(InkscapeApplication *app);


#include "vacards-cli-registry.h"
namespace Inkscape::VACardsCli {
struct Request;
struct DispatchContext;
struct ProductionContext;
struct Record;
std::vector<PackageCommand> nest_commands();
Record execute_nest(Request const &, DispatchContext &, ProductionContext &);
}

#endif // SEEN_ACTIONS_VACARDS_NEST_H
