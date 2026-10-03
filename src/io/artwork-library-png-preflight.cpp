// SPDX-License-Identifier: GPL-2.0-or-later
#include "artwork-library-png-preflight.h"
#include <png.h>
#include <zlib.h>
#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>

namespace Inkscape::IO::ArtworkLibrary::detail {
namespace {
using Failure = SvgPreflightFailure;
[[noreturn]] void bad(Failure f, char const *s) { throw SvgPreflightError(f, s); }
void poll(Cancelled const &c) { if (c && c()) bad(Failure::Cancelled, "PNG validation cancelled"); }
std::uint32_t be32(unsigned char const *p)
{
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) |
           (std::uint32_t(p[2]) << 8) | p[3];
}
constexpr std::size_t allocation_limit = 32u * 1024 * 1024;
struct alignas(std::max_align_t) Allocation { std::size_t size; };
struct Context {
    Bytes const *input;
    Cancelled const *cancelled;
    std::size_t offset = 0, allocated = 0;
    png_structp png = nullptr;
    png_infop info = nullptr;
    unsigned char *row = nullptr;
    std::exception_ptr exception;
    char message[192] = {};
    ~Context()
    {
        if (png) png_destroy_read_struct(&png, info ? &info : nullptr, nullptr);
        std::free(row);
    }
};
void png_failure(png_structp p, png_const_charp message)
{
    auto &c = *static_cast<Context *>(png_get_error_ptr(p));
    std::strncpy(c.message, message ? message : "PNG decode failed", sizeof(c.message) - 1);
    png_longjmp(p, 1);
}
void png_warning(png_structp p, png_const_charp message)
{
    // Do not admit bytes after libpng ignored damaged/oversized metadata.
    png_error(p, message ? message : "Unsupported PNG warning");
}
png_voidp allocate(png_structp p, png_alloc_size_t size)
{
    auto &c = *static_cast<Context *>(png_get_mem_ptr(p));
    if (size > allocation_limit - c.allocated) return nullptr;
    auto *a = static_cast<Allocation *>(std::malloc(sizeof(Allocation) + size));
    if (!a) return nullptr;
    a->size = size; c.allocated += size;
    return a + 1;
}
void deallocate(png_structp p, png_voidp pointer)
{
    if (!pointer) return;
    auto &c = *static_cast<Context *>(png_get_mem_ptr(p));
    auto *a = static_cast<Allocation *>(pointer) - 1;
    c.allocated -= a->size; std::free(a);
}
void read_data(png_structp p, png_bytep output, png_size_t length)
{
    auto &c = *static_cast<Context *>(png_get_io_ptr(p));
    if (length > c.input->size() - c.offset) png_error(p, "Truncated PNG");
    try {
        while (length) {
            poll(*c.cancelled);
            auto n = std::min<std::size_t>(length, 4096);
            std::memcpy(output, c.input->data() + c.offset, n);
            output += n; c.offset += n; length -= n;
        }
    } catch (...) {
        c.exception = std::current_exception();
    }
    if (c.exception) png_error(p, "PNG validation callback failed");
}
} // namespace

std::size_t preflight_png(Bytes const &bytes, std::size_t pixel_limit, Cancelled const &cancelled)
{
    constexpr unsigned char signature[] = {137,80,78,71,13,10,26,10};
    if (bytes.size() < 33 || std::memcmp(bytes.data(), signature, 8)) bad(Failure::Unsupported, "Image is not PNG");
    std::size_t at = 8, chunks = 0; bool end = false;
    while (at < bytes.size()) {
        poll(cancelled);
        if (++chunks > 4096 || bytes.size() - at < 12) bad(Failure::LimitExceeded, "PNG chunk budget/truncation");
        auto length = be32(bytes.data() + at);
        if (length > bytes.size() - at - 12) bad(Failure::MalformedXml, "Truncated PNG chunk");
        auto type = bytes.data() + at + 4;
        if (!std::memcmp(type, "acTL", 4) || !std::memcmp(type, "fcTL", 4) || !std::memcmp(type, "fdAT", 4)) {
            bad(Failure::Unsupported, "Animated PNG is not admitted by the static-image adapter");
        }
        auto crc = crc32(0, nullptr, 0);
        std::size_t remaining = std::size_t(length) + 4, pos = at + 4;
        while (remaining) {
            poll(cancelled);
            auto n = std::min<std::size_t>(remaining, 65536);
            crc = crc32(crc, bytes.data() + pos, static_cast<uInt>(n));
            remaining -= n; pos += n;
        }
        if (crc != be32(bytes.data() + at + 8 + length)) bad(Failure::Integrity, "PNG CRC mismatch");
        at += std::size_t(length) + 12;
        if (!std::memcmp(type, "IEND", 4)) {
            if (length || at != bytes.size()) bad(Failure::Unsupported, "PNG trailing bytes");
            end = true; break;
        }
    }
    if (!end || be32(bytes.data() + 8) != 13 || std::memcmp(bytes.data() + 12, "IHDR", 4)) {
        bad(Failure::Unsupported, "PNG requires first IHDR and terminal IEND");
    }
    auto width = be32(bytes.data() + 16), height = be32(bytes.data() + 20);
    if (!width || !height || width > 16384 || height > 16384 ||
        width > pixel_limit / height) bad(Failure::LimitExceeded, "PNG pixel/dimension budget");

    // All nontrivial C++ locals exist BEFORE setjmp. Callbacks capture exceptions
    // and longjmp; none unwind C or skip automatic C++ destructors.
    auto c = std::make_unique<Context>();
    c->input = &bytes; c->cancelled = &cancelled;
    c->png = png_create_read_struct_2(PNG_LIBPNG_VER_STRING, c.get(), png_failure, png_warning,
                                     c.get(), allocate, deallocate);
    if (!c->png) bad(Failure::LimitExceeded, "PNG allocator initialization failed");
    if (setjmp(png_jmpbuf(c->png))) {
        if (c->exception) std::rethrow_exception(c->exception);
        throw SvgPreflightError(Failure::Unsupported, std::string("PNG rejected: ") + c->message);
    }
    c->info = png_create_info_struct(c->png);
    if (!c->info) png_error(c->png, "PNG info allocation failed");
    png_set_read_fn(c->png, c.get(), read_data);
    png_set_user_limits(c->png, 16384, 16384);
    png_set_chunk_cache_max(c->png, 128);
    png_set_chunk_malloc_max(c->png, 1024u * 1024);
    png_set_crc_action(c->png, PNG_CRC_ERROR_QUIT, PNG_CRC_ERROR_QUIT);
    png_read_info(c->png, c->info);
    if (png_get_image_width(c->png, c->info) != width || png_get_image_height(c->png, c->info) != height) {
        png_error(c->png, "PNG dimensions changed");
    }
    int passes = png_set_interlace_handling(c->png);
    png_read_update_info(c->png, c->info);
    png_size_t row_size = png_get_rowbytes(c->png, c->info);
    if (!row_size || row_size > 16384u * 8) png_error(c->png, "PNG row budget");
    c->row = static_cast<unsigned char *>(std::calloc(row_size, 1));
    if (!c->row) png_error(c->png, "PNG row allocation failed");
    for (int pass = 0; pass < passes; ++pass) {
        for (png_uint_32 y = 0; y < height; ++y) {
            // Outside libpng: a thrown callback unwinds Context normally.
            poll(cancelled);
            png_read_row(c->png, c->row, nullptr);
        }
    }
    png_read_end(c->png, c->info);
    poll(cancelled);
    return std::size_t(width) * height;
}
} // namespace Inkscape::IO::ArtworkLibrary::detail
