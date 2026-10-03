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

/**
 * This class provided buffered endpoints for input and output.
 */

#include "bufferstream.h"
#include <new>
#include <atomic>
#include <algorithm>
#include <utility>
#include <cstring>
#include <stdexcept>

namespace Inkscape
{
namespace IO
{
namespace { std::atomic<bool> test_hooks_enabled{false}; }
void enable_file_io_test_hooks() noexcept { test_hooks_enabled.store(true); }
void reset_file_io_test_hooks_for_testing() noexcept { test_hooks_enabled.store(false); }
bool file_io_test_hooks_enabled() noexcept { return test_hooks_enabled.load(); }


//#########################################################################
//# B U F F E R    I N P U T    S T R E A M
//#########################################################################
/**
 *
 */
BufferInputStream::BufferInputStream(
           const std::vector<unsigned char> &sourceBuffer)
           : buffer(sourceBuffer)
{
    position = 0;
    closed = false;
}

/**
 *
 */
BufferInputStream::~BufferInputStream()
= default;

/**
 * Returns the number of bytes that can be read (or skipped over) from
 * this input stream without blocking by the next caller of a method for
 * this input stream.
 */
int BufferInputStream::available()
{
    if (closed)
        return -1;
    return buffer.size() - position;
}


/**
 *  Closes this input stream and releases any system resources
 *  associated with the stream.
 */
void BufferInputStream::close()
{
    closed = true;
}

/**
 * Reads the next byte of data from the input stream.  -1 if EOF
 */
int BufferInputStream::get()
{
    if (closed)
        return -1;
    if (position >= (int)buffer.size())
        return -1;
    int ch = (int) buffer[position++];
    return ch;
}




//#########################################################################
//# B U F F E R    O U T P U T    S T R E A M
//#########################################################################

/**
 *
 */
BufferOutputStream::BufferOutputStream(std::size_t maximum_bytes) : maximum_bytes(maximum_bytes)
{
    closed = false;
}

/**
 *
 */
BufferOutputStream::~BufferOutputStream()
= default;

/**
 * Closes this output stream and releases any system resources
 * associated with this stream.
 */
void BufferOutputStream::close()
{
    closed = true;
    if (failed) {
        throw StreamException("BufferOutputStream: output failed");
    }
}

/**
 *  Flushes this output stream and forces any buffered output
 *  bytes to be written out.
 */
void BufferOutputStream::flush()
{
    //nothing to do
}

bool BufferOutputStream::reserve(std::size_t size) noexcept
{
    try {
        buffer.reserve(std::min(size, maximum_bytes));
        return true;
    } catch (std::bad_alloc const &) {
        failure = Failure::Allocation;
    } catch (std::length_error const &) {
        failure = Failure::Allocation;
    }
    failed = true;
    return false;
}

std::vector<unsigned char> &BufferOutputStream::getBuffer()
{
    if (!legacy_valid) {
        legacy_buffer.clear();
        if (!buffer.empty()) {
            auto const *first = reinterpret_cast<unsigned char const *>(buffer.data());
            legacy_buffer.assign(first, first + buffer.size());
        }
        legacy_valid = true;
    }
    return legacy_buffer;
}

std::vector<std::byte> BufferOutputStream::release_bytes()
{
    legacy_buffer.clear();
    legacy_valid = false;
    return std::move(buffer);
}

/**
 * Writes the specified byte to this output stream.
 */
int BufferOutputStream::put(char ch)
{
    if (buffer.size() >= maximum_bytes && !failed && !closed) {
        failure = Failure::CapExceeded;
        failed = true;
        return -1;
    }
    if (closed || failed) {
        if (failure == Failure::None) failure = Failure::Other;
        failed = true;
        return -1;
    }
    try {
        buffer.push_back(static_cast<std::byte>(ch));
        legacy_valid = false;
    } catch (std::bad_alloc const &) {
        failure = Failure::Allocation;
        failed = true;
        return -1;
    } catch (std::length_error const &) {
        failure = Failure::Allocation;
        failed = true;
        return -1;
    }
    return 1;
}




}  //namespace IO
}  //namespace Inkscape

//#########################################################################
//# E N D    O F    F I L E
//#########################################################################
