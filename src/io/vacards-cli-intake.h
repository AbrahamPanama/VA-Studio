// SPDX-License-Identifier: GPL-2.0-or-later
// Agent command line: read-only inspection intake through the shared restricted loader.
// Frozen interface: the session's --inspect-file calls it, this module implements it. Members and overloads may be
// added, never renamed or removed.
#ifndef INKSCAPE_IO_VACARDS_CLI_INTAKE_H
#define INKSCAPE_IO_VACARDS_CLI_INTAKE_H
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <boost/json/object.hpp>
#include "document.h"
#include "io/vacards-cli-resources.h"
namespace Inkscape::VACardsCli {
/// BUG-025 owner ceilings; stricter existing engine limits win.
struct IntakeLimits
{
    std::uint64_t max_input_bytes = 512ull << 20;
    std::uint64_t max_objects = 1000000;
    unsigned max_xml_depth = 128;
    std::uint64_t max_conversion_bytes = 512ull << 20;
    unsigned max_conversion_pages = 1000;
};
struct IntakeError
{
    std::string code, message, hint;
    bool retryable = false;
    boost::json::object details;
};
struct IntakeResult
{
    std::unique_ptr<SPDocument> document; ///< immutable inspection document; null on error
    std::string document_id;             ///< new for every load
    boost::json::object report;          ///< format, normalization/resource report, ID map
    std::optional<IntakeError> error;
    boost::json::object source_version; ///< Identity/hash/size of the admitted source bytes; not part of the M1 report schema.
};
/// Load an absolute local path read-only. Never shows UI; never resolves remote or ungranted resources.
IntakeResult load_inspection_document(std::string const &absolute_path, Grants const &grants,
                                      IntakeLimits const &limits = {});
/// Formats load_inspection_document accepts, for hello and --version --json (for example "svg", "svgz").
std::vector<std::string> inspection_formats();
/// Immutable intake provenance, valid until the document is destroyed. Null for non-inspection documents.
boost::json::object const *inspection_report(SPDocument const *document);
/// Check available memory before a raster export; returns a typed engine-limit refusal when it cannot fit.
std::optional<IntakeError> admit_raster_export_memory(std::uint64_t pixels);
// Native-only synchronous observations of intake call sites, not filesystem interception.
// Borrowed for one request; no global state, CLI switch or retained callback.
struct IntakeObservation {
    std::string phase, path, identity, access;
    bool admitted = false;
};
using IntakeObserver = std::function<void(IntakeObservation const &)>;
struct FileLoadOptions { std::string format, resource_policy, font_policy;
    std::vector<unsigned> pages;
    // Native-only synchronous test observation; not exposed by CLI options.
    std::function<void(ResourceAccess const &)> before_linked_read_for_testing;
    IntakeObserver observe_for_testing; }; // pages: 1-based; explicit policy, no dialog
IntakeResult load_editable_document(std::string const &, Grants const &,
                                   FileLoadOptions const &, IntakeLimits const & = {});
// Re-admit serialization resources on an independent document; never touches live XML.
IntakeResult prepare_file_snapshot(SPDocument const &, Grants const &, std::string const &resource_policy);
IntakeResult load_inspection_document_for_testing(std::string const &, Grants const &,
                                                 IntakeLimits const &, IntakeObserver const &);
IntakeResult prepare_file_snapshot_for_testing(SPDocument const &, Grants const &,
                                              std::string const &, IntakeObserver const &);
} // namespace Inkscape::VACardsCli
#endif
