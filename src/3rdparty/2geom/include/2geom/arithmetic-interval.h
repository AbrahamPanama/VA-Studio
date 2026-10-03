// SPDX-License-Identifier: LGPL-2.1-only OR MPL-1.1
/**
 * @file
 * @brief ArithmeticInterval - Wrapper for boost::numeric::interval
 *//*
 * Copyright 2025 Muhammad Rafay Irfan <rafay119muhammad@gmail.com>
 */

#ifndef LIB2GEOM_SEEN_ARITHMETIC_INTERVAL_H
#define LIB2GEOM_SEEN_ARITHMETIC_INTERVAL_H

#include <boost/numeric/interval.hpp>
#include <2geom/coord.h>
#include <algorithm>

namespace Geom {
class ArithmeticInterval {
private:
    using boost_interval = boost::numeric::interval<Coord>;
    boost_interval _interval;

    const boost_interval& toBoost() const { return _interval; }

public:
    ArithmeticInterval() = default;

    ArithmeticInterval(const ArithmeticInterval&) = default;

    ArithmeticInterval& operator=(const ArithmeticInterval&) = default;

    explicit ArithmeticInterval(Coord val) : _interval(val) {}

    ArithmeticInterval(Coord lower, Coord upper) : _interval(lower, upper) {}

    Coord min() const { return lower(_interval); }
    Coord max() const { return upper(_interval); }
    Coord extent() const { return  width(_interval); }
    Coord middle() const { return median(_interval); }
    bool isSingular() const { return singleton(_interval); }
    bool isEmpty() const { return empty(_interval); }

    bool contains(Coord val) const {
        return in(val, _interval);
    }

    bool contains(ArithmeticInterval const &other) const {
        return subset(other._interval, _interval);
    }

    bool intersects(ArithmeticInterval const &other) const {
        return overlap(_interval, other._interval);
    }

    static ArithmeticInterval intersection(ArithmeticInterval const &a, ArithmeticInterval const &b) {
        ArithmeticInterval result;
        result._interval = intersect(a._interval, b._interval);
        return result;
    }

    // Arithmetic operators
    ArithmeticInterval operator+(const ArithmeticInterval& other) const {
        ArithmeticInterval result;
        result._interval = _interval + other._interval;
        return result;
    }

    ArithmeticInterval operator-(const ArithmeticInterval& other) const {
        ArithmeticInterval result;
        result._interval = _interval - other._interval;
        return result;
    }

    ArithmeticInterval operator*(const ArithmeticInterval& other) const {
        ArithmeticInterval result;
        result._interval = _interval * other._interval;
        return result;
    }

    ArithmeticInterval operator/(const ArithmeticInterval& other) const {
        ArithmeticInterval result;
        result._interval = _interval / other._interval;
        return result;
    }

    ArithmeticInterval operator+(Coord scalar) const {
        ArithmeticInterval result;
        result._interval = _interval + scalar;
        return result;
    }

    ArithmeticInterval operator-(Coord scalar) const {
        ArithmeticInterval result;
        result._interval = _interval - scalar;
        return result;
    }

    ArithmeticInterval operator*(Coord scalar) const {
        ArithmeticInterval result;
        result._interval = _interval * scalar;
        return result;
    }

    ArithmeticInterval operator/(Coord scalar) const {
        ArithmeticInterval result;
        result._interval = _interval / scalar;
        return result;
    }

    // Comparison operators
    bool operator==(const ArithmeticInterval& other) const {
        return equal(_interval, other._interval);
    }

    bool operator!=(const ArithmeticInterval& other) const {
        return !(equal(_interval, other._interval));
    }

    bool operator<(const ArithmeticInterval& other) const {
        return boost::numeric::interval_lib::cerlt(_interval, other._interval);
    }

    bool operator<=(const ArithmeticInterval& other) const {
        return boost::numeric::interval_lib::cerle(_interval, other._interval);
    }

    bool operator>(const ArithmeticInterval& other) const {
        return boost::numeric::interval_lib::cergt(_interval, other._interval);
    }

    bool operator>=(const ArithmeticInterval& other) const {
        return boost::numeric::interval_lib::cerge(_interval, other._interval);
    }

    ArithmeticInterval& operator+=(const ArithmeticInterval& other) {
        _interval += other._interval;
        return *this;
    }

    ArithmeticInterval& operator-=(const ArithmeticInterval& other) {
        _interval -= other._interval;
        return *this;
    }

    ArithmeticInterval& operator*=(const ArithmeticInterval& other) {
        _interval *= other._interval;
        return *this;
    }

    ArithmeticInterval& operator/=(const ArithmeticInterval& other) {
        _interval /= other._interval;
        return *this;
    }

    ArithmeticInterval& operator+=(Coord scalar) {
        _interval += scalar;
        return *this;
    }

    ArithmeticInterval& operator-=(Coord scalar) {
        _interval -= scalar;
        return *this;
    }

    ArithmeticInterval& operator*=(Coord scalar) {
        _interval *= scalar;
        return *this;
    }

    ArithmeticInterval& operator/=(Coord scalar) {
        _interval /= scalar;
        return *this;
    }

    friend ArithmeticInterval sqrt(ArithmeticInterval const &a);

    friend ArithmeticInterval abs(ArithmeticInterval const &a);

    friend ArithmeticInterval max(ArithmeticInterval const &a, ArithmeticInterval const &b);

    friend ArithmeticInterval min(ArithmeticInterval const &a, ArithmeticInterval const &b);

    friend ArithmeticInterval max(ArithmeticInterval const &a, Coord b);

    friend ArithmeticInterval min(ArithmeticInterval const &a, Coord b);

    friend ArithmeticInterval square(ArithmeticInterval const &a);
};

} // namespace Geom

#endif // LIB2GEOM_SEEN_ARITHMETIC_INTERVAL_H