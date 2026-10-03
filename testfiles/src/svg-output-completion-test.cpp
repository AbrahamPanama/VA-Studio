// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * Boundary checks for checked save completion in the XML save helpers
 * (src/xml/repr-io.cpp):
 *   - plain and gzip-compressed round-trips (the compressed side is verified
 *     with an independent zlib reader, not Inkscape's GzipInputStream),
 *   - a late write/flush failure is observable (throws StreamException),
 *   - a caller-owned FILE is not closed by the stream and can be closed once
 *     by the caller after the failure,
 *   - the bool save helpers keep returning false instead of throwing,
 *   - the shared in-memory helper and href/namespace output are unchanged.
 */
/*
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include <cstdio>
#include <cstring>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>
#include <glibmm/miscutils.h>
#include <glib/gfileutils.h>
#include <glib/gstdio.h>
#include <zlib.h>

#include "io/stream/inkscapestream.h"
#include "io/stream/bufferstream.h"
#include "io/existing-file-replacement.h"
#include "preferences.h"
#include "xml/repr.h"
#include "xml/repr-save-output-stream.h"

#ifndef _WIN32
#include <csignal>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {

constexpr char const *kSvg =
    "<svg xmlns=\"http://www.w3.org/2000/svg\""
    " xmlns:xlink=\"http://www.w3.org/1999/xlink\""
    " width=\"10\" height=\"10\">"
    "<g id=\"layer1\">"
    "<image id=\"img1\" xlink:href=\"a.png\" width=\"1\" height=\"1\"/>"
    "</g>"
    "</svg>";

constexpr char const *kGoldenSvg =
    "<svg xmlns='http://www.w3.org/2000/svg'"
    " xmlns:xlink='http://www.w3.org/1999/xlink'"
    " xmlns:inkscape='http://www.inkscape.org/namespaces/inkscape'"
    " xmlns:sodipodi='http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd'"
    " width='120' height='80'>"
    "<sodipodi:namedview id='view1' inkscape:document-units='px' sodipodi:docname='Arcángel.svg'/>"
    "<style><![CDATA[.title > tspan { fill: #123456; }]]></style>"
    "<!--keep & < > in this comment-->"
    "<g id='outer' inkscape:label='Pizarro &amp; Quirós &lt;Arcángel&gt; &quot;portrait&quot;'>"
    "<g id='inner' sodipodi:insensitive='true'>"
    "<image id='photo' xlink:href='assets/arcangel.png' width='20' height='20'/>"
    "<text id='caption' xml:space='preserve'>Pizarro Quirós "
    "<tspan style='font-weight:bold'>Arcángel 😀</tspan></text>"
    "</g></g></svg>";

constexpr char const *kOldHrefBase = "/vacards/save-oracle/source";
constexpr char const *kNewHrefBase = "/vacards/save-oracle/destination";

std::string temp_path(char const *suffix)
{
    static int counter = 0;
    std::string const name = "vacards-svg-output-" + std::to_string(g_get_monotonic_time()) + "-" +
                             std::to_string(counter++) + suffix;
    return Glib::build_filename(Glib::get_tmp_dir(), name);
}

std::string read_bytes(std::string const &path)
{
    std::string out;
    FILE *fp = std::fopen(path.c_str(), "rb");
    if (!fp) {
        return out;
    }
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), fp)) > 0) {
        out.append(buf, n);
    }
    std::fclose(fp);
    return out;
}

// Independent gzip reader: deliberately not the application's GzipInputStream.
// A clean end (0) rather than a zlib error (-1) is required, and gzclose must
// confirm the trailer CRC/length (Z_OK); matching payload alone is not enough.
std::string gunzip_bytes(std::string const &path)
{
    std::string out;
    gzFile gz = gzopen(path.c_str(), "rb");
    if (!gz) {
        ADD_FAILURE() << "gzopen failed for " << path;
        return out;
    }
    char buf[4096];
    int n;
    while ((n = gzread(gz, buf, sizeof(buf))) > 0) {
        out.append(buf, static_cast<size_t>(n));
    }
    // gzread must stop at end-of-stream, never at an error.
    EXPECT_EQ(n, 0);
    if (n < 0) {
        int errnum = 0;
        char const *msg = gzerror(gz, &errnum);
        ADD_FAILURE() << "gzread failed (" << errnum << "): " << (msg ? msg : "unknown");
    }
    EXPECT_EQ(gzclose(gz), Z_OK) << "gzclose must confirm gzip integrity";
    return out;
}

std::shared_ptr<Inkscape::XML::Document> read_doc(std::string const &path)
{
    return std::shared_ptr<Inkscape::XML::Document>(sp_repr_read_file(path.c_str(), SP_SVG_NS_URI));
}

} // namespace

TEST(SvgOutputCompletion, PlainRoundTripIsByteStable)
{
    auto doc = std::shared_ptr<Inkscape::XML::Document>(sp_repr_read_buf(kSvg, SP_SVG_NS_URI));
    ASSERT_TRUE(doc);

    auto const path = temp_path(".svg");
    ASSERT_TRUE(sp_repr_save_rebased_file(doc.get(), path.c_str(), SP_SVG_NS_URI, nullptr, nullptr));

    auto const bytes = read_bytes(path);
    ASSERT_FALSE(bytes.empty());
    EXPECT_NE(bytes.find("<?xml"), std::string::npos);

    auto reopened = read_doc(path);
    ASSERT_TRUE(reopened);
    ASSERT_NE(reopened->firstChild(), nullptr);
    EXPECT_STREQ(reopened->firstChild()->name(), "svg:svg");

    // Saving the same document again must not change the produced bytes.
    auto const path2 = temp_path(".svg");
    ASSERT_TRUE(sp_repr_save_rebased_file(doc.get(), path2.c_str(), SP_SVG_NS_URI, nullptr, nullptr));
    EXPECT_EQ(bytes, read_bytes(path2));

    std::remove(path.c_str());
    std::remove(path2.c_str());
}

TEST(SvgOutputCompletion, CheckedBufferMatchesGoldenSvg)
{
    auto doc = std::shared_ptr<Inkscape::XML::Document>(sp_repr_read_mem(
        kGoldenSvg, std::strlen(kGoldenSvg), SP_SVG_NS_URI));
    ASSERT_TRUE(doc);
    auto *prefs = Inkscape::Preferences::get();
    auto const prior_inline = prefs->getBool("/options/svgoutput/inlineattrs");
    struct RestoreInline {
        Inkscape::Preferences *prefs;
        bool value;
        ~RestoreInline() { prefs->setBool("/options/svgoutput/inlineattrs", value); }
    } restore_inline{prefs, prior_inline};
    prefs->setBool("/options/svgoutput/inlineattrs", true);
    for (bool const rebase : {false, true}) {
        auto const golden = read_bytes(std::string(INKSCAPE_TESTS_DIR) +
                                       (rebase ? "/data/svg-output-rebased-golden.svg"
                                               : "/data/svg-output-golden.svg"));
        ASSERT_FALSE(golden.empty());
        auto const *new_base = rebase ? kNewHrefBase : kOldHrefBase;
        for (bool const compress : {false, true}) {
            auto const path = temp_path(compress ? ".svgz" : ".svg");
            FILE *file = std::fopen(path.c_str(), "wb");
            ASSERT_NE(file, nullptr);
            EXPECT_NO_THROW(sp_repr_save_stream(doc.get(), file, SP_SVG_NS_URI, compress,
                                                kOldHrefBase, new_base));
            ASSERT_EQ(std::fclose(file), 0);

            Inkscape::IO::BufferOutputStream buffer;
            EXPECT_NO_THROW(sp_repr_save_output_stream(doc.get(), buffer, SP_SVG_NS_URI, compress,
                                                       kOldHrefBase, new_base));
            std::string const in_memory(buffer.getBuffer().begin(), buffer.getBuffer().end());
            if (!compress) {
                EXPECT_EQ(in_memory, golden);
                EXPECT_EQ(read_bytes(path), golden);
            }
            else EXPECT_EQ(gunzip_bytes(path), golden);
            if (compress) {
                auto const buffer_path = temp_path("-buffer.svgz");
                ASSERT_TRUE(g_file_set_contents(buffer_path.c_str(), in_memory.data(),
                                                in_memory.size(), nullptr));
                EXPECT_EQ(gunzip_bytes(buffer_path), golden);
                std::remove(buffer_path.c_str());
            }
            std::remove(path.c_str());
        }
    }

    Inkscape::IO::BufferOutputStream capped(4);
    EXPECT_THROW(sp_repr_save_output_stream(doc.get(), capped, SP_SVG_NS_URI),
                 Inkscape::IO::StreamException);
    EXPECT_FALSE(capped.good());
    EXPECT_THROW(capped.close(), Inkscape::IO::StreamException);
}

TEST(SvgOutputCompletion, SvgzSerializerExceptionAbandonsOutput)
{
    struct ThrowingBuffer : Inkscape::IO::BufferOutputStream {
        int put(char ch) override {
            if (++writes == 40) throw std::runtime_error("injected serializer exception");
            return BufferOutputStream::put(ch);
        }
        int writes = 0;
    } buffer;
    std::string payload;
    payload.reserve(50000);
    unsigned state = 17;
    for (int i = 0; i < 50000; ++i) {
        state = state * 1664525u + 1013904223u;
        payload += static_cast<char>('a' + state % 26);
    }
    auto const xml = std::string("<svg xmlns='http://www.w3.org/2000/svg'><desc>")
        + payload + "</desc></svg>";
    auto doc = std::shared_ptr<Inkscape::XML::Document>(sp_repr_read_buf(xml.c_str(), SP_SVG_NS_URI));
    ASSERT_TRUE(doc);
    auto const path = temp_path(".svgz");
    ASSERT_TRUE(g_file_set_contents(path.c_str(), "complete old file", -1, nullptr));
    bool completed = false;
    try {
        sp_repr_save_output_stream(doc.get(), buffer, SP_SVG_NS_URI, true);
        completed = true;
    } catch (std::runtime_error const &e) {
        EXPECT_STREQ(e.what(), "injected serializer exception");
    }
    if (completed) {
        auto const bytes = std::as_bytes(std::span(buffer.getBuffer()));
        Inkscape::IO::replace_existing_local_file(path, bytes);
    }
    EXPECT_FALSE(completed);
    EXPECT_EQ(buffer.getBuffer().size(), 39u) << "abort must not append a gzip finish or trailer";
    EXPECT_FALSE(buffer.good());
    EXPECT_THROW(buffer.close(), Inkscape::IO::StreamException);
    EXPECT_EQ(read_bytes(path), "complete old file");
    std::remove(path.c_str());
}

TEST(SvgOutputCompletion, CappedSvgzFailsMidStream)
{
    auto doc = std::shared_ptr<Inkscape::XML::Document>(sp_repr_read_buf(kSvg, SP_SVG_NS_URI));
    ASSERT_TRUE(doc);
    Inkscape::IO::BufferOutputStream buffer(40);
    EXPECT_THROW(sp_repr_save_output_stream(doc.get(), buffer, SP_SVG_NS_URI, true),
                 Inkscape::IO::StreamException);
    EXPECT_TRUE(buffer.cap_exceeded());
    EXPECT_FALSE(buffer.good());
}

TEST(SvgOutputCompletion, CompressedRoundTripDecompressesIndependently)
{
    auto doc = std::shared_ptr<Inkscape::XML::Document>(sp_repr_read_buf(kSvg, SP_SVG_NS_URI));
    ASSERT_TRUE(doc);

    auto const plain = temp_path(".svg");
    auto const compressed = temp_path(".svgz");

    ASSERT_TRUE(sp_repr_save_rebased_file(doc.get(), plain.c_str(), SP_SVG_NS_URI, nullptr, nullptr));
    ASSERT_TRUE(sp_repr_save_rebased_file(doc.get(), compressed.c_str(), SP_SVG_NS_URI, nullptr, nullptr));

    auto const plain_bytes = read_bytes(plain);
    auto const magic = read_bytes(compressed);
    ASSERT_GE(magic.size(), 2u);
    EXPECT_EQ(static_cast<unsigned char>(magic[0]), 0x1f);
    EXPECT_EQ(static_cast<unsigned char>(magic[1]), 0x8b);

    // The compressed payload must expand to exactly the plain XML bytes.
    EXPECT_EQ(gunzip_bytes(compressed), plain_bytes);

    std::remove(plain.c_str());
    std::remove(compressed.c_str());
}

TEST(SvgOutputCompletion, HrefAndNamespaceSurviveRebaseSave)
{
    auto doc = std::shared_ptr<Inkscape::XML::Document>(sp_repr_read_buf(kSvg, SP_SVG_NS_URI));
    ASSERT_TRUE(doc);

    auto const path = temp_path(".svg");
    // Same source and destination directory: the relative href must not move.
    std::string const dir = Glib::get_tmp_dir();
    ASSERT_TRUE(sp_repr_save_rebased_file(doc.get(), path.c_str(), SP_SVG_NS_URI,
                                          dir.c_str(), path.c_str()));

    auto reopened = read_doc(path);
    ASSERT_TRUE(reopened);
    auto *img = sp_repr_lookup_descendant(reopened->firstChild(), "id", "img1");
    ASSERT_NE(img, nullptr);
    EXPECT_STREQ(img->attribute("xlink:href"), "a.png");

    std::remove(path.c_str());
}

TEST(SvgOutputCompletion, SaveHelpersReportFailureThroughBool)
{
    auto doc = std::shared_ptr<Inkscape::XML::Document>(sp_repr_read_buf(kSvg, SP_SVG_NS_URI));
    ASSERT_TRUE(doc);

    // Test-owned exclusive regular file used as a would-be parent directory.
    // fopen_utf8name creates missing parents, so a missing directory would
    // legitimately succeed; a regular file cannot be descended into, so the
    // child path fails with ENOTDIR regardless of privileges or platform.
    GError *error = nullptr;
    gchar *parent_c = nullptr;
    int const fd = g_file_open_tmp("vacards-svg-parent-XXXXXX", &parent_c, &error);
    ASSERT_NE(fd, -1) << (error ? error->message : "g_file_open_tmp failed");
    g_clear_error(&error);
    g_close(fd, nullptr);

    std::string const parent = parent_c;
    g_free(parent_c);
    ASSERT_TRUE(g_file_test(parent.c_str(), G_FILE_TEST_IS_REGULAR))
        << "test setup: expected a regular file at " << parent;

    // <regular file>/out.svg fails; the bool contract returns false without
    // letting an exception escape.
    auto const missing = Glib::build_filename(parent, "out.svg");
    EXPECT_FALSE(sp_repr_save_rebased_file(doc.get(), missing.c_str(), SP_SVG_NS_URI, nullptr, nullptr));
    EXPECT_FALSE(sp_repr_save_file(doc.get(), missing.c_str()));

    std::remove(parent.c_str());
}

TEST(SvgOutputCompletion, SharedBufferHelperStillWorks)
{
    auto doc = std::shared_ptr<Inkscape::XML::Document>(sp_repr_read_buf(kSvg, SP_SVG_NS_URI));
    ASSERT_TRUE(doc);

    auto const buf = sp_repr_save_buf(doc.get());
    EXPECT_NE(buf.find("<svg"), std::string::npos);
    EXPECT_NE(buf.find("a.png"), std::string::npos);
}

#ifndef _WIN32
namespace {

// Keeps SIGPIPE ignored for the whole write/close cleanup and restores the
// previous disposition on every exit path, including an early ASSERT_* return.
// The caller's fclose can flush a retried buffered write to the broken pipe;
// restoring SIGPIPE before that would terminate the test process.
class ScopedSigpipeIgnore
{
public:
    ScopedSigpipeIgnore()
        : _old(signal(SIGPIPE, SIG_IGN))
    {}
    ~ScopedSigpipeIgnore() { signal(SIGPIPE, _old); }
    ScopedSigpipeIgnore(ScopedSigpipeIgnore const &) = delete;
    ScopedSigpipeIgnore &operator=(ScopedSigpipeIgnore const &) = delete;

private:
    using Handler = void (*)(int);
    Handler _old;
};

} // namespace

TEST(SvgOutputCompletion, LateWriteFailureSurfacesAndCallerOwnsFile)
{
    auto doc = std::shared_ptr<Inkscape::XML::Document>(sp_repr_read_buf(kSvg, SP_SVG_NS_URI));
    ASSERT_TRUE(doc);

    // Must outlive fp so SIGPIPE stays ignored through the caller's fclose.
    ScopedSigpipeIgnore const ignore_sigpipe;

    int fds[2];
    ASSERT_EQ(pipe(fds), 0);
    ::close(fds[0]); // no reader: writes fail with EPIPE
    FILE *fp = fdopen(fds[1], "wb");
    ASSERT_NE(fp, nullptr);

    EXPECT_THROW(sp_repr_save_stream(doc.get(), fp, SP_SVG_NS_URI), Inkscape::IO::StreamException);

    // FileOutputStream must never own the caller FILE: it is still open and the
    // caller closes it exactly once here (a double close would be UB).
    int const fd = fileno(fp);
    EXPECT_GE(fd, 0);
    if (fd >= 0) {
        EXPECT_NE(fcntl(fd, F_GETFD), -1);
    }
    std::fclose(fp);
}
#endif // !_WIN32
