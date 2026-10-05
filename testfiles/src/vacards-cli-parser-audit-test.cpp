// SPDX-License-Identifier: GPL-2.0-or-later
#include <gtest/gtest.h>
#include "actions/vacards-cli-production.h"
#include "document.h"
#include "inkscape.h"
#include <limits>
using namespace Inkscape::VACardsCli;
using namespace boost::json;
namespace {
PackageCommand command(std::string_view id) {
    for (auto const &c : production_commands()) if (c.id==id) return c;
    throw std::logic_error("Missing production descriptor");
}
}
TEST(M3ParserAudit, RequiredOutcomesNotImplemented) {
    if (!m3_accepted) GTEST_SKIP() << "M3 ships experimental (m3_accepted=false); this acceptance obligation is pending.";
    FAIL() << "Actual parser-open interception with positive bypass controls remains required on Mac and Windows; admission tests below do not satisfy it.";
}
TEST(M3Admission, RequiredExactUint64Seed) {
    auto c=command("nest.solve");
    auto p=c.example; p["seed"]=std::numeric_limits<std::uint64_t>::max();
    object normalized;
    ASSERT_FALSE(normalize_schema_params(p,c.spec.input->schema,normalized));
    EXPECT_TRUE(normalized.at("seed").is_uint64());
    EXPECT_EQ(normalized.at("seed").as_uint64(),std::numeric_limits<std::uint64_t>::max());
    for (auto raw : {"18446744073709551616", "1e3", "1.0", "-1"}) {
        p["seed"]=parse(raw); EXPECT_TRUE(normalize_schema_params(p,c.spec.input->schema,normalized)) << raw;
    }
}
TEST(M3Admission, RequiredConvertedCssPxLimit) {
    auto c=command("geometry.offset"); object normalized;
    auto p=c.example; p["distance"]=object{{"value",2},{"unit","in"}};
    ASSERT_FALSE(normalize_schema_params(p,c.spec.input->schema,normalized));
    EXPECT_EQ(normalized.at("distance").as_object().at("unit"),value("px"));
    EXPECT_DOUBLE_EQ(normalized.at("distance").as_object().at("value").to_number<double>(),192);
    p["distance"]=object{{"value",20000},{"unit","in"}};
    EXPECT_TRUE(normalize_schema_params(p,c.spec.input->schema,normalized));
    // A nested recipe length uses the same recursive descriptor, not just top-level ParamSpec.
    auto schema=object{{"type","object"},{"properties",object{{"recipe",object{{"type","object"},
        {"properties",object{{"distance",c.spec.input->schema.at("properties").as_object().at("distance")}}}}}}}};
    EXPECT_TRUE(normalize_schema_params(object{{"recipe",p}},schema,normalized));
}
TEST(M3Admission, RequiredDefaultsAfterRoute) {
    object normalized;
    auto copy=command("bitmap.copy"); auto p=copy.example;
    p.erase("dpi"); p["size"]=object{{"width",120},{"height",80}};
    ASSERT_FALSE(normalize_schema_params(p,copy.spec.input->schema,normalized));
    EXPECT_FALSE(normalized.contains("dpi"));
    p["dpi"]=96; EXPECT_TRUE(normalize_schema_params(p,copy.spec.input->schema,normalized));
    auto apply=command("nest.apply");
    ASSERT_FALSE(normalize_schema_params(object{{"solution-token","token"}},apply.spec.input->schema,normalized));
    EXPECT_FALSE(normalized.contains("analyze")); EXPECT_FALSE(normalized.contains("solve"));
    EXPECT_TRUE(normalize_schema_params({},apply.spec.input->schema,normalized));
    EXPECT_TRUE(normalize_schema_params(object{{"solution-token","token"},{"analyze",object{}}},apply.spec.input->schema,normalized));
}
TEST(M3Admission, RequiredGuardDomain) {
    if (!Inkscape::Application::exists()) Inkscape::Application::create(false,Inkscape::Application::RuntimePolicy::PreviewHelper);
    std::string svg=R"(<svg xmlns="http://www.w3.org/2000/svg"/>)";
    auto doc=SPDocument::createNewDocFromMem(std::span<char const>(svg.data(),svg.size())); ASSERT_TRUE(doc);
    auto stamp=document_stamp(doc.get());
    DispatchContext context; context.document=doc.get(); context.session_revision=23;
    unsigned called=0; context.production_handler=[&](Request const &){ ++called; return Record{}; };
    Request r; r.id="test"; r.command="selection.set"; r.document=stamp.id; r.if_revision=23; r.params={{"ids",array{}}};
    auto result=dispatch_production(r,context); EXPECT_EQ(result.status,Status::Ok); EXPECT_EQ(called,1u);
    r.if_revision=24; EXPECT_EQ(dispatch_production(r,context).reason,"stale-revision"); EXPECT_EQ(called,1u);
    r.command="history.query"; r.params={}; r.if_revision.reset();
    EXPECT_EQ(dispatch_production(r,context).status,Status::Ok); EXPECT_EQ(called,2u);
    r.if_revision=stamp.revision+1; EXPECT_EQ(dispatch_production(r,context).reason,"stale-revision");
    r.if_revision=stamp.revision; context.cancelled=[] { return true; };
    EXPECT_EQ(dispatch_production(r,context).status,Status::Cancelled); EXPECT_EQ(called,2u);
    context.cancelled={}; context.command_admission=[](Request const &)->std::optional<Record>{
        Record r; r.status=Status::Rejected; r.reason="document-read-only"; return r;
    };
    EXPECT_EQ(dispatch_production(r,context).reason,"document-read-only"); EXPECT_EQ(called,2u);
}
TEST(M3Admission, DuplicateIdsExclusiveMinimumAndPropertyNames) {
    auto c=command("selection.set");
    EXPECT_EQ(validate_schema(object{{"ids",array{"a","a"}}},c.spec.input->schema)->code,"invalid-argument");
    EXPECT_FALSE(validate_schema(object{{"ids",array{}}},c.spec.input->schema));
    object positive{{"type","number"},{"exclusiveMinimum",0}};
    EXPECT_TRUE(validate_schema(0,positive)); EXPECT_FALSE(validate_schema(0.01,positive));
    object names{{"type","object"},{"propertyNames",object{{"minLength",1}}}};
    EXPECT_TRUE(validate_schema(object{{"",1}},names));
}

TEST(M3Admission, EveryExampleNormalizesWithoutInventingRoutes) {
    for (auto const &c : production_commands()) {
        object normalized;
        auto error=normalize_schema_params(c.example,c.spec.input->schema,normalized);
        ASSERT_FALSE(error) << c.id << ": " << (error ? error->message : "");
        EXPECT_FALSE(validate_schema(normalized,c.spec.input->schema)) << c.id;
        if (c.id=="bitmap.explode.explode") {
            EXPECT_FALSE(normalized.contains("contour")); EXPECT_FALSE(normalized.contains("contour-style"));
            EXPECT_FALSE(normalized.at("recipe").as_object().empty());
        }
    }
}

TEST(M3Admission, RawOverlayParserPreservesExactSeedAndRejectsAmbiguousKeys) {
    auto c=command("nest.solve");
    auto p=c.example; p["seed"]=std::numeric_limits<std::uint64_t>::max();
    object envelope{{"schema","va-studio.cli-request/1"},{"id","raw"},{"command","nest.solve"},
        {"document","doc"},{"params",p}};
    auto parsed=parse_production_request(serialize(envelope)); ASSERT_TRUE(parsed.request);
    EXPECT_EQ(parsed.request->params.at("seed").as_uint64(),std::numeric_limits<std::uint64_t>::max());
    EXPECT_FALSE(parse_request(serialize(envelope)).request); // shipped registry still unavailable
    p["seed"]=1.0; envelope["params"]=p;
    auto rejected=parse_production_request(serialize(envelope)); ASSERT_TRUE(rejected.error);
    EXPECT_EQ(rejected.error->code,"invalid-argument"); EXPECT_EQ(rejected.error_id,"raw");
    auto duplicate=parse_production_request(R"({"schema":"va-studio.cli-request/1","id":"a","id":"b","command":"history.query","params":{},"document":"doc"})");
    ASSERT_TRUE(duplicate.error); EXPECT_EQ(duplicate.error->code,"repeated-key"); EXPECT_FALSE(duplicate.request);
}

TEST(M3Admission, NullableResultsPatternsAndExactUniqueIntegers) {
    object nullable{{"type",array{"string","null"}}};
    EXPECT_FALSE(validate_schema(nullptr,nullable)); EXPECT_FALSE(validate_schema("label",nullable));
    EXPECT_TRUE(validate_schema(1,nullable));
    object hex{{"type","string"},{"pattern","^#[0-9a-fA-F]{6}$"}};
    EXPECT_FALSE(validate_schema("#fA0123",hex)); EXPECT_TRUE(validate_schema("#xx0123",hex));
    auto max=std::numeric_limits<std::uint64_t>::max();
    EXPECT_TRUE(validate_schema(max,object{{"type","integer"},{"maximum",max-1}}));
    object unique{{"type","array"},{"uniqueItems",true}};
    EXPECT_FALSE(validate_schema(array{max,max-1},unique)); EXPECT_TRUE(validate_schema(array{max,max},unique));
}

TEST(M3Admission, OverlayParserPreservesLegacyControlAdmission) {
    auto request=R"({"schema":"va-studio.cli-request/1","id":"status","command":"session.status","params":{}})";
    auto old=parse_request(request); auto overlay=parse_production_request(request);
    ASSERT_TRUE(old.request); ASSERT_TRUE(overlay.request);
    EXPECT_EQ(overlay.request->command,old.request->command);
    EXPECT_EQ(overlay.request->params,old.request->params);
    auto invalid=R"({"schema":"va-studio.cli-request/1","id":"status","command":"session.status","params":{"bogus":true}})";
    auto old_error=parse_request(invalid); auto overlay_error=parse_production_request(invalid);
    ASSERT_TRUE(old_error.error); ASSERT_TRUE(overlay_error.error);
    EXPECT_EQ(old_error.error->code,overlay_error.error->code);
    EXPECT_FALSE(m3_accepted);
}
