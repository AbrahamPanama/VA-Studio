// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * W5a outcome tests for the core Application runtime policy.
 *
 * Two suites share this executable but are intended to run in separate
 * subprocesses (CTest registers one invocation per filter):
 *
 *   --gtest_filter=PreviewRuntimePolicyEditor.*
 *   --gtest_filter=PreviewRuntimePolicyHelper.*
 *
 * The Application singleton is process-global and its policy is immutable, so
 * running both suites in one process is NOT supported: the Helper suite would
 * observe the Editor policy created by the Editor suite.
 *
 * The Helper suite renders one compiled-in, safe SVG (a gradient-filled
 * rectangle) through the W4 primitive. It does not read a path, does not
 * admit input and does not claim isolation; the SVG literal here is a test
 * fixture, not a supported input channel. No signal is ever raised: the
 * sentinels only prove which handler is installed.
 *
 * Copyright 2026 VA Studio authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "inkscape.h"

#include "document.h"
#include "inkscape-application.h"
#include "ui/cache/welcome-drawing-preview.h"

#include <2geom/rect.h>
#include <cairomm/surface.h>
#include <csignal>
#include <cmath>
#include <cstddef>
#include <gtest/gtest.h>
#include <span>
#include <stdexcept>
#include <string>

using Inkscape::Application;

namespace {

using SignalHandler = void (*)(int);

void sentinel_handler(int) {}

#ifndef _WIN32
constexpr int policy_signals[] = {SIGSEGV, SIGABRT, SIGFPE, SIGILL, SIGBUS};
#else
constexpr int policy_signals[] = {SIGSEGV, SIGABRT, SIGFPE, SIGILL};
#endif
constexpr std::size_t policy_signal_count = sizeof(policy_signals) / sizeof(policy_signals[0]);

/**
 * Install the sentinels and restore the original handlers on scope exit.
 *
 * std::signal returns SIG_ERR on failure, which is not a function pointer.
 * Installation and every later query record that failure explicitly instead of
 * treating SIG_ERR as a handler, and the destructor never restores SIG_ERR.
 */
class SentinelScope
{
public:
    SentinelScope()
    {
        for (std::size_t i = 0; i < policy_signal_count; ++i) {
            SignalHandler const previous = std::signal(policy_signals[i], sentinel_handler);
            if (previous == SIG_ERR) {
                _install_failed = true;
                continue; // No prior handler to restore for this signal.
            }
            _original_valid[i] = true;
            _original[i] = previous;
        }
    }

    ~SentinelScope()
    {
        for (std::size_t i = 0; i < policy_signal_count; ++i) {
            // Only a real previous handler may be restored; SIG_ERR must never
            // be passed back to std::signal as a function pointer.
            if (_original_valid[i]) {
                std::signal(policy_signals[i], _original[i]);
            }
        }
    }

    /** True if installing any sentinel returned SIG_ERR. */
    bool install_failed() const { return _install_failed; }

    /** True if a later query returned SIG_ERR. */
    bool query_failed() const { return _query_failed; }

    /** Current handler for signal i, left unchanged; SIG_ERR if the query failed. */
    SignalHandler current(std::size_t i) const
    {
        SignalHandler const installed = std::signal(policy_signals[i], sentinel_handler);
        if (installed == SIG_ERR) {
            _query_failed = true;
            return SIG_ERR;
        }
        std::signal(policy_signals[i], installed);
        return installed;
    }

private:
    SignalHandler _original[policy_signal_count]{};
    bool _original_valid[policy_signal_count]{};
    bool _install_failed = false;
    mutable bool _query_failed = false;
};

/** All sentinels still in place? */
bool sentinels_preserved(SentinelScope const &scope)
{
    for (std::size_t i = 0; i < policy_signal_count; ++i) {
        SignalHandler const handler = scope.current(i);
        if (handler == SIG_ERR || handler != sentinel_handler) {
            return false;
        }
    }
    return true;
}

/** All sentinels replaced by the Application crash handler? */
bool crash_handler_installed(SentinelScope const &scope)
{
    for (std::size_t i = 0; i < policy_signal_count; ++i) {
        SignalHandler const handler = scope.current(i);
        if (handler == SIG_ERR || handler != &Application::crash_handler) {
            return false;
        }
    }
    return true;
}

// A single gradient-filled rectangle placed off-page, so bounds are negative
// and the drawing framing (not the page) is exercised. Compiled in on purpose.
char const *safe_svg = R"svg(<?xml version="1.0"?>
<svg xmlns="http://www.w3.org/2000/svg" width="100" height="50" viewBox="0 0 100 50">
  <defs>
    <linearGradient id="g" x1="0" y1="0" x2="1" y2="0">
      <stop offset="0" stop-color="#ff0000"/>
      <stop offset="1" stop-color="#0000ff"/>
    </linearGradient>
  </defs>
  <rect x="-40" y="-30" width="180" height="100" fill="url(#g)"/>
</svg>)svg";

struct Rgba {
    unsigned char r = 0, g = 0, b = 0, a = 0;
};

Rgba pixel(Cairo::RefPtr<Cairo::ImageSurface> const &surface, int x, int y)
{
    unsigned char const *row =
        surface->get_data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(surface->get_stride());
    unsigned char const *p = row + static_cast<std::size_t>(x) * 4;
    // Cairo ARGB32 memory order on little-endian is B,G,R,A.
    return Rgba{p[2], p[1], p[0], p[3]};
}

bool is_white(Rgba const &c)
{
    return c.r == 255 && c.g == 255 && c.b == 255;
}

bool contains_non_white(Cairo::RefPtr<Cairo::ImageSurface> const &surface)
{
    for (int y = 0; y < surface->get_height(); ++y) {
        for (int x = 0; x < surface->get_width(); ++x) {
            if (!is_white(pixel(surface, x, y))) {
                return true;
            }
        }
    }
    return false;
}

bool all_opaque(Cairo::RefPtr<Cairo::ImageSurface> const &surface)
{
    for (int y = 0; y < surface->get_height(); ++y) {
        for (int x = 0; x < surface->get_width(); ++x) {
            if (pixel(surface, x, y).a != 255) {
                return false;
            }
        }
    }
    return true;
}

} // namespace

// The default path must keep installing the real crash handler, keep the
// Editor policy immutable, and reject a PreviewHelper request.
TEST(PreviewRuntimePolicyEditor, DefaultEditorInstallsRecoveryAndRejectsHelper)
{
    ASSERT_FALSE(Application::exists());
    SentinelScope sentinels;
    ASSERT_FALSE(sentinels.install_failed()) << "could not install a supported signal sentinel";

    Application::create(false); // historical default call site

    ASSERT_TRUE(Application::exists());
    EXPECT_EQ(Application::instance().runtime_policy(), Application::RuntimePolicy::Editor);
    EXPECT_FALSE(Application::instance().use_gui());
    EXPECT_TRUE(crash_handler_installed(sentinels));
    EXPECT_FALSE(sentinels.query_failed()) << "a supported signal could not be queried";

    // Repeated same-policy creation stays the historical no-op.
    EXPECT_NO_THROW(Application::create(false));
    EXPECT_EQ(Application::instance().runtime_policy(), Application::RuntimePolicy::Editor);
    EXPECT_TRUE(crash_handler_installed(sentinels));
    EXPECT_FALSE(sentinels.query_failed());

    // A conflicting policy is rejected before any state changes.
    EXPECT_THROW(Application::create(false, Application::RuntimePolicy::PreviewHelper), std::logic_error);
    EXPECT_EQ(Application::instance().runtime_policy(), Application::RuntimePolicy::Editor);
    EXPECT_TRUE(crash_handler_installed(sentinels));

    // Editor GUI toggling keeps its unchanged behavior.
    EXPECT_NO_THROW(Application::instance().use_gui(true));
    EXPECT_TRUE(Application::instance().use_gui());
    EXPECT_NO_THROW(Application::instance().use_gui(false));
    EXPECT_FALSE(Application::instance().use_gui());
    EXPECT_TRUE(crash_handler_installed(sentinels));
}

// The preview helper must preserve the caller's signal handlers, never touch
// the GUI, reject a conflicting policy, and still render a private document.
TEST(PreviewRuntimePolicyHelper, PreservesSentinelsAndRendersPrivateDocument)
{
    ASSERT_FALSE(Application::exists());
    SentinelScope sentinels;
    ASSERT_FALSE(sentinels.install_failed()) << "could not install a supported signal sentinel";

    // A rejected helper creation must not create the singleton or touch the
    // caller's handlers.
    EXPECT_THROW(Application::create(true, Application::RuntimePolicy::PreviewHelper), std::invalid_argument);
    EXPECT_FALSE(Application::exists());
    EXPECT_TRUE(sentinels_preserved(sentinels));
    EXPECT_FALSE(sentinels.query_failed());

    Application::create(false, Application::RuntimePolicy::PreviewHelper);

    ASSERT_TRUE(Application::exists());
    EXPECT_EQ(Application::instance().runtime_policy(), Application::RuntimePolicy::PreviewHelper);
    EXPECT_FALSE(Application::instance().use_gui());
    EXPECT_TRUE(sentinels_preserved(sentinels));
    EXPECT_FALSE(sentinels.query_failed());

    // Repeated same-policy creation is a no-op for the helper too.
    EXPECT_NO_THROW(Application::create(false, Application::RuntimePolicy::PreviewHelper));
    EXPECT_EQ(Application::instance().runtime_policy(), Application::RuntimePolicy::PreviewHelper);
    EXPECT_TRUE(sentinels_preserved(sentinels));
    EXPECT_FALSE(sentinels.query_failed());

    // No supervisor-level application or desktop was created.
    EXPECT_EQ(InkscapeApplication::instance(), nullptr);
    EXPECT_TRUE(Application::instance().get_desktops().empty());

    // Conflicting policy and GUI attempts change nothing.
    EXPECT_THROW(Application::create(false, Application::RuntimePolicy::Editor), std::logic_error);
    EXPECT_THROW(Application::create(true, Application::RuntimePolicy::PreviewHelper), std::invalid_argument);
    EXPECT_THROW(Application::instance().use_gui(true), std::logic_error);
    EXPECT_EQ(Application::instance().runtime_policy(), Application::RuntimePolicy::PreviewHelper);
    EXPECT_FALSE(Application::instance().use_gui());
    EXPECT_TRUE(sentinels_preserved(sentinels));
    EXPECT_FALSE(sentinels.query_failed());

    // Private native document; the primitive frames the drawing envelope.
    std::span<char const> const buffer(safe_svg, std::char_traits<char>::length(safe_svg));
    auto doc = SPDocument::createNewDocFromMem(buffer);
    ASSERT_TRUE(doc);

    Inkscape::UI::Cache::WelcomeDrawingPreviewRequest request;
    request.width = 200;
    request.height = 150;
    request.background = Inkscape::UI::Cache::WelcomePreviewBackground::White;
    auto result = Inkscape::UI::Cache::renderWelcomeDrawingPreview(*doc, request);

    ASSERT_EQ(result.status, Inkscape::UI::Cache::WelcomePreviewStatus::Rendered)
        << Inkscape::UI::Cache::welcomePreviewStatusName(result.status);
    ASSERT_TRUE(result.surface);
    EXPECT_EQ(result.width, 200u);
    EXPECT_EQ(result.height, 150u);

    // Off-page rectangle: bounds are exactly (-40,-30)..(140,70), so the
    // drawing framing, not the page, is used.
    EXPECT_NEAR(result.document_bounds.min()[Geom::X], -40.0, 1e-6);
    EXPECT_NEAR(result.document_bounds.min()[Geom::Y], -30.0, 1e-6);
    EXPECT_NEAR(result.document_bounds.max()[Geom::X], 140.0, 1e-6);
    EXPECT_NEAR(result.document_bounds.max()[Geom::Y], 70.0, 1e-6);
    EXPECT_EQ(result.framing, Inkscape::UI::Cache::WelcomePreviewFraming::Drawing);
    EXPECT_GT(result.budget.surfaces, 0u);

    // s=1, T=(50,55): rect dev x 10..190, y 25..125.
    EXPECT_TRUE(contains_non_white(result.surface));
    // Gradient runs red (left) to blue (right); assert dominance, not exact
    // interpolation, so the check is robust to colour-space handling.
    Rgba left = pixel(result.surface, 20, 75);
    EXPECT_GT(left.r, 150);
    EXPECT_GT(left.r, left.b);
    Rgba right = pixel(result.surface, 180, 75);
    EXPECT_GT(right.b, 150);
    EXPECT_GT(right.b, right.r);
    // White letterbox outside the drawn rectangle.
    EXPECT_TRUE(is_white(pixel(result.surface, 2, 75)));
    EXPECT_TRUE(is_white(pixel(result.surface, 197, 75)));
    EXPECT_TRUE(all_opaque(result.surface));
}
