#include "actions/vacards-cli-dispatch.h"
// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * VACards agent command line: result records, channel, outcome latch and spec
 * registry (design 5.3-5.5). The expected strings below are the oracle for
 * vacards-cli-result.cpp.
 */

#include "actions/vacards-cli-result.h"

#include <fstream>
#include <sstream>
#include <gtest/gtest.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <boost/json.hpp>

using namespace Inkscape::VACardsCli;

namespace {

class VacardsCliResultTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        reset_for_testing();
        gchar *tmp = g_dir_make_tmp("vacards-cli-result-XXXXXX", nullptr);
        ASSERT_NE(tmp, nullptr);
        dir = tmp;
        g_free(tmp);
    }
    void TearDown() override { reset_for_testing(); }

    std::string path(char const *name) const
    {
        gchar *p = g_build_filename(dir.c_str(), name, nullptr);
        std::string result = p;
        g_free(p);
        return result;
    }

    static std::string read(std::string const &file)
    {
        std::ifstream in(file, std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    static Record sample()
    {
        Record r;
        r.action = "vacards-offset";
        r.params_text = "distance=2mm,direction=outward";
        r.params = {{"distance", "2mm"}, {"direction", "outward"}};
        r.status = Status::Changed;
        r.reason = "success";
        r.message = "Created 2 offset paths.";
        r.mode = "compatible-members";
        r.document_path = "/work/input.svg";
        r.selected = 3;
        r.eligible = 2;
        r.excluded = {{"text4", "not-a-closed-path"}};
        r.created = {"path301", "path302"};
        r.selection_after = {"path301", "path302"};
        r.one_undo_step = true;
        return r;
    }

    static Record with_status(Status status)
    {
        Record r;
        r.action = "vacards-test";
        r.status = status;
        return r;
    }

    std::string dir;
};

} // namespace

TEST_F(VacardsCliResultTest, StatusNames)
{
    EXPECT_EQ(status_name(Status::Ok), "ok");
    EXPECT_EQ(status_name(Status::Changed), "changed");
    EXPECT_EQ(status_name(Status::Unchanged), "unchanged");
    EXPECT_EQ(status_name(Status::Rejected), "rejected");
    EXPECT_EQ(status_name(Status::Cancelled), "cancelled");
    EXPECT_EQ(status_name(Status::Failed), "failed");
    EXPECT_EQ(status_name(Status::Uncertain), "uncertain");
    EXPECT_EQ(status_name(Status::Skipped), "skipped");
}

TEST_F(VacardsCliResultTest, RecordKeyOrderAndValues)
{
    EXPECT_EQ(to_json_line(sample(), 3),
              R"({"schema":"va-studio.cli-result/1","seq":3,"action":"vacards-offset",)"
              R"("params_text":"distance=2mm,direction=outward",)"
              R"("params":{"distance":"2mm","direction":"outward"},"status":"changed","reason":"success",)"
              R"("message":"Created 2 offset paths.","mode":"compatible-members","dry_run":false,)"
              R"("document":{"path":"/work/input.svg"},)"
              R"("targets":{"selected":3,"eligible":2,"covered":0,"excluded":[{"id":"text4","reason":"not-a-closed-path"}]},)"
              R"("created":["path301","path302"],"modified":[],"deleted":[],)"
              R"("selection_after":["path301","path302"],"undo":"one-step","metrics":{},"warnings":[]})");
}

TEST_F(VacardsCliResultTest, OptionalFieldsAndEscaping)
{
    Record r;
    r.action = "vacards-test";
    r.params_text = "bogus=1";
    r.status = Status::Rejected;
    r.reason = "invalid-argument";
    r.message = "Line one\nsaid \"hi\" \xE2\x9C\x93";
    r.dry_run = true;
    r.metrics["count"] = 2;
    r.warnings = {"w1"};
    r.error = ParseError{"unknown-key", "bogus", "Unknown parameter 'bogus' for vacards-test."};
    r.data["x"] = 1;
    EXPECT_EQ(to_json_line(r, 1),
              R"({"schema":"va-studio.cli-result/1","seq":1,"action":"vacards-test","params_text":"bogus=1",)"
              R"("params":{},"status":"rejected","reason":"invalid-argument",)"
              "\"message\":\"Line one\\nsaid \\\"hi\\\" \xE2\x9C\x93\","
              R"("mode":"","dry_run":true,"document":null,)"
              R"("targets":{"selected":0,"eligible":0,"covered":0,"excluded":[]},)"
              R"("created":[],"modified":[],"deleted":[],"selection_after":[],"undo":"none",)"
              R"("metrics":{"count":2},"warnings":["w1"],"error":{"code":"unknown-key","key":"bogus"},"data":{"x":1}})");
}

TEST_F(VacardsCliResultTest, SkippedRecord)
{
    auto const r = make_skipped_record("export-do", "");
    EXPECT_EQ(r.action, "export-do");
    EXPECT_EQ(r.status, Status::Skipped);
    EXPECT_EQ(r.reason, "halted-after-error");
    EXPECT_EQ(r.message, "Not run because an earlier action was rejected or failed.");
    EXPECT_FALSE(r.one_undo_step);
    EXPECT_FALSE(r.document_path.has_value());
}

TEST_F(VacardsCliResultTest, FileChannelWritesBareNumberedLines)
{
    std::string error;
    auto const file = path("run.jsonl");
    ASSERT_TRUE(g_file_set_contents(file.c_str(), "old content\n", -1, nullptr));
    ASSERT_TRUE(open_result_channel(file, error)) << error;
    EXPECT_TRUE(error.empty());
    emit(sample());
    emit(with_status(Status::Ok));
    auto const text = read(file);
    auto const first_end = text.find('\n');
    ASSERT_NE(first_end, std::string::npos);
    EXPECT_EQ(text.substr(0, first_end), to_json_line(sample(), 1)); // truncated, no prefix
    EXPECT_EQ(text.substr(first_end + 1), to_json_line(with_status(Status::Ok), 2) + "\n");
}

TEST_F(VacardsCliResultTest, FailedOpenKeepsPreviousChannel)
{
    std::string error;
    auto const file = path("first.jsonl");
    ASSERT_TRUE(open_result_channel(file, error)) << error;
    ASSERT_FALSE(open_result_channel(path("missing-dir/x.jsonl"), error));
    EXPECT_FALSE(error.empty());
    emit(with_status(Status::Ok));
    EXPECT_EQ(read(file), to_json_line(with_status(Status::Ok), 1) + "\n");
}

TEST_F(VacardsCliResultTest, ExitStatusLatch)
{
    std::string error;
    ASSERT_TRUE(open_result_channel(path("latch.jsonl"), error)) << error;
    EXPECT_EQ(exit_status(), 0);
    emit(with_status(Status::Ok));
    emit(with_status(Status::Changed));
    emit(with_status(Status::Unchanged));
    emit(with_status(Status::Cancelled));
    emit(with_status(Status::Skipped));
    EXPECT_EQ(exit_status(), 0);
    emit(with_status(Status::Rejected));
    EXPECT_EQ(exit_status(), 3);
    emit(with_status(Status::Uncertain));
    EXPECT_EQ(exit_status(), 4);
    emit(with_status(Status::Rejected));
    EXPECT_EQ(exit_status(), 4);

    reset_for_testing();
    EXPECT_EQ(exit_status(), 0);
    note_rejection();
    EXPECT_EQ(exit_status(), 3);
    EXPECT_FALSE(halt_pending()); // note_rejection never halts
    ASSERT_TRUE(open_result_channel(path("latch2.jsonl"), error)) << error;
    emit(with_status(Status::Failed));
    EXPECT_EQ(exit_status(), 4);
}

TEST_F(VacardsCliResultTest, HaltState)
{
    std::string error;
    ASSERT_TRUE(open_result_channel(path("halt.jsonl"), error)) << error;
    EXPECT_TRUE(halt_on_error());
    EXPECT_FALSE(halt_pending());
    for (auto status : {Status::Ok, Status::Changed, Status::Unchanged, Status::Cancelled, Status::Skipped}) {
        emit(with_status(status));
        EXPECT_FALSE(halt_pending()) << status_name(status);
    }
    for (auto status : {Status::Rejected, Status::Failed, Status::Uncertain}) {
        begin_chain();
        EXPECT_FALSE(halt_pending());
        emit(with_status(status));
        EXPECT_TRUE(halt_pending()) << status_name(status);
    }
    begin_chain();
    set_halt_on_error(false);
    EXPECT_FALSE(halt_on_error());
    emit(with_status(Status::Rejected));
    EXPECT_FALSE(halt_pending());
    EXPECT_EQ(exit_status(), 4); // the latch still counts

    reset_for_testing();
    EXPECT_TRUE(halt_on_error());
}

TEST_F(VacardsCliResultTest, ExportVetoIsStickyAcrossChainsUntilNextDocument)
{
    std::string error;
    ASSERT_TRUE(open_result_channel(path("veto.jsonl"), error)) << error;
    begin_document();
    EXPECT_FALSE(export_vetoed());
    begin_chain();
    emit(with_status(Status::Rejected));
    EXPECT_TRUE(export_vetoed());
    begin_chain(); // a later shell line clears the halt but not the veto
    EXPECT_FALSE(halt_pending());
    emit(with_status(Status::Changed));
    EXPECT_TRUE(export_vetoed());
    begin_document();
    EXPECT_FALSE(export_vetoed());
    note_rejection(); // upstream diagnostics never veto the export
    EXPECT_FALSE(export_vetoed());
    set_halt_on_error(false); // continue-on-error also keeps the automatic export
    emit(with_status(Status::Failed));
    EXPECT_FALSE(export_vetoed());
}

TEST_F(VacardsCliResultTest, CommandLineChainScopeNests)
{
    EXPECT_FALSE(command_line_chain_active());
    {
        CommandLineChainScope const outer;
        EXPECT_TRUE(command_line_chain_active());
        {
            CommandLineChainScope const inner;
            EXPECT_TRUE(command_line_chain_active());
        }
        EXPECT_TRUE(command_line_chain_active());
    }
    EXPECT_FALSE(command_line_chain_active());
}

namespace {
constexpr ActionSpec spec_b{.name = "vacards-zz-b", .mode = "read-only", .summary = "B."};
constexpr ActionSpec spec_a{.name = "vacards-zz-a", .mode = "read-only", .summary = "A."};
constexpr std::string_view dirs[] = {"outward", "inward"};
constexpr ParamSpec describe_params[] = {
    {.key = "distance", .type = ParamType::Length, .required = true, .min = -1000, .max = 1000, .help = "Offset distance."},
    {.key = "direction", .type = ParamType::Choice, .default_value = "outward", .choices = dirs},
    {.key = "time", .type = ParamType::Duration, .default_value = "5s"},
    {.key = "ratio", .type = ParamType::Number, .min = 0.5},
    {.key = "dry-run", .type = ParamType::Boolean, .default_value = "false"},
};
constexpr ActionSpec describe_spec{
    .name = "vacards-zz-describe", .mode = "compatible-members", .summary = "Test.", .params = describe_params};
} // namespace

TEST_F(VacardsCliResultTest, SpecRegistry)
{
    register_action_spec(spec_b);
    register_action_spec(spec_a);
    register_action_spec(spec_b);
    auto const specs = registered_action_specs();
    int count_a = 0;
    int count_b = 0;
    std::size_t index_a = 0;
    std::size_t index_b = 0;
    for (std::size_t i = 0; i < specs.size(); ++i) {
        if (specs[i]->name == "vacards-zz-a") { ++count_a; index_a = i; }
        if (specs[i]->name == "vacards-zz-b") { ++count_b; index_b = i; }
        if (i > 0) {
            EXPECT_LT(specs[i - 1]->name, specs[i]->name);
        }
    }
    EXPECT_EQ(count_a, 1);
    EXPECT_EQ(count_b, 1);
    EXPECT_LT(index_a, index_b);
    EXPECT_EQ(find_action_spec("vacards-zz-a"), &spec_a);
    EXPECT_EQ(find_action_spec("vacards-zz-none"), nullptr);
    reset_for_testing();
    EXPECT_EQ(find_action_spec("vacards-zz-a"), &spec_a); // specs survive reset
}

TEST_F(VacardsCliResultTest, DescribeAction)
{
    auto const json = boost::json::serialize(describe_action(describe_spec));
    EXPECT_EQ(json,
              R"({"name":"vacards-zz-describe","mode":"compatible-members","summary":"Test.","params":[)"
              R"({"key":"distance","type":"length","unit":"px","required":true,"min":-1000,"max":1000,"help":"Offset distance."},)"
              R"({"key":"direction","type":"choice","required":false,"default":"outward","choices":["outward","inward"]},)"
              R"({"key":"time","type":"duration","unit":"ms","required":false,"default":"5s"},)"
              R"({"key":"ratio","type":"number","required":false,"min":)"
              + boost::json::serialize(boost::json::value(0.5)) +
              R"(},{"key":"dry-run","type":"boolean","required":false,"default":"false"}]})");
    auto const parsed = boost::json::parse(json).as_object();
    auto const &params = parsed.at("params").as_array();
    EXPECT_TRUE(params[0].as_object().at("min").is_int64());
    EXPECT_TRUE(params[3].as_object().at("min").is_double());
    EXPECT_DOUBLE_EQ(params[3].as_object().at("min").as_double(), 0.5);
}

/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0)(case-label . +))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4 :

TEST(VACardsTypedResult, PreservesExplicitNativeErrorReceipt) {
    using namespace Inkscape::VACardsCli;
    Record record; record.action="clip.set"; record.status=Status::Rejected;
    record.reason="unsupported-target"; record.message="Expected native refusal";
    record.error_details={{"reason","ClipDocumentService::Reason::MissingSource"},{"mutation_state","none"}};
    record.error_retryable=false;
    auto result=typed_result(record,"receipt");
    auto const &error=result.at("error").as_object();
    EXPECT_EQ(error.at("details").as_object().at("reason").as_string(),"ClipDocumentService::Reason::MissingSource");
    EXPECT_EQ(error.at("details").as_object().at("mutation_state").as_string(),"none");
    EXPECT_FALSE(error.at("retryable").as_bool());
    EXPECT_TRUE(cli_error("document-busy","busy","retry").at("retryable").as_bool());
    record.action="nest.analyze"; record.warnings={"conservative-recovery: one part"};
    EXPECT_EQ(typed_result(record,"warning").at("coded_warnings").as_array()[0].as_object().at("code").as_string(),"conservative-recovery");
}
