// SPDX-License-Identifier: LGPL-2.1-only OR MPL-1.1
/*
 * Matrix<CoeffT> class template
 *
 * Authors:
 *      Marco Cecchetti <mrcekets at gmail.com>
 *
 * Copyright 2008  authors
 */


#ifndef _GEOM_SL_MATRIX_H_
#define _GEOM_SL_MATRIX_H_


#include <array>
#include <vector>
#include <map>

#include <2geom/point.h>
#include <2geom/numeric/matrix.h>
#include <2geom/symbolic/multipoly.h>




namespace Geom { namespace SL {

/*
 *  generic Matrix class template
 *  needed for building up a matrix with polynomial entries
 */
template< typename Coeff>
class Matrix
{
  public:
    typedef Coeff coeff_type;
    typedef std::vector<coeff_type> container_type;

    Matrix()
    {}

    Matrix(size_t m, size_t n)
        : m_data(m*n), m_rows(m), m_columns(n)
    {
    }

    void resize(size_t m, size_t n)
    {
        m_data.resize(m,n);
        m_rows = m;
        m_columns = n;
    }

    size_t rows() const
    {
        return m_rows;
    }

    size_t columns() const
    {
        return m_columns;
    }

    coeff_type const& operator() (size_t i, size_t j) const
    {
        return m_data[i * columns() + j];
    }

    coeff_type & operator() (size_t i, size_t j)
    {
        return m_data[i * columns() + j];
    }


  private:
    container_type m_data;
    size_t m_rows;
    size_t m_columns;
};


template< typename Coeff, typename charT >
inline
std::basic_ostream<charT> &
operator<< ( std::basic_ostream<charT> & os,
             const Matrix<Coeff> & _matrix )
{
    if (_matrix.rows() == 0 || _matrix.columns() == 0) return os;

    os << "{{" << _matrix(0,0);
    for (size_t j = 1; j < _matrix.columns(); ++j)
    {
        os << ", " << _matrix(0,j);
    }
    os << "}";

    for (size_t i = 1; i < _matrix.rows(); ++i)
    {
        os << ", {" << _matrix(i,0);
        for (size_t j = 1; j < _matrix.columns(); ++j)
        {
            os << ", " << _matrix(i,j);
        }
        os << "}";
    }
    os << "}";
    return os;
}

template <size_t N, typename CoeffT, typename T>
void polynomial_matrix_evaluate (Matrix<T> & A,
                                 Matrix< MultiPoly<N, CoeffT> > const& M,
                                 std::array<T, N> const& X)
{
    A.resize(M.rows(), M.columns());
    for (size_t i = 0; i < M.rows(); ++i)
    {
        for (size_t j = 0; j < M.columns(); ++j)
        {
            A(i,j) = M(i,j)(X);
        }
    }
}


inline
void polynomial_matrix_evaluate (NL::Matrix & A,
                                 Matrix< MultiPoly<2, double> > const& M,
                                 Point const& P)
{
    for (size_t i = 0; i < M.rows(); ++i)
    {
        for (size_t j = 0; j < M.columns(); ++j)
        {
            A(i,j) = M(i,j)(P[X], P[Y]);
        }
    }
}


/*
template< typename Coeff>
class SymmetricSquareMatrix
{
  public:
    typedef Coeff coeff_type;
    typedef std::vector<coeff_type> container_type;

    SymmetricSquareMatrix(size_t n)
        : m_data((n*n)/2 + n), m_size(n)
    {

    }

    size_t rows() const
    {
        return m_size;
    }

    size_t columns() const
    {
        return m_size;
    }

    coeff_type const& operator() (size_t i, size_t j) const
    {
        return m_data[i * columns() + j];
    }

    coeff_type & operator() (size_t i, size_t j)
    {
        return m_data[i * columns() + j];
    }

    coeff_type det()
    {

    }

  private:
    container_type m_data;
    size_t m_size;
};
*/

/*
 * This is an adaptation of the LU algorithm used in the numerical case.
 * This algorithm is based on the article due to Bareiss:
 * "Sylvester's identity and multistep integer-preserving Gaussian elimination"
 */

/*
template< typename CoeffT >
CoeffT det(Matrix<CoeffT> const& M)
{
    assert(M.rows() == M.columns());

    Matrix<CoeffT> A(M);
    CoeffT n;
    CoeffT d = one<CoeffT>()();
    for (size_t k = 1; k < A.rows(); ++k)
    {
        for (size_t i = k; i < A.rows(); ++i)
        {
            for (size_t j = k; j < A.columns(); ++j)
            {
                n = A(i,j) * A(k-1,k-1) - A(k-1,j) * A(i,k-1);
//                std::cout << "k, i, j: "
//                          << k << ", " << i << ",  " << j << std::endl;
//                std::cout << "n = " << n << std::endl;
//                std::cout << "d = " << d << std::endl;
                A(i,j) = factor(n, d);
            }
        }
        d = A(k-1,k-1);
    }
    return A(A.rows()-1, A.columns()-1);
}
*/



} /*end namespace Geom*/  } /*end namespace SL*/


#include <2geom/symbolic/determinant-minor.h>


#endif // _GEOM_SL_MATRIX_H_


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
