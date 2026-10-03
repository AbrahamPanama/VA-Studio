// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * BRK-1: Break Apart like CorelDRAW. See text-break-apart.h.
 *
 * Copyright 2026 VA Studio authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "text-break-apart.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <string_view>

#include <glibmm/i18n.h>
#include <glibmm/ustring.h>

#include "desktop.h"
#include "document.h"
#include "document-undo.h"
#include "libnrtype/Layout-TNG.h"
#include "message-stack.h"
#include "object/object-set.h"
#include "object/sp-path.h"
#include "object/sp-string.h"
#include "object/sp-text.h"
#include "object/sp-textpath.h"
#include "object/sp-tspan.h"
#include "style.h"
#include "svg/stringstream.h"
#include "svg/svg.h"
#include "ui/icon-names.h"
#include "util-string/context-string.h"
#include "xml/document.h"
#include "xml/node.h"

namespace Inkscape::TextBreakApart {
namespace {

using Text::Layout;

/// A run of characters [first, last) of one text.
struct Range {
    Layout::iterator first;
    Layout::iterator last;
};

/// Whitespace, or a control character such as a line break (no glyph).
bool blank(Layout const &layout, Layout::iterator const &it)
{
    return !it.hasGlyph() || layout.isWhitespace(it);
}

/// Where the character's pen sits on its own baseline: characterAnchorPoint()
/// gives the pen x but puts y on the line's baseline without the span's y/dy
/// offset (CorelDRAW lines are dy offsets), so y comes from the span. Baseline
/// shift is left out: the piece's run style applies it again.
Geom::Point pen_origin(Layout const &layout, Layout::iterator const &it)
{
    auto const &glyph = layout.glyphs()[it.glyphIndex()];
    return {layout.characterAnchorPoint(it)[Geom::X], glyph.line(&layout).baseline_y + glyph.span(&layout).y_offset};
}

bool has_attribute_in_subtree(Inkscape::XML::Node const *node, char const *key)
{
    if (node->attribute(key)) return true;
    for (auto const *child = node->firstChild(); child; child = child->next()) {
        if (has_attribute_in_subtree(child, key)) return true;
    }
    return false;
}

/// True when \a it is the first character of a text chunk (a run started by
/// an absolute x or y position, as each CorelDRAW-imported line is).
bool starts_chunk(Layout::iterator const &it)
{
    auto start = it;
    start.thisStartOfChunk();
    return start == it;
}

bool supported(SPText const *text)
{
    if (!text || !text->style) return false;
    // Text on a path, vertical and right-to-left text: the pieces' positions
    // would not follow the simple left-to-right anchor rule below.
    for (auto const &child : text->children) {
        if (is<SPTextPath>(&child)) return false;
    }
    if (text->style->writing_mode.computed != SP_CSS_WRITING_MODE_LR_TB) return false;
    if (text->style->direction.computed != SP_CSS_DIRECTION_LTR) return false;
    // Rotated glyphs and textLength stretching are not reproduced by the pieces.
    auto const *repr = text->getRepr();
    if (has_attribute_in_subtree(repr, "rotate") || has_attribute_in_subtree(repr, "textLength")) return false;
    // Text flowed into a shape: characters that overflow the shape have no
    // glyph and would be lost with the pieces.
    if (text->has_shape_inside() || text->layout.inputTruncated()) return false;
    // Clones and other references would point at nothing; a clip or mask
    // would not follow the pieces.
    if (text->hrefcount > 0) return false;
    if (repr->attribute("clip-path") || repr->attribute("mask") || text->getClipObject() || text->getMaskObject()) {
        return false;
    }
    // Right-to-left runs inside the text.
    auto const &layout = text->layout;
    for (auto it = layout.begin(); it != layout.end(); it.nextCharacter()) {
        if (it.hasGlyph() && layout.glyphs()[it.glyphIndex()].span(&layout).direction != Layout::LEFT_TO_RIGHT) {
            return false;
        }
    }
    return true;
}

std::vector<Range> lines_of(Layout const &layout)
{
    // A visual line is a layout line; a chunk that starts on another baseline
    // (CorelDRAW writes each line as a tspan with its own x and dy, which the
    // layout keeps on one "line") also starts a new one. Per-glyph x lists on
    // one baseline (PDF imports) do not.
    std::vector<Range> ranges;
    auto it = layout.begin();
    if (it == layout.end()) return ranges;
    auto start = it;
    auto line = layout.lineIndex(it);
    bool have_y = it.hasGlyph();
    double y = have_y ? pen_origin(layout, it)[Geom::Y] : 0.0;
    while (it.nextCharacter() && it != layout.end()) {
        bool split = layout.lineIndex(it) != line;
        if (it.hasGlyph()) {
            double const cy = pen_origin(layout, it)[Geom::Y];
            if (!split && have_y && starts_chunk(it) && std::abs(cy - y) > 1e-3) split = true;
            y = cy;
            have_y = true;
        }
        if (split) {
            ranges.push_back({start, it});
            start = it;
            line = layout.lineIndex(it);
        }
    }
    ranges.push_back({start, layout.end()});
    // Only lines with something visible become pieces.
    std::erase_if(ranges, [&](Range const &r) {
        for (auto c = r.first; c != r.last; c.nextCharacter()) {
            if (c.hasGlyph() && !layout.isWhitespace(c)) return false;
        }
        return true;
    });
    return ranges;
}

/// Maximal runs of non-whitespace characters.
std::vector<Range> words_of(Layout const &layout)
{
    std::vector<Range> ranges;
    auto it = layout.begin();
    bool in_word = false;
    Layout::iterator start = it;
    for (; it != layout.end(); it.nextCharacter()) {
        bool const space = blank(layout, it);
        if (!in_word && !space) {
            start = it;
            in_word = true;
        } else if (in_word && space) {
            ranges.push_back({start, it});
            in_word = false;
        }
    }
    if (in_word) ranges.push_back({start, layout.end()});
    return ranges;
}

/// One range per grapheme cluster: a character plus the combining marks and
/// ligature partners that share its glyph or cannot stand alone.
std::vector<Range> letters_of(Layout const &layout)
{
    std::vector<Range> ranges;
    auto it = layout.begin();
    while (it != layout.end()) {
        if (blank(layout, it)) {
            it.nextCharacter();
            continue;
        }
        auto start = it;
        int const glyph = it.glyphIndex();
        auto next = it;
        while (next.nextCharacter() && next != layout.end() && !blank(layout, next) &&
               (next.glyphIndex() == glyph || !layout.isCursorPosition(next))) {
        }
        ranges.push_back({start, next});
        it = next;
    }
    return ranges;
}

bool has_manual_kerns(SPObject const *object)
{
    if (auto const *repr = object->getRepr()) {
        for (auto const *key : {"dx", "dy", "x", "y"}) {
            auto const *value = repr->attribute(key);
            if (!value) continue;
            // A single number positions the run; a list positions characters.
            if (std::strpbrk(value, " ,")) return true;
        }
    }
    for (auto const &child : object->children) {
        if (has_manual_kerns(&child)) return true;
    }
    return false;
}

Glib::ustring write_number(double value)
{
    Inkscape::SVGOStringStream os;
    os << value;
    return os.str();
}

/// Style of the piece's root: the text's own style, left-to-right and
/// start-anchored (the piece's x is its first character's anchor).
Glib::ustring root_style_text(SPText const *text, SPStyle &root_style)
{
    root_style.merge(text->style);
    root_style.text_anchor.read("start");
    root_style.text_align.read("start");
    root_style.writing_mode = SP_CSS_WRITING_MODE_LR_TB;
    root_style.direction = SP_CSS_DIRECTION_LTR;
    auto const *parent_style = text->parent ? text->parent->style : nullptr;
    auto css = root_style.writeIfDiff(parent_style);
    // SPStyle::merge carries inherited properties (and opacity, filter,
    // baseline-shift) only; keep the text's own compositing properties.
    for (auto const *property : {static_cast<SPIBase const *>(&text->style->mix_blend_mode),
                                 static_cast<SPIBase const *>(&text->style->isolation)}) {
        if (!property->set) continue;
        if (!css.empty() && css[css.length() - 1] != ';') css += ';';
        css += property->write(SP_STYLE_FLAG_IFSET, SPStyleSrc::STYLE_PROP, nullptr);
    }
    return css;
}

/// Drawn origin of every visible character of \a range: the glyph's own
/// transform, with y/dy offsets and baseline shift, as it is rendered.
std::vector<Geom::Point> drawn_origins(Layout const &layout, Range const &range)
{
    std::vector<Geom::Point> result;
    for (auto it = range.first; it != range.last; it.nextCharacter()) {
        if (!it.hasGlyph()) continue;
        result.push_back(layout.glyphs()[it.glyphIndex()].transform(layout).translation());
    }
    return result;
}

/// True when a character of \a range shares its glyph with the previous one
/// (a ligature) or cannot stand alone (a combining mark): per-character
/// positions would pull such glyphs apart.
bool has_shared_glyphs(Layout const &layout, Range const &range)
{
    int previous = -1;
    for (auto it = range.first; it != range.last; it.nextCharacter()) {
        if (!it.hasGlyph()) { previous = -1; continue; }
        if (it.glyphIndex() == previous || !layout.isCursorPosition(it)) return true;
        previous = it.glyphIndex();
    }
    return false;
}

/// Where an explicitly positioned character goes so that its glyph lands where
/// the original drew it: the drawn x (kerning and glyph offsets included) and
/// the pen's baseline y (the run style re-applies baseline shift).
Geom::Point explicit_origin(Layout const &layout, Layout::iterator const &it)
{
    auto const drawn = layout.glyphs()[it.glyphIndex()].transform(layout).translation();
    return {drawn[Geom::X], pen_origin(layout, it)[Geom::Y]};
}

/// True when the piece must position its characters explicitly: the source
/// text uses per-character lists (kerns, rotation, textLength), or the piece
/// spans more than one absolutely positioned chunk.
bool piece_needs_positions(Layout const &layout, Range const &range, bool kerns)
{
    if (kerns) return true;
    bool first = true;
    for (auto it = range.first; it != range.last; it.nextCharacter()) {
        if (!it.hasGlyph()) continue;
        if (!first && starts_chunk(it)) return true;
        first = false;
    }
    return false;
}

/// How a piece places its characters.
enum class Placement
{
    Flow,     ///< the first character is positioned; the rest flows as text
    Explicit, ///< every character (or cluster) where the original drew it
};

/// Create one piece for the characters [range.first, range.last). The node
/// carries one creation reference, which the caller releases.
Inkscape::XML::Node *make_piece(SPText *text, Layout const &layout, Range const &range, Placement placement,
                                Glib::ustring const &root_style, SPStyle const &root_sp_style)
{
    auto *xml_doc = text->document->getReprDoc();
    auto *node = xml_doc->createElement("svg:text");
    node->setAttribute("xml:space", "preserve");
    node->setAttributeOrRemoveIfEmpty("style", root_style);
    node->setAttributeOrRemoveIfEmpty("transform", text->getRepr()->attribute("transform"));
    for (auto const *key : {"xml:lang", "lang"}) {
        if (auto const *value = text->getRepr()->attribute(key)) node->setAttribute(key, value);
    }
    bool const explicit_positions = placement == Placement::Explicit;
    // A positioned character starts a new chunk, which would pull a ligature,
    // or a base and its marks, apart: with shared glyphs, whole clusters are
    // positioned instead, each in a tspan of its own that carries only the
    // position; the runs inside it keep their own styles (a mark coloured
    // apart from its base stays so).
    bool const per_cluster = explicit_positions && has_shared_glyphs(layout, range);

    // Characters grouped by their source string: one text node per run, so
    // kerning between neighbours is kept; a run whose style differs from the
    // piece's root gets a tspan with that difference.
    Glib::ustring xs, ys;
    bool anchored = false;
    SPObject *run_source = nullptr;
    Inkscape::XML::Node *container = node; // the piece, or the current cluster
    Inkscape::XML::Node *run_parent = node;
    Glib::ustring pending;
    int previous_glyph = -1;
    auto flush = [&] {
        if (pending.empty()) return;
        auto *content = xml_doc->createTextNode(pending.c_str());
        run_parent->appendChild(content);
        Inkscape::GC::release(content);
        pending.clear();
    };
    auto open_cluster = [&](Geom::Point const &position) {
        flush();
        container = xml_doc->createElement("svg:tspan");
        container->setAttribute("x", write_number(position[Geom::X]));
        container->setAttribute("y", write_number(position[Geom::Y]));
        node->appendChild(container);
        Inkscape::GC::release(container);
        run_parent = container;
        run_source = nullptr; // the cluster's first character opens its run
    };
    auto open_run = [&](SPObject *source) {
        flush();
        run_source = source;
        SPStyle run_style(text->document);
        for (auto *sp = source->parent; sp && sp != text; sp = sp->parent) {
            run_style.merge(sp->style);
        }
        // Chunk-level properties belong to the piece's root, never to a run.
        run_style.text_anchor.read("start");
        run_style.text_align.read("start");
        run_style.writing_mode = SP_CSS_WRITING_MODE_LR_TB;
        run_style.direction = SP_CSS_DIRECTION_LTR;
        auto const run_css = run_style.writeIfDiff(&root_sp_style);
        if (run_css.empty()) {
            run_parent = container;
            return;
        }
        run_parent = xml_doc->createElement("svg:tspan");
        run_parent->setAttribute("style", run_css);
        container->appendChild(run_parent);
        Inkscape::GC::release(run_parent);
    };
    for (auto it = range.first; it != range.last; it.nextCharacter()) {
        if (!it.hasGlyph()) { // a line break: the piece ends here anyway
            previous_glyph = -1;
            continue;
        }
        SPObject *source = nullptr;
        layout.getSourceOfCharacter(it, &source);
        if (!source) continue;
        // A cluster: a character with a glyph of its own, with the ligature
        // partners and marks that follow it.
        bool const cluster_start = !anchored || (it.glyphIndex() != previous_glyph && layout.isCursorPosition(it));
        previous_glyph = it.glyphIndex();
        auto const point = explicit_positions ? explicit_origin(layout, it) : pen_origin(layout, it);
        if (!anchored) {
            node->setAttribute("x", write_number(point[Geom::X]));
            node->setAttribute("y", write_number(point[Geom::Y]));
            anchored = true;
        }
        if (per_cluster) {
            if (cluster_start) open_cluster(point);
        } else if (explicit_positions) {
            if (!xs.empty()) { xs += ' '; ys += ' '; }
            xs += write_number(point[Geom::X]);
            ys += write_number(point[Geom::Y]);
        }
        if (source != run_source) open_run(source);
        pending.push_back(layout.characterAt(it));
    }
    flush();
    if (explicit_positions && !per_cluster) {
        node->setAttribute("x", xs);
        node->setAttribute("y", ys);
    }
    return node;
}

} // namespace

Level level_of(SPText const *text)
{
    if (!supported(text)) return Level::None;
    auto const &layout = text->layout;
    if (layout.begin() == layout.end()) return Level::None;
    if (lines_of(layout).size() > 1) return Level::Lines;
    if (words_of(layout).size() > 1) return Level::Words;
    if (letters_of(layout).size() > 1) return Level::Letters;
    return Level::None;
}

std::vector<SPItem *> break_one_level(SPText *text)
{
    std::vector<SPItem *> pieces;
    auto const level = level_of(text);
    if (level == Level::None) return pieces;
    auto const &layout = text->layout;
    auto const ranges = level == Level::Lines ? lines_of(layout)
                      : level == Level::Words ? words_of(layout)
                                              : letters_of(layout);
    bool const kerns = has_manual_kerns(text);
    SPStyle root_sp_style(text->document);
    auto const root_style = root_style_text(text, root_sp_style);

    auto *document = text->document;
    auto *parent = text->parent->getRepr();

    // Each piece in its first form, plus, for a piece that flows, an
    // explicitly positioned variant: re-laying out a piece as ordinary text
    // can move a glyph at its edge by a fraction of a pixel (a kerning pair it
    // no longer has). Both are built while the text's layout is intact.
    struct Candidate
    {
        std::vector<Geom::Point> expected; // where the original drew each character
        Inkscape::XML::Node *node = nullptr;
        Inkscape::XML::Node *fallback = nullptr;
    };
    std::vector<Candidate> candidates;
    for (auto const &range : ranges) {
        bool const explicit_positions = piece_needs_positions(layout, range, kerns);
        Candidate candidate;
        candidate.expected = drawn_origins(layout, range);
        candidate.node = make_piece(text, layout, range, explicit_positions ? Placement::Explicit : Placement::Flow,
                                    root_style, root_sp_style);
        if (!explicit_positions) {
            candidate.fallback = make_piece(text, layout, range, Placement::Explicit, root_style, root_sp_style);
        }
        candidates.push_back(std::move(candidate));
    }
    auto release_all = [&] {
        for (auto &candidate : candidates) {
            Inkscape::GC::release(candidate.node);
            if (candidate.fallback) Inkscape::GC::release(candidate.fallback);
        }
    };
    // A new text's first layout precedes its resolved style (em offsets, font
    // features): request a style-level update before reading it.
    auto lay_out = [&](std::vector<Inkscape::XML::Node *> const &nodes) {
        for (auto *node : nodes) {
            if (auto *object = document->getObjectByRepr(node)) {
                object->requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG | SP_OBJECT_STYLE_MODIFIED_FLAG);
            }
        }
        document->ensureUpToDate();
    };

    // Trial, outside the history: lay every piece out next to the text and
    // compare its glyphs with the original drawing. A piece that differs takes
    // its explicit variant; if any piece still differs, the text is left as it
    // is (no piece, no history entry) and the caller reports it.
    bool accepted = true;
    {
        DocumentUndo::ScopedInsensitive trial(document);
        auto matches = [&](Candidate const &candidate) {
            auto *piece = cast<SPText>(document->getObjectByRepr(candidate.node));
            if (!piece) return false;
            auto const &piece_layout = piece->layout;
            auto const drawn = drawn_origins(piece_layout, {piece_layout.begin(), piece_layout.end()});
            if (drawn.size() != candidate.expected.size()) return false;
            for (std::size_t j = 0; j < drawn.size(); ++j) {
                if (Geom::distance(drawn[j], candidate.expected[j]) > 1e-2) return false;
            }
            return true;
        };
        auto *after = text->getRepr();
        std::vector<Inkscape::XML::Node *> placed;
        for (auto &candidate : candidates) {
            parent->addChild(candidate.node, after);
            after = candidate.node;
            placed.push_back(candidate.node);
        }
        lay_out(placed);
        std::vector<Inkscape::XML::Node *> replaced;
        for (auto &candidate : candidates) {
            if (!candidate.fallback || matches(candidate)) continue;
            parent->addChild(candidate.fallback, candidate.node);
            parent->removeChild(candidate.node);
            std::swap(candidate.node, candidate.fallback); // the flowing form is now the spare
            replaced.push_back(candidate.node);
        }
        if (!replaced.empty()) lay_out(replaced);
        accepted = std::all_of(candidates.begin(), candidates.end(), matches);
        for (auto &candidate : candidates) parent->removeChild(candidate.node);
    }
    if (!accepted) {
        release_all();
        return pieces;
    }

    // Commit, recorded for Undo: the verified pieces replace the text, in order.
    std::string const id = text->getRepr()->attribute("id") ? text->getRepr()->attribute("id") : "";
    auto *after = text->getRepr();
    std::vector<Inkscape::XML::Node *> nodes;
    for (auto &candidate : candidates) {
        parent->addChild(candidate.node, after);
        after = candidate.node;
        nodes.push_back(candidate.node);
    }
    text->deleteObject(false);
    // Like the path Break Apart, the first piece keeps the original id.
    if (!id.empty() && !nodes.empty()) nodes.front()->setAttribute("id", id);
    lay_out(nodes);
    for (auto *node : nodes) {
        if (auto *item = cast<SPItem>(document->getObjectByRepr(node))) pieces.push_back(item);
    }
    release_all();
    return pieces;
}

Outcome break_apart(ObjectSet &set)
{
    Outcome outcome;
    auto *document = set.document();
    if (!document) return outcome;
    auto const items = set.items_vector();
    std::vector<SPItem *> selection;
    std::vector<SPItem *> paths;
    for (auto *item : items) {
        if (auto *text = cast<SPText>(item)) {
            bool const breakable = level_of(text) != Level::None;
            auto pieces = break_one_level(text);
            if (pieces.empty()) {
                ++outcome.skipped;
                if (breakable) ++outcome.unmatched; // refused by the trial
                selection.push_back(item);
            } else {
                ++outcome.texts;
                selection.insert(selection.end(), pieces.begin(), pieces.end());
                outcome.pieces.insert(outcome.pieces.end(), pieces.begin(), pieces.end());
            }
        } else if (auto *path = cast<SPPath>(item)) {
            auto const *curve = path->curveForEdit();
            if (curve && curve->size() > 1) {
                paths.push_back(item);
            } else {
                selection.push_back(item); // one subpath: nothing to break, not a skip
            }
        } else {
            ++outcome.skipped;
            selection.push_back(item);
        }
    }
    // One set per path: ObjectSet::breakApart replaces the set's contents with
    // each path's pieces in turn, so a shared set keeps only the last path's.
    for (auto *path : paths) {
        ObjectSet path_set(document);
        path_set.set(path);
        path_set.breakApart(/*skip_undo=*/true, /*overlapping=*/true, /*silent=*/true);
        auto pieces = path_set.items_vector();
        ++outcome.paths;
        selection.insert(selection.end(), pieces.begin(), pieces.end());
        outcome.pieces.insert(outcome.pieces.end(), pieces.begin(), pieces.end());
    }
    if (outcome.texts == 0 && outcome.paths == 0) {
        if (auto *desktop = set.desktop()) {
            desktop->messageStack()->flash(Inkscape::WARNING_MESSAGE,
                outcome.unmatched
                    ? _("Break Apart: the pieces would not sit exactly where the text draws them; the text was left as it is.")
                    : _("Break Apart: nothing to break in the selection (select a text or a path)."));
        }
        return outcome;
    }
    set.setList(selection);
    DocumentUndo::done(document, RC_("Undo", "Break apart"), INKSCAPE_ICON("path-break-apart"));
    if (auto *desktop = set.desktop()) {
        Glib::ustring message;
        if (outcome.texts) {
            message += Glib::ustring::compose(ngettext("%1 text broken apart", "%1 texts broken apart", outcome.texts), outcome.texts);
        }
        if (outcome.paths) {
            if (!message.empty()) message += "; ";
            message += Glib::ustring::compose(ngettext("%1 path broken into subpaths", "%1 paths broken into subpaths", outcome.paths), outcome.paths);
        }
        if (outcome.skipped) {
            message += "; ";
            message += Glib::ustring::compose(ngettext("%1 object left as it is", "%1 objects left as they are", outcome.skipped), outcome.skipped);
        }
        if (outcome.unmatched) {
            message += " ";
            message += Glib::ustring::compose(ngettext("(%1 text whose pieces would not match its drawing)",
                                                       "(%1 texts whose pieces would not match their drawing)", outcome.unmatched),
                                              outcome.unmatched);
        }
        desktop->messageStack()->flash(outcome.skipped ? Inkscape::WARNING_MESSAGE : Inkscape::NORMAL_MESSAGE, message);
    }
    return outcome;
}

} // namespace Inkscape::TextBreakApart
