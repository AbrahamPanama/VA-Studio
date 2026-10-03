// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>

#include "util-string/string-convert.h"

using namespace Inkscape;

TEST(StringConvertTest, RoundTripsUnicodeAcrossWideRepresentation)
{
    std::string const input = "VA caf\xC3\xA9s \xE2\x80\x94 \xE4\xB8\xAD\xE6\x96\x87 \xF0\x9F\x98\x80";
    auto const wide = utf8_to_wstring(input);
    EXPECT_EQ(wstring_to_utf8(wide.c_str()), input);
}

TEST(StringConvertTest, PreservesEmptyStrings)
{
    EXPECT_TRUE(utf8_to_wstring("").empty());
    EXPECT_TRUE(wstring_to_utf8(L"").empty());
}

TEST(StringConvertTest, RejectsMalformedUtf8)
{
    std::string const malformed{"\xC3\x28", 2};
    EXPECT_THROW(utf8_to_wstring(malformed), std::range_error);
}

TEST(StringConvertTest, RejectsInvalidWideCodePoint)
{
    std::wstring malformed;
    if constexpr (sizeof(wchar_t) == 4) {
        malformed.push_back(static_cast<wchar_t>(0x110000));
    } else {
        malformed.push_back(static_cast<wchar_t>(0xD800));
    }
    EXPECT_THROW(wstring_to_utf8(malformed.c_str()), std::range_error);
}

TEST(StringConvertTest, RejectsNullWideString)
{
    EXPECT_THROW(wstring_to_utf8(nullptr), std::invalid_argument);
}
