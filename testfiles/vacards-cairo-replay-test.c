/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Public Cairo API regression for all-clipped recording replay.
 * No platform loader APIs: the caller must verify the actual Cairo runtime.
 * Stock Cairo 1.18.4 may crash in the all-clipped case; disable core dumps
 * in a supervising process when using that library as a negative control.
 */
#include <cairo.h>
#include <inttypes.h>
#include <stdio.h>

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
        /* Same overall extents, but no intersecting boxes. During direct
         * replay, clip intersection must return the all-clipped sentinel. */
        cairo_rectangle(cr, 20, 0, 4, 4);
        cairo_rectangle(cr, 0, 20, 4, 4);
        cairo_clip(cr);
    }
    /* SOURCE selects direct recording replay with the target clip retained,
     * rather than rasterizing the recording with a NULL replay clip. */
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_surface(cr, recording, 0, 0);
    cairo_paint(cr);
    cairo_surface_flush(image);

    int valid = cairo_status(rec) == CAIRO_STATUS_SUCCESS &&
                cairo_status(cr) == CAIRO_STATUS_SUCCESS &&
                cairo_surface_status(recording) == CAIRO_STATUS_SUCCESS &&
                cairo_surface_status(image) == CAIRO_STATUS_SUCCESS;
    if (!valid) {
        fprintf(stderr, "Cairo error: recording=%s, replay=%s, recording surface=%s, image=%s\n",
                cairo_status_to_string(cairo_status(rec)),
                cairo_status_to_string(cairo_status(cr)),
                cairo_status_to_string(cairo_surface_status(recording)),
                cairo_status_to_string(cairo_surface_status(image)));
    }
    unsigned char *pixels = cairo_image_surface_get_data(image);
    int stride = cairo_image_surface_get_stride(image);
    if (!pixels) {
        fputs("Missing image pixels\n", stderr);
        valid = 0;
    }
    for (int y = 0; valid && y < 24; ++y) {
        const uint32_t *row = (const uint32_t *)(pixels + y * stride);
        for (int x = 0; x < 24; ++x) {
            uint32_t expected = !disjoint && ((x < 4 && y < 4) || (x >= 20 && y >= 20))
                                    ? UINT32_C(0xffff0000) : 0;
            if (row[x] != expected) {
                fprintf(stderr, "Pixel (%d,%d): expected %08" PRIx32 ", got %08" PRIx32 "\n",
                        x, y, expected, row[x]);
                valid = 0;
                break;
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
    /* Preserve the last completed case even if stock Cairo faults. */
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("Cairo headers=%s runtime=%s\n", CAIRO_VERSION_STRING, cairo_version_string());
    if (cairo_version() != CAIRO_VERSION) {
        fputs("Header/runtime Cairo version mismatch\n", stderr);
        return 1;
    }
    puts("RUN visible-control");
    if (!check_replay(0)) {
        fputs("FAIL visible-control\n", stderr);
        return 2;
    }
    puts("PASS visible-control");
    puts("RUN all-clipped-replay");
    if (!check_replay(1)) {
        fputs("FAIL all-clipped-replay\n", stderr);
        return 3;
    }
    puts("PASS all-clipped-replay");
    return 0;
}
