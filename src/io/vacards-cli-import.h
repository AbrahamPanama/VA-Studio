// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_IO_VACARDS_CLI_IMPORT_H
#define INKSCAPE_IO_VACARDS_CLI_IMPORT_H
#include "actions/vacards-cli-dispatch.h"
#include "io/vacards-cli-resources.h"
#include "io/vacards-cli-intake.h"
namespace Inkscape::VACardsCli {
Record import_document(Request const &, DispatchContext &, Grants const &, IntakeObserver const & = {});
} // namespace Inkscape::VACardsCli
#endif
