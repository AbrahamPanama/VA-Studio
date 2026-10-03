// SPDX-License-Identifier: LGPL-2.1-only OR MPL-1.1
/**
 * \file
 * \brief Calculation of binomial cefficients
 *//*
 * Copyright 2006 Nathan Hurst <njh@mail.csse.monash.edu.au>
 */

#ifndef LIB2GEOM_SEEN_CHOOSE_H
#define LIB2GEOM_SEEN_CHOOSE_H

#include <vector>

namespace Geom {

/**
 * @brief Given a multiple of binomial(n, k), modify it to the same multiple of binomial(n + 1, k).
 */
template <typename T>
constexpr void binomial_increment_n(T &b, int n, int k)
{
    b = b * (n + 1) / (n + 1 - k);
}

/**
 * @brief Given a multiple of binomial(n, k), modify it to the same multiple of binomial(n - 1, k).
 */
template <typename T>
constexpr void binomial_decrement_n(T &b, int n, int k)
{
    b = b * (n - k) / n;
}

/**
 * @brief Given a multiple of binomial(n, k), modify it to the same multiple of binomial(n, k + 1).
 */
template <typename T>
constexpr void binomial_increment_k(T &b, int n, int k)
{
    b = b * (n - k) / (k + 1);
}

/**
 * @brief Given a multiple of binomial(n, k), modify it to the same multiple of binomial(n, k - 1).
 */
template <typename T>
constexpr void binomial_decrement_k(T &b, int n, int k)
{
    b = b * k / (n + 1 - k);
}

/**
 * @brief Calculate the (n, k)th binomial coefficient.
 */
template <typename T>
constexpr T choose(unsigned n, unsigned k)
{
    if (k > n) {
        return 0;
    }
    T b = 1;
    int max = std::min(k, n - k);
    for (int i = 0; i < max; i++) {
        binomial_increment_k(b, n, i);
    }
    return b;
}

/**
 * @brief Class for calculating and accessing a row of Pascal's triangle.
 */
template <typename ValueType>
class BinomialCoefficient
{
public:
    using value_type = ValueType;
    using container_type = std::vector<value_type>;

    BinomialCoefficient(unsigned int _n)
        : n(_n)
    {
        coefficients.reserve(n / 2 + 1);
        coefficients.emplace_back(1);
        value_type b = 1;
        for (int i = 0; i < n / 2; i++) {
            binomial_increment_k(b, n, i);
            coefficients.emplace_back(b);
        }
    }

    unsigned int size() const
    {
        return degree() + 1;
    }

    unsigned int degree() const
    {
        return n;
    }

    value_type operator[](unsigned int k) const
    {
        return coefficients[std::min(k, n - k)];
    }

private:
    int const n;
    container_type coefficients;
};

} // namespace Geom

#endif // LIB2GEOM_SEEN_CHOOSE_H

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
