// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef SEEN_INKSCAPE_IO_GZIPSTREAM_H
#define SEEN_INKSCAPE_IO_GZIPSTREAM_H
/**
 * @file
 * Zlib-enabled input and output streams.
 *
 * This is a thin wrapper of libz calls, in order
 * to provide a simple interface to our developers
 * for gzip input and output.
 */
/*
 * Authors:
 *   Bob Jamison <rjamison@titan.com>
 *
 * Copyright (C) 2004 Inkscape.org
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include <cstddef>
#include <cstdint>
#include <vector>
#include "inkscapestream.h"
#include <zlib.h>

namespace Inkscape
{
namespace IO
{

//#########################################################################
//# G Z I P    I N P U T    S T R E A M
//#########################################################################

/**
 * This class is for deflating a gzip-compressed InputStream source
 *
 */
class GzipInputStream : public BasicInputStream
{

public:

    GzipInputStream(InputStream &sourceStream);
    
    ~GzipInputStream() override;
    
    int available() override;
    
    void close() override;
    
    int get() override;

    // Sticky integrity result for callers that must distinguish a valid EOF
    // from truncated/corrupt compressed input. get() keeps its legacy API.
    bool hadError() const noexcept { return error_; }
    
private:

    bool load();
    int fetchMore();

    bool loaded;
    bool finished_ = false;
    bool error_ = false;
    bool inflater_active_ = false;
    
    unsigned char *outputBuf;
    unsigned char *srcBuf;

    unsigned long crc;
    unsigned long srcCrc;
    unsigned long srcSiz;
    unsigned long srcLen;
    long outputBufPos;
    long outputBufLen;

    z_stream d_stream;
}; // class GzipInputStream




//#########################################################################
//# G Z I P    O U T P U T    S T R E A M
//#########################################################################

/**
 * This class is for gzip-compressing data going to the
 * destination OutputStream
 *
 */
class GzipOutputStream : public BasicOutputStream
{

public:

    GzipOutputStream(OutputStream &destinationStream);
    
    ~GzipOutputStream() override;
    
    void close() override;
    // Stop after an upstream serializer exception. Never finish or append a trailer.
    void abandon() noexcept;
    
    void flush() override;
    
    int put(char ch) override;

private:

    // Explicit checked finalization. checked == true propagates StreamException;
    // checked == false is the non-throwing destructor path. Both are idempotent
    // and never clear a latched failure.
    void finalize(bool checked);

    void endZstream() noexcept;
    void deflateAndWrite(int flushMode);
    void feedInput();
    void writeBytes(const unsigned char *data, std::size_t len);
    void writeTrailer();
    void throwIfLatched() const;

    // Bounded pending-input staging: deflated with Z_NO_FLUSH once it reaches
    // the fixed chunk size, so memory does not grow with the total input.
    std::vector<unsigned char> inputBuf;

    // 64-bit counters so >4 GiB input cannot truncate or overflow (notably on
    // Windows, where long is 32-bit). ISIZE is emitted modulo 2^32.
    std::uint64_t totalIn;
    std::uint64_t totalOut;
    std::uint32_t crc;

    // Persistent raw-deflate state so a flush can be continued without emitting
    // a second, independent deflate body.
    z_stream d_stream;
    bool zstreamActive;
    bool error_;

}; // class GzipOutputStream

// The gzip ISIZE trailer field is the uncompressed input size modulo 2^32.
// Exposed so the >4 GiB wrap boundary can be unit-tested without allocating
// >4 GiB (which is not feasible on the target Mac).
std::uint32_t gzipIsizeModulo32(std::uint64_t totalIn);







} // namespace IO
} // namespace Inkscape


#endif // SEEN_INKSCAPE_IO_GZIPSTREAM_H
