// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_IO_PUBLICATION_WORKER_H
#define INKSCAPE_IO_PUBLICATION_WORKER_H

#include "extension/internal/svg-publication.h"

#include <glib.h>
#include <atomic>
#include <functional>
#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>

namespace Inkscape::IO {

inline bool async_save_enabled()
{
#ifdef __APPLE__
    return g_strcmp0(g_getenv("VACARDS_ASYNC_SAVE"), "1") == 0;
#else
    return false;
#endif
}

// Owner-thread API. The key is an identity token, never dereferenced here.
// A caller must wait before destroying its document. If start() throws, the
// caller must complete or cancel its file-operation lease.
class PublicationWorkerRegistry {
public:
    using Job = Extension::Internal::PublicationJob;
    using Result = Extension::Internal::PublicationResult;
    using Completion = std::function<void(Result)>;
    using Publisher = std::function<Result(Job)>;
    using DeliveryObserver = std::function<void(void const *)>;
    enum class StartResult { Started, Busy };

#if defined(__APPLE__) || defined(_WIN32)
    explicit PublicationWorkerRegistry(Publisher publisher = Extension::Internal::publish)
#else
    explicit PublicationWorkerRegistry(Publisher publisher = [](Job) {
        Result result;
        result.outcome = Extension::Internal::PublicationOutcome::Unsupported;
        return result;
    })
#endif
        : _publisher(std::move(publisher)), _owner(std::this_thread::get_id()) {}
    void setDeliveryObserver(DeliveryObserver observer) { _delivery_observer = std::move(observer); }
#ifdef VACARDS_FILE_IO_TEST_HOOKS
    static void fail_next_thread_start_for_testing() { _fail_next_thread_start = true; }
    static void fail_next_holder_allocation_for_testing() { _fail_next_holder_allocation = true; }
    static void fail_next_result_store_for_testing() { _fail_next_result_store = true; }
#endif
    PublicationWorkerRegistry(PublicationWorkerRegistry const &) = delete;
    PublicationWorkerRegistry &operator=(PublicationWorkerRegistry const &) = delete;

    ~PublicationWorkerRegistry()
    {
        g_assert(std::this_thread::get_id() == _owner);
        // Shutdown suppresses completions, including already attached idle sources.
        _closing = true;
        decltype(_slots) slots;
        slots.swap(_slots);
        for (auto &[key, slot] : slots) {
            if (slot->thread.joinable()) slot->thread.join();
        }
    }

    StartResult start(void const *doc_key, Job &&job, Completion completion)
    {
        g_assert(std::this_thread::get_id() == _owner);
        g_assert(!_closing);
        // A deferred-serialization snapshot must stay with its initiating thread
        // (take_publication_job moves it out); the worker destroys its job.
        // G_DISABLE_ASSERT builds drop g_assert, so refuse explicitly; callers
        // treat a throwing start() as a start failure.
        if (job.snapshot_owner)
            throw std::logic_error("publication job still owns its serialization snapshot");
        auto it = _slots.find(doc_key);
        if (it != _slots.end()) {
            if (it->second->state.load(std::memory_order_acquire) != State::Done)
                return StartResult::Busy;
            if (it->second->thread.joinable()) it->second->thread.join();
            _slots.erase(it);
        }

        auto slot = std::make_shared<Slot>();
        slot->owner = _owner;
        slot->context = g_main_context_ref(g_main_context_default());
        slot->completion = std::move(completion);
        slot->delivery_observer = [observer = _delivery_observer, doc_key] {
            if (observer) observer(doc_key);
        };
        slot->publisher = _publisher;
#ifdef VACARDS_FILE_IO_TEST_HOOKS
        if (std::exchange(_fail_next_holder_allocation, false)) throw std::bad_alloc();
#endif
        auto hold = std::make_unique<std::weak_ptr<Slot>>(slot);
        _slots.emplace(doc_key, slot);
        try {
#ifdef VACARDS_FILE_IO_TEST_HOOKS
            if (std::exchange(_fail_next_thread_start, false))
                throw std::runtime_error("injected publication thread start failure");
#endif
            slot->thread = std::thread([slot_ptr = slot.get(), job = std::move(job),
                                        hold = std::move(hold)]() mutable {
                // Registry ownership and join order keep this borrowed slot alive.
                auto &slot = *slot_ptr;
                Result result;
                try {
                    auto route = job.route;
                    result = slot.publisher(std::move(job));
                    if (result.route.empty()) result.route = std::move(route);
                } catch (...) {
                    // The publisher may have begun publication. Never claim a
                    // definite prepublication failure after an unknown exception.
                    result.outcome = Extension::Internal::PublicationOutcome::Uncertain;
                    try {
                        result.error = "Publication worker threw an exception";
                    } catch (...) {
                        // Preserve the typed outcome even if diagnostics cannot allocate.
                    }
                }
                try {
                    std::lock_guard lock(slot.mutex);
#ifdef VACARDS_FILE_IO_TEST_HOOKS
                    if (_fail_next_result_store.exchange(false)) throw std::bad_alloc();
#endif
                    slot.result = std::move(result);
                } catch (...) {
                    // Keep the terminal notification even if storing the result
                    // fails after publication has already happened.
                    try {
                        std::lock_guard lock(slot.mutex);
                        slot.result.outcome = Extension::Internal::PublicationOutcome::Uncertain;
                    } catch (...) {
                        g_message("Publication worker could not store its result");
                    }
                }
                slot.state.store(State::Posted, std::memory_order_release);
                auto *source = g_idle_source_new();
                g_source_set_callback(source, [](gpointer data) -> gboolean {
                    if (auto slot = static_cast<std::weak_ptr<Slot> *>(data)->lock()) {
                        g_assert(std::this_thread::get_id() == slot->owner);
                        try { deliver(*slot); }
                        catch (std::exception const &e) { g_warning("Publication completion threw: %s", e.what()); }
                        catch (...) { g_warning("Publication completion threw an unknown exception"); }
                    }
                    return G_SOURCE_REMOVE;
                }, hold.release(), [](gpointer data) {
                    delete static_cast<std::weak_ptr<Slot> *>(data);
                });
                g_source_attach(source, slot.context);
                g_source_unref(source);
            });
        } catch (...) {
            _slots.erase(doc_key);
            throw;
        }
        return StartResult::Started;
    }

    bool wait(void const *doc_key)
    {
        g_assert(std::this_thread::get_id() == _owner);
        auto it = _slots.find(doc_key);
        if (it == _slots.end()) return false;
        auto slot = std::move(it->second);
        _slots.erase(it);
        if (slot->thread.joinable()) slot->thread.join();
        try { deliver(*slot); }
        catch (std::exception const &e) { g_warning("Publication completion threw: %s", e.what()); }
        catch (...) { g_warning("Publication completion threw an unknown exception"); }
        return true;
    }

    bool pending(void const *doc_key) const
    {
        g_assert(std::this_thread::get_id() == _owner);
        auto it = _slots.find(doc_key);
        return it != _slots.end() &&
            it->second->state.load(std::memory_order_acquire) != State::Done;
    }

private:
    enum class State { Running, Posted, Done };
    struct Slot {
        ~Slot() { if (thread.joinable()) thread.join(); if (context) g_main_context_unref(context); }
        std::thread thread;
        std::thread::id owner;
        GMainContext *context = nullptr;
        std::mutex mutex;
        Result result;
        Completion completion;
        std::function<void()> delivery_observer;
        Publisher publisher;
        std::atomic<State> state{State::Running};
        // Only accessed on the owner thread; the worker touches state instead.
        bool completion_ran = false;
    };

    static void deliver(Slot &slot)
    {
        // Both idle dispatch and wait execute on the initiating owner thread.
        if (slot.completion_ran ||
            slot.state.load(std::memory_order_acquire) == State::Running) return;
        slot.completion_ran = true;
        slot.state.store(State::Done, std::memory_order_release);
        Result result;
        {
            std::lock_guard lock(slot.mutex);
            result = std::move(slot.result);
        }
        auto completion = std::exchange(slot.completion, {});
        slot.publisher = {};
        auto observer = std::exchange(slot.delivery_observer, {});
        try { if (completion) completion(std::move(result)); }
        catch (...) { if (observer) observer(); throw; }
        if (observer) observer();
    }

    Publisher _publisher;
    DeliveryObserver _delivery_observer;
    std::thread::id _owner;
    bool _closing = false;
#ifdef VACARDS_FILE_IO_TEST_HOOKS
    static inline thread_local bool _fail_next_thread_start = false;
    static inline thread_local bool _fail_next_holder_allocation = false;
    static inline std::atomic<bool> _fail_next_result_store = false;
#endif
    std::unordered_map<void const *, std::shared_ptr<Slot>> _slots;
};

} // namespace Inkscape::IO
#endif
