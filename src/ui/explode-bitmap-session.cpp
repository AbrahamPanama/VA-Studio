// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright 2026 VA Studio authors; released under GNU GPL v2+.
#include "ui/explode-bitmap-session.h"
#include <optional>
#include <list>
#include <algorithm>
#include <memory>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <sigc++/scoped_connection.h>
#include "document.h"
#include "inkgc/gc-core.h"
#include "object/sp-image.h"
#include "undo-stack-observer.h"
#include "xml/node.h"
#include "xml/document.h"
namespace Inkscape::Bitmap {
namespace {
std::thread::id const mainThread = std::this_thread::get_id();
void checkThread() {
    if (std::this_thread::get_id() != mainThread) throw std::logic_error("Bitmap session is main-thread only");
}
struct Settings { unsigned threshold = 128, softness = 40, faintFloor = 5; bool bypass = false, refine = true; };
// A hidden disappearing link, even in debug-mode scanned storage, never pins XML beyond
// native Undo lifetime. A collected node cannot pass an address-reuse comparison.
struct Record {
    void **weak;
    std::uint64_t generation = 1;
    Settings settings;
    explicit Record(XML::Node *node) {
        weak = static_cast<void **>(GC::Core::malloc_atomic_uncollectable(sizeof(void *)));
        if (!weak) throw std::bad_alloc();
        *weak = reinterpret_cast<void *>(GC_HIDE_POINTER(node));
        if (GC::Core::general_register_disappearing_link(weak, GC::Core::base(node)) != GC_SUCCESS) {
            GC::Core::free(weak); throw std::bad_alloc();
        }
    }
    ~Record() { GC::Core::unregister_disappearing_link(weak); GC::Core::free(weak); }
    XML::Node *node() const {
        return static_cast<XML::Node *>(GC_call_with_alloc_lock([](void *cell) -> void * {
            auto hidden = *static_cast<void **>(cell);
            return hidden ? GC_REVEAL_POINTER(hidden) : nullptr;
        }, weak));
    }
    void set(Settings s) { settings = s; ++generation; }
};
struct Change { Record *record; Settings before, after; std::uint64_t generation; };
struct Bake { std::vector<Change> changes; bool applied = true; Event *event = nullptr; };
struct Session final : UndoStackObserver {
    SPDocument &document;
    std::uint64_t lifetime, nextImage = 0, commit = 0;
    Event *latest = nullptr;
    std::optional<std::uint64_t> committedRevision;
    std::unordered_map<std::uint64_t, std::unique_ptr<Record>> images;
    std::unordered_map<XML::Node *, std::uint64_t> nodes;
    std::list<Bake> bakes;
    sigc::scoped_connection destroyed;
    Session(SPDocument &doc, std::uint64_t id) : document(doc), lifetime(id) { doc.addUndoObserver(*this); }
    ~Session() override { document.removeUndoObserver(*this); }
    Record *find(LogicalImageIdentity id) {
        auto it = images.find(id.image);
        return id.document == lifetime && it != images.end() && it->second->node() ? it->second.get() : nullptr;
    }
    void replay(Event *event, bool applied) {
        latest = nullptr; ++commit;
        if (auto it = std::find_if(bakes.begin(), bakes.end(), [=](auto const &b) { return b.event == event; }); it != bakes.end()) {
            for (auto &c : it->changes) c.record->set(applied ? c.after : c.before);
            it->applied = applied;
        }
    }
    void notifyUndoEvent(Event *e) override { replay(e, false); }
    void notifyRedoEvent(Event *e) override { replay(e, true); }
    void notifyUndoCommitEvent(Event *e) override {
        latest = e; ++commit; committedRevision = document.getReprDoc()->contentRevision();
    }
    void notifyUndoExpired(Event *e) override {
        if (latest == e) { latest = nullptr; ++commit; }
        bakes.remove_if([=](auto const &b) { return b.event == e; });
    }
    void clear(bool applied) {
        latest = nullptr; if (applied) ++commit; // clearing Redo is part of a new native publication
        bakes.remove_if([=](auto const &b) { return b.applied == applied; });
    }
    void notifyClearUndoEvent() override { clear(true); }
    void notifyClearRedoEvent() override { clear(false); }
};
auto &sessions() {
    // Destroy-signal cleanup avoids static shutdown ordering with application/GC.
    static auto *registry = new std::unordered_map<std::uint64_t, std::unique_ptr<Session>>;
    return *registry;
}
Session *session(LogicalImageIdentity id) {
    auto it = sessions().find(id.document);
    return it == sessions().end() ? nullptr : it->second.get();
}
Session &session(SPDocument &document) {
    for (auto &[id, s] : sessions()) if (&s->document == &document) return *s;
    static std::uint64_t nextLifetime = 0;
    auto id = ++nextLifetime;
    auto s = std::make_unique<Session>(document, id);
    s->destroyed = document.connectDestroy([id] { sessions().erase(id); });
    auto &result = *s;
    sessions().emplace(id, std::move(s));
    return result;
}
Record *record(LogicalImageIdentity id) { auto s = session(id); return s ? s->find(id) : nullptr; }
}
LogicalImageIdentity logicalImageIdentity(SPImage &image) {
    checkThread();
    auto doc = image.document;
    auto node = image.getRepr();
    if (!doc || !node || doc->getObjectByRepr(node) != &image) return {};
    auto &s = session(*doc);
    if (auto it = s.nodes.find(node); it != s.nodes.end()) {
        if (s.find({s.lifetime, it->second})) return {s.lifetime, it->second};
        s.nodes.erase(it); // collected node's address has been reused
    }
    auto id = ++s.nextImage;
    auto entry = std::make_unique<Record>(node);
    s.images.emplace(id, std::move(entry));
    try { s.nodes.emplace(node, id); } catch (...) { s.images.erase(id); throw; }
    return {s.lifetime, id};
}
SessionRecipe query(LogicalImageIdentity id) {
    checkThread();
    Settings s;
    if (auto r = record(id)) s = r->settings;
    SessionRecipe result;
    result.refine = s.refine;
    result.faintFloor = s.faintFloor; result.threshold = s.threshold; result.softness = s.softness; result.bypassAlpha = s.bypass;
    return result;
}
SessionRecipe requestRecipe(LogicalImageIdentity id, SessionRecipe requested) {
    auto current = query(id);
    requested.bypassAlpha = current.bypassAlpha && current.threshold == requested.threshold &&
        current.softness == requested.softness && current.faintFloor == requested.faintFloor && current.refine == requested.refine;
    return requested;
}
void remember(LogicalImageIdentity id, Recipe recipe, bool refine) {
    checkThread();
    if (auto r = record(id)) {
        if (recipe.threshold > 255 || recipe.softness > 127 || recipe.faintFloor > 25) throw std::out_of_range("Bitmap alpha recipe range");
        r->set({recipe.threshold, recipe.softness, recipe.faintFloor, false, refine});
    }
}
void invalidateSessionJobs(LogicalImageIdentity id) {
    checkThread(); if (auto r = record(id)) ++r->generation;
}
struct PreparedBake {
    LogicalImageIdentity source;
    std::uint64_t commit, revision;
    std::list<Bake> bakes;
};
OperationIdentity prepareBaked(LogicalImageIdentity source, std::vector<LogicalImageIdentity> const &results,
                               AllocationFault *fault) {
    checkThread();
    auto s = session(source);
    auto r = s ? s->find(source) : nullptr;
    if (!r || results.empty()) return {};
    auto revision = s->document.getReprDoc()->contentRevision();
    if (!revision) return {};
    auto allocate = [=] { if (fault && fault->fail()) throw std::bad_alloc(); };
    try {
        allocate(); auto prepared = std::make_shared<PreparedBake>();
        prepared->source = source; prepared->commit = s->commit + 1;
        prepared->revision = *revision;
        allocate(); prepared->bakes.emplace_back();
        auto &bake = prepared->bakes.front();
        allocate(); bake.changes.reserve(results.size() + 1);
        std::unordered_map<Record *, std::size_t> indices;
        allocate(); indices.reserve(results.size() + 1);
        allocate(); indices.emplace(r, 0);
        bake.changes.push_back({r, r->settings, r->settings, r->generation});
        auto baked = r->settings; baked.bypass = true;
        for (auto id : results) {
            auto result = s->find(id);
            if (!result || !s->document.getObjectByRepr(result->node())) return {};
            auto found = indices.find(result);
            if (found == indices.end()) {
                allocate(); indices.emplace(result, bake.changes.size());
                bake.changes.push_back({result, result->settings, baked, result->generation});
            } else bake.changes[found->second].after = baked;
        }
        OperationIdentity op; op.prepared = std::move(prepared); return op;
    } catch (std::bad_alloc const &) { return {}; }
}
OperationIdentity prepareBaked(LogicalImageIdentity source, std::vector<LogicalImageIdentity> const &results,
    SessionRecipe const &recipe, AllocationFault *fault) {
    auto op = prepareBaked(source, results, fault);
    if (op) for (auto &change : op.prepared->bakes.front().changes) {
        if (std::any_of(results.begin(), results.end(), [&](auto id) { return record(id) == change.record; }))
            change.after = {recipe.threshold, recipe.softness, recipe.faintFloor, true, recipe.refine};
    }
    return op;
}
OperationIdentity prepareSessionTransfer(LogicalImageIdentity source, LogicalImageIdentity copy) {
    checkThread();
    try {
        auto op = prepareBaked(source, {copy});
        if (op) {
            auto settings = record(source)->settings;
            for (auto &change : op.prepared->bakes.front().changes) change.after = settings;
        }
        return op;
    } catch (std::bad_alloc const &) { return {}; }
}
bool markBaked(OperationIdentity const &op) noexcept {
    if (std::this_thread::get_id() != mainThread || !op) return false;
    auto &prepared = *op.prepared;
    auto s = session(prepared.source);
    if (!s || prepared.bakes.empty() || !s->latest || s->commit != prepared.commit ||
        !s->committedRevision || *s->committedRevision != prepared.revision ||
        prepared.revision != s->document.getReprDoc()->contentRevision()) return false;
    if (std::any_of(s->bakes.begin(), s->bakes.end(), [&](auto const &b) { return b.event == s->latest; })) return false;
    auto &bake = prepared.bakes.front();
    for (auto const &c : bake.changes) if (!c.record->node() || c.record->generation != c.generation) return false;
    bake.event = s->latest;
    for (auto &c : bake.changes) c.record->set(c.after);
    s->bakes.splice(s->bakes.end(), prepared.bakes); // same allocator: no node allocation
    return true;
}
SessionJobIdentity sessionJobIdentity(LogicalImageIdentity id, TargetSnapshot const &target) {
    checkThread();
    auto r = record(id);
    return {id, r ? r->generation : 0, target};
}
bool valid(SessionJobIdentity const &job) {
    checkThread();
    auto s = session(job.image);
    auto r = s ? s->find(job.image) : nullptr;
    if (!r || r->generation != job.recipeGeneration || !valid(job.target, s->document)) return false;
    auto image = s->document.getObjectByRepr(r->node());
    return image && reinterpret_cast<std::uintptr_t>(image) == job.target.bitmap;
}
} // namespace Inkscape::Bitmap
