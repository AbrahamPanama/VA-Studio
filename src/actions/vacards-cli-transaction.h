// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_ACTIONS_VACARDS_CLI_TRANSACTION_H
#define INKSCAPE_ACTIONS_VACARDS_CLI_TRANSACTION_H
#include <cstdint>
#include <string>
#include <vector>

#include "document-undo.h"
namespace Inkscape {
class Selection;
}
namespace Inkscape::VACardsCli {
// Main-context only. Revision follows native commits and Undo/Redo, never rollback.
struct DocumentStamp
{
    std::string id;
    std::uint64_t revision = 0;
};
DocumentStamp document_stamp(SPDocument *document);
// Isolates pending XML and selection. Destruction rolls back without clearing Redo.
// Existing prepare/apply services that own native atomic settlement keep that ownership.
class EditTransaction
{
public:
    EditTransaction(SPDocument *document, Selection *selection,
                    std::shared_ptr<void> const &owner_lease = {});
    ~EditTransaction();
    EditTransaction(EditTransaction const &) = delete;
    EditTransaction &operator=(EditTransaction const &) = delete;
    bool active() const { return _interaction && _interaction->active(); }
    void commit(Util::Internal::ContextString label, Glib::ustring const &icon);
    void rollback() noexcept;

private:
    std::shared_ptr<void> _owner_lease;
    SPDocument *_document;
    Selection *_selection;
    std::vector<std::string> _selected;
    std::optional<DocumentUndo::PendingFence> _pending;
    std::optional<DocumentUndo::RollbackableInteraction> _interaction;
};
// Tokens are values, with no clock/TTL. The session owns retention and single-use consumption.
struct TokenBinding
{
    std::string session, document, engine, catalog_hash; // lineage is TokenParent ID + kind
    std::uint64_t revision = 0;
    std::vector<std::string> ordered_ids, dependency_hashes;
    std::string normalized_params;
    // session_revision is provenance, not a token freshness guard.
    std::uint64_t incarnation = 0, target_generation = 0, session_revision = 0;
};
} // namespace Inkscape::VACardsCli
#endif
