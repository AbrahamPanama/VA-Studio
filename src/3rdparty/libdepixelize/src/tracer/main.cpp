// SPDX-License-Identifier: LGPL-2.1-or-later OR GPL-2.0-or-later

#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>

#include <popt.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <depixelize/depixelize.h>

namespace {

enum Output : int
{
    VORONOI = 1,
    GROUPED_VORONOI,
    NON_SMOOTH,
    SMOOTH
};

const char *output;
Depixelize::Options opts;
int output_type = SMOOTH;
int show_version = 0;

poptOption options[] = {
    {"output", 'o',
     POPT_ARG_STRING, &output, 0,
     "Export document to a SVG file",
     "FILENAME"},
    {"curves", 'c',
     POPT_ARG_DOUBLE, &opts.curves_multiplier, 0,
     "Favors connections that are part of a long curve",
     "MULTIPLIER"},
    {"islands", 'i',
     POPT_ARG_INT, &opts.islands_weight, 0,
     "Avoid single disconnected pixels",
     "WEIGHT"},
    {"sparse-pixels", 'm',
     POPT_ARG_DOUBLE, &opts.sparse_pixels_multiplier, 0,
     "Favors connections that are part of foreground color",
     "MULTIPLIER"},
    {"sparse-pixels-radius", 'r',
     POPT_ARG_INT, &opts.sparse_pixels_radius, 0,
     "Favors connections that are part of foreground color",
     "WINDOW-RADIUS"},
    {"voronoi", 'v',
     POPT_ARG_VAL, &output_type, VORONOI,
     "Output composed of straight lines",
     nullptr},
    {"grouped-voronoi", 'g',
     POPT_ARG_VAL, &output_type, GROUPED_VORONOI,
     "Output composed of straight lines (grouping neighbours cells sharing the same color)",
     nullptr},
    {"no-smooth", 'n',
     POPT_ARG_VAL, &output_type, NON_SMOOTH,
     "Preserve staircasing artifacts",
     nullptr},
    {"version", '\0',
     POPT_ARG_NONE, &show_version, 0,
     "Displays version and exit",
     nullptr},
    POPT_AUTOHELP
    POPT_TABLEEND
};

void print_svg(std::ostream &svg, const Depixelize::Splines &splines)
{
    svg << std::setprecision(std::numeric_limits<Geom::Coord>::digits10)
        << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\""
        << splines.width() << "px\" height=\"" << splines.height() << "px\">\n";

    for (auto const &it : splines) {
        svg << "\t<path d=\""
            << it.pathVector
            << std::hex
            << "\" fill=\"#"
            << std::setfill('0')
            << std::setw(2) << int(it.rgba[0])
            << std::setw(2) << int(it.rgba[1])
            << std::setw(2) << int(it.rgba[2])
            << std::setw(0) << std::setfill(' ') << "\" fill-opacity=\""
            << double(it.rgba[3]) / 255
            << std::dec
            << "\" />\n";
    }

    svg << "</svg>" << std::endl;
}

template <auto f, typename T>
auto delete_with(T *p)
{
    struct Deleter { void operator()(T *p) { f(p); } };
    return std::unique_ptr<T, Deleter>(p);
}

} // namespace

int main(int argc, char const *argv[])
{
    auto popt_con = delete_with<poptFreeContext>(poptGetContext(argv[0], argc, argv, options, 0));
    poptGetNextOpt(popt_con.get());
    auto input = poptGetArg(popt_con.get());
    opts.optimize = output_type == SMOOTH;

    if (show_version) {
        std::cout << DEPIXELIZE_VERSION << std::endl;
        return EXIT_SUCCESS;
    }

    if (!input) {
        poptPrintUsage(popt_con.get(), stderr, 0);
        return EXIT_FAILURE;
    }

    if (opts.sparse_pixels_radius < 1) {
        std::cerr << "Sparse pixels radius (-r) should be a positive number" << std::endl;
        return EXIT_FAILURE;
    }

    if (opts.sparse_pixels_radius == 1) {
        std::cerr << "WARNING: Using value of 1 for sparse pixels radius (-r) doesn't make sense" << std::endl;
    }

    GError *error{};
    auto const pixbuf = delete_with<g_object_unref>(gdk_pixbuf_new_from_file(input, &error));
    if (error) {
        std::cerr << "Problem while processing file:\n"
                  << error->message
                  << std::endl;
        g_error_free(error);
        return EXIT_FAILURE;
    }

    auto const img = Depixelize::Image{
        .pixels = gdk_pixbuf_read_pixels(pixbuf.get()),
        .n_channels = gdk_pixbuf_get_n_channels(pixbuf.get()),
        .width = gdk_pixbuf_get_width(pixbuf.get()),
        .height = gdk_pixbuf_get_height(pixbuf.get()),
        .rowstride = gdk_pixbuf_get_rowstride(pixbuf.get())
    };

    Depixelize::Splines splines;
    switch (output_type) {
        case VORONOI:
            splines = Depixelize::to_voronoi(img, opts);
            break;
        case GROUPED_VORONOI:
            splines = Depixelize::to_grouped_voronoi(img, opts);
            break;
        default:
            splines = Depixelize::to_splines(img, opts);
            break;
    }

    if (output) {
        std::ofstream out(output);
        print_svg(out, splines);
    } else {
        print_svg(std::cout, splines);
    }
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
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:encoding=utf-8:textwidth=99 :
