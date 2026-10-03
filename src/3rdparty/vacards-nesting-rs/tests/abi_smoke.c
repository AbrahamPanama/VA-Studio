// SPDX-License-Identifier: GPL-2.0-or-later

#include "vacards_nesting.h"

#include <stdio.h>
#include <string.h>

int main(void)
{
    if (vac_nesting_api_version() != VAC_NESTING_API_VERSION) {
        fputs("unexpected nesting ABI version\n", stderr);
        return 1;
    }
    if (strcmp(vac_nesting_jagua_revision(), VAC_NESTING_JAGUA_REVISION) != 0) {
        fputs("unexpected jagua-rs revision\n", stderr);
        return 2;
    }
    if (vac_nesting_dependency_probe() != 4) {
        fputs("jagua-rs geometry probe failed\n", stderr);
        return 3;
    }
    if (vac_nesting_run_self_test() != 0) {
        fputs("nesting static-library self-test failed\n", stderr);
        return 4;
    }

    return 0;
}
