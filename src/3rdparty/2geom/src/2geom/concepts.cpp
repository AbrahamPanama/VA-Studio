// SPDX-License-Identifier: LGPL-2.1-only OR MPL-1.1
/**
 * \file
 * \brief Concept checking
 *//*
 * Copyright 2015 Krzysztof Kosiński <tweenk.pl@gmail.com>
 */

#include <boost/concept/assert.hpp>
#include <2geom/concepts.h>

#include <2geom/line.h>
#include <2geom/circle.h>
#include <2geom/ellipse.h>
#include <2geom/curves.h>
#include <2geom/convex-hull.h>
#include <2geom/path.h>
#include <2geom/pathvector.h>

#include <2geom/bezier.h>
#include <2geom/sbasis.h>
#include <2geom/linear.h>
#include <2geom/d2.h>

namespace Geom {

void concept_checks()
{
    BOOST_CONCEPT_ASSERT((ShapeConcept<Line>));
    //BOOST_CONCEPT_ASSERT((ShapeConcept<Circle>));
    //BOOST_CONCEPT_ASSERT((ShapeConcept<Ellipse>));
    BOOST_CONCEPT_ASSERT((ShapeConcept<BezierCurve>));
    BOOST_CONCEPT_ASSERT((ShapeConcept<EllipticalArc>));
    //BOOST_CONCEPT_ASSERT((ShapeConcept<SBasisCurve>));
    //BOOST_CONCEPT_ASSERT((ShapeConcept<ConvexHull>));
    //BOOST_CONCEPT_ASSERT((ShapeConcept<Path>));
    //BOOST_CONCEPT_ASSERT((ShapeConcept<PathVector>));

    BOOST_CONCEPT_ASSERT((NearConcept<Coord>));
    BOOST_CONCEPT_ASSERT((NearConcept<Point>));

    BOOST_CONCEPT_ASSERT((FragmentConcept<Bezier>));
    BOOST_CONCEPT_ASSERT((FragmentConcept<Linear>));
    BOOST_CONCEPT_ASSERT((FragmentConcept<SBasis>));
}

} // end namespace Geom
