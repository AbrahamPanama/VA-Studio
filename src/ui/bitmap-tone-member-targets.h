// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "actions/vacards-cli-edit-services.h"
namespace Inkscape::VACardsCli {
// Compatible image descendants once; excludes protected/missing/clone and
// unproved ancestor-tone combinations. Legacy root mode remains unchanged.
struct ToneMemberTargets {
    EditTargets targets;
    std::optional<ParseError> error;
};
ToneMemberTargets resolve_tone_members(SPDocument &, std::vector<std::string> const &explicit_roots);
}
