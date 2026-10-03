// SPDX-License-Identifier: LGPL-2.1-or-later
/** @file
 * Zlib-enabled input and output streams
 *//*
 * Authors:
 * see git history
 * Bob Jamison <rjamison@titan.com>
 * 
 *
 * Copyright (C) 2018 Authors
 * Released under GNU LGPL v2.1+, read the file 'COPYING' for more information.
 */
/*
 * This is a thin wrapper of libz calls, in order
 * to provide a simple interface to our developers
 * for gzip input and output.
 */

#include "gzipstream.h"
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace Inkscape
{
namespace IO
{

//#########################################################################
//# G Z I P    I N P U T    S T R E A M
//#########################################################################

#define OUT_SIZE 4000

/**
 *
 */ 
GzipInputStream::GzipInputStream(InputStream &sourceStream)
                    : BasicInputStream(sourceStream),
                      loaded(false),
                      outputBuf(nullptr),
                      srcBuf(nullptr),
                      crc(0),
                      srcCrc(0),
                      srcSiz(0),
                      srcLen(0),
                      outputBufPos(0),
                      outputBufLen(0)
{
    memset( &d_stream, 0, sizeof(d_stream) );
}

/**
 *
 */ 
GzipInputStream::~GzipInputStream()
{
    close();
    if ( srcBuf ) {
      delete[] srcBuf;
      srcBuf = nullptr;
    }
    if ( outputBuf ) {
        delete[] outputBuf;
        outputBuf = nullptr;
    }
}

/**
 * Returns the number of bytes that can be read (or skipped over) from
 * this input stream without blocking by the next caller of a method for
 * this input stream.
 */ 
int GzipInputStream::available()
{
    if (closed || !outputBuf)
        return 0;
    return outputBufLen - outputBufPos;
}

    
/**
 *  Closes this input stream and releases any system resources
 *  associated with the stream.
 */ 
void GzipInputStream::close()
{
    if (closed)
        return;

    if (inflater_active_) {
        int const zerr = inflateEnd(&d_stream);
        if (zerr != Z_OK) {
            printf("inflateEnd: Some kind of problem: %d\n", zerr);
        }
        inflater_active_ = false;
    }

    if ( srcBuf ) {
      delete[] srcBuf;
      srcBuf = nullptr;
    }
    if ( outputBuf ) {
        delete[] outputBuf;
        outputBuf = nullptr;
    }
    closed = true;
}
    
/**
 * Reads the next byte of data from the input stream.  -1 if EOF
 */ 
int GzipInputStream::get()
{
    int ch = -1;
    if (closed) {
        // leave return value -1
    }
    else if (!loaded && !load()) {
        error_ = true;
        close();
    } else {
        loaded = true;

        if ( outputBufPos >= outputBufLen ) {
            // time to read more, if we can
            fetchMore();
        }

        if ( outputBufPos < outputBufLen ) {
            ch = (int)outputBuf[outputBufPos++];
        }
    }

    return ch;
}

#define FTEXT 0x01
#define FHCRC 0x02
#define FEXTRA 0x04
#define FNAME 0x08
#define FCOMMENT 0x10

bool GzipInputStream::load()
{
    crc = crc32(0L, Z_NULL, 0);
    
    std::vector<Byte> inputBuf;
    while (true)
        {
        int ch = source.get();
        if (ch<0)
            break;
        inputBuf.push_back(static_cast<Byte>(ch & 0xff));
        }
    long inputBufLen = inputBuf.size();
    
    if (inputBufLen < 19) //header + tail + 1
        {
        return false;
        }

    srcLen = inputBuf.size();
    srcBuf = new (std::nothrow) Byte [srcLen];
    if (!srcBuf) {
        return false;
    }

    outputBuf = new (std::nothrow) unsigned char [OUT_SIZE];
    if ( !outputBuf ) {
        delete[] srcBuf;
        srcBuf = nullptr;
        return false;
    }
    outputBufLen = 0; // Not filled in yet

    std::vector<unsigned char>::iterator iter;
    Bytef *p = srcBuf;
    for (iter=inputBuf.begin() ; iter != inputBuf.end() ; ++iter)
	{
        *p++ = *iter;
	}

    size_t headerLen = 10;

    int flags = static_cast<int>(srcBuf[3]);

    constexpr size_t size_XLEN = 2;
    constexpr size_t size_CRC16 = 2;
    constexpr size_t size_CRC32 = 4;
    constexpr size_t size_ISIZE = 4;

    auto const check_not_truncated = [&] { return headerLen + size_CRC32 + size_ISIZE <= srcLen; };

    auto const skip_n = [&](size_t n) {
        headerLen += n;
        return check_not_truncated();
    };

    auto const skip_zero_terminated = [&] {
        while (headerLen < srcLen && srcBuf[headerLen++]) {
        }
        return check_not_truncated();
    };

    if (flags & FEXTRA) {
        if (!skip_n(size_XLEN)) {
            return false;
        }
        auto const xlen = size_t(srcBuf[headerLen - 2]) | //
                          size_t(srcBuf[headerLen - 1] << 8);
        if (!skip_n(xlen)) {
            return false;
        }
    }

    if ((flags & FNAME) && !skip_zero_terminated()) {
        return false;
    }

    if ((flags & FCOMMENT) && !skip_zero_terminated()) {
        return false;
    }

    if ((flags & FHCRC) && !skip_n(size_CRC16)) {
        return false;
    }

    if (!check_not_truncated()) {
        return false;
    }

    srcCrc = ((0x0ff & srcBuf[srcLen - 5]) << 24)
           | ((0x0ff & srcBuf[srcLen - 6]) << 16)
           | ((0x0ff & srcBuf[srcLen - 7]) <<  8)
           | ((0x0ff & srcBuf[srcLen - 8]) <<  0);
    //printf("srcCrc:%lx\n", srcCrc);
    
    srcSiz = ((0x0ff & srcBuf[srcLen - 1]) << 24)
           | ((0x0ff & srcBuf[srcLen - 2]) << 16)
           | ((0x0ff & srcBuf[srcLen - 3]) <<  8)
           | ((0x0ff & srcBuf[srcLen - 4]) <<  0);
    //printf("srcSiz:%lx/%ld\n", srcSiz, srcSiz);
    
    //outputBufLen = srcSiz + srcSiz/100 + 14;
    
    unsigned char *data = srcBuf + headerLen;
    unsigned long dataLen = srcLen - (headerLen + 8);
    //printf("%x %x\n", data[0], data[dataLen-1]);
    
    d_stream.zalloc    = (alloc_func)nullptr;
    d_stream.zfree     = (free_func)nullptr;
    d_stream.opaque    = (voidpf)nullptr;
    d_stream.next_in   = data;
    d_stream.avail_in  = dataLen;
    d_stream.next_out  = outputBuf;
    d_stream.avail_out = OUT_SIZE;
    
    int zerr = inflateInit2(&d_stream, -MAX_WBITS);
    if ( zerr == Z_OK )
    {
        inflater_active_ = true;
        zerr = fetchMore();
    } else {
        printf("inflateInit2: Some kind of problem: %d\n", zerr);
    }

        
    return (zerr == Z_OK) || (zerr == Z_STREAM_END);
}


int GzipInputStream::fetchMore()
{
    if (finished_) {
        return Z_STREAM_END;
    }
    // TODO assumes we aren't called till the buffer is empty
    d_stream.next_out  = outputBuf;
    d_stream.avail_out = OUT_SIZE;
    outputBufLen = 0;
    outputBufPos = 0;

    int zerr = inflate( &d_stream, Z_SYNC_FLUSH );
    if ( zerr == Z_OK || zerr == Z_STREAM_END ) {
        outputBufLen = OUT_SIZE - d_stream.avail_out;
        if ( outputBufLen ) {
            crc = crc32(crc, const_cast<const Bytef *>(outputBuf), outputBufLen);
        }
        //printf("crc:%lx\n", crc);
//     } else if ( zerr != Z_STREAM_END ) {
//         // TODO check to be sure this won't happen for partial end reads
//         printf("inflate: Some kind of problem: %d\n", zerr);
        if (zerr == Z_STREAM_END) {
            finished_ = true;
            if (static_cast<std::uint32_t>(crc) != static_cast<std::uint32_t>(srcCrc)
                || static_cast<std::uint32_t>(d_stream.total_out) != static_cast<std::uint32_t>(srcSiz)) {
                error_ = true;
            }
        }
    } else {
        error_ = true;
    }

    return zerr;
}

//#########################################################################
//# G Z I P   O U T P U T    S T R E A M
//#########################################################################

// Maximum pending-input chunk deflated with Z_NO_FLUSH. Keeps memory bounded
// (independent of total stream size) and always fits in a zlib uInt.
static constexpr std::size_t kInputChunk = 32 * 1024;

/**
 *
 */ 
GzipOutputStream::GzipOutputStream(OutputStream &destinationStream)
                     : BasicOutputStream(destinationStream)
{

    totalIn         = 0;
    totalOut        = 0;
    crc             = 0;

    std::memset(&d_stream, 0, sizeof(d_stream));
    zstreamActive   = false;
    error_          = false;

    //Gzip header: magic, deflate method, no flags, zero mtime, zero xflags,
    //zero OS. Routed through the checked writer because a destination may
    //report failure by return value rather than by throwing. No zlib state
    //exists yet, so a throw from here leaks nothing.
    unsigned char const header[10] = {
        0x1f, 0x8b, Z_DEFLATED, 0, 0, 0, 0, 0, 0, 0
    };
    writeBytes(header, sizeof(header));

    // Persistent raw-deflate state. -MAX_WBITS yields the raw deflate body the
    // gzip framing (header written above, CRC/ISIZE trailer at close) expects,
    // and lets a flush be continued instead of concatenating body streams.
    int const zerr = deflateInit2(&d_stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
                                  -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
    if (zerr != Z_OK) {
        throw StreamException("GzipOutputStream: deflateInit2 failed");
    }
    zstreamActive = true;

}

/**
 *
 */ 
GzipOutputStream::~GzipOutputStream()
{
    // Never throws and never retries a failed explicit finalization.
    finalize(false);
}

/**
 * Closes this output stream and releases any system resources
 * associated with this stream.
 *
 * Checked completion: compression, the gzip trailer and the destination
 * flush/close must all succeed, otherwise StreamException is thrown. The
 * operation is idempotent and a latched failure is never cleared.
 */ 
void GzipOutputStream::close()
{
    finalize(true);
}

void GzipOutputStream::abandon() noexcept
{
    error_ = true;
    closed = true;
    inputBuf.clear();
    endZstream();
}

void GzipOutputStream::throwIfLatched() const
{
    if (error_) {
        throw StreamException("GzipOutputStream: stream has already failed");
    }
}

void GzipOutputStream::endZstream() noexcept
{
    if (zstreamActive) {
        deflateEnd(&d_stream);
        zstreamActive = false;
    }
}

void GzipOutputStream::feedInput()
{
    if (inputBuf.empty()) {
        d_stream.next_in = Z_NULL;
        d_stream.avail_in = 0;
        return;
    }

    // Staging is bounded by kInputChunk, but clamp defensively so the uInt cast
    // can never truncate. CRC is accumulated exactly once per byte here, at the
    // single point where a chunk is handed to zlib.
    std::size_t const len = inputBuf.size() > static_cast<std::size_t>(UINT_MAX)
                                ? static_cast<std::size_t>(UINT_MAX)
                                : inputBuf.size();
    d_stream.next_in = reinterpret_cast<Bytef *>(inputBuf.data());
    d_stream.avail_in = static_cast<uInt>(len);
    crc = static_cast<std::uint32_t>(crc32(crc, d_stream.next_in, static_cast<uInt>(len)));
}

void GzipOutputStream::writeBytes(const unsigned char *data, std::size_t len)
{
    for (std::size_t i = 0; i < len; ++i) {
        // A destination may report failure by return value instead of throwing.
        if (destination.put(static_cast<char>(data[i])) < 0) {
            error_ = true;
            throw StreamException("GzipOutputStream: destination write failed");
        }
    }
    totalOut += static_cast<std::uint64_t>(len);
}

std::uint32_t gzipIsizeModulo32(std::uint64_t totalIn)
{
    return static_cast<std::uint32_t>(totalIn & 0xffffffffULL);
}

void GzipOutputStream::writeTrailer()
{
    unsigned char trailer[8];

    std::uint32_t outlong = crc;
    for (int n = 0; n < 4; ++n) {
        trailer[n] = static_cast<unsigned char>(outlong & 0xff);
        outlong >>= 8;
    }

    outlong = gzipIsizeModulo32(totalIn);
    for (int n = 0; n < 4; ++n) {
        trailer[4 + n] = static_cast<unsigned char>(outlong & 0xff);
        outlong >>= 8;
    }

    writeBytes(trailer, sizeof(trailer));
}

/**
 * Drive deflate over the pending input using a fixed-size output stage, so
 * memory stays bounded regardless of input size. The loop repeats until the
 * requested flush level has been fully emitted.
 */
void GzipOutputStream::deflateAndWrite(int flushMode)
{
    constexpr std::size_t kOutChunk = 16 * 1024;
    Bytef out[kOutChunk];

    for (;;) {
        d_stream.next_out = out;
        d_stream.avail_out = static_cast<uInt>(kOutChunk);

        int const zerr = ::deflate(&d_stream, flushMode);

        std::size_t const produced = kOutChunk - d_stream.avail_out;
        if (produced != 0) {
            writeBytes(out, produced);
        }

        if (flushMode == Z_FINISH) {
            if (zerr == Z_STREAM_END) {
                break;
            }
            if (zerr != Z_OK) {
                error_ = true;
                throw StreamException("GzipOutputStream: deflate failed");
            }
            continue;
        }

        // Z_NO_FLUSH / Z_SYNC_FLUSH: complete once all pending input is consumed
        // and the encoder has output room (no internal output pending).
        if (zerr != Z_OK && zerr != Z_BUF_ERROR) {
            error_ = true;
            throw StreamException("GzipOutputStream: deflate failed");
        }
        if (d_stream.avail_in == 0 && d_stream.avail_out != 0) {
            break;
        }
    }
}

/**
 *  Flushes this output stream and forces any buffered output
 *  bytes to be written out.
 *
 *  This emits a synchronous deflate flush (a valid continue point), not a
 *  finished stream, so a flush followed by more input stays one gzip stream.
 */ 
void GzipOutputStream::flush()
{
    if (closed) {
        throwIfLatched();
        return;
    }
    throwIfLatched();

    if (!zstreamActive)
        return;

    // Always emit a sync marker, even with no pending input, so repeated empty
    // flushes stay valid; a later write continues the same deflate stream.
    feedInput();
    try {
        deflateAndWrite(Z_SYNC_FLUSH);
        d_stream.next_in = Z_NULL;
        d_stream.avail_in = 0;
        inputBuf.clear();
        destination.flush();
    } catch (...) {
        error_ = true;
        inputBuf.clear();
        d_stream.next_in = Z_NULL;
        d_stream.avail_in = 0;
        throw;
    }
}

/**
 * Finish compression and write the gzip trailer. When checked, failure is
 * reported as an exception; either way a failure is latched, zlib state is
 * released and no later call may append another partial body or trailer.
 */
void GzipOutputStream::finalize(bool checked)
{
    if (closed) {
        if (checked)
            throwIfLatched();
        return;
    }

    if (error_) {
        closed = true;
        endZstream();
        if (checked)
            throwIfLatched();
        return;
    }

    closed = true;
    try {
        if (zstreamActive) {
            feedInput();
            deflateAndWrite(Z_FINISH);
            d_stream.next_in = Z_NULL;
            d_stream.avail_in = 0;
            inputBuf.clear();

            writeTrailer();
            destination.flush();
            destination.close();
        }
    } catch (const StreamException &) {
        error_ = true;
        endZstream();
        if (checked)
            throw;
        return;
    } catch (...) {
        error_ = true;
        endZstream();
        if (checked)
            throw StreamException("GzipOutputStream: close failed");
        return;
    }

    endZstream();
}

/**
 * Writes the specified byte to this output stream.
 */ 
int GzipOutputStream::put(char ch)
{
    if (closed || !zstreamActive)
        {
        //probably throw an exception here
        return -1;
        }

    throwIfLatched();

    //Add char to buffer
    inputBuf.push_back(static_cast<unsigned char>(ch));
    totalIn++;

    // Drain in bounded chunks so memory does not grow with the input size.
    // No byte is counted twice: CRC is updated once in feedInput().
    if (inputBuf.size() >= kInputChunk) {
        feedInput();
        try {
            deflateAndWrite(Z_NO_FLUSH);
        } catch (...) {
            error_ = true;
            inputBuf.clear();
            d_stream.next_in = Z_NULL;
            d_stream.avail_in = 0;
            throw;
        }
        inputBuf.clear();
        d_stream.next_in = Z_NULL;
        d_stream.avail_in = 0;
    }

    return 1;
}



} // namespace IO
} // namespace Inkscape


//#########################################################################
//# E N D    O F    F I L E
//#########################################################################

/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
// vim: expandtab:shiftwidth=4:tabstop=8:softtabstop=4 :
