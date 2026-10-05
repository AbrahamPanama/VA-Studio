// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * VACards agent command line: boolean, offset, corners and resize actions.
 */

#ifndef SEEN_ACTIONS_VACARDS_GEOMETRY_H
#define SEEN_ACTIONS_VACARDS_GEOMETRY_H

class InkscapeApplication;

/// Registers the app.vacards-* boolean, offset, corners and resize actions.
void add_actions_vacards_geometry(InkscapeApplication *app);


#include "vacards-cli-registry.h"
namespace Inkscape::VACardsCli { std::vector<PackageCommand> geometry_commands(); }

#endif // SEEN_ACTIONS_VACARDS_GEOMETRY_H
