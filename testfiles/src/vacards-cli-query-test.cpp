// SPDX-License-Identifier: GPL-2.0-or-later
#include <gtest/gtest.h>
#include <boost/json.hpp>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>
#include <chrono>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>
#include <libxml/parser.h>
#include <2geom/pathvector.h>
#include "undo-stack-observer.h"
#include <glib.h>
#include "actions/vacards-cli-dispatch.h"
#include "io/vacards-cli-intake.h"
#include "io/vacards-cli-intake-testing.h"
#include "document-undo.h"
#include "document.h"
#include "inkscape.h"
#include "object/sp-root.h"
#include "object/sp-item.h"
#include "selection.h"
#include "svg/svg.h"
#include "xml/repr.h"
using namespace Inkscape;
using namespace Inkscape::VACardsCli;
using namespace boost::json;
namespace {
// Independent schema oracle; it does not call the registry validator.
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

std::string const fixture_dir = std::string(INKSCAPE_TESTS_DIR) + "/cli_tests/vacards-agent-session/query/fixtures";
std::string svg(std::string body = "") { return "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"100\" height=\"100\">" + body + "</svg>"; }
std::string xml_hash(SPDocument *doc)
{
    auto bytes = sp_repr_save_buf(doc->getReprDoc());
    auto hash = g_compute_checksum_for_data(G_CHECKSUM_SHA256, reinterpret_cast<guchar const *>(bytes.c_str()), bytes.bytes());
    std::string result(hash); g_free(hash); return result;
}
class VacardsCliQuery : public ::testing::Test {
protected:
    ScopedIntakeMemoryForTesting fixed_headroom{8ull << 30};
    std::string temporary;
    void SetUp() override {
        if (!Application::exists()) Application::create(false);
        auto dir = g_dir_make_tmp("va-cli-query-XXXXXX", nullptr);
        ASSERT_NE(dir, nullptr); temporary = dir; g_free(dir);
    }
    void TearDown() override { std::filesystem::remove_all(temporary); }
    std::string write(std::string const &bytes, std::string name = "input.svg") {
        auto path = temporary + "/" + name;
        std::ofstream f(path, std::ios::binary); f.write(bytes.data(), bytes.size()); f.close(); return path;
    }
    IntakeResult sheet() { return load_inspection_document(fixture_dir + "/inspection.svg", Grants{{fixture_dir}}); }
    Record run(SPDocument *doc, std::string command, object params = {}, Selection *selection = nullptr, bool dry = false) {
        auto hash = xml_hash(doc); auto stamp = document_stamp(doc);
        auto mark = DocumentUndo::undoStackMark(doc);
        struct HistoryProbe : UndoStackObserver {
            unsigned events = 0;
            void notifyUndoEvent(Event *) override { ++events; }
            void notifyRedoEvent(Event *) override { ++events; }
            void notifyUndoCommitEvent(Event *) override { ++events; }
            void notifyUndoExpired(Event *) override { ++events; }
            void notifyClearUndoEvent() override { ++events; }
            void notifyClearRedoEvent() override { ++events; }
        } history;
        doc->addUndoObserver(history);
        bool dirty = doc->isModifiedSinceSave();
        std::set<std::string> selected;
        if (selection) for (auto item : selection->items()) selected.insert(item->getId());
        DispatchContext context{nullptr, doc, selection, "mm"};
        Request request{"query-test", command, stamp.id, {}, params, dry};
        auto record = dispatch(request, context);
        EXPECT_EQ(xml_hash(doc), hash) << command;
        EXPECT_EQ(document_stamp(doc).revision, stamp.revision) << command;
        doc->removeUndoObserver(history);
        EXPECT_EQ(history.events, 0u); EXPECT_EQ(DocumentUndo::undoStackMark(doc), mark);
        EXPECT_EQ(doc->isModifiedSinceSave(), dirty);
        std::set<std::string> after;
        if (selection) for (auto item : selection->items()) after.insert(item->getId());
        EXPECT_EQ(selected, after);
        auto spec = find_command(command);
        EXPECT_NE(spec, nullptr);
        if (spec) {
            auto wire = typed_result(record, request.id);
            EXPECT_TRUE(conforms(wire, result_schema_descriptor(*spec))) << command << " " << serialize(wire);
        }
        return record;
    }
    object query(SPDocument *doc, std::string command, object params = {}) {
        auto r = run(doc, command, params);
        EXPECT_EQ(r.status, Status::Ok) << r.reason << ": " << r.message;
        return r.data;
    }
    object row(object const &data, std::string_view key, std::string_view expected) {
        for (auto const &r : data.at("items").as_array()) {
            auto const &o = r.as_object();
            if (o.at(key).as_string() == expected) return o;
        }
        ADD_FAILURE() << "Missing row " << expected; return {};
    }
    double px(object const &o, std::string_view key) { return o.at(key).as_object().at("px").to_number<double>(); }
    void rect(object const &o, double x, double y, double width, double height, std::string_view kind) {
        EXPECT_EQ(o.at("kind").as_string(), kind);
        EXPECT_NEAR(px(o, "x"), x, 1e-8); EXPECT_NEAR(px(o, "y"), y, 1e-8);
        EXPECT_NEAR(px(o, "width"), width, 1e-8); EXPECT_NEAR(px(o, "height"), height, 1e-8);
        EXPECT_EQ(o.at("width").as_object().at("preferred").as_object().at("unit"), "mm");
        EXPECT_NEAR(o.at("width").as_object().at("preferred").as_object().at("value").to_number<double>(), width * 25.4 / 96, 1e-8);
    }
};
// Serial test-local loader spy: restoration happens even on ASSERT/exception exits.
// SAX refusals must occur before any DOM/native loader can resolve a resource.
class ScopedXmlResolverSpy {
    xmlExternalEntityLoader previous = xmlGetExternalEntityLoader();
    static inline unsigned calls = 0;
    static xmlParserInputPtr resolve(char const *, char const *, xmlParserCtxtPtr) {
        ++calls;
        return nullptr;
    }
public:
    ScopedXmlResolverSpy() { calls = 0; xmlSetExternalEntityLoader(resolve); }
    ~ScopedXmlResolverSpy() { xmlSetExternalEntityLoader(previous); }
    unsigned count() const { return calls; }
};
TEST_F(VacardsCliQuery, SaxRejectsNamespaceErrorsAndDuplicateAttributes) {
    for (auto const &body : {"<unbound:g/>", "<g unbound:a='x'/>",
         "<g xmlns:a='urn:same' xmlns:b='urn:same' a:x='1' b:x='2'/>", "<g x='1' x='2'/>"}) {
        SCOPED_TRACE(body);
        auto result = load_inspection_document(write(svg(body)), {});
        ASSERT_TRUE(result.error); EXPECT_EQ(result.error->code, "invalid-xml");
        EXPECT_FALSE(result.document); EXPECT_TRUE(result.document_id.empty());
    }
}
TEST_F(VacardsCliQuery, SaxRejectsProcessingInstructionsAtEveryPositionWithoutResolution) {
    ScopedXmlResolverSpy spy;
    auto pi = std::string("<?probe href='file:///must-not-resolve'?>");
    for (auto const &input : {pi + svg(), svg(pi), svg() + pi}) {
        SCOPED_TRACE(input);
        auto result = load_inspection_document(write(input), {});
        ASSERT_TRUE(result.error); EXPECT_EQ(result.error->code, "unsafe-xml");
        EXPECT_FALSE(result.document); EXPECT_TRUE(result.document_id.empty());
        EXPECT_EQ(spy.count(), 0u);
    }
}
TEST_F(VacardsCliQuery, SaxRejectsUndefinedEntitiesInTextAndAttributesWithoutResolution) {
    ScopedXmlResolverSpy spy;
    for (auto body : {"<text>&undefined;</text>", "<g data-x='&undefined;'/>"}) {
        auto result = load_inspection_document(write(svg(body)), {});
        ASSERT_TRUE(result.error); EXPECT_EQ(result.error->code, "invalid-xml");
        EXPECT_FALSE(result.document); EXPECT_TRUE(result.document_id.empty());
        EXPECT_EQ(spy.count(), 0u);
    }
}
TEST_F(VacardsCliQuery, SaxRejectsParameterAndAmplifyingEntitiesWithoutResolution) {
    ScopedXmlResolverSpy spy;
    std::vector<std::string> declarations{
        "<!DOCTYPE svg SYSTEM 'file:///must-not-resolve.dtd'>",
        "<!DOCTYPE svg [<!ENTITY ext SYSTEM 'https://example.invalid/entity'>]>",
        "<!DOCTYPE svg [<!ENTITY % ext SYSTEM 'file:///must-not-resolve.dtd'>%ext;]>",
        "<!DOCTYPE svg [<!ENTITY % local '<!ENTITY value \"expanded\">'>%local;]>",
        "<!DOCTYPE svg [<!ENTITY value 'expanded'>]>"};
    std::string exponential = "<!DOCTYPE svg [<!ENTITY e0 '0123456789'>";
    for (unsigned i = 1; i <= 9; ++i) {
        exponential += "<!ENTITY e" + std::to_string(i) + " '";
        for (unsigned j = 0; j < 10; ++j) exponential += "&e" + std::to_string(i - 1) + ";";
        exponential += "'>";
    }
    declarations.push_back(exponential + "]>");
    std::string large = "<!DOCTYPE svg [";
    for (unsigned i = 0; i < 16; ++i)
        large += "<!ENTITY large" + std::to_string(i) + " '" + std::string(8192, 'x') + "'>";
    declarations.push_back(large + "]>");
    for (unsigned i = 0; i < declarations.size(); ++i) {
        SCOPED_TRACE(i);
        auto body = i == 1 ? "&ext;" : i == 5 ? "&e9;" : i == 6 ? "&large0;&large0;" : "&value;";
        auto result = load_inspection_document(write(declarations[i] + svg(body)), {});
        ASSERT_TRUE(result.error); EXPECT_EQ(result.error->code, "unsafe-xml");
        EXPECT_FALSE(result.document); EXPECT_TRUE(result.document_id.empty());
        EXPECT_EQ(spy.count(), 0u);
    }
}
TEST_F(VacardsCliQuery, SaxQualifiedNameByteBoundariesIncludeUtf8AndColons) {
    auto name = [](unsigned bytes, bool utf8) {
        std::string result;
        if (utf8) for (unsigned i = 0; i < bytes / 2; ++i) result += "é";
        if (!utf8 || bytes % 2) result += std::string(utf8 ? 1 : bytes, 'a');
        return result;
    };
    for (auto kind : {"element", "attribute", "xmlns", "qualified-element", "qualified-attribute"})
        for (bool utf8 : {false, true}) for (unsigned bytes : {255u, 256u}) {
            SCOPED_TRACE(std::string(kind) + ":" + std::to_string(bytes) + (utf8 ? ":utf8" : ":ascii"));
            std::string body;
            if (std::string_view(kind) == "element") body = "<" + name(bytes, utf8) + "/>";
            if (std::string_view(kind) == "attribute") body = "<g " + name(bytes, utf8) + "='x'/>";
            if (std::string_view(kind) == "xmlns") body = "<g xmlns:" + name(bytes - 6, utf8) + "='urn:boundary'/>";
            if (std::string_view(kind) == "qualified-element") body = "<p:" + name(bytes - 2, utf8) + " xmlns:p='urn:boundary'/>";
            if (std::string_view(kind) == "qualified-attribute") body = "<g xmlns:p='urn:boundary' p:" + name(bytes - 2, utf8) + "='x'/>";
            auto result = load_inspection_document(write(svg(body)), {});
            if (bytes == 255) {
                ASSERT_FALSE(result.error) << (result.error ? result.error->message : "");
                EXPECT_TRUE(result.document);
            } else {
                ASSERT_TRUE(result.error); EXPECT_EQ(result.error->code, "engine-limit");
                EXPECT_EQ(result.error->details.at("limit_name"), "qualified-name-bytes");
                EXPECT_EQ(result.error->details.at("limit"), 255); EXPECT_EQ(result.error->details.at("found"), 256);
                EXPECT_FALSE(result.document); EXPECT_TRUE(result.document_id.empty());
            }
        }
}
TEST_F(VacardsCliQuery, SaxAcceptsExactObjectLimitIncludingRoot) {
    for (auto const &body : {std::string(), std::string("<g/><g/>")}) {
        auto objects = body.empty() ? 1u : 3u;
        auto allowed = load_inspection_document(write(svg(body)), {}, {1024, objects, 128});
        ASSERT_FALSE(allowed.error); EXPECT_TRUE(allowed.document);
        auto refused = load_inspection_document(write(svg(body)), {}, {1024, objects - 1, 128});
        ASSERT_TRUE(refused.error); EXPECT_EQ(refused.error->code, "engine-limit");
        EXPECT_EQ(refused.error->details.at("limit_name"), "objects");
        EXPECT_EQ(refused.error->details.at("limit"), objects - 1);
        EXPECT_EQ(refused.error->details.at("found"), objects);
    }
}
TEST_F(VacardsCliQuery, SaxRejectsTruncatedXml) {
    for (auto const &input : {"<svg xmlns='http://www.w3.org/2000/svg'", "<svg xmlns='http://www.w3.org/2000/svg'>",
         "<svg xmlns='http://www.w3.org/2000/svg'><g a='", "<svg xmlns='http://www.w3.org/2000/svg'><!--unfinished",
         "<svg xmlns='http://www.w3.org/2000/svg'><![CDATA[unfinished"}) {
        SCOPED_TRACE(input);
        auto result = load_inspection_document(write(input), {});
        ASSERT_TRUE(result.error); EXPECT_EQ(result.error->code, "invalid-xml");
        EXPECT_FALSE(result.document); EXPECT_TRUE(result.document_id.empty());
    }
}
TEST_F(VacardsCliQuery, SaxRepeatedRefusalsAndMemoryScopesRecover) {
    ScopedXmlResolverSpy spy;
    for (unsigned i = 0; i < 64; ++i) {
        SCOPED_TRACE(i);
        for (auto const &input : {svg("<?refuse?>"), std::string("<!DOCTYPE svg>") + svg(),
             svg("<" + std::string(256, 'a') + "/>"), svg("<g>"), svg("&undefined;")}) {
            auto result = load_inspection_document(write(input), {});
            ASSERT_TRUE(result.error); EXPECT_FALSE(result.document); EXPECT_TRUE(result.document_id.empty());
            EXPECT_NE(result.error->details.if_contains("reason") ? result.error->details.at("reason") : value(), "insufficient-memory");
        }
        auto path = write(svg());
        {
            ScopedIntakeMemoryForTesting no_memory(0);
            auto refused = load_inspection_document(path, {});
            ASSERT_TRUE(refused.error); EXPECT_EQ(refused.error->details.at("reason"), "insufficient-memory");
            {
                ScopedIntakeMemoryForTesting nested_headroom(8ull << 30);
                auto accepted = load_inspection_document(path, {}); ASSERT_FALSE(accepted.error);
            }
            auto restored = load_inspection_document(path, {});
            ASSERT_TRUE(restored.error); EXPECT_EQ(restored.error->details.at("reason"), "insufficient-memory");
        }
        auto recovered = load_inspection_document(path, {});
        ASSERT_FALSE(recovered.error); EXPECT_TRUE(recovered.document);
        EXPECT_EQ(spy.count(), 0u);
    }
}
TEST_F(VacardsCliQuery, ResourceReadsHonorExactAndEmptyBudgets) {
    for (auto bytes : {0u, 1u, 32767u, 32768u, 32769u, 65537u}) {
        SCOPED_TRACE(bytes);
        auto path = write(std::string(bytes, 'x'), "read-budget.bin");
        auto access = inspect_resource(path, temporary, Grants{{temporary}});
        ASSERT_EQ(access.state, "granted");
        auto exact = read_admitted(access, bytes);
        EXPECT_TRUE(exact.error.empty()); EXPECT_EQ(exact.bytes, std::string(bytes, 'x'));
        EXPECT_EQ(exact.sha256.size(), 64u);
        auto headroom = read_admitted(access, bytes + 1);
        EXPECT_TRUE(headroom.error.empty()); EXPECT_EQ(headroom.bytes, exact.bytes);
        if (bytes) {
            auto refused = read_admitted(access, bytes - 1);
            EXPECT_EQ(refused.error, "engine-limit"); EXPECT_EQ(refused.found, bytes);
            EXPECT_TRUE(refused.bytes.empty());
        }
    }
}
#ifndef _WIN32
TEST_F(VacardsCliQuery, ResourceGrowthAfterFstatRefusesWithoutAnEofProbe) {
    for (auto bytes : {0u, 1u, 32768u, 32769u}) {
        SCOPED_TRACE(bytes);
        auto path = write(std::string(bytes, 'x'), "grow-after-stat.bin");
        auto access = inspect_resource(path, temporary, Grants{{temporary}});
        ASSERT_EQ(access.state, "granted");
        unsigned observed = 0;
        auto result = read_admitted_after_stat_for_testing(access, bytes, [&] {
            ++observed;
            std::ofstream grow(path, std::ios::binary | std::ios::app);
            grow << std::string(32769, 'y'); grow.close();
            EXPECT_TRUE(grow.good());
        });
        EXPECT_EQ(observed, 1u); EXPECT_EQ(result.found, bytes);
        EXPECT_EQ(std::filesystem::file_size(path), bytes + 32769u);
        EXPECT_EQ(result.error, "stale-dependency"); EXPECT_TRUE(result.bytes.empty());
        EXPECT_TRUE(result.sha256.empty()); EXPECT_TRUE(result.identity.empty());
    }
}
#endif
TEST_F(VacardsCliQuery, DocumentUnitsMetadataCountsAndIncarnations) {
    auto input = sheet(); ASSERT_FALSE(input.error) << (input.error ? input.error->message : "");
    auto d = query(input.document.get(), "query.document");
    EXPECT_EQ(d.at("document_id").as_string(), input.document_id);
    EXPECT_EQ(d.at("revision").to_number<unsigned>(), 0u); EXPECT_FALSE(d.at("dirty").as_bool());
    EXPECT_EQ(d.at("title"), "Inspection sheet"); EXPECT_NE(d.at("metadata").as_string().find("Fixture metadata"), string::npos);
    EXPECT_DOUBLE_EQ(px(d.at("size").as_object(), "width"), 200);
    EXPECT_DOUBLE_EQ(px(d.at("size").as_object(), "height"), 100);
    EXPECT_EQ(d.at("units").as_object().at("viewbox_to_css_px"), (array{2., 0., 0., 2., -20., -40.}));
    EXPECT_DOUBLE_EQ(px(d.at("units").as_object().at("root_user_unit").as_object(), "x"), 2);
    EXPECT_EQ(d.at("object_counts").as_object().at("rect").to_number<unsigned>(), 4u);
    EXPECT_EQ(d.at("object_counts").as_object().at("image").to_number<unsigned>(), 5u);
    std::uint64_t count = 0; for (auto const &v : d.at("object_counts").as_object()) count += v.value().to_number<std::uint64_t>();
    EXPECT_EQ(d.at("object_count").to_number<std::uint64_t>(), count);
    auto next = sheet(); ASSERT_FALSE(next.error); EXPECT_NE(next.document_id, input.document_id);
}
TEST_F(VacardsCliQuery, PagesHaveRootRectanglesLabelsAndOrder) {
    auto input = sheet(); ASSERT_FALSE(input.error);
    auto data = query(input.document.get(), "query.pages"); ASSERT_EQ(data.at("items").as_array().size(), 3u);
    auto front = row(data, "id", "page1"); EXPECT_EQ(front.at("label"), "Front"); EXPECT_EQ(front.at("order"), 0);
    // Page attributes (10,20,100,50) times document scale (200/100,100/50).
    rect(front.at("rect").as_object(), 20, 40, 200, 100, "geometric");
    auto back = row(data, "id", "page2"); EXPECT_EQ(back.at("label"), "Back"); EXPECT_EQ(back.at("order"), 1);
    rect(back.at("rect").as_object(), 240, 40, 80, 100, "geometric");
    auto extra = row(data, "id", "page3"); rect(extra.at("rect").as_object(), 20, 160, 60, 40, "geometric");
}
TEST_F(VacardsCliQuery, LayersPreserveHierarchyHiddenAndLockedState) {
    auto input = sheet(); ASSERT_FALSE(input.error);
    auto data = query(input.document.get(), "query.layers"); ASSERT_EQ(data.at("items").as_array().size(), 4u);
    auto sheet_row = row(data, "id", "sheet"); EXPECT_EQ(sheet_row.at("label"), "Sheet"); EXPECT_TRUE(sheet_row.at("parent_id").is_null());
    auto child = row(data, "id", "childLayer"); EXPECT_EQ(child.at("parent_id"), "sheet"); EXPECT_EQ(child.at("depth"), 1);
    EXPECT_FALSE(row(data, "id", "hiddenLayer").at("visible").as_bool());
    EXPECT_TRUE(row(data, "id", "lockedLayer").at("locked").as_bool());
}
TEST_F(VacardsCliQuery, ObjectsHaveHierarchyIndependentBoundsAndAllFilters) {
    auto input = sheet(); ASSERT_FALSE(input.error); auto doc = input.document.get();
    auto data = query(doc, "query.objects", {{"ids", {"rect", "clone", "hidden", "locked"}}});
    auto tile = row(data, "id", "rect"); EXPECT_EQ(tile.at("parent_id"), "nested"); EXPECT_EQ(tile.at("layer_id"), "sheet");
    EXPECT_EQ(tile.at("type"), "rect"); EXPECT_EQ(tile.at("label"), "Tile");
    rect(tile.at("bounds").as_object(), 24, 32, 12, 24, "geometric");
    EXPECT_EQ(row(data, "id", "clone").at("type"), "use");
    rect(row(data, "id", "clone").at("bounds").as_object(), 82, 24, 6, 8, "geometric");
    EXPECT_FALSE(row(data, "id", "hidden").at("visible").as_bool());
    EXPECT_TRUE(row(data, "id", "locked").at("locked").as_bool());
    auto filtered = query(doc, "query.objects", {{"type", "rect"}, {"layer", "sheet"}, {"visibility", "visible"}, {"lock", "unlocked"}});
    ASSERT_EQ(filtered.at("items").as_array().size(), 1u); EXPECT_EQ(filtered.at("items").as_array()[0].as_object().at("id"), "rect");
    auto visual = query(doc, "query.objects", {{"ids", {"rect"}}, {"bounds", "visual"}});
    rect(visual.at("items").as_array()[0].as_object().at("bounds").as_object(), 20, 26, 20, 36, "visual");
}
TEST_F(VacardsCliQuery, GeometryTransformAndRootPathAreHandComputed) {
    auto input = sheet(); ASSERT_FALSE(input.error);
    auto data = query(input.document.get(), "query.geometry", {{"ids", {"path", "rect"}}});
    auto path = row(data, "id", "path"); EXPECT_EQ(path.at("transform"), (array{4., 0., 0., 6., 20., 20.}));
    ASSERT_TRUE(path.at("path_data").is_string()); auto vector = sp_svg_read_pathv(path.at("path_data").as_string().c_str());
    ASSERT_EQ(vector.size(), 1u); EXPECT_EQ(vector[0].initialPoint(), Geom::Point(24, 32));
    auto path_bounds = Geom::bounds_exact(vector); ASSERT_TRUE(path_bounds);
    EXPECT_EQ(path_bounds->min(), Geom::Point(24, 32)); EXPECT_EQ(path_bounds->max(), Geom::Point(36, 56));
    rect(path.at("geometric_bounds").as_object(), 24, 32, 12, 24, "geometric");
    rect(path.at("visual_bounds").as_object(), 20, 26, 20, 36, "visual");
    auto shape = row(data, "id", "rect"); EXPECT_EQ(shape.at("shape_kind"), "rect"); EXPECT_TRUE(shape.at("path_data").is_null());
}
TEST_F(VacardsCliQuery, StylesProjectComputedPaintLengthsOpacityAndFonts) {
    auto input = sheet(); ASSERT_FALSE(input.error);
    auto data = query(input.document.get(), "query.styles", {{"ids", {"rect"}}, {"properties", {"fill", "stroke", "stroke-width", "opacity", "paint-order"}}});
    auto p = row(data, "id", "rect").at("properties").as_object(); ASSERT_EQ(p.size(), 5u);
    EXPECT_EQ(p.at("fill"), "#123456"); EXPECT_EQ(p.at("stroke"), "#000000"); EXPECT_DOUBLE_EQ(px(p, "stroke-width"), 4);
    // CSS opacity is not inherited: the parent's .75 is compositing, the child's computed opacity is 1.
    EXPECT_DOUBLE_EQ(p.at("opacity").to_number<double>(), 1);
    EXPECT_EQ(p.at("paint-order"), "normal");
    auto font = query(input.document.get(), "query.styles", {{"ids", {"text"}}, {"properties", {"font-family", "font-style", "font-weight", "font-size"}}});
    auto props = row(font, "id", "text").at("properties").as_object();
    EXPECT_EQ(props.at("font-style"), "italic"); EXPECT_DOUBLE_EQ(px(props, "font-size"), 20);
    EXPECT_NE(props.at("font-family").as_string().find("VA_CLI_MissingFont_9f5e"), string::npos);
}
TEST_F(VacardsCliQuery, ImagesReuseMetadataAndReportEveryGrantState) {
    auto input = sheet(); ASSERT_FALSE(input.error); auto data = query(input.document.get(), "query.images");
    ASSERT_EQ(data.at("items").as_array().size(), 5u);
    for (auto name : {"embedded", "linked"}) {
        auto image = row(data, "id", name); ASSERT_FALSE(image.at("pixel_size").is_null()) << serialize(input.report); EXPECT_EQ(image.at("source_kind"), name);
        EXPECT_EQ(image.at("pixel_size"), (object{{"width", 32}, {"height", 24}}));
        EXPECT_EQ(image.at("mime"), "image/png");
        EXPECT_DOUBLE_EQ(image.at("density").as_object().at("x").to_number<double>(), 96);
        EXPECT_DOUBLE_EQ(image.at("density").as_object().at("y").to_number<double>(), 96);
    }
    EXPECT_EQ(row(data, "id", "linked").at("link_grant"), "granted");
    EXPECT_EQ(row(data, "id", "embedded").at("adjustment_attributes").as_object().at("inkscape:brightness"), "12");
    for (auto name : {"ungranted", "remote", "missing"}) {
        auto image = row(data, "id", name); EXPECT_EQ(image.at("source_kind"), "missing");
        EXPECT_EQ(image.at("link_grant"), name); EXPECT_TRUE(image.at("pixel_size").is_null());
    }
}
#ifdef _WIN32
TEST_F(VacardsCliQuery, WindowsRefusedLinkStatesConformToInspectionContract) {
 auto path=write(svg("<image id='unsafe' href='NUL.png'/><image id='directory' href='.'/>") );
 auto input=load_inspection_document(path,Grants{{temporary}});ASSERT_FALSE(input.error);
 auto data=query(input.document.get(),"query.images");
 EXPECT_EQ(row(data,"id","unsafe").at("link_grant"),"ungranted");
 EXPECT_EQ(row(data,"id","directory").at("link_grant"),"missing");
}
#endif
TEST_F(VacardsCliQuery, FontsUseNativeHeadlessAvailability) {
    auto input = sheet(); ASSERT_FALSE(input.error);
    auto data = query(input.document.get(), "query.fonts"); ASSERT_EQ(data.at("items").as_array().size(), 1u);
    auto font = data.at("items").as_array()[0].as_object(); EXPECT_EQ(font.at("family"), "VA_CLI_MissingFont_9f5e");
    EXPECT_EQ(font.at("style"), "italic"); EXPECT_EQ(font.at("object_id"), "text");
    EXPECT_FALSE(font.at("available").as_bool()); EXPECT_TRUE(font.at("missing").as_bool());
}
TEST_F(VacardsCliQuery, SelectionStartsEmptyAndReadsSessionSelectionWithoutChange) {
    auto input = sheet(); ASSERT_FALSE(input.error); auto doc = input.document.get(); auto sel = doc->getSelection();
    EXPECT_TRUE(run(doc, "query.selection", {}, sel).data.at("items").as_array().empty());
    sel->add(cast<SPItem>(doc->getObjectById("rect")));
    auto r = run(doc, "query.selection", {}, sel); ASSERT_EQ(r.status, Status::Ok);
    EXPECT_EQ(r.data.at("items"), (array{object{{"id", "rect"}}}));
}
TEST_F(VacardsCliQuery, EveryListPaginatesAndCursorBindsCommandFiltersAndDocument) {
    auto input = sheet(); ASSERT_FALSE(input.error); auto doc = input.document.get();
    for (auto command : {"query.pages", "query.layers", "query.objects", "query.geometry", "query.styles", "query.images"}) {
        SCOPED_TRACE(command);
        auto first = query(doc, command, {{"limit", 1}}); ASSERT_TRUE(first.at("next_cursor").is_string());
        auto second = query(doc, command, {{"limit", 1}, {"cursor", first.at("next_cursor")}});
        EXPECT_EQ(second.at("items").as_array().size(), 1u);
        EXPECT_NE(first.at("items").as_array()[0], second.at("items").as_array()[0]);
        auto next = sheet(); ASSERT_FALSE(next.error);
        auto stale = run(next.document.get(), command, {{"cursor", first.at("next_cursor")}});
        EXPECT_EQ(stale.reason, "stale-cursor");
    }
    auto cursor = query(doc, "query.objects", {{"limit", 1}}).at("next_cursor");
    EXPECT_EQ(run(doc, "query.images", {{"cursor", cursor}}).reason, "stale-cursor");
    EXPECT_EQ(run(doc, "query.objects", {{"cursor", cursor}, {"type", "rect"}}).reason, "stale-cursor");
}
TEST_F(VacardsCliQuery, FontsAndSelectionListsPaginateOccurrences) {
    auto body = "<text id=\"one\" style=\"font-family:serif\">A</text><text id=\"two\" style=\"font-family:sans-serif\">B</text>";
    auto input = load_inspection_document(write(svg(body)), {}); ASSERT_FALSE(input.error); auto doc = input.document.get();
    auto first = query(doc, "query.fonts", {{"limit", 1}});
    auto second = query(doc, "query.fonts", {{"limit", 1}, {"cursor", first.at("next_cursor")}});
    EXPECT_EQ(first.at("items").as_array()[0].as_object().at("object_id"), "one");
    EXPECT_EQ(second.at("items").as_array()[0].as_object().at("object_id"), "two");
    EXPECT_TRUE(first.at("items").as_array()[0].as_object().at("available").as_bool());
    auto sel = doc->getSelection(); sel->add(cast<SPItem>(doc->getObjectById("one"))); sel->add(cast<SPItem>(doc->getObjectById("two")));
    auto a = run(doc, "query.selection", {{"limit", 1}}, sel);
    auto b = run(doc, "query.selection", {{"limit", 1}, {"cursor", a.data.at("next_cursor")}}, sel);
    EXPECT_EQ(a.data.at("total"), 2); EXPECT_NE(a.data.at("items"), b.data.at("items"));
}
TEST_F(VacardsCliQuery, DefaultHundredThousandCapAndCompletePagination) {
    std::string body;
    for (unsigned i = 0; i < 1005; ++i) body += "<rect id=\"r" + std::to_string(i) + "\" width=\"1\" height=\"1\"/>";
    auto input = load_inspection_document(write(svg(body)), {}); ASSERT_FALSE(input.error); auto doc = input.document.get();
    auto first = query(doc, "query.objects", {{"type", "rect"}}); EXPECT_EQ(first.at("items").as_array().size(), 100u);
    auto capped = query(doc, "query.objects", {{"type", "rect"}, {"limit", 1000}}); EXPECT_EQ(capped.at("items").as_array().size(), 1000u);
    auto remainder = query(doc, "query.objects", {{"type", "rect"}, {"limit", 1000}, {"cursor", capped.at("next_cursor")}});
    EXPECT_EQ(remainder.at("items").as_array().size(), 5u); EXPECT_TRUE(remainder.at("next_cursor").is_null());
    std::set<std::string> ids;
    object page = first;
    while (true) {
        for (auto const &v : page.at("items").as_array()) EXPECT_TRUE(ids.insert(std::string(v.as_object().at("id").as_string())).second);
        if (page.at("next_cursor").is_null()) break;
        page = query(doc, "query.objects", {{"type", "rect"}, {"cursor", page.at("next_cursor")}});
    }
    EXPECT_EQ(ids.size(), 1005u); EXPECT_TRUE(ids.contains("r0")); EXPECT_TRUE(ids.contains("r1004"));
    EXPECT_EQ(run(doc, "query.objects", {{"limit", 1001}}).reason, "out-of-range");
}
TEST_F(VacardsCliQuery, StaleCursorAfterRevisionCommitHasHint) {
    auto input = sheet(); ASSERT_FALSE(input.error); auto doc = input.document.get();
    auto cursor = query(doc, "query.pages", {{"limit", 1}}).at("next_cursor");
    auto before = document_stamp(doc).revision;
    doc->getRoot()->getRepr()->setAttribute("data-test", "committed");
    DocumentUndo::done(doc, Util::Internal::ContextString("revision probe"), "");
    ASSERT_GT(document_stamp(doc).revision, before);
    auto r = run(doc, "query.pages", {{"cursor", cursor}}); EXPECT_EQ(r.reason, "stale-cursor");
    EXPECT_FALSE(typed_result(r, "stale").at("error").as_object().at("hint").as_string().empty());
    EXPECT_NE(r.message.find("restart"), std::string::npos);
}
TEST_F(VacardsCliQuery, UnknownKeysTypesRangesIdsAndProjectionRefuseWithoutMutation) {
    auto input = sheet(); ASSERT_FALSE(input.error); auto doc = input.document.get();
    for (auto command : {"query.document", "query.pages", "query.layers", "query.objects", "query.geometry", "query.styles", "query.images", "query.fonts", "query.selection"}) {
        SCOPED_TRACE(command); EXPECT_EQ(run(doc, command, {{"unknown", 1}}).reason, "unknown-key");
        if (std::string_view(command) != "query.document") {
            EXPECT_EQ(run(doc, command, {{"limit", "10"}}).reason, "wrong-type");
            EXPECT_EQ(run(doc, command, {{"limit", 0}}).reason, "out-of-range");
            EXPECT_EQ(run(doc, command, {{"limit", 1001}}).reason, "out-of-range");
            EXPECT_EQ(run(doc, command, {{"cursor", 1}}).reason, "wrong-type");
        }
    }
    for (auto command : {"query.objects", "query.geometry", "query.styles", "query.images", "query.fonts"}) {
        SCOPED_TRACE(command); EXPECT_EQ(run(doc, command, {{"ids", {"no-such-object"}}}).reason, "unknown-id");
        EXPECT_EQ(run(doc, command, {{"ids", {"rect", "rect"}}}).reason, "duplicate-id");
        EXPECT_EQ(run(doc, command, {{"ids", "rect"}}).reason, "wrong-type");
        EXPECT_EQ(run(doc, command, {{"ids", array{}}}).reason, "out-of-range");
    }
    EXPECT_EQ(run(doc, "query.styles", {{"properties", {"script"}}}).reason, "not-a-choice");
    EXPECT_EQ(run(doc, "query.objects", {{"visibility", "maybe"}}).reason, "not-a-choice");
    EXPECT_EQ(run(doc, "query.objects", {{"lock", false}}).reason, "wrong-type");
    EXPECT_EQ(run(doc, "query.objects", {{"bounds", "desktop"}}).reason, "not-a-choice");
    EXPECT_EQ(run(doc, "query.objects", {{"layer", "rect"}}).reason, "unknown-id");
    EXPECT_EQ(run(doc, "query.pages", {{"cursor", "garbage"}}).reason, "stale-cursor");
}
TEST_F(VacardsCliQuery, GeneratedExamplesSchemasAndDryRunAreReadOnly) {
    auto input = sheet(); ASSERT_FALSE(input.error);
    unsigned count = 0;
    for (auto spec : command_specs()) if (spec->canonical_id.starts_with("query.")) {
        ++count; SCOPED_TRACE(spec->canonical_id);
        auto example = parse(spec->example); EXPECT_TRUE(conforms(example, request_schema(*spec)));
        auto r = parse_request(spec->example); ASSERT_TRUE(r.request);
        for (bool dry : {false, true}) {
            auto record = run(input.document.get(), std::string(spec->canonical_id), r.request->params, nullptr, dry);
            EXPECT_EQ(record.status, Status::Ok); EXPECT_EQ(record.undo_effect, "none");
            EXPECT_TRUE(record.created.empty()); EXPECT_TRUE(record.modified.empty()); EXPECT_TRUE(record.deleted.empty());
        }
    }
    EXPECT_EQ(count, 9u);
    auto spec = find_command("query.pages");
    auto wire = typed_result(run(input.document.get(), "query.pages"), "schema-probe");
    wire.at("data").as_object().at("items").as_array()[0].as_object().at("rect").as_object()["width"] = "200px";
    EXPECT_FALSE(conforms(wire, result_schema_descriptor(*spec)));
}
TEST_F(VacardsCliQuery, QueryPreservesPreexistingUndoAndRedo) {
    auto input = sheet(); ASSERT_FALSE(input.error); auto doc = input.document.get();
    doc->getRoot()->getRepr()->setAttribute("data-history", "probe");
    DocumentUndo::done(doc, Util::Internal::ContextString("history probe"), "");
    ASSERT_TRUE(DocumentUndo::undo(doc));
    for (auto command : {"query.document", "query.pages", "query.layers", "query.objects", "query.geometry", "query.styles", "query.images", "query.fonts", "query.selection"}) EXPECT_EQ(run(doc, command).status, Status::Ok);
    ASSERT_TRUE(DocumentUndo::redo(doc)); EXPECT_STREQ(doc->getReprRoot()->attribute("data-history"), "probe");
}
TEST_F(VacardsCliQuery, IntakeRejectsRelativeUrlMissingDirectoryAndOversizeSparseInput) {
    for (auto path : {std::string("relative.svg"), std::string("https://example.invalid/a.svg"), temporary + "/absent.svg", temporary}) {
        SCOPED_TRACE(path);
        auto r = load_inspection_document(path, {}); ASSERT_TRUE(r.error) << path; EXPECT_FALSE(r.document);
        EXPECT_FALSE(r.error->hint.empty());
        EXPECT_EQ(r.error->code, path.ends_with("absent.svg") ? "missing-file" : "invalid-path");
    }
#ifdef _WIN32
    for (auto const &path : {std::string("C:relative.svg"), std::string("\\root-relative.svg")}) {
        SCOPED_TRACE(path);
        auto r = load_inspection_document(path, {});
        ASSERT_TRUE(r.error); EXPECT_FALSE(r.document);
        EXPECT_EQ(r.error->code, "invalid-path"); EXPECT_FALSE(r.error->hint.empty());
    }
#endif
    auto path = temporary + "/oversize.svg"; int fd = open(path.c_str(), O_WRONLY | O_CREAT, 0600); ASSERT_GE(fd, 0);
    auto resized = ftruncate(fd, (512ll << 20) + 1); close(fd);
    ASSERT_EQ(resized, 0);
    ASSERT_EQ(std::filesystem::file_size(path), (512ull << 20) + 1);
    auto r = load_inspection_document(path, {}); ASSERT_TRUE(r.error); EXPECT_EQ(r.error->code, "engine-limit");
    EXPECT_EQ(r.error->details.at("limit").to_number<std::uint64_t>(), 512ull << 20);
    EXPECT_EQ(r.error->details.at("found").to_number<std::uint64_t>(), (512ull << 20) + 1);
    EXPECT_FALSE(r.document); EXPECT_TRUE(r.document_id.empty());
    // Sparse old boundary reaches memory admission, rather than the input-byte ceiling.
    ASSERT_EQ(::truncate(path.c_str(), (64ll << 20) + 1), 0);
    ScopedIntakeMemoryForTesting no_memory(0);
    auto old = load_inspection_document(path, {});
    ASSERT_TRUE(old.error); EXPECT_FALSE(old.document); EXPECT_TRUE(old.document_id.empty());
    EXPECT_EQ(old.error->details.at("reason"), "insufficient-memory");
    EXPECT_EQ(old.error->details.at("estimated_bytes"), (64ull << 20) + 1);
}
TEST_F(VacardsCliQuery, IntakeChecksDepthAndObjectsBeforeNativeConstruction) {
    std::string body; for (unsigned i = 0; i < 128; ++i) body += "<g>";
    for (unsigned i = 0; i < 128; ++i) body += "</g>";
    auto depth = load_inspection_document(write(svg(body)), {}); ASSERT_TRUE(depth.error); EXPECT_FALSE(depth.document);
    EXPECT_EQ(depth.error->code, "engine-limit"); EXPECT_EQ(depth.error->details.at("limit_name"), "xml-depth");
    EXPECT_EQ(depth.error->details.at("limit"), 128); EXPECT_EQ(depth.error->details.at("found"), 129);
    body.clear(); for (unsigned i = 0; i < 1000000; ++i) body += "<g/>";
    auto objects = load_inspection_document(write(svg(body)), {}); ASSERT_TRUE(objects.error); EXPECT_FALSE(objects.document);
    EXPECT_EQ(objects.error->code, "engine-limit"); EXPECT_EQ(objects.error->details.at("limit_name"), "objects");
    EXPECT_EQ(objects.error->details.at("limit"), 1000000); EXPECT_EQ(objects.error->details.at("found"), 1000001);
    auto tight = load_inspection_document(write(svg("<rect/><rect/>")), {}, {64ull << 20, 2, 128});
    ASSERT_TRUE(tight.error); EXPECT_EQ(tight.error->details.at("limit"), 2);
    body.clear(); for (unsigned i = 0; i < 127; ++i) body += "<g>";
    for (unsigned i = 0; i < 127; ++i) body += "</g>";
    auto allowed = load_inspection_document(write(svg(body)), {}); ASSERT_FALSE(allowed.error) << (allowed.error ? allowed.error->message : "");
}
TEST_F(VacardsCliQuery, IntakeRefusesDtdEntitiesMalformedXmlAndUnsupportedFormats) {
    for (auto input : {"<!DOCTYPE svg SYSTEM 'file:///no-such-dtd'><svg xmlns='http://www.w3.org/2000/svg'/>",
        "<!DOCTYPE svg [<!ENTITY x SYSTEM 'https://example.invalid/entity'>]><svg xmlns='http://www.w3.org/2000/svg'>&x;</svg>",
        "<!DOCTYPE svg [<!ENTITY x 'expanded'>]><svg xmlns='http://www.w3.org/2000/svg'>&x;</svg>"}) {
        auto r = load_inspection_document(write(input), {}); ASSERT_TRUE(r.error); EXPECT_EQ(r.error->code, "unsafe-xml");
    }
    auto malformed = load_inspection_document(write("<svg xmlns='http://www.w3.org/2000/svg'><rect></svg>"), {});
    ASSERT_TRUE(malformed.error); EXPECT_EQ(malformed.error->code, "invalid-xml");
    for (auto bytes : {std::string("not SVG"), std::string("<html/>")}) {
        auto r = load_inspection_document(write(bytes, "looks-like.svg"), {}); ASSERT_TRUE(r.error);
        EXPECT_EQ(r.error->code, "unsupported-format"); EXPECT_NE(r.error->hint.find("M2"), std::string::npos);
    }
}
TEST_F(VacardsCliQuery, IntakeSniffsSvgSvgzAndChecksExpandedBytes) {
    auto plain = load_inspection_document(write(svg("<rect width='2' height='3'/>"), "not-svg.bin"), {}); ASSERT_FALSE(plain.error);
    EXPECT_EQ(plain.report.at("format"), "svg");
    auto path = temporary + "/gzip-content.bin"; auto gz = gzopen(path.c_str(), "wb"); ASSERT_NE(gz, nullptr);
    auto content = svg("<rect id='tile' width='2' height='3'/>"); ASSERT_GT(gzwrite(gz, content.data(), content.size()), 0); ASSERT_EQ(gzclose(gz), Z_OK);
    auto compressed = load_inspection_document(path, {}); ASSERT_FALSE(compressed.error); EXPECT_EQ(compressed.report.at("format"), "svgz");
    auto data = query(compressed.document.get(), "query.objects", {{"ids", {"tile"}}});
    rect(data.at("items").as_array()[0].as_object().at("bounds").as_object(), 0, 0, 2, 3, "geometric");
    EXPECT_EQ(inspection_formats(), (std::vector<std::string>{"svg", "svgz"}));
    auto bomb_path = temporary + "/expanded-limit.svgz";
    auto bomb = gzopen(bomb_path.c_str(), "wb"); ASSERT_NE(bomb, nullptr);
    auto expanded = svg("<!--" + std::string(10000, 'a') + "-->");
    ASSERT_GT(gzwrite(bomb, expanded.data(), expanded.size()), 0); ASSERT_EQ(gzclose(bomb), Z_OK);
    ASSERT_LT(std::filesystem::file_size(bomb_path), 512u);
    auto limited = load_inspection_document(bomb_path, {}, {512, 100000, 128});
    ASSERT_TRUE(limited.error); EXPECT_EQ(limited.error->code, "engine-limit");
    EXPECT_EQ(limited.error->details.at("limit_name"), "expanded-input-bytes");
    EXPECT_GT(limited.error->details.at("found").to_number<unsigned>(), 512u);
}
TEST_F(VacardsCliQuery, IntakeIdMapCoversUnnamedObjectsAndQueriesNeverAssignIds) {
    auto input = load_inspection_document(write(svg("<g><rect width='1' height='2'/></g>")), {}); ASSERT_FALSE(input.error);
    std::set<std::string> mapped;
    for (auto const &v : input.report.at("id_map").as_array()) {
        auto name = std::string(v.as_object().at("id").as_string()); EXPECT_TRUE(mapped.insert(name).second);
        EXPECT_NE(input.document->getObjectById(name.c_str()), nullptr);
    }
    auto data = query(input.document.get(), "query.objects");
    EXPECT_GE(mapped.size(), 3u);
    for (auto const &v : data.at("items").as_array()) EXPECT_TRUE(mapped.contains(std::string(v.as_object().at("id").as_string())));
}
TEST_F(VacardsCliQuery, UngrantedFifoExternalEntityAndFallbackAreProvablyNotRead) {
#ifndef _WIN32
    auto fifo = temporary + "/never-read.fifo"; ASSERT_EQ(mkfifo(fifo.c_str(), 0600), 0);
    auto content = "<svg xmlns='http://www.w3.org/2000/svg' xmlns:xlink='http://www.w3.org/1999/xlink' xmlns:sodipodi='http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd'><image id='fifo' width='1' height='1' xlink:href='" + fifo + "' sodipodi:absref='" + fifo + "'/></svg>";
    auto start = std::chrono::steady_clock::now(); auto input = load_inspection_document(write(content), {}); ASSERT_FALSE(input.error);
    EXPECT_LT(std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(), 2);
    EXPECT_EQ(input.report.at("resources").as_array()[0].as_object().at("state"), "ungranted");
    EXPECT_EQ(query(input.document.get(), "query.images").at("items").as_array()[0].as_object().at("source_kind"), "missing");
    EXPECT_EQ(input.document->getObjectById("fifo")->getRepr()->attribute("sodipodi:absref"), nullptr);
    auto entity = "<!DOCTYPE svg [<!ENTITY ext SYSTEM 'file://" + fifo + "'>]><svg xmlns='http://www.w3.org/2000/svg'>&ext;</svg>";
    auto r = load_inspection_document(write(entity), {}); ASSERT_TRUE(r.error); EXPECT_EQ(r.error->code, "unsafe-xml");
#else
    SUCCEED() << "FIFO witness is POSIX; denied-resource fixtures remain platform-independent.";
#endif
}
TEST_F(VacardsCliQuery, IntakeCssFontsExternalUseXincludeAndSymlinkCannotEscapeGrants) {
    auto outside = write("secret", "outside.bin"); auto granted = temporary + "/granted"; std::filesystem::create_directory(granted);
    auto style = "<style>@import 'https://example.invalid/a.css'; @font-face {font-family:X;src:url(" + outside + ");}</style>";
    auto use = "<use id='external' xmlns:xlink='http://www.w3.org/1999/xlink' xlink:href='" + outside + "#tile'/>";
    auto include = "<xi:include xmlns:xi='http://www.w3.org/2001/XInclude' href='" + outside + "'/>";
    auto input = load_inspection_document(write(svg(style + use + include)), Grants{{granted}}); ASSERT_FALSE(input.error);
    auto resources = input.report.at("resources").as_array();
    EXPECT_EQ(resources.size(), 4u);
    EXPECT_EQ(resources[0].as_object().at("state"), "remote");
    for (unsigned i = 1; i < 4; ++i) EXPECT_EQ(resources[i].as_object().at("state"), "ungranted");
    EXPECT_EQ(input.document->getObjectById("external")->getRepr()->attribute("xlink:href"), nullptr);
#ifndef _WIN32
    std::filesystem::create_symlink(outside, granted + "/escape.png");
    EXPECT_EQ(inspect_resource(granted + "/escape.png", temporary, Grants{{granted}}).state, "ungranted");
#endif
    EXPECT_EQ(inspect_resource("file://remote.invalid/a.png", temporary, Grants{{temporary}}).state, "remote");
    EXPECT_EQ(inspect_resource("//remote.invalid/a.png", temporary, Grants{{temporary}}).state, "remote");
}
TEST_F(VacardsCliQuery, ViewBoxAspectPoliciesAndPreferredUnitsAreExplicit) {
    for (bool stretch : {false, true}) {
        auto content = "<svg xmlns='http://www.w3.org/2000/svg' width='200' height='100' viewBox='10 20 50 50' preserveAspectRatio='" + std::string(stretch ? "none" : "xMidYMid meet") + "'><rect id='r' x='10' y='20' width='5' height='6'/></svg>";
        auto input = load_inspection_document(write(content), {}); ASSERT_FALSE(input.error);
        auto data = query(input.document.get(), "query.document");
        EXPECT_EQ(data.at("units").as_object().at("viewbox_to_css_px"), stretch ? (array{4., 0., 0., 2., -40., -40.}) : (array{2., 0., 0., 2., 30., -40.}));
        auto bounds = query(input.document.get(), "query.objects", {{"ids", {"r"}}}).at("items").as_array()[0].as_object().at("bounds").as_object();
        rect(bounds, stretch ? 0 : 50, 0, stretch ? 20 : 10, 12, "geometric");
        auto stamp = document_stamp(input.document.get());
        DispatchContext context{nullptr, input.document.get(), nullptr, "in"};
        auto before = xml_hash(input.document.get());
        auto record = dispatch({"inch", "query.document", stamp.id, {}, {}, false}, context);
        EXPECT_EQ(record.status, Status::Ok);
        EXPECT_NEAR(record.data.at("size").as_object().at("width").as_object().at("preferred").as_object().at("value").to_number<double>(), 200. / 96, 1e-10);
        EXPECT_EQ(xml_hash(input.document.get()), before); EXPECT_EQ(document_stamp(input.document.get()).revision, stamp.revision);
        EXPECT_TRUE(conforms(typed_result(record, "inch"), result_schema_descriptor(*find_command("query.document"))));
    }
}
TEST_F(VacardsCliQuery, ComputedInheritanceCurrentColorWeightAndQuotedFontFamilies) {
    auto content = svg("<g style='color:#ff0000;fill:currentColor;font-family:&quot;Missing, Family&quot;,serif;font-weight:400'><text id='t' style='fill:inherit;font-weight:bolder'>A</text></g>");
    auto input = load_inspection_document(write(content), {}); ASSERT_FALSE(input.error);
    auto style = query(input.document.get(), "query.styles", {{"ids", {"t"}}, {"properties", {"fill", "font-weight"}}});
    auto props = style.at("items").as_array()[0].as_object().at("properties").as_object();
    EXPECT_EQ(props.at("fill"), "#ff0000"); EXPECT_EQ(props.at("font-weight"), "700");
    auto fonts = query(input.document.get(), "query.fonts", {{"ids", {"t"}}});
    ASSERT_EQ(fonts.at("items").as_array().size(), 2u);
    EXPECT_EQ(fonts.at("items").as_array()[0].as_object().at("family"), "Missing, Family");
    EXPECT_TRUE(fonts.at("items").as_array()[0].as_object().at("missing").as_bool());
    EXPECT_EQ(fonts.at("items").as_array()[1].as_object().at("family"), "serif");
}
TEST_F(VacardsCliQuery, ResourcePolicyHandlesEscapedCssAndGrantedExternalUseWithoutExpansion) {
    auto external = write(svg("<rect id='r' width='1' height='1'/>"), "external.svg");
    auto content = svg("<style>@import 'external.svg';</style><rect id='r' width='1' height='1' style='fill:u\\72l(https://example.invalid/paint.svg#p)'/><use id='external' xmlns:xlink='http://www.w3.org/1999/xlink' xlink:href='" + external + "#r'/>");
    auto input = load_inspection_document(write(content), Grants{{temporary}}); ASSERT_FALSE(input.error);
    auto const &resources = input.report.at("resources").as_array(); ASSERT_EQ(resources.size(), 3u);
    EXPECT_EQ(resources[0].as_object().at("state"), "granted"); EXPECT_FALSE(resources[0].as_object().at("loaded").as_bool());
    EXPECT_EQ(resources[1].as_object().at("state"), "remote"); EXPECT_FALSE(resources[1].as_object().at("loaded").as_bool());
    EXPECT_EQ(resources[2].as_object().at("state"), "granted"); EXPECT_FALSE(resources[2].as_object().at("loaded").as_bool());
    EXPECT_EQ(input.document->getObjectById("r")->getRepr()->attribute("style"), nullptr);
    EXPECT_EQ(input.document->getObjectById("external")->getRepr()->attribute("xlink:href"), nullptr);
}
TEST_F(VacardsCliQuery, OversizeMetadataResponseRefusesWithinEightMiBCeiling) {
    auto input = load_inspection_document(write(svg("<metadata>" + std::string(8u << 20, 'x') + "</metadata>")), {}); ASSERT_FALSE(input.error);
    auto r = run(input.document.get(), "query.document"); EXPECT_EQ(r.reason, "engine-limit"); EXPECT_TRUE(r.data.empty());
    EXPECT_LT(serialize(typed_result(r, "oversize")).size(), 8388608u);
}
TEST_F(VacardsCliQuery, IntakeRejectsDuplicateIdsAndCloneCyclesBeforeNativeLoad) {
    auto duplicate = load_inspection_document(write(svg("<rect id='a'/><rect id='a'/>")), {});
    ASSERT_TRUE(duplicate.error); EXPECT_EQ(duplicate.error->code, "duplicate-id");
    auto cycle = load_inspection_document(write(svg("<g id='a'><use xmlns:xlink='http://www.w3.org/1999/xlink' xlink:href='#a'/></g>")), {});
    ASSERT_TRUE(cycle.error); EXPECT_EQ(cycle.error->code, "unsafe-reference");
    std::string chain = "<rect id='base'/>";
    for (unsigned i = 0; i < 129; ++i) chain += "<use xmlns:xlink='http://www.w3.org/1999/xlink' id='u" + std::to_string(i) + "' xlink:href='#" + (i ? "u" + std::to_string(i - 1) : "base") + "'/>";
    auto deep = load_inspection_document(write(svg(chain)), {}); ASSERT_TRUE(deep.error);
    EXPECT_EQ(deep.error->code, "engine-limit"); EXPECT_EQ(deep.error->details.at("limit_name"), "reference-depth");
}

TEST_F(VacardsCliQuery, IntakeRejectsNetworkAndDeviceNamespacesBeforeOpen) {
    // // + an existing local file is openable on POSIX; preflight must still refuse it.
    auto existing = write(svg());
    std::vector<std::string> paths{
        R"(\\server\share\a.svg)", "//server/share/a.svg", R"(\\?\UNC\server\share\a.svg)",
        R"(\\?\C:\art\a.svg)", R"(\\.\C:\art\a.svg)", R"(\??\C:\art\a.svg)",
        "//?/C:/art/a.svg"};
#ifndef _WIN32
    paths.push_back("/" + existing);
#endif
    for (auto const &path : paths) {
        SCOPED_TRACE(path);
        auto input = load_inspection_document(path, {});
        ASSERT_TRUE(input.error); EXPECT_FALSE(input.document); EXPECT_TRUE(input.report.empty());
        EXPECT_EQ(input.error->code, "invalid-path"); EXPECT_FALSE(input.error->retryable);
        EXPECT_NE(input.error->message.find("network"), std::string::npos);
        EXPECT_NE(input.error->hint.find("local SVG/SVGZ"), std::string::npos);
    }
}
TEST_F(VacardsCliQuery, ResourceNativeAbsolutePathsPrecedeUriSchemes) {
    // Foreign Windows paths must never be remote, even on the macOS runner.
    for (auto path : {R"(C:\art\image.png)", "C:/art/image.png"}) {
        auto resource = inspect_resource(path, temporary, Grants{{R"(C:\art)", "C:/art"}});
#ifndef _WIN32
        EXPECT_EQ(resource.state, "ungranted"); EXPECT_TRUE(resource.path.empty());
#else
        EXPECT_NE(resource.state, "remote");
#endif
    }
    auto image = write("local resource", "image.png");
    EXPECT_EQ(inspect_resource(image, temporary, Grants{{temporary}}).state, "granted");
#ifdef _WIN32
    // Exercise both drive separators against an actual file and an actual grant.
    auto backslashes = image; std::replace(backslashes.begin(), backslashes.end(), '/', '\\');
    auto slashes = image; std::replace(slashes.begin(), slashes.end(), '\\', '/');
    EXPECT_EQ(inspect_resource(backslashes, temporary, Grants{{temporary}}).state, "granted");
    EXPECT_EQ(inspect_resource(slashes, temporary, Grants{{temporary}}).state, "granted");
#endif
    auto uri = g_filename_to_uri(image.c_str(), nullptr, nullptr); ASSERT_NE(uri, nullptr);
    EXPECT_EQ(inspect_resource(uri, temporary, Grants{{temporary}}).state, "granted"); g_free(uri);
    EXPECT_EQ(inspect_resource(temporary + "/missing.png", temporary, Grants{{temporary}}).state, "missing");
    for (auto uri : {"https://example.invalid/image.png", "ftp://example.invalid/image.png",
                     "custom:resource", "C:relative.png", "file://remote.invalid/image.png"})
        EXPECT_EQ(inspect_resource(uri, temporary, Grants{{temporary}}).state, "remote") << uri;
}
TEST_F(VacardsCliQuery, PagesUseDocumentScaleWithoutViewBoxTranslation) {
    auto content = R"(<svg xmlns="http://www.w3.org/2000/svg" xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape" xmlns:sodipodi="http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd" width="300" height="100" viewBox="10 20 100 50" preserveAspectRatio="none"><sodipodi:namedview><inkscape:page id="p" x="15" y="25" width="30" height="20"/></sodipodi:namedview></svg>)";
    auto input = load_inspection_document(write(content), {}); ASSERT_FALSE(input.error);
    auto pages = query(input.document.get(), "query.pages");
    ASSERT_EQ(pages.at("items").as_array().size(), 1u);
    // Independently: x/width scale = 300/100 = 3; y/height scale = 100/50 = 2.
    rect(row(pages, "id", "p").at("rect").as_object(), 15 * 3, 25 * 2, 30 * 3, 20 * 2, "geometric");
}
TEST_F(VacardsCliQuery, VisibilityOverrideRetainsAncestorDisplaySuppression) {
    auto input = load_inspection_document(write(svg(
        "<g id='hidden-parent' visibility='hidden'>"
        "<rect id='override' visibility='visible' width='1' height='1'/>"
        "<rect id='inherited' width='1' height='1'/></g>"
        "<g id='display-parent' style='display:none'>"
        "<rect id='suppressed' style='display:inline;visibility:visible' width='1' height='1'/></g>")), {});
    ASSERT_FALSE(input.error); auto doc = input.document.get();
    auto data = query(doc, "query.objects", {{"type", "rect"}});
    EXPECT_TRUE(row(data, "id", "override").at("visible").as_bool());
    EXPECT_FALSE(row(data, "id", "inherited").at("visible").as_bool());
    EXPECT_FALSE(row(data, "id", "suppressed").at("visible").as_bool());
    for (auto visibility : {"visible", "hidden"}) {
        auto filtered = query(doc, "query.objects", {{"type", "rect"}, {"visibility", visibility}});
        std::set<std::string> actual;
        for (auto const &v : filtered.at("items").as_array()) actual.insert(std::string(v.as_object().at("id").as_string()));
        EXPECT_EQ(actual, std::string_view(visibility) == "visible" ? std::set<std::string>{"override"} :
                  (std::set<std::string>{"inherited", "suppressed"}));
    }
}
TEST_F(VacardsCliQuery, DocumentMetadataIgnoresDescendantElements) {
    auto nested = "<g><title>Tile</title><metadata>Tile metadata</metadata>"
                  "<rect width='1' height='1'><title>Rectangle</title><metadata>Rectangle metadata</metadata></rect></g>";
    auto input = load_inspection_document(write(svg(
        "<title id='document-title'>Document</title><metadata id='document-meta'>Document metadata</metadata>" +
        std::string(nested))), {});
    ASSERT_FALSE(input.error);
    auto data = query(input.document.get(), "query.document");
    EXPECT_EQ(data.at("title"), "Document");
    EXPECT_EQ(data.at("metadata"), "<metadata id=\"document-meta\">Document metadata</metadata>");
    EXPECT_EQ(input.report.at("title"), "Document");
    auto nested_only = load_inspection_document(write(svg(nested)), {}); ASSERT_FALSE(nested_only.error);
    auto absent = query(nested_only.document.get(), "query.document");
    EXPECT_FALSE(absent.contains("title")); EXPECT_FALSE(absent.contains("metadata"));
    EXPECT_FALSE(nested_only.report.contains("title")); EXPECT_FALSE(nested_only.report.contains("metadata"));
}

TEST_F(VacardsCliQuery, SuccessfulQuerySchemasRequireDocumentedPayload) {
    auto input = sheet(); ASSERT_FALSE(input.error);
    unsigned count = 0;
    for (auto spec : command_specs()) if (spec->canonical_id.starts_with("query.")) {
        ++count; SCOPED_TRACE(spec->canonical_id);
        auto schema = result_schema_descriptor(*spec);
        auto example = parse_request(spec->example); ASSERT_TRUE(example.request);
        auto wire = typed_result(run(input.document.get(), std::string(spec->canonical_id), example.request->params), "payload");
        ASSERT_TRUE(wire.contains("data"));
        // Expected truth values are protocol requirements, independent of schema construction.
        for (auto status : {"ok", "changed", "unchanged"}) {
            SCOPED_TRACE(status); auto valid = wire; valid["status"] = status;
            EXPECT_TRUE(conforms(valid, schema)); EXPECT_FALSE(validate_schema(valid, schema));
            auto empty = valid; empty["data"] = object{};
            EXPECT_FALSE(conforms(empty, schema)); EXPECT_TRUE(validate_schema(empty, schema));
            auto absent = valid; absent.erase("data");
            EXPECT_FALSE(conforms(absent, schema)); EXPECT_TRUE(validate_schema(absent, schema));
            auto incomplete = valid;
            incomplete.at("data").as_object().erase(spec->canonical_id == "query.document" ? "size" : "items");
            EXPECT_FALSE(conforms(incomplete, schema)); EXPECT_TRUE(validate_schema(incomplete, schema));
            auto extra = valid; extra.at("data").as_object()["undocumented"] = 1;
            EXPECT_FALSE(conforms(extra, schema)); EXPECT_TRUE(validate_schema(extra, schema));
        }
        auto error = typed_result(run(input.document.get(), std::string(spec->canonical_id), {{"unknown", true}}), "error");
        ASSERT_TRUE(error.contains("error"));
        for (auto status : {"rejected", "cancelled", "failed", "uncertain"}) {
            SCOPED_TRACE(status); error["status"] = status;
            error["data"] = object{};
            EXPECT_TRUE(conforms(error, schema)); EXPECT_FALSE(validate_schema(error, schema));
            error.erase("data");
            EXPECT_TRUE(conforms(error, schema)); EXPECT_FALSE(validate_schema(error, schema));
            auto malformed = error; malformed["data"] = "not an object";
            EXPECT_FALSE(conforms(malformed, schema)); EXPECT_TRUE(validate_schema(malformed, schema));
            auto extra = error; extra["undocumented"] = true;
            EXPECT_FALSE(conforms(extra, schema)); EXPECT_TRUE(validate_schema(extra, schema));
        }
    }
    EXPECT_EQ(count, 9u);
}
TEST_F(VacardsCliQuery, M1OriginalEmbeddedHrefAndInitialStroke) {
    auto input = sheet(); ASSERT_TRUE(input.document);
    auto images = run(input.document.get(), "query.images");
    bool checked = false;
    for (auto const &row : images.data.at("items").as_array()) {
        auto const &o = row.as_object();
        if (o.at("source_kind") != "embedded") continue;
        auto image = input.document->getObjectById(std::string(o.at("id").as_string()));
        ASSERT_NE(image, nullptr);
        auto original = image->getRepr()->attribute("xlink:href");
        ASSERT_NE(original, nullptr);
        EXPECT_EQ(o.at("href"), original); EXPECT_GT(o.at("href").as_string().size(), 5u);
        checked = true;
    }
    EXPECT_TRUE(checked);
    auto plain = load_inspection_document(write(svg("<rect id='plain' width='5' height='5'/>")), {});
    ASSERT_TRUE(plain.document);
    auto styles = run(plain.document.get(), "query.styles", {{"ids", array{"plain"}}, {"properties", array{"stroke"}}});
    EXPECT_EQ(styles.data.at("items").as_array().at(0).as_object().at("properties").as_object().at("stroke"), "none");
}
} // namespace
