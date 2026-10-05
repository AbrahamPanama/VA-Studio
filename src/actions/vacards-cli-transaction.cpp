// SPDX-License-Identifier: GPL-2.0-or-later
#include "vacards-cli-transaction.h"

#include <map>
#include <memory>
#include <stdexcept>
#include <cstring>
#include <utility>

#include "document.h"
#include "event.h"
#include "xml/event.h"
#include "xml/attribute-record.h"
#include "object/sp-item.h"
#include "selection.h"
#include "undo-stack-observer.h"
namespace Inkscape::VACardsCli {
namespace {
struct Revision : UndoStackObserver
{
    DocumentStamp stamp;
    void notifyUndoEvent(Event *) override { ++stamp.revision; }
    void notifyRedoEvent(Event *) override { ++stamp.revision; }
    void notifyUndoCommitEvent(Event *) override {}
    sigc::connection committed;
    void notifyUndoExpired(Event *) override {}
    void notifyClearUndoEvent() override {}
    void notifyClearRedoEvent() override {}
};
// Preserve the legacy pending-log ordering without exposing it to rollback of
// this edit. Transfer ownership only when native atomic history is published.
struct PendingPublication : UndoStackObserver
{
    SPDocument *document;
    DocumentUndo::PendingFence &pending;
    static std::size_t bytes(char const *text) { return text ? std::strlen(text) : 0; }
    static std::size_t node_bytes(XML::Node const *node)
    {
        if (!node) return 0;
        auto size = bytes(node->content());
        for (auto const &attr : node->attributeList()) size += bytes(attr.value);
        for (auto child = node->firstChild(); child; child = child->next()) size += node_bytes(child);
        return size;
    }
    PendingPublication(SPDocument *doc, DocumentUndo::PendingFence &fence) : document(doc), pending(fence)
    { document->addUndoObserver(*this); }
    ~PendingPublication() { document->removeUndoObserver(*this); }
    void notifyUndoCommitEvent(Event *event) override
    {
        // Same approximate payload accounting as native history; no allocations.
        for (auto log = pending.pending; log; log = log->next) {
            if (auto attr = dynamic_cast<XML::EventChgAttr *>(log))
                event->payload_bytes += bytes(attr->oldval) + bytes(attr->newval);
            else if (auto content = dynamic_cast<XML::EventChgContent *>(log))
                event->payload_bytes += bytes(content->oldval) + bytes(content->newval);
            else if (auto add = dynamic_cast<XML::EventAdd *>(log))
                event->payload_bytes += node_bytes(add->child);
            else if (auto del = dynamic_cast<XML::EventDel *>(log))
                event->payload_bytes += node_bytes(del->child);
        }
        event->event = sp_repr_coalesce_log(std::exchange(pending.pending, nullptr), event->event);
    }
    void notifyUndoEvent(Event *) override {}
    void notifyRedoEvent(Event *) override {}
    void notifyUndoExpired(Event *) override {}
    void notifyClearUndoEvent() override {}
    void notifyClearRedoEvent() override {}
};
std::map<SPDocument *, std::unique_ptr<Revision>> revisions;
std::uint64_t incarnation = 0;
} // namespace
DocumentStamp document_stamp(SPDocument *document)
{
    if (!document)
        return {};
    auto &entry = revisions[document];
    if (!entry) {
        entry = std::make_unique<Revision>();
        entry->stamp.id = "d" + std::to_string(++incarnation);
        document->addUndoObserver(*entry);
        entry->committed = document->connectCommit([tracker = entry.get()] { ++tracker->stamp.revision; });
        document->connectDestroy([document] {
            auto found = revisions.find(document);
            if (found != revisions.end()) {
                found->second->committed.disconnect();
                document->removeUndoObserver(*found->second);
                revisions.erase(found);
            }
        });
    }
    return entry->stamp;
}
EditTransaction::EditTransaction(SPDocument *document, Selection *selection,
                                 std::shared_ptr<void> const &owner_lease)
    : _owner_lease(owner_lease ? owner_lease : DocumentUndo::holdInteractionOperation(document))
    , _document(document)
    , _selection(selection)
{
    document_stamp(document);
    if (selection)
        for (auto *item : selection->items()) {
            if (item->getId())
                _selected.emplace_back(item->getId());
        }
    _pending = DocumentUndo::detachPendingChanges(document);
    if (!_pending) return;
    try {
        _interaction = DocumentUndo::beginAtomicCommandInteraction(document, _owner_lease);
    } catch (...) {
        DocumentUndo::reattachPendingChanges(document, *_pending);
        _pending.reset();
        throw;
    }
    if (!active()) {
        DocumentUndo::reattachPendingChanges(document, *_pending);
        _pending.reset();
    }
}
EditTransaction::~EditTransaction()
{
    rollback();
}
void EditTransaction::commit(Util::Internal::ContextString label, Glib::ustring const &icon)
{
    if (!active())
        return;
    PendingPublication publication(_document, *_pending);
    if (!_interaction->commitAtomically(label, icon, [] { return true; }))
        throw std::runtime_error("Atomic edit settlement failed.");
    _interaction.reset();
    _pending.reset();
}
void EditTransaction::rollback() noexcept
{
    if (!_pending)
        return;
    if (active()) _interaction->rollback();
    _interaction.reset();
    DocumentUndo::reattachPendingChanges(_document, *_pending);
    _pending.reset();
    if (_selection) {
        // Read-only refusals must not emit a synthetic clear/reselect cycle:
        // session dependency observers can invalidate captures on those signals.
        // XML rollback may already have restored the identical live selection.
        std::size_t index = 0;
        bool same = true;
        for (auto *item : _selection->items()) {
            if (index >= _selected.size() || !item->getId() || _selected[index] != item->getId() ||
                _document->getObjectById(_selected[index]) != item) {
                same = false;
                break;
            }
            ++index;
        }
        if (same && index == _selected.size()) return;
        _selection->clear();
        for (auto const &id : _selected) {
            if (auto *item = cast<SPItem>(_document->getObjectById(id)))
                _selection->add(item);
        }
    }
}
} // namespace Inkscape::VACardsCli
