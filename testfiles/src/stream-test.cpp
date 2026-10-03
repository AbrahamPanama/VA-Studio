// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Stream IO tests
 *//*
 * Authors: see git history
 *
 * Copyright (C) 2015-2023 Authors
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>
#include <gtest/gtest.h>
#include <zlib.h>

#include "io/stream/gzipstream.h"
#include "io/stream/bufferstream.h"
#include "io/stream/inkscapestream.h"
#include "io/stream/stringstream.h"
#include "io/stream/uristream.h"
#include "io/stream/xsltstream.h"
#include "util/delete-with.h"

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

// names and path storage for other tests
auto const xmlpath = INKSCAPE_TESTS_DIR "/data/crystalegg.xml";
auto const xslpath = INKSCAPE_TESTS_DIR "/data/doc2html.xsl";

class MyFile
{
protected:
    std::string _filename;
    std::string _mode;

public:
    MyFile(std::string filename, char const *mode = "rb")
        : _filename(std::move(filename))
        , _mode(mode)
    {}

    FILE *open(char const *mode) const { return std::fopen(_filename.c_str(), mode); }

    operator FILE *() const { return open(_mode.c_str()); }

    std::string getContents() const
    {
        std::string buf;
        auto fp = Inkscape::Util::delete_with<std::fclose>(open("rb"));

        if (!fp) {
            ADD_FAILURE() << "failed to open " << _filename;
            exit(1);
        }

        for (int c; (c = std::fgetc(fp.get())) != EOF;) {
            buf.push_back(c);
        }

        return buf;
    }
};

class MyOutFile : public MyFile
{
public:
    MyOutFile(std::string filename)
        : MyFile("test_stream-out-" + filename, "wb")
    {}

    ~MyOutFile() { std::remove(_filename.c_str()); }
};

TEST(StreamTest, FileStreamCopy)
{
    auto inFile = MyFile(xmlpath);
    auto outFile = MyOutFile("streamtest.copy");
    {
        auto ins = Inkscape::IO::FileInputStream(inFile);
        auto outs = Inkscape::IO::FileOutputStream(outFile);
        pipeStream(ins, outs);
    }
    ASSERT_EQ(inFile.getContents(), outFile.getContents());
}

TEST(StreamTest, OutputStreamWriter)
{
    Inkscape::IO::StdOutputStream outs;
    Inkscape::IO::OutputStreamWriter writer(outs);
    writer << "Hello, world!  " << 123.45 << " times\n";
    writer.printf("There are %f quick brown foxes in %d states\n", 123.45, 88);
}

TEST(StreamTest, CheckedBufferCapAndClose)
{
    Inkscape::IO::BufferOutputStream out(2);
    EXPECT_EQ(out.put('a'), 1);
    EXPECT_EQ(out.put('b'), 1);
    EXPECT_EQ(out.put('c'), -1);
    EXPECT_FALSE(out.good());
    EXPECT_EQ(out.getBuffer(), (std::vector<unsigned char>{'a', 'b'}));
    EXPECT_THROW(out.close(), Inkscape::IO::StreamException);
    EXPECT_THROW(out.close(), Inkscape::IO::StreamException);

    Inkscape::IO::BufferOutputStream closed;
    EXPECT_EQ(closed.put('x'), 1);
    EXPECT_NO_THROW(closed.close());
    EXPECT_EQ(closed.put('y'), -1);
    EXPECT_THROW(closed.close(), Inkscape::IO::StreamException);
}

TEST(StreamTest, WriterPropagatesBufferFailure)
{
    Inkscape::IO::BufferOutputStream out(1);
    Inkscape::IO::OutputStreamWriter writer(out);
    EXPECT_THROW(writer.writeString("more"), Inkscape::IO::StreamException);
    EXPECT_THROW(writer.close(), Inkscape::IO::StreamException);
    EXPECT_EQ(out.getBuffer(), (std::vector<unsigned char>{'m'}));
}

namespace { bool independentGzipDecode(std::string const &in, std::string &out); }

TEST(StreamTest, CheckedBufferGzipTrailer)
{
    std::string const payload = "checked gzip payload";
    Inkscape::IO::BufferOutputStream bytes;
    {
        Inkscape::IO::GzipOutputStream gzip(bytes);
        Inkscape::IO::OutputStreamWriter writer(gzip);
        writer.writeStdString(payload);
        EXPECT_NO_THROW(writer.close());
    }
    std::string const compressed(bytes.getBuffer().begin(), bytes.getBuffer().end());
    std::string decoded;
    ASSERT_TRUE(independentGzipDecode(compressed, decoded));
    EXPECT_EQ(decoded, payload);

    Inkscape::IO::BufferOutputStream capped(10); // header fits; trailer cannot.
    Inkscape::IO::GzipOutputStream gzip(capped);
    gzip.put('a');
    EXPECT_THROW(gzip.close(), Inkscape::IO::StreamException);
    EXPECT_THROW(gzip.close(), Inkscape::IO::StreamException);
}

TEST(StreamTest, StdWriter)
{
    Inkscape::IO::StdWriter writer;
    writer << "Hello, world!  " << 123.45 << " times\n";
    writer.printf("There are %f quick brown foxes in %d states\n", 123.45, 88);
}

TEST(StreamTest, Xslt)
{
    // ######### XSLT Sheet ############
    auto xsltSheetFile = MyFile(xslpath);
    auto xsltSheetIns = Inkscape::IO::FileInputStream(xsltSheetFile);
    auto stylesheet = Inkscape::IO::XsltStyleSheet(xsltSheetIns);
    xsltSheetIns.close();
    auto sourceFile = MyFile(xmlpath);
    auto xmlIns = Inkscape::IO::FileInputStream(sourceFile);

    // ######### XSLT Input ############
    auto destFile = MyOutFile("test.html");
    auto xmlOuts = Inkscape::IO::FileOutputStream(destFile);
    auto xsltIns = Inkscape::IO::XsltInputStream(xmlIns, stylesheet);
    pipeStream(xsltIns, xmlOuts);
    xsltIns.close();
    xmlOuts.close();

    // ######### XSLT Output ############
    auto xmlIns2 = Inkscape::IO::FileInputStream(sourceFile);
    auto destFile2 = MyOutFile("test2.html");
    auto xmlOuts2 = Inkscape::IO::FileOutputStream(destFile2);
    auto xsltOuts = Inkscape::IO::XsltOutputStream(xmlOuts2, stylesheet);
    pipeStream(xmlIns2, xsltOuts);
    xmlIns2.close();
    xsltOuts.close();

    auto htmlContent = destFile.getContents();
    ASSERT_NE(htmlContent.find("<html"), std::string::npos);
    ASSERT_EQ(htmlContent, destFile2.getContents());
}

TEST(StreamTest, Gzip)
{
    auto sourceFile = MyFile(xmlpath);
    auto gzFile = MyOutFile("test.gz");
    auto destFile = MyOutFile("crystalegg2.xml");

    // ######### Gzip Output ############
    {
        auto sourceIns = Inkscape::IO::FileInputStream(sourceFile);
        auto gzOuts = Inkscape::IO::FileOutputStream(gzFile);
        auto gzipOuts = Inkscape::IO::GzipOutputStream(gzOuts);
        pipeStream(sourceIns, gzipOuts);
    }

    // ######### Gzip Input ############
    {
        auto gzIns = Inkscape::IO::FileInputStream(gzFile.open("rb"));
        auto destOuts = Inkscape::IO::FileOutputStream(destFile);
        auto gzipIns = Inkscape::IO::GzipInputStream(gzIns);
        pipeStream(gzipIns, destOuts);
    }

    ASSERT_EQ(sourceFile.getContents(), destFile.getContents());
}

TEST(StreamTest, GzipFExtraFComment)
{
    auto inFile = MyFile(INKSCAPE_TESTS_DIR "/data/example-FEXTRA-FCOMMENT.gz");
    auto inStream = Inkscape::IO::FileInputStream(inFile);
    auto inStreamGzip = Inkscape::IO::GzipInputStream(inStream);
    auto outStreamString = Inkscape::IO::StringOutputStream();
    pipeStream(inStreamGzip, outStreamString);
    ASSERT_EQ(outStreamString.getString(), "the content");
}

// ---------------------------------------------------------------------------
// Checked stream-completion regression coverage (F1 stream packet).
// ---------------------------------------------------------------------------

namespace
{

bool independentGzipDecode(std::string const &in, std::string &out)
{
    z_stream strm;
    std::memset(&strm, 0, sizeof(strm));
    if (inflateInit2(&strm, 16 + MAX_WBITS) != Z_OK) {
        return false;
    }
    strm.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(in.data()));
    strm.avail_in = static_cast<uInt>(in.size());

    char buf[4096];
    int zerr;
    do {
        strm.next_out = reinterpret_cast<Bytef *>(buf);
        strm.avail_out = sizeof(buf);
        zerr = inflate(&strm, Z_NO_FLUSH);
        if (zerr != Z_OK && zerr != Z_STREAM_END) {
            inflateEnd(&strm);
            return false;
        }
        out.append(buf, sizeof(buf) - strm.avail_out);
    } while (zerr != Z_STREAM_END);

    bool const consumed = (strm.avail_in == 0);
    inflateEnd(&strm);
    return consumed;
}

std::string gzipBytes(std::string const &payload, bool midFlush, bool explicitClose,
                      std::string const &filename)
{
    MyOutFile outFile(filename);
    {
        auto fp = outFile.open("wb");
        Inkscape::IO::FileOutputStream outs(fp);
        {
            Inkscape::IO::GzipOutputStream gz(outs);
            std::size_t const half = midFlush ? payload.size() / 2 : payload.size();
            for (std::size_t i = 0; i < half; ++i) {
                gz.put(payload[i]);
            }
            if (midFlush) {
                gz.flush();
                for (std::size_t i = half; i < payload.size(); ++i) {
                    gz.put(payload[i]);
                }
            }
            if (explicitClose) {
                gz.close();
            }
        }
        if (fp) {
            std::fclose(fp);
        }
    }
    return outFile.getContents();
}

// Destination that can be armed to fail by exception or by negative return,
// independently on put/flush/close.
class FaultOutputSink : public Inkscape::IO::OutputStream
{
public:
    enum Mode { None, ThrowOnPut, NegativePut, ThrowOnFlush, ThrowOnClose };

    Mode mode = None;
    bool armed = false;
    int puts = 0;
    int flushes = 0;
    int closes = 0;
    // When >= 0, the put with this zero-based index fails, even before arming.
    // Used to target a "late" byte of the 10-byte gzip header.
    int failPutAt = -1;

    void close() override
    {
        ++closes;
        if (armed && (mode == ThrowOnPut || mode == ThrowOnClose)) {
            throw Inkscape::IO::StreamException("sink close failure");
        }
    }

    void flush() override
    {
        ++flushes;
        if (armed && (mode == ThrowOnPut || mode == ThrowOnFlush)) {
            throw Inkscape::IO::StreamException("sink flush failure");
        }
    }

    int put(char) override
    {
        int const index = puts;
        ++puts;
        if (failPutAt >= 0 && index == failPutAt) {
            return -1;
        }
        if (armed && mode == ThrowOnPut) {
            throw Inkscape::IO::StreamException("sink put failure");
        }
        if (armed && mode == NegativePut) {
            return -1;
        }
        return 1;
    }
};

#ifndef _WIN32
// funopen-backed caller FILE whose write callback always fails and counts how
// many times stdio invokes it. Lets a real fflush() failure be observed and a
// retry by the destructor/close be detected exactly.
struct CountingFailFile
{
    static int calls;
    static int writeCb(void *, const char *, int) { ++calls; return -1; }
    static int closeCb(void *) { return 0; }
};
int CountingFailFile::calls = 0;
#endif

#ifndef _WIN32
struct SigPipeGuard
{
    struct sigaction old {};
    SigPipeGuard()
    {
        struct sigaction ign {};
        ign.sa_handler = SIG_IGN;
        sigaction(SIGPIPE, &ign, &old);
    }
    ~SigPipeGuard() { sigaction(SIGPIPE, &old, nullptr); }
};
#endif

} // namespace

TEST(StreamTest, FileOutputBrokenPipeFlushIsChecked)
{
#ifndef _WIN32
    SigPipeGuard guard;
    int fds[2];
    ASSERT_EQ(::pipe(fds), 0);
    FILE *w = ::fdopen(fds[1], "w");
    ASSERT_NE(w, nullptr);
    {
        Inkscape::IO::FileOutputStream os(w);
        os.put('a');
        os.put('b');
        os.put('c');
        ::close(fds[0]);              // break the pipe before the buffered write lands
        EXPECT_THROW(os.flush(), Inkscape::IO::StreamException);
        EXPECT_NE(std::ferror(w), 0);
        EXPECT_THROW(os.close(), Inkscape::IO::StreamException); // failure latched
        int const fd = ::fileno(w);
        EXPECT_GE(fd, 0);
        EXPECT_NE(::fcntl(fd, F_GETFD), -1); // destructor must not close caller FILE
    }
    std::fclose(w);                   // exactly one caller close
#else
    // Native alternative to a SIGPIPE pipe: a read-only FILE makes buffered
    // writes fail, and the failure must be latched and checked.
    char const *path = "test_stream-readonly.tmp";
    FILE *seed = std::fopen(path, "wb");
    ASSERT_NE(seed, nullptr);
    std::fclose(seed);
    FILE *ro = std::fopen(path, "rb");
    ASSERT_NE(ro, nullptr);
    {
        Inkscape::IO::FileOutputStream os(ro);
        EXPECT_THROW(os.put('x'), Inkscape::IO::StreamException);
        EXPECT_THROW(os.flush(), Inkscape::IO::StreamException);
        EXPECT_THROW(os.close(), Inkscape::IO::StreamException);
    }
    std::fclose(ro);
    std::remove(path);
#endif
}

TEST(StreamTest, FileOutputWriteFailureIsLatched)
{
#ifdef _WIN32
    char const *path = "test_stream-writefail.tmp";
    FILE *seed = std::fopen(path, "wb");
    ASSERT_NE(seed, nullptr);
    std::fclose(seed);
    FILE *ro = std::fopen(path, "rb");
#else
    FILE *ro = std::fopen("/dev/null", "r");
#endif
    ASSERT_NE(ro, nullptr);
    {
        Inkscape::IO::FileOutputStream os(ro);
        EXPECT_THROW(os.put('z'), Inkscape::IO::StreamException);
        EXPECT_THROW(os.flush(), Inkscape::IO::StreamException);
        EXPECT_THROW(os.close(), Inkscape::IO::StreamException);
    }
    std::fclose(ro);
#ifdef _WIN32
    std::remove(path);
#endif
}

TEST(StreamTest, FileOutputCallerOwnsFileAndCloseIsIdempotent)
{
    char const *path = "test_stream-owner.tmp";
    FILE *fp = std::fopen(path, "wb");
    ASSERT_NE(fp, nullptr);
    {
        Inkscape::IO::FileOutputStream os(fp);
        os.put('h');
        os.put('i');
        EXPECT_NO_THROW(os.close());
        EXPECT_NO_THROW(os.close());   // idempotent successful close
        EXPECT_EQ(std::ferror(fp), 0);
        EXPECT_EQ(std::fflush(fp), 0); // caller FILE still open and usable
    }
    EXPECT_EQ(std::fclose(fp), 0);     // caller closes exactly once
    EXPECT_EQ(MyFile(path).getContents(), "hi");
    std::remove(path);
}

TEST(StreamTest, GzipIndependentDecoderRoundtrip)
{
    std::vector<std::pair<std::string, std::string>> cases;
    cases.emplace_back("empty", "");
    cases.emplace_back("small-incompressible", "the content");
    {
        std::string random;
        unsigned int x = 0x12345678u;
        for (int i = 0; i < 100000; ++i) {
            x = x * 1103515245u + 12345u;
            random.push_back(static_cast<char>(x >> 16));
        }
        cases.emplace_back("random-100k", random);
    }
    cases.emplace_back("large-compressible", MyFile(xmlpath).getContents());

    for (auto const &[name, payload] : cases) {
        std::string const gz = gzipBytes(payload, /*midFlush=*/false, /*explicitClose=*/true,
                                         name + ".gz");
        std::string decoded;
        ASSERT_TRUE(independentGzipDecode(gz, decoded)) << name;
        EXPECT_EQ(decoded, payload) << name;
    }
}

TEST(StreamTest, GzipDestructorFinalizesValidStream)
{
    for (auto const &payload : {std::string(), std::string("the content")}) {
        std::string const gz = gzipBytes(payload, /*midFlush=*/false, /*explicitClose=*/false,
                                         "dtor.gz");
        std::string decoded;
        ASSERT_TRUE(independentGzipDecode(gz, decoded));
        EXPECT_EQ(decoded, payload);
    }
}

TEST(StreamTest, GzipFlushThenMoreInputStaysOneStream)
{
    std::string const payload = "part one|part two|part three";
    std::string const gz = gzipBytes(payload, /*midFlush=*/true, /*explicitClose=*/true,
                                     "flush.gz");
    std::string decoded;
    ASSERT_TRUE(independentGzipDecode(gz, decoded));
    EXPECT_EQ(decoded, payload);
}

TEST(StreamTest, GzipSuccessfulBytesStable)
{
    std::string const payload = MyFile(xmlpath).getContents();
    std::string const explicitGz = gzipBytes(payload, false, true, "stable-explicit.gz");
    std::string const destructorGz = gzipBytes(payload, false, false, "stable-dtor.gz");
    EXPECT_EQ(explicitGz, destructorGz);
    std::string decoded;
    ASSERT_TRUE(independentGzipDecode(explicitGz, decoded));
    EXPECT_EQ(decoded, payload);
}

TEST(StreamTest, GzipDestinationFailuresAreLatchedAndDestructorSafe)
{
    // Throwing put: close reports it; the destructor must not retry or terminate.
    {
        FaultOutputSink sink;
        sink.mode = FaultOutputSink::ThrowOnPut;
        auto *gz = new Inkscape::IO::GzipOutputStream(sink);
        gz->put('x');
        sink.armed = true;
        EXPECT_THROW(gz->close(), Inkscape::IO::StreamException);
        int const atFailure = sink.puts;
        delete gz;
        EXPECT_EQ(sink.puts, atFailure);
    }
    // Negative-return put.
    {
        FaultOutputSink sink;
        sink.mode = FaultOutputSink::NegativePut;
        Inkscape::IO::GzipOutputStream gz(sink);
        gz.put('q');
        sink.armed = true;
        EXPECT_THROW(gz.close(), Inkscape::IO::StreamException);
    }
    // Destination flush failure.
    {
        FaultOutputSink sink;
        sink.mode = FaultOutputSink::ThrowOnFlush;
        Inkscape::IO::GzipOutputStream gz(sink);
        gz.put('a');
        sink.armed = true;
        EXPECT_THROW(gz.close(), Inkscape::IO::StreamException);
    }
    // Destination close failure.
    {
        FaultOutputSink sink;
        sink.mode = FaultOutputSink::ThrowOnClose;
        Inkscape::IO::GzipOutputStream gz(sink);
        gz.put('a');
        sink.armed = true;
        EXPECT_THROW(gz.close(), Inkscape::IO::StreamException);
    }
    // A failed flush is latched; close must not append a trailer/body.
    {
        FaultOutputSink sink;
        sink.mode = FaultOutputSink::ThrowOnPut;
        Inkscape::IO::GzipOutputStream gz(sink);
        gz.put('a');
        sink.armed = true;
        EXPECT_THROW(gz.flush(), Inkscape::IO::StreamException);
        int const atFlush = sink.puts;
        EXPECT_THROW(gz.close(), Inkscape::IO::StreamException);
        EXPECT_EQ(sink.puts, atFlush);
    }
    // Destructor-only finalization against a throwing destination must not terminate.
    {
        FaultOutputSink sink;
        sink.mode = FaultOutputSink::ThrowOnPut;
        {
            Inkscape::IO::GzipOutputStream gz(sink);
            gz.put('a');
            gz.put('b');
            sink.armed = true;
        }                              // destructor hits the throwing put
        SUCCEED();
    }
}

// ---------------------------------------------------------------------------
// B1: gzip header writes are checked, including during construction.
// ---------------------------------------------------------------------------

TEST(StreamTest, GzipHeaderWriteFailureIsChecked)
{
    // Negative return on the very first header byte, armed before construction.
    {
        FaultOutputSink sink;
        sink.mode = FaultOutputSink::NegativePut;
        sink.armed = true;
        EXPECT_THROW(Inkscape::IO::GzipOutputStream gz(sink),
                     Inkscape::IO::StreamException);
        EXPECT_EQ(sink.puts, 1);
    }
    // Throwing destination, armed before construction.
    {
        FaultOutputSink sink;
        sink.mode = FaultOutputSink::ThrowOnPut;
        sink.armed = true;
        EXPECT_THROW(Inkscape::IO::GzipOutputStream gz(sink),
                     Inkscape::IO::StreamException);
    }
    // Late header byte (index 9, the OS code) fails: the full 10-byte header
    // must be checked, not just the body/trailer. No zlib state exists yet, so
    // the throwing constructor leaks nothing.
    {
        FaultOutputSink sink;
        sink.mode = FaultOutputSink::NegativePut;
        sink.armed = false;
        sink.failPutAt = 9;
        EXPECT_THROW(Inkscape::IO::GzipOutputStream gz(sink),
                     Inkscape::IO::StreamException);
        EXPECT_EQ(sink.puts, 10);
    }
    // A fresh stream still constructs and round-trips after those failures.
    {
        FaultOutputSink sink;
        Inkscape::IO::GzipOutputStream gz(sink);
        gz.put('o');
        gz.put('k');
        EXPECT_NO_THROW(gz.close());
        EXPECT_GE(sink.puts, 10);
    }
}

// ---------------------------------------------------------------------------
// B2: FileOutputStream never retries fflush after a latched error.
// ---------------------------------------------------------------------------

#ifndef _WIN32
TEST(StreamTest, FileOutputDestructorDoesNotRetryFailedFlush)
{
    CountingFailFile::calls = 0;
    FILE *fp = funopen(nullptr, nullptr, &CountingFailFile::writeCb, nullptr,
                       &CountingFailFile::closeCb);
    ASSERT_NE(fp, nullptr);
    int afterFlush = 0;
    {
        Inkscape::IO::FileOutputStream os(fp);
        os.put('a');
        os.put('b');
        EXPECT_THROW(os.flush(), Inkscape::IO::StreamException); // real fflush fails
        afterFlush = CountingFailFile::calls;
        EXPECT_GE(afterFlush, 1);
    } // destructor runs WITHOUT an explicit close: must not re-flush
    EXPECT_EQ(CountingFailFile::calls, afterFlush);
    std::fclose(fp);
}

TEST(StreamTest, FileOutputCloseDoesNotRetryFailedFlush)
{
    CountingFailFile::calls = 0;
    FILE *fp = funopen(nullptr, nullptr, &CountingFailFile::writeCb, nullptr,
                       &CountingFailFile::closeCb);
    ASSERT_NE(fp, nullptr);
    {
        Inkscape::IO::FileOutputStream os(fp);
        os.put('a');
        EXPECT_THROW(os.flush(), Inkscape::IO::StreamException);
        int const afterFlush = CountingFailFile::calls;
        EXPECT_THROW(os.close(), Inkscape::IO::StreamException); // latched error preserved
        EXPECT_EQ(CountingFailFile::calls, afterFlush);          // close must not re-flush
    }
    std::fclose(fp);
}
#endif

// ---------------------------------------------------------------------------
// B3: bounded chunked streaming, wrap boundary and repeated flush.
// ---------------------------------------------------------------------------

TEST(StreamTest, GzipBoundedChunkBoundaryRoundtrips)
{
    std::vector<std::size_t> const sizes = {0, 1, 32767, 32768, 32769, 65535,
                                            65536, 65537, 100000};
    unsigned int x = 0x9e3779b9u;
    for (std::size_t n : sizes) {
        std::string payload;
        payload.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            x = x * 1103515245u + 12345u;
            payload.push_back(static_cast<char>(x >> 16)); // incompressible
        }
        std::string const gz = gzipBytes(payload, false, true, "boundary.gz");
        std::string decoded;
        ASSERT_TRUE(independentGzipDecode(gz, decoded)) << "size=" << n;
        EXPECT_EQ(decoded, payload) << "size=" << n;
    }
}

TEST(StreamTest, GzipRepeatedEmptyFlushStaysValid)
{
    MyOutFile outFile("emptyflush.gz");
    {
        auto fp = outFile.open("wb");
        Inkscape::IO::FileOutputStream outs(fp);
        {
            Inkscape::IO::GzipOutputStream gz(outs);
            gz.flush();
            gz.flush();
            gz.flush();
            gz.close();
        }
        if (fp) {
            std::fclose(fp);
        }
    }
    std::string const gz = outFile.getContents();
    std::string decoded;
    ASSERT_TRUE(independentGzipDecode(gz, decoded));
    EXPECT_TRUE(decoded.empty());
}

TEST(StreamTest, GzipIsizeTrailerUsesModulo32)
{
    EXPECT_EQ(Inkscape::IO::gzipIsizeModulo32(0u), 0u);
    EXPECT_EQ(Inkscape::IO::gzipIsizeModulo32(0xffffffffULL), 0xffffffffu);
    EXPECT_EQ(Inkscape::IO::gzipIsizeModulo32(0x100000000ULL), 0u);
    EXPECT_EQ(Inkscape::IO::gzipIsizeModulo32(0x100000005ULL), 5u);
    EXPECT_EQ(Inkscape::IO::gzipIsizeModulo32(0x1ffffffffULL), 0xffffffffu);

    // Real multichunk stream: trailer ISIZE is the payload size mod 2^32.
    std::string const payload(32769, 'a');
    std::string const gz = gzipBytes(payload, false, true, "isize.gz");
    ASSERT_GE(gz.size(), 8u);
    std::uint32_t isize = 0;
    for (int i = 0; i < 4; ++i) {
        isize |= static_cast<std::uint32_t>(static_cast<unsigned char>(gz[gz.size() - 4 + i])) << (8 * i);
    }
    EXPECT_EQ(isize, Inkscape::IO::gzipIsizeModulo32(payload.size()));
}
