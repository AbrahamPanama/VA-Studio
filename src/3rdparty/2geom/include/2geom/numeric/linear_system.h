// SPDX-License-Identifier: LGPL-2.1-only OR MPL-1.1
/*
 * LinearSystem class wraps some gsl routines for solving linear systems
 *
 * Authors:
 * 		Marco Cecchetti <mrcekets at gmail.com>
 * 
 * Copyright 2008  authors
 */


#ifndef _NL_LINEAR_SYSTEM_H_
#define _NL_LINEAR_SYSTEM_H_


#include <cassert>

#include <gsl/gsl_linalg.h>

#include <2geom/numeric/matrix.h>
#include <2geom/numeric/vector.h>


namespace Geom { namespace NL {


class LinearSystem
{
public:
	LinearSystem(MatrixView & _matrix, VectorView & _vector)
		: m_matrix(_matrix), m_vector(_vector), m_solution(_matrix.columns())
	{
	}
	
	LinearSystem(Matrix & _matrix, Vector & _vector)
		: m_matrix(_matrix), m_vector(_vector), m_solution(_matrix.columns())
	{
	}
	
	const Vector & LU_solve()
	{
		assert( matrix().rows() == matrix().columns() 
				&& matrix().rows() == vector().size() );
		int s;
		gsl_permutation * p = gsl_permutation_alloc(matrix().rows());
		gsl_linalg_LU_decomp (matrix().get_gsl_matrix(), p, &s);
		gsl_linalg_LU_solve( matrix().get_gsl_matrix(), 
							 p, 
				             vector().get_gsl_vector(), 
				             m_solution.get_gsl_vector()
				           );
		gsl_permutation_free(p);
		return solution();
	}
	
	const Vector & SV_solve()
	{
		assert( matrix().rows() >= matrix().columns()
				&& matrix().rows() == vector().size() );
		
		gsl_matrix* U = matrix().get_gsl_matrix();
		gsl_matrix* V = gsl_matrix_alloc(matrix().columns(), matrix().columns());
		gsl_vector* S = gsl_vector_alloc(matrix().columns());
		gsl_vector* work = gsl_vector_alloc(matrix().columns());
		
		gsl_linalg_SV_decomp( U, V, S, work );
		
		gsl_vector* b = vector().get_gsl_vector();
		gsl_vector* x = m_solution.get_gsl_vector();
		
		gsl_linalg_SV_solve( U, V, S, b, x);
		
		gsl_matrix_free(V);
		gsl_vector_free(S);
		gsl_vector_free(work);
		
		return solution();			  
	}
	
	MatrixView & matrix()
	{
		return m_matrix;
	}
	
	VectorView & vector()
	{
		return m_vector;
	}
	
	const Vector & solution() const
	{
		return m_solution;
	}
	
private:
	MatrixView m_matrix;
	VectorView m_vector;
	Vector m_solution;
};


} } // end namespaces


#endif /*_NL_LINEAR_SYSTEM_H_*/

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
