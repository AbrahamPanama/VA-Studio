// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Explode Bitmap: paired conversion and native-boundary publication (EB5-boundaries).
 *
 * Copyright 2026 VA Studio authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "ui/explode-bitmap-publication.h"
#include <thread>
#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <sigc++/scoped_connection.h>
#include "bitmap-copy-outcome.h"
#include "bitmap-explode-chemistry.h"
#include "desktop.h"
#include "display/cairo-utils.h"
#include "document.h"
#include "event-log.h"
#include "object/sp-image.h"
#include "object/sp-root.h"
#include "selection.h"
#include "ui/clipboard-lease.h"
#include "ui/explode-bitmap-undo.h"
#include "xml/document.h"
#include "xml/attribute-record.h"
#include "xml/repr.h"

namespace Inkscape::Bitmap {
namespace {
auto const mainThread=std::this_thread::get_id();
struct PairAdmission { Prepared const *prepared=nullptr; Ticket ticket=0; bool armed=false; };
std::unordered_map<SPDocument *,PairAdmission *> pairs;
std::string xml(XML::Node *node) { return sp_repr_write_buf(node,0,false,Glib::QueryQuark(0u),0,0).raw(); }
// Bound serialized/XML recovery without allocating a potentially enormous string.
bool payloadBound(XML::Node *root, std::uint64_t &bytes)
{
    auto add=[&](char const *value) {
        std::uint64_t escaped=0;
        return checkedMul(value ? std::strlen(value) : 0,6,escaped) && checkedAdd(bytes,escaped,bytes);
    };
    for (auto node=root;node;) {
        if (!checkedAdd(bytes,4096,bytes) || !add(node->name()) || !add(node->content())) return false;
        for (auto const &a:node->attributeList()) if (!add(g_quark_to_string(a.key)) || !add(a.value)) return false;
        if (bytes>256*MiB) return false;
        if (node->firstChild()) { node=node->firstChild(); continue; }
        while (node!=root && !node->next()) node=node->parent();
        node=node==root ? nullptr : node->next();
    }
    return true;
}
struct PairScope {
    SPDocument *doc;
    PairAdmission admission;
    std::shared_ptr<void> publication;
    // Each atomic guard owns its interaction operation. A publication hold
    // spans both settlements without blocking admission of the second guard.
    explicit PairScope(SPDocument *d) : doc(d),
        publication(DocumentUndo::holdPublication(d)) { pairs.emplace(d,&admission); }
    ~PairScope() { pairs.erase(doc); } // native scopes schedule only after this synchronous stack unwinds
};
Outcome failed(bool converted) {
    return {Status::failed,converted ? "Conversion completed; explode failed. Undo restores the original objects."
                                    : "Conversion failed; the original objects were restored."};
}
}
bool publicationBoundaryPending(SPDocument *doc)
{
    return doc && (pairs.contains(doc) || DocumentUndo::publicationInProgress(doc));
}
bool deferPublicationBoundary(SPDocument *doc, std::function<void(SPDocument &)> action)
{
    if (!publicationBoundaryPending(doc)) return false;
    DocumentUndo::deferUntilInteractionQuiescent(doc,[action=std::move(action)](SPDocument &settled) {
        // A nested loop in final capture teardown can drain the native queue
        // while a publication scope still lives. Refuse; never requeue forever.
        if (!publicationBoundaryPending(&settled)) action(settled);
    });
    return true; // Pending but no safe native replay also means refusal.
}
bool deferPublicationBoundary(SPDesktop &desktop, std::function<void(SPDesktop &)> action)
{
    auto doc=desktop.getDocument();
    if (!publicationBoundaryPending(doc)) return false;
    auto alive=std::make_shared<UI::ClipboardLease::DestinationLease>(&desktop,doc);
    return deferPublicationBoundary(doc,[alive,view=&desktop,action=std::move(action)](SPDocument &) {
        if (alive->valid()) action(*view);
    });
}
bool consumePairedPublicationAdmission(SPDesktop &desktop, Prepared const &p, Ticket ticket)
{
    auto doc=desktop.getDocument(); auto it=pairs.find(doc);
    if (it==pairs.end()) return false;
    auto &a=*it->second;
    if (!a.armed || a.prepared!=&p || a.ticket!=ticket) return false;
    a.armed=false;
    return !DocumentUndo::interactionCloseRequested(doc) && DocumentUndo::interactionIsQuiescent(doc);
}
Outcome convertAndExplode(SPDesktop &desktop, PreparedPair const &pair, Ticket ticket) try
{
    if (std::this_thread::get_id()!=mainThread) return {Status::unavailable,"Conversion is main-thread only."};
    auto doc=desktop.getDocument(); auto const &p=pair.explode; auto const &m=pair.conversion.metadata();
    if (!doc || pairs.contains(doc) || !p.activation) return {Status::unavailable,"Conversion publication is busy or incomplete."};
    auto confirmed=p.activation->confirm(ticket,p.dependencies);
    if (!confirmed.ok()) return confirmed;
    if (!valid(m.token,*doc) || !valid(p.dependencies) || m.token.desktop!=reinterpret_cast<std::uintptr_t>(&desktop) ||
        !DocumentUndo::interactionIsQuiescent(doc) || !DocumentUndo::fileOperationFreshReady(doc))
        return {Status::unavailable,"Conversion preview is stale or the document is busy."};
    if (!p.grid || !p.pieces || !p.budget || !m.candidateIdentity || p.grid->candidateIdentity!=m.candidateIdentity)
        return {Status::unavailable,"Prepare this candidate's exact grid and pieces before conversion."};
    auto count=p.pieces->count();
    if (count<2) return {Status::unchanged,"Fewer than two pieces; the selection is unchanged."};
    if (count>20000 || !p.grid->view().validate().ok()) return {Status::unavailable,"Invalid candidate pieces."};
    // Output cardinality is independent of conversion's source/temporary nodes.
    // Capture already bounds source observation; recovery, memory and both Undo
    // entries below still account for the complete conversion plus Explode work.
    auto conversionUnits=m.token.contexts.size()+1;
    auto units=conversionUnits+count;
    for (auto const &context:m.token.contexts) {
        auto image=cast<SPImage>(reinterpret_cast<SPObject *>(context.identity));
        if (image && image->pixbuf && (image->pixbuf->width()>int(MaxExplodeSourceAxis) ||
            image->pixbuf->height()>int(MaxExplodeSourceAxis))) return {Status::unavailable,ExplodeSourceSizeMessage};
    }
    if (m.width>MaxExplodeSourceAxis || m.height>MaxExplodeSourceAxis)
        return {Status::unavailable,ExplodeSourceSizeMessage};
    auto pixels=std::uint64_t(p.grid->width)*p.grid->height;
    auto latency=admitLatency({0,0,0,count,count,count,0,pixels,p.pieces->cropArea,p.grid->width,p.grid->height},p.limits);
    if (!latency.ok()) return latency;
    std::vector<std::string> selectedIds;
    std::uint64_t originalBytes=0,conversionBytes=0,explodeBytes=0,recovery=0,lower=0;
    for (auto identity:m.token.roots) {
        auto item=reinterpret_cast<SPItem *>(identity);
        if (!payloadBound(item->getRepr(),originalBytes)) return {Status::unavailable,"Conversion recovery exceeds its bound."};
    }
    for (auto item:desktop.getSelection()->items()) selectedIds.emplace_back(item->getId() ? item->getId() : "");
    // PNG worst-case plus base64/XML/native caches. The candidate already owns
    // its render reservation; reserve recovery and both history payloads too.
    auto rgba=pair.conversion.rgba();
    if (!rgba.validate().ok() || !checkedMul(rgba.bytes,12,conversionBytes) ||
        !checkedAdd(conversionBytes,MiB+originalBytes,conversionBytes) ||
        !checkedMul(p.pieces->hrefBytes,3,explodeBytes) ||
        !checkedAdd(explodeBytes,conversionBytes+4096*count,explodeBytes) ||
        !checkedMul(originalBytes,2,recovery) || !checkedAdd(recovery,rgba.bytes*2,recovery)) return failed(false);
    auto undo=preflightUndo(*doc,{true,conversionBytes,explodeBytes});
    std::uint64_t retained=0;
    if (!undo.admitted) return {Status::unavailable,undo.diagnostic};
    if (!checkedAdd(undo.usage.undoBytes,undo.newBytes,retained) || retained>undo.budgetBytes ||
        (undo.countCap && undo.usage.undoCount+2>undo.countCap))
        return {Status::unavailable,"The conversion pair cannot retain the existing Undo history."};
    auto plan=p.resources;
    if (!postCommitLowerBound(p.pieces->cropArea,p.pieces->encodedBytes,p.pieces->hrefBytes,lower) ||
        !plan.add(Term::recovery,recovery,0,0) || !plan.add(Term::queued,0,0,0) ||
        !plan.add(Term::prepared,conversionBytes,0,0) || !plan.add(Term::postCommit,lower,0,0) ||
        !plan.add(Term::nodes,4096*units,0,0) || !addUndoTerms(plan,undo,0,0)) return failed(false);
    auto memory=p.probe ? sampleMemory(*p.probe) : sampleMemory();
    if (!memory.ok()) return memory.outcome;
    auto admitted=admit(plan,memory.value,p.start,{p.budget->limit(),p.budget->reserved()});
    if (!admitted.ok()) return admitted.outcome;
    Budget::Token reservation;
    auto reserved=p.budget->acquire(Stage::prepared,admitted.value.peak,reservation);
    if (!reserved.ok()) return reserved;
    // Allocate the second activation before either mutation. It is private to
    // this pair; the caller's already consumed activation prevents any retry.
    Prepared rebound=p; rebound.activation=std::make_shared<DependencyRequest>();
    std::vector<SPItem *> restored; restored.reserve(selectedIds.size());
    auto guard=DocumentUndo::beginAtomicInteraction(doc);
    if (!guard) return {Status::unavailable,"Atomic conversion admission refused."};
    // Keep both settlements inside one publication boundary, with each atomic
    // guard owning only its own interaction operation.
    PairScope scope(doc);
    bool converted=false;
    auto history=[&] { return doc->get_event_log()->getCurrEventSerial(); };
    auto mark=history();
    auto restore=[&] {
        sigc::scoped_connection selection=doc->getRoot()->connectModified([&](SPObject *,unsigned) {
            restored.clear();
            for (auto const &id:selectedIds) if (auto item=cast<SPItem>(doc->getObjectById(id.c_str()))) restored.push_back(item);
            desktop.getSelection()->setList(restored);
        });
        if (guard->active()) guard->rollback();
    };
    try {
        auto result=publishBitmapCopy(*desktop.getSelection(),pair.conversion,&*guard);
        if (!result.ok()) { restore(); return failed(false); }
        auto image=cast<SPImage>(desktop.getSelection()->single());
        if (!image || reinterpret_cast<std::uintptr_t>(image)!=result.image) { restore(); return failed(false); }
        // The document-wide XML revision detects every callback write; compare
        // the new image too. Recapture follows only this exact owned transition.
        auto expected=xml(image->getRepr()); auto revision=doc->getReprDoc()->contentRevision();
        auto repr=image->getRepr();
        if (!revision) { restore(); return failed(false); }
        converted=guard->commitAtomically(Util::Internal::ContextString("Convert selection to bitmap"),"",[&] {
            return guard->validAtomicFor(doc) && desktop.getDocument()==doc &&
                doc->getReprDoc()->contentRevision()==revision && xml(repr)==expected &&
                desktop.getSelection()->single()==doc->getObjectByRepr(repr);
        });
        if (!converted) { restore(); return failed(false); }
        if (doc->getReprDoc()->contentRevision()!=revision || xml(repr)!=expected ||
            desktop.getSelection()->single()!=doc->getObjectByRepr(repr)) return failed(true);
        auto target=resolve(desktop,Intent::Explode);
        if (!target.ok()) return failed(true);
        rebound.target=std::move(target.value); rebound.dependencies=capture(rebound.target);
        auto identity=logicalImageIdentity(*cast<SPImage>(doc->getObjectByRepr(repr)));
        Recipe recipe; recipe.threshold=p.grid->candidateThreshold; recipe.softness=p.grid->candidateSoftness;
        recipe.faintFloor=p.grid->candidateFaintFloor;
        remember(identity,recipe,!p.grid->candidateBypassAlpha);
        rebound.session=sessionJobIdentity(identity,rebound.target);
        if (!rebound.dependencies || !valid(rebound.dependencies)) return failed(true);
        scope.admission={&rebound,ticket,true};
        auto exploded=publishExplode(desktop,rebound,ticket);
        return exploded.status==Status::changed ? exploded : failed(true);
    } catch (...) {
        // Native settlement can publish before a notification throws.
        if (converted || history()!=mark) return failed(true);
        restore(); return failed(false);
    }
} catch (...) { return {Status::failed,"Conversion preparation failed before publication."}; }
} // namespace Inkscape::Bitmap
