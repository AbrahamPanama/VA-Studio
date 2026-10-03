// SPDX-License-Identifier: GPL-2.0-or-later
// Synthetic output profile for CLI tests; never install or use for production.
#include <lcms2.h>

int main(int argc, char **argv)
{
    if (argc != 2) {
        return 2;
    }
    auto profile = cmsCreate_sRGBProfile();
    if (!profile) {
        return 1;
    }
    cmsSetDeviceClass(profile, cmsSigOutputClass);
    cmsSetHeaderRenderingIntent(profile, INTENT_RELATIVE_COLORIMETRIC);
    bool const saved = cmsSaveProfileToFile(profile, argv[1]);
    cmsCloseProfile(profile);
    return saved ? 0 : 1;
}
