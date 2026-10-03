/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. See http://mozilla.org/MPL/2.0/. */
#include <cppunit/TestFixture.h>
#include <cppunit/extensions/HelperMacros.h>
#include "CDRInternalStream.h"
#include "CDRParser.h"
#include "CDRStylesCollector.h"

namespace test
{
namespace
{
typedef std::vector<unsigned char> Bytes;

void put32(Bytes &b, size_t pos, unsigned value)
{
  for (unsigned i = 0; i < 4; ++i) b.at(pos + i) = (value >> (8 * i)) & 255;
}

void chunk(Bytes &b, const char *tag, const Bytes &data)
{
  size_t start = b.size(); b.resize(start + 8);
  std::copy(tag, tag + 4, b.begin() + start);
  put32(b, start + 4, unsigned(data.size()));
  b.insert(b.end(), data.begin(), data.end());
}

Bytes fromHex(const char *hex)
{
  Bytes b;
  for (; hex[0] && hex[1]; hex += 2)
  {
    auto nibble = [](char c) -> unsigned
    { return c <= '9' ? unsigned(c - '0') : unsigned(c - 'a' + 10); };
    b.push_back(static_cast<unsigned char>((nibble(hex[0]) << 4) | nibble(hex[1])));
  }
  return b;
}

Bytes record2023()
{
  return fromHex(
    "01000000600900001000000005df235a625cda4f9528d6daac4d37e3140500004d0000000100140500003b000000010c00000011000500000000002c242c000710000000000000000000000000000000000000000000000000000000000000000040a5ae023c00000000000000000000000000000000000000");
}

Bytes record2021()
{
  return fromHex(
    "01000000140500004d0000000100140500003b000000010c00000005000500000000000000000007100000000000000000000000000000000000000000000000000000e4770000000040a5ae023c00000000000000000000000000000000000000");
}

struct Fill
{
  unsigned id;
  unsigned short type;
  unsigned short model;
  unsigned value;
};

class Collector : public libcdr::CDRStylesCollector
{
public:
  explicit Collector(libcdr::CDRParserState &state) : CDRStylesCollector(state) {}
  std::vector<Fill> fills;
  void collectFillStyle(unsigned id, const libcdr::CDRFillStyle &style) override
  { fills.push_back(Fill{id, style.fillType, style.color1.m_colorModel, style.color1.m_colorValue}); }
};

std::vector<Fill> parse(unsigned version, const Bytes &record, bool expectSuccess = true)
{
  Bytes records;
  Bytes ver(2); ver[0] = version & 255; ver[1] = version >> 8;
  chunk(records, "vrsn", ver);
  chunk(records, "fild", record);
  libcdr::CDRInternalStream input(records);
  libcdr::CDRParserState state;
  Collector collector(state);
  const std::vector<std::unique_ptr<librevenge::RVNGInputStream>> external;
  libcdr::CDRParser parser(external, &collector);
  const bool success = parser.parseRecords(&input);
  if (expectSuccess) CPPUNIT_ASSERT(success);
  return collector.fills;
}

Fill onlySolid(unsigned version, const Bytes &record)
{
  const auto fills = parse(version, record);
  CPPUNIT_ASSERT_EQUAL(size_t(1), fills.size());
  CPPUNIT_ASSERT_EQUAL(1U, fills[0].id);
  CPPUNIT_ASSERT_EQUAL(static_cast<unsigned short>(1), fills[0].type);
  return fills[0];
}

void sameColor(const Fill &a, const Fill &b)
{
  CPPUNIT_ASSERT_EQUAL(a.model, b.model);
  CPPUNIT_ASSERT_EQUAL(a.value, b.value);
}
}

class CDRFillRecordTest : public CPPUNIT_NS::TestFixture
{
  CPPUNIT_TEST_SUITE(CDRFillRecordTest);
  CPPUNIT_TEST(test2023Header);
  CPPUNIT_TEST(test2021Header);
  CPPUNIT_TEST(test2026Header);
  CPPUNIT_TEST(testWrongBlockLength);
  CPPUNIT_TEST(testTruncatedIdentifier);
  CPPUNIT_TEST_SUITE_END();

  void test2023Header()
  {
    const Bytes newer = record2023();
    CPPUNIT_ASSERT_EQUAL(size_t(121), newer.size());
    Bytes older(newer.begin(), newer.begin() + 4);
    older.insert(older.end(), newer.begin() + 28, newer.end());
    sameColor(onlySolid(2510, newer), onlySolid(2100, older));
  }

  void test2021Header()
  {
    const Bytes older = record2021();
    CPPUNIT_ASSERT_EQUAL(size_t(97), older.size());
    onlySolid(2100, older);
  }

  void test2026Header()
  { sameColor(onlySolid(2700, record2023()), onlySolid(2510, record2023())); }

  void testWrongBlockLength()
  {
    Bytes bad = record2023();
    put32(bad, 8, 15);
    parse(2510, bad);
  }

  void testTruncatedIdentifier()
  {
    Bytes truncated = record2023();
    truncated.resize(28);
    parse(2510, truncated, false);
  }
};
CPPUNIT_TEST_SUITE_REGISTRATION(CDRFillRecordTest);
}
