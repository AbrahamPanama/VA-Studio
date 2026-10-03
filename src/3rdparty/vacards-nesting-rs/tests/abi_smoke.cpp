// SPDX-License-Identifier: GPL-2.0-or-later

#include "vacards_nesting.h"

#include <cstring>
#include <iostream>

int main()
{
    if (vac_nesting_api_version() != VAC_NESTING_API_VERSION) {
        std::cerr << "unexpected nesting ABI version\n";
        return 1;
    }
    if (std::strcmp(vac_nesting_jagua_revision(), VAC_NESTING_JAGUA_REVISION) != 0) {
        std::cerr << "unexpected jagua-rs revision\n";
        return 2;
    }
    if (vac_nesting_dependency_probe() != 4 || vac_nesting_run_self_test() != 0) {
        std::cerr << "Rust nesting dependency probe failed\n";
        return 3;
    }

    return 0;
}
