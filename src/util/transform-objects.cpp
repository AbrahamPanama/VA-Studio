// SPDX-License-Identifier: GPL-2.0-or-later
//

#include "transform-objects.h"

#include <cmath>
#include <glibmm/i18n.h>
#include <glibmm/ustring.h>
#include <2geom/rect.h>
#include <2geom/transforms.h>

#include "desktop.h"
#include "selection.h"
#include "object/algorithms/bboxsort.h"
#include "object/sp-item-transform.h"

using namespace Inkscape;

void transform_move(Inkscape::Selection* selection,
                    double x, double y,
                    bool relative,
                    bool apply_separately,
                    double yaxisdir)
{
    if (!selection || selection->isEmpty()) return;

    // If relative, Y is specified in screen coordinates (UI); adapt to document y-axis direction
    if (relative) {
        y *= yaxisdir;
    }

    if (!apply_separately) {
        if (relative) {
            selection->moveRelative(x, y);
        } else {
            Geom::OptRect bbox = selection->preferredBounds();
            if (bbox) {
                selection->moveRelative(x - bbox->min()[Geom::X], y - bbox->min()[Geom::Y]);
            }
        }
    } else {
        if (relative) {
            // shift each object relatively to the previous one
            auto selected = selection->items_vector();
            if (selected.empty()) return;

            if (fabs(x) > 1e-6) {
                std::vector<BBoxSort> sorted;
                for (auto item : selected) {
                    Geom::OptRect bbox = item->desktopPreferredBounds();
                    if (bbox) {
                        sorted.emplace_back(item, *bbox, Geom::X, x > 0 ? 1. : 0., x > 0 ? 0. : 1.);
                    }
                }
                std::stable_sort(sorted.begin(), sorted.end());

                double move = x;
                for (auto it = sorted.begin(); it < sorted.end(); ++it) {
                    it->item->move_rel(Geom::Translate(move, 0));
                    move += x;
                }
            }
            if (fabs(y) > 1e-6) {
                std::vector<BBoxSort> sorted;
                for (auto item : selected) {
                    Geom::OptRect bbox = item->desktopPreferredBounds();
                    if (bbox) {
                        sorted.emplace_back(item, *bbox, Geom::Y, y > 0 ? 1. : 0., y > 0 ? 0. : 1.);
                    }
                }
                std::stable_sort(sorted.begin(), sorted.end());

                double move = y;
                for (auto it = sorted.begin(); it < sorted.end(); ++it) {
                    it->item->move_rel(Geom::Translate(0, move));
                    move += y;
                }
            }
        } else {
            // absolute positioning per selection bbox (preserve previous behavior)
            Geom::OptRect bbox = selection->preferredBounds();
            if (bbox) {
                selection->moveRelative(x - bbox->min()[Geom::X], y - bbox->min()[Geom::Y]);
            }
        }
    }
}

void transform_scale(Inkscape::Selection* selection,
                     double sx, double sy,
                     bool is_percent,
                     bool apply_separately,
                     bool transform_stroke,
                     bool preserve)
{
    if (!selection || selection->isEmpty()) return;

    if (apply_separately) {
        auto tmp = selection->items();
        for (auto item : tmp) {
            Geom::OptRect bbox_pref = item->desktopPreferredBounds();
            Geom::OptRect bbox_geom = item->desktopGeometricBounds();
            if (bbox_pref && bbox_geom) {
                double new_width = sx;
                double new_height = sy;
                if (is_percent) {
                    new_width = sx / 100 * bbox_pref->width();
                    new_height = sy / 100 * bbox_pref->height();
                }
                if (fabs(new_width) < 1e-6) new_width = 1e-6;
                if (fabs(new_height) < 1e-6) new_height = 1e-6;

                double x0 = bbox_pref->midpoint()[Geom::X] - new_width / 2;
                double y0 = bbox_pref->midpoint()[Geom::Y] - new_height / 2;
                double x1 = bbox_pref->midpoint()[Geom::X] + new_width / 2;
                double y1 = bbox_pref->midpoint()[Geom::Y] + new_height / 2;

                Geom::Affine scaler = get_scale_transform_for_variable_stroke(*bbox_pref, *bbox_geom, transform_stroke, preserve, x0, y0, x1, y1);
                item->set_i2d_affine(item->i2dt_affine() * scaler);
                item->doWriteTransform(item->transform);
            }
        }
    } else {
        Geom::OptRect bbox_pref = selection->preferredBounds();
        Geom::OptRect bbox_geom = selection->geometricBounds();
        if (bbox_pref && bbox_geom) {
            double new_width = sx;
            double new_height = sy;
            if (is_percent) {
                new_width = sx / 100 * bbox_pref->width();
                new_height = sy / 100 * bbox_pref->height();
            }
            if (fabs(new_width) < 1e-6) new_width = 1e-6;
            if (fabs(new_height) < 1e-6) new_height = 1e-6;

            double x0 = bbox_pref->midpoint()[Geom::X] - new_width / 2;
            double y0 = bbox_pref->midpoint()[Geom::Y] - new_height / 2;
            double x1 = bbox_pref->midpoint()[Geom::X] + new_width / 2;
            double y1 = bbox_pref->midpoint()[Geom::Y] + new_height / 2;
            Geom::Affine scaler = get_scale_transform_for_variable_stroke(*bbox_pref, *bbox_geom, transform_stroke, preserve, x0, y0, x1, y1);
            selection->applyAffine(scaler);
        }
    }
}

void transform_rotate(Inkscape::Selection* selection,
                      double angle_degrees,
                      bool apply_separately)
{
    if (!selection || selection->isEmpty()) return;

    if (apply_separately) {
        auto tmp = selection->items();
        for (auto item : tmp) {
            item->rotate_rel(Geom::Rotate(angle_degrees * M_PI / 180.0));
        }
    } else {
        std::optional<Geom::Point> center = selection->center();
        if (center) {
            selection->rotateRelative(*center, angle_degrees);
        }
    }
}

void transform_skew(Inkscape::Selection* selection,
                    double hx, double hy,
                    SkewUnits units,
                    bool apply_separately,
                    double yaxisdir)
{
    if (!selection || selection->isEmpty()) return;

    if (units == SkewUnits::Percent) {
        hy *= yaxisdir; // match UI behavior for vertical direction
        if (fabs(0.01 * hx * 0.01 * hy - 1.0) < Geom::EPSILON) {
            selection->desktop()->messageStack()->flash(Inkscape::WARNING_MESSAGE, _("Transform matrix is singular, <b>not used</b>."));
            return;
        }
        if (apply_separately) {
            auto items = selection->items();
            for (auto item : items) {
                item->skew_rel(0.01 * hx, 0.01 * hy);
            }
        } else {
            std::optional<Geom::Point> center = selection->center();
            if (center) {
                selection->skewRelative(*center, 0.01 * hx, 0.01 * hy);
            }
        }
    } else if (units == SkewUnits::AngleRadians) {
        double angleX = hx;
        double angleY = hy;
        if ((fabs(angleX - angleY + M_PI / 2) < Geom::EPSILON)
            || (fabs(angleX - angleY - M_PI / 2) < Geom::EPSILON)
            || (fabs((angleX - angleY) / 3 + M_PI / 2) < Geom::EPSILON)
            || (fabs((angleX - angleY) / 3 - M_PI / 2) < Geom::EPSILON)) {
            selection->desktop()->messageStack()->flash(Inkscape::WARNING_MESSAGE, _("Transform matrix is singular, <b>not used</b>."));
            return;
        }
        double skewX = tan(angleX);
        double skewY = tan(angleY);
        if (apply_separately) {
            auto items = selection->items();
            for (auto item : items) {
                item->skew_rel(skewX, skewY);
            }
        } else {
            std::optional<Geom::Point> center = selection->center();
            if (center) {
                selection->skewRelative(*center, skewX, skewY);
            }
        }
    } else { // Absolute (linear) displacement inputs
        double x = hx;
        double y = hy * yaxisdir;
        // An absolute skew is x / height horizontally and y / width vertically. A displacement of 0 needs no
        // extent (a vertical line can still be sheared by y); a non-zero one cannot be expressed on a flat
        // axis (x / 0 = Inf), so that object is skipped rather than given a non-finite matrix.
        auto skew_terms = [&](Geom::Rect const &bbox, double &sx, double &sy) {
            double const height = bbox.height();
            double const width = bbox.width();
            if ((x != 0 && !(height > 1e-12)) || (y != 0 && !(width > 1e-12))) {
                return false;
            }
            sx = x != 0 ? x / height : 0.0;
            sy = y != 0 ? y / width : 0.0;
            return std::isfinite(sx) && std::isfinite(sy);
        };
        auto flash_skipped = [&](unsigned skipped) {
            if (skipped && selection->desktop()) {
                selection->desktop()->messageStack()->flash(
                    Inkscape::WARNING_MESSAGE,
                    Glib::ustring::compose(_("%1 object(s) have no extent in the skew direction and were left unchanged."),
                                           skipped).c_str());
            }
        };
        if (apply_separately) {
            auto items = selection->items();
            unsigned skipped = 0;
            for (auto item : items) {
                Geom::OptRect bbox = item->desktopPreferredBounds();
                if (!bbox) continue;
                double sx = 0, sy = 0;
                if (!skew_terms(*bbox, sx, sy)) {
                    ++skipped; // kept as it is; the other objects are still skewed
                    continue;
                }
                if (fabs(sx * sy - 1.0) < Geom::EPSILON) {
                    selection->desktop()->messageStack()->flash(Inkscape::WARNING_MESSAGE, _("Transform matrix is singular, <b>not used</b>."));
                    return;
                }
                item->skew_rel(sx, sy);
            }
            flash_skipped(skipped);
        } else {
            std::optional<Geom::Point> center = selection->center();
            Geom::OptRect bbox = selection->preferredBounds();
            if (center && bbox) {
                double sx = 0, sy = 0;
                if (!skew_terms(*bbox, sx, sy)) {
                    flash_skipped(selection->size()); // refused as a whole, nothing changed
                    return;
                }
                if (fabs(sx * sy - 1.0) < Geom::EPSILON) {
                    selection->desktop()->messageStack()->flash(Inkscape::WARNING_MESSAGE, _("Transform matrix is singular, <b>not used</b>."));
                    return;
                }
                selection->skewRelative(*center, sx, sy);
            }
        }
    }
}

void transform_apply_matrix(Inkscape::Selection* selection,
                            const Geom::Affine& affine,
                            bool replace_matrix)
{
    if (!selection || selection->isEmpty()) return;

    if (affine.isSingular()) {
        selection->desktop()->messageStack()->flash(Inkscape::WARNING_MESSAGE, _("Transform matrix is singular, <b>not used</b>."));
        return;
    }

    if (replace_matrix) {
        auto tmp = selection->items();
        for (auto item : tmp) {
            item->set_item_transform(affine);
            item->updateRepr();
        }
    } else {
        // post-multiply each object's transform
        selection->applyAffine(affine);
    }
}
