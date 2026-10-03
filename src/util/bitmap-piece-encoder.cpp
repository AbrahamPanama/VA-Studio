// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Explode Bitmap: piece crops, gutters, bleed and PNG encoding (EB2-png).
 *
 * Copyright 2026 VA Studio authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "util/bitmap-piece-encoder.h"

#include <algorithm>
#include <cmath>
#include <csetjmp>
#include <cstdlib>
#include <cstring>
#include <png.h>
#include <zlib.h>

namespace Inkscape::Bitmap {
namespace {

Outcome fail(char const *why) noexcept { return {Status::failed, why}; }
Outcome refuse(char const *why) noexcept { return {Status::incompatible, why}; }
constexpr std::uint64_t kPollPixels = 256;
constexpr char const *kIccName = "ICC Profile";

std::uint64_t zlibWorst(std::uint64_t raw) noexcept
{
    std::uint64_t bound;
    return checkedAdd(raw, raw / 512, bound) && checkedAdd(bound, 4096, bound) ? bound : 0;
}

struct Poll {
    JobWork &work; Stop stop; EncodeOptions const &options; PhaseTimer timer;
    Outcome outcome{}; EncodePhase phase = EncodePhase::crop;
    std::uint64_t pending = 0, phaseVisits = 0;
    bool flush() noexcept {
        phaseVisits += pending;
        outcome = work.advance(pending); pending = 0;
        if (options.observe) options.observe(phase, phaseVisits, options.observerData);
        auto timed = timer.check(stop); if (!timed.ok()) outcome = timed;
        return outcome.ok();
    }
    bool tick(std::uint64_t n = 1) noexcept { pending += n; return pending < kPollPixels || flush(); }
    bool enter(EncodePhase next) noexcept { if (!flush()) return false; phase = next; phaseVisits = 0; return flush(); }
};

// One libpng run. Plain data only: libpng longjmps through writePng.
struct PngJob {
    std::uint8_t const *rows = nullptr; // cw*ch RGBA
    std::uint32_t width = 0, height = 0;
    bool grey = false;
    std::uint8_t const *profile = nullptr;
    std::uint64_t profileBytes = 0;
    std::uint32_t ppmX = 0, ppmY = 0;
    std::uint8_t *out = nullptr;
    std::uint64_t cap = 0, used = 0;
    std::uint8_t *rowBuf = nullptr; // 2*width for grey+alpha
    z_stream z{}; bool zActive = false;
    std::uint8_t *compressedProfile = nullptr;
    Poll *poll = nullptr;
    AllocationFault *fault = nullptr;
    std::uint64_t limit = 0, live = 0;
    bool overflow = false, interrupted = false, mismatch = false, memFail = false;
};

png_voidp memAlloc(png_structp png, png_alloc_size_t size)
{
    auto *job = static_cast<PngJob *>(png_get_mem_ptr(png));
    if (job && !job->poll->flush()) { job->interrupted = true; return nullptr; }
    std::uint64_t total = std::uint64_t(size) + 16;
    if (!job || total > job->limit || job->live + total > job->limit || (job->fault && job->fault->fail())) {
        if (job) job->memFail = true;
        return nullptr;
    }
    auto *raw = static_cast<std::uint8_t *>(std::malloc(std::size_t(total)));
    if (!raw) { job->memFail = true; return nullptr; }
    std::memcpy(raw, &size, sizeof(size));
    job->live += total;
    return raw + 16;
}
void memFree(png_structp png, png_voidp p)
{
    if (!p) return;
    auto *raw = static_cast<std::uint8_t *>(p) - 16;
    png_alloc_size_t size;
    std::memcpy(&size, raw, sizeof(size));
    if (auto *job = static_cast<PngJob *>(png_get_mem_ptr(png))) job->live -= std::uint64_t(size) + 16;
    std::free(raw);
}
void onError(png_structp png, png_const_charp) { std::longjmp(png_jmpbuf(png), 1); }
void onWarning(png_structp, png_const_charp) {}
void onWrite(png_structp png, png_bytep data, png_size_t length)
{
    auto *job = static_cast<PngJob *>(png_get_io_ptr(png));
    if (!job->poll->flush()) { job->interrupted = true; png_error(png, "piece encoding interrupted"); }
    if (length > job->cap - job->used) { job->overflow = true; png_error(png, "piece PNG exceeds its worst-case bound"); }
    while (length) {
        auto n = std::min<png_size_t>(length, 65536);
        std::memcpy(job->out + job->used, data, n);
        job->used += n; data += n; length -= n;
        if (!job->poll->flush()) { job->interrupted = true; png_error(png, "piece encoding interrupted"); }
    }
}
void onFlush(png_structp) {}

voidpf zAlloc(voidpf png, uInt n, uInt size) { return memAlloc(static_cast<png_structp>(png), std::uint64_t(n) * size); }
void zFree(voidpf png, voidpf p) { memFree(static_cast<png_structp>(png), p); }
void cleanProfile(png_structp png, PngJob &job) noexcept
{
    if (job.zActive) { deflateEnd(&job.z); job.zActive = false; }
    memFree(png, job.compressedProfile); job.compressedProfile = nullptr;
}
// libpng's iCCP compression is one uninterrupted call. Compress the admission-validated bytes in bounded blocks.
bool writeProfile(png_structp png, PngJob &job) noexcept
{
    auto cap = zlibWorst(job.profileBytes);
    job.compressedProfile = static_cast<std::uint8_t *>(memAlloc(png, cap));
    if (!job.compressedProfile) return false;
    job.z.zalloc = zAlloc; job.z.zfree = zFree; job.z.opaque = png;
    if (deflateInit(&job.z, 6) != Z_OK) return false;
    job.zActive = true; job.z.next_out = job.compressedProfile; job.z.avail_out = uInt(cap);
    for (std::uint64_t at = 0; at < job.profileBytes;) {
        auto n = std::min<std::uint64_t>(16384, job.profileBytes - at);
        job.z.next_in = const_cast<Bytef *>(job.profile + at); job.z.avail_in = uInt(n);
        if (deflate(&job.z, Z_NO_FLUSH) != Z_OK || job.z.avail_in) return false;
        at += n;
        if (!job.poll->tick(n) || !job.poll->flush()) { job.interrupted = true; return false; }
    }
    if (deflate(&job.z, Z_FINISH) != Z_STREAM_END) return false;
    auto size = job.z.total_out;
    deflateEnd(&job.z); job.zActive = false;
    static png_byte const prefix[] = "ICC Profile\0"; // name terminator + compression method 0
    png_write_chunk_start(png, reinterpret_cast<png_const_bytep>("iCCP"), png_uint_32(sizeof(prefix) + size));
    png_write_chunk_data(png, prefix, sizeof(prefix));
    for (std::uint64_t at = 0; at < size;) {
        auto n = std::min<std::uint64_t>(65536, size - at);
        png_write_chunk_data(png, job.compressedProfile + at, n); at += n;
    }
    png_write_chunk_end(png);
    cleanProfile(png, job);
    return true;
}

// true on success; flags in the job describe failures. No C++ objects live in this frame.
bool writePng(PngJob &job) noexcept
{
    png_structp png = png_create_write_struct_2(PNG_LIBPNG_VER_STRING, &job, onError, onWarning, &job, memAlloc, memFree);
    if (!png) return false;
    png_infop info = png_create_info_struct(png);
    if (!info) { png_destroy_write_struct(&png, nullptr); return false; }
    if (setjmp(png_jmpbuf(png))) { cleanProfile(png, job); png_destroy_write_struct(&png, &info); return false; }
    png_set_write_fn(png, &job, onWrite, onFlush);
    png_set_compression_level(png, 6);
    png_set_compression_buffer_size(png, 65536);
    png_set_IHDR(png, info, job.width, job.height, 8, job.grey ? PNG_COLOR_TYPE_GRAY_ALPHA : PNG_COLOR_TYPE_RGB_ALPHA,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);
    if (job.profileBytes) {
        png_set_iCCP(png, info, kIccName, PNG_COMPRESSION_TYPE_BASE, job.profile, png_uint_32(job.profileBytes));
        png_set_invalid(png, info, PNG_INFO_iCCP); // admission already validated; suppress unbounded compression
        png_write_info_before_PLTE(png, info);
        if (!writeProfile(png, job)) { cleanProfile(png, job); png_destroy_write_struct(&png, &info); return false; }
    }
    png_set_pHYs(png, info, job.ppmX, job.ppmY, PNG_RESOLUTION_METER);
    png_write_info(png, info);
    for (std::uint32_t y = 0; y < job.height; ++y) {
        if (!job.poll->flush()) { job.interrupted = true; break; }
        std::uint8_t const *row = job.rows + std::uint64_t(y) * job.width * 4;
        if (job.grey) {
            for (std::uint32_t x = 0; x < job.width; ++x, row += 4) {
                if (!job.poll->tick()) { job.interrupted = true; png_error(png, "piece encoding interrupted"); }
                if (row[1] != row[0] || row[2] != row[0]) { job.mismatch = true; png_error(png, "grey profile on colour samples"); }
                job.rowBuf[2 * x] = row[0]; job.rowBuf[2 * x + 1] = row[3];
            }
            png_write_row(png, job.rowBuf);
        } else {
            png_write_row(png, row);
        }
        if (!job.poll->tick(job.width) || !job.poll->flush()) { job.interrupted = true; break; }
    }
    if (!job.interrupted) png_write_end(png, nullptr);
    png_destroy_write_struct(&png, &info);
    return !job.interrupted;
}

// Bleed neighbours sorted by (d^2, dy, dx), d^2 <= 4.
constexpr int kBleed[12][2] = {{0,-1},{-1,0},{1,0},{0,1},{-1,-1},{1,-1},{-1,1},{1,1},{0,-2},{-2,0},{2,0},{0,2}};

bool bleed(std::uint8_t *crop, std::uint32_t w, std::uint32_t h,
           Partition const &part, std::uint32_t piece, Poll &poll) noexcept
{
    auto const &bounds = part.pieces()[piece];
    // Only pixels within two cells of this piece's foreground can receive
    // bleed. Merge the five neighbouring rows' expanded run intervals, then
    // use the original ordered neighbour oracle inside that union. Sparse,
    // interleaved pieces no longer probe 12 neighbours across every empty
    // pixel of overlapping crop boxes. No extra raster/queue allocation.
    // pieceRuns is row-major. Five monotone cursors visit only this piece's
    // owned runs (including holes), never unrelated runs on neighbouring rows.
    auto const *runs = part.pieceRuns();
    auto begin = part.pieceOffsets()[piece], end = part.pieceOffsets()[piece+1];
    std::uint32_t starts[5]{begin, begin, begin, begin, begin};
    for (std::int64_t y = 0; y < h; ++y) {
        std::uint32_t cursors[5]{}, ends[5]{};
        for (int row = 0; row < 5; ++row) {
            auto sy = y + bounds.y - 1 + row - 2;
            while (starts[row] < end && std::int64_t(part.run(runs[starts[row]]).y) < sy) {
                if (!poll.tick()) return false;
                ++starts[row];
            }
            cursors[row] = ends[row] = starts[row];
            while (ends[row] < end && std::int64_t(part.run(runs[ends[row]]).y) == sy) {
                if (!poll.tick()) return false;
                ++ends[row];
            }
        }
        auto advance = [&](int row) {
            while (cursors[row] < ends[row]) {
                if (!poll.tick()) return false;
                if (part.run(runs[cursors[row]]).foreground) break;
                ++cursors[row];
            }
            return true;
        };
        for (int row = 0; row < 5; ++row) if (!advance(row)) return false;
        std::int64_t done = 0;
        for (;;) {
            int next = -1;
            for (int row = 0; row < 5; ++row) {
                if (!poll.tick()) return false;
                if (cursors[row] < ends[row] && (next < 0 ||
                    part.run(runs[cursors[row]]).x < part.run(runs[cursors[next]]).x)) next = row;
            }
            if (next < 0) break;
            auto r = part.run(runs[cursors[next]++]);
            auto left = std::max(done, std::int64_t(r.x) - bounds.x - 1);
            auto right = std::min(std::int64_t(w), std::int64_t(r.end) - bounds.x + 3);
            for (auto x = left; x < right; ++x) {
                if (!poll.tick()) return false;
                auto *p = crop + (std::uint64_t(y)*w + x)*4;
                if (p[3]) continue;
                for (auto const &o : kBleed) {
                    if (!poll.tick()) return false;
                    auto nx = x + o[0], ny = y + o[1];
                    if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                    auto const *q = crop + (std::uint64_t(ny)*w + nx)*4;
                    if (q[3]) { p[0] = q[0]; p[1] = q[1]; p[2] = q[2]; break; }
                }
            }
            done = std::max(done, right);
            if (!advance(next)) return false;
        }
    }
    return poll.flush();
}

enum class Space { rgb, grey, other };
// Allocation-free admission checks: libpng's writer length rules and its ICC
// header/tag-table hard errors. Warning-only fields retain libpng's compatibility.
Outcome validateProfile(PlainBuffer const &profile, Space &space, Poll &poll) noexcept
{
    auto n = profile.size();
    if (!n) return {};
    if (n < 132) return refuse("ICC profile is too short (minimum 132 bytes).");
    auto *p = reinterpret_cast<png_const_bytep>(profile.data());
    auto word = [&](unsigned at) { return png_get_uint_32(p + at); };
    if (word(0) != n) return refuse("ICC profile embedded length does not match the buffer.");
    if (p[8] > 3 && (n & 3)) return refuse("ICC profile length must be a multiple of four.");
    auto tags = word(128);
    if (tags > (n - 132) / 12) return refuse("ICC profile tag count exceeds its table.");
    if (word(36) != 0x61637370) return refuse("ICC profile header signature is invalid.");
    if (word(64) >= 0xffff) return refuse("ICC profile rendering intent is invalid.");
    if (word(20) != 0x58595a20 && word(20) != 0x4c616220)
        return refuse("ICC profile connection space must be XYZ or Lab.");
    if (word(12) == 0x61627374 || word(12) == 0x6c696e6b)
        return refuse("ICC profile class cannot be embedded in a PNG.");
    // Owner option C: RGB selects colour type 6; GRAY selects type 4 and
    // writePng additionally refuses non-neutral samples. No profile conversion.
    if (word(16) == 0x52474220) space = Space::rgb;
    else if (word(16) == 0x47524159) space = Space::grey;
    else return refuse("ICC profile colour space must be RGB or GRAY for this PNG.");
    for (std::uint32_t i = 0; i < tags; ++i) {
        auto start = word(132 + 12 * i + 4), length = word(132 + 12 * i + 8);
        if (start > n || length > n - start) return refuse("ICC profile tag lies outside the buffer.");
        if (!poll.tick()) return poll.outcome;
    }
    return {};
}

bool pixelsPerMetre(double dpi, std::uint32_t &out) noexcept
{
    if (!std::isfinite(dpi) || dpi <= 0) return false;
    double v = std::round(dpi / 0.0254);
    if (v < 1 || v > 2147483647.0) return false;
    out = std::uint32_t(v); return true;
}

} // namespace

EncodedPieces &EncodedPieces::operator=(EncodedPieces &&o) noexcept
{
    if (this == &o) return *this;
    reset();
    cropArea = o.cropArea; encodedBytes = o.encodedBytes; hrefBytes = o.hrefBytes;
    hrefReservation = std::move(o.hrefReservation);
    _pieces = o._pieces; _count = o._count;
    _indexToken = std::move(o._indexToken); _dataToken = std::move(o._dataToken);
    o._pieces = nullptr; o._count = 0; o.cropArea = o.encodedBytes = o.hrefBytes = 0;
    return *this;
}
void EncodedPieces::reset() noexcept
{
    for (std::uint32_t i = 0; i < _count; ++i) std::free(_pieces[i].data);
    std::free(_pieces);
    _pieces = nullptr; _count = 0; cropArea = encodedBytes = hrefBytes = 0;
    hrefReservation.release(); _indexToken.release(); _dataToken.release();
}

bool pieceAxesFit(std::uint32_t width, std::uint32_t height) noexcept
{
    return width && height && width <= maxPieceAxis - 2 && height <= maxPieceAxis - 2;
}

std::uint64_t worstPngBytes(std::uint32_t width, std::uint32_t height, std::uint64_t profileBytes) noexcept
{
    std::uint64_t row, raw, z, chunks, total = 8 + 25 + 21 + 12;
    if (!checkedMul(width, 4, row) || !checkedAdd(row, 1, row) || !checkedMul(row, height, raw)) return 0;
    z = zlibWorst(raw);
    if (!z || !checkedCeilDiv(z, 65536, chunks) || !checkedAdd(chunks, 1, chunks) || !checkedMul(chunks, 12, chunks) ||
        !checkedAdd(total, z, total) || !checkedAdd(total, chunks, total)) return 0;
    if (profileBytes) {
        z = zlibWorst(profileBytes);
        if (!z || !checkedAdd(z, 25, z) || !checkedAdd(total, z, total)) return 0;
    }
    return total;
}
std::uint64_t encoderScratchBytes(std::uint32_t maxWidth, std::uint64_t profileBytes) noexcept
{
    return MiB + 8 * (std::uint64_t(maxWidth) * 4 + 17) + 3 * profileBytes + 65536;
}

Result<EncodedPieces> encode(FinalGrid const &grid, Partition const &part, Budget &budget, Stop stop,
                             EncodeOptions opt) noexcept
{
    Result<EncodedPieces> result;
    auto bad = [&](Outcome o) { result.outcome = o; result.value.reset(); return std::move(result); };
    if (stop.requested()) return bad({Status::canceled, "Stop requested"});
    std::uint64_t P, bytes;
    if (!grid.width || !grid.height || !checkedMul(grid.width, grid.height, P) || !checkedMul(P, 4, bytes) ||
        grid.pixels.size() != bytes || part.width != grid.width || part.height != grid.height ||
        !part.pieceCount || !part.pieces() || !part.pieceOffsets() || !part.pieceRuns() || !part.runOwners() ||
        !part.sourceRuns() || !part.rows() || grid.profile.size() > 4 * MiB || opt.encoders < 1 || opt.encoders > 2)
        return bad(refuse("Piece encoding input is inconsistent."));
    std::uint32_t ppmX, ppmY;
    if (!pixelsPerMetre(grid.dpiX, ppmX) || !pixelsPerMetre(grid.dpiY, ppmY)) return bad(refuse("The bitmap resolution is not usable."));
    JobWork standalone(P);
    Poll poll{opt.work ? *opt.work : standalone, stop, opt, PhaseTimer{}};
    Space space = Space::other;
    auto admitted = validateProfile(grid.profile, space, poll);
    if (!admitted.ok()) return bad(admitted);
    std::uint64_t profBytes = grid.profile.size();

    // Pass 1: checked geometry, B, worst Q and worst H; nothing is allocated yet.
    std::uint64_t B = 0, worstQ = 0, worstH = 0, maxCrop = 0;
    std::uint32_t maxW = 0;
    std::uint64_t cap = std::min<std::uint64_t>({opt.maxCropPixels, maxCropPixels, 2 * P});
    for (std::uint32_t i = 0; i < part.pieceCount; ++i) {
        if (!poll.tick()) return bad(poll.outcome);
        Piece const &p = part.pieces()[i];
        if (p.x >= p.endX || p.y >= p.endY || p.endX > grid.width || p.endY > grid.height)
            return bad(refuse("A piece lies outside the bitmap."));
        std::uint64_t cw = std::uint64_t(p.endX - p.x) + 2, ch = std::uint64_t(p.endY - p.y) + 2, area, wq, wh;
        if (!pieceAxesFit(p.endX - p.x, p.endY - p.y)) return bad(refuse("A piece is too large for the canvas."));
        if (!checkedMul(cw, ch, area) || !checkedAdd(B, area, B) || B > cap)
            return bad(refuse("The pieces would need too much image data."));
        wq = worstPngBytes(std::uint32_t(cw), std::uint32_t(ch), profBytes);
        if (!wq || !checkedAdd(worstQ, wq, worstQ) || !checkedBase64Length(wq, wh) || !checkedAdd(worstH, wh, worstH) ||
            worstH > std::min(opt.maxHref, maxHrefBytes))
            return bad(refuse("The encoded pieces would be too large."));
        maxCrop = std::max(maxCrop, area); maxW = std::max<std::uint32_t>(maxW, std::uint32_t(cw));
    }
    std::uint64_t indexBytes, scratch = opt.scratchOverride ? opt.scratchOverride : encoderScratchBytes(maxW, profBytes), scratchAll;
    if (!checkedMul(part.pieceCount, sizeof(EncodedPiece), indexBytes) || !checkedMul(scratch, opt.encoders, scratchAll))
        return bad(refuse("The encoded pieces would be too large."));

    // Reserve the worst case first (png/href, encoder scratch, crop); allocation happens afterwards.
    EncodedPieces &out = result.value;
    Outcome o;
    if (!(o = budget.acquire(Stage::png, indexBytes, out._indexToken)).ok() ||
        !(o = budget.acquire(Stage::png, worstQ, out._dataToken)).ok() ||
        !(o = budget.acquire(Stage::href, worstH, out.hrefReservation)).ok()) return bad(o);
    Budget::Token scratchToken;
    if (!(o = budget.acquire(Stage::encoder, scratchAll, scratchToken)).ok()) return bad(o);
    PlainBuffer crop;
    if (!(o = crop.allocate(budget, Stage::crop, maxCrop, 4, opt.fault, stop)).ok()) return bad(o);
    out._pieces = opt.fault && opt.fault->fail() ? nullptr : static_cast<EncodedPiece *>(std::calloc(part.pieceCount, sizeof(EncodedPiece)));
    if (!out._pieces) return bad(fail("Piece index allocation failed."));
    auto *rgba = reinterpret_cast<std::uint8_t *>(crop.data());
    auto const *src = reinterpret_cast<std::uint8_t const *>(grid.pixels.data());
    bool grey = space == Space::grey;
    std::uint8_t *rowBuf = grey && !(opt.fault && opt.fault->fail()) ? static_cast<std::uint8_t *>(std::malloc(2 * std::size_t(maxW))) : nullptr;
    struct Free { std::uint8_t *p; ~Free() { std::free(p); } } freeRow{rowBuf};
    if (grey && !rowBuf) return bad(fail("Piece row allocation failed."));

    std::uint64_t Q = 0, H = 0, liveQ = 0;
    for (std::uint32_t i = 0; i < part.pieceCount; ++i) {
        Piece const &p = part.pieces()[i];
        std::uint32_t cw = p.endX - p.x + 2, ch = p.endY - p.y + 2;
        std::uint64_t cropBytes = std::uint64_t(cw) * ch * 4;
        if (!poll.enter(EncodePhase::crop)) return bad(poll.outcome);
        for (std::uint64_t at = 0; at < cropBytes;) {
            auto n = std::min<std::uint64_t>(65536, cropBytes - at);
            std::memset(rgba + at, 0, std::size_t(n)); at += n;
            if (!poll.tick(n / 4)) return bad(poll.outcome);
        }
        auto const *offsets = part.pieceOffsets();
        if (offsets[i] > offsets[i + 1] || offsets[i + 1] > part.runCount) return bad(fail("Inconsistent piece run index."));
        for (std::uint32_t k = offsets[i]; k < offsets[i + 1]; ++k) {
            PieceRun r = part.run(part.pieceRuns()[k]);
            if (!r.foreground) continue;
            if (r.piece != i || r.x >= r.end || r.x < p.x || r.end > p.endX || r.y < p.y || r.y >= p.endY)
                return bad(fail("A piece run lies outside its piece."));
            std::uint64_t n = r.end - r.x;
            auto *dst = rgba + (std::uint64_t(r.y - p.y + 1) * cw + (r.x - p.x + 1)) * 4;
            auto *source = src + (std::uint64_t(r.y) * grid.width + r.x) * 4;
            while (n) {
                auto chunk = std::min<std::uint64_t>(n, 16384);
                std::memcpy(dst, source, std::size_t(chunk * 4));
                dst += chunk * 4; source += chunk * 4; n -= chunk;
                if (!poll.tick(chunk)) return bad(poll.outcome);
            }
        }
        if (!poll.enter(EncodePhase::bleed) || !bleed(rgba, cw, ch, part, i, poll)) return bad(poll.outcome);
        if (!poll.enter(EncodePhase::encode)) return bad(poll.outcome);

        std::uint64_t wq = worstPngBytes(cw, ch, profBytes);
        auto *buf = opt.fault && opt.fault->fail() ? nullptr : static_cast<std::uint8_t *>(std::malloc(std::size_t(wq)));
        if (!buf) return bad(fail("Piece PNG allocation failed."));
        PngJob job;
        job.rows = rgba; job.width = cw; job.height = ch; job.grey = grey;
        job.profile = reinterpret_cast<std::uint8_t const *>(grid.profile.data()); job.profileBytes = profBytes;
        job.ppmX = ppmX; job.ppmY = ppmY; job.out = buf; job.cap = wq; job.rowBuf = rowBuf;
        job.poll = &poll; job.fault = opt.fault; job.limit = scratch;
        if (!writePng(job)) {
            std::free(buf);
            if (job.interrupted) return bad(poll.outcome);
            if (job.mismatch) return bad(refuse("A grey colour profile cannot describe coloured pixels."));
            if (job.memFail) return bad(fail("Piece PNG encoder allocation failed."));
            return bad(fail(job.overflow ? "Piece PNG exceeded its bound." : "Piece PNG encoding failed."));
        }
        auto *shrunk = opt.fault && opt.fault->fail() ? nullptr : static_cast<std::uint8_t *>(std::realloc(buf, std::size_t(job.used)));
        if (shrunk) buf = shrunk;
        liveQ += shrunk ? job.used : wq; // Failed shrink leaves the original allocation alive.
        EncodedPiece &e = out._pieces[i];
        e.x = std::int32_t(p.x) - 1; e.y = std::int32_t(p.y) - 1; e.width = cw; e.height = ch; e.size = job.used; e.data = buf;
        out._count = i + 1;
        std::uint64_t h;
        if (!checkedAdd(Q, job.used, Q) || !checkedBase64Length(job.used, h) || !checkedAdd(H, h, H) || H > std::min(opt.maxHref, maxHrefBytes))
            return bad(refuse("The encoded pieces would be too large."));
    }
    if (!poll.flush()) return bad(poll.outcome);
    // The PNG token tracks live capacity; Q remains the exact encoded byte count.
    if (!(o = out._dataToken.resize(Stage::png, liveQ)).ok() || !(o = out.hrefReservation.resize(Stage::href, H)).ok()) return bad(o);
    out.cropArea = B; out.encodedBytes = Q; out.hrefBytes = H;
    result.outcome = {Status::changed, ""};
    result.consumed = Q;
    return std::move(result);
}

Result<EncodedPieces> encodeWholeGrid(FinalGrid const &grid, Budget &budget, Stop stop, EncodeOptions opt) noexcept
{
    Result<EncodedPieces> result;
    auto bad = [&](Outcome o) { result.outcome = o; result.value.reset(); return std::move(result); };
    std::uint64_t P, bytes;
    if (!checkedMul(grid.width, grid.height, P) || !checkedMul(P, 4, bytes) ||
        !grid.view().validate().ok() || grid.pixels.size() != bytes || !P ||
        grid.width > maxPieceAxis || grid.height > maxPieceAxis || P > std::min(opt.maxCropPixels, maxCropPixels) ||
        grid.profile.size() > 4 * MiB || opt.encoders < 1 || opt.encoders > 2)
        return bad(refuse("Full-image encoding input is inconsistent."));
    if (stop.requested()) return bad({Status::canceled, "Stop requested"});
    JobWork work(P); Poll poll{opt.work ? *opt.work : work, stop, opt, PhaseTimer{}};
    Space space = Space::other;
    auto o = validateProfile(grid.profile, space, poll); if (!o.ok()) return bad(o);
    std::uint32_t ppmX, ppmY;
    if (!pixelsPerMetre(grid.dpiX, ppmX) || !pixelsPerMetre(grid.dpiY, ppmY)) return bad(refuse("Invalid image resolution."));
    auto worst = worstPngBytes(grid.width, grid.height, grid.profile.size());
    auto scratch = opt.scratchOverride ? opt.scratchOverride : encoderScratchBytes(grid.width, grid.profile.size());
    std::uint64_t href, scratchAll;
    if (!checkedMul(scratch, opt.encoders, scratchAll) || !worst || !checkedBase64Length(worst, href) || href > std::min(opt.maxHref, maxHrefBytes))
        return bad(refuse("The encoded image would be too large."));
    auto &out = result.value; Budget::Token scratchToken;
    if (!(o = budget.acquire(Stage::png, sizeof(EncodedPiece), out._indexToken)).ok() ||
        !(o = budget.acquire(Stage::png, worst, out._dataToken)).ok() ||
        !(o = budget.acquire(Stage::href, href, out.hrefReservation)).ok() ||
        !(o = budget.acquire(Stage::encoder, scratchAll, scratchToken)).ok()) return bad(o);
    out._pieces = opt.fault && opt.fault->fail() ? nullptr : static_cast<EncodedPiece *>(std::calloc(1, sizeof(EncodedPiece)));
    if (!out._pieces) return bad(fail("Image index allocation failed."));
    out._count = 1; auto &piece = out._pieces[0];
    piece.data = opt.fault && opt.fault->fail() ? nullptr : static_cast<std::uint8_t *>(std::malloc(worst));
    if (!piece.data) return bad(fail("Image PNG allocation failed."));
    PlainBuffer row;
    if (space == Space::grey && !(o = row.allocate(budget, Stage::encoder, grid.width, 2, opt.fault, stop)).ok()) return bad(o);
    if (!poll.enter(EncodePhase::encode)) return bad(poll.outcome);
    PngJob job;
    job.rows = reinterpret_cast<std::uint8_t const *>(grid.pixels.data()); job.width = grid.width; job.height = grid.height;
    job.grey = space == Space::grey; job.rowBuf = reinterpret_cast<std::uint8_t *>(row.data());
    job.profile = reinterpret_cast<std::uint8_t const *>(grid.profile.data()); job.profileBytes = grid.profile.size();
    job.ppmX = ppmX; job.ppmY = ppmY; job.out = piece.data; job.cap = worst;
    job.poll = &poll; job.fault = opt.fault; job.limit = scratch;
    if (!writePng(job)) return bad(job.interrupted ? poll.outcome :
        job.mismatch ? refuse("A grey colour profile cannot describe coloured pixels.") : fail("Image PNG encoding failed."));
    if (!poll.flush()) return bad(poll.outcome);
    auto shrunk = opt.fault && opt.fault->fail() ? nullptr : static_cast<std::uint8_t *>(std::realloc(piece.data, job.used));
    if (shrunk) piece.data = shrunk;
    if (!checkedBase64Length(job.used, href)) return bad(fail("Image href overflow."));
    if (!(o = out._dataToken.resize(Stage::png, shrunk ? job.used : worst)).ok() ||
        !(o = out.hrefReservation.resize(Stage::href, href)).ok()) return bad(o);
    piece.width = grid.width; piece.height = grid.height; piece.size = job.used;
    out.cropArea = P; out.encodedBytes = job.used; out.hrefBytes = href;
    result.outcome = {Status::changed, ""}; result.consumed = job.used; return result;
}

} // namespace Inkscape::Bitmap
