// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * VACards agent command line: bitmap tone queries and adjustments actions.
 */

#ifndef SEEN_ACTIONS_VACARDS_BITMAP_H
#define SEEN_ACTIONS_VACARDS_BITMAP_H

class InkscapeApplication;

/// Registers the app.vacards-* bitmap tone queries and adjustments actions.
void add_actions_vacards_bitmap(InkscapeApplication *app);


#include "vacards-cli-registry.h"
namespace Inkscape::VACardsCli {
struct Request; struct DispatchContext; struct ProductionContext; struct Record;
std::vector<PackageCommand> bitmap_commands();
Record execute_bitmap(Request const &, DispatchContext &, ProductionContext &);
}


#include <memory>
namespace Inkscape::VACardsCli { struct TokenSnapshot; }
namespace Inkscape::Bitmap {
class CliSession {
public:
    CliSession();
    ~CliSession();
    CliSession(CliSession const &) = delete;
    CliSession &operator=(CliSession const &) = delete;
    void retire() noexcept;
    void prune(VACardsCli::TokenSnapshot const &);
    VACardsCli::Record execute(VACardsCli::Request const &, VACardsCli::DispatchContext &, VACardsCli::ProductionContext &);
private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};
}

#endif // SEEN_ACTIONS_VACARDS_BITMAP_H
