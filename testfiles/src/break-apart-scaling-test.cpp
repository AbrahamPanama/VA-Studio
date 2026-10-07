// SPDX-License-Identifier: GPL-2.0-or-later
// BUG-029 exact pre-fix oracle and native Break Apart scaling regressions.
// Frozen containment code derived from geom-pathstroke.cpp:
// Copyright (C) 2014-2015 Authors; see that file for original authors.
// Copyright (C) 2026 VA Studio authors.
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <sstream>
#include <tuple>
#include <gtest/gtest.h>
#include <libxml/c14n.h>
#include <libxml/parser.h>
#include <2geom/path-intersection.h>
#include <2geom/svg-path-parser.h>
#include <2geom/svg-path-writer.h>
#include <2geom/sweeper.h>

#include "document-undo.h"
#include "document.h"
#include "helper/geom-pathstroke.h"
#include "helper/geom.h"
#include "inkscape.h"
#include "object/object-set.h"
#include "object/sp-item.h"
#include "object/sp-lpe-item.h"
#include "object/sp-path.h"
#include "object/sp-root.h"
#include "svg/svg.h"
#include "util/treeify.h"
#include "xml/node.h"
#include "xml/repr.h"
using namespace Inkscape;
namespace Inkscape::detail {
void set_break_apart_parallel_pairs_threshold_for_testing(std::size_t);
void set_break_apart_parallel_throw_event_for_testing(std::size_t);
std::vector<std::vector<int>> break_apart_containment_for_testing(Geom::PathVector const &);
}
// Frozen pre-fix BUG-029 geometry oracle, captured before production changes.
// Keep independent of the optimized containment implementation.
namespace baseline029 {
using namespace Inkscape;
Inkscape::Util::TreeifyResult baseline_treeify(int N, std::function<bool(int, int)> const &contains)
{
    // Todo: (C++23) Refactor away to a recursive lambda.
    class Treeifier
    {
    public:
        Treeifier(int N, std::function<bool(int, int)> const &contains)
            : N{N}
            , contains{contains}
            , data(N)
        {
            for (int i = 0; i < N; i++) {
                for (int j = 0; j < N; j++) {
                    if (j != i && contains(i, j)) {
                        data[j].num_containers++;
                        data[i].contained.emplace_back(j);
                    }
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

        Inkscape::Util::TreeifyResult moveResult() { return std::move(result); }

    private:
        // Input
        int N{};
        std::function<bool(int, int)> const &contains;

        // State
        struct Data
        {
            int num_containers = 0;
            std::vector<int> contained;
        };
        std::vector<Data> data;

        // Output
        Inkscape::Util::TreeifyResult result;

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

    return Treeifier(N, contains).moveResult();
}

static std::optional<Geom::Point> find_interior_point(Geom::Path const &path, FillRule fill_rule,
                                                      std::default_random_engine &gen)
{
    auto ran = [&] { return std::uniform_real_distribution()(gen); };

    auto const bounds = path.boundsFast();
    if (!bounds) {
        return {};
    }

    constexpr int max_iterations = 10; // arbitrary cutoff, typically one iteration needed
    for (int i = 0; i < max_iterations; i++) {
        // Pick a random horizontal line through the path.
        auto const y = Geom::lerp(ran(), bounds->top(), bounds->bottom());
        auto const line = Geom::LineSegment{Geom::Point{bounds->left() - bounds->width(), y},
                                            Geom::Point{bounds->right() + bounds->width(), y}};

        // Intersect the line with the path, recording the intersection times on the line.
        std::vector<double> times;
        for (auto const &curve : path) {
            for (auto const &intersection : line.intersect(curve)) {
                times.emplace_back(intersection.first);
            }
        }

        // Interior-test the points exactly halfway between intersections.
        for (int j = 1; j < times.size(); j++) {
            auto const t = (times[j - 1] + times[j]) / 2;
            auto const p = line.pointAt(t);
            if (is_point_inside(fill_rule, path.winding(p))) {
                return p;
            }
        }
    }

    return {};
}

namespace {

/**
 * Given a pathvector @a pathv and fill rule @a fill_rule, compute pathv_fully_contains(pathv[i], pathv[j], fill_rule)
 * for all possible i != j. This class must be used with the Geom::Sweeper API to actually compute results. The results
 * are then available by calling contains(i, j);
 */
class PathContainmentSweeper
{
public:
    using ItemIterator = Geom::PathVector::const_iterator;

    PathContainmentSweeper(Geom::PathVector const &pathv, FillRule fill_rule, double precision = Geom::EPSILON)
        : _pathv{pathv}
        , _fill_rule{fill_rule}
        , _precision{precision}
        , _contains(_pathv.size() * _pathv.size(), false) // allocate space for two-dimensional array
    {}

    Geom::PathVector const &items() const { return _pathv; }

    Geom::Interval itemBounds(ItemIterator path) const
    {
        auto const r = path->boundsFast();
        return r ? (*r)[Geom::X] : Geom::Interval{};
    }

    void addActiveItem(ItemIterator incoming)
    {
        for (auto const &path : _active) {
            _checkPair(path, incoming);
            _checkPair(incoming, path);
        }
        _active.push_back(incoming);
    }

    void removeActiveItem(ItemIterator to_remove)
    {
        auto const it = std::find(_active.begin(), _active.end(), to_remove);
        std::swap(*it, _active.back());
        _active.pop_back();
    }

    //// Return the value of pathv_fully_contains(pathv[i], pathv[j], fill_rule).
    bool contains(int i, int j) const { return _contains[index(i, j)]; }

private:
    Geom::PathVector const &_pathv;
    FillRule const _fill_rule;
    double const _precision;

    std::vector<ItemIterator> _active;
    std::vector<bool> _contains;

    int index(int i, int j) const { return i * _pathv.size() + j; }

    void _checkPair(ItemIterator a, ItemIterator b)
    {
        if (pathv_fully_contains(*a, *b, _fill_rule)) {
            auto const ia = std::distance(_pathv.begin(), a);
            auto const ib = std::distance(_pathv.begin(), b);
            _contains[index(ia, ib)] = true;
        }
    }
};

/**
 * Given a pathvector and a tree structure on its subpaths representing how they are nested,
 * split the pathvector into its connected components when filled with the given fill rule.
 */
class PathContainmentTraverser
{
public:
    PathContainmentTraverser(Geom::PathVector &paths, Util::TreeifyResult const &tree, FillRule fill_rule)
        : _paths{paths}
        , _tree{tree}
        , _fill_rule{fill_rule}
    {
        while (_pos < tree.preorder.size()) {
            _visit(nullptr, 0, nullptr);
        }
    }

    std::vector<Geom::PathVector> &&moveResult() { return std::move(_result); }

private:
    // Input
    Geom::PathVector &_paths;
    Util::TreeifyResult const &_tree;
    FillRule const _fill_rule;

    // State
    std::default_random_engine _gen{std::random_device()()};
    int _pos = 0;

    // Output
    std::vector<Geom::PathVector> _result;

    void _visit(Geom::Path const *parent, int winding, Geom::PathVector *component)
    {
        // Visit the next node in the preorder traversal.
        int const x = _tree.preorder[_pos];
        _pos++;

        // Determine winding number at paths[x] of its parents in the tree.
        // Skip the computation for fill_justDont, as it would be unused.
        if (parent && _fill_rule != fill_justDont) {
            if (auto const p = find_interior_point(_paths[x], _fill_rule, _gen)) {
                winding += parent->winding(*p);
            }
        }

        // Determine if paths[x] is inside a hole. If so, we start a new component.
        bool const boundary = !is_point_inside(_fill_rule, winding);

        std::optional<Geom::PathVector> pathv;
        if (boundary) {
            pathv.emplace();
            component = &*pathv;
        }

        assert(component);
        component->push_back(std::move(_paths[x]));

        for (int i = 0; i < _tree.num_children[x]; i++) {
            _visit(&_paths[x], winding, component);
        }

        if (boundary) {
            _result.emplace_back(std::move(*pathv));
        }
    }
};

} // namespace

std::vector<Geom::PathVector> split_non_intersecting_paths(Geom::PathVector &&paths, FillRule fill_rule)
{
    auto path_containment = PathContainmentSweeper{paths, fill_nonZero};
    Geom::Sweeper{path_containment}.process();

    auto const tree = baseline_treeify(paths.size(), [&](int i, int j) { return path_containment.contains(i, j); });

    return PathContainmentTraverser(paths, tree, fill_rule).moveResult();
}

} // namespace baseline029

namespace {
using Clock = std::chrono::steady_clock;
double seconds(Clock::time_point t)
{
    return std::chrono::duration<double>(Clock::now() - t).count();
}
std::string fixture(std::string const &kind, int nodes)
{
    std::ostringstream s;
    for (int i = 0, left = nodes; left > 0; ++i) {
        int count = std::min(left, 32);
        left -= count;
        int x = kind == "columns" ? 0 : (i % 100) * 128, y = (kind == "columns" ? i : i / 100) * 128;
        s << "M" << x << "," << y;
        for (int j = 0; j < count - 1; ++j) {
            switch (j / (count / 4)) {
                case 0:
                    s << " c1,1 3,-1 4,0";
                    break;
                case 1:
                    s << " c-1,1 1,3 0,4";
                    break;
                case 2:
                    s << " c-1,-1 -3,1 -4,0";
                    break;
                default:
                    s << " c1,-1 -1,-3 0,-4";
                    break;
            }
        }
        s << " z ";
    }
    return s.str();
}
std::string special()
{
    return "M0,0 H100 V100 H0 Z M10,10 V90 H90 V10 Z M20,20 H80 V80 H20 Z M200,0 H210 V10 H200 Z M210,0 H220 V10 H210 "
           "Z M200,0 H210 V10 H200 Z M205,-5 L215,5 205,15 Z M300,0 L305,5 M400,0 M410,0 L410,0 Z M500,0 C510,-20 "
           "530,20 540,0 A20,10 30 0,1 560,20 Z";
}
// Shared-tree rebuilds change only this generated provenance field. Keep raw
// oracle files intact and compare every other byte (including attribute order).
std::string normalize_build_stamp(std::string s)
{
    auto root = s.find("<svg:svg");
    if (root == std::string::npos)
        return s;
    auto root_end = s.find('>', root);
    std::string const key = "inkscape:version=\"";
    auto begin = s.find(key, root);
    if (begin < root_end) {
        begin += key.size();
        auto end = s.find('"', begin);
        if (end < root_end)
            s.replace(begin, end - begin, "BUILD_STAMP");
    }
    return s;
}
void oracle(std::string const &name, std::string const &data)
{
    auto dir = std::getenv("BUG029_ORACLE");
    if (!dir)
        return;
    std::filesystem::path p = std::filesystem::path(dir) / name;
    if (std::getenv("BUG029_RECORD")) {
        std::ofstream f(p);
        f << data;
        ASSERT_TRUE(f.good());
    } else {
        std::ifstream f(p);
        ASSERT_TRUE(f.good()) << p;
        std::ostringstream s;
        s << f.rdbuf();
        EXPECT_TRUE(normalize_build_stamp(s.str()) == normalize_build_stamp(data)) << name;
    }
}
std::string xml(SPDocument *d)
{
    return sp_repr_save_buf(d->getReprDoc()).raw();
}
// Native LPE replay can reorder attributes. Canonicalize only that ordering;
// retain every attribute value, child order, geometry and text node.
std::string canonical(std::string const &s)
{
    auto doc = xmlReadMemory(s.data(), s.size(), "oracle.svg", nullptr, XML_PARSE_NONET);
    if (!doc)
        return {};
    xmlChar *buffer = nullptr;
    int size = xmlC14NDocDumpMemory(doc, nullptr, XML_C14N_1_0, nullptr, 0, &buffer);
    std::string result = size >= 0 ? std::string(reinterpret_cast<char *>(buffer), size) : std::string{};
    xmlFree(buffer);
    xmlFreeDoc(doc);
    return result;
}
class BreakApartScaling : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        if (!Application::exists())
            Application::create(false);
    }
};
TEST_F(BreakApartScaling, GeometryOracle)
{
    int n = std::getenv("BUG029_N") ? std::atoi(std::getenv("BUG029_N")) : 10000;
    for (auto kind : {"grid", "columns", "special"}) {
        if (auto filter = std::getenv("BUG029_KIND"); filter && std::string(filter) != kind)
            continue;
        std::string d = std::string(kind) == "special" ? special() : fixture(kind, n);
        oracle(std::string(kind) + "-" + std::to_string(n) + ".path", d);
        auto original = Geom::parse_svg_path(d.c_str());
        std::map<std::pair<double, double>, std::vector<size_t>> indices;
        for (size_t i = 0; i < original.size(); ++i)
            indices[{original[i].initialPoint().x(), original[i].initialPoint().y()}].push_back(i);
        for (auto rule : {fill_justDont, fill_nonZero, fill_oddEven}) {
            if (auto filter = std::getenv("BUG029_RULE"); filter && std::atoi(filter) != rule)
                continue;
            auto copy = original;
            auto t = Clock::now();
            auto pieces = split_non_intersecting_paths(std::move(copy), rule);
            auto dt = seconds(t);
            if (n <= 10000 || std::string(kind) == "special") {
                auto reference = baseline029::split_non_intersecting_paths(Geom::PathVector{original}, rule);
                EXPECT_TRUE(pieces == reference) << kind << " rule=" << rule;
            } else {
                // These large grid/column fixtures have strictly disjoint 2D
                // bounds. The old tree therefore has roots 0..S-1 and emits
                // each unchanged path in input order, for every fill rule.
                ASSERT_EQ(pieces.size(), original.size());
                for (size_t i = 0; i < original.size(); ++i) {
                    ASSERT_EQ(pieces[i].size(), 1);
                    EXPECT_TRUE(pieces[i][0] == original[i]) << i;
                }
            }
            EXPECT_LE(dt, n <= 10000 ? 2.0 : n <= 100000 ? 10.0 : 40.0);
            std::ostringstream order;
            for (auto const &piece : pieces) {
                for (auto const &path : piece) {
                    bool found = false;
                    for (auto i : indices[{path.initialPoint().x(), path.initialPoint().y()}])
                        if (path == original[i]) {
                            order << i << ":" << path.closed() << ",";
                            found = true;
                            break;
                        }
                    ASSERT_TRUE(found) << "Piece changed exact curve geometry";
                }
                order << "\n";
            }
            oracle(std::string(kind) + "-" + std::to_string(n) + "-" + std::to_string(rule) + ".pieces", order.str());
            std::cout << "PHASE split kind=" << kind << " N=" << n << " S=" << original.size() << " rule=" << rule
                      << " seconds=" << dt << std::endl;
        }
    }
}
TEST_F(BreakApartScaling, DocumentOracle)
{
    int n = std::getenv("BUG029_N") ? std::atoi(std::getenv("BUG029_N")) : 10000;
    for (auto kind : {"grid", "columns", "special"})
        for (auto rule : {"nonzero", "evenodd"}) {
            if (auto filter = std::getenv("BUG029_KIND"); filter && std::string(filter) != kind)
                continue;
            if (auto filter = std::getenv("BUG029_DOC_RULE"); filter && std::string(filter) != rule)
                continue;
            auto d = std::string(kind) == "special" ? special() : fixture(kind, n);
            auto svg = std::string("<svg xmlns='http://www.w3.org/2000/svg' width='1000' height='1000'><g "
                                   "transform='translate(7,11)'><path id='before' d='M-2,-2 L-1,-1'/><path id='donor' "
                                   "transform='scale(2,3)' style='fill:#123456;fill-rule:") +
                       rule + ";stroke:#abcdef;stroke-width:0.5' d='" + d +
                       "'/><path id='after' d='M-4,-4 L-3,-3'/></g></svg>";
            auto doc = SPDocument::createNewDocFromMem(svg);
            ASSERT_TRUE(doc);
            doc->ensureUpToDate();
            DocumentUndo::setUndoSensitive(doc.get(), true);
            ObjectSet set(doc.get());
            set.add(cast<SPItem>(doc->getObjectById("donor")));
            std::vector<Geom::PathVector> reference;
            if (n <= 10000 || std::string(kind) == "special") {
                reference = baseline029::split_non_intersecting_paths(Geom::parse_svg_path(d.c_str()), fill_justDont);
            } else {
                // Analytic large-fixture oracle, justified by disjoint bounds.
                for (auto const &path : Geom::parse_svg_path(d.c_str())) {
                    reference.emplace_back(Geom::PathVector{path});
                }
            }
            auto before = xml(doc.get());
            auto t = Clock::now();
            set.breakApart(false, true, true);
            double action = seconds(t);
            t = Clock::now();
            doc->ensureUpToDate();
            double update = seconds(t);
            auto after = xml(doc.get());
            auto name = std::string(kind) + "-" + std::to_string(n) + "-" + rule;
            oracle(name + "-before.svg", before);
            oracle(name + "-after.svg", after);
            if (!reference.empty()) {
                auto *donor = doc->getObjectById("donor");
                ASSERT_TRUE(donor);
                EXPECT_EQ(std::string(donor->getRepr()->attribute("d")), sp_svg_write_path(reference.front()));
                size_t i = reference.size();
                for (auto node = donor->getRepr()->parent()->firstChild(); node; node = node->next()) {
                    auto id = node->attribute("id");
                    if (id && (std::string(id) == "before" || std::string(id) == "after"))
                        continue;
                    ASSERT_GT(i, 0);
                    --i;
                    ASSERT_NE(node->attribute("d"), nullptr);
                    EXPECT_EQ(std::string(node->attribute("d")), sp_svg_write_path(reference[i]));
                    auto *piece = cast<SPItem>(doc->getObjectByRepr(node));
                    ASSERT_TRUE(piece);
                    EXPECT_TRUE(piece->transform == Geom::Affine(2, 0, 0, 3, 0, 0));
                }
                EXPECT_EQ(i, 0);
            }
            t = Clock::now();
            ASSERT_TRUE(DocumentUndo::undo(doc.get()));
            double undo = seconds(t);
            EXPECT_TRUE(xml(doc.get()) == before) << name;
            t = Clock::now();
            ASSERT_TRUE(DocumentUndo::redo(doc.get()));
            double redo = seconds(t);
            EXPECT_TRUE(xml(doc.get()) == after) << name;
            auto reopened = SPDocument::createNewDocFromMem(after);
            ASSERT_TRUE(reopened);
            reopened->ensureUpToDate();
            EXPECT_TRUE(xml(reopened.get()) == after) << name;
            double budget = n <= 10000 ? 2.0 : n <= 100000 ? 10.0 : 40.0;
            EXPECT_LE(action, budget);
            EXPECT_LE(undo, budget);
            EXPECT_LE(redo, budget);
            std::cout << "PHASE document kind=" << kind << " N=" << n << " rule=" << rule << " action=" << action
                      << " update=" << update << " undo_including_update=" << undo << " redo_including_update=" << redo
                      << std::endl;
        }
}
} // namespace

TEST_F(BreakApartScaling, LpeReferencesAndSplit)
{
    for (bool effect : {false, true})
        for (bool overlapping : {false, true}) {
            std::string d = "M0,0 C10,-10 20,10 30,0 L30,30 L0,30 Z M100,0 C110,-10 120,10 130,0 L130,30 L100,30 Z";
            std::string svg =
                "<svg xmlns='http://www.w3.org/2000/svg' xmlns:inkscape='http://www.inkscape.org/namespaces/inkscape' "
                "xmlns:xlink='http://www.w3.org/1999/xlink' width='400' height='400'><defs><inkscape:path-effect "
                "effect='spiro' id='fx' is_visible='true' lpeversion='1'/></defs><g transform='translate(7,11)'><rect "
                "id='before' x='-10' y='-10' width='1' height='1'/><path id='donor' transform='scale(2,3)' "
                "style='fill:#123456;fill-rule:evenodd;stroke:#abcdef;stroke-width:0.5' d='" +
                d + "'";
            if (effect)
                svg += " inkscape:path-effect='#fx' inkscape:original-d='" + d + "'";
            svg += "/><rect id='after' x='-20' y='-20' width='1' height='1'/></g><use id='ref' xlink:href='#donor' "
                   "x='2'/></svg>";
            auto doc = SPDocument::createNewDocFromMem(svg);
            ASSERT_TRUE(doc);
            DocumentUndo::setUndoSensitive(doc.get(), false);
            if (effect)
                sp_lpe_item_update_patheffect(cast<SPLPEItem>(doc->getObjectById("donor")), true, true);
            doc->ensureUpToDate();
            DocumentUndo::setUndoSensitive(doc.get(), true);
            ObjectSet set(doc.get());
            set.add(cast<SPItem>(doc->getObjectById("donor")));
            set.add(cast<SPItem>(doc->getObjectById("before")));
            auto before = xml(doc.get());
            ASSERT_FALSE(canonical(before).empty());
            set.breakApart(false, overlapping, true);
            doc->ensureUpToDate();
            auto after = xml(doc.get());
            auto name = std::string("lpe-") + std::to_string(effect) + "-overlapping-" + std::to_string(overlapping);
            oracle(name + "-before.svg", before);
            oracle(name + "-after.svg", after);
            ASSERT_TRUE(doc->getObjectById("before"));
            ASSERT_TRUE(doc->getObjectById("after"));
            ASSERT_TRUE(doc->getObjectById("donor"));
            ASSERT_TRUE(doc->getObjectById("ref"));
            ASSERT_TRUE(DocumentUndo::undo(doc.get()));
            oracle(name + "-undo.svg", xml(doc.get()));
            EXPECT_TRUE(canonical(xml(doc.get())) == canonical(before)) << name;
            ASSERT_TRUE(DocumentUndo::redo(doc.get()));
            oracle(name + "-redo.svg", xml(doc.get()));
            EXPECT_TRUE(canonical(xml(doc.get())) == canonical(after)) << name;
            auto reopened = SPDocument::createNewDocFromMem(after);
            ASSERT_TRUE(reopened);
            reopened->ensureUpToDate();
            EXPECT_TRUE(xml(reopened.get()) == after) << name;
        }
}

TEST_F(BreakApartScaling, LongAndEmptyOracle)
{
    int n = std::getenv("BUG029_N") ? std::atoi(std::getenv("BUG029_N")) : 10000;
    std::ostringstream d;
    d << "M0,0";
    for (int i = 1; i < n; ++i)
        d << " c1,1 2,-1 3,0";
    d << " Z";
    auto longpath = Geom::parse_svg_path(d.str().c_str());
    auto degenerate =
        Geom::parse_svg_path("M0,0 L0,0 Z M0,0 H10 V10 H0 Z M0,0 H10 V10 H0 Z M0,0 L10,0.000000001 10,10 0,10 Z");
    degenerate.push_back(Geom::Path{});
    degenerate.push_back(Geom::Path{Geom::Point{500, 500}});
    int c = 0;
    for (auto const &original : {Geom::PathVector{}, degenerate, longpath}) {
        for (auto rule : {fill_justDont, fill_nonZero, fill_oddEven}) {
            auto copy = original;
            auto t = Clock::now();
            auto pieces = split_non_intersecting_paths(std::move(copy), rule);
            auto dt = seconds(t);
            auto reference = baseline029::split_non_intersecting_paths(Geom::PathVector{original}, rule);
            EXPECT_TRUE(pieces == reference) << "case=" << c << " rule=" << rule;
            std::ostringstream result;
            for (auto const &piece : pieces) {
                for (auto const &path : piece) {
                    bool found = false;
                    for (size_t i = 0; i < original.size(); ++i)
                        if (path == original[i]) {
                            result << i << ":" << path.closed() << ",";
                            found = true;
                            break;
                        }
                    ASSERT_TRUE(found);
                }
                result << "\n";
            }
            oracle("long-empty-" + std::to_string(n) + "-" + std::to_string(c) + "-" + std::to_string(rule) + ".pieces",
                   result.str());
            std::cout << "PHASE long-empty case=" << c << " N=" << n << " rule=" << rule << " seconds=" << dt
                      << std::endl;
        }
        ++c;
    }
    oracle("long-" + std::to_string(n) + ".path", d.str());
}

TEST_F(BreakApartScaling, SparseContainmentOrderAndCycles)
{
    std::mt19937 rng(29);
    for (int n = 0; n <= 16; ++n) {
        for (int trial = 0; trial < 100; ++trial) {
            std::vector<std::vector<int>> edges(n);
            for (int i = 0; i < n; ++i) {
                for (int j = 0; j < n; ++j) {
                    if (i != j && rng() % 4 == 0)
                        edges[i].push_back(j);
                }
                std::shuffle(edges[i].begin(), edges[i].end(), rng);
            }
            auto reference = baseline029::baseline_treeify(
                n, [&](int i, int j) { return std::find(edges[i].begin(), edges[i].end(), j) != edges[i].end(); });
            auto result = Inkscape::Util::treeify(edges);
            EXPECT_EQ(result.preorder, reference.preorder) << n << ":" << trial;
            EXPECT_EQ(result.num_children, reference.num_children) << n << ":" << trial;
        }
    }
}

TEST_F(BreakApartScaling, ParallelContainmentPreservesExactComponents)
{
    auto run = [](std::size_t threshold) {
        detail::set_break_apart_parallel_pairs_threshold_for_testing(threshold);
        std::ostringstream data;
        for (int i = 0; i < 28; ++i) {
            auto const radius = 30.0 - i * 0.5;
            data << "M" << radius << ",0 A" << radius << "," << radius << " 0 1 0 " << -radius << ",0 A"
                 << radius << "," << radius << " 0 1 0 " << radius << ",0 Z ";
        }
        // Two circles touch at one point and are included in the same sweep.
        data << "M70,0 A10,10 0 1 0 50,0 A10,10 0 1 0 70,0 Z "
             << "M90,0 A10,10 0 1 0 70,0 A10,10 0 1 0 90,0 Z";
        auto paths = Geom::parse_svg_path(data.str().c_str());
        auto containment = detail::break_apart_containment_for_testing(paths);
        auto pieces = split_non_intersecting_paths(Geom::PathVector{paths}, fill_nonZero);
        std::vector<std::string> serialized;
        for (auto const &piece : pieces) serialized.push_back(Geom::write_svg_path(piece, 17));
        return std::tuple{std::move(containment), std::move(pieces), std::move(serialized)};
    };
    auto serial = run(std::numeric_limits<std::size_t>::max());
    auto parallel = run(0);
    detail::set_break_apart_parallel_pairs_threshold_for_testing(256);
    EXPECT_EQ(std::get<0>(parallel), std::get<0>(serial));
    EXPECT_EQ(std::get<1>(parallel), std::get<1>(serial));
    EXPECT_EQ(std::get<2>(parallel), std::get<2>(serial));
}

TEST_F(BreakApartScaling, ParallelContainmentExceptionReturnsToCaller)
{
    std::ostringstream data;
    for (int i = 0; i < 28; ++i) {
        auto const radius = 30.0 - i * 0.5;
        data << "M" << radius << ",0 A" << radius << "," << radius << " 0 1 0 " << -radius << ",0 A"
             << radius << "," << radius << " 0 1 0 " << radius << ",0 Z ";
    }
    auto paths = Geom::parse_svg_path(data.str().c_str());
    detail::set_break_apart_parallel_pairs_threshold_for_testing(0);
    detail::set_break_apart_parallel_throw_event_for_testing(0);
    EXPECT_THROW(detail::break_apart_containment_for_testing(paths), std::runtime_error);
    detail::set_break_apart_parallel_throw_event_for_testing(std::numeric_limits<std::size_t>::max());
    detail::set_break_apart_parallel_pairs_threshold_for_testing(256);
}
