/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Public-API regression for the all-clipped recording replay intersection.
 * Built/run only against the installed Windows dependency by its builder.
 * Place the executable next to libcairo-2.dll; reject another loaded copy.
 */
#include <cairo.h>
#include <stdint.h>
#include <stdio.h>
#include <wchar.h>
#include <windows.h>

static int check_loaded_cairo(void)
{
    wchar_t executable[32768], library[32768];
    DWORD n = GetModuleFileNameW(NULL, executable, 32768);
    HMODULE module = GetModuleHandleW(L"libcairo-2.dll");
    if (!n || n >= 32768 || !module) return 0;
    wchar_t *separator = wcsrchr(executable, L'\\');
    if (!separator || (size_t)(separator - executable) + 16 >= 32768) return 0;
    wcscpy(separator + 1, L"libcairo-2.dll");
    n = GetModuleFileNameW(module, library, 32768);
    if (!n || n >= 32768 || _wcsicmp(executable, library) != 0) return 0;
    fwprintf(stdout, L"Loaded Cairo: %ls\n", library);
    return cairo_version() == CAIRO_VERSION;
}

static int check_replay(int disjoint)
{
    cairo_rectangle_t bounds = {0, 0, 24, 24};
    cairo_surface_t *recording = cairo_recording_surface_create(CAIRO_CONTENT_COLOR_ALPHA, &bounds);
    cairo_t *rec = cairo_create(recording);
    cairo_rectangle(rec, 0, 0, 4, 4);
    cairo_rectangle(rec, 20, 20, 4, 4);
    cairo_clip(rec);
    cairo_set_source_rgba(rec, 1, 0, 0, 1);
    cairo_paint(rec);

    cairo_surface_t *image = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 24, 24);
    cairo_t *cr = cairo_create(image);
    if (disjoint) {
        /* Same aggregate extents, disjoint boxes: replay must transition to
         * the immutable all-clipped sentinel inside intersect_clip(). */
        cairo_rectangle(cr, 20, 0, 4, 4);
        cairo_rectangle(cr, 0, 20, 4, 4);
        cairo_clip(cr);
    }
    /* SOURCE selects composite_aligned_boxes()'s direct recording replay,
     * which retains the target clip instead of rasterizing with a NULL clip. */
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_surface(cr, recording, 0, 0);
    cairo_paint(cr);
    cairo_surface_flush(image);
    int valid = cairo_status(rec) == CAIRO_STATUS_SUCCESS && cairo_status(cr) == CAIRO_STATUS_SUCCESS &&
                cairo_surface_status(recording) == CAIRO_STATUS_SUCCESS && cairo_surface_status(image) == CAIRO_STATUS_SUCCESS;
    unsigned char *pixels = cairo_image_surface_get_data(image);
    int stride = cairo_image_surface_get_stride(image);
    if (!pixels) valid = 0;
    if (valid) {
        for (int y = 0; y < 24; ++y) {
            const uint32_t *row = (const uint32_t *)(pixels + y * stride);
            for (int x = 0; x < 24; ++x) {
                uint32_t expected = !disjoint && ((x < 4 && y < 4) || (x >= 20 && y >= 20)) ? UINT32_C(0xffff0000) : 0;
                if (row[x] != expected) valid = 0;
            }
        }
    }
    cairo_destroy(cr);
    cairo_surface_destroy(image);
    cairo_destroy(rec);
    cairo_surface_destroy(recording);
    return valid;
}

int main(void)
{
    if (!check_loaded_cairo()) {
        fputs("Wrong loaded Cairo DLL or header/runtime version\n", stderr);
        return 1;
    }
    if (!check_replay(1) || !check_replay(0)) {
        fputs("Clipped recording replay or visible control failed\n", stderr);
        return 2;
    }
    fwprintf(stdout, L"VACards Cairo clipping regression passed\n");
    return 0;
}
