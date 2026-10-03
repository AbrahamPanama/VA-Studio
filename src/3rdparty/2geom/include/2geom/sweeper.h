// SPDX-License-Identifier: LGPL-2.1-only OR MPL-1.1
/** @file
 * @brief Class for implementing sweepline algorithms
 *//*
 * Authors:
 *   Krzysztof Kosiński <tweenk.pl@gmail.com>
 *
 * Copyright 2015 Authors
 */

#ifndef LIB2GEOM_SEEN_SWEEPER_H
#define LIB2GEOM_SEEN_SWEEPER_H

#include <2geom/coord.h>
#include <algorithm>
#include <vector>
#include <boost/intrusive/list.hpp>

namespace Geom {

// exposition only
template <typename Item>
class SweepVector {
public:
    typedef typename std::vector<Item>::const_iterator ItemIterator;

    SweepVector(std::vector<Item> const &v)
        : _items(v)
    {}

    std::vector<Item> const &items() { return _items; }
    Interval itemBounds(ItemIterator /*ii*/) { return Interval(); }

    void addActiveItem(ItemIterator /*ii*/) {}
    void removeActiveItem(ItemIterator /*ii*/) {}

private:
    std::vector<Item> const &_items;
};

/** @brief Generic sweepline algorithm.
 *
 * This class encapsulates an algorithm that sorts the objects according
 * to their bounds, then moves an imaginary line (sweepline) over those
 * bounds from left to right. Objects are added to the active list when
 * the line starts intersecting their bounds, and removed when it completely
 * passes over them.
 *
 * To use this, create a class that exposes the following methods:
 * - Range items() - returns a forward iterable range of items that will be swept.
 * - Interval itemBounds(iterator i) - given an iterator from the above range,
 *   compute the bounding interval of the referenced item in the direction of sweep.
 * - void addActiveItem(iterator i) - add an item to the active list.
 * - void removeActiveItem(iterator i) - remove an item from the active list.
 *
 * Create the object, then instantiate this template with the above class
 * as the template parameter, pass it the constructed object of the class,
 * and call the process() method.
 *
 * A good choice for the active list is a Boost intrusive list, which allows
 * you to get an iterator from a value in constant time.
 *
 * Look in path.cpp for example usage.
 *
 * @tparam Item The type of items to sweep
 * @tparam SweepTraits Traits class that defines the items' bounds,
 *   how to interpret them and how to sort the events
 * @ingroup Utilities
 */
template <typename SweepSet>
class Sweeper {
public:
    typedef typename SweepSet::ItemIterator Iter;

    explicit Sweeper(SweepSet &set)
        : _set(set)
    {
        std::size_t sz = std::distance(set.items().begin(), set.items().end());
        _entry_events.reserve(sz);
        _exit_events.reserve(sz);
    }

    /** @brief Process entry and exit events.
     * This will iterate over all inserted items, calling the methods
     * addActiveItem and removeActiveItem on the SweepSet passed at construction
     * according to the order of the boundaries of each item. */
    void process() {
        if (_set.items().empty()) return;

        Iter last = _set.items().end();
        for (Iter i = _set.items().begin(); i != last; ++i) {
            Interval b = _set.itemBounds(i);
            // guard against NANs
            assert(b.min() == b.min() && b.max() == b.max());
            _entry_events.push_back(Event(b.max(), i));
            _exit_events.push_back(Event(b.min(), i));
        }

        std::make_heap(_entry_events.begin(), _entry_events.end());
        std::make_heap(_exit_events.begin(), _exit_events.end());

        Event next_entry = _get_next(_entry_events);
        Event next_exit = _get_next(_exit_events);

        while (next_entry || next_exit) {
            assert(next_exit);

            if (!next_entry || next_exit > next_entry) {
                // exit event - remove record from active list
                _set.removeActiveItem(next_exit.item);
                next_exit = _get_next(_exit_events);
            } else {
                // entry event - add record to active list
                _set.addActiveItem(next_entry.item);
                next_entry = _get_next(_entry_events);
            }
        }
    }

private:
    struct Event
        : boost::totally_ordered<Event>
    {
        Coord coord;
        Iter item;

        Event(Coord c, Iter const &i)
            : coord(c), item(i)
        {}
        Event()
            : coord(nan("")), item()
        {}
        bool operator<(Event const &other) const { return coord < other.coord; }
        bool operator==(Event const &other) const { return coord == other.coord; }
        operator bool() const { return !std::isnan(coord); }
    };

    static Event _get_next(std::vector<Event> &heap) {
        if (heap.empty()) {
            Event e;
            return e;
        }
        std::pop_heap(heap.begin(), heap.end());
        Event ret = heap.back();
        heap.pop_back();
        return ret;
    }

    SweepSet &_set;
    std::vector<Event> _entry_events;
    std::vector<Event> _exit_events;
};

} // namespace Geom

#endif // !LIB2GEOM_SEEN_SWEEPER_H

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
