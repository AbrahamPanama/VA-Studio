// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * LPE test file wrapper
 *//*
 * Authors: see git history
 *
 * Copyright (C) 2020 Authors
 *
 * Released under GNU GPL version 2 or later, read the file 'COPYING' for more information
 */

#include <gtest/gtest.h>

#include <2geom/pathvector.h>

#include <cmath>
#include <optional>

#include "document.h"
#include "file.h"
#include "inkscape.h"
#include "inkscape-application.h"

#include "extension/init.h"
#include "object/sp-root.h"
#include "svg/svg.h"
#include "util/numeric/converters.h"

using namespace Inkscape;

/* This class allow test LPE's. To make possible in latest release of Inkscape
 * LPE is not updated on load (if in the future any do we must take account) so we load
 * a svg, get all "d" attribute from paths, shapes...
 * Update all path effects with root object and check equality of paths.
 * We use some helpers inside the SVG document to test:
 * inkscape:test-threshold="0.1" can be global using in root element or per item
 * inkscape:test-ignore="1" ignore this element from tests
 */

class ComparePathsTest : public ::testing::Test
{
protected:
    /*
    * @a is in LPE the path data before LPE in store integrity is the file just opened before save
    * @b is in LPE the path with LPE updated in, in integrity is the data in path after save to disk
    * @c the svg file name to compare
    */
    void pathCompare(char const *a, char const *b, Glib::ustring const &id, std::string const &svg, double precision = 0.001)
    {
        bool success = false;
        pathCompareInternal(a, b, precision, success);
        if (!success) {
            #define DIAG(x) "  " << #x << ":\n    " << x << std::endl
            std::cout << "More information about the failure:\n" << DIAG(svg) << DIAG(id) << DIAG(a) << DIAG(b);
            failed.emplace_back(id);
        }
    }
    /*
    * succes is pased because functions with gtest asserts usualy be void you can override using 
    * HasFatalFailure() or similar on caller but but fail in all next calls and we want the list of all failed id only
    */
    void pathCompareInternal(char const *a, char const *b, double precision, bool &success)
    {
        if (!b) { // a is never nullptr
            FAIL() << "Path not set";
        }
        bool a_empty = a[0] == '\0';
        bool b_empty = b[0] == '\0';
        if (a_empty || b_empty) {
            if (a_empty && b_empty) {
                success = true;
                return;
            } else {
                FAIL() << "Mismatching emptiness of paths";
            }
        }

        Geom::PathVector apv = sp_svg_read_pathv(a);
        Geom::PathVector bpv = sp_svg_read_pathv(b);
        if (apv.empty()) {
            FAIL() << "Couldn't parse original 'd'";
        }
        if (bpv.empty()) {
            FAIL() << "Couldn't parse 'd'";
        }

        if (apv == bpv) {
            success = true;
            return;
        }

        size_t totala = apv.curveCount();
        size_t totalb = bpv.curveCount();
        ASSERT_EQ(totala, totalb);

        auto paths_match = [&](Geom::PathVector const &candidate, size_t offset) {
            auto current = offset;
            for (size_t i = 0; i < totala; i++) {
                if (current >= totala) {
                    current = 0;
                }
                for (auto fraction : {0.0, 0.2, 0.4}) {
                    auto const pointa = apv.pointAt(i + fraction);
                    auto const pointb = candidate.pointAt(current + fraction);
                    // Preserve the historical per-axis threshold. Geom::are_near()
                    // applies a radial distance and would make diagonal deviations
                    // stricter than the assertions this helper replaces.
                    if (std::abs(pointa[Geom::X] - pointb[Geom::X]) > precision ||
                        std::abs(pointa[Geom::Y] - pointb[Geom::Y]) > precision) {
                        return false;
                    }
                }
                current++;
            }
            return true;
        };

        auto find_matching_offset = [&](Geom::PathVector const &candidate) -> std::optional<size_t> {
            for (size_t i = 0; i < totala; i++) {
                if (paths_match(candidate, i)) {
                    return i;
                }
            }
            return std::nullopt;
        };

        auto initial = find_matching_offset(bpv);
        bool reversed = false;
        if (!initial) {
            bpv.reverse();
            initial = find_matching_offset(bpv);
            reversed = initial.has_value();
        }

        if (!initial) {
            FAIL() << "Paths differ in both forward and reverse directions";
        }

        if (reversed) {
            std::cout << "[ WARN     ] Curve reversed. We do not block here. We reverse the path and test node positions on reverse" << std::endl;
        }
        if (*initial != 0) {
            std::cout << "[ WARN     ] Different starting node. We do not block here. We gap the origin to " << *initial << " de " << totala << " and test with the pathvector reindexed" << std::endl;
        }
        // we do whoole function without a fail so inform tall be sacessfull
        success = true;
    }
    
    void TearDown() override
    { 
        Glib::ustring ids;
        for (auto const &fail : failed) {
            if (!ids.empty()) {
                ids += ",";
            }
            ids += fail;
        }
        if (!ids.empty()) {
            FAIL() << "[FAILED IDS] " << ids; 
        }
    }
    
    double getPrecision(SPObject *root, SPObject *current) {
        if (auto threshold = root->getAttribute("inkscape:test-threshold")) {
            return Util::read_number(threshold);
        }
        if (auto threshold = current->getAttribute("inkscape:test-threshold")) {
            return Util::read_number(threshold);
        }
        return 0.001;
    }
    std::vector<Glib::ustring> failed;
};

/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
// vim: expandtab:shiftwidth=4:tabstop=8:softtabstop=4 :
