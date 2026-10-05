// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "vacards-cli-registry.h"
namespace Inkscape::VACardsCli {
struct Request; struct DispatchContext; struct ProductionContext; struct Record;
std::vector<PackageCommand> clip_commands();
Record execute_clip(Request const &, DispatchContext &, ProductionContext &);
}
