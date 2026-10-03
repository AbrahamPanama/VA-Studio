// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * SVG Extension test
 *//*
 * Authors: see git history
 *
 * Copyright (C) 2020 Authors
 *
 * Released under GNU GPL version 2 or later, read the file 'COPYING' for more information
 */

#include <gtest/gtest.h>

#include <glib/gstdio.h>

#include <array>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "document.h"
#include "document-undo.h"
#include "inkscape.h"

#include "actions/actions-svg-processing.h"
#include "extension/db.h"
#include "extension/init.h"
#include "extension/input.h"
#include "extension/internal/svg.h"
#include "extension/output.h"
#include "extension/system.h"
#include "object/sp-defs.h"
#include "object/sp-item.h"
#include "object/sp-text.h"
#include "object/sp-tspan.h"
#include "preferences.h"
#include "style.h"
#include "xml/attribute-record.h"
#include "xml/node.h"
#include "xml/repr.h"

using namespace Inkscape;
using namespace Inkscape::Extension;
using namespace Inkscape::Extension::Internal;
using namespace std::literals;

class SvgExtensionTest : public ::testing::Test
{
public:
    static std::string create_file(const std::string &filename, const std::string &content)
    {
        std::stringstream path_builder;
        path_builder << "SvgExtensionTest_" << _files.size() << "_" << filename;
        std::string path = path_builder.str();
        GError *error = nullptr;
        if (!g_file_set_contents(path.c_str(), content.c_str(), content.size(), &error)) {
            std::stringstream msg;
            msg << "SvgExtensionTest::create_file failed: GError(" << error->domain << ", " << error->code << ", "
                << error->message << ")";
            g_error_free(error);
            throw std::runtime_error(msg.str());
        }
        _files.insert(path);
        return path;
    }

    static std::set<std::string> _files;

protected:
    void SetUp() override
    {
        // setup hidden dependency
        Application::create(false);
    }

    static void TearDownTestCase()
    {
        for (auto file : _files) {
            if (g_remove(file.c_str())) {
                std::cout << "SvgExtensionTest was unable to remove file: " << file << std::endl;
            }
        }
    }
};

std::set<std::string> SvgExtensionTest::_files;

TEST_F(SvgExtensionTest, openingAsLinkInImageASizelessSvgFileReturnsNull)
{
    std::string sizeless_svg_file =
        create_file("sizeless.svg",
                    "<svg><path d=\"M 71.527648,186.14229 A 740.48715,740.48715 0 0 0 696.31258,625.8041 Z\"/></svg>");
    
    Svg::init();
    Input *svg_input_extension(dynamic_cast<Input *>(db.get(SP_MODULE_KEY_INPUT_SVG))); 
    
    Preferences *prefs = Preferences::get();
    prefs->setBool("/dialogs/import/ask_svg", false);
    prefs->setString("/dialogs/import/import_mode_svg", "link");

    ASSERT_EQ(svg_input_extension->open(sizeless_svg_file.c_str(), true), nullptr);
}

TEST_F(SvgExtensionTest, hiddenSvg2TextIsSaved)
{
    constexpr auto docString = R"""(
<svg width="100" height="200">
  <defs>
    <rect id="rect1" x="0" y="0"   width="100" height="100" />
    <rect id="rect2" x="0" y="100" width="100" height="100" />
  </defs>
  <g>
    <text id="text1" style="shape-inside:url(#rect1);display:inline;">
      <tspan id="tspan1" x="0" y="0">foo</tspan>
    </text>
    <text id="text2" style="shape-inside:url(#rect2);display:none;"  >
      <tspan id="tspan2" x="0" y="0">bar</tspan>
    </text>
  </g>
</svg>
)"""sv;
    auto doc = SPDocument::createNewDocFromMem(docString);
    ASSERT_TRUE(doc);

    std::map<std::string,std::string> textMap;
    textMap["text1"] = "foo";
    textMap["text2"] = "bar";

    // otherwise the layout reports a size of 0
    for (const auto& kv : textMap) {
        auto textElement = cast<SPText>(doc->getObjectById(kv.first));
        ASSERT_TRUE(textElement);
        textElement->rebuildLayout();
    }

    Inkscape::XML::Document *rdoc = doc->getReprDoc();
    ASSERT_TRUE(rdoc);

    insert_text_fallback(rdoc->root(), doc.get());

    for(const auto& kv : textMap) {
        auto textElement = doc->getObjectById(kv.first);
        ASSERT_TRUE(textElement);
        auto tspanElement = textElement->firstChild();
        ASSERT_TRUE(tspanElement);
        auto stringElement = cast<SPString>(tspanElement->firstChild());
        ASSERT_TRUE(stringElement);
        ASSERT_EQ(kv.second, stringElement->string.raw());
    }
}

// ---------------------------------------------------------------------------
// F2 text-fallback isolation: the output action must never touch the live
// source document, while the direct helper keeps its intentional live-edit
// semantics and the generated SVG 1.1 fallback keeps native fidelity.
// ---------------------------------------------------------------------------

namespace {

// Authored fixture with proper namespaces: visible and hidden inline-size and
// shape-inside text, authored display:inherit and missing display, one vertical
// case, and a legacy role:line run nested inside a wrapped text. The rect is
// the stable user-edit baseline (it really has an authored x="1").
constexpr auto kFallbackSvg = R"SVG(<svg xmlns="http://www.w3.org/2000/svg"
     xmlns:sodipodi="http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd"
     xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape"
     width="400" height="320" version="2.0">
  <defs>
    <rect id="frame-visible" x="0" y="0" width="120" height="80"/>
    <rect id="frame-hidden" x="0" y="100" width="120" height="80"/>
  </defs>
  <g id="layer-body" inkscape:groupmode="layer" inkscape:label="Layer 1">
    <text id="inline-visible" x="10" y="30"
          style="font-family:sans-serif;font-size:20px;line-height:24px;inline-size:160px;white-space:pre-wrap">alpha beta gamma delta epsilon zeta eta</text>
    <text id="inline-hidden" x="10" y="80"
          style="font-family:sans-serif;font-size:20px;line-height:24px;inline-size:160px;white-space:pre-wrap;display:none">hidden wrapped text stays complete</text>
    <text id="shape-visible" x="10" y="140"
          style="font-family:sans-serif;font-size:20px;line-height:24px;text-anchor:middle;shape-inside:url(#frame-visible)">shape inside text content wraps across</text>
    <text id="shape-hidden" x="10" y="200"
          style="font-family:sans-serif;font-size:20px;line-height:24px;shape-inside:url(#frame-hidden);display:none">hidden shape inside text content</text>
    <text id="display-inherit" x="200" y="30"
          style="font-family:sans-serif;font-size:20px;line-height:24px;inline-size:160px;white-space:pre-wrap;display:inherit">inherit display text wraps here</text>
    <text id="display-missing" x="200" y="90"
          style="font-family:sans-serif;font-size:20px;line-height:24px;inline-size:160px;white-space:pre-wrap">missing display text wraps here</text>
    <text id="vertical-inline" x="350" y="30"
          style="font-family:sans-serif;font-size:20px;line-height:24px;inline-size:160px;white-space:pre-wrap;writing-mode:vertical-rl">vertical wrapped text content</text>
    <text id="with-role-line" x="200" y="160"
          style="font-family:sans-serif;font-size:20px;line-height:24px;inline-size:160px;white-space:pre-wrap"><tspan id="role-line-tspan" x="200" y="160" sodipodi:role="line">role line wrapped text content</tspan></text>
    <text id="with-paragraphs" x="200" y="220"
          style="font-family:sans-serif;font-size:20px;line-height:24px;inline-size:160px;white-space:pre-wrap"><tspan sodipodi:role="paragraph">First wrapped paragraph text</tspan><tspan sodipodi:role="paragraph">Second wrapped paragraph text</tspan></text>
  </g>
  <rect id="user-edit" x="1" y="290" width="20" height="20"/>
</svg>)SVG"sv;

// No inline-size/shape-inside and no wrapped text: must take the eligibility
// fast path and never create fallback.
constexpr auto kNonWrappedSvg = R"SVG(<svg xmlns="http://www.w3.org/2000/svg"
     xmlns:sodipodi="http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd"
     width="200" height="120" version="2.0">
  <text id="legacy-line" x="10" y="30" style="font-family:sans-serif;font-size:20px"><tspan sodipodi:role="line">legacy line text</tspan></text>
  <text id="plain" x="10" y="60" style="font-family:sans-serif;font-size:20px">plain text</text>
  <rect id="only-shape" x="0" y="0" width="10" height="10"/>
</svg>)SVG"sv;

constexpr auto kEmptyWrappedSvg = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="120">
  <text id="empty-wrapped" x="10" y="30"
        style="font-family:sans-serif;font-size:20px;line-height:24px;inline-size:100px;white-space:pre-wrap"/>
</svg>)SVG"sv;

// Shape-only document with no text element at all: the eligibility fast path
// must be proven on a genuinely text-free source, not merely a non-wrapped one.
constexpr auto kTextFreeSvg = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="120">
  <defs>
    <rect id="shape-def" x="0" y="0" width="20" height="20"/>
  </defs>
  <g id="shape-layer">
    <rect id="shape-only" x="0" y="0" width="10" height="10"/>
    <circle id="only-circle" cx="40" cy="40" r="8"/>
  </g>
</svg>)SVG"sv;

std::vector<std::string> const kWrappedIds = {
    "inline-visible", "inline-hidden", "shape-visible", "shape-hidden",
    "display-inherit", "display-missing", "vertical-inline", "with-role-line",
    "with-paragraphs"
};

// Raw, nonmutating XML snapshot: node types, names, all attributes in their
// actual order, content and child order. Deliberately does NOT serialize
// (sp_repr_save_buf mutates), so it proves the live tree was not written.
void fingerprint_node(Inkscape::XML::Node const *node, std::string &out)
{
    auto field = [&](char const *text) {
        std::string_view value = text ? text : "";
        out += std::to_string(value.size()) + ":";
        out.append(value);
    };
    out += "[" + std::to_string(static_cast<int>(node->type()));
    field(node->name());
    field(node->content());
    for (auto const &attr : node->attributeList()) {
        field(g_quark_to_string(attr.key));
        field(static_cast<char const *>(attr.value));
    }
    out += ";";
    for (auto child = node->firstChild(); child; child = child->next()) {
        fingerprint_node(child, out);
    }
    out += "]";
}

std::string raw_fingerprint(SPDocument &document)
{
    std::string result;
    fingerprint_node(document.getReprDoc(), result);
    return result;
}

// Document-wide ID lookup. sp_repr_lookup_child() only examines immediate
// children, so it cannot find fixture text nested inside the layer <g> or the
// frames inside <defs>; this resolves through the document's id map instead
// and returns null when absent.
Inkscape::XML::Node *document_node(SPDocument &document, char const *id)
{
    auto *object = document.getObjectById(id);
    return object ? object->getRepr() : nullptr;
}

std::string descendant_text(Inkscape::XML::Node *node)
{
    std::string out;
    if (node->type() == Inkscape::XML::NodeType::TEXT_NODE && node->content()) {
        out += node->content();
    }
    for (auto child = node->firstChild(); child; child = child->next()) {
        out += descendant_text(child);
    }
    return out;
}

// One SVG 1.1 fallback line: the first tspan in a line that carries x or y.
struct FallbackLine {
    bool has_x = false;
    bool has_y = false;
    double x = 0.0;
    double y = 0.0;
    std::string style;
    std::string text;
};

void collect_fallback_lines(Inkscape::XML::Node *node, std::vector<FallbackLine> &out)
{
    for (auto child = node->firstChild(); child; child = child->next()) {
        if (std::strncmp("svg:tspan", child->name(), 9) == 0 &&
            (child->attribute("x") || child->attribute("y"))) {
            FallbackLine line;
            line.has_x = child->attribute("x") != nullptr;
            line.has_y = child->attribute("y") != nullptr;
            if (line.has_x) {
                line.x = child->getAttributeDouble("x", 0.0);
            }
            if (line.has_y) {
                line.y = child->getAttributeDouble("y", 0.0);
            }
            if (auto const s = child->attribute("style")) {
                line.style = s;
            }
            line.text = descendant_text(child);
            out.push_back(std::move(line));
        } else {
            collect_fallback_lines(child, out);
        }
    }
}

std::string style_property(Inkscape::XML::Node *node, char const *name)
{
    SPCSSAttr *css = sp_repr_css_attr(node, "style");
    std::string value;
    if (auto const v = sp_repr_css_property(css, name, "")) {
        value = v;
    }
    sp_repr_css_attr_unref(css);
    return value;
}

// Count descendant tags with the given sodipodi:role; used to prove semantic
// paragraph wrappers survive serialization/reopen.
int count_role(Inkscape::XML::Node *node, char const *role)
{
    int count = 0;
    for (auto child = node->firstChild(); child; child = child->next()) {
        if (std::strncmp("svg:tspan", child->name(), 9) == 0) {
            if (auto const value = child->attribute("sodipodi:role")) {
                if (std::strcmp(value, role) == 0) {
                    ++count;
                }
            }
        }
        count += count_role(child, role);
    }
    return count;
}

// Authored display flag state for each text; used to prove the live document's
// SPStyle was not rewritten by setHidden().
std::vector<std::array<long, 4>> capture_display(SPDocument &document, std::vector<std::string> const &ids)
{
    std::vector<std::array<long, 4>> out;
    out.reserve(ids.size());
    for (auto const &id : ids) {
        auto *text = cast<SPText>(document.getObjectById(id.c_str()));
        if (!text || !text->style) {
            out.push_back({-1L, -1L, -1L, -1L});
            continue;
        }
        out.push_back({text->style->display.set ? 1L : 0L,
                       text->style->display.inherit ? 1L : 0L,
                       static_cast<long>(text->style->display.value),
                       static_cast<long>(text->style->display.computed)});
    }
    return out;
}

struct PointXY {
    double x = 0.0;
    double y = 0.0;
};

PointXY first_xy(SPDocument &document, char const *id)
{
    auto *span = cast<SPTSpan>(document.getObjectById(id));
    if (!span) {
        return {};
    }
    auto const p = span->attributes.firstXY();
    return {p[Geom::X], p[Geom::Y]};
}

} // namespace

class SvgExtensionFallbackTest : public SvgExtensionTest
{
protected:
    void SetUp() override
    {
        SvgExtensionTest::SetUp();
        // Headless native extension initialization (no GUI). Guarded so it runs
        // once regardless of the gtest suite-hook spelling in use.
        static bool extensions_initialized = false;
        if (!extensions_initialized) {
            Inkscape::Extension::init();
            extensions_initialized = true;
        }
        _prefs = Preferences::get();
        _fallback_before = _prefs->getBool("/options/svgexport/text_insertfallback", true);
        _prefs->setBool("/options/svgexport/text_insertfallback", true);
    }

    void TearDown() override
    {
        if (_prefs) {
            _prefs->setBool("/options/svgexport/text_insertfallback", _fallback_before);
        }
    }

private:
    Preferences *_prefs = nullptr;
    bool _fallback_before = true;
};

// The real processing action on a live->copy projection must generate complete
// fallback while leaving the live source byte-structurally untouched, with its
// authored display flags, role:line coordinate state, dirty flag and undo stack
// all preserved.
TEST_F(SvgExtensionFallbackTest, ProjectionFallbackIsolatesLiveDocument)
{
    auto live = SPDocument::createNewDocFromMem(kFallbackSvg);
    ASSERT_TRUE(live);
    live->ensureUpToDate();
    DocumentUndo::setUndoSensitive(live.get(), true);
    // Settle the fixture/setup history, then clear it: the deliberate edit below
    // must be its own transaction so Undo restores the authored x="1".
    DocumentUndo::done(live.get(), Inkscape::Util::Internal::ContextString{"fallback isolation setup"}, "");
    DocumentUndo::clearUndo(live.get());
    DocumentUndo::clearRedo(live.get());

    // Stable baseline: the authored rect really has x="1"; the user edit is 7.
    auto *edit = live->getObjectById("user-edit");
    ASSERT_TRUE(edit);
    ASSERT_STREQ(edit->getRepr()->attribute("x"), "1");
    edit->getRepr()->setAttribute("x", "7");
    DocumentUndo::done(live.get(), Inkscape::Util::Internal::ContextString{"fallback isolation fixture"}, "");
    live->setModifiedSinceSave(true);

    // The authored role:line tspan carries exactly one x/y pair before the
    // projection runs, so rebuildLayout really exercises the
    // singleXYCoordinates mutation branch on the eligible text.
    auto *role_span = cast<SPTSpan>(live->getObjectById("role-line-tspan"));
    ASSERT_TRUE(role_span);
    ASSERT_TRUE(role_span->attributes.singleXYCoordinates());

    auto const fingerprint_before = raw_fingerprint(*live);
    auto const display_before = capture_display(*live, kWrappedIds);
    auto const role_xy_before = first_xy(*live, "role-line-tspan");
    bool const modified_before = live->isModifiedSinceSave();

    auto projection = live->copy();
    ASSERT_TRUE(projection);
    ASSERT_EQ(projection->getOriginalDocument(), live.get());
    projection->ensureUpToDate();
    projection->getActionGroup()->activate_action("insert-text-fallback");

    for (auto const &id : kWrappedIds) {
        auto *proj_text = document_node(*projection, id.c_str());
        ASSERT_TRUE(proj_text) << id;
        std::vector<FallbackLine> lines;
        collect_fallback_lines(proj_text, lines);
        EXPECT_GT(lines.size(), 0u) << id;
    }

    // Whole group/defs context is retained: every text is still inside the
    // authored group and the shape-inside defs remain, with no reparent/flatten.
    auto *projected_group = sp_repr_lookup_child(projection->getReprRoot(), "id", "layer-body");
    ASSERT_TRUE(projected_group);
    for (auto const &id : kWrappedIds) {
        EXPECT_TRUE(sp_repr_lookup_child(projected_group, "id", id.c_str())) << id;
    }
    auto *projected_defs = projection->getDefs()->getRepr();
    ASSERT_TRUE(projected_defs);
    EXPECT_TRUE(sp_repr_lookup_child(projected_defs, "id", "frame-visible"));
    EXPECT_TRUE(sp_repr_lookup_child(projected_defs, "id", "frame-hidden"));

    EXPECT_EQ(raw_fingerprint(*live), fingerprint_before);
    EXPECT_EQ(capture_display(*live, kWrappedIds), display_before);
    auto const role_xy_after = first_xy(*live, "role-line-tspan");
    EXPECT_DOUBLE_EQ(role_xy_after.x, role_xy_before.x);
    EXPECT_DOUBLE_EQ(role_xy_after.y, role_xy_before.y);
    EXPECT_EQ(live->isModifiedSinceSave(), modified_before);

    // The pre-existing edit still undoes and redoes, and the isolation added no
    // live undo entry.
    ASSERT_TRUE(DocumentUndo::undo(live.get()));
    live->ensureUpToDate();
    ASSERT_TRUE(live->getObjectById("user-edit"));
    EXPECT_STREQ(live->getObjectById("user-edit")->getRepr()->attribute("x"), "1");
    ASSERT_TRUE(DocumentUndo::redo(live.get()));
    live->ensureUpToDate();
    EXPECT_STREQ(live->getObjectById("user-edit")->getRepr()->attribute("x"), "7");
}

// Output geometry must match the existing direct native algorithm run on a
// separate sacrificial document, and a reopened serialization must retain the
// complete editable text and the same coordinates.
TEST_F(SvgExtensionFallbackTest, ProjectionFallbackMatchesSacrificialNativeReferenceAndReopens)
{
    auto live = SPDocument::createNewDocFromMem(kFallbackSvg);
    ASSERT_TRUE(live);
    live->ensureUpToDate();

    auto projection = live->copy();
    ASSERT_TRUE(projection);
    projection->ensureUpToDate();
    projection->getActionGroup()->activate_action("insert-text-fallback");

    // Sacrificial reference using the existing native same-document algorithm.
    auto reference = SPDocument::createNewDocFromMem(kFallbackSvg);
    ASSERT_TRUE(reference);
    reference->ensureUpToDate();
    insert_text_fallback(reference->getReprRoot(), reference.get());

    std::string const serialized = sp_repr_save_buf(projection->getReprDoc());
    auto reopened = SPDocument::createNewDocFromMem(serialized);
    ASSERT_TRUE(reopened);
    reopened->ensureUpToDate();

    for (auto const &id : kWrappedIds) {
        auto *proj_text = document_node(*projection, id.c_str());
        auto *ref_text = document_node(*reference, id.c_str());
        auto *live_text = document_node(*live, id.c_str());
        auto *re_text = document_node(*reopened, id.c_str());
        ASSERT_TRUE(proj_text) << id;
        ASSERT_TRUE(ref_text) << id;
        ASSERT_TRUE(live_text) << id;
        ASSERT_TRUE(re_text) << id;

        std::vector<FallbackLine> proj_lines;
        std::vector<FallbackLine> ref_lines;
        std::vector<FallbackLine> re_lines;
        collect_fallback_lines(proj_text, proj_lines);
        collect_fallback_lines(ref_text, ref_lines);
        collect_fallback_lines(re_text, re_lines);

        ASSERT_EQ(proj_lines.size(), ref_lines.size()) << id;
        ASSERT_EQ(proj_lines.size(), re_lines.size()) << id;
        for (std::size_t i = 0; i < proj_lines.size(); ++i) {
            EXPECT_EQ(proj_lines[i].has_x, ref_lines[i].has_x) << id << " #" << i;
            EXPECT_EQ(proj_lines[i].has_y, ref_lines[i].has_y) << id << " #" << i;
            EXPECT_NEAR(proj_lines[i].x, ref_lines[i].x, 0.01) << id << " #" << i;
            EXPECT_NEAR(proj_lines[i].y, ref_lines[i].y, 0.01) << id << " #" << i;
            EXPECT_EQ(proj_lines[i].style, ref_lines[i].style) << id << " #" << i;
            EXPECT_EQ(proj_lines[i].text, ref_lines[i].text) << id << " #" << i;

            EXPECT_EQ(re_lines[i].has_x, proj_lines[i].has_x) << id << " #" << i;
            EXPECT_EQ(re_lines[i].has_y, proj_lines[i].has_y) << id << " #" << i;
            EXPECT_NEAR(re_lines[i].x, proj_lines[i].x, 0.01) << id << " #" << i;
            EXPECT_NEAR(re_lines[i].y, proj_lines[i].y, 0.01) << id << " #" << i;
            EXPECT_EQ(re_lines[i].style, proj_lines[i].style) << id << " #" << i;
            EXPECT_EQ(re_lines[i].text, proj_lines[i].text) << id << " #" << i;
        }

        // Reopened output remains editable and keeps the complete authored text.
        auto *reopened_text = cast<SPText>(reopened->getObjectById(id.c_str()));
        ASSERT_TRUE(reopened_text) << id;
        EXPECT_EQ(descendant_text(re_text), descendant_text(live_text)) << id;
        if (id == "with-paragraphs") {
            // Both authored paragraphs must survive. Compare against the
            // sacrificial native reference and require the authored count of two
            // instead of accepting a lossy >= 1.
            EXPECT_EQ(count_role(re_text, "paragraph"), count_role(ref_text, "paragraph")) << id;
            EXPECT_EQ(count_role(re_text, "paragraph"), 2) << id;
        }
    }

    // Shape-inside fallback drops text-anchor on the projection only; the live
    // document keeps its authored value.
    auto *proj_shape = document_node(*projection, "shape-visible");
    auto *live_shape = document_node(*live, "shape-visible");
    ASSERT_TRUE(proj_shape);
    ASSERT_TRUE(live_shape);
    EXPECT_TRUE(style_property(proj_shape, "text-anchor").empty());
    EXPECT_EQ(style_property(live_shape, "text-anchor"), "middle");
}

// A direct action on a live document has no original-document projection and
// must keep its intentional in-place edit semantics.
TEST_F(SvgExtensionFallbackTest, DirectFallbackActionStillEditsLiveDocument)
{
    auto standalone = SPDocument::createNewDocFromMem(kFallbackSvg);
    ASSERT_TRUE(standalone);
    ASSERT_EQ(standalone->getOriginalDocument(), nullptr);
    standalone->ensureUpToDate();

    auto *text_repr = document_node(*standalone, "inline-visible");
    ASSERT_TRUE(text_repr);
    std::string const authored_text = descendant_text(text_repr);

    standalone->getActionGroup()->activate_action("insert-text-fallback");

    std::vector<FallbackLine> after_lines;
    collect_fallback_lines(text_repr, after_lines);
    EXPECT_GT(after_lines.size(), 0u);
    EXPECT_EQ(descendant_text(text_repr), authored_text);
}

// Non-wrapped legacy role:line and text-free documents must not create fallback
// and must not modify either the projection or the live source.
TEST_F(SvgExtensionFallbackTest, NonWrappedAndTextFreeDocumentsTakeFastPath)
{
    auto live = SPDocument::createNewDocFromMem(kNonWrappedSvg);
    ASSERT_TRUE(live);
    live->ensureUpToDate();
    auto const live_before = raw_fingerprint(*live);

    auto projection = live->copy();
    ASSERT_TRUE(projection);
    projection->ensureUpToDate();
    auto const projection_before = raw_fingerprint(*projection);

    projection->getActionGroup()->activate_action("insert-text-fallback");

    EXPECT_EQ(raw_fingerprint(*projection), projection_before);
    EXPECT_EQ(raw_fingerprint(*live), live_before);

    // A genuinely text-free (shape-only) document must also take the fast path:
    // run the real action on its projection and require both the source and the
    // projection to stay byte-structurally unchanged.
    auto text_free = SPDocument::createNewDocFromMem(kTextFreeSvg);
    ASSERT_TRUE(text_free);
    text_free->ensureUpToDate();
    auto const text_free_before = raw_fingerprint(*text_free);

    auto text_free_projection = text_free->copy();
    ASSERT_TRUE(text_free_projection);
    text_free_projection->ensureUpToDate();
    auto const text_free_projection_before = raw_fingerprint(*text_free_projection);

    text_free_projection->getActionGroup()->activate_action("insert-text-fallback");

    EXPECT_EQ(raw_fingerprint(*text_free_projection), text_free_projection_before);
    EXPECT_EQ(raw_fingerprint(*text_free), text_free_before);
}

// An eligible but empty wrapped text creates no fallback lines and must still
// leave the live source untouched.
TEST_F(SvgExtensionFallbackTest, EmptyWrappedTextDoesNotLeakToLive)
{
    auto live = SPDocument::createNewDocFromMem(kEmptyWrappedSvg);
    ASSERT_TRUE(live);
    live->ensureUpToDate();
    auto const live_before = raw_fingerprint(*live);

    auto projection = live->copy();
    ASSERT_TRUE(projection);
    projection->ensureUpToDate();
    projection->getActionGroup()->activate_action("insert-text-fallback");

    EXPECT_EQ(raw_fingerprint(*live), live_before);
    auto *proj_text = document_node(*projection, "empty-wrapped");
    ASSERT_TRUE(proj_text);
    std::vector<FallbackLine> lines;
    collect_fallback_lines(proj_text, lines);
    EXPECT_TRUE(lines.empty());
}

// A real native Extension::save that runs the fallback preprocessing and then
// fails at the blocked-parent destination must not mutate the live document.
TEST_F(SvgExtensionFallbackTest, NativeSaveFailureAfterFallbackPreservesLiveDocument)
{
    auto live = SPDocument::createNewDocFromMem(kFallbackSvg);
    ASSERT_TRUE(live);
    live->ensureUpToDate();
    DocumentUndo::setUndoSensitive(live.get(), true);
    // Settle the fixture/setup history, then clear it: the deliberate edit below
    // must be its own transaction so Undo restores the authored x="1".
    DocumentUndo::done(live.get(), Inkscape::Util::Internal::ContextString{"fallback native save setup"}, "");
    DocumentUndo::clearUndo(live.get());
    DocumentUndo::clearRedo(live.get());

    auto *edit = live->getObjectById("user-edit");
    ASSERT_TRUE(edit);
    ASSERT_STREQ(edit->getRepr()->attribute("x"), "1");
    edit->getRepr()->setAttribute("x", "7");
    DocumentUndo::done(live.get(), Inkscape::Util::Internal::ContextString{"fallback native save fixture"}, "");
    live->setModifiedSinceSave(true);

    auto *role_span = cast<SPTSpan>(live->getObjectById("role-line-tspan"));
    ASSERT_TRUE(role_span);
    ASSERT_TRUE(role_span->attributes.singleXYCoordinates());
    auto const fingerprint_before = raw_fingerprint(*live);
    auto const display_before = capture_display(*live, kWrappedIds);
    auto const role_xy_before = first_xy(*live, "role-line-tspan");
    bool const modified_before = live->isModifiedSinceSave();

    auto *module = dynamic_cast<Inkscape::Extension::Output *>(
        Inkscape::Extension::db.get(SP_MODULE_KEY_OUTPUT_SVG_INKSCAPE));
    ASSERT_NE(module, nullptr);

    // Portable regular-file parent: the native writer fails after preprocessing.
    std::string const blocked_parent = create_file("not-a-directory", "sentinel");
    std::string const target = blocked_parent + "/drawing.svg";
    ASSERT_FALSE(g_file_test(target.c_str(), G_FILE_TEST_EXISTS));

    EXPECT_THROW(Inkscape::Extension::save(module, live.get(), target.c_str(), false, true,
                                           Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS),
                 Inkscape::Extension::Output::save_failed);

    EXPECT_EQ(raw_fingerprint(*live), fingerprint_before);
    EXPECT_EQ(capture_display(*live, kWrappedIds), display_before);
    auto const role_xy_after = first_xy(*live, "role-line-tspan");
    EXPECT_DOUBLE_EQ(role_xy_after.x, role_xy_before.x);
    EXPECT_DOUBLE_EQ(role_xy_after.y, role_xy_before.y);
    EXPECT_EQ(live->isModifiedSinceSave(), modified_before);

    ASSERT_TRUE(DocumentUndo::undo(live.get()));
    live->ensureUpToDate();
    ASSERT_TRUE(live->getObjectById("user-edit"));
    EXPECT_STREQ(live->getObjectById("user-edit")->getRepr()->attribute("x"), "1");
    ASSERT_TRUE(DocumentUndo::redo(live.get()));
    live->ensureUpToDate();
    EXPECT_STREQ(live->getObjectById("user-edit")->getRepr()->attribute("x"), "7");
}
