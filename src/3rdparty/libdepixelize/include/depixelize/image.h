// SPDX-License-Identifier: LGPL-2.1-or-later OR GPL-2.0-or-later

#ifndef LIBDEPIXELIZE_IMAGE_H
#define LIBDEPIXELIZE_IMAGE_H

#include <cstdint>

namespace Depixelize {

/**
 * The data format for supplying bitmap images to libdepixelize.
 *
 * Images must be formatted as either RGB or RGBA with 8 bits per channel
 * in row-major order.
 *
 * This happens to exactly match the format used by GdkPixbuf, and indeed
 * all the fields of @a Image can be straightforwardly initialised from the
 * GdkPixbuf properties of the same name.
 */
struct Image
{
    std::uint8_t const *pixels;
    int n_channels; ///< Must be 3 (for RGB) or 4 (for RGBA).
    int width; ///< Must be positive.
    int height; ///< Must be positive.
    int rowstride;
};

} // namespace Depixelize

#endif // LIBDEPIXELIZE_IMAGE_H
