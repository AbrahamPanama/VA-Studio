// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Explode Bitmap: atomic Explode and alpha-only publication (EB5-publish).
 *
 * Copyright 2026 VA Studio authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "bitmap-explode-chemistry.h"
#include <algorithm>
#include <cstring>
#include <charconv>
#include <cmath>
#include <limits>
#include <2geom/svg-path-parser.h>
#include "colors/color.h"
#include <stdexcept>
#include <thread>
#include <unordered_set>
#include <utility>
#include <sigc++/scoped_connection.h>
#include "desktop.h"
#include "ui/explode-bitmap-context.h"
#include "display/cairo-utils.h"
#include "document.h"
#include "event-log.h"
#include "object/sp-clippath.h"
#include "object/sp-filter.h"
#include "object/sp-paint-server.h"
#include "object/sp-image.h"
#include "object/sp-path.h"
#include "object/sp-item-group.h"
#include "object/sp-root.h"
#include "selection.h"
#include "style.h"
#include "ui/explode-bitmap-undo.h"
#include "ui/explode-bitmap-publication.h"
#include "xml/document.h"
#include "xml/href-attribute-helper.h"
#include "xml/node.h"
#include "xml/repr.h"

namespace Inkscape::Bitmap {
namespace {
auto const mainThread = std::this_thread::get_id();
std::uintptr_t identity(void const *p) { return reinterpret_cast<std::uintptr_t>(p); }
std::string xml(XML::Node *n) {
    return sp_repr_write_buf(n, 0, false, Glib::QueryQuark(0u), 0, 0).raw();
}
unsigned drawingKey(SPDesktop &d) { return d.dkey; }
unsigned drawingKey(DocumentPublicationContext &c) { return c.drawingKey(); }
std::uintptr_t ownerIdentity(SPDesktop &d) { return identity(&d); }
std::uintptr_t ownerIdentity(DocumentPublicationContext &c) { return c.identity(); }
bool consumePairedPublicationAdmission(DocumentPublicationContext &, Prepared const &, Ticket) { return false; }
OperationIdentity bakePrepared(Prepared const &p, std::vector<LogicalImageIdentity> const &results) {
    return p.requestRecipe ? prepareBaked(p.session.image, results, *p.requestRecipe) : prepareBaked(p.session.image, results);
}
struct Anchor {
    XML::Node *node;
    explicit Anchor(XML::Node *n) : node(n) { GC::anchor(node); }
    ~Anchor() { GC::release(node); }
};
struct Staged {
    std::shared_ptr<XML::Node> node;
    std::string id, bytes;
    XML::Node *image = nullptr;
    XML::Node *path = nullptr;
    Geom::PathVector curve;
    double strokeWidth = 0;
};
void stageSelectionAfter(SPDesktop &, SPImage const &) {}
void stageSelectionAfter(SPDesktop &, std::vector<Staged> const &) {}
void stageSelectionAfter(DocumentPublicationContext &c, SPImage const &image) {
    if (!image.getId()) throw std::runtime_error("Missing selected bitmap ID");
    c.stageSelectionAfter({image.getId()});
}
void stageSelectionAfter(DocumentPublicationContext &c, std::vector<Staged> const &staged) {
    std::vector<std::string> ids; ids.reserve(staged.size());
    for (auto const &s : staged) {
        auto id = s.node->attribute("id");
        if (!id) throw std::runtime_error("Missing staged selection ID");
        ids.emplace_back(id);
    }
    c.stageSelectionAfter(std::move(ids));
}
void checkpoint(Prepared const &p, PublishStage s, unsigned i = 0) {
    if (p.hooks.checkpoint) p.hooks.checkpoint(s, i, p.hooks.data);
}
Outcome failed(char const *s) { return {Status::failed, s}; }
CliBitmapOutcome rolledBackFailure(char const *s) {
    CliBitmapOutcome outcome(Status::failed, s, CliBitmapReason::PublicationFailed);
    outcome.rolledBack = true; return outcome;
}
std::string number(double value) {
    if (!std::isfinite(value)) throw std::runtime_error("Nonfinite contour coordinate");
    char buffer[64];
    auto result = std::to_chars(buffer, buffer + sizeof(buffer), value,
                                std::chars_format::general, std::numeric_limits<double>::max_digits10);
    if (result.ec != std::errc{}) throw std::runtime_error("Contour number overflow");
    return {buffer, result.ptr};
}
// IDs are the zero-based Partition order shared by encode() and fitContours().
// Validate all ranges before serializing or touching the live document.
bool validContours(FittedContourSet const &f, unsigned count) {
    if (f.pieceCount != count || !f.pieces() || (f.ringCount && !f.rings()) ||
        (f.segmentCount && !f.segments()) || f.serializedBytes > 16 * MiB || f.anchorCount > 200000 || f.segmentCount > 200000 || f.ringCount > 50000)
        return false;
    unsigned ring = 0, segment = 0;
    for (unsigned i = 0; i < count; ++i) {
        auto const &p = f.pieces()[i];
        if (p.piece != i || p.ringBegin != ring || p.ringEnd < ring || p.ringEnd > f.ringCount ||
            p.noContour != (p.ringBegin == p.ringEnd)) return false;
        for (; ring < p.ringEnd; ++ring) {
            auto const &r = f.rings()[ring];
            if (r.begin != segment || r.end <= segment || r.end > f.segmentCount) return false;
            segment = r.end;
        }
    }
    return ring == f.ringCount && segment == f.segmentCount;
}
std::string contourData(FittedContourSet const &f, unsigned begin, unsigned end) {
    std::string d;
    auto point = [&](ContourPoint p) { d += number(p.x) + " " + number(p.y) + " "; };
    for (auto i = begin; i < end; ++i) {
        auto const &r = f.rings()[i];
        d += "M "; point(r.start);
        for (auto j = r.begin; j < r.end; ++j) {
            auto const &segment = f.segments()[j];
            d += segment.cubic ? "C " : "L ";
            if (segment.cubic) { point(segment.c1); point(segment.c2); }
            point(segment.end);
        }
        d += "Z ";
        if (d.size() > 16 * MiB) throw std::runtime_error("Contour serialization limit");
    }
    return d;
}
// Outcome diagnostics are borrowed, valid until the next contour publication on this thread.
Outcome contourSuccess(bool only, unsigned absent) {
    thread_local std::string diagnostic;
    diagnostic = std::string(only ? "Created contour. " : "Exploded bitmap with contours. ") +
                 std::to_string(absent) + " pieces without a contour.";
    return {Status::changed, diagnostic.c_str()};
}

}

std::string serializeContours(FittedContourSet const &f, unsigned begin, unsigned end) {
    if (begin > end || end > f.ringCount) throw std::out_of_range("Contour ring range");
    return contourData(f, begin, end);
}

Result<PreparedAlpha> prepareAlpha(FinalGrid const &grid, Budget &budget, Stop stop, EncodeOptions options) noexcept try {
    Result<PreparedAlpha> result;
    if (std::this_thread::get_id() == mainThread) {
        result.outcome = {Status::unavailable, "Prepare adjustment on a worker."}; return result;
    }
    auto encoded = encodeWholeGrid(grid, budget, stop, options);
    result.outcome = encoded.outcome;
    if (encoded.ok()) {
        result.value.image = std::move(encoded.value);
    }
    return result;
} catch (...) { Result<PreparedAlpha> result; result.outcome = failed("Adjustment preparation failed."); return result; }

template <typename Owner>
static CliBitmapOutcome publishAlphaNative(Owner &desktop, PreparedAlpha const &alpha, Ticket ticket, Prepared const *request = nullptr) try {
    auto const &p = request ? *request : alpha.publication;
    if (std::this_thread::get_id() != mainThread || !p.activation || !p.budget || alpha.image.count() != 1)
        return {Status::unavailable, "No prepared adjustment is available."};
    auto confirmation = p.activation->confirm(ticket, p.dependencies);
    if (!confirmation.ok()) return {confirmation, CliBitmapStage::Publish, CliBitmapReason::StaleCapture};
    auto doc = desktop.getDocument();
    if (!doc || identity(doc) != p.target.document || ownerIdentity(desktop) != p.target.desktop ||
        !valid(p.target, *doc) || !valid(p.session) || p.session.target.bitmap != p.target.bitmap ||
        p.target.mode != TargetMode::SingleBitmap || p.target.supportability != Supportability::Supported)
        return {Status::unavailable, "Bitmap adjustment is stale or unsupported.", CliBitmapReason::StaleCapture};
    auto recipe = p.requestRecipe ? *p.requestRecipe : query(p.session.image);
    if (!recipe.refine && !recipe.faintFloor) return {Status::unchanged, "Refine transparency is off; the bitmap is unchanged."};
    if (recipe.bypassAlpha) return {Status::unchanged, "Adjustment is already baked."};
    auto const &alphaPiece = alpha.image.piece(0);
    auto latency = admitLatency({0, 0, 0, 1, 1, 1, 0,
        std::uint64_t(alphaPiece.width) * alphaPiece.height, alpha.image.cropArea, alphaPiece.width, alphaPiece.height}, p.limits);
    if (!latency.ok()) return latency;
    if (!DocumentUndo::interactionIsQuiescent(doc) || !DocumentUndo::fileOperationFreshReady(doc))
        return {Status::unavailable, "Finish the current document operation before Apply.", CliBitmapReason::DocumentBusy};
    auto source = cast<SPImage>(reinterpret_cast<SPObject *>(p.target.bitmap));
    if (source && source->pixbuf && (source->pixbuf->width() > int(MaxExplodeSourceAxis) ||
        source->pixbuf->height() > int(MaxExplodeSourceAxis))) return {Status::unavailable, ExplodeSourceSizeMessage};
    auto const &png = alpha.image.piece(0);
    std::uint64_t hrefBytes;
    if (!checkedBase64Length(png.size, hrefBytes) || hrefBytes != alpha.image.hrefBytes || hrefBytes > maxHrefBytes ||
        alpha.image.cropArea != std::uint64_t(png.width) * png.height) return failed("Invalid prepared adjustment sizes.");
    if (!source || !source->pixbuf || !png.data || !png.size ||
        png.width != source->pixbuf->width() || png.height != source->pixbuf->height() ||
        doc->getObjectByRepr(source->getRepr()) != source || logicalImageIdentity(*source) != p.session.image)
        return failed("Prepared adjustment does not match the source bitmap.");
    auto original = source->getRepr(); Anchor anchor(original);
    auto [key, oldHref] = getHrefAttribute(*original);
    if (!oldHref) return failed("Missing source href.");
    auto parent = original->parent(), previous = original->prev();
    std::uint64_t stagingBytes, recoveryBytes, cacheBytes, payload, lower, originalCopies;
    auto originalXML = xml(original);
    if (!checkedMul(alpha.image.hrefBytes, 4, stagingBytes) ||
        !checkedMul(source->pixbuf->rowstride(), source->pixbuf->height(), recoveryBytes) ||
        !checkedMul(recoveryBytes, 2, recoveryBytes) ||
        !checkedMul(originalXML.size(), 4, originalCopies) ||
        !checkedAdd(recoveryBytes, originalCopies, recoveryBytes) ||
        !checkedMul(alpha.image.cropArea, 8, cacheBytes) ||
        !checkedAdd(originalXML.size(), alpha.image.hrefBytes, payload) ||
        !postCommitLowerBound(alpha.image.cropArea, png.size, alpha.image.hrefBytes, lower))
        return failed("Adjustment reservation overflow.");
    auto undo = preflightUndo(*doc, {false, 0, payload});
    if (!undo.admitted) return {Status::unavailable, undo.diagnostic, undo.reason == UndoRefusal::settlementUnavailable ? CliBitmapReason::MemoryAdmissionFailed : CliBitmapReason::PublicationFailed};
    auto plan = p.resources;
    if (!plan.add(Term::prepared, stagingBytes, 0, 0) || !plan.add(Term::recovery, recoveryBytes, 0, 0) ||
        !plan.add(Term::cache, cacheBytes, 0, 0) || !plan.add(Term::postCommit, lower, 0, 0) ||
        !plan.add(Term::queued, 0, 0, 0) || !addUndoTerms(plan, undo, 0, 0))
        return failed("Adjustment reservation overflow.");
    auto memory = p.probe ? sampleMemory(*p.probe) : sampleMemory();
    if (!memory.ok()) return {memory.outcome, CliBitmapStage::Publish, CliBitmapReason::MemoryAdmissionFailed};
    auto admission = admit(plan, memory.value, p.start, {p.budget->limit(), p.budget->reserved()});
    if (!admission.ok()) return {admission.outcome, CliBitmapStage::Publish, CliBitmapReason::MemoryAdmissionFailed};
    auto recheck = p.budget->recheck(memory.value); if (!recheck.ok()) return {recheck, CliBitmapStage::Publish, CliBitmapReason::MemoryAdmissionFailed};
    Budget::Token reservation;
    auto reserved = p.budget->acquire(Stage::prepared, admission.value.peak, reservation);
    if (!reserved.ok()) return {reserved, CliBitmapStage::Publish, CliBitmapReason::MemoryAdmissionFailed};
    checkpoint(p, PublishStage::Admission); checkpoint(p, PublishStage::Href);
    // Bounded throwing storage, avoiding GLib's aborting base64 allocator.
    std::string href(alpha.image.hrefBytes + 1, '\0');
    std::memcpy(href.data(), "data:image/png;base64,", 22);
    int state = 0, save = 0;
    auto size = g_base64_encode_step(png.data, png.size, false, href.data() + 22, &state, &save);
    size += g_base64_encode_close(false, href.data() + 22 + size, &state, &save);
    href.resize(22 + size);
    if (original->attribute(key) && href == original->attribute(key)) return {Status::unchanged, "Adjustment is already baked."};
    checkpoint(p, PublishStage::Node);
    auto staging = sp_repr_document_new("svg:svg");
    std::unique_ptr<XML::Document, void (*)(XML::Document *)> owner(staging, [](auto *d) { GC::release(d); });
    auto staged = original->duplicate(staging); Anchor stagedAnchor(staged); GC::release(staged);
    staged->setAttribute(key, href); auto expectedXML = xml(staged);
    ExpectedMutation event{MutationKind::Attribute}; event.node = identity(original);
    event.key = g_quark_from_string(key); event.before = original->attribute(key); event.after = href;
    checkpoint(p, PublishStage::Script);
    if (!valid(p.dependencies) || !valid(p.session)) return {Status::unavailable, "Bitmap changed during adjustment preparation.", CliBitmapReason::StaleCapture};
    stageSelectionAfter(desktop, *source);
    auto guard = DocumentUndo::beginAtomicInteraction(doc);
    if (!guard) return {Status::unavailable, "Atomic adjustment admission refused.", CliBitmapReason::DocumentBusy};
    auto mark = DocumentUndo::undoStackMark(doc);
    auto history = doc->get_event_log() ? doc->get_event_log()->getCurrEventSerial() : 0;
    auto hold = DocumentUndo::holdPublication(doc);
    OperationIdentity bake; bool settled = false;
    try {
        checkpoint(p, PublishStage::Guard);
        DependencyPublication publication(p.dependencies, *guard, {event});
        if (!publication.epoch().value) throw std::runtime_error("Invalid adjustment script");
        auto ready = [&] {
            return guard->validAtomicFor(doc) && desktop.getDocument() == doc && doc->getObjectByRepr(original) == source &&
                original->parent() == parent && original->prev() == previous && xml(original) == expectedXML &&
                !source->missing && source->pixbuf && source->get_arenaitem(drawingKey(desktop)) &&
                source->pixbuf->width() == png.width && source->pixbuf->height() == png.height &&
                source->href && href == source->href && desktop.getSelection()->singleItem() == source &&
                sessionJobIdentity(p.session.image, {}).recipeGeneration == p.session.recipeGeneration &&
                p.activation->check(p.dependencies, DependencyCheck::Settlement, publication.epoch()).ok();
        };
        if (!valid(p.dependencies, publication.epoch()) || !guard->validAtomicFor(doc))
            throw std::runtime_error("Stale adjustment");
        original->setAttribute(key, href); checkpoint(p, PublishStage::Binding);
        if (!doc->ensureUpToDate()) throw std::runtime_error("Adjustment update failed");
        checkpoint(p, PublishStage::Native);
        checkpoint(p, PublishStage::Selection);
        if (!ready()) throw std::runtime_error("Adjustment dependencies changed");
        bake = bakePrepared(p, {p.session.image}); checkpoint(p, PublishStage::Bake);
        if (!bake) throw std::bad_alloc();
        checkpoint(p, PublishStage::Settlement);
        settled = guard->commitAtomically(Util::Internal::ContextString("Apply alpha adjustment"), "", [&] {
            checkpoint(p, PublishStage::Readiness); return ready();
        });
        if (!settled) throw std::runtime_error("Adjustment settlement failed");
        if (!markBaked(bake)) return {Status::changed, "Adjustment committed; session bookkeeping could not settle."};
        return {Status::changed, "Adjustment baked; a new adjustment will change the pixels again."};
    } catch (...) {
        if (settled || (doc->get_event_log() && doc->get_event_log()->getCurrEventSerial() != history) ||
            (doc->action_key().empty() && DocumentUndo::undoStackMark(doc) != mark)) {
            markBaked(bake); return {Status::changed, "Adjustment committed; a post-commit notification failed."};
        }
        try { checkpoint(p, PublishStage::Rollback); } catch (...) {}
        sigc::scoped_connection restore = doc->getRoot()->connectModified([&](SPObject *, unsigned) {
            auto image = cast<SPItem>(doc->getObjectByRepr(original));
            if (image && desktop.getSelection()->singleItem() != image) desktop.getSelection()->set(image);
        });
        guard->rollback(); restore.disconnect(); doc->ensureUpToDate();
        if (xml(original) != originalXML || doc->getObjectByRepr(original) != source ||
            original->parent() != parent || original->prev() != previous || desktop.getSelection()->singleItem() != source)
            return failed("Adjustment native recovery could not complete (L-EB-3).");
        return rolledBackFailure("Adjustment failed; the original bitmap was restored.");
    }
} catch (...) { return failed("Adjustment preparation failed before publication."); }

template <typename Owner>
static CliBitmapOutcome publishPieces(Owner &desktop, Prepared const &p, Ticket ticket, bool contourOnly) try {
    if (std::this_thread::get_id() != mainThread)
        return {Status::unavailable, "Bitmap publication is main-thread only."};
    if (!p.activation) return failed("Missing bitmap activation owner.");
    auto confirmation = p.activation->confirm(ticket, p.dependencies); // before any callback
    if (!confirmation.ok()) return {confirmation, CliBitmapStage::Publish, CliBitmapReason::StaleCapture};
    auto doc = desktop.getDocument();
    if (!doc || identity(doc) != p.target.document || ownerIdentity(desktop) != p.target.desktop ||
        !valid(p.target, *doc) || !valid(p.session) || p.session.target.bitmap != p.target.bitmap ||
        p.target.mode != TargetMode::SingleBitmap || p.target.supportability != Supportability::Supported)
        return {Status::unavailable, "Bitmap preview is stale or unsupported.", CliBitmapReason::StaleCapture};
    if ((!contourOnly && !p.pieces) || !p.grid || !p.budget || (contourOnly && !p.contours)) return failed("Incomplete prepared bitmap result.");
    std::optional<Colors::Color> contourColor;
    auto count = contourOnly ? 1u : p.pieces->count();
    unsigned noContours = 0;
    if (p.contours) {
        if (!validContours(*p.contours, contourOnly ? p.contours->pieceCount : count))
            return failed("Contour piece order or IDs do not match encoded pieces.");
        for (unsigned i = 0; i < p.contours->pieceCount; ++i) noContours += p.contours->pieces()[i].noContour;
        if (contourOnly && !p.contours->ringCount) return {Status::unchanged, "No contour; the bitmap is unchanged."};
        auto const &style = p.contourStyle;
        contourColor = Colors::Color::parse(style.stroke);
        if (!contourColor || style.stroke.find_first_of(";{}") != std::string::npos ||
            !std::isfinite(style.strokeWidthMm) || style.strokeWidthMm <= 0)
            return failed("Invalid contour cut-line style.");
    }
    if (!count) return {Status::unchanged, "No pieces; the bitmap is unchanged."};
    if (count > 20000 || !p.grid->view().validate().ok()) return failed("Invalid prepared bitmap grid.");
    auto source = cast<SPImage>(reinterpret_cast<SPObject *>(p.target.bitmap));
    if (!source || !source->pixbuf) return failed("Bitmap binding changed.");
    if (source->pixbuf->width() > int(MaxExplodeSourceAxis) || source->pixbuf->height() > int(MaxExplodeSourceAxis))
        return {Status::unavailable, ExplodeSourceSizeMessage};
    auto pixels = std::max(std::uint64_t(p.grid->width) * p.grid->height,
                           std::uint64_t(source->pixbuf->width()) * source->pixbuf->height());
    auto units = p.contours ? count * 3u : count;
    if (contourOnly) {
        units = 2; // group and compound path, plus every copied image descendant
        auto root = source->getRepr();
        for (auto n = root; n;) {
            if (++units > 20000) return failed("Contour-only subtree exceeds publication limit.");
            if (n->firstChild()) { n = n->firstChild(); continue; }
            while (n != root && !n->next()) n = n->parent();
            n = n == root ? nullptr : n->next();
        }
    }
    auto sourceWidth = source->pixbuf->width(), sourceHeight = source->pixbuf->height();
    auto sourceTransform = source->transform;
    auto sourceX = source->x.computed, sourceY = source->y.computed;
    auto sourceSvgWidth = source->width.computed, sourceSvgHeight = source->height.computed;
    auto cropArea = contourOnly ? std::uint64_t(source->pixbuf->width()) * source->pixbuf->height() : p.pieces->cropArea;
    // LatencyWork publication/history units are output pieces, not XML nodes.
    // Keep the 50-piece cap; account every group/path/image separately in bytes.
    auto latency = admitLatency({0, 0, 0, count, count, count, 0, pixels, cropArea, p.grid->width, p.grid->height}, p.limits);
    if (!latency.ok()) return latency;
    auto paired = consumePairedPublicationAdmission(desktop, p, ticket);
    if (!DocumentUndo::interactionIsQuiescent(doc) || (!paired && !DocumentUndo::fileOperationFreshReady(doc)))
        return {Status::unavailable, "Finish the current document operation before Explode.", CliBitmapReason::DocumentBusy};
    auto parent = source ? source->parent : nullptr;
    if (!source || !is<SPGroup>(parent) || identity(parent) != p.target.destinationParent ||
        doc->getObjectByRepr(source->getRepr()) != source) return failed("Bitmap binding changed.");
    auto original = source->getRepr(); Anchor originalAnchor(original);
    auto container = original->parent(); Anchor parentAnchor(container);
    auto predecessor = original->prev();
    auto sourceId = std::string(source->getId() ? source->getId() : "");
    auto label = std::string(original->attribute("inkscape:label") ? original->attribute("inkscape:label") : "Bitmap");
    std::vector<Staged> staged;
    std::vector<SPItem *> selection;
    std::vector<LogicalImageIdentity> results;
    std::optional<DocumentUndo::RollbackableInteraction> guard;
    std::shared_ptr<void> publicationHold;
    OperationIdentity bake;
    Budget::Token reservation;
    // Read-only even on exception: undoStackMark() would clear a restored key.
    auto historySerial = [&] { return doc->get_event_log() ? doc->get_event_log()->getCurrEventSerial() : 0; };
    auto mark = historySerial();
    void const *stackMark = nullptr;
    bool wrote = false, settled = false;
    auto rollback = [&] {
        // A recovery fault models reservation pressure, not skipping recovery.
        try { checkpoint(p, PublishStage::Rollback); } catch (...) {}
        // Selection::set() clears the merge key. Restore selection within the
        // guard's native reconstruction, BEFORE it restores saved Undo state.
        sigc::scoped_connection restoreSelection;
        if (wrote && desktop.getDocument() == doc) {
            restoreSelection = doc->getRoot()->connectModified([&](SPObject *, unsigned) {
                auto restored = cast<SPItem>(doc->getObjectByRepr(original));
                if (restored && desktop.getSelection()->singleItem() != restored) desktop.getSelection()->set(restored);
            });
        }
        if (guard && guard->active()) guard->rollback();
        restoreSelection.disconnect();
        if (wrote && desktop.getDocument() == doc) {
            doc->ensureUpToDate();
            auto restored = cast<SPItem>(doc->getObjectByRepr(original));
            if (!restored || restored->document != doc || original->parent() != container ||
                original->prev() != predecessor || (!sourceId.empty() && doc->getObjectById(sourceId) != restored) ||
                desktop.getSelection()->singleItem() != restored)
                throw std::runtime_error("Bitmap rollback binding failed");
        }
    };
    try {
        selection.reserve(count); results.reserve(count);
        // Read-only admission: account original XML/native recovery, live outputs,
        // staging/base64/XML, drawing caches, and both history branches.
        std::vector<SPObject *> refs{source->getClipObject(), source->style->getFilter(),
                                    source->style->getFillPaintServer(), source->style->getStrokePaintServer()};
        std::vector<SPObject *> orphans;
        for (auto ref : refs) if (!contourOnly && ref) {
            SPObject *collect = nullptr;
            for (auto a = ref; a; a = a->parent) {
                unsigned drops = 0;
                for (auto r : refs) for (auto b = r; b; b = b->parent) if (b == a) ++drops;
                if (a->collectionPolicy() == SPObject::ALWAYS_COLLECT && a->_total_hrefcount == drops) collect = a;
            }
            if (collect && std::find(orphans.begin(), orphans.end(), collect) == orphans.end()) orphans.push_back(collect);
        }
        // The source Remove and rollback use its ORIGINAL predecessor. Adds
        // use the last sibling that survives ALL declared resource removals.
        auto insertionPredecessor = predecessor;
        while (insertionPredecessor && std::any_of(orphans.begin(), orphans.end(), [&](auto o) {
            return o->getRepr() == insertionPredecessor;
        })) insertionPredecessor = insertionPredecessor->prev();
        auto originalXML = xml(original);
        std::uint64_t originalBytes = originalXML.size();
        auto hrefBytes = contourOnly ? originalBytes : p.pieces->hrefBytes;
        auto encodedBytes = contourOnly ? originalBytes : p.pieces->encodedBytes;
        for (auto o : orphans) if (!checkedAdd(originalBytes, xml(o->getRepr()).size(), originalBytes))
            return failed("Bitmap resource payload overflow.");
        std::uint64_t nodeBytes, stagingBytes, recoveryBytes, cacheBytes, payload, lower, originalTwice, textBytes;
        if (!checkedAdd(label.size(), sourceId.size(), textBytes) || !checkedMul(textBytes, 6, textBytes) ||
            !checkedAdd(textBytes, 4096, textBytes) || !checkedMul(count, textBytes, nodeBytes) || !checkedMul(hrefBytes, 3, stagingBytes) ||
            !checkedAdd(stagingBytes, nodeBytes, stagingBytes) ||
            !checkedMul(source->pixbuf->rowstride(), source->pixbuf->height(), recoveryBytes) ||
            !checkedMul(recoveryBytes, 2, recoveryBytes) ||
            !checkedMul(originalBytes, 2, originalTwice) ||
            !checkedAdd(recoveryBytes, originalTwice, recoveryBytes) ||
            !checkedMul(cropArea, 8, cacheBytes) ||
            !checkedAdd(originalBytes, stagingBytes, payload) ||
            !postCommitLowerBound(cropArea, encodedBytes, hrefBytes, lower))
            return failed("Bitmap publication size overflow.");
        if (p.contours) {
            // Packed binary64 SVG, staged snapshots, Undo, native curves and wrappers.
            std::uint64_t contourBytes;
            if (!checkedMul(p.contours->segmentCount, 160, contourBytes) ||
                !checkedAdd(contourBytes, std::uint64_t(p.contours->ringCount) * 60, contourBytes) ||
                !checkedAdd(contourBytes, std::uint64_t(units) * 4096, contourBytes) ||
                !checkedMul(contourBytes, 8, contourBytes) ||
                !checkedAdd(stagingBytes, contourBytes, stagingBytes) ||
                !checkedAdd(payload, contourBytes, payload) || !checkedAdd(lower, contourBytes, lower))
                return failed("Contour publication size overflow.");
        }
        auto undo = preflightUndo(*doc, {false, 0, payload});
        if (!undo.admitted) return {Status::unavailable, undo.diagnostic, undo.reason == UndoRefusal::settlementUnavailable ? CliBitmapReason::MemoryAdmissionFailed : CliBitmapReason::PublicationFailed};
        auto plan = p.resources;
        if (!plan.add(Term::prepared, stagingBytes, 0, 0) || !plan.add(Term::nodes, nodeBytes, 0, 0) ||
            !plan.add(Term::recovery, recoveryBytes, 0, 0) || !plan.add(Term::cache, cacheBytes, 0, 0) ||
            !plan.add(Term::postCommit, lower, 0, 0) || !plan.add(Term::queued, 0, 0, 0) ||
            !addUndoTerms(plan, undo, 0, 0)) return failed("Bitmap reservation plan overflow.");
        auto memory = p.probe ? sampleMemory(*p.probe) : sampleMemory();
        if (!memory.ok()) return {memory.outcome, CliBitmapStage::Publish, CliBitmapReason::MemoryAdmissionFailed};
        auto admission = admit(plan, memory.value, p.start, {p.budget->limit(), p.budget->reserved()});
        if (!admission.ok()) return {admission.outcome, CliBitmapStage::Publish, CliBitmapReason::MemoryAdmissionFailed};
        auto recheck = p.budget->recheck(memory.value); if (!recheck.ok()) return {recheck, CliBitmapStage::Publish, CliBitmapReason::MemoryAdmissionFailed};
        auto reserved = p.budget->acquire(Stage::prepared, admission.value.peak, reservation);
        if (!reserved.ok()) return {reserved, CliBitmapStage::Publish, CliBitmapReason::MemoryAdmissionFailed};
        checkpoint(p, PublishStage::Admission);
        char transform[256]; auto length = serializeGridTransform(p.grid->pixelToParent, transform, sizeof(transform));
        if (!length) return failed("Invalid shared pixel-grid transform.");
        std::string matrix(transform, length);
        // Stage attributes in a separate XML document. Detached attribute writes
        // in the live document would correctly invalidate the capture.
        auto staging = sp_repr_document_new("svg:svg");
        std::unique_ptr<XML::Document, void (*)(XML::Document *)> stagingOwner(staging, [](auto *d) { GC::release(d); });
        staged.reserve(count);
        std::unordered_set<std::string> ids;
        // XML IDs include unbound metadata/resource nodes, in addition to native bindings.
        for (auto n = doc->getReprRoot(); n;) {
            if (auto id = n->attribute("id")) if (n != original) ids.emplace(id);
            if (n->firstChild()) { n = n->firstChild(); continue; }
            while (n != doc->getReprRoot() && !n->next()) n = n->parent();
            n = n == doc->getReprRoot() ? nullptr : n->next();
        }
        for (unsigned i = 0; i < count; ++i) {
            std::string id = i == 0 ? sourceId : "";
            if (id.empty()) {
                auto prefix = sourceId.empty() ? "bitmap-piece-" : sourceId + "-piece-";
                std::uint64_t suffix = i + 1;
                do { id = prefix + std::to_string(suffix++); }
                while (ids.contains(id) || doc->getObjectById(id));
            }
            if (!ids.insert(id).second) return failed("Staged bitmap ID collision.");
            XML::Node *node = nullptr;
            std::shared_ptr<XML::Node> temp;
            if (contourOnly) {
                checkpoint(p, PublishStage::Href, i); checkpoint(p, PublishStage::Node, i);
                node = original->duplicate(staging);
                temp.reset(node, [](auto *n) { GC::release(n); });
            } else {
                auto const &piece = p.pieces->piece(i);
                if (!piece.data || !piece.size || !piece.width || !piece.height ||
                    piece.width > maxPieceAxis || piece.height > maxPieceAxis) return failed("Invalid encoded piece.");
                checkpoint(p, PublishStage::Href, i);
                auto base64 = g_base64_encode(piece.data, piece.size);
                if (!base64) throw std::bad_alloc();
                std::unique_ptr<char, decltype(&g_free)> encoded(base64, g_free);
                auto href = std::string("data:image/png;base64,") + base64;
                checkpoint(p, PublishStage::Node, i);
                node = staging->createElement("svg:image");
                temp.reset(node, [](auto *n) { GC::release(n); });
                node->setAttribute("id", id);
                node->setAttribute("inkscape:label", label + " — piece " + std::to_string(i + 1));
                node->setAttribute("x", std::to_string(piece.x)); node->setAttribute("y", std::to_string(piece.y));
                node->setAttribute("width", std::to_string(piece.width)); node->setAttribute("height", std::to_string(piece.height));
                node->setAttribute("preserveAspectRatio", "none"); node->setAttribute("transform", matrix);
                node->setAttribute("xlink:href", href);
            }
            Geom::PathVector expectedCurve;
            double expectedWidth = 0;
            if (p.contours) {
                auto uniqueId = [&](std::string prefix) {
                    auto candidate = prefix; unsigned suffix = 1;
                    while (ids.contains(candidate) || doc->getObjectById(candidate)) candidate = prefix + "-" + std::to_string(suffix++);
                    ids.insert(candidate); return candidate;
                };
                auto group = staging->createElement("svg:g");
                std::shared_ptr<XML::Node> groupOwner(group, [](auto *n) { GC::release(n); });
                group->setAttribute("id", uniqueId(id + "-group"));
                group->setAttribute("inkscape:label", contourOnly ? label : node->attribute("inkscape:label"));
                if (!contourOnly) {
                    group->setAttribute("transform", matrix);
                    node->setAttribute("transform", nullptr);
                }
                group->appendChild(node);
                auto begin = contourOnly ? 0u : p.contours->pieces()[i].ringBegin;
                auto end = contourOnly ? p.contours->ringCount : p.contours->pieces()[i].ringEnd;
                if (begin != end) {
                    auto path = staging->createElement("svg:path");
                    std::shared_ptr<XML::Node> pathOwner(path, [](auto *n) { GC::release(n); });
                    path->setAttribute("id", uniqueId(id + "-contour"));
                    path->setAttribute("inkscape:label", "Contour");
                    auto d = contourData(*p.contours, begin, end);
                    expectedCurve = Geom::parse_svg_path(d.c_str());
                    if (expectedCurve.size() != end - begin) return failed("Invalid contour path.");
                    path->setAttribute("d", d);
                    // SVG physical mm are CSS px, divided by the complete grid-to-document
                    // metric. Like the stroke writer, nonuniform scaling uses the determinant.
                    auto const &m = p.grid->pixelToDocument;
                    auto scale = std::sqrt(std::abs(m[0] * m[3] - m[1] * m[2]));
                    if (!std::isfinite(scale) || scale <= 0) return failed("Invalid contour physical metric.");
                    expectedWidth = p.contourStyle.strokeWidthMm * (96.0 / 25.4) / scale;
                    path->setAttribute("style", "fill:none;stroke:" + p.contourStyle.stroke + ";stroke-width:" +
                        number(expectedWidth) + ";fill-rule:evenodd");
                    if (contourOnly) path->setAttribute("transform", matrix);
                    group->appendChild(path);
                }
                temp = std::move(groupOwner); node = group;
            }
            auto live = node->duplicate(doc->getReprDoc());
            std::shared_ptr<XML::Node> owned(live, [](auto *n) { GC::release(n); });
            auto imageNode = p.contours ? live->firstChild() : live;
            staged.push_back({std::move(owned), std::move(id), xml(live), imageNode, p.contours ? imageNode->next() : nullptr, std::move(expectedCurve), expectedWidth});
        }
        std::vector<ExpectedMutation> script;
        ExpectedMutation remove{MutationKind::Remove}; remove.node = identity(container);
        remove.child = identity(original); remove.previous = identity(predecessor); script.push_back(remove);
        ExpectedMutation released{MutationKind::Selection}; released.node = ownerIdentity(desktop); script.push_back(released);
        // Native source release queues automatic collection in clip/filter/
        // paint order. Consume that cleanup BEFORE native settlement and bake
        // preparation, so the receipt captures the final committed XML revision.
        for (auto o : orphans) {
            ExpectedMutation e{MutationKind::Remove}; e.node = identity(o->getRepr()->parent()); e.child = identity(o->getRepr());
            auto prev = o->getRepr()->prev();
            while (prev && std::any_of(script.begin(), script.end(), [&](auto const &x) {
                return x.kind == MutationKind::Remove && x.child == identity(prev);
            })) prev = prev->prev();
            e.previous = identity(prev); script.push_back(e);
        }
        auto previous = insertionPredecessor;
        for (auto const &s : staged) {
            ExpectedMutation add{MutationKind::Add}; add.node = identity(container); add.child = identity(s.node.get());
            add.previous = identity(previous); script.push_back(add); previous = s.node.get();
        }
        ExpectedMutation selected{MutationKind::Selection}; selected.node = ownerIdentity(desktop);
        selected.stagedSelection.emplace();
        for (auto const &s : staged) selected.stagedSelection->push_back(identity(s.node.get()));
        script.push_back(std::move(selected)); checkpoint(p, PublishStage::Script);
        if (!valid(p.dependencies) || !valid(p.session)) return {Status::unavailable, "Bitmap changed during preparation.", CliBitmapReason::StaleCapture};
        stageSelectionAfter(desktop, staged);
        guard = DocumentUndo::beginAtomicInteraction(doc); if (!guard) return {Status::unavailable, "Atomic bitmap admission refused.", CliBitmapReason::DocumentBusy};
        stackMark = DocumentUndo::undoStackMark(doc); // Admission saved the key/timer for rollback.
        publicationHold = DocumentUndo::holdPublication(doc);
        checkpoint(p, PublishStage::Guard);
        {
            DependencyPublication publication(p.dependencies, *guard, std::move(script));
            if (!publication.epoch().value) throw std::runtime_error("Invalid publication script");
            auto current = [&] {
                return guard->validAtomicFor(doc) && desktop.getDocument() == doc &&
                    sessionJobIdentity(p.session.image, {}).recipeGeneration == p.session.recipeGeneration;
            };
            auto bindings = [&] {
                for (auto const &s : staged) {
                    auto object = doc->getObjectByRepr(s.node.get());
                    if (s.node->parent()) {
                        auto image = cast<SPImage>(doc->getObjectByRepr(s.image));
                        if (p.contours && (!is<SPGroup>(object) || (s.path && !is<SPPath>(doc->getObjectByRepr(s.path))))) return false;
                        if (!image || image->document != doc || doc->getObjectById(s.id) != image ||
                            s.node->parent() != container || xml(s.node.get()) != s.bytes) return false;
                    } else if (doc->getObjectById(s.id)) return false;
                }
                return current();
            };
            auto nativeBindings = [&] {
                if (!bindings()) return false;
                for (unsigned i = 0; i < count; ++i) {
                    auto image = cast<SPImage>(doc->getObjectByRepr(staged[i].image));
                    if (p.contours) {
                        auto group = cast<SPGroup>(doc->getObjectByRepr(staged[i].node.get()));
                        if (!group || !group->get_arenaitem(drawingKey(desktop))) return false;
                        if (staged[i].path) {
                            auto path = cast<SPPath>(doc->getObjectByRepr(staged[i].path));
                            if (!path || !path->get_arenaitem(drawingKey(desktop)) || !path->style->fill.isNone() ||
                                !path->style->stroke.isColor() || path->style->stroke.getColor() != *contourColor ||
                                path->style->fill_rule.computed != SP_WIND_RULE_EVENODD ||
                                !std::isfinite(path->style->stroke_width.computed) ||
                                std::abs(path->style->stroke_width.computed - staged[i].strokeWidth) >
                                    1e-13 * staged[i].strokeWidth ||
                                !path->curve() || *path->curve() != staged[i].curve) return false;
                            if (contourOnly) {
                                for (unsigned j = 0; j < 6; ++j) if (path->transform[j] != p.grid->pixelToParent[j]) return false;
                            } else if (!path->transform.isIdentity()) return false;
                        }
                    }
                    if (contourOnly) {
                        auto activeHref = getHrefAttribute(*staged[i].image).second;
                        if (!image || image->missing || !image->pixbuf || !image->get_arenaitem(drawingKey(desktop)) ||
                            !activeHref || !image->href || std::strcmp(image->href, activeHref) ||
                            xml(staged[i].image) != originalXML || image->pixbuf->width() != sourceWidth ||
                            image->pixbuf->height() != sourceHeight || image->transform != sourceTransform ||
                            image->x.computed != sourceX || image->y.computed != sourceY ||
                            image->width.computed != sourceSvgWidth || image->height.computed != sourceSvgHeight ||
                            !cast<SPItem>(doc->getObjectByRepr(staged[i].node.get()))->transform.isIdentity()) return false;
                        continue;
                    }
                    auto const &piece = p.pieces->piece(i);
                    if (!image || image->missing || !image->pixbuf || !image->get_arenaitem(drawingKey(desktop)) ||
                        image->pixbuf->width() != piece.width || image->pixbuf->height() != piece.height ||
                        image->x.computed != piece.x || image->y.computed != piece.y ||
                        image->width.computed != piece.width || image->height.computed != piece.height ||
                        !image->href || std::strcmp(image->href, staged[i].image->attribute("xlink:href"))) return false;
                    auto item = cast<SPItem>(doc->getObjectByRepr(staged[i].node.get()));
                    for (unsigned j = 0; j < 6; ++j) if (item->transform[j] != p.grid->pixelToParent[j]) return false;
                    if (p.contours && !image->transform.isIdentity()) return false;
                }
                return true;
            };
            if (!valid(p.dependencies, publication.epoch()) || !current()) throw std::runtime_error("Stale publication");
            auto revision = doc->getReprDoc()->contentRevision();
            auto oneXML = [&] {
                auto now = doc->getReprDoc()->contentRevision();
                if (!now || !revision || *now - *revision != 1) return false;
                revision = now; return current();
            };
            wrote = true; source->deleteObject(); source = nullptr;
            checkpoint(p, PublishStage::Delete);
            if (!oneXML() || !bindings()) throw std::runtime_error("Source-release ID collision");
            if (!contourOnly) doc->collectOrphans();
            auto afterCollection = doc->getReprDoc()->contentRevision();
            if (!afterCollection || *afterCollection - *revision != orphans.size() || !current())
                throw std::runtime_error("Unexpected source resource cleanup");
            revision = afterCollection; checkpoint(p, PublishStage::Resources);
            previous = insertionPredecessor;
            for (unsigned i = 0; i < count; ++i) {
                if (!current() || doc->getObjectById(staged[i].id)) throw std::runtime_error("Pre-insertion binding changed");
                container->addChild(staged[i].node.get(), previous);
                checkpoint(p, PublishStage::Insert, i);
                auto image = cast<SPImage>(doc->getObjectByRepr(staged[i].image));
                if (!oneXML() || !image || image->document != doc || doc->getObjectById(staged[i].id) != image ||
                    staged[i].node->parent() != container || staged[i].node->prev() != previous ||
                    xml(staged[i].node.get()) != staged[i].bytes) throw std::runtime_error("Inserted bitmap binding changed");
                checkpoint(p, PublishStage::Binding, i); previous = staged[i].node.get();
            }
            if (contourOnly) doc->collectOrphans(); // copied references are now bound
            if (!doc->ensureUpToDate()) throw std::runtime_error("Native bitmap update failed");
            checkpoint(p, PublishStage::Native);
            if (!nativeBindings()) throw std::runtime_error("Native binding changed");
            if (!valid(p.dependencies, publication.epoch())) throw std::runtime_error("Native dependencies changed");
            for (auto const &s : staged) selection.push_back(cast<SPItem>(doc->getObjectByRepr(s.node.get())));
            desktop.getSelection()->setList(selection); checkpoint(p, PublishStage::Selection);
            if (!nativeBindings() || !p.activation->check(p.dependencies, DependencyCheck::Settlement, publication.epoch()).ok())
                throw std::runtime_error("Piece selection changed");
            for (auto const &s : staged) results.push_back(logicalImageIdentity(*cast<SPImage>(doc->getObjectByRepr(s.image))));
            bake = contourOnly ? prepareSessionTransfer(p.session.image, results.front()) : bakePrepared(p, results);
            checkpoint(p, PublishStage::Bake);
            if (!bake) throw std::bad_alloc();
            checkpoint(p, PublishStage::Settlement);
            settled = guard->commitAtomically(Util::Internal::ContextString(contourOnly ? "Create bitmap contour" : "Explode Bitmap"), "", [&] {
                checkpoint(p, PublishStage::Readiness);
                return nativeBindings() && p.activation->check(p.dependencies, DependencyCheck::Settlement, publication.epoch()).ok();
            });
        }
        if (!settled) { rollback(); return rolledBackFailure("Explode could not settle; the bitmap was restored."); }
        if (!markBaked(bake)) return {Status::changed, "Explode committed; session bake bookkeeping could not settle."};
        if (p.contours) return contourSuccess(contourOnly, noContours);
        return {Status::changed, "Exploded bitmap. Export hints, title and description were not copied to pieces."};
    } catch (...) {
        // L-EB-3: notification exceptions after publication cannot undo a commit.
        // A native stack push can precede a failing EventLog allocation. Probe
        // its marker only after admission and when clearing the key is a no-op.
        if (settled || historySerial() != mark ||
            (guard && doc->action_key().empty() && DocumentUndo::undoStackMark(doc) != stackMark)) {
            markBaked(bake);
            return {Status::changed, "Explode committed; a post-commit notification failed."};
        }
        try { rollback(); } catch (...) { return failed("Explode failed; native recovery could not complete (L-EB-3)."); }
        return rolledBackFailure("Explode failed; the original bitmap was restored.");
    }
} catch (...) { return failed("Explode preparation failed before publication."); }

Outcome publishAlpha(SPDesktop &desktop, PreparedAlpha const &p, Ticket ticket) {
    return publishAlphaNative(desktop, p, ticket);
}
namespace {
PublicationResult contextAdmission(DocumentPublicationContext &c, Prepared const &p) {
    if (!c.ownerThread()) return {{Status::unavailable, "Owner thread required."}, PublicationRefusal::wrongThread};
    if (c.canceled()) return {{Status::canceled, "Publication canceled."}, PublicationRefusal::canceled};
    if (p.activation != c.activation()) return {{Status::unavailable, "Wrong activation owner."}, PublicationRefusal::wrongActivation};
    if (!p.requestRecipe) return {{Status::unavailable, "Explicit recipe snapshot required."}, PublicationRefusal::missingRecipe};
    auto doc = c.getDocument();
    if (!doc || p.target.desktop != c.identity() || !valid(p.target, *doc))
        return {{Status::unavailable, "Stale publication target."}, PublicationRefusal::staleTarget};
    if (!valid(p.dependencies)) return {{Status::unavailable, "Stale publication dependency."}, PublicationRefusal::staleDependency};
    if (!valid(p.session)) return {{Status::unavailable, "Stale publication session."}, PublicationRefusal::staleSession};
    return {};
}
PublicationResult contextResult(DocumentPublicationContext &c, CliBitmapOutcome outcome) {
    PublicationResult result(outcome, outcome.ok() ? PublicationRefusal::none : PublicationRefusal::nativePublication);
    result.failure = outcome.failure;
    result.rolledBack = outcome.rolledBack;
    auto ids = c.takeSelectionAfter();
    if (outcome.status == Status::changed) result.selectionAfter = std::move(ids);
    return result;
}
}
PublicationResult publishExplode(DocumentPublicationContext &c, Prepared const &p, Ticket ticket) {
    auto admission = contextAdmission(c, p); if (!admission.ok()) { if (admission.failure) admission.failure->stage = CliBitmapStage::Publish; return admission; }
    return contextResult(c, publishPieces(c, p, ticket, false));
}
PublicationResult publishContourOnly(DocumentPublicationContext &c, PreparedContourOnly const &p, Ticket ticket) {
    auto admission = contextAdmission(c, p.publication); if (!admission.ok()) { if (admission.failure) admission.failure->stage = CliBitmapStage::Publish; return admission; }
    return contextResult(c, publishPieces(c, p.publication, ticket, true));
}
PublicationResult publishAlpha(DocumentPublicationContext &c, PreparedAlpha const &p, Ticket ticket) {
    auto admission = contextAdmission(c, p.publication); if (!admission.ok()) { if (admission.failure) admission.failure->stage = CliBitmapStage::Publish; return admission; }
    return contextResult(c, publishAlphaNative(c, p, ticket));
}
PublicationResult publishAlpha(DocumentPublicationContext &c, PreparedAlpha const &alpha, Prepared const &request, Ticket ticket) {
    auto admission = contextAdmission(c, request);
    if (!admission.ok()) { if (admission.failure) admission.failure->stage = CliBitmapStage::Publish; return admission; }
    return contextResult(c, publishAlphaNative(c, alpha, ticket, &request));
}
Outcome publishExplode(SPDesktop &desktop, Prepared const &p, Ticket ticket) {
    return publishPieces(desktop, p, ticket, false);
}
Outcome publishContourOnly(SPDesktop &desktop, PreparedContourOnly const &p, Ticket ticket) {
    return publishPieces(desktop, p.publication, ticket, true);
}

} // namespace Inkscape::Bitmap
