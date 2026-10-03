/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. See http://mozilla.org/MPL/2.0/. */

// Focused producer tests for the recovered custom libcdr bitmap crop
// relationship transport. They exercise the reader -> collector -> drawing
// interface path and assert that ownership comes from the parser state in the
// same readBitmap operation (never from ids, adjacency, order or geometry).

#include <cmath>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <cppunit/TestFixture.h>
#include <cppunit/extensions/HelperMacros.h>

#include <librevenge/librevenge.h>
#include <librevenge-stream/librevenge-stream.h>

#include "CDRContentCollector.h"
#include "CDRInternalStream.h"
#include "CDRParser.h"
#include "CDRStylesCollector.h"

namespace
{

typedef std::vector<unsigned char> Bytes;

void put16(Bytes &b, size_t pos, unsigned value)
{
  for (unsigned i = 0; i < 2; ++i) b.at(pos + i) = (value >> (8 * i)) & 255;
}

void put32(Bytes &b, size_t pos, unsigned value)
{
  for (unsigned i = 0; i < 4; ++i) b.at(pos + i) = (value >> (8 * i)) & 255;
}

void chunk(Bytes &b, const char *tag, const Bytes &data)
{
  size_t start = b.size();
  b.resize(start + 8);
  std::copy(tag, tag + 4, b.begin() + start);
  put32(b, start + 4, unsigned(data.size()));
  b.insert(b.end(), data.begin(), data.end());
}

bool almostEqual(double a, double b)
{
  return std::fabs(a - b) < 1e-9;
}

struct Node
{
  std::string action;
  double x, y, x1, y1, x2, y2;
  Node() : action(), x(0.0), y(0.0), x1(0.0), y1(0.0), x2(0.0), y2(0.0) {}
  bool operator==(const Node &o) const
  {
    return action == o.action && almostEqual(x, o.x) && almostEqual(y, o.y) &&
           almostEqual(x1, o.x1) && almostEqual(y1, o.y1) &&
           almostEqual(x2, o.x2) && almostEqual(y2, o.y2);
  }
};

double value(const librevenge::RVNGPropertyList &n, const char *key)
{
  return n[key] ? n[key]->getDouble() : 0.0;
}

std::string stringValue(const librevenge::RVNGPropertyList &n, const char *key)
{
  return n[key] ? n[key]->getStr().cstr() : std::string();
}

std::vector<Node> nodesOf(const librevenge::RVNGPropertyListVector &vec)
{
  std::vector<Node> out;
  for (unsigned long i = 0; i < vec.count(); ++i)
  {
    const librevenge::RVNGPropertyList &n = vec[i];
    Node node;
    node.action = stringValue(n, "librevenge:path-action");
    node.x = value(n, "svg:x");
    node.y = value(n, "svg:y");
    node.x1 = value(n, "svg:x1");
    node.y1 = value(n, "svg:y1");
    node.x2 = value(n, "svg:x2");
    node.y2 = value(n, "svg:y2");
    out.push_back(node);
  }
  return out;
}

std::vector<Node> pathNodes(const libcdr::CDRPath &path)
{
  librevenge::RVNGPropertyListVector vec;
  path.writeOut(vec);
  return nodesOf(vec);
}

// Minimal drawing-interface sink: records the events the crop transport rides on.
class Recorder : public librevenge::RVNGDrawingInterface
{
public:
  std::vector<librevenge::RVNGPropertyList> graphics;
  std::vector<librevenge::RVNGPropertyList> paths;
  std::vector<librevenge::RVNGPropertyList> styles;

  void startDocument(const librevenge::RVNGPropertyList &) override {}
  void endDocument() override {}
  void setDocumentMetaData(const librevenge::RVNGPropertyList &) override {}
  void defineEmbeddedFont(const librevenge::RVNGPropertyList &) override {}
  void startPage(const librevenge::RVNGPropertyList &) override {}
  void endPage() override {}
  void startMasterPage(const librevenge::RVNGPropertyList &) override {}
  void endMasterPage() override {}
  void setStyle(const librevenge::RVNGPropertyList &propList) override { styles.push_back(propList); }
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
  void drawPath(const librevenge::RVNGPropertyList &propList) override { paths.push_back(propList); }
  void drawGraphicObject(const librevenge::RVNGPropertyList &propList) override { graphics.push_back(propList); }
  void drawConnector(const librevenge::RVNGPropertyList &) override {}
  void startTextObject(const librevenge::RVNGPropertyList &) override {}
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
  void openParagraph(const librevenge::RVNGPropertyList &) override {}
  void closeParagraph() override {}
  void defineCharacterStyle(const librevenge::RVNGPropertyList &) override {}
  void openSpan(const librevenge::RVNGPropertyList &) override {}
  void closeSpan() override {}
  void openLink(const librevenge::RVNGPropertyList &) override {}
  void closeLink() override {}
  void insertTab() override {}
  void insertSpace() override {}
  void insertText(const librevenge::RVNGString &) override {}
  void insertLineBreak() override {}
  void insertField(const librevenge::RVNGPropertyList &) override {}
};

void addPayload(libcdr::CDRParserState &state, unsigned imageId, bool png)
{
  if (png)
  {
    const unsigned char sig[] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a, 0x01, 0x02};
    state.m_bmps[imageId] = librevenge::RVNGBinaryData(sig, sizeof(sig));
  }
  else
  {
    const unsigned char bmp[] = {'B', 'M', 0x00, 0x01, 0x02, 0x03};
    state.m_bmps[imageId] = librevenge::RVNGBinaryData(bmp, sizeof(bmp));
  }
}

libcdr::CDRPath rectanglePath()
{
  libcdr::CDRPath path;
  path.appendMoveTo(0.5, 0.25);
  path.appendLineTo(1.5, 0.25);
  path.appendLineTo(1.5, 1.25);
  path.appendLineTo(0.5, 1.25);
  path.appendClosePath();
  return path;
}

libcdr::CDRPath curvePath()
{
  libcdr::CDRPath path;
  path.appendMoveTo(0.25, 0.25);
  path.appendCubicBezierTo(0.75, 0.0, 1.25, 1.5, 1.75, 1.25);
  path.appendClosePath();
  return path;
}

void runObject(libcdr::CDRParserState &state, Recorder &rec, const libcdr::CDRPath &path,
               unsigned imageId, double x1, double x2, double y1, double y2,
               const libcdr::CDRBitmapCrop *crop,
               const libcdr::CDRTransforms &objTransform = libcdr::CDRTransforms(),
               unsigned fillStyleId = 0, const libcdr::CDRFillStyle *fillStyle = nullptr)
{
  if (fillStyleId && fillStyle)
    state.m_fillStyles[fillStyleId] = *fillStyle;
  {
    libcdr::CDRContentCollector collector(state, &rec, true);
    collector.collectObject(1);
    if (!objTransform.empty())
      collector.collectTransform(objTransform, false);
    if (fillStyleId)
      collector.collectFillStyleId(fillStyleId);
    collector.collectPath(path);
    collector.collectBitmap(imageId, x1, x2, y1, y2, crop);
    collector.collectLevel(0);
  }
}

// Parser probe that captures the explicit ownership passed in collectBitmap.
class CropProbe : public libcdr::CDRStylesCollector
{
public:
  explicit CropProbe(libcdr::CDRParserState &state) : CDRStylesCollector(state) {}
  unsigned paths = 0, bitmaps = 0;
  bool hasCrop = false;
  unsigned cropObjectId = 0;
  unsigned imageId = 0;
  libcdr::CDRPath cropPath;
  libcdr::CDRPath path;
  libcdr::CDRBitmapCrop::Space cropSpace = libcdr::CDRBitmapCrop::PARENT;
  libcdr::CDRBitmapCrop::FillRule cropFillRule = libcdr::CDRBitmapCrop::FILL_RULE_UNSPECIFIED;

  void collectPath(const libcdr::CDRPath &p) override { ++paths; path = p; }
  void collectBitmap(unsigned id, double, double, double, double,
                     const libcdr::CDRBitmapCrop *crop) override
  {
    ++bitmaps;
    imageId = id;
    hasCrop = crop != nullptr;
    if (crop)
    {
      cropPath = crop->m_path;
      cropObjectId = crop->m_objectId;
      cropSpace = crop->m_space;
      cropFillRule = crop->m_fillRule;
    }
  }
};

// One loda bitmap record (v>=900 layout) with an optional rectangle boundary.
Bytes lodaBitmapRecord(bool withPoints)
{
  Bytes bmp(128, 0);
  size_t p = 0;
  put32(bmp, p, 127000); p += 4; // x1 = 0.5in
  put32(bmp, p, 127000); p += 4; // y1 = 0.5in
  put32(bmp, p, 381000); p += 4; // x2 = 1.5in
  put32(bmp, p, 381000); p += 4; // y2 = 1.5in
  p += 16; // reserved
  p += 16; // reserved
  put32(bmp, p, 42); p += 4;     // source bitmap/image id
  p += 20; // v>=900 reserved
  put16(bmp, p, withPoints ? 4u : 0u); p += 2;
  p += 2; // reserved
  if (withPoints)
  {
    const int coords[4][2] = {{127000, 127000}, {381000, 127000}, {381000, 381000}, {127000, 381000}};
    for (int i = 0; i < 4; ++i)
    {
      put32(bmp, p, unsigned(coords[i][0])); p += 4;
      put32(bmp, p, unsigned(coords[i][1])); p += 4;
    }
    bmp[p++] = 0x00; // move
    bmp[p++] = 0x40; // line
    bmp[p++] = 0x40; // line
    bmp[p++] = 0x48; // line + close
  }
  bmp.resize(p);

  Bytes loda(28, 0);
  put32(loda, 0, 0);     // chunkLength
  put32(loda, 4, 1);     // numOfArgs
  put32(loda, 8, 20);    // startOfArgs
  put32(loda, 12, 24);   // startOfArgTypes
  put32(loda, 16, 5);    // chunkType: bitmap
  put32(loda, 20, 28);   // argOffsets[0]
  put32(loda, 24, 0x1e); // argTypes[0]: loda coords
  loda.insert(loda.end(), bmp.begin(), bmp.end());
  return loda;
}

Bytes parserStream(bool withPoints)
{
  Bytes records;
  Bytes ver(2);
  ver[0] = 2000 & 255;
  ver[1] = 2000 >> 8;
  chunk(records, "vrsn", ver);
  chunk(records, "loda", lodaBitmapRecord(withPoints));
  return records;
}

// One legacy (m_version < 600) loda bitmap record. That branch reads only the
// first corner (16-bit coordinates) and leaves x2=y2=0, so the boundary it
// collects is the known degenerate pre-v6 path. All loda header fields and the
// image id are read with readUnsigned, which is 16-bit at this precision.
Bytes lodaLegacyBitmapRecord(unsigned x1mm, unsigned y1mm, unsigned imageId)
{
  Bytes bmp(34, 0);
  put16(bmp, 0, x1mm);  // x1 = x1mm/1000 inch
  put16(bmp, 2, y1mm);  // y1 = y1mm/1000 inch
  put16(bmp, 12, imageId);

  Bytes loda(14, 0);
  put16(loda, 0, 0);     // chunkLength
  put16(loda, 2, 1);     // numOfArgs
  put16(loda, 4, 10);    // startOfArgs
  put16(loda, 6, 12);    // startOfArgTypes
  put16(loda, 8, 5);     // chunkType: bitmap (v>=400 numbering)
  put16(loda, 10, 14);   // argOffsets[0] -> payload
  put16(loda, 12, 0x1e); // argTypes[0]: loda coords
  loda.insert(loda.end(), bmp.begin(), bmp.end());
  return loda;
}

Bytes parserStreamLegacy()
{
  Bytes records;
  Bytes ver(2);
  ver[0] = 500 & 255;
  ver[1] = 500 >> 8;
  chunk(records, "vrsn", ver);
  chunk(records, "loda", lodaLegacyBitmapRecord(500, 250, 42));
  return records;
}

} // namespace

class CDRCropTransportTest : public CPPUNIT_NS::TestFixture
{
  CPPUNIT_TEST_SUITE(CDRCropTransportTest);
  CPPUNIT_TEST(testParserOwnsBoundary);
  CPPUNIT_TEST(testParserEmptyBoundaryHasNoCrop);
  CPPUNIT_TEST(testRectangleParentCrop);
  CPPUNIT_TEST(testCurveParentCrop);
  CPPUNIT_TEST(testRotatedCropNotDoubleTransformed);
  CPPUNIT_TEST(testImageSpaceTransform);
  CPPUNIT_TEST(testSharedPayloadIndependentCrops);
  CPPUNIT_TEST(testNoAdjacencyPairing);
  CPPUNIT_TEST(testEmptyBoundaryNoExtension);
  CPPUNIT_TEST(testMalformedSingleMoveToNoExtension);
  CPPUNIT_TEST(testVisibleFrameRole);
  CPPUNIT_TEST(testVisibleFrameRolePngFillOnly);
  CPPUNIT_TEST(testGroupTransformedCrop);
  CPPUNIT_TEST(testShearReflectionCrop);
  CPPUNIT_TEST(testCompoundSubpathClosure);
  CPPUNIT_TEST(testLegacyPreV6NoCropOwnership);
  CPPUNIT_TEST(testLegacyPreV6NoVacardsFields);
  CPPUNIT_TEST(testSanitizedEventTrace);
  CPPUNIT_TEST_SUITE_END();

  void testParserOwnsBoundary()
  {
    Bytes records = parserStream(true);
    libcdr::CDRInternalStream input(records);
    libcdr::CDRParserState state;
    CropProbe probe(state);
    const std::vector<std::unique_ptr<librevenge::RVNGInputStream>> external;
    libcdr::CDRParser parser(external, &probe);
    CPPUNIT_ASSERT(parser.parseRecords(&input));
    CPPUNIT_ASSERT_EQUAL(1U, probe.paths);
    CPPUNIT_ASSERT_EQUAL(1U, probe.bitmaps);
    CPPUNIT_ASSERT(probe.hasCrop);
    CPPUNIT_ASSERT_EQUAL(42U, probe.imageId);
    CPPUNIT_ASSERT_EQUAL(42U, probe.cropObjectId);
    CPPUNIT_ASSERT(probe.cropSpace == libcdr::CDRBitmapCrop::PARENT);
    CPPUNIT_ASSERT(probe.cropFillRule == libcdr::CDRBitmapCrop::FILL_RULE_UNSPECIFIED);
    // The boundary handed to collectBitmap is the same path object that was
    // collected in this operation, not a reconstruction.
    CPPUNIT_ASSERT(pathNodes(probe.cropPath) == pathNodes(probe.path));
    CPPUNIT_ASSERT(!probe.cropPath.empty());
  }

  void testParserEmptyBoundaryHasNoCrop()
  {
    Bytes records = parserStream(false);
    libcdr::CDRInternalStream input(records);
    libcdr::CDRParserState state;
    CropProbe probe(state);
    const std::vector<std::unique_ptr<librevenge::RVNGInputStream>> external;
    libcdr::CDRParser parser(external, &probe);
    CPPUNIT_ASSERT(parser.parseRecords(&input));
    CPPUNIT_ASSERT_EQUAL(1U, probe.bitmaps);
    CPPUNIT_ASSERT(!probe.hasCrop);
  }

  void testRectangleParentCrop()
  {
    libcdr::CDRParserState state;
    state.m_pages.push_back(libcdr::CDRPage());
    addPayload(state, 11, true);
    Recorder rec;
    const libcdr::CDRPath rect = rectanglePath();
    libcdr::CDRBitmapCrop crop;
    crop.m_objectId = 11;
    crop.m_hasPath = true;
    crop.m_path = rect;
    crop.m_space = libcdr::CDRBitmapCrop::PARENT;
    crop.m_fillRule = libcdr::CDRBitmapCrop::FILL_RULE_UNSPECIFIED;
    runObject(state, rec, rect, 11, 0.0, 2.0, 0.0, 1.0, &crop);

    CPPUNIT_ASSERT_EQUAL(size_t(1), rec.graphics.size());
    const librevenge::RVNGPropertyList &g = rec.graphics[0];
    CPPUNIT_ASSERT(g["vacards:cdr-fidelity-version"]);
    CPPUNIT_ASSERT_EQUAL(1, g["vacards:cdr-fidelity-version"]->getInt());
    CPPUNIT_ASSERT_EQUAL(11, g["vacards:cdr-object-id"]->getInt());
    CPPUNIT_ASSERT_EQUAL(std::string("bitmap"), stringValue(g, "vacards:cdr-content-kind"));
    CPPUNIT_ASSERT_EQUAL(std::string("parent"), stringValue(g, "vacards:cdr-crop-space"));
    CPPUNIT_ASSERT_EQUAL(std::string("crop-only"), stringValue(g, "vacards:cdr-crop-role"));
    CPPUNIT_ASSERT_EQUAL(std::string("unspecified"), stringValue(g, "vacards:cdr-crop-fill-rule"));
    CPPUNIT_ASSERT(!g.child("vacards:cdr-image-transform"));

    const librevenge::RVNGPropertyListVector *cropPath = g.child("vacards:cdr-crop-path");
    CPPUNIT_ASSERT(cropPath);
    const std::vector<Node> cropNodes = nodesOf(*cropPath);
    CPPUNIT_ASSERT(cropNodes.size() >= 4);
    CPPUNIT_ASSERT_EQUAL(std::string("M"), cropNodes[0].action);
    CPPUNIT_ASSERT_EQUAL(std::string("Z"), cropNodes.back().action);
    // Independent mapping: page y-flip with height 0 maps (0.5,0.25) to (0.5,-0.25).
    CPPUNIT_ASSERT(almostEqual(0.5, cropNodes[0].x));
    CPPUNIT_ASSERT(almostEqual(-0.25, cropNodes[0].y));
    // The crop path is the same geometry as the emitted svg:d.
    CPPUNIT_ASSERT_EQUAL(size_t(1), rec.paths.size());
    CPPUNIT_ASSERT(rec.paths[0].child("svg:d"));
    CPPUNIT_ASSERT(nodesOf(*rec.paths[0].child("svg:d")) == cropNodes);
  }

  void testCurveParentCrop()
  {
    libcdr::CDRParserState state;
    state.m_pages.push_back(libcdr::CDRPage());
    addPayload(state, 12, true);
    Recorder rec;
    const libcdr::CDRPath curve = curvePath();
    libcdr::CDRBitmapCrop crop;
    crop.m_objectId = 12;
    crop.m_hasPath = true;
    crop.m_path = curve;
    crop.m_space = libcdr::CDRBitmapCrop::PARENT;
    runObject(state, rec, curve, 12, 0.0, 2.0, 0.0, 1.5, &crop);

    CPPUNIT_ASSERT_EQUAL(size_t(1), rec.graphics.size());
    const librevenge::RVNGPropertyList &g = rec.graphics[0];
    const librevenge::RVNGPropertyListVector *cropPath = g.child("vacards:cdr-crop-path");
    CPPUNIT_ASSERT(cropPath);
    const std::vector<Node> cropNodes = nodesOf(*cropPath);
    bool sawCurve = false;
    for (const auto &n : cropNodes)
      if (n.action == "C") sawCurve = true;
    CPPUNIT_ASSERT(sawCurve);
    CPPUNIT_ASSERT_EQUAL(size_t(1), rec.paths.size());
    CPPUNIT_ASSERT(rec.paths[0].child("svg:d"));
    CPPUNIT_ASSERT(nodesOf(*rec.paths[0].child("svg:d")) == cropNodes);
  }

  void testRotatedCropNotDoubleTransformed()
  {
    libcdr::CDRParserState state;
    state.m_pages.push_back(libcdr::CDRPage());
    addPayload(state, 13, true);
    Recorder rec;
    double theta = 30.0 * M_PI / 180.0;
    libcdr::CDRTransforms trafo;
    trafo.append(std::cos(theta), -std::sin(theta), 0.0, std::sin(theta), std::cos(theta), 0.0);

    const libcdr::CDRPath rect = rectanglePath();
    libcdr::CDRBitmapCrop crop;
    crop.m_objectId = 13;
    crop.m_hasPath = true;
    crop.m_path = rect;
    crop.m_space = libcdr::CDRBitmapCrop::PARENT;
    runObject(state, rec, rect, 13, 0.0, 2.0, 0.0, 1.0, &crop, trafo);

    CPPUNIT_ASSERT_EQUAL(size_t(1), rec.graphics.size());
    const librevenge::RVNGPropertyList &g = rec.graphics[0];
    // The image keeps its own affine...
    CPPUNIT_ASSERT(g["librevenge:rotate"]);
    // ...but the crop is parent-space and must not be transformed a second time.
    CPPUNIT_ASSERT(!g.child("vacards:cdr-image-transform"));
    CPPUNIT_ASSERT_EQUAL(std::string("parent"), stringValue(g, "vacards:cdr-crop-space"));

    const std::vector<Node> cropNodes = nodesOf(*g.child("vacards:cdr-crop-path"));
    CPPUNIT_ASSERT(!cropNodes.empty());
    // Independent oracle: rotate (0.5,0.25) once, then page y-flip (height 0).
    const double srcX = 0.5, srcY = 0.25;
    const double expX = std::cos(theta) * srcX - std::sin(theta) * srcY;
    const double expY = -(std::sin(theta) * srcX + std::cos(theta) * srcY);
    CPPUNIT_ASSERT(almostEqual(expX, cropNodes[0].x));
    CPPUNIT_ASSERT(almostEqual(expY, cropNodes[0].y));
  }

  void testImageSpaceTransform()
  {
    // Protocol branch for a crop supplied in image space. The recovered source
    // always reads parent space, so this is exercised through the producer API.
    libcdr::CDRParserState state;
    state.m_pages.push_back(libcdr::CDRPage());
    addPayload(state, 14, true);
    Recorder rec;
    const libcdr::CDRPath rect = rectanglePath();
    libcdr::CDRBitmapCrop crop;
    crop.m_objectId = 14;
    crop.m_hasPath = true;
    crop.m_path = rect;
    crop.m_space = libcdr::CDRBitmapCrop::IMAGE;
    crop.m_hasImageTransform = true;
    crop.m_imageTransform[0] = 1.0;
    crop.m_imageTransform[1] = 0.5;
    crop.m_imageTransform[2] = -0.5;
    crop.m_imageTransform[3] = 1.0;
    crop.m_imageTransform[4] = 0.125;
    crop.m_imageTransform[5] = -0.25;
    runObject(state, rec, rect, 14, 0.0, 2.0, 0.0, 1.0, &crop);

    CPPUNIT_ASSERT_EQUAL(size_t(1), rec.graphics.size());
    const librevenge::RVNGPropertyList &g = rec.graphics[0];
    CPPUNIT_ASSERT_EQUAL(std::string("image"), stringValue(g, "vacards:cdr-crop-space"));
    const librevenge::RVNGPropertyListVector *xform = g.child("vacards:cdr-image-transform");
    CPPUNIT_ASSERT(xform);
    CPPUNIT_ASSERT_EQUAL(6UL, xform->count());
    const double expected[6] = {1.0, 0.5, -0.5, 1.0, 0.125, -0.25};
    for (unsigned i = 0; i < 6; ++i)
    {
      CPPUNIT_ASSERT_EQUAL(int(i), (*xform)[i]["vacards:cdr-matrix-index"]->getInt());
      CPPUNIT_ASSERT(almostEqual(expected[i], (*xform)[i]["vacards:cdr-matrix-value"]->getDouble()));
    }
    // The crop path stays in image space (untransformed).
    const std::vector<Node> cropNodes = nodesOf(*g.child("vacards:cdr-crop-path"));
    CPPUNIT_ASSERT(!cropNodes.empty());
    CPPUNIT_ASSERT(almostEqual(0.5, cropNodes[0].x));
    CPPUNIT_ASSERT(almostEqual(0.25, cropNodes[0].y));
  }

  void testSharedPayloadIndependentCrops()
  {
    libcdr::CDRParserState state;
    state.m_pages.push_back(libcdr::CDRPage());
    addPayload(state, 21, true);
    Recorder rec;

    libcdr::CDRPath a;
    a.appendMoveTo(0.0, 0.0);
    a.appendLineTo(1.0, 0.0);
    a.appendLineTo(1.0, 1.0);
    a.appendClosePath();
    libcdr::CDRPath b;
    b.appendMoveTo(0.25, 0.25);
    b.appendLineTo(0.75, 0.25);
    b.appendLineTo(0.75, 0.75);
    b.appendClosePath();

    libcdr::CDRBitmapCrop cropA;
    cropA.m_objectId = 21; cropA.m_hasPath = true; cropA.m_path = a;
    libcdr::CDRBitmapCrop cropB;
    cropB.m_objectId = 21; cropB.m_hasPath = true; cropB.m_path = b;
    runObject(state, rec, a, 21, 0.0, 2.0, 0.0, 1.0, &cropA);
    runObject(state, rec, b, 21, 0.0, 2.0, 0.0, 1.0, &cropB);

    CPPUNIT_ASSERT_EQUAL(size_t(2), rec.graphics.size());
    CPPUNIT_ASSERT(rec.graphics[0]["office:binary-data"]);
    CPPUNIT_ASSERT(rec.graphics[1]["office:binary-data"]);
    CPPUNIT_ASSERT_EQUAL(21, rec.graphics[0]["vacards:cdr-object-id"]->getInt());
    CPPUNIT_ASSERT_EQUAL(21, rec.graphics[1]["vacards:cdr-object-id"]->getInt());
    const std::vector<Node> nodesA = nodesOf(*rec.graphics[0].child("vacards:cdr-crop-path"));
    const std::vector<Node> nodesB = nodesOf(*rec.graphics[1].child("vacards:cdr-crop-path"));
    // Independent crops: the two boundary geometries differ even though the
    // payload and source id are shared.
    CPPUNIT_ASSERT(!(nodesA == nodesB));
    CPPUNIT_ASSERT(state.m_bmps[21].size() > 0); // payload still present
  }

  void testNoAdjacencyPairing()
  {
    // A path followed by a bitmap with no explicit ownership must not be paired.
    libcdr::CDRParserState state;
    state.m_pages.push_back(libcdr::CDRPage());
    addPayload(state, 31, true);
    Recorder rec;
    runObject(state, rec, rectanglePath(), 31, 0.0, 2.0, 0.0, 1.0, nullptr);
    CPPUNIT_ASSERT_EQUAL(size_t(1), rec.graphics.size());
    const librevenge::RVNGPropertyList &g = rec.graphics[0];
    CPPUNIT_ASSERT(!g["vacards:cdr-fidelity-version"]);
    CPPUNIT_ASSERT(!g["vacards:cdr-crop-role"]);
    CPPUNIT_ASSERT(!g["vacards:cdr-crop-path"]);
    CPPUNIT_ASSERT(!g["vacards:cdr-object-id"]);
  }

  void testEmptyBoundaryNoExtension()
  {
    libcdr::CDRParserState state;
    state.m_pages.push_back(libcdr::CDRPage());
    addPayload(state, 32, true);
    Recorder rec;
    libcdr::CDRBitmapCrop crop;
    crop.m_objectId = 32;
    crop.m_hasPath = false;
    runObject(state, rec, libcdr::CDRPath(), 32, 0.0, 2.0, 0.0, 1.0, &crop);
    CPPUNIT_ASSERT_EQUAL(size_t(1), rec.graphics.size());
    CPPUNIT_ASSERT(!rec.graphics[0]["vacards:cdr-fidelity-version"]);
    CPPUNIT_ASSERT(!rec.graphics[0]["vacards:cdr-crop-path"]);
  }

  void testMalformedSingleMoveToNoExtension()
  {
    // A crop whose owned boundary is a malformed single MoveTo is removed by the
    // path simplifier, so it has no emitted geometry. It must not be labeled as
    // an explicit crop with empty fields.
    libcdr::CDRParserState state;
    state.m_pages.push_back(libcdr::CDRPage());
    addPayload(state, 33, true);
    Recorder rec;
    libcdr::CDRPath malformed;
    malformed.appendMoveTo(0.5, 0.5);
    libcdr::CDRBitmapCrop crop;
    crop.m_objectId = 33;
    crop.m_hasPath = true;
    crop.m_path = malformed;
    runObject(state, rec, malformed, 33, 0.0, 1.0, 0.0, 1.0, &crop);
    CPPUNIT_ASSERT_EQUAL(size_t(1), rec.graphics.size());
    const librevenge::RVNGPropertyList &g = rec.graphics[0];
    CPPUNIT_ASSERT(!g["vacards:cdr-fidelity-version"]);
    CPPUNIT_ASSERT(!g["vacards:cdr-object-id"]);
    CPPUNIT_ASSERT(!g["vacards:cdr-content-kind"]);
    CPPUNIT_ASSERT(!g["vacards:cdr-crop-role"]);
    CPPUNIT_ASSERT(!g["vacards:cdr-crop-space"]);
    CPPUNIT_ASSERT(!g["vacards:cdr-crop-fill-rule"]);
    CPPUNIT_ASSERT(!g["vacards:cdr-crop-path"]);
    // The malformed boundary is not emitted as a visible path either.
    CPPUNIT_ASSERT_EQUAL(size_t(0), rec.paths.size());
  }

  void testVisibleFrameRole()
  {
    // A source-visible frame (solid fill) must be reported as visible-frame,
    // not consumed as an invisible crop helper.
    libcdr::CDRParserState state;
    state.m_pages.push_back(libcdr::CDRPage());
    addPayload(state, 41, false);
    Recorder rec;
    const libcdr::CDRPath rect = rectanglePath();
    libcdr::CDRColor color(0, 0, 0x000000);
    const libcdr::CDRFillStyle fill(1, color, color, libcdr::CDRGradient(), libcdr::CDRImageFill());
    libcdr::CDRBitmapCrop crop;
    crop.m_objectId = 41;
    crop.m_hasPath = true;
    crop.m_path = rect;
    runObject(state, rec, rect, 41, 0.0, 2.0, 0.0, 1.0, &crop,
              libcdr::CDRTransforms(), 7, &fill);
    CPPUNIT_ASSERT_EQUAL(size_t(1), rec.graphics.size());
    CPPUNIT_ASSERT_EQUAL(std::string("visible-frame"),
                         stringValue(rec.graphics[0], "vacards:cdr-crop-role"));
  }

  void testVisibleFrameRolePngFillOnly()
  {
    // The emitted drawPath forces draw:fill=none for PNG image output. The role
    // must still come from the raw source style, so a solid source fill alone is
    // a visible frame and not misclassified as an invisible crop helper.
    libcdr::CDRParserState state;
    state.m_pages.push_back(libcdr::CDRPage());
    addPayload(state, 42, true); // PNG payload triggers the normalization
    Recorder rec;
    const libcdr::CDRPath rect = rectanglePath();
    libcdr::CDRColor color(0, 0, 0x000000);
    const libcdr::CDRFillStyle fill(1, color, color, libcdr::CDRGradient(), libcdr::CDRImageFill());
    libcdr::CDRBitmapCrop crop;
    crop.m_objectId = 42;
    crop.m_hasPath = true;
    crop.m_path = rect;
    runObject(state, rec, rect, 42, 0.0, 2.0, 0.0, 1.0, &crop,
              libcdr::CDRTransforms(), 9, &fill);
    CPPUNIT_ASSERT_EQUAL(size_t(1), rec.graphics.size());
    CPPUNIT_ASSERT_EQUAL(std::string("visible-frame"),
                         stringValue(rec.graphics[0], "vacards:cdr-crop-role"));
    // The PNG normalization itself is preserved on the emitted path style.
    bool sawFillNone = false;
    for (const auto &s : rec.styles)
      if (stringValue(s, "draw:fill") == "none")
        sawFillNone = true;
    CPPUNIT_ASSERT(sawFillNone);
  }

  void testGroupTransformedCrop()
  {
    // Object inside a transformed group: the parent-space crop must carry the
    // object and group transforms exactly once, in that order, like svg:d.
    libcdr::CDRParserState state;
    state.m_pages.push_back(libcdr::CDRPage());
    addPayload(state, 51, true);
    Recorder rec;
    const libcdr::CDRPath rect = rectanglePath();
    libcdr::CDRBitmapCrop crop;
    crop.m_objectId = 51;
    crop.m_hasPath = true;
    crop.m_path = rect;

    const double a = 2.0, b = 0.0, c = 0.0, d = 2.0, e = 0.1, f = 0.2;
    libcdr::CDRTransforms groupTrafo;
    // append order is (v0, v1, x0, v3, v4, y0).
    groupTrafo.append(a, b, e, c, d, f);
    // Object affine nested in the group: pure translation.
    const double oa = 1.0, ob = 0.0, oc = 0.0, od = 1.0, oe = 0.3, of = -0.1;
    libcdr::CDRTransforms objTrafo;
    objTrafo.append(oa, ob, oe, oc, od, of);
    {
      libcdr::CDRContentCollector collector(state, &rec, true);
      collector.collectGroup(1);
      collector.collectTransform(groupTrafo, true);
      collector.collectObject(2);
      collector.collectTransform(objTrafo, false);
      collector.collectPath(rect);
      collector.collectBitmap(51, 0.0, 2.0, 0.0, 1.0, &crop);
      collector.collectLevel(0);
    }

    CPPUNIT_ASSERT_EQUAL(size_t(1), rec.graphics.size());
    const std::vector<Node> cropNodes = nodesOf(*rec.graphics[0].child("vacards:cdr-crop-path"));
    CPPUNIT_ASSERT(!cropNodes.empty());
    // Independent oracle: object affine, group affine, then page y-flip (h=0).
    const double srcX = 0.5, srcY = 0.25;
    const double objX = oa * srcX + ob * srcY + oe;
    const double objY = oc * srcX + od * srcY + of;
    const double expX = a * objX + b * objY + e;
    const double expY = -(c * objX + d * objY + f);
    CPPUNIT_ASSERT(almostEqual(expX, cropNodes[0].x));
    CPPUNIT_ASSERT(almostEqual(expY, cropNodes[0].y));
    CPPUNIT_ASSERT_EQUAL(size_t(1), rec.paths.size());
    CPPUNIT_ASSERT(nodesOf(*rec.paths[0].child("svg:d")) == cropNodes);
  }

  void testShearReflectionCrop()
  {
    // Reflection plus shear: the crop must get exactly one application of the
    // object affine, verified against an independent point oracle.
    libcdr::CDRParserState state;
    state.m_pages.push_back(libcdr::CDRPage());
    addPayload(state, 52, true);
    Recorder rec;
    const libcdr::CDRPath rect = rectanglePath();
    libcdr::CDRBitmapCrop crop;
    crop.m_objectId = 52;
    crop.m_hasPath = true;
    crop.m_path = rect;

    // [a b c d e f]: horizontal reflection (a<0) plus shear (b,c nonzero).
    const double a = -1.0, b = 0.5, c = 0.25, d = 1.0, e = 0.1, f = -0.2;
    libcdr::CDRTransforms trafo;
    // append order is (v0, v1, x0, v3, v4, y0).
    trafo.append(a, b, e, c, d, f);
    runObject(state, rec, rect, 52, 0.0, 2.0, 0.0, 1.0, &crop, trafo);

    CPPUNIT_ASSERT_EQUAL(size_t(1), rec.graphics.size());
    const std::vector<Node> cropNodes = nodesOf(*rec.graphics[0].child("vacards:cdr-crop-path"));
    CPPUNIT_ASSERT(cropNodes.size() >= 2);
    auto oracle = [&](double x, double y, double &ox, double &oy)
    {
      ox = a * x + b * y + e;
      oy = -(c * x + d * y + f); // page y-flip (offset 0, height 0)
    };
    const double src[2][2] = {{0.5, 0.25}, {1.5, 0.25}};
    for (unsigned i = 0; i < 2; ++i)
    {
      double ox = 0.0, oy = 0.0;
      oracle(src[i][0], src[i][1], ox, oy);
      CPPUNIT_ASSERT(almostEqual(ox, cropNodes[i].x));
      CPPUNIT_ASSERT(almostEqual(oy, cropNodes[i].y));
    }
    CPPUNIT_ASSERT_EQUAL(size_t(1), rec.paths.size());
    CPPUNIT_ASSERT(nodesOf(*rec.paths[0].child("svg:d")) == cropNodes);
  }

  void testCompoundSubpathClosure()
  {
    // A compound boundary with two individually closed subpaths must keep both
    // contours and both closures in the crop vector.
    libcdr::CDRParserState state;
    state.m_pages.push_back(libcdr::CDRPage());
    addPayload(state, 53, true);
    Recorder rec;
    libcdr::CDRPath compound;
    compound.appendMoveTo(0.1, 0.1);
    compound.appendLineTo(0.5, 0.1);
    compound.appendLineTo(0.5, 0.5);
    compound.appendClosePath();
    compound.appendMoveTo(0.7, 0.7);
    compound.appendLineTo(1.1, 0.7);
    compound.appendLineTo(1.1, 1.1);
    compound.appendClosePath();
    libcdr::CDRBitmapCrop crop;
    crop.m_objectId = 53;
    crop.m_hasPath = true;
    crop.m_path = compound;
    runObject(state, rec, compound, 53, 0.0, 1.5, 0.0, 1.5, &crop);

    CPPUNIT_ASSERT_EQUAL(size_t(1), rec.graphics.size());
    const librevenge::RVNGPropertyListVector *cropPath =
      rec.graphics[0].child("vacards:cdr-crop-path");
    CPPUNIT_ASSERT(cropPath);
    const std::vector<Node> cropNodes = nodesOf(*cropPath);
    size_t moveCount = 0, closeCount = 0;
    for (const auto &n : cropNodes)
    {
      if (n.action == "M") ++moveCount;
      if (n.action == "Z") ++closeCount;
    }
    CPPUNIT_ASSERT_EQUAL(size_t(2), moveCount);
    CPPUNIT_ASSERT_EQUAL(size_t(2), closeCount);
    CPPUNIT_ASSERT_EQUAL(std::string("Z"), cropNodes.back().action);
    CPPUNIT_ASSERT_EQUAL(size_t(1), rec.paths.size());
    CPPUNIT_ASSERT(nodesOf(*rec.paths[0].child("svg:d")) == cropNodes);
  }

  void testLegacyPreV6NoCropOwnership()
  {
    // The pre-v6 branch leaves x2=y2=0, so its collected boundary is a known
    // degenerate rectangle. It must still go through collectPath (pre-existing
    // behavior) but must not be handed to collectBitmap as an explicit crop.
    Bytes records = parserStreamLegacy();
    libcdr::CDRInternalStream input(records);
    libcdr::CDRParserState state;
    CropProbe probe(state);
    const std::vector<std::unique_ptr<librevenge::RVNGInputStream>> external;
    libcdr::CDRParser parser(external, &probe);
    CPPUNIT_ASSERT(parser.parseRecords(&input));
    CPPUNIT_ASSERT_EQUAL(1U, probe.paths);
    CPPUNIT_ASSERT_EQUAL(1U, probe.bitmaps);
    CPPUNIT_ASSERT_EQUAL(42U, probe.imageId);
    CPPUNIT_ASSERT(!probe.hasCrop);
  }

  void testLegacyPreV6NoVacardsFields()
  {
    // The exact pre-v6 degenerate boundary the parser collects, pushed through
    // collectPath + collectBitmap with no owned crop. No vacards crop field may
    // be emitted; the boundary keeps its pre-existing visible drawPath.
    libcdr::CDRParserState state;
    state.m_pages.push_back(libcdr::CDRPage());
    addPayload(state, 42, true);
    Recorder rec;
    libcdr::CDRPath legacy;
    legacy.appendMoveTo(0.5, 0.25);
    legacy.appendLineTo(0.5, 0.0);
    legacy.appendLineTo(0.0, 0.0);
    legacy.appendLineTo(0.0, 0.25);
    legacy.appendLineTo(0.5, 0.25);
    runObject(state, rec, legacy, 42, 0.5, 0.0, 0.25, 0.0, nullptr);

    CPPUNIT_ASSERT_EQUAL(size_t(1), rec.graphics.size());
    const librevenge::RVNGPropertyList &g = rec.graphics[0];
    CPPUNIT_ASSERT(g["office:binary-data"]);
    CPPUNIT_ASSERT(!g["vacards:cdr-fidelity-version"]);
    CPPUNIT_ASSERT(!g["vacards:cdr-object-id"]);
    CPPUNIT_ASSERT(!g["vacards:cdr-content-kind"]);
    CPPUNIT_ASSERT(!g["vacards:cdr-crop-role"]);
    CPPUNIT_ASSERT(!g["vacards:cdr-crop-space"]);
    CPPUNIT_ASSERT(!g["vacards:cdr-crop-fill-rule"]);
    CPPUNIT_ASSERT(!g["vacards:cdr-crop-path"]);
    // The pre-existing collectPath emission is preserved.
    CPPUNIT_ASSERT_EQUAL(size_t(1), rec.paths.size());
    CPPUNIT_ASSERT(rec.paths[0].child("svg:d"));
  }

  // Machine-captured sanitized producer events. Binary payloads are never printed.
  void testSanitizedEventTrace()
  {
    libcdr::CDRParserState state;
    state.m_pages.push_back(libcdr::CDRPage());
    addPayload(state, 77, true);
    Recorder rec;
    const libcdr::CDRPath rect = rectanglePath();
    libcdr::CDRBitmapCrop crop;
    crop.m_objectId = 77;
    crop.m_hasPath = true;
    crop.m_path = rect;
    runObject(state, rec, rect, 77, 0.0, 2.0, 0.0, 1.0, &crop);
    CPPUNIT_ASSERT_EQUAL(size_t(1), rec.graphics.size());
    const librevenge::RVNGPropertyList &g = rec.graphics[0];
    std::cout << "CDR_CROP_TRACE drawPath nodes="
              << nodesOf(*rec.paths[0].child("svg:d")).size() << " payload=<none>" << std::endl;
    std::cout << "CDR_CROP_TRACE drawGraphicObject"
              << " version=" << g["vacards:cdr-fidelity-version"]->getInt()
              << " object-id=" << g["vacards:cdr-object-id"]->getInt()
              << " content-kind=" << stringValue(g, "vacards:cdr-content-kind")
              << " crop-role=" << stringValue(g, "vacards:cdr-crop-role")
              << " crop-space=" << stringValue(g, "vacards:cdr-crop-space")
              << " crop-fill-rule=" << stringValue(g, "vacards:cdr-crop-fill-rule")
              << " crop-nodes=" << nodesOf(*g.child("vacards:cdr-crop-path")).size()
              << " image-transform=<absent>"
              << " payload=<redacted>" << std::endl;
  }
};

CPPUNIT_TEST_SUITE_REGISTRATION(CDRCropTransportTest);
