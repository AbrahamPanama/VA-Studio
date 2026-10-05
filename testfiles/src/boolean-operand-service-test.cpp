// SPDX-License-Identifier: GPL-2.0-or-later
// Boolean operand service: native document service, explicit operand roles and caller-owned settlement.
#include <gtest/gtest.h>
#include <algorithm>
#include <string>
#include <tuple>
#include <vector>

#include "document.h"
#include "document-undo.h"
#include "inkscape.h"
#include "object/object-set.h"
#include "object/sp-item.h"
#include "object/sp-path.h"
#include "path/path-boolop.h"
#include "svg/svg.h"
#include "ui/toolbar/boolean-assist.h"
#include "undo-stack-observer.h"
#include "xml/attribute-record.h"
#include "xml/node.h"
#include "xml/repr.h"

using namespace Inkscape;
using namespace Inkscape::UI::Toolbar;
using Op = BooleanOperandOp;
using Policy = BooleanEmptyPolicy;
using Reason = BooleanOperandReason;

namespace {
// Canonical XML snapshot: exact names, node types, content, attributes and sibling order;
// only attribute order is normalized. No geometry/style values or metadata are omitted.
std::string canonical(XML::Node const *node)
{
    auto field = [](char const *value) {
        std::string text = value ? value : "";
        return std::to_string(text.size()) + ':' + text;
    };
    std::string out = std::to_string(static_cast<int>(node->type())) + field(node->name()) + field(node->content());
    std::vector<std::pair<std::string, std::string>> attrs;
    for (auto const &attr : node->attributeList()) {
        attrs.emplace_back(g_quark_to_string(attr.key), attr.value.pointer());
    }
    std::sort(attrs.begin(), attrs.end());
    out += '[';
    for (auto const &[name, value] : attrs) out += field(name.c_str()) + field(value.c_str());
    out += ']';
    for (auto *child = node->firstChild(); child; child = child->next()) out += canonical(child);
    return out + '/';
}

std::string const a = "<rect id='a' width='20' height='20' style='fill:#ff0000'/>";
std::string const b = "<rect id='b' x='10' width='20' height='20' style='fill:#0000ff'/>";
std::string const cutter = "<path id='b' d='M10,-5 V25'/>";

struct HistoryObserver : UndoStackObserver {
    int commits = 0, undos = 0, redos = 0;
    void notifyUndoCommitEvent(Event *) override { ++commits; }
    void notifyUndoEvent(Event *) override { ++undos; }
    void notifyRedoEvent(Event *) override { ++redos; }
    void notifyUndoExpired(Event *) override {}
    void notifyClearUndoEvent() override {}
    void notifyClearRedoEvent() override {}
};

class BooleanOperandServiceTest : public testing::Test {
protected:
    void SetUp() override {
        if (!Application::exists()) Application::create(false);
    }
    std::unique_ptr<SPDocument> make(std::string const &body = a + b) {
        auto doc = SPDocument::createNewDocFromMem(
            "<svg xmlns='http://www.w3.org/2000/svg' xmlns:inkscape='http://www.inkscape.org/namespaces/inkscape' "
            "xmlns:sodipodi='http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd' "
            "width='100' height='100' viewBox='0 0 100 100'><defs/>" + body +
            "<g id='unselected'/><rect id='outside' x='80' y='80' width='5' height='5'/></svg>");
        doc->ensureUpToDate();
        DocumentUndo::setUndoSensitive(doc.get(), true);
        DocumentUndo::clearUndo(doc.get());
        DocumentUndo::clearRedo(doc.get());
        return doc;
    }
    SPItem *item(SPDocument &doc, char const *id) { return cast<SPItem>(doc.getObjectById(id)); }
    BooleanOperandResult apply(SPDocument &doc, Op op, Policy policy = Policy::Refuse,
                               std::vector<std::string> const &ids = {"a", "b"}) {
        std::vector<SPItem *> roots;
        for (auto const &id : ids) roots.push_back(item(doc, id.c_str()));
        return apply_boolean_assist(&doc, roots, op, policy);
    }
    void bounds(SPDocument &doc, std::string const &id, double left, double right, double top = 0, double bottom = 20) {
        auto *output = item(doc, id.c_str());
        ASSERT_TRUE(output);
        ASSERT_TRUE(is<SPPath>(output));
        auto box = output->documentGeometricBounds();
        ASSERT_TRUE(box);
        EXPECT_NEAR(box->left(), left, 1e-6);
        EXPECT_NEAR(box->right(), right, 1e-6);
        EXPECT_NEAR(box->top(), top, 1e-6);
        EXPECT_NEAR(box->bottom(), bottom, 1e-6);
    }
    bool contains(SPDocument &doc, std::vector<std::string> const &ids, double x, double y = 10) {
        for (auto const &id : ids) {
            auto *output = item(doc, id.c_str());
            if ((*boolean_operand_path(output) * output->i2doc_affine()).winding(Geom::Point(x, y))) return true;
        }
        return false;
    }
    void refusedUnchanged(SPDocument &doc, Op op, Reason reason, std::vector<std::string> const &ids = {"a", "b"}) {
        auto before = canonical(doc.getReprRoot());
        auto result = apply(doc, op, Policy::Refuse, ids);
        EXPECT_EQ(result.status, BooleanOperandStatus::Refused);
        EXPECT_EQ(result.reason, reason);
        EXPECT_FALSE(result.detail.empty());
        EXPECT_TRUE(result.output_ids.empty());
        EXPECT_TRUE(result.consumed_ids.empty());
        EXPECT_EQ(canonical(doc.getReprRoot()), before);
    }
};

class OrderedRolesTest : public BooleanOperandServiceTest, public testing::WithParamInterface<std::tuple<bool, bool>> {};
TEST_P(OrderedRolesTest, InputOrderWinsOverStacking)
{
    auto [reverse_stack, reverse_request] = GetParam();
    auto doc = make(reverse_stack ? b + a : a + b);
    auto ids = reverse_request ? std::vector<std::string>{"b", "a"} : std::vector<std::string>{"a", "b"};
    auto result = apply(*doc, Op::Difference, Policy::Refuse, ids);
    ASSERT_EQ(result.status, BooleanOperandStatus::Applied);
    ASSERT_EQ(result.output_ids.size(), 1u);
    EXPECT_EQ(result.consumed_ids, ids);
    bounds(*doc, result.output_ids[0], reverse_request ? 20 : 0, reverse_request ? 30 : 10);
    EXPECT_EQ(contains(*doc, result.output_ids, 5), !reverse_request);
    EXPECT_FALSE(contains(*doc, result.output_ids, 15));
    EXPECT_EQ(contains(*doc, result.output_ids, 25), reverse_request);
}
INSTANTIATE_TEST_SUITE_P(StackAndRequest, OrderedRolesTest, testing::Combine(testing::Bool(), testing::Bool()));

class OperationTest : public BooleanOperandServiceTest, public testing::WithParamInterface<Op> {};
TEST_P(OperationTest, GeometryOutputsAndConsumedIDs)
{
    auto op = GetParam();
    auto doc = make(op == Op::Division ? cutter + a : b + a);
    auto outside = canonical(doc->getObjectById("outside")->getRepr());
    auto result = apply(*doc, op);
    ASSERT_EQ(result.status, BooleanOperandStatus::Applied);
    EXPECT_EQ(result.reason, Reason::None);
    EXPECT_TRUE(result.detail.empty());
    EXPECT_EQ(result.consumed_ids, (std::vector<std::string>{"a", "b"}));
    EXPECT_FALSE(doc->getObjectById("b"));
    ASSERT_EQ(result.output_ids.size(), op == Op::Division ? 2u : 1u);
    EXPECT_NE(std::find(result.output_ids.begin(), result.output_ids.end(), "a"), result.output_ids.end());
    EXPECT_EQ(doc->getObjectsBySelector("path").size(), result.output_ids.size());
    EXPECT_EQ(canonical(doc->getObjectById("outside")->getRepr()), outside);
    EXPECT_TRUE(doc->getObjectById("unselected"));
    if (op == Op::Division) {
        std::vector<double> lefts;
        for (auto const &id : result.output_ids) {
            auto box = item(*doc, id.c_str())->documentGeometricBounds();
            ASSERT_TRUE(box);
            lefts.push_back(box->left());
            bounds(*doc, id, box->left(), box->left() + 10);
        }
        std::sort(lefts.begin(), lefts.end());
        EXPECT_EQ(lefts, (std::vector<double>{0, 10}));
        EXPECT_TRUE(contains(*doc, result.output_ids, 5));
        EXPECT_TRUE(contains(*doc, result.output_ids, 15));
        EXPECT_FALSE(contains(*doc, result.output_ids, 25));
    } else {
        bounds(*doc, result.output_ids[0], op == Op::Intersection ? 10 : 0,
               op == Op::Difference ? 10 : (op == Op::Intersection ? 20 : 30));
        EXPECT_EQ(contains(*doc, result.output_ids, 5), op != Op::Intersection);
        EXPECT_EQ(contains(*doc, result.output_ids, 15), op == Op::Union || op == Op::Intersection);
        EXPECT_EQ(contains(*doc, result.output_ids, 25), op == Op::Union || op == Op::Exclusion);
    }
    auto reopened = SPDocument::createNewDocFromMem(sp_repr_save_buf(doc->getReprDoc()).raw());
    ASSERT_TRUE(reopened);
    reopened->ensureUpToDate();
    EXPECT_EQ(reopened->getObjectsBySelector("path").size(), result.output_ids.size());
    for (auto const &id : result.output_ids) {
        ASSERT_TRUE(item(*reopened, id.c_str()));
        EXPECT_STREQ(item(*reopened, id.c_str())->getRepr()->attribute("d"),
                     item(*doc, id.c_str())->getRepr()->attribute("d"));
    }
    for (double x : {5, 15, 25}) {
        EXPECT_EQ(contains(*reopened, result.output_ids, x), contains(*doc, result.output_ids, x));
    }
}
INSTANTIATE_TEST_SUITE_P(AllFive, OperationTest, testing::Values(Op::Union, Op::Difference, Op::Intersection, Op::Exclusion, Op::Division));

TEST_F(BooleanOperandServiceTest, DifferenceSubtractsAllOrderedOthers)
{
    auto doc = make("<rect id='c' x='15' width='5' height='20'/>" + a +
                    "<rect id='b' width='5' height='20'/>");
    auto result = apply(*doc, Op::Difference, Policy::Refuse, {"a", "c", "b"});
    ASSERT_EQ(result.status, BooleanOperandStatus::Applied);
    EXPECT_EQ(result.consumed_ids, (std::vector<std::string>{"a", "c", "b"}));
    ASSERT_EQ(result.output_ids.size(), 1u);
    bounds(*doc, result.output_ids[0], 5, 15);
}

TEST_F(BooleanOperandServiceTest, DivisionRequiresExactlyTwoWithoutWriting)
{
    auto doc = make(a + cutter + "<rect id='c' width='4' height='4'/>");
    for (auto const &ids : {std::vector<std::string>{}, {"a"}, {"a", "b", "c"}}) {
        refusedUnchanged(*doc, Op::Division, Reason::InvalidCount, ids);
    }
}

class EmptyResultTest : public BooleanOperandServiceTest, public testing::WithParamInterface<std::tuple<Op, Policy>> {};
TEST_P(EmptyResultTest, ExplicitPolicyControlsAtomicRefusalOrConsumption)
{
    auto [op, policy] = GetParam();
    auto doc = make(a + (op == Op::Intersection ? "<rect id='b' x='40' width='20' height='20'/>"
                                             : "<rect id='b' width='20' height='20'/>") );
    auto before = canonical(doc->getReprRoot());
    auto result = apply(*doc, op, policy);
    EXPECT_TRUE(result.output_ids.empty());
    if (policy == Policy::Refuse) {
        EXPECT_EQ(result.status, BooleanOperandStatus::Refused);
        EXPECT_EQ(result.reason, Reason::EmptyResult);
        EXPECT_TRUE(result.consumed_ids.empty());
        EXPECT_EQ(canonical(doc->getReprRoot()), before);
    } else {
        EXPECT_EQ(result.status, BooleanOperandStatus::Applied);
        EXPECT_EQ(result.reason, Reason::None);
        EXPECT_EQ(result.consumed_ids, (std::vector<std::string>{"a", "b"}));
        EXPECT_FALSE(doc->getObjectById("a"));
        EXPECT_FALSE(doc->getObjectById("b"));
        EXPECT_TRUE(doc->getObjectById("outside"));
    }
}
INSTANTIATE_TEST_SUITE_P(Policies, EmptyResultTest,
    testing::Combine(testing::Values(Op::Difference, Op::Intersection, Op::Exclusion), testing::Values(Policy::Refuse, Policy::Allow)));

TEST_F(BooleanOperandServiceTest, NestedTransformedGroupIsOneUnionedOperand)
{
    auto doc = make("<g id='a' transform='translate(3,4)'><rect id='l' width='12' height='20'/>"
                    "<g id='nested'><rect id='r' x='8' width='12' height='20'/></g></g>"
                    "<rect id='b' x='8' y='4' width='10' height='20'/>");
    auto result = apply(*doc, Op::Difference);
    ASSERT_EQ(result.status, BooleanOperandStatus::Applied);
    EXPECT_EQ(result.consumed_ids, (std::vector<std::string>{"a", "b"}));
    ASSERT_EQ(result.output_ids.size(), 1u);
    bounds(*doc, result.output_ids[0], 3, 23, 4, 24);
    EXPECT_TRUE(contains(*doc, result.output_ids, 5));
    EXPECT_FALSE(contains(*doc, result.output_ids, 12));
    EXPECT_TRUE(contains(*doc, result.output_ids, 21));
    for (auto id : {"l", "r", "nested", "b"}) EXPECT_FALSE(doc->getObjectById(id));
    EXPECT_TRUE(doc->getObjectById("unselected"));
}

TEST_F(BooleanOperandServiceTest, LayerOperandRetainsItsContainer)
{
    auto doc = make("<g id='a' inkscape:groupmode='layer' transform='translate(3,4)'>"
                    "<g id='nested'><rect id='leaf' width='20' height='20'/></g></g>"
                    "<rect id='b' x='13' y='4' width='20' height='20'/>");
    auto result = apply(*doc, Op::Difference);
    ASSERT_EQ(result.status, BooleanOperandStatus::Applied);
    EXPECT_EQ(result.consumed_ids, (std::vector<std::string>{"a", "b"}));
    ASSERT_EQ(result.output_ids.size(), 1u);
    EXPECT_NE(result.output_ids[0], "a");
    EXPECT_TRUE(doc->getObjectById("a"));
    EXPECT_FALSE(doc->getObjectById("nested"));
    EXPECT_FALSE(doc->getObjectById("leaf"));
    EXPECT_EQ(item(*doc, result.output_ids[0].c_str())->parent, doc->getObjectById("a"));
    bounds(*doc, result.output_ids[0], 3, 13, 4, 24);
}

TEST_F(BooleanOperandServiceTest, LayerWithEmptyDescendantKeepsNativeAtomicRefusal)
{
    auto doc = make("<g id='a' inkscape:groupmode='layer'><g id='empty-before'/>"
                    "<rect id='leaf' width='20' height='20'/></g>" + b);
    refusedUnchanged(*doc, Op::Difference, Reason::EmptyGroup);
    EXPECT_TRUE(doc->getObjectById("empty-before"));
}

TEST_F(BooleanOperandServiceTest, LegacyCutHelperKeepsItsExistingFragmentPolicy)
{
    auto subject = sp_svg_read_pathv("M0,0 H20 V20 H0 Z");
    auto line = sp_svg_read_pathv("M10,-5 V25");
    EXPECT_EQ(pathvector_cut(subject, line).size(), 4u);
    EXPECT_EQ(pathvector_cut(subject, line, true).size(), 2u);
}

TEST_F(BooleanOperandServiceTest, ClosedDivisionCutterPreservesHoleNesting)
{
    auto doc = make(a + "<rect id='b' x='5' y='5' width='10' height='10'/>");
    auto result = apply(*doc, Op::Division);
    ASSERT_EQ(result.status, BooleanOperandStatus::Applied);
    ASSERT_EQ(result.output_ids.size(), 2u);
    int center_count = 0, corner_count = 0;
    for (auto const &id : result.output_ids) {
        center_count += contains(*doc, {id}, 10, 10);
        corner_count += contains(*doc, {id}, 2, 2);
        auto box = item(*doc, id.c_str())->documentGeometricBounds();
        ASSERT_TRUE(box);
        if (box->left() == 0) {
            EXPECT_FALSE(contains(*doc, {id}, 10, 10));
            bounds(*doc, id, 0, 20);
        } else {
            bounds(*doc, id, 5, 15, 5, 15);
        }
    }
    EXPECT_EQ(center_count, 1);
    EXPECT_EQ(corner_count, 1);
}

class IneligibleTest : public BooleanOperandServiceTest,
                       public testing::WithParamInterface<std::tuple<std::string, Reason>> {};
TEST_P(IneligibleTest, WholeOperandRefusedWithReasonAndNoMutation)
{
    auto [operand, reason] = GetParam();
    auto doc = make(a + operand);
    refusedUnchanged(*doc, Op::Union, reason);
    auto result = apply(*doc, Op::Union);
    EXPECT_FALSE(result.offending_id.empty());
}
INSTANTIATE_TEST_SUITE_P(Compatibility, IneligibleTest, testing::Values(
    std::make_tuple("<image id='b' width='20' height='20'/>", Reason::NotAShape),
    std::make_tuple("<path id='b' d=''/>", Reason::EmptyGeometry),
    std::make_tuple("<g id='b'/>", Reason::EmptyGroup),
    std::make_tuple("<g id='b' style='opacity:0.5'><rect id='leaf' width='20' height='20'/></g>", Reason::GroupEffect),
    std::make_tuple("<g id='b'><image id='leaf' width='20' height='20'/></g>", Reason::NotAShape),
    std::make_tuple("<rect id='b' width='20' height='20' sodipodi:insensitive='true'/>", Reason::Unavailable),
    std::make_tuple("<g id='b'><rect id='leaf' width='20' height='20' style='display:none'/></g>", Reason::Unavailable),
    std::make_tuple("<rect id='b' width='20' height='20' transform='scale(0)'/>", Reason::UnsafeTransform)));

TEST_F(BooleanOperandServiceTest, InvalidIdentityAndOverlappingRootsAreAtomic)
{
    auto doc = make(a + "<g id='b'><rect id='leaf' width='20' height='20'/></g>");
    refusedUnchanged(*doc, Op::Union, Reason::OverlappingOperands, {"a", "a"});
    refusedUnchanged(*doc, Op::Union, Reason::OverlappingOperands, {"b", "leaf"});
    refusedUnchanged(*doc, Op::Union, Reason::InvalidOperand, {"a", "missing"});
    auto foreign = make();
    auto before = canonical(doc->getReprRoot());
    auto result = apply_boolean_assist(doc.get(), {item(*doc, "a"), item(*foreign, "b")}, Op::Union, Policy::Allow);
    EXPECT_EQ(result.reason, Reason::InvalidOperand);
    EXPECT_EQ(canonical(doc->getReprRoot()), before);
    EXPECT_EQ(apply_boolean_assist(nullptr, {}, Op::Union, Policy::Allow).reason, Reason::MissingDocument);
    refusedUnchanged(*doc, static_cast<Op>(99), Reason::InvalidOperation);
}

class UndoTest : public BooleanOperandServiceTest, public testing::WithParamInterface<int> {};
TEST_P(UndoTest, CallerOwnsExactlyOneStepAndExactCanonicalUndoRedo)
{
    int scenario = GetParam();
    auto op = scenario < 5 ? static_cast<Op>(scenario) : Op::Difference;
    auto doc = make(scenario == 4 ? a + cutter : scenario == 5 ? a + "<rect id='b' width='20' height='20'/>" :
                    scenario == 6 ? "<g id='a'><rect id='leaf' width='20' height='20'/></g>" + b :
                    scenario == 7 ? "<g id='a' inkscape:groupmode='layer'><rect id='leaf' width='20' height='20'/></g>" + b : a + b);
    auto before = canonical(doc->getReprRoot());
    HistoryObserver history;
    doc->addUndoObserver(history);
    auto transaction = DocumentUndo::beginAtomicInteraction(doc.get());
    ASSERT_TRUE(transaction);
    auto result = apply(*doc, op, scenario == 5 ? Policy::Allow : Policy::Refuse);
    ASSERT_EQ(result.status, BooleanOperandStatus::Applied);
    EXPECT_EQ(history.commits, 0);
    ASSERT_TRUE(transaction->commitAtomically(RC_("Undo", "Ordered boolean test"), "path-union", [] { return true; }));
    EXPECT_EQ(history.commits, 1);
    auto after = canonical(doc->getReprRoot());
    EXPECT_NE(before, after);
    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    EXPECT_EQ(history.undos, 1);
    EXPECT_EQ(canonical(doc->getReprRoot()), before);
    ASSERT_TRUE(DocumentUndo::redo(doc.get()));
    EXPECT_EQ(history.redos, 1);
    EXPECT_EQ(canonical(doc->getReprRoot()), after);
    EXPECT_EQ(history.commits, 1);
    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    EXPECT_FALSE(DocumentUndo::undo(doc.get()));
    doc->removeUndoObserver(history);
}
INSTANTIATE_TEST_SUITE_P(OperationsEmptyAndGroup, UndoTest, testing::Range(0, 8));

TEST_F(BooleanOperandServiceTest, RefusalPreservesCallerPendingEditAndRedo)
{
    auto doc = make(a + "<image id='b' width='20' height='20'/>");
    item(*doc, "outside")->getRepr()->setAttribute("data-previous", "yes");
    DocumentUndo::done(doc.get(), RC_("Undo", "Previous"), "");
    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    auto before = canonical(doc->getReprRoot());
    refusedUnchanged(*doc, Op::Union, Reason::NotAShape);
    ASSERT_TRUE(DocumentUndo::redo(doc.get()));
    EXPECT_STREQ(item(*doc, "outside")->getRepr()->attribute("data-previous"), "yes");
    item(*doc, "outside")->getRepr()->setAttribute("data-pending", "kept");
    refusedUnchanged(*doc, Op::Division, Reason::InvalidCount, {"a", "b", "outside"});
    EXPECT_STREQ(item(*doc, "outside")->getRepr()->attribute("data-pending"), "kept");
    DocumentUndo::done(doc.get(), RC_("Undo", "Pending caller"), "");
    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    EXPECT_FALSE(item(*doc, "outside")->getRepr()->attribute("data-pending"));
    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    EXPECT_EQ(canonical(doc->getReprRoot()), before);
}

TEST_F(BooleanOperandServiceTest, SingleUnionKeepsNativeEvenOddFillAndTransforms)
{
    auto doc = make("<g id='parent' transform='translate(4,6)'><path id='a' transform='translate(2,3)' "
                    "fill-rule='evenodd' d='M0,0 H20 V20 H0 Z M5,5 H15 V15 H5 Z'/></g>");
    auto result = apply(*doc, Op::Union, Policy::Refuse, {"a"});
    ASSERT_EQ(result.status, BooleanOperandStatus::Applied);
    EXPECT_EQ(result.consumed_ids, (std::vector<std::string>{"a"}));
    ASSERT_EQ(result.output_ids.size(), 1u);
    bounds(*doc, result.output_ids[0], 6, 26, 9, 29);
    EXPECT_TRUE(contains(*doc, result.output_ids, 8, 11));
    EXPECT_FALSE(contains(*doc, result.output_ids, 16, 19));
    EXPECT_EQ(item(*doc, result.output_ids[0].c_str())->parent, doc->getObjectById("parent"));
}
} // namespace
