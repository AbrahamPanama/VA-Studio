// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * Authors:
 *   Rafał Siejakowski <rs@rs-math.net>
 *
 * @copyright
 * Copyright (C) 2025 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <glibmm/ustring.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <string>
#include <variant>
#include <vector>

#include "3rdparty/libcroco/src/cr-parser.h"
#include "3rdparty/libcroco/src/cr-term.h"
#include "attribute-rel-svg.h"
#include "css/syntactic-decomposition.h"
#include "util/delete-with.h"

/// Mock Glib::ustring
namespace Glib {
ustring::ustring(char const *c_string)
    : string_{c_string}
{}
ustring::~ustring() noexcept = default;
} // namespace Glib

struct MockStatics
{
    MOCK_CONST_METHOD1(m_isSVGElement, bool(std::string const &));

    MockStatics() { instance = this; }
    ~MockStatics() { instance = nullptr; }

    inline static MockStatics *instance = nullptr;
};

bool SPAttributeRelSVG::isSVGElement(Glib::ustring const &element)
{
    return MockStatics::instance->m_isSVGElement(element.raw());
}

namespace Inkscape::CSS {

using SimpleOutput = std::string;
using SelectorAndRule = std::pair<std::string, std::string>;

struct ParseCSSTestCase
{
    using ExpectedRepresentation = std::variant<SimpleOutput, SelectorAndRule>;
    std::string input_css;
    std::vector<ExpectedRepresentation> expected_repr;
};

/// Helper functions to check if the string representation in a test case matches the structured output
void representation_check(ParseCSSTestCase::ExpectedRepresentation const &rep, RuleStatement const &rule)
{
    auto *rule_display = std::get_if<SelectorAndRule>(&rep);
    ASSERT_TRUE(rule_display);
    EXPECT_EQ(rule_display->first, rule.selectors);
    EXPECT_EQ(rule_display->second, rule.rules);
}

void representation_check(ParseCSSTestCase::ExpectedRepresentation const &rep, BlockAtStatement const &block_at)
{
    auto *at_display = std::get_if<SelectorAndRule>(&rep);
    ASSERT_TRUE(at_display);
    EXPECT_EQ(at_display->first, block_at.at_statement);
}

void representation_check(ParseCSSTestCase::ExpectedRepresentation const &rep, OtherStatement const &other)
{
    auto *output = std::get_if<SimpleOutput>(&rep);
    ASSERT_TRUE(output);
    EXPECT_EQ(*output, other);
}

void representation_check(ParseCSSTestCase::ExpectedRepresentation const &rep, SyntacticElement const &e)
{
    return std::visit([&rep](auto const &element) { return representation_check(rep, element); }, e);
}

ParseCSSTestCase const parse_test_cases[] = {
    // Basic rules
    {.input_css = "text { color: red; }", .expected_repr = {SelectorAndRule{"text", "color: red;"}}},
    {.input_css = "* { color: red; }", .expected_repr = {SelectorAndRule{"*", "color: red;"}}},
    // Rule with comma-separated selector
    {.input_css = "text, circle { color: red; }", .expected_repr = {SelectorAndRule{"text, circle", "color: red;"}}},
    // Check that composite selectors work; insert some whitespace
    {.input_css = ".myclass .myother.foo {\n\t cx: 5; \n}",
     .expected_repr = {SelectorAndRule{".myclass.myother.foo", "cx: 5;"}}},
    // Check that comments are stripped; TODO: maybe show comments in the Selectors & CSS dialog?
    {.input_css = R"EOD(
circle { stroke: none; }
/* This is a CSS comment */
rect { fill: none; }
)EOD",
     .expected_repr =
         {
             SelectorAndRule{"circle", "stroke: none;"},
             SelectorAndRule{"rect", "fill: none;"},
         }},
    // Check that @media rules are parsed (note: the entire content of the block following the media rule
    // will be shown as "ruleset" due to a limitation of the Selectors & CSS dialog). TODO: remove the limitation.
    {.input_css = "@media print { rect { fill: green; } }",
     .expected_repr = {SelectorAndRule{"@media print", "rect { fill: green; }"}}},
    // @media rule followed by another rule
    {.input_css = R"EOD(
    @media print {
        rect { fill: green; }
    }
    circle { stroke: none; opacity: 90% }
    )EOD",
     .expected_repr = {SelectorAndRule{"@media print", "rect { fill: green; }"},
                       SelectorAndRule{"circle", "stroke: none; opacity: 90%;"}}},
// Example from https://gitlab.com/inkscape/inkscape/-/issues/3003 - this is still not handled properly by Libcroco
#if 0
    {.input_css = R"EOD("
    @import url(https://fonts.googleapis.com/css?family=UnifrakturCook:700);
                text {
                    font-family: UnifrakturCook;
                }
    )EOD",
     .expected_repr = {SimpleOutput("@import url(https://fonts.googleapis.com/css?family=UnifrakturCook:700);"),
                       SelectorAndRule("text", "font-family: UnifrakturCook;")}},
#endif
    // Legacy behaviour: "fix" non-SVG element selectors by making them classes
    {.input_css = "div { fill: none; }", .expected_repr = {SelectorAndRule{".div", "fill: none;"}}},
    // Check that @charset works
    {.input_css = "@charset 'UTF-8';", .expected_repr = {SimpleOutput{R"(@charset "UTF-8";)"}}},
};

using namespace ::testing;

struct ParseCSSTest : TestWithParam<ParseCSSTestCase>
{
    ParseCSSTest()
    {
        EXPECT_CALL(mock, m_isSVGElement(AnyOf(StrEq("text"), StrEq("circle"), StrEq("rect"))))
            .WillRepeatedly(Return(true));
        EXPECT_CALL(mock, m_isSVGElement(StrEq("div"))).WillRepeatedly(Return(false));
    }

    MockStatics mock;
};

TEST_P(ParseCSSTest, ParseCSSForDialogDisplay)
{
    auto const &test_case = GetParam();
    SyntacticDecomposition const decomposition{test_case.input_css};
    decomposition.for_each([&expected = test_case.expected_repr, pos = 0](auto const &element) mutable {
        ASSERT_LT(pos, expected.size());
        representation_check(expected[pos++], element);
    });
}

INSTANTIATE_TEST_SUITE_P(ParseCSSForDialogTests, ParseCSSTest, ::testing::ValuesIn(parse_test_cases));

namespace {

void check_css_file_parse(char const *directory_utf8, char const *filename_utf8)
{
    using Inkscape::Util::delete_with;

    struct TemporaryCssFile
    {
        std::string root;
        std::string directory;
        std::string file;
        bool directory_created = false;
        bool file_created = false;

        ~TemporaryCssFile()
        {
            // Remove only the exact paths owned by this fresh temporary fixture.
            if (file_created) EXPECT_EQ(g_remove(file.c_str()), 0) << file;
            if (directory_created) EXPECT_EQ(g_rmdir(directory.c_str()), 0) << directory;
            if (!root.empty()) EXPECT_EQ(g_rmdir(root.c_str()), 0) << root;
        }
    } temporary;

    auto root = delete_with<g_free>(g_dir_make_tmp("inkscape-css-file-test-XXXXXX", nullptr));
    ASSERT_TRUE(root);
    temporary.root = root.get();

    // GLib filenames are UTF-8 on Windows and platform-native on Unix.
    auto directory = delete_with<g_free>(g_filename_from_utf8(directory_utf8, -1, nullptr, nullptr, nullptr));
    auto filename = delete_with<g_free>(g_filename_from_utf8(filename_utf8, -1, nullptr, nullptr, nullptr));
    ASSERT_TRUE(directory);
    ASSERT_TRUE(filename);
    auto path = delete_with<g_free>(g_build_filename(temporary.root.c_str(), directory.get(), nullptr));
    temporary.directory = path.get();
    ASSERT_EQ(g_mkdir(temporary.directory.c_str(), 0700), 0) << temporary.directory;
    temporary.directory_created = true;
    path = delete_with<g_free>(g_build_filename(temporary.directory.c_str(), filename.get(), nullptr));
    temporary.file = path.get();

    // Keep CRLF bytes in the fixture: the production fix must preserve text-mode parsing.
    char const css[] = "circle {\r\n  fill: red;\r\n  stroke: none;\r\n}\r\n";
    ASSERT_TRUE(g_file_set_contents(temporary.file.c_str(), css, sizeof(css) - 1, nullptr));
    temporary.file_created = true;

    using Properties = std::vector<std::pair<std::string, std::string>>;
    Properties properties;
    auto handler = delete_with<cr_doc_handler_unref>(cr_doc_handler_new());
    ASSERT_TRUE(handler);
    handler->app_data = &properties;
    handler->property = +[](CRDocHandler *sac, CRString *name, CRTerm *expression, gboolean important) {
        ASSERT_NE(sac, nullptr);
        ASSERT_NE(sac->app_data, nullptr);
        ASSERT_NE(name, nullptr);
        ASSERT_NE(name->stryng, nullptr);
        ASSERT_NE(name->stryng->str, nullptr);
        ASSERT_NE(expression, nullptr);
        auto value = delete_with<g_free>(cr_term_to_string(expression));
        ASSERT_TRUE(value);
        EXPECT_FALSE(important);
        static_cast<Properties *>(sac->app_data)->emplace_back(
            name->stryng->str, reinterpret_cast<char const *>(value.get()));
    };

    auto parser = delete_with<cr_parser_destroy>(cr_parser_new_from_file(
        reinterpret_cast<guchar const *>(temporary.file.c_str()), CR_UTF_8));
    ASSERT_TRUE(parser) << temporary.file;
    ASSERT_EQ(cr_parser_set_sac_handler(parser.get(), handler.get()), CR_OK);
    ASSERT_EQ(cr_parser_parse(parser.get()), CR_OK);
    EXPECT_THAT(properties, ElementsAre(std::make_pair(std::string("fill"), std::string("red")),
                                       std::make_pair(std::string("stroke"), std::string("none"))));
}

} // namespace

TEST(CssFileParserTest, ParsesPropertiesFromAsciiPathWithCrlf)
{
    check_css_file_parse("ascii parent", "ascii theme.css");
}

TEST(CssFileParserTest, ParsesPropertiesFromUnicodeParentAndFilenameWithCrlf)
{
    // UTF-8 for "Portable ñ 测试" and "thème-测试.css", independent of compiler source encoding.
    check_css_file_parse("Portable \xc3\xb1 \xe6\xb5\x8b\xe8\xaf\x95",
                         "th\xc3\xa8me-\xe6\xb5\x8b\xe8\xaf\x95.css");
}

} // namespace Inkscape::CSS
