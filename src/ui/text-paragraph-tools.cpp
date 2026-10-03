// SPDX-License-Identifier: GPL-2.0-or-later

#include "text-paragraph-tools.h"

#include <cmath>
#include <iostream>
#include <string>

#include <glibmm/unicode.h>
#include <glibmm/ustring.h>

#include "document.h"
#include "object/sp-flowdiv.h"
#include "object/sp-flowtext.h"
#include "object/sp-item.h"
#include "object/sp-object.h"
#include "object/sp-string.h"
#include "object/sp-text.h"
#include "object/sp-tspan.h"
#include "style.h"
#include "text-style-units.h"
#include "util/cast.h"
#include "util/delete-with.h"
#include "xml/document.h"
#include "xml/node.h"
#include "xml/repr.h"

namespace Inkscape::UI {
namespace {

constexpr char marker_attribute[] = "inkscape:list-marker";
constexpr char style_attribute[] = "inkscape:list-style";
constexpr char drop_cap_attribute[] = "inkscape:drop-cap";
constexpr char drop_cap_lines_attribute[] = "inkscape:drop-cap-lines";

XML::Node const *markerNode(XML::Node const *paragraph)
{
    for (auto child = paragraph ? paragraph->firstChild() : nullptr; child; child = child->next()) {
        if (auto value = child->attribute(marker_attribute); value && std::string_view{value} == "true") {
            return child;
        }
        if (child->type() == XML::NodeType::ELEMENT_NODE) {
            if (auto marker = markerNode(child)) return marker;
        }
    }
    return nullptr;
}

XML::Node *markerNode(XML::Node *paragraph)
{
    return const_cast<XML::Node *>(markerNode(static_cast<XML::Node const *>(paragraph)));
}

char const *modeName(TextListMode mode)
{
    switch (mode) {
        case TextListMode::Bulleted: return "bullet";
        case TextListMode::Numbered: return "number";
        case TextListMode::None: return nullptr;
    }
    return nullptr;
}

std::string markerText(TextListMode mode, unsigned number)
{
    if (mode == TextListMode::Bulleted) return "•\u00a0";
    if (mode == TextListMode::Numbered) return std::to_string(number) + ".\u00a0";
    return {};
}

XML::Node *findDropCap(XML::Node *parent)
{
    for (auto child = parent ? parent->firstChild() : nullptr; child; child = child->next()) {
        if (auto value = child->attribute(drop_cap_attribute);
            value && std::string_view{value} == "true") return child;
        if (child->type() == XML::NodeType::ELEMENT_NODE &&
            !(child->attribute(marker_attribute) || child->attribute("inkscape:auto-hyphen"))) {
            if (auto found = findDropCap(child)) return found;
        }
    }
    return nullptr;
}

XML::Node *findFirstText(XML::Node *parent)
{
    for (auto child = parent ? parent->firstChild() : nullptr; child; child = child->next()) {
        if (child->type() == XML::NodeType::TEXT_NODE && child->content() && *child->content()) {
            return child;
        }
        if (child->type() == XML::NodeType::ELEMENT_NODE &&
            !(child->attribute(marker_attribute) || child->attribute("inkscape:auto-hyphen"))) {
            if (auto found = findFirstText(child)) return found;
        }
    }
    return nullptr;
}

bool removeDropCap(XML::Node *paragraph)
{
    auto wrapper = findDropCap(paragraph);
    if (!wrapper || !wrapper->parent()) return false;
    auto parent = wrapper->parent();
    auto cursor = wrapper->prev();
    std::vector<Glib::ustring> contents;
    for (auto child = wrapper->firstChild(); child; child = child->next()) {
        if (child->type() == XML::NodeType::TEXT_NODE && child->content()) {
            contents.emplace_back(child->content());
        }
    }
    parent->removeChild(wrapper);
    for (auto const &content : contents) {
        auto text = parent->document()->createTextNode(content.c_str());
        parent->addChild(text, cursor);
        cursor = text;
        GC::release(text);
    }
    return true;
}

// Any text alignment in, right/center/left/justify out.
SPCSSTextAlign text_align_to_side(SPCSSTextAlign const &align, SPCSSDirection const &direction)
{
    auto new_align = align;

    if ((align == SP_CSS_TEXT_ALIGN_START && direction == SP_CSS_DIRECTION_LTR) ||
        (align == SP_CSS_TEXT_ALIGN_END   && direction == SP_CSS_DIRECTION_RTL)) {
        new_align = SP_CSS_TEXT_ALIGN_LEFT;
    }

    if ((align == SP_CSS_TEXT_ALIGN_START && direction == SP_CSS_DIRECTION_RTL) ||
        (align == SP_CSS_TEXT_ALIGN_END   && direction == SP_CSS_DIRECTION_LTR)) {
        new_align = SP_CSS_TEXT_ALIGN_RIGHT;
    }

    return new_align;
}

// Single source of truth for the alignment pair produced by a native integer
// mode. Both the CSS builder and the no-op comparison read this table so they
// cannot drift apart.
struct AlignmentCssValues {
    SPCSSTextAlign align;
    SPTextAnchor anchor;
    char const *align_name;
    char const *anchor_name;
};

AlignmentCssValues alignment_css_values(int align_mode, int direction)
{
    if (align_mode == 1) {
        return {SP_CSS_TEXT_ALIGN_CENTER, SP_CSS_TEXT_ANCHOR_MIDDLE, "center", "middle"};
    }
    if (align_mode == 3) {
        return {SP_CSS_TEXT_ALIGN_JUSTIFY, SP_CSS_TEXT_ANCHOR_START, "justify", "start"};
    }

    bool const right = (align_mode == 0 && direction == SP_CSS_DIRECTION_RTL) ||
                       (align_mode == 2 && direction == SP_CSS_DIRECTION_LTR);
    if (right) {
        return {SP_CSS_TEXT_ALIGN_END, SP_CSS_TEXT_ANCHOR_END, "end", "end"};
    }
    return {SP_CSS_TEXT_ALIGN_START, SP_CSS_TEXT_ANCHOR_START, "start", "start"};
}

// True when an element authors an alignment override, either as a style
// property or as a presentation attribute.
bool has_authored_alignment(XML::Node const *repr)
{
    if (!repr) return false;
    if (repr->attribute("text-align") || repr->attribute("text-anchor")) return true;
    auto css = sp_repr_css_attr(repr, "style");
    bool const authored = sp_repr_css_property(css, "text-align", nullptr) ||
                          sp_repr_css_property(css, "text-anchor", nullptr);
    sp_repr_css_attr_unref(css);
    return authored;
}

bool has_authored_paragraph(SPObject const &object)
{
    for (auto const &child : object.children) {
        if (auto tspan = cast<SPTSpan>(&child);
            tspan && tspan->role == SP_TSPAN_ROLE_PARAGRAPH) {
            return true;
        }
        if (has_authored_paragraph(child)) return true;
    }
    return false;
}

// Descendant normalization applies only to ordinary object-scoped text. Wrapped
// text and authored role=paragraph structures keep their own scope.
bool is_ordinary_alignment_text(SPText const &text)
{
    return !text.has_inline_size() && !text.has_shape_inside() &&
           !has_authored_paragraph(text);
}

bool has_conflicting_descendant_alignment(SPObject const &object,
                                          SPCSSTextAlign desired_align,
                                          SPTextAnchor desired_anchor)
{
    for (auto const &child : object.children) {
        if (child.style && has_authored_alignment(child.getRepr()) &&
            (child.style->text_align.value != desired_align ||
             child.style->text_anchor.value != desired_anchor)) {
            return true;
        }
        if (has_conflicting_descendant_alignment(child, desired_align, desired_anchor)) {
            return true;
        }
    }
    return false;
}

// Remove only the two alignment properties so descendants inherit the root's
// object-scoped alignment; style, geometry, role and group data are untouched.
bool clear_conflicting_descendant_alignment(SPObject &object,
                                            SPCSSTextAlign desired_align,
                                            SPTextAnchor desired_anchor)
{
    bool changed = false;
    for (auto &child : object.children) {
        if (child.style && has_authored_alignment(child.getRepr()) &&
            (child.style->text_align.value != desired_align ||
             child.style->text_anchor.value != desired_anchor)) {
            auto css = sp_repr_css_attr_new();
            sp_repr_css_unset_property(css, "text-align");
            sp_repr_css_unset_property(css, "text-anchor");
            child.changeCSS(css, "style");
            sp_repr_css_attr_unref(css);
            if (auto repr = child.getRepr()) {
                repr->setAttribute("text-align", nullptr);
                repr->setAttribute("text-anchor", nullptr);
            }
            changed = true;
        }
        changed |= clear_conflicting_descendant_alignment(child, desired_align, desired_anchor);
    }
    return changed;
}

// Move repr out of its parent to just before prevchild under its grandparent,
// duplicating it and giving the copy sodipodi:role="line". Mirrors the toolbar's
// former TextToolbar::unindent_node cleanup.
XML::Node *unindentNode(SPDocument &doc, XML::Node *repr, XML::Node *prevchild)
{
    g_assert(repr != nullptr);

    XML::Node *parent = repr->parent();
    if (parent) {
        XML::Node *grandparent = parent->parent();
        if (grandparent) {
            XML::Document *xml_doc = doc.getReprDoc();
            XML::Node *newrepr = repr->duplicate(xml_doc);
            parent->removeChild(repr);
            grandparent->addChild(newrepr, prevchild);
            GC::release(newrepr);
            newrepr->setAttribute("sodipodi:role", "line");
            return newrepr;
        }
    }
    std::cerr << "unindentNode error: node has no (grand)parent, nothing done.\n";
    return repr;
}

} // namespace

TextListMode paragraphListMode(SPObject const &paragraph)
{
    auto value = paragraph.getRepr() ? paragraph.getRepr()->attribute(style_attribute) : nullptr;
    if (!value) return TextListMode::None;
    if (std::string_view{value} == "bullet") return TextListMode::Bulleted;
    if (std::string_view{value} == "number") return TextListMode::Numbered;
    return TextListMode::None;
}

unsigned paragraphListStart(SPObject const &paragraph)
{
    if (paragraphListMode(paragraph) != TextListMode::Numbered) return 1;
    auto marker = markerNode(paragraph.getRepr());
    auto content = marker && marker->firstChild() ? marker->firstChild()->content() : nullptr;
    if (!content) return 1;
    try {
        auto const parsed = std::stoul(content);
        return parsed > 0 ? static_cast<unsigned>(parsed) : 1;
    } catch (...) {
        return 1;
    }
}

bool setParagraphListMode(SPDocument &document,
                          std::vector<SPObject *> const &paragraphs,
                          TextListMode mode,
                          unsigned start)
{
    auto xml = document.getReprDoc();
    if (!xml) return false;

    bool changed = false;
    unsigned number = start;
    for (auto paragraph : paragraphs) {
        auto repr = paragraph ? paragraph->getRepr() : nullptr;
        if (!repr) continue;

        auto marker = markerNode(repr);
        auto const desired = markerText(mode, number);
        auto current_mode = paragraphListMode(*paragraph);
        auto current_text = marker && marker->firstChild() && marker->firstChild()->content()
                          ? std::string{marker->firstChild()->content()} : std::string{};

        if (mode == TextListMode::None) {
            if (marker) {
                marker->parent()->removeChild(marker);
                changed = true;
            }
            if (repr->attribute(style_attribute)) {
                repr->setAttribute(style_attribute, nullptr);
                changed = true;
            }
            continue;
        }

        if (!marker) {
            marker = xml->createElement("svg:tspan");
            marker->setAttribute(marker_attribute, "true");
            auto text = xml->createTextNode(desired.c_str());
            marker->addChild(text, nullptr);
            repr->addChild(marker, nullptr);
            GC::release(text);
            GC::release(marker);
            changed = true;
        } else if (current_text != desired) {
            if (marker->firstChild()) {
                marker->firstChild()->setContent(desired.c_str());
            } else {
                auto text = xml->createTextNode(desired.c_str());
                marker->addChild(text, nullptr);
                GC::release(text);
            }
            changed = true;
        }

        if (current_mode != mode) {
            repr->setAttribute(style_attribute, modeName(mode));
            changed = true;
        }
        ++number;
    }
    return changed;
}

unsigned paragraphDropCapLines(SPObject const &paragraph)
{
    auto value = const_cast<SPObject &>(paragraph).getRepr()->attribute(drop_cap_lines_attribute);
    if (!value) return 0;
    try {
        auto parsed = std::stoul(value);
        return parsed >= 2 ? static_cast<unsigned>(parsed) : 0;
    } catch (...) {
        return 0;
    }
}

bool setParagraphDropCapLines(SPDocument &document,
                              std::vector<SPObject *> const &paragraphs,
                              unsigned lines)
{
    auto xml = document.getReprDoc();
    if (!xml) return false;
    bool changed = false;
    for (auto paragraph : paragraphs) {
        auto repr = paragraph ? paragraph->getRepr() : nullptr;
        if (!repr) continue;
        changed |= removeDropCap(repr);

        if (lines >= 2) {
            auto source = findFirstText(repr);
            if (!source) continue;
            Glib::ustring content{source->content()};
            unsigned cluster_end = 1;
            while (cluster_end < content.size() && g_unichar_ismark(content[cluster_end])) ++cluster_end;
            auto first = content.substr(0, cluster_end);
            auto rest = content.substr(cluster_end);
            auto parent = source->parent();
            auto cursor = source->prev();
            parent->removeChild(source);

            auto wrapper = xml->createElement("svg:tspan");
            wrapper->setAttribute(drop_cap_attribute, "true");
            auto style = "font-size:" + std::to_string(lines * 100) +
                         "%;line-height:0;baseline-shift:-" +
                         std::to_string((lines - 1) * 45) + "%";
            wrapper->setAttribute("style", style.c_str());
            auto text = xml->createTextNode(first.c_str());
            wrapper->addChild(text, nullptr);
            parent->addChild(wrapper, cursor);
            cursor = wrapper;
            GC::release(text);
            GC::release(wrapper);
            if (!rest.empty()) {
                auto remainder = xml->createTextNode(rest.c_str());
                parent->addChild(remainder, cursor);
                GC::release(remainder);
            }
            repr->setAttributeInt(drop_cap_lines_attribute, lines);
            changed = true;
        } else if (repr->attribute(drop_cap_lines_attribute)) {
            repr->setAttribute(drop_cap_lines_attribute, nullptr);
            changed = true;
        }
    }
    return changed;
}

SPCSSAttr *cssForTextAlignment(int align_mode, int direction)
{
    auto const values = alignment_css_values(align_mode, direction);
    auto css = sp_repr_css_attr_new();
    sp_repr_css_set_property(css, "text-anchor", values.anchor_name);
    sp_repr_css_set_property(css, "text-align", values.align_name);
    return css;
}

bool applyNativeTextAlignment(SPText &text, int align_mode)
{
    // Below, variable names suggest horizontal move, but we check the writing direction
    // and move in the corresponding axis.
    Geom::Dim2 axis;
    unsigned writing_mode = text.style->writing_mode.value;
    if (writing_mode == SP_CSS_WRITING_MODE_LR_TB || writing_mode == SP_CSS_WRITING_MODE_RL_TB) {
        axis = Geom::X;
    } else {
        axis = Geom::Y;
    }

    // Find current text inline-size (width for horizontal text, height for vertical text.
    Geom::OptRect bbox = text.get_frame(); // 'inline-size' or rectangle frame.
    if (!bbox) {
        bbox = text.geometricBounds();
    }
    if (!bbox) {
        return false; // No bounding box, no joy!
    }
    double width = bbox->dimensions()[axis];

    double move = 0;
    auto direction = text.style->direction.value;

    // Switch alignment point
    auto old_side = text_align_to_side(text.style->text_align.value, direction);
    switch (old_side) {
        case SP_CSS_TEXT_ALIGN_LEFT:
            switch (align_mode) {
                case 0:
                    break;
                case 1:
                    move = width/2;
                    break;
                case 2:
                    move = width;
                    break;
                case 3:
                    break; // Justify
                default:
                    std::cerr << "applyNativeTextAlignment() Unexpected value (mode): " << align_mode << std::endl;
            }
            break;
        case SP_CSS_TEXT_ALIGN_CENTER:
            switch (align_mode) {
                case 0:
                    move = -width/2;
                    break;
                case 1:
                    break;
                case 2:
                    move = width/2;
                    break;
                case 3:
                    break; // Justify
                default:
                    std::cerr << "applyNativeTextAlignment() Unexpected value (mode): " << align_mode << std::endl;
            }
            break;
        case SP_CSS_TEXT_ALIGN_RIGHT:
            switch (align_mode) {
                case 0:
                    move = -width;
                    break;
                case 1:
                    move = -width/2;
                    break;
                case 2:
                    break;
                case 3:
                    break; // Justify
                default:
                    std::cerr << "applyNativeTextAlignment() Unexpected value (mode): " << align_mode << std::endl;
            }
            break;
        case SP_CSS_TEXT_ALIGN_JUSTIFY:
            // Do nothing
            break;
        default:
            std::cerr << "applyNativeTextAlignment() Unexpected value (old_side): " << old_side << std::endl;
    }

    bool const position_changed = std::abs(move) > 0;

    auto const desired = alignment_css_values(align_mode, direction);
    bool const css_changed = text.style->text_align.value != desired.align ||
                             text.style->text_anchor.value != desired.anchor;

    bool const ordinary = is_ordinary_alignment_text(text);
    bool const descendant_conflict = ordinary &&
        has_conflicting_descendant_alignment(text, desired.align, desired.anchor);

    // An identical request must be a true no-op: do not rewrite CSS, geometry,
    // repr or display state.
    if (!position_changed && !css_changed && !descendant_conflict) {
        return false;
    }

    if (css_changed) {
        auto css = cssForTextAlignment(align_mode, direction);
        text.changeCSS(css, "style");
        sp_repr_css_attr_unref(css);
    }

    if (ordinary) {
        clear_conflicting_descendant_alignment(text, desired.align, desired.anchor);
    }

    if (position_changed) {
        Geom::Point XY = text.attributes.firstXY();
        if (axis == Geom::X) {
            XY = XY + Geom::Point (move, 0);
        } else {
            XY = XY + Geom::Point (0, move);
        }
        text.attributes.setFirstXY(XY);
    }
    text.updateRepr();
    text.requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG);

    return true;
}

void setTextRootStyleAndUnsetDescendants(SPObject &object, SPCSSAttr *css, bool unset_descendants)
{
    object.changeCSS(css, "style");

    auto css_unset = sp_repr_css_attr_unset_all(css);
    for (auto child : object.childList(false)) {
        setTextRootStyleAndUnsetDescendants(*child, unset_descendants ? css_unset : css);
    }
    sp_repr_css_attr_unref(css_unset);
}

void applyDocumentTextStyle(SPItem &item, SPCSSAttr *css)
{
    // Scale by inverse of accumulated parent transform.
    SPCSSAttr *css_set = sp_repr_css_attr_new();
    sp_repr_css_merge(css_set, css);
    auto const local = item.i2doc_affine();
    double const ex = local.descrim();
    if (ex != 0.0 && ex != 1.0) {
        sp_css_attr_scale(css_set, 1 / ex);
    }
    setTextRootStyleAndUnsetDescendants(item, css_set);
    sp_repr_css_attr_unref(css_set);
}

void prepareInnerText(SPItem &item, Text::Layout::iterator &start, Text::Layout::iterator &end)
{
    auto const layout = te_get_layout(&item);
    if (!layout) {
      return;
    }
    auto doc = item.document;
    auto spobject = &item;
    auto spitem = &item;
    auto text = cast<SPText>(&item);
    auto flowtext = cast<SPFlowtext>(&item);
    XML::Document *xml_doc = doc->getReprDoc();

    // We check for external files with text nodes direct children of text element
    // and wrap it into a tspan elements as inkscape do.
    if (text) {
        bool changed = false;
        std::vector<SPObject *> childs = spitem->childList(false);
        for (auto child : childs) {
            auto spstring = cast<SPString>(child);
            if (spstring) {
                Glib::ustring content = spstring->string;
                if (content != "\n") {
                    XML::Node *rstring = xml_doc->createTextNode(content.c_str());
                    XML::Node *rtspan  = xml_doc->createElement("svg:tspan");
                    //XML::Node *rnl     = xml_doc->createTextNode("\n");
                    rtspan->setAttribute("sodipodi:role", "line");
                    rtspan->addChild(rstring, nullptr);
                    text->getRepr()->addChild(rtspan, child->getRepr());
                    GC::release(rstring);
                    GC::release(rtspan);
                    text->getRepr()->removeChild(spstring->getRepr());
                    changed = true;
                }
            }
        }
        if (changed) {
            // proper rebuild happens later,
            // this just updates layout to use now, avoids use after free
            text->rebuildLayout();
        }
    }

    std::vector<SPObject *> containers;
    {
        // populate `containers` with objects that will be modified.

        // Temporarily remove the shape so Layout calculates
        // the position of wrap_end and wrap_start, even if
        // one of these are hidden because the previous line height was changed
        if (text) {
            text->hide_shape_inside();
        } else if (flowtext) {
            flowtext->fix_overflow_flowregion(false);
        }
        SPObject *rawptr_start = nullptr;
        SPObject *rawptr_end = nullptr;
        layout->validateIterator(&start);
        layout->validateIterator(&end);
        layout->getSourceOfCharacter(start, &rawptr_start);
        layout->getSourceOfCharacter(end, &rawptr_end);
        if (text) {
            text->show_shape_inside();
        } else if (flowtext) {
            flowtext->fix_overflow_flowregion(true);
        }
        if (!rawptr_start || !rawptr_end) {
            return;
        }

        // Loop through parents of start and end till we reach
        // first children of the text element.
        // Get all objects between start and end (inclusive)
        SPObject *startobj = rawptr_start;
        SPObject *endobj   = rawptr_end;
        while (startobj->parent != spobject) {
            startobj = startobj->parent;
        }
        while (endobj->parent != spobject) {
            endobj = endobj->parent;
        }

        while (startobj && startobj != endobj) {
            containers.push_back(startobj);
            startobj = startobj->getNext();
        }
        if (startobj) {
            containers.push_back(startobj);
        }
    }

    for (auto container : containers) {
        XML::Node *prevchild = container->getRepr();
        std::vector<SPObject*> childs = container->childList(false);
        for (auto child : childs) {
            auto spstring = cast<SPString>(child);
            auto flowtspan = cast<SPFlowtspan>(child);
            auto tspan = cast<SPTSpan>(child);
            // we need to upper all flowtspans to container level
            // to do this we need to change the element from flowspan to flowpara
            if (flowtspan) {
                XML::Node *flowpara = xml_doc->createElement("svg:flowPara");
                std::vector<SPObject*> fts_childs = flowtspan->childList(false);
                bool hascontent = false;
                // we need to move the contents to the new created element
                // maybe we can move directly but it is safer for me to duplicate,
                // inject into the new element and delete original
                for (auto fts_child : fts_childs) {
                    // is this check necessary?
                    if (fts_child) {
                        XML::Node *fts_child_node = fts_child->getRepr()->duplicate(xml_doc);
                        flowtspan->getRepr()->removeChild(fts_child->getRepr());
                        flowpara->addChild(fts_child_node, nullptr);
                        GC::release(fts_child_node);
                        hascontent = true;
                    }
                }
                // if no contents we dont want to add
                if (hascontent) {
                    flowpara->setAttribute("style", flowtspan->getRepr()->attribute("style"));
                    spobject->getRepr()->addChild(flowpara, prevchild);
                    GC::release(flowpara);
                    prevchild = flowpara;
                }
                container->getRepr()->removeChild(flowtspan->getRepr());
            } else if (tspan) {
                if (child->childList(false).size()) {
                    child->getRepr()->setAttribute("sodipodi:role", "line");
                    // maybe we need to move unindent function here
                    // to be the same as other here
                    prevchild = unindentNode(*doc, child->getRepr(), prevchild);
                } else {
                    // if no contents we dont want to add
                    container->getRepr()->removeChild(child->getRepr());
                }
            } else if (spstring) {
                // we are on a text node, we act different if in a text or flowtext.
                // wrap a duplicate of the element and unindent after the prevchild
                // and finally delete original
                XML::Node *string_node = xml_doc->createTextNode(spstring->string.c_str());
                if (text) {
                    XML::Node *tspan_node = xml_doc->createElement("svg:tspan");
                    tspan_node->setAttribute("style", container->getRepr()->attribute("style"));
                    tspan_node->addChild(string_node, nullptr);
                    tspan_node->setAttribute("sodipodi:role", "line");
                    text->getRepr()->addChild(tspan_node, prevchild);
                    GC::release(string_node);
                    GC::release(tspan_node);
                    prevchild = tspan_node;
                } else if (flowtext) {
                    XML::Node *flowpara_node = xml_doc->createElement("svg:flowPara");
                    flowpara_node->setAttribute("style", container->getRepr()->attribute("style"));
                    flowpara_node->addChild(string_node, nullptr);
                    flowtext->getRepr()->addChild(flowpara_node, prevchild);
                    GC::release(string_node);
                    GC::release(flowpara_node);
                    prevchild = flowpara_node;
                }
                container->getRepr()->removeChild(spstring->getRepr());
            }
        }
        spitem->getRepr()->removeChild(container->getRepr());
    }
}

namespace {

// An explicit descendant line-height whose physical local height sits above the
// root floor. The value is captured before setTextRootStyleAndUnsetDescendants
// flattens the subtree, then re-applied so only inherited/below-floor heights
// take the native root value.
struct AuthoredLineHeightOverride {
    SPObject *object;
    Glib::ustring value;
};

void collectAuthoredLineHeightOverrides(SPObject &object,
                                        double root_floor,
                                        std::vector<AuthoredLineHeightOverride> &overrides)
{
    // `set && !inherit` covers both inline style and presentation attribute
    // sources; line-heightFromStyle + convertLineHeight map the authored value
    // to physical local units, including unitless multipliers.
    SPStyle *style = object.style;
    if (style && style->line_height.set && !style->line_height.inherit) {
        double const authored = convertLineHeight(
            lineHeightFromStyle(*style), TextLineHeightUnit::Px,
            style->font_size.computed).value;
        if (authored > root_floor) {
            auto css = Util::delete_with<sp_repr_css_attr_unref>(
                sp_css_attr_from_style(style, SP_STYLE_FLAG_IFSET));
            Glib::ustring value = sp_repr_css_property(css.get(), "line-height", "");
            if (!value.empty()) {
                overrides.push_back({&object, value});
            }
        }
    }

    for (auto child : object.childList(false)) {
        if (child) {
            collectAuthoredLineHeightOverrides(*child, root_floor, overrides);
        }
    }
}

void restoreAuthoredLineHeightOverrides(std::vector<AuthoredLineHeightOverride> const &overrides)
{
    for (auto const &entry : overrides) {
        auto css = Util::delete_with<sp_repr_css_attr_unref>(sp_repr_css_attr_new());
        sp_repr_css_set_property(css.get(), "line-height", entry.value.c_str());
        entry.object->changeCSS(css.get(), "style");
    }
}

// A direct text-node (SPString) child cannot carry a style, so once the root
// floor is zeroed it would lose the line-height it inherited from the root.
// Preserve it by wrapping the node in a plain svg:tspan, exactly like an
// equivalent authored plain span: no sodipodi:role (that would insert paragraph
// semantics), no geometry copies, and the exact text/whitespace/order kept.
// The native child normalization then pushes the pre-zero root line-height onto
// the wrapper. Mirrors prepareInnerText's structural-newline guard. Returns true
// when the tree changed and the caller must rebuild the layout.
bool wrapDirectTextChildrenForRootLineHeight(SPText &text)
{
    std::vector<SPObject *> direct;
    for (auto child : text.childList(false)) {
        auto spstring = cast<SPString>(child);
        if (spstring && spstring->string != "\n") {
            direct.push_back(child);
        }
    }
    if (direct.empty()) {
        return false;
    }

    XML::Node *text_repr = text.getRepr();
    XML::Document *xml_doc = text_repr->document();
    for (auto child : direct) {
        auto spstring = cast<SPString>(child);
        XML::Node *rstring = xml_doc->createTextNode(spstring->string.c_str());
        XML::Node *rtspan = xml_doc->createElement("svg:tspan");
        rtspan->addChild(rstring, nullptr);
        text_repr->addChild(rtspan, spstring->getRepr());
        GC::release(rstring);
        GC::release(rtspan);
        text_repr->removeChild(spstring->getRepr());
    }
    return true;
}

} // namespace

void prepareInnerRootLineHeight(SPItem &parent)
{
    SPStyle *parent_style = parent.style;
    auto parent_css = Util::delete_with<sp_repr_css_attr_unref>(
        sp_css_attr_from_style(parent_style, SP_STYLE_FLAG_IFSET));
    Glib::ustring parent_lineheight = sp_repr_css_property(parent_css.get(), "line-height", "1.25");
    auto cssfit = Util::delete_with<sp_repr_css_attr_unref>(sp_repr_css_attr_new());
    sp_repr_css_set_property(cssfit.get(), "line-height", parent_lineheight.c_str());
    double minheight = 0;
    if (parent_style) {
        minheight = parent_style->line_height.computed;
    }
    std::vector<AuthoredLineHeightOverride> preserved;
    if (minheight) {
        // Compare physical local heights, not line_height.computed directly: the
        // authored root value may be a unitless multiplier.
        double const root_floor = convertLineHeight(
            lineHeightFromStyle(*parent_style), TextLineHeightUnit::Px,
            parent_style->font_size.computed).value;
        // Preserve the inherited root line-height of direct text-node children
        // before the root floor is zeroed. The normalization below then styles
        // the plain wrapper just like an equivalent authored plain tspan.
        if (auto text = cast<SPText>(&parent)) {
            if (wrapDirectTextChildrenForRootLineHeight(*text)) {
                // The original SPString may still be a layout character source;
                // rebuild to avoid a dangling reference. Content length is
                // unchanged, so logical character indices stay stable.
                text->rebuildLayout();
            }
        }
        for (auto child : parent.childList(false)) {
            if (auto item = cast<SPItem>(child)) {
                collectAuthoredLineHeightOverrides(*item, root_floor, preserved);
            }
        }
        for (auto child : parent.childList(false)) {
            if (auto item = cast<SPItem>(child)) {
                setTextRootStyleAndUnsetDescendants(*item, cssfit.get());
            }
        }
    }
    sp_repr_css_set_property(cssfit.get(), "line-height", "0");
    parent.changeCSS(cssfit.get(), "style");
    // Root already at 0 leaves `preserved` empty, so preparation stays a no-op.
    restoreAuthoredLineHeightOverrides(preserved);
}

} // namespace Inkscape::UI
