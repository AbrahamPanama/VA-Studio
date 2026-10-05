// SPDX-License-Identifier: GPL-2.0-or-later
#include <gtest/gtest.h>
#include "actions/vacards-cli-production.h"
#include "actions/vacards-cli-edit-services.h"
#include "actions/vacards-cli-fault-testing.h"
#include "document.h"
#include "document-undo.h"
#include "inkscape.h"
#include "selection.h"
#include "object/sp-item.h"
#include "object/sp-shape.h"
#include "ui/tools/corner-rounding-controller.h"
#include "xml/node.h"
#include "xml/attribute-record.h"
#include <boost/json.hpp>
#include <algorithm>
using namespace Inkscape;
using namespace Inkscape::VACardsCli;
namespace {
std::string canonical(XML::Node const *node)
{
    auto field = [](char const *v) { std::string s = v ? v : ""; return std::to_string(s.size()) + ':' + s; };
    std::string result = std::to_string(static_cast<int>(node->type())) + field(node->name()) + field(node->content());
    std::vector<std::pair<std::string, std::string>> attrs;
    for (auto const &attr : node->attributeList()) attrs.emplace_back(g_quark_to_string(attr.key), attr.value.pointer());
    std::sort(attrs.begin(), attrs.end());
    for (auto const &[key, value] : attrs) result += field(key.c_str()) + field(value.c_str());
    for (auto *child = node->firstChild(); child; child = child->next()) result += canonical(child);
    return result + '/';
}
class M3Offset : public testing::Test {
protected:
    void SetUp() override {
        if (!Application::exists()) Application::create(false);
        doc = SPDocument::createNewDocFromMem(
            "<svg xmlns='http://www.w3.org/2000/svg' xmlns:inkscape='http://www.inkscape.org/namespaces/inkscape' "
            "width='100' height='100' viewBox='0 0 100 100'><defs/>"
            "<g id='g' transform='translate(10,10)'><rect id='a' width='20' height='20'/></g>"
            "<path id='open' d='M50,50 L60,60'/><rect id='outside' x='80' y='80' width='5' height='5'/></svg>");
        doc->ensureUpToDate(); DocumentUndo::setUndoSensitive(doc.get(), true);
        DocumentUndo::clearUndo(doc.get()); DocumentUndo::clearRedo(doc.get());
        doc->getSelection()->set(doc->getObjectById("outside"));
    }
    boost::json::object params() {
        return {{"ids", boost::json::array{"a"}}, {"distance", boost::json::object{{"value", 2}, {"unit", "px"}}},
            {"direction", "outward"}, {"corner", "miter"}, {"miter-limit", 4}, {"outer-only", false},
            {"delete-originals", false}, {"simplify", false},
            {"simplify-tolerance", boost::json::object{{"value", 0.05}, {"unit", "px"}}}, {"select-results", true}};
    }
    Record call(boost::json::object p, bool dry = false) {
        Request request; request.command = "geometry.offset"; request.params = std::move(p); request.dry_run = dry;
        EditServices edits{*doc, *doc->getSelection(), document_stamp(doc.get()), {}};
        return execute_geometry(request, context, edits);
    }
    void redo_fixture() {
        doc->getObjectById("outside")->getRepr()->setAttribute("x", "81");
        DocumentUndo::done(doc.get(), Util::Internal::ContextString("Previous edit"), "object-move");
        ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    }
    std::unique_ptr<SPDocument> doc;
    DispatchContext context;
};
TEST_F(M3Offset, NativeOffsetSuccessOneUndoExactRestorationAndRedo) {
    auto before = canonical(doc->getReprRoot());
    auto revision = document_stamp(doc.get()).revision;
    auto result = call(params());
    ASSERT_EQ(result.status, Status::Changed);
    ASSERT_EQ(result.created.size(), 1u);
    EXPECT_EQ(result.undo_effect, "one-step");
    EXPECT_EQ(result.revision_after, revision + 1);
    auto *created = cast<SPItem>(doc->getObjectById(result.created[0])); ASSERT_TRUE(created);
    EXPECT_EQ(created->parent, doc->getObjectById("g"));
    auto bounds = created->documentGeometricBounds(); ASSERT_TRUE(bounds);
    EXPECT_NEAR(bounds->left(), 8, 1e-5); EXPECT_NEAR(bounds->top(), 8, 1e-5);
    EXPECT_NEAR(bounds->right(), 32, 1e-5); EXPECT_NEAR(bounds->bottom(), 32, 1e-5);
    EXPECT_EQ(doc->getSelection()->items_vector()[0]->getId(), std::string("outside"));
    auto after = canonical(doc->getReprRoot());
    ASSERT_TRUE(DocumentUndo::undo(doc.get())); doc->ensureUpToDate();
    EXPECT_EQ(canonical(doc->getReprRoot()), before);
    ASSERT_TRUE(DocumentUndo::redo(doc.get())); doc->ensureUpToDate();
    EXPECT_EQ(canonical(doc->getReprRoot()), after);
}
TEST_F(M3Offset, PreviewReportsGeometryWithNullIdsAndPreservesRedo) {
    redo_fixture(); auto before = canonical(doc->getReprRoot()); auto revision = document_stamp(doc.get()).revision;
    auto preview = call(params(), true);
    ASSERT_EQ(preview.status, Status::Ok);
    EXPECT_EQ(preview.data.at("variant"), "computed-dry-run");
    auto const &path = preview.data.at("paths").as_array().at(0).as_object();
    EXPECT_EQ(path.at("id"), nullptr);
    EXPECT_EQ(path.at("bounds"), (boost::json::array{8.,8.,32.,32.}));
    EXPECT_TRUE(preview.created.empty());
    EXPECT_EQ(canonical(doc->getReprRoot()), before);
    EXPECT_EQ(document_stamp(doc.get()).revision, revision);
    EXPECT_EQ(doc->getSelection()->items_vector()[0]->getId(), std::string("outside"));
    ASSERT_TRUE(DocumentUndo::redo(doc.get()));
    EXPECT_STREQ(doc->getObjectById("outside")->getRepr()->attribute("x"), "81");
}
TEST_F(M3Offset, UnsupportedOnlyAndCancellationPreserveXmlAndRedo) {
    redo_fixture(); auto before = canonical(doc->getReprRoot()); auto revision = document_stamp(doc.get()).revision;
    auto p = params(); p["ids"] = boost::json::array{"open"};
    auto refused = call(p);
    EXPECT_EQ(refused.reason, "no-closed-shapes");
    EXPECT_EQ(refused.status, Status::Rejected);
    EXPECT_EQ(canonical(doc->getReprRoot()), before);
    context.cancelled = [] { return true; };
    EXPECT_EQ(call(params()).status, Status::Cancelled);
    EXPECT_EQ(canonical(doc->getReprRoot()), before);
    EXPECT_EQ(document_stamp(doc.get()).revision, revision);
    ASSERT_TRUE(DocumentUndo::redo(doc.get()));
}
TEST_F(M3Offset, MixedAndParentChildTargetsProduceOneRootOffset) {
    auto p = params(); p["ids"] = boost::json::array{"a", "g", "open"};
    auto result = call(p);
    ASSERT_EQ(result.status, Status::Changed);
    EXPECT_EQ(result.created.size(), 1u);
    EXPECT_EQ(result.covered, 1);
    ASSERT_EQ(result.excluded.size(), 1u); EXPECT_EQ(result.excluded[0].id, "open");
    EXPECT_TRUE(doc->getObjectById("a")); EXPECT_TRUE(doc->getObjectById("open"));
    auto const &map = result.data.at("source-output").as_array(); ASSERT_EQ(map.size(), 1u);
    EXPECT_EQ(map[0].as_object().at("source"), "g");
}
TEST_F(M3Offset, DeleteOriginalsRestoresExactlyOnUndo) {
    auto before = canonical(doc->getReprRoot());
    auto p = params(); p["delete-originals"] = true;
    auto result = call(p); ASSERT_EQ(result.status, Status::Changed);
    EXPECT_EQ(result.deleted, (std::vector<std::string>{"a"}));
    EXPECT_FALSE(doc->getObjectById("a"));
    ASSERT_TRUE(DocumentUndo::undo(doc.get())); doc->ensureUpToDate();
    EXPECT_EQ(canonical(doc->getReprRoot()), before);
}
TEST_F(M3Offset, FailedAtomicSettlementRestoresXmlAndRedo) {
    redo_fixture(); auto before = canonical(doc->getReprRoot()); auto revision = document_stamp(doc.get()).revision;
    struct Reset { ~Reset() { DocumentUndo::setAtomicSettlementFaultForTesting(nullptr); } } reset;
    DocumentUndo::setAtomicSettlementFaultForTesting([](auto stage) {
        return stage == DocumentUndo::AtomicSettlementStage::HistoryInsertion;
    });
    auto result = call(params());
    EXPECT_EQ(result.status, Status::Failed);
    EXPECT_EQ(canonical(doc->getReprRoot()), before);
    EXPECT_EQ(document_stamp(doc.get()).revision, revision);
    EXPECT_TRUE(result.created.empty());
    EXPECT_EQ(doc->getSelection()->items_vector()[0]->getId(), std::string("outside"));
    DocumentUndo::setAtomicSettlementFaultForTesting(nullptr);
    ASSERT_TRUE(DocumentUndo::redo(doc.get()));
}

class M3Boolean : public M3Offset {
protected:
    void SetUp() override { M3Offset::SetUp(); make(); }
    void make(std::string body = "<rect id='a' width='20' height='20'/><rect id='b' x='10' width='20' height='20'/>") {
        doc = SPDocument::createNewDocFromMem(
            "<svg xmlns='http://www.w3.org/2000/svg' xmlns:inkscape='http://www.inkscape.org/namespaces/inkscape' "
            "width='100' height='100' viewBox='0 0 100 100'><defs/>" + body +
            "<rect id='outside' x='80' y='80' width='5' height='5'/></svg>");
        doc->ensureUpToDate(); DocumentUndo::setUndoSensitive(doc.get(), true);
        DocumentUndo::clearUndo(doc.get()); DocumentUndo::clearRedo(doc.get());
        doc->getSelection()->set(doc->getObjectById("outside"));
    }
    Record boolean(std::string op, bool dry = false, std::string policy = "reject",
                   boost::json::array ids = {"a", "b"}) {
        Request request; request.command = "geometry.boolean"; request.dry_run = dry;
        request.params = {{"ids", ids}, {"op", op}, {"empty-result", policy}};
        EditServices edits{*doc, *doc->getSelection(), document_stamp(doc.get()), {}};
        return execute_geometry(request, context, edits);
    }
};
TEST_F(M3Boolean, AllFourClosedOperationsHaveAnalyticBoundsAndExactUndoRedo) {
    for (auto op : {"union", "intersection", "difference", "xor"}) {
        SCOPED_TRACE(op); make();
        auto before = canonical(doc->getReprRoot()); auto revision = document_stamp(doc.get()).revision;
        auto result = boolean(op);
        ASSERT_EQ(result.status, Status::Changed);
        ASSERT_EQ(result.selection_after.size(), 1u);
        auto *output = cast<SPItem>(doc->getObjectById(result.selection_after[0])); ASSERT_TRUE(output);
        auto box = output->documentGeometricBounds(); ASSERT_TRUE(box);
        EXPECT_NEAR(box->left(), std::string(op) == "intersection" ? 10 : 0, 1e-6);
        EXPECT_NEAR(box->right(), std::string(op) == "difference" ? 10 : std::string(op) == "intersection" ? 20 : 30, 1e-6);
        EXPECT_EQ(result.data.at("consumed-ids"), (boost::json::array{"a","b"}));
        EXPECT_EQ(result.revision_after, revision + 1);
        EXPECT_EQ(doc->getSelection()->items_vector()[0]->getId(), std::string("outside"));
        auto after = canonical(doc->getReprRoot());
        ASSERT_TRUE(DocumentUndo::undo(doc.get())); doc->ensureUpToDate();
        EXPECT_EQ(canonical(doc->getReprRoot()), before);
        ASSERT_TRUE(DocumentUndo::redo(doc.get())); doc->ensureUpToDate();
        EXPECT_EQ(canonical(doc->getReprRoot()), after);
    }
}
TEST_F(M3Boolean, OrderedDifferenceAndMultipleDivisionOutputs) {
    auto reversed = boolean("difference", false, "reject", {"b","a"});
    ASSERT_EQ(reversed.status, Status::Changed);
    auto box = cast<SPItem>(doc->getObjectById("b"))->documentGeometricBounds(); ASSERT_TRUE(box);
    EXPECT_NEAR(box->left(),20,1e-6); EXPECT_NEAR(box->right(),30,1e-6);
    make("<rect id='a' width='20' height='20'/><path id='b' d='M10,-5 V25'/>");
    auto before = canonical(doc->getReprRoot());
    auto division = boolean("division"); ASSERT_EQ(division.status, Status::Changed);
    ASSERT_EQ(division.selection_after.size(), 2u);
    EXPECT_EQ(division.data.at("paths").as_array().size(), 2u);
    auto left = cast<SPItem>(doc->getObjectById(division.selection_after[0]))->documentGeometricBounds();
    auto right = cast<SPItem>(doc->getObjectById(division.selection_after[1]))->documentGeometricBounds();
    ASSERT_TRUE(left); ASSERT_TRUE(right);
    EXPECT_NEAR(left->width()+right->width(),20,1e-6);
    EXPECT_NEAR(left->width(),10,1e-6); EXPECT_NEAR(right->width(),10,1e-6);
    ASSERT_TRUE(DocumentUndo::undo(doc.get())); doc->ensureUpToDate();
    EXPECT_EQ(canonical(doc->getReprRoot()),before);
    auto refused = boolean("division",false,"reject",{"a","b","outside"});
    EXPECT_EQ(refused.reason,"invalid-argument");
    EXPECT_EQ(refused.error_details.at("reason"),"BooleanOperandReason::InvalidCount");
    EXPECT_EQ(canonical(doc->getReprRoot()),before);
    ASSERT_TRUE(DocumentUndo::redo(doc.get()));
}
TEST_F(M3Boolean, ComputedPreviewUsesNoLiveIdsAndPreservesExactStateAndRedo) {
    redo_fixture(); auto before = canonical(doc->getReprRoot()); auto revision = document_stamp(doc.get()).revision;
    auto preview = boolean("union",true); ASSERT_EQ(preview.status,Status::Ok);
    EXPECT_EQ(preview.data.at("variant"),"computed-dry-run");
    auto const &path = preview.data.at("paths").as_array().at(0).as_object();
    EXPECT_EQ(path.at("id"),nullptr);
    EXPECT_EQ(path.at("bounds"),(boost::json::array{0.,0.,30.,20.}));
    for (auto const &map : preview.data.at("source-output").as_array())
        EXPECT_TRUE(map.as_object().at("outputs").as_array().empty());
    EXPECT_EQ(preview.selection_after,(std::vector<std::string>{"outside"}));
    EXPECT_EQ(canonical(doc->getReprRoot()),before);
    EXPECT_EQ(document_stamp(doc.get()).revision,revision);
    EXPECT_EQ(context.session_revision,0u);
    ASSERT_TRUE(DocumentUndo::redo(doc.get()));
}
TEST_F(M3Boolean, EmptyRejectPreservesRedoAndAllowCommitsConsumption) {
    make("<rect id='a' width='20' height='20'/><rect id='b' width='20' height='20'/>");
    redo_fixture(); auto before = canonical(doc->getReprRoot());
    auto reject = boolean("difference");
    EXPECT_EQ(reject.reason,"empty-result"); EXPECT_EQ(reject.status,Status::Rejected);
    EXPECT_EQ(canonical(doc->getReprRoot()),before);
    ASSERT_TRUE(DocumentUndo::redo(doc.get())); ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    auto preview = boolean("difference",true,"allow");
    ASSERT_EQ(preview.status,Status::Ok); EXPECT_TRUE(preview.data.at("paths").as_array().empty());
    EXPECT_EQ(canonical(doc->getReprRoot()),before);
    auto allow = boolean("difference",false,"allow"); ASSERT_EQ(allow.status,Status::Changed);
    EXPECT_TRUE(allow.selection_after.empty()); EXPECT_TRUE(allow.data.at("paths").as_array().empty());
    EXPECT_FALSE(doc->getObjectById("a")); EXPECT_FALSE(doc->getObjectById("b"));
    EXPECT_EQ(allow.data.at("consumed-ids"),(boost::json::array{"a","b"}));
    ASSERT_TRUE(DocumentUndo::undo(doc.get())); doc->ensureUpToDate();
    EXPECT_EQ(canonical(doc->getReprRoot()),before);
}
TEST_F(M3Boolean, OverlapUnsupportedAndCancellationNeverConsumeRoots) {
    make("<g id='a'><rect id='child' width='20' height='20'/></g><g id='b'/>");
    auto before = canonical(doc->getReprRoot());
    unsigned selection_events = 0;
    auto connection = doc->getSelection()->connectChanged([&](Selection *) { ++selection_events; });
    auto overlap = boolean("union",false,"reject",{"a","child"});
    EXPECT_EQ(overlap.reason,"incompatible-operands");
    EXPECT_EQ(overlap.error_details.at("reason"),"BooleanOperandReason::OverlappingOperands");
    auto unsupported = boolean("union");
    EXPECT_EQ(unsupported.error_details.at("reason"),"BooleanOperandReason::EmptyGroup");
    EXPECT_EQ(canonical(doc->getReprRoot()),before);
    context.cancelled=[] {return true;};
    EXPECT_EQ(boolean("union").status,Status::Cancelled);
    EXPECT_EQ(canonical(doc->getReprRoot()),before);
    EXPECT_EQ(selection_events,0u);
    connection.disconnect();
}
TEST_F(M3Boolean, AtomicFailureRestoresExactXmlAndPreexistingRedo) {
    redo_fixture(); auto before = canonical(doc->getReprRoot()); auto revision = document_stamp(doc.get()).revision;
    struct Reset { ~Reset() { DocumentUndo::setAtomicSettlementFaultForTesting(nullptr); } } reset;
    DocumentUndo::setAtomicSettlementFaultForTesting([](auto stage) {
        return stage == DocumentUndo::AtomicSettlementStage::HistoryInsertion;
    });
    auto result = boolean("union"); EXPECT_EQ(result.status,Status::Failed);
    EXPECT_EQ(canonical(doc->getReprRoot()),before);
    EXPECT_EQ(document_stamp(doc.get()).revision,revision);
    EXPECT_TRUE(result.created.empty()); EXPECT_TRUE(result.deleted.empty());
    DocumentUndo::setAtomicSettlementFaultForTesting(nullptr);
    ASSERT_TRUE(DocumentUndo::redo(doc.get()));
}
} // namespace

#include "object/transform-policy-scope.h"
#include "ui/tools/corner-rounding-controller.h"
#include "object/sp-shape.h"
#include "preferences.h"
#include "style.h"
namespace {
class M3Transforms : public M3Offset {
protected:
    Record run(std::string command, boost::json::object p, bool dry = false) {
        if (!p.if_contains("ids")) p["ids"] = boost::json::array{"g", "a", "outside"};
        Request r; r.command = "geometry." + command; r.params = std::move(p); r.dry_run = dry;
        EditServices edits{*doc, *doc->getSelection(), document_stamp(doc.get()), {}};
        return execute_geometry(r, context, edits);
    }
    void outcome(std::string command, boost::json::object p, Geom::Rect expected) {
        redo_fixture();
        auto before = canonical(doc->getReprRoot()); auto revision = document_stamp(doc.get()).revision;
        auto dry = run(command, p, true);
        ASSERT_EQ(dry.status, Status::Ok) << dry.reason << ": " << dry.message;
        EXPECT_EQ(canonical(doc->getReprRoot()), before);
        EXPECT_EQ(document_stamp(doc.get()).revision, revision);
        EXPECT_EQ(doc->getSelection()->items_vector()[0]->getId(), std::string("outside"));
        EditServices edits{*doc, *doc->getSelection(), document_stamp(doc.get()), {}};
        ASSERT_TRUE(history_snapshot(edits).value->can_redo);
        auto result = run(command, p);
        ASSERT_EQ(result.status, Status::Changed) << result.reason << ": " << result.message;
        EXPECT_EQ(result.covered, 1u); EXPECT_EQ(result.eligible, 2u);
        EXPECT_EQ(result.revision_after, revision + 1); EXPECT_EQ(result.undo_effect, "one-step");
        EXPECT_EQ(result.data.at("bounds-after"), dry.data.at("bounds-after"));
        auto const &bounds = result.data.at("bounds-after").as_array();
        ASSERT_EQ(bounds.size(), 4u);
        EXPECT_NEAR(bounds[0].to_number<double>(), expected.left(), 1e-6);
        EXPECT_NEAR(bounds[1].to_number<double>(), expected.top(), 1e-6);
        EXPECT_NEAR(bounds[2].to_number<double>(), expected.right(), 1e-6);
        EXPECT_NEAR(bounds[3].to_number<double>(), expected.bottom(), 1e-6);
        EXPECT_EQ(doc->getObjectById("a")->parent, doc->getObjectById("g"));
        auto after = canonical(doc->getReprRoot()); EXPECT_NE(after, before);
        ASSERT_TRUE(DocumentUndo::undo(doc.get())); doc->ensureUpToDate();
        EXPECT_EQ(canonical(doc->getReprRoot()), before);
        EXPECT_FALSE(history_snapshot(edits).value->can_undo);
        ASSERT_TRUE(DocumentUndo::redo(doc.get())); doc->ensureUpToDate();
        EXPECT_EQ(canonical(doc->getReprRoot()), after);
        p["ids"] = boost::json::array{"missing"};
        auto rejected = run(command, p); EXPECT_EQ(rejected.status, Status::Rejected); EXPECT_EQ(rejected.reason, "unknown-id");
        EXPECT_EQ(canonical(doc->getReprRoot()), after);
    }
};
TEST_F(M3Transforms, geometry_move) {
    outcome("move", {{"dx", 5}, {"dy", -3}}, Geom::Rect(15,7,90,82));
}
TEST_F(M3Transforms, geometry_resize) {
    outcome("resize", {{"width", 150}, {"keep-ratio", true}, {"bbox", "geometric"}, {"anchor", "nw"}}, Geom::Rect(10,10,160,160));
}
TEST_F(M3Transforms, geometry_rotate) {
    outcome("rotate", {{"angle", 90}, {"pivot", boost::json::object{{"x", 0}, {"y", 0}}}}, Geom::Rect(-85,10,-10,85));
}
TEST_F(M3Transforms, geometry_skew) {
    outcome("skew", {{"angle", 45}, {"axis", "x"}, {"pivot", boost::json::object{{"x", 0}, {"y", 0}}}}, Geom::Rect(20,10,170,85));
}
TEST_F(M3Transforms, geometry_flip) {
    outcome("flip", {{"axis", "horizontal"}}, Geom::Rect(10,10,85,85));
}
TEST_F(M3Transforms, geometry_matrix) {
    outcome("matrix", {{"matrix", boost::json::array{2,0,0,3,5,-2}}}, Geom::Rect(25,28,175,253));
}
TEST_F(M3Transforms, IdentityTransformsPreserveRedoAndXML) {
    redo_fixture(); auto before = canonical(doc->getReprRoot());
    for (auto const &[name,p] : std::vector<std::pair<std::string,boost::json::object>>{
        {"move", {{"dx",0},{"dy",0}}}, {"rotate",{{"angle",360}}}, {"skew",{{"axis","y"},{"angle",0}}},
        {"matrix",{{"matrix",boost::json::array{1,0,0,1,0,0}}}},
        {"resize",{{"width",75},{"height",75},{"bbox","geometric"},{"anchor","center"}}}}) {
        auto result = run(name,p); EXPECT_EQ(result.status,Status::Unchanged) << name;
        EXPECT_EQ(canonical(doc->getReprRoot()),before); EXPECT_EQ(result.undo_effect,"none");
    }
    ASSERT_TRUE(DocumentUndo::redo(doc.get()));
}
TEST_F(M3Transforms, SingularAndDegenerateRefusalPreservesRedo) {
    redo_fixture(); auto before = canonical(doc->getReprRoot());
    auto bad = run("matrix",{{"matrix",boost::json::array{1,0,0,0,0,0}}});
    EXPECT_EQ(bad.reason,"invalid-transform"); EXPECT_EQ(bad.status,Status::Rejected);
    auto zero = run("resize",{{"width",0},{"bbox","geometric"}}); EXPECT_EQ(zero.reason,"invalid-transform");
    EXPECT_EQ(canonical(doc->getReprRoot()),before); ASSERT_TRUE(DocumentUndo::redo(doc.get()));
}
TEST_F(M3Transforms, SettlementFailureRestoresXMLSelectionAndRedo) {
    redo_fixture(); auto before = canonical(doc->getReprRoot());
    DocumentUndo::setAtomicSettlementFaultForTesting([](auto stage) { return stage == DocumentUndo::AtomicSettlementStage::HistoryInsertion; });
    auto result = run("move",{{"dx",5},{"dy",3}});
    DocumentUndo::setAtomicSettlementFaultForTesting(nullptr);
    EXPECT_EQ(result.status,Status::Failed); EXPECT_EQ(canonical(doc->getReprRoot()),before);
    EXPECT_EQ(doc->getSelection()->items_vector()[0]->getId(),std::string("outside"));
    ASSERT_TRUE(DocumentUndo::redo(doc.get()));
}
struct TransformPreferences {
    Preferences *prefs = Preferences::get();
    std::vector<std::pair<std::string,bool>> saved;
    explicit TransformPreferences(bool defaults) {
        for (auto const *key : {"stroke","rectcorners","pattern","gradient","hatch"}) {
            auto path = std::string("/options/transform/")+key;
            saved.emplace_back(path,prefs->getBool(path,true)); prefs->setBool(path,defaults);
        }
        std::string path = "/options/preservetransform/value";
        saved.emplace_back(path,prefs->getBool(path,false)); prefs->setBool(path,!defaults);
    }
    ~TransformPreferences() { for (auto const &[path,value] : saved) prefs->setBool(path,value); }
    void expect(bool defaults) {
        for (auto const &[path,value] : saved) EXPECT_EQ(prefs->getBool(path),path == "/options/preservetransform/value" ? !defaults : defaults) << path;
    }
};
TEST_F(M3Transforms, RequiredOutcomesPreferenceIndependentStrokeCornersPaintAndDash) {
    doc = SPDocument::createNewDocFromMem(
        "<svg xmlns='http://www.w3.org/2000/svg' width='100' height='100'><defs>"
        "<linearGradient id='grad'><stop stop-color='red'/><stop offset='1' stop-color='blue'/></linearGradient>"
        "<pattern id='pat' width='4' height='4' patternUnits='userSpaceOnUse'><rect width='2' height='2'/></pattern></defs>"
        "<rect id='a' x='10' y='10' width='20' height='10' rx='2' style='fill:url(#grad);stroke:black;stroke-width:2;stroke-dasharray:3,2'/>"
        "<rect id='b' x='50' y='10' width='10' height='10' fill='url(#pat)'/></svg>");
    doc->ensureUpToDate(); DocumentUndo::setUndoSensitive(doc.get(),true);
    auto p = boost::json::object{{"ids",boost::json::array{"a","b"}}, {"matrix",boost::json::array{2,0,0,2,0,0}}};
    std::string expected;
    { TransformPreferences prefs(true); auto r=run("matrix",p); ASSERT_EQ(r.status,Status::Changed);
      expected=canonical(doc->getReprRoot()); prefs.expect(true); }
    ASSERT_TRUE(DocumentUndo::undo(doc.get())); doc->ensureUpToDate();
    { TransformPreferences prefs(false); auto r=run("matrix",p); ASSERT_EQ(r.status,Status::Changed);
      EXPECT_EQ(canonical(doc->getReprRoot()),expected); prefs.expect(false);
      auto *a=cast<SPItem>(doc->getObjectById("a")); ASSERT_TRUE(a);
      EXPECT_NEAR(a->style->stroke_width.computed,4,1e-8);
      ASSERT_EQ(a->style->stroke_dasharray.values.size(),2u);
      EXPECT_NEAR(a->style->stroke_dasharray.values[0].computed,6,1e-8); }
}
TEST_F(M3Transforms, EveryTransformIgnoresNondefaultUserPreferences) {
    for (auto const &[name,p] : std::vector<std::pair<std::string,boost::json::object>>{
        {"move", {{"dx",5},{"dy",3}}}, {"rotate",{{"angle",30}}}, {"skew",{{"axis","y"},{"angle",15}}},
        {"matrix",{{"matrix",boost::json::array{2,0,0,3,5,7}}}}, {"flip",{{"axis","vertical"}}},
        {"resize",{{"width",150},{"height",120},{"bbox","geometric"},{"anchor","center"}}}}) {
        SCOPED_TRACE(name);
        std::string expected;
        { TransformPreferences prefs(true); auto r=run(name,p); ASSERT_EQ(r.status,Status::Changed);
          expected=canonical(doc->getReprRoot()); prefs.expect(true); }
        ASSERT_TRUE(DocumentUndo::undo(doc.get())); doc->ensureUpToDate();
        { TransformPreferences prefs(false); auto r=run(name,p); ASSERT_EQ(r.status,Status::Changed);
          EXPECT_EQ(canonical(doc->getReprRoot()),expected); prefs.expect(false); }
        ASSERT_TRUE(DocumentUndo::undo(doc.get())); doc->ensureUpToDate();
    }
}
TEST_F(M3Transforms, RootViewportCoordinatesAndMultipleParents) {
    doc=SPDocument::createNewDocFromMem(
        "<svg xmlns='http://www.w3.org/2000/svg' width='200' height='100' viewBox='0 0 100 50'>"
        "<g id='g' transform='translate(10,5)'><rect id='a' width='10' height='10'/></g>"
        "<g id='h' transform='scale(2)'><rect id='b' x='30' y='10' width='5' height='5'/></g></svg>");
    doc->ensureUpToDate(); DocumentUndo::setUndoSensitive(doc.get(),true);
    auto before=canonical(doc->getReprRoot());
    auto r=run("move",{{"ids",boost::json::array{"a","b"}},{"dx",7},{"dy",11}});
    ASSERT_EQ(r.status,Status::Changed) << r.message;
    auto a=cast<SPItem>(doc->getObjectById("a"))->documentGeometricBounds();
    auto b=cast<SPItem>(doc->getObjectById("b"))->documentGeometricBounds(); ASSERT_TRUE(a); ASSERT_TRUE(b);
    EXPECT_NEAR(a->left(),27,1e-7); EXPECT_NEAR(a->top(),21,1e-7);
    EXPECT_NEAR(b->left(),127,1e-7); EXPECT_NEAR(b->top(),51,1e-7);
    EXPECT_EQ(doc->getObjectById("a")->parent,doc->getObjectById("g"));
    EXPECT_EQ(doc->getObjectById("b")->parent,doc->getObjectById("h"));
    ASSERT_TRUE(DocumentUndo::undo(doc.get())); doc->ensureUpToDate(); EXPECT_EQ(canonical(doc->getReprRoot()),before);
}
TEST_F(M3Transforms, NestedPolicyRestoresOuterAndInactivePreferenceBehavior) {
    TransformPreferences prefs(false);
    EXPECT_EQ(ScopedTransformPolicy::active(),nullptr);
    {
        ScopedTransformPolicy outer; auto const *active=ScopedTransformPolicy::active();
        ASSERT_TRUE(active); EXPECT_FALSE(active->preserve);
        { TransformPolicy p; p.preserve=true; ScopedTransformPolicy inner(p);
          EXPECT_TRUE(ScopedTransformPolicy::active()->preserve); }
        EXPECT_EQ(ScopedTransformPolicy::active(),active);
        auto *a=cast<SPItem>(doc->getObjectById("a")); a->doWriteTransform(Geom::Scale(2));
        EXPECT_FALSE(a->getRepr()->attribute("transform"));
    }
    EXPECT_EQ(ScopedTransformPolicy::active(),nullptr);
    auto *a=cast<SPItem>(doc->getObjectById("outside")); a->doWriteTransform(Geom::Scale(2));
    EXPECT_TRUE(a->getRepr()->attribute("transform")); prefs.expect(false);
}
class M3Corners : public M3Transforms {
protected:
    boost::json::object params(std::string id="a",double radius=2) {
        return {{"ids",boost::json::array{id}}, {"radius",radius}, {"mode","round"},{"scope","all"}};
    }
};
TEST_F(M3Corners, geometry_corners) {
    redo_fixture(); auto before=canonical(doc->getReprRoot());
    auto dry=run("corners",params(),true); ASSERT_EQ(dry.status,Status::Ok) << dry.message;
    EXPECT_EQ(canonical(doc->getReprRoot()),before);
    auto r=run("corners",params()); ASSERT_EQ(r.status,Status::Changed) << r.message;
    EXPECT_EQ(r.undo_effect,"one-step"); EXPECT_EQ(r.data.at("paths"),dry.data.at("paths"));
    EXPECT_TRUE(doc->getObjectById("a")->getRepr()->attribute("inkscape:path-effect"));
    auto after=canonical(doc->getReprRoot());
    auto same=run("corners",params()); EXPECT_EQ(same.status,Status::Unchanged); EXPECT_EQ(canonical(doc->getReprRoot()),after);
    ASSERT_TRUE(DocumentUndo::undo(doc.get())); doc->ensureUpToDate(); EXPECT_EQ(canonical(doc->getReprRoot()),before);
    ASSERT_TRUE(DocumentUndo::redo(doc.get())); doc->ensureUpToDate(); EXPECT_EQ(canonical(doc->getReprRoot()),after);
}
TEST_F(M3Corners, NativePolygonAndPolylineConversionUndoAndPreview) {
    for (auto type : {"polygon","polyline"}) {
        auto svg=std::string("<svg xmlns='http://www.w3.org/2000/svg' width='100' height='100'><")+type+
            " id='a' points='10,10 40,10 40,40 10,40' fill='red'/></svg>";
        doc=SPDocument::createNewDocFromMem(svg); doc->ensureUpToDate(); DocumentUndo::setUndoSensitive(doc.get(),true);
        auto before=canonical(doc->getReprRoot()); auto dry=run("corners",params(),true);
        ASSERT_EQ(dry.status,Status::Ok) << dry.message; EXPECT_EQ(canonical(doc->getReprRoot()),before);
        auto result=run("corners",params()); ASSERT_EQ(result.status,Status::Changed) << result.message;
        EXPECT_EQ(std::string(doc->getObjectById("a")->getRepr()->name()),"svg:path");
        EXPECT_EQ(result.data.at("converted").as_array()[0].as_object().at("from").as_string(),type);
        EXPECT_EQ(result.data.at("paths"),dry.data.at("paths"));
        ASSERT_TRUE(DocumentUndo::undo(doc.get())); doc->ensureUpToDate(); EXPECT_EQ(canonical(doc->getReprRoot()),before);
    }
}
TEST_F(M3Corners, RefusalAndZeroRadiusPreserveRedo) {
    redo_fixture(); auto before=canonical(doc->getReprRoot());
    EXPECT_EQ(run("corners",params("open")).reason,"path-not-supported");
    EXPECT_EQ(run("corners",params("g")).reason,"requires-single-shape");
    auto p=params(); p["scope"]="nodes"; p["nodes"]=boost::json::array{"0:999"};
    EXPECT_EQ(run("corners",p).reason,"no-such-node");
    EXPECT_EQ(run("corners",params("a",0)).status,Status::Unchanged);
    EXPECT_EQ(canonical(doc->getReprRoot()),before); ASSERT_TRUE(DocumentUndo::redo(doc.get()));
}
TEST_F(M3Corners, AtomicFailurePreservesRedo) {
    redo_fixture(); auto before=canonical(doc->getReprRoot());
    DocumentUndo::setAtomicSettlementFaultForTesting([](auto stage) { return stage == DocumentUndo::AtomicSettlementStage::HistoryInsertion; });
    auto result=run("corners",params()); DocumentUndo::setAtomicSettlementFaultForTesting(nullptr);
    EXPECT_EQ(result.status,Status::Failed); EXPECT_EQ(canonical(doc->getReprRoot()),before);
    ASSERT_TRUE(DocumentUndo::redo(doc.get()));
}
TEST_F(M3Corners, CallerProtocolRefusesWithoutInteraction) {
    namespace Tools = UI::Tools;
    namespace CE = LivePathEffect::CornerEdit;
    redo_fixture(); auto before=canonical(doc->getReprRoot());
    auto *shape=cast<SPShape>(doc->getObjectById("a")); ASSERT_TRUE(shape);
    auto captured=Tools::capture_corner_rounding_document(*shape,0); ASSERT_TRUE(captured.snapshot);
    CE::Request request; request.radius=2; request.scope=CE::Scope::All; request.mode=CE::Mode::Round;
    auto result=Tools::apply_corner_plan(*shape,nullptr,*captured.snapshot,request,
        Tools::CornerCommitProtocol::CallerOwnedAtomic, [] { return true; });
    EXPECT_EQ(result.outcome,Tools::CornerRoundingController::Outcome::Rejected);
    EXPECT_TRUE(result.refused_by_document); EXPECT_FALSE(result.mutation_started);
    EXPECT_EQ(canonical(doc->getReprRoot()),before); ASSERT_TRUE(DocumentUndo::redo(doc.get()));
}
TEST_F(M3Corners, NativePostMutationFailureRollsBackWithoutLosingRedo) {
    redo_fixture(); auto before=canonical(doc->getReprRoot());
    int calls=0; context.cancelled=[&] { return ++calls >= 2; };
    auto result=run("corners",params());
    EXPECT_EQ(result.status,Status::Failed); EXPECT_EQ(result.reason,"corner-edit-failed");
    EXPECT_EQ(result.error_details.at("mutation_state").as_string(),"rolled-back");
    EXPECT_EQ(canonical(doc->getReprRoot()),before); ASSERT_TRUE(DocumentUndo::redo(doc.get()));
}

}

// Literal independent oracles, never computed from service output.
#include <filesystem>
#include <fstream>
#include <iostream>
namespace {
class P9Independent : public testing::Test {
protected:
    struct Payload : TokenPayload { TokenKind kind() const noexcept override { return TokenKind::NestAnalysis; } };
    void SetUp() override {
        if (!Application::exists()) Application::create(false);
        auto path = std::filesystem::path(__FILE__).parent_path().parent_path() / "cli_tests/vacards-agent/fixtures/m3/geometry.svg";
        std::ifstream stream(path); std::string svg((std::istreambuf_iterator<char>(stream)), {});
        ASSERT_FALSE(svg.empty());
        doc = SPDocument::createNewDocFromMem(svg); ASSERT_TRUE(doc);
        doc->ensureUpToDate(); DocumentUndo::setUndoSensitive(doc.get(), true);
        DocumentUndo::clearUndo(doc.get()); DocumentUndo::clearRedo(doc.get());
        context.document = doc.get(); context.selection = doc->getSelection(); context.session_revision = 17;
        doc->getSelection()->set(doc->getObjectById("untouched"));
        TokenBinding binding; binding.session="P9"; binding.document=document_stamp(doc.get()).id;
        binding.engine="native"; binding.catalog_hash="P9-independent"; binding.incarnation=1;
        auto retained=tokens.retain(TokenKind::NestAnalysis,binding,std::make_shared<Payload>(),
            {{"P9-held-buffer",37,std::make_shared<std::vector<unsigned char>>(37,9)}});
        ASSERT_TRUE(retained.value); token_id=retained.value->id;
    }
    void TearDown() override { DocumentUndo::setAtomicSettlementFaultForTesting(nullptr); }
    boost::json::array selection() {
        boost::json::array a; for(auto *item:doc->getSelection()->items()) a.emplace_back(item->getId()); return a;
    }
    boost::json::object state() {
        EditServices e{*doc,*doc->getSelection(),document_stamp(doc.get()),{},lease};
        auto h=history_snapshot(e); EXPECT_TRUE(h.value);
        auto t=tokens.snapshot(); boost::json::array ids; for(auto const &id:t.active_tokens) ids.emplace_back(id);
        boost::json::object s{{"xml",canonical(doc->getReprRoot())},{"selection",selection()},
            {"document_revision",document_stamp(doc.get()).revision},{"session_revision",context.session_revision},
            {"tokens",boost::json::object{{"ids",ids},{"roots",t.active_roots},{"held_bytes",t.charged_bytes}}}};
        if(h.value) {
            s["undo"]=boost::json::object{{"available",h.value->can_undo},{"label",h.value->next_undo_label?boost::json::value(*h.value->next_undo_label):boost::json::value(nullptr)}};
            s["redo"]=boost::json::object{{"available",h.value->can_redo},{"label",h.value->next_redo_label?boost::json::value(*h.value->next_redo_label):boost::json::value(nullptr)}};
        }
        return s;
    }
    Request request(std::string const &command, boost::json::object params) {
        auto parsed=parse_production_request(boost::json::serialize(boost::json::object{
            {"schema","va-studio.cli-request/1"},{"id","P9"},{"command",command},{"params",params},
            {"document",document_stamp(doc.get()).id},{"if_revision",command.starts_with("selection.")?context.session_revision:document_stamp(doc.get()).revision}}));
        EXPECT_TRUE(parsed.request) << (parsed.error?parsed.error->message:"");
        return parsed.request ? *parsed.request : Request{};
    }
    Record call(Request r, bool dry=false) {
        r.dry_run=dry;
        EditServices e{*doc,*doc->getSelection(),document_stamp(doc.get()),{}};
        return r.command.starts_with("selection.")?execute_selection(r,context,e):
            r.command.starts_with("history.")?execute_history(r,context,e):execute_geometry(r,context,e);
    }
    void seed_redo() {
        // Fixture mutations must survive the unrelated edit's Undo.
        DocumentUndo::done(doc.get(),Util::Internal::ContextString("P9 fixture setup"),"object-move");
        DocumentUndo::clearUndo(doc.get()); DocumentUndo::clearRedo(doc.get());
        doc->getObjectById("untouched")->getRepr()->setAttribute("x","151");
        DocumentUndo::done(doc.get(),Util::Internal::ContextString("P9 prior edit"),"object-move");
        ASSERT_TRUE(DocumentUndo::undo(doc.get())); doc->ensureUpToDate();
    }
    void subset(boost::json::value const &actual, boost::json::value const &expected) {
        if(expected.is_object()) { ASSERT_TRUE(actual.is_object()); for(auto const &v:expected.as_object()) {
            ASSERT_TRUE(actual.as_object().contains(v.key())) << v.key(); subset(actual.as_object().at(v.key()),v.value()); } }
        else if(expected.is_array()) { ASSERT_TRUE(actual.is_array()); ASSERT_EQ(actual.as_array().size(),expected.as_array().size());
            for(size_t i=0;i<expected.as_array().size();++i) subset(actual.as_array()[i],expected.as_array()[i]); }
        else if(expected.is_number()) { ASSERT_TRUE(actual.is_number()); EXPECT_NEAR(actual.to_number<double>(),expected.to_number<double>(),1e-6); }
        else EXPECT_EQ(actual,expected);
    }
    void independent(std::string const &command, boost::json::object p, boost::json::object expected) {
        auto r=request(command,p); ASSERT_FALSE(r.command.empty());
        doc->getSelection()->set(doc->getObjectById("untouched"));
        if(command=="selection.clear") doc->getSelection()->set(doc->getObjectById("shape1"));
        auto untouched=canonical(doc->getObjectById("untouched")->getRepr());
        if(command=="history.undo" || command=="history.redo") {
            auto move=request("geometry.move",boost::json::parse(R"({"ids":["shape1"],"dx":{"value":2,"unit":"px"},"dy":{"value":3,"unit":"px"}})").as_object());
            ASSERT_EQ(call(move).status,Status::Changed);
            if(command=="history.redo") ASSERT_TRUE(DocumentUndo::undo(doc.get()));
        } else if(command!="history.query") seed_redo();
        auto before=state();
        auto preview=call(r,true); ASSERT_EQ(preview.status,Status::Ok) << preview.reason;
        auto preview_after=state(); EXPECT_EQ(preview_after,before);
        context.cancelled=[] {return true;}; auto cancel=call(r); context.cancelled={};
        ASSERT_EQ(cancel.status,Status::Cancelled); auto cancel_after=state(); EXPECT_EQ(cancel_after,before);
        auto result=call(r); ASSERT_EQ(result.status,command=="history.query"?Status::Ok:Status::Changed) << result.reason;
        subset(result.data,expected);
        auto after=state();
        EXPECT_EQ(after.at("tokens"),before.at("tokens")); EXPECT_EQ(context.session_revision,17u);
        EXPECT_EQ(canonical(doc->getObjectById("untouched")->getRepr()),untouched);
        EXPECT_TRUE(DocumentUndo::fileOperationFreshReady(doc.get()));
        if(command.starts_with("geometry.")) {
            auto actual_bounds=[&](std::string const &id, boost::json::value const &bounds) {
                auto *item=cast<SPItem>(doc->getObjectById(id)); ASSERT_TRUE(item);
                auto b=item->documentGeometricBounds(); ASSERT_TRUE(b);
                subset(boost::json::array{b->left(),b->top(),b->right(),b->bottom()},bounds);
            };
            if(expected.contains("bounds-after")) actual_bounds("shape1",expected.at("bounds-after"));
            if(expected.contains("paths")) {
                auto const &paths=result.data.at("paths").as_array();
                for(size_t i=0;i<expected.at("paths").as_array().size();++i)
                    actual_bounds(std::string(paths[i].as_object().at("id").as_string()),expected.at("paths").as_array()[i].as_object().at("bounds"));
            }
            EXPECT_TRUE(result.one_undo_step); EXPECT_EQ(result.undo_effect,"one-step");
            EXPECT_EQ(document_stamp(doc.get()).revision,before.at("document_revision").to_number<uint64_t>()+1);
            ASSERT_TRUE(DocumentUndo::undo(doc.get())); doc->ensureUpToDate();
            EXPECT_EQ(canonical(doc->getReprRoot()),before.at("xml").as_string()); EXPECT_EQ(selection(),before.at("selection"));
            ASSERT_TRUE(DocumentUndo::redo(doc.get())); doc->ensureUpToDate();
            EXPECT_EQ(canonical(doc->getReprRoot()),after.at("xml").as_string()); EXPECT_EQ(selection(),after.at("selection"));
        } else if(command=="history.undo" || command=="history.redo") {
            EXPECT_EQ(result.data.at("consumed-label"),"Transform objects");
            EXPECT_EQ(document_stamp(doc.get()).revision,before.at("document_revision").to_number<uint64_t>()+1);
            ASSERT_TRUE(command=="history.undo"?DocumentUndo::redo(doc.get()):DocumentUndo::undo(doc.get()));
            doc->ensureUpToDate(); EXPECT_EQ(canonical(doc->getReprRoot()),before.at("xml").as_string());
            EXPECT_EQ(selection(),before.at("selection"));
        } else {
            EXPECT_EQ(after.at("xml"),before.at("xml")); EXPECT_EQ(after.at("document_revision"),before.at("document_revision"));
            EXPECT_EQ(after.at("undo"),before.at("undo")); EXPECT_EQ(after.at("redo"),before.at("redo"));
        }
        std::cout << "P9-NATIVE-OBSERVATION " << boost::json::serialize(boost::json::object{
            {"command",command},{"before",before},{"after",after},{"preview_after",preview_after},
            {"cancel_after",cancel_after},{"settled",DocumentUndo::fileOperationFreshReady(doc.get())},
            {"status",static_cast<int>(result.status)}}) << std::endl;
    }
    void receipt(Request r, std::string const &code, std::string const &branch, bool detail=true, std::string const &mutation_state="") {
        auto before=state(); auto result=call(r); auto after=state();
        ASSERT_TRUE(result.error); ASSERT_EQ(result.error->code,code);
        if(detail) { ASSERT_TRUE(result.error_details.contains("reason")); EXPECT_EQ(result.error_details.at("reason"),boost::json::value(branch)); }
        EXPECT_EQ(before,after); EXPECT_FALSE(result.one_undo_step);
        ASSERT_TRUE(result.error_details.contains("mutation_state"));
        EXPECT_EQ(result.error_details.at("mutation_state"),boost::json::value(mutation_state.empty()
            ? (code=="internal-error"?"rolled-back":"none") : mutation_state));
        if(code=="corner-edit-failed") {
            EXPECT_EQ(result.status,mutation_state=="rolled-back"?Status::Failed:Status::Rejected);
            ASSERT_TRUE(result.error_details.at("reason").is_string());
            EXPECT_FALSE(result.error_details.at("reason").as_string().empty());
        }
        bool settled=lease?DocumentUndo::fileOperationOutputReady(doc.get()):DocumentUndo::fileOperationFreshReady(doc.get());
        EXPECT_TRUE(settled);
        if(HasFailure()) return;
        emit_receipt(result.action,result.error->code,branch,before,after,
            result.error_details.at("reason"),result.error_details.at("mutation_state"),settled);
    }
    void emit_receipt(std::string const &command, std::string const &code, std::string const &branch,
                      boost::json::object const &before, boost::json::object const &after,
                      boost::json::value const &native_reason, boost::json::value const &mutation_state,
                      bool settled, bool native_only=false) {
        if(HasFailure()) return;
        boost::json::object o{{"obligation","P9:"+command+":command-service:"+code+":"+branch},
            {"command",command},{"code",code},{"branch",branch},{"before",before},{"after",after},{"settled",settled},
            {"native_reason",native_reason},{"mutation_state",mutation_state}};
        if(native_only) {
            o["boundary"]="apply_corner_plan; missing caller interaction";
            o["native_only"]=true;
            o["code_origin"]="descriptor mapping; no CLI Record observed";
        }
        std::cout << (native_only ? "P9-NATIVE-OBSERVATION " : "P9-OBSERVATION ") << boost::json::serialize(o) << std::endl;
    }
    std::unique_ptr<SPDocument> doc; DispatchContext context; TokenStore tokens; std::string token_id; std::shared_ptr<void> lease;
};
TEST_F(P9Independent, geometry_move) { independent("geometry.move",boost::json::parse(R"p9({"ids": ["shape1"], "dx": {"value": 2, "unit": "px"}, "dy": {"value": 3, "unit": "px"}})p9").as_object(),boost::json::parse(R"p9({"applied-affine": [1, 0, 0, 1, 2, 3], "bounds-before": [0, 0, 10, 10], "bounds-after": [2, 3, 12, 13]})p9").as_object()); }
TEST_F(P9Independent, geometry_resize) { independent("geometry.resize",boost::json::parse(R"p9({"ids": ["shape1"], "width": {"value": 20, "unit": "px"}, "height": {"value": 30, "unit": "px"}, "bbox": "geometric", "anchor": "nw"})p9").as_object(),boost::json::parse(R"p9({"bounds-before": [0, 0, 10, 10], "bounds-after": [0, 0, 20, 30]})p9").as_object()); }
TEST_F(P9Independent, geometry_rotate) { independent("geometry.rotate",boost::json::parse(R"p9({"ids": ["shape1"], "angle": 90, "pivot": {"x": {"value": 0, "unit": "px"}, "y": {"value": 0, "unit": "px"}}})p9").as_object(),boost::json::parse(R"p9({"bounds-after": [-10, 0, 0, 10]})p9").as_object()); }
TEST_F(P9Independent, geometry_skew) { independent("geometry.skew",boost::json::parse(R"p9({"ids": ["shape1"], "axis": "x", "angle": 45, "pivot": {"x": {"value": 0, "unit": "px"}, "y": {"value": 0, "unit": "px"}}})p9").as_object(),boost::json::parse(R"p9({"bounds-after": [0, 0, 20, 10]})p9").as_object()); }
TEST_F(P9Independent, geometry_flip) { independent("geometry.flip",boost::json::parse(R"p9({"ids": ["shape1"], "axis": "horizontal", "pivot": {"x": {"value": 0, "unit": "px"}, "y": {"value": 0, "unit": "px"}}})p9").as_object(),boost::json::parse(R"p9({"bounds-after": [-10, 0, 0, 10]})p9").as_object()); }
TEST_F(P9Independent, geometry_matrix) { independent("geometry.matrix",boost::json::parse(R"p9({"ids": ["shape1"], "matrix": [1, 0, 0, 1, 2, 3]})p9").as_object(),boost::json::parse(R"p9({"applied-affine": [1, 0, 0, 1, 2, 3], "bounds-after": [2, 3, 12, 13]})p9").as_object()); }
TEST_F(P9Independent, geometry_boolean) { independent("geometry.boolean",boost::json::parse(R"p9({"ids": ["shape1", "shape2"], "op": "difference", "empty-result": "reject"})p9").as_object(),boost::json::parse(R"p9({"paths": [{"bounds": [0, 0, 5, 10]}], "consumed-ids": ["shape1", "shape2"]})p9").as_object()); }
TEST_F(P9Independent, geometry_offset) { independent("geometry.offset",boost::json::parse(R"p9({"ids": ["shape1"], "distance": {"value": 2, "unit": "px"}, "corner": "miter"})p9").as_object(),boost::json::parse(R"p9({"paths": [{"bounds": [-2, -2, 12, 12]}]})p9").as_object()); }
TEST_F(P9Independent, geometry_corners) { independent("geometry.corners",boost::json::parse(R"p9({"ids": ["child1"], "radius": {"value": 1, "unit": "px"}, "scope": "all", "mode": "round"})p9").as_object(),boost::json::parse(R"p9({"paths": [{"bounds": [30, 0, 34, 4]}]})p9").as_object()); }
TEST_F(P9Independent, D12_geometry_move_applyAffine_nonfinite_or_singular_affine) {
doc->getObjectById("shape1")->getRepr()->setAttribute("transform","scale(0)"); doc->ensureUpToDate(); seed_redo();
receipt(request("geometry.move",boost::json::parse(R"p9({"ids": ["shape1"], "dx": {"value": 2, "unit": "px"}, "dy": {"value": 3, "unit": "px"}})p9").as_object()),"invalid-transform","applyAffine: nonfinite or singular affine");
}
TEST_F(P9Independent, D12_geometry_move_collective_bounds_absent) {
doc->getObjectById("child1")->getRepr()->setAttribute("width","0");doc->getObjectById("child2")->getRepr()->setAttribute("width","0"); doc->ensureUpToDate(); seed_redo();
receipt(request("geometry.move",boost::json::parse(R"p9({"ids": ["group1"], "dx": {"value": 2, "unit": "px"}, "dy": {"value": 3, "unit": "px"}})p9").as_object()),"no-bounds","collective bounds absent");
}
TEST_F(P9Independent, D12_geometry_resize_applyAffine_nonfinite_or_singular_affine) {
doc->getObjectById("shape1")->getRepr()->setAttribute("transform","scale(0)"); doc->ensureUpToDate(); seed_redo();
receipt(request("geometry.resize",boost::json::parse(R"p9({"ids": ["shape1"], "width": {"value": 20, "unit": "px"}, "height": {"value": 30, "unit": "px"}, "bbox": "geometric", "anchor": "nw"})p9").as_object()),"invalid-transform","applyAffine: nonfinite or singular affine");
}
TEST_F(P9Independent, D12_geometry_resize_collective_bounds_absent) {
doc->getObjectById("child1")->getRepr()->setAttribute("width","0");doc->getObjectById("child2")->getRepr()->setAttribute("width","0"); doc->ensureUpToDate(); seed_redo();
receipt(request("geometry.resize",boost::json::parse(R"p9({"ids": ["group1"], "width": {"value": 20, "unit": "px"}, "height": {"value": 30, "unit": "px"}, "bbox": "geometric", "anchor": "nw"})p9").as_object()),"no-bounds","collective bounds absent");
}
TEST_F(P9Independent, D12_geometry_resize_collective_dimension_zero) {
doc->getObjectById("shape1")->getRepr()->setAttribute("d","M0,0 L10,0"); doc->ensureUpToDate(); seed_redo();
receipt(request("geometry.resize",boost::json::parse(R"p9({"ids": ["shape1"], "width": {"value": 20, "unit": "px"}, "height": {"value": 30, "unit": "px"}, "bbox": "geometric", "anchor": "nw"})p9").as_object()),"zero-dimension","collective dimension zero");
}
TEST_F(P9Independent, D12_geometry_rotate_applyAffine_nonfinite_or_singular_affine) {
doc->getObjectById("shape1")->getRepr()->setAttribute("transform","scale(0)"); doc->ensureUpToDate(); seed_redo();
receipt(request("geometry.rotate",boost::json::parse(R"p9({"ids": ["shape1"], "angle": 90, "pivot": {"x": {"value": 0, "unit": "px"}, "y": {"value": 0, "unit": "px"}}})p9").as_object()),"invalid-transform","applyAffine: nonfinite or singular affine");
}
TEST_F(P9Independent, D12_geometry_rotate_collective_bounds_absent) {
doc->getObjectById("child1")->getRepr()->setAttribute("width","0");doc->getObjectById("child2")->getRepr()->setAttribute("width","0"); doc->ensureUpToDate(); seed_redo();
receipt(request("geometry.rotate",boost::json::parse(R"p9({"ids": ["group1"], "angle": 90, "pivot": {"x": {"value": 0, "unit": "px"}, "y": {"value": 0, "unit": "px"}}})p9").as_object()),"no-bounds","collective bounds absent");
}
TEST_F(P9Independent, D12_geometry_skew_applyAffine_nonfinite_or_singular_affine) {
doc->getObjectById("shape1")->getRepr()->setAttribute("transform","scale(0)"); doc->ensureUpToDate(); seed_redo();
receipt(request("geometry.skew",boost::json::parse(R"p9({"ids": ["shape1"], "axis": "x", "angle": 45, "pivot": {"x": {"value": 0, "unit": "px"}, "y": {"value": 0, "unit": "px"}}})p9").as_object()),"invalid-transform","applyAffine: nonfinite or singular affine");
}
TEST_F(P9Independent, D12_geometry_skew_collective_bounds_absent) {
doc->getObjectById("child1")->getRepr()->setAttribute("width","0");doc->getObjectById("child2")->getRepr()->setAttribute("width","0"); doc->ensureUpToDate(); seed_redo();
receipt(request("geometry.skew",boost::json::parse(R"p9({"ids": ["group1"], "axis": "x", "angle": 45, "pivot": {"x": {"value": 0, "unit": "px"}, "y": {"value": 0, "unit": "px"}}})p9").as_object()),"no-bounds","collective bounds absent");
}
TEST_F(P9Independent, D12_geometry_skew_collective_dimension_zero) {
doc->getObjectById("shape1")->getRepr()->setAttribute("d","M0,0 L10,0"); doc->ensureUpToDate(); seed_redo();
receipt(request("geometry.skew",boost::json::parse(R"p9({"ids": ["shape1"], "axis": "x", "angle": 45, "pivot": {"x": {"value": 0, "unit": "px"}, "y": {"value": 0, "unit": "px"}}})p9").as_object()),"zero-dimension","collective dimension zero");
}
TEST_F(P9Independent, D12_geometry_flip_applyAffine_nonfinite_or_singular_affine) {
doc->getObjectById("shape1")->getRepr()->setAttribute("transform","scale(0)"); doc->ensureUpToDate(); seed_redo();
receipt(request("geometry.flip",boost::json::parse(R"p9({"ids": ["shape1"], "axis": "horizontal", "pivot": {"x": {"value": 0, "unit": "px"}, "y": {"value": 0, "unit": "px"}}})p9").as_object()),"invalid-transform","applyAffine: nonfinite or singular affine");
}
TEST_F(P9Independent, D12_geometry_flip_collective_bounds_absent) {
doc->getObjectById("child1")->getRepr()->setAttribute("width","0");doc->getObjectById("child2")->getRepr()->setAttribute("width","0"); doc->ensureUpToDate(); seed_redo();
receipt(request("geometry.flip",boost::json::parse(R"p9({"ids": ["group1"], "axis": "horizontal", "pivot": {"x": {"value": 0, "unit": "px"}, "y": {"value": 0, "unit": "px"}}})p9").as_object()),"no-bounds","collective bounds absent");
}
TEST_F(P9Independent, D12_geometry_matrix_applyAffine_nonfinite_or_singular_affine) {
doc->getObjectById("shape1")->getRepr()->setAttribute("transform","scale(0)"); doc->ensureUpToDate(); seed_redo();
receipt(request("geometry.matrix",boost::json::parse(R"p9({"ids": ["shape1"], "matrix": [1, 0, 0, 1, 2, 3]})p9").as_object()),"invalid-transform","applyAffine: nonfinite or singular affine");
}
TEST_F(P9Independent, D12_geometry_move_unexpected_service_exception_rollback_before_return) {
seed_redo(); DocumentUndo::setAtomicSettlementFaultForTesting([](auto s) {return s==DocumentUndo::AtomicSettlementStage::HistoryInsertion;});
receipt(request("geometry.move",boost::json::parse(R"p9({"ids": ["shape1"], "dx": {"value": 2, "unit": "px"}, "dy": {"value": 3, "unit": "px"}})p9").as_object()),"internal-error","unexpected service exception; rollback before return");
}
TEST_F(P9Independent, D12_geometry_resize_unexpected_service_exception_rollback_before_return) {
seed_redo(); DocumentUndo::setAtomicSettlementFaultForTesting([](auto s) {return s==DocumentUndo::AtomicSettlementStage::HistoryInsertion;});
receipt(request("geometry.resize",boost::json::parse(R"p9({"ids": ["shape1"], "width": {"value": 20, "unit": "px"}, "height": {"value": 30, "unit": "px"}, "bbox": "geometric", "anchor": "nw"})p9").as_object()),"internal-error","unexpected service exception; rollback before return");
}
TEST_F(P9Independent, D12_geometry_rotate_unexpected_service_exception_rollback_before_return) {
seed_redo(); DocumentUndo::setAtomicSettlementFaultForTesting([](auto s) {return s==DocumentUndo::AtomicSettlementStage::HistoryInsertion;});
receipt(request("geometry.rotate",boost::json::parse(R"p9({"ids": ["shape1"], "angle": 90, "pivot": {"x": {"value": 0, "unit": "px"}, "y": {"value": 0, "unit": "px"}}})p9").as_object()),"internal-error","unexpected service exception; rollback before return");
}
TEST_F(P9Independent, D12_geometry_skew_unexpected_service_exception_rollback_before_return) {
seed_redo(); DocumentUndo::setAtomicSettlementFaultForTesting([](auto s) {return s==DocumentUndo::AtomicSettlementStage::HistoryInsertion;});
receipt(request("geometry.skew",boost::json::parse(R"p9({"ids": ["shape1"], "axis": "x", "angle": 45, "pivot": {"x": {"value": 0, "unit": "px"}, "y": {"value": 0, "unit": "px"}}})p9").as_object()),"internal-error","unexpected service exception; rollback before return");
}
TEST_F(P9Independent, D12_geometry_flip_unexpected_service_exception_rollback_before_return) {
seed_redo(); DocumentUndo::setAtomicSettlementFaultForTesting([](auto s) {return s==DocumentUndo::AtomicSettlementStage::HistoryInsertion;});
receipt(request("geometry.flip",boost::json::parse(R"p9({"ids": ["shape1"], "axis": "horizontal", "pivot": {"x": {"value": 0, "unit": "px"}, "y": {"value": 0, "unit": "px"}}})p9").as_object()),"internal-error","unexpected service exception; rollback before return");
}
TEST_F(P9Independent, D12_geometry_matrix_unexpected_service_exception_rollback_before_return) {
seed_redo(); DocumentUndo::setAtomicSettlementFaultForTesting([](auto s) {return s==DocumentUndo::AtomicSettlementStage::HistoryInsertion;});
receipt(request("geometry.matrix",boost::json::parse(R"p9({"ids": ["shape1"], "matrix": [1, 0, 0, 1, 2, 3]})p9").as_object()),"internal-error","unexpected service exception; rollback before return");
}
TEST_F(P9Independent, D12_geometry_boolean_unexpected_service_exception_rollback_before_return) {
seed_redo(); DocumentUndo::setAtomicSettlementFaultForTesting([](auto s) {return s==DocumentUndo::AtomicSettlementStage::HistoryInsertion;});
receipt(request("geometry.boolean",boost::json::parse(R"p9({"ids": ["shape1", "shape2"], "op": "difference", "empty-result": "reject"})p9").as_object()),"internal-error","unexpected service exception; rollback before return");
}
TEST_F(P9Independent, D12_geometry_offset_unexpected_service_exception_rollback_before_return) {
seed_redo(); DocumentUndo::setAtomicSettlementFaultForTesting([](auto s) {return s==DocumentUndo::AtomicSettlementStage::HistoryInsertion;});
receipt(request("geometry.offset",boost::json::parse(R"p9({"ids": ["shape1"], "distance": {"value": 2, "unit": "px"}, "corner": "miter"})p9").as_object()),"internal-error","unexpected service exception; rollback before return");
}
TEST_F(P9Independent, D12_geometry_corners_unexpected_service_exception_rollback_before_return) {
seed_redo(); DocumentUndo::setAtomicSettlementFaultForTesting([](auto s) {return s==DocumentUndo::AtomicSettlementStage::HistoryInsertion;});
receipt(request("geometry.corners",boost::json::parse(R"p9({"ids": ["child1"], "radius": {"value": 1, "unit": "px"}, "scope": "all", "mode": "round"})p9").as_object()),"internal-error","unexpected service exception; rollback before return");
}
TEST_F(P9Independent, D12_geometry_boolean_BooleanOperandReason_InvalidOperand) {
 doc->ensureUpToDate(); seed_redo();
receipt(request("geometry.boolean",boost::json::parse(R"p9({"ids": ["missing", "shape2"], "op": "difference", "empty-result": "reject"})p9").as_object()),"unknown-id","BooleanOperandReason::InvalidOperand");
}
TEST_F(P9Independent, D12_geometry_boolean_BooleanOperandReason_OverlappingOperands) {
 doc->ensureUpToDate(); seed_redo();
receipt(request("geometry.boolean",boost::json::parse(R"p9({"ids": ["group1", "child1"], "op": "difference", "empty-result": "reject"})p9").as_object()),"incompatible-operands","BooleanOperandReason::OverlappingOperands");
}
TEST_F(P9Independent, D12_geometry_boolean_BooleanOperandReason_Unavailable) {
doc->getObjectById("shape1")->getRepr()->setAttribute("style","display:none"); doc->ensureUpToDate(); seed_redo();
receipt(request("geometry.boolean",boost::json::parse(R"p9({"ids": ["shape1", "shape2"], "op": "difference", "empty-result": "reject"})p9").as_object()),"unavailable","BooleanOperandReason::Unavailable");
}
TEST_F(P9Independent, D12_geometry_boolean_BooleanOperandReason_GroupEffect) {
doc->getObjectById("group1")->getRepr()->setAttribute("opacity","0.5"); doc->ensureUpToDate(); seed_redo();
receipt(request("geometry.boolean",boost::json::parse(R"p9({"ids": ["group1", "shape2"], "op": "difference", "empty-result": "reject"})p9").as_object()),"incompatible-operands","BooleanOperandReason::GroupEffect");
}
TEST_F(P9Independent, D12_geometry_boolean_BooleanOperandReason_NotAShape) {
 doc->ensureUpToDate(); seed_redo();
receipt(request("geometry.boolean",boost::json::parse(R"p9({"ids": ["image1", "shape2"], "op": "difference", "empty-result": "reject"})p9").as_object()),"incompatible-operands","BooleanOperandReason::NotAShape");
}
TEST_F(P9Independent, D12_geometry_boolean_BooleanOperandReason_EmptyGeometry) {
doc->getObjectById("shape1")->getRepr()->setAttribute("d",""); doc->ensureUpToDate(); seed_redo();
receipt(request("geometry.boolean",boost::json::parse(R"p9({"ids": ["shape1", "shape2"], "op": "difference", "empty-result": "reject"})p9").as_object()),"incompatible-operands","BooleanOperandReason::EmptyGeometry");
}
TEST_F(P9Independent, D12_geometry_boolean_BooleanOperandReason_EmptyGroup) {
doc->getObjectById("child1")->deleteObject();doc->getObjectById("child2")->deleteObject(); doc->ensureUpToDate(); seed_redo();
receipt(request("geometry.boolean",boost::json::parse(R"p9({"ids": ["group1", "shape2"], "op": "difference", "empty-result": "reject"})p9").as_object()),"incompatible-operands","BooleanOperandReason::EmptyGroup");
}
TEST_F(P9Independent, D12_geometry_boolean_BooleanOperandReason_UnsafeTransform) {
doc->getObjectById("shape1")->getRepr()->setAttribute("transform","scale(0)"); doc->ensureUpToDate(); seed_redo();
receipt(request("geometry.boolean",boost::json::parse(R"p9({"ids": ["shape1", "shape2"], "op": "difference", "empty-result": "reject"})p9").as_object()),"invalid-transform","BooleanOperandReason::UnsafeTransform");
}
TEST_F(P9Independent, D12_geometry_boolean_BooleanOperandReason_EmptyResult) {
 doc->ensureUpToDate(); seed_redo();
receipt(request("geometry.boolean",boost::json::parse(R"p9({"ids": ["shape1", "contour1"], "op": "difference", "empty-result": "reject"})p9").as_object()),"empty-result","BooleanOperandReason::EmptyResult");
}
TEST_F(P9Independent, D12_geometry_offset_prepare_no_closed_compatible_sources) {
doc->getObjectById("shape1")->getRepr()->setAttribute("d","M0,0 L10,0");doc->ensureUpToDate();seed_redo();
receipt(request("geometry.offset",boost::json::parse(R"p9({"ids": ["shape1"], "distance": {"value": 2, "unit": "px"}, "corner": "miter"})p9").as_object()),"no-closed-shapes","prepare: no closed compatible sources");
}
TEST_F(P9Independent, D12_geometry_offset_build_failed_geometry) {
seed_redo();
receipt(request("geometry.offset",boost::json::parse(R"p9({"ids": ["shape1"], "distance": {"value": 100, "unit": "px"}, "corner": "miter", "direction": "inward"})p9").as_object()),"offset-failed","build: failed geometry");
}
TEST_F(P9Independent, D12_geometry_corners_requires_single_shape) {
doc->ensureUpToDate();seed_redo();
receipt(request("geometry.corners",boost::json::parse(R"p9({"ids": ["group1"], "radius": {"value": 1, "unit": "px"}, "scope": "all", "mode": "round"})p9").as_object()),"requires-single-shape","requires-single-shape",false);
}
TEST_F(P9Independent, D12_geometry_corners_unsupported_shape) {
doc->getObjectById("child1")->getRepr()->setAttribute("width","0");doc->ensureUpToDate();seed_redo();
receipt(request("geometry.corners",boost::json::parse(R"p9({"ids": ["child1"], "radius": {"value": 1, "unit": "px"}, "scope": "all", "mode": "round"})p9").as_object()),"unsupported-shape","unsupported-shape",false);
}
TEST_F(P9Independent, D12_geometry_corners_path_not_supported) {
doc->ensureUpToDate();seed_redo();
receipt(request("geometry.corners",boost::json::parse(R"p9({"ids": ["shape1"], "radius": {"value": 1, "unit": "px"}, "scope": "all", "mode": "round"})p9").as_object()),"path-not-supported","path-not-supported",false);
}
TEST_F(P9Independent, D12_geometry_corners_non_similarity_transform) {
doc->getObjectById("child1")->getRepr()->setAttribute("transform","scale(2,1)");doc->ensureUpToDate();seed_redo();
receipt(request("geometry.corners",boost::json::parse(R"p9({"ids": ["child1"], "radius": {"value": 1, "unit": "px"}, "scope": "all", "mode": "round"})p9").as_object()),"non-similarity-transform","non-similarity-transform",false);
}
TEST_F(P9Independent, D12_geometry_corners_no_such_node) {
doc->ensureUpToDate();seed_redo();
receipt(request("geometry.corners",boost::json::parse(R"p9({"ids": ["child1"], "radius": {"value": 1, "unit": "px"}, "scope": "nodes", "mode": "round", "nodes": ["0:999"]})p9").as_object()),"no-such-node","no-such-node",false);
}
TEST_F(P9Independent, D12_geometry_corners_CornerEdit_Status_EngineLimit) {
doc->ensureUpToDate();seed_redo();
receipt(request("geometry.corners",boost::json::parse(R"p9({"ids": ["child1"], "radius": {"value": 100, "unit": "px"}, "scope": "all", "mode": "round"})p9").as_object()),"engine-limit","CornerEdit::Status::EngineLimit",false);
}
TEST_F(P9Independent, D12_geometry_corners_document_busy) {
doc->ensureUpToDate();seed_redo();lease=DocumentUndo::holdInteractionOperation(doc.get());ASSERT_TRUE(lease);
receipt(request("geometry.corners",boost::json::parse(R"p9({"ids": ["child1"], "radius": {"value": 1, "unit": "px"}, "scope": "all", "mode": "round"})p9").as_object()),"document-busy","document-busy",false);
}
TEST_F(P9Independent, D12_geometry_corners_no_corners) {
    doc->getObjectById("child1")->getRepr()->setAttribute("rx","1"); doc->ensureUpToDate(); seed_redo();
    receipt(request("geometry.corners",boost::json::parse(R"({"ids":["child1"],"radius":{"value":1,"unit":"px"},"scope":"all","mode":"round"})").as_object()),"no-corners","no-corners",false);
}
TEST_F(P9Independent, D12_geometry_corners_invalid_input) {
    // Valid wire radius, but its document-to-local conversion overflows on this tiny similarity frame.
    doc->getObjectById("child1")->getRepr()->setAttribute("transform","scale(1e-310)"); doc->ensureUpToDate(); seed_redo();
    receipt(request("geometry.corners",boost::json::parse(R"({"ids":["child1"],"radius":{"value":1000000,"unit":"px"},"scope":"all","mode":"round"})").as_object()),"invalid-input","invalid-input",false);
}
TEST_F(P9Independent, NativeOnly_geometry_corners_missing_caller_interaction) {
    namespace Tools = UI::Tools;
    namespace CE = LivePathEffect::CornerEdit;
    seed_redo(); auto before=state();
    auto *shape=cast<SPShape>(doc->getObjectById("child1")); ASSERT_TRUE(shape);
    auto capture=Tools::capture_corner_rounding_document(*shape,0); ASSERT_TRUE(capture.snapshot);
    CE::Request native_request; native_request.radius=1; native_request.mode=CE::Mode::Round;
    native_request.scope=CE::Scope::All;
    ASSERT_TRUE(Tools::check_corner_plan(false,*capture.snapshot,native_request).ready);
    ASSERT_FALSE(DocumentUndo::interactionActive(doc.get()));
    int current_probes=0;
    // Exercise the real native admission guard without manufacturing a CLI Record.
    // The synchronous adapter owns a transaction before this call; this receipt
    // is native-only and cannot establish an executed CLI refusal obligation.
    auto native=Tools::apply_corner_plan(*shape,nullptr,*capture.snapshot,native_request,
        Tools::CornerCommitProtocol::CallerOwnedAtomic,[&] { ++current_probes; return true; });
    ASSERT_EQ(native.outcome,Tools::CornerRoundingController::Outcome::Rejected);
    EXPECT_TRUE(native.refused_by_document); EXPECT_FALSE(native.mutation_started);
    EXPECT_EQ(current_probes,0);
    EXPECT_EQ(native.reason,"The caller must own an active atomic document interaction.");
    auto after=state(); EXPECT_EQ(after,before);
    bool settled=DocumentUndo::fileOperationFreshReady(doc.get()); EXPECT_TRUE(settled);
    emit_receipt("geometry.corners","corner-edit-failed","apply refused before mutation",
        before,after,boost::json::value(native.reason),"none",settled,true);
}
TEST_F(P9Independent, Fault_geometry_offset_stale_dependency_before_check) {
    seed_redo();
    ScopedCliFaultPlanForTesting fault({{{"geometry.offset.before-dependency-check", CliFaultKind::StaleDependency}}, {}});
    receipt(request("geometry.offset", boost::json::parse(R"({"ids":["shape1"],"distance":{"value":2,"unit":"px"},"corner":"miter"})").as_object()),
        "stale-dependency", "commit: geometry_still_matches false");
    EXPECT_EQ(fault.plan().faults.at(0).visits, 1);
}
TEST_F(P9Independent, Fault_geometry_offset_commit_refusal_after_publication) {
    seed_redo();
    ScopedCliFaultPlanForTesting fault({{{"geometry.offset.after-native-commit", CliFaultKind::CommitRefusal}}, {}});
    receipt(request("geometry.offset", boost::json::parse(R"({"ids":["shape1"],"distance":{"value":2,"unit":"px"},"corner":"miter","delete-originals":true,"select-results":true})").as_object()),
        "offset-failed", "commit failure; caller rollback", true, "rolled-back");
    EXPECT_EQ(fault.plan().faults.at(0).visits, 1);
}
TEST_F(P9Independent, Fault_geometry_corners_apply_refused_before_mutation) {
    seed_redo();
    ScopedCliFaultPlanForTesting fault({{{"geometry.corners.before-native-apply", CliFaultKind::CommitRefusal}}, {}});
    receipt(request("geometry.corners", boost::json::parse(R"({"ids":["child1"],"radius":{"value":1,"unit":"px"},"scope":"all","mode":"round"})").as_object()),
        "corner-edit-failed", "apply refused before mutation", false, "none");
    EXPECT_EQ(fault.plan().faults.at(0).visits, 1);
}
TEST_F(P9Independent, D12_geometry_corners_failed_edit_contract_evidence) {
    seed_redo(); int calls=0;
    // The second cancellation probe follows the real LPE definition write.
    context.cancelled=[&] { return ++calls>=2; };
    receipt(request("geometry.corners",boost::json::parse(R"({"ids":["child1"],"radius":{"value":1,"unit":"px"},"scope":"all","mode":"round"})").as_object()),
        "corner-edit-failed","apply failed after mutation; settled rollback",false,"rolled-back");
    context.cancelled={}; EXPECT_EQ(calls,2);
}

} // namespace
