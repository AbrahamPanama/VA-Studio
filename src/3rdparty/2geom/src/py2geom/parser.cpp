// SPDX-License-Identifier: LGPL-2.1-only OR MPL-1.1
/*
 * Copyright 2008 Aaron Spike <aaron@ekips.org>
 */

#include <boost/python.hpp>

#include "py2geom.h"
#include "helpers.h"

#include "2geom/path-sink.h"
#include "2geom/svg-path-parser.h"


using namespace boost::python;

void (*parse_svg_path_str_sink) (char const *, Geom::PathSink &) = &Geom::parse_svg_path;
Geom::PathVector (*parse_svg_path_str) (char const *) = &Geom::parse_svg_path;

void (Geom::PathSink::*feed_path)(Geom::Path const &) = &Geom::PathSink::feed;
void (Geom::PathSink::*feed_pathvector)(Geom::PathVector const &) = &Geom::PathSink::feed;

class PathSinkWrap: public Geom::PathSink, public wrapper<Geom::PathSink> {
    void moveTo(Geom::Point const &p) {this->get_override("moveTo")(p);}
    void lineTo(Geom::Point const &p) {this->get_override("lineTo")(p);}
    void curveTo(Geom::Point const &c0, Geom::Point const &c1, Geom::Point const &p) {this->get_override("curveTo")(c0, c1, p);}
    void quadTo(Geom::Point const &c, Geom::Point const &p) {this->get_override("quadTo")(c, p);}
    void arcTo(double rx, double ry, double angle, bool large_arc, bool sweep, Geom::Point const &p) {this->get_override("arcTo")(rx, ry, angle, large_arc, sweep, p);}
    bool backspace() {return this->get_override("backspace")();}
    void closePath() {this->get_override("closePath")();}
    void flush() {this->get_override("flush")();}
};

void wrap_parser() {
    def("parse_svg_path", parse_svg_path_str_sink);
    def("parse_svg_path", parse_svg_path_str);
    def("read_svgd", Geom::read_svgd);

    class_<PathSinkWrap, boost::noncopyable>("PathSink")
        .def("moveTo", pure_virtual(&Geom::PathSink::moveTo))
        .def("lineTo", pure_virtual(&Geom::PathSink::lineTo))
        .def("curveTo", pure_virtual(&Geom::PathSink::curveTo))
        .def("quadTo", pure_virtual(&Geom::PathSink::quadTo))
        .def("arcTo", pure_virtual(&Geom::PathSink::arcTo))
        .def("backspace", pure_virtual(&Geom::PathSink::backspace))
        .def("closePath", pure_virtual(&Geom::PathSink::closePath))
        .def("flush", pure_virtual(&Geom::PathSink::flush))
        .def("feed", feed_path)
        .def("feed", feed_pathvector)
    ;
};

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
