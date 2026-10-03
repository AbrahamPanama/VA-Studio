// SPDX-License-Identifier: LGPL-2.1-or-later OR GPL-2.0-or-later
/*
 * Authors:
 *   Vinícius dos Santos Oliveira <vini.ipsmaker@gmail.com>
 *
 * Copyright (C) 2013 Authors
 */

#ifndef LIBDEPIXELIZE_ITERATOR_H
#define LIBDEPIXELIZE_ITERATOR_H

#include <vector>
#include <iterator>

namespace Depixelize {

template<typename T>
const T *to_ptr(typename std::vector<T>::const_iterator it)
{
    return &*it;
}

template<typename T>
T *to_ptr(typename std::vector<T>::iterator it)
{
    return &*it;
}

template<typename T>
typename std::vector<T>::const_iterator to_iterator(T const *ptr,
                                                    typename std::vector<T>
                                                    ::const_iterator begin)
{
    using It = typename std::vector<T>::const_iterator;
    using difference_type = typename std::iterator_traits<It>::difference_type;
    difference_type idx = ptr - to_ptr<T>(begin);
    return begin + idx;
}

template<typename T>
typename std::vector<T>::iterator to_iterator(T *ptr,
                                              typename std::vector<T>::iterator
                                              begin)
{
    using It = typename std::vector<T>::iterator;
    using difference_type = typename std::iterator_traits<It>::difference_type;
    difference_type idx = ptr - to_ptr<T>(begin);
    return begin + idx;
}

template<typename T>
class ToIter
{
public:
    using const_iterator = typename std::vector<T>::const_iterator;
    using iterator = typename std::vector<T>::iterator;

    ToIter(const_iterator begin) :
        begin(begin)
    {}

    const_iterator operator()(T const *ptr) const
    {
        return to_iterator<T>(ptr, begin);
    }

    iterator operator()(T *ptr) const
    {
        return to_iterator<T>(ptr, begin);
    }

private:
    typename std::vector<T>::const_iterator begin;
};

template<typename T>
class ToPtr
{
public:
    using const_iterator = typename std::vector<T>::const_iterator;
    using iterator = typename std::vector<T>::iterator;

    const T *operator()(const_iterator it) const
    {
        return to_ptr<T>(it);
    }

    T *operator()(iterator it) const
    {
        return to_ptr<T>(it);
    }
};

} // namespace Depixelize

#endif // LIBDEPIXELIZE_ITERATOR_H

/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0)(case-label . +))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:encoding=utf-8:textwidth=99 :
