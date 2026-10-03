// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * @brief FontDiscovery::connect_to_fonts cached-replay subscription contract
 *
 * Independent regression for the F3 defect: when fonts were already cached,
 * connect_to_fonts() replayed OperationResult+OperationFinished by emitting on
 * the shared _events signal, so every earlier subscriber saw the replay as if a
 * fresh scan had completed (FontList resets its stores and schedules a rebuild
 * for each such event). F3 routes the cached replay to the newly attached
 * callback only, while real background events still broadcast through _events.
 *
 * These cases use the real FontDiscovery singleton and real callback counters.
 * A bounded Glib main loop is used only to establish the warm-cache precondition
 * and to observe genuine re-scans; no test asserts on elapsed time, and a
 * missing prerequisite fails rather than skips.
 *
 * Prerequisites (must be provided by the CTest environment; root owns CMake):
 *   - INKSCAPE_PROFILE_DIR : isolated per-test profile directory
 *   - INKSCAPE_FONTCONFIG  : testfiles/rendering_tests/fonts/isolated.conf
 * Either missing is a fatal test failure (the fixture refuses to touch the
 * user's real profile or the system font configuration).
 */

#include <gtest/gtest.h>

#include <glibmm/main.h>
#include <glibmm/miscutils.h>

#include <chrono>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "io/resource.h"
#include "libnrtype/font-factory.h"
#include "util/font-discovery.h"

using namespace Inkscape;

namespace {

// Upper bound that only terminates a genuinely hung product path. Every pass is
// decided by real callback counters observed on the main context.
constexpr unsigned readiness_ceiling_ms = 30000;

std::string catalogPath()
{
    // Same persistent catalog name as font-discovery.cpp's private constant.
    return Glib::build_filename(IO::Resource::profile_path(), "font-catalog-v2.ini");
}

FontFamilies warmCatalog()
{
    FontInfo font;
    font.family_name = "F3 Replay Fixture";
    font.face_name = "Regular";
    font.description = "F3 Replay Fixture Regular";
    font.fontspec = "F3 Replay Fixture";
    font.id = "f3-replay-fixture-regular";
    font.source_path = "/fonts/f3-replay-fixture.otf";
    font.source_size = 4096;
    font.source_mtime = 1234567890;
    font.weight = 0.5;
    font.width = 0.5;
    font.family_kind = 8;
    font.available = true;
    return FontFamilies{{font}};
}

struct Counters
{
    int started = 0;
    int results = 0;
    int progress = 0;
    int cancelled = 0;
    int finished = 0;
    FontDiscovery::FontsPayload last_result;
};

void observe(Counters &counters, FontDiscovery::MessageType const &msg)
{
    if (auto result = Async::Msg::get_result(msg)) {
        ++counters.results;
        counters.last_result = *result;
    } else if (Async::Msg::get_progress(msg)) {
        ++counters.progress;
    } else if (Async::Msg::is_finished(msg)) {
        ++counters.finished;
    } else if (std::get_if<Async::Msg::OperationCancelled>(&msg)) {
        ++counters.cancelled;
    } else if (std::get_if<Async::Msg::OperationStarted>(&msg)) {
        ++counters.started;
    }
}

// Polls real callback state on the main context. The interval and ceiling are
// detection bounds only; the predicate is the oracle.
bool pumpUntil(std::function<bool ()> const &done, unsigned ceiling_ms)
{
    if (done()) return true;
    auto loop = Glib::MainLoop::create();
    auto poll = Glib::signal_timeout().connect([&] {
        if (done()) { loop->quit(); return false; }
        return true;
    }, 10);
    auto ceiling = Glib::signal_timeout().connect([&] { loop->quit(); return false; }, ceiling_ms);
    loop->run();
    poll.disconnect();
    ceiling.disconnect();
    return done();
}

// Write a structurally valid persistent catalog using the live fontconfig
// manifest, then force the singleton through the real load path and wait for
// the real Finished. Readiness only: the oracle cases start after this returns.
bool warmCachedCatalog(std::string &why)
{
    auto const manifest = FontFactory::get().font_config_manifest();
    if (manifest.empty()) {
        why = "FontFactory::font_config_manifest() returned an empty manifest";
        return false;
    }
    if (!FontCatalog::save(catalogPath(), warmCatalog(), manifest)) {
        why = "FontCatalog::save() could not write " + catalogPath();
        return false;
    }

    // Drop any cache from an earlier case. With no subscribers this does not
    // start a load, so the connect below owns the fresh real load.
    FontDiscovery::get().invalidate();

    bool finished = false;
    int results = 0;
    auto loop = Glib::MainLoop::create();
    auto connection = FontDiscovery::get().connect_to_fonts(
        [&](FontDiscovery::MessageType const &msg) {
            if (Async::Msg::get_result(msg)) ++results;
            if (Async::Msg::is_finished(msg)) { finished = true; loop->quit(); }
        });
    auto ceiling = Glib::signal_timeout().connect([&] { loop->quit(); return false; }, readiness_ceiling_ms);
    loop->run();
    ceiling.disconnect();
    connection.disconnect();

    if (!finished) {
        why = "warm catalog load did not finish within the readiness ceiling";
        return false;
    }
    if (results != 1) {
        why = "warm catalog load delivered " + std::to_string(results) + " Results, expected 1";
        return false;
    }
    return true;
}

// Let any background load started by a case finish before teardown.
bool settleDiscovery()
{
    bool saw_result = false;
    bool finished = false;
    auto connection = FontDiscovery::get().connect_to_fonts(
        [&](FontDiscovery::MessageType const &msg) {
            if (Async::Msg::get_result(msg)) saw_result = true;
            if (saw_result && Async::Msg::is_finished(msg)) finished = true;
        });
    bool const settled = pumpUntil([&] { return finished; }, readiness_ceiling_ms);
    connection.disconnect();
    return settled;
}

} // namespace

class FontDiscoverySubscriptionTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        auto const profile = Glib::getenv("INKSCAPE_PROFILE_DIR");
        ASSERT_FALSE(profile.empty())
            << "F3 subscription test requires an isolated INKSCAPE_PROFILE_DIR; refusing to "
               "write a font catalog into the user's real profile";

        auto const fontconfig = Glib::getenv("INKSCAPE_FONTCONFIG");
        ASSERT_FALSE(fontconfig.empty())
            << "F3 subscription test requires the isolated INKSCAPE_FONTCONFIG fixture "
               "(testfiles/rendering_tests/fonts/isolated.conf); fixture errors must fail, not skip";

        std::string why;
        ASSERT_TRUE(warmCachedCatalog(why)) << why;
    }
};

// A late subscriber's cached replay must not reach subscribers that were
// already attached, and must arrive synchronously with no event pump.
TEST_F(FontDiscoverySubscriptionTest, CachedReplayReachesOnlyTheNewSubscriber)
{
    Counters older;
    auto older_connection = FontDiscovery::get().connect_to_fonts(
        [&](FontDiscovery::MessageType const &msg) { observe(older, msg); });
    ASSERT_EQ(older.results, 1) << "new subscriber must receive its own cached replay";
    ASSERT_EQ(older.finished, 1) << "new subscriber must receive its own cached replay Finished";

    Counters newer;
    auto newer_connection = FontDiscovery::get().connect_to_fonts(
        [&](FontDiscovery::MessageType const &msg) { observe(newer, msg); });

    // Deliberately no main-context iteration between attach and assertion.
    EXPECT_EQ(newer.results, 1);
    EXPECT_EQ(newer.finished, 1);
    EXPECT_EQ(older.results, 1) << "earlier subscriber received the late subscriber's replay";
    EXPECT_EQ(older.finished, 1) << "earlier subscriber received the late subscriber's replay";
}

// A subscriber attached from inside another subscriber's cached Result callback
// must leave both at exactly one Result and one Finished.
TEST_F(FontDiscoverySubscriptionTest, ReentrantSubscriptionDeliversEachExactlyOnce)
{
    Counters inner;
    Counters outer;
    sigc::scoped_connection inner_connection;
    bool attached = false;

    auto outer_connection = FontDiscovery::get().connect_to_fonts(
        [&](FontDiscovery::MessageType const &msg) {
            observe(outer, msg);
            if (Async::Msg::get_result(msg) && !attached) {
                attached = true;
                inner_connection = FontDiscovery::get().connect_to_fonts(
                    [&](FontDiscovery::MessageType const &inner_msg) { observe(inner, inner_msg); });
            }
        });

    ASSERT_TRUE(attached);
    EXPECT_EQ(outer.results, 1);
    EXPECT_EQ(outer.finished, 1);
    EXPECT_EQ(inner.results, 1);
    EXPECT_EQ(inner.finished, 1);
}

// Disconnecting removes future broadcasts: a real invalidate() re-scan must
// complete without invoking the disconnected callback.
TEST_F(FontDiscoverySubscriptionTest, DisconnectedSubscriberIsNotInvokedByARealScan)
{
    Counters dead;
    {
        auto dead_connection = FontDiscovery::get().connect_to_fonts(
            [&](FontDiscovery::MessageType const &msg) { observe(dead, msg); });
        ASSERT_EQ(dead.results, 1);
        ASSERT_EQ(dead.finished, 1);
        dead_connection.disconnect();
    }

    Counters sentinel;
    bool invalidated = false;
    bool genuine_result = false;
    bool genuine_finished = false;
    auto sentinel_connection = FontDiscovery::get().connect_to_fonts(
        [&](FontDiscovery::MessageType const &msg) {
            observe(sentinel, msg);
            if (invalidated && Async::Msg::get_result(msg)) genuine_result = true;
            if (genuine_result && Async::Msg::is_finished(msg)) genuine_finished = true;
        });
    ASSERT_EQ(sentinel.results, 1) << "sentinel must observe its own cached replay";

    invalidated = true;
    FontDiscovery::get().invalidate();
    ASSERT_TRUE(pumpUntil([&] { return genuine_finished; }, readiness_ceiling_ms))
        << "real invalidate() scan did not complete within the readiness ceiling";

    EXPECT_EQ(dead.results, 1) << "disconnected subscriber received a Result";
    EXPECT_EQ(dead.finished, 1) << "disconnected subscriber received a Finished";
    sentinel_connection.disconnect();
}

// Disconnecting must actually release the callback functor (and the object it
// owns), not merely silence it.
TEST_F(FontDiscoverySubscriptionTest, DisconnectedSlotReleasesTheCallbackOwner)
{
    std::weak_ptr<Counters> weak_owner;
    {
        auto owner = std::make_shared<Counters>();
        weak_owner = owner;
        auto owner_connection = FontDiscovery::get().connect_to_fonts(
            [owner](FontDiscovery::MessageType const &msg) { observe(*owner, msg); });
        ASSERT_EQ(owner->results, 1);
        owner_connection.disconnect();
    }
    ASSERT_TRUE(weak_owner.expired()) << "disconnect left the callback functor owner alive";

    // A real re-scan must not resurrect the released callback.
    Counters sentinel;
    bool invalidated = false;
    bool genuine_finished = false;
    bool saw_result = false;
    auto sentinel_connection = FontDiscovery::get().connect_to_fonts(
        [&](FontDiscovery::MessageType const &msg) {
            observe(sentinel, msg);
            if (invalidated && Async::Msg::get_result(msg)) saw_result = true;
            if (saw_result && Async::Msg::is_finished(msg)) genuine_finished = true;
        });
    invalidated = true;
    FontDiscovery::get().invalidate();
    ASSERT_TRUE(pumpUntil([&] { return genuine_finished; }, readiness_ceiling_ms))
        << "real invalidate() scan did not complete within the readiness ceiling";
    sentinel_connection.disconnect();
}

// Cached replay routing must not disable the real background broadcast.
TEST_F(FontDiscoverySubscriptionTest, GenuineInvalidateScanReachesCurrentSubscribers)
{
    Counters live;
    bool invalidated = false;
    bool genuine_result = false;
    bool genuine_finished = false;
    auto live_connection = FontDiscovery::get().connect_to_fonts(
        [&](FontDiscovery::MessageType const &msg) {
            observe(live, msg);
            if (invalidated && Async::Msg::get_result(msg)) genuine_result = true;
            if (genuine_result && Async::Msg::is_finished(msg)) genuine_finished = true;
        });
    ASSERT_EQ(live.results, 1) << "current subscriber must first receive the cached replay";

    invalidated = true;
    FontDiscovery::get().invalidate();
    ASSERT_TRUE(pumpUntil([&] { return genuine_finished; }, readiness_ceiling_ms))
        << "current subscriber did not receive the genuine re-scan Result/Finished";

    EXPECT_EQ(live.results, 2) << "genuine re-scan Result must reach the current subscriber";
    ASSERT_TRUE(live.last_result);
    EXPECT_FALSE(live.last_result->empty()) << "genuine re-scan delivered an empty payload";
    live_connection.disconnect();
}

// invalidate() during the cached Result callback must suppress the replay's own
// Finished. A cancellation Finished emitted synchronously inside the callback is
// a real event from invalidate() and is tolerated; the stale replay Finished is
// the one emitted after the Result callback returns.
TEST_F(FontDiscoverySubscriptionTest, InvalidateDuringCachedResultSuppressesStaleFinished)
{
    struct ReplayState
    {
        int results = 0;
        int finished = 0;
        int finished_after_result_callback = 0;
        bool in_result_callback = false;
        bool invalidate_called = false;
    } replay;

    auto replay_connection = FontDiscovery::get().connect_to_fonts(
        [&](FontDiscovery::MessageType const &msg) {
            if (Async::Msg::get_result(msg)) {
                ++replay.results;
                if (!replay.invalidate_called) {
                    replay.invalidate_called = true;
                    replay.in_result_callback = true;
                    FontDiscovery::get().invalidate();
                    replay.in_result_callback = false;
                }
            } else if (Async::Msg::is_finished(msg)) {
                ++replay.finished;
                if (!replay.in_result_callback) ++replay.finished_after_result_callback;
            }
        });

    ASSERT_TRUE(replay.invalidate_called);
    EXPECT_EQ(replay.results, 1) << "cached replay must deliver exactly one Result";
    EXPECT_EQ(replay.finished_after_result_callback, 0)
        << "stale cached-replay Finished delivered after the cache was invalidated";
    EXPECT_LE(replay.finished, 2) << "more Finished events than invalidate() can explain";

    ASSERT_TRUE(settleDiscovery()) << "replacement discovery did not settle";
    replay_connection.disconnect();
}
