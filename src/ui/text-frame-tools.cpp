// SPDX-License-Identifier: GPL-2.0-or-later

#include "text-frame-tools.h"

#include <algorithm>
#include <memory>
#include <string>

#include "document.h"
#include "id-clash.h"
#include "object/sp-defs.h"
#include "object/sp-text.h"
#include "style.h"
#include "xml/document.h"
#include "xml/node.h"
#include "xml/repr.h"

namespace Inkscape::UI {
namespace {

constexpr char owner_attribute[] = "inkscape:text-frame-owner";

unsigned unsignedAttribute(XML::Node const &repr, char const *name, unsigned fallback)
{
    auto value = repr.attribute(name);
    if (!value) return fallback;
    try { return static_cast<unsigned>(std::stoul(value)); } catch (...) { return fallback; }
}

double doubleAttribute(XML::Node const &repr, char const *name, double fallback)
{
    return repr.getAttributeDouble(name, fallback);
}

TextFrameVerticalAlignment alignment(XML::Node const &repr)
{
    auto value = repr.attribute("inkscape:text-frame-align");
    if (value && std::string_view{value} == "middle") return TextFrameVerticalAlignment::Middle;
    if (value && std::string_view{value} == "bottom") return TextFrameVerticalAlignment::Bottom;
    return TextFrameVerticalAlignment::Top;
}

char const *alignmentName(TextFrameVerticalAlignment value)
{
    switch (value) {
        case TextFrameVerticalAlignment::Middle: return "middle";
        case TextFrameVerticalAlignment::Bottom: return "bottom";
        case TextFrameVerticalAlignment::Top: return "top";
    }
    return "top";
}

void removeGeneratedFrames(SPDocument &document, SPText &text)
{
    // Follow the edited text's actual frame references, including nested defs.
    // An owner string alone must never authorize deleting another/unreferenced
    // destination frame. Snapshot anchored XML before removal changes hrefs.
    auto owner = std::string(text.getId() ? text.getId() : "");
    if (owner.empty() || text.document != &document || !text.style) return;
    using NodeOwner = std::shared_ptr<XML::Node>;
    std::vector<NodeOwner> frames;
    for (auto *href : text.style->shape_inside.hrefs) {
        auto object = href->getObject();
        auto node = object && object->document == &document ? object->getRepr() : nullptr;
        auto parent = node ? node->parent() : nullptr;
        if (!node || !parent || std::string_view(node->name()) != "svg:rect" ||
            std::string_view(parent->name()) != "svg:defs" ||
            g_strcmp0(node->attribute(owner_attribute), owner.c_str())) continue;
        if (std::any_of(frames.begin(), frames.end(), [&](auto const &p) { return p.get() == node; })) continue;
        GC::anchor(node);
        frames.emplace_back(node, [](auto *p) { GC::release(p); });
    }
    for (auto const &node : frames) {
        auto parent = node->parent();
        if (parent && std::string_view(parent->name()) == "svg:defs" &&
            !g_strcmp0(node->attribute(owner_attribute), owner.c_str()) &&
            document.getObjectByRepr(node.get())) parent->removeChild(node.get());
    }
}

} // namespace

TextFrameSettings textFrameSettings(SPText const &text)
{
    TextFrameSettings result;
    auto repr = const_cast<SPText &>(text).getRepr();
    if (!repr) return result;
    result.columns = std::max(1u, unsignedAttribute(*repr, "inkscape:text-frame-columns", 1));
    result.gap = doubleAttribute(*repr, "inkscape:text-frame-gap", 0.0);
    result.vertical_alignment = alignment(*repr);
    result.generated = repr->attribute("inkscape:text-frame-generated") != nullptr;
    if (result.generated) {
        result.width = doubleAttribute(*repr, "inkscape:text-frame-width", 0.0);
        result.height = doubleAttribute(*repr, "inkscape:text-frame-height", 0.0);
    }
    if (auto frame = const_cast<SPText &>(text).get_frame()) {
        if (result.width <= 0.0 && frame->width() < 100000.0) result.width = frame->width();
        if (result.height <= 0.0 && frame->height() < 100000.0) result.height = frame->height();
    }
    return result;
}

bool setTextFrameSettings(SPDocument &document, std::vector<SPText *> const &texts,
                          TextFrameSettings const &requested)
{
    auto xml = document.getReprDoc();
    auto defs = document.getDefs();
    if (!xml || !defs || requested.width <= 0.0 || requested.height <= 0.0) return false;

    bool changed = false;
    for (auto text : texts) {
        auto repr = text ? text->getRepr() : nullptr;
        auto id = text ? text->getId() : nullptr;
        if (!repr || !id) continue;
        auto const columns = std::clamp(requested.columns, 1u, 20u);
        auto const gap = std::max(0.0, requested.gap);
        auto const column_width = (requested.width - gap * (columns - 1)) / columns;
        if (column_width <= 0.0) continue;

        auto existing = textFrameSettings(*text);
        if (existing.generated && existing.width == requested.width &&
            existing.height == requested.height && existing.columns == columns &&
            existing.gap == gap && existing.vertical_alignment == requested.vertical_alignment) {
            continue;
        }

        auto bounds = text->geometricBounds();
        auto x = bounds ? bounds->left() : 0.0;
        auto y = bounds ? bounds->top() : 0.0;
        if (auto frame = text->get_frame(); frame && frame->width() < 100000.0 &&
            frame->height() < 100000.0) {
            x = frame->left();
            y = frame->top();
        }

        // XML callbacks from removing old frames may invalidate borrowed strings.
        auto owner_id = std::string(id);
        removeGeneratedFrames(document, *text);
        std::string shapes;
        for (unsigned column = 0; column < columns; ++column) {
            auto rectangle = xml->createElement("svg:rect");
            auto rectangle_id = generate_similar_unique_id(
                &document, owner_id + "-text-frame-" + std::to_string(column + 1)).raw();
            rectangle->setAttribute("id", rectangle_id.c_str());
            rectangle->setAttribute(owner_attribute, owner_id.c_str());
            rectangle->setAttributeSvgDouble("x", x + column * (column_width + gap));
            rectangle->setAttributeSvgDouble("y", y);
            rectangle->setAttributeSvgDouble("width", column_width);
            rectangle->setAttributeSvgDouble("height", requested.height);
            defs->getRepr()->appendChild(rectangle);
            GC::release(rectangle);
            if (!shapes.empty()) shapes += ' ';
            shapes += "url(#" + rectangle_id + ")";
        }

        auto css = sp_repr_css_attr(repr, "style");
        sp_repr_css_set_property(css, "shape-inside", shapes.c_str());
        sp_repr_css_unset_property(css, "inline-size");
        sp_repr_css_set_property(css, "white-space", "pre-wrap");
        sp_repr_css_set(repr, css, "style");
        sp_repr_css_attr_unref(css);
        // inline-size may be authored as a presentation attribute rather than
        // inside style. Keeping it alongside shape-inside leaves two competing
        // flow geometries and makes SVG 1.1 fallback positioning choose the
        // inline-size path, which breaks generated columns after reopening.
        repr->setAttribute("inline-size", nullptr);
        repr->setAttribute("inkscape:text-frame-generated", "true");
        repr->setAttributeCssDouble("inkscape:text-frame-width", requested.width);
        repr->setAttributeCssDouble("inkscape:text-frame-height", requested.height);
        repr->setAttributeInt("inkscape:text-frame-columns", columns);
        repr->setAttributeCssDouble("inkscape:text-frame-gap", gap);
        repr->setAttribute("inkscape:text-frame-align",
                           columns == 1 ? alignmentName(requested.vertical_alignment) : "top");
        changed = true;
    }
    return changed;
}

} // namespace Inkscape::UI
