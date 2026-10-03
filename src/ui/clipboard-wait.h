// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_UI_CLIPBOARD_WAIT_H
#define INKSCAPE_UI_CLIPBOARD_WAIT_H

#include <glib.h>
#include <giomm/cancellable.h>
#include <glibmm/main.h>
#include <memory>

/**
 * Bounded main-context pumping for the clipboard waits.
 *
 * The clipboard must never block the GUI thread on a producer that does not
 * answer, and it must never let a busy source defer the caller's absolute
 * deadline. Both properties are implemented here, in one place, so they can be
 * exercised by a clipboard-free unit test with a private main context:
 *
 *  - every dispatch loop checks the absolute deadline *inside* the loop, not
 *    only between helper calls;
 *  - every call drains at most @a dispatch_budget events before returning to
 *    its caller, so a source that re-arms immediately (for example
 *    SPDocument::idle_handler, which returns G_SOURCE_CONTINUE while document
 *    work is pending) cannot keep the pump inside this helper past the
 *    deadline;
 *  - pending events are dispatched with iteration(false), which cannot block.
 *    When nothing was dispatched the helper sleeps for IDLE_SLEEP_US, so an
 *    idle wait is cheap instead of a CPU spin.
 *
 * The state objects waited on are shared_ptr-owned and every operation carries
 * a Gio::Cancellable, so a producer that answers after a timeout can never
 * touch a destroyed stack frame. Cancellation semantics are unchanged: on
 * expiry the operation is cancelled and drained for a bounded grace period.
 */
namespace Inkscape::UI::ClipboardWait {

/// Sleep used when no event was ready; keeps an idle wait cheap.
constexpr gint64 IDLE_SLEEP_US = 1000;
/// Maximum number of events one pump_step() call dispatches before returning.
constexpr int DEFAULT_DISPATCH_BUDGET = 64;

/// True once @a deadline (g_get_monotonic_time() base) has passed.
[[nodiscard]] inline bool deadline_expired(gint64 deadline)
{
    return g_get_monotonic_time() > deadline;
}

/**
 * Advance @a context by at most @a dispatch_budget non-blocking iterations.
 *
 * @return true when @a deadline expired (checked before every dispatch and once
 *         more before returning); false while there is still time left.
 */
inline bool pump_step(Glib::MainContext &context, gint64 deadline,
                      int dispatch_budget = DEFAULT_DISPATCH_BUDGET)
{
    bool dispatched = false;
    for (int budget = dispatch_budget > 0 ? dispatch_budget : 1; budget > 0; --budget) {
        if (deadline_expired(deadline)) {
            return true;
        }
        if (!context.pending()) {
            break;
        }
        if (!context.iteration(false)) {
            break;
        }
        dispatched = true;
    }
    if (!dispatched) {
        g_usleep(IDLE_SLEEP_US);
    }
    return deadline_expired(deadline);
}

/**
 * Wait for one asynchronous clipboard operation to complete, or for
 * @a deadline to expire.
 *
 * @param context main context whose pending events are dispatched (the default
 *        context in production; a private context in the unit test).
 * @param state shared completion state; only `done` is required.
 * @param cancellable cancelled on expiry so the producer stops early; may be
 *        null in tests that have no cancellable.
 * @param cancel_grace_us bounded drain after cancellation.
 * @return false when @a deadline expired before the operation completed.
 */
template <typename State>
bool wait_for_request_in(Glib::MainContext &context, std::shared_ptr<State> const &state,
                         Glib::RefPtr<Gio::Cancellable> const &cancellable, gint64 deadline,
                         gint64 cancel_grace_us, int dispatch_budget = DEFAULT_DISPATCH_BUDGET)
{
    while (!state->done) {
        if (deadline_expired(deadline)) {
            if (cancellable) {
                cancellable->cancel();
            }
            gint64 const grace_deadline = g_get_monotonic_time() + cancel_grace_us;
            while (!state->done && g_get_monotonic_time() < grace_deadline) {
                pump_step(context, grace_deadline, dispatch_budget);
            }
            return false;
        }
        pump_step(context, deadline, dispatch_budget);
    }
    return true;
}

} // namespace Inkscape::UI::ClipboardWait

#endif // INKSCAPE_UI_CLIPBOARD_WAIT_H
