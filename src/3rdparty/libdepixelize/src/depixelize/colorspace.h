// SPDX-License-Identifier: LGPL-2.1-or-later OR GPL-2.0-or-later
/*
 * Authors:
 *   Vinícius dos Santos Oliveira <vini.ipsmaker@gmail.com>
 *
 * Copyright (C) 2013 Authors
 */

#ifndef LIBDEPIXELIZE_YUV_H
#define LIBDEPIXELIZE_YUV_H

#include <cmath>
#include <cstdint>

namespace Depixelize::ColorSpace {

/**
 * The same algorithm used in hqx filter
 */
inline void rgb2yuv(uint8_t r, uint8_t g, uint8_t b,
                    uint8_t &y, uint8_t &u, uint8_t &v)
{
    y = 0.299 * r + 0.587 * g + 0.114 * b;
    u = uint16_t(-0.169 * r - 0.331 * g + 0.5 * b) + 128;
    v = uint16_t(0.5 * r - 0.419 * g - 0.081 * b) + 128;
}

inline void rgb2yuv(const uint8_t *rgb, uint8_t *yuv)
{
    rgb2yuv(rgb[0], rgb[1], rgb[2], yuv[0], yuv[1], yuv[2]);
}

inline bool same_color(const uint8_t (a)[4], const uint8_t (&b)[4])
{
    return a[0] == b[0]
        && a[1] == b[1]
        && a[2] == b[2]
        && a[3] == b[3];
}

inline bool dissimilar_colors(const uint8_t *a, const uint8_t *b)
{
    // C uses row-major order, so 
    // A[2][3] = { {1, 2, 3}, {4, 5, 6} } = {1, 2, 3, 4, 5, 6}
    uint8_t yuv[2][3];
    rgb2yuv(a, yuv[0]);
    rgb2yuv(b, yuv[1]);

    // Magic numbers taken from hqx algorithm
    // Only used to describe the level of tolerance
    return std::abs(yuv[0][0] - yuv[1][0]) > 0x30
        || std::abs(yuv[0][1] - yuv[1][1]) > 7
        || std::abs(yuv[0][2] - yuv[1][2]) > 6;
}

inline bool similar_colors(const uint8_t *a, const uint8_t *b)
{
    return !dissimilar_colors(a, b);
}

inline bool shading_edge(const uint8_t *a, const uint8_t *b)
{
    // C uses row-major order, so 
    // A[2][3] = { {1, 2, 3}, {4, 5, 6} } = {1, 2, 3, 4, 5, 6}
    uint8_t yuv[2][3];
    rgb2yuv(a, yuv[0]);
    rgb2yuv(b, yuv[1]);

    // Magic numbers taken from Kopf-Lischinski algorithm
    // Only used to describe the level of tolerance
    return std::abs(yuv[0][0] - yuv[1][0]) <= 100
        && std::abs(yuv[0][1] - yuv[1][1]) <= 100
        && std::abs(yuv[0][2] - yuv[1][2]) <= 100;
}

inline bool contour_edge(const uint8_t *a, const uint8_t *b)
{
    return !shading_edge(a, b);
}

} // namespace Depixelize::ColorSpace

#endif // LIBDEPIXELIZE_YUV_H

/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0)(case-label . +))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:encoding=utf-8:textwidth=99 :
