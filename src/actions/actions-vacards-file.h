// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * VACards agent command line: safe save actions.
 */

#ifndef SEEN_ACTIONS_VACARDS_FILE_H
#define SEEN_ACTIONS_VACARDS_FILE_H

#include <vector>
#include "vacards-cli-registry.h"

class InkscapeApplication;

/// Registers the app.vacards-* safe save actions.
void add_actions_vacards_file(InkscapeApplication *app);

namespace Inkscape::VACardsCli {
std::vector<PackageCommand> file_commands();
} // namespace Inkscape::VACardsCli

#endif // SEEN_ACTIONS_VACARDS_FILE_H
