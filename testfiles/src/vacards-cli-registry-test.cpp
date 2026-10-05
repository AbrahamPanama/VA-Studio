// SPDX-License-Identifier: GPL-2.0-or-later
#include <fstream>
#include <cmath>
#include <algorithm>
#include <glib.h>
#include <limits>
#include <boost/json.hpp>
#include <set>

#include <gtest/gtest.h>

#include "actions/actions-vacards-cli.h"
#include "actions/vacards-cli-dispatch.h"
#include "document-undo.h"
#include "document.h"
#include "inkscape.h"
#include "object/sp-item.h"
#include "object/sp-root.h"
#include "selection.h"
#include "xml/repr.h"
using namespace Inkscape;
using namespace Inkscape::VACardsCli;
namespace {
// Independent test oracle for the generated schema vocabulary. Never calls validate_schema.
bool conforms(boost::json::value const &v, boost::json::object const &s)
{
    auto number = [](boost::json::value const &x) { return x.to_number<double>(); };
    if (auto t = s.if_contains("type")) {
        auto type = t->as_string();
        if (type == "object" && !v.is_object()) return false;
        if (type == "array" && !v.is_array()) return false;
        if (type == "string" && !v.is_string()) return false;
        if (type == "boolean" && !v.is_bool()) return false;
        if (type == "null" && !v.is_null()) return false;
        if ((type == "number" || type == "integer") &&
            (!v.is_number() || !std::isfinite(number(v)))) return false;
        if (type == "integer" && std::floor(number(v)) != number(v)) return false;
    }
    if (auto x = s.if_contains("const"); x && *x != v) return false;
    if (auto x = s.if_contains("enum")) {
        if (std::find(x->as_array().begin(), x->as_array().end(), v) == x->as_array().end()) return false;
    }
    if (v.is_number()) {
        if (auto x = s.if_contains("minimum"); x && number(v) < number(*x)) return false;
        if (auto x = s.if_contains("maximum"); x && number(v) > number(*x)) return false;
    }
    if (v.is_string()) {
        auto const &text = v.as_string();
        auto count = g_utf8_strlen(text.data(), text.size());
        if (auto x = s.if_contains("minLength"); x && count < number(*x)) return false;
        if (auto x = s.if_contains("maxLength"); x && count > number(*x)) return false;
        if (auto x = s.if_contains("maxBytes"); x && text.size() > number(*x)) return false;
    }
    if (v.is_object()) {
        auto const &o = v.as_object();
        if (auto x = s.if_contains("required"))
            for (auto const &key : x->as_array()) if (!o.contains(key.as_string())) return false;
        for (auto const &field : o) {
            auto properties = s.if_contains("properties");
            auto rule = properties ? properties->as_object().if_contains(field.key()) : nullptr;
            if (!rule) {
                auto extra = s.if_contains("additionalProperties");
                if (extra && extra->is_bool() && !extra->as_bool()) return false;
                if (extra && extra->is_object()) rule = extra;
            }
            if (rule && !conforms(field.value(), rule->as_object())) return false;
        }
    }
    if (v.is_array()) {
        if (auto x = s.if_contains("minItems"); x && v.as_array().size() < number(*x)) return false;
        if (auto x = s.if_contains("maxItems"); x && v.as_array().size() > number(*x)) return false;
        if (auto x = s.if_contains("items"))
            for (auto const &item : v.as_array()) if (!conforms(item, x->as_object())) return false;
    }
    for (auto keyword : {"anyOf", "oneOf", "allOf"}) {
        if (auto x = s.if_contains(keyword)) {
            unsigned matches = 0;
            for (auto const &rule : x->as_array()) matches += conforms(v, rule.as_object());
            if (std::string_view(keyword) == "anyOf" && !matches) return false;
            if (std::string_view(keyword) == "oneOf" && matches != 1) return false;
            if (std::string_view(keyword) == "allOf" && matches != x->as_array().size()) return false;
        }
    }
    if (auto x = s.if_contains("if")) {
        auto branch = s.if_contains(conforms(v, x->as_object()) ? "then" : "else");
        if (branch && !conforms(v, branch->as_object())) return false;
    }
    return true;
}
class VacardsCliRegistry : public ::testing::Test
{
protected:
    void SetUp() override
    {
        if (!Application::exists())
            Application::create(false);
        reset_for_testing();
    }
    std::unique_ptr<SPDocument> document()
    {
        std::string svg =
            R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100"><path id="a" d="M0,0H10V10H0Z"/><path id="b" d="M30,30H40V40H30Z"/><rect id="corner" x="50" y="50" width="20" height="20"/></svg>)";
        auto doc = SPDocument::createNewDocFromMem(std::span<char const>{svg.data(), svg.size()});
        doc->ensureUpToDate();
        return doc;
    }
    Request request(SPDocument *doc, std::string command, boost::json::object params = {})
    {
        auto stamp = document_stamp(doc);
        return {"test", std::move(command), stamp.id, stamp.revision, std::move(params), false};
    }
};
TEST_F(VacardsCliRegistry, EveryCommandHasValidExampleAndGeneratedSchemas)
{
    // The registry's own commands are all present and every id is unique; package tables (session.*, query.*)
    // add more commands without changing this test.
    std::set<std::string_view> ids;
    for (auto s : command_specs())
        EXPECT_TRUE(ids.insert(s->canonical_id).second) << "duplicate " << s->canonical_id;
    for (std::string_view id : {"system.describe", "system.options", "system.catalog", "system.result-file",
                                "history.undo", "history.redo", "geometry.boolean", "geometry.corners",
                                "geometry.offset", "geometry.resize", "bitmap.histogram", "bitmap.tone-query"})
        EXPECT_TRUE(ids.count(id)) << "missing " << id;
    for (auto s : command_specs()) {
        SCOPED_TRACE(s->canonical_id);
        ASSERT_NE(s->handler, nullptr);
        ASSERT_FALSE(s->example.empty());
        auto value = boost::json::parse(s->example);
        ASSERT_NO_THROW({ EXPECT_TRUE(conforms(value, request_schema(*s))); });
        auto parsed = parse_request(s->example);
        ASSERT_TRUE(parsed.request) << (parsed.error ? parsed.error->message : "");
        // Session/file commands require agent ownership and grants. Session and M2
        // integration tests execute those services; registry still validates their examples.
        if (s->canonical_id.starts_with("session.") || s->canonical_id.starts_with("file."))
            continue;
        for (bool dry : {false, true}) {
            SCOPED_TRACE(dry);
            auto doc = document();
            auto sel = doc->getSelection();
            if (s->canonical_id.starts_with("bitmap.")) {
                std::string svg = R"(<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" width="100" height="100"><image id="image" width="32" height="24" xlink:href="data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAACAAAAAYCAYAAACbU/80AAAAK0lEQVR4nO3OIQEAAAgDMDrRiU6khRg3E/Ornr2kEhAQEBAQEBAQEBBIBx5hyZNb1iuQawAAAABJRU5ErkJggg=="/></svg>)";
                doc = SPDocument::createNewDocFromMem(std::span<char const>{svg.data(), svg.size()});
                doc->ensureUpToDate();
                sel = doc->getSelection();
                sel->add(cast<SPItem>(doc->getObjectById("image")));
            } else {
                sel->add(cast<SPItem>(doc->getObjectById(s->canonical_id == "geometry.corners" ? "corner" : "a")));
                if (s->canonical_id == "geometry.boolean") sel->add(cast<SPItem>(doc->getObjectById("b")));
            }
            if (s->canonical_id.starts_with("history.")) {
                doc->getRoot()->getRepr()->setAttribute("data-probe", "yes");
                DocumentUndo::done(doc.get(), Util::Internal::ContextString("probe"), "");
                if (s->canonical_id == "history.redo") ASSERT_TRUE(DocumentUndo::undo(doc.get()));
            }
            auto r = *parsed.request;
            if (s->needs_document) {
                auto stamp = document_stamp(doc.get());
                r.document = stamp.id;
                r.if_revision = stamp.revision;
            }
            r.dry_run = dry;
            DispatchContext context{nullptr, doc.get(), sel};
            auto record = dispatch(r, context);
            ASSERT_TRUE(record.status == Status::Ok || record.status == Status::Changed ||
                        record.status == Status::Unchanged) << record.reason << ": " << record.message;
            auto output = typed_result(record, "example");
            auto schema = result_schema_descriptor(*s);
            SCOPED_TRACE(boost::json::serialize(schema));
            ASSERT_NO_THROW({ EXPECT_TRUE(conforms(output, schema)) << boost::json::serialize(output); });
            auto wrong = output;
            wrong["created"] = 42;
            EXPECT_FALSE(conforms(wrong, schema));
            wrong = output;
            wrong.erase("status");
            EXPECT_FALSE(conforms(wrong, schema));
        }
    }
}
TEST_F(VacardsCliRegistry, CatalogHashStableSensitiveAndOrderIndependent)
{
    auto a = command_catalog(), b = command_catalog();
    if (auto path = std::getenv("VACARDS_INT_CATALOG_SNAPSHOT")) {
        std::ofstream snapshot(path); snapshot << boost::json::serialize(a);
        ASSERT_TRUE(snapshot.good());
    }
    EXPECT_EQ(a, b);
    auto hash = a.at("hash");
    a.erase("hash");
    EXPECT_EQ(hash.as_string(), catalog_hash(a));
    a.at("commands").as_array()[0].as_object()["version"] = 2;
    EXPECT_NE(hash.as_string(), catalog_hash(a));
    EXPECT_EQ(catalog_hash(boost::json::object{{"a", 1}, {"b", 2}}),
              catalog_hash(boost::json::object{{"b", 2}, {"a", 1}}));
    auto mcp = mcp_descriptors();
    EXPECT_EQ(mcp.size(), command_specs().size());
}
TEST_F(VacardsCliRegistry, RejectsEachInvalidRequestClass)
{
    for (
        auto text :
        {R"({"schema":"va-studio.cli-request/1","id":"x","id":"y","command":"system.catalog","params":{}})",
         R"({"schema":"va-studio.cli-request/1","id":"x","command":"system.catalog","params":{},"surprise":1})",
         R"({"schema":"va-studio.cli-request/1","id":"x","command":"system.options","params":{"halt-on-error":1}})",
         R"({"schema":"va-studio.cli-request/1","id":"x","command":"bitmap.histogram","params":{"brightness":101}})",
         R"({"schema":"va-studio.cli-request/1","id":"x","command":"bitmap.histogram","params":{"brightness":1e999}})",
         R"({"schema":"va-studio.cli-request/1","id":"x","command":"geometry.resize","document":"d1","if_revision":0,"params":{"width":5}})",
         R"({"schema":"va-studio.cli-request/1","id":"x","command":"geometry.resize","document":"d1","if_revision":0,"params":{}})",
         R"({"schema":"va-studio.cli-request/1","id":"x","command":"geometry.corners","document":"d1","if_revision":0,"params":{"radius":{"value":1,"unit":"mm"},"scope":"nodes"}})",
         R"({"schema":"va-studio.cli-request/1","id":"x","command":"geometry.resize","document":"d1","if_revision":0,"params":{"width":{"value":1,"value":2,"unit":"mm"}}})",
         R"({"schema":"va-studio.cli-request/1","id":"x","command":"geometry.resize","document":"d1","if_revision":-1,"params":{"width":{"value":1,"unit":"mm"}}})"}) {
        SCOPED_TRACE(text);
        EXPECT_TRUE(parse_request(text).error);
    }
    std::string invalid = "\xff";
    EXPECT_EQ(parse_request(invalid).error->code, "invalid-utf8");
    DispatchContext context;
    Request r{"x", "bitmap.histogram", {}, {}, {{"brightness", std::numeric_limits<double>::infinity()}}, false};
    EXPECT_EQ(dispatch(r, context).status, Status::Rejected);
}
TEST_F(VacardsCliRegistry, RevisionBoundsRejectWithoutThrowingAndMatchGeneratedSchema)
{
    for (auto spec : command_specs()) {
        auto schema = request_schema(*spec);
        auto const &revision = schema.at("properties").as_object().at("if_revision").as_object();
        EXPECT_EQ(revision.at("type").as_string(), "integer");
        EXPECT_EQ(revision.at("minimum").to_number<unsigned>(), 0u);
        EXPECT_EQ(revision.at("maximum").to_number<std::uint64_t>(), 9007199254740991ULL);
    }
    auto spec = find_command("system.options");
    ASSERT_NE(spec, nullptr);
    for (auto literal : {"1e100", "-1", "1.5", "9007199254740992"}) {
        SCOPED_TRACE(literal);
        std::string text = R"({"schema":"va-studio.cli-request/1","id":"x","command":"system.options","params":{},"if_revision":)" +
                           std::string(literal) + "}";
        RequestParse parsed;
        ASSERT_NO_THROW(parsed = parse_request(text));
        EXPECT_FALSE(parsed.request);
        ASSERT_TRUE(parsed.error);
        EXPECT_EQ(parsed.error->code, std::string_view(literal) == "1.5" ? "wrong-type" : "out-of-range");
        EXPECT_EQ(parsed.error->key, "/if_revision");
        EXPECT_FALSE(conforms(boost::json::parse(text), request_schema(*spec)));
        Record record;
        record.status = Status::Rejected;
        record.reason = parsed.error->code;
        record.message = parsed.error->message;
        record.error = parsed.error;
        auto output = typed_result(record, "x");
        auto const &error = output.at("error").as_object();
        EXPECT_EQ(error.at("code").as_string(), parsed.error->code);
        EXPECT_EQ(error.at("key").as_string(), "/if_revision");
        EXPECT_FALSE(error.at("hint").as_string().empty());
    }
    for (auto literal : {"0", "0.0", "9007199254740991", "9007199254740991.0"}) {
        SCOPED_TRACE(literal);
        std::string text = R"({"schema":"va-studio.cli-request/1","id":"x","command":"system.options","params":{},"if_revision":)" +
                           std::string(literal) + "}";
        RequestParse parsed;
        ASSERT_NO_THROW(parsed = parse_request(text));
        ASSERT_TRUE(parsed.request);
        // Integral semantics apply to the parsed JSON value. The existing Boost
        // decimal parser can round this large .0 spelling by one; its accuracy
        // is separate from the conversion guard and is unchanged by this round.
        auto value = boost::json::parse(text).as_object().at("if_revision");
        auto expected = value.is_double() ? static_cast<std::uint64_t>(value.as_double())
                                         : std::string_view(literal).starts_with("0") ? 0ULL : 9007199254740991ULL;
        EXPECT_EQ(*parsed.request->if_revision, expected);
        EXPECT_TRUE(conforms(boost::json::parse(text), request_schema(*spec)));
    }
}
TEST_F(VacardsCliRegistry, TypedIntegerConversionRejectsUnrepresentableValuesWithoutThrowing)
{
    // No current command has an integer parameter. Exercise the shared conversion
    // with an unbounded descriptor so schema bounds cannot mask overflow.
    ParamSpec parameter{.key = "count", .type = ParamType::Integer};
    TypeDescriptor input{boost::json::object{{"type", "object"}, {"properties", {{"count", {{"type", "integer"}}}}}}};
    ActionSpec spec{.name = "integer-test", .params = std::span<ParamSpec const>(&parameter, 1)};
    spec.input = &input;
    for (auto literal : {"1e100", "-1e100", "9223372036854775808", "-9223372036854775809", "1.5"}) {
        SCOPED_TRACE(literal);
        auto params = boost::json::parse("{\"count\":" + std::string(literal) + "}").as_object();
        ParseResult parsed;
        ASSERT_NO_THROW(parsed = typed_params(spec, params));
        ASSERT_TRUE(parsed.error);
        EXPECT_EQ(parsed.error->code, std::string_view(literal) == "1.5" ? "wrong-type" : "out-of-range");
        EXPECT_EQ(parsed.error->key, "/count");
    }
    for (auto literal : {"-1", "3.0", "9223372036854775807", "-9223372036854775808"}) {
        SCOPED_TRACE(literal);
        auto params = boost::json::parse("{\"count\":" + std::string(literal) + "}").as_object();
        ParseResult parsed;
        ASSERT_NO_THROW(parsed = typed_params(spec, params));
        ASSERT_FALSE(parsed.error);
        EXPECT_EQ(parsed.at("count").integer, params.at("count").to_number<long long>());
    }
}
TEST_F(VacardsCliRegistry, RolledBackDispatchClearsEffectsAndReportsRestoredSelection)
{
    for (auto status : {Status::Failed, Status::Rejected, Status::Cancelled}) {
        auto doc = document();
        auto sel = doc->getSelection();
        sel->add(cast<SPItem>(doc->getObjectById("a")));
        auto before = sp_repr_save_buf(doc->getReprDoc()).raw();
        DispatchContext context{nullptr, doc.get(), sel};
        Record record;
        dispatch_validated(*find_command("geometry.boolean"), {}, context, record, [&](ActionContext &c) {
            EditTransaction transaction(c.document, c.selection);
            ASSERT_TRUE(transaction.active());
            c.document->getRoot()->getRepr()->setAttribute("data-rollback", "temporary");
            c.selection->clear();
            c.record.created = {"temporary"};
            c.record.modified = {"a"};
            c.record.deleted = {"b"};
            c.record.status = status;
        });
        EXPECT_EQ(record.status, status);
        EXPECT_TRUE(record.created.empty());
        EXPECT_TRUE(record.modified.empty());
        EXPECT_TRUE(record.deleted.empty());
        EXPECT_EQ(record.selection_after, (std::vector<std::string>{"a"}));
        EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(), before);
    }
}
TEST_F(VacardsCliRegistry, AliasAndCanonicalReturnIdenticalResults)
{
    DispatchContext context;
    Request r{"x", "system.describe", {}, {}, {}, false};
    auto a = typed_result(dispatch(r, context), r.id);
    r.command = "vacards-describe";
    auto b = typed_result(dispatch(r, context), r.id);
    EXPECT_EQ(a, b);
}
TEST_F(VacardsCliRegistry, ErrorShapeAndLegacySerializationAreSeparate)
{
    DispatchContext context;
    Request r{"x", "geometry.resize", {}, {}, {}, false};
    auto record = dispatch(r, context);
    auto result = typed_result(record, r.id);
    auto const &e = result.at("error").as_object();
    for (auto key : {"code", "message", "hint", "retryable", "details"})
        EXPECT_TRUE(e.contains(key));
    auto legacy = boost::json::parse(to_json_line(record, 1)).as_object();
    EXPECT_FALSE(legacy.contains("normalized_params"));
    EXPECT_EQ(typed_exit_status(Status::Cancelled), 3);
    auto length = report_length(96);
    EXPECT_NEAR(length.at("preferred").as_object().at("value").as_double(), 25.4, 1e-12);
}
TEST_F(VacardsCliRegistry, BooleanFailurePreservesRedoSelectionXmlAndRevision)
{
    auto doc = document();
    auto sel = doc->getSelection();
    auto stamp = document_stamp(doc.get());
    doc->getRoot()->getRepr()->setAttribute("data-redo-probe", "present");
    DocumentUndo::done(doc.get(), Util::Internal::ContextString("probe"), "");
    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    stamp = document_stamp(doc.get());
    auto before = sp_repr_save_buf(doc->getReprDoc()).raw();
    sel->add(cast<SPItem>(doc->getObjectById("a")));
    sel->add(cast<SPItem>(doc->getObjectById("b")));
    auto selection = sel->items_vector();
    DispatchContext context{nullptr, doc.get(), sel};
    auto r = dispatch(request(doc.get(), "geometry.boolean", {{"op", "intersection"}}), context);
    EXPECT_EQ(r.status, Status::Failed);
    EXPECT_EQ(r.reason, "boolean-failed");
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(), before);
    EXPECT_EQ(sel->size(), 2u);
    EXPECT_TRUE(sel->includes(doc->getObjectById("a")));
    EXPECT_TRUE(sel->includes(doc->getObjectById("b")));
    EXPECT_EQ(document_stamp(doc.get()).revision, stamp.revision);
    EXPECT_TRUE(DocumentUndo::redo(doc.get()));
    EXPECT_STREQ(doc->getRoot()->getRepr()->attribute("data-redo-probe"), "present");
    EXPECT_GT(document_stamp(doc.get()).revision, stamp.revision);
}
TEST_F(VacardsCliRegistry, DryRunAndCancellationPreserveLiveDocument)
{
    auto doc = document();
    auto sel = doc->getSelection();
    sel->add(cast<SPItem>(doc->getObjectById("a")));
    auto before = sp_repr_save_buf(doc->getReprDoc()).raw();
    auto stamp = document_stamp(doc.get());
    DispatchContext context{nullptr, doc.get(), sel};
    auto r = request(doc.get(), "geometry.resize", {{"width", {{"value", 20}, {"unit", "px"}}}});
    r.dry_run = true;
    auto result = dispatch(r, context);
    EXPECT_EQ(result.status, Status::Ok);
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(), before);
    EXPECT_EQ(document_stamp(doc.get()).revision, stamp.revision);
    context.cancelled = [] { return true; };
    r.dry_run = false;
    EXPECT_EQ(dispatch(r, context).status, Status::Cancelled);
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(), before);
}
TEST_F(VacardsCliRegistry, OneEditOneUndoAndMonotonicRevision)
{
    auto doc = document();
    auto sel = doc->getSelection();
    sel->add(cast<SPItem>(doc->getObjectById("a")));
    DispatchContext context{nullptr, doc.get(), sel};
    auto before = sp_repr_save_buf(doc->getReprDoc()).raw();
    auto stamp = document_stamp(doc.get());
    auto result =
        dispatch(request(doc.get(), "geometry.resize", {{"width", {{"value", 20}, {"unit", "px"}}}}), context);
    EXPECT_EQ(result.status, Status::Changed);
    EXPECT_TRUE(result.one_undo_step);
    EXPECT_EQ(document_stamp(doc.get()).revision, stamp.revision + 1);
    auto undo = dispatch(request(doc.get(), "history.undo"), context);
    EXPECT_EQ(undo.status, Status::Changed);
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(), before);
    EXPECT_EQ(document_stamp(doc.get()).revision, stamp.revision + 2);
}
TEST_F(VacardsCliRegistry, SessionOptionsDryRunAndPreferredUnits)
{
    DispatchContext context;
    Request r{"options", "system.options", {}, 0, {{"preferred-unit", "in"}}, true};
    EXPECT_EQ(dispatch(r, context).status, Status::Ok);
    EXPECT_EQ(context.preferred_unit, "mm");
    EXPECT_EQ(context.session_revision, 0u);
    r.dry_run = false;
    auto result = dispatch(r, context);
    EXPECT_EQ(result.status, Status::Ok);
    EXPECT_EQ(context.preferred_unit, "in");
    EXPECT_EQ(context.session_revision, 1u);
    EXPECT_EQ(dispatch(r, context).reason, "stale-revision");
    auto s = find_command(r.command);
    EXPECT_FALSE(validate_schema(typed_result(result, r.id), result_schema_descriptor(*s)));
    EXPECT_DOUBLE_EQ(report_length(96, context.preferred_unit).at("preferred").as_object().at("value").as_double(), 1);
}
TEST_F(VacardsCliRegistry, NoOpPreservesRedoAndStaleRequestDoesNotEdit)
{
    auto doc = document();
    auto sel = doc->getSelection();
    sel->add(cast<SPItem>(doc->getObjectById("a")));
    DispatchContext context{nullptr, doc.get(), sel};
    auto stamp = document_stamp(doc.get());
    doc->getRoot()->getRepr()->setAttribute("data-probe", "present");
    DocumentUndo::done(doc.get(), Util::Internal::ContextString("probe"), "");
    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    auto before = sp_repr_save_buf(doc->getReprDoc()).raw();
    auto r = request(doc.get(), "geometry.resize", {{"width", {{"value", 10}, {"unit", "px"}}}, {"bbox", "geometric"}});
    auto result = dispatch(r, context);
    EXPECT_EQ(result.status, Status::Unchanged);
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(), before);
    EXPECT_EQ(result.revision_before, result.revision_after);
    r.if_revision = stamp.revision;
    EXPECT_EQ(dispatch(r, context).reason, "stale-revision");
    ASSERT_TRUE(DocumentUndo::redo(doc.get()));
    EXPECT_STREQ(doc->getRoot()->getRepr()->attribute("data-probe"), "present");
}
TEST_F(VacardsCliRegistry, RecursiveSchemaRejectsNestedErrorsAndHasTypedDefaults)
{
    auto spec = find_command("geometry.offset");
    auto schema = request_schema(*spec);
    auto &props = schema.at("properties").as_object().at("params").as_object().at("properties").as_object();
    EXPECT_TRUE(props.at("outer-only").as_object().at("default").is_bool());
    EXPECT_TRUE(props.at("miter-limit").as_object().at("default").is_number());
    EXPECT_TRUE(props.at("simplify-tolerance").as_object().at("default").is_object());
    auto example = boost::json::parse(spec->example);
    auto &length = example.as_object().at("params").as_object().at("distance").as_object();
    length["extra"] = true;
    EXPECT_TRUE(validate_schema(example, schema));
    length.erase("extra");
    length["value"] = -1;
    EXPECT_TRUE(validate_schema(example, schema));
    length["value"] = 1;
    length["unit"] = "furlong";
    EXPECT_TRUE(validate_schema(example, schema));
    auto escaped =
        R"({"schema":"va-studio.cli-request/1","id":"x","command":"system.options","params":{"halt-on-error":true,"halt\u002don-error":false}})";
    EXPECT_EQ(parse_request(escaped).error->code, "repeated-key");
}

TEST_F(VacardsCliRegistry, SettlementAllocationFailureRestoresXmlSelectionAndRedo)
{
    for (auto stage : {DocumentUndo::AtomicSettlementStage::EventConstruction,
                       DocumentUndo::AtomicSettlementStage::HistoryInsertion}) {
        auto doc = document();
        auto sel = doc->getSelection();
        doc->getRoot()->getRepr()->setAttribute("data-redo-probe", "present");
        DocumentUndo::done(doc.get(), Util::Internal::ContextString("probe"), "");
        ASSERT_TRUE(DocumentUndo::undo(doc.get()));
        sel->add(cast<SPItem>(doc->getObjectById("a")));
        sel->add(cast<SPItem>(doc->getObjectById("b")));
        doc->getRoot()->getRepr()->setAttribute("data-pending", "preserved");
        std::vector<std::string> selected;
        for (auto *item : sel->items()) selected.emplace_back(item->getId());
        auto before = sp_repr_save_buf(doc->getReprDoc()).raw();
        auto stamp = document_stamp(doc.get());
        auto mark = DocumentUndo::undoStackMark(doc.get());
        static DocumentUndo::AtomicSettlementStage fail_at;
        fail_at = stage;
        struct FaultReset { ~FaultReset() { DocumentUndo::setAtomicSettlementFaultForTesting(nullptr); } } reset;
        DocumentUndo::setAtomicSettlementFaultForTesting([](auto point) { return point == fail_at; });
        DispatchContext context{nullptr, doc.get(), sel};
        auto result = dispatch(request(doc.get(), "geometry.boolean", {{"op", "union"}}), context);
        DocumentUndo::setAtomicSettlementFaultForTesting(nullptr);
        EXPECT_EQ(result.status, Status::Failed);
        EXPECT_TRUE(result.created.empty());
        EXPECT_TRUE(result.modified.empty());
        EXPECT_TRUE(result.deleted.empty());
        EXPECT_EQ(result.selection_after, selected);
        EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(), before);
        EXPECT_EQ(document_stamp(doc.get()).revision, stamp.revision);
        EXPECT_EQ(DocumentUndo::undoStackMark(doc.get()), mark);
        EXPECT_EQ(sel->size(), 2u);
        EXPECT_TRUE(sel->includes(doc->getObjectById("a")));
        EXPECT_TRUE(sel->includes(doc->getObjectById("b")));
        ASSERT_TRUE(DocumentUndo::redo(doc.get()));
        EXPECT_STREQ(doc->getRoot()->getRepr()->attribute("data-redo-probe"), "present");
        EXPECT_FALSE(DocumentUndo::redo(doc.get()));
    }
}
TEST_F(VacardsCliRegistry, ScopedBooleanSettlementRestoresXmlSelectionAndRedo)
{
    for (auto stage : {DocumentUndo::AtomicSettlementStage::EventConstruction,
                       DocumentUndo::AtomicSettlementStage::HistoryInsertion}) {
        // A control document establishes the exact Redo outcome with unrelated pending XML.
        auto control = document();
        control->getRoot()->getRepr()->setAttribute("data-redo-probe", "present");
        DocumentUndo::done(control.get(), Util::Internal::ContextString("probe"), "");
        ASSERT_TRUE(DocumentUndo::undo(control.get()));
        control->getRoot()->getRepr()->setAttribute("data-pending", "preserved");
        ASSERT_TRUE(DocumentUndo::redo(control.get()));
        auto expected_redo = sp_repr_save_buf(control->getReprDoc()).raw();
        auto doc = document();
        auto sel = doc->getSelection();
        doc->getRoot()->getRepr()->setAttribute("data-redo-probe", "present");
        DocumentUndo::done(doc.get(), Util::Internal::ContextString("probe"), "");
        ASSERT_TRUE(DocumentUndo::undo(doc.get()));
        sel->add(cast<SPItem>(doc->getObjectById("a")));
        sel->add(cast<SPItem>(doc->getObjectById("b")));
        doc->getRoot()->getRepr()->setAttribute("data-pending", "preserved");
        auto selection_ids = [&] {
            std::vector<std::string> ids;
            for (auto *item : sel->items()) ids.emplace_back(item->getId());
            return ids;
        };
        auto selected = selection_ids();
        auto before = sp_repr_save_buf(doc->getReprDoc()).raw();
        auto stamp = document_stamp(doc.get());
        auto mark = DocumentUndo::undoStackMark(doc.get());
        static DocumentUndo::AtomicSettlementStage fail_at;
        fail_at = stage;
        static bool fault_hit;
        fault_hit = false;
        struct FaultReset { ~FaultReset() { DocumentUndo::setAtomicSettlementFaultForTesting(nullptr); } } reset;
        DocumentUndo::setAtomicSettlementFaultForTesting([](auto point) { if (point == fail_at) { fault_hit = true; return true; } return false; });
        DispatchContext context{nullptr, doc.get(), sel};
        auto owner = DocumentUndo::holdInteractionOperation(doc.get());
        Record result;
        {
            ActionOperationScope scope(context, owner);
            result = dispatch(request(doc.get(), "geometry.boolean", {{"op", "union"}}), context);
        }
        owner.reset();
        EXPECT_TRUE(fault_hit);
        DocumentUndo::setAtomicSettlementFaultForTesting(nullptr);
        EXPECT_EQ(result.status, Status::Failed);
        EXPECT_TRUE(result.created.empty());
        EXPECT_TRUE(result.modified.empty());
        EXPECT_TRUE(result.deleted.empty());
        EXPECT_EQ(result.selection_after, selected);
        EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(), before);
        EXPECT_EQ(document_stamp(doc.get()).revision, stamp.revision);
        EXPECT_EQ(DocumentUndo::undoStackMark(doc.get()), mark);
        EXPECT_EQ(selection_ids(), selected);
        EXPECT_EQ(sel->size(), 2u);
        EXPECT_TRUE(sel->includes(doc->getObjectById("a")));
        EXPECT_TRUE(sel->includes(doc->getObjectById("b")));
        ASSERT_TRUE(DocumentUndo::redo(doc.get()));
        EXPECT_STREQ(doc->getRoot()->getRepr()->attribute("data-redo-probe"), "present");
        EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(), expected_redo);
        EXPECT_FALSE(DocumentUndo::redo(doc.get()));
    }
}
TEST_F(VacardsCliRegistry, ScopedOffsetSettlementRestoresXmlSelectionAndRedo)
{
    for (auto stage : {DocumentUndo::AtomicSettlementStage::EventConstruction,
                       DocumentUndo::AtomicSettlementStage::HistoryInsertion}) {
        // A control document establishes the exact Redo outcome with unrelated pending XML.
        auto control = document();
        control->getRoot()->getRepr()->setAttribute("data-redo-probe", "present");
        DocumentUndo::done(control.get(), Util::Internal::ContextString("probe"), "");
        ASSERT_TRUE(DocumentUndo::undo(control.get()));
        control->getRoot()->getRepr()->setAttribute("data-pending", "preserved");
        ASSERT_TRUE(DocumentUndo::redo(control.get()));
        auto expected_redo = sp_repr_save_buf(control->getReprDoc()).raw();
        auto doc = document();
        auto sel = doc->getSelection();
        doc->getRoot()->getRepr()->setAttribute("data-redo-probe", "present");
        DocumentUndo::done(doc.get(), Util::Internal::ContextString("probe"), "");
        ASSERT_TRUE(DocumentUndo::undo(doc.get()));
        sel->add(cast<SPItem>(doc->getObjectById("a")));
        sel->add(cast<SPItem>(doc->getObjectById("b")));
        doc->getRoot()->getRepr()->setAttribute("data-pending", "preserved");
        auto selection_ids = [&] {
            std::vector<std::string> ids;
            for (auto *item : sel->items()) ids.emplace_back(item->getId());
            return ids;
        };
        auto selected = selection_ids();
        auto before = sp_repr_save_buf(doc->getReprDoc()).raw();
        auto stamp = document_stamp(doc.get());
        auto mark = DocumentUndo::undoStackMark(doc.get());
        static DocumentUndo::AtomicSettlementStage fail_at;
        fail_at = stage;
        static bool fault_hit;
        fault_hit = false;
        struct FaultReset { ~FaultReset() { DocumentUndo::setAtomicSettlementFaultForTesting(nullptr); } } reset;
        DocumentUndo::setAtomicSettlementFaultForTesting([](auto point) { if (point == fail_at) { fault_hit = true; return true; } return false; });
        DispatchContext context{nullptr, doc.get(), sel};
        auto owner = DocumentUndo::holdInteractionOperation(doc.get());
        Record result;
        {
            ActionOperationScope scope(context, owner);
            result = dispatch(request(doc.get(), "geometry.offset", {{"distance", {{"value", 2}, {"unit", "px"}}}, {"delete-originals", true}, {"select-results", true}}), context);
        }
        owner.reset();
        EXPECT_TRUE(fault_hit);
        DocumentUndo::setAtomicSettlementFaultForTesting(nullptr);
        EXPECT_EQ(result.status, Status::Failed);
        EXPECT_TRUE(result.created.empty());
        EXPECT_TRUE(result.modified.empty());
        EXPECT_TRUE(result.deleted.empty());
        EXPECT_EQ(result.selection_after, selected);
        EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(), before);
        EXPECT_EQ(document_stamp(doc.get()).revision, stamp.revision);
        EXPECT_EQ(DocumentUndo::undoStackMark(doc.get()), mark);
        EXPECT_EQ(selection_ids(), selected);
        EXPECT_EQ(sel->size(), 2u);
        EXPECT_TRUE(sel->includes(doc->getObjectById("a")));
        EXPECT_TRUE(sel->includes(doc->getObjectById("b")));
        ASSERT_TRUE(DocumentUndo::redo(doc.get()));
        EXPECT_STREQ(doc->getRoot()->getRepr()->attribute("data-redo-probe"), "present");
        EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(), expected_redo);
        EXPECT_FALSE(DocumentUndo::redo(doc.get()));
    }
}
TEST_F(VacardsCliRegistry, ExplicitOwnerTransactionRestoresPendingXmlSelectionAndRedo)
{
    for (auto stage : {DocumentUndo::AtomicSettlementStage::EventConstruction,
                       DocumentUndo::AtomicSettlementStage::HistoryInsertion}) {
        auto doc = document();
        auto sel = doc->getSelection();
        auto root = doc->getRoot()->getRepr();
        root->setAttribute("data-redo", "present");
        DocumentUndo::done(doc.get(), Util::Internal::ContextString("probe"), "");
        ASSERT_TRUE(DocumentUndo::undo(doc.get()));
        sel->add(cast<SPItem>(doc->getObjectById("b")));
        sel->add(cast<SPItem>(doc->getObjectById("a")));
        auto ids = [&] {
            std::vector<std::string> result;
            for (auto item : sel->items()) result.emplace_back(item->getId());
            return result;
        };
        auto selected = ids();
        root->setAttribute("data-pending", "preserved");
        auto before = sp_repr_save_buf(doc->getReprDoc()).raw();
        auto stamp = document_stamp(doc.get());
        auto mark = DocumentUndo::undoStackMark(doc.get());
        auto owner = DocumentUndo::holdInteractionOperation(doc.get());
        {
            EditTransaction transaction(doc.get(), sel, owner);
            ASSERT_TRUE(transaction.active());
            root->setAttribute("data-pending", "edited");
            sel->clear();
            static DocumentUndo::AtomicSettlementStage fail_at;
            fail_at = stage;
            struct Reset { ~Reset() { DocumentUndo::setAtomicSettlementFaultForTesting(nullptr); } } reset;
            DocumentUndo::setAtomicSettlementFaultForTesting([](auto point) { return point == fail_at; });
            EXPECT_THROW(transaction.commit(Util::Internal::ContextString("probe"), ""), std::bad_alloc);
        }
        EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(), before);
        EXPECT_EQ(ids(), selected);
        EXPECT_EQ(document_stamp(doc.get()).revision, stamp.revision);
        EXPECT_EQ(DocumentUndo::undoStackMark(doc.get()), mark);
        owner.reset();
        ASSERT_TRUE(DocumentUndo::redo(doc.get()));
        EXPECT_STREQ(root->attribute("data-redo"), "present");
        EXPECT_FALSE(DocumentUndo::redo(doc.get()));
    }
}

TEST_F(VacardsCliRegistry, CoalescedCommitsInvalidateRevisionGuards)
{
    auto doc = document();
    auto initial = document_stamp(doc.get());
    doc->getRoot()->getRepr()->setAttribute("data-probe", "one");
    DocumentUndo::maybeDone(doc.get(), "same-key", Util::Internal::ContextString("probe"), "");
    auto first = document_stamp(doc.get());
    auto stale = request(doc.get(), "geometry.resize", {{"width", {{"value", 20}, {"unit", "px"}}}});
    doc->getRoot()->getRepr()->setAttribute("data-probe", "two");
    DocumentUndo::maybeDone(doc.get(), "same-key", Util::Internal::ContextString("probe"), "");
    EXPECT_EQ(first.revision, initial.revision + 1);
    EXPECT_EQ(document_stamp(doc.get()).revision, first.revision + 1);
    DispatchContext context{nullptr, doc.get(), doc->getSelection()};
    EXPECT_EQ(dispatch(stale, context).reason, "stale-revision");
    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    EXPECT_EQ(doc->getRoot()->getRepr()->attribute("data-probe"), nullptr);
    EXPECT_FALSE(DocumentUndo::undo(doc.get()));
}
TEST_F(VacardsCliRegistry, TypedLegacyDryRunIsRejectedWithEnvelopeHint)
{
    auto doc = document();
    auto before = sp_repr_save_buf(doc->getReprDoc()).raw();
    DispatchContext context{nullptr, doc.get(), doc->getSelection()};
    for (auto id : {"geometry.offset", "geometry.corners"}) {
        auto spec = find_command(id);
        auto parsed = parse_request(spec->example);
        ASSERT_TRUE(parsed.request);
        auto r = request(doc.get(), id, parsed.request->params);
        r.params["dry-run"] = true;
        auto record = dispatch(r, context);
        EXPECT_EQ(record.status, Status::Rejected);
        EXPECT_NE(typed_result(record, "x").at("error").as_object().at("hint").as_string().find("dry_run"),
                  boost::json::string::npos);
        auto example = boost::json::parse(spec->example);
        example.as_object()["params"] = r.params;
        auto invalid = parse_request(boost::json::serialize(example));
        ASSERT_TRUE(invalid.error);
        EXPECT_NE(invalid.error->message.find("envelope dry_run"), std::string::npos);
        EXPECT_FALSE(spec->input->schema.at("properties").as_object().contains("dry-run"));
        EXPECT_TRUE(parse_params(*spec, id == std::string_view("geometry.offset")
            ? "distance=1mm,dry-run=true" : "radius=1mm,dry-run=true").ok());
    }
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(), before);
}
TEST_F(VacardsCliRegistry, DisposableOffsetAndCornersReportActualEffects)
{
    for (auto id : {"geometry.offset", "geometry.corners"}) {
        auto doc = document();
        auto sel = doc->getSelection();
        sel->add(cast<SPItem>(doc->getObjectById(id == std::string_view("geometry.corners") ? "corner" : "a")));
        auto before = sp_repr_save_buf(doc->getReprDoc()).raw();
        auto parsed = parse_request(find_command(id)->example);
        auto r = request(doc.get(), id, parsed.request->params);
        if (id == std::string_view("geometry.offset")) r.params["delete-originals"] = true;
        DispatchContext context{nullptr, doc.get(), sel};
        r.dry_run = true;
        auto dry = dispatch(r, context);
        ASSERT_EQ(dry.status, Status::Ok) << dry.message;
        EXPECT_EQ(dry.data.at("validation_level").as_string(), "computed");
        EXPECT_TRUE(dry.dry_run);
        EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(), before);
        EXPECT_EQ(dry.revision_before, dry.revision_after);
        EXPECT_EQ(sel->size(), 1u);
        EXPECT_TRUE(sel->includes(doc->getObjectById(id == std::string_view("geometry.corners") ? "corner" : "a")));
        r.dry_run = false;
        auto real = dispatch(r, context);
        ASSERT_EQ(real.status, Status::Changed) << real.message;
        EXPECT_EQ(dry.created, real.created);
        EXPECT_EQ(dry.modified, real.modified);
        EXPECT_EQ(dry.deleted, real.deleted);
        EXPECT_EQ(dry.selection_after, real.selection_after);
        EXPECT_FALSE(dry.created.empty() && dry.modified.empty() && dry.deleted.empty());
    }
}
TEST_F(VacardsCliRegistry, FileCommandsFailClosedWithoutAgentOwnership)
{
    auto doc = document();
    for (auto id : {"file.new", "file.open", "file.close", "file.import", "file.save", "file.export"}) {
        auto spec = find_command(id); ASSERT_NE(spec, nullptr);
        auto parsed = parse_request(spec->example); ASSERT_TRUE(parsed.request);
        auto r = *parsed.request;
        auto stamp = document_stamp(doc.get()); r.document = stamp.id;
        r.if_revision = spec->needs_document ? stamp.revision : 0;
        DispatchContext context{nullptr, doc.get(), doc->getSelection()};
        auto result = dispatch(r, context);
        EXPECT_EQ(result.reason, "session-required");
        EXPECT_EQ(result.status, Status::Rejected);
        EXPECT_FALSE(validate_schema(typed_result(result, "refused"), result_schema_descriptor(*spec)));
    }
}

TEST_F(VacardsCliRegistry, RecursiveUniqueItemsAreEnforced)
{
    boost::json::object schema{{"type", "array"}, {"uniqueItems", true},
        {"items", boost::json::object{{"type", "integer"}}}};
    EXPECT_FALSE(validate_schema(boost::json::array{1, 2}, schema));
    auto duplicate = validate_schema(boost::json::array{1, 1.0}, schema);
    ASSERT_TRUE(duplicate);
    EXPECT_EQ(duplicate->code, "invalid-argument");
}

TEST_F(VacardsCliRegistry, SchemaIntegersUseValueAndLengthsUseCodePoints)
{
    auto r = parse_request(R"({"schema":"va-studio.cli-request/1","id":"x","command":"system.options","params":{},"if_revision":0.0})");
    ASSERT_TRUE(r.request);
    EXPECT_EQ(r.request->if_revision, 0u);
    boost::json::object integer{{"type", "integer"}};
    EXPECT_FALSE(validate_schema(3.0, integer));
    EXPECT_TRUE(validate_schema(3.5, integer));
    boost::json::object text{{"type", "string"}, {"minLength", 2}, {"maxLength", 2}};
    EXPECT_FALSE(validate_schema("é😀", text));
    EXPECT_TRUE(validate_schema("é", text));
    EXPECT_TRUE(validate_schema("é😀x", text));
    text["maxBytes"] = 5;
    EXPECT_TRUE(validate_schema("é😀", text));
}

TEST_F(VacardsCliRegistry, PendingXmlJoinsSuccessfulAtomicCommitInOriginalOrder)
{
    auto doc = document();
    auto root = doc->getRoot()->getRepr();
    auto before = sp_repr_save_buf(doc->getReprDoc()).raw();
    auto stamp = document_stamp(doc.get());
    root->setAttribute("data-pending", "before-edit");
    {
        EditTransaction transaction(doc.get(), doc->getSelection());
        ASSERT_TRUE(transaction.active());
        root->setAttribute("data-pending", "after-edit");
        transaction.commit(Util::Internal::ContextString("probe"), "");
    }
    auto after = sp_repr_save_buf(doc->getReprDoc()).raw();
    EXPECT_EQ(document_stamp(doc.get()).revision, stamp.revision + 1);
    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(), before);
    EXPECT_FALSE(DocumentUndo::undo(doc.get()));
    ASSERT_TRUE(DocumentUndo::redo(doc.get()));
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(), after);
}

} // namespace

TEST_F(VacardsCliRegistry, RetryabilityUsesTheStructuredErrorCode)
{
    for (auto code : {"resource-unavailable", "publication-unavailable", "publication-conflict",
                      "stale-dependency", "publication-uncertain", "cancelled", "internal-error", "wrong-type"}) {
        Record record; record.action = "file.save"; record.status = Status::Rejected;
        record.reason = "publication-conflict"; // The structured error takes precedence.
        record.error = ParseError{code, "path", "Native refusal."}; record.message = record.error->message;
        auto error = typed_result(record, "retry").at("error").as_object();
        bool expected = std::string_view(code) == "resource-unavailable" ||
                        std::string_view(code) == "publication-unavailable";
        EXPECT_EQ(error.at("code"), code);
        EXPECT_EQ(error.at("retryable"), expected) << code;
        EXPECT_EQ(cli_error(code, "Native refusal.", "Inspect.").at("retryable"), expected);
        EXPECT_EQ(boost::json::parse(to_json_line(record, 1)).as_object().at("error"),
                  (boost::json::object{{"code", code}, {"key", "path"}}));
        record.error.reset(); record.reason = code;
        EXPECT_EQ(typed_result(record, "fallback").at("error").as_object().at("retryable"), expected);
    }
}
TEST_F(VacardsCliRegistry, FileAvailabilityCodesReachDescriptorsAndCatalog)
{
    auto catalog = command_catalog();
    for (auto const &entry : catalog.at("commands").as_array()) {
        auto const &descriptor = entry.as_object();
        std::string id(descriptor.at("id").as_string());
        auto spec = find_command(id); ASSERT_NE(spec, nullptr);
        // These exact pairs were erased as unreachable.
        static std::set<std::pair<std::string, std::string>> const d21_removed{
            {"file.close", "publication-unavailable"}, {"file.close", "resource-unavailable"},
            {"file.import", "publication-unavailable"}, {"file.new", "publication-unavailable"},
            {"file.new", "resource-unavailable"}, {"file.open", "publication-unavailable"}};
        for (auto code : {"resource-unavailable", "publication-unavailable"}) {
            auto count = std::count(spec->error_codes.begin(), spec->error_codes.end(), std::string_view(code));
            EXPECT_EQ(count, id.starts_with("file.") && !d21_removed.contains({id, code}) ? 1 : 0) << id << " " << code;
            EXPECT_EQ(std::count(descriptor.at("error_codes").as_array().begin(),
                                 descriptor.at("error_codes").as_array().end(), boost::json::value(code)), count) << id;
        }
    }
}

// Frozen debt is input, never inferred from observed return codes. These cases
// use the real owner-thread native session capabilities and shared dispatch.
#include <filesystem>
#include <iostream>
#include <map>
#include "actions/vacards-cli-production.h"
#include "actions/vacards-cli-edit-services.h"
#include "actions/vacards-cli-session.h"
#include "actions/actions-vacards-bitmap.h"
#include "io/vacards-cli-files.h"
#include "nesting/nesting-cli-service.h"
#include "xml/node.h"
#include "xml/attribute-record.h"
#include "xml/document.h"
#include "async/bitmap-job-reaper.h"
namespace {
namespace D12 {
using namespace boost::json;
std::filesystem::path root() {
    return std::filesystem::path(__FILE__).parent_path().parent_path().parent_path().parent_path();
}
// Versioned inputs: derive the checkout from this file, since it is not always named source/ (Windows: src/).
std::filesystem::path source_root() {
    return std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
}
value read(std::filesystem::path const &path) {
    std::ifstream in(path); if (!in) throw std::runtime_error("D12 input absent: " + path.string());
    return parse(std::string(std::istreambuf_iterator<char>(in), {}));
}
bool workspace_available(std::vector<std::filesystem::path> const &paths) {
    return std::all_of(paths.begin(), paths.end(), [](auto const &path) { return std::filesystem::exists(path); });
}
std::filesystem::path first_missing_workspace_input(std::vector<std::filesystem::path> const &paths) {
    auto missing = std::find_if(paths.begin(), paths.end(), [](auto const &path) { return !std::filesystem::exists(path); });
    return missing == paths.end() ? std::filesystem::path{} : *missing;
}
value canonical(XML::Node const *n) {
    std::map<std::string, std::string> sorted;
    for (auto const &a : n->attributeList()) sorted[g_quark_to_string(a.key)] = a.value.pointer();
    object attrs; for (auto const &[k,v] : sorted) attrs[k] = v;
    array children; for (auto c=n->firstChild(); c; c=c->next()) children.push_back(canonical(c));
    return array{n->name() ? n->name() : "", attrs, n->content() ? n->content() : "", children};
}
struct Fixture {
    FileState files; Grants grants; TokenStore tokens;
    Nesting::CliSession nest; Bitmap::CliSession bitmap; DispatchContext context;
    std::string session = "INT-D12-native-session";
    unsigned calls = 0;
    explicit Fixture(TokenBudget budget={}) : tokens(budget) {
        if (!Application::exists()) Application::create(false, Application::RuntimePolicy::PreviewHelper);
        Bitmap::recordBitmapMainThread();
        auto path=source_root()/"testfiles/cli_tests/vacards-agent/fixtures/m3/explode.svg";
        std::ifstream in(path); std::string svg(std::istreambuf_iterator<char>(in), {});
        files.document = SPDocument::createNewDocFromMem(std::span<char const>(svg.data(),svg.size()));
        if (!files.document) throw std::runtime_error("D12 document missing");
        auto &doc=*files.document; doc.ensureUpToDate();
        DocumentUndo::done(&doc, Util::Internal::ContextString("D12 fixture"), "document-new");
        DocumentUndo::clearUndo(&doc); DocumentUndo::clearRedo(&doc);
        doc.getReprRoot()->setAttribute("data-d12-history", "undo");
        DocumentUndo::done(&doc, Util::Internal::ContextString("D12 undo seed"), "document-new");
        doc.getReprRoot()->setAttribute("data-d12-history", "redo");
        DocumentUndo::done(&doc, Util::Internal::ContextString("D12 redo seed"), "document-new");
        if (!DocumentUndo::undo(&doc)) throw std::runtime_error("D12 redo seed failed");
        doc.getSelection()->add(doc.getObjectById("untouched"));
        context.document=&doc; context.selection=doc.getSelection(); context.session_revision=37;
        context.command_admission=[this](Request const &r) { return session_command_admission(r,context,files); };
        context.production_handler=[this](Request const &r) {
            ++calls;
            auto &d=*files.document; EditServices edits{d,*d.getSelection(),document_stamp(&d),{}};
            ProductionContext p{files,grants,edits,tokens,[]{},session,"INT-D12-catalog",1,1};
            p.nest=&nest; p.bitmap=&bitmap;
            return execute_production(r,context,p);
        };
    }
    ~Fixture() { bitmap.retire(); nest.retire(); }
    object state() {
        auto &doc=*files.document; doc.ensureUpToDate();
        EditServices edits{doc,*doc.getSelection(),document_stamp(&doc),{}};
        auto h=history_snapshot(edits); if (!h.value) throw std::runtime_error("D12 history unavailable");
        auto label=[](auto const &s)->value { return s ? value(*s) : value(nullptr); };
        array selected, ids;
        for (auto i:doc.getSelection()->items()) selected.emplace_back(i->getId());
        auto t=tokens.snapshot(); for (auto const &id:t.active_tokens) ids.emplace_back(id);
        auto doctype=static_cast<XML::Node *>(doc.getReprDoc())->attribute("doctype");
        return {{"xml",canonical(doc.getReprRoot())},{"doctype",doctype ? value(doctype) : value(nullptr)},{"selection",selected},
            {"document_revision",document_stamp(&doc).revision},{"session_revision",context.session_revision},
            {"undo",object{{"available",h.value->can_undo},{"label",label(h.value->next_undo_label)}}},
            {"redo",object{{"available",h.value->can_redo},{"label",label(h.value->next_redo_label)}}},
            {"tokens",object{{"ids",ids},{"roots",t.active_roots},{"unique-held-bytes",t.charged_bytes}}}};
    }
    Request request(std::string const &command) {
        Request r; r.id="int-d12"; r.command=command;
        r.document=document_stamp(files.document.get()).id;
        auto spec=find_production_command(command);
        auto const &meta=spec->input->schema.at("x-m3-contract").as_object();
        r.if_revision=meta.at("guard_domain")=="session" ? context.session_revision : document_stamp(files.document.get()).revision;
        for (auto const &c:production_commands()) if(c.id==command) r.params=c.example;
        return r;
    }
};
std::set<std::string> r5_skipped_ids() {
    std::set<std::string> ids;
    auto retired=read(root()/"work/cli-b31/evidence/P9/r5-retired-rows.json");
    for (auto const &v:retired.as_object().at("rows").as_array()) {
        auto const &r=v.as_object(); auto disposition=std::string(r.at("disposition").as_string());
        if (disposition=="removed-inapplicable" || disposition=="superseded") ids.emplace(r.at("id").as_string());
    }
    auto actions=read(root()/"work/cli-b31/evidence/P9/r5-owner-actions.json");
    for (auto const &v:actions.as_object().at("rows").as_array()) {
        auto const &r=v.as_object();
        // Owner actions qualify the erase, e.g. "legacy registry erase (D18-style)".
        if (std::string(r.at("next_action").as_string()).rfind("legacy registry erase",0)==0) ids.emplace(r.at("id").as_string());
    }
    return ids;
}
array rows() {
    auto all=read(root()/"work/cli-b31/evidence/P9/waveB-pending-branches.json").as_object().at("INT").as_array();
    auto r5_skipped=r5_skipped_ids();
    auto manifest=read(source_root()/"doc/vacards/cli/m3-outcome-manifest.json");
    std::map<std::string,object> expected;
    for (auto const &c:manifest.as_object().at("commands").as_array())
        for (auto const &e:c.as_object().at("errors").as_array()) {
            auto row=e.as_object(); expected.emplace(std::string(row.at("oracle").as_string()),row);
        }
    std::set<std::string> unique;
    std::set<std::string> const d18_removed{
        "P9:bitmap.explode.contour:identity/admission:session-required:no owning session",
        "P9:bitmap.explode.explode:identity/admission:session-required:no owning session",
        "P9:bitmap.explode.create-contour-only:identity/admission:session-required:no owning session",
        "P9:bitmap.explode.apply-adjustment:identity/admission:session-required:no owning session",
        "P9:nest.solve:identity/admission:session-required:no owning session",
        "P9:nest.solve:identity/admission:session-required:retention requested without session",
        "P9:nest.apply:identity/admission:session-required:no owning session",
        "P9:nest.solve:command-service:engine-limit:native budget exceeded",
        "P9:clip.destructive:command-service:cancelled:Cancelled",
        "P9:clip.set:command-service:no-document:ClipDocumentService::Reason::InvalidDocument",
        "P9:clip.release:command-service:no-document:ClipDocumentService::Reason::InvalidDocument",
        "P9:clip.release:command-service:invalid-transform:ClipDocumentService::Reason::SingularTransform",
    };
    array applicable;
    for (auto &v:all) {
        auto &row=v.as_object(); auto id=std::string(row.at("id").as_string());
        if (!unique.insert(id).second) throw std::runtime_error("Duplicate D12 obligation");
        if (r5_skipped.contains(id)) continue;
        if (row.at("branch").is_null()) { row["layer"]="legacy-unresolved"; applicable.emplace_back(row); continue; }
        auto found=expected.find(id);
        if (found==expected.end()) {
            // These exact 12 M3 rows were removed as unreachable or inapplicable.
            if (d18_removed.contains(id)) continue;
            throw std::runtime_error("Frozen D12 manifest drift: missing "+id);
        }
        auto const &e=found->second;
        if (row.at("branch")!=e.at("native_branch") || row.at("code")!=e.at("code") || row.at("mutation_state")!=e.at("mutation_state"))
            throw std::runtime_error("Frozen D12 manifest drift: "+id);
        row["layer"]=e.at("layer"); row["retryable"]=e.at("retryable"); applicable.emplace_back(row);
    }
    return applicable;
}
array r5_geometry_rows() {
    auto ledger=read(root()/"work/cli-b31/evidence/P9/r5-ledger.json");
    array out;
    for (auto const &v:ledger.as_object().at("rows").as_array()) {
        auto const &r=v.as_object();
        if (std::string(r.at("id").as_string()).starts_with("P9:geometry.") &&
            r.at("branch")=="fresh readiness guard rejects busy document") {
            auto copy=r; copy["retryable"]=true; out.push_back(copy);
        }
    }
    return out;
}
void pending(object const &row, std::string const &why) {
    std::cout<<"INT-D12-PENDING "<<serialize(object{{"id",row.at("id")},{"reason",why}})<<std::endl;
}
void observe(object const &row, Fixture &f, object const &before, std::string const &code,
             bool retryable, std::string const &mutation, object const &actual) {
    auto after=f.state();
    ASSERT_EQ(code,row.at("code").as_string()); ASSERT_EQ(retryable,row.at("retryable").as_bool());
    ASSERT_EQ(mutation,row.at("mutation_state").as_string()); ASSERT_EQ(before,after);
    ASSERT_FALSE(DocumentUndo::interactionActive(f.files.document.get()));
    std::cout<<"P9-OBSERVATION "<<serialize(object{{"obligation",row.at("id")},
        {"branch",row.at("branch")},{"command",row.at("command")},{"code",code},
        {"retryable",retryable},{"mutation_state",mutation},{"before",before},{"after",after},
        {"settled",true},{"actual",actual}})<<std::endl;
}
object wire(Request const &r) {
    return {{"schema","va-studio.cli-request/1"},{"id",r.id},{"command",r.command},
        {"document",*r.document},{"if_revision",*r.if_revision},{"params",r.params}};
}
void layer(std::string const &wanted) {
    unsigned exercised=0;
    for (auto const &v:rows()) {
        auto row=v.as_object(); if (row.at("layer").as_string()!=wanted) continue;
        SCOPED_TRACE(serialize(row));
        if (row.at("branch").is_null()) { pending(row,"Original ledger supplies no branch or mutation state; requires P9 branch assignment, not a removal."); continue; }
        auto branch=std::string(row.at("branch").as_string()); auto cmd=std::string(row.at("command").as_string());
        if (wanted=="transport/schema" && branch.starts_with("parse_request:")) {
            Fixture f; auto r=f.request(cmd); auto before=f.state(); auto bytes=serialize(wire(r));
            if (branch=="parse_request: request-too-large") bytes.append(1048577,' ');
            else if (branch=="parse_request: repeated-key") bytes.insert(bytes.size()-1,",\"id\":\"duplicate\"");
            else if (branch=="parse_request: invalid-utf8") bytes.insert(bytes.size()-1,std::string(1,char(0xff)));
            else if (branch=="parse_request: malformed-json") bytes.pop_back();
            else { ADD_FAILURE()<<"Unknown transport branch"; continue; }
            auto parsed=parse_production_request(bytes);
            ASSERT_FALSE(parsed.request); ASSERT_TRUE(parsed.error); ASSERT_EQ(f.calls,0u);
            // Framing failures intentionally have no trusted action. Preserve that fact.
            observe(row,f,before,parsed.error->code,cli_error(parsed.error->code,parsed.error->message,"").at("retryable").as_bool(),"none",
                {{"parser_error",parsed.error->code},{"parser_message",parsed.error->message},{"trusted_command",parsed.error_command},
                 {"request_command",r.command},{"dispatch_calls",f.calls},{"request_bytes",bytes.size()}});
            ++exercised; continue;
        }
        bool schema=branch=="raw schema/type/range/route/uniqueItems violation" || branch=="BooleanOperandReason::InvalidOperation" || branch=="BooleanOperandReason::InvalidCount";
        bool identity=branch=="missing current document" || branch=="document identity mismatch" || branch.starts_with("supplied guard differs") || branch=="cancellation observed before commit";
        bool readonly=branch=="read-only session rejects requested document mutation";
        bool unknown=branch=="explicit ID cannot resolve";
        bool missing_token=branch=="token missing or wrong kind/parent";
        if (!schema && !identity && !unknown && !missing_token && !readonly) {
            if (branch=="EditTransaction inactive" || branch=="stored document generation/revision differs" || branch=="stored dependency revalidation failed" ||
                branch=="retention allocation/root budget exceeded" || branch=="retention requested without session") continue;
            pending(row,"Branch requires a separate exact reachability fixture; no code-only receipt emitted."); continue;
        }
        Fixture f; auto r=f.request(cmd); auto before=f.state();
        if (schema) {
            // This row explicitly names schema checks. Use a declared property with
            // a guaranteed wrong type, not an unrelated missing-document refusal.
            auto const &properties=find_production_command(cmd)->input->schema.at("properties").as_object();
            if (branch=="BooleanOperandReason::InvalidOperation") r.params["op"]="int-d12-invalid-operation";
            else if (branch=="BooleanOperandReason::InvalidCount") { r.params["op"]="division"; r.params["ids"]=array{"shape1"}; }
            else if (!properties.empty()) r.params[properties.begin()->key()]=nullptr;
            else r.params["int-d12-undeclared"]=true;
        } else if(readonly) f.files.read_only=true;
        else if(branch=="missing current document") f.context.document=nullptr;
        else if(branch=="document identity mismatch") r.document="int-d12-other-document";
        else if(branch.starts_with("supplied guard differs")) ++*r.if_revision;
        else if(branch=="cancellation observed before commit") f.context.cancelled=[]{return true;};
        else if(unknown) {
            if(r.params.contains("ids")) r.params["ids"].as_array().front()="int-d12-missing";
            else if(r.params.contains("id")) r.params["id"]="int-d12-missing";
            else if(r.params.contains("target-id")) r.params["target-id"]="int-d12-missing";
            else if(r.params.contains("payload-id")) r.params["payload-id"]="int-d12-missing";
            else { pending(row,"No explicit target in descriptor example; exact route fixture required."); continue; }
        } else if(missing_token) {
            if(cmd=="bitmap.explode.create-contour-only") r.params={{"contour-token","int-d12-missing-token"}};
            else if(cmd=="nest.apply") r.params={{"solution-token","int-d12-missing-token"}};
            else if(cmd=="nest.solve") r.params["analysis-token"]="int-d12-missing-token";
            else { r.params={{"analysis-token","int-d12-missing-token"}}; if(cmd=="bitmap.explode.contour" || cmd=="bitmap.explode.create-contour-only") r.params["contour"]=object{}; }
        }
        if (!schema) ASSERT_FALSE(validate_schema(r.params,find_production_command(cmd)->input->schema));
        auto result=dispatch(r,f.context); auto typed=typed_result(result,r.id,1);
        ASSERT_TRUE(typed.contains("error"))<<serialize(typed);
        auto const &error=typed.at("error").as_object();
        ASSERT_EQ(result.action,cmd); ASSERT_FALSE(result.one_undo_step);
        ASSERT_TRUE(result.created.empty()); ASSERT_TRUE(result.modified.empty()); ASSERT_TRUE(result.deleted.empty());
        if(schema || identity || readonly) ASSERT_EQ(f.calls,0u); else ASSERT_EQ(f.calls,1u);
        // An absent mutation detail is not copied from the expected manifest:
        // infer 'none' only after exact observed-state equality and no committed IDs.
        ASSERT_EQ(before,f.state());
        std::string mutation="none";
        if (auto p=result.error_details.if_contains("mutation_state")) mutation=std::string(p->as_string());
        observe(row,f,before,std::string(error.at("code").as_string()),error.at("retryable").as_bool(),mutation,
            {{"request",wire(r)},{"result",typed},{"dispatch_calls",f.calls}});
        ++exercised;
    }
    if(wanted!="legacy-unresolved") EXPECT_GT(exercised,0u);
}
// Retained routes are established by successful real native preparation, never
// manufactured TokenPayloads or callbacks that synthesize an expected failure.
Request analyzed_route(Fixture &f, std::string const &cmd) {
    bool nest=cmd.starts_with("nest.");
    auto prep=f.request(nest ? "nest.analyze" : "bitmap.explode.analyze");
    prep.params["retain"]=true;
    auto result=dispatch(prep,f.context);
    if (result.status!=Status::Ok || !result.data.contains("analysis-token"))
        throw std::runtime_error("D12 native preparation failed: "+result.reason+" "+result.message);
    auto r=f.request(cmd);
    r.params={{"analysis-token",result.data.at("analysis-token")}};
    if(cmd=="nest.solve") { r.params["iterations"]=1; r.params["retain"]=true; }
    if(cmd=="bitmap.explode.create-contour-only") {
        auto contour=f.request("bitmap.explode.contour");
        contour.params={{"analysis-token",result.data.at("analysis-token")},{"contour",object{}},{"retain",true}};
        auto contoured=dispatch(contour,f.context);
        if(contoured.status!=Status::Ok) throw std::runtime_error("D12 native contour failed: "+contoured.reason+" "+contoured.message);
        r=f.request(cmd); r.params={{"contour-token",contoured.data.at("contour-token")}};
    }
    if(cmd=="bitmap.explode.contour") { r.params["contour"]=object{}; r.params["retain"]=true; }
    if(cmd=="nest.apply") {
        auto solve=f.request("nest.solve");
        solve.params={{"analysis-token",result.data.at("analysis-token")},{"iterations",1},{"retain",true}};
        auto solved=dispatch(solve,f.context);
        if(solved.status!=Status::Ok) throw std::runtime_error("D12 native solve failed: "+solved.reason+" "+solved.message);
        r=f.request(cmd); r.params={{"solution-token",solved.data.at("solution-token")}};
    }
    return r;
}
void token_cases() {
    unsigned exercised=0;
    for(auto const &v:rows()) {
        auto row=v.as_object(); if(row.at("branch").is_null()) continue;
        auto branch=std::string(row.at("branch").as_string());
        bool stale=branch=="stored document generation/revision differs";
        bool dependency=branch=="stored dependency revalidation failed";
        bool capacity=branch=="retention allocation/root budget exceeded";
        bool no_session=branch=="retention requested without session";
        if(!stale && !dependency && !capacity && !no_session) continue;
        SCOPED_TRACE(serialize(row)); auto cmd=std::string(row.at("command").as_string());
        // A retained nest analysis can never be looked up in an empty session:
        // lookup rejects its session first, before nest.solve's retain check.
        if(no_session && cmd=="nest.solve") { pending(row,"Proposed unreachable: lookup rejects empty-session lineage before solve retention."); continue; }
        bool analysis=cmd=="nest.analyze" || cmd=="bitmap.explode.analyze";
        TokenBudget budget;
        if(capacity && analysis) budget.roots=0;
        if(capacity && !analysis) {
            Fixture measure; (void)analyzed_route(measure,cmd);
            budget.bytes=measure.tokens.snapshot().charged_bytes;
        }
        Fixture f(budget); auto r=f.request(cmd);
        if(stale || dependency || (capacity && !analysis)) r=analyzed_route(f,cmd);
        if(no_session) { f.session.clear(); r.params["retain"]=true; }
        if(stale) {
            f.files.document->getReprRoot()->setAttribute("data-d12-stale","changed");
            DocumentUndo::done(f.files.document.get(),Util::Internal::ContextString("D12 stale token"),"document-new");
            r.if_revision=document_stamp(f.files.document.get()).revision;
        }
        if(dependency) {
            auto id=cmd.starts_with("nest.") ? "shape1" : "image1";
            f.files.document->getObjectById(id)->getRepr()->setAttribute("transform","translate(3,0)");
            f.files.document->ensureUpToDate();
            // Pending XML changes invalidate the native capture without advancing
            // the document commit revision, so the TokenStore freshness guard passes.
            ASSERT_EQ(*r.if_revision,document_stamp(f.files.document.get()).revision);
        }
        auto before=f.state(); auto prior_calls=f.calls;
        auto result=dispatch(r,f.context); auto typed=typed_result(result,r.id,1);
        ASSERT_TRUE(typed.contains("error"))<<serialize(typed);
        ASSERT_EQ(f.calls,prior_calls+1); ASSERT_EQ(result.action,cmd);
        ASSERT_FALSE(result.one_undo_step); ASSERT_TRUE(result.created.empty()); ASSERT_TRUE(result.modified.empty()); ASSERT_TRUE(result.deleted.empty());
        auto const &error=typed.at("error").as_object();
        ASSERT_EQ(before,f.state());
        auto mutation=result.error_details.if_contains("mutation_state");
        observe(row,f,before,std::string(error.at("code").as_string()),error.at("retryable").as_bool(),
            mutation ? std::string(mutation->as_string()) : "none",{{"request",wire(r)},{"result",typed},{"dispatch_calls",f.calls-prior_calls}});
        ++exercised;
    }
    EXPECT_GT(exercised,0u);
}
void transaction_cases() {
    unsigned attempted=0;
    for(auto const &v:r5_geometry_rows()) {
        auto row=v.as_object();
        SCOPED_TRACE(serialize(row)); auto cmd=std::string(row.at("command").as_string());
        Fixture f; auto r=f.request(cmd);
        if (cmd=="geometry.matrix") r.params["matrix"]=array{1,0,0,1,2,0};
        auto before=f.state(); Record result;
        bool fresh_ready_while_busy=true;
        {
            EditTransaction active(f.files.document.get(),f.files.document->getSelection());
            ASSERT_TRUE(active.active()); fresh_ready_while_busy=DocumentUndo::fileOperationFreshReady(f.files.document.get());
            ASSERT_FALSE(fresh_ready_while_busy); result=dispatch(r,f.context);
            ASSERT_EQ(result.reason,"transaction-unavailable")<<result.message;
            active.rollback();
        }
        auto typed=typed_result(result,r.id,1); ASSERT_EQ(typed.at("error").as_object().at("code"),"transaction-unavailable");
        ASSERT_EQ(f.calls,1u); ASSERT_FALSE(result.one_undo_step);
        observe(row,f,before,"transaction-unavailable",true,"none",
            {{"request",wire(r)}, {"result",typed}, {"busy_state","active DocumentUndo interaction"},
             {"fresh_ready_while_busy",fresh_ready_while_busy}});
        ++attempted;
    }
    for(auto const &v:rows()) {
        auto row=v.as_object();
        if(row.at("branch")!="EditTransaction inactive") continue;
        SCOPED_TRACE(serialize(row)); auto cmd=std::string(row.at("command").as_string());
        Fixture f; std::string binding;
        // Release needs an existing relation; prepare it through the real shared
        // dispatch before constructing the refused request and taking snapshots.
        if(cmd=="clip.release" || cmd=="nest.contour-release") {
            auto setup=f.request(cmd=="clip.release" ? "clip.set" : "nest.contour-set");
            if(cmd=="clip.release") setup.params={{"target-id","shape1"},{"cutter-id","contour1"},{"keep-cutter",true}};
            auto prepared=dispatch(setup,f.context);
            ASSERT_EQ(prepared.status,Status::Changed)<<prepared.message;
            if(cmd=="nest.contour-release") binding=std::string(prepared.data.at("binding-ids").as_array().front().as_string());
        }
        if(cmd=="clip.release" || cmd=="nest.contour-release") {
            f.files.document->getReprRoot()->setAttribute("data-d12-redo", "seed");
            DocumentUndo::done(f.files.document.get(),Util::Internal::ContextString("D12 release redo"),"document-new");
            ASSERT_TRUE(DocumentUndo::undo(f.files.document.get()));
        }
        auto r=f.request(cmd);
        if(!binding.empty()) r.params["ids"]=array{binding};
        if(cmd=="geometry.matrix") r.params["matrix"]=array{1,0,0,1,2,0};
        if(cmd=="clip.set") r.params={{"target-id","shape1"},{"cutter-id","contour1"},{"keep-cutter",true}};
        auto before=f.state(); auto calls=f.calls;
        DocumentUndo::setUndoSensitive(f.files.document.get(),false);
        auto result=dispatch(r,f.context);
        DocumentUndo::setUndoSensitive(f.files.document.get(),true);
        auto typed=typed_result(result,r.id,1);
        ASSERT_EQ(before,f.state()); ASSERT_FALSE(DocumentUndo::interactionActive(f.files.document.get()));
        ASSERT_EQ(f.calls,calls+1); ASSERT_TRUE(typed.contains("error"))<<serialize(typed);
        auto const &error=typed.at("error").as_object();
        // Tone admission was repaired to the advertised exact transaction branch.
        ASSERT_EQ(error.at("code"),row.at("code")); ASSERT_EQ(error.at("retryable"),row.at("retryable"));
        ASSERT_FALSE(result.one_undo_step); ASSERT_TRUE(result.created.empty()); ASSERT_TRUE(result.modified.empty()); ASSERT_TRUE(result.deleted.empty());
        auto reason=result.error_details.if_contains("reason");
        if(!reason || *reason!=row.at("branch")) {
            pending(row,"Real Undo-insensitive refusal is a different branch; the exact inactive-transaction row remains unproved.");
            std::cout<<"INT-D12-COUNTEREXAMPLE "<<serialize(object{{"id",row.at("id")},{"request",wire(r)},{"result",typed},{"before",before},{"after",f.state()}})<<std::endl;
        } else {
            observe(row,f,before,std::string(error.at("code").as_string()),error.at("retryable").as_bool(),
                std::string(result.error_details.at("mutation_state").as_string()),{{"request",wire(r)},{"result",typed},{"precondition","Undo insensitive"}});
        }
        ++attempted;
    }
    EXPECT_EQ(attempted,14u);
}
void absent_session_counterexamples() {
    unsigned attempted=0;
    for(auto const &v:rows()) {
        auto row=v.as_object(); if(row.at("branch")!="no owning session" &&
            !(row.at("command")=="nest.solve" && row.at("branch")=="retention requested without session")) continue;
        SCOPED_TRACE(serialize(row)); auto cmd=std::string(row.at("command").as_string());
        Fixture f; auto r=analyzed_route(f,cmd); f.session.clear();
        if(cmd=="bitmap.explode.contour") r.params["retain"]=false;
        auto before=f.state(); auto result=dispatch(r,f.context); auto typed=typed_result(result,r.id,1);
        ASSERT_TRUE(typed.contains("error")); ASSERT_EQ(typed.at("error").as_object().at("code"),"invalid-token");
        ASSERT_EQ(before,f.state()); ASSERT_FALSE(DocumentUndo::interactionActive(f.files.document.get()));
        std::cout<<"INT-D12-COUNTEREXAMPLE "<<serialize(object{{"id",row.at("id")},{"request",wire(r)},{"result",typed},{"before",before},{"after",f.state()}})<<std::endl;
        ++attempted;
    }
    EXPECT_EQ(attempted,0u);
}
} // D12
} // namespace
TEST(INTD12, TransportSchemaExactBranches) {
    auto inputs = std::vector<std::filesystem::path>{D12::root()/"work/cli-b31/evidence/P9/waveB-pending-branches.json"};
    if (!D12::workspace_available(inputs)) GTEST_SKIP() << "D12 evidence workspace absent: " << D12::first_missing_workspace_input(inputs).string() << " (the evidence workspace is not part of this repository)";
    D12::layer("transport/schema");
}
TEST(INTD12, IdentityAdmissionExactBranches) {
    auto inputs = std::vector<std::filesystem::path>{D12::root()/"work/cli-b31/evidence/P9/waveB-pending-branches.json"};
    if (!D12::workspace_available(inputs)) GTEST_SKIP() << "D12 evidence workspace absent: " << D12::first_missing_workspace_input(inputs).string() << " (the evidence workspace is not part of this repository)";
    D12::layer("identity/admission");
}

TEST(INTD12, TokenSessionExactBranches) {
    auto inputs = std::vector<std::filesystem::path>{D12::root()/"work/cli-b31/evidence/P9/waveB-pending-branches.json"};
    if (!D12::workspace_available(inputs)) GTEST_SKIP() << "D12 evidence workspace absent: " << D12::first_missing_workspace_input(inputs).string() << " (the evidence workspace is not part of this repository)";
    D12::token_cases();
}

TEST(INTD12, TransactionAdmissionExactBranches) {
    auto inputs = std::vector<std::filesystem::path>{D12::root()/"work/cli-b31/evidence/P9/waveB-pending-branches.json"};
    if (!D12::workspace_available(inputs)) GTEST_SKIP() << "D12 evidence workspace absent: " << D12::first_missing_workspace_input(inputs).string() << " (the evidence workspace is not part of this repository)";
    D12::transaction_cases();
}
TEST(INTD12, AbsentSessionReachabilityCounterexamples) {
    auto inputs = std::vector<std::filesystem::path>{D12::root()/"work/cli-b31/evidence/P9/waveB-pending-branches.json"};
    if (!D12::workspace_available(inputs)) GTEST_SKIP() << "D12 evidence workspace absent: " << D12::first_missing_workspace_input(inputs).string() << " (the evidence workspace is not part of this repository)";
    D12::absent_session_counterexamples();
}

#include "actions/vacards-cli-fault-testing.h"
#include <thread>
namespace {
boost::json::object engine_state(DispatchContext &context, FileState &files, TokenStore &tokens) {
    using namespace boost::json;
    auto &doc=*files.document; doc.ensureUpToDate();
    EditServices edits{doc,*doc.getSelection(),document_stamp(&doc),{}};
    auto h=history_snapshot(edits); if (!h.value) throw std::runtime_error("Engine history unavailable");
    auto label=[](auto const &s)->value {return s ? value(*s) : value(nullptr);};
    array selected, ids;
    for(auto i:doc.getSelection()->items()) selected.emplace_back(i->getId());
    auto t=tokens.snapshot();for(auto const &id:t.active_tokens) ids.emplace_back(id);
    return {{"xml",D12::canonical(doc.getReprRoot())},{"selection",selected},
        {"document_revision",document_stamp(&doc).revision},{"session_revision",context.session_revision},
        {"undo",object{{"available",h.value->can_undo},{"label",label(h.value->next_undo_label)}}},
        {"redo",object{{"available",h.value->can_redo},{"label",label(h.value->next_redo_label)}}},
        {"tokens",object{{"ids",ids},{"roots",t.active_roots},{"unique-held-bytes",t.charged_bytes}}}};
}
void seed_engine(DispatchContext &context, FileState &files) {
    auto &doc=*files.document;
    DocumentUndo::clearUndo(&doc);DocumentUndo::clearRedo(&doc);
    doc.getReprRoot()->setAttribute("data-fault-history","undo");
    DocumentUndo::done(&doc,Util::Internal::ContextString("fault undo seed"),"");
    doc.getReprRoot()->setAttribute("data-fault-history","redo");
    DocumentUndo::done(&doc,Util::Internal::ContextString("fault redo seed"),"");
    if(!DocumentUndo::undo(&doc))throw std::runtime_error("fault redo seed failed");
    doc.getSelection()->add(doc.getObjectById("untouched"));
}
SessionOptions fault_options() {
    if(!Application::exists())Application::create(false,Application::RuntimePolicy::PreviewHelper);
    Bitmap::recordBitmapMainThread();
    SessionOptions o;o.document_path=(D12::source_root()/"testfiles/cli_tests/vacards-agent/fixtures/m3/explode.svg").string();
    o.grants.read_files={o.document_path};return o;
}
Request engine_request(std::string const &cmd, DispatchContext &context) {
    Request r;r.id="int-waveD";r.command=cmd;r.document=document_stamp(context.document).id;
    auto const &meta=find_production_command(cmd)->input->schema.at("x-m3-contract").as_object();
    r.if_revision=meta.at("guard_domain")=="session" ? context.session_revision : document_stamp(context.document).revision;
    if(cmd=="selection.set")r.params={{"ids",boost::json::array{"bitmap"}}};return r;
}
}
TEST(INTFaultSeam, DefaultsNestedRestoreAndCountedLimits) {
    EXPECT_FALSE(cli_fault("native.prepare",CliFaultKind::Analysis));
    EXPECT_NO_THROW(cli_fault_throw("native.prepare"));
    EXPECT_EQ(cli_counted_limit("tone.members",100000),100000u);
    {
        ScopedCliFaultPlanForTesting outer({{{"native.prepare",CliFaultKind::Analysis,2}},{{"tone.members",3}}});
        EXPECT_FALSE(cli_fault("native.prepare",CliFaultKind::Contour));
        EXPECT_FALSE(cli_fault("native.prepare",CliFaultKind::Analysis));
        EXPECT_EQ(cli_counted_limit("tone.members",100000),3u);
        {
            ScopedCliFaultPlanForTesting inner({{{"native.prepare",CliFaultKind::ServiceException}},{{"tone.members",8}}});
            EXPECT_THROW(cli_fault_throw("native.prepare"),std::runtime_error);
            EXPECT_NO_THROW(cli_fault_throw("native.prepare"));
            EXPECT_EQ(cli_counted_limit("tone.members",5),5u);
            EXPECT_FALSE(cli_fault("native.prepare",CliFaultKind::Analysis));
        }
        EXPECT_TRUE(cli_fault("native.prepare",CliFaultKind::Analysis));
        EXPECT_FALSE(cli_fault("native.prepare",CliFaultKind::Analysis));
        EXPECT_EQ(outer.plan().faults[0].visits,3u);
        EXPECT_EQ(cli_counted_limit("tone.members",100000),3u);
        EXPECT_EQ(cli_counted_limit("different",100000),100000u);
    }
    EXPECT_FALSE(cli_fault("native.prepare",CliFaultKind::Analysis));
    EXPECT_EQ(cli_counted_limit("tone.members",100000),100000u);
}
TEST(INTFaultSeam, ThreadIsolationAndExceptionUnwind) {
    ScopedCliFaultPlanForTesting outer({{{"thread.point",CliFaultKind::MemoryAdmission}},{{"thread.limit",2}}});
    bool clean=false,armed=false;
    std::thread worker([&] {
        clean=!cli_fault("thread.point",CliFaultKind::MemoryAdmission) && cli_counted_limit("thread.limit",99)==99;
        ScopedCliFaultPlanForTesting local({{{"thread.point",CliFaultKind::StaleDependency}}, {}});
        armed=cli_fault("thread.point",CliFaultKind::StaleDependency);
    });worker.join();EXPECT_TRUE(clean);EXPECT_TRUE(armed);
    try {ScopedCliFaultPlanForTesting inner({{{"throw.point",CliFaultKind::ServiceException}}, {}});cli_fault_throw("throw.point");}
    catch(std::runtime_error const &) {}
    EXPECT_TRUE(cli_fault("thread.point",CliFaultKind::MemoryAdmission));
    EXPECT_EQ(cli_counted_limit("thread.limit",99),2u);
}
TEST(INTFaultSeam, AllNamedKindsAreIndependent) {
    for(auto kind:{CliFaultKind::Analysis,CliFaultKind::Contour,CliFaultKind::Encoding,CliFaultKind::Publication,
                  CliFaultKind::Renderer,CliFaultKind::MemoryAdmission,CliFaultKind::StaleCapture,
                  CliFaultKind::StaleDependency,CliFaultKind::CommitRefusal}) {
        ScopedCliFaultPlanForTesting scope({{{"native.stage",kind}}, {}});
        EXPECT_FALSE(cli_fault("other.stage",kind));EXPECT_TRUE(cli_fault("native.stage",kind));
        EXPECT_FALSE(cli_fault("native.stage",kind));
    }
}
TEST(INTFaultSeam, NativePostMutationExceptionRollsBackAndSettles) {
    D12::Fixture f;auto before=f.state();
    try {
        ScopedCliFaultPlanForTesting scope({{{"native.after-mutation",CliFaultKind::ServiceException}}, {}});
        EditTransaction transaction(f.context.document,f.context.selection);ASSERT_TRUE(transaction.active());
        f.context.document->getReprRoot()->setAttribute("data-fault-partial","written");
        ASSERT_NE(D12::canonical(f.context.document->getReprRoot()),before.at("xml"));
        cli_fault_throw("native.after-mutation");FAIL()<<"expected fault";
    } catch(std::runtime_error const &) {}
    EXPECT_EQ(f.state(),before);EXPECT_FALSE(DocumentUndo::interactionActive(f.context.document));
}
TEST(INTFaultSeam, UnarmedProductionSelectionAndHistoryRemainNative) {
    with_cli_engine_for_testing(fault_options(),[&](auto &context,auto &files,auto &tokens,auto const &execute) {
        seed_engine(context,files);auto before=engine_state(context,files,tokens);
        auto query=execute(engine_request("history.query",context));ASSERT_EQ(query.status,Status::Ok);
        EXPECT_EQ(engine_state(context,files,tokens),before);
        auto clear=execute(engine_request("selection.clear",context));ASSERT_EQ(clear.status,Status::Changed);
        EXPECT_TRUE(context.selection->isEmpty());EXPECT_EQ(context.session_revision,before.at("session_revision").as_uint64()+1);
        auto set=engine_request("selection.set",context);set.params={{"ids",boost::json::array{"untouched"}}};
        ASSERT_EQ(execute(set).status,Status::Changed);EXPECT_TRUE(context.selection->includes(context.document->getObjectById("untouched")));
        auto xml=D12::canonical(files.document->getReprRoot());
        ASSERT_EQ(execute(engine_request("history.undo",context)).status,Status::Changed);
        EXPECT_NE(D12::canonical(files.document->getReprRoot()),xml);
        ASSERT_EQ(execute(engine_request("history.redo",context)).status,Status::Changed);
        EXPECT_EQ(D12::canonical(files.document->getReprRoot()),xml);
        EXPECT_FALSE(DocumentUndo::interactionActive(context.document));
    });
}
TEST(INTD12, EngineContainmentExactBranches) {
    using namespace boost::json;
    auto manifest=D12::read(D12::source_root()/"doc/vacards/cli/m3-outcome-manifest.json");unsigned count=0;
    for(auto const &cmd:{"selection.set","selection.clear","history.query","history.undo","history.redo"}) {
        object row;
        for(auto const &c:manifest.as_object().at("commands").as_array()) if(c.as_object().at("id")==cmd)
            for(auto const &e:c.as_object().at("errors").as_array()) if(e.as_object().at("code")=="internal-error")row=e.as_object();
        ASSERT_FALSE(row.empty());
        with_cli_engine_for_testing(fault_options(),[&](auto &context,auto &files,auto &tokens,auto const &execute) {
            seed_engine(context,files);auto before=engine_state(context,files,tokens);
            auto request=engine_request(cmd,context);
            ScopedCliFaultPlanForTesting scope({{{"engine.before-service",CliFaultKind::ServiceException}}, {}});
            auto result=execute(request);auto after=engine_state(context,files,tokens);auto typed=typed_result(result,request.id,1);
            ASSERT_EQ(scope.plan().faults[0].visits,1u);ASSERT_EQ(result.status,Status::Failed);
            ASSERT_EQ(result.reason,"internal-error");ASSERT_EQ(before,after);
            ASSERT_FALSE(DocumentUndo::interactionActive(context.document));ASSERT_FALSE(result.one_undo_step);
            ASSERT_FALSE(typed.at("error").as_object().at("retryable").as_bool());
            std::cout<<"P9-OBSERVATION "<<serialize(object{{"obligation",row.at("oracle")},{"branch",row.at("native_branch")},
                {"command",cmd},{"code",result.reason},{"retryable",false},{"mutation_state","none"},
                {"before",before},{"after",after},{"settled",true},
                {"actual",object{{"result",typed},{"fault_point","engine.before-service"},{"fault_visits",scope.plan().faults[0].visits},
                                {"native_write_started",false},{"interaction_active",false}}}})<<std::endl;++count;
        });
    }
    EXPECT_EQ(count,5u);
}

namespace {
namespace D12 {
array legacy_rows() {
    auto ledger=read(root()/"work/cli-b31/evidence/P9/deferred-errors.json");
    auto r5_skipped=r5_skipped_ids();
    array out;for(auto const &v:ledger.as_object().at("rows").as_array()) {
        auto const &r=v.as_object();if(!r5_skipped.contains(std::string(r.at("id").as_string())) && std::string(r.at("id").as_string()).starts_with("error:") && r.at("disposition")=="pending")out.push_back(r);
    }return out;
}
struct LegacyFixture : Fixture {
    std::filesystem::path dir;
    LegacyFixture() {
        auto p=g_dir_make_tmp("int-waveD-XXXXXX",nullptr);if(!p)throw std::runtime_error("temporary fixture directory failed");
        dir=std::filesystem::canonical(p);g_free(p);grants.read_roots={dir.string()};grants.write_roots={dir.string()};
        write("source.svg","<svg xmlns=\"http://www.w3.org/2000/svg\"><rect width=\"10\" height=\"10\"/></svg>");
        context.file_handler=[this](Request const &r){++calls;return execute_file(r,context,files,grants);};
    }
    ~LegacyFixture(){std::error_code ec;std::filesystem::remove_all(dir,ec);}
    std::string write(std::string const &name,std::string const &bytes) {
        auto path=dir/name;std::ofstream out(path,std::ios::binary);out<<bytes;out.close();return path.string();
    }
    Request request(std::string const &cmd) {
        auto parsed=parse_production_request(find_command(cmd)->example);
        if(!parsed.request)throw std::runtime_error("legacy example invalid");
        auto r=*parsed.request;r.id="legacy-waveD";r.document=document_stamp(context.document).id;
        r.if_revision=find_command(cmd)->needs_document ? document_stamp(context.document).revision : context.session_revision;
        if(r.params.contains("path"))r.params["path"]=(dir/"source.svg").string();
        r.params["discard"]=true;
        if(cmd!="file.open" && cmd!="file.new" && cmd!="file.close")r.params.erase("discard");
        return r;
    }
};
void legacy_pending(object const &row,std::string const &why,bool removal=false) {
    std::cout<<"INT-D12-LEGACY-PENDING "<<serialize(object{{"id",row.at("id")},{"reason",why},
        {"removal_proposal",removal}})<<std::endl;
}
void legacy_observe(object const &row,LegacyFixture &f,object const &before,Record const &result,Request const &r,object extra={}) {
    auto typed=typed_result(result,r.id,1);ASSERT_TRUE(typed.contains("error"))<<serialize(typed);
    auto const &error=typed.at("error").as_object();
    ASSERT_EQ(error.at("code"),row.at("code"))<<result.message;
    ASSERT_EQ(error.at("retryable"),row.at("retryable"));ASSERT_EQ(result.action,std::string(row.at("command").as_string()));
    ASSERT_EQ(before,f.state());ASSERT_FALSE(DocumentUndo::interactionActive(f.context.document));
    ASSERT_FALSE(result.one_undo_step);ASSERT_FALSE(result.publication_persisted);
    if(row.at("code")=="nonfinite-number")extra["native_request_serialization"]=serialize(wire(r));
    else extra["request"]=wire(r);extra["result"]=typed;extra["dispatch_calls"]=f.calls;
    observe(row,f,before,std::string(error.at("code").as_string()),error.at("retryable").as_bool(),"none",extra);
}
void legacy_schema() {
    unsigned count=0;
    for(auto const &v:legacy_rows()) {
        auto row=v.as_object();if(row.at("layer")!="transport/schema")continue;
        auto cmd=std::string(row.at("command").as_string()),code=std::string(row.at("code").as_string());
        if(code=="unknown-command"){legacy_pending(row,"Known canonical file command is found in registry; changing its name tests another command. P9 r3 accepted reachability removal.",true);continue;}
        SCOPED_TRACE(serialize(row));LegacyFixture f;auto r=f.request(cmd);bool available=true;
        if(code=="invalid-argument" && cmd!="file.close") {
            auto before=f.state();auto input=wire(r);input["schema"]="wrong-schema";
            auto parsed=parse_production_request(serialize(input));ASSERT_TRUE(parsed.error);ASSERT_FALSE(parsed.request);
            ASSERT_EQ(parsed.error_command,cmd);ASSERT_EQ(parsed.error->code,code);
            observe(row,f,before,parsed.error->code,false,"none",{{"request",input},{"parser_error",parsed.error->code},
                {"parser_key",parsed.error->key},{"trusted_command",parsed.error_command},{"dispatch_calls",f.calls}});++count;continue;
        }

        if(code=="wrong-type") {
            if(cmd=="file.new")r.params["width"]=true;else r.params["path"]=true;
        } else if(code=="missing-required") {
            if(cmd=="file.close")r.params["reconcile"]=object{};
            else if(cmd=="file.new")r.params.erase("width");else r.params.erase("path");
        } else if(code=="not-a-choice") {
            if(cmd=="file.close")available=false;
            else if(cmd=="file.new")r.params["width"]=object{{"value",1},{"unit","invalid-unit"}};
            else if(cmd=="file.save")r.params["embedding-policy"]="invalid-policy";
            else r.params["format"]="invalid-format";
        } else if(code=="nonfinite-number") {
            if(cmd=="file.new")r.params["width"]=object{{"value",std::numeric_limits<double>::infinity()},{"unit","px"}};
            else if(cmd=="file.import")r.params["position"]=object{{"x",object{{"value",std::numeric_limits<double>::infinity()},{"unit","px"}}},{"y",object{{"value",0},{"unit","px"}}}};
            else if(cmd=="file.export")r.params["dpi"]=std::numeric_limits<double>::infinity();
            else available=false;
        } else if(code=="out-of-range") {
            auto version=object{{"identity","v"},{"sha256","too-short"},{"bytes",0}};
            if(cmd=="file.close")r.params["reconcile"]=object{{"destination","/none"},{"observed-version",version},{"acknowledge",true}};
            else r.params["expected-version"]=version;
        } else if(code=="invalid-argument") {
            if(cmd=="file.close")r.params["reconcile"]=object{{"destination","/none"},{"observed-version",object{{"identity","v"},{"sha256",std::string(64,'0')},{"bytes",0}}},{"acknowledge",false}};
            else available=false;
        } else available=false;
        if(!available){legacy_pending(row,"Current parameter schema has no matching constraint for this exact legacy code; native/schema source review required before descriptor removal.",true);continue;}
        auto before=f.state();auto result=dispatch(r,f.context);
        legacy_observe(row,f,before,result,r);ASSERT_EQ(f.calls,0u);++count;
    }EXPECT_GT(count,0u);
}
void legacy_admission() {
    unsigned count=0;
    for(auto const &v:legacy_rows()) {
        auto row=v.as_object();if(row.at("layer")!="identity/admission")continue;
        SCOPED_TRACE(serialize(row));auto cmd=std::string(row.at("command").as_string()),code=std::string(row.at("code").as_string());
        LegacyFixture f;auto r=f.request(cmd);
        if(code=="session-required")f.context.file_handler={};
        else if(code=="reconciliation-required") {f.files.writes_blocked=true;f.files.reconciliation={{"destination",(f.dir/"uncertain.svg").string()},{"acknowledged",false}};}
        else if(code=="write-grant-denied") {
            auto path=(f.dir/"ungranted.svg").string();f.grants.write_roots.clear();f.files.writes_blocked=true;
            f.files.reconciliation={{"destination",path},{"acknowledged",false}};
            r.params["reconcile"]=object{{"destination",path},{"observed-version",object{{"identity","v"},{"sha256",std::string(64,'0')},{"bytes",0}}},{"acknowledge",true}};
        } else if(code=="document-busy") {
            f.write("source.svg","<svg xmlns=\"http://www.w3.org/2000/svg\"><rect width=\"10\" height=\"10\"/></svg>");
            auto before=f.state();Record result;
            { EditTransaction outer(f.context.document,f.context.selection);ASSERT_TRUE(outer.active());
              result=dispatch(r,f.context);outer.rollback(); }
            legacy_observe(row,f,before,result,r);++count;continue;
        } else {legacy_pending(row,"Legacy transaction-unavailable is not emitted by native import; actual inactive transaction emits document-busy. The paired busy obligation is exercised separately.",true);continue;}
        auto before=f.state();auto result=dispatch(r,f.context);legacy_observe(row,f,before,result,r);++count;
    }EXPECT_GT(count,0u);
}
void legacy_files() {
    unsigned count=0;
    for(auto const &v:legacy_rows()) {
        auto row=v.as_object();if(row.at("layer")!="command-service")continue;
        auto cmd=std::string(row.at("command").as_string()),code=std::string(row.at("code").as_string());
        SCOPED_TRACE(serialize(row));LegacyFixture f;auto r=f.request(cmd);
        if(code=="internal-error") {
            auto before=f.state();ScopedCliFaultPlanForTesting scope({{{"file.before-service",CliFaultKind::ServiceException}}, {}});
            auto result=dispatch(r,f.context);ASSERT_EQ(scope.plan().faults[0].visits,1u);
            legacy_observe(row,f,before,result,r,{{"fault_point","file.before-service"},{"fault_visits",scope.plan().faults[0].visits}});++count;continue;
        }
        if(code=="publication-failed" || code=="publication-unsupported" || code=="publication-uncertain")continue;
        bool intake=cmd=="file.open" || cmd=="file.import";
#ifndef _WIN32
        if(code=="resource-unavailable" && cmd=="file.save") {
            auto denied=f.dir/"denied";std::filesystem::create_directory(denied);
            auto image=f.context.document->getObjectById("image1");ASSERT_NE(image,nullptr);
            std::string href=image->getRepr()->attribute("xlink:href");auto comma=href.find(',');ASSERT_NE(comma,std::string::npos);
            gsize n=0;auto decoded=g_base64_decode(href.c_str()+comma+1,&n);ASSERT_NE(decoded,nullptr);
            auto path=f.write("denied/source.png",std::string(reinterpret_cast<char const *>(decoded),n));g_free(decoded);
            image->getRepr()->setAttribute("xlink:href",path.c_str());
            DocumentUndo::done(f.context.document,Util::Internal::ContextString("linked fixture"),"");
            r.if_revision=document_stamp(f.context.document).revision;
            r.params["path"]=(f.dir/"output.svg").string();
            struct Restore {std::filesystem::path p;~Restore(){std::error_code ec;std::filesystem::permissions(p,std::filesystem::perms::owner_all,ec);}} restore{denied};
            std::filesystem::permissions(denied,std::filesystem::perms::none);
            auto before=f.state();auto result=dispatch(r,f.context);legacy_observe(row,f,before,result,r);++count;continue;
        }
        if((code=="resource-unavailable" && intake) || (code=="publication-unavailable" && cmd=="file.export")) {
            auto denied=f.dir/"denied";std::filesystem::create_directory(denied);
            f.write("denied/source.svg","<svg xmlns=\"http://www.w3.org/2000/svg\"/>");
            r.params["path"]=(denied/(intake ? "source.svg" : "output.svg")).string();
            struct Restore {std::filesystem::path p;~Restore(){std::error_code ec;std::filesystem::permissions(p,std::filesystem::perms::owner_all,ec);}} restore{denied};
            std::filesystem::permissions(denied,std::filesystem::perms::none);
            auto before=f.state();auto result=dispatch(r,f.context);
            legacy_observe(row,f,before,result,r,{{"native_permission_denial",true}});++count;continue;
        }
#endif
        if(code=="unsafe-xml" && !intake) {
            static_cast<XML::Node *>(f.context.document->getReprDoc())->setAttribute("doctype","<!DOCTYPE svg [<!ENTITY int_probe 'probe'>]>\n");
            DocumentUndo::done(f.context.document,Util::Internal::ContextString("doctype fixture"),"");
            r.if_revision=document_stamp(f.context.document).revision;r.params["path"]=(f.dir/"output.svg").string();
            auto before=f.state();ASSERT_FALSE(before.at("doctype").is_null());
            ASSERT_NE(sp_repr_save_buf(f.context.document->getReprDoc()).raw().find("<!DOCTYPE"),std::string::npos);
            auto copy=f.context.document->copy();
            ASSERT_EQ(sp_repr_save_buf(copy->getReprDoc()).raw().find("<!DOCTYPE"),std::string::npos);
            r.dry_run=true;auto result=dispatch(r,f.context);ASSERT_EQ(result.status,Status::Ok)<<result.reason;
            ASSERT_EQ(before,f.state());ASSERT_FALSE(result.publication_persisted);
            std::cout<<"INT-D12-COUNTEREXAMPLE "<<serialize(object{{"id",row.at("id")},{"claimed_code",code},
                {"actual_code",result.reason},{"native_snapshot_strips_doctype",true},{"before",before},{"after",f.state()},
                {"settled",true},{"result",typed_result(result,r.id,1)}})<<std::endl;
            legacy_pending(row,"Native SPDocument::copy omits the document doctype attribute before snapshot serialization and preflight; real live DTD and safe copied XML are both asserted.",true);continue;
        }
        if(code=="input-too-large" && intake) {
            std::ifstream fixture(source_root()/"testfiles/cli_tests/vacards-agent/fixtures/m2/sheet.cdr",std::ios::binary);
            ASSERT_TRUE(fixture.good());std::string bytes(std::istreambuf_iterator<char>(fixture),{});
            r.params["path"]=f.write("bounded.cdr",bytes);r.params["format"]="cdr";
            auto before=f.state();ScopedCliFaultPlanForTesting scope({{},{{"intake.conversion.bytes",32}}});
            auto result=dispatch(r,f.context);legacy_observe(row,f,before,result,r,{{"counted_limit","intake.conversion.bytes"},{"test_ceiling",32},{"production_ceiling",512ull<<20}});++count;continue;
        }
        if(code=="stale-dependency") {
            if(!intake) {
                auto image=f.context.document->getObjectById("image1");ASSERT_NE(image,nullptr);
                std::string href=image->getRepr()->attribute("xlink:href");auto comma=href.find(',');ASSERT_NE(comma,std::string::npos);
                gsize n=0;auto decoded=g_base64_decode(href.c_str()+comma+1,&n);ASSERT_NE(decoded,nullptr);
                auto path=f.write("linked.png",std::string(reinterpret_cast<char const *>(decoded),n));g_free(decoded);
                image->getRepr()->setAttribute("xlink:href",path.c_str());
                DocumentUndo::done(f.context.document,Util::Internal::ContextString("linked fixture"),"");
                r.if_revision=document_stamp(f.context.document).revision;r.params["path"]=(f.dir/"output.svg").string();
            }
            auto before=f.state();ScopedCliFaultPlanForTesting scope({{{"resources.read.identity",CliFaultKind::StaleDependency}}, {}});
            auto result=dispatch(r,f.context);ASSERT_EQ(scope.plan().faults[0].visits,1u);
            legacy_observe(row,f,before,result,r,{{"fault_point","resources.read.identity"},{"fault_visits",scope.plan().faults[0].visits},{"byte_read_started",false}});++count;continue;
        }
        if(code=="invalid-xml" && !intake) {
            r.params["path"]=(f.dir/"output.svg").string();
            auto before=f.state();ScopedCliFaultPlanForTesting scope({{{"intake.xml.preflight",CliFaultKind::Analysis}}, {}});
            auto result=dispatch(r,f.context);ASSERT_EQ(scope.plan().faults[0].visits,1u);
            legacy_observe(row,f,before,result,r,{{"fault_point","intake.xml.preflight"},{"fault_visits",scope.plan().faults[0].visits}});++count;continue;
        }
        if(code=="invalid-svg" && intake) {
            auto before=f.state();ScopedCliFaultPlanForTesting scope({{{"intake.native-document",CliFaultKind::Analysis}}, {}});
            auto result=dispatch(r,f.context);ASSERT_EQ(scope.plan().faults[0].visits,1u);
            legacy_observe(row,f,before,result,r,{{"fault_point","intake.native-document"},{"fault_visits",scope.plan().faults[0].visits}});++count;continue;
        }
        if(code=="intake-failed" && intake) {
            if(cmd=="file.save" || cmd=="file.export")r.params["path"]=(f.dir/"output.svg").string();
            auto point=intake ? "intake.source.after-read" : "intake.snapshot.after-copy";
            auto before=f.state();ScopedCliFaultPlanForTesting scope({{{point,CliFaultKind::ServiceException}}, {}});
            auto result=dispatch(r,f.context);ASSERT_EQ(scope.plan().faults[0].visits,1u);
            legacy_observe(row,f,before,result,r,{{"fault_point",point},{"fault_visits",scope.plan().faults[0].visits}});++count;continue;
        }
        if(code=="export-failed") {
            r.params["path"]=(f.dir/"output.png").string();r.params["profile"]=object{{"id","srgb"}};
            auto before=f.state();ScopedCliFaultPlanForTesting scope({{{"file.png.render",CliFaultKind::Renderer}}, {}});
            auto result=dispatch(r,f.context);ASSERT_EQ(scope.plan().faults[0].visits,1u);
            legacy_observe(row,f,before,result,r,{{"fault_point","file.png.render"},{"destination_exists",std::filesystem::exists(f.dir/"output.png")}});
            EXPECT_FALSE(std::filesystem::exists(f.dir/"output.png"));++count;continue;
        }


        if(!intake && (code=="remote-resource" || code=="resource-denied" || code=="duplicate-id" || code=="unsafe-reference")) {
            r.params["path"]=(f.dir/"output.svg").string();if(cmd=="file.export")r.params["format"]="svg";
            if(code=="remote-resource" || code=="resource-denied") {
                auto image=f.context.document->getObjectById("image1");ASSERT_NE(image,nullptr);
                image->getRepr()->setAttribute("xlink:href",code=="remote-resource" ? "https://invalid.example/image.png" : "/ungranted-int-waveD/image.png");
            } else if(code=="duplicate-id") {
                f.context.document->getObjectById("shape1")->getRepr()->setAttribute("id","duplicate");
                f.context.document->getObjectById("shape2")->getRepr()->setAttribute("id","duplicate");
            } else if(code=="unsafe-reference") {
                f.context.document->getObjectById("broken-clone")->getRepr()->setAttribute("xlink:href","#broken-clone");
            }
            DocumentUndo::done(f.context.document,Util::Internal::ContextString("legacy malformed fixture"),"");
            r.if_revision=document_stamp(f.context.document).revision;
            auto before=f.state();
            if(code=="duplicate-id") {
                std::set<std::string> ids;
                std::function<void(value const &)> unique=[&](value const &n) {
                    auto const &a=n.as_array();if(auto id=a[1].as_object().if_contains("id"))EXPECT_TRUE(ids.insert(std::string(id->as_string())).second);
                    for(auto const &child:a[3].as_array())unique(child);
                };unique(before.at("xml"));
                r.dry_run=true;auto result=dispatch(r,f.context);ASSERT_EQ(result.status,Status::Ok)<<result.reason;
                ASSERT_EQ(before,f.state());ASSERT_FALSE(result.publication_persisted);
                std::cout<<"INT-D12-COUNTEREXAMPLE "<<serialize(object{{"id",row.at("id")},{"claimed_code",code},
                    {"actual_code",result.reason},{"native_ids_unique",true},{"before",before},{"after",f.state()},
                    {"settled",true},{"result",typed_result(result,r.id,1)}})<<std::endl;
                legacy_pending(row,"Native SPObject ID assignment and SPDocument::copy enforce unique IDs before snapshot preflight; duplicate source IDs remain reachable on open/import only. Native duplicate-ID attempt plus successful nonmutating snapshot recorded.",true);continue;
            }
            auto result=dispatch(r,f.context);legacy_observe(row,f,before,result,r);++count;continue;
        }
        if(intake && (code=="invalid-path" || code=="invalid-xml" || code=="duplicate-id" || code=="unsafe-reference" || code=="format-unsupported" || code=="invalid-file")) {
            std::string bytes="<svg xmlns=\"http://www.w3.org/2000/svg\" xmlns:xlink=\"http://www.w3.org/1999/xlink\"><rect width=\"5\" height=\"5\"/></svg>";
            if(code=="invalid-svg")bytes="<not-svg/>";
            if(code=="invalid-file") {bytes="%PDF-1.7\ninvalid";r.params["format"]="pdf";}
            if(code=="invalid-xml")bytes="<svg xmlns=\"http://www.w3.org/2000/svg\"><broken></svg>";
            if(code=="duplicate-id")bytes="<svg xmlns=\"http://www.w3.org/2000/svg\"><rect id=\"duplicate\"/><rect id=\"duplicate\"/></svg>";
            if(code=="unsafe-reference")bytes="<svg xmlns=\"http://www.w3.org/2000/svg\" xmlns:xlink=\"http://www.w3.org/1999/xlink\"><use id=\"a\" xlink:href=\"#b\"/><use id=\"b\" xlink:href=\"#a\"/></svg>";
            r.params["path"]=f.write("source.svg",bytes);
            if(code=="invalid-path")r.params["path"]="relative.svg";
            if(code=="format-unsupported")r.params["format"]="png";
            auto before=f.state();auto result=dispatch(r,f.context);legacy_observe(row,f,before,result,r);++count;continue;
        }
        legacy_pending(row,"Exact native fixture or owner fault point still required; no receipt inferred from matching code or vocabulary.");
    }EXPECT_GT(count,0u);
}
}
}
TEST(INTD12, LegacySchemaExactBranches) {
    auto inputs = std::vector<std::filesystem::path>{D12::root()/"work/cli-b31/evidence/P9/deferred-errors.json"};
    if (!D12::workspace_available(inputs)) GTEST_SKIP() << "D12 evidence workspace absent: " << D12::first_missing_workspace_input(inputs).string() << " (the evidence workspace is not part of this repository)";
    D12::legacy_schema();
}
TEST(INTD12, LegacyAdmissionExactBranches) {
    auto inputs = std::vector<std::filesystem::path>{D12::root()/"work/cli-b31/evidence/P9/deferred-errors.json"};
    if (!D12::workspace_available(inputs)) GTEST_SKIP() << "D12 evidence workspace absent: " << D12::first_missing_workspace_input(inputs).string() << " (the evidence workspace is not part of this repository)";
    D12::legacy_admission();
}
TEST(INTD12, LegacyFileExactBranches) {
    auto inputs = std::vector<std::filesystem::path>{D12::root()/"work/cli-b31/evidence/P9/deferred-errors.json"};
    if (!D12::workspace_available(inputs)) GTEST_SKIP() << "D12 evidence workspace absent: " << D12::first_missing_workspace_input(inputs).string() << " (the evidence workspace is not part of this repository)";
    D12::legacy_files();
}

namespace {
namespace D12 {
namespace DT=IO::DocumentTransaction;
class LegacyPublicationCalls final : public DT::SystemCalls {
public:
    std::unique_ptr<DT::SystemCalls> native=DT::make_platform_system_calls();
    unsigned publications=0;
    bool create_exclusive_file(std::string &p,FILE *&f,bool &exists,std::string &e) override {
        if(cli_fault("file.publication.create",CliFaultKind::Publication)){exists=false;e="INT scoped create failure";return false;}
        return native->create_exclusive_file(p,f,exists,e);
    }
    bool flush_file(FILE *f,std::string &e) override{return native->flush_file(f,e);}
    bool sync_file(FILE *f,bool &u,std::string &e) override{return native->sync_file(f,u,e);}
    bool close_file(FILE *f,std::string &e) override{return native->close_file(f,e);}
    DT::PublicationStatus publish_new_file(std::string const &s,std::string const &p,std::string &e) override {
        ++publications;
        if(cli_fault("file.publication.unsupported",CliFaultKind::Publication)){e="INT unsupported primitive";return DT::PublicationStatus::Unsupported;}
        auto result=native->publish_new_file(s,p,e);
        if(result==DT::PublicationStatus::Published && cli_fault("file.publication.confirmation",CliFaultKind::Publication)){e="INT confirmation lost after actual publication";return DT::PublicationStatus::Uncertain;}
        return result;
    }
    bool remove_file(std::string const &p) noexcept override{return native->remove_file(p);}
    bool sync_parent_directory(std::string const &p,bool &u,std::string &e) override{return native->sync_parent_directory(p,u,e);}
};
void legacy_publication() {
    unsigned count=0;
    for(auto const &v:legacy_rows()) {
        auto row=v.as_object();auto code=std::string(row.at("code").as_string());
        if(code!="publication-failed" && code!="publication-unsupported" && code!="publication-uncertain")continue;
        LegacyFixture f;auto r=f.request(std::string(row.at("command").as_string()));
        auto path=f.dir/"published.svg";r.params["path"]=path.string();r.params["format"]="svg";
        LegacyPublicationCalls calls;auto point=code=="publication-failed" ? "file.publication.create" : code=="publication-unsupported" ? "file.publication.unsupported" : "file.publication.confirmation";
        ScopedCliFaultPlanForTesting scope({{{point,CliFaultKind::Publication}}, {}});
        FileServiceTestHooks hooks;hooks.calls=&calls;
        f.context.file_handler=[&](Request const &r){++f.calls;return execute_file_for_testing(r,f.context,f.files,f.grants,hooks);};
        auto before=f.state();auto result=dispatch(r,f.context);auto after=f.state();auto typed=typed_result(result,r.id,1);
        ASSERT_EQ(scope.plan().faults[0].visits,1u);
        ASSERT_EQ(result.reason,code)<<result.message;ASSERT_EQ(before,after);
        ASSERT_FALSE(DocumentUndo::interactionActive(f.context.document));ASSERT_FALSE(result.one_undo_step);
        bool uncertain=code=="publication-uncertain";
        ASSERT_EQ(std::filesystem::exists(path),uncertain);ASSERT_EQ(f.files.writes_blocked,uncertain);
        ASSERT_EQ(calls.publications,code=="publication-failed" ? 0u : 1u);
        if(uncertain) {
            ASSERT_EQ(result.status,Status::Uncertain);ASSERT_EQ(f.files.reconciliation.at("destination"),value(path.string()));
            std::ifstream file(path);std::string contents(std::istreambuf_iterator<char>(file),{});
            ASSERT_NE(contents.find("<svg"),std::string::npos);
        }
        ASSERT_FALSE(typed.at("error").as_object().at("retryable").as_bool());
        std::cout<<"P9-OBSERVATION "<<serialize(object{{"obligation",row.at("id")},{"branch",row.at("branch")},
            {"command",row.at("command")},{"code",code},{"retryable",false},{"mutation_state",uncertain ? "uncertain" : "none"},
            {"before",before},{"after",after},{"settled",true},
            {"actual",object{{"result",typed},{"destination",path.string()},{"destination_exists",std::filesystem::exists(path)},
                {"writes_blocked",f.files.writes_blocked},{"reconciliation",f.files.reconciliation},{"native_publications",calls.publications},{"fault_point",point},{"fault_visits",scope.plan().faults[0].visits}}}})<<std::endl;++count;
    }EXPECT_EQ(count,3u);
}
}
}
TEST(INTD12, LegacyPublicationExactBranches) {
    auto inputs = std::vector<std::filesystem::path>{D12::root()/"work/cli-b31/evidence/P9/deferred-errors.json"};
    if (!D12::workspace_available(inputs)) GTEST_SKIP() << "D12 evidence workspace absent: " << D12::first_missing_workspace_input(inputs).string() << " (the evidence workspace is not part of this repository)";
    D12::legacy_publication();
}

TEST(INTFaultSeam, FileCountedLimitAndUnarmedPublicationUseNativePath) {
    D12::LegacyFixture f;auto r=f.request("file.save");auto path=f.dir/"limit.svg";r.params["path"]=path.string();
    auto before=f.state();
    {
        ScopedCliFaultPlanForTesting scope({{},{{"file.payload.bytes",16}}});
        auto result=dispatch(r,f.context);ASSERT_EQ(result.reason,"input-too-large");
        EXPECT_EQ(f.state(),before);EXPECT_FALSE(std::filesystem::exists(path));
    }
    auto result=dispatch(r,f.context);ASSERT_TRUE(result.publication_persisted)<<result.reason<<" "<<result.message;
    EXPECT_TRUE(std::filesystem::exists(path));
    EXPECT_FALSE(f.context.document->isModifiedSinceSave());
    EXPECT_EQ(std::string(f.context.document->getDocumentFilename()),path.string());
    auto after=f.state();for(auto key:{"selection","document_revision","session_revision","undo","redo","tokens"})EXPECT_EQ(after.at(key),before.at(key))<<key;
}

TEST(INTD12, LegacyReachabilityCounterexamples) {
    auto inputs = std::vector<std::filesystem::path>{D12::root()/"work/cli-b31/evidence/P9/deferred-errors.json"};
    if (!D12::workspace_available(inputs)) GTEST_SKIP() << "D12 evidence workspace absent: " << D12::first_missing_workspace_input(inputs).string() << " (the evidence workspace is not part of this repository)";
    using namespace boost::json;
    unsigned count=0;
    for(auto const &v:D12::legacy_rows()) {
        auto row=v.as_object();auto cmd=std::string(row.at("command").as_string()),code=std::string(row.at("code").as_string());
        D12::LegacyFixture f;auto r=f.request(cmd);std::string expected;
        if(code=="missing-file" && (cmd=="file.open" || cmd=="file.import")) {
            r.params["path"]=(f.dir/"missing.svg").string();expected="read-grant-denied";
        } else if(code=="nonfinite-number" && (cmd=="file.open" || cmd=="file.close" || cmd=="file.save")) {
            if(cmd=="file.open")r.params["pages"]=array{std::numeric_limits<double>::infinity()};
            else {
                object version{{"identity","v"},{"sha256",std::string(64,'0')},{"bytes",std::numeric_limits<double>::infinity()}};
                if(cmd=="file.close")r.params["reconcile"]=object{{"destination","/none"},{"observed-version",version},{"acknowledge",true}};
                else r.params["expected-version"]=version;
            }
            expected="wrong-type";
        } else if(code=="unknown-command") {
            ASSERT_NE(find_command(cmd),nullptr);
            r.params["not-a-real-parameter"]=true;expected="unknown-key";
        } else continue;
        auto before=f.state();auto result=dispatch(r,f.context);ASSERT_EQ(result.reason,expected)<<cmd;
        ASSERT_NE(result.reason,code);ASSERT_EQ(before,f.state());ASSERT_FALSE(DocumentUndo::interactionActive(f.context.document));
        std::cout<<"INT-D12-COUNTEREXAMPLE "<<serialize(object{{"id",row.at("id")},{"command",cmd},
            {"claimed_code",code},{"actual_code",result.reason},{"before",before},{"after",f.state()},
            {"settled",true},{"result",typed_result(result,r.id,1)}})<<std::endl;++count;
    }
    EXPECT_EQ(count,11u);
}
