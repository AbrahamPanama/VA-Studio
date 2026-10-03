// SPDX-License-Identifier: LGPL-2.1-or-later OR GPL-2.0-or-later
/*
 * Authors:
 *   Vinícius dos Santos Oliveira <vini.ipsmaker@gmail.com>
 *
 * Copyright (C) 2013 Authors
 */

#include "depixelize.h"

#include <algorithm>

#include "colorspace.h"
#include "homogeneoussplines.h"
#include "splines-impl.h"
#include "iterator.h"

#ifdef LIBDEPIXELIZE_WITH_PROFILING
#include <chrono>
#include <iostream>
using Clock = std::chrono::steady_clock;
#endif // LIBDEPIXELIZE_WITH_PROFILING

namespace Depixelize {

using Precision = Geom::Coord;

namespace Heuristics {

static int curves(const PixelGraph &graph, PixelGraph::const_iterator a, PixelGraph::const_iterator b);
static bool islands(PixelGraph::const_iterator a, PixelGraph::const_iterator b);

struct SparsePixels
{
    enum Diagonal {
        /**
         * From (first) the top left corner to (second) the bottom right.
         */
        MAIN_DIAGONAL      = 0,
        /**
         * From (first) the top right to (second) the bottom left.
         */
        SECONDARY_DIAGONAL = 1
    };

    using Edge = std::pair<PixelGraph::const_iterator, PixelGraph::const_iterator>;
    using EdgeWeight = std::pair<Edge, int>;

    void operator()(const PixelGraph &graph, unsigned radius);

    static bool similar_colors(PixelGraph::const_iterator n,
                               const uint8_t (&a)[4], const uint8_t (&b)[4]);

    /*
     * Precondition: Must be filled according to Diagonal enum.
     */
    EdgeWeight diagonals[2];
};

} // namespace Heuristics

template<class T, bool adjust_splines>
static SimplifiedVoronoi<T, adjust_splines>
_voronoi(const Image &buf,
         const Options &options);

static void _disconnect_neighbors_with_dissimilar_colors(PixelGraph &graph);

// here, T/template is only used as an easy way to not expose internal symbols
template<class T>
static void _remove_crossing_edges_safe(T &container);

template<class T>
static void _remove_crossing_edges_unsafe(PixelGraph &graph, T &edges,
                                          const Options &options);

Splines to_voronoi(const Image &buf,
                   const Options &options)
{
#ifdef LIBDEPIXELIZE_WITH_PROFILING
    SimplifiedVoronoi<Precision, false> voronoi = _voronoi<Precision, false>(buf, options);

    Clock::time_point profiling_info[2];
    profiling_info[0] = Clock::now();

    Splines ret(voronoi);

    profiling_info[1] =  Clock::now();
    std::cerr << "Depixelize::Splines construction time: "
              << std::chrono::duration_cast<std::chrono::microseconds>(profiling_info[1] - profiling_info[0])
              << std::endl;

    return ret;
#else // LIBDEPIXELIZE_WITH_PROFILING
    return Splines(_voronoi<Precision, false>(buf, options));
#endif // LIBDEPIXELIZE_WITH_PROFILING
}

Splines to_grouped_voronoi(const Image &buf,
                           const Options &options)
{
#ifdef LIBDEPIXELIZE_WITH_PROFILING
    SimplifiedVoronoi<Precision, false> voronoi = _voronoi<Precision, false>(buf, options);

    Clock::time_point profiling_info[2];
    profiling_info[0] = Clock::now();

    HomogeneousSplines<Precision> splines(voronoi);

#else // LIBDEPIXELIZE_WITH_PROFILING
    HomogeneousSplines<Precision> splines(_voronoi<Precision, false>(buf, options));
#endif // LIBDEPIXELIZE_WITH_PROFILING

#ifdef LIBDEPIXELIZE_WITH_PROFILING
    profiling_info[1] = Clock::now();
    std::cerr << "Depixelize::HomogeneousSplines<" << typeid(Precision).name()
              << ">(Depixelize::SimplifiedVoronoi<" << typeid(Precision).name()
              << ",false>) construction time: "
              << std::chrono::duration_cast<std::chrono::microseconds>(profiling_info[1] - profiling_info[0])
              << std::endl;
    profiling_info[0] = Clock::now();
#endif // LIBDEPIXELIZE_WITH_PROFILING

    for (auto &spline : splines) {
        for (auto &pt : spline.vertices) {
            pt.smooth = false;
        }
        for (auto &hole : spline.holes) {
            for (auto &pt : hole) {
                pt.smooth = false;
            }
        }
    }

#ifdef LIBDEPIXELIZE_WITH_PROFILING
    profiling_info[1] = Clock::now();
    std::cerr << "Depixelize::Kopf2011::to_grouped_voronoi internal work time: "
              << std::chrono::duration_cast<std::chrono::microseconds>(profiling_info[1] - profiling_info[0])
              << std::endl;
    profiling_info[0] = Clock::now();

    Splines ret(splines, false);

    profiling_info[1] = Clock::now();
    std::cerr << "Depixelize::Splines construction time: "
        << std::chrono::duration_cast<std::chrono::microseconds>(profiling_info[1] - profiling_info[0])
        << std::endl;

    return ret;
#else // LIBDEPIXELIZE_WITH_PROFILING
    return Splines(splines, false);
#endif // LIBDEPIXELIZE_WITH_PROFILING
}

Splines to_splines(const Image &buf,
                   const Options &options)
{
#ifdef LIBDEPIXELIZE_WITH_PROFILING
    SimplifiedVoronoi<Precision, true> voronoi
        = _voronoi<Precision, true>(buf, options);

    Clock::time_point profiling_info[2];
    profiling_info[0] = Clock::now();

    HomogeneousSplines<Precision> splines(voronoi);

    profiling_info[1] = Clock::now();
    std::cerr << "Depixelize::HomogeneousSplines<" << typeid(Precision).name()
        << "> construction time: "
        << std::chrono::duration_cast<std::chrono::microseconds>(profiling_info[1] - profiling_info[0])
        << std::endl;

    profiling_info[0] = Clock::now();

    Splines ret(splines, options.optimize);

    profiling_info[1] = Clock::now();
    std::cerr << "Depixelize::Splines construction time: "
        << std::chrono::duration_cast<std::chrono::microseconds>(profiling_info[1] - profiling_info[0])
        << std::endl;

    return ret;
#else // LIBDEPIXELIZE_WITH_PROFILING
    HomogeneousSplines<Precision> splines(_voronoi<Precision, true>(buf, options));
    return Splines(splines, options.optimize);
#endif // LIBDEPIXELIZE_WITH_PROFILING
}

template<class T, bool adjust_splines>
SimplifiedVoronoi<T, adjust_splines>
_voronoi(const Image &buf,
         const Options &options)
{
#ifdef LIBDEPIXELIZE_WITH_PROFILING
    Clock::time_point profiling_info[2];
    profiling_info[0] = Clock::now();
#endif // LIBDEPIXELIZE_WITH_PROFILING

    PixelGraph graph(buf);

#ifdef LIBDEPIXELIZE_WITH_PROFILING
    profiling_info[1] = Clock::now();
    std::cerr << "Depixelize::PixelGraph creation time: "
              << std::chrono::duration_cast<std::chrono::microseconds>(profiling_info[1] - profiling_info[0]) << std::endl;
#endif // LIBDEPIXELIZE_WITH_PROFILING

    assert(graph.width() > 0);
    assert(graph.height() > 0);

#ifndef NDEBUG
    graph.checkConsistency();
#endif

#ifdef LIBDEPIXELIZE_WITH_PROFILING
    profiling_info[0] = Clock::now();
#endif // LIBDEPIXELIZE_WITH_PROFILING

    // This step could be part of the initialization of PixelGraph
    // and decrease the necessary number of passes
    graph.connectAllNeighbors();

#ifdef LIBDEPIXELIZE_WITH_PROFILING
    profiling_info[1] = Clock::now();
    std::cerr << "Depixelize::PixelGraph::connectAllNeighbors() time: "
              << std::chrono::duration_cast<std::chrono::microseconds>(profiling_info[1] - profiling_info[0]) << std::endl;
#endif // LIBDEPIXELIZE_WITH_PROFILING

#ifndef NDEBUG
    graph.checkConsistency();
#endif

#ifdef LIBDEPIXELIZE_WITH_PROFILING
    profiling_info[0] = Clock::now();
#endif // LIBDEPIXELIZE_WITH_PROFILING

    // This step can't be part of PixelGraph initilization without adding some
    // cache misses due to random access patterns that might be injected
    _disconnect_neighbors_with_dissimilar_colors(graph);

#ifdef LIBDEPIXELIZE_WITH_PROFILING
    profiling_info[1] = Clock::now();
    std::cerr << "Depixelize::Kopf2011::"
        "_disconnect_neighbors_with_dissimilar_colors(Depixelize::PixelGraph) time: "
              << std::chrono::duration_cast<std::chrono::microseconds>(profiling_info[1] - profiling_info[0]) << std::endl;
#endif // LIBDEPIXELIZE_WITH_PROFILING

#ifndef NDEBUG
    graph.checkConsistency();
#endif

#ifdef LIBDEPIXELIZE_WITH_PROFILING
    profiling_info[0] = Clock::now();
#endif // LIBDEPIXELIZE_WITH_PROFILING

    {
        // edges_safe and edges_unsafe must be executed in separate.
        // Otherwise, there will be colateral effects due to misassumption about
        // the data being read.
        PixelGraph::EdgePairContainer edges = graph.crossingEdges();

#ifdef LIBDEPIXELIZE_WITH_PROFILING
    profiling_info[1] = Clock::now();
    std::cerr << "Depixelize::PixelGraph::crossingEdges() time: "
              << std::chrono::duration_cast<std::chrono::microseconds>(profiling_info[1] - profiling_info[0]) << std::endl;
    profiling_info[0] = Clock::now();
#endif // LIBDEPIXELIZE_WITH_PROFILING

        _remove_crossing_edges_safe(edges);

#ifdef LIBDEPIXELIZE_WITH_PROFILING
        profiling_info[1] = Clock::now();
        std::cerr << "Depixelize::Kopf2011::_remove_crossing_edges_safe"
            "(Depixelize::PixelGraph) time: "
                  << std::chrono::duration_cast<std::chrono::microseconds>(profiling_info[1] - profiling_info[0])
                  << std::endl;
#endif // LIBDEPIXELIZE_WITH_PROFILING

#ifndef NDEBUG
        graph.checkConsistency();
#endif

#ifdef LIBDEPIXELIZE_WITH_PROFILING
        profiling_info[0] = Clock::now();
#endif // LIBDEPIXELIZE_WITH_PROFILING

        _remove_crossing_edges_unsafe(graph, edges, options);
    }

#ifdef LIBDEPIXELIZE_WITH_PROFILING
    profiling_info[1] = Clock::now();
    std::cerr << "Depixelize::Kopf2011::_remove_crossing_edges_unsafe"
        "(Depixelize::PixelGraph) time: "
              << std::chrono::duration_cast<std::chrono::microseconds>(profiling_info[1] - profiling_info[0]) << std::endl;
#endif // LIBDEPIXELIZE_WITH_PROFILING

#ifndef NDEBUG
    graph.checkConsistency();
#endif

    assert(graph.crossingEdges().size() == 0);

#ifdef LIBDEPIXELIZE_WITH_PROFILING
    profiling_info[0] = Clock::now();

    SimplifiedVoronoi<T, adjust_splines> ret(graph);

    profiling_info[1] = Clock::now();
    std::cerr << "Depixelize::SimplifiedVoronoi<" << typeid(T).name() << ','
              << (adjust_splines ? "true" : "false")
              << ">(Depixelize::PixelGraph) construction time: "
              << std::chrono::duration_cast<std::chrono::microseconds>(profiling_info[1] - profiling_info[0]) << std::endl;

    return ret;
#else // LIBDEPIXELIZE_WITH_PROFILING
    return SimplifiedVoronoi<T, adjust_splines>(graph);
#endif // LIBDEPIXELIZE_WITH_PROFILING
}

// TODO: move this function (plus connectAllNeighbors) to PixelGraph constructor
inline void _disconnect_neighbors_with_dissimilar_colors(PixelGraph &graph)
{
    using ColorSpace::similar_colors;
    for ( PixelGraph::iterator it = graph.begin(), end = graph.end() ; it != end
              ; ++it ) {
        if ( it->adj.top )
            it->adj.top = similar_colors(it->rgba, (it - graph.width())->rgba);
        if ( it->adj.topright ) {
            it->adj.topright
                = similar_colors(it->rgba, (it - graph.width() + 1)->rgba);
        }
        if ( it->adj.right )
            it->adj.right = similar_colors(it->rgba, (it + 1)->rgba);
        if ( it->adj.bottomright ) {
            it->adj.bottomright
                = similar_colors(it->rgba, (it + graph.width() + 1)->rgba);
        }
        if ( it->adj.bottom ) {
            it->adj.bottom
                = similar_colors(it->rgba, (it + graph.width())->rgba);
        }
        if ( it->adj.bottomleft ) {
            it->adj.bottomleft
                = similar_colors(it->rgba, (it + graph.width() - 1)->rgba);
        }
        if ( it->adj.left )
            it->adj.left = similar_colors(it->rgba, (it - 1)->rgba);
        if ( it->adj.topleft ) {
            it->adj.topleft = similar_colors(it->rgba,
                                             (it - graph.width() - 1)->rgba);
        }
    }
}

/**
 * This method removes crossing edges if the 2x2 block is fully connected.
 *
 * In this case the two diagonal connections can be safely removed without
 * affecting the final result.
 */
template<class T>
void _remove_crossing_edges_safe(T &container)
{
    for ( typename T::reverse_iterator it = container.rbegin(),
              end = container.rend() ; it != end ; ) {
        /* A | B
           --+--
           C | D */
        PixelGraph::iterator a = it->first.first;
        PixelGraph::iterator b = it->second.first;
        PixelGraph::iterator c = it->second.second;
        PixelGraph::iterator d = it->first.second;

        if ( !a->adj.right || !a->adj.bottom || !b->adj.bottom
             || !c->adj.right ) {
            ++it;
            continue;
        }

        // main diagonal
        a->adj.bottomright = 0;
        d->adj.topleft = 0;

        // secondary diagonal
        b->adj.bottomleft = 0;
        c->adj.topright = 0;

        // base iterator is always past one
        typename T::iterator current = --(it.base());
        ++it;
        container.erase(current);
    }
}

/**
 * This method removes crossing edges using the heuristics.
 */
template<class T>
void _remove_crossing_edges_unsafe(PixelGraph &graph, T &edges,
                                   const Options &options)
{
    std::vector< std::pair<int, int> > weights(edges.size(),
                                               std::make_pair(0, 0));

    // Compute weights
    for ( typename T::size_type i = 0 ; i != edges.size() ; ++i ) {
        /* A | B
           --+--
           C | D */
        PixelGraph::iterator a = edges[i].first.first;
        PixelGraph::iterator b = edges[i].second.first;
        PixelGraph::iterator c = edges[i].second.second;
        PixelGraph::iterator d = edges[i].first.second;

        // Curves heuristic
        weights[i].first += Heuristics::curves(graph, a, d)
                            * options.curves_multiplier;
        weights[i].second += Heuristics::curves(graph, b, c)
                             * options.curves_multiplier;

        // Islands heuristic
        weights[i].first += Heuristics::islands(a, d) * options.islands_weight;
        weights[i].second += Heuristics::islands(b, c) * options.islands_weight;

        // Sparse pixels heuristic
        using Heuristics::SparsePixels;
        SparsePixels sparse_pixels;

        sparse_pixels.diagonals[SparsePixels::MAIN_DIAGONAL].first
            = edges[i].first;
        sparse_pixels.diagonals[SparsePixels::SECONDARY_DIAGONAL].first
            = edges[i].second;

        sparse_pixels(graph, options.sparse_pixels_radius);

        weights[i].first
            += sparse_pixels.diagonals[SparsePixels::MAIN_DIAGONAL].second
               * options.sparse_pixels_multiplier;
        weights[i].second
            += sparse_pixels.diagonals[SparsePixels::SECONDARY_DIAGONAL].second
               * options.sparse_pixels_multiplier;
    }

    // Remove edges with lower weight
    for ( typename T::size_type i = 0 ; i != edges.size() ; ++i ) {
        /* A | B
           --+--
           C | D */
        PixelGraph::iterator a = edges[i].first.first;
        PixelGraph::iterator b = edges[i].second.first;
        PixelGraph::iterator c = edges[i].second.second;
        PixelGraph::iterator d = edges[i].first.second;

        if ( weights[i].first > weights[i].second ) {
            b->adj.bottomleft = 0;
            c->adj.topright = 0;
        } else if ( weights[i].first < weights[i].second ) {
            a->adj.bottomright = 0;
            d->adj.topleft = 0;
        } else {
            a->adj.bottomright = 0;
            b->adj.bottomleft = 0;
            c->adj.topright = 0;
            d->adj.topleft = 0;
        }
    }

    edges.clear();
}

inline int Heuristics::curves(const PixelGraph &graph,
                              PixelGraph::const_iterator a,
                              PixelGraph::const_iterator b)
{
    int count = 1;
    ToPtr<PixelGraph::Node> to_ptr;
    ToIter<PixelGraph::Node> to_iter(graph.begin());

    // b -> a
    // and then a -> b
    for ( int i = 0 ; i != 2 ; ++i ) {
        PixelGraph::const_iterator it = i ? a : b;
        PixelGraph::const_iterator prev = i ? b : a;
        int local_count = 0;

        // Used to avoid inifinite loops in circular-like edges
        const PixelGraph::const_iterator initial = it;

        while ( it->adjsize() == 2 ) {
            ++local_count;

            // Iterate to next
            {
                // There are only two values that won't be zero'ed
                // and one of them has the same value of prev
                uintptr_t aux = it->adj.top
                       * uintptr_t(to_ptr(graph.nodeTop(it)))
                    + it->adj.topright
                       * uintptr_t(to_ptr(graph.nodeTopRight(it)))
                    + it->adj.right
                       * uintptr_t(to_ptr(graph.nodeRight(it)))
                    + it->adj.bottomright
                       * uintptr_t(to_ptr(graph.nodeBottomRight(it)))
                    + it->adj.bottom
                       * uintptr_t(to_ptr(graph.nodeBottom(it)))
                    + it->adj.bottomleft
                       * uintptr_t(to_ptr(graph.nodeBottomLeft(it)))
                    + it->adj.left
                       * uintptr_t(to_ptr(graph.nodeLeft(it)))
                    + it->adj.topleft
                       * uintptr_t(to_ptr(graph.nodeTopLeft(it)))
                    - uintptr_t(to_ptr(prev));
                prev = it;
                it = to_iter(reinterpret_cast<PixelGraph::Node const*>(aux));
            }

            // Break infinite loops
            if ( it == initial )
                return local_count;
        }
        count += local_count;
    }

    return count;
}

inline void Heuristics::SparsePixels::operator ()(const PixelGraph &graph,
                                                  unsigned radius)
{
    if ( !graph.width() || !graph.height() )
        return;

    // Clear weights
    for ( int i = 0 ; i != 2 ; ++i )
        diagonals[i].second = 0;

    if ( !radius )
        return;

    // Fix radius/bounds
    {
        unsigned x = graph.toX(diagonals[MAIN_DIAGONAL].first.first);
        unsigned y = graph.toY(diagonals[MAIN_DIAGONAL].first.first);
        unsigned displace = radius - 1;

        {
            unsigned minor = std::min(x, y);

            if ( displace > minor ) {
                displace = minor;
                radius = displace + 1;
            }
        }

        displace = radius;

        if ( x + displace >= unsigned(graph.width()) ) {
            displace = unsigned(graph.width()) - x - 1;
            radius = displace;
        }

        if ( y + displace >= unsigned(graph.height()) ) {
            displace = unsigned(graph.height()) - y - 1;
            radius = displace;
        }
    }

    if ( !radius )
        return;

    // Iterate over nodes and count them
    {
        PixelGraph::const_iterator it = diagonals[MAIN_DIAGONAL].first.first;
        for ( unsigned i = radius - 1 ; i ; --i )
            it = graph.nodeTopLeft(it);

        bool invert = false;
        for ( unsigned i = 0 ; i != 2 * radius ; ++i ) {
            for ( unsigned j = 0 ; j != 2 * radius ; ++j ) {
                for ( int k = 0 ; k != 2 ; ++k ) {
                    diagonals[k].second
                        += similar_colors(it, diagonals[k].first.first->rgba,
                                          diagonals[k].first.second->rgba);
                }
                it = invert ? graph.nodeLeft(it) : graph.nodeRight(it);
            }
            it = invert ? graph.nodeRight(it) : graph.nodeLeft(it);


            invert = !invert;
            it = graph.nodeBottom(it);
        }
    }

    int minor = std::min(diagonals[0].second, diagonals[1].second);
    for ( int i = 0 ; i != 2 ; ++i )
        diagonals[i].second -= minor;
    std::swap(diagonals[0].second, diagonals[1].second);
}

inline bool
Heuristics::SparsePixels::similar_colors(PixelGraph::const_iterator n,
                                         const uint8_t (&a)[4],
                                         const uint8_t (&b)[4])
{
    using ColorSpace::similar_colors;
    return similar_colors(n->rgba, a) || similar_colors(n->rgba, b);
}

inline bool Heuristics::islands(PixelGraph::const_iterator a,
                                PixelGraph::const_iterator b)
{
    return a->adjsize() == 1 || b->adjsize() == 1;
}

} // namespace Depixelize

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
