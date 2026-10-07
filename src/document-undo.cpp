// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Undo/Redo stack implementation
 *
 * Authors:
 *   Lauris Kaplinski <lauris@kaplinski.com>
 *   MenTaLguY <mental@rydia.net>
 *   Abhishek Sharma
 *
 * Copyright (C) 2007  MenTaLguY <mental@rydia.net>
 * Copyright (C) 1999-2003 authors
 * Copyright (C) 2001-2002 Ximian, Inc.
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 *
 * Using the split document model gives sodipodi a very simple and clean
 * undo implementation. Whenever mutation occurs in the XML tree,
 * SPObject invokes one of the five corresponding handlers of its
 * container document. This writes down a generic description of the
 * given action, and appends it to the recent action list, kept by the
 * document. There will be as many action records as there are mutation
 * events, which are all kept and processed together in the undo
 * stack. Two methods exist to indicate that the given action is completed:
 *
 * \verbatim
   void sp_document_done( SPDocument *document );
   void sp_document_maybe_done( SPDocument *document, const unsigned char *key ) \endverbatim
 *
 * Both move the recent action list into the undo stack and clear the
 * list afterwards.  While the first method does an unconditional push,
 * the second one first checks the key of the most recent stack entry. If
 * the keys are identical, the current action list is appended to the
 * existing stack entry, instead of pushing it onto its own.  This
 * behaviour can be used to collect multi-step actions (like winding the
 * Gtk spinbutton) from the UI into a single undoable step.
 *
 * For controls implemented by Sodipodi itself, implementing undo as a
 * single step is usually done in a more efficient way. Most controls have
 * the abstract model of grab, drag, release, and change user
 * action. During the grab phase, all modifications are done to the
 * SPObject directly - i.e. they do not change XML tree, and thus do not
 * generate undo actions either.  Only at the release phase (normally
 * associated with releasing the mousebutton), changes are written back
 * to the XML tree, thus generating only a single set of undo actions.
 * (Lauris Kaplinski)
 */

#include "document-undo.h"

#include <glibmm/i18n.h>
#include <glibmm/ustring.h>                 // for ustring, operator==
#include <vector>                           // for vector
#include <algorithm>
#include <cstring>
#include <deque>
#include <utility>
#include <glibmm/main.h>

#include "document.h"                       // for SPDocument
#include "desktop.h"
#include "event.h"                          // for Event
#include "event-log.h"
#include "inkscape.h"                       // for Application, INKSCAPE
#include "inkscape-application.h"
#include "message-stack.h"
#include "composite-undo-stack-observer.h"  // for CompositeUndoStackObserver

#include "debug/event-tracker.h"            // for EventTracker
#include "debug/event.h"                    // for Event
#include "debug/simple-event.h"             // for SimpleEvent
#include "debug/timestamp.h"                // for timestamp
#include "object/sp-lpe-item.h"             // for sp_lpe_item_update_pathef...
#include "object/sp-root.h"                 // for SPRoot
#include "preferences.h"
#include "xml/event-fns.h"                  // for sp_repr_begin_transaction
#include "xml/event.h"
#include "xml/node.h"
#include "xml/attribute-record.h"
#include "xml/document.h"                   // for XML::Document::inTransaction

namespace Inkscape::XML {
class Event;
} // namespace Inkscape::XML

namespace {
std::vector<Inkscape::XML::Node *> replay_removals(Inkscape::XML::Event *event, bool undo)
{
    std::vector<Inkscape::XML::Node *> nodes;
    for (auto current = event; current; current = current->next) {
        if (undo) {
            if (auto add = dynamic_cast<Inkscape::XML::EventAdd *>(current)) nodes.push_back(add->child);
        } else {
            if (auto del = dynamic_cast<Inkscape::XML::EventDel *>(current)) nodes.push_back(del->child);
        }
    }
    return nodes;
}

void emit_replay_removals(SPDocument *document, Inkscape::XML::Event *event, bool undo)
{
    auto nodes = replay_removals(event, undo);
    document->emitHistoryReplayRemovals(nodes);
}
} // namespace

namespace Inkscape {
struct UndoInteractionLifetime : std::enable_shared_from_this<UndoInteractionLifetime> {
    SPDocument *document = nullptr; // Invalidated before SPDocument emits destroySignal.
    unsigned commit_depth = 0;
    unsigned operation_depth = 0;
    std::weak_ptr<void> command_operation; // Identity of a sole public operation lease.
    unsigned publication_depth = 0;
    unsigned atomic_publication_depth = 0;
    unsigned settlement_depth = 0;
    bool close_requested = false;
    bool stale_close = false;
    bool running = false;
    std::weak_ptr<UndoInteractionState> atomic_owner;
    bool atomic_commit_permit = false;
    bool atomic_clear_redo_permit = false;
    std::deque<std::function<void(SPDocument &)>> cleanup;
    std::deque<std::function<void(SPDocument &)>> owner_continuations;
    struct PublicationCompletion {
        std::function<void(SPDocument &)> full;
        std::function<void(SPDocument &)> report_only;
        std::function<bool()> finished;
    };
    std::deque<PublicationCompletion> publication_continuations;
    sigc::connection idle;
    sigc::connection mutation_finished;

    bool safe() const {
        return document && !commit_depth && DocumentUndo::getUndoSensitive(document) &&
            document->getReprDoc()->inTransaction() && !document->getReprDoc()->mutationActive();
    }
    bool ownerQuiescent() const {
        return document && !commit_depth && !operation_depth &&
            !document->getReprDoc()->mutationActive();
    }
    struct OperationScope {
        std::shared_ptr<UndoInteractionLifetime> lifetime;
        std::function<void()> release_application;
        bool settlement;
        explicit OperationScope(std::shared_ptr<UndoInteractionLifetime> value, bool admitted_settlement = false)
            : lifetime(std::move(value)), settlement(admitted_settlement) {
            if (auto app = InkscapeApplication::instance()) {
                release_application = app->holdDocumentOperation(lifetime->document);
            }
            ++lifetime->operation_depth;
            if (settlement) ++lifetime->settlement_depth;
        }
        ~OperationScope() {
            if (settlement) --lifetime->settlement_depth;
            --lifetime->operation_depth;
            lifetime->schedule(); // Never close inline while a caller is unwinding.
            if (release_application) release_application();
        }
    };
    struct PublicationScope {
        std::shared_ptr<UndoInteractionLifetime> lifetime;
        std::function<void()> release_application;
        bool atomic;
        explicit PublicationScope(std::shared_ptr<UndoInteractionLifetime> value)
            : lifetime(std::move(value)), atomic(!lifetime->atomic_owner.expired()) {
            if (auto app = InkscapeApplication::instance())
                release_application = app->holdDocumentOperation(lifetime->document);
            ++lifetime->publication_depth;
            if (atomic) ++lifetime->atomic_publication_depth;
        }
        ~PublicationScope() {
            if (atomic) --lifetime->atomic_publication_depth;
            --lifetime->publication_depth;
            lifetime->schedule();
            if (release_application) release_application();
        }
    };
    void schedule() {
        if (!ownerQuiescent() || running ||
            (cleanup.empty() && owner_continuations.empty() && publication_continuations.empty()) ||
            idle.connected()) return;
        if (stale_close && owner_continuations.empty() && cleanup.empty()) return;
        if (close_requested && owner_continuations.empty()) return;
        if (owner_continuations.empty() && !safe()) return;
        if (owner_continuations.empty() &&
            (cleanup.empty() || !safe()) &&
            DocumentUndo::interactionActive(document)) return;
        idle = Glib::signal_idle().connect([self = shared_from_this()] {
            self->idle.disconnect();
            if (!self->ownerQuiescent()) return false;
            self->running = true;
            struct RunningGuard {
                bool &running;
                ~RunningGuard() { running = false; }
            } running_guard{self->running};
            // Owner continuations are NOT settlement tasks: a confirmed close
            // must run without a lease and before any more cleanup is started.
            while (self->ownerQuiescent() && !self->owner_continuations.empty()) {
                auto continuation = std::move(self->owner_continuations.front());
                self->owner_continuations.pop_front();
                bool continuation_threw = false;
                try { continuation(*self->document); }
                catch (std::exception const &e) { continuation_threw = true; g_warning("Owner continuation failed: %s", e.what()); }
                catch (...) { continuation_threw = true; g_warning("Owner continuation failed with an unknown exception"); }
                if (self->document && self->close_requested && self->owner_continuations.empty())
                    self->close_requested = false;
                auto *app = InkscapeApplication::instance();
                bool const still_open = self->document && app &&
                    !DocumentUndo::interactionCloseRequested(self->document) &&
                    !app->documentClosePending(self->document) &&
                    !self->close_requested &&
                    [&] { auto docs = app->get_documents();
                           return std::find(docs.begin(), docs.end(), self->document) != docs.end(); }();
                if (self->document && self->stale_close && (continuation_threw || still_open)) {
                    if (std::any_of(self->publication_continuations.begin(),
                                    self->publication_continuations.end(),
                                    [](auto const &entry) { return !entry.finished || !entry.finished(); })) {
                        self->document->get_event_log()->invalidateFileSave();
                        self->document->setModifiedSinceSave(true);
                        if (SP_ACTIVE_DESKTOP && SP_ACTIVE_DESKTOP->getDocument() == self->document)
                            SP_ACTIVE_DESKTOP->messageStack()->flash(Inkscape::WARNING_MESSAGE,
                                _("Save outcome uncertain; inspect the destination."));
                    }
                    while (!self->publication_continuations.empty()) {
                        auto result = std::move(self->publication_continuations.front());
                        self->publication_continuations.pop_front();
                        try { result.report_only(*self->document); }
                        catch (...) { g_warning("Publication report failed after stale close"); }
                    }
                    self->stale_close = false;
                }
            }
            while (self->ownerQuiescent() && self->safe() && !self->close_requested && !self->stale_close &&
                   !DocumentUndo::interactionActive(self->document) &&
                   self->owner_continuations.empty() &&
                   !self->publication_continuations.empty()) {
                OperationScope operation(self, true);
                auto continuation = std::move(self->publication_continuations.front());
                self->publication_continuations.pop_front();
                try {
                    continuation.full(*self->document);
                } catch (std::exception const &e) {
                    g_warning("Publication completion failed: %s", e.what());
                } catch (...) {
                    g_warning("Publication completion failed with an unknown exception");
                }
            }
            while (self->safe() && !self->close_requested && !self->cleanup.empty() &&
                   self->owner_continuations.empty() &&
                   (self->publication_continuations.empty() || self->stale_close ||
                    DocumentUndo::interactionActive(self->document))) {
                OperationScope operation(self, true); // Includes task/capture destruction.
                auto task = std::move(self->cleanup.front());
                self->cleanup.pop_front();
                try { task(*self->document); }
                catch (std::exception const &e) { g_warning("Interaction cleanup failed: %s", e.what()); }
                catch (...) { g_warning("Interaction cleanup failed with an unknown exception"); }
            }
            running_guard.running = false;
            self->schedule();
            return false;
        }, G_PRIORITY_HIGH_IDLE);
    }
    ~UndoInteractionLifetime() {
        idle.disconnect();
        mutation_finished.disconnect();
    }
};

struct UndoInteractionState {
    std::weak_ptr<UndoInteractionLifetime> lifetime;
    XML::Event *previous_partial = nullptr;
    bool active = true;
    bool rollback_queued = false;
    bool atomic = false, interrupted = false, published = false;
    bool saved_dirty = false, saved_autosave = false, saved_virgin = false;
    Glib::ustring saved_key;
    double saved_expires = 0;
    std::optional<std::chrono::steady_clock::time_point> saved_timer;
    unsigned long saved_id_counter = 0;
    std::shared_ptr<void> atomic_operation; // Settlement admitted BEFORE any close request.
    sigc::signal<void()> retired;

    // Call only AFTER native commit/no-log/rollback settlement, not merely when
    // active becomes false: sealInteraction clears active before publication.
    // Stop fencing later history, but transfer the admitted operation to the
    // caller so retirement callbacks (including token destruction and close)
    // remain leased. An inactive token must not hold the owner busy forever.
    std::shared_ptr<void> finishAtomicSettlement() {
        g_assert(atomic && !active);
        if (auto alive = lifetime.lock(); alive && alive->atomic_owner.lock().get() == this) {
            alive->atomic_owner.reset();
        }
        return std::exchange(atomic_operation, {});
    }

    ~UndoInteractionState() { if (previous_partial) sp_repr_free_log(previous_partial); }
};
} // namespace Inkscape

std::shared_ptr<Inkscape::UndoInteractionLifetime>
Inkscape::DocumentUndo::interactionLifetime(SPDocument *document)
{
    if (!document->undo_interaction_lifetime) {
        auto lifetime = std::make_shared<UndoInteractionLifetime>();
        lifetime->document = document;
        lifetime->mutation_finished = document->getReprDoc()->signalMutationFinished().connect(
            [weak = std::weak_ptr(lifetime)] {
                if (auto alive = weak.lock()) alive->schedule();
            });
        document->undo_interaction_lifetime = std::move(lifetime);
    }
    return document->undo_interaction_lifetime;
}

bool Inkscape::DocumentUndo::interactionIsQuiescent(SPDocument *document)
{
    if (!document || document->undo_interaction_closing) return false;
    auto lifetime = interactionLifetime(document);
    return lifetime->safe() && !lifetime->operation_depth;
}

bool Inkscape::DocumentUndo::deferUntilInteractionQuiescent(
    SPDocument *document, std::function<void(SPDocument &)> continuation, bool closing, bool no_defer)
{
    auto lifetime = interactionLifetime(document);
    // An accepted close may reach this boundary with Undo disabled. No native
    // rollback or full publication completion is safe there; close reports the
    // pending result conservatively during documentClosing().
    if (closing && lifetime->close_requested && lifetime->ownerQuiescent() && !lifetime->safe())
        return false;
    if (lifetime->ownerQuiescent() && !(closing && !no_defer && interactionActive(document))) return false;
    if (closing) {
        lifetime->close_requested = true;
        lifetime->owner_continuations.emplace_front(std::move(continuation));
    } else {
        lifetime->owner_continuations.emplace_back(std::move(continuation));
    }
    lifetime->schedule();
    return true;
}

std::shared_ptr<void> Inkscape::DocumentUndo::holdInteractionOperation(SPDocument *document)
{
    if (!document || document->undo_interaction_closing) return {};
    auto lifetime = interactionLifetime(document);
    auto operation = std::make_shared<UndoInteractionLifetime::OperationScope>(lifetime);
    if (lifetime->operation_depth == 1) lifetime->command_operation = operation;
    return operation;
}

std::shared_ptr<void> Inkscape::DocumentUndo::holdPublication(SPDocument *document)
{
    if (!document || document->undo_interaction_closing) return {};
    return std::make_shared<UndoInteractionLifetime::PublicationScope>(interactionLifetime(document));
}

bool Inkscape::DocumentUndo::publicationCompletable(SPDocument const *document) noexcept
{
    if (!document || document->undo_interaction_closing || document->undo_interaction_active ||
        !document->sensitive) return false;
    auto *rdoc = document->rdoc;
    if (!rdoc || !rdoc->inTransaction() || rdoc->mutationActive()) return false;
    if (auto const &lifetime = document->undo_interaction_lifetime) {
        if (!lifetime->document || lifetime->commit_depth ||
            lifetime->operation_depth) return false;
    }
    return true;
}

bool Inkscape::DocumentUndo::publicationInProgress(SPDocument const *document) noexcept
{
    auto lifetime = document ? document->undo_interaction_lifetime.get() : nullptr;
    return lifetime && (lifetime->atomic_publication_depth || lifetime->commit_depth ||
                        lifetime->settlement_depth || !lifetime->atomic_owner.expired());
}

bool Inkscape::DocumentUndo::publicationPending(SPDocument const *document) noexcept
{
    return document && document->undo_interaction_lifetime &&
        !document->undo_interaction_lifetime->publication_continuations.empty();
}

void Inkscape::DocumentUndo::markInteractionBaselineDirty(SPDocument *document)
{
    if (!document) return;
    if (auto state = document->undo_interaction.lock(); state && state->active) {
        state->saved_dirty = true;
        state->saved_autosave = true;
    }
}

bool Inkscape::DocumentUndo::whenPublicationCompletable(
    SPDocument *document, std::function<void(SPDocument &)> continuation,
    std::function<void(SPDocument &)> report_only, std::function<bool()> finished)
{
    if (!document || document->undo_interaction_closing) return false;
    auto lifetime = interactionLifetime(document);
    if (!lifetime->document) return false;
    if (publicationCompletable(document) && !lifetime->running && !lifetime->stale_close) {
        UndoInteractionLifetime::OperationScope operation(lifetime, true);
        try { continuation(*document); }
        catch (std::exception const &e) { g_warning("Publication completion failed: %s", e.what()); }
        catch (...) { g_warning("Publication completion failed with an unknown exception"); }
        return true;
    }
    if (!report_only) report_only = [](SPDocument &) {
        g_warning("Publication outcome could not be applied safely before close; inspect the destination before reopening");
    };
    lifetime->publication_continuations.push_back({std::move(continuation), std::move(report_only), std::move(finished)});
    lifetime->schedule();
    return true;
}

void Inkscape::DocumentUndo::flushPublicationCompletions(SPDocument *document)
{
    if (!publicationCompletable(document)) return;
    auto lifetime = interactionLifetime(document);
    while (!lifetime->stale_close && publicationCompletable(document) && !lifetime->publication_continuations.empty()) {
        auto continuation = std::move(lifetime->publication_continuations.front());
        lifetime->publication_continuations.pop_front();
        UndoInteractionLifetime::OperationScope operation(lifetime, true);
        try {
            continuation.full(*document);
        } catch (std::exception const &e) {
            g_warning("Publication completion failed: %s", e.what());
        } catch (...) {
            g_warning("Publication completion failed with an unknown exception");
        }
    }
    lifetime->schedule();
}

void Inkscape::DocumentUndo::rollbackLiveInteractionForClose(SPDocument *document)
{
    if (!document || !interactionActive(document)) return;
    auto lifetime = interactionLifetime(document);
    if (!lifetime->ownerQuiescent() || !lifetime->safe()) return;
    if (auto state = document->undo_interaction.lock()) {
        // The accepted close permits settlement of the already-live preview.
        ++lifetime->settlement_depth;
        RollbackableInteraction token(state);
        token.rollback();
        --lifetime->settlement_depth;
    }
}

void Inkscape::DocumentUndo::clearStaleInteractionForClose(SPDocument *document) noexcept
{
    if (document && interactionActive(document) && document->undo_interaction.expired()) {
        document->undo_interaction_active = false;
        interactionLifetime(document)->stale_close = true;
    }
}

void sp_file_flush_async_save(SPDocument *document)
{
    Inkscape::DocumentUndo::flushPublicationCompletions(document);
}

bool Inkscape::DocumentUndo::interactionCloseRequested(SPDocument *document)
{
    return document->undo_interaction_closing || interactionLifetime(document)->close_requested;
}

bool Inkscape::DocumentUndo::fileOperationFreshReady(SPDocument const *document) noexcept
{
    if (!document || document->undo_interaction_closing) return false;
    if (document->undo_interaction_active) return false; // live rollbackable preview
    if (!document->sensitive) return false;              // Undo-insensitive
    auto *rdoc = document->rdoc;
    if (!rdoc || !rdoc->inTransaction() || rdoc->mutationActive()) return false;
    // Read any existing lifetime only; never create state just to answer a query.
    if (auto const &lifetime = document->undo_interaction_lifetime) {
        if (!lifetime->document || lifetime->close_requested || lifetime->commit_depth ||
            lifetime->operation_depth || lifetime->publication_depth) {
            return false;
        }
    }
    return true;
}

bool Inkscape::DocumentUndo::fileOperationOutputReady(SPDocument const *document) noexcept
{
    if (!document || document->undo_interaction_closing) return false;
    auto const &lifetime = document->undo_interaction_lifetime;
    if (!lifetime || !lifetime->document || lifetime->document != document) return false;
    // Exactly the validated request's own lease; a foreign lease is rejected.
    if (lifetime->commit_depth || lifetime->operation_depth != 1) return false;
    if (document->undo_interaction_active) return false; // live rollbackable preview
    if (!document->sensitive) return false;              // Undo-insensitive
    auto *rdoc = document->rdoc;
    if (!rdoc || !rdoc->inTransaction() || rdoc->mutationActive()) return false;
    // lifetime->close_requested is intentionally permitted here: a close that
    // becomes pending while this request is owned may wait for settlement.
    return true;
}

void Inkscape::DocumentUndo::deferInteractionCleanup(
    SPDocument *document, std::function<void(SPDocument &)> cleanup, bool immediate_if_quiescent)
{
    if (!document || document->undo_interaction_closing) return;
    auto lifetime = interactionLifetime(document);
    if (!lifetime->document || lifetime->close_requested) return;
    if (immediate_if_quiescent && lifetime->safe() && lifetime->ownerQuiescent() && !lifetime->running) {
        UndoInteractionLifetime::OperationScope operation(lifetime, true);
        cleanup(*document);
        cleanup = {}; // Destroy captured SPItem refs while still leased.
        return;
    }
    lifetime->cleanup.emplace_back(std::move(cleanup));
    lifetime->schedule();
}

void Inkscape::DocumentUndo::documentClosing(SPDocument *document) noexcept
{
    document->undo_interaction_closing = true;
    if (auto repr = document->getReprDoc()) repr->closeMutationFence();
    auto lifetime = document->undo_interaction_lifetime;
    if (lifetime) {
        lifetime->document = nullptr;
        lifetime->idle.disconnect();
        lifetime->mutation_finished.disconnect();
    }
    if (auto state = document->undo_interaction.lock()) {
        state->active = false;
        if (auto log = std::exchange(state->previous_partial, nullptr)) sp_repr_free_log(log);
    }
    document->undo_interaction.reset();
    document->undo_interaction_active = false;
    if (lifetime) lifetime->cleanup.clear(); // Tokens are inert before task destructors run.
    if (lifetime) lifetime->owner_continuations.clear();
    if (lifetime && !lifetime->publication_continuations.empty()) {
        document->get_event_log()->invalidateFileSave();
        while (!lifetime->publication_continuations.empty()) {
            auto continuation = std::move(lifetime->publication_continuations.front());
            lifetime->publication_continuations.pop_front();
            try {
                continuation.report_only(*document);
            } catch (std::exception const &e) {
                g_warning("Publication report failed: %s", e.what());
            } catch (...) {
                g_warning("Publication report failed with an unknown exception");
            }
        }
    }
}

Inkscape::DocumentUndo::RollbackableInteraction::RollbackableInteraction(
    RollbackableInteraction &&other) noexcept
    : _state(std::move(other._state))
{}

Inkscape::DocumentUndo::RollbackableInteraction &
Inkscape::DocumentUndo::RollbackableInteraction::operator=(RollbackableInteraction &&other) noexcept
{
    if (this != &other) {
        rollback();
        _state = std::move(other._state);
    }
    return *this;
}

Inkscape::DocumentUndo::RollbackableInteraction::~RollbackableInteraction()
{
    rollback();
}

bool Inkscape::DocumentUndo::RollbackableInteraction::interrupted() const
{
    return !_state || _state->interrupted;
}

bool Inkscape::DocumentUndo::RollbackableInteraction::commitAtomically(
    Util::Internal::ContextString description, Glib::ustring const &icon,
    std::function<bool()> ready)
{
    return DocumentUndo::commitAtomicInteraction(*this, description, icon, std::move(ready));
}

bool Inkscape::DocumentUndo::RollbackableInteraction::active() const
{
    return _state && _state->active;
}

bool Inkscape::DocumentUndo::RollbackableInteraction::validFor(SPDocument const *document) const
{
    return _state && DocumentUndo::interactionValidFor(*_state, document);
}

bool Inkscape::DocumentUndo::RollbackableInteraction::validAtomicFor(SPDocument const *document) const
{
    return _state && _state->atomic && DocumentUndo::interactionValidFor(*_state, document);
}

bool Inkscape::DocumentUndo::interactionValidFor(UndoInteractionState const &state, SPDocument const *document)
{
    if (!state.active || state.interrupted) return false;
    // Match the native lifetime document identity FIRST. The comparison rejects
    // nullptr and any stale/foreign raw argument without dereferencing it; no
    // lifetime or state is ever created here.
    auto lifetime = state.lifetime.lock();
    if (!lifetime || !lifetime->document || lifetime->document != document) return false;
    // Only the validated live native document is dereferenced from here on.
    if (lifetime->close_requested || !lifetime->safe()) return false;
    if (document->undo_interaction_closing || !document->undo_interaction_active ||
        document->undo_interaction.lock().get() != &state) {
        return false;
    }
    // Atomic tokens additionally own the admitted settlement lease. Their nonzero
    // operation_depth is expected and intentionally accepted.
    if (state.atomic && (lifetime->atomic_owner.lock().get() != &state ||
                         !state.atomic_operation)) {
        return false;
    }
    return true;
}

sigc::connection Inkscape::DocumentUndo::RollbackableInteraction::connectRetired(sigc::slot<void()> slot)
{
    return _state->retired.connect(std::move(slot));
}

void Inkscape::DocumentUndo::RollbackableInteraction::commit(
    Util::Internal::ContextString event_description,
    Glib::ustring const &undo_icon,
    unsigned int object_modified_tag)
{
    DocumentUndo::commitInteraction(*this, event_description, undo_icon, object_modified_tag);
}

void Inkscape::DocumentUndo::RollbackableInteraction::rollback() noexcept
{
    DocumentUndo::rollbackInteraction(*this);
}

std::optional<Inkscape::DocumentUndo::RollbackableInteraction>
Inkscape::DocumentUndo::beginRollbackableInteraction(SPDocument *document)
{
    return beginRollbackableInteraction(document, nullptr);
}

std::optional<Inkscape::DocumentUndo::RollbackableInteraction>
Inkscape::DocumentUndo::beginRollbackableInteraction(
    SPDocument *document, std::shared_ptr<void> const *owner_lease)
{
    g_return_val_if_fail(document != nullptr, std::nullopt);
    if (document->undo_interaction_closing) return std::nullopt;
    auto lifetime = interactionLifetime(document);
    auto const admitted_operation = [&] {
        if (!owner_lease) return lifetime->operation_depth == 0;
        return *owner_lease && lifetime->operation_depth == 1 &&
            !lifetime->command_operation.expired() &&
            !lifetime->command_operation.owner_before(*owner_lease) &&
            !owner_lease->owner_before(lifetime->command_operation);
    };
    if (!admitted_operation() || lifetime->close_requested) return std::nullopt;
    g_return_val_if_fail(document->sensitive, std::nullopt);
    g_return_val_if_fail(!document->undo_interaction_active, std::nullopt);
    g_return_val_if_fail(document->rdoc->inTransaction(), std::nullopt);
    if (!lifetime->safe() || !admitted_operation() || lifetime->close_requested) return std::nullopt;

    // Keep unrelated pending XML outside the rollback point. During the live
    // interaction ScopedInsensitive sections may move their own changes into
    // document->partial; those still belong to this rollback boundary.
    auto previous_partial = sp_repr_coalesce_log(
        document->partial, sp_repr_commit_undoable(document->rdoc));
    document->partial = nullptr;
    sp_repr_begin_transaction(document->rdoc);
    document->undo_interaction_active = true;

    auto state = std::make_shared<UndoInteractionState>();
    state->lifetime = lifetime;
    state->previous_partial = previous_partial;
    document->undo_interaction = state;
    // A live, non-atomic preview can dirty the document before it is rolled
    // back. Preserve the publication state for both kinds of interaction.
    state->saved_dirty = document->modified_since_save;
    state->saved_autosave = document->modified_since_autosave;
    state->saved_virgin = document->virgin;
    state->saved_key = document->actionkey;
    state->saved_expires = document->action_expires;
    state->saved_timer = document->undo_timer;
    state->saved_id_counter = document->object_id_counter;
    return RollbackableInteraction(std::move(state));
}

bool Inkscape::DocumentUndo::interactionActive(SPDocument const *document)
{
    return document && document->undo_interaction_active;
}

std::optional<Inkscape::DocumentUndo::RollbackableInteraction>
Inkscape::DocumentUndo::beginAtomicInteraction(SPDocument *document)
{
    return beginAtomicInteraction(document, nullptr);
}

std::optional<Inkscape::DocumentUndo::RollbackableInteraction>
Inkscape::DocumentUndo::beginAtomicCommandInteraction(
    SPDocument *document, std::shared_ptr<void> const &owner_lease)
{
    return beginAtomicInteraction(document, &owner_lease);
}

std::optional<Inkscape::DocumentUndo::RollbackableInteraction>
Inkscape::DocumentUndo::beginAtomicInteraction(
    SPDocument *document, std::shared_ptr<void> const *owner_lease)
{
    if (!document || !document->sensitive || document->undo_interaction_active ||
        document->undo_interaction_closing || !document->rdoc->inTransaction()) return std::nullopt;
    auto token = beginRollbackableInteraction(document, owner_lease);
    if (!token) return std::nullopt;
    auto state = token->_state;
    if (state->previous_partial) {
        // Preserve the complete pre-existing log; no rollback/update callbacks.
        document->partial = std::exchange(state->previous_partial, nullptr);
        state->active = false;
        document->undo_interaction.reset();
        document->undo_interaction_active = false;
        return std::nullopt;
    }
    auto lifetime = state->lifetime.lock();
    state->atomic = true;
    state->saved_dirty = document->modified_since_save;
    state->saved_autosave = document->modified_since_autosave;
    state->saved_virgin = document->virgin;
    state->saved_key = document->actionkey;
    state->saved_expires = document->action_expires;
    state->saved_timer = document->undo_timer;
    state->saved_id_counter = document->object_id_counter;
    state->atomic_operation = std::make_shared<UndoInteractionLifetime::OperationScope>(lifetime, true);
    lifetime->atomic_owner = state;
    return token;
}

bool Inkscape::DocumentUndo::atomicHistoryProtected(SPDocument const *document)
{
    return document && document->undo_interaction_lifetime &&
           !document->undo_interaction_lifetime->atomic_owner.expired();
}

bool Inkscape::DocumentUndo::atomicHistoryConflict(SPDocument *document)
{
    if (document && document->undo_interaction_lifetime) {
        if (auto owner = document->undo_interaction_lifetime->atomic_owner.lock()) {
            owner->interrupted = true;
            return true;
        }
    }
    return false;
}

namespace {
thread_local Inkscape::DocumentUndo::AtomicSettlementFault atomic_settlement_fault = nullptr;
void atomicSettlementCheckpoint(Inkscape::DocumentUndo::AtomicSettlementStage stage)
{
    if (atomic_settlement_fault && atomic_settlement_fault(stage)) throw std::bad_alloc();
}
}
void Inkscape::DocumentUndo::setAtomicSettlementFaultForTesting(AtomicSettlementFault fault) noexcept
{
    atomic_settlement_fault = fault;
}

bool Inkscape::DocumentUndo::commitAtomicInteraction(
    RollbackableInteraction &token, Util::Internal::ContextString description,
    Glib::ustring const &icon, std::function<bool()> ready)
{
    // Pin the admitted lease BEFORE preparation can roll back/destroy the token.
    // Outlive both local state and callback captures, including on early return.
    std::shared_ptr<void> settlement_operation;
    auto state = token._state;
    if (!state || !state->active || !state->atomic || state->interrupted) return false;
    settlement_operation = state->atomic_operation;
    if (!settlement_operation) return false;
    auto lifetime = state->lifetime.lock();
    if (!lifetime || !lifetime->safe() || lifetime->close_requested) return false;
    auto doc = lifetime->document;
    // Empty the parameter explicitly: its captures must be destroyed before
    // the local lease, not later during parameter teardown.
    std::function<bool()> ready_callback;
    ready_callback.swap(ready);
    auto still_active = [&] {
        return state->active && !state->interrupted &&
            lifetime->document == doc && !lifetime->close_requested && lifetime->safe();
    };
    // Keep the transaction rollbackable throughout callbacks and native updates.
    // maybeDone below skips this already completed preparation.
    doc->before_commit_signal.emit();
    if (!still_active()) return false;
    doc->collectOrphans();
    if (!still_active()) return false;
    doc->ensureUpToDate();
    if (!still_active()) return false;
    if (!ready_callback() || !still_active()) return false;
    lifetime->atomic_commit_permit = true;
    done(doc, description, icon);
    // done() has now completed native publication (or the no-log transaction).
    // Keep the history fence THROUGH its callbacks, then release it BEFORE
    // retirement: retirement may publish a distinct subsequent undo entry.
    // Pin both state and the admitted operation locally if retirement destroys
    // the token. Close stays deferred until this call returns, not until an
    // otherwise inactive token happens to be destroyed.
    if (!state->active) state->finishAtomicSettlement();
    // A close during publication is a committed outcome, never a failed insert.
    if (state->published) state->retired.emit();
    return state->published;
}

void Inkscape::DocumentUndo::sealInteraction(SPDocument *document)
{
    auto state = document->undo_interaction.lock();
    bool const retiring = state && state->active;
    if (retiring) {
        state->active = false;
        document->partial = sp_repr_coalesce_log(std::exchange(state->previous_partial, nullptr), document->partial);
    }
    document->undo_interaction.reset();
    document->undo_interaction_active = false;
    // Atomic retirement is delivered by commitAtomicInteraction only after
    // publication and a fresh XML transaction. Normal live tokens keep their
    // existing pre-commit notification timing, including no-log done.
    if (retiring && !state->atomic) state->retired.emit();
}

void Inkscape::DocumentUndo::commitInteraction(
    RollbackableInteraction &interaction,
    Util::Internal::ContextString event_description,
    Glib::ustring const &undo_icon,
    unsigned int object_modified_tag)
{
    if (!interaction.active()) return;
    auto lifetime = interaction._state->lifetime.lock();
    if (!lifetime || !lifetime->document) return;
    auto document = lifetime->document;
    UndoInteractionLifetime::OperationScope operation(lifetime);
    if (interaction._state->atomic) return; // Atomic tokens require checked commitAtomically().
    sealInteraction(document);
    done(document, event_description, undo_icon, object_modified_tag);
}

void Inkscape::DocumentUndo::rollbackInteraction(RollbackableInteraction &interaction) noexcept
{
    if (!interaction.active()) return;
    std::shared_ptr<void> settlement_operation;
    auto state = interaction._state;
    auto lifetime = state->lifetime.lock();
    if (!lifetime || !lifetime->document) {
        state->active = false;
        return;
    }
    // A close already accepted by the owner wins over NEW rollback work.
    // Settlement admitted before that request still owns its partial-log
    // restoration and must finish, including a close from an LPE/selection callback.
    if (lifetime->close_requested && !lifetime->settlement_depth) return;
    if (!lifetime->safe()) {
        if (!state->rollback_queued) {
            state->rollback_queued = true;
            deferInteractionCleanup(lifetime->document, [state](SPDocument &) {
                state->rollback_queued = false;
                RollbackableInteraction token(state);
                token.rollback();
            });
        }
        return;
    }
    auto document = lifetime->document;
    UndoInteractionLifetime::OperationScope operation(lifetime, true);
    auto previous_partial = std::exchange(state->previous_partial, nullptr);
    struct RollbackScope {
        std::shared_ptr<UndoInteractionLifetime> lifetime;
        explicit RollbackScope(std::shared_ptr<UndoInteractionLifetime> value) : lifetime(std::move(value)) {
            ++lifetime->commit_depth;
        }
        ~RollbackScope() { --lifetime->commit_depth; lifetime->schedule(); }
    } rollback_scope(lifetime);
    state->active = false;
    document->undo_interaction.reset();

    // Rollback must use the same reconstruction mode as Undo. In particular,
    // SPObject's ID conflict resolver must not rename surviving objects while
    // duplicate nodes are being removed in reverse order.
    auto const was_sensitive = document->sensitive;
    auto const was_seeking = document->seeking;
    document->sensitive = false;
    document->seeking = true;

    if (document->rdoc->inTransaction()) {
        sp_repr_rollback(document->rdoc);
    }
    if (document->partial) {
        emit_replay_removals(document, document->partial, true);
        sp_repr_undo_log(document->partial);
        sp_repr_free_log(document->partial);
    }
    document->partial = previous_partial;
    sp_repr_begin_transaction(document->rdoc);
    document->ensureUpToDate();
    document->update_lpobjs();
    // Live path effects may rewrite their result path while reconstructing
    // the rolled-back document. Those writes are baseline maintenance, not a
    // pending user edit. Settle their XML before the next atomic preview frame.
    if (state->atomic) {
        if (auto *update_log = sp_repr_commit_undoable(document->rdoc)) {
            sp_repr_free_log(update_log);
        }
        sp_repr_begin_transaction(document->rdoc);
    }
    document->sensitive = was_sensitive;
    document->seeking = was_seeking;
    document->undo_interaction_active = false;
    document->modified_since_save = state->saved_dirty;
    document->modified_since_autosave = state->saved_autosave;
    document->virgin = state->saved_virgin;
    document->actionkey = state->saved_key;
    document->action_expires = state->saved_expires;
    document->undo_timer = state->saved_timer;
    document->object_id_counter = state->saved_id_counter;
    // Reconstruction callbacks above still require the fence even though the
    // token is already inactive. Transfer its lease only after exact restoration;
    // the local rollback operation also covers the remaining stack unwinding.
    if (state->atomic) settlement_operation = state->finishAtomicSettlement();
}

/*
 * Undo & redo
 */

void Inkscape::DocumentUndo::setUndoSensitive(SPDocument *doc, bool sensitive)
{
    if (doc && sensitive != doc->sensitive && atomicHistoryConflict(doc)) return;
	g_assert (doc != nullptr);

	if ( sensitive == doc->sensitive )
		return;

	if (sensitive) {
		sp_repr_begin_transaction (doc->rdoc);
	} else {
		doc->partial = sp_repr_coalesce_log (
			doc->partial,
			sp_repr_commit_undoable (doc->rdoc)
		);
	}

	doc->sensitive = sensitive;
    if (doc->undo_interaction_lifetime) doc->undo_interaction_lifetime->schedule();
}

bool Inkscape::DocumentUndo::getUndoSensitive(SPDocument const *document) {
	g_assert(document != nullptr);

	return document->sensitive;
}

void Inkscape::DocumentUndo::done(SPDocument *doc,
                                  Inkscape::Util::Internal::ContextString event_description,
                                  Glib::ustring const &icon_name,
                                  unsigned int object_modified_tag)
{
    if (doc->sensitive) {
        maybeDone(doc, nullptr, event_description, icon_name, object_modified_tag);
    }
}

void Inkscape::DocumentUndo::resetKey( SPDocument *doc )
{
    doc->actionkey.clear();
}

void Inkscape::DocumentUndo::setKeyExpires(SPDocument *doc, double seconds)
{
    doc->action_expires = seconds;
}

namespace {

using Inkscape::Debug::Event;
using Inkscape::Debug::SimpleEvent;
using Inkscape::Debug::timestamp;
using Inkscape::Util::Internal::ContextString;

std::size_t string_bytes(char const *value)
{
    return value ? std::strlen(value) : 0;
}

// Attribute and text bytes of a node and everything below it.
std::size_t node_payload_bytes(Inkscape::XML::Node const *node)
{
    std::size_t total = string_bytes(node->content());
    for (auto const &attribute : node->attributeList()) {
        total += string_bytes(attribute.value);
    }
    for (auto const *child = node->firstChild(); child; child = child->next()) {
        total += node_payload_bytes(child);
    }
    return total;
}

// Approximate memory a committed XML event chain pins: the old and new attribute and content values, and the
// contents of added or deleted subtrees. A bitmap edit keeps a whole base64 image here.
std::size_t event_payload_bytes(Inkscape::XML::Event const *log)
{
    std::size_t total = 0;
    for (; log; log = log->next) {
        if (auto const *attr = dynamic_cast<Inkscape::XML::EventChgAttr const *>(log)) {
            total += string_bytes(attr->oldval) + string_bytes(attr->newval);
        } else if (auto const *content = dynamic_cast<Inkscape::XML::EventChgContent const *>(log)) {
            total += string_bytes(content->oldval) + string_bytes(content->newval);
        } else if (auto const *add = dynamic_cast<Inkscape::XML::EventAdd const *>(log)) {
            if (add->child) total += node_payload_bytes(add->child);
        } else if (auto const *del = dynamic_cast<Inkscape::XML::EventDel const *>(log)) {
            if (del->child) total += node_payload_bytes(del->child);
        }
    }
    return total;
}

typedef SimpleEvent<Event::INTERACTION> InteractionEvent;

class CommitEvent : public InteractionEvent {
public:

    CommitEvent(SPDocument *doc, const gchar *key, const gchar* event_description, const gchar *icon_name)
    : InteractionEvent("commit")
    {
        _addProperty("timestamp", timestamp());
        _addProperty("document", doc->serial());

        if (key) {
            _addProperty("merge-key", key);
        }

        if (event_description) {
            _addProperty("description", event_description);
        }

        if (icon_name) {
            _addProperty("icon-name", icon_name);
        }
    }
};

}

// 'key' is used to coalesce changes of the same type.
// 'event_description' and 'icon_name' are used in the Undo History dialog.
void Inkscape::DocumentUndo::maybeDone(SPDocument *doc,
                                       const gchar *key,
                                       ContextString event_description,
                                       Glib::ustring const &icon_name,
                                       unsigned int object_modified_tag)
{
	g_assert (doc != nullptr);
    if (doc->undo_interaction_closing) return;
    auto lifetime = interactionLifetime(doc);
    bool atomic_prepared = std::exchange(lifetime->atomic_commit_permit, false);
    auto atomic_state = atomic_prepared ? lifetime->atomic_owner.lock() : nullptr;
    if (!atomic_prepared && atomicHistoryConflict(doc)) return;
    UndoInteractionLifetime::OperationScope operation(lifetime);
    struct CommitScope {
        std::shared_ptr<UndoInteractionLifetime> lifetime;
        explicit CommitScope(std::shared_ptr<UndoInteractionLifetime> value) : lifetime(std::move(value)) {
            ++lifetime->commit_depth;
        }
        ~CommitScope() {
            --lifetime->commit_depth;
            lifetime->schedule();
        }
    } commit_scope(lifetime);
    // std::deque has no reserve(). Prepare a complete replacement deque with
    // the new slot, retaining only borrowed old Event pointers. Swap after XML
    // commit cannot allocate. A failed preparation leaves the atomic guard and
    // Redo untouched, and the null-log Event is independently owned here.
    std::unique_ptr<Inkscape::Event> prepared_event;
    std::optional<std::deque<Inkscape::Event *>> prepared_history;
    if (atomic_prepared) {
        atomicSettlementCheckpoint(AtomicSettlementStage::EventConstruction);
        prepared_event = std::make_unique<Inkscape::Event>(nullptr, event_description.c_str(), icon_name);
        atomicSettlementCheckpoint(AtomicSettlementStage::HistoryInsertion);
        prepared_history.emplace(doc->undo);
        prepared_history->push_back(prepared_event.get());
    }
    // External done owns this log, including pre-interaction partial XML. Retire
    // the token before callbacks and even if the eventual log is empty. Low-level
    // ScopedInsensitive commitUndoable handoffs deliberately do NOT seal it.
    if (!atomic_prepared) sealInteraction(doc);
    g_assert (doc->sensitive);
    if ( key && !*key ) {
        g_warning("Blank undo key specified.");
    }

    // On unless the user explicitly turned it off; an unset preference (a default install) must not mean
    // "keep every step forever", because bitmap edits keep whole images in the log.
    bool limit_undo = Inkscape::Preferences::get()->getBool("/options/undo/limit", true);
    auto undo_size = Inkscape::Preferences::get()->getInt("/options/undo/size", 200);
    auto const atomic_max_mb = atomic_prepared ?
        Inkscape::Preferences::get()->getInt("/options/undo/max-mb", 1024) : 0;

    // Undo size zero will cause crashes when changing the preference during an active document
    assert(undo_size > 0);

    if (!atomic_prepared) doc->before_commit_signal.emit();
    if (!lifetime->document) return;
    // This is only used for output to debug log file (and not for undo).
    Inkscape::Debug::EventTracker<CommitEvent> tracker(doc, key, event_description.c_str(), icon_name.c_str());

    if (!atomic_prepared) {
        doc->collectOrphans();
        doc->ensureUpToDate(object_modified_tag);
    }

    if (atomic_prepared) sealInteraction(doc);
    Inkscape::XML::Event *log = sp_repr_coalesce_log (doc->partial, sp_repr_commit_undoable (doc->rdoc));
    doc->partial = nullptr;

    if (!log) {
        sp_repr_begin_transaction (doc->rdoc);
        return;
    }

    // A no-op interaction must not destroy a valid redo branch.
    lifetime->atomic_clear_redo_permit = atomic_prepared;
    DocumentUndo::clearRedo(doc);

    bool expired = doc->undo_timer && std::chrono::steady_clock::now() - *doc->undo_timer > std::chrono::duration<double>(doc->action_expires);
    auto const log_payload = event_payload_bytes(log);
    if (key && !expired && !doc->actionkey.empty() && (doc->actionkey == key) && !doc->undo.empty()) {
        doc->undo.back()->payload_bytes += log_payload;
        doc->undo.back()->event = sp_repr_coalesce_log(doc->undo.back()->event, log);
    } else {
        Inkscape::Event *event = atomic_prepared ? prepared_event.get() :
            new Inkscape::Event(log, event_description.c_str(), icon_name);
        event->event = log;
        event->payload_bytes = log_payload;
        if (prepared_history) doc->undo.swap(*prepared_history);
        else doc->undo.push_back(event);
        prepared_event.release();
        if (atomic_state) atomic_state->published = true;
        doc->undoStackObservers.notifyUndoCommitEvent(event);
        if (!lifetime->document) return;
    }

    if (key) {
        doc->actionkey = key;
        // Action key will expire in 10 seconds by default
        doc->undo_timer = std::chrono::steady_clock::now();
        doc->action_expires = 10.0;
    } else {
        doc->actionkey.clear();
        doc->undo_timer = {};
    }

    doc->virgin = FALSE;
    doc->setModifiedSinceSave();
    sp_repr_begin_transaction (doc->rdoc);
    doc->commit_signal.emit();
    if (!lifetime->document) return;

    // Memory budget: old and new attribute values (embedded bitmaps) are kept by every step, so a count cap alone
    // cannot bound the history. Drop the oldest steps while the total is over budget, but never the step just
    // committed.
    if (limit_undo) {
        auto const max_mb = atomic_prepared ? atomic_max_mb :
            Inkscape::Preferences::get()->getInt("/options/undo/max-mb", 1024);
        if (max_mb > 0) {
            auto const budget = static_cast<std::size_t>(max_mb) * 1024u * 1024u;
            std::size_t total = 0;
            for (auto const *e : doc->undo) total += e->payload_bytes;
            while (doc->undo.size() > 1 && total > budget) {
                Inkscape::Event *e = doc->undo.front();
                total -= std::min(total, e->payload_bytes);
                doc->undoStackObservers.notifyUndoExpired(e);
                doc->undo.pop_front();
                delete e;
            }
        }
    }

    // Keeping the undo stack to a reasonable size is done when we're not maybeDone.
    // Note: Redo does not need the same controls since in theory it should never be
    // able to get larger than the undo size as it's only populated with undo items.
    if (!key) {
        // We remove undo items from the front of the stack
        while (limit_undo && (int)doc->undo.size() > undo_size) {
            Inkscape::Event *e = doc->undo.front();
            doc->undoStackObservers.notifyUndoExpired(e);
            doc->undo.pop_front();
            delete e;
        }
    }
}

void Inkscape::DocumentUndo::cancel(SPDocument *doc)
{
    if (atomicHistoryConflict(doc)) return;
    g_assert (doc != nullptr);
    g_assert (doc->sensitive);
    done(doc, ContextString("undozone"), "");
    // ensure there is something to undo (extension crash can do nothing)
    if (!doc->undo.empty() && doc->undo.back()->description == "undozone") {
        undo(doc);
        clearRedo(doc);
    }
}

std::optional<Inkscape::DocumentUndo::PendingFence>
Inkscape::DocumentUndo::detachPendingChanges(SPDocument *document)
{
    if (!document || !document->sensitive || !document->rdoc->inTransaction() ||
        document->undo_interaction_active) {
        return std::nullopt;
    }
    PendingFence fence;
    fence.pending = sp_repr_coalesce_log(document->partial, sp_repr_commit_undoable(document->rdoc));
    document->partial = nullptr;
    sp_repr_begin_transaction(document->rdoc);
    return fence;
}

void Inkscape::DocumentUndo::reattachPendingChanges(SPDocument *document, PendingFence fence)
{
    // This operation's changes stay in the open XML transaction; the caller's
    // earlier changes go back in front of them, as done() will coalesce them.
    document->partial = sp_repr_coalesce_log(fence.pending, document->partial);
}

void Inkscape::DocumentUndo::rollbackToDetachedChanges(SPDocument *document, PendingFence fence) noexcept
{
    // Same reconstruction mode as Undo (see rollbackInteraction).
    auto const was_sensitive = document->sensitive;
    auto const was_seeking = document->seeking;
    document->sensitive = false;
    document->seeking = true;
    if (document->rdoc->inTransaction()) {
        sp_repr_rollback(document->rdoc);
    }
    if (document->partial) {
        emit_replay_removals(document, document->partial, true);
        sp_repr_undo_log(document->partial);
        sp_repr_free_log(document->partial);
    }
    document->partial = fence.pending;
    sp_repr_begin_transaction(document->rdoc);
    document->ensureUpToDate();
    document->update_lpobjs();
    document->sensitive = was_sensitive;
    document->seeking = was_seeking;
}

// Member function for friend access to SPDocument privates.
void Inkscape::DocumentUndo::finish_incomplete_transaction(SPDocument &doc) {
    Inkscape::XML::Event *log=sp_repr_commit_undoable(doc.rdoc);
    if (log || doc.partial) {
        g_warning ("Incomplete undo transaction (added to next undo):");
        doc.partial = sp_repr_coalesce_log(doc.partial, log);
        if (!doc.undo.empty()) {
            Inkscape::Event* undo_stack_top = doc.undo.back();
            undo_stack_top->event = sp_repr_coalesce_log(undo_stack_top->event, doc.partial);
        } else {
            sp_repr_free_log(doc.partial);
        }
        doc.partial = nullptr;
	}
}

// Member function for friend access to SPDocument privates.
void Inkscape::DocumentUndo::perform_document_update(SPDocument &doc) {
    sp_repr_begin_transaction(doc.rdoc);
    doc.ensureUpToDate();

    Inkscape::XML::Event *update_log=sp_repr_commit_undoable(doc.rdoc);
    doc.emitReconstructionFinish();

    if (update_log != nullptr) {
        g_warning("Document was modified while being updated after undo operation");
        sp_repr_debug_print_log(update_log);

        //Coalesce the update changes with the last action performed by user
        if (!doc.undo.empty()) {
            Inkscape::Event* undo_stack_top = doc.undo.back();
            undo_stack_top->event = sp_repr_coalesce_log(undo_stack_top->event, update_log);
        } else {
            sp_repr_free_log(update_log);
        }
    }
}

gboolean Inkscape::DocumentUndo::undo(SPDocument *doc)
{
    if (atomicHistoryConflict(doc)) return FALSE;
    using Inkscape::Debug::EventTracker;
    using Inkscape::Debug::SimpleEvent;

    gboolean ret;

    EventTracker<SimpleEvent<Inkscape::Debug::Event::DOCUMENT> > tracker("undo");
    g_assert (doc != nullptr);
    g_assert (doc->sensitive);
    if (doc->undo_interaction_active) {
        g_warning("Undo requested during a rollbackable pointer interaction; cancel or finish it first");
        return FALSE;
    }

    doc->sensitive = FALSE;
    doc->seeking = true;

    doc->actionkey.clear();

    finish_incomplete_transaction(*doc);
    if (! doc->undo.empty()) {
        Inkscape::Event *log = doc->undo.back();
        doc->undo.pop_back();
        emit_replay_removals(doc, log->event, true);
        sp_repr_undo_log (log->event);
        perform_document_update(*doc);
        doc->redo.push_back(log);
        doc->setModifiedSinceSave();
        doc->undoStackObservers.notifyUndoEvent(log);
        ret = TRUE;
    } else {
	    ret = FALSE;
    }

    sp_repr_begin_transaction (doc->rdoc);
    doc->update_lpobjs();
    doc->sensitive = TRUE;
    doc->seeking = false;
    return ret;
}

gboolean Inkscape::DocumentUndo::redo(SPDocument *doc)
{
    if (atomicHistoryConflict(doc)) return FALSE;
	using Inkscape::Debug::EventTracker;
	using Inkscape::Debug::SimpleEvent;

	gboolean ret;

	EventTracker<SimpleEvent<Inkscape::Debug::Event::DOCUMENT> > tracker("redo");

    g_assert (doc != nullptr);
    g_assert (doc->sensitive);
    if (doc->undo_interaction_active) {
        g_warning("Redo requested during a rollbackable pointer interaction; cancel or finish it first");
        return FALSE;
    }
    doc->sensitive = FALSE;
    doc->seeking = true;
	doc->actionkey.clear();

    finish_incomplete_transaction(*doc);
    if (! doc->redo.empty()) {
        Inkscape::Event *log = doc->redo.back();
		doc->redo.pop_back();
		emit_replay_removals(doc, log->event, false);
		sp_repr_replay_log (log->event);
        doc->undo.push_back(log);
        perform_document_update(*doc);

        doc->setModifiedSinceSave();
        doc->undoStackObservers.notifyRedoEvent(log);
		ret = TRUE;
	} else {
		ret = FALSE;
	}

	sp_repr_begin_transaction (doc->rdoc);
    doc->update_lpobjs();
	doc->sensitive = TRUE;
    doc->seeking = false;
	if (ret) {
        doc->emitReconstructionFinish();
    }
	return ret;
}

void const *Inkscape::DocumentUndo::undoStackMark(SPDocument *doc)
{
    if (doc) {
        resetKey(doc);
    }
    return (doc && !doc->undo.empty()) ? static_cast<void const *>(doc->undo.back()) : nullptr;
}

bool Inkscape::DocumentUndo::undoSinceMark(SPDocument *doc, void const *mark)
{
    void const *const top = !doc->undo.empty() ? static_cast<void const *>(doc->undo.back()) : nullptr;
    if (top != mark) {
        return undo(doc);
    }
    // No step was pushed by the interaction: the previous step belongs to something else. Roll back only what is
    // still uncommitted (cancel() is a no-op when nothing is pending).
    cancel(doc);
    return false;
}

void Inkscape::DocumentUndo::clearUndo(SPDocument *doc)
{
    if (atomicHistoryConflict(doc)) return;
    if (! doc->undo.empty())
        doc->undoStackObservers.notifyClearUndoEvent();
    while (! doc->undo.empty()) {
        Inkscape::Event *e = doc->undo.back();
        doc->undo.pop_back();
        delete e;
    }
}

void Inkscape::DocumentUndo::clearRedo(SPDocument *doc)
{
    auto lifetime = doc ? doc->undo_interaction_lifetime : nullptr;
    if (!(lifetime && std::exchange(lifetime->atomic_clear_redo_permit, false)) &&
        atomicHistoryConflict(doc)) return;
        if (!doc->redo.empty())
                doc->undoStackObservers.notifyClearRedoEvent();

    while (! doc->redo.empty()) {
        Inkscape::Event *e = doc->redo.back();
        doc->redo.pop_back();
        delete e;
    }
}

/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0)(case-label . +))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4 :
