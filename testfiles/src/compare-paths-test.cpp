// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * Regression tests for the LPE path comparison helper.
 */

#include "testfiles/compare-paths-test.h"

class ComparePathsRegressionTest : public ComparePathsTest
{
protected:
    void expect_paths_match(char const *first, char const *second)
    {
        bool success = false;
        pathCompareInternal(first, second, 0.001, success);
        EXPECT_TRUE(success);
    }
};

TEST_F(ComparePathsRegressionTest, AcceptsReversedClosedPath)
{
    expect_paths_match("M 0,0 L 10,0 L 10,10 L 0,10 Z",
                       "M 0,0 L 0,10 L 10,10 L 10,0 Z");
}

TEST_F(ComparePathsRegressionTest, AcceptsReindexedClosedPath)
{
    expect_paths_match("M 0,0 L 10,0 L 10,10 L 0,10 Z",
                       "M 10,10 L 0,10 L 0,0 L 10,0 Z");
}

TEST_F(ComparePathsRegressionTest, AcceptsReversedOpenPath)
{
    expect_paths_match("M 0,0 C 2,1 8,1 10,0",
                       "M 10,0 C 8,1 2,1 0,0");
}

