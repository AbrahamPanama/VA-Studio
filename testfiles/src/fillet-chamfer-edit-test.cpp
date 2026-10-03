// SPDX-License-Identifier: GPL-2.0-or-later
#include <gtest/gtest.h>
#include <doc-per-case-test.h>
#include <2geom/svg-path-parser.h>
#include <2geom/elliptical-arc.h>
#include <limits>
#include <string_view>
#include "document-undo.h"
#include "helper/geom.h"
#include "live_effects/fillet-chamfer-edit.h"
#include "live_effects/lpe-fillet-chamfer.h"
#include "object/sp-path.h"

namespace CE = Inkscape::LivePathEffect::CornerEdit;
using Inkscape::LivePathEffect::LPEFilletChamfer;
using Inkscape::DocumentUndo;

namespace {
CE::Snapshot fixture(char const *d = "M0,0 H100 V100 H0 Z")
{
    CE::Snapshot result;
    result.path = Geom::parse_svg_path(d);
    PathVectorNodeSatellites native;
    NodeSatellite initial(FILLET);
    initial.has_mirror = true;
    initial.hidden = false;
    initial.steps = 1;
    initial.angle = 100;
    native.recalculateForNewPathVector(result.path, initial);
    result.satellites = native.getNodeSatellites();
    return result;
}

TEST(CornerEdit, SelectedEmptyNeverMeansAll)
{
    auto input = fixture();
    EXPECT_EQ(CE::inspect(input, CE::Scope::Selected).status, CE::Status::NoCorners);
    auto plan = CE::prepare(input, {CE::Scope::Selected, CE::Mode::InverseRound, 10});
    EXPECT_EQ(plan.status, CE::Status::NoCorners);
    EXPECT_TRUE(plan.satellites.empty());
    EXPECT_EQ(CE::inspect(input, CE::Scope::All).count, 4u);
}

TEST(CornerEdit, NativeRadiusConversionAndUntargetedRecords)
{
    auto input = fixture();
    input.selected = {{0, 1}, {0, 1}}; // Duplicate selection is not duplicate work.
    input.satellites[0][3].amount = 7;
    input.satellites[0][3].nodesatellite_type = CHAMFER;
    input.satellites[0][3].steps = 9;
    input.satellites[0][3].selected = true;
    auto before = input.satellites;
    auto plan = CE::prepare(input, {CE::Scope::Selected, CE::Mode::InverseRound, 10});
    ASSERT_EQ(plan.status, CE::Status::Ready);
    EXPECT_EQ(plan.count, 1u);
    EXPECT_NEAR(plan.satellites[0][1].amount, 10, 1e-7);
    EXPECT_EQ(plan.satellites[0][1].nodesatellite_type, INVERSE_FILLET);
    plan.satellites[0][1] = before[0][1];
    EXPECT_TRUE(CE::equal(before, plan.satellites));
    EXPECT_TRUE(CE::equal(before, input.satellites));
}

TEST(CornerEdit, MixedIndependentFieldsAndModeOnlyPreservesRadii)
{
    auto input = fixture();
    input.satellites[0][0].amount = 5;
    input.satellites[0][1].amount = 10;
    input.satellites[0][1].nodesatellite_type = INVERSE_FILLET;
    input.selected = {{0, 0}, {0, 1}};
    auto summary = CE::inspect(input, CE::Scope::Selected);
    ASSERT_EQ(summary.status, CE::Status::Ready);
    EXPECT_FALSE(summary.mode);
    EXPECT_FALSE(summary.radius);
    auto plan = CE::prepare(input, {CE::Scope::Selected, CE::Mode::Round, {}});
    ASSERT_EQ(plan.status, CE::Status::Ready);
    EXPECT_EQ(plan.satellites[0][0].amount, 5);
    EXPECT_EQ(plan.satellites[0][1].amount, 10);
}

TEST(CornerEdit, ZeroRestoresOnlyRequestedCornerAndRepeatedZeroIsNoop)
{
    auto input = fixture();
    input.selected = {{0, 0}};
    input.satellites[0][0].amount = 12;
    input.satellites[0][2].amount = 8;
    auto plan = CE::prepare(input, {CE::Scope::Selected, {}, 0});
    ASSERT_EQ(plan.status, CE::Status::Ready);
    EXPECT_EQ(plan.satellites[0][0].amount, 0);
    EXPECT_EQ(plan.satellites[0][2].amount, 8);
    input.satellites = plan.satellites;
    EXPECT_EQ(CE::prepare(input, {CE::Scope::Selected, {}, 0}).status, CE::Status::NoChange);
}

TEST(CornerEdit, OpenEndpointsAndSmoothJoinsAreNeverTargets)
{
    auto input = fixture("M0,0 H30 H60 V60 H0");
    EXPECT_EQ(CE::inspect(input, CE::Scope::All).count, 2u);
    input.smooth = {{0, 2}};
    auto plan = CE::prepare(input, {CE::Scope::All, CE::Mode::InverseRound, 5});
    ASSERT_EQ(plan.status, CE::Status::Ready);
    EXPECT_EQ(plan.count, 1u);
    EXPECT_EQ(plan.satellites[0][0].amount, 0);
    EXPECT_EQ(plan.satellites[0][1].amount, 0);
    EXPECT_EQ(plan.satellites[0][2].amount, 0);
    EXPECT_EQ(plan.satellites[0].back().amount, 0);
    EXPECT_GT(plan.satellites[0][3].amount, 0);
}

TEST(CornerEdit, AllIncludesHoleButSelectedPreservesOtherSubpath)
{
    auto input = fixture("M0,0 H100 V100 H0 Z M30,30 V70 H70 V30 Z");
    EXPECT_EQ(CE::inspect(input, CE::Scope::All).count, 8u);
    input.selected = {{1, 2}};
    auto plan = CE::prepare(input, {CE::Scope::Selected, {}, 3});
    ASSERT_EQ(plan.status, CE::Status::Ready);
    for (unsigned j = 0; j < 4; ++j) EXPECT_EQ(plan.satellites[0][j].amount, 0);
    EXPECT_GT(plan.satellites[1][2].amount, 0);
    EXPECT_EQ(plan.satellites[1][1].amount, 0);
}

TEST(CornerEdit, OversizedRadiusCannotStealNeighbourExtent)
{
    auto input = fixture();
    input.selected = {{0, 1}};
    input.satellites[0][0].amount = 90;
    auto before = input.satellites;
    EXPECT_EQ(CE::prepare(input, {CE::Scope::Selected, {}, 20}).status, CE::Status::EngineLimit);
    EXPECT_TRUE(CE::equal(input.satellites, before));
}

TEST(CornerEdit, InvalidInputAndAdmissionLimitsFailBeforeNativeSolver)
{
    auto input = fixture();
    for (auto v : {-1., std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
        EXPECT_EQ(CE::prepare(input, {CE::Scope::All, {}, v}).status, CE::Status::InvalidInput);
    }
    input.selected = {{9, 9}};
    EXPECT_EQ(CE::prepare(input, {CE::Scope::Selected, {}, 5}).status, CE::Status::InvalidInput);
    input.selected.clear();
    input.satellites[0].resize(CE::max_nodes + 1, NodeSatellite(FILLET));
    EXPECT_FALSE(CE::bounded_shape(input.path, input.satellites));
    input = fixture();
    input.satellites[0][2].is_time = true;
    EXPECT_EQ(CE::prepare(input, {CE::Scope::All, {}, 5}).status, CE::Status::InvalidInput);
}

TEST(CornerEdit, SimilarityIncludesReflectionButNeverAveragesNonuniformScale)
{
    EXPECT_EQ(CE::document_scale(Geom::Affine(0, 2, -2, 0, 7, 8)), 2);
    EXPECT_EQ(CE::document_scale(Geom::Affine(-2, 0, 0, 2, 7, 8)), 2);
    EXPECT_FALSE(CE::document_scale(Geom::Affine(2, 0, 0, 1, 0, 0)));
    EXPECT_FALSE(CE::document_scale(Geom::Affine(1, 0, .5, 1, 0, 0)));
    EXPECT_FALSE(CE::document_scale(Geom::Affine(0, 0, 0, 0, 0, 0)));
}

TEST(CornerEdit, CurvedJoinReadoutIsExplicitlyApproximate)
{
    auto input = fixture("M0,0 C10,0 20,0 30,10 L30,50 L0,50 Z");
    input.selected = {{0, 1}};
    auto summary = CE::inspect(input, CE::Scope::Selected);
    EXPECT_EQ(summary.count, 1u);
    EXPECT_TRUE(summary.approximate);
}

TEST(CornerEdit, ExactReadoutReapplyPreservesStoredDistance)
{
    auto input = fixture();
    input.selected = {{0, 1}};
    input.satellites[0][1].amount = 7.123456789;
    auto summary = CE::inspect(input, CE::Scope::Selected);
    ASSERT_TRUE(summary.radius);
    auto plan = CE::prepare(input, {CE::Scope::Selected, {}, summary.radius});
    EXPECT_EQ(plan.status, CE::Status::NoChange);
    EXPECT_TRUE(CE::equal(plan.satellites, input.satellites));
}

// This proves the detached edit speaks the existing native LPE format. It is
// not a claim that the pending toolbar/creation/lifecycle adapter has run.
TEST_F(DocPerCaseTest, CornerEditNativeRoundInverseAndHistory)
{
    using namespace std::literals;
    auto doc = SPDocument::createNewDocFromMem(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape"
           width="120" height="120" viewBox="0 0 120 120">
        <defs><inkscape:path-effect id="corners" effect="fillet_chamfer" lpeversion="1"
          method="auto" flexible="false" radius="0" use_knot_distance="false" unit="px"
          mode="F" only_selected="false" hide_knots="false" chamfer_steps="1" chamfer_scale="100"
          nodesatellites_param="F,0,0,1,0,0,0,1 @ F,0,0,1,0,0,0,1 @ F,0,0,1,0,0,0,1 @ F,0,0,1,0,0,0,1"/></defs>
        <path id="p" inkscape:path-effect="#corners" inkscape:original-d="M0,0 H100 V100 H0 Z"
          d="M0,0 H100 V100 H0 Z" fill="#3875a5"/>
      </svg>)svg"sv);
    ASSERT_TRUE(doc);
    doc->ensureUpToDate();
    auto path = cast<SPPath>(doc->getObjectById("p")); ASSERT_TRUE(path);
    auto lpe = dynamic_cast<LPEFilletChamfer *>(path->getFirstPathEffectOfType(Inkscape::LivePathEffect::FILLET_CHAMFER));
    ASSERT_TRUE(lpe);
    CE::Snapshot input{*path->curveForEdit(), lpe->nodesatellites_param.data(), {{0, 1}}, {}};
    auto baseline = std::string(lpe->getRepr()->attribute("nodesatellites_param"));
    DocumentUndo::setUndoSensitive(doc.get(), true);
    DocumentUndo::done(doc.get(), Inkscape::Util::Internal::ContextString{"Setup"}, "");
    DocumentUndo::clearUndo(doc.get());
    DocumentUndo::clearRedo(doc.get());
    std::optional<Geom::Point> round_mid;
    for (auto mode : {CE::Mode::Round, CE::Mode::InverseRound}) {
        auto plan = CE::prepare(input, {CE::Scope::Selected, mode, 10});
        ASSERT_EQ(plan.status, CE::Status::Ready);
        lpe->nodesatellites_param.param_set_and_write_new_value(plan.satellites);
        sp_lpe_item_update_patheffect(path, false, true);
        DocumentUndo::done(doc.get(), Inkscape::Util::Internal::ContextString{"Corner edit"}, "");
        doc->ensureUpToDate();
        // Ask the actual engine for its unflattened arc; persisted SVG may use cubic curves.
        auto native = lpe->doEffect_path(lpe->pathvector_before_effect);
        unsigned arcs = 0;
        for (auto const &sub : native) for (auto const &curve : sub) {
            if (auto arc = dynamic_cast<Geom::EllipticalArc const *>(&curve)) {
                ++arcs;
                EXPECT_NEAR(arc->ray(Geom::X), 10, 1e-6);
                EXPECT_NEAR(arc->ray(Geom::Y), 10, 1e-6);
                if (mode == CE::Mode::Round) round_mid = arc->pointAt(.5);
                else { ASSERT_TRUE(round_mid); EXPECT_GT(Geom::distance(*round_mid, arc->pointAt(.5)), 1.0); }
            }
        }
        EXPECT_EQ(arcs, 1u);
        auto committed = std::string(path->getRepr()->attribute("d"));
        EXPECT_STREQ(path->getRepr()->attribute("inkscape:original-d"), "M0,0 H100 V100 H0 Z");
        EXPECT_STREQ(path->getRepr()->attribute("fill"), "#3875a5");
        ASSERT_TRUE(DocumentUndo::undo(doc.get()));
        EXPECT_EQ(lpe->getRepr()->attribute("nodesatellites_param"), baseline);
        EXPECT_FALSE(DocumentUndo::undo(doc.get()));
        ASSERT_TRUE(DocumentUndo::redo(doc.get()));
        EXPECT_EQ(path->getRepr()->attribute("d"), committed);
        ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    }
}
} // namespace
