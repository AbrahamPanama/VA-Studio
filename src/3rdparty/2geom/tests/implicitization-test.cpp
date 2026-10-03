// SPDX-License-Identifier: LGPL-2.1-only OR MPL-1.1
/*
 * Test program for implicitization routines
 *
 * Authors:
 *      Marco Cecchetti <mrcekets at gmail.com>
 *
 * Copyright 2008  authors
 */



#include <2geom/symbolic/implicit.h>

#include "pick.h"

#include <iostream>




void print_basis(Geom::SL::basis_type const& b)
{
    for (size_t i= 0; i < 2; ++i)
    {
        for (size_t j= 0; j < 3; ++j)
        {
            std::cout << "b[" << i << "][" << j << "] = " << b[i][j] << "\n";
        }
    }
}




int main( int argc, char * argv[] )
{
    // degree of polinomial parametrization
    // warning: not set N to a value greater than 20!
    // (10 in case you don't utilize the micro-basis)
    // determinant computation becomes very expensive
    unsigned int N = 4;
    // max modulus of polynomial coefficients
    unsigned int M = 1000;

    if (argc > 1)
        N = std::atoi(argv[1]);
    if (argc > 2)
        M = std::atoi(argv[2]);

    Geom::SL::MVPoly1 f, g;
    Geom::SL::basis_type b;
    Geom::SL::MVPoly3 p, q;
    Geom::SL::Matrix<Geom::SL::MVPoly2> B;
    Geom::SL::MVPoly2 r;

    // generate two univariate polynomial with degree N
    // and coeffcient in the range [-M, M]
    f = pick_multipoly_max<1>(N, M);
    g = pick_multipoly_max<1>(N, M);

    std::cout << "parametrization: \n";
    std::cout << "f = " << f << std::endl;
    std::cout << "g = " << g << "\n\n";

    // computes the micro-basis
    microbasis(b, f, g);
    // in case you want utilize directly the initial basis
    // you should uncomment the next row and comment
    // the microbasis function call
    //make_initial_basis(b, f, g);

    std::cout << "generators in vector form : \n";
    print_basis(b);
    std::cout << std::endl;

    // micro-basis generators
    basis_to_poly(p, b[0]);
    basis_to_poly(q, b[1]);

    std::cout << "generators as polynomial in R[t,x,y] : \n";
    std::cout << "p = " << p << std::endl;
    std::cout << "q = " << q << "\n\n";

    // make up the Bezout matrix and compute the determinant
    B = make_bezout_matrix(p, q);
    r = determinant_minor(B);
    r.normalize();

    std::cout << "Bezout matrix: (entries are bivariate polynomials) \n";
    std::cout << "B = " << B << "\n\n";
    std::cout << "determinant: \n";
    std::cout << "r(x, y) = " << r << "\n\n";

    return EXIT_SUCCESS;
}


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
