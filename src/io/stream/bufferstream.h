// SPDX-License-Identifier: LGPL-2.1-or-later
/**
 * @file
 * Phoebe DOM Implementation.
 *
 * This is a C++ approximation of the W3C DOM model, which follows
 * fairly closely the specifications in the various .idl files, copies of
 * which are provided for reference.  Most important is this one:
 *
 * http://www.w3.org/TR/2004/REC-DOM-Level-3-Core-20040407/idl-definitions.html
 *//*
 * Authors:
 * see git history
 *   Bob Jamison
 *
 * Copyright (C) 2018 Authors
 * Released under GNU LGPL v2.1+, read the file 'COPYING' for more information.
 */
#ifndef SEEN_BUFFERSTREAM_H
#define SEEN_BUFFERSTREAM_H


#include <vector>
#include <limits>
#include "inkscapestream.h"


namespace Inkscape
{
namespace IO
{

//#########################################################################
//# S T R I N G    I N P U T    S T R E A M
//#########################################################################

/**
 * This class is for reading character from a DOMString
 *
 */
class BufferInputStream : public InputStream
{

public:

    BufferInputStream(const std::vector<unsigned char> &sourceBuffer);
    ~BufferInputStream() override;
    int available() override;
    void close() override;
    int get() override;

private:
    const std::vector<unsigned char> &buffer;
    long position;
    bool closed;

}; // class BufferInputStream




//#########################################################################
//# B U F F E R     O U T P U T    S T R E A M
//#########################################################################

/**
 * This class is for sending a stream to a character buffer
 *
 */
class BufferOutputStream : public OutputStream
{

public:
    enum class Failure { None, CapExceeded, Allocation, Other };

    explicit BufferOutputStream(std::size_t maximum_bytes = std::numeric_limits<std::size_t>::max());
    ~BufferOutputStream() override;
    void close() override;
    void flush() override;
    int put(char ch) override;
    bool good() const noexcept { return !failed; }
    Failure failure_reason() const noexcept { return failure; }
    bool cap_exceeded() const noexcept { return failure == Failure::CapExceeded; }
    void mark_failed() noexcept override {
        failed = true;
        if (failure == Failure::None) failure = Failure::Other;
    }
    std::size_t maximum_size() const noexcept { return maximum_bytes; }
    bool reserve(std::size_t size) noexcept;
    std::vector<std::byte> release_bytes();
    virtual std::vector<unsigned char> &getBuffer();

    virtual void clear()
        { buffer.clear(); legacy_buffer.clear(); legacy_valid = false; }
    void discard() noexcept { std::vector<std::byte>().swap(buffer); legacy_buffer.clear(); legacy_valid = false; }

private:
    std::vector<std::byte> buffer;
    std::vector<unsigned char> legacy_buffer;
    bool legacy_valid = false;
    bool closed;
    bool failed = false;
    Failure failure = Failure::None;
    std::size_t maximum_bytes;

}; // class BufferOutputStream

// Test binaries opt in once; the application never enables fault hooks.
void enable_file_io_test_hooks() noexcept;
void reset_file_io_test_hooks_for_testing() noexcept;
bool file_io_test_hooks_enabled() noexcept;



}  //namespace IO
}  //namespace Inkscape



#endif // SEEN_BUFFERSTREAM_H
