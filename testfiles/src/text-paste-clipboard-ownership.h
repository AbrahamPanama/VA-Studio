// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef VACARDS_TEST_TEXT_PASTE_CLIPBOARD_OWNERSHIP_H
#define VACARDS_TEST_TEXT_PASTE_CLIPBOARD_OWNERSHIP_H

/**
 * Ownership decision for the in-process clipboard fixture's cleanup.
 *
 * GTK 4 exposes no pasteboard change count: GdkClipboard offers formats,
 * `is_local()` and content, but no generation token (the product keeps its own
 * counter in src/ui/clipboard.cpp, which is not reachable from a test). The
 * fixture therefore combines the two ownership probes GTK does expose:
 *
 *  - `GdkClipboard::is_local()`: TRUE while this process still owns the
 *    pasteboard. A third-party write (the user copying something) makes GDK
 *    re-claim the clipboard remotely, so the flag goes FALSE;
 *  - a change token: the fixture counts `GdkClipboard::signal_changed()`
 *    emissions and records the count right after every publication it makes. If
 *    the counter has moved since the last fixture publication, something wrote
 *    the clipboard during the run.
 *
 * When either probe says the clipboard is no longer the fixture's, the stale
 * snapshot must NOT be restored over the newer content.
 *
 * This is a pure function so the decision table is covered by the clipboard-free
 * unit test binary as well as by the integration fixture.
 */
namespace TextPasteTestSupport {

enum class RestoreDecision {
    restore,             ///< the fixture still owns the clipboard: restore the snapshot
    skip_no_snapshot,    ///< nothing restorable was captured
    skip_foreign_change, ///< a newer write happened, or ownership cannot be proven
};

inline RestoreDecision decide_clipboard_restore(bool has_snapshot, bool tracking_available,
                                                bool clipboard_is_local,
                                                unsigned current_change_token,
                                                unsigned own_publish_token)
{
    if (!has_snapshot) {
        return RestoreDecision::skip_no_snapshot;
    }
    if (!tracking_available || !clipboard_is_local) {
        // Without changed-signal tracking, or once another owner holds the
        // pasteboard, the fixture cannot prove it still owns the clipboard;
        // leaving the current contents alone is the only safe choice.
        return RestoreDecision::skip_foreign_change;
    }
    if (current_change_token != own_publish_token) {
        return RestoreDecision::skip_foreign_change;
    }
    return RestoreDecision::restore;
}

/**
 * Publication bookkeeping for the in-process clipboard fixture.
 *
 * Every fixture publication must refresh the ownership token **exactly once**,
 * after the pasteboard write has been pumped. A helper that forgets the refresh
 * (the r3 tests review P2-1 bug in setRawClipboardMulti) leaves the token at the
 * *previous* publication, so the fixture's own newest write looks like a foreign
 * change and cleanup refuses a restore it could safely perform; a helper that
 * refreshed it twice would be equally wrong for a publication that emits two
 * change events.
 *
 * The accounting is deliberately split from the GTK/display code so the
 * clipboard-free unit binary can drive the same logic the live fixture uses.
 */
class FixturePublicationLedger
{
public:
    /// Start tracking at the change-counter value observed before any publication.
    void begin_tracking(unsigned current_change_token)
    {
        tracking_ = true;
        own_publish_token_ = current_change_token;
    }

    [[nodiscard]] bool tracking_available() const { return tracking_; }

    /// Token recorded after the fixture's most recent completed publication.
    [[nodiscard]] unsigned own_publish_token() const { return own_publish_token_; }

    /// Number of publications that refreshed the token (never counts failures).
    [[nodiscard]] unsigned publication_count() const { return publications_; }

    /**
     * Drive exactly one fixture publication.
     *
     * @param publish        performs the pasteboard write (and pumping); returns
     *                       false when nothing was published
     * @param observed_token reads the changed-signal counter after the write
     * @return what @a publish returned; a failed publication records nothing
     */
    template <typename PublishFn, typename TokenFn>
    bool publication(PublishFn &&publish, TokenFn &&observed_token)
    {
        if (!publish()) {
            return false;
        }
        if (tracking_) {
            own_publish_token_ = observed_token();
            ++publications_;
        }
        return true;
    }

private:
    bool tracking_ = false;
    unsigned own_publish_token_ = 0;
    unsigned publications_ = 0;
};

} // namespace TextPasteTestSupport

#endif // VACARDS_TEST_TEXT_PASTE_CLIPBOARD_OWNERSHIP_H
