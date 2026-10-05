// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/explode-bitmap-dependencies.h"
#include <algorithm>
#include <cmath>
#include "object/object-set.h"
#include "util/units.h"
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <sigc++/scoped_connection.h>
#include "desktop.h"
#include "ui/explode-bitmap-context.h"
#include "display/drawing-image.h"
#include "document.h"
#include "inkgc/gc-core.h"
#include "layer-manager.h"
#include "object/sp-image.h"
#include "object/sp-root.h"
#include "object/weakptr.h"
#include "selection.h"
#include "ui/tools/destructive-bitmap-coverage.h"
#include "xml/document.h"
#include "xml/node.h"
#include "xml/node-observer.h"
#include "xml/repr.h"
namespace Inkscape::Bitmap {
namespace {
std::thread::id const mainThread = std::this_thread::get_id();
bool main() { return std::this_thread::get_id() == mainThread; }
void checkThread() { if (!main()) throw std::logic_error("Bitmap dependencies are main-thread only"); }
std::uintptr_t id(void const *p) { return reinterpret_cast<std::uintptr_t>(p); }
std::uint64_t nextLease = 0, nextEpoch = 0;
// No worker ownership/destructors or process-shutdown ordering for live observers.
auto &states() { static auto *s = new std::unordered_map<std::uint64_t, void *>; return *s; }
bool same(std::optional<std::string> const &expected, char const *value) {
    return value ? expected && *expected == value : !expected;
}
}
struct DependencyLease::State final : XML::NodeObserver {
    SPDesktop *desktop = nullptr;
    DocumentPublicationContext *context = nullptr;
    Selection *ownerSelection() const { return context ? context->getSelection() : desktop->getSelection(); }
    std::uintptr_t ownerIdentity() const { return context ? context->identity() : id(desktop); }
    SPDocument *ownerDocument() const { return context ? context->getDocument() : desktop ? desktop->getDocument() : nullptr; }
    unsigned ownerKey() const { return context ? context->drawingKey() : desktop->dkey; }
    SPDocument *document;
    XML::Node *root;
    std::uint64_t lease = ++nextLease, generation = 1, captured = 0, epoch = 0;
    TargetSnapshot target, epochTarget;
    std::vector<SPItem *> selection;
    SPWeakPtr<SPObject> parent;
    struct NativeWatch {
        SPWeakPtr<SPObject> object;
        sigc::scoped_connection modified;
    };
    std::unordered_map<SPObject *, NativeWatch> native;
    std::optional<std::uint64_t> revision; // document-wide, before observer delivery
    struct BitmapWatch {
        SPWeakPtr<SPImage> image;
        void const *source;
        ViewDependencyStamp stamp;
        std::uintptr_t repr; // comparison-only captured XML identity
        bool retired = false;
    };
    std::vector<BitmapWatch> bitmaps;
    sigc::scoped_connection destroyed, closed, replaced, selected, tool, layer;
    unsigned dkey = 0;
    std::uint64_t observationLimit = 0;
    bool attached = false, failed = false;
    DocumentUndo::RollbackableInteraction *guard = nullptr;
    std::vector<ExpectedMutation> expected;
    std::size_t consumed = 0;
    // Anchored XML only; never attach native observers to inserted pieces.
    std::vector<std::pair<std::shared_ptr<XML::Node>, std::string>> staged;
    static std::string xml(XML::Node *node) {
        return sp_repr_write_buf(node, 0, false, Glib::QueryQuark(0u), 0, 0).raw();
    }
    bool stagedIntact() const {
        for (auto const &[node, value] : staged)
            if (xml(node.get()) != value) return false;
        return true;
    }
    State(SPDesktop &d, PlatformEvidence const &e) : desktop(&d), document(d.getDocument()), root(nullptr),
        observationLimit(measuredLimits(e).observationUnits) {
        destroyed = d.connectDestroy([this](auto) { detach(); desktop = nullptr; });
        replaced = d.connectDocumentReplaced([this](auto, auto) { detach(); });
        connectSelection();
        layer = d.layerManager().connectCurrentLayerChanged([this](auto) { invalidate(); });
        tool = d.connectEventContextChanged([this](auto, auto tool) { if (!compatibleExplodeBitmapTool(tool)) invalidate(); });
        if (document) {
            root = document->getReprRoot();
            closed = document->connectDestroy([this] { detach(); });
            GC::anchor(root); root->addSubtreeObserver(*this); attached = true;
        }
    }
    State(DocumentPublicationContext &c, PlatformEvidence const &e) : context(&c),
        document(c.getDocument()), root(nullptr), observationLimit(measuredLimits(e).observationUnits) {
        connectSelection();
        if (auto d = c.desktopView()) {
            destroyed = d->connectDestroy([this](auto) { detach(); });
            replaced = d->connectDocumentReplaced([this](auto, auto) { detach(); });
            layer = d->layerManager().connectCurrentLayerChanged([this](auto) { invalidate(); });
            tool = d->connectEventContextChanged([this](auto, auto t) { if (!compatibleExplodeBitmapTool(t)) invalidate(); });
        }
        if (document) {
            root = document->getReprRoot();
            closed = document->connectDestroy([this] { detach(); });
            GC::anchor(root); root->addSubtreeObserver(*this); attached = true;
        }
    }
    void connectSelection() {
        selection = ownerSelection()->items_vector();
        selected = ownerSelection()->connectChanged([this](auto) {
            auto next = ownerSelection()->items_vector();
            bool changed = next != selection;
            selection = std::move(next);
            if (!epoch) { if (changed) ++generation; return; }
            if (!revision || document->getReprDoc()->contentRevision() != revision) { failed = true; return; }
            if (consumed >= expected.size() || expected[consumed].kind != MutationKind::Selection ||
                expected[consumed].node != ownerIdentity()) { failed = true; return; }
            try {
                auto items = ownerSelection()->items_vector();
                auto const &e = expected[consumed];
                auto const &ids = e.stagedSelection ? *e.stagedSelection : e.selection;
                if (items.size() != ids.size() || !std::equal(items.begin(), items.end(), ids.begin(),
                        [&](auto *p, auto v) {
                            if (!e.stagedSelection) return id(p) == v;
                            return p->document == document && id(p->getRepr()) == v &&
                                p->getRepr()->document() == document->getReprDoc() &&
                                document->getObjectByRepr(p->getRepr()) == p;
                        })) failed = true;
                else { ++consumed; ++epochTarget.generation; }
            } catch (...) { failed = true; }
        });
    }
    ~State() override { detach(); }
    void invalidate() { ++generation; if (epoch) failed = true; }
    void clearNative() { native.clear(); }
    void pruneRemoved(XML::Node &removed) {
        for (auto it = native.begin(); it != native.end();) {
            auto object = it->second.object.get();
            auto repr = object ? object->getRepr() : nullptr;
            while (repr && repr != &removed) repr = repr->parent();
            if (!object || repr) it = native.erase(it);
            else ++it;
        }
    }
    void detach() {
        selected.disconnect();
        invalidate(); clearNative(); bitmaps.clear(); staged.clear(); parent.reset();
        if (attached) { root->removeSubtreeObserver(*this); GC::release(root); attached = false; }
        root = nullptr; document = nullptr;
    }
    void event(MutationKind kind, void const *node, void const *child = nullptr,
               void const *prev = nullptr, void const *oldPrev = nullptr, unsigned key = 0,
               char const *before = nullptr, char const *after = nullptr, bool targetObserved = true) {
        if (!epoch) { ++generation; return; }
        if (kind == MutationKind::Native) {
            ++epochTarget.generation; // EB2 observes the same captured objects.
            return; // Consequences of XML are not a publication script.
        }
        if (!document || failed || consumed == expected.size()) { failed = true; return; }
        auto current = document->getReprDoc()->contentRevision();
        if (!revision || !current || *current <= *revision || *current - *revision != 1) { failed = true; return; }
        auto const &e = expected[consumed];
        if (e.kind != kind || e.node != id(node) || e.child != id(child) || e.previous != id(prev) ||
            e.oldPrevious != id(oldPrev) || e.key != key || !same(e.before, before) || !same(e.after, after))
            failed = true;
        else {
            ++consumed; revision = current;
            // Released native objects may already be gone. The callback's live
            // XML subtree still identifies every captured descendant, without
            // dereferencing captured addresses or crossing into its siblings.
            if (kind == MutationKind::Remove) {
                auto removed = static_cast<XML::Node const *>(child);
                for (auto repr = removed; repr;) {
                    for (auto &watch : bitmaps) if (watch.repr == id(repr)) watch.retired = true;
                    if (repr->firstChild()) { repr = repr->firstChild(); continue; }
                    while (repr != removed && !repr->next()) repr = repr->parent();
                    repr = repr == removed ? nullptr : repr->next();
                }
            }
            if (kind == MutationKind::Attribute &&
                (key == g_quark_from_static_string("href") || key == g_quark_from_static_string("xlink:href")))
                for (auto &watch : bitmaps) if (watch.repr == id(node)) watch.retired = true;
            // Reconcile only exact owned events seen by EB2's capture-time tracker.
            // Keep its incarnation, document/lifetime and pending-native validation.
            if (targetObserved) ++epochTarget.generation;
        }
    }
    bool observe(SPObject *rootObject) {
        std::vector<SPObject *> stack{rootObject};
        while (!stack.empty()) {
            auto object = stack.back(); stack.pop_back();
            auto found = native.find(object);
            if (found == native.end() || found->second.object.get() != object) {
                if (found == native.end() && native.size() >= observationLimit) return false;
                // Allocate bookkeeping BEFORE attaching either live callback.
                auto &watch = native[object];
                watch.object.reset(object);
                watch.modified = object->connectModified([this](SPObject *o, unsigned f) {
                    event(MutationKind::Native, o, nullptr, nullptr, nullptr, f);
                });
            }
            for (auto &child : object->children) {
                if (stack.size() + native.size() >= observationLimit) return false;
                stack.push_back(&child);
            }
        }
        return true;
    }
    void notifyChildAdded(XML::Node &n, XML::Node &c, XML::Node *p) override {
        event(MutationKind::Add, &n, &c, p);
    }
    void notifyChildRemoved(XML::Node &n, XML::Node &c, XML::Node *p) override { pruneRemoved(c); event(MutationKind::Remove, &n, &c, p); }
    void notifyChildOrderChanged(XML::Node &n, XML::Node &c, XML::Node *o, XML::Node *p) override { event(MutationKind::Order, &n, &c, p, o); }
    void notifyAttributeChanged(XML::Node &n, GQuark k, Util::ptr_shared a, Util::ptr_shared b) override { event(MutationKind::Attribute, &n, nullptr, nullptr, nullptr, k, a, b); }
    void notifyContentChanged(XML::Node &n, Util::ptr_shared a, Util::ptr_shared b) override { event(MutationKind::Content, &n, nullptr, nullptr, nullptr, 0, a, b); }
    void notifyElementNameChanged(XML::Node &n, GQuark a, GQuark b) override { event(MutationKind::Name, &n, nullptr, nullptr, nullptr, 0, g_quark_to_string(a), g_quark_to_string(b)); }
};
DependencyLease::DependencyLease(SPDesktop &d) : DependencyLease(d, macDependencyEvidence()) {}
DependencyLease::DependencyLease(SPDesktop &desktop, PlatformEvidence const &e) {
    checkThread();
    for (auto const &[key, value] : states())
        if (static_cast<State *>(value)->ownerIdentity() == id(&desktop))
            throw std::logic_error("A bitmap dependency lease already owns this desktop");
    _state = std::make_unique<State>(desktop, e); states().emplace(_state->lease, _state.get());
}
DependencyLease::DependencyLease(DocumentPublicationContext &c) : DependencyLease(c, macDependencyEvidence()) {}
DependencyLease::DependencyLease(DocumentPublicationContext &c, PlatformEvidence const &e) {
    checkThread();
    for (auto const &[key, value] : states())
        if (static_cast<State *>(value)->ownerIdentity() == c.identity())
            throw std::logic_error("A bitmap dependency lease already owns this context");
    _state = std::make_unique<State>(c, e); states().emplace(_state->lease, _state.get());
}
DependencyLease::~DependencyLease() { checkThread(); states().erase(_state->lease); }
void DependencyLease::invalidate() { checkThread(); _state->invalidate(); }
std::size_t DependencyLease::nativeWatchCount() const { checkThread(); return _state->native.size(); }
DependencyToken capture(TargetSnapshot const &target) try {
    if (!main() || target.supportability == Supportability::Refused) return {};
    for (auto const &[key, value] : states()) {
        auto &s = *static_cast<DependencyLease::State *>(value);
        if (!s.ownerDocument() || s.ownerIdentity() != target.desktop || !s.document || s.epoch ||
            !valid(target, *s.document)) continue;
        // EB2 validation proves all opaque identities live before constructing weak refs.
        s.clearNative(); s.bitmaps.clear(); s.parent.reset(); ++s.captured;
        s.target = target; s.dkey = s.ownerKey();
        s.revision = s.document->getReprDoc()->contentRevision();
        if (!s.revision) return {};
        if (!s.observe(s.document->getRoot())) { s.invalidate(); s.clearNative(); return {}; }
        // Contexts include selected descendants and clone source dependencies.
        // Resolve proved their identities live; native watches give weak lifetimes.
        for (auto const &context : target.contexts) {
            auto found = s.native.find(reinterpret_cast<SPObject *>(context.identity));
            if (found == s.native.end() || !found->second.object) return {};
            if (auto image = cast<SPImage>(found->second.object.get())) {
                auto stamp = image->viewDependencyStamp(s.dkey);
                if (!image->pixbuf || !stamp.available()) return {};
                s.bitmaps.push_back({SPWeakPtr<SPImage>(image), image->pixbuf.get(), stamp, id(image->getRepr())});
            }
        }
        s.parent.reset(reinterpret_cast<SPObject *>(target.destinationParent));
        return {key, s.generation, s.captured};
    }
    return {};
} catch (...) { return {}; }
bool valid(DependencyToken const &token, PublicationEpoch epoch) {
    if (!main() || !token) return false;
    auto found = states().find(token.lease); if (found == states().end()) return false;
    auto &s = *static_cast<DependencyLease::State *>(found->second);
    if (!s.document || s.ownerDocument() != s.document ||
        s.document->serial() != s.target.documentSerial || s.captured != token.capture ||
        s.generation != token.generation || s.ownerKey() != s.dkey || !s.revision ||
        s.document->getReprDoc()->contentRevision() != s.revision) return false;
    bool publishing = epoch.value && epoch.lease == token.lease && epoch.value == s.epoch &&
        !s.failed && s.guard && s.guard->validAtomicFor(s.document);
    if (epoch.value && !publishing) return false;
    for (auto const &watch : s.bitmaps) {
        if (publishing && watch.retired) continue;
        if (!watch.image || !watch.stamp.available() || watch.image->pixbuf.get() != watch.source) return false;
        auto current = watch.image->viewDependencyStamp(s.dkey);
        if (!current.available() || current != watch.stamp) return false;
    }
    if (epoch.value) {
        if (s.consumed == s.expected.size()) {
            try { if (!s.stagedIntact()) s.failed = true; }
            catch (...) { s.failed = true; }
        }
        return s.parent && epoch.lease == token.lease && epoch.value == s.epoch && !s.failed &&
            s.guard && s.guard->validAtomicFor(s.document) &&
            valid(s.epochTarget, *s.document);
    }
    return !s.epoch && s.parent && valid(s.target, *s.document);
}
DependencyPublication::DependencyPublication(DependencyToken token, DocumentUndo::RollbackableInteraction &guard,
                                            std::vector<ExpectedMutation> expected) {
    checkThread();
    if (!valid(token)) return;
    auto &s = *static_cast<DependencyLease::State *>(states().at(token.lease));
    if (!guard.validAtomicFor(s.document)) return;
    s.staged.clear();
    struct StagingCleanup {
        DependencyLease::State &state; bool keep = false;
        ~StagingCleanup() { if (!keep) state.staged.clear(); }
    } cleanup{s};
    try {
        std::size_t units = 0;
        for (auto const &e : expected) if (e.kind == MutationKind::Add) {
            // The caller owns live, fully staged Add children until construction.
            auto node = reinterpret_cast<XML::Node *>(e.child);
            if (!node || node->document() != s.document->getReprDoc()) { s.staged.clear(); return; }
            std::vector<XML::Node *> stack{node};
            while (!stack.empty()) {
                auto current = stack.back(); stack.pop_back();
                if (++units > 20000) { s.staged.clear(); return; }
                for (auto c = current->firstChild(); c; c = c->next()) {
                    if (stack.size() + units >= 20000) { s.staged.clear(); return; }
                    stack.push_back(c);
                }
            }
            GC::anchor(node);
            std::shared_ptr<XML::Node> anchor(node, [](auto *n) { GC::release(n); });
            s.staged.emplace_back(std::move(anchor), DependencyLease::State::xml(node));
        }
        auto const additions = s.staged.size();
        for (auto const &e : expected) {
            if (e.kind == MutationKind::Native) return;
            if (e.kind != MutationKind::Attribute ||
                (e.key != g_quark_from_static_string("href") && e.key != g_quark_from_static_string("xlink:href"))) continue;
            for (auto const &watch : s.bitmaps) if (watch.repr == e.node) {
                auto node = watch.image->getRepr();
                if (!same(e.before, node->attribute(g_quark_to_string(e.key))) || !e.after ||
                    !e.after->starts_with("data:image/png;base64,") ||
                    std::any_of(s.staged.begin(), s.staged.end(), [&](auto const &v) { return v.first.get() == node; })) return;
                auto staging = sp_repr_document_new("svg:svg");
                std::unique_ptr<XML::Document, void (*)(XML::Document *)> owner(staging, [](auto *d) { GC::release(d); });
                std::shared_ptr<XML::Node> copy(node->duplicate(staging), [](auto *n) { GC::release(n); });
                copy->setAttribute(g_quark_to_string(e.key), *e.after);
                GC::anchor(node);
                std::shared_ptr<XML::Node> anchor(node, [](auto *n) { GC::release(n); });
                s.staged.emplace_back(std::move(anchor), DependencyLease::State::xml(copy.get()));
            }
        }
        for (auto const &e : expected) if (e.stagedSelection) {
            if (e.kind != MutationKind::Selection || e.node != s.ownerIdentity() || !e.selection.empty() ||
                e.stagedSelection->size() > units) {
                s.staged.clear(); return;
            }
            for (auto it = e.stagedSelection->begin(); it != e.stagedSelection->end(); ++it) {
                auto identity = *it;
                if (std::find(e.stagedSelection->begin(), it, identity) != it) return;
                // Locate within live anchored roots before dereferencing any
                // caller address; XML id strings never substitute for identity.
                bool found = false;
                for (std::size_t i = 0; i < additions; ++i) {
                    auto const &root = s.staged[i].first;
                    for (auto node = root.get(); node;) {
                        if (id(node) == identity) found = true;
                        if (node->firstChild()) { node = node->firstChild(); continue; }
                        while (node != root.get() && !node->next()) node = node->parent();
                        node = node == root.get() ? nullptr : node->next();
                    }
                }
                if (!found) { s.staged.clear(); return; }
            }
        }
    } catch (...) { s.staged.clear(); return; }
    s.expected = std::move(expected); s.consumed = 0; s.epochTarget = s.target; s.failed = false; s.guard = &guard;
    s.epoch = ++nextEpoch; _epoch = {token.lease, s.epoch};
    cleanup.keep = true;
}
DependencyPublication::~DependencyPublication() {
    checkThread(); if (!_epoch.value) return;
    auto found = states().find(_epoch.lease); if (found == states().end()) return;
    auto &s = *static_cast<DependencyLease::State *>(found->second);
    if (s.epoch == _epoch.value) { s.epoch = 0; s.guard = nullptr; s.expected.clear(); s.staged.clear(); s.invalidate(); }
}
Outcome DependencyRequest::check(DependencyToken token, DependencyCheck point, PublicationEpoch epoch) {
    checkThread();
    if (_paused) return {Status::unavailable, "Bitmap changed three times. Preview paused; retry explicitly."};
    bool current = valid(token, epoch);
    if (current && point == DependencyCheck::Settlement && epoch.value) {
        auto &s = *static_cast<DependencyLease::State *>(states().at(token.lease));
        current = s.consumed == s.expected.size();
    }
    if (current) return {};
    if (token && std::none_of(_invalidated.begin(), _invalidated.end(), [&](auto const &seen) {
            return seen.lease == token.lease && seen.capture == token.capture && seen.generation == token.generation;
        })) {
        _invalidated.push_back(token); _paused = _invalidated.size() >= 3;
    }
    if (point == DependencyCheck::Confirmation)
        return {Status::incompatible, "Confirmation is stale: the bitmap, dependencies or preview changed. Preview and confirm again."};
    return {Status::unavailable, _paused ? "Bitmap changed three times. Preview paused; retry explicitly." :
        "Bitmap dependencies changed. Recalculate the preview."};
}
Outcome DependencyRequest::confirm(Ticket ticket, DependencyToken token) {
    checkThread();
    if (!ticket || ticket <= _confirmed) return {Status::incompatible, "Confirmation ticket was already used or superseded."};
    if (_activation.lease == token.lease && _activation.capture == token.capture &&
        _activation.generation == token.generation && token)
        return {Status::incompatible, "This prepared result was already confirmed. Prepare a new preview."};
    _confirmed = ticket; _activation = token; // latch result as well as ticket before validation
    return check(token, DependencyCheck::Confirmation);
}
namespace {
// Full raw records and source/binary identities: EB7-calibration/promote-integrated-4mp.
// Active decoder close: EB7-calibration/promote-active-close. Capture object cap:
// EB7-calibration/promote-round1-capture (three repetitions of each full call).
constexpr PlatformEvidence MacEb7PromotionEvidence{
    EvidencePlatform::Mac, 1757, UI::Tools::DestructiveBitmapCoverage::ClipUnitNanoseconds,
    0, 0, 0, 0, 10000, 16'000'000, 32'000'000, 246083, 687625,
    {3, 100, 4000000, 7950932,
     {6074833, 411334, 138000, 402125, 2726125},
     {27864833, 10144042, 119709, 9621125, 20878958},
     4745208, 14903292, 249137917, 80149291, 19792, 45712500, 246083, 0, 0, true, 20000}
};
// Retained optional Windows diagnostic record; not an admission gate.
constexpr PlatformEvidence WindowsEb7UnqualifiedEvidence{EvidencePlatform::Windows,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, {}};
}
AdmissionLimits measuredLimits(PlatformEvidence const &e) {
    AdmissionLimits l;
    if (e.platform != EvidencePlatform::Mac && e.platform != EvidencePlatform::Windows) { l.sourceAxis = 0; return l; }
    l.observationUnits = UnlimitedExplodeObservationUnits;
    l.clipUnits = UI::Tools::DestructiveBitmapCoverage::ClipMainUnitLimit;
    l.renderUnits = std::uint64_t(MaxExplodeSourceAxis) * MaxExplodeSourceAxis;
    l.outlineUnits = 2'000'000;
    l.publicationUnits = l.rollbackUnits = l.historyUnits = MaxExplodePieces;
    l.sourceAxis = MaxExplodeSourceAxis;
    l.stopPixels = 1'000'000;
    l.stopBytes = 64 * 1024;
    l.stopNs = 10'000'000;
    l.workerQualified = true;
    return l;
}
PlatformEvidence macExplodePromotionEvidence() { return MacEb7PromotionEvidence; }
PlatformEvidence windowsExplodePromotionEvidence() { return WindowsEb7UnqualifiedEvidence; }
PlatformEvidence diagnosticDependencyEvidence() {
    return PlatformEvidence{EvidencePlatform::Mac};
}
PlatformEvidence macDependencyEvidence() {
#ifdef __APPLE__
    return {EvidencePlatform::Mac};
#elif defined(_WIN32)
    return {EvidencePlatform::Windows};
#else
    return {};
#endif
}
ExplodeSourceSize fitExplodeSourceSize(unsigned width, unsigned height) {
    auto longest = std::max(width, height);
    if (longest <= MaxExplodeSourceAxis) return {width, height};
    return {std::max(width ? 1u : 0u, unsigned(std::uint64_t(width) * MaxExplodeSourceAxis / longest)),
            std::max(height ? 1u : 0u, unsigned(std::uint64_t(height) * MaxExplodeSourceAxis / longest))};
}
int maxResizeDpi(Geom::Rect const &bounds) {
    if (!std::isfinite(bounds.width()) || !std::isfinite(bounds.height()) ||
        bounds.width() <= 0 || bounds.height() <= 0) return 0;
    for (int dpi = BitmapCopyOptions::max_dpi; dpi > 0; --dpi) {
        // SingleImageResize retains the exact extent at every DPI, including 96.
        double scale = Util::Quantity::convert(double(dpi), "px", "in");
        double width = std::ceil(scale * bounds.width()), height = std::ceil(scale * bounds.height());
        if (width >= 1 && height >= 1 && width <= MaxExplodeSourceAxis && height <= MaxExplodeSourceAxis) return dpi;
    }
    return 0;
}
std::string explodeSourceSizeMessage(std::uint64_t width, std::uint64_t height) {
    return Glib::ustring::compose(ExplodeSourceSizeMessage,width,height,MaxExplodeSourceAxis).raw();
}
Outcome admitLatency(LatencyWork const &w, AdmissionLimits const &l) {
    if (w.publication && (!w.rollback || !w.history))
        return {Status::unavailable,"Publication requires reserved rollback and resulting select/Undo/Redo work."};
    auto const cap = l.extremeStressForTest ? 300u : MaxExplodePieces;
    if (w.publication > cap || w.rollback > cap || w.history > cap ||
        w.observation > l.observationUnits || w.clip > l.clipUnits || w.render > l.renderUnits ||
        w.publication > l.publicationUnits || w.rollback > l.rollbackUnits || w.history > l.historyUnits ||
        w.outlines > l.outlineUnits)
        return {Status::unavailable,"Explode Bitmap fixed operation limit exceeded or platform unsupported."};
    // Publishers also check every original bitmap, before native mutation.
    if (w.width > MaxExplodeSourceAxis || w.height > MaxExplodeSourceAxis ||
        w.width > l.sourceAxis || w.height > l.sourceAxis)
        return {Status::unavailable,ExplodeSourceSizeMessage};
    if (w.piecePixels && (!w.sourcePixels || w.piecePixels > 64'000'000 || w.piecePixels / 2 > w.sourcePixels ||
                         (w.piecePixels % 2 && w.piecePixels / 2 == w.sourcePixels)))
        return {Status::unavailable,"Explode Bitmap pieces exceed the crop pixel limit."};
    return {};
}
StopCadence::StopCadence(AdmissionLimits l, JobNow now) : _limits(l), _now(now), _last(now()) {
    _limits.stopNs = std::min<std::uint64_t>(l.stopNs, 10'000'000);
    _limits.stopPixels = std::min<std::uint64_t>(l.stopPixels, 1'000'000);
    _limits.stopBytes = std::min<std::uint64_t>(l.stopBytes, 65536);
}
void StopCadence::checked() { _pixels = _bytes = 0; _last = _now(); }
Outcome StopCadence::advance(std::uint64_t pixels, std::uint64_t bytes, Stop stop) {
    if (stop.requested()) return {Status::canceled, "Bitmap work canceled."};
    auto now = _now();
    if (!_limits.workerQualified || now < _last || now - _last > std::chrono::nanoseconds(_limits.stopNs) ||
        !checkedAdd(_pixels, pixels, _pixels) || !checkedAdd(_bytes, bytes, _bytes) ||
        _pixels > _limits.stopPixels || _bytes > _limits.stopBytes)
        return {Status::unavailable, "Worker stop cadence exceeds the qualified time/pixel/input bounds."};
    return {};
}
} // namespace Inkscape::Bitmap
