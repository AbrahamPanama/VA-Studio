// SPDX-License-Identifier: LGPL-2.1-only OR MPL-1.1
/** @file
 * @brief Path sink which writes an SVG-compatible command string
 *//*
 * Authors:
 *   Krzysztof Kosiński <tweenk.pl@gmail.com>
 * 
 * Copyright 2014 Authors
 */

#ifndef LIB2GEOM_SEEN_SVG_PATH_WRITER_H
#define LIB2GEOM_SEEN_SVG_PATH_WRITER_H

#include <2geom/path-sink.h>
#include <sstream>

namespace Geom {

/** @brief Serialize paths to SVG path data strings.
 * You can access the generated string by calling the str() method.
 * @ingroup Paths
 */
class SVGPathWriter
    : public PathSink
{
public:
    SVGPathWriter();
    ~SVGPathWriter() override {}

    void moveTo(Point const &p) override;
    void lineTo(Point const &p) override;
    void quadTo(Point const &c, Point const &p) override;
    void curveTo(Point const &c0, Point const &c1, Point const &p) override;
    void arcTo(double rx, double ry, double angle,
               bool large_arc, bool sweep, Point const &p) override;
    void closePath() override;
    void flush() override;

    /// Clear any path data written so far.
    void clear();

    /** @brief Set output precision.
     * When the parameter is negative, the path writer enters a verbatim mode
     * which preserves all values exactly. */
    void setPrecision(int prec);

    /** @brief Enable or disable length optimization.
     * 
     * When set to true, the path writer will optimize the generated path data
     * for minimum length. However, this will make the data less readable,
     * because spaces between commands and coordinates will be omitted where
     * unnecessary for correct parsing.
     *
     * When set to false, the string will be a straightforward, partially redundant
     * representation of the passed commands, optimized for readability.
     * Commands and coordinates will always be separated by spaces and the command
     * symbol will not be omitted for multiple consecutive commands of the same type.
     *
     * Length optimization is turned off by default. */
    void setOptimize(bool opt) { _optimize = opt; }

    /** @brief Enable or disable the use of V, H, T and S commands where possible.
     * Shorthands are turned on by default. */
    void setUseShorthands(bool use) { _use_shorthands = use; }

    /// Retrieve the generated path data string.
    std::string str() const { return _s.str(); }

private:
    void _setCommand(char cmd);
    std::string _formatCoord(Coord par);

    std::ostringstream _s, _ns;
    std::vector<Coord> _current_pars;
    Point _subpath_start;
    Point _current;
    Point _quad_tangent;
    Point _cubic_tangent;
    Coord _epsilon;
    int _precision;
    bool _optimize;
    bool _use_shorthands;
    char _command;
};

std::string write_svg_path(PathVector const &pv, int prec = -1, bool optimize = false, bool shorthands = true);

} // namespace Geom

#endif // LIB2GEOM_SEEN_SVG_PATH_WRITER_H
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
