// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_IO_VACARDS_CLI_FILES_H
#define INKSCAPE_IO_VACARDS_CLI_FILES_H
#include <memory>
#include "io/document-file-transaction.h"
#include "io/export-color-profiles.h"
#include <boost/json/object.hpp>
#include "document.h"
#include "actions/vacards-cli-dispatch.h"
#include "io/vacards-cli-resources.h"
#include "io/vacards-cli-intake.h"
namespace Inkscape::VACardsCli {
// The session owns lifetime; the file service owns operations on this state.
struct FileState { std::unique_ptr<SPDocument> document; bool read_only = false;
    bool writes_blocked = false; boost::json::object provenance, reconciliation;
    std::function<void()> before_document_retire; // Owner thread, before actual replacement/destruction only.
};
Record execute_file(Request const &, DispatchContext &, FileState &, Grants const &);
// Native-test-only request-local seam; never selected by CLI input or environment.
// Calls are borrowed for this synchronous invocation. Production entry stays unchanged.
struct FileServiceTestHooks {
    IO::DocumentTransaction::SystemCalls *calls = nullptr;
    IO::ExportColorProfiles const *default_profiles = nullptr;
    std::function<bool(unsigned)> new_stage, replacement_stage;
    IntakeObserver intake_observer;
};
Record execute_file_for_testing(Request const &, DispatchContext &, FileState &, Grants const &,
                                FileServiceTestHooks const &);
boost::json::object file_capabilities();
} // namespace Inkscape::VACardsCli
#endif
