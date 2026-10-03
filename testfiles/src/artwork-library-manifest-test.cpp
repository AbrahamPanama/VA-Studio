// SPDX-License-Identifier: GPL-2.0-or-later
#include <gtest/gtest.h>

#include "io/artwork-library-manifest.h"
#include <chrono>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <random>
#include <stdexcept>

using namespace Inkscape::IO::ArtworkLibrary;

namespace {
constexpr auto library_id = "77bcd302-a034-4b99-9270-139e70273123";
constexpr auto asset_id = "b203e8e9-640c-4195-b0ea-f9b790245678";

Manifest specimen()
{
    Manifest value;
    value.id = library_id;
    value.name = "Invitaciones / María";
    value.revision = 3;
    Asset asset;
    asset.id = asset_id;
    asset.name = "Marco – bodas 🌸";
    asset.path = "assets/" + asset.id + ".svg";
    asset.sha256 = std::string(64, 'a');
    asset.width_mm = 97.875;
    asset.height_mm = 54.271;
    asset.tags = {"boda", "corte", "中文"};
    asset.extra_json = R"({"sourceFormat":"lbart","importDiagnostics":["synthetic test"],"custom":{"retained":true}})";
    value.assets.push_back(asset);
    value.extra_json = R"({"createdBy":"fixture","optionalMetadata":{"language":"es"}})";
    return value;
}

std::string replace_once(std::string value, std::string const &from, std::string const &to)
{
    auto pos = value.find(from);
    if (pos == std::string::npos) throw std::runtime_error("Broken test mutation");
    value.replace(pos, from.size(), to);
    return value;
}

std::string replace_number(std::string value, std::string const &key, std::string const &replacement)
{
    auto marker = "\"" + key + "\":";
    auto start = value.find(marker);
    if (start == std::string::npos) throw std::runtime_error("Missing numeric test field");
    start += marker.size();
    auto end = value.find_first_of(",}", start);
    if (end == std::string::npos) throw std::runtime_error("Unterminated numeric test field");
    value.replace(start, end - start, replacement);
    return value;
}

TEST(ArtworkLibraryManifestTest, RoundTripPreservesUnicodePhysicalSizeAndOptionalMetadata)
{
    auto input = specimen();
    auto serialized = input.serialize();
    auto result = Manifest::parse(serialized);
    ASSERT_EQ(result.assets.size(), 1);
    EXPECT_EQ(result.id, input.id);
    EXPECT_EQ(result.name, input.name);
    EXPECT_EQ(result.revision, input.revision);
    EXPECT_EQ(result.extra_json, input.extra_json);
    EXPECT_EQ(result.assets[0].extra_json, input.assets[0].extra_json);
    EXPECT_EQ(result.assets[0].tags, input.assets[0].tags);
    EXPECT_EQ(result.assets[0].name, input.assets[0].name);
    EXPECT_DOUBLE_EQ(result.assets[0].width_mm, 97.875);
    EXPECT_DOUBLE_EQ(result.assets[0].height_mm, 54.271);
    EXPECT_EQ(result.serialize(), serialized);
}

TEST(ArtworkLibraryManifestTest, EmptyLibraryAndFullWidthRevisionRemainValid)
{
    auto input = specimen();
    input.assets.clear();
    input.revision = std::numeric_limits<std::uint64_t>::max();
    auto result = Manifest::parse(input.serialize());
    EXPECT_TRUE(result.assets.empty());
    EXPECT_EQ(result.revision, input.revision);
}

TEST(ArtworkLibraryManifestTest, RejectsWrongFormatFutureVersionAndWrongScalarTypes)
{
    auto input = specimen().serialize();
    for (auto const &bad : {
             replace_once(input, "org.vacards.artwork-library", "renamed.lbart"),
             replace_once(input, "\"formatVersion\":1", "\"formatVersion\":2"),
             replace_once(input, "\"formatVersion\":1", "\"formatVersion\":1.0"),
             replace_once(input, "\"revision\":3", "\"revision\":-1"),
             replace_once(input, "\"revision\":3", "\"revision\":\"3\""),
             replace_number(input, "widthMm", "\"97.875\"")}) {
        EXPECT_THROW(Manifest::parse(bad), std::runtime_error);
    }
}

TEST(ArtworkLibraryManifestTest, RejectsDuplicateMembersIncludingDecodedEscapesAndNestedMetadata)
{
    auto input = specimen().serialize();
    EXPECT_THROW(Manifest::parse(replace_once(input, "\"revision\":3", "\"revision\":3,\"revision\":4")), std::runtime_error);
    EXPECT_THROW(Manifest::parse(replace_once(input, "\"revision\":3", "\"revision\":3,\"revis\\u0069on\":4")), std::runtime_error);
    EXPECT_THROW(Manifest::parse(replace_once(input, "\"retained\":true", "\"retained\":true,\"retained\":false")), std::runtime_error);
}

TEST(ArtworkLibraryManifestTest, RejectsMalformedUtf8SurrogatesTrailingDataAndNonStandardJson)
{
    auto input = specimen().serialize();
    for (auto const &bad : {input + "{}", input.substr(0, input.size() - 1),
                           std::string("/* comment */") + input,
                           replace_number(input, "widthMm", "NaN"),
                           replace_once(input, "fixture", "\\uD800"),
                           replace_once(input, "fixture", std::string(1, char(0xff)))}) {
        EXPECT_THROW(Manifest::parse(bad), std::runtime_error);
    }
    EXPECT_EQ(Manifest::parse(" \n" + input + "\r\n").id, library_id);
}

TEST(ArtworkLibraryManifestTest, RejectsInvalidDimensionsPathsHashesAndDuplicateIds)
{
    auto original = specimen();
    for (double size : {0.0, -1.0, 1e10, std::numeric_limits<double>::infinity(),
                        std::numeric_limits<double>::quiet_NaN()}) {
        auto input = original;
        input.assets[0].width_mm = size;
        EXPECT_THROW(input.serialize(), std::runtime_error);
    }
    for (auto const &path : {"../escape.svg", "assets/other.svg", "C:/outside.svg"}) {
        auto input = original;
        input.assets[0].path = path;
        EXPECT_THROW(input.serialize(), std::runtime_error);
    }
    auto input = original;
    input.assets[0].sha256[0] = 'G';
    EXPECT_THROW(input.serialize(), std::runtime_error);
    input = original;
    input.assets.push_back(input.assets.front());
    EXPECT_THROW(input.serialize(), std::runtime_error);
    EXPECT_FALSE(valid_library_uuid("../b203e8e9-640c-4195-b0ea-f9b790245678"));
    EXPECT_FALSE(valid_library_uuid("B203E8E9-640C-4195-B0EA-F9B790245678"));
}

TEST(ArtworkLibraryManifestTest, NamesAreMetadataButMustBeBoundedPrintableUtf8)
{
    auto input = specimen();
    input.assets[0].name = "../user visible name.svg";
    EXPECT_NO_THROW(input.serialize());
    for (auto const &name : {std::string{}, std::string("  "), std::string("bad\nlabel"),
                             std::string(1025, 'x'), std::string(1, char(0xff))}) {
        input.assets[0].name = name;
        EXPECT_THROW(input.serialize(), std::runtime_error);
    }
    input = specimen();
    input.assets[0].tags.push_back("boda");
    EXPECT_THROW(input.serialize(), std::runtime_error);
    input.assets[0].tags = std::vector<std::string>(33, "tag");
    EXPECT_THROW(input.serialize(), std::runtime_error);
}

TEST(ArtworkLibraryManifestTest, UnknownOptionalMetadataCannotShadowReservedFields)
{
    auto input = specimen();
    input.extra_json = R"({"formatVersion":2})";
    EXPECT_THROW(input.serialize(), std::runtime_error);
    input = specimen();
    input.assets[0].extra_json = R"({"path":"outside.svg"})";
    EXPECT_THROW(input.serialize(), std::runtime_error);
}

TEST(ArtworkLibraryManifestTest, EnforcesBytesAssetCountAndDepthBeforePublication)
{
    auto input = specimen();
    auto serialized = input.serialize();
    ManifestLimits small;
    small.bytes = 128;
    EXPECT_THROW(Manifest::parse(serialized, small), std::runtime_error);
    EXPECT_THROW(input.serialize(small), std::runtime_error);
    small = {};
    small.assets = 0;
    EXPECT_THROW(Manifest::parse(serialized, small), std::runtime_error);
    EXPECT_THROW(input.serialize(small), std::runtime_error);
    input.extra_json = "{\"deep\":" + std::string(17, '[') + "0" + std::string(17, ']') + "}";
    EXPECT_THROW(input.serialize(), std::runtime_error);
}

TEST(ArtworkLibraryManifestTest, CancellationDuringParsingAndSerializationDoesNotMutateInput)
{
    auto input = specimen();
    auto serialized = input.serialize();
    unsigned calls = 0;
    EXPECT_THROW(Manifest::parse(serialized, {}, [&] { return ++calls > 10; }), std::runtime_error);
    calls = 0;
    EXPECT_THROW(input.serialize({}, [&] { return ++calls > 10; }), std::runtime_error);
    EXPECT_EQ(input.serialize(), serialized);
}

TEST(ArtworkLibraryManifestTest, ExactSerializedByteLimitAcceptsZeroOneAndMultipleAssets)
{
    for (unsigned count : {0u, 1u, 2u}) {
        auto input = specimen();
        input.assets.resize(count, specimen().assets.front());
        if (count == 2) {
            input.assets[1].id = library_id;
            input.assets[1].path = "assets/" + input.assets[1].id + ".svg";
        }
        auto serialized = input.serialize();
        ManifestLimits limit;
        limit.bytes = serialized.size();
        EXPECT_NO_THROW(Manifest::parse(serialized, limit));
        EXPECT_EQ(input.serialize(limit), serialized);
        --limit.bytes;
        EXPECT_THROW(Manifest::parse(serialized, limit), std::runtime_error);
        EXPECT_THROW(input.serialize(limit), std::runtime_error);
    }
}

TEST(ArtworkLibraryManifestTest, UnsupportedNumericMetadataIsRejectedInsteadOfChanged)
{
    for (auto token : {"18446744073709551617", "1e-9999", "1e400", "0.10000000000000001"}) {
        auto input = specimen();
        auto extra = std::string("{\"number\":") + token + "}";
        input.extra_json = extra;
        EXPECT_THROW(input.serialize(), std::runtime_error) << token;
        input.extra_json = "{\"nested\":" + extra + "}";
        EXPECT_THROW(input.serialize(), std::runtime_error) << token;
        input.extra_json = "{}";
        input.assets[0].extra_json = extra;
        EXPECT_THROW(input.serialize(), std::runtime_error) << token;
        auto raw = replace_once(specimen().serialize(), "\"retained\":true", std::string("\"number\":") + token);
        EXPECT_THROW(Manifest::parse(raw), std::runtime_error) << token;
    }
    auto input = specimen();
    input.extra_json = R"({"exactInteger":18446744073709551615,"negativeInteger":-9223372036854775808,"fraction":0.1,"scientific":1e20,"tiny":5e-324,"largeId":"18446744073709551617"})";
    auto result = Manifest::parse(input.serialize());
    EXPECT_EQ(Manifest::parse(result.serialize()).serialize(), result.serialize());
    EXPECT_NE(result.extra_json.find("18446744073709551615"), std::string::npos);
    EXPECT_NE(result.extra_json.find("18446744073709551617"), std::string::npos);
}

TEST(ArtworkLibraryManifestTest, PhysicalDimensionsRoundTripWithoutLosingAnUlp)
{
    for (double value : {std::nextafter(1.0, 2.0), std::nextafter(97.875, 100.0),
                        std::nextafter(0.1, 0.0), std::nextafter(1e9, 0.0)}) {
        auto input = specimen();
        input.assets[0].width_mm = value;
        auto result = Manifest::parse(input.serialize());
        EXPECT_EQ(result.assets[0].width_mm, value);
    }
}

TEST(ArtworkLibraryManifestTest, TruncationsAndMutationsRemainBoundedOrRoundTrip)
{
    auto serialized = specimen().serialize();
    for (std::size_t length = 0; length < serialized.size(); ++length) {
        EXPECT_THROW(Manifest::parse(std::string_view(serialized).substr(0, length)), std::runtime_error);
    }
    std::mt19937 random(4711);
    for (unsigned trial = 0; trial < 1000; ++trial) {
        auto changed = serialized;
        changed[random() % changed.size()] = static_cast<char>(random() & 255);
        try {
            auto accepted = Manifest::parse(changed);
            auto stable = accepted.serialize();
            EXPECT_EQ(Manifest::parse(stable).serialize(), stable);
        } catch (std::runtime_error const &) {
            // Malformed or unsupported metadata is rejected, not partially read.
        }
    }
}

TEST(ArtworkLibraryManifestTest, HandlesTenThousandAssetsWithoutLoadingArtwork)
{
    auto input = specimen();
    auto prototype = input.assets.front();
    input.assets.clear();
    for (unsigned i = 0; i < 10000; ++i) {
        auto asset = prototype;
        std::ostringstream id;
        id.imbue(std::locale::classic()); // UUID hex digits must never acquire locale grouping.
        id << "b203e8e9-640c-4195-b0ea-" << std::hex << std::setw(12) << std::setfill('0') << i;
        asset.id = id.str();
        asset.path = "assets/" + asset.id + ".svg";
        input.assets.push_back(std::move(asset));
    }
    auto serialized = input.serialize();
    auto start = std::chrono::steady_clock::now();
    auto result = Manifest::parse(serialized);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    RecordProperty("manifest_10000_parse_ms", ms);
    ASSERT_EQ(result.assets.size(), 10000);
    EXPECT_EQ(result.assets.back().id, input.assets.back().id);
    input.assets.push_back(prototype);
    EXPECT_THROW(input.serialize(), std::runtime_error);
}
} // namespace
