// SPDX-License-Identifier: GPL-2.0-or-later
#include "artwork-library-placement.h"
#include "desktop.h"
#include "document.h"
#include "document-undo.h"
#include "selection.h"
#include "object/sp-root.h"
#include <sigc++/sigc++.h>
#include <functional>
#include <limits>

namespace Inkscape::UI::Dialog {
IO::ArtworkLibrary::InsertedArtwork place_library_artwork(
    SPDesktop &desktop, IO::ArtworkLibrary::ValidatedSvg svg, Geom::Point origin)
{
    namespace Art = IO::ArtworkLibrary;
    auto document = desktop.getDocument();
    if (!document || DocumentUndo::interactionCloseRequested(document))
        throw Art::InsertionError(Art::InsertionFailure::Ineligible, "No live destination document");
    auto operation = DocumentUndo::holdInteractionOperation(document);
    bool alive = true;
    sigc::scoped_connection destroyed = desktop.connectDestroy([&](SPDesktop *) { alive = false; });
    auto current = [&] { return alive && desktop.getDocument() == document && !DocumentUndo::interactionCloseRequested(document); };
    document->ensureUpToDate();
    if (!current()) throw Art::InsertionError(Art::InsertionFailure::StaleTarget, "Destination changed before insertion");
    auto selection = desktop.getSelection();
    auto parent = selection->activeContext();
    if (!parent) parent = document->getRoot();
    auto target = Art::capture_insertion_target(*document, *parent);
    // insert_artwork owns preparation and atomic-settlement leases itself.
    // Do not nest our UI preparation lease across its quiescence check.
    // Releasing a lease schedules cleanup; it does not dispatch callbacks.
    operation.reset();
    auto result = Art::insert_artwork(*document, std::move(target), std::move(svg), origin, {}, [&] { return !current(); });
    // A committed insertion stays successful even if post-publication observers
    // close the desktop/remove the new object. Never retry or make a second Undo.
    if (current() && result.document_serial == document->serial() && !result.inserted_ids.empty()) {
        operation = DocumentUndo::holdInteractionOperation(document);
        auto object = document->getObjectById(result.inserted_ids.front());
        if (object) {
            if (selection->selectionPolicyRevision() > std::numeric_limits<std::uint64_t>::max() - 3) {
                result.warnings.emplace_back("Insertion committed; temporary selection policy revision exhausted");
                return result;
            }
            auto change_layer = selection->getChangeLayer(), change_page = selection->getChangePage();
            selection->setChangeLayer(false); selection->setChangePage(false);
            struct Restore {
                std::function<bool()> selection_alive; Selection *selection; bool layer, page; std::uint64_t policy;
                ~Restore() {
                    // A live desktop rebind preserves Selection identity. Destination
                    // eligibility is irrelevant to restoring temporary view policy.
                    // Any subsequent policy write belongs to its caller, even if it
                    // wrote false again. Rebinding alone is not a policy write.
                    if (selection_alive() && selection->selectionPolicyRevision() == policy) {
                        selection->setChangeLayer(layer); selection->setChangePage(page);
                    }
                }
            } restore{[&] { return alive && desktop.getSelection() == selection; }, selection, change_layer, change_page, selection->selectionPolicyRevision()};
            try { selection->set(object, false); }
            catch (std::exception const &e) { result.warnings.emplace_back(std::string("Insertion committed; view selection failed: ") + e.what()); }
        }
    }
    return result;
}
}
