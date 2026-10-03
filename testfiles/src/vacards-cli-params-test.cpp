// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * VACards agent command line: parameter grammar (design 5.2).
 * The tables below are the oracle for vacards-cli-params.cpp.
 */

#include "actions/vacards-cli-params.h"

#include <cmath>
#include <gtest/gtest.h>

using namespace Inkscape::VACardsCli;

namespace {

constexpr std::string_view directions[] = {"outward", "inward", "both"};

constexpr ParamSpec test_params[] = {
    {.key = "flag", .type = ParamType::Boolean},
    {.key = "count", .type = ParamType::Integer, .min = 0, .max = 10},
    {.key = "ratio", .type = ParamType::Number, .min = -1, .max = 1},
    {.key = "distance", .type = ParamType::Length, .required = true, .min = -1000, .max = 1000},
    {.key = "angle", .type = ParamType::Angle, .default_value = "15", .min = 0, .max = 360},
    {.key = "time", .type = ParamType::Duration, .default_value = "5s", .min = 100, .max = 600000},
    {.key = "direction", .type = ParamType::Choice, .default_value = "outward", .choices = directions},
    {.key = "name", .type = ParamType::Text},
    {.key = "ids", .type = ParamType::List},
};

constexpr ActionSpec test_action{
    .name = "vacards-test", .mode = "read-only", .summary = "Test action.", .params = test_params};

constexpr ParamSpec bad_default_params[] = {
    {.key = "count", .type = ParamType::Integer, .default_value = "eleven"},
};
constexpr ActionSpec bad_default_action{.name = "vacards-bad", .mode = "read-only", .params = bad_default_params};

constexpr ActionSpec no_params_action{.name = "vacards-none", .mode = "read-only"};

struct RejectCase
{
    std::string_view input;
    std::string_view code;
    std::string_view key;
};

// Every input here must be rejected with exactly this code and key.
constexpr RejectCase reject_cases[] = {
    // structure
    {"distance=1,,count=2", "empty-pair", ""},
    {",distance=1", "empty-pair", ""},
    {"distance=1,", "empty-pair", ""},
    {"Distance=1", "bad-key", ""},
    {"1distance=1", "bad-key", ""},
    {"dist_ance=1", "bad-key", ""},
    {"=1", "bad-key", ""},
    {"distance=1,bogus=2", "unknown-key", "bogus"},
    {"distance=1,distance=2", "repeated-key", "distance"},
    {"distance=1,flag,flag=false", "repeated-key", "flag"},
    {"distance", "missing-value", "distance"},
    {"distance=1,count", "missing-value", "count"},
    {"distance=", "empty-value", "distance"},
    {"distance=1,name=", "empty-value", "name"},
    {"distance=1,ids=a||b", "empty-value", "ids"},
    {"distance=1,ids=a|", "empty-value", "ids"},
    {"distance=1,name=a%2", "bad-percent-encoding", "name"},
    {"distance=1,name=a%zz", "bad-percent-encoding", "name"},
    {"distance=1,name=%", "bad-percent-encoding", "name"},
    // first error in input order wins; missing-required is checked after all pairs
    {"bogus=1,distance=x", "unknown-key", "bogus"},
    {"count=99", "out-of-range", "count"},
    {"", "missing-required", "distance"},
    {"count=2", "missing-required", "distance"},
    // boolean
    {"distance=1,flag=yes", "malformed-value", "flag"},
    {"distance=1,flag=TRUE", "malformed-value", "flag"},
    {"distance=1,flag=2", "malformed-value", "flag"},
    // integer
    {"distance=1,count=1.5", "malformed-value", "count"},
    {"distance=1,count=1e1", "malformed-value", "count"},
    {"distance=1,count=0x5", "malformed-value", "count"},
    {"distance=1,count= 5", "malformed-value", "count"},
    {"distance=1,count=5 ", "malformed-value", "count"},
    {"distance=1,count=+", "malformed-value", "count"},
    {"distance=1,count=11", "out-of-range", "count"},
    {"distance=1,count=-1", "out-of-range", "count"},
    {"distance=1,count=99999999999999999999999", "out-of-range", "count"},
    // number
    {"distance=1,ratio=abc", "malformed-value", "ratio"},
    {"distance=1,ratio=nan", "malformed-value", "ratio"},
    {"distance=1,ratio=inf", "malformed-value", "ratio"},
    {"distance=1,ratio=0x1", "malformed-value", "ratio"},
    {"distance=1,ratio=1,5", "bad-key", ""},
    {"distance=1,ratio=1%2C5", "malformed-value", "ratio"},
    {"distance=1,ratio=.", "malformed-value", "ratio"},
    {"distance=1,ratio=1e", "malformed-value", "ratio"},
    {"distance=1,ratio=--1", "malformed-value", "ratio"},
    {"distance=1,ratio=0.5mm", "malformed-value", "ratio"},
    {"distance=1,ratio=1.5", "out-of-range", "ratio"},
    {"distance=1,ratio=1e400", "out-of-range", "ratio"},
    // length
    {"distance=2km", "malformed-value", "distance"},
    {"distance=2MM", "malformed-value", "distance"},
    {"distance=mm", "malformed-value", "distance"},
    {"distance=2 mm", "malformed-value", "distance"},
    {"distance=2mm2", "malformed-value", "distance"},
    {"distance=11in", "out-of-range", "distance"},
    {"distance=-1001", "out-of-range", "distance"},
    // angle
    {"distance=1,angle=90rad", "malformed-value", "angle"},
    {"distance=1,angle=361", "out-of-range", "angle"},
    {"distance=1,angle=-0.5deg", "out-of-range", "angle"},
    // duration
    {"distance=1,time=5", "malformed-value", "time"},
    {"distance=1,time=5h", "malformed-value", "time"},
    {"distance=1,time=50ms", "out-of-range", "time"},
    {"distance=1,time=11m", "out-of-range", "time"},
    // choice
    {"distance=1,direction=Outward", "not-a-choice", "direction"},
    {"distance=1,direction=sideways", "not-a-choice", "direction"},
};

class RejectTest : public ::testing::TestWithParam<RejectCase> {};

TEST_P(RejectTest, RejectsWithCodeAndKey)
{
    auto const &c = GetParam();
    auto const result = parse_params(test_action, c.input);
    ASSERT_FALSE(result.ok()) << "input: " << c.input;
    EXPECT_EQ(result.error->code, c.code) << "input: " << c.input;
    EXPECT_EQ(result.error->key, c.key) << "input: " << c.input;
    EXPECT_FALSE(result.error->message.empty()) << "input: " << c.input;
}

INSTANTIATE_TEST_SUITE_P(Grammar, RejectTest, ::testing::ValuesIn(reject_cases));

ParseResult parse_ok(std::string_view input)
{
    auto result = parse_params(test_action, input);
    EXPECT_TRUE(result.ok()) << "input: " << input << " error: "
                             << (result.error ? result.error->code + " " + result.error->key : "");
    return result;
}

} // namespace

TEST(VacardsCliParams, MinimalInputAppliesDefaults)
{
    auto const r = parse_ok("distance=1");
    ASSERT_TRUE(r.ok());
    EXPECT_DOUBLE_EQ(r.at("distance").number, 1.0);
    EXPECT_FALSE(r.at("distance").from_default);
    // defaults are parsed like user input and marked
    EXPECT_TRUE(r.at("angle").from_default);
    EXPECT_DOUBLE_EQ(r.at("angle").number, 15.0);
    EXPECT_TRUE(r.at("time").from_default);
    EXPECT_DOUBLE_EQ(r.at("time").number, 5000.0);
    EXPECT_EQ(r.at("direction").text, "outward");
    EXPECT_EQ(r.at("direction").raw, "outward");
    // optional without default: absent
    EXPECT_FALSE(r.has("flag"));
    EXPECT_FALSE(r.has("count"));
    EXPECT_FALSE(r.has("name"));
    EXPECT_FALSE(r.has("ids"));
    EXPECT_EQ(r.values.size(), 4u);
    ASSERT_EQ(r.given.size(), 1u);
    EXPECT_EQ(r.given[0].first, "distance");
    EXPECT_EQ(r.given[0].second, "1");
}

TEST(VacardsCliParams, LengthUnitsConvertToCssPx)
{
    struct { std::string_view input; double px; } const cases[] = {
        {"distance=2", 2.0},
        {"distance=2px", 2.0},
        {"distance=25.4mm", 96.0},
        {"distance=2.54cm", 96.0},
        {"distance=1in", 96.0},
        {"distance=72pt", 96.0},
        {"distance=6pc", 96.0},
        {"distance=-0.5mm", -0.5 * 96.0 / 25.4},
        {"distance=+3px", 3.0},
        {"distance=.5in", 48.0},
        {"distance=1e1px", 10.0},
        {"distance=1000", 1000.0},
        {"distance=-1000", -1000.0},
    };
    for (auto const &c : cases) {
        auto const r = parse_ok(c.input);
        ASSERT_TRUE(r.ok()) << c.input;
        EXPECT_NEAR(r.at("distance").number, c.px, 1e-9) << c.input;
        EXPECT_EQ(r.at("distance").type, ParamType::Length);
    }
}

TEST(VacardsCliParams, ScalarTypes)
{
    auto const r = parse_ok("distance=1,flag,count=10,ratio=-1,angle=360deg,time=250ms,direction=both");
    ASSERT_TRUE(r.ok());
    EXPECT_TRUE(r.at("flag").boolean);
    EXPECT_EQ(r.at("flag").raw, "true");
    EXPECT_EQ(r.at("count").integer, 10);
    EXPECT_DOUBLE_EQ(r.at("ratio").number, -1.0);
    EXPECT_DOUBLE_EQ(r.at("angle").number, 360.0);
    EXPECT_FALSE(r.at("angle").from_default);
    EXPECT_DOUBLE_EQ(r.at("time").number, 250.0);
    EXPECT_EQ(r.at("direction").text, "both");
    ASSERT_EQ(r.given.size(), 7u);
    EXPECT_EQ(r.given[1].first, "flag");
    EXPECT_EQ(r.given[1].second, "true");

    struct { std::string_view input; bool value; } const bools[] = {
        {"distance=1,flag=true", true}, {"distance=1,flag=1", true},
        {"distance=1,flag=false", false}, {"distance=1,flag=0", false},
    };
    for (auto const &c : bools) {
        auto const b = parse_ok(c.input);
        ASSERT_TRUE(b.ok()) << c.input;
        EXPECT_EQ(b.at("flag").boolean, c.value) << c.input;
    }

    EXPECT_DOUBLE_EQ(parse_ok("distance=1,time=2m").at("time").number, 120000.0);
    EXPECT_DOUBLE_EQ(parse_ok("distance=1,time=0.1s").at("time").number, 100.0);
    EXPECT_DOUBLE_EQ(parse_ok("distance=1,time=10m").at("time").number, 600000.0);
    EXPECT_EQ(parse_ok("distance=1,count=+0").at("count").integer, 0);
    EXPECT_EQ(parse_ok("distance=1,count=007").at("count").integer, 7);
    EXPECT_DOUBLE_EQ(parse_ok("distance=1,ratio=1.").at("ratio").number, 1.0);
    EXPECT_DOUBLE_EQ(parse_ok("distance=1,ratio=-1E-1").at("ratio").number, -0.1);
}

TEST(VacardsCliParams, PercentDecodingAndLists)
{
    auto const r = parse_ok("distance=1,name=a%2Cb%3Dc%7Cd%3Be%25f%3a%3A,ids=r1|group%7C2|%E2%9C%93");
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.at("name").text, "a,b=c|d;e%f::");
    EXPECT_EQ(r.at("name").raw, "a,b=c|d;e%f::");
    std::vector<std::string> const ids = {"r1", "group|2", "\xE2\x9C\x93"};
    EXPECT_EQ(r.at("ids").items, ids);
    // Colons and spaces need no encoding in text.
    EXPECT_EQ(parse_ok("distance=1,name=C:/tmp/a b.svg").at("name").text, "C:/tmp/a b.svg");
    EXPECT_EQ(parse_ok("distance=1,ids=single").at("ids").items, std::vector<std::string>{"single"});
    // only the first '=' separates key and value
    EXPECT_EQ(parse_ok("distance=1,name=a=b").at("name").text, "a=b");
    // given keeps decoded values
    auto const g = parse_ok("distance=1,name=x%2Cy");
    ASSERT_EQ(g.given.size(), 2u);
    EXPECT_EQ(g.given[1].second, "x,y");
}

TEST(VacardsCliParams, PercentDecodeHelper)
{
    EXPECT_EQ(percent_decode(""), std::optional<std::string>(""));
    EXPECT_EQ(percent_decode("abc"), std::optional<std::string>("abc"));
    EXPECT_EQ(percent_decode("%41%62"), std::optional<std::string>("Ab"));
    EXPECT_EQ(percent_decode("%e2%9c%93"), std::optional<std::string>("\xE2\x9C\x93"));
    EXPECT_EQ(percent_decode("100%"), std::nullopt);
    EXPECT_EQ(percent_decode("%4"), std::nullopt);
    EXPECT_EQ(percent_decode("%G1"), std::nullopt);
}

TEST(VacardsCliParams, NoParamsAction)
{
    EXPECT_TRUE(parse_params(no_params_action, "").ok());
    auto const r = parse_params(no_params_action, "x=1");
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error->code, "unknown-key");
    EXPECT_EQ(r.error->key, "x");
}

TEST(VacardsCliParams, BadDefaultIsReported)
{
    auto const r = parse_params(bad_default_action, "");
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error->code, "bad-default");
    EXPECT_EQ(r.error->key, "count");
}

TEST(VacardsCliParams, MessageNamesKeyAndAction)
{
    auto const r = parse_params(test_action, "distance=1,count=11");
    ASSERT_FALSE(r.ok());
    EXPECT_NE(r.error->message.find("count"), std::string::npos) << r.error->message;
    EXPECT_NE(r.error->message.find("vacards-test"), std::string::npos) << r.error->message;
}

TEST(VacardsCliParams, TypeNamesAndUnits)
{
    EXPECT_EQ(type_name(ParamType::Boolean), "boolean");
    EXPECT_EQ(type_name(ParamType::Integer), "integer");
    EXPECT_EQ(type_name(ParamType::Number), "number");
    EXPECT_EQ(type_name(ParamType::Length), "length");
    EXPECT_EQ(type_name(ParamType::Angle), "angle");
    EXPECT_EQ(type_name(ParamType::Duration), "duration");
    EXPECT_EQ(type_name(ParamType::Choice), "choice");
    EXPECT_EQ(type_name(ParamType::Text), "text");
    EXPECT_EQ(type_name(ParamType::List), "list");
    EXPECT_EQ(stored_unit(ParamType::Length), "px");
    EXPECT_EQ(stored_unit(ParamType::Angle), "deg");
    EXPECT_EQ(stored_unit(ParamType::Duration), "ms");
    EXPECT_EQ(stored_unit(ParamType::Number), "");
    EXPECT_EQ(stored_unit(ParamType::Text), "");
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
