/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/*
 * This file is part of the libcdr project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "CDRStylesCollector.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <stdint.h>
#include <vector>
#include <zlib.h>

#include "libcdr_utils.h"

#ifndef DUMP_IMAGE
#define DUMP_IMAGE 0
#endif

namespace
{

void appendU32BE(std::vector<unsigned char> &buffer, uint32_t value)
{
  buffer.push_back(static_cast<unsigned char>((value >> 24) & 0xff));
  buffer.push_back(static_cast<unsigned char>((value >> 16) & 0xff));
  buffer.push_back(static_cast<unsigned char>((value >> 8) & 0xff));
  buffer.push_back(static_cast<unsigned char>(value & 0xff));
}

bool appendPNGChunk(std::vector<unsigned char> &png, const char type[4], const unsigned char *data, size_t size)
{
  if (size > std::numeric_limits<uint32_t>::max() || size > std::numeric_limits<uInt>::max() - 4)
    return false;

  appendU32BE(png, static_cast<uint32_t>(size));
  const size_t crcStart = png.size();
  png.insert(png.end(), type, type + 4);
  if (size)
    png.insert(png.end(), data, data + size);

  uLong crc = crc32(0L, Z_NULL, 0);
  crc = crc32(crc, reinterpret_cast<const Bytef *>(&png[crcStart]), static_cast<uInt>(4 + size));
  appendU32BE(png, static_cast<uint32_t>(crc));
  return true;
}

bool getRGBPixel(libcdr::CDRParserState &ps, unsigned colorModel, unsigned width, unsigned height, unsigned bpp,
                 const std::vector<unsigned> &palette, const std::vector<unsigned char> &bitmap,
                 size_t lineWidth, unsigned row, unsigned column, unsigned &rgb)
{
  if (!height || row >= height || column >= width)
    return false;
  if (!lineWidth)
    return false;

  unsigned color = 0;
  if (colorModel == 6)
  {
    const size_t offset = static_cast<size_t>(row) * lineWidth + column / 8;
    if (column / 8 >= lineWidth || offset >= bitmap.size())
      return false;
    rgb = bitmap[offset] & (0x80 >> (column % 8)) ? 0xffffff : 0;
    return true;
  }
  if (colorModel == 5)
  {
    const size_t offset = static_cast<size_t>(row) * lineWidth + column;
    if (column >= lineWidth || offset >= bitmap.size())
      return false;
    color = bitmap[offset];
  }
  else if (!palette.empty())
  {
    const size_t offset = static_cast<size_t>(row) * lineWidth + column;
    if (column >= lineWidth || offset >= bitmap.size())
      return false;
    const size_t paletteIndex = std::min<size_t>(bitmap[offset], palette.size() - 1);
    color = palette[paletteIndex];
  }
  else if (bpp == 24)
  {
    const size_t columnOffset = static_cast<size_t>(column) * 3;
    const size_t offset = static_cast<size_t>(row) * lineWidth + columnOffset;
    if (columnOffset + 2 >= lineWidth || offset + 2 >= bitmap.size())
      return false;
    color = (static_cast<unsigned>(bitmap[offset + 2]) << 16) |
            (static_cast<unsigned>(bitmap[offset + 1]) << 8) |
            static_cast<unsigned>(bitmap[offset]);
  }
  else if (bpp == 32)
  {
    const size_t columnOffset = static_cast<size_t>(column) * 4;
    const size_t offset = static_cast<size_t>(row) * lineWidth + columnOffset;
    if (columnOffset + 3 >= lineWidth || offset + 3 >= bitmap.size())
      return false;
    color = (static_cast<unsigned>(bitmap[offset + 3]) << 24) |
            (static_cast<unsigned>(bitmap[offset + 2]) << 16) |
            (static_cast<unsigned>(bitmap[offset + 1]) << 8) |
            static_cast<unsigned>(bitmap[offset]);
  }
  else
    return false;

  rgb = ps.getBMPColor(libcdr::CDRColor(static_cast<unsigned short>(colorModel), color));
  return true;
}

uint64_t filterScore(const std::vector<unsigned char> &row)
{
  uint64_t score = 0;
  for (auto value : row)
    score += std::min<unsigned>(value, 256U - value);
  return score;
}

void choosePNGFilter(const std::vector<unsigned char> &row, const std::vector<unsigned char> &previous,
                     size_t bytesPerPixel, std::vector<unsigned char> &filtered,
                     std::vector<unsigned char> &candidate)
{
  filtered = row;
  unsigned char filter = 0;
  uint64_t bestScore = filterScore(filtered);

  for (size_t i = 0; i < row.size(); ++i)
    candidate[i] = static_cast<unsigned char>(row[i] - (i >= bytesPerPixel ? row[i - bytesPerPixel] : 0));
  uint64_t score = filterScore(candidate);
  if (score < bestScore)
  {
    filtered.swap(candidate);
    bestScore = score;
    filter = 1;
  }

  for (size_t i = 0; i < row.size(); ++i)
    candidate[i] = static_cast<unsigned char>(row[i] - previous[i]);
  score = filterScore(candidate);
  if (score < bestScore)
  {
    filtered.swap(candidate);
    filter = 2;
  }

  filtered.insert(filtered.begin(), filter);
}

bool appendDeflated(z_stream &stream, const unsigned char *data, size_t size, int flush,
                    std::vector<unsigned char> &compressed)
{
  if (size > std::numeric_limits<uInt>::max())
    return false;
  stream.next_in = const_cast<Bytef *>(reinterpret_cast<const Bytef *>(data));
  stream.avail_in = static_cast<uInt>(size);

  do
  {
    unsigned char output[256 * 1024];
    stream.next_out = output;
    stream.avail_out = sizeof(output);
    const int result = deflate(&stream, flush);
    if (result != Z_OK && result != Z_STREAM_END)
      return false;
    compressed.insert(compressed.end(), output, output + sizeof(output) - stream.avail_out);
    if (result == Z_STREAM_END)
      return true;
  }
  while (stream.avail_in || (flush == Z_FINISH && stream.avail_out == 0));

  return flush != Z_FINISH;
}

uint64_t bitmapPixelLimit()
{
  const char *value = std::getenv("LIBCDR_MAX_BITMAP_PIXELS");
  if (!value || !*value)
    return 0;

  char *end = nullptr;
  const unsigned long long limit = std::strtoull(value, &end, 10);
  return end && !*end ? static_cast<uint64_t>(limit) : 0;
}

bool createPNG(libcdr::CDRParserState &ps, librevenge::RVNGBinaryData &image,
               unsigned colorModel, unsigned width, unsigned height, unsigned bpp,
               const std::vector<unsigned> &palette, const std::vector<unsigned char> &bitmap,
               unsigned alphaWidth = 0, unsigned alphaHeight = 0, unsigned alphaBpp = 0,
               const std::vector<unsigned char> *alpha = nullptr)
{
  const bool hasAlpha = alpha && !alpha->empty();
  if (!width || !height || bitmap.empty())
    return false;
  if (hasAlpha && (alphaBpp != 8 || alphaWidth < width || alphaHeight != height))
    return false;

  const size_t lineWidth = bitmap.size() / height;
  const size_t alphaLineWidth = hasAlpha ? alpha->size() / alphaHeight : 0;
  if (!lineWidth || (hasAlpha && alphaLineWidth < alphaWidth))
    return false;

  unsigned outputWidth = width;
  unsigned outputHeight = height;
  const uint64_t pixelLimit = bitmapPixelLimit();
  const uint64_t sourcePixels = static_cast<uint64_t>(width) * height;
  if (pixelLimit && sourcePixels > pixelLimit)
  {
    const long double scale = std::sqrt(static_cast<long double>(pixelLimit) / sourcePixels);
    outputWidth = std::max(1U, static_cast<unsigned>(width * scale));
    outputHeight = std::max(1U, static_cast<unsigned>(height * scale));
    while (static_cast<uint64_t>(outputWidth) * outputHeight > pixelLimit)
    {
      if (outputWidth >= outputHeight && outputWidth > 1)
        --outputWidth;
      else if (outputHeight > 1)
        --outputHeight;
      else
        break;
    }
  }

  const size_t channels = hasAlpha ? 4 : 3;
  if (outputWidth > std::numeric_limits<size_t>::max() / channels)
    return false;
  const size_t rowSize = static_cast<size_t>(outputWidth) * channels;
  std::vector<unsigned char> row(rowSize);
  std::vector<unsigned char> previous(rowSize, 0);
  std::vector<unsigned char> filtered(rowSize);
  std::vector<unsigned char> candidate(rowSize);
  std::vector<unsigned char> compressed;
  compressed.reserve(std::min<size_t>(bitmap.size(), 16 * 1024 * 1024));

  z_stream stream = {};
  if (deflateInit(&stream, Z_BEST_SPEED) != Z_OK)
    return false;

  bool success = true;
  for (unsigned outputRow = 0; outputRow < outputHeight && success; ++outputRow)
  {
    // Corel stores scanlines bottom-up like a Windows DIB; PNG is top-down.
    const unsigned displayRow = static_cast<unsigned>(static_cast<uint64_t>(outputRow) * height / outputHeight);
    const unsigned sourceRow = height - displayRow - 1;
    size_t output = 0;
    for (unsigned column = 0; column < outputWidth; ++column)
    {
      const unsigned sourceColumn = static_cast<unsigned>(static_cast<uint64_t>(column) * width / outputWidth);
      unsigned rgb = 0;
      if (!getRGBPixel(ps, colorModel, width, height, bpp, palette, bitmap,
                       lineWidth, sourceRow, sourceColumn, rgb))
      {
        success = false;
        break;
      }
      row[output++] = static_cast<unsigned char>((rgb >> 16) & 0xff);
      row[output++] = static_cast<unsigned char>((rgb >> 8) & 0xff);
      row[output++] = static_cast<unsigned char>(rgb & 0xff);
      if (hasAlpha)
      {
        const size_t alphaOffset = static_cast<size_t>(sourceRow) * alphaLineWidth + sourceColumn;
        if (alphaOffset >= alpha->size())
        {
          success = false;
          break;
        }
        row[output++] = (*alpha)[alphaOffset];
      }
    }
    if (!success)
      break;

    choosePNGFilter(row, previous, channels, filtered, candidate);
    success = appendDeflated(stream, filtered.data(), filtered.size(), Z_NO_FLUSH, compressed);
    previous.swap(row);
  }

  if (success)
    success = appendDeflated(stream, nullptr, 0, Z_FINISH, compressed);
  deflateEnd(&stream);
  if (!success || compressed.empty())
    return false;

  std::vector<unsigned char> png;
  png.reserve(compressed.size() + 64);
  static const unsigned char signature[] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
  png.insert(png.end(), signature, signature + sizeof(signature));

  std::vector<unsigned char> header;
  appendU32BE(header, outputWidth);
  appendU32BE(header, outputHeight);
  header.push_back(8); // bit depth
  header.push_back(hasAlpha ? 6 : 2); // RGBA or RGB
  header.push_back(0); // compression
  header.push_back(0); // filter
  header.push_back(0); // no interlace
  if (!appendPNGChunk(png, "IHDR", &header[0], header.size()) ||
      !appendPNGChunk(png, "IDAT", &compressed[0], compressed.size()) ||
      !appendPNGChunk(png, "IEND", nullptr, 0))
    return false;

  image = librevenge::RVNGBinaryData(&png[0], png.size());
  return true;
}

}


libcdr::CDRStylesCollector::CDRStylesCollector(libcdr::CDRParserState &ps) :
  m_ps(ps), m_page(8.5, 11.0, -4.25, -5.5)
{
}

libcdr::CDRStylesCollector::~CDRStylesCollector()
{
}

void libcdr::CDRStylesCollector::collectBmp(unsigned imageId, unsigned colorModel, unsigned width, unsigned height, unsigned bpp, const std::vector<unsigned> &palette, const std::vector<unsigned char> &bitmap)
{
  librevenge::RVNGBinaryData image;
  if (createPNG(m_ps, image, colorModel, width, height, bpp, palette, bitmap))
  {
#if DUMP_IMAGE
    librevenge::RVNGString filename;
    filename.sprintf("bitmap%.8x.png", imageId);
    FILE *f = fopen(filename.cstr(), "wb");
    if (f)
    {
      const unsigned char *tmpBuffer = image.getDataBuffer();
      for (unsigned long k = 0; k < image.size(); k++)
        fprintf(f, "%c",tmpBuffer[k]);
      fclose(f);
    }
#endif

    m_ps.m_bmps[imageId] = image;
  }
}

void libcdr::CDRStylesCollector::collectBmp(unsigned imageId, unsigned colorModel, unsigned width, unsigned height, unsigned bpp, const std::vector<unsigned> &palette, const std::vector<unsigned char> &bitmap,
                                             unsigned alphaWidth, unsigned alphaHeight, unsigned alphaBpp, const std::vector<unsigned char> &alpha)
{
  librevenge::RVNGBinaryData image;
  if (!createPNG(m_ps, image, colorModel, width, height, bpp, palette, bitmap,
                 alphaWidth, alphaHeight, alphaBpp, &alpha))
  {
    collectBmp(imageId, colorModel, width, height, bpp, palette, bitmap);
    return;
  }

#if DUMP_IMAGE
  librevenge::RVNGString filename;
  filename.sprintf("bitmap%.8x.png", imageId);
  FILE *f = fopen(filename.cstr(), "wb");
  if (f)
  {
    const unsigned char *tmpBuffer = image.getDataBuffer();
    for (unsigned long k = 0; k < image.size(); k++)
      fprintf(f, "%c", tmpBuffer[k]);
    fclose(f);
  }
#endif

  m_ps.m_bmps[imageId] = image;
}

void libcdr::CDRStylesCollector::collectBmp(unsigned imageId, const std::vector<unsigned char> &bitmap)
{
  librevenge::RVNGBinaryData image(&bitmap[0], bitmap.size());
#if DUMP_IMAGE
  librevenge::RVNGString filename;
  filename.sprintf("bitmap%.8x.bmp", imageId);
  FILE *f = fopen(filename.cstr(), "wb");
  if (f)
  {
    const unsigned char *tmpBuffer = image.getDataBuffer();
    for (unsigned long k = 0; k < image.size(); k++)
      fprintf(f, "%c",tmpBuffer[k]);
    fclose(f);
  }
#endif

  m_ps.m_bmps[imageId] = image;
}

void libcdr::CDRStylesCollector::collectPageSize(double width, double height, double offsetX, double offsetY)
{
  if (m_ps.m_pages.empty())
    m_page = CDRPage(width, height, offsetX, offsetY);
  else
    m_ps.m_pages.back() = CDRPage(width, height, offsetX, offsetY);
}

void libcdr::CDRStylesCollector::collectPage(unsigned /* level */)
{
  m_ps.m_pages.push_back(m_page);
}

void libcdr::CDRStylesCollector::collectBmpf(unsigned patternId, unsigned width, unsigned height, const std::vector<unsigned char> &pattern)
{
  m_ps.m_patterns[patternId] = CDRPattern(width, height, pattern);
}

void libcdr::CDRStylesCollector::collectColorProfile(const std::vector<unsigned char> &profile)
{
  if (!profile.empty())
    m_ps.setColorTransform(profile);
}

void libcdr::CDRStylesCollector::collectPaletteEntry(unsigned colorId, unsigned /* userId */, const libcdr::CDRColor &color)
{
  m_ps.m_documentPalette[colorId] = color;
}

void libcdr::CDRStylesCollector::collectText(unsigned textId, unsigned styleId, const std::vector<unsigned char> &data,
                                             const std::vector<unsigned char> &charDescriptions, const std::map<unsigned, CDRStyle> &styleOverrides)
{
  if (data.empty() && styleOverrides.empty())
    return;

  unsigned char tmpCharDescription = 0;
  unsigned i = 0;
  unsigned j = 0;
  std::vector<unsigned char> tmpTextData;
  CDRStyle defaultCharStyle, tmpCharStyle;
  m_ps.getRecursedStyle(defaultCharStyle, styleId);

  CDRTextLine line;
  for (i=0, j=0; i<charDescriptions.size() && j<data.size(); ++i)
  {
    tmpCharStyle = defaultCharStyle;
    auto iter = styleOverrides.find(tmpCharDescription & 0xfe);
    if (iter != styleOverrides.end())
      tmpCharStyle.overrideStyle(iter->second);
    if (charDescriptions[i] != tmpCharDescription)
    {
      librevenge::RVNGString text;
      if (!tmpTextData.empty())
      {
        if (tmpCharDescription & 0x01)
          appendCharacters(text, tmpTextData);
        else
          appendCharacters(text, tmpTextData, tmpCharStyle.m_charSet);
      }
      line.append(CDRText(text, tmpCharStyle));
      tmpTextData.clear();
      tmpCharDescription = charDescriptions[i];

    }
    tmpTextData.push_back(data[j++]);
    if ((tmpCharDescription & 0x01) && (j < data.size()))
      tmpTextData.push_back(data[j++]);
  }
  librevenge::RVNGString text;
  if (!tmpTextData.empty())
  {
    // The style above was resolved from the previous character description at
    // the top of the last loop iteration; the loop then advanced
    // tmpCharDescription to the final run. Re-resolve it for the trailing run
    // so the flush uses the last run's own style instead of the previous one.
    tmpCharStyle = defaultCharStyle;
    auto iter = styleOverrides.find(tmpCharDescription & 0xfe);
    if (iter != styleOverrides.end())
      tmpCharStyle.overrideStyle(iter->second);

    if (tmpCharDescription & 0x01)
      appendCharacters(text, tmpTextData);
    else
      appendCharacters(text, tmpTextData, tmpCharStyle.m_charSet);
  }
  line.append(CDRText(text, tmpCharStyle));
  CDR_DEBUG_MSG(("CDRStylesCollector::collectText - Text: %s\n", text.cstr()));

  std::vector<CDRTextLine> &paragraphVector = m_ps.m_texts[textId];
  paragraphVector.push_back(line);
}

void libcdr::CDRStylesCollector::collectStld(unsigned id, const CDRStyle &style)
{
  m_ps.m_styles[id] = style;
}

void libcdr::CDRStylesCollector::collectFillStyle(unsigned id, const CDRFillStyle &fillStyle)
{
  m_ps.m_fillStyles[id] = fillStyle;
}

void libcdr::CDRStylesCollector::collectLineStyle(unsigned id, const CDRLineStyle &lineStyle)
{
  m_ps.m_lineStyles[id] = lineStyle;
}

/* vim:set shiftwidth=2 softtabstop=2 expandtab: */
