// SPDX-License-Identifier: GPL-2.0-or-later

#include "path/offset-shapes.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <string>
#include <vector>
#include <gtest/gtest.h>

#include "document.h"
#include "document-undo.h"
#include "inkscape.h"
#include "object/sp-item.h"
#include "xml/repr.h"

using namespace Inkscape;
using namespace Inkscape::OffsetShapes;

namespace {

std::unique_ptr<SPDocument> make_document(char const *body)
{
    auto const svg = std::string{R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="300" height="220">)svg"} +
                     body + "</svg>";
    auto document = SPDocument::createNewDocFromMem(std::span<char const>{svg.data(), svg.size()});
    if (document) {
        document->ensureUpToDate();
        document->setModifiedSinceSave(false);
    }
    return document;
}

SPItem *item(SPDocument &document, char const *id)
{
    return cast<SPItem>(document.getObjectById(id));
}

Geom::Rect bounds(Geom::PathVector const &pathvector)
{
    auto result = pathvector.boundsFast();
    EXPECT_TRUE(result);
    return result.value_or(Geom::Rect{});
}

double directed_sample_distance(Geom::PathVector const &from, Geom::PathVector const &to)
{
    double maximum = 0.0;
    for (auto const &path : from) {
        for (auto const &curve : path) {
            for (unsigned step = 0; step <= 128; ++step) {
                auto const point = curve.pointAt(static_cast<double>(step) / 128.0);
                Geom::Coord distance = 0.0;
                EXPECT_TRUE(to.nearestTime(point, &distance));
                maximum = std::max(maximum, static_cast<double>(distance));
            }
        }
    }
    return maximum;
}

double path_signed_area(Geom::Path const &path)
{
    double area = 0.0;
    Geom::Point centroid;
    Geom::centroid(path.toPwSb(), centroid, area);
    return area;
}

class OffsetShapesTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        if (!Application::exists()) {
            Application::create(false);
        }
    }
};

} // namespace

TEST_F(OffsetShapesTest, PreviewBuildIsNonMutatingAndOutwardExpands)
{
    auto document = make_document(R"svg(<rect id="part" x="20" y="30" width="40" height="20"/>)svg");
    ASSERT_TRUE(document);
    auto const before = sp_repr_save_buf(document->getReprDoc()).raw();
    std::vector<SPItem *> selected{item(*document, "part")};

    auto prepared = prepare(selected);
    ASSERT_TRUE(prepared) << prepared.error;
    Options options;
    options.distance_px = 5.0;
    auto result = build(prepared.sources, options);

    ASSERT_TRUE(result) << result.error;
    ASSERT_EQ(result.results.size(), 1);
    auto const result_bounds = bounds(result.results.front().geometry_document);
    EXPECT_NEAR(result_bounds.left(), 15.0, 0.05);
    EXPECT_NEAR(result_bounds.top(), 25.0, 0.05);
    EXPECT_NEAR(result_bounds.right(), 65.0, 0.05);
    EXPECT_NEAR(result_bounds.bottom(), 55.0, 0.05);
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before);
    EXPECT_FALSE(document->isModifiedSinceSave());
}

TEST_F(OffsetShapesTest, InwardAndBothUseOriginalGeometryIndependently)
{
    auto document = make_document(R"svg(<rect id="part" x="20" y="30" width="40" height="20"/>)svg");
    ASSERT_TRUE(document);
    std::vector<SPItem *> selected{item(*document, "part")};
    auto prepared = prepare(selected);
    ASSERT_TRUE(prepared) << prepared.error;
    Options options;
    options.distance_px = 4.0;
    options.direction = Direction::Both;

    auto result = build(prepared.sources, options);

    ASSERT_TRUE(result) << result.error;
    ASSERT_EQ(result.results.size(), 2);
    auto const outward = bounds(result.results[0].geometry_document);
    auto const inward = bounds(result.results[1].geometry_document);
    EXPECT_NEAR(outward.width(), 48.0, 0.05);
    EXPECT_NEAR(outward.height(), 28.0, 0.05);
    EXPECT_NEAR(inward.width(), 32.0, 0.05);
    EXPECT_NEAR(inward.height(), 12.0, 0.05);
}

TEST_F(OffsetShapesTest, GroupGeometryAndOuterOnlyAreCombinedWithoutChangingChildren)
{
    auto document = make_document(R"svg(
      <g id="group"><rect id="a" x="20" y="20" width="10" height="10"/>
        <rect id="b" x="35" y="20" width="10" height="10"/></g>
      <rect id="other" x="55" y="20" width="10" height="10"/>)svg");
    ASSERT_TRUE(document);
    auto const before = sp_repr_save_buf(document->getReprDoc()).raw();
    std::vector<SPItem *> selected{item(*document, "group"), item(*document, "other")};
    auto prepared = prepare(selected);
    ASSERT_TRUE(prepared) << prepared.error;
    EXPECT_EQ(prepared.sources.front().geometry_document.size(), 2);
    Options options;
    options.distance_px = 2.0;
    options.outer_shapes_only = true;

    auto result = build(prepared.sources, options);

    ASSERT_TRUE(result) << result.error;
    EXPECT_EQ(result.results.size(), 1);
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before);
}

TEST_F(OffsetShapesTest, OpenPathsAndZeroDistanceFailClosed)
{
    auto document = make_document(R"svg(<path id="part" d="M10 10H40"/>)svg");
    ASSERT_TRUE(document);
    std::vector<SPItem *> selected{item(*document, "part")};
    auto prepared = prepare(selected);
    EXPECT_FALSE(prepared);

    auto closed_document = make_document(R"svg(<rect id="part" width="20" height="20"/>)svg");
    ASSERT_TRUE(closed_document);
    selected = {item(*closed_document, "part")};
    prepared = prepare(selected);
    ASSERT_TRUE(prepared);
    Options options;
    options.distance_px = 0.0;
    EXPECT_FALSE(build(prepared.sources, options));
}

TEST_F(OffsetShapesTest, ExternalGeometryChangeInvalidatesFrozenSources)
{
    auto document = make_document(R"svg(<rect id="part" width="20" height="20"/>)svg");
    ASSERT_TRUE(document);
    std::vector<SPItem *> selected{item(*document, "part")};
    auto prepared = prepare(selected);
    ASSERT_TRUE(prepared);
    EXPECT_TRUE(geometry_still_matches(prepared.sources));

    item(*document, "part")->getRepr()->setAttribute("width", "30");
    document->ensureUpToDate();

    EXPECT_FALSE(geometry_still_matches(prepared.sources));
}

TEST_F(OffsetShapesTest, MixedSelectionSkipsUnsupportedObjectsAndOpenSubpaths)
{
    auto document = make_document(R"svg(
      <rect id="closed" x="10" y="10" width="20" height="20"/>
      <path id="open" d="M50 10H80"/>
      <path id="mixed" d="M100 10h20v20h-20z M100 50h20"/>
      <image id="bitmap" width="10" height="10" href="data:image/png;base64,"/>)svg");
    ASSERT_TRUE(document);
    std::vector<SPItem *> selected{item(*document, "closed"), item(*document, "open"),
                                   item(*document, "mixed"), item(*document, "bitmap")};

    auto prepared = prepare(selected);

    ASSERT_TRUE(prepared) << prepared.error;
    EXPECT_EQ(prepared.sources.size(), 2);
    EXPECT_EQ(prepared.skipped_count, 2);
    EXPECT_EQ(prepared.open_subpaths_skipped, 2);
    EXPECT_TRUE(build(prepared.sources, Options{}));
}

TEST_F(OffsetShapesTest, VectorCloneIsResolvedWithoutUnlinking)
{
    auto document = make_document(R"svg(
      <defs><rect id="original" width="20" height="10"/></defs>
      <use id="clone" href="#original" x="40" y="30"/>)svg");
    ASSERT_TRUE(document);
    auto const before = sp_repr_save_buf(document->getReprDoc()).raw();
    std::vector<SPItem *> selected{item(*document, "clone")};

    auto prepared = prepare(selected);
    ASSERT_TRUE(prepared) << prepared.error;
    auto result = build(prepared.sources, Options{});

    EXPECT_TRUE(result) << result.error;
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before);
    EXPECT_NE(item(*document, "clone"), nullptr);
    EXPECT_NE(document->getObjectById("original"), nullptr);
}

TEST_F(OffsetShapesTest, MaskedAndFilteredSourcesAreSkippedSafely)
{
    auto document = make_document(R"svg(
      <defs>
        <mask id="mask"><rect width="20" height="20" fill="white"/></mask>
        <filter id="blur"><feGaussianBlur stdDeviation="2"/></filter>
      </defs>
      <rect id="good" width="20" height="20"/>
      <rect id="masked" x="30" width="20" height="20" mask="url(#mask)"/>
      <rect id="filtered" x="60" width="20" height="20" filter="url(#blur)"/>)svg");
    ASSERT_TRUE(document);
    std::vector<SPItem *> selected{item(*document, "good"), item(*document, "masked"), item(*document, "filtered")};

    auto prepared = prepare(selected);

    ASSERT_TRUE(prepared) << prepared.error;
    EXPECT_EQ(prepared.sources.size(), 1);
    EXPECT_EQ(prepared.skipped_count, 2);
}

TEST_F(OffsetShapesTest, OuterOnlyRemovesInteriorHole)
{
    auto document = make_document(
        R"svg(<path id="donut" fill-rule="evenodd" d="M10 10h100v100H10z M35 35h50v50H35z"/>)svg");
    ASSERT_TRUE(document);
    std::vector<SPItem *> selected{item(*document, "donut")};
    auto prepared = prepare(selected);
    ASSERT_TRUE(prepared);
    Options options;
    options.distance_px = 2;
    options.outer_shapes_only = true;

    auto result = build(prepared.sources, options);

    ASSERT_TRUE(result) << result.error;
    ASSERT_EQ(result.results.size(), 1);
    EXPECT_EQ(result.results.front().geometry_document.size(), 1);
}

TEST_F(OffsetShapesTest, SimplificationUsesExplicitToleranceAndRemainsClosed)
{
    auto document = make_document(
        R"svg(<path id="part" d="M10 10L20 10L30 10L40 10L40 40L10 40Z"/>)svg");
    ASSERT_TRUE(document);
    std::vector<SPItem *> selected{item(*document, "part")};
    auto prepared = prepare(selected);
    ASSERT_TRUE(prepared);
    Options options;
    options.distance_px = 2;
    options.simplify_results = true;
    options.simplify_tolerance_px = 0.01;

    auto result = build(prepared.sources, options);

    ASSERT_TRUE(result) << result.error;
    for (auto const &path : result.results.front().geometry_document) EXPECT_TRUE(path.closed());
    options.simplify_tolerance_px = 0;
    EXPECT_FALSE(build(prepared.sources, options));
}

TEST_F(OffsetShapesTest, SimplificationStaysWithinBidirectionalGeometricTolerance)
{
    auto document = make_document(
        R"svg(<path id="part" d="M20,80 C20,20 120,20 120,80 C120,140 20,140 20,80 Z"/>)svg");
    ASSERT_TRUE(document);
    std::vector<SPItem *> selected{item(*document, "part")};
    auto prepared = prepare(selected);
    ASSERT_TRUE(prepared);
    Options exact_options;
    exact_options.distance_px = 7;
    exact_options.corner = Corner::Round;
    auto exact = build(prepared.sources, exact_options);
    ASSERT_TRUE(exact) << exact.error;

    auto simplified_options = exact_options;
    simplified_options.simplify_results = true;
    simplified_options.simplify_tolerance_px = 0.05;
    auto simplified = build(prepared.sources, simplified_options);
    ASSERT_TRUE(simplified) << simplified.error;
    ASSERT_EQ(exact.results.size(), simplified.results.size());

    auto const &reference = exact.results.front().geometry_document;
    auto const &candidate = simplified.results.front().geometry_document;
    EXPECT_LE(directed_sample_distance(reference, candidate), 0.050001);
    EXPECT_LE(directed_sample_distance(candidate, reference), 0.050001);
    ASSERT_EQ(reference.size(), candidate.size());
    for (std::size_t index = 0; index < reference.size(); ++index) {
        EXPECT_EQ(std::signbit(path_signed_area(reference[index])),
                  std::signbit(path_signed_area(candidate[index])));
    }
}

TEST_F(OffsetShapesTest, OffsetShapesCommitHeadless)
{
    // The shared commit() places every new path right after its source in the same parent and
    // leaves exactly one Undo step; the tool and the command-line action both use it.
    {
        auto document = make_document(R"svg(
          <rect id="a" x="10" y="10" width="20" height="20"/>
          <rect id="b" x="50" y="10" width="20" height="20"/>)svg");
        ASSERT_TRUE(document);
        std::vector<SPItem *> selected{item(*document, "a"), item(*document, "b")};
        auto prepared = prepare(selected);
        ASSERT_TRUE(prepared) << prepared.error;
        Options options;
        options.distance_px = 2.0;
        auto built = build(prepared.sources, options);
        ASSERT_TRUE(built) << built.error;

        auto *root = document->getReprDoc()->root();
        unsigned const before_children = root->childCount();

        auto committed = commit(document.get(), prepared, built, options, CommitProtocol::CommandLine);

        ASSERT_TRUE(committed) << committed.error;
        ASSERT_EQ(committed.created.size(), 2u);
        EXPECT_TRUE(committed.deleted.empty());
        EXPECT_TRUE(committed.deleted_ids.empty());

        ASSERT_EQ(root->childCount(), before_children + 2);
        for (std::size_t index = 0; index < committed.created.size(); ++index) {
            auto *created_repr = committed.created[index]->getRepr();
            ASSERT_TRUE(created_repr);
            EXPECT_EQ(created_repr->parent(), root);
            EXPECT_EQ(created_repr->prev(), selected[index]->getRepr());
            EXPECT_EQ(selected[index]->getRepr()->next(), created_repr);
        }

        EXPECT_TRUE(DocumentUndo::undo(document.get()));
        document->ensureUpToDate();
        EXPECT_EQ(document->getReprDoc()->root()->childCount(), before_children);
        EXPECT_FALSE(DocumentUndo::undo(document.get()));
    }

    // delete_originals removes exactly the produced sources (in source order) and reports their ids.
    {
        auto document = make_document(R"svg(
          <rect id="a" x="10" y="10" width="20" height="20"/>
          <rect id="b" x="50" y="10" width="20" height="20"/>)svg");
        ASSERT_TRUE(document);
        std::vector<SPItem *> selected{item(*document, "a"), item(*document, "b")};
        auto prepared = prepare(selected);
        ASSERT_TRUE(prepared) << prepared.error;
        Options options;
        options.distance_px = 2.0;
        options.delete_originals = true;
        auto built = build(prepared.sources, options);
        ASSERT_TRUE(built) << built.error;

        auto committed = commit(document.get(), prepared, built, options, CommitProtocol::CommandLine);

        ASSERT_TRUE(committed) << committed.error;
        EXPECT_EQ(committed.created.size(), 2u);
        EXPECT_EQ(committed.deleted_ids, (std::vector<std::string>{"a", "b"}));
        EXPECT_EQ(document->getObjectById("a"), nullptr);
        EXPECT_EQ(document->getObjectById("b"), nullptr);

        EXPECT_TRUE(DocumentUndo::undo(document.get()));
        document->ensureUpToDate();
        EXPECT_NE(document->getObjectById("a"), nullptr);
        EXPECT_NE(document->getObjectById("b"), nullptr);
    }
}

TEST_F(OffsetShapesTest, CommandLineUnderOperationLeaseCommitsExactlyOneUndoStep)
{
    // InkscapeApplication::process_document holds the operation lease for the whole command-line
    // scope. CommitProtocol::CommandLine never takes a rollbackable interaction, so it must still
    // record exactly one Undo step under that lease while CommitProtocol::Interaction would be
    // refused.
    auto document = make_document(R"svg(<rect id="part" x="10" y="10" width="20" height="20"/>)svg");
    ASSERT_TRUE(document);
    std::vector<SPItem *> selected{item(*document, "part")};
    auto prepared = prepare(selected);
    ASSERT_TRUE(prepared) << prepared.error;
    Options options;
    options.distance_px = 2.0;
    auto built = build(prepared.sources, options);
    ASSERT_TRUE(built) << built.error;

    auto const before = sp_repr_save_buf(document->getReprDoc()).raw();
    unsigned const before_children = document->getReprDoc()->root()->childCount();

    auto operation = DocumentUndo::holdInteractionOperation(document.get());
    ASSERT_TRUE(operation);

    auto committed = commit(document.get(), prepared, built, options, CommitProtocol::CommandLine);
    ASSERT_TRUE(committed) << committed.error;
    ASSERT_EQ(committed.created.size(), 1u);
    document->ensureUpToDate();
    EXPECT_EQ(document->getReprDoc()->root()->childCount(), before_children + 1);

    EXPECT_TRUE(DocumentUndo::undo(document.get()));
    document->ensureUpToDate();
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before);
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

TEST_F(OffsetShapesTest, InteractionIsRefusedUnderOperationLease)
{
    auto document = make_document(R"svg(<rect id="part" x="10" y="10" width="20" height="20"/>)svg");
    ASSERT_TRUE(document);
    std::vector<SPItem *> selected{item(*document, "part")};
    auto prepared = prepare(selected);
    ASSERT_TRUE(prepared) << prepared.error;
    Options options;
    options.distance_px = 2.0;
    auto built = build(prepared.sources, options);
    ASSERT_TRUE(built) << built.error;

    auto const before = sp_repr_save_buf(document->getReprDoc()).raw();
    auto operation = DocumentUndo::holdInteractionOperation(document.get());
    ASSERT_TRUE(operation);

    auto committed = commit(document.get(), prepared, built, options, CommitProtocol::Interaction);
    EXPECT_FALSE(committed);
    EXPECT_EQ(committed.error, "another document interaction is active");
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before);
    EXPECT_FALSE(document->isPartial());
}

TEST_F(OffsetShapesTest, BeforeMutationCallbackRunsOnceRightBeforeTheFirstMutation)
{
    auto document = make_document(R"svg(<rect id="a" x="10" y="10" width="20" height="20"/>)svg");
    ASSERT_TRUE(document);
    std::vector<SPItem *> selected{item(*document, "a")};
    auto prepared = prepare(selected);
    ASSERT_TRUE(prepared) << prepared.error;
    Options options;
    options.distance_px = 2.0;
    auto built = build(prepared.sources, options);
    ASSERT_TRUE(built) << built.error;
    auto *root = document->getReprDoc()->root();
    unsigned const before_children = root->childCount();

    unsigned calls = 0;
    unsigned children_when_called = 0;
    auto committed = commit(document.get(), prepared, built, options, CommitProtocol::CommandLine,
                            [&] {
                                ++calls;
                                children_when_called = root->childCount();
                            });
    ASSERT_TRUE(committed) << committed.error;
    EXPECT_EQ(calls, 1u);
    EXPECT_EQ(children_when_called, before_children); // ran before the first path was inserted
    EXPECT_EQ(root->childCount(), before_children + 1);
    EXPECT_TRUE(committed.mutation_started); // the mutation began, so a live tool must leave on failure
}

TEST_F(OffsetShapesTest, PreMutationRefusalDoesNotCallBeforeMutationOrStartAMutation)
{
    // A refusal before the first document mutation (here: outer-only sources in different parents)
    // must not arm the caller's guard, so a live tool stays connected to selection changes.
    auto document = make_document(R"svg(
      <g id="g1"><rect id="a" x="10" y="10" width="20" height="20"/></g>
      <g id="g2"><rect id="b" x="50" y="10" width="20" height="20"/></g>)svg");
    ASSERT_TRUE(document);
    std::vector<SPItem *> selected{item(*document, "a"), item(*document, "b")};
    auto prepared = prepare(selected);
    ASSERT_TRUE(prepared) << prepared.error;
    Options options;
    options.distance_px = 2.0;
    options.outer_shapes_only = true;
    auto built = build(prepared.sources, options);
    ASSERT_TRUE(built) << built.error;
    auto const before = sp_repr_save_buf(document->getReprDoc()).raw();

    unsigned calls = 0;
    auto refused = commit(document.get(), prepared, built, options, CommitProtocol::CommandLine,
                          [&] { ++calls; });
    EXPECT_FALSE(refused);
    EXPECT_EQ(refused.error, "Outer only requires eligible sources to share one editable parent");
    EXPECT_FALSE(refused.mutation_started);
    EXPECT_EQ(calls, 0u);
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before);
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

TEST_F(OffsetShapesTest, RemovedParentBetweenPrepareAndCommitIsRefusedWithoutChanges)
{
    // The reachable failure boundary: a source whose parent is removed between prepare and commit.
    // commit() must refuse atomically, before any mutation, and add no Undo step.
    auto document = make_document(R"svg(
      <g id="parent"><rect id="a" x="10" y="10" width="20" height="20"/></g>)svg");
    ASSERT_TRUE(document);
    std::vector<SPItem *> selected{item(*document, "a")};
    auto prepared = prepare(selected);
    ASSERT_TRUE(prepared) << prepared.error;
    Options options;
    options.distance_px = 2.0;
    auto built = build(prepared.sources, options);
    ASSERT_TRUE(built) << built.error;

    item(*document, "parent")->deleteObject(false);
    document->ensureUpToDate();
    auto const after_removal = sp_repr_save_buf(document->getReprDoc()).raw();
    unsigned const children_after_removal = document->getReprDoc()->root()->childCount();

    auto committed = commit(document.get(), prepared, built, options, CommitProtocol::CommandLine);
    EXPECT_FALSE(committed);
    EXPECT_EQ(committed.error, "an offset source no longer has a valid parent transform");
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), after_removal);
    EXPECT_EQ(document->getReprDoc()->root()->childCount(), children_after_removal);
    EXPECT_FALSE(DocumentUndo::undo(document.get())); // no Undo step was recorded
}

// P2: a non-finite source used to loop forever in livarot.
TEST_F(OffsetShapesTest, NonFiniteSourceIsSkippedAndCounted)
{
    auto document = make_document(
        R"svg(<path id="bad" d="M 10 10 L 1e999 10 L 40 40 Z"/>)svg"
        R"svg(<rect id="good" x="20" y="30" width="40" height="20"/>)svg");
    ASSERT_TRUE(document);
    ASSERT_TRUE(item(*document, "bad"));
    std::vector<SPItem *> selected{item(*document, "bad"), item(*document, "good")};

    auto const started = std::chrono::steady_clock::now();
    auto prepared = prepare(selected);
    Options options;
    options.distance_px = 5.0;
    auto result = build(prepared.sources, options);
    auto const elapsed = std::chrono::steady_clock::now() - started;

    EXPECT_LT(elapsed, std::chrono::seconds(10));
    ASSERT_TRUE(prepared) << prepared.error;
    EXPECT_EQ(prepared.skipped_count, 1u);
    ASSERT_EQ(prepared.sources.size(), 1u);
    ASSERT_TRUE(result) << result.error;
    EXPECT_EQ(result.results.size(), 1u);

    std::vector<SPItem *> only_bad{item(*document, "bad")};
    auto none = prepare(only_bad);
    EXPECT_FALSE(none);
    EXPECT_EQ(none.skipped_count, 1u);
}

// P2: a finite but absurd coordinate overflows livarot (the old code never returned).
TEST_F(OffsetShapesTest, HugeFiniteCoordinateSourceIsSkippedAndCounted)
{
    auto document = make_document(
        R"svg(<path id="huge" d="M 10 10 L 1e300 10 L 40 40 Z"/>)svg"
        R"svg(<rect id="good" x="20" y="30" width="40" height="20"/>)svg");
    ASSERT_TRUE(document);
    ASSERT_TRUE(item(*document, "huge"));
    std::vector<SPItem *> selected{item(*document, "huge"), item(*document, "good")};

    auto const started = std::chrono::steady_clock::now();
    auto prepared = prepare(selected);
    Options options;
    options.distance_px = 5.0;
    auto result = build(prepared.sources, options);
    auto const elapsed = std::chrono::steady_clock::now() - started;

    EXPECT_LT(elapsed, std::chrono::seconds(10));
    ASSERT_TRUE(prepared) << prepared.error;
    EXPECT_EQ(prepared.skipped_count, 1u);
    ASSERT_EQ(prepared.sources.size(), 1u);
    ASSERT_TRUE(result) << result.error;
    EXPECT_EQ(result.results.size(), 1u);
}

// P1: simplification over the work budget is skipped, kept as generated, and counted.
TEST_F(OffsetShapesTest, SimplifyOverWorkBudgetIsCountedNotSilent)
{
    auto document = make_document(R"svg(<rect id="big" x="0" y="0" width="200000" height="100000"/>)svg");
    ASSERT_TRUE(document);
    std::vector<SPItem *> selected{item(*document, "big")};
    auto prepared = prepare(selected);
    ASSERT_TRUE(prepared) << prepared.error;
    Options options;
    options.distance_px = 5.0;
    options.simplify_results = true;
    options.simplify_tolerance_px = 0.05; // 600000 px / 0.05 = 1.2e7 steps, over the 5e6 budget

    auto result = build(prepared.sources, options);

    ASSERT_TRUE(result) << result.error;
    EXPECT_EQ(result.simplify_skipped, 1u);
    for (auto const &path : result.results.front().geometry_document) EXPECT_TRUE(path.closed());

    options.simplify_results = false;
    auto plain = build(prepared.sources, options);
    ASSERT_TRUE(plain);
    EXPECT_EQ(plain.simplify_skipped, 0u);
}

// P1: a tiny tolerance used to make ConvertEvenLines explode before any cap.
TEST_F(OffsetShapesTest, TinySimplifyToleranceIsClampedAndReturnsPromptly)
{
    auto document = make_document(R"svg(<circle id="c" cx="100" cy="100" r="50"/>)svg");
    ASSERT_TRUE(document);
    std::vector<SPItem *> selected{item(*document, "c")};
    auto prepared = prepare(selected);
    ASSERT_TRUE(prepared) << prepared.error;
    Options options;
    options.distance_px = 5.0;
    options.simplify_results = true;
    options.simplify_tolerance_px = 1e-12;

    auto const started = std::chrono::steady_clock::now();
    auto result = build(prepared.sources, options);
    auto const elapsed = std::chrono::steady_clock::now() - started;

    EXPECT_LT(elapsed, std::chrono::seconds(10));
    ASSERT_TRUE(result) << result.error;
    ASSERT_EQ(result.results.size(), 1u);
    EXPECT_FALSE(result.results.front().geometry_document.empty());
}
