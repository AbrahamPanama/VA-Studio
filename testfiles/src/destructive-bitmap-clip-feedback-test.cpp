// SPDX-License-Identifier: GPL-2.0-or-later
//
// B05 focused outcome tests: the destructive bitmap clip action must map every
// B04 transaction status to a stable, user-visible reason and must leave the
// document unchanged on failure.

#include "ui/tools/destructive-bitmap-clip.h"

#include <cstring>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib.h>
#include <glibmm/ustring.h>
#include <gtest/gtest.h>

#include "desktop.h"
#include "display/cairo-utils.h"
#include "document-undo.h"
#include "document.h"
#include "inkscape-application.h"
#include "inkscape.h"
#include "message.h"
#include "object/sp-image.h"
#include "object/sp-item.h"
#include "object/sp-root.h"
#include "selection.h"
#include "ui/tools/destructive-bitmap-clip-chemistry.h"
#include "xml/document.h"
#include "xml/repr.h"

using namespace Inkscape;

namespace BitmapClip = Inkscape::UI::Tools::DestructiveBitmapClip;

// The B05 feedback surface is defined in src/actions/actions-object.cpp. It is
// not declared in a public header, so the focused test declares exactly the
// functions it exercises. `commit_selection`/`resolve_targets` come from the
// accepted B04 chemistry header.
namespace Inkscape::UI::Tools::DestructiveBitmapClip {

char const *action_outcome_reason(CommitStatus status, TargetStatus targets) noexcept;
Inkscape::MessageType action_outcome_message_type(char const *reason) noexcept;
char const *action_outcome_message(char const *reason, bool inverse) noexcept;

} // namespace Inkscape::UI::Tools::DestructiveBitmapClip

namespace {

constexpr char const *REASON_SUCCESS = "success";
constexpr char const *REASON_SUCCESS_FULLY_TRANSPARENT = "success-fully-transparent";
constexpr char const *REASON_NO_CHANGE = "no-change";
constexpr char const *REASON_INVALID_SELECTION = "invalid-selection";
constexpr char const *REASON_MISSING_SOURCE = "missing-source";
constexpr char const *REASON_INVALID_CUTTER = "invalid-cutter";
constexpr char const *REASON_UNSUPPORTED_EFFECTS = "unsupported-effects-or-references";
constexpr char const *REASON_UNSUPPORTED_TRIM = "unsupported-trim";
constexpr char const *REASON_PROTECTED_OBJECT = "protected-object";
constexpr char const *REASON_RASTERIZATION_FAILED = "rasterization-failed";
constexpr char const *REASON_ENCODING_FAILED = "encoding-failed";
constexpr char const *REASON_CANCELLED = "cancelled";

std::unique_ptr<Pixbuf> solid_pixbuf(int width, int height, guint32 rgba)
{
    auto *raw = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, width, height);
    if (!raw) {
        return {};
    }
    gdk_pixbuf_fill(raw, rgba);
    return std::make_unique<Pixbuf>(raw);
}

std::string png_uri(Pixbuf const &pixbuf)
{
    auto encoded = sp_image_encode_png_data_uri(pixbuf);
    return encoded ? *encoded : std::string{};
}

InkscapeApplication *initialize_application()
{
    static auto *application = [] {
        g_setenv("INKSCAPE_APP_ID_TAG", "destructivebitmapclipfeedbacktest", TRUE);
        auto *result = new InkscapeApplication(); // Process-lifetime test fixture.
        result->gio_app()->register_application();
        return result;
    }();
    return application;
}

std::string live_xml(XML::Document *document)
{
    return sp_repr_save_buf(document).raw();
}

} // namespace

// ---------------------------------------------------------------------------
// Pure status -> reason mapping. No document is needed.
// ---------------------------------------------------------------------------

TEST(DestructiveBitmapClipFeedbackMapping, SuccessfulStatusesMapToConciseReasons)
{
    using C = BitmapClip::CommitStatus;
    using T = BitmapClip::TargetStatus;

    EXPECT_STREQ(BitmapClip::action_outcome_reason(C::Committed, T::Resolved), REASON_SUCCESS);
    EXPECT_STREQ(BitmapClip::action_outcome_reason(C::CommittedAllTransparent, T::Resolved),
                 REASON_SUCCESS_FULLY_TRANSPARENT);
    EXPECT_STREQ(BitmapClip::action_outcome_reason(C::NoChange, T::Resolved), REASON_NO_CHANGE);
}

TEST(DestructiveBitmapClipFeedbackMapping, RejectedPairKeepsMissingSourceAndCutterDistinct)
{
    using C = BitmapClip::CommitStatus;
    using T = BitmapClip::TargetStatus;

    EXPECT_STREQ(BitmapClip::action_outcome_reason(C::InvalidSelection, T::NotPair),
                 REASON_INVALID_SELECTION);
    EXPECT_STREQ(BitmapClip::action_outcome_reason(C::InvalidSelection, T::NestedOrDuplicateRoots),
                 REASON_INVALID_SELECTION);
    EXPECT_STREQ(BitmapClip::action_outcome_reason(C::InvalidSelection, T::AmbiguousImageBranch),
                 REASON_INVALID_SELECTION);
    EXPECT_STREQ(BitmapClip::action_outcome_reason(C::InvalidSelection, T::MultipleImages),
                 REASON_INVALID_SELECTION);
    EXPECT_STREQ(BitmapClip::action_outcome_reason(C::InvalidSelection, T::ProtectedObject),
                 REASON_PROTECTED_OBJECT);
    EXPECT_STREQ(BitmapClip::action_outcome_reason(C::InvalidSelection, T::MissingImage),
                 REASON_MISSING_SOURCE);
    EXPECT_STREQ(BitmapClip::action_outcome_reason(C::InvalidGeometry, T::InvalidGeometry),
                 REASON_MISSING_SOURCE);
    EXPECT_STREQ(BitmapClip::action_outcome_reason(C::InvalidGeometry, T::UnsupportedCutter),
                 REASON_INVALID_CUTTER);
    EXPECT_STREQ(BitmapClip::action_outcome_reason(C::InvalidSelection, T::UnsupportedBranch),
                 REASON_UNSUPPORTED_EFFECTS);
}

TEST(DestructiveBitmapClipFeedbackMapping, ResolvedPairFailureUsesTheCommitStatus)
{
    using C = BitmapClip::CommitStatus;
    using T = BitmapClip::TargetStatus;

    EXPECT_STREQ(BitmapClip::action_outcome_reason(C::InvalidSelection, T::Resolved),
                 REASON_UNSUPPORTED_EFFECTS);
    EXPECT_STREQ(BitmapClip::action_outcome_reason(C::UnsupportedTrim, T::Resolved),
                 REASON_UNSUPPORTED_TRIM);
    EXPECT_STREQ(BitmapClip::action_outcome_reason(C::RasterizationFailed, T::Resolved),
                 REASON_RASTERIZATION_FAILED);
    EXPECT_STREQ(BitmapClip::action_outcome_reason(C::EncodingFailed, T::Resolved),
                 REASON_ENCODING_FAILED);
    EXPECT_STREQ(BitmapClip::action_outcome_reason(C::Cancelled, T::Resolved), REASON_CANCELLED);
    EXPECT_STREQ(BitmapClip::action_outcome_reason(C::InvalidGeometry, T::Resolved),
                 REASON_MISSING_SOURCE);
}

TEST(DestructiveBitmapClipFeedbackMapping, FailureReasonsAreDistinctAndNeverBareNoSelection)
{
    char const *failures[] = {
        REASON_INVALID_SELECTION, REASON_MISSING_SOURCE, REASON_INVALID_CUTTER,
        REASON_UNSUPPORTED_EFFECTS, REASON_UNSUPPORTED_TRIM, REASON_PROTECTED_OBJECT,
        REASON_RASTERIZATION_FAILED, REASON_ENCODING_FAILED, REASON_CANCELLED,
    };
    char const *invalid_selection_message = BitmapClip::action_outcome_message(REASON_INVALID_SELECTION, false);
    ASSERT_NE(invalid_selection_message, nullptr);
    // Wording checks are only meaningful when no translation catalog is active;
    // the reason-key mapping above is the locale-independent contract.
    bool const untranslated =
        std::strcmp(invalid_selection_message,
                    "Select exactly one bitmap and one closed vector cutter; no changes were made.") == 0;

    std::unordered_set<std::string> seen;
    for (auto const *reason : failures) {
        char const *message = BitmapClip::action_outcome_message(reason, false);
        ASSERT_NE(message, nullptr) << "reason " << reason << " has no message";
        EXPECT_FALSE(std::string(message).empty()) << "reason " << reason << " has an empty message";
        if (untranslated) {
            EXPECT_NE(std::string(message).find("no changes were made"), std::string::npos)
                << "failure reason " << reason << " must state the document is unchanged: " << message;
        }
        EXPECT_TRUE(seen.insert(message).second) << "duplicate failure message: " << message;
        if (std::strcmp(reason, REASON_INVALID_SELECTION) != 0) {
            // The action must not claim that every failure is simply "no selection".
            EXPECT_STRNE(message, invalid_selection_message)
                << "reason " << reason << " collapses into the invalid-selection message";
        }
    }
}

TEST(DestructiveBitmapClipFeedbackMapping, MessageSeverityMatchesTheOutcome)
{
    EXPECT_EQ(BitmapClip::action_outcome_message_type(REASON_SUCCESS), NORMAL_MESSAGE);
    EXPECT_EQ(BitmapClip::action_outcome_message_type(REASON_NO_CHANGE), NORMAL_MESSAGE);
    EXPECT_EQ(BitmapClip::action_outcome_message_type(REASON_SUCCESS_FULLY_TRANSPARENT),
              WARNING_MESSAGE);
    EXPECT_EQ(BitmapClip::action_outcome_message_type(REASON_UNSUPPORTED_EFFECTS),
              WARNING_MESSAGE);
    EXPECT_EQ(BitmapClip::action_outcome_message_type(REASON_UNSUPPORTED_TRIM),
              WARNING_MESSAGE);
    EXPECT_EQ(BitmapClip::action_outcome_message_type(REASON_PROTECTED_OBJECT), ERROR_MESSAGE);
    EXPECT_EQ(BitmapClip::action_outcome_message_type(REASON_CANCELLED), WARNING_MESSAGE);
    EXPECT_EQ(BitmapClip::action_outcome_message_type(REASON_INVALID_SELECTION), ERROR_MESSAGE);
    EXPECT_EQ(BitmapClip::action_outcome_message_type(REASON_MISSING_SOURCE), ERROR_MESSAGE);
    EXPECT_EQ(BitmapClip::action_outcome_message_type(REASON_INVALID_CUTTER), ERROR_MESSAGE);
    EXPECT_EQ(BitmapClip::action_outcome_message_type(REASON_RASTERIZATION_FAILED),
              ERROR_MESSAGE);
    EXPECT_EQ(BitmapClip::action_outcome_message_type(REASON_ENCODING_FAILED), ERROR_MESSAGE);
    EXPECT_EQ(BitmapClip::action_outcome_message_type(nullptr), ERROR_MESSAGE);
}

TEST(DestructiveBitmapClipFeedbackMapping, InverseSuccessUsesDistinctWording)
{
    EXPECT_STRNE(BitmapClip::action_outcome_message(REASON_SUCCESS, false),
                 BitmapClip::action_outcome_message(REASON_SUCCESS, true));
    EXPECT_STRNE(BitmapClip::action_outcome_message(REASON_SUCCESS_FULLY_TRANSPARENT, false),
                 BitmapClip::action_outcome_message(REASON_SUCCESS_FULLY_TRANSPARENT, true));
}

// A resolved pair that the commit path rejects as InvalidSelection is the
// wrapper/shared-reference rejection, not a bad pair. It must classify as
// unsupported effects/references; the genuinely unresolved pair statuses above
// keep their invalid-selection reason.
TEST(DestructiveBitmapClipFeedbackMapping, ResolvedInvalidSelectionIsUnsupportedEffectsOrReferences)
{
    using C = BitmapClip::CommitStatus;
    using T = BitmapClip::TargetStatus;

    EXPECT_STREQ(BitmapClip::action_outcome_reason(C::InvalidSelection, T::Resolved),
                 REASON_UNSUPPORTED_EFFECTS);
    EXPECT_STRNE(BitmapClip::action_outcome_reason(C::InvalidSelection, T::Resolved),
                 REASON_INVALID_SELECTION);
    EXPECT_STREQ(BitmapClip::action_outcome_reason(C::InvalidSelection, T::UnsupportedBranch),
                 REASON_UNSUPPORTED_EFFECTS);
}

// A valid pair with a hidden/locked member is not "select exactly one"; it has
// its own reason and must tell the user to reveal/unlock the object.
TEST(DestructiveBitmapClipFeedbackMapping, ProtectedObjectHasItsOwnVisibleUnlockWording)
{
    using C = BitmapClip::CommitStatus;
    using T = BitmapClip::TargetStatus;

    EXPECT_STREQ(BitmapClip::action_outcome_reason(C::InvalidSelection, T::ProtectedObject),
                 REASON_PROTECTED_OBJECT);

    char const *message = BitmapClip::action_outcome_message(REASON_PROTECTED_OBJECT, false);
    ASSERT_NE(message, nullptr);
    EXPECT_TRUE(std::string(message).find("select exactly one") == std::string::npos)
        << "protected-object must not reuse the invalid-selection wording: " << message;
    EXPECT_STRNE(message, BitmapClip::action_outcome_message(REASON_INVALID_SELECTION, false));

    bool const untranslated =
        std::strcmp(BitmapClip::action_outcome_message(REASON_INVALID_SELECTION, false),
                    "Select exactly one bitmap and one closed vector cutter; no changes were made.") == 0;
    if (untranslated) {
        std::string const text(message);
        EXPECT_NE(text.find("visible"), std::string::npos)
            << "protected-object must say the object has to be made visible: " << message;
        EXPECT_NE(text.find("unlock"), std::string::npos)
            << "protected-object must say the object has to be unlocked: " << message;
        EXPECT_NE(text.find("no changes were made"), std::string::npos)
            << "protected-object must state the document is unchanged: " << message;
    }
}

// UnsupportedTrim keeps the prior viewport wording (Viewport/effects/references)
// and still states that nothing was changed.
TEST(DestructiveBitmapClipFeedbackMapping, UnsupportedTrimKeepsViewportWordingAndNoMutation)
{
    char const *message = BitmapClip::action_outcome_message(REASON_UNSUPPORTED_TRIM, false);
    ASSERT_NE(message, nullptr);
    EXPECT_STRNE(message, BitmapClip::action_outcome_message(REASON_UNSUPPORTED_EFFECTS, false));

    bool const untranslated =
        std::strcmp(BitmapClip::action_outcome_message(REASON_INVALID_SELECTION, false),
                    "Select exactly one bitmap and one closed vector cutter; no changes were made.") == 0;
    if (untranslated) {
        std::string const text(message);
        EXPECT_NE(text.find("viewport"), std::string::npos)
            << "unsupported-trim must retain the prior viewport wording: " << message;
        EXPECT_NE(text.find("no changes were made"), std::string::npos)
            << "unsupported-trim must retain the no-mutation statement: " << message;
    }
}

// ---------------------------------------------------------------------------
// Outcome + no-op-on-failure contract through the accepted B04 entry point.
// ---------------------------------------------------------------------------

class DestructiveBitmapClipFeedbackOutcomeTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        auto const *datadir = g_getenv("INKSCAPE_DATADIR");
        ASSERT_TRUE(datadir && *datadir)
            << "Run this integration test through CTest so INKSCAPE_DATADIR is configured";
        auto const units = std::string(datadir) + "/inkscape/ui/units.xml";
        ASSERT_TRUE(g_file_test(units.c_str(), G_FILE_TEST_IS_REGULAR))
            << "INKSCAPE_DATADIR does not contain the test share tree";

        application = initialize_application();
        ASSERT_TRUE(application);
        if (!Application::exists()) {
            Application::create(false);
        }

        auto source = solid_pixbuf(8, 8, 0x336699ff);
        ASSERT_TRUE(source);
        source_uri = png_uri(*source);
        ASSERT_FALSE(source_uri.empty());
    }

    void TearDown() override
    {
        close_document();
    }

    void open_document(std::string const &svg)
    {
        document = SPDocument::createNewDocFromMem(std::span<char const>(svg.data(), svg.size()));
        ASSERT_TRUE(document);
        document->ensureUpToDate();
        desktop = std::make_unique<SPDesktop>(document->getNamedView());
        ASSERT_TRUE(desktop);
        INKSCAPE.add_desktop(desktop.get());
        desktop_registered = true;
        application->set_active_desktop(desktop.get());
        application->set_active_document(document.get());
        application->set_active_selection(desktop->getSelection());
        desktop->getSelection()->clear();
        document->ensureUpToDate();
    }

    void close_document()
    {
        if (application) {
            application->set_active_selection(nullptr);
            application->set_active_document(nullptr);
            application->set_active_desktop(nullptr);
        }
        if (desktop_registered && desktop && Application::exists()) {
            INKSCAPE.remove_desktop(desktop.get());
            desktop_registered = false;
        }
        desktop.reset();
        document.reset();
    }

    std::vector<SPItem *> selected_items() const
    {
        std::vector<SPItem *> items;
        if (auto *selection = desktop ? desktop->getSelection() : nullptr) {
            for (auto *item : selection->items()) {
                items.push_back(item);
            }
        }
        return items;
    }

    // Run one action attempt and assert the failure is an atomic no-op that maps
    // to `expected_reason`.
    void expect_failure_is_atomic(std::vector<SPItem *> const &selection, char const *expected_status,
                                  char const *expected_reason)
    {
        desktop->getSelection()->setList(std::vector<SPItem *>(selection));
        document->ensureUpToDate();

        auto const xml_before = live_xml(document->getReprDoc());
        auto const selected_before = selected_items();
        auto const modified_before = document->isModifiedSinceSave();

        auto const resolved = BitmapClip::resolve_targets(*desktop->getSelection());
        auto const status = BitmapClip::commit_selection(*desktop->getSelection(), BitmapClip::Mode::KeepInside);
        document->ensureUpToDate();

        EXPECT_STREQ(BitmapClip::action_outcome_reason(status, resolved.status), expected_reason)
            << "status=" << static_cast<int>(status)
            << " target=" << static_cast<int>(resolved.status) << " expected " << expected_status;

        EXPECT_EQ(live_xml(document->getReprDoc()), xml_before)
            << "a failed attempt must leave the document byte-identical";
        EXPECT_EQ(selected_items(), selected_before)
            << "a failed attempt must not change the selection";
        EXPECT_EQ(document->isModifiedSinceSave(), modified_before)
            << "a failed attempt must not mark the document modified";
        EXPECT_FALSE(DocumentUndo::undo(document.get()))
            << "a failed attempt must not create an Undo entry";
    }

    InkscapeApplication *application = nullptr;
    std::unique_ptr<SPDocument> document;
    std::unique_ptr<SPDesktop> desktop;
    bool desktop_registered = false;
    std::string source_uri;
};

TEST_F(DestructiveBitmapClipFeedbackOutcomeTest, InvalidSingleItemSelectionIsAtomic)
{
    open_document(Glib::ustring::compose(
        R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="20" height="12">
 <image id="bitmap" x="1" y="1" width="8" height="8" preserveAspectRatio="none" href="%1"/>
 <rect id="cutter" x="3" y="3" width="4" height="4" fill="#ff00ff"/>
</svg>)svg",
        source_uri));

    ASSERT_TRUE(document->getObjectById("bitmap"));
    ASSERT_TRUE(document->getObjectById("cutter"));

    expect_failure_is_atomic({cast<SPItem>(document->getObjectById("bitmap"))},
                             "InvalidSelection", REASON_INVALID_SELECTION);
}

TEST_F(DestructiveBitmapClipFeedbackOutcomeTest, MissingSourceIsAtomicAndReportsMissingSource)
{
    open_document(R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="20" height="12">
 <image id="bitmap" x="1" y="1" width="8" height="8" preserveAspectRatio="none"
        href="file:///nonexistent/b05-missing-source.png"/>
 <rect id="cutter" x="3" y="3" width="4" height="4" fill="#ff00ff"/>
</svg>)svg");

    auto *image = document->getObjectById("bitmap");
    auto *cutter = document->getObjectById("cutter");
    ASSERT_TRUE(image);
    ASSERT_TRUE(cutter);
    auto *sp_image = cast<SPImage>(image);
    ASSERT_TRUE(sp_image->missing || sp_image->pixbuf == nullptr)
        << "fixture must actually be missing its decoded source (missing="
        << sp_image->missing << ", pixbuf=" << (sp_image->pixbuf ? "set" : "null") << ")";

    expect_failure_is_atomic({cast<SPItem>(image), cast<SPItem>(cutter)},
                             "InvalidSelection", REASON_MISSING_SOURCE);
}

TEST_F(DestructiveBitmapClipFeedbackOutcomeTest, OpenCutterIsAtomicAndReportsInvalidCutter)
{
    open_document(Glib::ustring::compose(
        R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="20" height="12">
 <image id="bitmap" x="1" y="1" width="8" height="8" preserveAspectRatio="none" href="%1"/>
 <path id="cutter" d="M 3,3 L 4,4" fill="none" stroke="#ff00ff" stroke-width="0.2"/>
</svg>)svg",
        source_uri));

    auto *image = document->getObjectById("bitmap");
    auto *cutter = document->getObjectById("cutter");
    ASSERT_TRUE(image);
    ASSERT_TRUE(cutter);

    expect_failure_is_atomic({cast<SPItem>(image), cast<SPItem>(cutter)},
                             "InvalidGeometry", REASON_INVALID_CUTTER);
}

TEST_F(DestructiveBitmapClipFeedbackOutcomeTest, CancellationIsAtomicAndReportedSeparately)
{
    open_document(Glib::ustring::compose(
        R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="20" height="12">
 <image id="bitmap" x="1" y="1" width="8" height="8" preserveAspectRatio="none" href="%1"/>
 <rect id="cutter" x="3" y="3" width="4" height="4" fill="#ff00ff"/>
</svg>)svg",
        source_uri));

    auto *image = cast<SPItem>(document->getObjectById("bitmap"));
    auto *cutter = cast<SPItem>(document->getObjectById("cutter"));
    ASSERT_TRUE(image);
    ASSERT_TRUE(cutter);
    desktop->getSelection()->setList(std::vector<SPItem *>{image, cutter});
    document->ensureUpToDate();

    auto const xml_before = live_xml(document->getReprDoc());
    auto const selected_before = selected_items();

    std::stop_source cancellation;
    cancellation.request_stop();
    auto const resolved = BitmapClip::resolve_targets(*desktop->getSelection());
    auto const status = BitmapClip::commit_selection(*desktop->getSelection(), BitmapClip::Mode::KeepInside,
                                                     cancellation.get_token());
    document->ensureUpToDate();

    ASSERT_EQ(status, BitmapClip::CommitStatus::Cancelled);
    EXPECT_STREQ(BitmapClip::action_outcome_reason(status, resolved.status), REASON_CANCELLED);
    EXPECT_EQ(live_xml(document->getReprDoc()), xml_before);
    EXPECT_EQ(selected_items(), selected_before);
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

TEST_F(DestructiveBitmapClipFeedbackOutcomeTest, ValidPairReportsSuccessAndCreatesOneUndo)
{
    open_document(Glib::ustring::compose(
        R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="20" height="12">
 <image id="bitmap" x="1" y="1" width="8" height="8" preserveAspectRatio="none" href="%1"/>
 <rect id="cutter" x="3" y="3" width="4" height="4" fill="#ff00ff"/>
</svg>)svg",
        source_uri));

    auto *image = cast<SPItem>(document->getObjectById("bitmap"));
    auto *cutter = cast<SPItem>(document->getObjectById("cutter"));
    ASSERT_TRUE(image);
    ASSERT_TRUE(cutter);
    desktop->getSelection()->setList(std::vector<SPItem *>{image, cutter});
    document->ensureUpToDate();

    auto const xml_before = live_xml(document->getReprDoc());
    auto const status = BitmapClip::commit_selection(*desktop->getSelection(), BitmapClip::Mode::KeepInside);
    document->ensureUpToDate();

    ASSERT_EQ(status, BitmapClip::CommitStatus::Committed);
    EXPECT_STREQ(BitmapClip::action_outcome_reason(status, BitmapClip::TargetStatus::Resolved),
                 REASON_SUCCESS);
    EXPECT_NE(live_xml(document->getReprDoc()), xml_before) << "committing must change the document";
    EXPECT_EQ(selected_items().size(), 1u) << "the surviving bitmap is selected";
    EXPECT_TRUE(DocumentUndo::undo(document.get())) << "success must create exactly one Undo entry";
    document->ensureUpToDate();
    EXPECT_FALSE(DocumentUndo::undo(document.get())) << "success must create exactly one Undo entry";
}

TEST_F(DestructiveBitmapClipFeedbackOutcomeTest, FullyTransparentResultIsASuccessfulConversion)
{
    open_document(Glib::ustring::compose(
        R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="20" height="12">
 <image id="bitmap" x="1" y="1" width="8" height="8" preserveAspectRatio="none" href="%1"/>
 <rect id="cutter" x="50" y="50" width="4" height="4" fill="#ff00ff"/>
</svg>)svg",
        source_uri));

    auto *image = cast<SPItem>(document->getObjectById("bitmap"));
    auto *cutter = cast<SPItem>(document->getObjectById("cutter"));
    ASSERT_TRUE(image);
    ASSERT_TRUE(cutter);
    desktop->getSelection()->setList(std::vector<SPItem *>{image, cutter});
    document->ensureUpToDate();

    auto const status = BitmapClip::commit_selection(*desktop->getSelection(), BitmapClip::Mode::KeepInside);
    document->ensureUpToDate();

    ASSERT_EQ(status, BitmapClip::CommitStatus::CommittedAllTransparent);
    EXPECT_STREQ(BitmapClip::action_outcome_reason(status, BitmapClip::TargetStatus::Resolved),
                 REASON_SUCCESS_FULLY_TRANSPARENT);
    EXPECT_EQ(selected_items().size(), 1u) << "transparent result is still one selected bitmap";
    EXPECT_TRUE(DocumentUndo::undo(document.get())) << "transparent conversion is a real change";
}

TEST(DestructiveBitmapClipFeedbackMapping, StraightenedReasonsSeverityAndMessages)
{
    using C=BitmapClip::CommitStatus; using T=BitmapClip::TargetStatus;
    struct Outcome { C status; char const *reason; MessageType severity; bool success; };
    for (auto const &o : {
        Outcome{C::CommittedStraightened,"success-straightened",NORMAL_MESSAGE,true},
        Outcome{C::CommittedStraightenedAllTransparent,"success-straightened-fully-transparent",WARNING_MESSAGE,true},
        Outcome{C::StraighteningEmptyRegion,"straightening-empty-region",WARNING_MESSAGE,false},
        Outcome{C::StraighteningTooLarge,"straightening-too-large",WARNING_MESSAGE,false}}) {
        SCOPED_TRACE(o.reason);
        EXPECT_STREQ(BitmapClip::action_outcome_reason(o.status,T::Resolved),o.reason);
        EXPECT_EQ(BitmapClip::action_outcome_message_type(o.reason),o.severity);
        for (bool inverse : {false,true}) {
            auto const *message=BitmapClip::action_outcome_message(o.reason,inverse);
            ASSERT_NE(message,nullptr); EXPECT_STRNE(message,"");
            if (!o.success) EXPECT_NE(std::string(message).find("no changes were made"),std::string::npos);
        }
        if (o.success) EXPECT_STRNE(BitmapClip::action_outcome_message(o.reason,false),
                                  BitmapClip::action_outcome_message(o.reason,true));
    }
    EXPECT_STREQ(BitmapClip::action_outcome_message("success-straightened-fully-transparent",false),
                 "Bitmap straightened; the clip made it fully transparent.");
    EXPECT_STREQ(BitmapClip::action_outcome_message("success-straightened-fully-transparent",true),
                 "Bitmap straightened; the inverse clip made it fully transparent.");
}
