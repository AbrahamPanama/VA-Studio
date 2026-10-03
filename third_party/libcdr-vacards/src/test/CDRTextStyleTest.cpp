/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. See http://mozilla.org/MPL/2.0/. */

// Focused reader tests for per-run font face, weight and italic preservation.
//
// They cover three independent layers:
//   1. CDRStyle::overrideStyle inheritance/reset semantics.
//   2. CDRStylesCollector::collectText resolving a paragraph style plus run
//      overrides into per-run styles.
//   3. CDRContentCollector emitting the standard span properties
//      style:font-name, fo:font-size, fo:color, fo:font-weight and
//      fo:font-style for independently constructed run records.
//   4. CDRParser::_readX6StyleString on a hand-built v27 txsm stream carrying
//      the independent source JSON shape (character.latin.weight/italic).

#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <cppunit/TestFixture.h>
#include <cppunit/extensions/HelperMacros.h>

#include <librevenge/librevenge.h>

#include "CDRContentCollector.h"
#include "CDRInternalStream.h"
#include "CDRParser.h"
#include "CDRStylesCollector.h"

namespace
{

typedef std::vector<unsigned char> Bytes;

// The two raw CorelDRAW weight values proven by the independent source oracle.
const int kWeightNormal = libcdr::CDR_FONT_WEIGHT_NORMAL;
const int kWeightBold = libcdr::CDR_FONT_WEIGHT_BOLD;

void put16(Bytes &b, size_t pos, unsigned value)
{
  for (unsigned i = 0; i < 2; ++i) b.at(pos + i) = (value >> (8 * i)) & 255;
}

void put32(Bytes &b, size_t pos, unsigned value)
{
  for (unsigned i = 0; i < 4; ++i) b.at(pos + i) = (value >> (8 * i)) & 255;
}

void put64(Bytes &b, size_t pos, unsigned long long value)
{
  for (unsigned i = 0; i < 8; ++i) b.at(pos + i) = (value >> (8 * i)) & 255;
}

void chunk(Bytes &b, const char *tag, const Bytes &data)
{
  size_t start = b.size();
  b.resize(start + 8);
  std::copy(tag, tag + 4, b.begin() + start);
  put32(b, start + 4, unsigned(data.size()));
  b.insert(b.end(), data.begin(), data.end());
}

std::string stringValue(const librevenge::RVNGPropertyList &props, const char *name)
{
  if (!props[name]) return std::string();
  return std::string(props[name]->getStr().cstr());
}

bool hasProperty(const librevenge::RVNGPropertyList &props, const char *name)
{
  return props[name] != nullptr;
}

libcdr::CDRStyle styleOf(const char *font, double pointSize, int weight, int italic)
{
  libcdr::CDRStyle style;
  if (font) style.m_fontName = font;
  style.m_fontSize = pointSize / 72.0;
  style.m_fontWeight = weight;
  style.m_fontItalic = italic;
  return style;
}

// Records the text events the collector sends to the drawing interface.
class TextRecorder : public librevenge::RVNGDrawingInterface
{
public:
  std::vector<librevenge::RVNGPropertyList> startTextObjects;
  std::vector<librevenge::RVNGPropertyList> paragraphs;
  std::vector<librevenge::RVNGPropertyList> spans;
  std::vector<std::string> texts;
  // Ordered event trace: "startTextObject", "openParagraph",
  // "insertText:<text>", "lineBreak", "tab", "space". Proves the transport
  // order, including consecutive blank-line breaks.
  std::vector<std::string> events;

  void startDocument(const librevenge::RVNGPropertyList &) override {}
  void endDocument() override {}
  void setDocumentMetaData(const librevenge::RVNGPropertyList &) override {}
  void defineEmbeddedFont(const librevenge::RVNGPropertyList &) override {}
  void startPage(const librevenge::RVNGPropertyList &) override {}
  void endPage() override {}
  void startMasterPage(const librevenge::RVNGPropertyList &) override {}
  void endMasterPage() override {}
  void setStyle(const librevenge::RVNGPropertyList &) override {}
  void startLayer(const librevenge::RVNGPropertyList &) override {}
  void endLayer() override {}
  void startEmbeddedGraphics(const librevenge::RVNGPropertyList &) override {}
  void endEmbeddedGraphics() override {}
  void openGroup(const librevenge::RVNGPropertyList &) override {}
  void closeGroup() override {}
  void drawRectangle(const librevenge::RVNGPropertyList &) override {}
  void drawEllipse(const librevenge::RVNGPropertyList &) override {}
  void drawPolygon(const librevenge::RVNGPropertyList &) override {}
  void drawPolyline(const librevenge::RVNGPropertyList &) override {}
  void drawPath(const librevenge::RVNGPropertyList &) override {}
  void drawGraphicObject(const librevenge::RVNGPropertyList &) override {}
  void drawConnector(const librevenge::RVNGPropertyList &) override {}
  void startTextObject(const librevenge::RVNGPropertyList &p) override
  {
    startTextObjects.push_back(p);
    events.push_back("startTextObject");
  }
  void endTextObject() override {}
  void startTableObject(const librevenge::RVNGPropertyList &) override {}
  void openTableRow(const librevenge::RVNGPropertyList &) override {}
  void closeTableRow() override {}
  void openTableCell(const librevenge::RVNGPropertyList &) override {}
  void closeTableCell() override {}
  void insertCoveredTableCell(const librevenge::RVNGPropertyList &) override {}
  void endTableObject() override {}
  void openOrderedListLevel(const librevenge::RVNGPropertyList &) override {}
  void closeOrderedListLevel() override {}
  void openUnorderedListLevel(const librevenge::RVNGPropertyList &) override {}
  void closeUnorderedListLevel() override {}
  void openListElement(const librevenge::RVNGPropertyList &) override {}
  void closeListElement() override {}
  void defineParagraphStyle(const librevenge::RVNGPropertyList &) override {}
  void openParagraph(const librevenge::RVNGPropertyList &p) override
  {
    paragraphs.push_back(p);
    events.push_back("openParagraph");
  }
  void closeParagraph() override {}
  void defineCharacterStyle(const librevenge::RVNGPropertyList &) override {}
  void openSpan(const librevenge::RVNGPropertyList &propList) override { spans.push_back(propList); }
  void closeSpan() override {}
  void openLink(const librevenge::RVNGPropertyList &) override {}
  void closeLink() override {}
  void insertTab() override { events.push_back("tab"); }
  void insertSpace() override { events.push_back("space"); }
  void insertText(const librevenge::RVNGString &text) override
  {
    texts.push_back(std::string(text.cstr()));
    events.push_back("insertText:" + std::string(text.cstr()));
  }
  void insertLineBreak() override { events.push_back("lineBreak"); }
  void insertField(const librevenge::RVNGPropertyList &) override {}
};

// Run the collected text through the content collector and capture the spans.
void emitText(libcdr::CDRParserState &state, unsigned spnd,
              const std::vector<libcdr::CDRText> &runs, TextRecorder &rec)
{
  state.m_pages.push_back(libcdr::CDRPage(8.0, 11.0, 0.0, 0.0));
  libcdr::CDRTextLine line;
  for (const auto &run : runs)
    line.append(run);
  state.m_texts[spnd].push_back(line);
  libcdr::CDRContentCollector collector(state, &rec, true);
  collector.collectObject(1);
  collector.collectSpnd(spnd);
  collector.collectArtisticText(1.0, 1.0);
  collector.collectLevel(0);
}

// Run one text object through a shared collector, selecting the same parser
// callback CDRParser uses for the two source text kinds (artistic vs paragraph).
void appendTextObject(libcdr::CDRParserState &state, libcdr::CDRContentCollector &collector,
                      unsigned spnd, const std::vector<libcdr::CDRText> &runs, bool paragraph)
{
  libcdr::CDRTextLine line;
  for (const auto &run : runs)
    line.append(run);
  state.m_texts[spnd].push_back(line);
  collector.collectObject(1);
  collector.collectSpnd(spnd);
  if (paragraph)
    collector.collectParagraphText(0.0, 0.0, 4.0, 2.0);
  else
    collector.collectArtisticText(1.0, 1.0);
  collector.collectLevel(0);
}

// Contract 3.2 envelope: a fully classified text object carries version=1,
// content-kind=text, the source object id and the proven kind.
void assertTextEnvelope(const librevenge::RVNGPropertyList &p, int objectId, const char *textKind)
{
  CPPUNIT_ASSERT(p["vacards:cdr-fidelity-version"]);
  CPPUNIT_ASSERT_EQUAL(1, p["vacards:cdr-fidelity-version"]->getInt());
  CPPUNIT_ASSERT(p["vacards:cdr-object-id"]);
  CPPUNIT_ASSERT_EQUAL(objectId, p["vacards:cdr-object-id"]->getInt());
  CPPUNIT_ASSERT_EQUAL(std::string("text"), stringValue(p, "vacards:cdr-content-kind"));
  CPPUNIT_ASSERT_EQUAL(std::string(textKind), stringValue(p, "vacards:cdr-text-kind"));
}

// Build a v27 txsm record (m_version 2000, so readTxsm16). Each paragraph has
// one run: the default JSON resolves the paragraph-level style and the optional
// record JSON resolves the run override.
Bytes txsmRecord(const std::string &defaultJson, const std::string &runJson)
{
  Bytes b;
  const size_t headerStart = 0;
  b.resize(41, 0); // frameFlag + 37 reserved bytes
  put32(b, headerStart, 0); // frameFlag = 0

  size_t p = 41;
  b.resize(p + 4, 0);
  put32(b, p, 1); p += 4; // numFrames

  b.resize(p + 4, 0);
  put32(b, p, 7); p += 4; // frameId == textId
  b.resize(p + 48, 0); p += 48;
  b.resize(p + 4, 0);
  put32(b, p, 0); p += 4; // textOnPath = 0
  b.resize(p + 8, 0); p += 8;
  // frameFlag == 0 branch
  b.resize(p + 16, 0); p += 16;
  b.resize(p + 4, 0);
  put32(b, p, 0); p += 4; // tlen = 0

  b.resize(p + 4, 0);
  put32(b, p, 1); p += 4; // numPara

  b.resize(p + 4, 0);
  put32(b, p, 0); p += 4; // stlId = 0 (no binary paragraph style)
  b.resize(p + 1, 0); p += 1;

  b.resize(p + 4, 0);
  put32(b, p, unsigned(defaultJson.size())); p += 4;
  b.insert(b.end(), defaultJson.begin(), defaultJson.end()); p += defaultJson.size();

  b.resize(p + 4, 0);
  put32(b, p, 1); p += 4; // numRecords

  b.resize(p + 2, 0); p += 2; // record reserved
  b.resize(p + 2, 0);
  put16(b, p, 1); p += 2; // stFlag1 != 0: this record carries a run JSON
  b.resize(p + 2, 0);
  put16(b, p, 0); p += 2; // stFlag2
  b.resize(p + 4, 0);
  put32(b, p, unsigned(runJson.size())); p += 4;
  b.insert(b.end(), runJson.begin(), runJson.end()); p += runJson.size();

  b.resize(p + 4, 0);
  put32(b, p, 1); p += 4; // numChars
  b.resize(p + 8, 0);
  put64(b, p, 0); p += 8; // char description 0: run 0

  b.resize(p + 4, 0);
  put32(b, p, 1); p += 4; // numBytes
  b.push_back('A'); p += 1; // text
  b.push_back(0); // trailing 0 skipped by readTxsm16

  Bytes records;
  Bytes ver(2);
  ver[0] = 2000 & 255;
  ver[1] = 2000 >> 8;
  chunk(records, "vrsn", ver);
  chunk(records, "txsm", b);
  return records;
}

// Multi-run variant of txsmRecord: one paragraph with a paragraph/default JSON
// style and one JSON record per run. Each run i is addressed by the logical
// char descriptor value 2*i (the even key collectText looks up in its override
// map). The char descriptions are stored as (value << 16), matching the
// readTxsm16 extraction `(tmpCharDescription >> 16) | (tmpCharDescription & 1)`.
Bytes txsmMultiRunRecord(const std::string &defaultJson,
                         const std::vector<std::string> &runJsons,
                         const std::vector<unsigned short> &runDescriptors,
                         const std::string &text)
{
  Bytes b;
  const size_t headerStart = 0;
  b.resize(41, 0); // frameFlag + 37 reserved bytes
  put32(b, headerStart, 0); // frameFlag = 0

  size_t p = 41;
  b.resize(p + 4, 0);
  put32(b, p, 1); p += 4; // numFrames

  b.resize(p + 4, 0);
  put32(b, p, 7); p += 4; // frameId == textId
  b.resize(p + 48, 0); p += 48;
  b.resize(p + 4, 0);
  put32(b, p, 0); p += 4; // textOnPath = 0
  b.resize(p + 8, 0); p += 8;
  // frameFlag == 0 branch
  b.resize(p + 16, 0); p += 16;
  b.resize(p + 4, 0);
  put32(b, p, 0); p += 4; // tlen = 0

  b.resize(p + 4, 0);
  put32(b, p, 1); p += 4; // numPara

  b.resize(p + 4, 0);
  put32(b, p, 0); p += 4; // stlId = 0 (no binary paragraph style)
  b.resize(p + 1, 0); p += 1;

  b.resize(p + 4, 0);
  put32(b, p, unsigned(defaultJson.size())); p += 4;
  b.insert(b.end(), defaultJson.begin(), defaultJson.end()); p += defaultJson.size();

  b.resize(p + 4, 0);
  put32(b, p, unsigned(runJsons.size())); p += 4; // numRecords

  for (size_t r = 0; r < runJsons.size(); ++r)
  {
    b.resize(p + 2, 0); p += 2; // record reserved
    b.resize(p + 2, 0);
    put16(b, p, 1); p += 2; // stFlag1 != 0: this record carries a run JSON
    b.resize(p + 2, 0);
    put16(b, p, 0); p += 2; // stFlag2
    b.resize(p + 4, 0);
    put32(b, p, unsigned(runJsons[r].size())); p += 4;
    b.insert(b.end(), runJsons[r].begin(), runJsons[r].end()); p += runJsons[r].size();
  }

  b.resize(p + 4, 0);
  put32(b, p, unsigned(runDescriptors.size())); p += 4; // numChars
  for (size_t c = 0; c < runDescriptors.size(); ++c)
  {
    b.resize(p + 8, 0);
    put64(b, p, static_cast<unsigned long long>(runDescriptors[c]) << 16); p += 8;
  }

  b.resize(p + 4, 0);
  put32(b, p, unsigned(text.size())); p += 4; // numBytes
  b.insert(b.end(), text.begin(), text.end()); p += text.size();
  b.push_back(0); // trailing 0 skipped by readTxsm16

  Bytes records;
  Bytes ver(2);
  ver[0] = 2000 & 255;
  ver[1] = 2000 >> 8;
  chunk(records, "vrsn", ver);
  chunk(records, "txsm", b);
  return records;
}

std::string jsonStyle(const char *font, unsigned size, bool withWeight, int weight, int italic)
{
  std::string s = std::string("{\"character\":{\"latin\":{\"font\":\"") + font +
                  "\",\"charset\":\"0\",\"size\":\"" + std::to_string(size) + "\"";
  if (withWeight)
    s += ",\"weight\":\"" + std::to_string(weight) + "\",\"italic\":\"" + std::to_string(italic) + "\"";
  s += "}}}";
  return s;
}

} // namespace

class CDRTextStyleTest : public CPPUNIT_NS::TestFixture
{
  CPPUNIT_TEST_SUITE(CDRTextStyleTest);
  CPPUNIT_TEST(testDefaultDoesNotClobberAbsent);
  CPPUNIT_TEST(testExplicitNormalResetsBoldItalic);
  CPPUNIT_TEST(testPartialOverrideKeepsOtherAxis);
  CPPUNIT_TEST(testNamedStyleParentChain);
  CPPUNIT_TEST(testCollectTextInheritsParagraphDefault);
  CPPUNIT_TEST(testCollectTextRunOverrideResetsToNormal);
  CPPUNIT_TEST(testCollectTextRunOverrideAddsBoldItalic);
  CPPUNIT_TEST(testRegularBoldItalicBoldItalicTransitions);
  CPPUNIT_TEST(testExplicitNormalResetInRuns);
  CPPUNIT_TEST(testTwoFamilies);
  CPPUNIT_TEST(testTwoObjectsSameFamilyDifferentFaces);
  CPPUNIT_TEST(testAbsentWeightAndItalicStayAbsent);
  CPPUNIT_TEST(testMissingFaceNamePreserved);
  CPPUNIT_TEST(testUnicodeRunPreserved);
  CPPUNIT_TEST(testUnprovenWeightPreservedAsAbsent);
  CPPUNIT_TEST(testParserReadsWeightAndItalic);
  CPPUNIT_TEST(testParserRunOverrideResetsWeightAndItalic);
  CPPUNIT_TEST(testMultiRunTrailingFlushUsesLastRunStyle);
  CPPUNIT_TEST(testSingleRunCollectTextStyleUnchanged);
  CPPUNIT_TEST(testCollectTextExplicitNormalResetSingleRun);
  CPPUNIT_TEST(testArtisticTextEnvelope);
  CPPUNIT_TEST(testParagraphTextEnvelope);
  CPPUNIT_TEST(testTextObjectStateReset);
  CPPUNIT_TEST(testEmbeddedNewlinesPreserveEventOrder);
  CPPUNIT_TEST_SUITE_END();

  // ---- CDRStyle inheritance -------------------------------------------------

  void testDefaultDoesNotClobberAbsent()
  {
    libcdr::CDRStyle base = styleOf("Lora", 14.0, kWeightBold, 1);
    libcdr::CDRStyle absent; // weight and italic are unset
    base.overrideStyle(absent);
    CPPUNIT_ASSERT_EQUAL(kWeightBold, base.m_fontWeight);
    CPPUNIT_ASSERT_EQUAL(1, base.m_fontItalic);
    CPPUNIT_ASSERT_EQUAL(std::string("Lora"), std::string(base.m_fontName.cstr()));
  }

  void testExplicitNormalResetsBoldItalic()
  {
    libcdr::CDRStyle base = styleOf("Lora", 14.0, kWeightBold, 1);
    base.overrideStyle(styleOf("Lora", 14.0, kWeightNormal, 0));
    CPPUNIT_ASSERT_EQUAL(kWeightNormal, base.m_fontWeight);
    CPPUNIT_ASSERT_EQUAL(0, base.m_fontItalic);
  }

  void testPartialOverrideKeepsOtherAxis()
  {
    libcdr::CDRStyle base = styleOf("Lora", 14.0, kWeightBold, 1);
    libcdr::CDRStyle overrideStyle = styleOf(nullptr, 0.0, kWeightNormal, -1);
    base.overrideStyle(overrideStyle);
    CPPUNIT_ASSERT_EQUAL(kWeightNormal, base.m_fontWeight);
    // Italic was not present in the override, so bold-italic's italic remains.
    CPPUNIT_ASSERT_EQUAL(1, base.m_fontItalic);
  }

  void testNamedStyleParentChain()
  {
    // Parent style: Lora bold-italic; child style only overrides weight.
    libcdr::CDRParserState state;
    libcdr::CDRStyle parent = styleOf("Lora", 14.0, kWeightBold, 1);
    libcdr::CDRStyle child = styleOf(nullptr, 0.0, kWeightNormal, -1);
    child.m_parentId = 1;
    state.m_styles[1] = parent;
    state.m_styles[2] = child;
    libcdr::CDRStylesCollector collector(state);
    const std::vector<unsigned char> text = {'A'};
    const std::vector<unsigned char> desc = {0};
    collector.collectText(5, 2, text, desc, std::map<unsigned, libcdr::CDRStyle>());
    const libcdr::CDRText &run = state.m_texts[5][0].m_line[0];
    CPPUNIT_ASSERT_EQUAL(std::string("Lora"), std::string(run.m_style.m_fontName.cstr()));
    CPPUNIT_ASSERT_EQUAL(kWeightNormal, run.m_style.m_fontWeight);
    // The child did not restate italic, so the parent's italic is inherited.
    CPPUNIT_ASSERT_EQUAL(1, run.m_style.m_fontItalic);
  }

  // ---- CDRStylesCollector::collectText inheritance --------------------------

  void testCollectTextInheritsParagraphDefault()
  {
    libcdr::CDRParserState state;
    state.m_styles[3] = styleOf("Lora", 14.0, kWeightBold, 1);
    libcdr::CDRStylesCollector collector(state);
    const std::vector<unsigned char> text = {'A'};
    const std::vector<unsigned char> desc = {0};
    collector.collectText(5, 3, text, desc, std::map<unsigned, libcdr::CDRStyle>());
    CPPUNIT_ASSERT_EQUAL(size_t(1), state.m_texts[5].size());
    const libcdr::CDRText &run = state.m_texts[5][0].m_line[0];
    CPPUNIT_ASSERT_EQUAL(kWeightBold, run.m_style.m_fontWeight);
    CPPUNIT_ASSERT_EQUAL(1, run.m_style.m_fontItalic);
  }

  void testCollectTextRunOverrideResetsToNormal()
  {
    libcdr::CDRParserState state;
    state.m_styles[3] = styleOf("Lora", 14.0, kWeightBold, 1);
    libcdr::CDRStylesCollector collector(state);
    std::map<unsigned, libcdr::CDRStyle> overrides;
    overrides[0] = styleOf(nullptr, 0.0, kWeightNormal, 0);
    const std::vector<unsigned char> text = {'A'};
    const std::vector<unsigned char> desc = {0};
    collector.collectText(5, 3, text, desc, overrides);
    const libcdr::CDRText &run = state.m_texts[5][0].m_line[0];
    CPPUNIT_ASSERT_EQUAL(kWeightNormal, run.m_style.m_fontWeight);
    CPPUNIT_ASSERT_EQUAL(0, run.m_style.m_fontItalic);
  }

  void testCollectTextRunOverrideAddsBoldItalic()
  {
    libcdr::CDRParserState state;
    state.m_styles[3] = styleOf("Arial", 10.0, kWeightNormal, 0);
    libcdr::CDRStylesCollector collector(state);
    std::map<unsigned, libcdr::CDRStyle> overrides;
    overrides[0] = styleOf("Lora", 14.0, kWeightBold, 1);
    const std::vector<unsigned char> text = {'A'};
    const std::vector<unsigned char> desc = {0};
    collector.collectText(5, 3, text, desc, overrides);
    const libcdr::CDRText &run = state.m_texts[5][0].m_line[0];
    CPPUNIT_ASSERT_EQUAL(kWeightBold, run.m_style.m_fontWeight);
    CPPUNIT_ASSERT_EQUAL(1, run.m_style.m_fontItalic);
    CPPUNIT_ASSERT_EQUAL(std::string("Lora"), std::string(run.m_style.m_fontName.cstr()));
  }

  // ---- CDRContentCollector span emission ------------------------------------

  void testRegularBoldItalicBoldItalicTransitions()
  {
    libcdr::CDRParserState state;
    TextRecorder rec;
    std::vector<libcdr::CDRText> runs;
    runs.push_back(libcdr::CDRText("A", styleOf("Lora", 14.0, kWeightNormal, 0)));
    runs.push_back(libcdr::CDRText("B", styleOf("Lora", 14.0, kWeightBold, 0)));
    runs.push_back(libcdr::CDRText("C", styleOf("Lora", 14.0, kWeightNormal, 1)));
    runs.push_back(libcdr::CDRText("D", styleOf("Lora", 14.0, kWeightBold, 1)));
    emitText(state, 5, runs, rec);

    CPPUNIT_ASSERT_EQUAL(size_t(4), rec.spans.size());
    CPPUNIT_ASSERT_EQUAL(std::string("normal"), stringValue(rec.spans[0], "fo:font-weight"));
    CPPUNIT_ASSERT_EQUAL(std::string("normal"), stringValue(rec.spans[0], "fo:font-style"));
    CPPUNIT_ASSERT_EQUAL(std::string("bold"), stringValue(rec.spans[1], "fo:font-weight"));
    CPPUNIT_ASSERT_EQUAL(std::string("normal"), stringValue(rec.spans[1], "fo:font-style"));
    CPPUNIT_ASSERT_EQUAL(std::string("normal"), stringValue(rec.spans[2], "fo:font-weight"));
    CPPUNIT_ASSERT_EQUAL(std::string("italic"), stringValue(rec.spans[2], "fo:font-style"));
    CPPUNIT_ASSERT_EQUAL(std::string("bold"), stringValue(rec.spans[3], "fo:font-weight"));
    CPPUNIT_ASSERT_EQUAL(std::string("italic"), stringValue(rec.spans[3], "fo:font-style"));
    // Test-local trace for the evidence log.
    for (size_t i = 0; i < rec.spans.size(); ++i)
      std::cout << "CDR_TEXT_TRACE run=" << i
                << " font=" << stringValue(rec.spans[i], "style:font-name")
                << " weight=" << stringValue(rec.spans[i], "fo:font-weight")
                << " style=" << stringValue(rec.spans[i], "fo:font-style")
                << " text=" << (i < rec.texts.size() ? rec.texts[i] : std::string()) << std::endl;
  }

  void testExplicitNormalResetInRuns()
  {
    libcdr::CDRParserState state;
    TextRecorder rec;
    std::vector<libcdr::CDRText> runs;
    runs.push_back(libcdr::CDRText("A", styleOf("Lora", 14.0, kWeightBold, 1)));
    runs.push_back(libcdr::CDRText("B", styleOf("Lora", 14.0, kWeightNormal, 0)));
    runs.push_back(libcdr::CDRText("C", styleOf("Lora", 14.0, kWeightBold, 1)));
    emitText(state, 5, runs, rec);
    CPPUNIT_ASSERT_EQUAL(size_t(3), rec.spans.size());
    CPPUNIT_ASSERT_EQUAL(std::string("bold"), stringValue(rec.spans[0], "fo:font-weight"));
    CPPUNIT_ASSERT_EQUAL(std::string("italic"), stringValue(rec.spans[0], "fo:font-style"));
    CPPUNIT_ASSERT_EQUAL(std::string("normal"), stringValue(rec.spans[1], "fo:font-weight"));
    CPPUNIT_ASSERT_EQUAL(std::string("normal"), stringValue(rec.spans[1], "fo:font-style"));
    CPPUNIT_ASSERT_EQUAL(std::string("bold"), stringValue(rec.spans[2], "fo:font-weight"));
    CPPUNIT_ASSERT_EQUAL(std::string("italic"), stringValue(rec.spans[2], "fo:font-style"));
  }

  void testTwoFamilies()
  {
    libcdr::CDRParserState state;
    TextRecorder rec;
    std::vector<libcdr::CDRText> runs;
    runs.push_back(libcdr::CDRText("A", styleOf("Arial", 10.0, kWeightNormal, 0)));
    runs.push_back(libcdr::CDRText("B", styleOf("Delighter Script", 22.0, kWeightNormal, 0)));
    runs.push_back(libcdr::CDRText("C", styleOf("Lora", 14.0, kWeightBold, 1)));
    emitText(state, 5, runs, rec);
    CPPUNIT_ASSERT_EQUAL(size_t(3), rec.spans.size());
    CPPUNIT_ASSERT_EQUAL(std::string("Arial"), stringValue(rec.spans[0], "style:font-name"));
    CPPUNIT_ASSERT_EQUAL(std::string("Delighter Script"), stringValue(rec.spans[1], "style:font-name"));
    CPPUNIT_ASSERT_EQUAL(std::string("Lora"), stringValue(rec.spans[2], "style:font-name"));
    CPPUNIT_ASSERT_EQUAL(std::string("bold"), stringValue(rec.spans[2], "fo:font-weight"));
    CPPUNIT_ASSERT_EQUAL(std::string("italic"), stringValue(rec.spans[2], "fo:font-style"));
  }

  void testTwoObjectsSameFamilyDifferentFaces()
  {
    libcdr::CDRParserState state;
    TextRecorder rec;
    std::vector<libcdr::CDRText> regular;
    regular.push_back(libcdr::CDRText("A", styleOf("Lora", 14.0, kWeightNormal, 0)));
    std::vector<libcdr::CDRText> boldItalic;
    boldItalic.push_back(libcdr::CDRText("B", styleOf("Lora", 14.0, kWeightBold, 1)));
    emitText(state, 5, regular, rec);
    emitText(state, 6, boldItalic, rec);
    CPPUNIT_ASSERT_EQUAL(size_t(2), rec.spans.size());
    CPPUNIT_ASSERT_EQUAL(std::string("Lora"), stringValue(rec.spans[0], "style:font-name"));
    CPPUNIT_ASSERT_EQUAL(std::string("normal"), stringValue(rec.spans[0], "fo:font-weight"));
    CPPUNIT_ASSERT_EQUAL(std::string("normal"), stringValue(rec.spans[0], "fo:font-style"));
    CPPUNIT_ASSERT_EQUAL(std::string("Lora"), stringValue(rec.spans[1], "style:font-name"));
    CPPUNIT_ASSERT_EQUAL(std::string("bold"), stringValue(rec.spans[1], "fo:font-weight"));
    CPPUNIT_ASSERT_EQUAL(std::string("italic"), stringValue(rec.spans[1], "fo:font-style"));
  }

  void testAbsentWeightAndItalicStayAbsent()
  {
    libcdr::CDRParserState state;
    TextRecorder rec;
    std::vector<libcdr::CDRText> runs;
    // -1 means the source record did not carry weight/italic at all.
    runs.push_back(libcdr::CDRText("A", styleOf("Arial", 10.0, -1, -1)));
    emitText(state, 5, runs, rec);
    CPPUNIT_ASSERT_EQUAL(size_t(1), rec.spans.size());
    CPPUNIT_ASSERT(!hasProperty(rec.spans[0], "fo:font-weight"));
    CPPUNIT_ASSERT(!hasProperty(rec.spans[0], "fo:font-style"));
    CPPUNIT_ASSERT_EQUAL(std::string("Arial"), stringValue(rec.spans[0], "style:font-name"));
    CPPUNIT_ASSERT(hasProperty(rec.spans[0], "fo:font-size"));
  }

  void testMissingFaceNamePreserved()
  {
    libcdr::CDRParserState state;
    TextRecorder rec;
    std::vector<libcdr::CDRText> runs;
    runs.push_back(libcdr::CDRText("A", styleOf("NoSuchFamilyXYZ", 12.0, kWeightBold, 1)));
    emitText(state, 5, runs, rec);
    CPPUNIT_ASSERT_EQUAL(size_t(1), rec.spans.size());
    // The reader preserves the requested face; resolution is the consumer's job.
    CPPUNIT_ASSERT_EQUAL(std::string("NoSuchFamilyXYZ"), stringValue(rec.spans[0], "style:font-name"));
    CPPUNIT_ASSERT_EQUAL(std::string("bold"), stringValue(rec.spans[0], "fo:font-weight"));
    CPPUNIT_ASSERT_EQUAL(std::string("italic"), stringValue(rec.spans[0], "fo:font-style"));
  }

  void testUnicodeRunPreserved()
  {
    libcdr::CDRParserState state;
    TextRecorder rec;
    std::vector<libcdr::CDRText> runs;
    runs.push_back(libcdr::CDRText("Grad\u00faaci\u00f3n \u65e5", styleOf("Lora", 14.0, kWeightBold, 1)));
    emitText(state, 5, runs, rec);
    CPPUNIT_ASSERT_EQUAL(size_t(1), rec.spans.size());
    CPPUNIT_ASSERT_EQUAL(std::string("bold"), stringValue(rec.spans[0], "fo:font-weight"));
    CPPUNIT_ASSERT_EQUAL(std::string("italic"), stringValue(rec.spans[0], "fo:font-style"));
    CPPUNIT_ASSERT_EQUAL(size_t(1), rec.texts.size());
    CPPUNIT_ASSERT_EQUAL(std::string("Grad\u00faaci\u00f3n \u65e5"), rec.texts[0]);
  }

  void testUnprovenWeightPreservedAsAbsent()
  {
    libcdr::CDRParserState state;
    TextRecorder rec;
    std::vector<libcdr::CDRText> runs;
    // 700 is not one of the two source values proven by the F-TEXT-A oracle;
    // it must not be guessed into "bold".
    runs.push_back(libcdr::CDRText("A", styleOf("Lora", 14.0, 700, 1)));
    emitText(state, 5, runs, rec);
    CPPUNIT_ASSERT_EQUAL(size_t(1), rec.spans.size());
    CPPUNIT_ASSERT(!hasProperty(rec.spans[0], "fo:font-weight"));
    CPPUNIT_ASSERT_EQUAL(std::string("italic"), stringValue(rec.spans[0], "fo:font-style"));
  }

  // ---- Parser source JSON ---------------------------------------------------

  void testParserReadsWeightAndItalic()
  {
    const std::string def = jsonStyle("Lora", 49362, true, kWeightBold, 1);
    Bytes records = txsmRecord(def, jsonStyle("Lora", 49362, false, 0, 0));
    libcdr::CDRInternalStream input(records);
    libcdr::CDRParserState state;
    libcdr::CDRStylesCollector probe(state);
    const std::vector<std::unique_ptr<librevenge::RVNGInputStream>> external;
    libcdr::CDRParser parser(external, &probe);
    CPPUNIT_ASSERT(parser.parseRecords(&input));
    CPPUNIT_ASSERT_EQUAL(size_t(1), state.m_texts[7].size());
    const libcdr::CDRText &run = state.m_texts[7][0].m_line[0];
    CPPUNIT_ASSERT_EQUAL(std::string("Lora"), std::string(run.m_style.m_fontName.cstr()));
    CPPUNIT_ASSERT_EQUAL(kWeightBold, run.m_style.m_fontWeight);
    CPPUNIT_ASSERT_EQUAL(1, run.m_style.m_fontItalic);
  }

  void testParserRunOverrideResetsWeightAndItalic()
  {
    const std::string def = jsonStyle("Lora", 49362, true, kWeightBold, 1);
    // Run record JSON carries an explicit reset to normal plus a new family.
    const std::string run = jsonStyle("Arial", 84667, true, kWeightNormal, 0);
    Bytes records = txsmRecord(def, run);
    libcdr::CDRInternalStream input(records);
    libcdr::CDRParserState state;
    libcdr::CDRStylesCollector probe(state);
    const std::vector<std::unique_ptr<librevenge::RVNGInputStream>> external;
    libcdr::CDRParser parser(external, &probe);
    CPPUNIT_ASSERT(parser.parseRecords(&input));
    const libcdr::CDRText &parsed = state.m_texts[7][0].m_line[0];
    CPPUNIT_ASSERT_EQUAL(std::string("Arial"), std::string(parsed.m_style.m_fontName.cstr()));
    CPPUNIT_ASSERT_EQUAL(kWeightNormal, parsed.m_style.m_fontWeight);
    CPPUNIT_ASSERT_EQUAL(0, parsed.m_style.m_fontItalic);
  }

  // T03 outcome test: the trailing flush must use the style of the last
  // character description, not the previous run's description. Two runs in one
  // binary txsm paragraph: run 0 is Arial normal, run 1 is Lora bold-italic.
  // Before the fix, the post-loop flush still held run 0's resolved style, so
  // the final "B" run was emitted as Arial normal.
  void testMultiRunTrailingFlushUsesLastRunStyle()
  {
    const std::string def = jsonStyle("Lora", 49362, true, kWeightBold, 1);
    const std::string run0 = jsonStyle("Arial", 84667, true, kWeightNormal, 0);
    const std::string run1 = jsonStyle("Lora", 49362, true, kWeightBold, 1);
    Bytes records = txsmMultiRunRecord(def, {run0, run1}, {0, 2}, "AB");
    libcdr::CDRInternalStream input(records);
    libcdr::CDRParserState state;
    libcdr::CDRStylesCollector probe(state);
    const std::vector<std::unique_ptr<librevenge::RVNGInputStream>> external;
    libcdr::CDRParser parser(external, &probe);
    CPPUNIT_ASSERT(parser.parseRecords(&input));

    CPPUNIT_ASSERT_EQUAL(size_t(1), state.m_texts[7].size());
    const std::vector<libcdr::CDRText> &runs = state.m_texts[7][0].m_line;
    CPPUNIT_ASSERT_EQUAL(size_t(2), runs.size());

    CPPUNIT_ASSERT_EQUAL(std::string("A"), std::string(runs[0].m_text.cstr()));
    CPPUNIT_ASSERT_EQUAL(std::string("Arial"), std::string(runs[0].m_style.m_fontName.cstr()));
    CPPUNIT_ASSERT_EQUAL(kWeightNormal, runs[0].m_style.m_fontWeight);
    CPPUNIT_ASSERT_EQUAL(0, runs[0].m_style.m_fontItalic);

    CPPUNIT_ASSERT_EQUAL(std::string("B"), std::string(runs[1].m_text.cstr()));
    CPPUNIT_ASSERT_EQUAL(std::string("Lora"), std::string(runs[1].m_style.m_fontName.cstr()));
    CPPUNIT_ASSERT_EQUAL(kWeightBold, runs[1].m_style.m_fontWeight);
    CPPUNIT_ASSERT_EQUAL(1, runs[1].m_style.m_fontItalic);
  }

  // No-regression: a single run must keep resolving to the paragraph default.
  void testSingleRunCollectTextStyleUnchanged()
  {
    libcdr::CDRParserState state;
    state.m_styles[3] = styleOf("Lora", 14.0, kWeightBold, 1);
    libcdr::CDRStylesCollector collector(state);
    const std::vector<unsigned char> text = {'A'};
    const std::vector<unsigned char> desc = {0};
    collector.collectText(5, 3, text, desc, std::map<unsigned, libcdr::CDRStyle>());
    CPPUNIT_ASSERT_EQUAL(size_t(1), state.m_texts[5][0].m_line.size());
    const libcdr::CDRText &run = state.m_texts[5][0].m_line[0];
    CPPUNIT_ASSERT_EQUAL(std::string("Lora"), std::string(run.m_style.m_fontName.cstr()));
    CPPUNIT_ASSERT_EQUAL(kWeightBold, run.m_style.m_fontWeight);
    CPPUNIT_ASSERT_EQUAL(1, run.m_style.m_fontItalic);
  }

  // No-regression: a single run with an explicit normal run override must stay
  // normal; absent values keep inheriting and specified values still win.
  void testCollectTextExplicitNormalResetSingleRun()
  {
    libcdr::CDRParserState state;
    state.m_styles[3] = styleOf("Lora", 14.0, kWeightBold, 1);
    libcdr::CDRStylesCollector collector(state);
    std::map<unsigned, libcdr::CDRStyle> overrides;
    overrides[0] = styleOf(nullptr, 0.0, kWeightNormal, 0);
    const std::vector<unsigned char> text = {'A'};
    const std::vector<unsigned char> desc = {0};
    collector.collectText(5, 3, text, desc, overrides);
    CPPUNIT_ASSERT_EQUAL(size_t(1), state.m_texts[5][0].m_line.size());
    const libcdr::CDRText &run = state.m_texts[5][0].m_line[0];
    // Family and size were not restated by the override, so the paragraph
    // default is inherited; weight/italic were restated and must be normal.
    CPPUNIT_ASSERT_EQUAL(std::string("Lora"), std::string(run.m_style.m_fontName.cstr()));
    CPPUNIT_ASSERT_EQUAL(kWeightNormal, run.m_style.m_fontWeight);
    CPPUNIT_ASSERT_EQUAL(0, run.m_style.m_fontItalic);
  }

  // ---- Versioned text activation metadata (contract 3.2) --------------------

  // Artistic text: the parser callback is the classification proof; the emitted
  // frame carries the version/content-kind/object-id/text-kind envelope while
  // the existing frame coordinates and padding stay untouched.
  void testArtisticTextEnvelope()
  {
    libcdr::CDRParserState state;
    state.m_pages.push_back(libcdr::CDRPage(8.0, 11.0, 0.0, 0.0));
    TextRecorder rec;
    std::vector<libcdr::CDRText> runs;
    runs.push_back(libcdr::CDRText("A", styleOf("Lora", 14.0, kWeightNormal, 0)));
    {
      libcdr::CDRContentCollector collector(state, &rec, true);
      appendTextObject(state, collector, 5, runs, false);
    }
    CPPUNIT_ASSERT_EQUAL(size_t(1), rec.startTextObjects.size());
    assertTextEnvelope(rec.startTextObjects[0], 5, "artistic");
    CPPUNIT_ASSERT(rec.startTextObjects[0]["svg:x"]);
    CPPUNIT_ASSERT(rec.startTextObjects[0]["svg:width"]);
    CPPUNIT_ASSERT(rec.startTextObjects[0]["fo:padding-left"]);
  }

  // Paragraph text uses the other parser callback and is classified as
  // paragraph; the frame path is unchanged.
  void testParagraphTextEnvelope()
  {
    libcdr::CDRParserState state;
    state.m_pages.push_back(libcdr::CDRPage(8.0, 11.0, 0.0, 0.0));
    TextRecorder rec;
    std::vector<libcdr::CDRText> runs;
    runs.push_back(libcdr::CDRText("P", styleOf("Lora", 14.0, kWeightNormal, 0)));
    {
      libcdr::CDRContentCollector collector(state, &rec, true);
      appendTextObject(state, collector, 7, runs, true);
    }
    CPPUNIT_ASSERT_EQUAL(size_t(1), rec.startTextObjects.size());
    assertTextEnvelope(rec.startTextObjects[0], 7, "paragraph");
  }

  // Two text objects through one collector: each envelope uses its own object id
  // and kind. A missing reset leaks the first object's kind/id into the second.
  void testTextObjectStateReset()
  {
    libcdr::CDRParserState state;
    state.m_pages.push_back(libcdr::CDRPage(8.0, 11.0, 0.0, 0.0));
    TextRecorder rec;
    std::vector<libcdr::CDRText> artisticRuns;
    artisticRuns.push_back(libcdr::CDRText("A", styleOf("Lora", 14.0, kWeightNormal, 0)));
    std::vector<libcdr::CDRText> paragraphRuns;
    paragraphRuns.push_back(libcdr::CDRText("B", styleOf("Lora", 14.0, kWeightNormal, 0)));
    {
      // Queue order (reverseOrder=false) preserves object order in the trace.
      libcdr::CDRContentCollector collector(state, &rec, false);
      appendTextObject(state, collector, 5, artisticRuns, false);
      appendTextObject(state, collector, 6, paragraphRuns, true);
    }
    CPPUNIT_ASSERT_EQUAL(size_t(2), rec.startTextObjects.size());
    assertTextEnvelope(rec.startTextObjects[0], 5, "artistic");
    assertTextEnvelope(rec.startTextObjects[1], 6, "paragraph");
  }

  // Embedded newlines still use the native split path (CDROutputElementList):
  // one insertLineBreak per '\n', consecutive breaks for a blank line, and the
  // text fragments keep their source order. No collector-side splitting.
  void testEmbeddedNewlinesPreserveEventOrder()
  {
    libcdr::CDRParserState state;
    state.m_pages.push_back(libcdr::CDRPage(8.0, 11.0, 0.0, 0.0));
    TextRecorder rec;
    std::vector<libcdr::CDRText> runs;
    runs.push_back(libcdr::CDRText("A\nB\n\nC", styleOf("Lora", 14.0, kWeightNormal, 0)));
    {
      libcdr::CDRContentCollector collector(state, &rec, true);
      appendTextObject(state, collector, 5, runs, false);
    }
    std::vector<std::string> expected;
    expected.push_back("insertText:A");
    expected.push_back("lineBreak");
    expected.push_back("insertText:B");
    expected.push_back("lineBreak");
    expected.push_back("lineBreak");
    expected.push_back("insertText:C");
    std::vector<std::string> textEvents;
    for (const auto &event : rec.events)
    {
      if (event == "lineBreak" || event.compare(0, 11, "insertText:") == 0)
        textEvents.push_back(event);
    }
    CPPUNIT_ASSERT_EQUAL(expected.size(), textEvents.size());
    for (size_t i = 0; i < expected.size(); ++i)
      CPPUNIT_ASSERT_EQUAL(expected[i], textEvents[i]);
  }
};

CPPUNIT_TEST_SUITE_REGISTRATION(CDRTextStyleTest);
