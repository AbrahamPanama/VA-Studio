// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef VACARDS_CLI_ENTRY_H
#define VACARDS_CLI_ENTRY_H
#include <boost/json.hpp>
#include <functional>
namespace Inkscape::VACardsCli {
boost::json::object cli_identity();
// Optional in-process diagnostic seam; no command-line or environment switch.
int agent_main(int argc, char **argv, std::function<void()> diagnostic_probe = {});
}
#endif
