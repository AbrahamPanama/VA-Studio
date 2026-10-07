// SPDX-License-Identifier: GPL-2.0-or-later

/** @file
 *
 * Two related object to path operations:
 *
 * 1. Find a path that includes fill, stroke, and markers. Useful for finding a visual bounding box.
 * 2. Take a set of objects and find an identical visual representation using only paths.
 *
 * Copyright (C) 2020 Tavmjong Bah
 * Copyright (C) 2018 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 *
 * Code moved from splivarot.cpp
 *
 */

#include <vector>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include "document.h"
#include "document-undo.h"
#include "selection.h"
#include "style.h"

#include "live_effects/lpeobject-reference.h"

#include "helper/geom.h"    // pathv_to_linear_and_cubic()

#include "livarot/LivarotDefs.h"
#include "livarot/Path.h"
#include "livarot/Shape.h"

#include "object/object-set.h"
#include "object/box3d.h"
#include "object/sp-item.h"
#include "object/weakptr.h"
#include "object/sp-clippath.h"
#include "object/sp-defs.h"
#include "object/sp-mask.h"
#include "object/sp-marker.h"
#include "object/sp-shape.h"
#include "object/sp-text.h"
#include "object/sp-flowtext.h"
#include "object/sp-use.h"

#include "path-chemistry.h"
#include "path-curve.h"
#include "path-outline.h"

#include "svg/svg.h"
#include "svg/svg-length.h"
#include "xml/attribute-record.h"

/**
 * Given an item, find a path representing the fill and a path representing the stroke.
 * Returns true if fill path found. Item may not have a stroke in which case stroke path is empty.
 * bbox_only==true skips cleaning up the stroke path.
 * Encapsulates use of livarot.
 */
bool
item_find_paths(const SPItem *item, Geom::PathVector& fill, Geom::PathVector& stroke, bool bbox_only)
{
    auto shape = cast<SPShape>(item);
    auto text = cast<SPText>(item);

    if (!shape && !text) {
        return false;
    }

    std::optional<Geom::PathVector> curve;
    if (shape) {
        curve = ptr_to_opt(shape->curve());
    } else if (text) {
        curve = text->getNormalizedBpath();
    } else {
        std::cerr << "item_find_paths: item not shape or text!" << std::endl;
        return false;
    }

    if (!curve) {
        std::cerr << "item_find_paths: no curve!" << std::endl;
        return false;
    }

    if (curve->empty()) {
        std::cerr << "item_find_paths: curve empty!" << std::endl;
        return false;
    }

    fill = std::move(*curve);

    if (!item->style) {
        // Should never happen
        std::cerr << "item_find_paths: item with no style!" << std::endl;
        return false;
    }

    if (item->style->stroke.isNone() || item->style->stroke_width.computed <= Geom::EPSILON) {
        // No stroke, no chocolate!
        return true;
    }

    // Now that we have a valid curve with stroke, do offset. We use Livarot for this as
    // lib2geom does not yet handle offsets correctly.

    // Livarot's outline of arcs is broken. So convert the path to linear and cubics only, for
    // which the outline is created correctly.
    Geom::PathVector pathv = pathv_to_linear_and_cubic_beziers( fill );

    SPStyle *style = item->style;

    double stroke_width = style->stroke_width.computed;
    double miter = style->stroke_miterlimit.value * stroke_width;

    JoinType join;
    switch (style->stroke_linejoin.computed) {
        case SP_STROKE_LINEJOIN_MITER:
            join = join_pointy;
            break;
        case SP_STROKE_LINEJOIN_ROUND:
            join = join_round;
            break;
        default:
            join = join_straight;
            break;
    }

    ButtType butt;
    switch (style->stroke_linecap.computed) {
        case SP_STROKE_LINECAP_SQUARE:
            butt = butt_square;
            break;
        case SP_STROKE_LINECAP_ROUND:
            butt = butt_round;
            break;
        default:
            butt = butt_straight;
            break;
    }

    Path *origin = new Path; // Fill
    Path *offset = new Path;

    Geom::Affine const transform(item->transform);
    double const scale = transform.descrim();

    origin->LoadPathVector(pathv);
    offset->SetBackData(false);

    if (!style->stroke_dasharray.values.empty() && style->stroke_dasharray.is_valid()) {
        // We have dashes!
        origin->ConvertWithBackData(0.005); // Approximate by polyline
        origin->DashPolyline(style->stroke_dasharray.get_computed(), style->stroke_dashoffset.computed, scale, 0);
        auto bounds = Geom::bounds_fast(pathv);
        if (bounds) {
            double size = Geom::L2(bounds->dimensions());
            origin->Simplify(size * 0.000005); // Polylines to Beziers
        }
    }

    // Finally do offset!
    origin->Outline(offset, 0.5 * stroke_width, join, butt, 0.5 * miter);

    if (bbox_only) {
        stroke = offset->MakePathVector();
    } else {
        // Clean-up shape

        offset->ConvertWithBackData(1.0); // Approximate by polyline

        Shape *theShape  = new Shape;
        offset->Fill(theShape, 0); // Convert polyline to shape, step 1.

        Shape *theOffset = new Shape;
        theOffset->ConvertToShape(theShape, fill_positive); // Create an intersection free polygon (theOffset), step2.
        theOffset->ConvertToForme(origin, 1, &offset); // Turn shape into contour (stored in origin).

        stroke = origin->MakePathVector(); // Note origin was replaced above by stroke!
    }

    delete origin;
    delete offset;

    // std::cout << "    fill:   " << sp_svg_write_path(fill)   << "  count: " << fill.curveCount() << std::endl;
    // std::cout << "    stroke: " << sp_svg_write_path(stroke) << "  count: " << stroke.curveCount() << std::endl;
    return true;
}


// ======================== Item to Outline ===================== //

static
void item_to_outline_add_marker_child( SPItem const *item, Geom::Affine marker_transform, Geom::PathVector* pathv_in )
{
    Geom::Affine tr(marker_transform);
    tr = item->transform * tr;

    // note: a marker child item can be an item group!
    if (is<SPGroup>(item)) {
        // recurse through all childs:
        for (auto& o: item->children) {
            if (auto childitem = cast<SPItem>(&o)) {
                item_to_outline_add_marker_child(childitem, tr, pathv_in);
            }
        }
    } else {
        Geom::PathVector* marker_pathv = item_to_outline(item);

        if (marker_pathv) {
            for (const auto & j : *marker_pathv) {
                pathv_in->push_back(j * tr);
            }
            delete marker_pathv;
        }
    }
}

/**
 *  Returns a pathvector that is the outline of the stroked item, with markers.
 *  item must be an SPShape or an SPText.
 *  The only current use of this function has exclude_markers true! (SPShape::either_bbox).
 *  TODO: See if SPShape::either_bbox's union with markers is the same as one would get
 *  with bbox_only false.
 */
Geom::PathVector* item_to_outline(SPItem const *item, bool exclude_markers)
{
    Geom::PathVector fill;   // Used for locating markers.
    Geom::PathVector stroke; // Used for creating outline (and finding bbox).
    item_find_paths(item, fill, stroke, true); // Skip cleaning up stroke shape.

    Geom::PathVector *ret_pathv = nullptr;

    if (fill.curveCount() == 0) {
        std::cerr << "item_to_outline: fill path has no segments!" << std::endl;
        return ret_pathv;
    }

    if (stroke.size() > 0) {
        ret_pathv = new Geom::PathVector(stroke);
    } else {
        // No stroke, use fill path.
        ret_pathv = new Geom::PathVector(fill);
    }

    if (exclude_markers) {
        return ret_pathv;
    }

    auto shape = cast<SPShape>(item);
    if (shape && shape->hasMarkers()) {
        for (auto const &[_, marker, tr] : shape->get_markers()) {
            if (auto const marker_item = sp_item_first_item_child(marker)) {
                item_to_outline_add_marker_child(marker_item, marker->c2p * tr, ret_pathv);
            }
        }
    }

    return ret_pathv;
}

// ========================= Stroke to Path ====================== //
// Read the original subtree before any bake can replace its members. A group
// with effects cannot be baked around excluded descendants: those effects own
// the entire subtree, so conservatively exclude that group too.
bool item_to_paths_preflight(SPItem *item, bool legacy, StrokeToPathConversion &conversion)
{
    auto exclude = [&](char const *reason) {
        conversion.excluded.insert(item->getId() ? item->getId() : "");
        conversion.exclusions.emplace_back(std::string(item->getId() ? item->getId() : "") + ": " + reason);
        g_warning("Stroke to Path: excluded '%s': %s", item->getId() ? item->getId() : "", reason);
        return false;
    };
    for (auto object = static_cast<SPObject *>(item); object; object = object->parent) {
        if (auto ancestor = cast<SPItem>(object)) {
            if (ancestor->isLocked() || ancestor->isHidden() ||
                (ancestor->style && ancestor->style->visibility.computed != SP_CSS_VISIBILITY_VISIBLE)) {
                return exclude("hidden or locked");
            }
        }
    }
    if (auto use = cast<SPUse>(item)) {
        if (!conversion.unlink_clones) return exclude("clone unlinking is disabled");
        auto source = use->get_original();
        // A nested reference graph needs its own admission model. Refuse it
        // before unlink can copy protected or unsupported source members.
        if (!source || is<SPUse>(source)) return exclude("missing or chained clone source");
        StrokeToPathConversion source_plan;
        if (!item_to_paths_preflight(source, legacy, source_plan) || !source_plan.exclusions.empty()) {
            return exclude("clone source contains protected or unsupported members");
        }
        return true;
    }
    if (auto lpe = cast<SPLPEItem>(item); lpe && lpe->hasPathEffect()) {
        for (auto const &ref : lpe->getEffectList()) {
            auto repr = ref->lpeobject_repr;
            if (!repr) return exclude("unavailable path effect");
            // Do not call parameter satellite getters here: some write XML.
            // Linked/satellite effects can mutate objects outside this subtree.
            for (auto const &attr : repr->attributeList()) {
                std::string key = g_quark_to_string(attr.key);
                std::string value = attr.value.pointer();
                if (value.find('#') != std::string::npos ||
                    (!value.empty() && (key.find("linked") != std::string::npos ||
                                       key.find("satellite") != std::string::npos || key == "href"))) {
                    return exclude("linked or satellite path effect requires broader ownership");
                }
            }
        }
    }
    // Text spans and box faces are not independent editable members. Any
    // protected internal node refuses conversion of the entire compound item.
    if (is<SPText>(item) || is<SPFlowtext>(item) || is<SPBox3D>(item)) {
        std::vector<SPObject *> descendants = item->childList(false);
        for (size_t i = 0; i < descendants.size(); ++i) {
            auto child = descendants[i];
            auto children = child->childList(false);
            descendants.insert(descendants.end(), children.begin(), children.end());
            auto repr = child->getRepr();
            auto style = child->style;
            if (g_strcmp0(repr->attribute("sodipodi:insensitive"), "true") == 0 ||
                (style && (style->display.computed == SP_CSS_DISPLAY_NONE ||
                           style->visibility.computed != SP_CSS_VISIBILITY_VISIBLE))) {
                return exclude("compound object contains protected members");
            }
        }
    }
    if (legacy && !is<SPShape>(item)) return exclude("unsupported in legacy mode");
    if (auto group = cast<SPGroup>(item); group && !is<SPBox3D>(item)) {
        auto const excluded_before = conversion.exclusions.size();
        bool excluded_child = false;
        bool eligible_child = false;
        for (auto child : group->item_list()) {
            if (item_to_paths_preflight(child, legacy, conversion)) eligible_child = true;
            else excluded_child = true;
        }
        if ((excluded_child || conversion.exclusions.size() != excluded_before) && group->hasPathEffect()) return exclude("group path effect contains excluded members");
        return eligible_child || group->hasPathEffect();
    }
    if (!is<SPShape>(item) && !is<SPText>(item) && !is<SPFlowtext>(item) && !is<SPBox3D>(item)) {
        return exclude("unsupported object type");
    }
    return true;
}

// Called only after every root was admitted and the caller acquired its fence.
// Unlink all eligible instances before converting any selected source.
bool item_to_paths_unlink(SPItem *item, StrokeToPathConversion &conversion)
{
    if (conversion.excluded.count(item->getId() ? item->getId() : "")) return false;
    if (auto use = cast<SPUse>(item)) {
        if (!conversion.unlink_clones) return false;
        if (!use->unlink()) { conversion.failed = true; return false; }
        return true;
    }
    bool changed = false;
    if (auto group = cast<SPGroup>(item); group && !is<SPBox3D>(item)) {
        for (auto child : group->item_list()) {
            changed = item_to_paths_unlink(child, conversion) || changed;
            if (conversion.failed) break;
        }
    }
    return changed;
}

static Inkscape::XML::Node *item_to_paths_impl(SPItem *, bool, SPItem *, StrokeToPathConversion &);

static void item_to_paths_add_marker(SPItem *context, SPMarker const *marker, Geom::Affine const &marker_transform,
                                     Inkscape::XML::Node *g_repr, bool legacy, StrokeToPathConversion &conversion)
{
    auto doc = context->document;
    for (auto &obj : marker->children) {
        if (auto item = cast<SPItem>(&obj)) {
            // NOTE: The SVG spec says that a <marker> cannot have a transform attribute, even if it's set, it should be ignored.
            // The SPMarker in Inkscape inherits from SPGroup so it does allow a transform, even though it shouldn't.
            auto const tr = item->transform * marker_transform;

            Inkscape::XML::Node *m_repr = obj.getRepr()->duplicate(doc->getReprDoc());
            g_repr->appendChild(m_repr);

            if (auto m_item = cast<SPItem>(doc->getObjectByRepr(m_repr))) {
                m_item->doWriteTransform(tr);
                if (!legacy) {
                    item_to_paths_impl(m_item, legacy, context, conversion);
                }
            }
        }
    }
}

// Stroke outlines have a different geometric bbox from their centreline. A
// bbox-relative clip/mask therefore needs an equivalent user-space resource.
// Share the original resource's children via <use>, and reuse each mapping;
// never duplicate the clipping geometry or change the shared source's units.
static std::string item_to_paths_resource(SPItem *item, SPObject *resource,
                                         Geom::OptRect const &bounds, bool mask, StrokeToPathConversion &conversion)
{
    auto source = resource->getRepr();
    bool const content_bbox = mask ? cast<SPMask>(resource)->mask_content_units()
                                   : cast<SPClipPath>(resource)->clippath_units();
    bool const region_bbox = mask && g_strcmp0(source->attribute("maskUnits"), "userSpaceOnUse") != 0;
    if ((!content_bbox && !region_bbox) || !bounds) {
        return std::string("url(#") + resource->getId() + ')';
    }

    auto doc = item->document;
    auto defs = doc->getDefs()->getRepr();
    auto const mapping = Geom::Scale(bounds->dimensions()) * Geom::Translate(bounds->min());
    std::ostringstream number;
    number.imbue(std::locale::classic());
    number << std::setprecision(std::numeric_limits<double>::max_digits10);
    number << "matrix(" << mapping[0] << ",0,0," << mapping[3] << ',' << mapping[4] << ',' << mapping[5] << ')';
    auto const key = number.str();
    auto const cache_key = std::string(resource->getId()) + '\n' + key;
    if (auto found = conversion.resources.find(cache_key); found != conversion.resources.end()) {
        return found->second;
    }

    auto xml_doc = doc->getReprDoc();
    auto mapped = xml_doc->createElement(source->name());
    for (auto const &attr : source->attributeList()) {
        auto name = g_quark_to_string(attr.key);
        if (g_strcmp0(name, "id") != 0) mapped->setAttribute(name, attr.value.pointer());
    }
    mapped->setAttribute(mask ? "maskContentUnits" : "clipPathUnits", "userSpaceOnUse");
    if (region_bbox) {
        mapped->setAttribute("maskUnits", "userSpaceOnUse");
        char const *names[] = {"x", "y", "width", "height"};
        double const defaults[] = {-0.1, -0.1, 1.2, 1.2};
        for (unsigned i = 0; i < 4; ++i) {
            SVGLength length;
            double const value = length.read(source->attribute(names[i])) ? length.value : defaults[i];
            number.str(""); number.clear();
            number << value * bounds->dimensions()[i % 2] + (i < 2 ? bounds->min()[i] : 0);
            mapped->setAttribute(names[i], number.str());
        }
    }
    for (auto &child : resource->children) {
        if (!is<SPItem>(&child)) continue;
        if (!child.getId()) child.setAttribute("id", child.generate_unique_id());
        auto use = xml_doc->createElement("svg:use");
        auto original = cast<SPItem>(&child);
        auto transform = content_bbox ? mapping : Geom::Affine{};
        if (auto reference = cast<SPUse>(original)) {
            // Duplicate the existing use, not a use OF that use. Fold its x/y
            // into transform too: the clip geometry reader resolves one level
            // and does not apply use x/y itself.
            Inkscape::GC::release(use);
            use = reference->getRepr()->duplicate(xml_doc);
            use->setAttribute("id", nullptr);
            // The duplicate has a new ID; freeze the use's own cascade so an
            // ID selector on the original use does not disappear with that ID.
            auto css = sp_css_attr_from_style(reference->style, SP_STYLE_FLAG_ALWAYS);
            sp_repr_css_change(use, css, "style");
            sp_repr_css_attr_unref(css);
            use->setAttribute("x", nullptr);
            use->setAttribute("y", nullptr);
            transform = reference->get_parent_transform() * transform;
            original = reference->get_original();
            std::unordered_set<SPItem *> seen{reference};
            // Only flatten intermediate uses with purely geometric attributes;
            // styles/compositing there require keeping their existing semantics.
            while (auto nested = cast<SPUse>(original)) {
                bool plain = seen.insert(nested).second &&
                             nested->style->write(SP_STYLE_FLAG_IFSET).empty();
                for (auto const &attr : nested->getRepr()->attributeList()) {
                    auto name = g_quark_to_string(attr.key);
                    if (g_strcmp0(name, "id") && g_strcmp0(name, "xlink:href") &&
                        g_strcmp0(name, "href") && g_strcmp0(name, "x") &&
                        g_strcmp0(name, "y") && g_strcmp0(name, "transform")) plain = false;
                }
                if (!plain || !nested->get_original()) break;
                transform = nested->get_parent_transform() * transform;
                original = nested->get_original();
            }
        }
        if (original && original->getId()) {
            use->setAttribute("xlink:href", std::string("#") + original->getId());
            use->setAttribute("href", nullptr);
        }
        number.str(""); number.clear();
        number << "matrix(";
        for (unsigned i = 0; i < 6; ++i) number << (i ? "," : "") << transform[i];
        number << ')';
        use->setAttribute("transform", number.str());
        mapped->appendChild(use);
        Inkscape::GC::release(use);
    }
    defs->appendChild(mapped);
    std::string const uri = std::string("url(#") + mapped->attribute("id") + ')';
    Inkscape::GC::release(mapped);
    conversion.resources.emplace(cache_key, uri);
    return uri;
}

static void item_to_paths_compositing(SPItem *item, Inkscape::XML::Node *out,
                                      Geom::OptRect const &bounds, StrokeToPathConversion &conversion)
{
    // Generated paths and marker groups use the item's local coordinates. Put
    // compositing on the final root, next to its transform, exactly once.
    for (auto key : {"clip-path", "mask"}) {
        auto resource = g_strcmp0(key, "mask") == 0 ? static_cast<SPObject *>(item->getMaskObject())
                                                   : static_cast<SPObject *>(item->getClipObject());
        if (resource) {
            auto const uri = item_to_paths_resource(item, resource, bounds, g_strcmp0(key, "mask") == 0, conversion);
            out->setAttribute(key, uri);
            if (uri != std::string("url(#") + resource->getId() + ')') {
                // Retained groups can also specify these properties inline.
                // Keep that declaration in sync so reopen cannot restore the
                // bbox-relative reference over the mapped presentation attribute.
                auto css = sp_repr_css_attr(out, "style");
                if (auto declaration = sp_repr_css_property(css, key, nullptr)) {
                    auto const value = uri + (g_str_has_suffix(declaration, "!important") ? " !important" : "");
                    sp_repr_css_set_property(css, key, value.c_str());
                    sp_repr_css_change(out, css, "style");
                }
                sp_repr_css_attr_unref(css);
            }
        } else {
            out->setAttribute(key, item->getRepr()->attribute(key));
        }
    }
}

/*
 * Find an outline that represents an item.
 * If legacy, text will not be handled as it is not a shape.
 * If a new item is created it is returned.
 * If the input item is a group and that group contains a changed item, the group node is returned
 * (marking a change).
 *
 * The return value is used externally to update a selection. It is nullptr if no change is made.
 */
Inkscape::XML::Node*
item_to_paths_impl(SPItem *item, bool legacy, SPItem *context, StrokeToPathConversion &conversion)
{
    if (conversion.failed || conversion.excluded.count(item->getId() ? item->getId() : "")) return nullptr;
    std::string const id = item->getId() ? item->getId() : "";
    SPDocument *doc = item->document;
    auto const original_bounds = item->geometricBounds();
    bool flatten = false;
    // flatten all paths effects
    auto lpeitem = cast<SPLPEItem>(item);
    if (lpeitem && lpeitem->hasPathEffect()) {
        lpeitem->removeAllPathEffects(true);
        // removeAllPathEffects may replace OR delete its holder. Never keep
        // the pre-bake pointer merely because lookup failed.
        item = id.empty() ? nullptr : cast<SPItem>(doc->getObjectById(id));
        if (!item) {
            conversion.failed = true;
            return nullptr;
        }
        auto elemref = item;
        auto flat_item = cast<SPLPEItem>(elemref);
        if (!flat_item || !flat_item->hasPathEffect()) {
            flatten = true;
        }
    }
    // convert text/3dbox to path
    if (is<SPText>(item) || is<SPFlowtext>(item) || is<SPBox3D>(item)) {
        if (legacy) {
            return nullptr;
        }

        Inkscape::ObjectSet original_objects {doc}; // doc or desktop shouldn't be necessary
        original_objects.add(item);
        Inkscape::SPWeakPtr<SPItem> original_weak(item);
        original_objects.toCurves(true);
        SPItem * new_item = original_objects.singleItem();
        if (Inkscape::PathOutline::replacement_succeeded(item, !original_weak, new_item)) {
            flatten = true;
            item = new_item;
            // Text conversion may change the geometric bbox before recursion.
            item_to_paths_compositing(item, item->getRepr(), original_bounds, conversion);
        } else {
            conversion.failed = true;
            g_warning("item_to_paths: flattening text or 3D box failed.");
            return nullptr;
        }
    }
    // if group, recurse
    auto group = cast<SPGroup>(item);
    if (group) {
        if (legacy) {
            return nullptr;
        }
        std::vector<std::string> child_ids;
        for (auto child : group->item_list()) child_ids.emplace_back(child->getId() ? child->getId() : "");
        bool did = false;
        for (auto const &child_id : child_ids) {
            auto subitem = cast<SPItem>(doc->getObjectById(child_id));
            if (!subitem) { conversion.failed = true; return nullptr; }
            if (item_to_paths_impl(subitem, legacy, nullptr, conversion)) {
                did = true;
            }
            if (conversion.failed) return nullptr;
        }
        if (did || flatten) {
            // Descendant outlines can change the group bbox too.
            item_to_paths_compositing(group, group->getRepr(), original_bounds, conversion);
            // This indicates that at least one thing was changed inside the group.
            return group->getRepr();
        } else {
            return nullptr;
        }
    }

    auto shape = cast<SPShape>(item);
    if (!shape) {
        return nullptr;
    }

    Geom::PathVector fill_path;
    Geom::PathVector stroke_path;
    bool status = item_find_paths(item, fill_path, stroke_path);

    if (!status) {
        // Was not a well structured shape (or text).
        return nullptr;
    }

    // The styles ------------------------

    // Copying stroke style to fill will fail for properties not defined by style attribute
    // (i.e., properties defined in style sheet or by attributes).
    SPStyle *style = item->style;
    SPCSSAttr *ncss = sp_css_attr_from_style(style, SP_STYLE_FLAG_ALWAYS);
    SPCSSAttr *ncsf = sp_css_attr_from_style(style, SP_STYLE_FLAG_ALWAYS);

    if (context) {
        SPCSSAttr *ctxt_style = sp_css_attr_from_style(context->style, SP_STYLE_FLAG_ALWAYS);

        // TODO: browsers have different behaviours with context on markers
        // we need to revisit in the future for best matching
        // also dont know if opacity is or should be included in context
        gchar const *s_val   = sp_repr_css_property(ctxt_style, "stroke", nullptr);
        gchar const *f_val   = sp_repr_css_property(ctxt_style, "fill", nullptr);
        if (style->fill.paintOrigin == SP_CSS_PAINT_ORIGIN_CONTEXT_STROKE ||
            style->fill.paintOrigin == SP_CSS_PAINT_ORIGIN_CONTEXT_FILL) 
        {
            gchar const *fill_value = (style->fill.paintOrigin == SP_CSS_PAINT_ORIGIN_CONTEXT_STROKE) ? s_val : f_val;
            sp_repr_css_set_property(ncss, "fill", fill_value);
            sp_repr_css_set_property(ncsf, "fill", fill_value);
        }
        if (style->stroke.paintOrigin == SP_CSS_PAINT_ORIGIN_CONTEXT_STROKE ||
            style->stroke.paintOrigin == SP_CSS_PAINT_ORIGIN_CONTEXT_FILL) 
        {
            gchar const *stroke_value = (style->stroke.paintOrigin == SP_CSS_PAINT_ORIGIN_CONTEXT_FILL) ? f_val : s_val;
            sp_repr_css_set_property(ncss, "stroke", stroke_value);
            sp_repr_css_set_property(ncsf, "stroke", stroke_value);
        }
    }
    // Stroke
    
    gchar const *s_val   = sp_repr_css_property(ncss, "stroke", nullptr);
    gchar const *s_opac  = sp_repr_css_property(ncss, "stroke-opacity", nullptr);
    gchar const *f_val   = sp_repr_css_property(ncss, "fill", nullptr);
    SPCSSAttr *r_style = sp_repr_css_attr_new();
    sp_repr_css_set_property(r_style, "opacity", sp_repr_css_property(ncss, "opacity", nullptr));
    sp_repr_css_set_property(r_style, "filter", sp_repr_css_property(ncss, "filter", nullptr));
    SPIPaintOrder temp;
    temp.read(sp_repr_css_property(ncss, "paint-order", nullptr));

    sp_repr_css_set_property(ncss, "stroke", "none");
    sp_repr_css_set_property(ncss, "stroke-width", nullptr);
    sp_repr_css_set_property(ncss, "stroke-opacity", "1.0");
    sp_repr_css_set_property(ncss, "filter", nullptr);
    sp_repr_css_set_property(ncss, "opacity", nullptr);
    sp_repr_css_unset_property(ncss, "marker-start");
    sp_repr_css_unset_property(ncss, "marker-mid");
    sp_repr_css_unset_property(ncss, "marker-end");

    // we change the stroke to fill on ncss to create the filled stroke
    sp_repr_css_set_property(ncss, "fill", s_val);
    if ( s_opac ) {
        sp_repr_css_set_property(ncss, "fill-opacity", s_opac);
    } else {
        sp_repr_css_set_property(ncss, "fill-opacity", "1.0");
    }
    
    sp_repr_css_set_property(ncsf, "stroke", "none");
    sp_repr_css_set_property(ncsf, "stroke-width", nullptr);
    sp_repr_css_set_property(ncsf, "stroke-opacity", "1.0");
    sp_repr_css_set_property(ncsf, "filter", nullptr);
    sp_repr_css_set_property(ncsf, "opacity", nullptr);
    sp_repr_css_unset_property(ncsf, "marker-start");
    sp_repr_css_unset_property(ncsf, "marker-mid");
    sp_repr_css_unset_property(ncsf, "marker-end");

    // The object tree -------------------

    // Remember the position of the item
    gint pos = item->getRepr()->position();

    // Remember parent
    Inkscape::XML::Node *parent = item->getRepr()->parent();

    Inkscape::XML::Document *xml_doc = doc->getReprDoc();

    // Create a group to put everything in.
    Inkscape::XML::Node *g_repr = xml_doc->createElement("svg:g");

    Inkscape::copy_object_properties(g_repr, item->getRepr());
    // drop copied style, children will be re-styled (stroke becomes fill)
    g_repr->removeAttribute("style");

    // Add the group to the parent, move to the saved position
    parent->addChildAtPos(g_repr, pos);

    // The stroke ------------------------
    Inkscape::XML::Node *stroke = nullptr;
    if (s_val && g_strcmp0(s_val,"none") != 0 && stroke_path.size() > 0) {
        auto stroke_style = std::make_unique<SPStyle>(doc);
        stroke_style->mergeCSS(ncss);

        stroke = xml_doc->createElement("svg:path");
        stroke->setAttribute("style", stroke_style->writeIfDiff(item->parent->style));
        stroke->setAttribute("d", sp_svg_write_path(stroke_path));
    }
    sp_repr_css_attr_unref(ncss);

    // The fill --------------------------
    Inkscape::XML::Node *fill = nullptr;
    if (f_val && g_strcmp0(f_val,"none") != 0 && !legacy) {
        auto fill_style = std::make_unique<SPStyle>(doc);
        fill_style->mergeCSS(ncsf);

        fill = xml_doc->createElement("svg:path");
        fill->setAttribute("style", fill_style->writeIfDiff(item->parent->style));
        fill->setAttribute("d", sp_svg_write_path(fill_path));
    }
    sp_repr_css_attr_unref(ncsf);

    // The markers -----------------------
    Inkscape::XML::Node *markers = nullptr;

    if (shape->hasMarkers()) {
        if (!legacy) {
            markers = xml_doc->createElement("svg:g");
            g_repr->addChildAtPos(markers, pos);
        } else {
            markers = g_repr;
        }

        for (auto const &[_, marker, tr] : shape->get_markers()) {
            item_to_paths_add_marker(item, marker, marker->c2p * tr, markers, legacy, conversion);
        }
    }

    bool unique = false;
    if ((!fill && !markers) || (!fill && !stroke) || (!markers && !stroke)) {
        unique = true;
    }
    if (temp.layer[0] != SP_CSS_PAINT_ORDER_NORMAL && !legacy && !unique) {

        if (temp.layer[0] == SP_CSS_PAINT_ORDER_FILL) {
            if (temp.layer[1] == SP_CSS_PAINT_ORDER_STROKE) {
                if ( fill ) {
                    g_repr->appendChild(fill);
                }
                if ( stroke ) {
                    g_repr->appendChild(stroke);
                }
                if ( markers ) {
                    markers->setPosition(2);
                }
            } else {
                if ( fill ) {
                    g_repr->appendChild(fill);
                }
                if ( markers ) {
                    markers->setPosition(1);
                }
                if ( stroke ) {
                    g_repr->appendChild(stroke);
                }
            }
        } else if (temp.layer[0] == SP_CSS_PAINT_ORDER_STROKE) {
            if (temp.layer[1] == SP_CSS_PAINT_ORDER_FILL) {
                if ( stroke ) {
                    g_repr->appendChild(stroke);
                }
                if ( fill ) {
                    g_repr->appendChild(fill);
                }
                if ( markers ) {
                    markers->setPosition(2);
                }
            } else {
                if ( stroke ) {
                    g_repr->appendChild(stroke);
                }
                if ( markers ) {
                    markers->setPosition(1);
                }
                if ( fill ) {
                    g_repr->appendChild(fill);
                }
            }
        } else {
            if (temp.layer[1] == SP_CSS_PAINT_ORDER_STROKE) {
                if ( markers ) {
                    markers->setPosition(0);
                }
                if ( stroke ) {
                    g_repr->appendChild(stroke);
                }
                if ( fill ) {
                    g_repr->appendChild(fill);
                }
            } else {
                if ( markers ) {
                    markers->setPosition(0);
                }
                if ( fill ) {
                    g_repr->appendChild(fill);
                }
                if ( stroke ) {
                    g_repr->appendChild(stroke);
                }
            }
        }

    } else if (!unique) {
        if ( fill ) {
            g_repr->appendChild(fill);
        }
        if ( stroke ) {
            g_repr->appendChild(stroke);
        }
        if ( markers ) {
            markers->setPosition(2);
        }
    }

    bool did = false;
    // only consider it a change if more than a fill is created.
    if (stroke || markers) {
        did = true;
    }

    Inkscape::XML::Node *out = nullptr;

    if (!fill && !markers && did) {
        out = stroke;
    } else if (!fill && !stroke  && did) {
        out = markers;
    } else if(did) {
        out = g_repr;
    } else {
        parent->removeChild(g_repr);
        Inkscape::GC::release(g_repr);
        if (fill) {
            // Copy the style, to preserve context-fill cascade
            if (context) {
                item->setAttribute("style", fill->attribute("style"));
            }
            Inkscape::GC::release(fill);
        }
        sp_repr_css_attr_unref(r_style);
        return (flatten ? item->getRepr() : nullptr);
    }

    item_to_paths_compositing(item, out, original_bounds, conversion);
    sp_repr_css_change(out, r_style, "style");

    sp_repr_css_attr_unref(r_style);
    if (unique && out != g_repr) { // Promote the final root, including a markers-only group.
        if (out->parent()) out->parent()->removeChild(out);
        parent->addChild(out, g_repr);
        parent->removeChild(g_repr);
        Inkscape::GC::release(g_repr);
    }
    out->setAttribute("transform", item->getRepr()->attribute("transform"));

    // We're replacing item, delete it.
    item->deleteObject(false);

    out->setAttribute("id",id);
    Inkscape::GC::release(out);

    return out;
}

Inkscape::XML::Node *item_to_paths_apply(SPItem *item, bool legacy, StrokeToPathConversion &conversion)
{
    return item_to_paths_impl(item, legacy, nullptr, conversion);
}

Inkscape::XML::Node *item_to_paths(SPItem *item, bool legacy, SPItem *context)
{
    StrokeToPathConversion conversion;
    if (!item || !item_to_paths_preflight(item, legacy, conversion)) return nullptr;
    auto doc = item->document;
    // This protects this root and all its recursive children. The ObjectSet
    // caller must supply the whole-selection fence before unlinking/iteration.
    auto fence = Inkscape::DocumentUndo::detachPendingChanges(doc);
    if (!fence && Inkscape::DocumentUndo::getUndoSensitive(doc)) {
        g_warning("Stroke to Path: cannot acquire rollback fence");
        return nullptr;
    }
    Inkscape::XML::Node *result = nullptr;
    try {
        result = item_to_paths_impl(item, legacy, context, conversion);
    } catch (...) {
        if (fence) Inkscape::DocumentUndo::rollbackToDetachedChanges(doc, *fence);
        throw;
    }
    if (conversion.failed) {
        if (fence) Inkscape::DocumentUndo::rollbackToDetachedChanges(doc, *fence);
        g_warning("Stroke to Path: conversion failed%s", fence ? "; restored this root" : "; non-undoable caller must discard its projection");
        return nullptr;
    }
    if (fence) Inkscape::DocumentUndo::reattachPendingChanges(doc, *fence);
    return result;
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
