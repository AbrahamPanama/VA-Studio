// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_SPARROW_ADAPTER_H
#define INKSCAPE_SPARROW_ADAPTER_H
#include <chrono>

#include "nesting-document.h"

namespace Inkscape::Nesting {
// No PATH search or user-environment executable override. The adjacent helper
// must match the build's pinned artifact. Unsupported jobs keep the native lane.
bool sparrowAvailable();
bool sparrowEligible(PreparedDocumentNesting const &, Options const &);
SolveResult solveSparrow(PreparedDocumentNesting const &, Options const &,
                         std::chrono::steady_clock::time_point deadline, std::stop_token);
// Both vectors must have passed geometric validation before comparison.
bool betterNestingCandidate(PreparedDocumentNesting const &, std::span<Placement const> candidate,
                            std::span<Placement const> incumbent);
double capturedNestingArea(PreparedDocumentNesting const &, std::span<Placement const>);
} // namespace Inkscape::Nesting
#endif
