/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * gtk-cairo-buffer-test.c — outcome-based regression harness for the default
 * Windows GTK4/GDK Cairo window-buffer path (GDI, DComp off).
 *
 * The harness drives the real patched GDK Win32 Cairo context
 * (gdk_surface_create_cairo_context + gdk_draw_context_begin_frame/
 * gdk_cairo_context_cairo_create/gdk_draw_context_end_frame) on real toplevel
 * and popup HWNDs, and reads back *presented* client pixels with GDI BitBlt +
 * GdiFlush.  It does NOT re-implement the proposed buffer algorithm; every
 * correctness case checks the pixels that reach the window.  One additional
 * case ("gtk_renderer") drives the full GtkWindow/GtkDrawingArea/GskCairoRenderer
 * path to confirm the GTK-level entry point presents the same content.
 *
 * A separate, opt-in "--benchmark" mode measures the arm's real window-buffer
 * presentation: per frame the timer starts before draw_surface_frame() and
 * stops after a successful GdiFlush() on the same thread (including queued GDI
 * completion, excluding capture/readback/PNG). It reports p50/p95 and a
 * per-trial CSV; it is NOT display/monitor FPS. The pixel oracle is verified
 * only at display scale 1, so benchmark mode fails closed on any other scale.
 * A CSV write/close failure is a nonzero benchmark failure.
 *
 * Oracles are defined in testfiles/windows/gtk-cairo-buffer/ORACLES.md and
 * were written before this implementation.
 *
 * Output (stdout, stable for the PowerShell driver):
 *   SESSION id=<session> physical_requested=0|1 active_console=<id>
 *           protocol=<n|unknown> remote=0|1 physical_ok=0|1
 *   ENV GDK_DISABLE=.. GDK_DEBUG=.. GSK_RENDERER=.. GDK_BACKEND=..
 *   MONITORS count=N + MONITOR i scale=.. x=.. y=.. w=.. h=..
 *   CASE <name> RESULT PASS|FAIL|SKIP <detail>
 *   ORACLE <name> <what> expected=..,..,.. actual=..,..,.. tol=N PASS|FAIL
 *   LAYOUT visual_artwork width=.. height=.. scale=.. text=.. font=..
 *   TIMING <name> draw_us=.. present_us=.. capture_us=.. appclip=x,y,w,h
 *   LOADED <module> <path>
 * and per-case raw dumps <out>/<name>-s<scale>.bgra + <out>/<name>-s<scale>.meta;
 * the visual_artwork case also writes <out>/visual_artwork-s<scale>.png with the
 * CairoPNG API.  The BitBlt DIB alpha byte is undefined for a window;
 * capture_dump() normalizes only that alpha byte to 255 before writing, so the
 * raw byte hash is stable.  RGB is never altered and every RGB oracle stays
 * strict.
 *
 * Exit: 0 every requested case PASSed; 1 a requested case FAILed; 3 no case
 * failed but a requested case was SKIPped or produced no CASE line (incomplete);
 * 2 usage. A missing/skipped/unexecuted case is never reported as a pass.
 * Requires an interactive console session; session 0 GTK window creation is
 * not supported by this host and is reported as a harness precondition
 * failure, not as a rendering verdict.
 */

#include <gtk/gtk.h>
#include <gdk/win32/gdkwin32.h>
#include <pango/pangocairo.h>
#include <windows.h>
#include <psapi.h>
#include <wtsapi32.h>
#include <stdio.h>
#include <stdarg.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>

#define MAX_RECTS 8
#define WIN_W 200
#define WIN_H 160
#define LIFECYCLE_ROUNDS 50

typedef struct {
    int x, y, w, h;
    double r, g, b, a;
} Rect;

typedef struct {
    Rect overlay[MAX_RECTS];
    int n_overlay;
    gint64 t_draw_start, t_draw_end;
    double clip[4];
    int artwork;                /* draw the O11 visual_artwork composite */
    char font_report[128];      /* actual Pango font resolved by draw_artwork */
} Scene;

typedef struct {
    int width, height, stride;
    unsigned char *pixels; /* BGRA top-down */
} Capture;

typedef struct {
    GdkSurface *surface;
    GdkCairoContext *ctx;
    int lw, lh;
    double scale;
} DrawSurface;

typedef struct {
    GtkWidget *win;
    GtkWidget *da;
    GdkSurface *surface;
    gboolean painted;
    gint64 t_submit;  /* queue_draw submission boundary (monotonic us) */
    gint64 t_painted; /* after-paint completion (monotonic us) */
    Scene scene;
} GtkScene;

typedef struct {
    int pass, fail, skip;
} Results;

static Results g_results;
static const char *g_outdir = NULL;
static char *g_dump_tag = NULL; /* "s<scale>", keeps dumps unique per scale */
static int g_physical = 0;
static int g_rounds = LIFECYCLE_ROUNDS;
static int g_expect_retained = 0;
static int g_in_render = 0;

/* Physical-session classification. A nonzero session id alone is NOT physical
 * (RDP sessions are nonzero, and session 0 has been observed to report
 * WTSClientProtocolType=Console), so classification also requires the session
 * to be the active console session, the WTS protocol to be console, and
 * GetSystemMetrics(SM_REMOTESESSION) to be false. */
static DWORD g_session_id = 0;
static DWORD g_active_console = 0xFFFFFFFFu;
static int g_wts_protocol = -1;   /* WTSClientProtocolType; -1 = unknown */
static int g_remote_session = -1; /* SM_REMOTESESSION */
static int g_physical_ok = 0;

static void detect_session(void)
{
    LPWSTR buf = NULL;
    DWORD bytes = 0;
    BOOL got;

    ProcessIdToSessionId(GetCurrentProcessId(), &g_session_id);
    g_active_console = WTSGetActiveConsoleSessionId();
    g_remote_session = GetSystemMetrics(SM_REMOTESESSION) ? 1 : 0;
    got = WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, g_session_id,
                                      WTSClientProtocolType, &buf, &bytes);
    if (got && buf && bytes >= sizeof(USHORT))
        g_wts_protocol = (int)(*(USHORT *)buf);
    else
        g_wts_protocol = -1;
    if (buf)
        WTSFreeMemory(buf);

    g_physical_ok = g_session_id != 0 &&
                    g_session_id == g_active_console &&
                    g_wts_protocol == 0 &&
                    g_remote_session == 0;
}

static gint64 now_us(void) { return g_get_monotonic_time(); }

/* Convert a logical coordinate to a physical pixel with explicit rounding.
 * gdk_surface_get_scale() is a double and may be fractional (e.g. 1.5); never
 * truncate or round the scale itself. */
static int px_phys(double logical, double scale)
{
    double v = logical * scale;
    return (int)(v + 0.5);
}

/* Compare a requested scale against the runtime scale without truncation. */
static int scale_matches(double actual, double wanted)
{
    return wanted <= 0.0 || (actual <= wanted + 1e-6 && actual >= wanted - 1e-6);
}

/* Non-blocking event drain: a blocking g_main_context_iteration() can sleep
 * past a deadline, so every bounded wait interval is non-blocking + short
 * sleep instead. */
static void pump_events(void)
{
    while (g_main_context_pending(NULL))
        g_main_context_iteration(NULL, FALSE);
    g_usleep(1000);
}

static void out(const char *fmt, ...) G_GNUC_PRINTF(1, 2);

static void out(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
}

/* ---------------------------------------------------------------- capture */

static int capture_hwnd(HWND hwnd, Capture *cap)
{
    RECT rc;
    HDC hdc, mem;
    BITMAPINFO bi;
    HBITMAP dib;
    HGDIOBJ old;
    void *bits = NULL;
    BOOL ok;

    memset(cap, 0, sizeof(*cap));
    if (hwnd == NULL || !GetClientRect(hwnd, &rc))
        return 0;
    if (rc.right <= 0 || rc.bottom <= 0)
        return 0;

    hdc = GetDC(hwnd);
    if (!hdc)
        return 0;

    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = rc.right;
    bi.bmiHeader.biHeight = -rc.bottom;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    dib = CreateDIBSection(hdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!dib) {
        ReleaseDC(hwnd, hdc);
        return 0;
    }
    mem = CreateCompatibleDC(hdc);
    if (!mem) {
        DeleteObject(dib);
        ReleaseDC(hwnd, hdc);
        return 0;
    }
    old = SelectObject(mem, dib);
    if (old == NULL || old == HGDI_ERROR) {
        DeleteDC(mem);
        DeleteObject(dib);
        ReleaseDC(hwnd, hdc);
        return 0;
    }
    ok = BitBlt(mem, 0, 0, rc.right, rc.bottom, hdc, 0, 0, SRCCOPY);
    GdiFlush(); /* completion boundary: queued GDI reads of the window finish */

    /* Copy the presented pixels out of the DIB section while it is still alive.
     * DeleteObject() below destroys the section and frees `bits`, so reading
     * `bits` afterwards is a use-after-free (observed on the physical console
     * as a 0xC0000005 access violation inside the CRT memcpy), independent of
     * the renderer. */
    if (ok) {
        cap->width = rc.right;
        cap->height = rc.bottom;
        cap->stride = rc.right * 4;
        cap->pixels = g_malloc((gsize)cap->stride * cap->height);
        memcpy(cap->pixels, bits, (gsize)cap->stride * cap->height);
    }

    SelectObject(mem, old);
    DeleteDC(mem);
    DeleteObject(dib);
    ReleaseDC(hwnd, hdc);

    if (!ok) {
        g_free(cap->pixels);
        memset(cap, 0, sizeof(*cap));
        return 0;
    }
    return 1;
}

static void capture_free(Capture *cap)
{
    g_free(cap->pixels);
    memset(cap, 0, sizeof(*cap));
}

static int captures_rgb_equal(const Capture *a, const Capture *b)
{
    int y;
    if (a->width != b->width || a->height != b->height || a->stride != b->stride)
        return 0;
    for (y = 0; y < a->height; y++) {
        const unsigned char *pa = a->pixels + (gsize)y * a->stride;
        const unsigned char *pb = b->pixels + (gsize)y * b->stride;
        int x;
        for (x = 0; x < a->width; x++)
            if (pa[x * 4 + 0] != pb[x * 4 + 0] ||
                pa[x * 4 + 1] != pb[x * 4 + 1] ||
                pa[x * 4 + 2] != pb[x * 4 + 2])
                return 0;
    }
    return 1;
}

/* Compare RGB outside a physical-pixel rect: the untouched-area oracle. */
static int captures_rgb_equal_outside(const Capture *a, const Capture *b,
                                      int rx, int ry, int rw, int rh)
{
    int y;
    if (a->width != b->width || a->height != b->height || a->stride != b->stride)
        return 0;
    for (y = 0; y < a->height; y++) {
        const unsigned char *pa = a->pixels + (gsize)y * a->stride;
        const unsigned char *pb = b->pixels + (gsize)y * b->stride;
        int x;
        for (x = 0; x < a->width; x++) {
            if (x >= rx && x < rx + rw && y >= ry && y < ry + rh)
                continue;
            if (pa[x * 4 + 0] != pb[x * 4 + 0] ||
                pa[x * 4 + 1] != pb[x * 4 + 1] ||
                pa[x * 4 + 2] != pb[x * 4 + 2])
                return 0;
        }
    }
    return 1;
}

/* Half-open membership in a logical/physical rect: x in [rx,rx+rw),
 * y in [ry,ry+rh). Used by self_test() to keep O2's outside-damage sample
 * provably outside the damage rect. */
static int point_in_rect(int x, int y, int rx, int ry, int rw, int rh)
{
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

static int capture_dump(const char *name, Capture *cap)
{
    char *path;
    FILE *f;
    int y;

    if (!g_outdir)
        return 1;

    /* A window BitBlt DIB has no alpha channel: the alpha byte is undefined.
     * Normalize only alpha so the raw byte hash is stable across runs; RGB is
     * left exactly as captured. */
    for (y = 0; y < cap->height; y++) {
        unsigned char *p = cap->pixels + (gsize)y * cap->stride;
        int x;
        for (x = 0; x < cap->width; x++)
            p[x * 4 + 3] = 255;
    }

    path = g_strdup_printf("%s/%s-%s.bgra", g_outdir,
                           name, g_dump_tag ? g_dump_tag : "sauto");
    f = fopen(path, "wb");
    if (!f) {
        out("DUMP %s FAIL cannot-open %s\n", name, path);
        g_free(path);
        return 0;
    }
    fwrite(cap->pixels, 1, (size_t)cap->stride * cap->height, f);
    fclose(f);
    {
        char *meta = g_strdup_printf("%s/%s-%s.meta", g_outdir, name,
                                     g_dump_tag ? g_dump_tag : "sauto");
        f = fopen(meta, "w");
        if (f) {
            fprintf(f, "name=%s\nwidth=%d\nheight=%d\nstride=%d\n",
                    name, cap->width, cap->height, cap->stride);
            fclose(f);
        }
        g_free(meta);
    }
    out("DUMP %s %s\n", name, path);
    g_free(path);
    return 1;
}

/* Write the same normalized capture as a PNG for human review. On little-endian
 * Windows the BGRA bytes with alpha forced to 255 are exactly
 * CAIRO_FORMAT_ARGB32, so no new framework is needed: the CairoPNG API writes
 * the image surface directly. */
static int capture_dump_png(const char *name, Capture *cap)
{
    char *path;
    cairo_surface_t *surface;
    cairo_status_t st;

    if (!g_outdir)
        return 1;

    path = g_strdup_printf("%s/%s-%s.png", g_outdir, name,
                           g_dump_tag ? g_dump_tag : "sauto");
    surface = cairo_image_surface_create_for_data(cap->pixels,
                                                  CAIRO_FORMAT_ARGB32,
                                                  cap->width, cap->height,
                                                  cap->stride);
    st = cairo_surface_write_to_png(surface, path);
    cairo_surface_destroy(surface);
    if (st != CAIRO_STATUS_SUCCESS) {
        out("PNG %s FAIL %s\n", name, cairo_status_to_string(st));
        g_free(path);
        return 0;
    }
    out("PNG %s %s\n", name, path);
    g_free(path);
    return 1;
}

/* ---------------------------------------------------------------- oracle  */

static void bg_rgb(int w, int h, int x, int y, int *r, int *g, int *b)
{
    if (x < w / 2 && y < h / 2) { *r = 255; *g = 0; *b = 0; }
    else if (x >= w / 2 && y < h / 2) { *r = 0; *g = 255; *b = 0; }
    else if (x < w / 2) { *r = 0; *g = 0; *b = 255; }
    else { *r = 255; *g = 255; *b = 255; }
}

/* Exact whole-client RGB for the opaque grid; even logical half-boundaries
 * land on whole physical pixels at the proof scales. */
static int surface_client_size(GdkSurface *surface, int *cw, int *ch);

static int whole_grid(const char *name, DrawSurface *ds, const Capture *cap)
{
    int cw, ch, x, y, bad = 0;
    if (!surface_client_size(ds->surface, &cw, &ch) ||
        !cap->pixels || cap->width != cw || cap->height != ch)
        return 0;
    for (y = 0; y < ch; y++) for (x = 0; x < cw; x++) {
        const unsigned char *p = cap->pixels + (gsize)y * cap->stride + x * 4;
        int left = x < px_phys(ds->lw / 2.0, ds->scale);
        int top = y < px_phys(ds->lh / 2.0, ds->scale);
        int r = left && top ? 255 : !left && !top ? 255 : 0;
        int g = !left ? 255 : 0, b = !top ? 255 : 0;
        if (p[2] != r || p[1] != g || p[0] != b) bad++;
    }
    out("ORACLE %s whole-grid size=%dx%d mismatches=%d %s\n",
        name, cw, ch, bad, bad ? "FAIL" : "PASS");
    return bad == 0;
}

static int seed_client(DrawSurface *ds)
{
    HWND hwnd = GDK_SURFACE_HWND(ds->surface);
    RECT rc; HDC dc; HBRUSH brush;
    if (!GetClientRect(hwnd, &rc) || !(dc = GetDC(hwnd))) return 0;
    brush = CreateSolidBrush(RGB(19, 23, 29));
    if (!brush) { ReleaseDC(hwnd, dc); return 0; }
    int ok = FillRect(dc, &rc, brush) && GdiFlush();
    DeleteObject(brush); ReleaseDC(hwnd, dc);
    return ok;
}

static SIZE_T private_bytes(void)
{
    PROCESS_MEMORY_COUNTERS_EX m = {0};
    return GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&m,
                                sizeof(m)) ? m.PrivateUsage : 0;
}

static int check_px(const char *test, const char *what, const Capture *cap,
                    int px, int py, int er, int eg, int eb, int tol)
{
    const unsigned char *p;
    int ar, ag, ab;

    if (px < 0 || py < 0 || px >= cap->width || py >= cap->height) {
        out("ORACLE %s %s expected=%d,%d,%d actual=out-of-bounds tol=%d FAIL\n",
            test, what, er, eg, eb, tol);
        return 0;
    }
    p = cap->pixels + (gsize)py * cap->stride + (gsize)px * 4;
    ab = p[0]; ag = p[1]; ar = p[2];
    if (abs(ar - er) <= tol && abs(ag - eg) <= tol && abs(ab - eb) <= tol) {
        out("ORACLE %s %s expected=%d,%d,%d actual=%d,%d,%d tol=%d PASS\n",
            test, what, er, eg, eb, ar, ag, ab, tol);
        return 1;
    }
    out("ORACLE %s %s expected=%d,%d,%d actual=%d,%d,%d tol=%d FAIL\n",
        test, what, er, eg, eb, ar, ag, ab, tol);
    return 0;
}

/* ---------------------------------------------------------------- scene   */

/* ---- O11 visual_artwork content oracle (defined in ORACLES.md) ----------- */

/* Gather per-image content statistics used to reject a blank/uniform capture
 * before any baseline/candidate byte comparison can "pass" on a blank arm. */
static void artwork_content_stats(const Capture *cap, int *distinct_out,
                                  long *nonbg_out, int span_out[3])
{
    GHashTable *seen = g_hash_table_new(g_direct_hash, g_direct_equal);
    const unsigned char *p0 = cap->pixels;
    int rmin[3] = { 255, 255, 255 }, rmax[3] = { 0, 0, 0 };
    long nonbg = 0;
    int x, y;

    for (y = 0; y < cap->height; y++) {
        const unsigned char *p = cap->pixels + (gsize)y * cap->stride;
        for (x = 0; x < cap->width; x++) {
            int b = p[0], g = p[1], r = p[2];
            int c = (r << 16) | (g << 8) | b;
            g_hash_table_add(seen, GINT_TO_POINTER(c + 1));
            if (r < rmin[0]) rmin[0] = r;
            if (g < rmin[1]) rmin[1] = g;
            if (b < rmin[2]) rmin[2] = b;
            if (r > rmax[0]) rmax[0] = r;
            if (g > rmax[1]) rmax[1] = g;
            if (b > rmax[2]) rmax[2] = b;
            if (abs(r - p0[2]) > 2 || abs(g - p0[1]) > 2 || abs(b - p0[0]) > 2)
                nonbg++;
            p += 4;
        }
    }
    if (distinct_out)
        *distinct_out = (int)g_hash_table_size(seen);
    if (nonbg_out)
        *nonbg_out = nonbg;
    if (span_out) {
        span_out[0] = rmax[0] - rmin[0];
        span_out[1] = rmax[1] - rmin[1];
        span_out[2] = rmax[2] - rmin[2];
    }
    g_hash_table_destroy(seen);
}

/* Nonempty/nonuniform gate: at least 64 distinct RGB triples, at least 5% of
 * pixels differing from the first pixel, and at least two channels spanning
 * >= 32 levels. Pure so the self-test can exercise the blank-negative case. */
static int content_nonuniform_ok(int distinct, long nonbg, long total,
                                 const int span[3])
{
    int spanning = (span[0] >= 32) + (span[1] >= 32) + (span[2] >= 32);
    return total > 0 && distinct >= 64 && nonbg * 100 >= total * 5 &&
           spanning >= 2;
}

static int artwork_content_check(const char *name, const Capture *cap)
{
    int distinct = 0, span[3] = { 0, 0, 0 };
    long nonbg = 0, total = (long)cap->width * cap->height;
    int ok;

    artwork_content_stats(cap, &distinct, &nonbg, span);
    ok = content_nonuniform_ok(distinct, nonbg, total, span);
    out("ORACLE %s content distinct=%d nonbg=%ld/%ld span=%d,%d,%d "
        "expected=nonempty,nonuniform %s\n",
        name, distinct, nonbg, total, span[0], span[1], span[2],
        ok ? "PASS" : "FAIL");
    return ok;
}

/* Deterministic composite for O11: gradient, thin antialiased lines, a cubic
 * Bezier, a rotated/scaled checkerboard bitmap, translucent overlap, and
 * clipped text whose actual resolved font family is reported. */
static void draw_artwork(cairo_t *cr, Scene *s, int lw, int lh)
{
    double w = lw > 0 ? (double)lw : 1.0;
    double h = lh > 0 ? (double)lh : 1.0;
    cairo_pattern_t *grad;
    cairo_surface_t *check;
    cairo_t *cc;
    PangoLayout *layout;
    PangoFontDescription *desc;
    PangoContext *pctx;
    PangoFont *font;
    PangoFontDescription *actual;
    char *fam;
    int i;

    cairo_clip_extents(cr, &s->clip[0], &s->clip[1], &s->clip[2], &s->clip[3]);
    s->clip[2] -= s->clip[0];
    s->clip[3] -= s->clip[1];

    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cairo_set_antialias(cr, CAIRO_ANTIALIAS_DEFAULT);

    /* opaque base */
    cairo_set_source_rgb(cr, 0.06, 0.07, 0.10);
    cairo_paint(cr);

    /* linear gradient band */
    grad = cairo_pattern_create_linear(0, 0, w, 0);
    cairo_pattern_add_color_stop_rgb(grad, 0.0, 0.90, 0.10, 0.10);
    cairo_pattern_add_color_stop_rgb(grad, 0.5, 0.10, 0.80, 0.30);
    cairo_pattern_add_color_stop_rgb(grad, 1.0, 0.15, 0.25, 0.95);
    cairo_set_source(cr, grad);
    cairo_rectangle(cr, 0, 0, w, h * 0.24);
    cairo_fill(cr);
    cairo_pattern_destroy(grad);

    /* thin antialiased lines */
    cairo_set_line_width(cr, 0.6);
    for (i = 0; i < 6; i++) {
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.65);
        cairo_move_to(cr, 6.0, h * 0.30 + i * 3.0);
        cairo_line_to(cr, w - 6.0, h * 0.30 + i * 3.0);
        cairo_stroke(cr);
    }

    /* antialiased cubic Bezier */
    cairo_set_source_rgba(cr, 1.0, 0.85, 0.10, 1.0);
    cairo_set_line_width(cr, 2.0);
    cairo_move_to(cr, 8.0, h * 0.60);
    cairo_curve_to(cr, w * 0.30, h * 0.40, w * 0.70, h * 0.84, w - 8.0,
                   h * 0.56);
    cairo_stroke(cr);

    /* rotated/scaled checkerboard bitmap */
    check = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 8, 8);
    cc = cairo_create(check);
    cairo_set_source_rgb(cc, 0.05, 0.05, 0.05);
    cairo_paint(cc);
    cairo_set_source_rgb(cc, 0.95, 0.95, 0.95);
    cairo_rectangle(cc, 0, 0, 4, 4);
    cairo_fill(cc);
    cairo_rectangle(cc, 4, 4, 4, 4);
    cairo_fill(cc);
    cairo_destroy(cc);

    cairo_save(cr);
    cairo_translate(cr, w * 0.62, h * 0.74);
    cairo_rotate(cr, 0.35);
    cairo_scale(cr, 2.0, 2.0);
    cairo_set_source_surface(cr, check, -8, -8);
    cairo_rectangle(cr, -8, -8, 16, 16);
    cairo_fill(cr);
    cairo_restore(cr);
    cairo_surface_destroy(check);

    /* translucent overlap */
    cairo_set_source_rgba(cr, 0.95, 0.15, 0.35, 0.65);
    cairo_rectangle(cr, w * 0.10, h * 0.42, w * 0.36, h * 0.34);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, 0.15, 0.45, 0.95, 0.65);
    cairo_rectangle(cr, w * 0.24, h * 0.52, w * 0.36, h * 0.34);
    cairo_fill(cr);

    /* clipped text, with the actual resolved font family reported */
    cairo_save(cr);
    cairo_rectangle(cr, 6.0, 4.0, w - 12.0, h * 0.16);
    cairo_clip(cr);

    layout = pango_cairo_create_layout(cr);
    pango_layout_set_text(layout, "VA Studio 0123", -1);
    desc = pango_font_description_new();
    pango_font_description_set_family(desc, "DejaVu Sans");
    pango_font_description_set_absolute_size(desc, 13 * PANGO_SCALE);
    pango_layout_set_font_description(layout, desc);
    pango_font_description_free(desc);

    s->font_report[0] = '\0';
    pctx = pango_layout_get_context(layout);
    font = pango_context_load_font(pctx,
                                   pango_layout_get_font_description(layout));
    if (font) {
        actual = pango_font_describe(font);
        if (actual) {
            fam = pango_font_description_to_string(actual);
            if (fam) {
                g_strlcpy(s->font_report, fam, sizeof(s->font_report));
                g_strdelimit(s->font_report, " ", '_');
                g_free(fam);
            }
            pango_font_description_free(actual);
        }
        g_object_unref(font);
    }
    if (!s->font_report[0])
        g_strlcpy(s->font_report, "unknown", sizeof(s->font_report));

    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_move_to(cr, 8.0, 6.0);
    pango_cairo_show_layout(cr, layout);
    g_object_unref(layout);
    cairo_restore(cr);
}

static void draw_scene(cairo_t *cr, Scene *s, int lw, int lh)
{
    double x1, y1, x2, y2;
    int i;

    s->t_draw_start = now_us();
    if (s->artwork) {
        draw_artwork(cr, s, lw, lh);
        s->t_draw_end = now_us();
        return;
    }
    cairo_clip_extents(cr, &x1, &y1, &x2, &y2);
    s->clip[0] = x1; s->clip[1] = y1; s->clip[2] = x2 - x1; s->clip[3] = y2 - y1;

    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cairo_set_source_rgb(cr, 1, 0, 0);
    cairo_rectangle(cr, 0, 0, lw / 2.0, lh / 2.0);
    cairo_fill(cr);
    cairo_set_source_rgb(cr, 0, 1, 0);
    cairo_rectangle(cr, lw / 2.0, 0, lw / 2.0, lh / 2.0);
    cairo_fill(cr);
    cairo_set_source_rgb(cr, 0, 0, 1);
    cairo_rectangle(cr, 0, lh / 2.0, lw / 2.0, lh / 2.0);
    cairo_fill(cr);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_rectangle(cr, lw / 2.0, lh / 2.0, lw / 2.0, lh / 2.0);
    cairo_fill(cr);

    for (i = 0; i < s->n_overlay; i++) {
        Rect *r = &s->overlay[i];
        cairo_set_source_rgba(cr, r->r, r->g, r->b, r->a);
        cairo_rectangle(cr, r->x, r->y, r->w, r->h);
        cairo_fill(cr);
    }
    s->t_draw_end = now_us();
}

/* ------------------------------------------------------------- DrawSurface */

static double surface_scale(GdkSurface *surface)
{
    double s = gdk_surface_get_scale(surface);
    return s > 0.0 ? s : 1.0;
}

static void surface_set_client_size(GdkSurface *surface, int cw, int ch)
{
    HWND hwnd = GDK_SURFACE_HWND(surface);
    RECT r = { 0, 0, cw, ch };
    DWORD style = (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE);
    DWORD ex = (DWORD)GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    AdjustWindowRectEx(&r, style, FALSE, ex);
    SetWindowPos(hwnd, NULL, 120, 120, r.right - r.left, r.bottom - r.top,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    while (g_main_context_pending(NULL))
        g_main_context_iteration(NULL, FALSE);
}

/* Report the real HWND client size so callers never assume a resize was
 * accepted by the window manager. */
static int surface_client_size(GdkSurface *surface, int *cw, int *ch)
{
    RECT rc;
    HWND hwnd = GDK_SURFACE_HWND(surface);
    if (hwnd == NULL || !GetClientRect(hwnd, &rc))
        return 0;
    *cw = rc.right;
    *ch = rc.bottom;
    return 1;
}

/* Bounded readiness predicate for a resize/allocation. GDK publishes the new
 * surface metrics from GdkSurface::layout on the frame clock (Win32 WM_SIZE ->
 * gdk_surface_request_layout -> gdk_win32_toplevel_compute_size), not inside
 * SetWindowPos, so a single drain can observe the old size. Return 1 only when
 * the real Win32 client rect AND the public GDK surface metrics both equal the
 * requested physical size; the old size is never accepted. */
static int surface_wait_physical_size(GdkSurface *surface, int pw, int ph)
{
    gint64 deadline = now_us() + 5 * G_TIME_SPAN_SECOND;

    for (;;) {
        int cw = -1, ch = -1;
        double scale = surface_scale(surface);
        int gw = gdk_surface_get_width(surface);
        int gh = gdk_surface_get_height(surface);

        if (surface_client_size(surface, &cw, &ch) &&
            cw == pw && ch == ph &&
            gw > 0 && gh > 0 &&
            px_phys(gw, scale) == pw && px_phys(gh, scale) == ph)
            return 1;
        if (now_us() >= deadline)
            return 0;
        pump_events();
    }
}

/* Bounded wait for a GtkWindow/GtkDrawingArea to be mapped and allocated at
 * the requested logical size. Uses only public GDK/Gtk size getters so the
 * scene is genuinely 200x160 before the frame is queued. */
static int gtk_wait_allocated(GtkScene *g, int lw, int lh)
{
    gint64 deadline = now_us() + 5 * G_TIME_SPAN_SECOND;

    for (;;) {
        if (g->surface && gdk_surface_get_mapped(g->surface) &&
            gdk_surface_get_width(g->surface) == lw &&
            gdk_surface_get_height(g->surface) == lh &&
            gtk_widget_get_width(g->da) == lw &&
            gtk_widget_get_height(g->da) == lh)
            return 1;
        if (now_us() >= deadline)
            return 0;
        pump_events();
    }
}

static int surface_sync_metrics(DrawSurface *ds)
{
    ds->lw = gdk_surface_get_width(ds->surface);
    ds->lh = gdk_surface_get_height(ds->surface);
    ds->scale = surface_scale(ds->surface);
    return ds->lw > 1 && ds->lh > 1;
}

static int draw_surface_create(DrawSurface *ds, int lw, int lh)
{
    GdkDisplay *d = gdk_display_get_default();
    GdkToplevelLayout *layout;
    gint64 deadline;

    memset(ds, 0, sizeof(*ds));
    if (!d)
        return 0;
    ds->surface = gdk_surface_new_toplevel(d);
    if (!ds->surface)
        return 0;
    gdk_toplevel_set_title(GDK_TOPLEVEL(ds->surface), "gtk-cairo-buffer-test");
    layout = gdk_toplevel_layout_new();
    gdk_toplevel_present(GDK_TOPLEVEL(ds->surface), layout);
    gdk_toplevel_layout_unref(layout);
    deadline = now_us() + 5 * G_TIME_SPAN_SECOND;
    while (!gdk_surface_get_mapped(ds->surface) && now_us() < deadline)
        pump_events();
    if (!gdk_surface_get_mapped(ds->surface))
        return 0;

    ds->scale = surface_scale(ds->surface);
    surface_set_client_size(ds->surface, px_phys(lw, ds->scale),
                            px_phys(lh, ds->scale));
    deadline = now_us() + 5 * G_TIME_SPAN_SECOND;
    while ((gdk_surface_get_width(ds->surface) <= 1 ||
            gdk_surface_get_height(ds->surface) <= 1) && now_us() < deadline)
        pump_events();

    ds->ctx = gdk_surface_create_cairo_context(ds->surface);
    return ds->ctx && surface_sync_metrics(ds);
}

static void draw_surface_destroy(DrawSurface *ds)
{
    if (ds->ctx)
        g_object_unref(ds->ctx);
    if (ds->surface)
        gdk_surface_destroy(ds->surface);
    memset(ds, 0, sizeof(*ds));
}

/* Draw one damage-limited frame through the real patched GDK context. */
static int draw_surface_frame(DrawSurface *ds, Scene *scene, int nrects,
                              const cairo_rectangle_int_t *rects)
{
    cairo_region_t *region;
    cairo_t *cr;
    int i;

    if (nrects > 0) {
        region = cairo_region_create();
        for (i = 0; i < nrects; i++)
            cairo_region_union_rectangle(region, &rects[i]);
    } else {
        region = cairo_region_create_rectangle(
            &(cairo_rectangle_int_t){ 0, 0, ds->lw, ds->lh });
    }

    gdk_draw_context_begin_frame(GDK_DRAW_CONTEXT(ds->ctx), region);
    cr = gdk_cairo_context_cairo_create(GDK_CAIRO_CONTEXT(ds->ctx));
    if (!cr) {
        gdk_draw_context_end_frame(GDK_DRAW_CONTEXT(ds->ctx));
        cairo_region_destroy(region);
        return 0;
    }
    draw_scene(cr, scene, ds->lw, ds->lh);
    cairo_destroy(cr);
    gdk_draw_context_end_frame(GDK_DRAW_CONTEXT(ds->ctx));
    cairo_region_destroy(region);
    if (!g_in_render)
        while (g_main_context_pending(NULL))
            g_main_context_iteration(NULL, FALSE);
    return 1;
}

static int draw_surface_capture(DrawSurface *ds, Capture *cap)
{
    return capture_hwnd(GDK_SURFACE_HWND(ds->surface), cap);
}

static int draw_surface_size_ok(DrawSurface *ds, const Capture *cap)
{
    return cap->width == px_phys(ds->lw, ds->scale) &&
           cap->height == px_phys(ds->lh, ds->scale);
}

/* ---------------------------------------------------------------- reporting */

static void case_result(const char *name, int outcome, const char *detail)
{
    const char *r = outcome == 0 ? "PASS" : (outcome == 2 ? "SKIP" : "FAIL");
    out("CASE %s RESULT %s %s\n", name, r, detail ? detail : "");
    if (outcome == 0) g_results.pass++;
    else if (outcome == 2) g_results.skip++;
    else g_results.fail++;
}

static void report_timing(const char *name, const Scene *s, gint64 present_us,
                          gint64 capture_us)
{
    out("TIMING %s draw_us=%" G_GINT64_FORMAT " present_us=%" G_GINT64_FORMAT
        " capture_us=%" G_GINT64_FORMAT " appclip=%.0f,%.0f,%.0f,%.0f\n",
        name, s->t_draw_end - s->t_draw_start, present_us, capture_us,
        s->clip[0], s->clip[1], s->clip[2], s->clip[3]);
}

/* ---------------------------------------------------------------- cases   */

static int full_quadrant_checks(const char *name, Capture *cap, int lw, int lh,
                                double scale, int *mismatch)
{
    int cx[4], cy[4], i, ok = 1;
    cx[0] = lw / 4;     cy[0] = lh / 4;
    cx[1] = 3 * lw / 4; cy[1] = lh / 4;
    cx[2] = lw / 4;     cy[2] = 3 * lh / 4;
    cx[3] = 3 * lw / 4; cy[3] = 3 * lh / 4;
    for (i = 0; i < 4; i++) {
        int r, g, b;
        char what[16];
        bg_rgb(lw, lh, cx[i], cy[i], &r, &g, &b);
        g_snprintf(what, sizeof(what), "quad%d", i);
        if (!check_px(name, what, cap, px_phys(cx[i], scale),
                      px_phys(cy[i], scale), r, g, b, 0)) {
            ok = 0;
            if (mismatch) *mismatch = 1;
        }
    }
    return ok;
}

static int case_full(double wanted_scale)
{
    DrawSurface ds;
    Scene scene = {0};
    Capture cap;
    int ok = 1, mismatch = 0;
    gint64 t_present, t_capture;

    if (!draw_surface_create(&ds, WIN_W, WIN_H)) {
        case_result("full", 1, "surface-create-failed");
        return 1;
    }
    if (!scale_matches(ds.scale, wanted_scale)) {
        draw_surface_destroy(&ds);
        case_result("full", 2, "requested-scale-unavailable");
        return 2;
    }
    t_present = now_us();
    if (!draw_surface_frame(&ds, &scene, 0, NULL)) {
        draw_surface_destroy(&ds);
        case_result("full", 1, "frame-failed");
        return 1;
    }
    t_present = now_us() - t_present;
    t_capture = now_us();
    if (!draw_surface_capture(&ds, &cap)) {
        draw_surface_destroy(&ds);
        case_result("full", 1, "capture-failed");
        return 1;
    }
    t_capture = now_us() - t_capture;
    if (!draw_surface_size_ok(&ds, &cap)) {
        out("ORACLE full size expected=%dx%d actual=%dx%d FAIL\n",
            px_phys(ds.lw, ds.scale), px_phys(ds.lh, ds.scale),
            cap.width, cap.height);
        mismatch = 1;
    }
    full_quadrant_checks("full", &cap, ds.lw, ds.lh, ds.scale, &mismatch);
    capture_dump("full", &cap);
    report_timing("full", &scene, t_present, t_capture);
    capture_free(&cap);
    ok = !mismatch;
    draw_surface_destroy(&ds);
    case_result("full", ok ? 0 : 1, ok ? "quadrants-exact" : "pixel-mismatch");
    return ok ? 0 : 1;
}

static int case_partial(double wanted_scale)
{
    DrawSurface ds;
    Scene scene = {0};
    Capture before, after;
    int ok = 1, mismatch = 0, r, g, b;
    gint64 t_present, t_capture;
    cairo_rectangle_int_t damage = { 20, 20, 40, 30 };

    if (!draw_surface_create(&ds, WIN_W, WIN_H)) {
        case_result("partial", 1, "surface-create-failed");
        return 1;
    }
    if (!scale_matches(ds.scale, wanted_scale)) {
        draw_surface_destroy(&ds);
        case_result("partial", 2, "requested-scale-unavailable");
        return 2;
    }
    if (!draw_surface_frame(&ds, &scene, 0, NULL) || !draw_surface_capture(&ds, &before)) {
        draw_surface_destroy(&ds);
        case_result("partial", 1, "baseline-frame-failed");
        return 1;
    }
    scene.overlay[0] = (Rect){ 20, 20, 40, 30, 0, 1, 1, 1 };
    scene.n_overlay = 1;
    t_present = now_us();
    if (!draw_surface_frame(&ds, &scene, 1, &damage)) {
        capture_free(&before);
        draw_surface_destroy(&ds);
        case_result("partial", 1, "frame-failed");
        return 1;
    }
    t_present = now_us() - t_present;
    t_capture = now_us();
    if (!draw_surface_capture(&ds, &after)) {
        capture_free(&before);
        draw_surface_destroy(&ds);
        case_result("partial", 1, "capture-failed");
        return 1;
    }
    t_capture = now_us() - t_capture;

    if (!check_px("partial", "inside", &after, px_phys(40, ds.scale),
                  px_phys(35, ds.scale), 0, 255, 255, 0))
        mismatch = 1;
    bg_rgb(ds.lw, ds.lh, ds.lw / 4, ds.lh / 4, &r, &g, &b);
    /* Sample provably OUTSIDE damage (20,20,40,30): x=10<20 and y=10<20, yet
     * still inside the top-left quadrant (x<w/2, y<h/2). The previous sample at
     * (lw/4,lh/4)=(50,40) was inside R and only compared the overlay to
     * itself. Kept in logical coordinates so it stays outside R at any scale. */
    if (!check_px("partial", "outside-tl", &after, px_phys(10, ds.scale),
                  px_phys(10, ds.scale), r, g, b, 0))
        mismatch = 1;
    bg_rgb(ds.lw, ds.lh, 3 * ds.lw / 4, 3 * ds.lh / 4, &r, &g, &b);
    if (!check_px("partial", "outside-br", &after,
                  px_phys(3 * ds.lw / 4, ds.scale),
                  px_phys(3 * ds.lh / 4, ds.scale), r, g, b, 0))
        mismatch = 1;
    /* Every pixel outside the single damage rect must be RGB-identical. */
    if (!captures_rgb_equal_outside(&before, &after, px_phys(20, ds.scale),
                                    px_phys(20, ds.scale), px_phys(40, ds.scale),
                                    px_phys(30, ds.scale))) {
        out("ORACLE partial untouched-outside-damage byte-compare FAIL\n");
        mismatch = 1;
    } else {
        out("ORACLE partial untouched-outside-damage byte-compare PASS\n");
    }
    capture_dump("partial", &after);
    report_timing("partial", &scene, t_present, t_capture);
    capture_free(&after);
    capture_free(&before);
    ok = !mismatch;
    draw_surface_destroy(&ds);
    case_result("partial", ok ? 0 : 1, ok ? "damage-and-untouched" : "pixel-mismatch");
    return ok ? 0 : 1;
}

static int case_multi(double wanted_scale)
{
    DrawSurface ds;
    Scene scene = {0};
    Capture cap;
    int ok = 1, mismatch = 0, r, g, b;
    gint64 t_present, t_capture;
    cairo_rectangle_int_t damage[2] = { { 20, 20, 40, 30 }, { 120, 90, 40, 30 } };

    if (!draw_surface_create(&ds, WIN_W, WIN_H)) {
        case_result("multi_damage", 1, "surface-create-failed");
        return 1;
    }
    if (!scale_matches(ds.scale, wanted_scale)) {
        draw_surface_destroy(&ds);
        case_result("multi_damage", 2, "requested-scale-unavailable");
        return 2;
    }
    if (!draw_surface_frame(&ds, &scene, 0, NULL)) {
        draw_surface_destroy(&ds);
        case_result("multi_damage", 1, "baseline-frame-failed");
        return 1;
    }
    scene.overlay[0] = (Rect){ 20, 20, 40, 30, 0, 1, 1, 1 };
    scene.overlay[1] = (Rect){ 120, 90, 40, 30, 1, 1, 0, 1 };
    scene.n_overlay = 2;
    t_present = now_us();
    if (!draw_surface_frame(&ds, &scene, 2, damage)) {
        draw_surface_destroy(&ds);
        case_result("multi_damage", 1, "frame-failed");
        return 1;
    }
    t_present = now_us() - t_present;
    t_capture = now_us();
    if (!draw_surface_capture(&ds, &cap)) {
        draw_surface_destroy(&ds);
        case_result("multi_damage", 1, "capture-failed");
        return 1;
    }
    t_capture = now_us() - t_capture;
    if (!check_px("multi_damage", "r1", &cap, px_phys(40, ds.scale),
                  px_phys(35, ds.scale), 0, 255, 255, 0))
        mismatch = 1;
    if (!check_px("multi_damage", "r2", &cap, px_phys(140, ds.scale),
                  px_phys(105, ds.scale), 255, 255, 0, 0))
        mismatch = 1;
    bg_rgb(ds.lw, ds.lh, 80, 35, &r, &g, &b);
    if (!check_px("multi_damage", "gap", &cap, px_phys(80, ds.scale),
                  px_phys(35, ds.scale), r, g, b, 0))
        mismatch = 1;
    capture_dump("multi_damage", &cap);
    report_timing("multi_damage", &scene, t_present, t_capture);
    capture_free(&cap);
    ok = !mismatch;
    draw_surface_destroy(&ds);
    case_result("multi_damage", ok ? 0 : 1, ok ? "two-dirty-rects" : "pixel-mismatch");
    return ok ? 0 : 1;
}

static int case_alpha_boundaries(double wanted_scale)
{
    DrawSurface ds;
    Scene scene = {0};
    Capture cap;
    int ok = 1, mismatch = 0;
    gint64 t_present, t_capture;

    if (!draw_surface_create(&ds, WIN_W, WIN_H)) {
        case_result("alpha", 1, "surface-create-failed");
        return 1;
    }
    if (!scale_matches(ds.scale, wanted_scale)) {
        draw_surface_destroy(&ds);
        case_result("alpha", 2, "requested-scale-unavailable");
        return 2;
    }

    scene.overlay[0] = (Rect){ 20, 20, 40, 30, 0, 1, 0, 0 };
    scene.n_overlay = 1;
    t_present = now_us();
    if (!draw_surface_frame(&ds, &scene, 0, NULL)) {
        draw_surface_destroy(&ds);
        case_result("alpha", 1, "frame-failed");
        return 1;
    }
    t_present = now_us() - t_present;
    t_capture = now_us();
    if (!draw_surface_capture(&ds, &cap)) {
        draw_surface_destroy(&ds);
        case_result("alpha", 1, "capture-failed");
        return 1;
    }
    t_capture = now_us() - t_capture;
    if (!check_px("alpha", "alpha0", &cap, px_phys(40, ds.scale),
                  px_phys(35, ds.scale), 255, 0, 0, 0))
        mismatch = 1;
    capture_dump("alpha0", &cap);
    report_timing("alpha0", &scene, t_present, t_capture);
    capture_free(&cap);

    scene.overlay[0] = (Rect){ 20, 20, 40, 30, 1, 0, 1, 1 };
    scene.n_overlay = 1;
    t_present = now_us();
    if (!draw_surface_frame(&ds, &scene, 0, NULL)) {
        draw_surface_destroy(&ds);
        case_result("alpha", 1, "frame-failed");
        return 1;
    }
    t_present = now_us() - t_present;
    t_capture = now_us();
    if (!draw_surface_capture(&ds, &cap)) {
        draw_surface_destroy(&ds);
        case_result("alpha", 1, "capture-failed");
        return 1;
    }
    t_capture = now_us() - t_capture;
    if (!check_px("alpha", "alpha1", &cap, px_phys(40, ds.scale),
                  px_phys(35, ds.scale), 255, 0, 255, 0))
        mismatch = 1;
    capture_dump("alpha1", &cap);
    report_timing("alpha1", &scene, t_present, t_capture);
    capture_free(&cap);
    ok = !mismatch;
    draw_surface_destroy(&ds);
    case_result("alpha", ok ? 0 : 1, ok ? "opaque-boundaries" : "pixel-mismatch");
    return ok ? 0 : 1;
}

static int case_alpha_half(double wanted_scale)
{
    DrawSurface ds;
    Scene scene = {0};
    Capture cap;
    int ok = 1, mismatch = 0, x, y;
    gint64 t_present, t_capture;
    const unsigned char *p;
    int ox, oy, ow, oh;

    if (!draw_surface_create(&ds, WIN_W, WIN_H)) {
        case_result("alpha_half", 1, "surface-create-failed");
        return 1;
    }
    if (!scale_matches(ds.scale, wanted_scale)) {
        draw_surface_destroy(&ds);
        case_result("alpha_half", 2, "requested-scale-unavailable");
        return 2;
    }
    ox = ds.lw / 2 + 10; oy = ds.lh / 2 + 10;
    ow = ds.lw / 4; oh = ds.lh / 4;
    scene.overlay[0] = (Rect){ ox, oy, ow, oh, 1, 0, 0, 0.5 };
    scene.n_overlay = 1;
    t_present = now_us();
    if (!draw_surface_frame(&ds, &scene, 0, NULL)) {
        draw_surface_destroy(&ds);
        case_result("alpha_half", 1, "frame-failed");
        return 1;
    }
    t_present = now_us() - t_present;
    t_capture = now_us();
    if (!draw_surface_capture(&ds, &cap)) {
        draw_surface_destroy(&ds);
        case_result("alpha_half", 1, "capture-failed");
        return 1;
    }
    t_capture = now_us() - t_capture;
    x = px_phys(ox + ow / 2, ds.scale);
    y = px_phys(oy + oh / 2, ds.scale);
    p = cap.pixels + (gsize)y * cap.stride + (gsize)x * 4;
    /* background here is white (255,255,255); source red (255,0,0). Sanity only. */
    if (!(p[2] >= 250 && p[1] >= 120 && p[1] <= 136 &&
          p[0] >= 120 && p[0] <= 136)) {
        out("ORACLE alpha_half sanity expected~=255,128,128 actual=%u,%u,%u FAIL\n",
            p[2], p[1], p[0]);
        mismatch = 1;
    } else {
        out("ORACLE alpha_half sanity expected~=255,128,128 actual=%u,%u,%u PASS\n",
            p[2], p[1], p[0]);
    }
    if (p[2] == 255 && p[1] == 255 && p[0] == 255) mismatch = 1;
    if (p[2] == 255 && p[1] == 0 && p[0] == 0) mismatch = 1;
    capture_dump("alpha_half", &cap);
    report_timing("alpha_half", &scene, t_present, t_capture);
    capture_free(&cap);
    ok = !mismatch;
    draw_surface_destroy(&ds);
    case_result("alpha_half", ok ? 0 : 1, "compare-with-baseline-required");
    return ok ? 0 : 1;
}

static int case_resize(double wanted_scale)
{
    DrawSurface ds;
    Scene scene = {0};
    Capture grown, shrunk;
    int ok = 1, mismatch = 0, r, g, b;
    int reqw, reqh, aw = -1, ah = -1;
    gint64 t_present, t_capture;

    memset(&grown, 0, sizeof(grown));
    memset(&shrunk, 0, sizeof(shrunk));
    if (!draw_surface_create(&ds, WIN_W, WIN_H)) {
        case_result("resize", 1, "surface-create-failed");
        return 1;
    }
    if (!scale_matches(ds.scale, wanted_scale)) {
        draw_surface_destroy(&ds);
        case_result("resize", 2, "requested-scale-unavailable");
        return 2;
    }

    /* Grow.  Never assume the WM accepted the size: read the real client rect
     * and require both the OS client size and GDK metrics to match. */
    reqw = px_phys(WIN_W + 80, ds.scale);
    reqh = px_phys(WIN_H + 60, ds.scale);
    surface_set_client_size(ds.surface, reqw, reqh);
    if (!surface_wait_physical_size(ds.surface, reqw, reqh)) {
        surface_client_size(ds.surface, &aw, &ah);
        out("ORACLE resize grow client-size requested=%dx%d actual=%dx%d FAIL\n",
            reqw, reqh, aw, ah);
        mismatch = 1;
    }
    surface_sync_metrics(&ds);
    if (px_phys(ds.lw, ds.scale) != reqw || px_phys(ds.lh, ds.scale) != reqh) {
        out("ORACLE resize grow metrics requested=%dx%d actual=%dx%d FAIL\n",
            reqw, reqh, px_phys(ds.lw, ds.scale), px_phys(ds.lh, ds.scale));
        mismatch = 1;
    }
    t_present = now_us();
    if (!draw_surface_frame(&ds, &scene, 0, NULL)) mismatch = 1;
    t_present = now_us() - t_present;
    t_capture = now_us();
    if (!draw_surface_capture(&ds, &grown)) mismatch = 1;
    t_capture = now_us() - t_capture;
    if (!mismatch) {
        if (!draw_surface_size_ok(&ds, &grown)) mismatch = 1;
        bg_rgb(ds.lw, ds.lh, ds.lw / 4, ds.lh / 4, &r, &g, &b);
        if (!check_px("resize", "grow-tl", &grown, px_phys(ds.lw / 4, ds.scale),
                      px_phys(ds.lh / 4, ds.scale), r, g, b, 0))
            mismatch = 1;
        capture_dump("resize_grow", &grown);
        report_timing("resize_grow", &scene, t_present, t_capture);
    }
    if (grown.pixels) capture_free(&grown);

    /* Shrink, with the same real-size verification. */
    reqw = px_phys(WIN_W - 60, ds.scale);
    reqh = px_phys(WIN_H - 40, ds.scale);
    surface_set_client_size(ds.surface, reqw, reqh);
    if (!surface_wait_physical_size(ds.surface, reqw, reqh)) {
        surface_client_size(ds.surface, &aw, &ah);
        out("ORACLE resize shrink client-size requested=%dx%d actual=%dx%d FAIL\n",
            reqw, reqh, aw, ah);
        mismatch = 1;
    }
    surface_sync_metrics(&ds);
    if (px_phys(ds.lw, ds.scale) != reqw || px_phys(ds.lh, ds.scale) != reqh) {
        out("ORACLE resize shrink metrics requested=%dx%d actual=%dx%d FAIL\n",
            reqw, reqh, px_phys(ds.lw, ds.scale), px_phys(ds.lh, ds.scale));
        mismatch = 1;
    }
    t_present = now_us();
    if (!draw_surface_frame(&ds, &scene, 0, NULL)) mismatch = 1;
    t_present = now_us() - t_present;
    t_capture = now_us();
    if (!draw_surface_capture(&ds, &shrunk)) mismatch = 1;
    t_capture = now_us() - t_capture;
    if (!mismatch) {
        if (!draw_surface_size_ok(&ds, &shrunk)) mismatch = 1;
        bg_rgb(ds.lw, ds.lh, ds.lw / 4, ds.lh / 4, &r, &g, &b);
        if (!check_px("resize", "shrink-tl", &shrunk, px_phys(ds.lw / 4, ds.scale),
                      px_phys(ds.lh / 4, ds.scale), r, g, b, 0))
            mismatch = 1;
        capture_dump("resize_shrink", &shrunk);
        report_timing("resize_shrink", &scene, t_present, t_capture);
    }
    if (shrunk.pixels) capture_free(&shrunk);

    ok = !mismatch;
    draw_surface_destroy(&ds);
    case_result("resize", ok ? 0 : 1, ok ? "grow-shrink-exact" : "resize-mismatch");
    return ok ? 0 : 1;
}

static int case_minimize_restore(double wanted_scale)
{
    DrawSurface ds;
    Scene scene = {0};
    Capture before, after;
    int ok = 1, setup_failed = 0, oracle_failed = 0;
    gint64 t_present, t_capture, deadline;

    memset(&after, 0, sizeof(after));
    if (!draw_surface_create(&ds, WIN_W, WIN_H)) {
        case_result("minimize_restore", 1, "surface-create-failed");
        return 1;
    }
    if (!scale_matches(ds.scale, wanted_scale)) {
        draw_surface_destroy(&ds);
        case_result("minimize_restore", 2, "requested-scale-unavailable");
        return 2;
    }
    if (!draw_surface_frame(&ds, &scene, 0, NULL) || !draw_surface_capture(&ds, &before)) {
        draw_surface_destroy(&ds);
        case_result("minimize_restore", 1, "initial-capture-failed");
        return 1;
    }

    DWORD retained = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    DWORD hidden;
    gdk_toplevel_minimize(GDK_TOPLEVEL(ds.surface));
    deadline = now_us() + 3 * G_TIME_SPAN_SECOND;
    while (!(gdk_toplevel_get_state(GDK_TOPLEVEL(ds.surface)) & GDK_TOPLEVEL_STATE_MINIMIZED) &&
           now_us() < deadline)
        pump_events();

    if (!(gdk_toplevel_get_state(GDK_TOPLEVEL(ds.surface)) & GDK_TOPLEVEL_STATE_MINIMIZED)) setup_failed = 1;
    hidden = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    out("MINIMIZE retained=%lu hidden=%lu\n", (unsigned long)retained, (unsigned long)hidden);
    if (!setup_failed && hidden >= retained) oracle_failed = 1;
    {
        GdkToplevelLayout *layout = gdk_toplevel_layout_new();
        gdk_toplevel_present(GDK_TOPLEVEL(ds.surface), layout);
        gdk_toplevel_layout_unref(layout);
    }
    deadline = now_us() + 3 * G_TIME_SPAN_SECOND;
    while ((!gdk_surface_get_mapped(ds.surface) ||
            (gdk_toplevel_get_state(GDK_TOPLEVEL(ds.surface)) & GDK_TOPLEVEL_STATE_MINIMIZED)) &&
           now_us() < deadline) pump_events();
    if (!gdk_surface_get_mapped(ds.surface) ||
        (gdk_toplevel_get_state(GDK_TOPLEVEL(ds.surface)) & GDK_TOPLEVEL_STATE_MINIMIZED)) setup_failed = 1;
    t_present = now_us();
    if (!seed_client(&ds) || !draw_surface_frame(&ds, &scene, 1,
        &(cairo_rectangle_int_t){8, 8, 16, 16})) setup_failed = 1;
    t_present = now_us() - t_present;
    t_capture = now_us();
    if (!draw_surface_capture(&ds, &after)) setup_failed = 1;
    t_capture = now_us() - t_capture;

    if (!setup_failed) {
        if (!captures_rgb_equal(&before, &after) || !whole_grid("minimize_restore", &ds, &after)) {
            out("ORACLE minimize_restore byte-identical FAIL\n");
            oracle_failed = 1;
        } else {
            out("ORACLE minimize_restore byte-identical PASS\n");
        }
        capture_dump("minimize_restore", &after);
        report_timing("minimize_restore", &scene, t_present, t_capture);
    }
    if (after.pixels) capture_free(&after);
    capture_free(&before);
    draw_surface_destroy(&ds);
    ok = !setup_failed && !oracle_failed;
    case_result("minimize_restore", ok ? 0 : 1,
                setup_failed ? "restore-setup-failed" : oracle_failed ? "proof-oracle-mismatch" : "restored-identical");
    return ok ? 0 : 1;
}

static int case_popup(double wanted_scale)
{
    DrawSurface parent, popup;
    Scene scene = {0};
    GdkPopupLayout *layout;
    GdkRectangle anchor;
    Capture cap;
    int ok = 1, mismatch = 0;
    gint64 deadline, t_present, t_capture;

    if (!draw_surface_create(&parent, WIN_W, WIN_H)) {
        case_result("popup", 1, "parent-create-failed");
        return 1;
    }
    if (!scale_matches(parent.scale, wanted_scale)) {
        draw_surface_destroy(&parent);
        case_result("popup", 2, "requested-scale-unavailable");
        return 2;
    }
    memset(&popup, 0, sizeof(popup));
    popup.surface = gdk_surface_new_popup(parent.surface, TRUE);
    if (!popup.surface) {
        draw_surface_destroy(&parent);
        case_result("popup", 2, "popup-surface-unavailable");
        return 2;
    }
    anchor.x = 10; anchor.y = 10; anchor.width = 10; anchor.height = 10;
    layout = gdk_popup_layout_new(&anchor, GDK_GRAVITY_SOUTH_WEST, GDK_GRAVITY_NORTH_WEST);
    if (!gdk_popup_present(GDK_POPUP(popup.surface), 80, 60, layout)) {
        gdk_popup_layout_unref(layout);
        gdk_surface_destroy(popup.surface);
        draw_surface_destroy(&parent);
        case_result("popup", 2, "popup-present-refused");
        return 2;
    }
    gdk_popup_layout_unref(layout);
    deadline = now_us() + 3 * G_TIME_SPAN_SECOND;
    while (!gdk_surface_get_mapped(popup.surface) && now_us() < deadline)
        pump_events();
    popup.scale = surface_scale(popup.surface);
    /* The popup is sized asynchronously (present -> layout -> move_resize ->
     * request_layout), so its metrics can still be the unallocated 1x1 clip.
     * Require the real Win32 client rect AND the public GDK metrics to reach
     * the requested 80x60; the old 1x1 size is never treated as ready. */
    if (!surface_wait_physical_size(popup.surface, px_phys(80, popup.scale),
                                    px_phys(60, popup.scale))) {
        int aw = -1, ah = -1;
        surface_client_size(popup.surface, &aw, &ah);
        out("ORACLE popup allocation requested=%dx%d actual=%dx%d FAIL\n",
            px_phys(80, popup.scale), px_phys(60, popup.scale), aw, ah);
        gdk_surface_destroy(popup.surface);
        draw_surface_destroy(&parent);
        case_result("popup", 1, "popup-allocation-timeout");
        return 1;
    }
    surface_sync_metrics(&popup);
    popup.ctx = gdk_surface_create_cairo_context(popup.surface);
    if (!popup.ctx) {
        gdk_surface_destroy(popup.surface);
        draw_surface_destroy(&parent);
        case_result("popup", 1, "popup-context-null");
        return 1;
    }

    t_present = now_us();
    if (!draw_surface_frame(&popup, &scene, 0, NULL)) mismatch = 1;
    t_present = now_us() - t_present;
    t_capture = now_us();
    if (!draw_surface_capture(&popup, &cap)) {
        out("ORACLE popup hwnd-capture skipped\n");
        g_object_unref(popup.ctx);
        gdk_surface_destroy(popup.surface);
        draw_surface_destroy(&parent);
        case_result("popup", 2, "popup-hwnd-capture-unavailable");
        return 2;
    }
    t_capture = now_us() - t_capture;
    if (!mismatch) {
        if (!check_px("popup", "tl-red", &cap, cap.width / 4, cap.height / 4, 255, 0, 0, 0))
            mismatch = 1;
        if (!check_px("popup", "br-white", &cap, 3 * cap.width / 4, 3 * cap.height / 4,
                      255, 255, 255, 0))
            mismatch = 1;
        capture_dump("popup", &cap);
        report_timing("popup", &scene, t_present, t_capture);
    }
    capture_free(&cap);
    g_object_unref(popup.ctx);
    gdk_surface_destroy(popup.surface);
    draw_surface_destroy(&parent);
    ok = !mismatch;
    case_result("popup", ok ? 0 : 1, ok ? "popup-exact" : "pixel-mismatch");
    return ok ? 0 : 1;
}

static int case_dialog(double wanted_scale)
{
    DrawSurface parent, child;
    Scene scene = {0};
    Capture pcap, dcap;
    int ok = 1, mismatch = 0, r, g, b;

    memset(&pcap, 0, sizeof(pcap));
    memset(&dcap, 0, sizeof(dcap));
    if (!draw_surface_create(&parent, WIN_W, WIN_H)) {
        case_result("dialog", 1, "parent-create-failed");
        return 1;
    }
    if (!scale_matches(parent.scale, wanted_scale)) {
        draw_surface_destroy(&parent);
        case_result("dialog", 2, "requested-scale-unavailable");
        return 2;
    }
    if (!draw_surface_frame(&parent, &scene, 0, NULL) || !draw_surface_capture(&parent, &pcap)) {
        draw_surface_destroy(&parent);
        case_result("dialog", 1, "parent-capture-failed");
        return 1;
    }

    if (!draw_surface_create(&child, 120, 90)) {
        capture_free(&pcap);
        draw_surface_destroy(&parent);
        case_result("dialog", 1, "dialog-create-failed");
        return 1;
    }
    gdk_toplevel_set_transient_for(GDK_TOPLEVEL(child.surface), parent.surface);
    if (!draw_surface_frame(&child, &scene, 0, NULL) || !draw_surface_capture(&child, &dcap)) {
        capture_free(&pcap);
        draw_surface_destroy(&child);
        draw_surface_destroy(&parent);
        case_result("dialog", 1, "dialog-capture-failed");
        return 1;
    }
    if (!check_px("dialog", "dlg-tl", &dcap, dcap.width / 4, dcap.height / 4, 255, 0, 0, 0))
        mismatch = 1;
    bg_rgb(parent.lw, parent.lh, parent.lw / 4, parent.lh / 4, &r, &g, &b);
    if (!check_px("dialog", "parent-tl", &pcap, pcap.width / 4, pcap.height / 4, r, g, b, 0))
        mismatch = 1;
    capture_dump("dialog", &dcap);

    draw_surface_destroy(&child);
    if (!draw_surface_frame(&parent, &scene, 0, NULL) || !draw_surface_capture(&parent, &pcap)) {
        mismatch = 1;
    } else {
        bg_rgb(parent.lw, parent.lh, parent.lw / 4, parent.lh / 4, &r, &g, &b);
        if (!check_px("dialog", "parent-after", &pcap, pcap.width / 4,
                      pcap.height / 4, r, g, b, 0))
            mismatch = 1;
        capture_dump("dialog_parent_after", &pcap);
    }
    if (dcap.pixels) capture_free(&dcap);
    if (pcap.pixels) capture_free(&pcap);
    draw_surface_destroy(&parent);
    ok = !mismatch;
    case_result("dialog", ok ? 0 : 1, ok ? "dialog-and-parent" : "pixel-mismatch");
    return ok ? 0 : 1;
}

/* -------------------------------- GTK renderer case (full app-level path) */

static void gtk_scene_draw(GtkDrawingArea *area, cairo_t *cr, int width, int height,
                           gpointer data)
{
    (void)area;
    draw_scene(cr, (Scene *)data, width, height);
}

static gboolean gtk_after_paint(GdkFrameClock *clock, gpointer data)
{
    GtkScene *g = data;
    (void)clock;
    g->painted = TRUE;
    g->t_painted = now_us();
    return G_SOURCE_CONTINUE;
}

static int case_gtk_renderer(double wanted_scale)
{
    GtkScene g;
    Capture cap;
    GdkFrameClock *clock;
    GskRenderer *renderer;
    gint64 t_present, t_capture, deadline;
    int ok = 1, mismatch = 0, lw, lh;
    double scale;

    memset(&g, 0, sizeof(g));
    g.win = gtk_window_new();
    /* Undecorated before realize: GTK's client-side titlebar/shadow would add
     * 14 px per side to the HWND client (observed 228x228 for 200x160 content).
     * gtk_window_should_use_csd() then returns FALSE, gtk_window_update_csd_size()
     * adds no shadow and gdk_toplevel_set_decorated(FALSE) clears the Win32
     * decoration bits, so the client rect matches the O10 200x160 oracle. */
    gtk_window_set_decorated(GTK_WINDOW(g.win), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(g.win), WIN_W, WIN_H);
    g.da = gtk_drawing_area_new();
    gtk_widget_set_size_request(g.da, WIN_W, WIN_H);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(g.da), gtk_scene_draw, &g.scene, NULL);
    gtk_window_set_child(GTK_WINDOW(g.win), g.da);
    gtk_window_present(GTK_WINDOW(g.win));
    deadline = now_us() + 5 * G_TIME_SPAN_SECOND;
    while (!gtk_widget_get_mapped(g.win) && now_us() < deadline)
        pump_events();
    g.surface = gtk_native_get_surface(gtk_widget_get_native(g.win));
    if (!g.surface) {
        gtk_window_destroy(GTK_WINDOW(g.win));
        case_result("gtk_renderer", 1, "no-surface");
        return 1;
    }
    scale = surface_scale(g.surface);
    if (!scale_matches(scale, wanted_scale)) {
        gtk_window_destroy(GTK_WINDOW(g.win));
        case_result("gtk_renderer", 2, "requested-scale-unavailable");
        return 2;
    }
    /* Stable allocation before the frame is queued: surface and drawing area
     * must both be the requested 200x160 logical size. */
    if (!gtk_wait_allocated(&g, WIN_W, WIN_H)) {
        gtk_window_destroy(GTK_WINDOW(g.win));
        case_result("gtk_renderer", 1, "allocation-timeout");
        return 1;
    }
    /* The GTK-level entry point must really be the Cairo renderer (the GTK4
     * equivalent of the old GDK-level check); otherwise the case is invalid. */
    renderer = gtk_native_get_renderer(gtk_widget_get_native(g.win));
    if (!renderer || !GSK_IS_CAIRO_RENDERER(renderer)) {
        out("ORACLE gtk_renderer renderer=%s expected=GskCairoRenderer FAIL\n",
            renderer ? G_OBJECT_TYPE_NAME(renderer) : "(none)");
        mismatch = 1;
    }
    clock = gdk_surface_get_frame_clock(g.surface);
    g_signal_connect(clock, "after-paint", G_CALLBACK(gtk_after_paint), &g);

    g.painted = FALSE;
    g.t_painted = 0;
    g.t_submit = now_us();
    gtk_widget_queue_draw(g.da);
    deadline = now_us() + 5 * G_TIME_SPAN_SECOND;
    while (!g.painted && now_us() < deadline)
        pump_events();
    if (!g.painted) {
        gtk_window_destroy(GTK_WINDOW(g.win));
        case_result("gtk_renderer", 1, "frame-timeout");
        return 1;
    }
    /* Elapsed monotonic submission->after-paint latency, NOT a display rate.
     * The GDI capture below is timed separately and excluded from present_us. */
    t_present = g.t_painted - g.t_submit;
    t_capture = now_us();
    if (!capture_hwnd(GDK_SURFACE_HWND(g.surface), &cap)) {
        gtk_window_destroy(GTK_WINDOW(g.win));
        case_result("gtk_renderer", 1, "capture-failed");
        return 1;
    }
    t_capture = now_us() - t_capture;
    /* Never assume the window kept the requested size; compare the whole
     * captured client bounds explicitly. */
    if (cap.width != px_phys(WIN_W, scale) || cap.height != px_phys(WIN_H, scale)) {
        out("ORACLE gtk_renderer size expected=%dx%d actual=%dx%d FAIL\n",
            px_phys(WIN_W, scale), px_phys(WIN_H, scale), cap.width, cap.height);
        mismatch = 1;
    }
    lw = WIN_W;
    lh = WIN_H;
    if (!full_quadrant_checks("gtk_renderer", &cap, lw, lh, scale, &mismatch))
        mismatch = 1;
    capture_dump("gtk_renderer", &cap);
    report_timing("gtk_renderer", &g.scene, t_present, t_capture);
    capture_free(&cap);
    gtk_window_destroy(GTK_WINDOW(g.win));
    while (g_main_context_pending(NULL))
        g_main_context_iteration(NULL, FALSE);
    ok = !mismatch;
    case_result("gtk_renderer", ok ? 0 : 1, ok ? "gtk-path-exact" : "pixel-mismatch");
    return ok ? 0 : 1;
}

/* O11: the same real GSK entry point/readback as gtk_renderer, but with a rich
 * deterministic artwork scene. The strict baseline/candidate byte identity is
 * enforced by the driver over the normalized .bgra dumps; this case enforces
 * the nonempty/nonuniform content gate, the fixed layout/font report and the
 * required normalized PNG. */
static int case_visual_artwork(double wanted_scale)
{
    GtkScene g;
    Capture cap;
    GdkFrameClock *clock;
    GskRenderer *renderer;
    gint64 t_present, t_capture, deadline;
    int ok = 1, mismatch = 0;
    double scale;

    memset(&g, 0, sizeof(g));
    g.scene.artwork = 1;
    g.win = gtk_window_new();
    /* Same undecorated-before-realize contract as gtk_renderer: the HWND
     * client rect must be the 200x160 content, not a 228x228 CSD frame. */
    gtk_window_set_decorated(GTK_WINDOW(g.win), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(g.win), WIN_W, WIN_H);
    g.da = gtk_drawing_area_new();
    gtk_widget_set_size_request(g.da, WIN_W, WIN_H);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(g.da), gtk_scene_draw,
                                   &g.scene, NULL);
    gtk_window_set_child(GTK_WINDOW(g.win), g.da);
    gtk_window_present(GTK_WINDOW(g.win));
    deadline = now_us() + 5 * G_TIME_SPAN_SECOND;
    while (!gtk_widget_get_mapped(g.win) && now_us() < deadline)
        pump_events();
    g.surface = gtk_native_get_surface(gtk_widget_get_native(g.win));
    if (!g.surface) {
        gtk_window_destroy(GTK_WINDOW(g.win));
        case_result("visual_artwork", 1, "no-surface");
        return 1;
    }
    scale = surface_scale(g.surface);
    if (!scale_matches(scale, wanted_scale)) {
        gtk_window_destroy(GTK_WINDOW(g.win));
        case_result("visual_artwork", 2, "requested-scale-unavailable");
        return 2;
    }
    /* Stable 200x160 allocation (surface + drawing area) before queuing. */
    if (!gtk_wait_allocated(&g, WIN_W, WIN_H)) {
        gtk_window_destroy(GTK_WINDOW(g.win));
        case_result("visual_artwork", 1, "allocation-timeout");
        return 1;
    }
    renderer = gtk_native_get_renderer(gtk_widget_get_native(g.win));
    if (!renderer || !GSK_IS_CAIRO_RENDERER(renderer)) {
        out("ORACLE visual_artwork renderer=%s expected=GskCairoRenderer FAIL\n",
            renderer ? G_OBJECT_TYPE_NAME(renderer) : "(none)");
        mismatch = 1;
    }
    clock = gdk_surface_get_frame_clock(g.surface);
    g_signal_connect(clock, "after-paint", G_CALLBACK(gtk_after_paint), &g);

    g.painted = FALSE;
    g.t_painted = 0;
    g.t_submit = now_us();
    gtk_widget_queue_draw(g.da);
    deadline = now_us() + 5 * G_TIME_SPAN_SECOND;
    while (!g.painted && now_us() < deadline)
        pump_events();
    if (!g.painted) {
        gtk_window_destroy(GTK_WINDOW(g.win));
        case_result("visual_artwork", 1, "frame-timeout");
        return 1;
    }
    /* Elapsed monotonic submission->after-paint latency, NOT a display rate;
     * the GDI capture (and PNG write) below are timed separately/excluded. */
    t_present = g.t_painted - g.t_submit;
    t_capture = now_us();
    if (!capture_hwnd(GDK_SURFACE_HWND(g.surface), &cap)) {
        gtk_window_destroy(GTK_WINDOW(g.win));
        case_result("visual_artwork", 1, "capture-failed");
        return 1;
    }
    t_capture = now_us() - t_capture;
    /* Whole captured client bounds, checked explicitly against 200x160. */
    if (cap.width != px_phys(WIN_W, scale) || cap.height != px_phys(WIN_H, scale)) {
        out("ORACLE visual_artwork size expected=%dx%d actual=%dx%d FAIL\n",
            px_phys(WIN_W, scale), px_phys(WIN_H, scale), cap.width, cap.height);
        mismatch = 1;
    }
    out("LAYOUT visual_artwork width=%d height=%d scale=%.3f "
        "text=VA_Studio_0123 font=%s\n",
        cap.width, cap.height, scale,
        g.scene.font_report[0] ? g.scene.font_report : "unknown");
    if (!artwork_content_check("visual_artwork", &cap))
        mismatch = 1;
    capture_dump("visual_artwork", &cap);
    if (!capture_dump_png("visual_artwork", &cap))
        mismatch = 1;
    report_timing("visual_artwork", &g.scene, t_present, t_capture);
    capture_free(&cap);
    gtk_window_destroy(GTK_WINDOW(g.win));
    while (g_main_context_pending(NULL))
        g_main_context_iteration(NULL, FALSE);
    ok = !mismatch;
    case_result("visual_artwork", ok ? 0 : 1,
                ok ? "artwork-content" : "pixel-mismatch");
    return ok ? 0 : 1;
}

static int case_retention(double wanted_scale)
{
    DrawSurface ds; Scene scene = {0}; Capture cap = {0};
    cairo_rectangle_int_t r = { 8, 8, 16, 16 };
    DWORD a0, a, b, c; int ok;
    if (!draw_surface_create(&ds, WIN_W, WIN_H)) {
        case_result("retention", 1, "surface-create-failed"); return 1;
    }
    if (!scale_matches(ds.scale, wanted_scale)) {
        draw_surface_destroy(&ds); case_result("retention", 2, "scale-unavailable"); return 2;
    }
    if (!surface_wait_physical_size(ds.surface, px_phys(WIN_W, ds.scale), px_phys(WIN_H, ds.scale)) ||
        !surface_sync_metrics(&ds)) {
        draw_surface_destroy(&ds); case_result("retention", 2, "client-size-unavailable"); return 2;
    }
    a0 = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    /* Warm up lazy GDI resources with a full frame, one partial frame, and one
     * seeded partial frame: GTK 4.22 allocates one object on the first partial
     * frame and one on the first partial frame after a client seed, in both
     * buffer modes. Reuse is then judged across a further seeded partial frame. */
    ok = draw_surface_frame(&ds, &scene, 0, NULL) && draw_surface_frame(&ds, &scene, 1, &r) &&
         seed_client(&ds) && draw_surface_frame(&ds, &scene, 1, &r);
    a = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    if (ok) ok = seed_client(&ds);
    if (ok) ok = draw_surface_frame(&ds, &scene, 1, &r);
    b = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    if (ok) {
        /* Plateau evidence: the count must not grow with further partial frames. */
        DWORD plateau[3]; int i;
        for (i = 0; i < 3 && ok; ++i) {
            ok = draw_surface_frame(&ds, &scene, 1, &r);
            plateau[i] = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
        }
        if (ok) {
            out("RETENTION-PLATEAU %lu %lu %lu\n", (unsigned long)plateau[0], (unsigned long)plateau[1],
                (unsigned long)plateau[2]);
            if (g_expect_retained && (plateau[0] != b || plateau[1] != b || plateau[2] != b))
                ok = 0;
        }
        if (ok) ok = seed_client(&ds) && draw_surface_frame(&ds, &scene, 1, &r);
    }
    if (ok && g_expect_retained) {
        ok = b == a && draw_surface_capture(&ds, &cap) &&
             check_px("retention", "outside-damage", &cap,
                      px_phys(100, ds.scale), px_phys(100, ds.scale), 19, 23, 29, 0);
    }
    if (cap.pixels) capture_free(&cap);
    draw_surface_destroy(&ds);
    c = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    out("RETENTION mode=%s initial=%lu before=%lu drawn=%lu destroyed=%lu\n",
        getenv("GDK_WIN32_CAIRO_GDI_BUFFER") ? "off" : "on",
        (unsigned long)a0, (unsigned long)a, (unsigned long)b, (unsigned long)c);
    case_result("retention", ok ? 0 : 1, ok ? "stable-reuse" : "reuse-or-pixel-failed");
    return ok ? 0 : 1;
}

static int case_first_partial(double wanted_scale, int odd)
{
    DrawSurface ds; Scene scene = {0}; Capture cap = {0};
    cairo_rectangle_int_t r = { 8, 8, 16, 16 };
    int steps = odd ? 2 : 3, i, ok = 1, oracle_failed = 0, cw = -1, ch = -1;
    const int w[] = { 201, 203, 0 }, h[] = { 161, 163, 0 };
    const char *name = odd ? "odd_client" : "first_partial";
    if (!draw_surface_create(&ds, WIN_W, WIN_H)) {
        case_result(name, 1, "surface-create-failed"); return 1;
    }
    if (!scale_matches(ds.scale, wanted_scale) || (odd && ds.scale != 2.0)) {
        draw_surface_destroy(&ds); case_result(name, 2, "scale-2-unavailable"); return 2;
    }
    for (i = 0; i < steps; i++) {
        if (odd || i) {
            int pw = odd ? w[i] : px_phys(i == 1 ? WIN_W + 80 : WIN_W - 60, ds.scale);
            int ph = odd ? h[i] : px_phys(i == 1 ? WIN_H + 60 : WIN_H - 40, ds.scale);
            surface_set_client_size(ds.surface, pw, ph);
            if (odd) {
                gint64 deadline = now_us() + 5 * G_TIME_SPAN_SECOND;
                do { if (surface_client_size(ds.surface, &cw, &ch) && cw == pw && ch == ph &&
                         gdk_surface_get_width(ds.surface) == (pw + 1) / 2 && gdk_surface_get_height(ds.surface) == (ph + 1) / 2) break;
                     pump_events(); } while (now_us() < deadline);
                if (cw != pw || ch != ph) {
                    draw_surface_destroy(&ds); case_result(name, 2, "odd-client-unavailable"); return 2;
                }
            } else if (!surface_wait_physical_size(ds.surface, pw, ph)) { ok = 0; break; }
            if (!surface_sync_metrics(&ds)) { ok = 0; break; }
        }
        if (!surface_client_size(ds.surface, &cw, &ch) ||
            (odd && (px_phys(ds.lw, ds.scale) < cw || px_phys(ds.lw, ds.scale) > cw + 1 ||
                     px_phys(ds.lh, ds.scale) < ch || px_phys(ds.lh, ds.scale) > ch + 1))) { ok = 0; break; }
        if (!seed_client(&ds) || !draw_surface_frame(&ds, &scene, 1, &r) ||
            !draw_surface_capture(&ds, &cap)) { ok = 0; break; }
        if (odd) out("ODD_GDI step=%d retained=%lu\n", i,
                     (unsigned long)GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS));
        out("METRICS %s step=%d client=%dx%d logical=%dx%d scale=%.3f bound=%dx%d\n",
            name, i, cw, ch, ds.lw, ds.lh, ds.scale,
            px_phys(ds.lw, ds.scale), px_phys(ds.lh, ds.scale));
        if (!whole_grid(name, &ds, &cap)) { oracle_failed = 1; ok = 0; }
        capture_dump(odd ? (i ? "odd_203" : "odd_201") :
                     (i == 0 ? "first_create" : i == 1 ? "first_grow" : "first_shrink"), &cap);
        capture_free(&cap);
        if (!ok) break;
    }
    if (cap.pixels) capture_free(&cap);
    draw_surface_destroy(&ds);
    case_result(name, ok ? 0 : 1,
                ok ? "whole-client-exact" : oracle_failed ? "proof-oracle-mismatch" : "frame-setup-failed");
    return ok ? 0 : 1;
}

typedef struct {
    DrawSurface *ds; Scene *scene; int count, failed;
    gint64 at[8]; double clip[8][4];
} FaultRender;

static gboolean fault_render(GdkSurface *surface, cairo_region_t *region, gpointer data)
{
    FaultRender *f = data;
    g_in_render = 1;
    if (cairo_region_num_rectangles(region) == 1) {
        cairo_rectangle_int_t r;
        cairo_region_get_rectangle(region, 0, &r);
        f->failed |= !draw_surface_frame(f->ds, f->scene, 1, &r);
    } else f->failed = 1;
    g_in_render = 0;
    if (f->count < 8) {
        f->at[f->count] = now_us();
        memcpy(f->clip[f->count], f->scene->clip, sizeof(f->scene->clip));
    }
    f->count++;
    out("TIMING retry frame=%d at_us=%" G_GINT64_FORMAT " appclip=%.0f,%.0f,%.0f,%.0f\n",
        f->count, now_us(), f->scene->clip[0], f->scene->clip[1],
        f->scene->clip[2], f->scene->clip[3]);
    return TRUE;
}

static int fault_clip_ok(const double *c, cairo_rectangle_int_t r, int lw, int lh, double scale)
{
    double slack = 2.0 / scale + 1.0;
    return c[0] <= r.x && c[1] <= r.y &&
           c[0] + c[2] >= r.x + r.width && c[1] + c[3] >= r.y + r.height &&
           c[0] >= r.x - slack && c[1] >= r.y - slack &&
           c[0] + c[2] <= r.x + r.width + slack &&
           c[1] + c[3] <= r.y + r.height + slack &&
           c[2] < lw && c[3] < lh;
}

static int case_fault(const char *name, double wanted_scale)
{
    DrawSurface ds = {0}; Scene scene = {0}; Capture cap = {0}, before = {0};
    cairo_rectangle_int_t r = {20, 20, 40, 30};
    FaultRender f = {0}; DWORD a, b, c; gint64 start, deadline;
    int ok = 1, skip = 0, partial = strcmp(name, "partial_transfer") == 0;
    int backoff = strcmp(name, "retry_backoff") == 0;
    int alloc = strcmp(name, "alloc_latch") == 0;
    if (!draw_surface_create(&ds, WIN_W, WIN_H)) { ok = 0; goto done; }
    if (!scale_matches(ds.scale, wanted_scale) ||
        !surface_wait_physical_size(ds.surface, px_phys(WIN_W, ds.scale),
                                    px_phys(WIN_H, ds.scale)) ||
        !surface_sync_metrics(&ds)) { skip = 1; goto done; }
    if (!alloc && strcmp(name, "first_transfer") != 0) {
        if (!draw_surface_frame(&ds, &scene, 0, NULL) ||
            !draw_surface_capture(&ds, &before) ||
            !whole_grid(name, &ds, &before)) { ok = 0; goto done; }
    } else if (!seed_client(&ds)) { ok = 0; goto done; }
    if (alloc) {
        int cw, ch;
        if (!surface_client_size(ds.surface, &cw, &ch)) { ok = 0; goto done; }
        /* Match the patched path's physical buffer bound using live metrics. */
        int align = (int)ceil(ds.scale);
        int bw = MIN((int)ceil(ds.lw * ds.scale), ((cw + align - 1) / align) * align);
        int bh = MIN((int)ceil(ds.lh * ds.scale), ((ch + align - 1) / align) * align);
        out("FAULT_SIZE width=%d height=%d client=%dx%d\n", bw, bh, cw, ch);
    }
    a = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    if (!alloc && strcmp(name, "first_transfer") != 0) {
        scene.overlay[0] = (Rect){20,20,40,30,0,1,1,1};
        scene.n_overlay = 1;
    }
    start = now_us();
    if (!draw_surface_frame(&ds, &scene, 1, &r) ||
        !draw_surface_capture(&ds, &cap)) { ok = 0; goto done; }
    b = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS); out("TIMING %s failed_at_us=%" G_GINT64_FORMAT " appclip=%.0f,%.0f,%.0f,%.0f\n", name, start, scene.clip[0],scene.clip[1],scene.clip[2],scene.clip[3]);
    if (alloc) {
        int cw, ch;
        ok &= b == a && check_px(name, "fallback-outside", &cap, px_phys(100,ds.scale),
                                   px_phys(100,ds.scale), 19,23,29,0);
        ok &= check_px(name, "fallback-inside", &cap, px_phys(40,ds.scale),
                       px_phys(35,ds.scale), 255,0,0,0);
        capture_free(&cap);
        ok &= draw_surface_frame(&ds, &scene, 1, &r);
        ok &= GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) == b;
        surface_set_client_size(ds.surface, px_phys(WIN_W + 80, ds.scale),
                                px_phys(WIN_H + 60, ds.scale));
        if (!surface_wait_physical_size(ds.surface, px_phys(WIN_W + 80, ds.scale),
                                        px_phys(WIN_H + 60, ds.scale)) ||
            !surface_sync_metrics(&ds)) { skip = 1; goto done; }
        ok &= surface_client_size(ds.surface, &cw, &ch);
        out("FAULT_ALLOC size=%dx%d retained=%lu,%lu\n", cw, ch,
            (unsigned long)a, (unsigned long)b);
        ok &= draw_surface_frame(&ds, &scene, 1, &r) && draw_surface_capture(&ds, &cap);
        c = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
        ok &= c > b && whole_grid(name, &ds, &cap);
    } else if (!partial && !backoff) {
        ok &= check_px(name, "failed-transfer", &cap, px_phys(40,ds.scale),
                       px_phys(35,ds.scale), 19,23,29,0);
        capture_free(&cap);
        ok &= draw_surface_frame(&ds, &scene, 1, &r) && draw_surface_capture(&ds, &cap);
        ok &= whole_grid(name, &ds, &cap);
    } else {
        f.ds = &ds; f.scene = &scene;
        ok &= captures_rgb_equal_outside(&before, &cap, 0, 0, 0, 0);
        ok &= check_px(name, "failed-transfer", &cap, px_phys(40,ds.scale),
                       px_phys(35,ds.scale), 255,0,0,0);
        g_signal_connect(ds.surface, "render", G_CALLBACK(fault_render), &f);
        deadline = now_us() + (backoff ? 10000 : 2000) * G_TIME_SPAN_MILLISECOND;
        while (f.count < (backoff ? 5 : 1) && now_us() < deadline) pump_events();
        if (f.count < (backoff ? 5 : 1)) { skip = 1; goto done; }
        for (int i = 0; i < f.count && i < 8; i++)
            ok &= fault_clip_ok(f.clip[i], r, ds.lw, ds.lh, ds.scale);
        ok &= !f.failed;
        if (backoff) {
            const int minimum[] = {210,460,960,1960};
            ok &= f.at[0] - start >= minimum[0] * G_TIME_SPAN_MILLISECOND;
            for (int i = 1; i < 4; i++)
                ok &= f.at[i] - f.at[i-1] >= minimum[i] * G_TIME_SPAN_MILLISECOND;
        } else {
            /* First retry fails (bitblt-after:1:2); a natural frame repairs pending
             * before the next timer can render. */
            ok &= draw_surface_frame(&ds, &scene, 1, &r);
        }
        capture_free(&cap);
        ok &= draw_surface_capture(&ds, &cap);
        ok &= check_px(name, "recovered-inside", &cap, px_phys(40,ds.scale),
                       px_phys(35,ds.scale), 0,255,255,0);
        ok &= captures_rgb_equal_outside(&before, &cap, px_phys(r.x,ds.scale),
                                          px_phys(r.y,ds.scale), px_phys(r.width,ds.scale),
                                          px_phys(r.height,ds.scale));
        c = f.count;
        deadline = now_us() + (backoff ? 2000 : 700) * G_TIME_SPAN_MILLISECOND;
        while (now_us() < deadline) pump_events();
        ok &= f.count == (int)c;
    }
 done:
    if (ds.surface && (partial || backoff))
        g_signal_handlers_disconnect_by_func(ds.surface, G_CALLBACK(fault_render), &f);
    if (cap.pixels) capture_free(&cap);
    if (before.pixels) capture_free(&before);
    draw_surface_destroy(&ds);
    case_result(name, skip ? 2 : ok ? 0 : 1,
                skip ? "retry-or-client-unavailable" : ok ? "fault-oracles-exact" : "fault-oracle-failed");
    return skip ? 2 : ok ? 0 : 1;
}

static int case_lifecycle(void)
{
    int i, ok = 1, wait_failed = 0, mid = g_rounds / 2;
    DWORD g0, u0, gm = 0, um = 0, ge, ue;
    SIZE_T p0, pm = 0, pe;
    Scene scene = {0}; cairo_rectangle_int_t r = { 8, 8, 16, 16 };
    g0 = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    u0 = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS); p0 = private_bytes();
    for (i = 0; i < g_rounds; i++) {
        DrawSurface ds;
        if (!draw_surface_create(&ds, 80, 60)) { ok = 0; break; }
        if (!draw_surface_frame(&ds, &scene, 1, &r)) ok = 0;
        surface_set_client_size(ds.surface, px_phys(100, ds.scale), px_phys(80, ds.scale));
        if (!surface_wait_physical_size(ds.surface, px_phys(100, ds.scale), px_phys(80, ds.scale))) {
            wait_failed = 1; ok = 0;
        } else if (!surface_sync_metrics(&ds) || !draw_surface_frame(&ds, &scene, 1, &r)) ok = 0;
        draw_surface_destroy(&ds);
        out("LIFECYCLE round=%d gdi=%lu user=%lu private=%llu\n", i + 1,
            (unsigned long)GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS),
            (unsigned long)GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS),
            (unsigned long long)private_bytes());
        if (i + 1 == mid) {
            gm = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
            um = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS); pm = private_bytes();
        }
        if (!ok) break;
    }
    ge = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    ue = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS); pe = private_bytes();
    out("PLATEAU completed=%d midpoint=%d gdi=%lu,%lu,%lu user=%lu,%lu,%lu private=%llu,%llu,%llu\n",
        i, mid, (unsigned long)g0, (unsigned long)gm, (unsigned long)ge,
        (unsigned long)u0, (unsigned long)um, (unsigned long)ue,
        (unsigned long long)p0, (unsigned long long)pm, (unsigned long long)pe);
    if (i != g_rounds || !p0 || !pm || !pe ||
        ge > gm + 4 || ue > um + 4 || pe > pm + 2 * 1024 * 1024) ok = 0;
    case_result("lifecycle", wait_failed ? 2 : ok ? 0 : 1,
                wait_failed ? "resize-wait-incomplete" : ok ? "draw-resize-plateau" : "resource-growth");
    return wait_failed ? 2 : ok ? 0 : 1;
}

/* ---------------------------------------------------------------- benchmark */
/*
 * Numerical benchmark of one arm's real GDK Cairo-buffer presentation path.
 * Root metric: elapsed rendering + GDI completion per frame. The timer starts
 * immediately before draw_surface_frame() and stops after a successful
 * GdiFlush() on the same thread; it therefore includes Cairo scene
 * construction, gdk_draw_context_end_frame() and the queued GDI work, and
 * excludes capture/readback/PNG/output. A failed GdiFlush is a nonzero
 * invalid trial. This is NOT display/monitor FPS and NOT an app FPS claim.
 */

/* Nearest-rank percentile over an ascending-sorted array: rank = ceil(p*n),
 * clamped to [1,n]. Deliberately math.h-free so no new link dependency. */
static gint64 bench_percentile(const gint64 *sorted, int n, double p)
{
    int rank;

    if (n <= 0)
        return 0;
    rank = (int)(p * (double)n);
    if ((double)rank < p * (double)n)
        rank++;
    if (rank < 1)
        rank = 1;
    if (rank > n)
        rank = n;
    return sorted[rank - 1];
}

static int bench_cmp_i64(const void *a, const void *b)
{
    gint64 x = *(const gint64 *)a;
    gint64 y = *(const gint64 *)b;
    return (x > y) - (x < y);
}

/* Damage rect in logical surface coordinates. full = whole viewport;
 * partial = centred width/4 x height/4 rect, exactly 1/16 of the area. */
static void bench_damage_rect(const char *mode, int lw, int lh,
                              cairo_rectangle_int_t *r)
{
    if (g_strcmp0(mode, "partial") == 0) {
        int dw = lw / 4, dh = lh / 4;
        if (dw < 1) dw = 1;
        if (dh < 1) dh = 1;
        r->x = (lw - dw) / 2;
        r->y = (lh - dh) / 2;
        r->width = dw;
        r->height = dh;
    } else {
        r->x = 0;
        r->y = 0;
        r->width = lw;
        r->height = lh;
    }
}

/* partial damage overlay: opaque cyan on an even frame index, magenta on odd. */
static void bench_overlay(const char *mode, int index,
                          const cairo_rectangle_int_t *dmg, Scene *scene)
{
    Rect *o;

    scene->n_overlay = 0;
    if (g_strcmp0(mode, "partial") != 0)
        return;
    o = &scene->overlay[0];
    o->x = dmg->x;
    o->y = dmg->y;
    o->w = dmg->width;
    o->h = dmg->height;
    o->a = 1.0;
    if (index % 2 == 0) { o->r = 0.0; o->g = 1.0; o->b = 1.0; }
    else                { o->r = 1.0; o->g = 0.0; o->b = 1.0; }
    scene->n_overlay = 1;
}

/* The real Win32 window rect must sit inside the physical bounds of the
 * monitor nearest the window. MonitorFromWindow/GetMonitorInfoW return Win32
 * virtual-screen coordinates, which are physical pixels on Windows; this is
 * the physical rect the GDK logical geometry + integer scale comparison could
 * not represent on mixed-DPI layouts. Offscreen or larger-than-monitor
 * placements fail, so no 4K/offscreen claim is possible. */
static int bench_monitor_contains(GdkSurface *surface, RECT *mon_rect)
{
    HWND hwnd = GDK_SURFACE_HWND(surface);
    HMONITOR hmon;
    MONITORINFO mi;
    RECT wr;

    if (!hwnd || !GetWindowRect(hwnd, &wr))
        return 0;
    hmon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (!hmon)
        return 0;
    memset(&mi, 0, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hmon, &mi))
        return 0;
    if (wr.left >= mi.rcMonitor.left && wr.top >= mi.rcMonitor.top &&
        wr.right <= mi.rcMonitor.right && wr.bottom <= mi.rcMonitor.bottom) {
        if (mon_rect)
            *mon_rect = mi.rcMonitor;
        return 1;
    }
    return 0;
}

/* Strict full-string bounded integer parse for the benchmark CLI. Rejects
 * NULL/empty, no-leading-digit, trailing junk, overflow (ERANGE) and any value
 * outside [lo,hi]. The caller must reject; there is no silent clamp. */
static int bench_parse_bounded(const char *s, long long lo, long long hi,
                               long long *dst)
{
    char *end = NULL;
    long long v;

    if (s == NULL || s[0] == '\0')
        return 0;
    errno = 0;
    v = g_ascii_strtoll(s, &end, 10);
    if (end == s || end == NULL || *end != '\0')
        return 0;
    if (errno == ERANGE || v < lo || v > hi)
        return 0;
    *dst = v;
    return 1;
}

/* Labeled wrapper: on bad input print the exact input, label and allowed
 * range, then let main return the usage exit code before any GUI is created. */
static int bench_cli_int(const char *s, long long lo, long long hi,
                         int *dst, const char *label)
{
    long long v;

    if (!bench_parse_bounded(s, lo, hi, &v)) {
        out("HARNESS FAIL %s-invalid value='%s' allowed=%lld..%lld\n",
            label, s, lo, hi);
        return 0;
    }
    *dst = (int)v;
    return 1;
}

static int case_benchmark(int lw, int lh, const char *mode, int frames,
                          int warmup, const char *csv_override)
{
    DrawSurface ds;
    Scene scene;
    cairo_rectangle_int_t dmg;
    Capture base, final;
    gint64 *samples = NULL, *sorted = NULL;
    gint64 sum_us = 0, p50 = 0, p95 = 0, vmin = 0, vmax = 0;
    double scale, elapsed_s, mean_us, cyc_per_s, req_mpix_s, req_damage_px;
    int i, ok = 1, mismatch = 0, n_done = 0, n_partial;
    int cw = -1, ch = -1, reqw, reqh;
    RECT mon_rect;
    char *csv_path = NULL;
    FILE *csv;

    memset(&ds, 0, sizeof(ds));
    memset(&scene, 0, sizeof(scene));
    memset(&base, 0, sizeof(base));
    memset(&final, 0, sizeof(final));
    memset(&mon_rect, 0, sizeof(mon_rect));

    if (frames < 1) {
        case_result("benchmark", 1, "bench-frames-must-be-positive");
        return 1;
    }
    if (warmup < 0) {
        case_result("benchmark", 1, "bench-warmup-must-be-nonnegative");
        return 1;
    }
    if (mode == NULL || (g_strcmp0(mode, "full") != 0 &&
                         g_strcmp0(mode, "partial") != 0)) {
        case_result("benchmark", 1, "bench-mode-must-be-full-or-partial");
        return 1;
    }

    if (!draw_surface_create(&ds, lw, lh) || !surface_sync_metrics(&ds)) {
        draw_surface_destroy(&ds);
        case_result("benchmark", 1, "surface-create-failed");
        return 1;
    }
    /* The verified pixel oracle for this benchmark is scale 1 only; a scaled
     * surface would invalidate the physical-damage/readback assumptions. */
    if (ds.scale != 1.0) {
        out("BENCH scale=%.6f FAIL oracle-verified-only-at-scale-1\n", ds.scale);
        draw_surface_destroy(&ds);
        case_result("benchmark", 1, "bench-scale-must-be-1");
        return 1;
    }
    reqw = px_phys(lw, ds.scale);
    reqh = px_phys(lh, ds.scale);
    if (!surface_wait_physical_size(ds.surface, reqw, reqh)) {
        surface_client_size(ds.surface, &cw, &ch);
        out("BENCH size requested_physical=%dx%d actual=%dx%d FAIL\n",
            reqw, reqh, cw, ch);
        draw_surface_destroy(&ds);
        case_result("benchmark", 1, "client-size-not-reached");
        return 1;
    }
    surface_sync_metrics(&ds);
    if (!surface_client_size(ds.surface, &cw, &ch) || cw != reqw || ch != reqh) {
        out("BENCH size requested_physical=%dx%d actual=%dx%d FAIL\n",
            reqw, reqh, cw, ch);
        draw_surface_destroy(&ds);
        case_result("benchmark", 1, "client-size-mismatch");
        return 1;
    }
    if (!bench_monitor_contains(ds.surface, &mon_rect)) {
        out("BENCH containment window=offscreen-or-larger-than-monitor FAIL\n");
        draw_surface_destroy(&ds);
        case_result("benchmark", 1, "window-not-contained-in-monitor");
        return 1;
    }
    scale = ds.scale;
    out("BENCH config mode=%s width=%d height=%d scale=%.6f frames=%d "
        "warmup=%d client=%dx%d monitor_rect=%ld,%ld,%ld,%ld\n",
        mode, lw, lh, scale, frames, warmup, cw, ch,
        (long)mon_rect.left, (long)mon_rect.top,
        (long)mon_rect.right, (long)mon_rect.bottom);

    bench_damage_rect(mode, ds.lw, ds.lh, &dmg);
    n_partial = g_strcmp0(mode, "partial") == 0;
    req_damage_px = (double)px_phys(dmg.width, scale) *
                    (double)px_phys(dmg.height, scale);

    /* One full frame lays the base quadrants outside a partial damage region;
     * then warmup frames exercise exactly the timed code path. */
    scene.n_overlay = 0;
    if (!draw_surface_frame(&ds, &scene, 0, NULL) || !GdiFlush()) {
        draw_surface_destroy(&ds);
        case_result("benchmark", 1, "base-frame-or-gdi-flush-failed");
        return 1;
    }
    for (i = 0; i < warmup; i++) {
        bench_overlay(mode, i, &dmg, &scene);
        if (!draw_surface_frame(&ds, &scene, n_partial, n_partial ? &dmg : NULL) ||
            !GdiFlush()) {
            draw_surface_destroy(&ds);
            case_result("benchmark", 1, "warmup-frame-or-gdi-flush-failed");
            return 1;
        }
    }
    if (!draw_surface_capture(&ds, &base)) {
        draw_surface_destroy(&ds);
        case_result("benchmark", 1, "baseline-capture-failed");
        return 1;
    }

    /* Timed region: timer starts before the draw and stops after a successful
     * GdiFlush on this thread. Durations are buffered; CSV is written after. */
    samples = g_new(gint64, frames);
    for (i = 0; i < frames; i++) {
        gint64 t0, t1;
        bench_overlay(mode, i, &dmg, &scene);
        t0 = now_us();
        if (!draw_surface_frame(&ds, &scene, n_partial, n_partial ? &dmg : NULL)) {
            out("BENCH frame=%d draw-failed FAIL\n", i);
            ok = 0;
            break;
        }
        if (!GdiFlush()) {
            out("BENCH frame=%d gdi-flush-failed FAIL\n", i);
            ok = 0;
            break;
        }
        t1 = now_us();
        samples[n_done] = t1 - t0;
        sum_us += samples[n_done];
        n_done++;
    }
    if (!ok || n_done != frames) {
        g_free(samples);
        capture_free(&base);
        draw_surface_destroy(&ds);
        case_result("benchmark", 1, "timed-frames-invalid");
        return 1;
    }

    if (!draw_surface_capture(&ds, &final)) {
        g_free(samples);
        capture_free(&base);
        draw_surface_destroy(&ds);
        case_result("benchmark", 1, "final-capture-failed");
        return 1;
    }

    /* Correct final colour + untouched baseline, using the existing pixel
     * helpers, only AFTER the timed region. A verification that cannot run is
     * a failure, never a pass. */
    if (!full_quadrant_checks("benchmark", &final, ds.lw, ds.lh, scale, &mismatch))
        mismatch = 1;
    if (n_partial) {
        int rx = px_phys(dmg.x, scale), ry = px_phys(dmg.y, scale);
        int rw = px_phys(dmg.width, scale), rh = px_phys(dmg.height, scale);
        int cx = rx + rw / 2, cy = ry + rh / 2;
        int er, eg, eb;
        if ((frames - 1) % 2 == 0) { er = 0; eg = 255; eb = 255; }
        else                       { er = 255; eg = 0; eb = 255; }
        if (!check_px("benchmark", "final-overlay", &final, cx, cy,
                      er, eg, eb, 0))
            mismatch = 1;
        if (!captures_rgb_equal_outside(&base, &final, rx, ry, rw, rh)) {
            out("ORACLE benchmark untouched-outside-damage FAIL\n");
            mismatch = 1;
        } else {
            out("ORACLE benchmark untouched-outside-damage PASS\n");
        }
    } else {
        if (!captures_rgb_equal(&base, &final)) {
            out("ORACLE benchmark untouched-baseline FAIL\n");
            mismatch = 1;
        } else {
            out("ORACLE benchmark untouched-baseline PASS\n");
        }
    }

    sorted = g_new(gint64, frames);
    memcpy(sorted, samples, sizeof(gint64) * (size_t)frames);
    qsort(sorted, (size_t)frames, sizeof(gint64), bench_cmp_i64);
    p50 = bench_percentile(sorted, frames, 0.50);
    p95 = bench_percentile(sorted, frames, 0.95);
    vmin = sorted[0];
    vmax = sorted[frames - 1];
    mean_us = (double)sum_us / (double)frames;
    elapsed_s = (double)sum_us / 1e6;
    cyc_per_s = elapsed_s > 0.0 ? (double)frames / elapsed_s : 0.0;
    req_mpix_s = elapsed_s > 0.0
                     ? req_damage_px * (double)frames / elapsed_s / 1e6 : 0.0;

    if (csv_override && csv_override[0]) {
        csv_path = g_strdup(csv_override);
    } else if (g_outdir) {
        csv_path = g_strdup_printf("%s/bench-%s-w%dx%d-s%.3f.csv", g_outdir,
                                   mode, cw, ch, scale);
    } else {
        csv_path = g_strdup("gtk-cairo-buffer-bench.csv");
    }
    csv = fopen(csv_path, "wb");
    if (!csv) {
        out("BENCH csv-open-failed %s\n", csv_path);
        ok = 0;
    } else {
        int csv_err = 0;
        if (fprintf(csv, "width,height,scale,mode,index,elapsed_us,"
                         "requested_damage_pixels\n") < 0)
            csv_err = 1;
        for (i = 0; i < frames && !csv_err; i++) {
            if (fprintf(csv, "%d,%d,%.6f,%s,%d,%" G_GINT64_FORMAT ",%.0f\n",
                        cw, ch, scale, mode, i, samples[i],
                        req_damage_px) < 0)
                csv_err = 1;
        }
        if (ferror(csv))
            csv_err = 1;
        if (fclose(csv) != 0)
            csv_err = 1;
        if (csv_err) {
            out("BENCH csv-write-failed %s\n", csv_path);
            ok = 0;
        } else {
            out("BENCH csv %s\n", csv_path);
        }
    }

    out("BENCH samples mode=%s n=%d p50_us=%" G_GINT64_FORMAT
        " p95_us=%" G_GINT64_FORMAT " min_us=%" G_GINT64_FORMAT
        " max_us=%" G_GINT64_FORMAT " mean_us=%.1f "
        "requested_damage_pixels=%.0f\n",
        mode, frames, p50, p95, vmin, vmax, mean_us, req_damage_px);
    out("BENCH totals elapsed_us=%" G_GINT64_FORMAT
        " completed_gdi_frame_cycles_per_s=%.3f "
        "requested_damage_mpix_per_s=%.4f\n",
        sum_us, cyc_per_s, req_mpix_s);
    out("BENCH labels completed_gdi_frame_cycles_per_s="
        "NOT-monitor-or-display-FPS requested_damage_mpix_per_s="
        "NOT-unique-actual-displayed-pixels\n");

    g_free(samples);
    g_free(sorted);
    g_free(csv_path);
    capture_free(&base);
    capture_free(&final);
    draw_surface_destroy(&ds);

    if (!ok || mismatch) {
        case_result("benchmark", 1, mismatch ? "oracle-mismatch" : "csv-failed");
        return 1;
    }
    case_result("benchmark", 0, "numeric-benchmark-complete");
    return 0;
}

/* ---------------------------------------------------------------- self-test */

static int self_test(void)
{
    int ok = 1;
    unsigned char data[4 * 4 * 4];
    Capture cap;
    int x, y;

    for (y = 0; y < 4; y++)
        for (x = 0; x < 4; x++) {
            unsigned char *p = data + (y * 4 + x) * 4;
            p[0] = 10; p[1] = 20; p[2] = 30; p[3] = 255;
        }
    memset(&cap, 0, sizeof(cap));
    cap.width = 4; cap.height = 4; cap.stride = 16; cap.pixels = data;
    if (!check_px("selftest", "match", &cap, 1, 1, 30, 20, 10, 0)) ok = 0;
    if (check_px("selftest", "mismatch", &cap, 1, 1, 30, 20, 11, 0)) ok = 0;
    if (!check_px("selftest", "tolerance", &cap, 1, 1, 30, 20, 11, 1)) ok = 0;
    /* Fractional scales must round, never truncate. */
    if (px_phys(1, 1.5) != 2 || px_phys(3, 1.5) != 5 || px_phys(2, 1.25) != 3) {
        out("SELFTEST px_phys fractional FAIL\n");
        ok = 0;
    }
    if (!scale_matches(1.5, 1.5) || scale_matches(1.0, 1.5)) {
        out("SELFTEST scale_matches FAIL\n");
        ok = 0;
    }
    /* O2 sample-point regression lock: the new outside-tl sample (10,10) must
     * be provably outside damage (20,20,40,30), and the previous lw/4,lh/4
     * sample (50,40) must be inside it. If either changes, the O2 oracle became
     * self-comparing again. */
    if (point_in_rect(10, 10, 20, 20, 40, 30) ||
        !point_in_rect(50, 40, 20, 20, 40, 30)) {
        out("SELFTEST partial-outside-sample FAIL\n");
        ok = 0;
    }
    /* O11 content gate: a uniform (blank) capture must be rejected, a varied
     * capture must pass, so a blank arm can never satisfy the byte comparison. */
    {
        unsigned char uniform[8 * 8 * 4];
        unsigned char varied[8 * 8 * 4];
        Capture c;
        int distinct = 0, span[3] = { 0, 0, 0 };
        long nonbg = 0;

        memset(uniform, 0, sizeof(uniform));
        memset(varied, 0, sizeof(varied));
        for (y = 0; y < 8; y++) {
            for (x = 0; x < 8; x++) {
                unsigned char *p = varied + (y * 8 + x) * 4;
                p[0] = (unsigned char)(x * 32);
                p[1] = (unsigned char)(y * 32);
                p[2] = (unsigned char)((x + y) * 16);
                p[3] = 255;
            }
        }
        memset(&c, 0, sizeof(c));
        c.width = 8; c.height = 8; c.stride = 32;
        c.pixels = uniform;
        artwork_content_stats(&c, &distinct, &nonbg, span);
        if (content_nonuniform_ok(distinct, nonbg, 64, span)) {
            out("SELFTEST blank-content accepted FAIL\n");
            ok = 0;
        }
        c.pixels = varied;
        artwork_content_stats(&c, &distinct, &nonbg, span);
        if (!content_nonuniform_ok(distinct, nonbg, 64, span)) {
            out("SELFTEST varied-content rejected FAIL\n");
            ok = 0;
        }
    }
    /* Benchmark math is pure, so preflight verifies it without a GUI: sorting,
     * nearest-rank p50/p95, the 1/16 partial rect, and the overlay sequence. */
    {
        gint64 v[5] = { 50, 10, 30, 20, 40 };
        gint64 s[5];
        cairo_rectangle_int_t r;
        Scene sc;
        memcpy(s, v, sizeof(s));
        qsort(s, 5, sizeof(gint64), bench_cmp_i64);
        if (s[0] != 10 || s[4] != 50) {
            out("SELFTEST bench-sort FAIL\n");
            ok = 0;
        }
        if (bench_percentile(s, 5, 0.50) != 30 ||
            bench_percentile(s, 5, 0.95) != 50 ||
            bench_percentile(s, 5, 1.00) != 50) {
            out("SELFTEST bench-percentile FAIL\n");
            ok = 0;
        }
        bench_damage_rect("partial", 200, 160, &r);
        if (r.x != 75 || r.y != 60 || r.width != 50 || r.height != 40) {
            out("SELFTEST bench-partial-rect FAIL\n");
            ok = 0;
        }
        bench_damage_rect("full", 200, 160, &r);
        if (r.x != 0 || r.y != 0 || r.width != 200 || r.height != 160) {
            out("SELFTEST bench-full-rect FAIL\n");
            ok = 0;
        }
        memset(&sc, 0, sizeof(sc));
        bench_overlay("partial", 0, &r, &sc);
        if (sc.n_overlay != 1 || sc.overlay[0].r != 0.0 ||
            sc.overlay[0].g != 1.0 || sc.overlay[0].b != 1.0) {
            out("SELFTEST bench-overlay-even FAIL\n");
            ok = 0;
        }
        bench_overlay("partial", 1, &r, &sc);
        if (sc.n_overlay != 1 || sc.overlay[0].r != 1.0 ||
            sc.overlay[0].g != 0.0 || sc.overlay[0].b != 1.0) {
            out("SELFTEST bench-overlay-odd FAIL\n");
            ok = 0;
        }
        bench_overlay("full", 0, &r, &sc);
        if (sc.n_overlay != 0) {
            out("SELFTEST bench-overlay-full FAIL\n");
            ok = 0;
        }
    }
    /* Strict CLI bounds parser: accept the inclusive endpoints, reject empty,
     * non-numeric, trailing junk, fraction, negative and out-of-range so a bad
     * --bench-* value can never reach GUI creation with a silent clamp. */
    {
        long long pv = -1;
        int good = bench_parse_bounded("1", 1, 16384, &pv) && pv == 1 &&
                   bench_parse_bounded("16384", 1, 16384, &pv) && pv == 16384 &&
                   bench_parse_bounded("0", 0, 100000, &pv) && pv == 0 &&
                   bench_parse_bounded("100000", 0, 100000, &pv) && pv == 100000;
        int bad = bench_parse_bounded("", 1, 16384, &pv) ||
                  bench_parse_bounded("0", 1, 16384, &pv) ||
                  bench_parse_bounded("-1", 1, 16384, &pv) ||
                  bench_parse_bounded("16385", 1, 16384, &pv) ||
                  bench_parse_bounded("-1", 0, 100000, &pv) ||
                  bench_parse_bounded("100001", 0, 100000, &pv) ||
                  bench_parse_bounded("abc", 1, 16384, &pv) ||
                  bench_parse_bounded("12x", 1, 16384, &pv) ||
                  bench_parse_bounded("1.5", 1, 16384, &pv);
        if (!good || bad) {
            out("SELFTEST bench-cli-bounds FAIL\n");
            ok = 0;
        }
    }
    out("SELFTEST %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

/* ---------------------------------------------------------------- main    */

static const char *arg_value(int argc, char **argv, const char *key)
{
    int i;
    for (i = 1; i + 1 < argc; i++)
        if (g_strcmp0(argv[i], key) == 0)
            return argv[i + 1];
    return NULL;
}

static int has_flag(int argc, char **argv, const char *key)
{
    int i;
    for (i = 1; i < argc; i++)
        if (g_strcmp0(argv[i], key) == 0)
            return 1;
    return 0;
}

static void list_monitors(void)
{
    GdkDisplay *d = gdk_display_get_default();
    GListModel *m;
    guint n, i;

    if (!d) {
        out("MONITORS display=none\n");
        return;
    }
    m = gdk_display_get_monitors(d);
    n = m ? g_list_model_get_n_items(m) : 0;
    out("MONITORS count=%u\n", n);
    for (i = 0; i < n; i++) {
        GdkMonitor *mo = g_list_model_get_item(m, i);
        GdkRectangle g;
        gdk_monitor_get_geometry(mo, &g);
        out("MONITOR %u scale=%d x=%d y=%d w=%d h=%d\n", i,
            gdk_monitor_get_scale_factor(mo), g.x, g.y, g.width, g.height);
        g_object_unref(mo);
    }
}

static void print_loaded_modules(void)
{
    const wchar_t *names[] = { L"libgtk-4-1.dll", L"libgdk-4-1.dll",
                               L"libcairo-2.dll" };
    guint i;
    for (i = 0; i < G_N_ELEMENTS(names); i++) {
        HMODULE mod = GetModuleHandleW(names[i]);
        wchar_t wpath[32768];
        char *name = g_utf16_to_utf8((const gunichar2 *)names[i], -1, NULL, NULL, NULL);
        if (mod && GetModuleFileNameW(mod, wpath, 32768)) {
            char *path = g_utf16_to_utf8((const gunichar2 *)wpath, -1, NULL, NULL, NULL);
            out("LOADED %s %s\n", name, path ? path : "(unprintable)");
            g_free(path);
        } else {
            out("LOADED %s (not loaded)\n", name);
        }
        g_free(name);
    }
}

static int merge_rc(int rc, int outcome)
{
    if (outcome == 1)      /* FAIL always dominates */
        return 1;
    if (outcome == 2 && rc == 0)   /* SKIP/unexecuted is incomplete, not pass */
        return 3;
    return rc;
}

/* GTK 4.22 warns once when a public GdkDrawContext is used without the private
 * attach step, then attaches it itself (gdkdrawcontext.c). That notice, and its
 * disposal counterpart, are compatibility messages for public-API drawing, not
 * rendering faults. Route exactly those two to stdout as GDK-COMPAT and never let
 * G_DEBUG=fatal-warnings abort on them; every other message goes to the default
 * writer unchanged (so it stays fatal and stays in stderr). A message that cannot
 * be read reliably is left to the default writer. */
static const char *log_field(const GLogField *fields, gsize n_fields, const char *key)
{
    for (gsize i = 0; i < n_fields; ++i) {
        if (strcmp(fields[i].key, key) == 0)
            return fields[i].length == -1 ? (const char *)fields[i].value : NULL;
    }
    return NULL;
}

static GLogWriterOutput harness_log_writer(GLogLevelFlags level, const GLogField *fields, gsize n_fields,
                                           gpointer user_data)
{
    const char *domain = log_field(fields, n_fields, "GLIB_DOMAIN");
    const char *message = log_field(fields, n_fields, "MESSAGE");
    if ((level & G_LOG_LEVEL_WARNING) && domain && strcmp(domain, "Gdk") == 0 && message &&
        (strstr(message, "has not been set up for rendering. Attaching ") != NULL ||
         strstr(message, "is still attached for rendering on disposal, detaching it.") != NULL)) {
        printf("GDK-COMPAT %s\n", message);
        fflush(stdout);
        return G_LOG_WRITER_HANDLED;
    }
    return g_log_writer_default(level, fields, n_fields, user_data);
}

int main(int argc, char **argv)
{
    const char *case_name;
    const char *scale_s;
    const char *bench_mode;
    const char *bench_csv;
    double scale = 0.0;
    int rc = 0;
    int benchmark = has_flag(argc, argv, "--benchmark");
    int bench_width = 1280, bench_height = 720;
    int bench_frames = 120, bench_warmup = 20;

    g_log_set_writer_func(harness_log_writer, NULL, NULL);

    if (has_flag(argc, argv, "--self-test"))
        return self_test();

    g_outdir = arg_value(argc, argv, "--out");
    if (has_flag(argc, argv, "--physical"))
        g_physical = 1;
    g_expect_retained = has_flag(argc, argv, "--expect-retained");
    scale_s = arg_value(argc, argv, "--scale");
    if (scale_s)
        scale = g_ascii_strtod(scale_s, NULL);
    g_dump_tag = g_strdup_printf("s%s", scale_s ? scale_s : "auto");
    {
        const char *rounds = arg_value(argc, argv, "--rounds");
        if (rounds)
            g_rounds = atoi(rounds);
    }
    /* Separate benchmark mode: the existing --case/--scale flows above are
     * untouched when --benchmark is absent. Every numeric bound is validated
     * as a full string BEFORE detect_session()/gtk_init_check(), so a bad
     * value can never create a window and is never silently clamped.
     * Exit 2 is the documented usage failure. */
    bench_mode = arg_value(argc, argv, "--bench-mode");
    if (!bench_mode)
        bench_mode = "full";
    bench_csv = arg_value(argc, argv, "--bench-csv");
    if (benchmark) {
        const char *v;
        if ((v = arg_value(argc, argv, "--bench-width")) != NULL &&
            !bench_cli_int(v, 1, 16384, &bench_width, "bench-width"))
            return 2;
        if ((v = arg_value(argc, argv, "--bench-height")) != NULL &&
            !bench_cli_int(v, 1, 16384, &bench_height, "bench-height"))
            return 2;
        if ((v = arg_value(argc, argv, "--bench-frames")) != NULL &&
            !bench_cli_int(v, 1, 100000, &bench_frames, "bench-frames"))
            return 2;
        if ((v = arg_value(argc, argv, "--bench-warmup")) != NULL &&
            !bench_cli_int(v, 0, 100000, &bench_warmup, "bench-warmup"))
            return 2;
        if (g_strcmp0(bench_mode, "full") != 0 &&
            g_strcmp0(bench_mode, "partial") != 0) {
            out("HARNESS FAIL bench-mode-invalid value='%s' "
                "allowed=full|partial\n", bench_mode);
            return 2;
        }
    }

    /* A requested-physical flag is not proof of an interactive console. Record
     * the active-console id, the WTS client protocol type and SM_REMOTESESSION,
     * and refuse --physical unless this process is the active console with a
     * console protocol. RDP/legacy/unknown and session 0 are all rejected. */
    detect_session();
    {
        char proto_str[16];
        if (g_wts_protocol < 0)
            g_strlcpy(proto_str, "unknown", sizeof(proto_str));
        else
            g_snprintf(proto_str, sizeof(proto_str), "%d", g_wts_protocol);
        out("SESSION id=%lu physical_requested=%d active_console=%lu "
            "protocol=%s remote=%d physical_ok=%d\n",
            (unsigned long)g_session_id, g_physical,
            (unsigned long)g_active_console, proto_str, g_remote_session,
            g_physical_ok);
    }
    /* Benchmark mode makes the same physical-console classification mandatory
     * even without an explicit --physical. */
    if ((g_physical || benchmark) && !g_physical_ok) {
        out("HARNESS FAIL physical-requested-not-active-console "
            "(id=%lu active_console=%lu protocol=%d remote=%d)\n",
            (unsigned long)g_session_id, (unsigned long)g_active_console,
            g_wts_protocol, g_remote_session);
        return 3;
    }
    /* Record the run configuration before GTK can consume it. The driver
     * requires GDK_DISABLE to contain dcomp (GDK_FEATURE_DCOMP off, so the
     * Win32 Cairo context has no swap chain and uses the GDI buffer path). */
    out("ENV GDK_DISABLE=%s GDK_DEBUG=%s GSK_RENDERER=%s GDK_BACKEND=%s\n",
        getenv("GDK_DISABLE") ? getenv("GDK_DISABLE") : "",
        getenv("GDK_DEBUG") ? getenv("GDK_DEBUG") : "",
        getenv("GSK_RENDERER") ? getenv("GSK_RENDERER") : "",
        getenv("GDK_BACKEND") ? getenv("GDK_BACKEND") : "");
    out("ENV BUFFER=%s\n", getenv("GDK_WIN32_CAIRO_GDI_BUFFER") ?
        getenv("GDK_WIN32_CAIRO_GDI_BUFFER") : "unset");

    if (!gtk_init_check()) {
        out("HARNESS FAIL gtk_init_check-failed (interactive Windows session required)\n");
        return 1;
    }
    print_loaded_modules();
    /* Record the actual monitors (dimensions + scale) GDK exposes for this run;
     * --list-monitors prints them and exits before any case. */
    list_monitors();
    if (has_flag(argc, argv, "--list-monitors"))
        return 0;
    if (g_outdir)
        g_mkdir_with_parents(g_outdir, 0755);

    if (benchmark) {
        rc = merge_rc(rc, case_benchmark(bench_width, bench_height, bench_mode,
                                         bench_frames, bench_warmup, bench_csv));
        out("SUMMARY pass=%d fail=%d skip=%d physical=%d physical_ok=%d exit=%d\n",
            g_results.pass, g_results.fail, g_results.skip, g_physical,
            g_physical_ok, rc);
        return rc;
    }

    case_name = arg_value(argc, argv, "--case");
    if ((!case_name || g_strcmp0(case_name, "lifecycle") == 0) && g_rounds < 26) {
        g_printerr("lifecycle requires --rounds >= 26\n"); return 2;
    }
    if (!case_name) {
        rc = merge_rc(rc, case_full(scale));
        rc = merge_rc(rc, case_partial(scale));
        rc = merge_rc(rc, case_multi(scale));
        rc = merge_rc(rc, case_alpha_boundaries(scale));
        rc = merge_rc(rc, case_alpha_half(scale));
        rc = merge_rc(rc, case_resize(scale));
        rc = merge_rc(rc, case_minimize_restore(scale));
        rc = merge_rc(rc, case_popup(scale));
        rc = merge_rc(rc, case_dialog(scale));
        rc = merge_rc(rc, case_gtk_renderer(scale));
        rc = merge_rc(rc, case_visual_artwork(scale));
        rc = merge_rc(rc, case_lifecycle());
        rc = merge_rc(rc, case_retention(scale));
        rc = merge_rc(rc, case_first_partial(scale, 0));
    } else if (g_strcmp0(case_name, "full") == 0) {
        rc = merge_rc(rc, case_full(scale));
    } else if (g_strcmp0(case_name, "partial") == 0) {
        rc = merge_rc(rc, case_partial(scale));
    } else if (g_strcmp0(case_name, "multi_damage") == 0) {
        rc = merge_rc(rc, case_multi(scale));
    } else if (g_strcmp0(case_name, "alpha") == 0) {
        rc = merge_rc(rc, case_alpha_boundaries(scale));
    } else if (g_strcmp0(case_name, "alpha_half") == 0) {
        rc = merge_rc(rc, case_alpha_half(scale));
    } else if (g_strcmp0(case_name, "resize") == 0) {
        rc = merge_rc(rc, case_resize(scale));
    } else if (g_strcmp0(case_name, "minimize_restore") == 0) {
        rc = merge_rc(rc, case_minimize_restore(scale));
    } else if (g_strcmp0(case_name, "popup") == 0) {
        rc = merge_rc(rc, case_popup(scale));
    } else if (g_strcmp0(case_name, "dialog") == 0) {
        rc = merge_rc(rc, case_dialog(scale));
    } else if (g_strcmp0(case_name, "gtk_renderer") == 0) {
        rc = merge_rc(rc, case_gtk_renderer(scale));
    } else if (g_strcmp0(case_name, "visual_artwork") == 0) {
        rc = merge_rc(rc, case_visual_artwork(scale));
    } else if (g_strcmp0(case_name, "retention") == 0) {
        rc = merge_rc(rc, case_retention(scale));
    } else if (g_strcmp0(case_name, "first_partial") == 0) {
        rc = merge_rc(rc, case_first_partial(scale, 0));
    } else if (g_strcmp0(case_name, "odd_client") == 0) {
        rc = merge_rc(rc, case_first_partial(scale, 1));
    } else if (g_strcmp0(case_name, "alloc_latch") == 0 ||
               g_strcmp0(case_name, "first_transfer") == 0 ||
               g_strcmp0(case_name, "partial_transfer") == 0 ||
               g_strcmp0(case_name, "retry_backoff") == 0) {
        rc = merge_rc(rc, case_fault(case_name, scale));
    } else if (g_strcmp0(case_name, "lifecycle") == 0) {
        rc = merge_rc(rc, case_lifecycle());
    } else {
        g_printerr("unknown case: %s\n", case_name);
        return 2;
    }

    out("SUMMARY pass=%d fail=%d skip=%d physical=%d physical_ok=%d exit=%d\n",
        g_results.pass, g_results.fail, g_results.skip, g_physical,
        g_physical_ok, rc);
    return rc;
}
