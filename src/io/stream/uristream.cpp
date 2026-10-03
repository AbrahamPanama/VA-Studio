// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Our base String stream classes.  We implement these to
 * be based on Glib::ustring
 *
 * Authors:
 *   Bob Jamison <rjamison@titan.com>
 *
 * Copyright (C) 2004 Inkscape.org
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */


#include "uristream.h"
#include "io/sys.h"

#include <cerrno>
#include <cstring>


namespace Inkscape
{
namespace IO
{

//#########################################################################
//# F I L E    I N P U T    S T R E A M
//#########################################################################


/**
 *
 */
FileInputStream::FileInputStream(FILE *source)
    : inf(source)
{
    if (!inf) {
        Glib::ustring err = "FileInputStream passed NULL";
        throw StreamException(err);
    }
}

/**
 *
 */
FileInputStream::~FileInputStream()
{
    close();
}

/**
 * Returns the number of bytes that can be read (or skipped over) from
 * this input stream without blocking by the next caller of a method for
 * this input stream.
 */
int FileInputStream::available()
{
    return 0;
}


/**
 *  Closes this input stream and releases any system resources
 *  associated with the stream.
 */
void FileInputStream::close()
{
            if (!inf)
                return;
            fflush(inf);
            fclose(inf);
            inf=nullptr;
}

/**
 * Reads the next byte of data from the input stream.  -1 if EOF
 */
int FileInputStream::get()
{
    int retVal = -1;
                if (!inf || feof(inf))
                {
                    retVal = -1;
                }
                else
                {
                    retVal = fgetc(inf);
                }

    return retVal;
}




//#########################################################################
//#  F I L E    O U T P U T    S T R E A M
//#########################################################################

namespace
{

/**
 * Build a StreamException for a checked file-output failure. Appends the
 * current errno when one is set; this is diagnostic only.
 */
void throwFileOutputStreamError(char const *what)
{
    Glib::ustring err = what;
    if (errno != 0) {
        err += ": ";
        err += std::strerror(errno);
    }
    throw StreamException(err);
}

} // namespace

FileOutputStream::FileOutputStream(FILE *fp)
    : ownsFile(false)
    , outf(fp)
{
    if (!outf) {
        Glib::ustring err = "FileOutputStream given null file ";
        throw StreamException(err);
    }
}

/**
 * Non-throwing best-effort cleanup. This never takes ownership of a
 * caller-supplied FILE, never retries a flushed/write that has already failed
 * (error_ latched), and never retries after an explicit close().
 */
FileOutputStream::~FileOutputStream()
{
    if (closed_)
        return;
    closed_ = true;
    if (!outf)
        return;
    // Only a stream with no latched failure is flushed, and at most once.
    if (!error_)
        std::fflush(outf);
    // Caller owns the FILE; just drop our pointer and state.
    outf = nullptr;
}

/**
 * Closes this output stream and releases any system resources
 * associated with this stream.
 *
 * This is a checked operation: a flush/close failure or a previously latched
 * write error is reported as StreamException. A successful close is idempotent;
 * once a failure is latched, the stream can never subsequently report success,
 * and a latched failure is never re-flushed.
 */
void FileOutputStream::close()
{
    if (closed_) {
        if (error_)
            throwFileOutputStreamError("FileOutputStream: previous write/flush failed");
        return;
    }

    bool failed = error_;
    if (outf) {
        // A latched error_ means fflush/ferror already failed; do not retry it.
        if (!error_ && std::fflush(outf) != 0)
            failed = true;
        if (std::ferror(outf) != 0)
            failed = true;
        if (ownsFile && std::fclose(outf) != 0)
            failed = true;
        outf = nullptr;
    }
    closed_ = true;

    if (failed) {
        error_ = true;
        throwFileOutputStreamError("FileOutputStream: close failed");
    }
}

/**
 *  Flushes this output stream and forces any buffered output
 *  bytes to be written out.
 *
 *  Checked: both the fflush() result and any accumulated ferror() state are
 *  reported as StreamException.
 */
void FileOutputStream::flush()
{
    if (closed_) {
        if (error_)
            throwFileOutputStreamError("FileOutputStream: previous write/flush failed");
        return;
    }
    if (!outf)
        return;
    if (error_)
        throwFileOutputStreamError("FileOutputStream: previous write/flush failed");

    if (std::fflush(outf) != 0 || std::ferror(outf) != 0) {
        error_ = true;
        throwFileOutputStreamError("FileOutputStream: flush failed");
    }
}

/**
 * Writes the specified byte to this output stream.
 */
int FileOutputStream::put(char ch)
{
    if (closed_ || !outf)
        return -1;
    if (error_)
        throwFileOutputStreamError("FileOutputStream: previous write/flush failed");

    unsigned char const uch = static_cast<unsigned char>(ch & 0xff);
    if (fputc(uch, outf) == EOF) {
        error_ = true;
        throwFileOutputStreamError("ERROR writing to file");
    }

    return 1;
}





} // namespace IO
} // namespace Inkscape


//#########################################################################
//# E N D    O F    F I L E
//#########################################################################

// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:fileencoding=utf-8:textwidth=99 :
