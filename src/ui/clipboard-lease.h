// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_UI_CLIPBOARD_LEASE_H
#define INKSCAPE_UI_CLIPBOARD_LEASE_H

#include <sigc++/scoped_connection.h>

#include "desktop.h"
#include "document.h"

/**
 * Destination-lifetime revalidation for clipboard pastes.
 *
 * Every paste path captures its destination (desktop + document) before a
 * bounded clipboard wait, and the wait pumps the GTK main context
 * (`ClipboardWait::wait_for_request_in`). During that pump a queued window close
 * can destroy the desktop and free the document, and the user can switch to a
 * different document/window. The captured pointers are therefore only usable
 * after revalidation.
 *
 * The desktop and document destruction signals are connected as
 * `sigc::scoped_connection` leases for the duration of the paste; the caller
 * fills in the facts below and refuses every mutation (import, node paste,
 * ungrouping, selection change, message, Undo) when the decision is false. This
 * pure predicate is the single decision point so it can be unit-tested without
 * a display, an application or a clipboard.
 */
namespace Inkscape::UI::ClipboardLease {

/**
 * The facts of one destination check. The first three describe the captured
 * pair; `active_is_captured` additionally requires that the paste still belongs
 * to the window it was issued on.
 */
struct DestinationFacts {
    bool desktop_alive = true;      ///< the captured SPDesktop has not emitted destroy
    bool document_alive = true;     ///< the captured SPDocument has not emitted destroy
    bool same_document = true;      ///< the desktop still points at the captured document
    bool active_is_captured = true; ///< SP_ACTIVE_DESKTOP is still the captured desktop
};

/**
 * `captured_is_active` for a captured desktop pointer and the process-wide
 * active desktop. This is pure pointer identity: it never dereferences either
 * argument, so it is safe to evaluate even when one of the windows has been
 * destroyed, and it is directly unit-testable without an application.
 */
[[nodiscard]] inline constexpr bool is_active_desktop(void const *sp_active_desktop, void const *captured_desktop)
{
    return sp_active_desktop == captured_desktop;
}

/**
 * True only when the paste may touch the captured destination again.
 *
 * All four facts must hold. `active_is_captured` is required by a path that
 * resolves its destination through a process-wide lookup after the wait — the
 * image route hands its document to `file_import()`, which reads
 * `SP_ACTIVE_DESKTOP` for the insertion layer and the selection — so a window
 * switch must abort there before any mutation. Paths that keep every use on the
 * captured desktop itself leave the fact at its default and are unaffected.
 */
[[nodiscard]] inline constexpr bool destination_valid(DestinationFacts const &facts)
{
    return facts.desktop_alive && facts.document_alive && facts.same_document && facts.active_is_captured;
}

/**
 * RAII destination lease for one clipboard wait.
 *
 * Construct it BEFORE the wait with the destination the paste will mutate
 * afterwards; call valid() AFTER the wait and refuse every mutation when it is
 * false. Both destroy signals are connected as scoped connections (a plain
 * sigc::connection does not disconnect on destruction), and the flags are
 * declared before the connections so they outlive them. The object is
 * non-movable because the slots capture `this`. A null desktop or document is
 * allowed (nothing to lease). The desktop's document is dereferenced only while
 * both objects are known alive.
 */
class DestinationLease
{
public:
    DestinationLease(SPDesktop *desktop, SPDocument *document)
        : _desktop(desktop)
        , _document(document)
    {
        if (_desktop) {
            _desktop_lease = _desktop->connectDestroy([this](SPDesktop *) { _desktop_alive = false; });
        }
        if (_document) {
            _document_lease = _document->connectDestroy([this] { _document_alive = false; });
        }
    }
    DestinationLease(DestinationLease const &) = delete;
    DestinationLease &operator=(DestinationLease const &) = delete;

    /// True while the captured desktop and document are alive and still paired.
    [[nodiscard]] bool valid() const
    {
        DestinationFacts facts;
        facts.desktop_alive = _desktop_alive;
        facts.document_alive = _document_alive;
        facts.same_document = _desktop_alive && _document_alive &&
                              (!_desktop || _desktop->getDocument() == _document);
        return destination_valid(facts);
    }

private:
    SPDesktop *_desktop;
    SPDocument *_document;
    bool _desktop_alive = true;
    bool _document_alive = true;
    sigc::scoped_connection _desktop_lease;
    sigc::scoped_connection _document_lease;
};

/**
 * Scope guard that makes paste() non-reentrant (K7). `entered()` is false when
 * the flag was already set, in which case the guard leaves the flag alone, so
 * only the outermost call clears it.
 */
class ReentryGuard
{
public:
    explicit ReentryGuard(bool &flag)
        : _flag(flag)
        , _entered(!flag)
    {
        if (_entered) {
            _flag = true;
        }
    }
    ~ReentryGuard()
    {
        if (_entered) {
            _flag = false;
        }
    }
    ReentryGuard(ReentryGuard const &) = delete;
    ReentryGuard &operator=(ReentryGuard const &) = delete;

    [[nodiscard]] bool entered() const { return _entered; }

private:
    bool &_flag;
    bool _entered;
};

/// Largest clipboard image that is pasted: 100 megapixels, the same budget as the
/// destructive bitmap clip (destructive-bitmap-clip-chemistry.cpp).
constexpr long long MAX_PASTE_IMAGE_PIXELS = 100'000'000LL;

/// K8: true when a width x height texture may be pasted. Computed in 64 bits.
[[nodiscard]] inline constexpr bool image_within_paste_limit(int width, int height,
                                                             long long max_pixels = MAX_PASTE_IMAGE_PIXELS)
{
    return width > 0 && height > 0 && static_cast<long long>(width) * height <= max_pixels;
}

} // namespace Inkscape::UI::ClipboardLease

#endif // INKSCAPE_UI_CLIPBOARD_LEASE_H
