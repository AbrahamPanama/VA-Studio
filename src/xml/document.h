// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 *  Interface for XML documents
 *//*
 * Authors: see git history
 *
 * Copyright (C) 2011 Authors
 * Copyright 2005 MenTaLguY <mental@rydia.net>
 * 
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef SEEN_INKSCAPE_XML_SP_REPR_DOC_H
#define SEEN_INKSCAPE_XML_SP_REPR_DOC_H

#include "xml/node.h"
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <sigc++/signal.h>

namespace Inkscape {
namespace XML {

class Event;

/**
 * @brief Interface for XML documents
 *
 * This class represents a complete document tree. You have to go through this class
 * to create new nodes. It also contains transaction support, which forms the base
 * of the undo system.
 *
 * The document is also a node. It usually contains only two child nodes - a processing
 * instruction node (PINode) containing the XML prolog, and the root node. You can get
 * the root node of the document by calling the root() method.
 *
 * The name "transaction" can be misleading, because they are not atomic. Their main feature
 * is that they provide rollback. After starting a transaction,
 * all changes made to the document are stored in an internal event log. At any time
 * after starting the transaction, you can call the rollback() method, which restores
 * the document to the state it was before starting the transaction. Calling the commit()
 * method causes the internal event log to be discarded, and you can establish a new
 * "restore point" by calling beginTransaction() again. There can be only one active
 * transaction at a time for a given document.
 */
struct Document : virtual public Node {
public:
    // Covers the entire mutating/observer call stack, including nested main loops.
    // The state outlives its document if closing occurs inside a guarded scope.
    struct MutationState {
        unsigned depth = 0;
        bool closing = false;
        sigc::signal<void()> quiescent;
    };
    class MutationScope final {
    public:
        explicit MutationScope(Document &document) : _state(document._mutation_state) { ++_state->depth; }
        ~MutationScope() noexcept {
            if (--_state->depth == 0 && !_state->closing) {
                try {
                    // Dispatch itself can allocate before any observer runs.
                    _state->quiescent.emit();
                } catch (...) {
                    // Keep subscriptions intact. Pending observers can retry at
                    // the next quiescence; no failure may escape scope cleanup.
                }
            }
        }
        MutationScope(MutationScope const &) = delete;
        MutationScope &operator=(MutationScope const &) = delete;
    private:
        std::shared_ptr<MutationState> _state;
    };
    bool mutationActive() const { return _mutation_state->depth != 0; }
    sigc::signal<void()> &signalMutationFinished() { return _mutation_state->quiescent; }
    void closeMutationFence() { _mutation_state->closing = true; }

    /**
     * @brief Monotonic owner-thread XML content generation, for change detection.
     *
     * Starts at 0 and advances before any transaction log or observer callback
     * for every successful native XML mutation (child add/remove/reorder and
     * attribute/content/element-name change), including detached nodes and
     * mutations made outside a transaction. It is not reset by commit, rollback
     * or Undo replay, and duplicated documents get independent counters. Reading
     * the value mutates nothing and it is never serialized.
     *
     * A returned value equal to a previously observed value proves there was no
     * notified XML mutation in between. A different value may include reverted
     * or detached work and does not, by itself, establish any visual difference.
     * Stamps are comparable only within one live Document's own timeline;
     * equality or inequality between independently constructed documents is not
     * content identity.
     * After uint64_t exhaustion the value becomes empty permanently (never
     * wrapping); an empty value can never authorize a clean state. This method
     * is read-only and only safe to call from the owning thread.
     *
     * @return Current generation, or empty if the counter is exhausted.
     */
    std::optional<uint64_t> contentRevision() const { return _content_revision; }

protected:
    /**
     * @brief Advance the content revision once, saturating to empty on overflow.
     *
     * Intended to be called by the document logger on each successful native
     * mutation notification, before transaction logging or observer callbacks
     * may run. Once empty the value stays empty forever.
     */
    void noteContentMutation() {
        if (!_content_revision) {
            return;
        }
        if (*_content_revision == std::numeric_limits<uint64_t>::max()) {
            _content_revision.reset();
        } else {
            ++*_content_revision;
        }
    }

public:
    /**
     * @name Document transactions
     * @{
     */
    /**
     * @brief Checks whether there is an active transaction for this document
     * @return true if there's an established transaction for this document, false otherwise
     */
    virtual bool inTransaction()=0;
    /**
     * @brief Begin a transaction and start recording changes
     *
     * By calling this method you effectively establish a resotre point.
     * You can undo all changes made to the document after this call using rollback().
     */
    virtual void beginTransaction()=0;
    /**
     * @brief Restore the state of the document prior to the transaction
     *
     * This method applies the inverses of all recorded changes in reverse order,
     * restoring the document state from before the transaction. For some implementations,
     * this function may do nothing.
     */
    virtual void rollback()=0;
    /**
     * @brief Commit a transaction and discard change data
     *
     * This method finishes the active transaction and discards the recorded changes.
     */
    virtual void commit()=0;
    /**
     * @brief Commit a transaction and store the events for later use
     *
     * This method finishes a transaction and returns an event chain
     * that describes the changes made to the document. This method may return NULL,
     * which means that the document implementation doesn't support event logging,
     * or that no changes were made.
     *
     * @return Event chain describing the changes, or NULL
     */
    virtual Event *commitUndoable()=0;
    /*@}*/

    /**
     * @name Create new nodes
     * @{
     */
    virtual Node *createElement(char const *name)=0;
    virtual Node *createTextNode(char const *content)=0;
    virtual Node *createTextNode(char const *content, bool is_CData)=0;
    virtual Node *createComment(char const *content)=0;
    virtual Node *createPI(char const *target, char const *content)=0;
    /*@}*/

    Document *duplicate(Document *doc) const override = 0;

    /**
     * @brief Get the event logger for this document
     *
     * This is an implementation detail that should not be used outside of node implementations.
     * It should be made non-public in the future.
     */
    virtual NodeObserver *logger()=0;
private:
    std::shared_ptr<MutationState> _mutation_state = std::make_shared<MutationState>();
    std::optional<uint64_t> _content_revision{0};
};

}
}

#endif
/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0)(case-label . +))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:fileencoding=utf-8:textwidth=99 :
