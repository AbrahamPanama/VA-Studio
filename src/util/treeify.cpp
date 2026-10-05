// SPDX-License-Identifier: GPL-2.0-or-later
#include "treeify.h"

#include <cassert>
#include <algorithm>

namespace Inkscape::Util {

TreeifyResult treeify(int N, std::function<bool(int, int)> const &contains)
{
    std::vector<std::vector<int>> edges(N);
    for (int i = 0; i < N; i++) {
        for (int j = 0; j < N; j++) {
            if (j != i && contains(i, j)) edges[i].emplace_back(j);
        }
    }
    return treeify(std::move(edges));
}

TreeifyResult treeify(std::vector<std::vector<int>> edges)
{
    // Todo: (C++23) Refactor away to a recursive lambda.
    class Treeifier
    {
    public:
        Treeifier(std::vector<std::vector<int>> edges)
            : N{static_cast<int>(edges.size())}
            , data(N)
        {
            for (int i = 0; i < N; i++) {
                data[i].contained = std::move(edges[i]);
                // Preserve the ascending visit order of the all-pairs API,
                // independently of spatial candidate enumeration order.
                std::sort(data[i].contained.begin(), data[i].contained.end());
                for (auto j : data[i].contained) {
                    assert(j >= 0 && j < N && j != i);
                    data[j].num_containers++;
                }
            }

            result.num_children.resize(N);

            for (int i = 0; i < N; i++) {
                if (data[i].num_containers == 0) {
                    visit(i);
                }
            }

            for (int i = 0; i < N; i++) {
                if (data[i].num_containers != -1) {
                    result.preorder.emplace_back(i);
                }
            }

            assert(result.preorder.size() == N);
        }

        TreeifyResult moveResult() { return std::move(result); }

    private:
        // Input
        int N{};

        // State
        struct Data
        {
            int num_containers = 0;
            std::vector<int> contained;
        };
        std::vector<Data> data;

        // Output
        TreeifyResult result;

        void visit(int i)
        {
            result.preorder.emplace_back(i);

            for (auto j : data[i].contained) {
                data[j].num_containers--;
            }

            for (auto j : data[i].contained) {
                if (data[j].num_containers == 0) {
                    result.num_children[i]++;
                    visit(j);
                }
            }

            data[i].num_containers = -1;
        }
    };

    return Treeifier(std::move(edges)).moveResult();
}

} // namespace Inkscape::Util

/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0)(case-label . +))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:fileencoding=utf-8:textwidth=99 :
