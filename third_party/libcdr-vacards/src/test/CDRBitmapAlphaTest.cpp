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
Bytes bitmap()
{
  // Two RGB pixels (including genuine black), padded to an 8-byte row.
  Bytes b(54 + 64 + 8 + 78 + 4, 0);
  put32(b, 0, 7);
  put32(b, 54, 1); put32(b, 62, 2); put32(b, 66, 1);
  put32(b, 74, 24); put32(b, 78, 8); put32(b, 82, 8);
  b[121] = 20; b[122] = 30; b[123] = 40;
  const size_t a = 126;
  b[a] = 'R'; b[a + 1] = 'I'; put32(b, a + 2, 82);
  put32(b, a + 10, 78); put32(b, a + 14, 99);
  put32(b, a + 22, 2); put32(b, a + 26, 1);
  put32(b, a + 34, 8); put32(b, a + 38, 4); put32(b, a + 42, 4);
  b[a + 78] = 0; b[a + 79] = 128;
  return b;
}
class Collector : public libcdr::CDRStylesCollector
{
public:
  explicit Collector(libcdr::CDRParserState &state) : CDRStylesCollector(state) {}
  unsigned calls = 0;
  Bytes color, alpha;
  void collectBmp(unsigned, unsigned, unsigned, unsigned, unsigned,
                  const std::vector<unsigned> &, const Bytes &pixels) override
  { ++calls; color = pixels; alpha.clear(); }
  void collectBmp(unsigned, unsigned, unsigned, unsigned, unsigned,
                  const std::vector<unsigned> &, const Bytes &pixels,
                  unsigned, unsigned, unsigned, const Bytes &mask) override
  { ++calls; color = pixels; alpha = mask; }
};
void check(unsigned version, const Bytes &bmp, bool expectedAlpha)
{
  Bytes records;
  Bytes ver(2); ver[0] = version & 255; ver[1] = version >> 8;
  chunk(records, "vrsn", ver);
  chunk(records, "bmp ", bmp);
  // Following data must not be consumed as missing alpha payload.
  chunk(records, "junk", Bytes(100, 255));
  libcdr::CDRInternalStream input(records);
  libcdr::CDRParserState state;
  Collector collector(state);
  const std::vector<std::unique_ptr<librevenge::RVNGInputStream>> external;
  libcdr::CDRParser parser(external, &collector);
  CPPUNIT_ASSERT(parser.parseRecords(&input));
  CPPUNIT_ASSERT_EQUAL(1U, collector.calls);
  CPPUNIT_ASSERT(collector.color == Bytes(bmp.begin() + 118, bmp.begin() + 126));
  CPPUNIT_ASSERT_EQUAL(expectedAlpha, !collector.alpha.empty());
  if (expectedAlpha) CPPUNIT_ASSERT(collector.alpha == Bytes({0, 128, 0, 0}));
}
}

class CDRBitmapAlphaTest : public CPPUNIT_NS::TestFixture
{
  CPPUNIT_TEST_SUITE(CDRBitmapAlphaTest);
  CPPUNIT_TEST(testVersions20Through29);
  CPPUNIT_TEST(testOlderVersionUnchanged);
  CPPUNIT_TEST(testOpaqueBitmap);
  CPPUNIT_TEST(testInvalidSignature);
  CPPUNIT_TEST(testInvalidPlane);
  CPPUNIT_TEST(testTruncatedRecord);
  CPPUNIT_TEST(testInvalidLengths);
  CPPUNIT_TEST_SUITE_END();

  void testVersions20Through29()
  { for (unsigned v = 2000; v <= 2900; v += 100) check(v, bitmap(), true); }
  void testOlderVersionUnchanged() { check(1900, bitmap(), false); }
  void testOpaqueBitmap() { auto b = bitmap(); b.resize(126); check(2000, b, false); }
  void testInvalidSignature() { auto b = bitmap(); b[126] = 'X'; check(2600, b, false); }
  void testInvalidPlane()
  {
    for (const auto field : {14, 22, 26, 34, 38})
    { auto b = bitmap(); put32(b, 126 + field, 0); check(2000, b, false); }
  }
  void testTruncatedRecord()
  { auto b = bitmap(); b.resize(b.size() - 2); check(2600, b, false); }
  void testInvalidLengths()
  {
    for (const unsigned len : {0U, 77U, 81U, 83U, 0xffffffffU})
    { auto b = bitmap(); put32(b, 128, len); check(2700, b, false); }
    auto b = bitmap(); put32(b, 168, 3); check(2000, b, false);
  }
};
CPPUNIT_TEST_SUITE_REGISTRATION(CDRBitmapAlphaTest);
}
