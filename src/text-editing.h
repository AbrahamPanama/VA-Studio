// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef SEEN_SP_TEXT_EDITING_H
#define SEEN_SP_TEXT_EDITING_H

/*
 * Text editing functions common for for text and flowtext
 *
 * Authors:
 *   bulia byak
 *   Richard Hughes
 *
 * Copyright (C) 2004-5 authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include <utility> // std::pair

#include "libnrtype/Layout-TNG.h"
#include "text-tag-attributes.h"

class SPCSSAttr;
class SPDesktop;
class SPItem;
class SPObject;
class SPStyle;

typedef std::pair<Inkscape::Text::Layout::iterator, Inkscape::Text::Layout::iterator> iterator_pair; 


Inkscape::Text::Layout const * te_get_layout (SPItem const *item);

void te_update_layout_now_recursive(SPItem *item);

/** Returns true if there are no visible characters on the canvas. */
bool sp_te_output_is_empty(SPItem const *item);

/** Returns true if the user has typed nothing in the text box. */
bool sp_te_input_is_empty(SPObject const *item);

/** Recursively gets the length of all the SPStrings at or below the given
\a item. Also adds 1 for each line break encountered. */
unsigned sp_text_get_length(SPObject const *item);

/** Recursively gets the length of all the SPStrings at or below the given
\a item, before and not including \a upto. Also adds 1 for each line break encountered. */
unsigned sp_text_get_length_upto(SPObject const *item, SPObject const *upto);

std::vector<Geom::Point> sp_te_create_selection_quads(SPItem const *item, Inkscape::Text::Layout::iterator start, Inkscape::Text::Layout::iterator end, Geom::Affine const &transform);

Inkscape::Text::Layout::iterator sp_te_get_position_by_coords (SPItem const *item, Geom::Point const &i_p);
void sp_te_get_cursor_coords (SPItem const *item, Inkscape::Text::Layout::iterator position, Geom::Point &p0, Geom::Point &p1);
double sp_te_get_average_linespacing (SPItem const *text);

SPStyle const * sp_te_style_at_position(SPItem const *text, Inkscape::Text::Layout::iterator position);
SPObject const * sp_te_object_at_position(SPItem const *text, Inkscape::Text::Layout::iterator position);

Inkscape::Text::Layout::iterator sp_te_insert(SPItem *item, Inkscape::Text::Layout::iterator position, char const *utf8);
Inkscape::Text::Layout::iterator sp_te_replace(SPItem *item, Inkscape::Text::Layout::iterator start, Inkscape::Text::Layout::iterator end, char const *utf8);

/**
 * Replace the range [start, end) with utf8 and, when css is non-null, apply that
 * style to exactly the inserted characters. The style is applied by character
 * index so it stays anchored across the layout update performed by the insertion.
 * Returns the iterator at the end of the inserted text.
 */
Inkscape::Text::Layout::iterator sp_te_replace_styled(SPItem *item, Inkscape::Text::Layout::iterator start,
                                                     Inkscape::Text::Layout::iterator end, char const *utf8,
                                                     SPCSSAttr const *css);

Inkscape::Text::Layout::iterator sp_te_insert_line (SPItem *text, Inkscape::Text::Layout::iterator &position,
                                                   bool carry_run_style = false);
bool sp_te_delete (SPItem *item, Inkscape::Text::Layout::iterator start, Inkscape::Text::Layout::iterator end, iterator_pair &iter_pair);

Glib::ustring sp_te_get_string_multiline(SPItem const *text);
Glib::ustring sp_te_get_string_multiline(SPItem const *text, Inkscape::Text::Layout::iterator start, Inkscape::Text::Layout::iterator end);
void sp_te_set_repr_text_multiline(SPItem *text, char const *str);

TextTagAttributes*
text_tag_attributes_at_position(SPItem *item, Inkscape::Text::Layout::iterator position, unsigned *char_index);

bool is_kerning_supported(SPItem const *text);

void sp_te_adjust_kerning_screen(SPItem *text, Inkscape::Text::Layout::iterator start, Inkscape::Text::Layout::iterator end, SPDesktop const *desktop, Geom::Point by);
void sp_te_adjust_dx (SPItem *item, Inkscape::Text::Layout::iterator start, Inkscape::Text::Layout::iterator end, SPDesktop *desktop, double delta);
void sp_te_adjust_dy (SPItem *item, Inkscape::Text::Layout::iterator start, Inkscape::Text::Layout::iterator end, SPDesktop *desktop, double delta);

void sp_te_adjust_rotation_screen(SPItem *text, Inkscape::Text::Layout::iterator start, Inkscape::Text::Layout::iterator end, SPDesktop *desktop, double pixels);
void sp_te_adjust_rotation(SPItem *text, Inkscape::Text::Layout::iterator start, Inkscape::Text::Layout::iterator end, SPDesktop *desktop, double degrees);
void sp_te_set_rotation(SPItem *text, Inkscape::Text::Layout::iterator start, Inkscape::Text::Layout::iterator end, SPDesktop *desktop, double degrees);

void sp_te_adjust_tspan_letterspacing_screen(SPItem *text, Inkscape::Text::Layout::iterator start, Inkscape::Text::Layout::iterator end, SPDesktop const *desktop, double by);
void sp_te_adjust_linespacing_screen(SPItem *text, Inkscape::Text::Layout::iterator start, Inkscape::Text::Layout::iterator end, SPDesktop const *desktop, double by);

/**
 * Opt-in declaration that selected properties in the CSS passed to
 * sp_te_apply_style() are already expressed in the target run's local units.
 *
 * Unit contract: sp_te_apply_style() merges the incoming CSS and inverse-scales
 * document-space lengths by the common ancestor's descrim, matching the native
 * style write. A caller that computed a length in local run units (for example
 * a percentage of the run's own font advance) opts in per property so the
 * engine restores that property verbatim after the inverse scale. A
 * default-constructed value, or any flag left false, changes nothing: every
 * property then stays in the engine's native document units.
 */
struct TextStyleLocalSpacing {
    bool letter_spacing = false;
    bool word_spacing = false;
};

/**
 * Opt-in declaration that selected stroke properties in the CSS passed to
 * sp_te_apply_style() are already expressed in the target run's local units.
 *
 * Unit contract: identical to TextStyleLocalSpacing, for stroke-width,
 * stroke-dasharray and stroke-dashoffset. The engine restores only the opted-in
 * property and only when the original input provided it, so an absent property
 * is never inserted or removed. A default-constructed value, or any flag left
 * false, keeps the native document-unit conversion. vector-effect is not
 * inferred or rewritten by the engine; a caller that must preserve a run's
 * non-inheriting convention includes the run's own full native vector-effect
 * value and priority in the input CSS.
 */
struct TextStyleLocalStroke {
    bool stroke_width = false;
    bool stroke_dasharray = false;
    bool stroke_dashoffset = false;

    /**
     * Opt-in traversal flag, unrelated to the local-unit contract above. When
     * true, an exclusive end that lands on a line-break object (for example
     * flowLine or flowRegionBreak) is left at that break instead of being
     * advanced to the next sibling, so the endpoint control is excluded from
     * the style write. Default false keeps the native advance and therefore the
     * existing behavior for every other caller.
     */
    bool exclude_end_line_break = false;
};

void sp_te_apply_style(SPItem *text, Inkscape::Text::Layout::iterator start, Inkscape::Text::Layout::iterator end, SPCSSAttr const *css,
                       TextStyleLocalSpacing const &local_spacing = {});

void sp_te_apply_style(SPItem *text, Inkscape::Text::Layout::iterator start, Inkscape::Text::Layout::iterator end, SPCSSAttr const *css,
                       TextStyleLocalSpacing const &local_spacing, TextStyleLocalStroke const &local_stroke);

bool is_part_of_text_subtree (SPObject const *obj);
bool is_top_level_text_object (SPObject const *obj);
bool has_visible_text (SPObject const *obj);

#endif // SEEN_SP_TEXT_EDITING_H
