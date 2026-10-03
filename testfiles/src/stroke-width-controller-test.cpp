#include <set>
// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * SW1 acceptance tests for the read-only stroke-width resolver/query.
 *
 * Uses the existing DocPerCaseTest harness and builds real SPDocuments per case.
 */

#include <gtest/gtest.h>
#include "object/sp-lpe-item.h"
#include "object/sp-root.h"
#include <doc-per-case-test.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <filesystem>
#include <map>
#include <memory>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <glib.h>

#include "colors/color.h"
#include "document.h"
#include "document-undo.h"
#include "object/sp-item.h"
#include "object/sp-item-group.h"
#include "object/sp-rect.h"
#include "object/sp-string.h"
#include "object/sp-text.h"
#include "object/sp-tspan.h"
#include "object/sp-use.h"
#include "style.h"
#include "preferences.h"
#include "style-internal.h"
#include "text-editing.h"
#include "libnrtype/Layout-TNG.h"
#include "ui/stroke-width-controller.h"
#include "util-string/context-string.h"
#include "util/delete-with.h"
#include "util/units.h"
#include "inkgc/gc-core.h"
#include "xml/attribute-record.h"
#include "xml/document.h"
#include "xml/node.h"
#include "xml/node-observer.h"
#include "xml/repr.h"
#include "xml/sp-css-attr.h"

namespace SW = Inkscape::UI;
using namespace Inkscape;

namespace {

class StrokeWidthControllerTest : public DocPerCaseTest
{
protected:
    static std::unique_ptr<SPDocument> parse(std::string const &svg)
    {
        auto document = SPDocument::createNewDocFromMem(svg);
        EXPECT_TRUE(document);
        if (document) document->ensureUpToDate();
        return document;
    }

    static std::unique_ptr<SPDocument> mixed_fixture()
    {
        auto const path = std::filesystem::path(__FILE__).parent_path().parent_path() /
                          "data/stroke-width/mixed-members.svg";
        gchar *bytes = nullptr;
        gsize size = 0;
        if (!g_file_get_contents(path.string().c_str(), &bytes, &size, nullptr)) return {};
        std::string svg(bytes, size);
        g_free(bytes);
        return parse(svg);
    }

    static std::string serialize(SPDocument &document)
    {
        return sp_repr_save_buf(document.getReprDoc()).raw();
    }

    static SPItem *item(SPDocument &document, char const *id)
    {
        return cast<SPItem>(document.getObjectById(id));
    }

    static SW::StrokeWidthTarget const *find_excluded(SW::StrokeWidthResult const &result,
                                                      SW::StrokeWidthExclusion exclusion)
    {
        for (auto const &target : result.excluded) {
            if (target.exclusion == exclusion) return &target;
        }
        return nullptr;
    }

    // --- SW2A prepare helpers ------------------------------------------------

    static SW::StrokeWidthIntent absolute(double value, bool scale_dashes = false)
    {
        SW::StrokeWidthIntent intent;
        intent.kind = SW::StrokeWidthIntentKind::AbsoluteCssPx;
        intent.value = value;
        intent.scale_dashes = scale_dashes;
        return intent;
    }

    static SW::StrokeWidthIntent additive(double delta, bool scale_dashes = false)
    {
        return {SW::StrokeWidthIntentKind::AdditiveCssPx, delta, scale_dashes};
    }

    // Outcome check, then one exact XML Undo/Redo cycle. A no-op never publishes.
    static SW::StrokeWidthApplyResult wp1_roundtrip(
        SPDocument &document, SW::StrokeWidthPlan const &plan, std::string const &before,
        std::function<void(SW::StrokeWidthApplyResult const &)> const &check, std::size_t skipped_runs = 0)
    {
        EXPECT_EQ(serialize(document), before);
        auto const dirty = document.isModifiedSinceSave();
        auto const virgin = document.getVirgin();
        auto token = DocumentUndo::beginAtomicInteraction(&document);
        EXPECT_TRUE(token);
        if (!token) return {};
        auto const result = SW::apply_stroke_widths_compatible(document, plan, plan.scope_generation, *token);
        EXPECT_EQ(result.skipped_runs, skipped_runs);
        check(result);
        if (result.state == SW::StrokeWidthApplyState::Applied) {
            EXPECT_TRUE(SW::stroke_widths_compatible_output_ready(document, plan, plan.scope_generation, *token));
            auto const after = serialize(document);
            EXPECT_NE(after, before);
            EXPECT_TRUE(token->commitAtomically(Util::Internal::ContextString("WP1 width"), "",
                [&] { return SW::stroke_widths_compatible_output_ready(document, plan, plan.scope_generation, *token); }));
            EXPECT_TRUE(document.isModifiedSinceSave());
            EXPECT_FALSE(document.getVirgin());
            EXPECT_TRUE(DocumentUndo::undo(&document));
            EXPECT_EQ(serialize(document), before);
            EXPECT_TRUE(DocumentUndo::redo(&document));
            EXPECT_EQ(serialize(document), after);
            EXPECT_TRUE(DocumentUndo::undo(&document));
            EXPECT_EQ(serialize(document), before);
        } else {
            EXPECT_EQ(result.changed, 0u);
            if (result.state != SW::StrokeWidthApplyState::Failed) {
                EXPECT_EQ(serialize(document), before);
                EXPECT_EQ(document.isModifiedSinceSave(), dirty);
                EXPECT_EQ(document.getVirgin(), virgin);
            }
            token->rollback();
            EXPECT_EQ(serialize(document), before);
            EXPECT_EQ(document.isModifiedSinceSave(), dirty);
            EXPECT_EQ(document.getVirgin(), virgin);
        }
        return result;
    }

    static void wp1_width(SPDocument &document, char const *id, double local, double effective)
    {
        EXPECT_NEAR(computed_width(document, id), local, 1e-9 * std::max(1.0, local));
        auto const query = SW::query_stroke_widths(document, {item(document, id)});
        ASSERT_EQ(query.targets.size(), 1u);
        ASSERT_EQ(query.targets[0].runs.size(), 1u);
        ASSERT_TRUE(query.targets[0].runs[0].style.effective_px);
        EXPECT_NEAR(*query.targets[0].runs[0].style.effective_px, effective, 1e-9 * std::max(1.0, effective));
    }

    static SW::StrokeWidthIntent relative(double percent, bool scale_dashes = false)
    {
        SW::StrokeWidthIntent intent;
        intent.kind = SW::StrokeWidthIntentKind::RelativePercent;
        intent.value = percent;
        intent.scale_dashes = scale_dashes;
        return intent;
    }

    static SW::StrokeWidthMemberPlan const *find_member(SW::StrokeWidthPlan const &plan, SPItem *owner)
    {
        for (auto const &member : plan.members) {
            if (member.target.owner.get() == owner) return &member;
        }
        return nullptr;
    }

    /// Test-side oracle: the exact authored repr attributes of `item`, excluding
    /// only `style`, sorted by name. Captured from the parsed document so the
    /// test makes no assumption about authored ordering or parser normalization.
    static std::vector<std::pair<std::string, std::string>> authored_attributes(SPItem *item)
    {
        std::vector<std::pair<std::string, std::string>> attributes;
        for (auto const &attr : item->getRepr()->attributeList()) {
            char const *name = g_quark_to_string(attr.key);
            if (!name || std::string(name) == "style") continue;
            attributes.emplace_back(name, static_cast<char const *>(attr.value));
        }
        std::sort(attributes.begin(), attributes.end());
        return attributes;
    }

    /// Same oracle for flow-break controls, which are SPObject (not SPItem).
    static std::vector<std::pair<std::string, std::string>> authored_attributes(SPObject *item)
    {
        std::vector<std::pair<std::string, std::string>> attributes;
        if (!item || !item->getRepr()) return attributes;
        for (auto const &attr : item->getRepr()->attributeList()) {
            char const *name = g_quark_to_string(attr.key);
            if (!name || std::string(name) == "style") continue;
            attributes.emplace_back(name, static_cast<char const *>(attr.value));
        }
        std::sort(attributes.begin(), attributes.end());
        return attributes;
    }

    static constexpr char const *svg_open =
        R"(<svg xmlns="http://www.w3.org/2000/svg" )"
        R"(xmlns:xlink="http://www.w3.org/1999/xlink" )"
        R"(xmlns:sodipodi="http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd">)";

    // --- SW2B writer helpers -------------------------------------------------

    /// Locally settle a freshly parsed fixture so a caller-owned atomic token can
    /// open without inheriting a partial log. Shared parse() is untouched.
    static void settle(SPDocument &document)
    {
        Inkscape::DocumentUndo::done(&document, Inkscape::Util::Internal::ContextString("SW2B fixture"), "");
        Inkscape::DocumentUndo::clearUndo(&document);
        Inkscape::DocumentUndo::clearRedo(&document);
        document.setModifiedSinceSave(false);
    }

    /// Native SPCSSAttr property map of an item's inline style.
    static std::map<std::string, std::string> inline_style_map(SPItem *item)
    {
        std::map<std::string, std::string> out;
        if (!item || !item->getRepr()) return out;
        auto css = Util::delete_with<sp_repr_css_attr_unref>(sp_repr_css_attr(item->getRepr(), "style"));
        for (auto const &attr : css->attributeList()) {
            char const *name = g_quark_to_string(attr.key);
            if (!name || !attr.value) continue;
            out.emplace(name, static_cast<char const *>(attr.value));
        }
        return out;
    }

    /// Same inline-style oracle for flow-break controls (SPObject, not SPItem).
    static std::map<std::string, std::string> inline_style_map(SPObject *item)
    {
        std::map<std::string, std::string> out;
        if (!item || !item->getRepr()) return out;
        auto css = Util::delete_with<sp_repr_css_attr_unref>(sp_repr_css_attr(item->getRepr(), "style"));
        for (auto const &attr : css->attributeList()) {
            char const *name = g_quark_to_string(attr.key);
            if (!name || !attr.value) continue;
            out.emplace(name, static_cast<char const *>(attr.value));
        }
        return out;
    }

    static double computed_width(SPDocument &document, char const *id)
    {
        auto *owner = item(document, id);
        EXPECT_TRUE(owner);
        return owner && owner->style ? owner->style->stroke_width.computed : -1.0;
    }

    // --- SW3 native diagnostic helpers --------------------------------------
    // Read-only per-character observation of computed content/fill/font via the
    // authoritative layout source, plus width via the read-only resolver. Owner
    // style alone is never used: range styling can live on normalized child spans.

    struct NativeCharIdentity {
        gunichar character = 0;
        Glib::ustring fill;
        double font_size = 0.0;
        Glib::ustring font_family;

        bool operator==(NativeCharIdentity const &other) const
        {
            return character == other.character && fill == other.fill
                && font_size == other.font_size && font_family == other.font_family;
        }
        bool operator!=(NativeCharIdentity const &other) const { return !(*this == other); }
    };

    struct NativeCharWidths {
        double local = 0.0;
        std::optional<double> effective;
        SW::StrokeWidthConvention convention = SW::StrokeWidthConvention::Ordinary;
        SPObject *style_source = nullptr; ///< run source (string chars only)
    };

    static std::string numeric_text(double value)
    {
        char buf[64];
        g_ascii_dtostr(buf, sizeof(buf), value);
        return buf;
    }

    static char const *convention_text(SW::StrokeWidthConvention convention)
    {
        switch (convention) {
        case SW::StrokeWidthConvention::Ordinary: return "Ordinary";
        case SW::StrokeWidthConvention::NonScaling: return "NonScaling";
        case SW::StrokeWidthConvention::Hairline: return "Hairline";
        }
        return "Unknown";
    }

    static NativeCharIdentity native_char_identity(SPItem *owner, unsigned index)
    {
        NativeCharIdentity identity;
        auto *layout = te_get_layout(owner);
        EXPECT_TRUE(layout);
        if (!layout) return identity;
        auto const count = static_cast<unsigned>(layout->iteratorToCharIndex(layout->end()));
        EXPECT_LT(index, count);
        if (index >= count) return identity;
        auto const it = layout->charIndexToIterator(static_cast<int>(index));
        auto const character = layout->characterAt(it);
        SPStyle const *style = sp_te_style_at_position(owner, it);
        EXPECT_TRUE(style);
        if (!style) return identity;
        identity.character = character;
        identity.fill = style->fill.get_value();
        identity.font_size = style->font_size.computed;
        identity.font_family = style->font_family.get_value();
        return identity;
    }

    static NativeCharWidths native_char_widths(SPDocument &document, SPItem *owner, unsigned index)
    {
        NativeCharWidths widths;
        SW::StrokeWidthTextRange range;
        range.owner = owner;
        range.first_char = index;
        range.last_char = index + 1;
        auto const result = SW::query_stroke_widths(document, range, {owner});
        EXPECT_FALSE(result.range_rejected);
        EXPECT_EQ(result.eligible, 1u);
        EXPECT_EQ(result.targets.size(), 1u);
        if (result.range_rejected || result.eligible != 1u || result.targets.size() != 1u) {
            return widths;
        }
        auto const &target = result.targets[0];
        EXPECT_EQ(target.runs.size(), 1u);
        if (target.runs.size() != 1u) return widths;
        auto const &run = target.runs[0];
        EXPECT_EQ(run.first_char, index);
        EXPECT_EQ(run.last_char, index + 1);
        if (run.first_char != index || run.last_char != index + 1) return widths;
        auto const &style = run.style;
        widths.local = style.local_computed;
        widths.effective = style.effective_px;
        widths.convention = style.convention;
        widths.style_source = run.style_source.get();
        return widths;
    }

    static unsigned native_char_count(SPItem *owner)
    {
        auto *layout = te_get_layout(owner);
        return layout ? static_cast<unsigned>(layout->iteratorToCharIndex(layout->end())) : 0;
    }

    /// Native width write through the existing default-argument range engine.
    /// The layout and both iterators are reacquired per call and never retained.
    static void native_apply_width(SPDocument &document, SPItem *owner, unsigned lo, unsigned hi,
                                   char const *value)
    {
        auto *layout = te_get_layout(owner);
        ASSERT_TRUE(layout);
        auto const count = static_cast<unsigned>(layout->iteratorToCharIndex(layout->end()));
        ASSERT_LT(lo, hi);
        ASSERT_LE(hi, count);
        SPCSSAttr *css = sp_repr_css_attr_new();
        sp_repr_css_set_property(css, "stroke-width", value);
        sp_te_apply_style(owner, layout->charIndexToIterator(static_cast<int>(lo)),
                          layout->charIndexToIterator(static_cast<int>(hi)), css);
        sp_repr_css_attr_unref(css);
        document.ensureUpToDate();
    }

    /// Native layout rendering scope for one logical character. The renderer
    /// draws with the style passed separately to Layout::appendText, which for
    /// ordinary text characters is the immediate parent element of the SPString
    /// source returned by getSourceOfCharacter (src/object/sp-text.cpp:780;
    /// src/libnrtype/Layout-TNG-Output.cpp:188). The SPString's own style lacks
    /// the non-inheriting vector-effect and its priority, so it must not be used
    /// here. An unavailable mapping records a test failure and returns no style;
    /// it never falls back to the owner style.
    struct NativeRenderStyle {
        SPObject *source = nullptr;
        SPObject *element = nullptr;
        SPStyle const *style = nullptr;
    };

    static NativeRenderStyle native_render_style_at(SPItem *owner, unsigned index)
    {
        NativeRenderStyle out;
        auto *layout = te_get_layout(owner);
        EXPECT_TRUE(layout);
        if (!layout) return out;
        auto const count = static_cast<unsigned>(layout->iteratorToCharIndex(layout->end()));
        EXPECT_LT(index, count);
        if (index >= count) return out;
        auto const it = layout->charIndexToIterator(static_cast<int>(index));
        SPObject *source = nullptr;
        layout->getSourceOfCharacter(it, &source);
        EXPECT_TRUE(source);
        EXPECT_TRUE(is<SPString>(source));
        if (!source || !is<SPString>(source)) return out;
        SPObject *element = source->parent;
        EXPECT_TRUE(element);
        if (!element) return out;
        EXPECT_TRUE(element->style);
        if (!element->style) return out;
        out.source = source;
        out.element = element;
        out.style = element->style;
        return out;
    }

    /// Native parsed stroke readout at one logical character via the layout
    /// source: local computed width, computed dash list/offset, the three
    /// independent priority flags, and the full vector-effect value/priority.
    struct NativeCharStroke {
        double width = 0.0;
        std::vector<double> dasharray;
        double dashoffset = 0.0;
        bool width_important = false;
        bool dasharray_important = false;
        bool dashoffset_important = false;
        Glib::ustring vector_effect;
        bool vector_effect_important = false;
    };

    static NativeCharStroke native_char_stroke(SPItem *owner, unsigned index)
    {
        NativeCharStroke out;
        SPStyle const *style = native_render_style_at(owner, index).style;
        EXPECT_TRUE(style);
        if (!style) return out;
        out.width = style->stroke_width.computed;
        out.dasharray = style->stroke_dasharray.get_computed();
        out.dashoffset = style->stroke_dashoffset.computed;
        out.width_important = style->stroke_width.important;
        out.dasharray_important = style->stroke_dasharray.important;
        out.dashoffset_important = style->stroke_dashoffset.important;
        out.vector_effect = style->vector_effect.get_value();
        out.vector_effect_important = style->vector_effect.important;
        return out;
    }

    /// Caller-owned freeze of each actual logical character's full native
    /// vector-effect value and priority. The whole range must carry one frozen
    /// convention/priority; the returned pair is copied into value types before
    /// any mutation, so no style/layout pointer is retained across the write.
    struct FrozenVectorEffect {
        Glib::ustring value;
        bool important = false;
    };

    static FrozenVectorEffect frozen_vector_effect(SPItem *owner, unsigned lo, unsigned hi)
    {
        FrozenVectorEffect frozen;
        auto *layout = te_get_layout(owner);
        EXPECT_TRUE(layout);
        if (!layout) return frozen;
        auto const count = static_cast<unsigned>(layout->iteratorToCharIndex(layout->end()));
        EXPECT_LT(lo, hi);
        EXPECT_LE(hi, count);
        if (lo >= hi || hi > count) return frozen;
        for (unsigned i = lo; i < hi; ++i) {
            SPStyle const *style = native_render_style_at(owner, i).style;
            EXPECT_TRUE(style);
            if (!style) return frozen;
            Glib::ustring const value = style->vector_effect.get_value();
            bool const important = style->vector_effect.important;
            if (i == lo) {
                frozen.value = value;
                frozen.important = important;
            } else {
                EXPECT_TRUE(value == frozen.value);
                EXPECT_EQ(important, frozen.important);
            }
        }
        return frozen;
    }

    /// Caller-provided local stroke CSS. An empty string means the property is
    /// absent from the input and must never be inserted or removed.
    struct NativeLocalStrokeInput {
        std::string width;
        std::string dasharray;
        std::string dashoffset;
    };

    /// Native opt-in stroke write through the explicit overload. The caller has
    /// already built `css` (including the frozen vector-effect value/priority);
    /// layout and both iterators are reacquired for this call and never retained.
    static void native_apply_local_stroke(SPDocument &document, SPItem *owner, unsigned lo, unsigned hi,
                                          SPCSSAttr *css, TextStyleLocalStroke const &flags)
    {
        auto *layout = te_get_layout(owner);
        ASSERT_TRUE(layout);
        auto const count = static_cast<unsigned>(layout->iteratorToCharIndex(layout->end()));
        ASSERT_LT(lo, hi);
        ASSERT_LE(hi, count);
        sp_te_apply_style(owner, layout->charIndexToIterator(static_cast<int>(lo)),
                          layout->charIndexToIterator(static_cast<int>(hi)), css,
                          TextStyleLocalSpacing{}, flags);
        document.ensureUpToDate();
    }

    static std::map<std::string, std::string> css_attr_map(SPCSSAttr *css)
    {
        std::map<std::string, std::string> out;
        if (!css) return out;
        for (auto const &attr : css->attributeList()) {
            char const *name = g_quark_to_string(attr.key);
            if (!name || !attr.value) continue;
            out.emplace(name, static_cast<char const *>(attr.value));
        }
        return out;
    }

    static unsigned count_tspans(SPObject *object)
    {
        unsigned count = 0;
        for (auto &child : object->children) {
            if (is<SPTSpan>(&child)) ++count;
            count += count_tspans(&child);
        }
        return count;
    }

    /// Baseline fatal facts shared by cases 1-3 (and case 4): the owner is an
    /// eligible whole text target at transform scale 2 with the authored 2px
    /// local width and the expected ordinary/non-scaling convention.
    static void assert_native_baseline(SPDocument &document, SPItem *owner, bool non_scaling)
    {
        auto *layout = te_get_layout(owner);
        ASSERT_TRUE(layout);
        ASSERT_EQ(layout->iteratorToCharIndex(layout->end()), 4);
        auto const whole = SW::query_stroke_widths(document, {owner});
        ASSERT_EQ(whole.state, SW::StrokeWidthQuery::Uniform);
        ASSERT_EQ(whole.eligible, 1u);
        ASSERT_EQ(whole.non_scaling, non_scaling ? 1u : 0u);
        ASSERT_EQ(whole.targets.size(), 1u);
        ASSERT_EQ(whole.targets[0].kind, SW::StrokeWidthTargetKind::TextOwner);
        ASSERT_DOUBLE_EQ(whole.targets[0].transform_scale, 2.0);

        for (unsigned i = 0; i < 4; ++i) {
            auto const widths = native_char_widths(document, owner, i);
            ASSERT_DOUBLE_EQ(widths.local, 2.0);
            ASSERT_TRUE(widths.effective.has_value());
            ASSERT_DOUBLE_EQ(*widths.effective, non_scaling ? 2.0 : 4.0);
            ASSERT_EQ(widths.convention, non_scaling ? SW::StrokeWidthConvention::NonScaling
                                                     : SW::StrokeWidthConvention::Ordinary);
        }
    }

    void record_char_widths(char const *prefix, unsigned index, NativeCharWidths const &widths)
    {
        std::string const base = std::string(prefix) + ".char" + std::to_string(index);
        RecordProperty(base + ".local", numeric_text(widths.local));
        RecordProperty(base + ".effective",
                       widths.effective ? numeric_text(*widths.effective) : std::string("none"));
        RecordProperty(base + ".convention", convention_text(widths.convention));
    }

    /// Record the observed native widths, then assert the required controller
    /// contract independently. A mismatch is an intentional current-path failure.
    void expect_required_widths(char const *prefix, unsigned index, NativeCharWidths const &widths,
                                double required_local, double required_effective)
    {
        SCOPED_TRACE(std::string(prefix) + " char " + std::to_string(index)
                     + " actual local=" + numeric_text(widths.local)
                     + " effective="
                     + (widths.effective ? numeric_text(*widths.effective) : std::string("none"))
                     + " convention=" + convention_text(widths.convention));
        record_char_widths(prefix, index, widths);
        EXPECT_DOUBLE_EQ(widths.local, required_local);
        EXPECT_TRUE(widths.effective.has_value());
        if (widths.effective) {
            EXPECT_DOUBLE_EQ(*widths.effective, required_effective);
        }
    }

    // --- SW3 A flow-source diagnostics --------------------------------------

    struct NativeCharSource {
        bool valid = false;
        bool has_glyph = false;
        gunichar value = 0;
        SPObject *source = nullptr;
        SPObject *parent = nullptr;
    };

    /// Raw per-character layout facts; the source may be a break SPObject, so it
    /// is never assumed to be an SPString and no glyph geometry is used.
    static NativeCharSource native_char_source(SPItem *owner, unsigned index)
    {
        NativeCharSource out;
        auto *layout = te_get_layout(owner);
        EXPECT_TRUE(layout);
        if (!layout) return out;
        auto const count = static_cast<unsigned>(layout->iteratorToCharIndex(layout->end()));
        EXPECT_LT(index, count);
        if (index >= count) return out;
        auto const it = layout->charIndexToIterator(static_cast<int>(index));
        out.valid = true;
        out.has_glyph = it.hasGlyph();
        out.value = layout->characterAt(it);
        layout->getSourceOfCharacter(it, &out.source);
        out.parent = out.source ? out.source->parent : nullptr;
        return out;
    }

    /// Record per-character native facts for the independent flow review; `style`
    /// is non-null only for a real SPString character.
    void record_native_char(char const *prefix, unsigned index, NativeCharSource const &observed,
                            SPStyle const *style = nullptr)
    {
        std::string const base = std::string(prefix) + ".char" + std::to_string(index);
        RecordProperty(base + ".index", std::to_string(index));
        RecordProperty(base + ".value", std::to_string(static_cast<unsigned>(observed.value)));
        RecordProperty(base + ".hasGlyph", observed.has_glyph ? "true" : "false");
        RecordProperty(base + ".parentId",
                       observed.parent && observed.parent->getId() ? observed.parent->getId() : "none");
        RecordProperty(base + ".sourceType",
                       observed.source && is<SPString>(observed.source) ? "SPString" : "other");
        RecordProperty(base + ".sourceId",
                       observed.source && observed.source->getId() ? observed.source->getId() : "none");
        if (style) {
            RecordProperty(base + ".vectorEffect", style->vector_effect.get_value().raw());
            RecordProperty(base + ".vectorEffectImportant", style->vector_effect.important ? "true" : "false");
        }
    }

    /// Assert one real flow/text character and its one-character query identity.
    void expect_flow_string_char(char const *prefix, SPDocument &document, SPItem *owner, unsigned index,
                                 gunichar expected_value, SPObject *expected_element, char const *ve,
                                 bool ve_important, double local, double effective,
                                 SW::StrokeWidthConvention convention)
    {
        SCOPED_TRACE(std::string(prefix) + " char " + std::to_string(index));
        auto const observed = native_char_source(owner, index);
        ASSERT_TRUE(observed.valid);
        ASSERT_TRUE(observed.has_glyph);
        EXPECT_EQ(observed.value, expected_value);
        ASSERT_TRUE(observed.source && is<SPString>(observed.source));
        ASSERT_EQ(observed.parent, expected_element);
        auto const render = native_render_style_at(owner, index);
        ASSERT_TRUE(render.style);
        EXPECT_EQ(render.element, expected_element);
        ASSERT_STREQ(render.style->vector_effect.get_value().c_str(), ve);
        EXPECT_EQ(render.style->vector_effect.important, ve_important);
        record_native_char(prefix, index, observed, render.style);
        auto const widths = native_char_widths(document, owner, index);
        expect_required_widths(prefix, index, widths, local, effective);
        EXPECT_EQ(widths.convention, convention);
        EXPECT_EQ(widths.style_source, expected_element);
    }

    /// Assert one non-glyph break: raw source is the break object, not SPString;
    /// no rendered width/style is required here.
    void expect_flow_break(SPItem *owner, SPObject *expected_break, unsigned index, char const *prefix)
    {
        auto const observed = native_char_source(owner, index);
        ASSERT_TRUE(observed.valid);
        EXPECT_FALSE(observed.has_glyph);
        EXPECT_EQ(observed.source, expected_break);
        EXPECT_FALSE(observed.source && is<SPString>(observed.source));
        record_native_char(prefix, index, observed);
    }
};

/// Real native XML observer (no controller hook) that mutates the second target's
/// transform the first time the first target's style attribute is written.
class SecondTargetTransformObserver : public Inkscape::XML::NodeObserver
{
public:
    SecondTargetTransformObserver(Inkscape::XML::Node &observed, Inkscape::XML::Node *second)
        : _observed(&observed)
        , _second(second)
    {
        observed.addObserver(*this);
    }

    ~SecondTargetTransformObserver() override
    {
        detach();
    }

    /// Unregister while the observed node is still alive (rollback may rebuild
    /// the observed repr, so the observer must not outlive its registration).
    void detach()
    {
        if (_observed) {
            _observed->removeObserver(*this);
            _observed = nullptr;
        }
    }

    void notifyAttributeChanged(Inkscape::XML::Node &, GQuark name, Inkscape::Util::ptr_shared,
                                Inkscape::Util::ptr_shared) override
    {
        if (_fired) return;
        if (std::strcmp(g_quark_to_string(name), "style") != 0) return;
        _fired = true;
        if (_second) _second->removeAttribute("transform");
    }

    bool fired() const { return _fired; }

private:
    Inkscape::XML::Node *_observed = nullptr;
    Inkscape::XML::Node *_second = nullptr;
    bool _fired = false;
};

/// Real native single-fire XML observer (no controller hook): the first `style`
/// attribute change on the observed node runs `action` exactly once. Used to
/// simulate a callback that mutates another target or an ancestor while the
/// writer is mid-transaction.
class SingleFireStyleObserver : public Inkscape::XML::NodeObserver
{
public:
    SingleFireStyleObserver(Inkscape::XML::Node &observed, std::function<void()> action)
        : _observed(&observed)
        , _action(std::move(action))
    {
        observed.addObserver(*this);
    }

    ~SingleFireStyleObserver() override
    {
        detach();
    }

    /// Unregister while the observed node is still alive (rollback may rebuild
    /// the observed repr, so the observer must not outlive its registration).
    void detach()
    {
        if (_observed) {
            _observed->removeObserver(*this);
            _observed = nullptr;
        }
    }

    void notifyAttributeChanged(Inkscape::XML::Node &, GQuark name, Inkscape::Util::ptr_shared,
                                Inkscape::Util::ptr_shared) override
    {
        if (_fired) return;
        if (std::strcmp(g_quark_to_string(name), "style") != 0) return;
        _fired = true;
        if (_action) _action();
    }

    bool fired() const { return _fired; }

private:
    Inkscape::XML::Node *_observed = nullptr;
    std::function<void()> _action;
    bool _fired = false;
};

// ---------------------------------------------------------------------------
// A03: equal 2 px vectors plus a bitmap stay Uniform 2; order-independent;
// bitmap excluded without being written.
// ---------------------------------------------------------------------------

TEST_F(StrokeWidthControllerTest, A03EqualVectorsAndBitmapStayUniform)
{
    auto document = parse(std::string{svg_open} +
        R"(<g id="grp">)"
        R"(<rect id="a" width="10" height="10" style="stroke-width:2px"/>)"
        R"(<rect id="b" width="10" height="10" style="stroke-width:2px"/>)"
        R"(<rect id="c" width="10" height="10" style="stroke-width:2px"/>)"
        R"(<image id="img" width="10" height="10"/>)"
        R"(</g>)"
        R"(</svg>)");
    ASSERT_TRUE(document);

    auto const before = serialize(*document);
    std::vector<SPItem *> roots = {item(*document, "a"), item(*document, "b"),
                                   item(*document, "c"), item(*document, "img")};
    auto const result = SW::query_stroke_widths(*document, roots);

    EXPECT_EQ(result.state, SW::StrokeWidthQuery::Uniform);
    ASSERT_TRUE(result.uniform_px.has_value());
    EXPECT_NEAR(*result.uniform_px, 2.0, 1e-9);
    EXPECT_EQ(result.eligible, 3u);
    EXPECT_EQ(result.incompatible, 1u);
    EXPECT_EQ(result.unavailable, 0u);
    EXPECT_EQ(result.covered, 0u);
    EXPECT_TRUE(find_excluded(result, SW::StrokeWidthExclusion::Bitmap));
    for (auto const &target : result.targets) {
        EXPECT_NE(target.owner.get(), item(*document, "img"));
    }

    // Selection order must not change the result.
    std::vector<SPItem *> permuted = {item(*document, "c"), item(*document, "img"),
                                      item(*document, "a"), item(*document, "b")};
    auto const permuted_result = SW::query_stroke_widths(*document, permuted);
    EXPECT_EQ(permuted_result.state, SW::StrokeWidthQuery::Uniform);
    ASSERT_TRUE(permuted_result.uniform_px.has_value());
    EXPECT_NEAR(*permuted_result.uniform_px, 2.0, 1e-9);
    EXPECT_EQ(permuted_result.eligible, 3u);
    EXPECT_EQ(permuted_result.incompatible, 1u);

    // Group root produces the same compatible members and no group target.
    auto const group_result = SW::query_stroke_widths(*document, {item(*document, "grp")});
    EXPECT_EQ(group_result.state, SW::StrokeWidthQuery::Uniform);
    EXPECT_EQ(group_result.eligible, 3u);
    EXPECT_EQ(group_result.incompatible, 1u);
    for (auto const &target : group_result.targets) {
        EXPECT_NE(target.owner.get(), item(*document, "grp"));
    }

    EXPECT_EQ(serialize(*document), before);
}

// ---------------------------------------------------------------------------
// A04: bitmap-only is Empty; XML/history untouched; a seeded Redo survives.
// ---------------------------------------------------------------------------

TEST_F(StrokeWidthControllerTest, A04BitmapOnlyIsEmptyAndPreservesXml)
{
    auto document = parse(std::string{svg_open} +
        R"(<image id="img" width="10" height="10"/>)"
        R"(</svg>)");
    ASSERT_TRUE(document);

    auto const before = serialize(*document);
    auto const result = SW::query_stroke_widths(*document, {item(*document, "img")});

    EXPECT_EQ(result.state, SW::StrokeWidthQuery::Empty);
    EXPECT_EQ(result.eligible, 0u);
    EXPECT_EQ(result.incompatible, 1u);
    EXPECT_TRUE(find_excluded(result, SW::StrokeWidthExclusion::Bitmap));
    EXPECT_TRUE(result.targets.empty());
    EXPECT_EQ(serialize(*document), before);
}

TEST_F(StrokeWidthControllerTest, A04SeededRedoSurvivesQuery)
{
    auto document = parse(std::string{svg_open} +
        R"(<rect id="a" width="10" height="10" style="stroke-width:2px"/>)"
        R"(</svg>)");
    ASSERT_TRUE(document);

    auto *rect = item(*document, "a");
    ASSERT_TRUE(rect);
    rect->setLocked(true);
    Inkscape::DocumentUndo::done(document.get(), Inkscape::Util::Internal::ContextString("Fixture"), "");
    ASSERT_TRUE(Inkscape::DocumentUndo::undo(document.get()));

    auto const before = serialize(*document);
    auto const result = SW::query_stroke_widths(*document, {rect});
    EXPECT_EQ(result.eligible, 1u);

    EXPECT_EQ(serialize(*document), before);
    EXPECT_TRUE(Inkscape::DocumentUndo::redo(document.get()));
}

// ---------------------------------------------------------------------------
// A06: nested groups, bitmap, hidden/locked ancestry, duplicate roots and
// directly selected protected/text descendants.
// ---------------------------------------------------------------------------

TEST_F(StrokeWidthControllerTest, A06GroupsProtectionAndCoverage)
{
    auto document = parse(std::string{svg_open} +
        R"(<g id="outer">)"
        R"(<rect id="r2" width="10" height="10" style="stroke-width:2"/>)"
        R"(<g id="inner"><rect id="r8" width="10" height="10" style="stroke-width:8"/></g>)"
        R"(<image id="img" width="10" height="10"/>)"
        R"(<g id="hidden" style="display:none"><rect id="rh" width="10" height="10" style="stroke-width:4"/></g>)"
        R"(<g id="locked" sodipodi:insensitive="true"><rect id="rl" width="10" height="10" style="stroke-width:4"/></g>)"
        R"(</g>)"
        R"(<text id="txt" x="0" y="80" style="font-family:sans-serif;font-size:20px"><tspan id="tsp">x</tspan></text>)"
        R"(<flowRoot id="flow" style="font-family:sans-serif;font-size:10px"><flowRegion id="region"><rect id="frect" width="50" height="20"/></flowRegion><flowPara id="fpara">x</flowPara></flowRoot>)"
        R"(</svg>)");
    ASSERT_TRUE(document);

    auto const before = serialize(*document);

    // Group root: eligible leaves once each, no group broadcast.
    auto const group_result = SW::query_stroke_widths(*document, {item(*document, "outer")});
    EXPECT_EQ(group_result.state, SW::StrokeWidthQuery::Mixed);
    EXPECT_EQ(group_result.eligible, 2u);
    EXPECT_EQ(group_result.incompatible, 1u);
    EXPECT_EQ(group_result.unavailable, 2u);
    EXPECT_EQ(group_result.covered, 0u);
    EXPECT_TRUE(find_excluded(group_result, SW::StrokeWidthExclusion::Bitmap));
    EXPECT_TRUE(find_excluded(group_result, SW::StrokeWidthExclusion::HiddenAncestor));
    EXPECT_TRUE(find_excluded(group_result, SW::StrokeWidthExclusion::LockedAncestor));
    // Groups themselves are never targets.
    for (auto const &target : group_result.targets) {
        EXPECT_NE(target.owner.get(), item(*document, "outer"));
        EXPECT_NE(target.owner.get(), item(*document, "inner"));
    }

    // Group plus an explicitly selected child: the child is compatible once.
    auto const group_child = SW::query_stroke_widths(
        *document, {item(*document, "outer"), item(*document, "r2")});
    EXPECT_EQ(group_child.eligible, 2u);
    EXPECT_EQ(group_child.covered, 1u);

    // Duplicate root: compatible once.
    auto const duplicate = SW::query_stroke_widths(
        *document, {item(*document, "r2"), item(*document, "r2")});
    EXPECT_EQ(duplicate.eligible, 1u);
    EXPECT_EQ(duplicate.covered, 1u);

    // Directly selected child of a protected ancestor stays unavailable.
    auto const hidden_child = SW::query_stroke_widths(*document, {item(*document, "rh")});
    EXPECT_EQ(hidden_child.eligible, 0u);
    EXPECT_EQ(hidden_child.unavailable, 1u);
    EXPECT_TRUE(find_excluded(hidden_child, SW::StrokeWidthExclusion::HiddenAncestor));

    auto const locked_child = SW::query_stroke_widths(*document, {item(*document, "rl")});
    EXPECT_EQ(locked_child.eligible, 0u);
    EXPECT_EQ(locked_child.unavailable, 1u);
    EXPECT_TRUE(find_excluded(locked_child, SW::StrokeWidthExclusion::LockedAncestor));

    // A directly selected text span is not independent artwork.
    auto const span = SW::query_stroke_widths(*document, {item(*document, "tsp")});
    EXPECT_EQ(span.eligible, 0u);
    EXPECT_EQ(span.unavailable, 1u);
    EXPECT_TRUE(find_excluded(span, SW::StrokeWidthExclusion::TextDescendant));

    // Flow-region geometry is owned by its text owner and is not independent.
    auto const region_shape = SW::query_stroke_widths(*document, {item(*document, "frect")});
    EXPECT_EQ(region_shape.eligible, 0u);
    EXPECT_EQ(region_shape.unavailable, 1u);
    EXPECT_TRUE(find_excluded(region_shape, SW::StrokeWidthExclusion::TextDescendant));

    EXPECT_EQ(serialize(*document), before);
}

// ---------------------------------------------------------------------------
// A07: text run extraction with logical indices and inherited dash owner.
// ---------------------------------------------------------------------------

TEST_F(StrokeWidthControllerTest, A07TextRunsAndInheritedDash)
{
    auto document = parse(std::string{svg_open} +
        R"(<text id="t" x="0" y="20" style="font-family:sans-serif;font-size:20px;stroke-dasharray:4 2;stroke-dashoffset:1">)"
        R"(<tspan id="a" style="stroke-width:0">aaa</tspan>)"
        R"(<tspan id="b" style="stroke-width:2">bbb</tspan>)"
        R"(<tspan id="c" style="stroke-width:8">ccc</tspan>)"
        R"(</text>)"
        R"(</svg>)");
    ASSERT_TRUE(document);

    auto const before = serialize(*document);
    auto const result = SW::query_stroke_widths(*document, {item(*document, "t")});

    EXPECT_EQ(result.state, SW::StrokeWidthQuery::Mixed);
    EXPECT_EQ(result.eligible, 1u);
    ASSERT_EQ(result.targets.size(), 1u);
    auto const &target = result.targets[0];
    EXPECT_EQ(target.kind, SW::StrokeWidthTargetKind::TextOwner);
    ASSERT_EQ(target.runs.size(), 3u);

    EXPECT_EQ(target.runs[0].first_char, 0u);
    EXPECT_EQ(target.runs[0].last_char, 3u);
    EXPECT_EQ(target.runs[1].first_char, 3u);
    EXPECT_EQ(target.runs[1].last_char, 6u);
    EXPECT_EQ(target.runs[2].first_char, 6u);
    EXPECT_EQ(target.runs[2].last_char, 9u);

    EXPECT_NEAR(target.runs[0].style.local_computed, 0.0, 1e-9);
    EXPECT_NEAR(target.runs[1].style.local_computed, 2.0, 1e-9);
    EXPECT_NEAR(target.runs[2].style.local_computed, 8.0, 1e-9);

    // Dash pattern is inherited from the text owner, not the width owner.
    for (auto const &run : target.runs) {
        ASSERT_EQ(run.style.dash_computed.size(), 2u);
        EXPECT_NEAR(run.style.dash_computed[0], 4.0, 1e-9);
        EXPECT_NEAR(run.style.dash_computed[1], 2.0, 1e-9);
        EXPECT_NEAR(run.style.dash_offset_computed, 1.0, 1e-9);
        EXPECT_FALSE(run.style.dash_set);
    }

    EXPECT_EQ(serialize(*document), before);
}

// ---------------------------------------------------------------------------
// A08: explicit range eclipses roots; reverse direction; caret; rejection.
// ---------------------------------------------------------------------------

TEST_F(StrokeWidthControllerTest, A08TextRangeScopeAndRejection)
{
    auto document = parse(std::string{svg_open} +
        R"svg(<text id="t" x="0" y="20" transform="scale(2)" style="font-family:sans-serif;font-size:20px">)svg"
        R"(aaa<tspan id="p" sodipodi:insensitive="true" style="-inkscape-stroke:hairline;vector-effect:non-scaling-stroke">bbb</tspan>ccc</text>)"
        R"(<text id="zero" x="0" y="60" style="font-family:sans-serif;font-size:20px"></text>)"
        R"(<rect id="other" width="10" height="10" style="stroke-width:5"/>)"
        R"(</svg>)");
    ASSERT_TRUE(document);

    auto const before = serialize(*document);
    auto *text = item(*document, "t");
    ASSERT_TRUE(text);

    // Explicit range [0,6) eclipses the unrelated root and excludes the locked run.
    SW::StrokeWidthTextRange range;
    range.owner = text;
    range.first_char = 0;
    range.last_char = 6;
    auto const ranged = SW::query_stroke_widths(
        *document, range, {text, item(*document, "other")});
    EXPECT_EQ(ranged.state, SW::StrokeWidthQuery::Uniform);
    ASSERT_TRUE(ranged.uniform_px.has_value());
    EXPECT_NEAR(*ranged.uniform_px, 2.0, 1e-9);
    EXPECT_EQ(ranged.eligible, 1u);
    EXPECT_EQ(ranged.covered, 0u);
    ASSERT_EQ(ranged.targets.size(), 1u);
    EXPECT_EQ(ranged.targets[0].owner.get(), text);
    EXPECT_NEAR(ranged.targets[0].transform_scale, 2.0, 1e-9);
    EXPECT_FALSE(ranged.targets[0].reversed);
    ASSERT_EQ(ranged.targets[0].runs.size(), 2u);
    EXPECT_EQ(ranged.targets[0].runs[1].eligibility, SW::StrokeWidthEligibility::Unavailable);
    EXPECT_EQ(ranged.targets[0].runs[1].exclusion, SW::StrokeWidthExclusion::LockedAncestor);
    EXPECT_TRUE(ranged.targets[0].runs[1].style_source);
    EXPECT_EQ(ranged.targets[0].runs[1].style.convention, SW::StrokeWidthConvention::Hairline);
    EXPECT_TRUE(ranged.targets[0].runs[1].style.non_scaling);
    EXPECT_EQ(ranged.hairline, 0u);
    EXPECT_EQ(ranged.non_scaling, 0u);
    // No target for the unrelated root.
    for (auto const &target : ranged.targets) {
        EXPECT_NE(target.owner.get(), item(*document, "other"));
    }

    // Reverse direction is recorded raw and normalized only for traversal.
    SW::StrokeWidthTextRange reverse = range;
    reverse.first_char = 6;
    reverse.last_char = 0;
    auto const reversed = SW::query_stroke_widths(*document, reverse, {text});
    ASSERT_EQ(reversed.targets.size(), 1u);
    EXPECT_TRUE(reversed.targets[0].reversed);
    EXPECT_EQ(reversed.targets[0].raw_first_char, 6u);
    EXPECT_EQ(reversed.targets[0].raw_last_char, 0u);
    EXPECT_EQ(reversed.targets[0].first_char, 0u);
    EXPECT_EQ(reversed.targets[0].last_char, 6u);

    // Caret means the whole owner. A nonzero caller caret keeps its raw collapsed
    // index while the normalized edit range is the whole owner [0, count).
    SW::StrokeWidthTextRange caret;
    caret.owner = text;
    caret.caret = true;
    caret.first_char = 4;
    caret.last_char = 4;
    auto const caret_result = SW::query_stroke_widths(*document, caret, {text});
    ASSERT_EQ(caret_result.targets.size(), 1u);
    EXPECT_TRUE(caret_result.targets[0].whole_object);
    EXPECT_TRUE(caret_result.targets[0].caret_scope);
    EXPECT_EQ(caret_result.targets[0].first_char, 0u);
    EXPECT_EQ(caret_result.targets[0].last_char, 9u);
    EXPECT_EQ(caret_result.targets[0].raw_first_char, 4u);
    EXPECT_EQ(caret_result.targets[0].raw_last_char, 4u);

    // A collapsed caret past the logical end rejects as out of bounds; a
    // non-collapsed caret is not a scope and rejects as invalid. Neither may
    // broaden a stale caret into a whole-owner query.
    SW::StrokeWidthTextRange caret_oob = caret;
    caret_oob.first_char = 100;
    caret_oob.last_char = 100;
    auto const caret_oob_result = SW::query_stroke_widths(*document, caret_oob, {text});
    EXPECT_TRUE(caret_oob_result.range_rejected);
    EXPECT_EQ(caret_oob_result.range_exclusion, SW::StrokeWidthExclusion::TextRangeOutOfBounds);
    EXPECT_TRUE(caret_oob_result.targets.empty());

    SW::StrokeWidthTextRange caret_open = caret;
    caret_open.first_char = 1;
    caret_open.last_char = 2;
    auto const caret_open_result = SW::query_stroke_widths(*document, caret_open, {text});
    EXPECT_TRUE(caret_open_result.range_rejected);
    EXPECT_EQ(caret_open_result.range_exclusion, SW::StrokeWidthExclusion::InvalidTextRange);
    EXPECT_TRUE(caret_open_result.targets.empty());

    // A zero-character caret at 0 preserves scope metadata but excludes EmptyText.
    auto *zero = item(*document, "zero");
    ASSERT_TRUE(zero);
    SW::StrokeWidthTextRange zero_caret;
    zero_caret.owner = zero;
    zero_caret.caret = true;
    zero_caret.first_char = 0;
    zero_caret.last_char = 0;
    auto const zero_result = SW::query_stroke_widths(*document, zero_caret, {zero});
    ASSERT_EQ(zero_result.excluded.size(), 1u);
    EXPECT_EQ(zero_result.excluded[0].exclusion, SW::StrokeWidthExclusion::EmptyText);
    EXPECT_TRUE(zero_result.excluded[0].whole_object);
    EXPECT_TRUE(zero_result.excluded[0].caret_scope);
    EXPECT_TRUE(zero_result.excluded[0].empty_text);
    EXPECT_EQ(zero_result.excluded[0].raw_first_char, 0u);
    EXPECT_EQ(zero_result.excluded[0].raw_last_char, 0u);

    // Invalid/dead/binding-mismatch/out-of-bounds reject atomically, no fallback.
    SW::StrokeWidthTextRange empty = range;
    empty.first_char = 3;
    empty.last_char = 3;
    auto const empty_result = SW::query_stroke_widths(*document, empty, {text});
    EXPECT_TRUE(empty_result.range_rejected);
    EXPECT_EQ(empty_result.range_exclusion, SW::StrokeWidthExclusion::InvalidTextRange);
    EXPECT_TRUE(empty_result.targets.empty());
    EXPECT_EQ(empty_result.eligible, 0u);

    SW::StrokeWidthTextRange dead = range;
    dead.owner.reset();
    auto const dead_result = SW::query_stroke_widths(*document, dead, {text});
    EXPECT_TRUE(dead_result.range_rejected);
    EXPECT_EQ(dead_result.range_exclusion, SW::StrokeWidthExclusion::InvalidTextRange);

    auto const mismatch = SW::query_stroke_widths(
        *document, range, {item(*document, "other")});
    EXPECT_TRUE(mismatch.range_rejected);
    EXPECT_EQ(mismatch.range_exclusion, SW::StrokeWidthExclusion::NotBoundToRoot);
    EXPECT_TRUE(mismatch.targets.empty());

    SW::StrokeWidthTextRange out_of_bounds = range;
    out_of_bounds.first_char = 0;
    out_of_bounds.last_char = 100;
    auto const bounds_result = SW::query_stroke_widths(*document, out_of_bounds, {text});
    EXPECT_TRUE(bounds_result.range_rejected);
    EXPECT_EQ(bounds_result.range_exclusion, SW::StrokeWidthExclusion::TextRangeOutOfBounds);
    EXPECT_TRUE(bounds_result.targets.empty());

    // The rejected queries never fell back to any root.
    EXPECT_EQ(serialize(*document), before);
    auto *locked_span = item(*document, "p");
    ASSERT_TRUE(locked_span);
    EXPECT_TRUE(cast<SPTSpan>(locked_span));
    EXPECT_EQ(cast<SPTSpan>(locked_span)->getRepr()->firstChild()->content(),
              std::string("bbb"));
}

// ---------------------------------------------------------------------------
// A10: ordinary/hairline/non-scaling aggregation.
// ---------------------------------------------------------------------------

TEST_F(StrokeWidthControllerTest, A10HairlineAndNonScaling)
{
    auto document = parse(std::string{svg_open} +
        R"(<rect id="hair" width="10" height="10" style="stroke-width:2;-inkscape-stroke:hairline"/>)"
        R"(<rect id="ord" width="10" height="10" style="stroke-width:2"/>)"
        R"(<rect id="ns" width="10" height="10" style="stroke-width:2;vector-effect:non-scaling-stroke"/>)"
        R"(<rect id="both" width="10" height="10" style="stroke-width:1;vector-effect:non-scaling-stroke;-inkscape-stroke:hairline"/>)"
        R"(</svg>)");
    ASSERT_TRUE(document);

    auto const before = serialize(*document);

    auto const only_hairline = SW::query_stroke_widths(*document, {item(*document, "hair")});
    EXPECT_EQ(only_hairline.state, SW::StrokeWidthQuery::Uniform);
    EXPECT_FALSE(only_hairline.uniform_px.has_value());
    EXPECT_EQ(only_hairline.hairline, 1u);
    EXPECT_EQ(only_hairline.eligible, 1u);

    // The hairline extension wins over the vector-effect fallback: the native
    // combined value stays a nonnumeric Uniform hairline, and the aggregate
    // ordinary non-scaling counter stays 0 even though the raw per-run
    // vector-effect flag remains true.
    auto const combined = SW::query_stroke_widths(*document, {item(*document, "both")});
    EXPECT_EQ(combined.state, SW::StrokeWidthQuery::Uniform);
    EXPECT_FALSE(combined.uniform_px.has_value());
    EXPECT_EQ(combined.hairline, 1u);
    EXPECT_EQ(combined.non_scaling, 0u);
    EXPECT_EQ(combined.eligible, 1u);
    ASSERT_EQ(combined.targets.size(), 1u);
    ASSERT_EQ(combined.targets[0].runs.size(), 1u);
    EXPECT_EQ(combined.targets[0].runs[0].style.convention, SW::StrokeWidthConvention::Hairline);
    EXPECT_TRUE(combined.targets[0].runs[0].style.non_scaling);

    auto const mixed = SW::query_stroke_widths(
        *document, {item(*document, "ord"), item(*document, "hair")});
    EXPECT_EQ(mixed.state, SW::StrokeWidthQuery::Mixed);
    EXPECT_EQ(mixed.hairline, 1u);

    // Shape non-scaling carries the measured local numeric value.
    auto const ordinary_plus_non_scaling = SW::query_stroke_widths(
        *document, {item(*document, "ord"), item(*document, "ns")});
    EXPECT_EQ(ordinary_plus_non_scaling.state, SW::StrokeWidthQuery::Uniform);
    EXPECT_EQ(ordinary_plus_non_scaling.eligible, 2u);
    EXPECT_EQ(ordinary_plus_non_scaling.unavailable, 0u);
    EXPECT_EQ(ordinary_plus_non_scaling.non_scaling, 1u);
    EXPECT_TRUE(ordinary_plus_non_scaling.excluded.empty());
    ASSERT_TRUE(ordinary_plus_non_scaling.uniform_px.has_value());
    EXPECT_NEAR(*ordinary_plus_non_scaling.uniform_px, 2.0, 1e-9);

    EXPECT_EQ(serialize(*document), before);
}

// ---------------------------------------------------------------------------
// A10f: a synthetic shape that paints nothing is still an eligible width
// target; the paint:none flag and the serialized paint are preserved.
// ---------------------------------------------------------------------------

TEST_F(StrokeWidthControllerTest, A10fPaintNoneShapeIsEligible)
{
    auto document = parse(std::string{svg_open} +
        R"(<rect id="pn" width="10" height="10" style="stroke:none;stroke-width:4"/>)"
        R"(</svg>)");
    ASSERT_TRUE(document);

    auto const before = serialize(*document);
    auto *rect = cast<SPRect>(item(*document, "pn"));
    ASSERT_TRUE(rect);

    auto const result = SW::query_stroke_widths(*document, {rect});
    EXPECT_EQ(result.state, SW::StrokeWidthQuery::Uniform);
    ASSERT_TRUE(result.uniform_px.has_value());
    EXPECT_NEAR(*result.uniform_px, 4.0, 1e-9);
    EXPECT_EQ(result.eligible, 1u);
    EXPECT_EQ(result.paint_none, 1u);
    ASSERT_EQ(result.targets.size(), 1u);
    ASSERT_EQ(result.targets[0].runs.size(), 1u);
    EXPECT_TRUE(result.targets[0].runs[0].style.paint_none);

    // The read-only query preserves the serialized `stroke:none` paint.
    auto const *style_attr = rect->getRepr()->attribute("style");
    ASSERT_TRUE(style_attr);
    EXPECT_NE(std::string(style_attr).find("stroke:none"), std::string::npos);
    EXPECT_EQ(serialize(*document), before);
}

// ---------------------------------------------------------------------------
// A10b: native shape non-scaling width is local_computed, independent of
// item/root scale and of a scaled viewport.
// ---------------------------------------------------------------------------

TEST_F(StrokeWidthControllerTest, A10NonScalingShapeIsScaleIndependent)
{
    auto document = parse(std::string{svg_open} +
        R"svg(<rect id="o2" width="10" height="10" style="stroke-width:2" transform="scale(2)"/>)svg"
        R"svg(<rect id="n2" width="10" height="10" style="stroke-width:2;vector-effect:non-scaling-stroke" transform="scale(2)"/>)svg"
        R"(</svg>)");
    ASSERT_TRUE(document);

    auto const ordinary = SW::query_stroke_widths(*document, {item(*document, "o2")});
    ASSERT_TRUE(ordinary.uniform_px.has_value());
    EXPECT_NEAR(*ordinary.uniform_px, 4.0, 1e-9);

    auto const non_scaling = SW::query_stroke_widths(*document, {item(*document, "n2")});
    EXPECT_EQ(non_scaling.state, SW::StrokeWidthQuery::Uniform);
    ASSERT_TRUE(non_scaling.uniform_px.has_value());
    EXPECT_NEAR(*non_scaling.uniform_px, 2.0, 1e-9);
    EXPECT_EQ(non_scaling.non_scaling, 1u);
    EXPECT_EQ(non_scaling.eligible, 1u);

    // Viewport scale factor 2: the native non-scaling width stays 2 while an
    // ordinary width doubles.
    auto viewport = parse(
        R"(<svg xmlns="http://www.w3.org/2000/svg" width="20" height="20" viewBox="0 0 10 10">)"
        R"(<rect id="no" width="10" height="10" style="stroke-width:2"/>)"
        R"(<rect id="nn" width="10" height="10" style="stroke-width:2;vector-effect:non-scaling-stroke"/>)"
        R"(</svg>)");
    ASSERT_TRUE(viewport);

    auto const viewport_ordinary = SW::query_stroke_widths(*viewport, {item(*viewport, "no")});
    ASSERT_TRUE(viewport_ordinary.uniform_px.has_value());
    EXPECT_NEAR(*viewport_ordinary.uniform_px, 4.0, 1e-9);

    auto const viewport_non_scaling = SW::query_stroke_widths(*viewport, {item(*viewport, "nn")});
    ASSERT_TRUE(viewport_non_scaling.uniform_px.has_value());
    EXPECT_NEAR(*viewport_non_scaling.uniform_px, 2.0, 1e-9);
    EXPECT_NEAR(viewport_non_scaling.targets[0].transform_scale, 2.0, 1e-9);
}

// ---------------------------------------------------------------------------
// A10c: text follows the same settled numerical convention as shapes: an
// ordinary SPText scales by its i2doc descrim, a non-scaling SPText keeps its
// local computed value. The supervisor's ordinary-text native oracle qualifies
// this convention; it does not qualify flow/textPath rendering.
// ---------------------------------------------------------------------------

TEST_F(StrokeWidthControllerTest, A10cTextNonScalingConvention)
{
    auto document = parse(std::string{svg_open} +
        R"svg(<text id="to" x="0" y="20" transform="scale(2)" style="font-family:sans-serif;font-size:20px;stroke-width:2">abc</text>)svg"
        R"svg(<text id="tns" x="0" y="40" transform="scale(2)" style="font-family:sans-serif;font-size:20px;stroke-width:2;vector-effect:non-scaling-stroke">abc</text>)svg"
        R"(</svg>)");
    ASSERT_TRUE(document);

    auto const before = serialize(*document);

    auto const ordinary = SW::query_stroke_widths(*document, {item(*document, "to")});
    EXPECT_EQ(ordinary.state, SW::StrokeWidthQuery::Uniform);
    ASSERT_TRUE(ordinary.uniform_px.has_value());
    EXPECT_NEAR(*ordinary.uniform_px, 4.0, 1e-9);
    ASSERT_EQ(ordinary.targets.size(), 1u);
    EXPECT_NEAR(ordinary.targets[0].transform_scale, 2.0, 1e-9);

    auto const non_scaling = SW::query_stroke_widths(*document, {item(*document, "tns")});
    EXPECT_EQ(non_scaling.state, SW::StrokeWidthQuery::Uniform);
    ASSERT_TRUE(non_scaling.uniform_px.has_value());
    EXPECT_NEAR(*non_scaling.uniform_px, 2.0, 1e-9);
    EXPECT_EQ(non_scaling.non_scaling, 1u);
    EXPECT_EQ(non_scaling.unavailable, 0u);
    ASSERT_EQ(non_scaling.targets.size(), 1u);
    ASSERT_EQ(non_scaling.targets[0].runs.size(), 1u);
    ASSERT_TRUE(non_scaling.targets[0].runs[0].style.effective_px.has_value());
    EXPECT_NEAR(*non_scaling.targets[0].runs[0].style.effective_px, 2.0, 1e-9);

    EXPECT_EQ(serialize(*document), before);
}

// ---------------------------------------------------------------------------
// A10d: a scaled viewport doubles an ordinary SPText width but leaves a
// non-scaling SPText width at its local computed value.
// ---------------------------------------------------------------------------

TEST_F(StrokeWidthControllerTest, A10dTextNonScalingViewport)
{
    auto viewport = parse(
        R"(<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" width="20" height="20" viewBox="0 0 10 10">)"
        R"(<text id="no" x="0" y="5" style="font-family:sans-serif;font-size:2px;stroke-width:2">abc</text>)"
        R"(<text id="nn" x="0" y="8" style="font-family:sans-serif;font-size:2px;stroke-width:2;vector-effect:non-scaling-stroke">abc</text>)"
        R"(</svg>)");
    ASSERT_TRUE(viewport);

    auto const ordinary = SW::query_stroke_widths(*viewport, {item(*viewport, "no")});
    ASSERT_TRUE(ordinary.uniform_px.has_value());
    EXPECT_NEAR(*ordinary.uniform_px, 4.0, 1e-9);
    ASSERT_EQ(ordinary.targets.size(), 1u);
    EXPECT_NEAR(ordinary.targets[0].transform_scale, 2.0, 1e-9);

    auto const non_scaling = SW::query_stroke_widths(*viewport, {item(*viewport, "nn")});
    ASSERT_TRUE(non_scaling.uniform_px.has_value());
    EXPECT_NEAR(*non_scaling.uniform_px, 2.0, 1e-9);
    ASSERT_EQ(non_scaling.targets.size(), 1u);
    EXPECT_NEAR(non_scaling.targets[0].transform_scale, 2.0, 1e-9);
    EXPECT_EQ(non_scaling.non_scaling, 1u);
}

// ---------------------------------------------------------------------------
// A10e: mixed styled text runs keep per-run source snapshots under the common
// convention: the non-scaling run reports local_computed and the ordinary run
// its scaled effective width.
// ---------------------------------------------------------------------------

TEST_F(StrokeWidthControllerTest, A10eTextMixedStyledRuns)
{
    auto document = parse(std::string{svg_open} +
        R"svg(<text id="tm" x="0" y="20" transform="scale(2)" style="font-family:sans-serif;font-size:20px;stroke-width:2">)svg"
        R"(<tspan id="mns" style="vector-effect:non-scaling-stroke">aa</tspan>)"
        R"(<tspan id="mord" style="stroke-width:4">bb</tspan>)"
        R"(</text>)"
        R"(</svg>)");
    ASSERT_TRUE(document);

    auto const before = serialize(*document);
    auto const result = SW::query_stroke_widths(*document, {item(*document, "tm")});

    EXPECT_EQ(result.state, SW::StrokeWidthQuery::Mixed);
    ASSERT_EQ(result.targets.size(), 1u);
    ASSERT_EQ(result.targets[0].runs.size(), 2u);
    auto const &ns_run = result.targets[0].runs[0];
    auto const &ord_run = result.targets[0].runs[1];

    EXPECT_EQ(ns_run.eligibility, SW::StrokeWidthEligibility::Eligible);
    EXPECT_EQ(ns_run.style.convention, SW::StrokeWidthConvention::NonScaling);
    EXPECT_EQ(ns_run.style_source.get(), static_cast<SPObject *>(item(*document, "mns")));
    EXPECT_NEAR(ns_run.style.local_computed, 2.0, 1e-9);
    ASSERT_TRUE(ns_run.style.effective_px.has_value());
    EXPECT_NEAR(*ns_run.style.effective_px, 2.0, 1e-9);

    EXPECT_EQ(ord_run.eligibility, SW::StrokeWidthEligibility::Eligible);
    EXPECT_EQ(ord_run.style.convention, SW::StrokeWidthConvention::Ordinary);
    EXPECT_EQ(ord_run.style_source.get(), static_cast<SPObject *>(item(*document, "mord")));
    EXPECT_NEAR(ord_run.style.local_computed, 4.0, 1e-9);
    ASSERT_TRUE(ord_run.style.effective_px.has_value());
    EXPECT_NEAR(*ord_run.style.effective_px, 8.0, 1e-9);

    EXPECT_EQ(serialize(*document), before);
}

// ---------------------------------------------------------------------------
// A11: ordinary transform scaling, reflection, nonuniform, singular, viewport.
// ---------------------------------------------------------------------------

TEST_F(StrokeWidthControllerTest, A11TransformConventions)
{
    auto document = parse(std::string{svg_open} +
        R"svg(<rect id="s2" width="10" height="10" style="stroke-width:2" transform="scale(2)"/>)svg"
        R"svg(<rect id="refl" width="10" height="10" style="stroke-width:2" transform="scale(-1,1)"/>)svg"
        R"svg(<rect id="nonuni" width="10" height="10" style="stroke-width:2" transform="scale(2,8)"/>)svg"
        R"svg(<rect id="sing" width="10" height="10" style="stroke-width:2" transform="scale(0)"/>)svg"
        R"svg(<g id="nest" transform="scale(2)"><rect id="nested" width="10" height="10" style="stroke-width:2" transform="scale(3)"/></g>)svg"
        R"(</svg>)");
    ASSERT_TRUE(document);

    auto const before = serialize(*document);

    auto const s2 = SW::query_stroke_widths(*document, {item(*document, "s2")});
    ASSERT_TRUE(s2.uniform_px.has_value());
    EXPECT_NEAR(*s2.uniform_px, 4.0, 1e-9);

    auto const refl = SW::query_stroke_widths(*document, {item(*document, "refl")});
    EXPECT_EQ(refl.state, SW::StrokeWidthQuery::Uniform);
    ASSERT_TRUE(refl.uniform_px.has_value());
    EXPECT_NEAR(*refl.uniform_px, 2.0, 1e-9);

    // Nonuniform nominal width uses the geometric mean of the determinant.
    auto const nonuni = SW::query_stroke_widths(*document, {item(*document, "nonuni")});
    ASSERT_TRUE(nonuni.uniform_px.has_value());
    EXPECT_NEAR(*nonuni.uniform_px, 8.0, 1e-9);

    auto const nested = SW::query_stroke_widths(*document, {item(*document, "nest")});
    ASSERT_TRUE(nested.uniform_px.has_value());
    EXPECT_NEAR(*nested.uniform_px, 12.0, 1e-9);

    auto const singular = SW::query_stroke_widths(*document, {item(*document, "sing")});
    EXPECT_EQ(singular.eligible, 0u);
    EXPECT_EQ(singular.unavailable, 1u);
    EXPECT_TRUE(find_excluded(singular, SW::StrokeWidthExclusion::SingularTransform));

    EXPECT_EQ(serialize(*document), before);
    EXPECT_NE(cast<SPRect>(item(*document, "s2"))->getRepr()->attribute("transform"), nullptr);
}

TEST_F(StrokeWidthControllerTest, A11ViewportConversion)
{
    auto document = parse(
        R"(<svg xmlns="http://www.w3.org/2000/svg" width="8.5in" height="11in" viewBox="0 0 612 792">)"
        R"(<rect id="v" width="10" height="10" style="stroke-width:0.75"/>)"
        R"(</svg>)");
    ASSERT_TRUE(document);

    auto const result = SW::query_stroke_widths(*document, {item(*document, "v")});
    ASSERT_TRUE(result.uniform_px.has_value());
    EXPECT_NEAR(*result.uniform_px, 1.0, 1e-9);
    EXPECT_NEAR(result.targets[0].transform_scale, 816.0 / 612.0, 1e-9);
}

// ---------------------------------------------------------------------------
// A11b: non-finite and overflowing transforms are rejected directly on the
// public SPItem::transform without any layout refresh.
// ---------------------------------------------------------------------------

TEST_F(StrokeWidthControllerTest, A11NonFiniteAndOverflowTransformsRejected)
{
    auto document = parse(std::string{svg_open} +
        R"(<rect id="r" width="10" height="10" style="stroke-width:2"/>)"
        R"(<rect id="big" width="10" height="10" style="stroke-width:1"/>)"
        R"(</svg>)");
    ASSERT_TRUE(document);

    auto *rect = item(*document, "r");
    auto *big = item(*document, "big");
    ASSERT_TRUE(rect);
    ASSERT_TRUE(big);
    ASSERT_TRUE(big->style);

    // Set the already-parsed computed width directly to 1e308 so the overflow
    // case does not depend on the CSS parser admitting that value. This isolates
    // the numerical validation from CSS parser admissibility.
    big->style->stroke_width.computed = 1e308;

    double const inf = std::numeric_limits<double>::infinity();
    rect->transform = Geom::Affine(inf, 0, 0, 1, 0, 0);
    auto const non_finite_entry = SW::query_stroke_widths(*document, {rect});
    EXPECT_EQ(non_finite_entry.eligible, 0u);
    EXPECT_EQ(non_finite_entry.unavailable, 1u);
    EXPECT_TRUE(find_excluded(non_finite_entry, SW::StrokeWidthExclusion::NonFiniteTransform));

    // Finite entries, but det() overflows so descrim() is not finite.
    rect->transform = Geom::Affine(1e200, 0, 0, 1e200, 0, 0);
    auto const overflowed_descrim = SW::query_stroke_widths(*document, {rect});
    EXPECT_EQ(overflowed_descrim.eligible, 0u);
    EXPECT_EQ(overflowed_descrim.unavailable, 1u);
    EXPECT_TRUE(find_excluded(overflowed_descrim, SW::StrokeWidthExclusion::NonFiniteTransform));

    // Finite descrim (1e100) but the effective width overflows to infinity. The
    // fixture is fully prepared before this snapshot; only the query follows.
    big->transform = Geom::Affine(1e100, 0, 0, 1e100, 0, 0);
    auto const before = serialize(*document);
    auto const overflowed_width = SW::query_stroke_widths(*document, {big});
    EXPECT_EQ(overflowed_width.eligible, 0u);
    EXPECT_EQ(overflowed_width.unavailable, 1u);
    EXPECT_TRUE(find_excluded(overflowed_width, SW::StrokeWidthExclusion::InvalidStyle));
    EXPECT_EQ(serialize(*document), before);
}

// ---------------------------------------------------------------------------
// A12: per-run dash snapshots without CSS mutation.
// ---------------------------------------------------------------------------

TEST_F(StrokeWidthControllerTest, A12RunDashSnapshots)
{
    auto document = parse(std::string{svg_open} +
        R"(<text id="t" x="0" y="20" style="font-family:sans-serif;font-size:20px;stroke-width:2;stroke-dasharray:4 2;stroke-dashoffset:1">)"
        R"(<tspan id="a">aa</tspan>)"
        R"(<tspan id="b" style="stroke-width:8;stroke-dasharray:1 3;stroke-dashoffset:0">bb</tspan>)"
        R"(</text>)"
        R"(</svg>)");
    ASSERT_TRUE(document);

    auto const before = serialize(*document);
    auto const result = SW::query_stroke_widths(*document, {item(*document, "t")});

    ASSERT_EQ(result.targets.size(), 1u);
    ASSERT_EQ(result.targets[0].runs.size(), 2u);
    auto const &first = result.targets[0].runs[0];
    auto const &second = result.targets[0].runs[1];

    EXPECT_NEAR(first.style.local_computed, 2.0, 1e-9);
    ASSERT_EQ(first.style.dash_computed.size(), 2u);
    EXPECT_NEAR(first.style.dash_computed[0], 4.0, 1e-9);
    EXPECT_NEAR(first.style.dash_computed[1], 2.0, 1e-9);
    EXPECT_NEAR(first.style.dash_offset_computed, 1.0, 1e-9);

    EXPECT_NEAR(second.style.local_computed, 8.0, 1e-9);
    ASSERT_EQ(second.style.dash_computed.size(), 2u);
    EXPECT_NEAR(second.style.dash_computed[0], 1.0, 1e-9);
    EXPECT_NEAR(second.style.dash_computed[1], 3.0, 1e-9);
    EXPECT_NEAR(second.style.dash_offset_computed, 0.0, 1e-9);

    EXPECT_EQ(serialize(*document), before);
}

// ---------------------------------------------------------------------------
// A13: narrow clone boundary, source isolation, important metadata.
// ---------------------------------------------------------------------------

TEST_F(StrokeWidthControllerTest, A13CloneBoundaries)
{
    auto document = parse(std::string{svg_open} +
        R"(<rect id="src" width="10" height="10"/>)"
        R"(<use id="u" xlink:href="#src" x="20"/>)"
        R"(<rect id="src2" width="10" height="10" style="stroke-width:5"/>)"
        R"(<use id="u2" xlink:href="#src2" x="40"/>)"
        R"(<use id="u3" xlink:href="#missing" x="60"/>)"
        R"(<rect id="src4" width="10" height="10" style="stroke-width:5 !important"/>)"
        R"(<use id="u4" xlink:href="#src4" x="80"/>)"
        R"(<rect id="imp" width="10" height="10" style="stroke-width:7 !important"/>)"
        R"(</svg>)");
    ASSERT_TRUE(document);

    auto const before = serialize(*document);

    // Narrow clone: source does not own width, so the instance reads child style.
    auto const narrow = SW::query_stroke_widths(*document, {item(*document, "u")});
    ASSERT_EQ(narrow.eligible, 1u);
    ASSERT_EQ(narrow.targets.size(), 1u);
    EXPECT_EQ(narrow.targets[0].kind, SW::StrokeWidthTargetKind::CloneInstance);
    EXPECT_EQ(narrow.targets[0].clone_source.get(), item(*document, "src"));
    ASSERT_EQ(narrow.targets[0].runs.size(), 1u);
    auto *use = cast<SPUse>(item(*document, "u"));
    ASSERT_TRUE(use);
    EXPECT_EQ(narrow.targets[0].runs[0].style_source.get(), static_cast<SPObject *>(use->child));

    // Explicit source width blocks the instance override.
    auto const explicit_source = SW::query_stroke_widths(*document, {item(*document, "u2")});
    EXPECT_EQ(explicit_source.eligible, 0u);
    EXPECT_EQ(explicit_source.unavailable, 1u);
    EXPECT_TRUE(find_excluded(explicit_source, SW::StrokeWidthExclusion::CloneSourceOverrides));

    // Important source width blocks the instance override.
    auto const important_source = SW::query_stroke_widths(*document, {item(*document, "u4")});
    EXPECT_TRUE(find_excluded(important_source, SW::StrokeWidthExclusion::CloneSourceOverrides));

    // Missing source is a distinct reason.
    auto const missing = SW::query_stroke_widths(*document, {item(*document, "u3")});
    EXPECT_EQ(missing.missing_sources, 1u);
    EXPECT_TRUE(find_excluded(missing, SW::StrokeWidthExclusion::MissingSource));

    // Source plus clone both resolve; neither is covered.
    auto const both = SW::query_stroke_widths(
        *document, {item(*document, "src"), item(*document, "u")});
    EXPECT_EQ(both.eligible, 2u);
    EXPECT_EQ(both.covered, 0u);

    // Width importance metadata is preserved on an ordinary shape.
    auto const important = SW::query_stroke_widths(*document, {item(*document, "imp")});
    ASSERT_EQ(important.targets.size(), 1u);
    ASSERT_EQ(important.targets[0].runs.size(), 1u);
    EXPECT_TRUE(important.targets[0].runs[0].style.width_important);

    EXPECT_EQ(serialize(*document), before);
    // Source geometry is not mutated by the clone query.
    EXPECT_EQ(cast<SPRect>(item(*document, "src"))->getRepr()->attribute("stroke-width"),
              nullptr);
}

// ---------------------------------------------------------------------------
// A13b: the clone instance child's own hidden/locked state is checked, stopping
// before the use (checked separately). No referenced-source defs/ancestry walk.
// ---------------------------------------------------------------------------

TEST_F(StrokeWidthControllerTest, A13bCloneInstanceChildProtection)
{
    auto document = parse(std::string{svg_open} +
        R"(<defs>)"
        R"(<rect id="hsrc" width="10" height="10" style="display:none"/>)"
        R"(<rect id="lsrc" width="10" height="10" sodipodi:insensitive="true"/>)"
        R"(</defs>)"
        R"(<use id="uh" xlink:href="#hsrc" x="20"/>)"
        R"(<use id="ul" xlink:href="#lsrc" x="40"/>)"
        R"(</svg>)");
    ASSERT_TRUE(document);

    auto const before = serialize(*document);

    auto const hidden = SW::query_stroke_widths(*document, {item(*document, "uh")});
    EXPECT_EQ(hidden.eligible, 0u);
    EXPECT_EQ(hidden.unavailable, 1u);
    EXPECT_TRUE(find_excluded(hidden, SW::StrokeWidthExclusion::HiddenAncestor));

    auto const locked = SW::query_stroke_widths(*document, {item(*document, "ul")});
    EXPECT_EQ(locked.eligible, 0u);
    EXPECT_EQ(locked.unavailable, 1u);
    EXPECT_TRUE(find_excluded(locked, SW::StrokeWidthExclusion::LockedAncestor));

    EXPECT_EQ(serialize(*document), before);
}

// ---------------------------------------------------------------------------
// A13c: the use width is usable when the source inherits or does not set it;
// the instance child's ordinary effective width is scaled by its own transform.
// Source and clone stay distinct and the clone source XML is never written.
// ---------------------------------------------------------------------------

TEST_F(StrokeWidthControllerTest, A13cCloneInstanceUseWidthConventions)
{
    auto document = parse(std::string{svg_open} +
        R"(<rect id="inh" width="10" height="10" style="stroke-width:inherit"/>)"
        R"(<rect id="inhi" width="10" height="10" style="stroke-width:inherit !important"/>)"
        R"(<rect id="uns" width="10" height="10"/>)"
        R"(<use id="ui" xlink:href="#inh" x="20" style="stroke-width:4"/>)"
        R"(<use id="uii" xlink:href="#inhi" x="40" style="stroke-width:4"/>)"
        R"svg(<use id="uu" xlink:href="#uns" x="60" transform="scale(2)" style="stroke-width:4"/>)svg"
        R"(</svg>)");
    ASSERT_TRUE(document);

    auto const before = serialize(*document);

    // Explicit inherit on the source is not an override: the use width 4 wins.
    auto const inherit_source = SW::query_stroke_widths(*document, {item(*document, "ui")});
    EXPECT_EQ(inherit_source.eligible, 1u);
    ASSERT_EQ(inherit_source.targets.size(), 1u);
    ASSERT_EQ(inherit_source.targets[0].runs.size(), 1u);
    EXPECT_NEAR(inherit_source.targets[0].runs[0].style.local_computed, 4.0, 1e-9);
    ASSERT_TRUE(inherit_source.targets[0].runs[0].style.effective_px.has_value());
    EXPECT_NEAR(*inherit_source.targets[0].runs[0].style.effective_px, 4.0, 1e-9);

    // `!important` alone must not exclude an inherited source width: explicit
    // `stroke-width:inherit !important` stays compatible with the instance 4.
    auto const inherit_important = SW::query_stroke_widths(*document, {item(*document, "uii")});
    EXPECT_EQ(inherit_important.eligible, 1u);
    EXPECT_EQ(inherit_important.unavailable, 0u);
    ASSERT_EQ(inherit_important.targets.size(), 1u);
    ASSERT_EQ(inherit_important.targets[0].runs.size(), 1u);
    EXPECT_NEAR(inherit_important.targets[0].runs[0].style.local_computed, 4.0, 1e-9);
    ASSERT_TRUE(inherit_important.targets[0].runs[0].style.effective_px.has_value());
    EXPECT_NEAR(*inherit_important.targets[0].runs[0].style.effective_px, 4.0, 1e-9);

    // Unset source, use width 4 on a scale(2) instance: ordinary 4 * 2 = 8.
    auto const unset_source = SW::query_stroke_widths(*document, {item(*document, "uu")});
    EXPECT_EQ(unset_source.eligible, 1u);
    ASSERT_EQ(unset_source.targets.size(), 1u);
    EXPECT_NEAR(unset_source.targets[0].transform_scale, 2.0, 1e-9);
    ASSERT_EQ(unset_source.targets[0].runs.size(), 1u);
    EXPECT_NEAR(unset_source.targets[0].runs[0].style.local_computed, 4.0, 1e-9);
    ASSERT_TRUE(unset_source.targets[0].runs[0].style.effective_px.has_value());
    EXPECT_NEAR(*unset_source.targets[0].runs[0].style.effective_px, 8.0, 1e-9);

    // Source and clone remain distinct eligible targets.
    auto const both = SW::query_stroke_widths(
        *document, {item(*document, "inh"), item(*document, "ui")});
    EXPECT_EQ(both.eligible, 2u);
    EXPECT_EQ(both.covered, 0u);

    EXPECT_EQ(serialize(*document), before);
    // Query never writes either clone source.
    EXPECT_EQ(cast<SPRect>(item(*document, "inh"))->getRepr()->attribute("stroke-width"),
              nullptr);
    EXPECT_EQ(cast<SPRect>(item(*document, "inhi"))->getRepr()->attribute("stroke-width"),
              nullptr);
}

// ---------------------------------------------------------------------------
// A14: a root from another document is excluded before traversal; range
// binding only accepts roots belonging to the query document.
// ---------------------------------------------------------------------------

TEST_F(StrokeWidthControllerTest, A14WrongDocumentRootIsExcluded)
{
    auto document = parse(std::string{svg_open} +
        R"(<rect id="a" width="10" height="10" style="stroke-width:2"/>)"
        R"(<text id="t" x="0" y="20" style="font-family:sans-serif;font-size:20px">x</text>)"
        R"(</svg>)");
    auto foreign = parse(std::string{svg_open} +
        R"(<rect id="f" width="10" height="10" style="stroke-width:9"/>)"
        R"(</svg>)");
    ASSERT_TRUE(document);
    ASSERT_TRUE(foreign);

    auto *a = item(*document, "a");
    auto *f = item(*foreign, "f");
    ASSERT_TRUE(a);
    ASSERT_TRUE(f);

    auto const mixed = SW::query_stroke_widths(*document, {a, f});
    EXPECT_EQ(mixed.eligible, 1u);
    ASSERT_EQ(mixed.targets.size(), 1u);
    EXPECT_EQ(mixed.targets[0].owner.get(), a);
    ASSERT_EQ(mixed.targets[0].runs.size(), 1u);
    EXPECT_NEAR(mixed.targets[0].runs[0].style.local_computed, 2.0, 1e-9);
    EXPECT_EQ(mixed.unavailable, 1u);
    EXPECT_TRUE(find_excluded(mixed, SW::StrokeWidthExclusion::WrongDocument));
    for (auto const &target : mixed.excluded) {
        if (target.exclusion == SW::StrokeWidthExclusion::WrongDocument) {
            EXPECT_EQ(target.owner.get(), f);
        }
    }

    // A range can only be bound by a root in the query document.
    auto *text = item(*document, "t");
    ASSERT_TRUE(text);
    SW::StrokeWidthTextRange range;
    range.owner = text;
    range.first_char = 0;
    range.last_char = 1;
    auto const foreign_only = SW::query_stroke_widths(*document, range, {f});
    EXPECT_TRUE(foreign_only.range_rejected);
    EXPECT_EQ(foreign_only.range_exclusion, SW::StrokeWidthExclusion::NotBoundToRoot);
    EXPECT_TRUE(foreign_only.targets.empty());
}

// ---------------------------------------------------------------------------
// A14b: a range owner that is real text in another document rejects as
// InvalidTextRange even when a current-document root is supplied; the query
// never falls back to that root and both documents stay unchanged.
// ---------------------------------------------------------------------------

TEST_F(StrokeWidthControllerTest, A14bForeignTextOwnerRangeRejected)
{
    auto document = parse(std::string{svg_open} +
        R"(<text id="t" x="0" y="20" style="font-family:sans-serif;font-size:20px;stroke-width:2">abc</text>)"
        R"(</svg>)");
    auto foreign = parse(std::string{svg_open} +
        R"(<text id="ft" x="0" y="20" style="font-family:sans-serif;font-size:20px;stroke-width:9">xyz</text>)"
        R"(</svg>)");
    ASSERT_TRUE(document);
    ASSERT_TRUE(foreign);

    auto const before = serialize(*document);
    auto const foreign_before = serialize(*foreign);

    auto *text = item(*document, "t");
    auto *foreign_text = item(*foreign, "ft");
    ASSERT_TRUE(text);
    ASSERT_TRUE(foreign_text);

    SW::StrokeWidthTextRange range;
    range.owner = foreign_text;
    range.first_char = 0;
    range.last_char = 1;
    auto const result = SW::query_stroke_widths(*document, range, {text});
    EXPECT_TRUE(result.range_rejected);
    EXPECT_EQ(result.range_exclusion, SW::StrokeWidthExclusion::InvalidTextRange);
    EXPECT_TRUE(result.targets.empty());
    EXPECT_EQ(result.eligible, 0u);

    // No fallback to the bound current-document root, and neither document moved.
    EXPECT_EQ(serialize(*document), before);
    EXPECT_EQ(serialize(*foreign), foreign_before);
}

// ---------------------------------------------------------------------------
// A15: an SPTextPath is a legitimate interior style scope while resolving an
// explicit text owner's run, but is still excluded when directly selected and
// its referenced path geometry is never targeted or followed.
// ---------------------------------------------------------------------------

TEST_F(StrokeWidthControllerTest, A15TextPathOwnerRunScope)
{
    auto document = parse(std::string{svg_open} +
        R"(<defs><path id="path-in-defs" d="M0,0 L100,0"/></defs>)"
        R"(<text id="tp" x="0" y="20" style="font-family:sans-serif;font-size:20px;stroke-width:2">)"
        R"(<textPath id="pathrun" xlink:href="#path-in-defs">abc</textPath>)"
        R"(</text>)"
        R"(</svg>)");
    ASSERT_TRUE(document);

    auto const before = serialize(*document);
    auto *textpath = item(*document, "pathrun");
    auto *path = item(*document, "path-in-defs");
    ASSERT_TRUE(textpath);
    ASSERT_TRUE(path);

    auto const owner_result = SW::query_stroke_widths(*document, {item(*document, "tp")});
    EXPECT_EQ(owner_result.state, SW::StrokeWidthQuery::Uniform);
    ASSERT_EQ(owner_result.targets.size(), 1u);
    auto const &target = owner_result.targets[0];
    EXPECT_EQ(target.kind, SW::StrokeWidthTargetKind::TextOwner);
    EXPECT_EQ(target.owner.get(), item(*document, "tp"));
    ASSERT_EQ(target.runs.size(), 1u);
    EXPECT_EQ(target.runs[0].first_char, 0u);
    EXPECT_EQ(target.runs[0].last_char, 3u);
    EXPECT_EQ(target.runs[0].eligibility, SW::StrokeWidthEligibility::Eligible);
    EXPECT_EQ(target.runs[0].exclusion, SW::StrokeWidthExclusion::None);
    EXPECT_NEAR(target.runs[0].style.local_computed, 2.0, 1e-9);
    ASSERT_TRUE(owner_result.uniform_px.has_value());
    EXPECT_NEAR(*owner_result.uniform_px, 2.0, 1e-9);

    // The referenced path is never a target and its geometry is untouched.
    for (auto const &t : owner_result.targets) {
        EXPECT_NE(t.owner.get(), path);
    }
    EXPECT_EQ(path->getRepr()->attribute("d"), std::string("M0,0 L100,0"));

    // Directly selected textPath stays an unavailable text descendant.
    auto const direct = SW::query_stroke_widths(*document, {textpath});
    EXPECT_EQ(direct.eligible, 0u);
    EXPECT_EQ(direct.unavailable, 1u);
    EXPECT_TRUE(find_excluded(direct, SW::StrokeWidthExclusion::TextDescendant));
    EXPECT_TRUE(direct.targets.empty());

    EXPECT_EQ(serialize(*document), before);
}

// ---------------------------------------------------------------------------
// A17: only an ordinary `svg:g` GROUP container is traversed. An Inkscape layer
// (`inkscape:groupmode="layer"`) and a non-`svg:g` `SPGroup` (SVG `<a>`) are
// unsupported containers: excluded without descending, XML unchanged.
// ---------------------------------------------------------------------------

TEST_F(StrokeWidthControllerTest, A17UnsupportedContainerExclusion)
{
    auto document = parse(std::string{
        R"(<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" )"
        R"(xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape">)"
        R"(<g id="layerroot" inkscape:groupmode="layer">)"
        R"(<rect id="lrect" width="10" height="10" style="stroke-width:5"/>)"
        R"(</g>)"
        R"(<a id="anchor"><rect id="arect" width="10" height="10" style="stroke-width:5"/></a>)"
        R"(</svg>)"});
    ASSERT_TRUE(document);

    auto const before = serialize(*document);
    auto *layer = item(*document, "layerroot");
    auto *anchor = item(*document, "anchor");
    ASSERT_TRUE(layer);
    ASSERT_TRUE(anchor);
    ASSERT_TRUE(cast<SPGroup>(anchor));

    // A layer group is not an ordinary svg:g and must not be descended into.
    auto const layer_result = SW::query_stroke_widths(*document, {layer});
    EXPECT_EQ(layer_result.eligible, 0u);
    EXPECT_EQ(layer_result.incompatible, 1u);
    EXPECT_TRUE(find_excluded(layer_result, SW::StrokeWidthExclusion::UnsupportedContainer));
    EXPECT_TRUE(layer_result.targets.empty());
    for (auto const &target : layer_result.targets) {
        EXPECT_NE(target.owner.get(), item(*document, "lrect"));
    }

    // A real SPGroup that is not `svg:g` (SVG anchor) is also not traversed.
    auto const anchor_result = SW::query_stroke_widths(*document, {anchor});
    EXPECT_EQ(anchor_result.eligible, 0u);
    EXPECT_EQ(anchor_result.incompatible, 1u);
    EXPECT_TRUE(find_excluded(anchor_result, SW::StrokeWidthExclusion::UnsupportedContainer));
    EXPECT_TRUE(anchor_result.targets.empty());
    for (auto const &target : anchor_result.targets) {
        EXPECT_NE(target.owner.get(), item(*document, "arect"));
    }

    EXPECT_EQ(serialize(*document), before);
}

// ---------------------------------------------------------------------------
// A16: a whole-owner tref run is unsupported (no editable width through the
// tref) while the owner's own text stays eligible; an explicit non-empty range
// touching the tref rejects atomically.
// ---------------------------------------------------------------------------

TEST_F(StrokeWidthControllerTest, A16TRefRunExclusion)
{
    auto document = parse(std::string{svg_open} +
        R"(<defs><text id="ref" x="0" y="20" style="font-family:sans-serif;font-size:20px">xy</text></defs>)"
        R"(<text id="owner" x="0" y="20" style="font-family:sans-serif;font-size:20px;stroke-width:2">a<tref id="tr" xlink:href="#ref"/>b</text>)"
        R"(</svg>)");
    ASSERT_TRUE(document);

    auto const before = serialize(*document);
    auto *owner = item(*document, "owner");
    ASSERT_TRUE(owner);

    auto const whole = SW::query_stroke_widths(*document, {owner});
    ASSERT_EQ(whole.eligible, 1u);
    ASSERT_EQ(whole.targets.size(), 1u);
    auto const &target = whole.targets[0];
    EXPECT_EQ(target.owner.get(), owner);
    EXPECT_TRUE(target.whole_object);
    // Native layout expands the tref reference: 'a' + referenced "xy" + 'b' = 4.
    EXPECT_EQ(target.first_char, 0u);
    EXPECT_EQ(target.last_char, 4u);
    ASSERT_EQ(target.runs.size(), 3u);

    // The owner's own characters stay eligible at the owner width.
    EXPECT_EQ(target.runs[0].first_char, 0u);
    EXPECT_EQ(target.runs[0].last_char, 1u);
    EXPECT_EQ(target.runs[0].eligibility, SW::StrokeWidthEligibility::Eligible);
    EXPECT_EQ(target.runs[0].exclusion, SW::StrokeWidthExclusion::None);
    EXPECT_NEAR(target.runs[0].style.local_computed, 2.0, 1e-9);

    // The [1,3) tref run is unavailable through the tref itself.
    EXPECT_EQ(target.runs[1].first_char, 1u);
    EXPECT_EQ(target.runs[1].last_char, 3u);
    EXPECT_EQ(target.runs[1].eligibility, SW::StrokeWidthEligibility::Unavailable);
    EXPECT_EQ(target.runs[1].exclusion, SW::StrokeWidthExclusion::UnsupportedTref);

    EXPECT_EQ(target.runs[2].first_char, 3u);
    EXPECT_EQ(target.runs[2].last_char, 4u);
    EXPECT_EQ(target.runs[2].eligibility, SW::StrokeWidthEligibility::Eligible);
    EXPECT_EQ(target.runs[2].exclusion, SW::StrokeWidthExclusion::None);
    EXPECT_NEAR(target.runs[2].style.local_computed, 2.0, 1e-9);

    EXPECT_EQ(whole.state, SW::StrokeWidthQuery::Uniform);
    ASSERT_TRUE(whole.uniform_px.has_value());
    EXPECT_NEAR(*whole.uniform_px, 2.0, 1e-9);

    // A settled caret inside the [1,3) tref run keeps the whole-owner scope:
    // the unavailable tref run is retained while the owner's own characters
    // still contribute uniform 2.0. This is observational, not a fallback.
    SW::StrokeWidthTextRange caret;
    caret.owner = owner;
    caret.caret = true;
    caret.first_char = 2;
    caret.last_char = 2;
    auto const caret_query = SW::query_stroke_widths(*document, caret, {owner});
    EXPECT_FALSE(caret_query.range_rejected);
    EXPECT_EQ(caret_query.eligible, 1u);
    ASSERT_EQ(caret_query.targets.size(), 1u);
    auto const &caret_target = caret_query.targets[0];
    EXPECT_EQ(caret_target.owner.get(), owner);
    EXPECT_TRUE(caret_target.whole_object);
    EXPECT_TRUE(caret_target.caret_scope);
    EXPECT_EQ(caret_target.first_char, 0u);
    EXPECT_EQ(caret_target.last_char, 4u);
    EXPECT_EQ(caret_target.raw_first_char, 2u);
    EXPECT_EQ(caret_target.raw_last_char, 2u);
    ASSERT_EQ(caret_target.runs.size(), 3u);
    EXPECT_EQ(caret_target.runs[0].eligibility, SW::StrokeWidthEligibility::Eligible);
    EXPECT_EQ(caret_target.runs[1].first_char, 1u);
    EXPECT_EQ(caret_target.runs[1].last_char, 3u);
    EXPECT_EQ(caret_target.runs[1].eligibility, SW::StrokeWidthEligibility::Unavailable);
    EXPECT_EQ(caret_target.runs[1].exclusion, SW::StrokeWidthExclusion::UnsupportedTref);
    EXPECT_EQ(caret_target.runs[2].eligibility, SW::StrokeWidthEligibility::Eligible);
    EXPECT_EQ(caret_query.state, SW::StrokeWidthQuery::Uniform);
    ASSERT_TRUE(caret_query.uniform_px.has_value());
    EXPECT_NEAR(*caret_query.uniform_px, 2.0, 1e-9);

    // An explicit [1,3) range touching the tref rejects as UnsupportedTref and
    // is never accepted as TextRangeOutOfBounds.
    SW::StrokeWidthTextRange range;
    range.owner = owner;
    range.first_char = 1;
    range.last_char = 3;
    auto const rejected = SW::query_stroke_widths(*document, range, {owner});
    EXPECT_TRUE(rejected.range_rejected);
    EXPECT_EQ(rejected.range_exclusion, SW::StrokeWidthExclusion::UnsupportedTref);
    EXPECT_TRUE(rejected.targets.empty());
    EXPECT_EQ(rejected.eligible, 0u);

    EXPECT_EQ(serialize(*document), before);
}

// ===========================================================================
// SW2A read-only preparation tests. These never apply a patch; they assert the
// prepared numeric intent only, and that XML is untouched.
// ===========================================================================

// ---------------------------------------------------------------------------
// SW2A-1: 2/2/8 plus a bitmap. Absolute 10 plans three local changes and counts
// the bitmap as excluded; a fresh relative 200 plans 4/4/16. XML unchanged.
// ---------------------------------------------------------------------------

TEST_F(StrokeWidthControllerTest, SW2APrepareAbsoluteRelativeMixedWithBitmap)
{
    auto document = parse(std::string{svg_open} +
        R"(<rect id="a" width="10" height="10" style="stroke-width:2"/>)"
        R"(<rect id="b" width="10" height="10" style="stroke-width:2"/>)"
        R"(<rect id="c" width="10" height="10" style="stroke-width:8"/>)"
        R"(<image id="img" width="10" height="10"/>)"
        R"(</svg>)");
    ASSERT_TRUE(document);

    auto const before = serialize(*document);
    std::vector<SPItem *> roots = {item(*document, "a"), item(*document, "b"),
                                   item(*document, "c"), item(*document, "img")};

    auto const absolute_plan = SW::prepare_stroke_widths(*document, roots, absolute(10.0), 7);
    EXPECT_EQ(absolute_plan.state, SW::StrokeWidthPlanState::Prepared);
    EXPECT_EQ(absolute_plan.planned_changes, 3u);
    EXPECT_EQ(absolute_plan.unchanged, 0u);
    EXPECT_EQ(absolute_plan.excluded, 1u); // the bitmap
    EXPECT_EQ(absolute_plan.query.eligible, 3u);
    EXPECT_EQ(absolute_plan.query.incompatible, 1u);
    EXPECT_EQ(absolute_plan.scope_generation, 7u);
    for (char const *id : {"a", "b", "c"}) {
        auto const *member = find_member(absolute_plan, item(*document, id));
        ASSERT_TRUE(member);
        EXPECT_EQ(member->outcome, SW::StrokeWidthMemberOutcome::Change);
        ASSERT_TRUE(member->local_width.has_value());
        EXPECT_NEAR(*member->local_width, 10.0, 1e-9);
    }
    EXPECT_EQ(find_member(absolute_plan, item(*document, "img")), nullptr);

    auto const relative_plan = SW::prepare_stroke_widths(*document, roots, relative(200.0), 8);
    EXPECT_EQ(relative_plan.planned_changes, 3u);
    EXPECT_NEAR(*find_member(relative_plan, item(*document, "a"))->local_width, 4.0, 1e-9);
    EXPECT_NEAR(*find_member(relative_plan, item(*document, "b"))->local_width, 4.0, 1e-9);
    EXPECT_NEAR(*find_member(relative_plan, item(*document, "c"))->local_width, 16.0, 1e-9);

    EXPECT_EQ(serialize(*document), before);
}

// ---------------------------------------------------------------------------
// SW2A-2: zero and positive widths, relative no-op, percent 100, equal
// absolute, and a paint:none member that stays eligible.
// ---------------------------------------------------------------------------

TEST_F(StrokeWidthControllerTest, SW2APrepareZeroAndPositiveWidths)
{
    auto document = parse(std::string{svg_open} +
        R"(<rect id="z" width="10" height="10" style="stroke-width:0"/>)"
        R"(<rect id="p" width="10" height="10" style="stroke-width:2"/>)"
        R"(<rect id="pn" width="10" height="10" style="stroke:none;stroke-width:4"/>)"
        R"(</svg>)");
    ASSERT_TRUE(document);

    auto const before = serialize(*document);

    auto const abs_plan = SW::prepare_stroke_widths(
        *document, {item(*document, "z"), item(*document, "p")}, absolute(10.0), 1);
    EXPECT_EQ(abs_plan.planned_changes, 2u);
    EXPECT_NEAR(*find_member(abs_plan, item(*document, "z"))->local_width, 10.0, 1e-9);
    EXPECT_NEAR(*find_member(abs_plan, item(*document, "p"))->local_width, 10.0, 1e-9);

    auto const rel_plan = SW::prepare_stroke_widths(
        *document, {item(*document, "z"), item(*document, "p")}, relative(200.0), 1);
    EXPECT_EQ(rel_plan.planned_changes, 1u);
    EXPECT_EQ(rel_plan.unchanged, 1u);
    auto const *z_member = find_member(rel_plan, item(*document, "z"));
    ASSERT_TRUE(z_member);
    EXPECT_EQ(z_member->outcome, SW::StrokeWidthMemberOutcome::Unchanged);
    EXPECT_FALSE(z_member->local_width.has_value());
    EXPECT_FALSE(z_member->local_dasharray.has_value());
    EXPECT_FALSE(z_member->local_dashoffset.has_value());
    EXPECT_NEAR(*find_member(rel_plan, item(*document, "p"))->local_width, 4.0, 1e-9);

    auto const percent_plan = SW::prepare_stroke_widths(
        *document, {item(*document, "p")}, relative(100.0), 1);
    EXPECT_EQ(percent_plan.planned_changes, 0u);
    EXPECT_EQ(percent_plan.unchanged, 1u);
    EXPECT_FALSE(find_member(percent_plan, item(*document, "p"))->local_width.has_value());

    auto const equal_plan = SW::prepare_stroke_widths(
        *document, {item(*document, "p")}, absolute(2.0), 1);
    EXPECT_EQ(equal_plan.planned_changes, 0u);
    EXPECT_FALSE(find_member(equal_plan, item(*document, "p"))->local_width.has_value());

    auto const paint_plan = SW::prepare_stroke_widths(
        *document, {item(*document, "pn")}, absolute(3.0), 1);
    EXPECT_EQ(paint_plan.query.paint_none, 1u);
    auto const *pn_member = find_member(paint_plan, item(*document, "pn"));
    ASSERT_TRUE(pn_member);
    EXPECT_EQ(pn_member->outcome, SW::StrokeWidthMemberOutcome::Change);
    EXPECT_NEAR(*pn_member->local_width, 3.0, 1e-9);
    EXPECT_TRUE(pn_member->target.runs[0].style.paint_none);

    EXPECT_EQ(serialize(*document), before);
}

// ---------------------------------------------------------------------------
// SW2A-3: ordinary scale 2 -> 0.5 local, viewport 4/3 -> 0.75, non-scaling
// stays local, the frozen 6-entry affine matches the known matrix, and the
// transform/id are preserved.
// ---------------------------------------------------------------------------

TEST_F(StrokeWidthControllerTest, SW2APrepareScaleAndViewportAffine)
{
    auto document = parse(std::string{svg_open} +
        R"svg(<rect id="s2" width="10" height="10" style="stroke-width:2" transform="scale(2)"/>)svg"
        R"svg(<rect id="ns" width="10" height="10" style="stroke-width:2;vector-effect:non-scaling-stroke" transform="scale(2)"/>)svg"
        R"(</svg>)");
    ASSERT_TRUE(document);

    auto const before = serialize(*document);

    auto const scaled = SW::prepare_stroke_widths(*document, {item(*document, "s2")}, absolute(1.0), 1);
    auto const *s2 = find_member(scaled, item(*document, "s2"));
    ASSERT_TRUE(s2);
    EXPECT_EQ(s2->outcome, SW::StrokeWidthMemberOutcome::Change);
    ASSERT_TRUE(s2->local_width.has_value());
    EXPECT_NEAR(*s2->local_width, 0.5, 1e-9);
    EXPECT_NEAR(s2->i2doc_affine[0], 2.0, 1e-9);
    EXPECT_NEAR(s2->i2doc_affine[1], 0.0, 1e-9);
    EXPECT_NEAR(s2->i2doc_affine[2], 0.0, 1e-9);
    EXPECT_NEAR(s2->i2doc_affine[3], 2.0, 1e-9);
    EXPECT_NEAR(s2->i2doc_affine[4], 0.0, 1e-9);
    EXPECT_NEAR(s2->i2doc_affine[5], 0.0, 1e-9);

    auto const non_scaling = SW::prepare_stroke_widths(*document, {item(*document, "ns")}, absolute(1.0), 1);
    auto const *ns = find_member(non_scaling, item(*document, "ns"));
    ASSERT_TRUE(ns);
    EXPECT_NEAR(*ns->local_width, 1.0, 1e-9);
    EXPECT_EQ(ns->target.runs[0].style.convention, SW::StrokeWidthConvention::NonScaling);

    // 4:3 viewport: requested 1 document px stores 0.75 local.
    auto viewport = parse(
        R"(<svg xmlns="http://www.w3.org/2000/svg" width="4" height="4" viewBox="0 0 3 3">)"
        R"(<rect id="v" width="10" height="10" style="stroke-width:2"/>)"
        R"(</svg>)");
    ASSERT_TRUE(viewport);
    auto const view_plan = SW::prepare_stroke_widths(*viewport, {item(*viewport, "v")}, absolute(1.0), 1);
    auto const *v = find_member(view_plan, item(*viewport, "v"));
    ASSERT_TRUE(v);
    ASSERT_TRUE(v->local_width.has_value());
    EXPECT_NEAR(*v->local_width, 0.75, 1e-9);
    EXPECT_NEAR(v->i2doc_affine[0], 4.0 / 3.0, 1e-9);
    EXPECT_NEAR(v->i2doc_affine[3], 4.0 / 3.0, 1e-9);

    EXPECT_EQ(serialize(*document), before);
    EXPECT_NE(cast<SPRect>(item(*document, "s2"))->getRepr()->attribute("transform"), nullptr);
    EXPECT_STREQ(cast<SPRect>(item(*document, "s2"))->getRepr()->attribute("id"), "s2");
}

// ---------------------------------------------------------------------------
// SW2A-3b: effective-width numerics. A tiny local delta under a huge scale is a
// real effective change (old 0 -> 1e-10 at scale 1e10 gives effective 1), while
// a relative value finite locally but overflowing after the descrim multiply is
// an explicit InvalidIntent. The overflow fixture uses a natively parsable
// stroke-width 20 at scale 10 and asserts that baseline (old local 20, old
// effective 200, eligible) before planning. RelativePercent = DBL_MAX then
// yields local 20 * (DBL_MAX / 100) ~ 3.59e307 (finite) and effective
// 3.59e307 * 10 ~ 3.59e308 (non-finite), so the rejection is proved to come
// from the descrim multiply. The earlier inline-CSS 1e307 baseline was an
// unverified oracle: it assumed that local survived native parsing without
// asserting it (the parser cause itself was never established).
// ---------------------------------------------------------------------------

TEST_F(StrokeWidthControllerTest, SW2APrepareEffectiveWidthNumerics)
{
    auto document = parse(std::string{svg_open} +
        R"svg(<rect id="tiny" width="10" height="10" style="stroke-width:0" transform="scale(10000000000)"/>)svg"
        R"svg(<rect id="ovf" width="10" height="10" style="stroke-width:20" transform="scale(10)"/>)svg"
        R"(</svg>)");
    ASSERT_TRUE(document);

    auto const before = serialize(*document);

    // scale(1e10): descrim 1e10 and determinant 1e20 both finite. Absolute 1
    // document px stores 1e-10 local; the effective width is 1e-10*1e10 = 1.
    auto const tiny = SW::prepare_stroke_widths(*document, {item(*document, "tiny")}, absolute(1.0), 1);
    auto const *tiny_member = find_member(tiny, item(*document, "tiny"));
    ASSERT_TRUE(tiny_member);
    EXPECT_EQ(tiny_member->outcome, SW::StrokeWidthMemberOutcome::Change);
    ASSERT_TRUE(tiny_member->local_width.has_value());
    EXPECT_NEAR(*tiny_member->local_width, 1e-10, 1e-20);
    EXPECT_NEAR(*tiny_member->local_width * tiny_member->target.transform_scale, 1.0, 1e-9);

    // The overflow fixture is a genuinely native, finite baseline. Assert the
    // query snapshot first: old local 20 under transform_scale 10, old effective
    // 200, and the target eligible. This proves the fixture parses as intended
    // instead of assuming an inline-CSS 1e307 local survived native parsing.
    auto const ovf_query = SW::query_stroke_widths(*document, {item(*document, "ovf")});
    EXPECT_EQ(ovf_query.eligible, 1u);
    ASSERT_EQ(ovf_query.targets.size(), 1u);
    auto const &ovf_target = ovf_query.targets[0];
    EXPECT_EQ(ovf_target.eligibility, SW::StrokeWidthEligibility::Eligible);
    EXPECT_NEAR(ovf_target.transform_scale, 10.0, 1e-9);
    ASSERT_EQ(ovf_target.runs.size(), 1u);
    EXPECT_NEAR(ovf_target.runs[0].style.local_computed, 20.0, 1e-9);
    ASSERT_TRUE(ovf_target.runs[0].style.effective_px.has_value());
    EXPECT_NEAR(*ovf_target.runs[0].style.effective_px, 200.0, 1e-9);

    // Intended arithmetic, asserted before the outcome: 20 * (DBL_MAX / 100) is
    // finite (~3.59e307), but the ordinary effective multiply by transform_scale
    // 10 overflows (~3.59e308, above DBL_MAX ~1.80e308) to a non-finite value.
    double const intended_local = 20.0 * (std::numeric_limits<double>::max() / 100.0);
    ASSERT_TRUE(std::isfinite(intended_local));
    EXPECT_FALSE(std::isfinite(intended_local * 10.0));

    auto const overflow = SW::prepare_stroke_widths(*document, {item(*document, "ovf")},
                                                    relative(std::numeric_limits<double>::max()), 1);
    auto const *ovf = find_member(overflow, item(*document, "ovf"));
    ASSERT_TRUE(ovf);
    // The member retained the native eligible baseline and carries no patch.
    ASSERT_EQ(ovf->target.runs.size(), 1u);
    EXPECT_EQ(ovf->target.runs[0].eligibility, SW::StrokeWidthEligibility::Eligible);
    EXPECT_NEAR(ovf->target.runs[0].style.local_computed, 20.0, 1e-9);
    EXPECT_NEAR(ovf->target.transform_scale, 10.0, 1e-9);
    EXPECT_EQ(ovf->outcome, SW::StrokeWidthMemberOutcome::Excluded);
    EXPECT_EQ(ovf->reason, SW::StrokeWidthMemberReason::InvalidIntent);
    EXPECT_FALSE(ovf->local_width.has_value());

    EXPECT_EQ(serialize(*document), before);
}

// ---------------------------------------------------------------------------
// SW2A-4: dash scaling by each member's own ratio; disabled and old-zero omit;
// an inherited pattern materializes on the leaf while the parent XML stays.
// ---------------------------------------------------------------------------

TEST_F(StrokeWidthControllerTest, SW2APrepareDashScalingAndInheritance)
{
    auto document = parse(std::string{svg_open} +
        R"(<rect id="d" width="10" height="10" style="stroke-width:2;stroke-dasharray:2 5;stroke-dashoffset:3"/>)"
        R"(<rect id="z" width="10" height="10" style="stroke-width:0;stroke-dasharray:2 5;stroke-dashoffset:3"/>)"
        R"(<rect id="neg" width="10" height="10" style="stroke-width:2;stroke-dasharray:2 5;stroke-dashoffset:-3"/>)"
        R"(<g id="g" style="stroke-dasharray:2 5;stroke-dashoffset:3"><rect id="leaf" width="10" height="10" style="stroke-width:2"/></g>)"
        R"svg(<rect id="micro" width="10" height="10" style="stroke-width:1;stroke-dasharray:1e-6 2e-6;stroke-dashoffset:3e-6" transform="scale(10000000000)"/>)svg"
        R"(</svg>)");
    ASSERT_TRUE(document);

    auto const before = serialize(*document);

    auto const scaled = SW::prepare_stroke_widths(*document, {item(*document, "d")}, absolute(4.0, true), 1);
    auto const *d = find_member(scaled, item(*document, "d"));
    ASSERT_TRUE(d);
    EXPECT_EQ(d->outcome, SW::StrokeWidthMemberOutcome::Change);
    ASSERT_TRUE(d->local_width.has_value());
    EXPECT_NEAR(*d->local_width, 4.0, 1e-9);
    ASSERT_TRUE(d->local_dasharray.has_value());
    ASSERT_EQ(d->local_dasharray->size(), 2u);
    EXPECT_NEAR((*d->local_dasharray)[0], 4.0, 1e-9);
    EXPECT_NEAR((*d->local_dasharray)[1], 10.0, 1e-9);
    ASSERT_TRUE(d->local_dashoffset.has_value());
    EXPECT_NEAR(*d->local_dashoffset, 6.0, 1e-9);

    // Native canonicalization of the normal (4,10) pattern round-trips exactly.
    ASSERT_TRUE(d->native_dasharray_css.has_value());
    EXPECT_FALSE(d->native_dasharray_css->empty());
    EXPECT_NE(*d->native_dasharray_css, "none");
    ASSERT_TRUE(d->native_dasharray_computed.has_value());
    ASSERT_EQ(d->native_dasharray_computed->size(), 2u);
    EXPECT_DOUBLE_EQ((*d->native_dasharray_computed)[0], (*d->local_dasharray)[0]);
    EXPECT_DOUBLE_EQ((*d->native_dasharray_computed)[1], (*d->local_dasharray)[1]);

    // Absolute 0 scales the width to 0: the pattern becomes {0,0} and the
    // scaled-to-zero offset is still planned as an explicit change, not gated
    // away by a next_offset != 0 test.
    auto const to_zero = SW::prepare_stroke_widths(*document, {item(*document, "d")}, absolute(0.0, true), 1);
    auto const *d_zero = find_member(to_zero, item(*document, "d"));
    ASSERT_TRUE(d_zero);
    EXPECT_EQ(d_zero->outcome, SW::StrokeWidthMemberOutcome::Change);
    ASSERT_TRUE(d_zero->local_width.has_value());
    EXPECT_NEAR(*d_zero->local_width, 0.0, 1e-12);
    ASSERT_TRUE(d_zero->local_dasharray.has_value());
    ASSERT_EQ(d_zero->local_dasharray->size(), 2u);
    EXPECT_NEAR((*d_zero->local_dasharray)[0], 0.0, 1e-12);
    EXPECT_NEAR((*d_zero->local_dasharray)[1], 0.0, 1e-12);
    ASSERT_TRUE(d_zero->local_dashoffset.has_value());
    EXPECT_NEAR(*d_zero->local_dashoffset, 0.0, 1e-12);

    // All-zero raw pattern: the raw CSS string is retained for a later write
    // while the native computed result is legitimately empty (native zero-pattern
    // normalization). The string stays parseable and is never "none".
    ASSERT_TRUE(d_zero->native_dasharray_css.has_value());
    EXPECT_FALSE(d_zero->native_dasharray_css->empty());
    EXPECT_NE(*d_zero->native_dasharray_css, "none");
    {
        SPIDashArray reparsed;
        reparsed.read(d_zero->native_dasharray_css->c_str());
        EXPECT_TRUE(reparsed.is_valid());
    }
    ASSERT_TRUE(d_zero->native_dasharray_computed.has_value());
    EXPECT_TRUE(d_zero->native_dasharray_computed->empty());

    // Negative finite offset is valid and scales: 2 x (-3) = -6.
    auto const negative = SW::prepare_stroke_widths(*document, {item(*document, "neg")}, absolute(4.0, true), 1);
    auto const *neg = find_member(negative, item(*document, "neg"));
    ASSERT_TRUE(neg);
    EXPECT_EQ(neg->outcome, SW::StrokeWidthMemberOutcome::Change);
    ASSERT_TRUE(neg->local_dasharray.has_value());
    EXPECT_NEAR((*neg->local_dasharray)[0], 4.0, 1e-9);
    EXPECT_NEAR((*neg->local_dasharray)[1], 10.0, 1e-9);
    ASSERT_TRUE(neg->local_dashoffset.has_value());
    EXPECT_NEAR(*neg->local_dashoffset, -6.0, 1e-9);

    auto const disabled = SW::prepare_stroke_widths(*document, {item(*document, "d")}, absolute(4.0, false), 1);
    auto const *d_disabled = find_member(disabled, item(*document, "d"));
    ASSERT_TRUE(d_disabled);
    EXPECT_FALSE(d_disabled->local_dasharray.has_value());
    EXPECT_FALSE(d_disabled->local_dashoffset.has_value());
    EXPECT_FALSE(d_disabled->native_dasharray_css.has_value());
    EXPECT_FALSE(d_disabled->native_dasharray_computed.has_value());

    auto const old_zero = SW::prepare_stroke_widths(*document, {item(*document, "z")}, absolute(4.0, true), 1);
    auto const *z = find_member(old_zero, item(*document, "z"));
    ASSERT_TRUE(z);
    EXPECT_NEAR(*z->local_width, 4.0, 1e-9);
    EXPECT_FALSE(z->local_dasharray.has_value());
    EXPECT_FALSE(z->local_dashoffset.has_value());
    EXPECT_FALSE(z->native_dasharray_css.has_value());
    EXPECT_FALSE(z->native_dasharray_computed.has_value());

    auto const inherited = SW::prepare_stroke_widths(*document, {item(*document, "leaf")}, absolute(4.0, true), 1);
    auto const *leaf = find_member(inherited, item(*document, "leaf"));
    ASSERT_TRUE(leaf);
    ASSERT_TRUE(leaf->local_dasharray.has_value());
    EXPECT_NEAR((*leaf->local_dasharray)[0], 4.0, 1e-9);
    EXPECT_NEAR((*leaf->local_dasharray)[1], 10.0, 1e-9);
    ASSERT_TRUE(leaf->local_dashoffset.has_value());
    EXPECT_NEAR(*leaf->local_dashoffset, 6.0, 1e-9);
    auto const *g_style = cast<SPGroup>(item(*document, "g"))->getRepr()->attribute("style");
    ASSERT_TRUE(g_style);
    EXPECT_NE(std::string(g_style).find("stroke-dasharray:2 5"), std::string::npos);

    // A tiny local dash delta under a huge transform scale is a real effective
    // change: width 1 at scale 1e10 with relative 100.01 moves the effective
    // width by 1e6, while the local dashes move by only ~1e-10, below the 1e-9
    // nearly_equal floor. Exact local inequality must still emit the patch.
    auto const micro = SW::prepare_stroke_widths(*document, {item(*document, "micro")}, relative(100.01, true), 1);
    auto const *micro_member = find_member(micro, item(*document, "micro"));
    ASSERT_TRUE(micro_member);
    EXPECT_EQ(micro_member->outcome, SW::StrokeWidthMemberOutcome::Change);
    ASSERT_TRUE(micro_member->local_dasharray.has_value());
    ASSERT_EQ(micro_member->local_dasharray->size(), 2u);
    EXPECT_NEAR((*micro_member->local_dasharray)[0], 1.0001e-6, 1e-15);
    EXPECT_NEAR((*micro_member->local_dasharray)[1], 2.0002e-6, 1e-15);
    ASSERT_TRUE(micro_member->local_dashoffset.has_value());
    EXPECT_NEAR(*micro_member->local_dashoffset, 3.0003e-6, 1e-15);

    // The 1e-6 pattern is above the native zero boundary and round-trips exactly.
    ASSERT_TRUE(micro_member->native_dasharray_css.has_value());
    ASSERT_TRUE(micro_member->native_dasharray_computed.has_value());
    ASSERT_EQ(micro_member->native_dasharray_computed->size(), 2u);
    EXPECT_DOUBLE_EQ((*micro_member->native_dasharray_computed)[0],
                     (*micro_member->local_dasharray)[0]);
    EXPECT_DOUBLE_EQ((*micro_member->native_dasharray_computed)[1],
                     (*micro_member->local_dasharray)[1]);

    EXPECT_EQ(serialize(*document), before);
}

// ---------------------------------------------------------------------------
// SW2A-5: priority. Inline !important width is retained; a stylesheet
// !important width that a change demands is excluded; a non-changing
// stylesheet width is a legitimate no-op; a competing dash !important excludes
// only when a dash change is demanded.
// ---------------------------------------------------------------------------

TEST_F(StrokeWidthControllerTest, SW2APrepareStylePriorityDecisions)
{
    auto document = parse(
        R"(<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink">)"
        R"(<style>.sw { stroke-width: 3 !important; } .nsw { stroke-width: 2 !important; } .dd { stroke-dasharray: 2 5 !important; } .so { stroke-dashoffset: 3 !important; } .sa0 { stroke-dashoffset: 0 !important; } .san { stroke-dasharray: none !important; }</style>)"
        R"(<rect id="iw" width="10" height="10" style="stroke-width:2 !important"/>)"
        R"(<rect id="sw" class="sw" width="10" height="10"/>)"
        R"(<rect id="nsw" class="nsw" width="10" height="10"/>)"
        R"(<rect id="dd" class="dd" width="10" height="10" style="stroke-width:2"/>)"
        R"(<rect id="so" class="so" width="10" height="10" style="stroke-width:2;stroke-dasharray:2 5"/>)"
        R"(<rect id="sa0" class="sa0" width="10" height="10" style="stroke-width:2;stroke-dasharray:2 5"/>)"
        R"(<rect id="san" class="san" width="10" height="10" style="stroke-width:2;stroke-dashoffset:3"/>)"
        R"(<rect id="ii" width="10" height="10" style="stroke-width:2;stroke-dasharray:2 5 !important;stroke-dashoffset:3 !important"/>)"
        R"(</svg>)");
    ASSERT_TRUE(document);

    auto const before = serialize(*document);

    auto const inline_plan = SW::prepare_stroke_widths(*document, {item(*document, "iw")}, absolute(10.0), 1);
    auto const *iw = find_member(inline_plan, item(*document, "iw"));
    ASSERT_TRUE(iw);
    EXPECT_EQ(iw->outcome, SW::StrokeWidthMemberOutcome::Change);
    EXPECT_TRUE(iw->width_important);
    EXPECT_NEAR(*iw->local_width, 10.0, 1e-9);

    auto const sheet_plan = SW::prepare_stroke_widths(*document, {item(*document, "sw")}, absolute(10.0), 1);
    auto const *sw = find_member(sheet_plan, item(*document, "sw"));
    ASSERT_TRUE(sw);
    EXPECT_EQ(sw->outcome, SW::StrokeWidthMemberOutcome::Excluded);
    EXPECT_EQ(sw->reason, SW::StrokeWidthMemberReason::StylePriority);
    EXPECT_FALSE(sw->local_width.has_value());

    auto const noop_plan = SW::prepare_stroke_widths(*document, {item(*document, "nsw")}, absolute(2.0), 1);
    auto const *nsw = find_member(noop_plan, item(*document, "nsw"));
    ASSERT_TRUE(nsw);
    EXPECT_EQ(nsw->outcome, SW::StrokeWidthMemberOutcome::Unchanged);

    auto const dash_plan = SW::prepare_stroke_widths(*document, {item(*document, "dd")}, absolute(4.0, true), 1);
    auto const *dd = find_member(dash_plan, item(*document, "dd"));
    ASSERT_TRUE(dd);
    EXPECT_EQ(dd->outcome, SW::StrokeWidthMemberOutcome::Excluded);
    EXPECT_EQ(dd->reason, SW::StrokeWidthMemberReason::StylePriority);

    auto const dash_noop = SW::prepare_stroke_widths(*document, {item(*document, "dd")}, absolute(2.0, true), 1);
    auto const *dd_noop = find_member(dash_noop, item(*document, "dd"));
    ASSERT_TRUE(dd_noop);
    EXPECT_EQ(dd_noop->outcome, SW::StrokeWidthMemberOutcome::Unchanged);

    // Independent dash priorities. A sheet-important offset alone blocks only
    // the demanded offset scaling (unimportant array could still change):
    // absolute 4 on width 2 demands 3 -> 6, controlled by the sheet -> excluded.
    auto const offset_blocked = SW::prepare_stroke_widths(*document, {item(*document, "so")}, absolute(4.0, true), 1);
    auto const *so = find_member(offset_blocked, item(*document, "so"));
    ASSERT_TRUE(so);
    EXPECT_EQ(so->outcome, SW::StrokeWidthMemberOutcome::Excluded);
    EXPECT_EQ(so->reason, SW::StrokeWidthMemberReason::StylePriority);

    // A sheet-important `stroke-dasharray:none` is unchanged, so its priority
    // must not block the unrelated normal offset change 3 -> 6.
    auto const offset_allowed = SW::prepare_stroke_widths(*document, {item(*document, "san")}, absolute(4.0, true), 1);
    auto const *san = find_member(offset_allowed, item(*document, "san"));
    ASSERT_TRUE(san);
    EXPECT_EQ(san->outcome, SW::StrokeWidthMemberOutcome::Change);
    EXPECT_FALSE(san->local_dasharray.has_value());
    ASSERT_TRUE(san->local_dashoffset.has_value());
    EXPECT_NEAR(*san->local_dashoffset, 6.0, 1e-9);

    // A sheet-important offset 0 is unchanged, so it must not block the
    // unrelated normal array change 2,5 -> 4,10.
    auto const array_allowed = SW::prepare_stroke_widths(*document, {item(*document, "sa0")}, absolute(4.0, true), 1);
    auto const *sa0 = find_member(array_allowed, item(*document, "sa0"));
    ASSERT_TRUE(sa0);
    EXPECT_EQ(sa0->outcome, SW::StrokeWidthMemberOutcome::Change);
    ASSERT_TRUE(sa0->local_dasharray.has_value());
    EXPECT_NEAR((*sa0->local_dasharray)[0], 4.0, 1e-9);
    EXPECT_NEAR((*sa0->local_dasharray)[1], 10.0, 1e-9);
    EXPECT_FALSE(sa0->local_dashoffset.has_value());

    // Inline !important for the array and the offset is retained independently
    // and never excludes the member's own inline change.
    auto const inline_dash = SW::prepare_stroke_widths(*document, {item(*document, "ii")}, absolute(4.0, true), 1);
    auto const *ii = find_member(inline_dash, item(*document, "ii"));
    ASSERT_TRUE(ii);
    EXPECT_EQ(ii->outcome, SW::StrokeWidthMemberOutcome::Change);
    EXPECT_TRUE(ii->dasharray_important);
    EXPECT_TRUE(ii->dashoffset_important);
    ASSERT_TRUE(ii->local_dasharray.has_value());
    EXPECT_NEAR((*ii->local_dasharray)[0], 4.0, 1e-9);
    EXPECT_NEAR((*ii->local_dasharray)[1], 10.0, 1e-9);
    ASSERT_TRUE(ii->local_dashoffset.has_value());
    EXPECT_NEAR(*ii->local_dashoffset, 6.0, 1e-9);

    EXPECT_EQ(serialize(*document), before);
}

// ---------------------------------------------------------------------------
// SW2A-6 (owner decision 2026-09-28): a changed selected source with selected
// dependent clones is planned; the clones are never written and are counted as
// following their original. A no-op source has none. XML and a seeded Redo
// stay intact while preparing.
// ---------------------------------------------------------------------------

TEST_F(StrokeWidthControllerTest, SW2APrepareDependentClonesFollowTheirSource)
{
    auto document = parse(std::string{svg_open} +
        R"(<rect id="src" width="10" height="10" style="stroke-width:2"/>)"
        R"svg(<use id="uexplicit" xlink:href="#src" x="20" transform="scale(2)"/>)svg"
        R"(<use id="uprotected" xlink:href="#src" x="40" sodipodi:insensitive="true"/>)"
        R"(<rect id="other" width="10" height="10" style="stroke-width:5"/>)"
        R"(</svg>)");
    ASSERT_TRUE(document);

    auto *other = cast<SPRect>(item(*document, "other"));
    ASSERT_TRUE(other);
    other->setLocked(true);
    Inkscape::DocumentUndo::done(document.get(), Inkscape::Util::Internal::ContextString("Fixture"), "");
    ASSERT_TRUE(Inkscape::DocumentUndo::undo(document.get()));

    auto const before = serialize(*document);
    std::vector<SPItem *> roots = {item(*document, "src"), item(*document, "uexplicit"),
                                   item(*document, "uprotected")};

    auto const conflicting = SW::prepare_stroke_widths(*document, roots, absolute(10.0), 3);
    EXPECT_EQ(conflicting.state, SW::StrokeWidthPlanState::Prepared);
    EXPECT_EQ(conflicting.rejection, SW::StrokeWidthMemberReason::None);
    EXPECT_EQ(conflicting.following_clones, 2u);
    EXPECT_EQ(conflicting.planned_changes, 1u);
    EXPECT_EQ(conflicting.unchanged, 0u);
    EXPECT_EQ(conflicting.excluded, 2u);
    ASSERT_TRUE(find_member(conflicting, item(*document, "src")));
    EXPECT_EQ(find_member(conflicting, item(*document, "src"))->outcome, SW::StrokeWidthMemberOutcome::Change);
    // Both clone records were reported by the query (explicit source + locked).
    EXPECT_EQ(conflicting.query.unavailable, 2u);
    EXPECT_EQ(conflicting.query.eligible, 1u);

    auto const noop = SW::prepare_stroke_widths(*document, roots, absolute(2.0), 3);
    EXPECT_EQ(noop.state, SW::StrokeWidthPlanState::Prepared);
    EXPECT_EQ(noop.planned_changes, 0u);
    EXPECT_EQ(noop.unchanged, 1u);
    EXPECT_EQ(noop.excluded, 2u);
    EXPECT_EQ(noop.following_clones, 0u);

    EXPECT_EQ(serialize(*document), before);
    EXPECT_TRUE(Inkscape::DocumentUndo::redo(document.get()));
}

// ---------------------------------------------------------------------------
// SW2A-7: intent validation and interim adapter boundaries. Invalid intents and
// unsupported intents reject the whole plan; hairline relative/absolute members
// are excluded distinctly; text/range are pending; an invalid range never falls
// back; group roots and duplicates reuse the query result.
// ---------------------------------------------------------------------------

TEST_F(StrokeWidthControllerTest, SW2APrepareIntentAndAdapterExclusions)
{
    auto document = parse(std::string{svg_open} +
        R"(<rect id="hair" width="10" height="10" style="stroke-width:2;-inkscape-stroke:hairline"/>)"
        R"(<text id="txt" x="0" y="20" style="font-family:sans-serif;font-size:20px;stroke-width:2">abc</text>)"
        R"(<g id="grp"><rect id="g1" width="10" height="10" style="stroke-width:2"/><rect id="g2" width="10" height="10" style="stroke-width:8"/><image id="gimg" width="10" height="10"/></g>)"
        R"(<rect id="plain" width="10" height="10" style="stroke-width:2"/>)"
        R"(<rect id="painted" width="10" height="10" style="stroke:black;stroke-width:2"/>)"
        R"(</svg>)");
    ASSERT_TRUE(document);

    auto const before = serialize(*document);

    double const nan = std::numeric_limits<double>::quiet_NaN();
    double const inf = std::numeric_limits<double>::infinity();
    auto const bad_negative = SW::prepare_stroke_widths(*document, {item(*document, "plain")}, absolute(-1.0), 1);
    EXPECT_EQ(bad_negative.state, SW::StrokeWidthPlanState::Rejected);
    EXPECT_EQ(bad_negative.rejection, SW::StrokeWidthMemberReason::InvalidIntent);
    // Even a rejected plan freezes the requested intent for later re-derivation.
    EXPECT_EQ(bad_negative.intent.kind, SW::StrokeWidthIntentKind::AbsoluteCssPx);
    EXPECT_DOUBLE_EQ(bad_negative.intent.value, -1.0);
    auto const bad_nan = SW::prepare_stroke_widths(*document, {item(*document, "plain")}, absolute(nan), 1);
    EXPECT_EQ(bad_nan.rejection, SW::StrokeWidthMemberReason::InvalidIntent);
    auto const bad_inf = SW::prepare_stroke_widths(*document, {item(*document, "plain")}, absolute(inf), 1);
    EXPECT_EQ(bad_inf.rejection, SW::StrokeWidthMemberReason::InvalidIntent);
    auto const bad_relative = SW::prepare_stroke_widths(*document, {item(*document, "plain")}, relative(-5.0), 1);
    EXPECT_EQ(bad_relative.rejection, SW::StrokeWidthMemberReason::InvalidIntent);

    // An out-of-range enum cast must be rejected by the exhaustive boundary
    // validation, never silently no-op through the numeric switch.
    SW::StrokeWidthIntent bad_kind;
    bad_kind.kind = static_cast<SW::StrokeWidthIntentKind>(99);
    auto const bad_kind_plan = SW::prepare_stroke_widths(*document, {item(*document, "plain")}, bad_kind, 1);
    EXPECT_EQ(bad_kind_plan.state, SW::StrokeWidthPlanState::Rejected);
    EXPECT_EQ(bad_kind_plan.rejection, SW::StrokeWidthMemberReason::InvalidIntent);
    EXPECT_EQ(static_cast<int>(bad_kind_plan.intent.kind), 99);

    SW::StrokeWidthIntent hairline_intent;
    hairline_intent.kind = SW::StrokeWidthIntentKind::Hairline;
    auto const hairline_plan = SW::prepare_stroke_widths(*document, {item(*document, "plain")}, hairline_intent, 1);
    // Owner decision 2026-09-24: explicit Hairline is a supported width mode.
    EXPECT_EQ(hairline_plan.state, SW::StrokeWidthPlanState::Prepared);
    EXPECT_EQ(hairline_plan.planned_changes, 1u);

    SW::StrokeWidthIntent remove_intent;
    remove_intent.kind = SW::StrokeWidthIntentKind::RemoveStroke;
    auto const remove_plan = SW::prepare_stroke_widths(*document, {item(*document, "painted")}, remove_intent, 1);
    // Owner decision 2026-09-24: final zero removes eligible strokes.
    EXPECT_EQ(remove_plan.state, SW::StrokeWidthPlanState::Prepared);
    EXPECT_EQ(remove_plan.planned_changes, 1u);
    EXPECT_EQ(remove_plan.intent.kind, SW::StrokeWidthIntentKind::RemoveStroke);

    // Numeric intents on a hairline member are excluded, never reinterpreted.
    auto const hair_relative = SW::prepare_stroke_widths(*document, {item(*document, "hair")}, relative(200.0), 1);
    auto const *hair_rel = find_member(hair_relative, item(*document, "hair"));
    ASSERT_TRUE(hair_rel);
    EXPECT_EQ(hair_rel->outcome, SW::StrokeWidthMemberOutcome::Excluded);
    EXPECT_EQ(hair_rel->reason, SW::StrokeWidthMemberReason::UnsupportedIntent);
    auto const hair_absolute = SW::prepare_stroke_widths(*document, {item(*document, "hair")}, absolute(3.0), 1);
    auto const *hair_abs = find_member(hair_absolute, item(*document, "hair"));
    ASSERT_TRUE(hair_abs);
    // Owner decision 2026-09-24: deliberate absolute input converts a hairline.
    EXPECT_EQ(hair_abs->outcome, SW::StrokeWidthMemberOutcome::Change);

    // Text is always pending, whole-object and via an explicit valid range.
    auto const text_plan = SW::prepare_stroke_widths(*document, {item(*document, "txt")}, absolute(3.0), 1);
    auto const *txt = find_member(text_plan, item(*document, "txt"));
    ASSERT_TRUE(txt);
    EXPECT_EQ(txt->outcome, SW::StrokeWidthMemberOutcome::Excluded);
    EXPECT_EQ(txt->reason, SW::StrokeWidthMemberReason::TextAdapterPending);
    EXPECT_EQ(text_plan.planned_changes, 0u);

    SW::StrokeWidthTextRange range;
    range.owner = item(*document, "txt");
    range.first_char = 0;
    range.last_char = 3;
    auto const range_plan = SW::prepare_stroke_widths(*document, range, {item(*document, "txt")}, absolute(3.0), 1);
    EXPECT_FALSE(range_plan.query.range_rejected);
    ASSERT_TRUE(range_plan.text_scope.has_value());
    EXPECT_EQ(find_member(range_plan, item(*document, "txt"))->reason,
              SW::StrokeWidthMemberReason::TextAdapterPending);

    // Invalid range rejects with the query reason and never falls back to the
    // supplied other root.
    SW::StrokeWidthTextRange bad_range = range;
    bad_range.first_char = 5;
    bad_range.last_char = 6;
    auto const rejected_range = SW::prepare_stroke_widths(
        *document, bad_range, {item(*document, "txt"), item(*document, "plain")}, absolute(3.0), 1);
    EXPECT_EQ(rejected_range.state, SW::StrokeWidthPlanState::Rejected);
    EXPECT_TRUE(rejected_range.query.range_rejected);
    EXPECT_EQ(rejected_range.query.range_exclusion, SW::StrokeWidthExclusion::TextRangeOutOfBounds);
    EXPECT_TRUE(rejected_range.members.empty());
    EXPECT_EQ(rejected_range.planned_changes, 0u);
    // The intent is frozen even on the range-rejected early return.
    EXPECT_EQ(rejected_range.intent.kind, SW::StrokeWidthIntentKind::AbsoluteCssPx);
    EXPECT_DOUBLE_EQ(rejected_range.intent.value, 3.0);

    // Group resolution and duplicate roots reuse the query result.
    auto const group_plan = SW::prepare_stroke_widths(*document, {item(*document, "grp")}, absolute(10.0), 1);
    EXPECT_EQ(group_plan.query.eligible, 2u);
    EXPECT_EQ(group_plan.query.incompatible, 1u);
    EXPECT_EQ(group_plan.planned_changes, 2u);
    EXPECT_EQ(group_plan.excluded, 1u);
    EXPECT_EQ(group_plan.members.size(), 2u);

    auto const dedup_plan = SW::prepare_stroke_widths(
        *document, {item(*document, "g1"), item(*document, "g1")}, absolute(10.0), 1);
    EXPECT_EQ(dedup_plan.query.eligible, 1u);
    EXPECT_EQ(dedup_plan.planned_changes, 1u);
    EXPECT_EQ(dedup_plan.members.size(), 1u);

    EXPECT_EQ(serialize(*document), before);
}

// ---------------------------------------------------------------------------
// SW2A-DEP-1: the bounded dependent-selection guard follows a selected clone's
// native original graph. A changed shape reached through a nested use chain, a
// clone of a group that owns the changed shape, or a clone of a group whose
// intermediate use references the changed shape all count the selected clone
// as following its original; the source change is planned.
// ---------------------------------------------------------------------------

TEST_F(StrokeWidthControllerTest, SW2APrepareDependentCloneNestedConflict)
{
    auto document = parse(std::string{svg_open} +
        R"(<g id="srcgrp"><rect id="src" width="10" height="10" style="stroke-width:2"/></g>)"
        R"(<g id="midgrp"><use id="miduse" xlink:href="#src"/></g>)"
        R"(<use id="chainuse" xlink:href="#src"/>)"
        R"(<use id="clonegroup" xlink:href="#srcgrp" x="20"/>)"
        R"(<use id="cloneintermediate" xlink:href="#midgrp" x="40"/>)"
        R"(<use id="chain" xlink:href="#chainuse" x="60"/>)"
        R"(</svg>)");
    ASSERT_TRUE(document);

    auto const before = serialize(*document);
    auto *src = item(*document, "src");
    ASSERT_TRUE(src);

    char const *const clone_ids[] = {"chain", "clonegroup", "cloneintermediate"};
    for (auto const *clone_id : clone_ids) {
        auto *clone = item(*document, clone_id);
        ASSERT_TRUE(clone) << clone_id;
        auto const plan = SW::prepare_stroke_widths(*document, {src, clone}, absolute(10.0), 5);
        EXPECT_EQ(plan.state, SW::StrokeWidthPlanState::Prepared) << clone_id;
        EXPECT_EQ(plan.rejection, SW::StrokeWidthMemberReason::None) << clone_id;
        EXPECT_EQ(plan.following_clones, 1u) << clone_id;
        EXPECT_EQ(plan.planned_changes, 1u) << clone_id;
        EXPECT_EQ(plan.query.eligible, 1u) << clone_id;

        // Root order is irrelevant.
        auto const reversed = SW::prepare_stroke_widths(*document, {clone, src}, absolute(10.0), 5);
        EXPECT_EQ(reversed.state, SW::StrokeWidthPlanState::Prepared) << clone_id;
        EXPECT_EQ(reversed.following_clones, 1u) << clone_id;
        EXPECT_EQ(reversed.planned_changes, 1u) << clone_id;
    }

    EXPECT_EQ(serialize(*document), before);
}

// ---------------------------------------------------------------------------
// SW2A-DEP-2: an explicitly selected protected group is inspected read-only, so
// a nested owned clone that depends on the changed source rejects the plan with
// the protected clone recorded. A no-op source stays Prepared; an unselected
// protected clone and an independent bitmap do not block a source change.
// ---------------------------------------------------------------------------

TEST_F(StrokeWidthControllerTest, SW2APrepareDependentCloneProtectedPartial)
{
    auto document = parse(std::string{svg_open} +
        R"(<rect id="src" width="10" height="10" style="stroke-width:2"/>)"
        R"(<g id="locked" sodipodi:insensitive="true"><use id="lockedclone" xlink:href="#src"/></g>)"
        R"(<g id="unselectedlocked" sodipodi:insensitive="true"><use id="unselclone" xlink:href="#src"/></g>)"
        R"(<image id="bmp" width="10" height="10"/>)"
        R"(</svg>)");
    ASSERT_TRUE(document);

    auto const before = serialize(*document);
    auto *src = item(*document, "src");
    ASSERT_TRUE(src);

    // Selected locked group: its nested owned clone is inspected read-only even
    // though the query never descends the protected group.
    auto const conflict = SW::prepare_stroke_widths(
        *document, {src, item(*document, "locked")}, absolute(10.0), 6);
    EXPECT_EQ(conflict.state, SW::StrokeWidthPlanState::Prepared);
    EXPECT_EQ(conflict.rejection, SW::StrokeWidthMemberReason::None);
    EXPECT_EQ(conflict.following_clones, 1u) << "the locked group's clone follows the source";
    EXPECT_EQ(conflict.planned_changes, 1u);
    EXPECT_EQ(conflict.query.eligible, 1u);
    ASSERT_TRUE(find_excluded(conflict.query, SW::StrokeWidthExclusion::LockedAncestor));

    // No-op source: no changed shapes, so the same scope stays Prepared.
    auto const noop = SW::prepare_stroke_widths(
        *document, {src, item(*document, "locked")}, absolute(2.0), 6);
    EXPECT_EQ(noop.state, SW::StrokeWidthPlanState::Prepared);
    EXPECT_EQ(noop.planned_changes, 0u);
    EXPECT_EQ(noop.unchanged, 1u);
    EXPECT_EQ(noop.excluded, 1u);
    EXPECT_EQ(noop.dependent_target.get(), nullptr);

    // Unselected protected clone does not block: the source alone is planned.
    auto const source_only = SW::prepare_stroke_widths(*document, {src}, absolute(10.0), 6);
    EXPECT_EQ(source_only.state, SW::StrokeWidthPlanState::Prepared);
    EXPECT_EQ(source_only.planned_changes, 1u);
    EXPECT_EQ(source_only.excluded, 0u);
    EXPECT_EQ(source_only.dependent_target.get(), nullptr);

    // Independent bitmap stays excluded while the source change proceeds.
    auto const with_bitmap = SW::prepare_stroke_widths(
        *document, {src, item(*document, "bmp")}, absolute(10.0), 6);
    EXPECT_EQ(with_bitmap.state, SW::StrokeWidthPlanState::Prepared);
    EXPECT_EQ(with_bitmap.planned_changes, 1u);
    EXPECT_EQ(with_bitmap.excluded, 1u);
    EXPECT_EQ(with_bitmap.query.incompatible, 1u);
    EXPECT_TRUE(find_excluded(with_bitmap.query, SW::StrokeWidthExclusion::Bitmap));

    EXPECT_EQ(serialize(*document), before);
}

// ---------------------------------------------------------------------------
// SW2A-DEP-3: a selected clone whose source cannot be resolved rejects the plan
// as DependentCloneUncertain with the concrete clone recorded, and a valid
// unrelated selected clone never blocks. Source order does not change the
// outcome.
//
// Unexecuted oracle (not run here): a bounded native `use -> use -> use` cycle
// cannot be asserted without first proving the native loader tolerates it during
// ensureUpToDate. The source scan keeps its in-progress cycle guard, which maps
// such a cycle to the same DependentCloneUncertain outcome with the concrete
// selected clone recorded; this is deliberately not constructed in a live
// fixture instead of inventing fake objects.
// ---------------------------------------------------------------------------

TEST_F(StrokeWidthControllerTest, SW2APrepareDependentCloneUncertainAndUnrelated)
{
    auto document = parse(std::string{svg_open} +
        R"(<rect id="src" width="10" height="10" style="stroke-width:2"/>)"
        R"(<use id="missing" xlink:href="#nope" x="20"/>)"
        R"(<rect id="plain" width="10" height="10"/>)"
        R"(<use id="unrelated" xlink:href="#plain" x="40"/>)"
        R"(</svg>)");
    ASSERT_TRUE(document);

    auto const before = serialize(*document);
    auto *src = item(*document, "src");
    auto *missing = item(*document, "missing");
    ASSERT_TRUE(src);
    ASSERT_TRUE(missing);

    auto const uncertain = SW::prepare_stroke_widths(*document, {src, missing}, absolute(10.0), 7);
    EXPECT_EQ(uncertain.state, SW::StrokeWidthPlanState::Rejected);
    EXPECT_EQ(uncertain.rejection, SW::StrokeWidthMemberReason::DependentCloneUncertain);
    EXPECT_EQ(uncertain.dependent_target.get(), missing);
    EXPECT_TRUE(uncertain.members.empty());
    EXPECT_EQ(uncertain.planned_changes, 0u);
    EXPECT_EQ(uncertain.unchanged, 0u);
    EXPECT_EQ(uncertain.excluded, 0u);
    EXPECT_EQ(uncertain.query.missing_sources, 1u);

    // Reversed source order agrees.
    auto const uncertain_reversed = SW::prepare_stroke_widths(*document, {missing, src}, absolute(10.0), 7);
    EXPECT_EQ(uncertain_reversed.state, SW::StrokeWidthPlanState::Rejected);
    EXPECT_EQ(uncertain_reversed.rejection, SW::StrokeWidthMemberReason::DependentCloneUncertain);
    EXPECT_EQ(uncertain_reversed.dependent_target.get(), missing);

    // A valid unrelated selected clone does not block the source change.
    auto const unrelated = SW::prepare_stroke_widths(
        *document, {src, item(*document, "unrelated")}, absolute(10.0), 7);
    EXPECT_EQ(unrelated.state, SW::StrokeWidthPlanState::Prepared);
    EXPECT_EQ(unrelated.rejection, SW::StrokeWidthMemberReason::None);
    EXPECT_EQ(unrelated.dependent_target.get(), nullptr);
    EXPECT_EQ(unrelated.planned_changes, 1u);
    auto const unrelated_reversed = SW::prepare_stroke_widths(
        *document, {item(*document, "unrelated"), src}, absolute(10.0), 7);
    EXPECT_EQ(unrelated_reversed.state, SW::StrokeWidthPlanState::Prepared);
    EXPECT_EQ(unrelated_reversed.planned_changes, 1u);

    EXPECT_EQ(serialize(*document), before);
}

// ---------------------------------------------------------------------------
// SW2B-PREP-A: a requested nonzero dash pattern too small for the native
// parser's zero normalization is honestly excluded (InvalidDash, no patch
// fields) rather than downgraded; the same member scaled to exactly zero is a
// valid change whose native computed array is legitimately empty.
// ---------------------------------------------------------------------------

TEST_F(StrokeWidthControllerTest, SW2BPrepareUnrepresentableTinyDashRejected)
{
    auto document = parse(std::string{svg_open} +
        R"(<rect id="t" width="10" height="10" style="stroke-width:2;stroke-dasharray:2 5"/>)"
        R"(</svg>)");
    ASSERT_TRUE(document);

    auto const before = serialize(*document);
    auto *target = item(*document, "t");
    ASSERT_TRUE(target);

    // width 2 -> 1e-12 local with scaling on: the scaled {1e-12, 2.5e-12} is
    // nonzero but the native parser normalizes it to empty, so it is rejected.
    auto const tiny = SW::prepare_stroke_widths(*document, {target}, absolute(1e-12, true), 3);
    EXPECT_EQ(tiny.state, SW::StrokeWidthPlanState::Prepared);
    EXPECT_EQ(tiny.query.eligible, 1u);
    EXPECT_EQ(tiny.planned_changes, 0u);
    EXPECT_EQ(tiny.excluded, 1u);
    auto const *tiny_member = find_member(tiny, target);
    ASSERT_TRUE(tiny_member);
    EXPECT_EQ(tiny_member->outcome, SW::StrokeWidthMemberOutcome::Excluded);
    EXPECT_EQ(tiny_member->reason, SW::StrokeWidthMemberReason::InvalidDash);
    EXPECT_FALSE(tiny_member->local_width.has_value());
    EXPECT_FALSE(tiny_member->local_dasharray.has_value());
    EXPECT_FALSE(tiny_member->local_dashoffset.has_value());
    EXPECT_FALSE(tiny_member->native_dasharray_css.has_value());
    EXPECT_FALSE(tiny_member->native_dasharray_computed.has_value());

    // The same width moved to exactly 0 is accepted: the raw {0,0} is retained
    // while the native computed result is empty.
    auto const to_zero = SW::prepare_stroke_widths(*document, {target}, absolute(0.0, true), 3);
    EXPECT_EQ(to_zero.planned_changes, 1u);
    auto const *zero_member = find_member(to_zero, target);
    ASSERT_TRUE(zero_member);
    EXPECT_EQ(zero_member->outcome, SW::StrokeWidthMemberOutcome::Change);
    ASSERT_TRUE(zero_member->local_width.has_value());
    EXPECT_DOUBLE_EQ(*zero_member->local_width, 0.0);
    ASSERT_TRUE(zero_member->local_dasharray.has_value());
    ASSERT_EQ(zero_member->local_dasharray->size(), 2u);
    EXPECT_DOUBLE_EQ((*zero_member->local_dasharray)[0], 0.0);
    EXPECT_DOUBLE_EQ((*zero_member->local_dasharray)[1], 0.0);
    ASSERT_TRUE(zero_member->native_dasharray_css.has_value());
    EXPECT_FALSE(zero_member->native_dasharray_css->empty());
    ASSERT_TRUE(zero_member->native_dasharray_computed.has_value());
    EXPECT_TRUE(zero_member->native_dasharray_computed->empty());

    EXPECT_EQ(serialize(*document), before);
}

// ---------------------------------------------------------------------------
// SW2B-PREP-B: authored XML/ownership preservation snapshot. The member records
// the weak original parent and the sorted exact non-style repr attributes, and
// plan.intent copies the request. Expected attributes are captured from the
// parsed document before prepare to avoid authored-normalization assumptions.
// No assertion claims these raw attributes prove computed rendering.
// ---------------------------------------------------------------------------

TEST_F(StrokeWidthControllerTest, SW2BPreparePreservationSnapshot)
{
    auto document = parse(
        R"(<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" xmlns:custom="http://example.com/custom">)"
        R"(<g id="g">)"
        R"svg(<rect id="styled" transform="scale(2)" width="10" height="10" clip-path="url(#cp)" custom:meta="alpha" style="stroke-width:2"/>)svg"
        R"(<rect id="empty" width="10" height="10" style=""/>)"
        R"(<rect id="nostyle" width="10" height="10"/>)"
        R"(</g>)"
        R"(</svg>)");
    ASSERT_TRUE(document);
    auto const before = serialize(*document);

    auto *group = item(*document, "g");
    auto *styled = item(*document, "styled");
    auto *empty = item(*document, "empty");
    auto *nostyle = item(*document, "nostyle");
    ASSERT_TRUE(group);
    ASSERT_TRUE(styled);
    ASSERT_TRUE(empty);
    ASSERT_TRUE(nostyle);

    // Explicit fixture preconditions; if the native parser normalized these
    // away, the test reports that parser assumption instead of masking it.
    ASSERT_NE(styled->getRepr()->attribute("custom:meta"), nullptr);
    ASSERT_NE(styled->getRepr()->attribute("clip-path"), nullptr);
    ASSERT_NE(empty->getRepr()->attribute("style"), nullptr);
    ASSERT_EQ(nostyle->getRepr()->attribute("style"), nullptr);

    // Capture the parsed authored attributes before prepare.
    auto const styled_expected = authored_attributes(styled);
    auto const empty_expected = authored_attributes(empty);
    auto const nostyle_expected = authored_attributes(nostyle);
    char const *styled_style_before = styled->getRepr()->attribute("style");

    auto const plan = SW::prepare_stroke_widths(*document, {group}, absolute(3.0, true), 9);
    EXPECT_EQ(plan.state, SW::StrokeWidthPlanState::Prepared);
    EXPECT_EQ(plan.intent.kind, SW::StrokeWidthIntentKind::AbsoluteCssPx);
    EXPECT_DOUBLE_EQ(plan.intent.value, 3.0);
    EXPECT_TRUE(plan.intent.scale_dashes);

    auto check_snapshot = [&](SPItem *owner, SPObject *expected_parent,
                              std::vector<std::pair<std::string, std::string>> const &expected) {
        auto const *member = find_member(plan, owner);
        ASSERT_TRUE(member);
        EXPECT_EQ(member->original_parent.get(), expected_parent);
        EXPECT_TRUE(member->non_style_attributes == expected);
        EXPECT_TRUE(std::is_sorted(member->non_style_attributes.begin(),
                                   member->non_style_attributes.end()));
        for (auto const &attribute : member->non_style_attributes) {
            EXPECT_NE(attribute.first, "style");
        }
    };
    check_snapshot(styled, static_cast<SPObject *>(group), styled_expected);
    check_snapshot(empty, static_cast<SPObject *>(group), empty_expected);
    check_snapshot(nostyle, static_cast<SPObject *>(group), nostyle_expected);

    // The fixture really carries clip-path and namespaced metadata.
    auto const *styled_member = find_member(plan, styled);
    ASSERT_TRUE(styled_member);
    auto const &styled_attrs = styled_member->non_style_attributes;
    auto has_name = [&](char const *name) {
        return std::any_of(styled_attrs.begin(), styled_attrs.end(),
                           [name](auto const &a) { return a.first == name; });
    };
    EXPECT_TRUE(has_name("clip-path"));
    EXPECT_TRUE(has_name("custom:meta"));
    EXPECT_TRUE(has_name("transform"));

    // inline_style keeps the nullopt-versus-empty distinction.
    if (styled_style_before) {
        ASSERT_TRUE(styled_member->inline_style.has_value());
        EXPECT_EQ(*styled_member->inline_style, std::string(styled_style_before));
    } else {
        EXPECT_FALSE(styled_member->inline_style.has_value());
    }
    auto const *empty_member = find_member(plan, empty);
    ASSERT_TRUE(empty_member);
    ASSERT_TRUE(empty_member->inline_style.has_value());
    EXPECT_TRUE(empty_member->inline_style->empty());
    auto const *nostyle_member = find_member(plan, nostyle);
    ASSERT_TRUE(nostyle_member);
    EXPECT_FALSE(nostyle_member->inline_style.has_value());

    EXPECT_EQ(serialize(*document), before);
}

// ---------------------------------------------------------------------------
// SW2B-1: group/multiroot with an independent bitmap. Absolute 10 writes each
// eligible member once (inherited leaf included), the bitmap is excluded, the
// group's own style is never rewritten, and one caller-owned atomic commit with
// an output-ready callback gives one Undo/Redo and a save/reopen round trip.
// ---------------------------------------------------------------------------

TEST_F(StrokeWidthControllerTest, SW2BApplyGroupMultirootBitmapAbsoluteCommitUndoRedo)
{
    auto document = parse(std::string{svg_open} +
        R"(<g id="grp" style="stroke-width:2">)"
        R"(<rect id="a" width="10" height="10"/>)"
        R"(<rect id="b" width="10" height="10" style="stroke-width:2"/>)"
        R"(</g>)"
        R"(<rect id="c" width="10" height="10" style="stroke-width:8"/>)"
        R"(<image id="img" width="10" height="10"/>)"
        R"(</svg>)");
    ASSERT_TRUE(document);
    settle(*document);
    auto const before = serialize(*document);
    // Authored non-style snapshot of the untouched group and bitmap targets.
    auto const grp_attrs_before = authored_attributes(item(*document, "grp"));
    auto const img_attrs_before = authored_attributes(item(*document, "img"));

    std::vector<SPItem *> roots = {item(*document, "grp"), item(*document, "c"),
                                   item(*document, "a"), item(*document, "img")};
    auto const plan = SW::prepare_stroke_widths(*document, roots, absolute(10.0), 11);
    EXPECT_EQ(plan.state, SW::StrokeWidthPlanState::Prepared);
    EXPECT_EQ(plan.planned_changes, 3u);
    EXPECT_EQ(plan.unchanged, 0u);
    EXPECT_EQ(plan.excluded, 1u);
    EXPECT_EQ(plan.query.eligible, 3u);
    EXPECT_EQ(plan.query.incompatible, 1u);
    EXPECT_EQ(plan.query.covered, 1u); // duplicate "a" is covered, not re-planned
    auto const *a_member = find_member(plan, item(*document, "a"));
    ASSERT_TRUE(a_member);
    EXPECT_FALSE(a_member->target.runs[0].style.width_set); // inherited input

    auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
    ASSERT_TRUE(token);
    auto const result = SW::apply_stroke_widths(*document, plan, 11, *token);
    EXPECT_EQ(result.state, SW::StrokeWidthApplyState::Applied);
    EXPECT_EQ(result.reason, SW::StrokeWidthMemberReason::None);
    EXPECT_EQ(result.changed, 3u);
    EXPECT_EQ(result.unchanged, 0u);
    EXPECT_EQ(result.excluded, 1u);
    EXPECT_EQ(result.attempted_writes, 3u);
    EXPECT_EQ(result.attempted_writes, result.changed);

    EXPECT_DOUBLE_EQ(computed_width(*document, "a"), 10.0);
    EXPECT_DOUBLE_EQ(computed_width(*document, "b"), 10.0);
    EXPECT_DOUBLE_EQ(computed_width(*document, "c"), 10.0);
    EXPECT_TRUE(item(*document, "a")->style->stroke_width.set);
    ASSERT_NE(item(*document, "grp")->getRepr()->attribute("style"), nullptr);
    EXPECT_NE(std::string(item(*document, "grp")->getRepr()->attribute("style")).find("stroke-width:2"),
              std::string::npos);
    EXPECT_EQ(item(*document, "img")->getRepr()->attribute("style"), nullptr);
    // The group and bitmap keep their exact authored non-style metadata; the
    // bitmap geometry is present and unchanged.
    EXPECT_TRUE(authored_attributes(item(*document, "grp")) == grp_attrs_before);
    EXPECT_TRUE(authored_attributes(item(*document, "img")) == img_attrs_before);
    EXPECT_TRUE(std::any_of(img_attrs_before.begin(), img_attrs_before.end(),
                            [](auto const &a) { return a.first == "width" && a.second == "10"; }));

    EXPECT_TRUE(SW::stroke_widths_output_ready(*document, plan, 11, *token));
    EXPECT_TRUE(token->commitAtomically(Inkscape::Util::Internal::ContextString("Stroke width"), "", [&] {
        return SW::stroke_widths_output_ready(*document, plan, 11, *token);
    }));
    auto const applied = serialize(*document);

    ASSERT_TRUE(Inkscape::DocumentUndo::undo(document.get()));
    EXPECT_EQ(serialize(*document), before);
    // Exactly one history entry: a second Undo must find nothing and leave the
    // baseline intact.
    EXPECT_FALSE(Inkscape::DocumentUndo::undo(document.get()));
    EXPECT_EQ(serialize(*document), before);
    ASSERT_TRUE(Inkscape::DocumentUndo::redo(document.get()));
    EXPECT_EQ(serialize(*document), applied);
    EXPECT_DOUBLE_EQ(computed_width(*document, "a"), 10.0);
    EXPECT_DOUBLE_EQ(computed_width(*document, "c"), 10.0);

    // Buffer round trip only (parse of the saved XML buffer), not a filesystem
    // save/reopen claim.
    auto reopened = parse(applied);
    ASSERT_TRUE(reopened);
    EXPECT_DOUBLE_EQ(computed_width(*reopened, "a"), 10.0);
    EXPECT_DOUBLE_EQ(computed_width(*reopened, "b"), 10.0);
    EXPECT_DOUBLE_EQ(computed_width(*reopened, "c"), 10.0);
    token.reset();
}

// ---------------------------------------------------------------------------
// SW2B-2: a separate fresh relative plan, inherited-width materialization, and
// unrelated inline declarations (opacity, font-size, dash, marker, fill:none,
// vector-effect, inline fill !important versus stylesheet !important) preserved.
// ---------------------------------------------------------------------------

TEST_F(StrokeWidthControllerTest, SW2BApplyPreservesUnrelatedInheritedAndRelative)
{
    auto document = parse(
        R"(<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink">)"
        R"(<style>.pf { fill: blue !important; }</style>)"
        R"(<g id="grp" style="stroke-width:2"><rect id="inh" width="10" height="10"/></g>)"
        R"(<rect id="rel" width="10" height="10" style="stroke-width:2"/>)"
        R"(<rect id="pres" width="10" height="10" style="stroke-width:2;opacity:0.5;font-size:12px;stroke-dasharray:2 5;stroke-dashoffset:3;marker-end:url(#m);fill:none;vector-effect:non-scaling-stroke"/>)"
        R"(<rect id="imp" class="pf" width="10" height="10" style="stroke-width:2;fill:red !important"/>)"
        R"(</svg>)");
    ASSERT_TRUE(document);
    settle(*document);
    auto const before = serialize(*document);

    // Inherited width materializes inline on the leaf; the parent style stays.
    {
        auto const plan = SW::prepare_stroke_widths(*document, {item(*document, "inh")}, absolute(10.0), 20);
        ASSERT_EQ(plan.planned_changes, 1u);
        auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
        ASSERT_TRUE(token);
        auto const result = SW::apply_stroke_widths(*document, plan, 20, *token);
        EXPECT_EQ(result.state, SW::StrokeWidthApplyState::Applied);
        EXPECT_EQ(result.attempted_writes, 1u);
        EXPECT_EQ(result.attempted_writes, result.changed);
        EXPECT_TRUE(item(*document, "inh")->style->stroke_width.set);
        EXPECT_DOUBLE_EQ(computed_width(*document, "inh"), 10.0);
        ASSERT_NE(item(*document, "grp")->getRepr()->attribute("style"), nullptr);
        EXPECT_NE(std::string(item(*document, "grp")->getRepr()->attribute("style")).find("stroke-width:2"),
                  std::string::npos);
        token->rollback();
    }

    // A separate fresh relative plan doubles a different shape.
    {
        auto const plan = SW::prepare_stroke_widths(*document, {item(*document, "rel")}, relative(200.0), 21);
        auto const *member = find_member(plan, item(*document, "rel"));
        ASSERT_TRUE(member);
        ASSERT_TRUE(member->local_width.has_value());
        EXPECT_DOUBLE_EQ(*member->local_width, 4.0);
        auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
        ASSERT_TRUE(token);
        auto const result = SW::apply_stroke_widths(*document, plan, 21, *token);
        EXPECT_EQ(result.state, SW::StrokeWidthApplyState::Applied);
        EXPECT_EQ(result.attempted_writes, 1u);
        EXPECT_DOUBLE_EQ(computed_width(*document, "rel"), 4.0);
        token->rollback();
    }

    // Unrelated declarations survive a width change on a non-scaling shape.
    {
        auto const plan = SW::prepare_stroke_widths(*document, {item(*document, "pres")}, absolute(10.0), 22);
        auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
        ASSERT_TRUE(token);
        auto const result = SW::apply_stroke_widths(*document, plan, 22, *token);
        EXPECT_EQ(result.state, SW::StrokeWidthApplyState::Applied);
        EXPECT_EQ(result.attempted_writes, 1u);
        EXPECT_DOUBLE_EQ(computed_width(*document, "pres"), 10.0);
        auto *pres = item(*document, "pres");
        ASSERT_TRUE(pres->style);
        EXPECT_TRUE(pres->style->vector_effect.stroke);
        EXPECT_TRUE(pres->style->fill.isNone());
        auto const map = inline_style_map(pres);
        ASSERT_TRUE(map.count("opacity"));
        EXPECT_EQ(map.at("opacity"), "0.5");
        ASSERT_TRUE(map.count("font-size"));
        EXPECT_EQ(map.at("font-size"), "12px");
        ASSERT_TRUE(map.count("stroke-dasharray"));
        EXPECT_EQ(map.at("stroke-dasharray"), "2 5");
        ASSERT_TRUE(map.count("stroke-dashoffset"));
        EXPECT_EQ(map.at("stroke-dashoffset"), "3");
        ASSERT_TRUE(map.count("marker-end"));
        EXPECT_FALSE(map.at("marker-end").empty());
        ASSERT_TRUE(map.count("vector-effect"));
        EXPECT_EQ(map.at("vector-effect"), "non-scaling-stroke");
        token->rollback();
    }

    // Inline fill !important survives and still names red, not the stylesheet's
    // blue !important. The native SPStyle winner/priority/source is asserted, not
    // just the string map (mirrors object-style-test.cpp).
    {
        auto *imp = item(*document, "imp");
        ASSERT_TRUE(imp);
        ASSERT_TRUE(imp->style);
        // Initial native effective winner before the width write.
        ASSERT_EQ(imp->style->fill.get_value(), Glib::ustring("red"));
        ASSERT_TRUE(imp->style->fill.important);
        ASSERT_EQ(imp->style->fill.style_src, SPStyleSrc::STYLE_PROP);

        auto const plan = SW::prepare_stroke_widths(*document, {imp}, absolute(10.0), 23);
        auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
        ASSERT_TRUE(token);
        auto const result = SW::apply_stroke_widths(*document, plan, 23, *token);
        EXPECT_EQ(result.state, SW::StrokeWidthApplyState::Applied);
        EXPECT_EQ(result.attempted_writes, 1u);
        EXPECT_DOUBLE_EQ(computed_width(*document, "imp"), 10.0);
        auto const map = inline_style_map(imp);
        ASSERT_TRUE(map.count("fill"));
        EXPECT_EQ(map.at("fill"), "red !important");
        EXPECT_FALSE(imp->style->fill.isNone());
        // Final native winner/priority/source must still be the inline red.
        EXPECT_EQ(imp->style->fill.get_value(), Glib::ustring("red"));
        EXPECT_TRUE(imp->style->fill.important);
        EXPECT_EQ(imp->style->fill.style_src, SPStyleSrc::STYLE_PROP);
        token->rollback();
    }

    // A fresh mixed fixture: 2/2/8 plus an independent bitmap, relative 200
    // doubles each eligible member to 4/4/16 and excludes the bitmap.
    {
        auto mixed = parse(std::string{svg_open} +
            R"(<rect id="m1" width="10" height="10" style="stroke-width:2"/>)"
            R"(<rect id="m2" width="10" height="10" style="stroke-width:2"/>)"
            R"(<rect id="m3" width="10" height="10" style="stroke-width:8"/>)"
            R"(<image id="mimg" width="10" height="10"/>)"
            R"(</svg>)");
        ASSERT_TRUE(mixed);
        settle(*mixed);
        auto const mixed_plan = SW::prepare_stroke_widths(
            *mixed, {item(*mixed, "m1"), item(*mixed, "m2"), item(*mixed, "m3"), item(*mixed, "mimg")},
            relative(200.0), 24);
        EXPECT_EQ(mixed_plan.state, SW::StrokeWidthPlanState::Prepared);
        EXPECT_EQ(mixed_plan.planned_changes, 3u);
        EXPECT_EQ(mixed_plan.excluded, 1u);
        auto token = Inkscape::DocumentUndo::beginAtomicInteraction(mixed.get());
        ASSERT_TRUE(token);
        auto const result = SW::apply_stroke_widths(*mixed, mixed_plan, 24, *token);
        EXPECT_EQ(result.state, SW::StrokeWidthApplyState::Applied);
        EXPECT_EQ(result.changed, 3u);
        EXPECT_EQ(result.excluded, 1u);
        EXPECT_EQ(result.attempted_writes, 3u);
        EXPECT_DOUBLE_EQ(computed_width(*mixed, "m1"), 4.0);
        EXPECT_DOUBLE_EQ(computed_width(*mixed, "m2"), 4.0);
        EXPECT_DOUBLE_EQ(computed_width(*mixed, "m3"), 16.0);
        EXPECT_EQ(item(*mixed, "mimg")->getRepr()->attribute("style"), nullptr);
        token->rollback();
    }

    EXPECT_EQ(serialize(*document), before);
}

// ---------------------------------------------------------------------------
// SW2B-3: scale(2), a 4:3 viewport and NonScaling native width outcomes, plus a
// zero width that keeps stroke paint and vector-effect.
// ---------------------------------------------------------------------------

TEST_F(StrokeWidthControllerTest, SW2BApplyScaleViewportNonScalingAndZeroPreserved)
{
    {
        auto document = parse(std::string{svg_open} +
            R"svg(<rect id="s2" width="10" height="10" style="stroke-width:2" transform="scale(2)"/>)svg"
            R"(</svg>)");
        ASSERT_TRUE(document);
        settle(*document);
        auto const plan = SW::prepare_stroke_widths(*document, {item(*document, "s2")}, absolute(1.0), 30);
        auto const *member = find_member(plan, item(*document, "s2"));
        ASSERT_TRUE(member);
        ASSERT_TRUE(member->local_width.has_value());
        double const local = *member->local_width;
        auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
        ASSERT_TRUE(token);
        auto const result = SW::apply_stroke_widths(*document, plan, 30, *token);
        EXPECT_EQ(result.state, SW::StrokeWidthApplyState::Applied);
        EXPECT_EQ(result.attempted_writes, 1u);
        EXPECT_DOUBLE_EQ(computed_width(*document, "s2"), local);
        EXPECT_NEAR(computed_width(*document, "s2") * item(*document, "s2")->i2doc_affine().descrim(), 1.0,
                    1e-12);
        EXPECT_STREQ(item(*document, "s2")->getRepr()->attribute("transform"), "scale(2)");
        token->rollback();
    }
    {
        auto document = parse(
            R"(<svg xmlns="http://www.w3.org/2000/svg" width="4" height="4" viewBox="0 0 3 3">)"
            R"(<rect id="v" width="10" height="10" style="stroke-width:2"/>)"
            R"(</svg>)");
        ASSERT_TRUE(document);
        settle(*document);
        auto const plan = SW::prepare_stroke_widths(*document, {item(*document, "v")}, absolute(1.0), 31);
        auto const *member = find_member(plan, item(*document, "v"));
        ASSERT_TRUE(member);
        ASSERT_TRUE(member->local_width.has_value());
        double const local = *member->local_width;
        auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
        ASSERT_TRUE(token);
        auto const result = SW::apply_stroke_widths(*document, plan, 31, *token);
        EXPECT_EQ(result.state, SW::StrokeWidthApplyState::Applied);
        EXPECT_EQ(result.attempted_writes, 1u);
        EXPECT_DOUBLE_EQ(computed_width(*document, "v"), local);
        EXPECT_NEAR(computed_width(*document, "v") * item(*document, "v")->i2doc_affine().descrim(), 1.0, 1e-12);
        token->rollback();
    }
    {
        auto document = parse(std::string{svg_open} +
            R"svg(<rect id="ns" width="10" height="10" style="stroke-width:2;vector-effect:non-scaling-stroke" transform="scale(2)"/>)svg"
            R"(</svg>)");
        ASSERT_TRUE(document);
        settle(*document);
        auto const plan = SW::prepare_stroke_widths(*document, {item(*document, "ns")}, absolute(1.0), 32);
        auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
        ASSERT_TRUE(token);
        auto const result = SW::apply_stroke_widths(*document, plan, 32, *token);
        EXPECT_EQ(result.state, SW::StrokeWidthApplyState::Applied);
        EXPECT_EQ(result.attempted_writes, 1u);
        EXPECT_DOUBLE_EQ(computed_width(*document, "ns"), 1.0);
        EXPECT_TRUE(item(*document, "ns")->style->vector_effect.stroke);
        EXPECT_DOUBLE_EQ(item(*document, "ns")->style->stroke_width.computed, 1.0); // effective == local
        token->rollback();
    }
    {
        auto document = parse(std::string{svg_open} +
            R"(<rect id="z" width="10" height="10" style="stroke-width:5;stroke:blue;fill:none;vector-effect:non-scaling-stroke"/>)"
            R"(</svg>)");
        ASSERT_TRUE(document);
        settle(*document);
        auto const plan = SW::prepare_stroke_widths(*document, {item(*document, "z")}, absolute(0.0), 33);
        auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
        ASSERT_TRUE(token);
        auto const result = SW::apply_stroke_widths(*document, plan, 33, *token);
        EXPECT_EQ(result.state, SW::StrokeWidthApplyState::Applied);
        EXPECT_EQ(result.attempted_writes, 1u);
        EXPECT_DOUBLE_EQ(computed_width(*document, "z"), 0.0);
        ASSERT_TRUE(item(*document, "z")->style);
        EXPECT_FALSE(item(*document, "z")->style->stroke.isNone());
        EXPECT_TRUE(item(*document, "z")->style->fill.isNone());
        EXPECT_TRUE(item(*document, "z")->style->vector_effect.stroke);
        token->rollback();
    }
}

// ---------------------------------------------------------------------------
// SW2B-4: native dash scaling, all-zero numeric CSS canonicalized to an empty
// computed array, old-zero/disabled untouched, an unrepresentable tiny nonzero
// pattern rejected as InvalidDash with no writes, and a representable exponent
// width/offset round-tripping exactly.
// ---------------------------------------------------------------------------

TEST_F(StrokeWidthControllerTest, SW2BApplyDashScalingCanonicalZeroAndTinyRejected)
{
    auto document = parse(std::string{svg_open} +
        R"(<rect id="d" width="10" height="10" style="stroke-width:2;stroke-dasharray:2 5;stroke-dashoffset:3"/>)"
        R"(<rect id="az" width="10" height="10" style="stroke-width:2;stroke-dasharray:2 5;stroke-dashoffset:3"/>)"
        R"(<rect id="tz" width="10" height="10" style="stroke-width:0;stroke-dasharray:2 5;stroke-dashoffset:3"/>)"
        R"(<rect id="tiny" width="10" height="10" style="stroke-width:2;stroke-dasharray:2 5"/>)"
        R"(<rect id="exp" width="10" height="10" style="stroke-width:2;stroke-dashoffset:3"/>)"
        R"(</svg>)");
    ASSERT_TRUE(document);
    settle(*document);
    auto const before = serialize(*document);

    {
        auto const plan = SW::prepare_stroke_widths(*document, {item(*document, "d")}, absolute(4.0, true), 40);
        auto const *member = find_member(plan, item(*document, "d"));
        ASSERT_TRUE(member);
        ASSERT_TRUE(member->local_dasharray.has_value());
        ASSERT_TRUE(member->native_dasharray_computed.has_value());
        auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
        ASSERT_TRUE(token);
        auto const result = SW::apply_stroke_widths(*document, plan, 40, *token);
        EXPECT_EQ(result.state, SW::StrokeWidthApplyState::Applied);
        EXPECT_EQ(result.attempted_writes, 1u);
        EXPECT_DOUBLE_EQ(computed_width(*document, "d"), 4.0);
        auto *d = item(*document, "d");
        auto const dash = d->style->stroke_dasharray.get_computed();
        ASSERT_EQ(dash.size(), member->native_dasharray_computed->size());
        for (std::size_t i = 0; i < dash.size(); ++i) {
            EXPECT_DOUBLE_EQ(dash[i], (*member->native_dasharray_computed)[i]);
        }
        EXPECT_DOUBLE_EQ(d->style->stroke_dashoffset.computed, 6.0);
        token->rollback();
    }
    {
        auto const plan = SW::prepare_stroke_widths(*document, {item(*document, "az")}, absolute(0.0, true), 41);
        auto const *member = find_member(plan, item(*document, "az"));
        ASSERT_TRUE(member);
        ASSERT_TRUE(member->local_dasharray.has_value());
        ASSERT_TRUE(member->native_dasharray_computed.has_value());
        EXPECT_TRUE(member->native_dasharray_computed->empty());
        auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
        ASSERT_TRUE(token);
        auto const result = SW::apply_stroke_widths(*document, plan, 41, *token);
        EXPECT_EQ(result.state, SW::StrokeWidthApplyState::Applied);
        EXPECT_EQ(result.attempted_writes, 1u);
        EXPECT_DOUBLE_EQ(computed_width(*document, "az"), 0.0);
        auto *az = item(*document, "az");
        EXPECT_TRUE(az->style->stroke_dasharray.get_computed().empty());
        EXPECT_DOUBLE_EQ(az->style->stroke_dashoffset.computed, 0.0);
        auto const map = inline_style_map(az);
        ASSERT_TRUE(map.count("stroke-dasharray"));
        EXPECT_NE(map.at("stroke-dasharray"), "none"); // authored zero pattern, never "none"
        token->rollback();
    }
    {
        auto const plan = SW::prepare_stroke_widths(*document, {item(*document, "tz")}, absolute(4.0, true), 42);
        auto const *member = find_member(plan, item(*document, "tz"));
        ASSERT_TRUE(member);
        EXPECT_FALSE(member->local_dasharray.has_value());
        EXPECT_FALSE(member->local_dashoffset.has_value());
        auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
        ASSERT_TRUE(token);
        auto const result = SW::apply_stroke_widths(*document, plan, 42, *token);
        EXPECT_EQ(result.state, SW::StrokeWidthApplyState::Applied);
        EXPECT_EQ(result.attempted_writes, 1u);
        EXPECT_DOUBLE_EQ(computed_width(*document, "tz"), 4.0);
        auto *tz = item(*document, "tz");
        auto const dash = tz->style->stroke_dasharray.get_computed();
        ASSERT_EQ(dash.size(), 2u);
        EXPECT_DOUBLE_EQ(dash[0], 2.0);
        EXPECT_DOUBLE_EQ(dash[1], 5.0);
        EXPECT_DOUBLE_EQ(tz->style->stroke_dashoffset.computed, 3.0);
        token->rollback();
    }
    {
        // scale_dashes == false leaves an authored dash/offset untouched.
        auto const plan = SW::prepare_stroke_widths(*document, {item(*document, "d")}, absolute(4.0, false), 42);
        auto const *member = find_member(plan, item(*document, "d"));
        ASSERT_TRUE(member);
        EXPECT_FALSE(member->local_dasharray.has_value());
        EXPECT_FALSE(member->local_dashoffset.has_value());
        auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
        ASSERT_TRUE(token);
        auto const result = SW::apply_stroke_widths(*document, plan, 42, *token);
        EXPECT_EQ(result.state, SW::StrokeWidthApplyState::Applied);
        EXPECT_EQ(result.attempted_writes, 1u);
        EXPECT_DOUBLE_EQ(computed_width(*document, "d"), 4.0);
        auto *d = item(*document, "d");
        auto const dash = d->style->stroke_dasharray.get_computed();
        ASSERT_EQ(dash.size(), 2u);
        EXPECT_DOUBLE_EQ(dash[0], 2.0);
        EXPECT_DOUBLE_EQ(dash[1], 5.0);
        EXPECT_DOUBLE_EQ(d->style->stroke_dashoffset.computed, 3.0);
        token->rollback();
    }
    {
        auto const plan = SW::prepare_stroke_widths(*document, {item(*document, "tiny")}, absolute(1e-12, true), 43);
        EXPECT_EQ(plan.planned_changes, 0u);
        EXPECT_EQ(plan.excluded, 1u);
        auto const *member = find_member(plan, item(*document, "tiny"));
        ASSERT_TRUE(member);
        EXPECT_EQ(member->reason, SW::StrokeWidthMemberReason::InvalidDash);
        auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
        ASSERT_TRUE(token);
        auto const result = SW::apply_stroke_widths(*document, plan, 43, *token);
        EXPECT_EQ(result.state, SW::StrokeWidthApplyState::Unchanged);
        EXPECT_EQ(result.changed, 0u);
        EXPECT_EQ(result.excluded, 1u);
        EXPECT_EQ(result.attempted_writes, 0u);
        EXPECT_EQ(serialize(*document), before);
        token->rollback();
    }
    {
        auto const plan = SW::prepare_stroke_widths(*document, {item(*document, "exp")}, absolute(1e-7, true), 44);
        auto const *member = find_member(plan, item(*document, "exp"));
        ASSERT_TRUE(member);
        ASSERT_TRUE(member->local_width.has_value());
        ASSERT_TRUE(member->local_dashoffset.has_value());
        EXPECT_DOUBLE_EQ(*member->local_width, 1e-7);
        auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
        ASSERT_TRUE(token);
        auto const result = SW::apply_stroke_widths(*document, plan, 44, *token);
        EXPECT_EQ(result.state, SW::StrokeWidthApplyState::Applied);
        EXPECT_EQ(result.attempted_writes, 1u);
        EXPECT_DOUBLE_EQ(computed_width(*document, "exp"), *member->local_width);
        EXPECT_DOUBLE_EQ(item(*document, "exp")->style->stroke_dashoffset.computed, *member->local_dashoffset);
        EXPECT_NEAR(item(*document, "exp")->style->stroke_dashoffset.computed, 1.5e-7, 1e-21);
        token->rollback();
    }
    EXPECT_EQ(serialize(*document), before);
}

// ---------------------------------------------------------------------------
// SW2B-5: a no-op keeps XML/history exactly, a seeded Redo survives, the
// no-log atomic commit reports no publication, and an unsupported bitmap plan
// reports an honest exclusion without writes.
// ---------------------------------------------------------------------------

TEST_F(StrokeWidthControllerTest, SW2BApplyNoOpSeededRedoAndExclusions)
{
    auto document = parse(std::string{svg_open} +
        R"(<rect id="same" width="10" height="10" style="stroke-width:2"/>)"
        R"(<image id="img" width="10" height="10"/>)"
        R"(</svg>)");
    ASSERT_TRUE(document);
    settle(*document);

    item(*document, "same")->setLocked(true);
    Inkscape::DocumentUndo::done(document.get(), Inkscape::Util::Internal::ContextString("Seed"), "");
    ASSERT_TRUE(Inkscape::DocumentUndo::undo(document.get()));
    auto const before = serialize(*document);

    auto const plan = SW::prepare_stroke_widths(*document, {item(*document, "same")}, absolute(2.0), 50);
    ASSERT_EQ(plan.planned_changes, 0u);
    ASSERT_EQ(plan.unchanged, 1u);
    auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
    ASSERT_TRUE(token);
    auto const result = SW::apply_stroke_widths(*document, plan, 50, *token);
    EXPECT_EQ(result.state, SW::StrokeWidthApplyState::Unchanged);
    EXPECT_EQ(result.changed, 0u);
    EXPECT_EQ(result.unchanged, 1u);
    EXPECT_EQ(result.attempted_writes, 0u);
    EXPECT_EQ(serialize(*document), before);
    EXPECT_TRUE(SW::stroke_widths_output_ready(*document, plan, 50, *token));
    EXPECT_FALSE(token->commitAtomically(Inkscape::Util::Internal::ContextString("No-op"), "", [&] {
        return SW::stroke_widths_output_ready(*document, plan, 50, *token);
    }));
    EXPECT_FALSE(token->active());
    token.reset();
    ASSERT_TRUE(Inkscape::DocumentUndo::redo(document.get()));
    EXPECT_TRUE(item(*document, "same")->isLocked());
    auto const after_redo = serialize(*document);

    auto const bitmap_plan = SW::prepare_stroke_widths(*document, {item(*document, "img")}, absolute(10.0), 51);
    EXPECT_EQ(bitmap_plan.planned_changes, 0u);
    EXPECT_EQ(bitmap_plan.excluded, 1u);
    auto bitmap_token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
    ASSERT_TRUE(bitmap_token);
    auto const bitmap_result = SW::apply_stroke_widths(*document, bitmap_plan, 51, *bitmap_token);
    EXPECT_EQ(bitmap_result.state, SW::StrokeWidthApplyState::Unchanged);
    EXPECT_EQ(bitmap_result.changed, 0u);
    EXPECT_EQ(bitmap_result.excluded, 1u);
    EXPECT_EQ(bitmap_result.attempted_writes, 0u);
    EXPECT_EQ(serialize(*document), after_redo);
    bitmap_token->rollback();
}

// ---------------------------------------------------------------------------
// SW2B-6: invalid/foreign/moved-from/retired tokens and stale generation/style/
// transform/removed-owner plans are rejected without writes; only the caller's
// own valid token rolls back.
// ---------------------------------------------------------------------------

TEST_F(StrokeWidthControllerTest, SW2BApplyTokenAndStaleRejections)
{
    auto document = parse(std::string{svg_open} +
        R"(<rect id="a" width="10" height="10" style="stroke-width:2"/>)"
        R"svg(<rect id="b" width="10" height="10" style="stroke-width:2" transform="scale(3)"/>)svg"
        R"(</svg>)");
    ASSERT_TRUE(document);
    settle(*document);
    auto const before = serialize(*document);

    auto const plan = SW::prepare_stroke_widths(*document, {item(*document, "a")}, absolute(10.0), 60);
    ASSERT_EQ(plan.state, SW::StrokeWidthPlanState::Prepared);

    // Foreign token: identity check refuses before any weak/live access.
    {
        auto other = parse(std::string{svg_open} +
            R"(<rect id="x" width="10" height="10"/>)"
            R"(</svg>)");
        ASSERT_TRUE(other);
        settle(*other);
        auto other_token = Inkscape::DocumentUndo::beginAtomicInteraction(other.get());
        ASSERT_TRUE(other_token);
        auto const result = SW::apply_stroke_widths(*document, plan, 60, *other_token);
        EXPECT_EQ(result.state, SW::StrokeWidthApplyState::Rejected);
        EXPECT_EQ(result.reason, SW::StrokeWidthMemberReason::InvalidTransaction);
        EXPECT_EQ(result.attempted_writes, 0u);
        EXPECT_EQ(serialize(*document), before);
        other_token->rollback();
    }

    // Moved-from token.
    {
        auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
        ASSERT_TRUE(token);
        auto moved = std::move(*token);
        auto const result = SW::apply_stroke_widths(*document, plan, 60, *token);
        EXPECT_EQ(result.state, SW::StrokeWidthApplyState::Rejected);
        EXPECT_EQ(result.reason, SW::StrokeWidthMemberReason::InvalidTransaction);
        EXPECT_EQ(result.attempted_writes, 0u);
        EXPECT_EQ(serialize(*document), before);
        moved.rollback();
    }

    // Retired (already rolled-back) token.
    {
        auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
        ASSERT_TRUE(token);
        token->rollback();
        auto const result = SW::apply_stroke_widths(*document, plan, 60, *token);
        EXPECT_EQ(result.state, SW::StrokeWidthApplyState::Rejected);
        EXPECT_EQ(result.reason, SW::StrokeWidthMemberReason::InvalidTransaction);
        EXPECT_EQ(result.attempted_writes, 0u);
    }

    // Stale opaque scope generation.
    {
        auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
        ASSERT_TRUE(token);
        auto const result = SW::apply_stroke_widths(*document, plan, 999, *token);
        EXPECT_EQ(result.state, SW::StrokeWidthApplyState::Rejected);
        EXPECT_EQ(result.reason, SW::StrokeWidthMemberReason::StalePlan);
        EXPECT_EQ(result.attempted_writes, 0u);
        EXPECT_EQ(serialize(*document), before);
        token->rollback();
    }

    // Stale style: prepared before an in-token mutation.
    {
        auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
        ASSERT_TRUE(token);
        auto const stale_plan = SW::prepare_stroke_widths(*document, {item(*document, "a")}, absolute(10.0), 61);
        item(*document, "a")->setAttribute("style", "stroke-width:3");
        auto const stale_before = serialize(*document);
        auto const result = SW::apply_stroke_widths(*document, stale_plan, 61, *token);
        EXPECT_EQ(result.state, SW::StrokeWidthApplyState::Rejected);
        EXPECT_EQ(result.reason, SW::StrokeWidthMemberReason::StalePlan);
        EXPECT_EQ(result.attempted_writes, 0u);
        EXPECT_EQ(serialize(*document), stale_before);
        token->rollback();
    }

    // Stale transform.
    {
        auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
        ASSERT_TRUE(token);
        auto const stale_plan = SW::prepare_stroke_widths(*document, {item(*document, "b")}, absolute(10.0), 62);
        item(*document, "b")->setAttribute("transform", "scale(4)");
        auto const stale_before = serialize(*document);
        auto const result = SW::apply_stroke_widths(*document, stale_plan, 62, *token);
        EXPECT_EQ(result.state, SW::StrokeWidthApplyState::Rejected);
        EXPECT_EQ(result.reason, SW::StrokeWidthMemberReason::StalePlan);
        EXPECT_EQ(result.attempted_writes, 0u);
        EXPECT_EQ(serialize(*document), stale_before);
        token->rollback();
    }

    // Removed owner binding.
    {
        auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
        ASSERT_TRUE(token);
        auto const stale_plan = SW::prepare_stroke_widths(*document, {item(*document, "a")}, absolute(10.0), 63);
        auto *repr = item(*document, "a")->getRepr();
        repr->parent()->removeChild(repr);
        auto const stale_before = serialize(*document);
        auto const result = SW::apply_stroke_widths(*document, stale_plan, 63, *token);
        EXPECT_EQ(result.state, SW::StrokeWidthApplyState::Rejected);
        EXPECT_EQ(result.reason, SW::StrokeWidthMemberReason::StalePlan);
        EXPECT_EQ(result.attempted_writes, 0u);
        EXPECT_EQ(serialize(*document), stale_before);
        token->rollback();
        EXPECT_EQ(serialize(*document), before);
    }
}

// ---------------------------------------------------------------------------
// SW2B-7: a real native XML observer removes the second target's transform when
// the first target's style is written. The writer must fail after the first
// native write; the caller's stable rollback restores exact XML and a seeded
// Redo, with no controller test hook.
// ---------------------------------------------------------------------------

TEST_F(StrokeWidthControllerTest, SW2BApplyObserverMutationFailsAndRollsBack)
{
    auto document = parse(std::string{svg_open} +
        R"(<rect id="a" width="10" height="10" style="stroke-width:2"/>)"
        R"svg(<rect id="b" width="10" height="10" style="stroke-width:2" transform="scale(3)"/>)svg"
        R"(</svg>)");
    ASSERT_TRUE(document);
    settle(*document);

    item(*document, "b")->setLocked(true);
    Inkscape::DocumentUndo::done(document.get(), Inkscape::Util::Internal::ContextString("Seed"), "");
    ASSERT_TRUE(Inkscape::DocumentUndo::undo(document.get()));
    auto const before = serialize(*document);

    auto const plan = SW::prepare_stroke_widths(*document, {item(*document, "a"), item(*document, "b")},
                                               absolute(10.0), 70);
    ASSERT_EQ(plan.planned_changes, 2u);

    auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
    ASSERT_TRUE(token);
    SecondTargetTransformObserver observer(*item(*document, "a")->getRepr(),
                                           item(*document, "b")->getRepr());
    auto const result = SW::apply_stroke_widths(*document, plan, 70, *token);

    EXPECT_TRUE(observer.fired());
    EXPECT_EQ(result.state, SW::StrokeWidthApplyState::Failed);
    EXPECT_EQ(result.changed, 0u); // no verified change is claimed on Failed
    EXPECT_EQ(result.attempted_writes, 1u); // one native changeCSS was issued
    // The first target really changed before caller rollback.
    ASSERT_NE(item(*document, "a")->getRepr()->attribute("style"), nullptr);
    EXPECT_NE(std::string(item(*document, "a")->getRepr()->attribute("style")).find("stroke-width:10"),
              std::string::npos);
    // The native observer really removed the second target's transform.
    EXPECT_TRUE(item(*document, "b")->getRepr()->attribute("transform") == nullptr);

    observer.detach();
    token->rollback();
    EXPECT_EQ(serialize(*document), before);
    EXPECT_TRUE(item(*document, "b")->getRepr()->attribute("transform") != nullptr);
    ASSERT_TRUE(Inkscape::DocumentUndo::redo(document.get()));
    EXPECT_TRUE(item(*document, "b")->isLocked());
}

// ---------------------------------------------------------------------------
// SW2B-8 (owner decision 2026-09-28): a selected source plus its dependent
// clone changes the source only, in one commit; the clone is not written and
// follows its original.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// SW2B-9: clones reached only through selected locked groups. The note reports
// both the following clone and the skipped group; swapping which locked clone
// follows the changed source before commit (same count) fails the output check.
// A broken reference in a graph that also holds the changed source rejects.
// ---------------------------------------------------------------------------
TEST_F(StrokeWidthControllerTest, SW2BFollowingClonesInLockedGroupsAreCheckedByIdentity)
{
    auto document = parse(std::string{svg_open} +
        R"(<rect id="src" width="10" height="10" style="stroke-width:2"/>)"
        R"(<rect id="other" width="10" height="10" style="stroke-width:3"/>)"
        R"(<g id="lockeda" sodipodi:insensitive="true"><use id="ua" xlink:href="#src" x="20"/></g>)"
        R"(<g id="lockedb" sodipodi:insensitive="true"><use id="ub" xlink:href="#other" x="40"/></g>)"
        R"(</svg>)");
    ASSERT_TRUE(document);
    settle(*document);
    auto const before = serialize(*document);

    auto const one_group = SW::prepare_stroke_widths(
        *document, {item(*document, "src"), item(*document, "lockeda")}, absolute(10.0), 81);
    ASSERT_EQ(one_group.state, SW::StrokeWidthPlanState::Prepared);
    EXPECT_EQ(one_group.following_clones, 1u);
    EXPECT_EQ(SW::stroke_width_applied_note(one_group, one_group.excluded),
              "Stroke width applied; 1 linked clone follows its original; incompatible or protected items were skipped")
        << "the locked group is still reported as skipped";

    auto const plan = SW::prepare_stroke_widths(
        *document, {item(*document, "src"), item(*document, "lockeda"), item(*document, "lockedb")},
        absolute(10.0), 82);
    ASSERT_EQ(plan.state, SW::StrokeWidthPlanState::Prepared);
    ASSERT_EQ(plan.following_clones, 1u);

    // Swap between prepare and apply: the pre-write check refuses, no write.
    {
        auto stale_token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
        ASSERT_TRUE(stale_token);
        item(*document, "ua")->setAttribute("xlink:href", "#other");
        item(*document, "ub")->setAttribute("xlink:href", "#src");
        auto const stale = SW::apply_stroke_widths(*document, plan, 82, *stale_token);
        EXPECT_EQ(stale.state, SW::StrokeWidthApplyState::Rejected);
        EXPECT_EQ(stale.reason, SW::StrokeWidthMemberReason::StalePlan);
        EXPECT_EQ(stale.attempted_writes, 0u);
        EXPECT_DOUBLE_EQ(computed_width(*document, "src"), 2.0);
        stale_token->rollback();
        ASSERT_EQ(serialize(*document), before) << "rollback restores the references";
    }

    auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
    ASSERT_TRUE(token);
    auto const result = SW::apply_stroke_widths(*document, plan, 82, *token);
    ASSERT_EQ(result.state, SW::StrokeWidthApplyState::Applied);
    ASSERT_TRUE(SW::stroke_widths_output_ready(*document, plan, 82, *token));

    // Swap: ua leaves the source, ub joins it. Still one following clone.
    item(*document, "ua")->setAttribute("xlink:href", "#other");
    item(*document, "ub")->setAttribute("xlink:href", "#src");
    EXPECT_FALSE(SW::stroke_widths_output_ready(*document, plan, 82, *token));
    token->rollback();
    EXPECT_EQ(serialize(*document), before);
}

TEST_F(StrokeWidthControllerTest, SW2BSourceAndItsCloneNoteSkipsNothing)
{
    // The usual case: the source sets its own width, so its clone is a query
    // exclusion (CloneSourceOverrides) that follows the source; nothing else
    // is skipped.
    auto document = parse(std::string{svg_open} +
        R"(<rect id="src" width="10" height="10" style="stroke-width:2"/>)"
        R"(<use id="clone" xlink:href="#src" x="20"/>)"
        R"(</svg>)");
    ASSERT_TRUE(document);
    auto const plan = SW::prepare_stroke_widths(*document, {item(*document, "src"), item(*document, "clone")},
                                                absolute(10.0), 84);
    ASSERT_EQ(plan.state, SW::StrokeWidthPlanState::Prepared);
    ASSERT_EQ(plan.following_clones, 1u);
    EXPECT_EQ(SW::stroke_width_applied_note(plan, plan.excluded),
              "Stroke width applied; 1 linked clone follows its original");
}

TEST_F(StrokeWidthControllerTest, SW2ABrokenReferenceBesideTheChangedSourceRejects)
{
    auto document = parse(std::string{svg_open} +
        R"(<g id="g"><rect id="src" width="10" height="10" style="stroke-width:2"/>)"
        R"(<use id="broken" xlink:href="#nope"/></g>)"
        R"(<use id="c" xlink:href="#g" x="20"/>)"
        R"(</svg>)");
    ASSERT_TRUE(document);
    auto *src = item(*document, "src");
    auto *clone = item(*document, "c");
    for (auto const &roots : {std::vector<SPItem *>{src, clone}, std::vector<SPItem *>{clone, src}}) {
        auto const plan = SW::prepare_stroke_widths(*document, roots, absolute(10.0), 83);
        EXPECT_EQ(plan.state, SW::StrokeWidthPlanState::Rejected);
        EXPECT_EQ(plan.rejection, SW::StrokeWidthMemberReason::DependentCloneUncertain);
        EXPECT_EQ(plan.dependent_target.get(), clone);
        EXPECT_EQ(plan.following_clones, 0u);
    }
}

TEST_F(StrokeWidthControllerTest, SW2BApplyWithDependentCloneChangesTheSourceOnly)
{
    auto document = parse(std::string{svg_open} +
        R"(<rect id="src" width="10" height="10" style="stroke-width:2"/>)"
        R"(<rect id="other" width="10" height="10" style="stroke-width:3"/>)"
        R"(<use id="clone" xlink:href="#src" x="20"/>)"
        R"(<use id="clone2" xlink:href="#other" x="40"/>)"
        R"(</svg>)");
    ASSERT_TRUE(document);
    settle(*document);
    auto const before = serialize(*document);
    auto const clone_before = std::string(item(*document, "clone")->getRepr()->attribute("style") ?: "");

    auto const plan = SW::prepare_stroke_widths(
        *document, {item(*document, "src"), item(*document, "clone"), item(*document, "clone2")},
        absolute(10.0), 80);
    ASSERT_EQ(plan.state, SW::StrokeWidthPlanState::Prepared);
    ASSERT_EQ(plan.following_clones, 1u);
    ASSERT_EQ(plan.planned_changes, 1u);

    auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
    ASSERT_TRUE(token);
    auto const result = SW::apply_stroke_widths(*document, plan, 80, *token);
    ASSERT_EQ(result.state, SW::StrokeWidthApplyState::Applied);
    EXPECT_EQ(result.changed, 1u);
    EXPECT_EQ(result.excluded, 2u);
    EXPECT_DOUBLE_EQ(computed_width(*document, "src"), 10.0);
    EXPECT_EQ(std::string(item(*document, "clone")->getRepr()->attribute("style") ?: ""), clone_before)
        << "the clone itself is never written";
    EXPECT_TRUE(SW::stroke_widths_output_ready(*document, plan, 80, *token));
    // §2.2 reports the verified changed source's none paint before clones/skips.
    EXPECT_EQ(result.paint_none, 1u);
    EXPECT_EQ(result.skipped_runs, 0u);
    EXPECT_EQ(SW::stroke_width_applied_note(plan, result.excluded),
              "Stroke width applied; 1 object has no stroke colour; 1 linked clone follows its original; incompatible or protected items were skipped");

    EXPECT_TRUE(token->commitAtomically(Inkscape::Util::Internal::ContextString("Stroke width"), "", [&] {
        return SW::stroke_widths_output_ready(*document, plan, 80, *token);
    }));
    ASSERT_TRUE(Inkscape::DocumentUndo::undo(document.get()));
    EXPECT_EQ(serialize(*document), before) << "one Undo step restores everything";
}

// ---------------------------------------------------------------------------
// SW2B-CORR1: the no-op return happens only AFTER the read-only reprepare, so a
// stale or tampered unchanged/excluded plan is rejected without any write while a
// genuine no-op stays Unchanged.
// ---------------------------------------------------------------------------

TEST_F(StrokeWidthControllerTest, SW2BApplyStaleNoOpAndTamperedCounterRejected)
{
    auto document = parse(std::string{svg_open} +
        R"(<rect id="same" width="10" height="10" style="stroke-width:2"/>)"
        R"(<image id="img" width="10" height="10"/>)"
        R"(</svg>)");
    ASSERT_TRUE(document);
    settle(*document);
    auto const before = serialize(*document);

    auto const plan = SW::prepare_stroke_widths(*document, {item(*document, "same")}, absolute(2.0), 90);
    ASSERT_EQ(plan.planned_changes, 0u);
    ASSERT_EQ(plan.unchanged, 1u);

    // Stale no-op: the inline width changes inside the caller token after prepare.
    {
        auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
        ASSERT_TRUE(token);
        item(*document, "same")->setAttribute("style", "stroke-width:3");
        auto const mutated = serialize(*document);
        auto const result = SW::apply_stroke_widths(*document, plan, 90, *token);
        EXPECT_EQ(result.state, SW::StrokeWidthApplyState::Rejected);
        EXPECT_EQ(result.reason, SW::StrokeWidthMemberReason::StalePlan);
        EXPECT_EQ(result.attempted_writes, 0u);
        EXPECT_EQ(serialize(*document), mutated); // apply itself wrote nothing
        token->rollback();
        EXPECT_EQ(serialize(*document), before); // caller rollback restores original
    }

    // Tampered no-op counter: a copy claiming one planned change must be rejected
    // by the read-only reprepare before the no-op return.
    {
        auto tampered = plan;
        tampered.planned_changes = 1;
        auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
        ASSERT_TRUE(token);
        auto const result = SW::apply_stroke_widths(*document, tampered, 90, *token);
        EXPECT_EQ(result.state, SW::StrokeWidthApplyState::Rejected);
        EXPECT_EQ(result.reason, SW::StrokeWidthMemberReason::StalePlan);
        EXPECT_EQ(result.attempted_writes, 0u);
        EXPECT_EQ(serialize(*document), before);
        token->rollback();
    }

    // Tampered excluded counter on an excluded-only plan is likewise stale.
    {
        auto const bitmap_plan =
            SW::prepare_stroke_widths(*document, {item(*document, "img")}, absolute(4.0), 91);
        ASSERT_EQ(bitmap_plan.planned_changes, 0u);
        ASSERT_EQ(bitmap_plan.excluded, 1u);
        auto tampered = bitmap_plan;
        tampered.excluded = 0;
        auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
        ASSERT_TRUE(token);
        auto const result = SW::apply_stroke_widths(*document, tampered, 91, *token);
        EXPECT_EQ(result.state, SW::StrokeWidthApplyState::Rejected);
        EXPECT_EQ(result.reason, SW::StrokeWidthMemberReason::StalePlan);
        EXPECT_EQ(result.attempted_writes, 0u);
        EXPECT_EQ(serialize(*document), before);
        token->rollback();
    }

    // A validated genuine no-op stays Unchanged with zero writes.
    {
        auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
        ASSERT_TRUE(token);
        auto const result = SW::apply_stroke_widths(*document, plan, 90, *token);
        EXPECT_EQ(result.state, SW::StrokeWidthApplyState::Unchanged);
        EXPECT_EQ(result.changed, 0u);
        EXPECT_EQ(result.attempted_writes, 0u);
        EXPECT_EQ(serialize(*document), before);
        token->rollback();
    }
}

// ---------------------------------------------------------------------------
// SW2B-CORR2: a first-write callback that edits the second member's inline width
// is caught before the second write, and a callback that changes an unchanged
// selected member's ancestor width makes the intended-output check fail. Both use
// a real generic single-fire XML observer and caller rollback restores exact XML.
// ---------------------------------------------------------------------------

TEST_F(StrokeWidthControllerTest, SW2BApplyCallbackFrozenStyleAndScopeFails)
{
    // Subcase A: the callback edits the second target's inline width.
    {
        auto document = parse(std::string{svg_open} +
            R"(<rect id="a" width="10" height="10" style="stroke-width:2"/>)"
            R"(<rect id="b" width="10" height="10" style="stroke-width:2"/>)"
            R"(</svg>)");
        ASSERT_TRUE(document);
        settle(*document);
        auto const before = serialize(*document);

        auto const plan = SW::prepare_stroke_widths(*document, {item(*document, "a"), item(*document, "b")},
                                                   absolute(10.0), 92);
        ASSERT_EQ(plan.planned_changes, 2u);

        auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
        ASSERT_TRUE(token);
        SingleFireStyleObserver observer(*item(*document, "a")->getRepr(), [&] {
            item(*document, "b")->setAttribute("style", "stroke-width:7");
        });
        auto const result = SW::apply_stroke_widths(*document, plan, 92, *token);

        EXPECT_TRUE(observer.fired());
        EXPECT_EQ(result.state, SW::StrokeWidthApplyState::Failed);
        EXPECT_EQ(result.changed, 0u);
        EXPECT_EQ(result.attempted_writes, 1u);
        // The first target was written, the second was not overwritten.
        ASSERT_NE(item(*document, "a")->getRepr()->attribute("style"), nullptr);
        EXPECT_NE(std::string(item(*document, "a")->getRepr()->attribute("style")).find("stroke-width:10"),
                  std::string::npos);
        auto const b_map = inline_style_map(item(*document, "b"));
        ASSERT_TRUE(b_map.count("stroke-width"));
        EXPECT_EQ(b_map.at("stroke-width"), "7");

        observer.detach();
        token->rollback();
        EXPECT_EQ(serialize(*document), before);
        EXPECT_DOUBLE_EQ(computed_width(*document, "b"), 2.0);
    }

    // Subcase B: the callback changes an unchanged member's ancestor width without
    // touching the leaf inline XML; the intended-output check must fail.
    {
        auto document = parse(std::string{svg_open} +
            R"(<g id="grp" style="stroke-width:2">)"
            R"(<rect id="leaf" width="10" height="10"/>)"
            R"(</g>)"
            R"(<rect id="chg" width="10" height="10" style="stroke-width:5"/>)"
            R"(</svg>)");
        ASSERT_TRUE(document);
        settle(*document);
        auto const before = serialize(*document);

        auto const plan = SW::prepare_stroke_widths(*document, {item(*document, "grp"), item(*document, "chg")},
                                                   absolute(2.0), 93);
        ASSERT_EQ(plan.planned_changes, 1u);
        ASSERT_EQ(plan.unchanged, 1u);

        auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
        ASSERT_TRUE(token);
        SingleFireStyleObserver observer(*item(*document, "chg")->getRepr(), [&] {
            item(*document, "grp")->setAttribute("style", "stroke-width:7");
        });
        auto const result = SW::apply_stroke_widths(*document, plan, 93, *token);

        EXPECT_TRUE(observer.fired());
        EXPECT_EQ(result.state, SW::StrokeWidthApplyState::Failed);
        EXPECT_EQ(result.changed, 0u);
        EXPECT_EQ(result.attempted_writes, 1u);
        // The leaf kept its own inline XML (absent); only its inherited effective
        // width moved.
        auto const *leaf_member = find_member(plan, item(*document, "leaf"));
        ASSERT_TRUE(leaf_member);
        EXPECT_FALSE(leaf_member->inline_style.has_value());
        EXPECT_EQ(item(*document, "leaf")->getRepr()->attribute("style"), nullptr);
        EXPECT_EQ(computed_width(*document, "leaf"), 7.0);
        EXPECT_FALSE(SW::stroke_widths_output_ready(*document, plan, 93, *token));

        observer.detach();
        token->rollback();
        EXPECT_EQ(serialize(*document), before);
        EXPECT_DOUBLE_EQ(computed_width(*document, "leaf"), 2.0);
    }

    // Subcase C: the callback locks the second member's ancestor without touching
    // the second leaf's authored XML, parent, affine or inline style. That leaf
    // snapshot is unchanged, but a callback can newly protect an ancestor; the
    // pre-write native re-resolution must refuse to write it transiently.
    {
        auto document = parse(std::string{svg_open} +
            R"(<rect id="a" width="10" height="10" style="stroke-width:2"/>)"
            R"(<g id="grp">)"
            R"(<rect id="b" width="10" height="10" style="stroke-width:2"/>)"
            R"(</g>)"
            R"(</svg>)");
        ASSERT_TRUE(document);
        settle(*document);
        auto const before = serialize(*document);
        double const b_before = computed_width(*document, "b");
        auto const b_style_before = inline_style_map(item(*document, "b"));
        // Copy the authored raw style now; keeping only the pointer across the
        // callback/rollback would compare through an unstable attribute lifetime.
        char const *b_style_attr_before = item(*document, "b")->getRepr()->attribute("style");
        ASSERT_NE(b_style_attr_before, nullptr);
        std::string const b_style_raw_before{b_style_attr_before};
        ASSERT_TRUE(item(*document, "grp")->isSensitive());

        auto const plan = SW::prepare_stroke_widths(
            *document, {item(*document, "a"), item(*document, "grp")}, absolute(10.0), 95);
        ASSERT_EQ(plan.planned_changes, 2u);

        auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
        ASSERT_TRUE(token);
        SingleFireStyleObserver observer(*item(*document, "a")->getRepr(), [&] {
            item(*document, "grp")->setAttribute("sodipodi:insensitive", "true");
        });
        auto const result = SW::apply_stroke_widths(*document, plan, 95, *token);

        EXPECT_TRUE(observer.fired());
        EXPECT_EQ(result.state, SW::StrokeWidthApplyState::Failed);
        EXPECT_EQ(result.changed, 0u);
        EXPECT_EQ(result.attempted_writes, 1u);
        // The second child was never written: computed width and inline style are
        // exactly as prepared.
        EXPECT_DOUBLE_EQ(computed_width(*document, "b"), b_before);
        EXPECT_EQ(inline_style_map(item(*document, "b")), b_style_before);
        // Presence and exact authored bytes must be unchanged from the baseline.
        EXPECT_STREQ(item(*document, "b")->getRepr()->attribute("style"), b_style_raw_before.c_str());
        EXPECT_FALSE(SW::stroke_widths_output_ready(*document, plan, 95, *token));

        observer.detach();
        token->rollback();
        EXPECT_EQ(serialize(*document), before);
        EXPECT_DOUBLE_EQ(computed_width(*document, "b"), b_before);
    }
}

// ---------------------------------------------------------------------------
// SW2B-CORR3: a post-write callback retargets a clone under an explicitly selected
// protected group to a changed shape. The read-only dependency guard re-run at
// intended-output must fail the action before any silent protected-instance change.
// ---------------------------------------------------------------------------

TEST_F(StrokeWidthControllerTest, SW2BApplyDependencyRetargetAfterWriteFails)
{
    auto document = parse(std::string{svg_open} +
        R"(<rect id="other" width="10" height="10" style="stroke-width:2"/>)"
        R"(<g id="prot" sodipodi:insensitive="true">)"
        R"(<use id="clone" xlink:href="#other" x="20"/>)"
        R"(</g>)"
        R"(<rect id="a" width="10" height="10" style="stroke-width:2"/>)"
        R"(</svg>)");
    ASSERT_TRUE(document);
    settle(*document);
    auto const before = serialize(*document);

    auto *clone_use = cast<SPUse>(item(*document, "clone"));
    ASSERT_TRUE(clone_use);
    ASSERT_EQ(clone_use->get_original(), item(*document, "other"));

    auto const plan = SW::prepare_stroke_widths(*document, {item(*document, "prot"), item(*document, "a")},
                                                absolute(10.0), 94);
    ASSERT_EQ(plan.state, SW::StrokeWidthPlanState::Prepared);
    ASSERT_EQ(plan.planned_changes, 1u);

    auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
    ASSERT_TRUE(token);
    SingleFireStyleObserver observer(*item(*document, "a")->getRepr(), [&] {
        item(*document, "clone")->setAttribute("xlink:href", "#a");
    });
    auto const result = SW::apply_stroke_widths(*document, plan, 94, *token);

    EXPECT_TRUE(observer.fired());
    EXPECT_EQ(result.state, SW::StrokeWidthApplyState::Failed);
    EXPECT_EQ(result.changed, 0u);
    EXPECT_EQ(result.attempted_writes, 1u);
    EXPECT_FALSE(SW::stroke_widths_output_ready(*document, plan, 94, *token));

    observer.detach();
    token->rollback();
    EXPECT_EQ(serialize(*document), before);
    EXPECT_STREQ(item(*document, "clone")->getRepr()->attribute("xlink:href"), "#other");
    auto *clone_after = cast<SPUse>(item(*document, "clone"));
    ASSERT_TRUE(clone_after);
    EXPECT_EQ(clone_after->get_original(), item(*document, "other"));
    EXPECT_DOUBLE_EQ(computed_width(*document, "a"), 2.0);
}

// ---------------------------------------------------------------------------
// SW3 NDIAG-1: existing native sp_te_apply_style ordinary scale-2 incoming
// document width "10" must give required local5/effective10 on all 4 chars.
// Predicted to fail; no runtime result observed yet. Native values are recorded,
// then the required contract asserted.
// ---------------------------------------------------------------------------

TEST_F(StrokeWidthControllerTest, SW3DiagnosticNativeOrdinaryScale2DocumentWidth)
{
    auto document = parse(std::string{svg_open} +
        R"svg(<text id="t" x="10" y="20" transform="scale(2)" style="font-family:Arial;font-size:20px;stroke:black;fill:#008000;stroke-width:2">ABCD</text>)svg"
        R"svg(</svg>)svg");
    ASSERT_TRUE(document);
    settle(*document);
    auto const before = serialize(*document);
    auto *text = item(*document, "t");
    ASSERT_TRUE(text);
    auto const owner_attrs_before = authored_attributes(text);

    ASSERT_NO_FATAL_FAILURE(assert_native_baseline(*document, text, false));
    ASSERT_EQ(native_char_count(text), 4u);
    std::vector<NativeCharIdentity> identity_before;
    for (unsigned i = 0; i < 4; ++i) identity_before.push_back(native_char_identity(text, i));

    auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
    ASSERT_TRUE(token);
    ASSERT_NO_FATAL_FAILURE(native_apply_width(*document, text, 0, 4, "10"));

    for (unsigned i = 0; i < 4; ++i) {
        auto const widths = native_char_widths(*document, text, i);
        expect_required_widths("ordinary_scale2", i, widths, 5.0, 10.0);
        EXPECT_EQ(widths.convention, SW::StrokeWidthConvention::Ordinary);
        EXPECT_EQ(native_char_identity(text, i), identity_before[i]);
    }
    EXPECT_TRUE(authored_attributes(text) == owner_attrs_before);

    token->rollback();
    EXPECT_EQ(serialize(*document), before);
}

// ---------------------------------------------------------------------------
// SW3 NDIAG-2: opt-in local stroke path on a non-scaling-stroke owner at scale 2.
// The caller prepares local width 10 with stroke_width=true and includes the
// actual run's frozen full native vector-effect value/priority, so the created
// span keeps the owner's non-inheriting convention. Required controller contract
// stays local10/effective10 (NS effective == local). The original default-path
// failure (actual local5/effective10/Ordinary) is retained in the r3 evidence;
// this qualifies the new opt-in capability, not the old default callers.
// ---------------------------------------------------------------------------

TEST_F(StrokeWidthControllerTest, SW3DiagnosticNativeNonScalingScale2ContractWidth)
{
    auto document = parse(std::string{svg_open} +
        R"svg(<text id="t" x="10" y="20" transform="scale(2)" style="font-family:Arial;font-size:20px;stroke:black;fill:#008000;stroke-width:2;vector-effect:non-scaling-stroke">ABCD</text>)svg"
        R"svg(</svg>)svg");
    ASSERT_TRUE(document);
    settle(*document);
    auto const before = serialize(*document);
    auto *text = item(*document, "t");
    ASSERT_TRUE(text);
    auto const owner_attrs_before = authored_attributes(text);

    ASSERT_NO_FATAL_FAILURE(assert_native_baseline(*document, text, true));
    ASSERT_EQ(native_char_count(text), 4u);
    std::vector<NativeCharIdentity> identity_before;
    for (unsigned i = 0; i < 4; ++i) identity_before.push_back(native_char_identity(text, i));

    auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
    ASSERT_TRUE(token);

    // Pre-write rendering-source oracle: each character's render style must come
    // from this fixture's <text> element, and the authored full vector-effect
    // value/priority must be non-scaling-stroke/false. A regression to the
    // SPString's own non-inherited style fails here, before any write.
    for (unsigned i = 0; i < 4; ++i) {
        auto const render = native_render_style_at(text, i);
        ASSERT_TRUE(render.source);
        ASSERT_EQ(render.element, static_cast<SPObject *>(text));
        ASSERT_TRUE(render.style);
        ASSERT_STREQ(render.style->vector_effect.get_value().c_str(), "non-scaling-stroke");
        ASSERT_FALSE(render.style->vector_effect.important);
    }

    // Freeze the actual run's full native vector-effect value and priority, then
    // include it verbatim in the native CSS patch. No hardcoded NS token.
    auto const frozen = frozen_vector_effect(text, 0, 4);
    ASSERT_STREQ(frozen.value.c_str(), "non-scaling-stroke");
    ASSERT_FALSE(frozen.important);
    auto patch = Util::delete_with<sp_repr_css_attr_unref>(sp_repr_css_attr_new());
    sp_repr_css_set_property(patch.get(), "stroke-width", "10");
    std::string const ve = frozen.important ? std::string(frozen.value.raw()) + " !important"
                                            : frozen.value.raw();
    sp_repr_css_set_property(patch.get(), "vector-effect", ve.c_str());
    TextStyleLocalStroke stroke_flags;
    stroke_flags.stroke_width = true;
    ASSERT_NO_FATAL_FAILURE(native_apply_local_stroke(*document, text, 0, 4, patch.get(), stroke_flags));

    for (unsigned i = 0; i < 4; ++i) {
        auto const widths = native_char_widths(*document, text, i);
        auto const stroke = native_char_stroke(text, i);
        expect_required_widths("non_scaling_scale2", i, widths, 10.0, 10.0);
        EXPECT_EQ(widths.convention, SW::StrokeWidthConvention::NonScaling);
        EXPECT_STREQ(stroke.vector_effect.c_str(), "non-scaling-stroke");
        EXPECT_FALSE(stroke.vector_effect_important);
        EXPECT_EQ(native_char_identity(text, i), identity_before[i]);
    }
    EXPECT_TRUE(authored_attributes(text) == owner_attrs_before);

    token->rollback();
    EXPECT_EQ(serialize(*document), before);
}

// ---------------------------------------------------------------------------
// SW3 NDIAG-3: opt-in local stroke path for the tiny width. The prepared local
// value 5e-21 is serialized with the existing locale-independent round-trip
// convention (numeric_text/g_ascii_dtostr) and restored verbatim, so it survives
// as positive local 5e-21/effective1e-20. The original default-path failure
// (CSSOStringStream %.16f rounded the write to 0) is retained in the r3
// evidence; this qualifies the new opt-in capability.
// ---------------------------------------------------------------------------

TEST_F(StrokeWidthControllerTest, SW3DiagnosticNativeTinyPositiveWidthContract)
{
    auto document = parse(std::string{svg_open} +
        R"svg(<text id="t" x="10" y="20" transform="scale(2)" style="font-family:Arial;font-size:20px;stroke:black;fill:#008000;stroke-width:2">ABCD</text>)svg"
        R"svg(</svg>)svg");
    ASSERT_TRUE(document);
    settle(*document);
    auto const before = serialize(*document);
    auto *text = item(*document, "t");
    ASSERT_TRUE(text);
    auto const owner_attrs_before = authored_attributes(text);

    ASSERT_NO_FATAL_FAILURE(assert_native_baseline(*document, text, false));
    ASSERT_EQ(native_char_count(text), 4u);
    std::vector<NativeCharIdentity> identity_before;
    for (unsigned i = 0; i < 4; ++i) identity_before.push_back(native_char_identity(text, i));

    // Native source representation precondition, asserted before the range call.
    SPILength parsed;
    parsed.read("0.00000000000000000001");
    ASSERT_TRUE(parsed.set);
    ASSERT_DOUBLE_EQ(parsed.computed, 1e-20);
    ASSERT_GT(parsed.computed, 0.0);

    // Native source representation precondition for the prepared local value,
    // asserted before the range call.
    std::string const local_width = numeric_text(5e-21);
    SPILength local_parsed;
    local_parsed.read(local_width.c_str());
    ASSERT_TRUE(local_parsed.set);
    ASSERT_DOUBLE_EQ(local_parsed.computed, 5e-21);
    ASSERT_GT(local_parsed.computed, 0.0);

    auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
    ASSERT_TRUE(token);

    // Freeze the actual run's full native vector-effect value and priority.
    auto const frozen = frozen_vector_effect(text, 0, 4);
    auto patch = Util::delete_with<sp_repr_css_attr_unref>(sp_repr_css_attr_new());
    sp_repr_css_set_property(patch.get(), "stroke-width", local_width.c_str());
    std::string const ve = frozen.important ? std::string(frozen.value.raw()) + " !important"
                                            : frozen.value.raw();
    sp_repr_css_set_property(patch.get(), "vector-effect", ve.c_str());
    TextStyleLocalStroke stroke_flags;
    stroke_flags.stroke_width = true;
    ASSERT_NO_FATAL_FAILURE(native_apply_local_stroke(*document, text, 0, 4, patch.get(), stroke_flags));

    for (unsigned i = 0; i < 4; ++i) {
        auto const widths = native_char_widths(*document, text, i);
        EXPECT_GT(widths.local, 0.0);
        expect_required_widths("tiny_scale2", i, widths, 5e-21, 1e-20);
        EXPECT_EQ(widths.convention, SW::StrokeWidthConvention::Ordinary);
        EXPECT_EQ(native_char_identity(text, i), identity_before[i]);
    }
    EXPECT_TRUE(authored_attributes(text) == owner_attrs_before);

    token->rollback();
    EXPECT_EQ(serialize(*document), before);
}

// ---------------------------------------------------------------------------
// SW3 NDIAG-4: native logical range reuse. Ordinary scale-2 text ABCD split
// into AB(red)/CD(blue). First write [1,3) must leave local widths 2/5/5/2;
// after reacquiring layout and fresh iterators, [0,1) must give 3/5/5/2 while
// content/fill/font/owner coordinates stay unchanged. Span identity and exact
// normalized XML are informational only.
// ---------------------------------------------------------------------------

TEST_F(StrokeWidthControllerTest, SW3DiagnosticNativeRangeNormalizationAndReacquire)
{
    auto document = parse(std::string{svg_open} +
        R"svg(<text id="t" x="10" y="20" transform="scale(2)" style="font-family:Arial;font-size:20px;stroke:black;stroke-width:2">)svg"
        R"svg(<tspan id="s1" style="fill:red">AB</tspan><tspan id="s2" style="fill:blue">CD</tspan>)svg"
        R"svg(</text></svg>)svg");
    ASSERT_TRUE(document);
    settle(*document);
    auto const before = serialize(*document);
    auto *text = item(*document, "t");
    ASSERT_TRUE(text);
    auto const owner_attrs_before = authored_attributes(text);

    ASSERT_NO_FATAL_FAILURE(assert_native_baseline(*document, text, false));
    ASSERT_EQ(native_char_count(text), 4u);
    std::vector<NativeCharIdentity> identity_before;
    for (unsigned i = 0; i < 4; ++i) identity_before.push_back(native_char_identity(text, i));

    auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
    ASSERT_TRUE(token);

    // First range [1,3): B,C move to local 5; A,D keep local 2.
    ASSERT_NO_FATAL_FAILURE(native_apply_width(*document, text, 1, 3, "10"));
    double const first_local[4] = {2.0, 5.0, 5.0, 2.0};
    double const first_effective[4] = {4.0, 10.0, 10.0, 4.0};
    for (unsigned i = 0; i < 4; ++i) {
        auto const widths = native_char_widths(*document, text, i);
        expect_required_widths("range_first", i, widths, first_local[i], first_effective[i]);
        EXPECT_EQ(native_char_identity(text, i), identity_before[i]);
    }
    EXPECT_TRUE(authored_attributes(text) == owner_attrs_before);
    RecordProperty("range_first.spans", std::to_string(count_tspans(text)));

    // Reacquire layout and fresh iterators: [0,1) moves A to local 3.
    ASSERT_NO_FATAL_FAILURE(native_apply_width(*document, text, 0, 1, "6"));
    double const second_local[4] = {3.0, 5.0, 5.0, 2.0};
    double const second_effective[4] = {6.0, 10.0, 10.0, 4.0};
    for (unsigned i = 0; i < 4; ++i) {
        auto const widths = native_char_widths(*document, text, i);
        expect_required_widths("range_second", i, widths, second_local[i], second_effective[i]);
        EXPECT_EQ(native_char_identity(text, i), identity_before[i]);
    }
    EXPECT_TRUE(authored_attributes(text) == owner_attrs_before);
    RecordProperty("range_second.spans", std::to_string(count_tspans(text)));

    token->rollback();
    EXPECT_EQ(serialize(*document), before);
}

// ---------------------------------------------------------------------------
// SW2-NUMERIC-1: a representable effective 0 -> 1e-20 change at ordinary
// scale(2) and NonScaling scale(2) is planned and written. The shared
// query/display nearly_equal floor (1e-9 absolute) previously marked it
// Unchanged and emitted no patch, losing the change.
// ---------------------------------------------------------------------------

TEST_F(StrokeWidthControllerTest, SW2NumericZeroToTinyIsApplied)
{
    auto document = parse(std::string{svg_open} +
        R"svg(<rect id="ord" width="10" height="10" style="stroke:black;fill:none;stroke-width:0" transform="scale(2)"/>)svg"
        R"svg(<rect id="ns" width="10" height="10" style="stroke:black;fill:none;stroke-width:0;vector-effect:non-scaling-stroke" transform="scale(2)"/>)svg"
        R"(</svg>)");
    ASSERT_TRUE(document);
    settle(*document);
    auto const before = serialize(*document);
    auto *ord = item(*document, "ord");
    auto *ns = item(*document, "ns");
    ASSERT_TRUE(ord);
    ASSERT_TRUE(ns);
    auto const ord_attrs_before = authored_attributes(ord);
    auto const ns_attrs_before = authored_attributes(ns);

    auto find_target = [](SW::StrokeWidthResult const &result, SPItem *owner) -> SW::StrokeWidthTarget const * {
        for (auto const &target : result.targets) {
            if (target.owner.get() == owner) return &target;
        }
        return nullptr;
    };

    // Read-only native query baseline before acting: eligible 2, local 0,
    // descrim 2 and the two conventions as authored.
    auto const baseline = SW::query_stroke_widths(*document, {ord, ns});
    ASSERT_EQ(baseline.eligible, 2u);
    ASSERT_EQ(baseline.targets.size(), 2u);
    auto const *ord_baseline = find_target(baseline, ord);
    auto const *ns_baseline = find_target(baseline, ns);
    ASSERT_TRUE(ord_baseline);
    ASSERT_TRUE(ns_baseline);
    ASSERT_EQ(ord_baseline->runs.size(), 1u);
    ASSERT_EQ(ns_baseline->runs.size(), 1u);
    EXPECT_EQ(ord_baseline->runs[0].style.convention, SW::StrokeWidthConvention::Ordinary);
    EXPECT_EQ(ns_baseline->runs[0].style.convention, SW::StrokeWidthConvention::NonScaling);
    EXPECT_DOUBLE_EQ(ord_baseline->runs[0].style.local_computed, 0.0);
    EXPECT_DOUBLE_EQ(ns_baseline->runs[0].style.local_computed, 0.0);
    EXPECT_DOUBLE_EQ(ord_baseline->transform_scale, 2.0);
    EXPECT_DOUBLE_EQ(ns_baseline->transform_scale, 2.0);
    EXPECT_EQ(serialize(*document), before);

    auto const plan = SW::prepare_stroke_widths(*document, {ord, ns}, absolute(1e-20), 100);
    EXPECT_EQ(plan.state, SW::StrokeWidthPlanState::Prepared);
    EXPECT_EQ(plan.planned_changes, 2u);
    EXPECT_EQ(plan.unchanged, 0u);
    auto const *ord_member = find_member(plan, ord);
    auto const *ns_member = find_member(plan, ns);
    ASSERT_TRUE(ord_member);
    ASSERT_TRUE(ns_member);
    EXPECT_EQ(ord_member->outcome, SW::StrokeWidthMemberOutcome::Change);
    EXPECT_EQ(ns_member->outcome, SW::StrokeWidthMemberOutcome::Change);
    ASSERT_TRUE(ord_member->local_width.has_value());
    ASSERT_TRUE(ns_member->local_width.has_value());
    EXPECT_DOUBLE_EQ(*ord_member->local_width, 5e-21); // 1e-20 / scale 2
    EXPECT_DOUBLE_EQ(*ns_member->local_width, 1e-20);  // NS effective == local

    auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
    ASSERT_TRUE(token);
    auto const result = SW::apply_stroke_widths(*document, plan, 100, *token);
    EXPECT_EQ(result.state, SW::StrokeWidthApplyState::Applied);
    EXPECT_EQ(result.reason, SW::StrokeWidthMemberReason::None);
    EXPECT_EQ(result.changed, 2u);
    EXPECT_EQ(result.attempted_writes, 2u);

    // Native computed widths are positive and exactly the intended local values;
    // the ordinary effective is 5e-21 * 2 == 1e-20 and NS effective == local.
    EXPECT_GT(computed_width(*document, "ord"), 0.0);
    EXPECT_GT(computed_width(*document, "ns"), 0.0);
    EXPECT_DOUBLE_EQ(computed_width(*document, "ord"), *ord_member->local_width);
    EXPECT_DOUBLE_EQ(computed_width(*document, "ns"), *ns_member->local_width);
    EXPECT_DOUBLE_EQ(computed_width(*document, "ord") * ord->i2doc_affine().descrim(), 1e-20);
    EXPECT_DOUBLE_EQ(computed_width(*document, "ns"), 1e-20);

    // Query convention enum and native paint/vector-effect preserved.
    auto const after = SW::query_stroke_widths(*document, {ord, ns});
    ASSERT_EQ(after.eligible, 2u);
    auto const *ord_after = find_target(after, ord);
    auto const *ns_after = find_target(after, ns);
    ASSERT_TRUE(ord_after);
    ASSERT_TRUE(ns_after);
    EXPECT_EQ(ord_after->runs[0].style.convention, SW::StrokeWidthConvention::Ordinary);
    EXPECT_EQ(ns_after->runs[0].style.convention, SW::StrokeWidthConvention::NonScaling);
    ASSERT_TRUE(ord->style);
    ASSERT_TRUE(ns->style);
    EXPECT_FALSE(ord->style->stroke.isNone());
    EXPECT_FALSE(ns->style->stroke.isNone());
    EXPECT_TRUE(ord->style->fill.isNone());
    EXPECT_TRUE(ns->style->fill.isNone());
    EXPECT_TRUE(ns->style->vector_effect.stroke);
    EXPECT_TRUE(authored_attributes(ord) == ord_attrs_before);
    EXPECT_TRUE(authored_attributes(ns) == ns_attrs_before);

    // Nonfatal output assertions so the caller rollback always runs.
    EXPECT_TRUE(SW::stroke_widths_output_ready(*document, plan, 100, *token));
    token->rollback();
    EXPECT_EQ(serialize(*document), before);
}

// ---------------------------------------------------------------------------
// SW2-NUMERIC-2: the reverse representable transition. Authored local 1e-20 at
// ordinary scale(2) and NonScaling scale(2); absolute 0 is a real change to
// exactly 0, not a no-op, and stroke/fill/vector-effect survive.
// ---------------------------------------------------------------------------

TEST_F(StrokeWidthControllerTest, SW2NumericTinyToZeroIsApplied)
{
    auto document = parse(std::string{svg_open} +
        R"svg(<rect id="ord" width="10" height="10" style="stroke:black;fill:none;stroke-width:0.00000000000000000001" transform="scale(2)"/>)svg"
        R"svg(<rect id="ns" width="10" height="10" style="stroke:black;fill:none;stroke-width:0.00000000000000000001;vector-effect:non-scaling-stroke" transform="scale(2)"/>)svg"
        R"(</svg>)");
    ASSERT_TRUE(document);
    settle(*document);
    auto const before = serialize(*document);
    auto *ord = item(*document, "ord");
    auto *ns = item(*document, "ns");
    ASSERT_TRUE(ord);
    ASSERT_TRUE(ns);
    auto const ord_attrs_before = authored_attributes(ord);
    auto const ns_attrs_before = authored_attributes(ns);

    auto find_target = [](SW::StrokeWidthResult const &result, SPItem *owner) -> SW::StrokeWidthTarget const * {
        for (auto const &target : result.targets) {
            if (target.owner.get() == owner) return &target;
        }
        return nullptr;
    };

    // Fatal parser/computed preconditions: the authored decimal is a positive
    // representable 1e-20, never a parser zero.
    SPILength parsed;
    parsed.read("0.00000000000000000001");
    ASSERT_TRUE(parsed.set);
    ASSERT_DOUBLE_EQ(parsed.computed, 1e-20);
    ASSERT_GT(parsed.computed, 0.0);
    ASSERT_TRUE(ord->style);
    ASSERT_TRUE(ns->style);
    ASSERT_DOUBLE_EQ(ord->style->stroke_width.computed, 1e-20);
    ASSERT_DOUBLE_EQ(ns->style->stroke_width.computed, 1e-20);
    ASSERT_GT(ord->style->stroke_width.computed, 0.0);
    ASSERT_GT(ns->style->stroke_width.computed, 0.0);

    auto const baseline = SW::query_stroke_widths(*document, {ord, ns});
    ASSERT_EQ(baseline.eligible, 2u);
    ASSERT_EQ(baseline.targets.size(), 2u);
    auto const *ord_baseline = find_target(baseline, ord);
    auto const *ns_baseline = find_target(baseline, ns);
    ASSERT_TRUE(ord_baseline);
    ASSERT_TRUE(ns_baseline);
    EXPECT_EQ(ord_baseline->runs[0].style.convention, SW::StrokeWidthConvention::Ordinary);
    EXPECT_EQ(ns_baseline->runs[0].style.convention, SW::StrokeWidthConvention::NonScaling);
    EXPECT_DOUBLE_EQ(ord_baseline->runs[0].style.local_computed, 1e-20);
    EXPECT_DOUBLE_EQ(ns_baseline->runs[0].style.local_computed, 1e-20);
    EXPECT_DOUBLE_EQ(ord_baseline->transform_scale, 2.0);
    EXPECT_DOUBLE_EQ(ns_baseline->transform_scale, 2.0);
    EXPECT_EQ(serialize(*document), before);

    auto const plan = SW::prepare_stroke_widths(*document, {ord, ns}, absolute(0.0), 101);
    EXPECT_EQ(plan.state, SW::StrokeWidthPlanState::Prepared);
    EXPECT_EQ(plan.planned_changes, 2u);
    EXPECT_EQ(plan.unchanged, 0u);
    for (SPItem *owner : {ord, ns}) {
        auto const *member = find_member(plan, owner);
        ASSERT_TRUE(member);
        EXPECT_EQ(member->outcome, SW::StrokeWidthMemberOutcome::Change);
        ASSERT_TRUE(member->local_width.has_value());
        EXPECT_DOUBLE_EQ(*member->local_width, 0.0);
    }

    auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
    ASSERT_TRUE(token);
    auto const result = SW::apply_stroke_widths(*document, plan, 101, *token);
    EXPECT_EQ(result.state, SW::StrokeWidthApplyState::Applied);
    EXPECT_EQ(result.reason, SW::StrokeWidthMemberReason::None);
    EXPECT_EQ(result.changed, 2u);
    EXPECT_EQ(result.attempted_writes, 2u);

    EXPECT_DOUBLE_EQ(computed_width(*document, "ord"), 0.0);
    EXPECT_DOUBLE_EQ(computed_width(*document, "ns"), 0.0);
    EXPECT_FALSE(ord->style->stroke.isNone());
    EXPECT_FALSE(ns->style->stroke.isNone());
    EXPECT_TRUE(ord->style->fill.isNone());
    EXPECT_TRUE(ns->style->fill.isNone());
    EXPECT_TRUE(ns->style->vector_effect.stroke);
    EXPECT_TRUE(authored_attributes(ord) == ord_attrs_before);
    EXPECT_TRUE(authored_attributes(ns) == ns_attrs_before);

    EXPECT_TRUE(SW::stroke_widths_output_ready(*document, plan, 101, *token));
    token->rollback();
    EXPECT_EQ(serialize(*document), before);
}

// ---------------------------------------------------------------------------
// SW2-NUMERIC-3: exact transformed equality stays a no-op. Ordinary authored
// 5e-21 at scale(2) and NonScaling authored 1e-20 both have effective 1e-20, so
// absolute 1e-20 and relative 100 are genuine no-ops even though the two local
// values differ. This is effective equality, not local equality and not a
// display tolerance.
// ---------------------------------------------------------------------------

TEST_F(StrokeWidthControllerTest, SW2NumericIdenticalTinyRemainsNoOp)
{
    auto document = parse(std::string{svg_open} +
        R"svg(<rect id="ord" width="10" height="10" style="stroke:black;fill:none;stroke-width:0.000000000000000000005" transform="scale(2)"/>)svg"
        R"svg(<rect id="ns" width="10" height="10" style="stroke:black;fill:none;stroke-width:0.00000000000000000001;vector-effect:non-scaling-stroke" transform="scale(2)"/>)svg"
        R"(</svg>)");
    ASSERT_TRUE(document);
    settle(*document);
    auto const before = serialize(*document);
    auto *ord = item(*document, "ord");
    auto *ns = item(*document, "ns");
    ASSERT_TRUE(ord);
    ASSERT_TRUE(ns);

    auto find_target = [](SW::StrokeWidthResult const &result, SPItem *owner) -> SW::StrokeWidthTarget const * {
        for (auto const &target : result.targets) {
            if (target.owner.get() == owner) return &target;
        }
        return nullptr;
    };

    // Fatal native baseline: computed widths, conventions and transform.
    ASSERT_TRUE(ord->style);
    ASSERT_TRUE(ns->style);
    ASSERT_DOUBLE_EQ(ord->style->stroke_width.computed, 5e-21);
    ASSERT_DOUBLE_EQ(ns->style->stroke_width.computed, 1e-20);
    auto const baseline = SW::query_stroke_widths(*document, {ord, ns});
    ASSERT_EQ(baseline.eligible, 2u);
    ASSERT_EQ(baseline.targets.size(), 2u);
    auto const *ord_baseline = find_target(baseline, ord);
    auto const *ns_baseline = find_target(baseline, ns);
    ASSERT_TRUE(ord_baseline);
    ASSERT_TRUE(ns_baseline);
    ASSERT_EQ(ord_baseline->runs.size(), 1u);
    ASSERT_EQ(ns_baseline->runs.size(), 1u);
    EXPECT_EQ(ord_baseline->runs[0].style.convention, SW::StrokeWidthConvention::Ordinary);
    EXPECT_EQ(ns_baseline->runs[0].style.convention, SW::StrokeWidthConvention::NonScaling);
    EXPECT_DOUBLE_EQ(ord_baseline->transform_scale, 2.0);
    EXPECT_DOUBLE_EQ(ns_baseline->transform_scale, 2.0);
    ASSERT_TRUE(ord_baseline->runs[0].style.effective_px.has_value());
    ASSERT_TRUE(ns_baseline->runs[0].style.effective_px.has_value());
    EXPECT_DOUBLE_EQ(*ord_baseline->runs[0].style.effective_px, 1e-20);
    EXPECT_DOUBLE_EQ(*ns_baseline->runs[0].style.effective_px, 1e-20);
    // Exact transformed equality, not local equality: the two locals differ.
    EXPECT_NE(ord->style->stroke_width.computed, ns->style->stroke_width.computed);
    EXPECT_DOUBLE_EQ(ord->style->stroke_width.computed * ord_baseline->transform_scale,
                     ns->style->stroke_width.computed);
    EXPECT_EQ(serialize(*document), before);

    auto run_case = [&](SW::StrokeWidthIntent const &intent, std::uint64_t generation) {
        auto const plan = SW::prepare_stroke_widths(*document, {ord, ns}, intent, generation);
        EXPECT_EQ(plan.state, SW::StrokeWidthPlanState::Prepared);
        EXPECT_EQ(plan.planned_changes, 0u);
        EXPECT_EQ(plan.unchanged, 2u);
        for (SPItem *owner : {ord, ns}) {
            auto const *member = find_member(plan, owner);
            ASSERT_TRUE(member);
            EXPECT_EQ(member->outcome, SW::StrokeWidthMemberOutcome::Unchanged);
            EXPECT_FALSE(member->local_width.has_value());
            EXPECT_FALSE(member->local_dasharray.has_value());
            EXPECT_FALSE(member->local_dashoffset.has_value());
            EXPECT_FALSE(member->native_dasharray_css.has_value());
        }

        auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
        ASSERT_TRUE(token);
        auto const result = SW::apply_stroke_widths(*document, plan, generation, *token);
        EXPECT_EQ(result.state, SW::StrokeWidthApplyState::Unchanged);
        EXPECT_EQ(result.reason, SW::StrokeWidthMemberReason::None);
        EXPECT_EQ(result.changed, 0u);
        EXPECT_EQ(result.unchanged, 2u);
        EXPECT_EQ(result.attempted_writes, 0u);
        EXPECT_TRUE(token->validFor(document.get()));
        EXPECT_TRUE(SW::stroke_widths_output_ready(*document, plan, generation, *token));
        EXPECT_EQ(serialize(*document), before);
        token->rollback();
        EXPECT_FALSE(token->validFor(document.get()));
        EXPECT_EQ(serialize(*document), before);
    };
    run_case(absolute(1e-20), 110);
    run_case(relative(100.0), 111);
}

// ---------------------------------------------------------------------------
// SW3 native-local-stroke A: opt-in local width/dasharray/dashoffset with
// !important applied to [1,3) of ordinary scale-2 ABCD. Selected chars take the
// local winners and priorities; unselected chars stay exact baseline. Unrelated
// inline fill/font !important must beat a conflicting stylesheet on the text and
// survive the span write, and caller rollback must restore exact XML.
// ---------------------------------------------------------------------------

TEST_F(StrokeWidthControllerTest, SW3NativeLocalStrokePriorityAndPreservation)
{
    auto document = parse(std::string{svg_open} +
        R"svg(<style>.txt { fill:blue; font-family:'Courier New'; font-size:30px; }</style>)svg"
        R"svg(<text id="t" class="txt" x="10" y="20" transform="scale(2)" style="fill:#008000 !important;font-family:Arial !important;font-size:20px !important;stroke:black;stroke-width:2;stroke-dasharray:6,2;stroke-dashoffset:1;vector-effect:none !important">ABCD</text>)svg"
        R"svg(</svg>)svg");
    ASSERT_TRUE(document);
    settle(*document);
    auto const before = serialize(*document);
    auto *text = item(*document, "t");
    ASSERT_TRUE(text);
    auto const owner_attrs_before = authored_attributes(text);

    ASSERT_NO_FATAL_FAILURE(assert_native_baseline(*document, text, false));
    ASSERT_EQ(native_char_count(text), 4u);
    std::vector<NativeCharIdentity> identity_before;
    for (unsigned i = 0; i < 4; ++i) identity_before.push_back(native_char_identity(text, i));

    // Parsed native baseline and effective winners before any write: the inline
    // !important fill/font must win over the conflicting stylesheet (font 20,
    // not 30), the stroke locals stay un-important, and the inline
    // vector-effect:none !important priority is already true.
    for (unsigned i = 0; i < 4; ++i) {
        auto *layout = te_get_layout(text);
        ASSERT_TRUE(layout);
        auto const it = layout->charIndexToIterator(static_cast<int>(i));
        SPStyle const *style = sp_te_style_at_position(text, it);
        ASSERT_TRUE(style);
        ASSERT_EQ(style->fill.getColor().toRGBA(), 0x008000ffu);
        ASSERT_STREQ(style->font_family.get_value().c_str(), "Arial");

        // Pre-write rendering-source oracle for this fixture's <text> element:
        // the authored vector-effect is none with !important priority, so the
        // renderer's element style (not the SPString's own style) must say so.
        auto const render = native_render_style_at(text, i);
        ASSERT_TRUE(render.source);
        ASSERT_EQ(render.element, static_cast<SPObject *>(text));
        ASSERT_TRUE(render.style);
        ASSERT_STREQ(render.style->vector_effect.get_value().c_str(), "none");
        ASSERT_TRUE(render.style->vector_effect.important);

        auto const stroke = native_char_stroke(text, i);
        EXPECT_DOUBLE_EQ(stroke.width, 2.0);
        ASSERT_EQ(stroke.dasharray.size(), 2u);
        EXPECT_DOUBLE_EQ(stroke.dasharray[0], 6.0);
        EXPECT_DOUBLE_EQ(stroke.dasharray[1], 2.0);
        EXPECT_DOUBLE_EQ(stroke.dashoffset, 1.0);
        EXPECT_FALSE(stroke.width_important);
        EXPECT_FALSE(stroke.dasharray_important);
        EXPECT_FALSE(stroke.dashoffset_important);
        EXPECT_STREQ(stroke.vector_effect.c_str(), "none");
        EXPECT_TRUE(stroke.vector_effect_important);
        EXPECT_DOUBLE_EQ(identity_before[i].font_size, 20.0);
        EXPECT_TRUE(identity_before[i].font_family == identity_before[0].font_family);
        EXPECT_TRUE(identity_before[i].fill == identity_before[0].fill);
    }

    auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
    ASSERT_TRUE(token);

    auto const frozen = frozen_vector_effect(text, 1, 3);
    ASSERT_TRUE(frozen.important);
    ASSERT_STREQ(frozen.value.c_str(), "none");
    auto patch = Util::delete_with<sp_repr_css_attr_unref>(sp_repr_css_attr_new());
    sp_repr_css_set_property(patch.get(), "stroke-width", "5 !important");
    sp_repr_css_set_property(patch.get(), "stroke-dasharray", "8, 4 !important");
    sp_repr_css_set_property(patch.get(), "stroke-dashoffset", "3 !important");
    std::string const ve = frozen.important ? std::string(frozen.value.raw()) + " !important"
                                            : frozen.value.raw();
    sp_repr_css_set_property(patch.get(), "vector-effect", ve.c_str());
    auto const patch_before = css_attr_map(patch.get());

    TextStyleLocalStroke flags;
    flags.stroke_width = true;
    flags.stroke_dasharray = true;
    flags.stroke_dashoffset = true;
    ASSERT_NO_FATAL_FAILURE(native_apply_local_stroke(*document, text, 1, 3, patch.get(), flags));
    EXPECT_TRUE(css_attr_map(patch.get()) == patch_before);

    for (unsigned i = 0; i < 4; ++i) {
        auto const widths = native_char_widths(*document, text, i);
        auto const stroke = native_char_stroke(text, i);
        EXPECT_EQ(native_char_identity(text, i), identity_before[i]);
        if (i == 1 || i == 2) {
            expect_required_widths("local_stroke_selected", i, widths, 5.0, 10.0);
            ASSERT_EQ(stroke.dasharray.size(), 2u);
            EXPECT_DOUBLE_EQ(stroke.dasharray[0], 8.0);
            EXPECT_DOUBLE_EQ(stroke.dasharray[1], 4.0);
            EXPECT_DOUBLE_EQ(stroke.dashoffset, 3.0);
            EXPECT_TRUE(stroke.width_important);
            EXPECT_TRUE(stroke.dasharray_important);
            EXPECT_TRUE(stroke.dashoffset_important);
        } else {
            expect_required_widths("local_stroke_unselected", i, widths, 2.0, 4.0);
            ASSERT_EQ(stroke.dasharray.size(), 2u);
            EXPECT_DOUBLE_EQ(stroke.dasharray[0], 6.0);
            EXPECT_DOUBLE_EQ(stroke.dasharray[1], 2.0);
            EXPECT_DOUBLE_EQ(stroke.dashoffset, 1.0);
            EXPECT_FALSE(stroke.width_important);
            EXPECT_FALSE(stroke.dasharray_important);
            EXPECT_FALSE(stroke.dashoffset_important);
        }
        EXPECT_EQ(widths.convention, SW::StrokeWidthConvention::Ordinary);
        EXPECT_STREQ(stroke.vector_effect.c_str(), "none");
        EXPECT_TRUE(stroke.vector_effect_important);
    }
    EXPECT_TRUE(authored_attributes(text) == owner_attrs_before);

    token->rollback();
    EXPECT_EQ(serialize(*document), before);
}

// ---------------------------------------------------------------------------
// SW3 native-local-stroke B: independent opt-in flags at ordinary scale 2.
// Incoming width10/dash8,4/offset6 is document-space; each row enables exactly
// one restore. The default-empty row matches the established legacy defaults.
// The final row enables all flags but supplies width only, proving absent
// dash/offset are neither inserted nor removed and keep the native baseline.
// ---------------------------------------------------------------------------

TEST_F(StrokeWidthControllerTest, SW3NativeLocalStrokeIndependentFlags)
{
    auto run_row = [&](char const *label, NativeLocalStrokeInput const &input,
                       TextStyleLocalStroke const &flags, double expected_width,
                       std::vector<double> const &expected_dash, double expected_offset) {
        SCOPED_TRACE(label);
        auto document = parse(std::string{svg_open} +
            R"svg(<text id="t" x="10" y="20" transform="scale(2)" style="font-family:Arial;font-size:20px;stroke:black;fill:#008000;stroke-width:2;stroke-dasharray:6,2;stroke-dashoffset:1">ABCD</text>)svg"
            R"svg(</svg>)svg");
        ASSERT_TRUE(document);
        settle(*document);
        auto const before = serialize(*document);
        auto *text = item(*document, "t");
        ASSERT_TRUE(text);
        auto const owner_attrs_before = authored_attributes(text);
        std::vector<NativeCharIdentity> identity_before;
        for (unsigned i = 0; i < 4; ++i) identity_before.push_back(native_char_identity(text, i));

        auto const frozen = frozen_vector_effect(text, 0, 4);
        auto patch = Util::delete_with<sp_repr_css_attr_unref>(sp_repr_css_attr_new());
        if (!input.width.empty()) sp_repr_css_set_property(patch.get(), "stroke-width", input.width.c_str());
        if (!input.dasharray.empty()) sp_repr_css_set_property(patch.get(), "stroke-dasharray", input.dasharray.c_str());
        if (!input.dashoffset.empty()) sp_repr_css_set_property(patch.get(), "stroke-dashoffset", input.dashoffset.c_str());
        std::string const ve = frozen.important ? std::string(frozen.value.raw()) + " !important"
                                                : frozen.value.raw();
        sp_repr_css_set_property(patch.get(), "vector-effect", ve.c_str());
        auto const patch_before = css_attr_map(patch.get());

        auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
        ASSERT_TRUE(token);
        ASSERT_NO_FATAL_FAILURE(native_apply_local_stroke(*document, text, 0, 4, patch.get(), flags));
        EXPECT_TRUE(css_attr_map(patch.get()) == patch_before);

        for (unsigned i = 0; i < 4; ++i) {
            auto const widths = native_char_widths(*document, text, i);
            auto const stroke = native_char_stroke(text, i);
            expect_required_widths(label, i, widths, expected_width, expected_width * 2.0);
            EXPECT_EQ(widths.convention, SW::StrokeWidthConvention::Ordinary);
            ASSERT_EQ(stroke.dasharray.size(), expected_dash.size());
            for (std::size_t k = 0; k < expected_dash.size(); ++k) {
                EXPECT_DOUBLE_EQ(stroke.dasharray[k], expected_dash[k]);
            }
            EXPECT_DOUBLE_EQ(stroke.dashoffset, expected_offset);
            EXPECT_EQ(native_char_identity(text, i), identity_before[i]);
        }
        EXPECT_TRUE(authored_attributes(text) == owner_attrs_before);
        token->rollback();
        EXPECT_EQ(serialize(*document), before);
    };

    NativeLocalStrokeInput const full{"10", "8, 4", "6"};

    TextStyleLocalStroke const none_flags;
    run_row("default_empty", full, none_flags, 5.0, {4.0, 2.0}, 3.0);

    TextStyleLocalStroke width_flag;
    width_flag.stroke_width = true;
    run_row("width_only", full, width_flag, 10.0, {4.0, 2.0}, 3.0);

    TextStyleLocalStroke dash_flag;
    dash_flag.stroke_dasharray = true;
    run_row("dasharray_only", full, dash_flag, 5.0, {8.0, 4.0}, 3.0);

    TextStyleLocalStroke offset_flag;
    offset_flag.stroke_dashoffset = true;
    run_row("dashoffset_only", full, offset_flag, 5.0, {4.0, 2.0}, 6.0);

    NativeLocalStrokeInput const width_only_input{"10", "", ""};
    TextStyleLocalStroke all_flags;
    all_flags.stroke_width = true;
    all_flags.stroke_dasharray = true;
    all_flags.stroke_dashoffset = true;
    run_row("all_flags_width_only", width_only_input, all_flags, 10.0, {6.0, 2.0}, 1.0);
}

// SW3 A FLOW-SRC-1: ordinary text tspans; per-char query/render source is the
// tspan, AB 3/3 NonScaling full VE !important, CD 4/8 Ordinary none. Read-only.
TEST_F(StrokeWidthControllerTest, SW3FlowSourceOrdinaryTspanControl)
{
    auto document = parse(std::string{svg_open} +
        R"svg(<text id="t" x="10" y="20" transform="scale(2)" style="font-family:Arial;font-size:20px;stroke:black">)svg"
        R"svg(<tspan id="s1" style="stroke-width:3;vector-effect:non-scaling-stroke !important">AB</tspan>)svg"
        R"svg(<tspan id="s2" style="stroke-width:4;vector-effect:none">CD</tspan>)svg"
        R"svg(</text></svg>)svg");
    ASSERT_TRUE(document);
    settle(*document);
    auto const before = serialize(*document);
    auto *text = item(*document, "t");
    auto *s1 = document->getObjectById("s1");
    auto *s2 = document->getObjectById("s2");
    ASSERT_TRUE(text);
    ASSERT_TRUE(s1 && s1->style);
    ASSERT_TRUE(s2 && s2->style);
    ASSERT_STREQ(s1->style->vector_effect.get_value().c_str(), "non-scaling-stroke");
    EXPECT_TRUE(s1->style->vector_effect.important);
    ASSERT_STREQ(s2->style->vector_effect.get_value().c_str(), "none");
    ASSERT_EQ(native_char_count(text), 4u);
    RecordProperty("fixture.svg", before);
    expect_flow_string_char("tspan_ab", *document, text, 0, 'A', s1, "non-scaling-stroke", true,
                            3.0, 3.0, SW::StrokeWidthConvention::NonScaling);
    expect_flow_string_char("tspan_ab", *document, text, 1, 'B', s1, "non-scaling-stroke", true,
                            3.0, 3.0, SW::StrokeWidthConvention::NonScaling);
    expect_flow_string_char("tspan_cd", *document, text, 2, 'C', s2, "none", false,
                            4.0, 8.0, SW::StrokeWidthConvention::Ordinary);
    expect_flow_string_char("tspan_cd", *document, text, 3, 'D', s2, "none", false,
                            4.0, 8.0, SW::StrokeWidthConvention::Ordinary);
    EXPECT_EQ(serialize(*document), before);
}

// SW3 A FLOW-SRC-2: legacy flowRoot/flowPara/flowSpan; AB maps to flowPara p and
// CD to flowSpan s with the same one-character numeric/source identity. Read-only.
TEST_F(StrokeWidthControllerTest, SW3FlowSourceLegacyParaSpanMapping)
{
    auto document = parse(std::string{svg_open} +
        R"svg(<flowRoot id="f" transform="scale(2)" style="font-family:Arial;font-size:20px;stroke:black">)svg"
        R"svg(<flowRegion id="region"><rect id="r" width="400" height="400"/></flowRegion>)svg"
        R"svg(<flowPara id="p" style="stroke-width:3;vector-effect:non-scaling-stroke !important">AB)svg"
        R"svg(<flowSpan id="s" style="stroke-width:4;vector-effect:none">CD</flowSpan></flowPara>)svg"
        R"svg(</flowRoot></svg>)svg");
    ASSERT_TRUE(document);
    settle(*document);
    auto const before = serialize(*document);
    auto *flow = item(*document, "f");
    auto *p = document->getObjectById("p");
    auto *s = document->getObjectById("s");
    ASSERT_TRUE(flow);
    ASSERT_TRUE(p && p->style);
    ASSERT_TRUE(s && s->style);
    ASSERT_STREQ(p->style->vector_effect.get_value().c_str(), "non-scaling-stroke");
    EXPECT_TRUE(p->style->vector_effect.important);
    ASSERT_STREQ(s->style->vector_effect.get_value().c_str(), "none");
    ASSERT_EQ(native_char_count(flow), 4u);
    RecordProperty("fixture.svg", before);
    expect_flow_string_char("para_ab", *document, flow, 0, 'A', p, "non-scaling-stroke", true,
                            3.0, 3.0, SW::StrokeWidthConvention::NonScaling);
    expect_flow_string_char("para_ab", *document, flow, 1, 'B', p, "non-scaling-stroke", true,
                            3.0, 3.0, SW::StrokeWidthConvention::NonScaling);
    expect_flow_string_char("span_cd", *document, flow, 2, 'C', s, "none", false,
                            4.0, 8.0, SW::StrokeWidthConvention::Ordinary);
    expect_flow_string_char("span_cd", *document, flow, 3, 'D', s, "none", false,
                            4.0, 8.0, SW::StrokeWidthConvention::Ordinary);
    EXPECT_EQ(serialize(*document), before);
}

// SW3 A FLOW-SRC-3: two independent fixtures. A real empty flowLine and a real
// empty flowRegionBreak each leave one non-glyph break position whose raw source
// is the break SPObject, not an SPString; A/B stay flowPara 3/3 NonScaling and
// the region break advances B to shape 1.
TEST_F(StrokeWidthControllerTest, SW3FlowSourceEmptyBreakControlIndices)
{
    // Native geometry diagnostics for the region-bound evidence recorded below.
    auto const rect_text = [](Geom::OptRect const &r) {
        return r ? std::to_string(r->left()) + " " + std::to_string(r->top()) + " "
                       + std::to_string(r->right()) + " " + std::to_string(r->bottom())
                 : std::string("none");
    };
    auto const point_text = [](Geom::Point const &p) {
        return std::to_string(p.x()) + " " + std::to_string(p.y());
    };

    auto line_doc = parse(std::string{svg_open} +
        R"svg(<flowRoot id="f" style="font-family:Arial;font-size:20px;stroke:black">)svg"
        R"svg(<flowRegion id="region"><rect id="r" width="400" height="400"/></flowRegion>)svg"
        R"svg(<flowPara id="p" style="stroke-width:3;vector-effect:non-scaling-stroke !important">A)svg"
        R"svg(<flowLine id="br"/>B</flowPara></flowRoot></svg>)svg");
    ASSERT_TRUE(line_doc);
    settle(*line_doc);
    auto const line_before = serialize(*line_doc);
    auto *line_flow = item(*line_doc, "f");
    auto *line_p = line_doc->getObjectById("p");
    auto *line_br = line_doc->getObjectById("br");
    ASSERT_TRUE(line_flow);
    ASSERT_TRUE(line_p && line_p->style);
    ASSERT_TRUE(line_br);

    ASSERT_EQ(native_char_count(line_flow), 3u);
    RecordProperty("flowline.svg", line_before);
    expect_flow_string_char("flowline_a", *line_doc, line_flow, 0, 'A', line_p,
                            "non-scaling-stroke", true, 3.0, 3.0,
                            SW::StrokeWidthConvention::NonScaling);
    expect_flow_break(line_flow, line_br, 1, "flowline_break");
    expect_flow_string_char("flowline_b", *line_doc, line_flow, 2, 'B', line_p,
                            "non-scaling-stroke", true, 3.0, 3.0,
                            SW::StrokeWidthConvention::NonScaling);
    auto *line_layout = te_get_layout(line_flow);
    ASSERT_TRUE(line_layout);
    EXPECT_EQ(line_layout->lineIndex(line_layout->charIndexToIterator(0)), 0u);
    EXPECT_EQ(line_layout->lineIndex(line_layout->charIndexToIterator(2)), 1u);
    // A flowLine is a PARAGRAPH_BREAK, not a SHAPE_BREAK: B must keep using the
    // first (only) wrap shape even though it starts a new line.
    EXPECT_EQ(line_layout->shapeIndex(line_layout->charIndexToIterator(0)), 0u);
    EXPECT_EQ(line_layout->shapeIndex(line_layout->charIndexToIterator(2)), 0u);
    EXPECT_EQ(serialize(*line_doc), line_before);

    auto region_doc = parse(std::string{svg_open} +
        R"svg(<flowRoot id="f" style="font-family:Arial;font-size:20px;stroke:black">)svg"
        R"svg(<flowRegion id="region"><rect id="r0" width="400" height="200"/><rect id="r1" y="200" width="400" height="200"/></flowRegion>)svg"
        R"svg(<flowPara id="p" style="stroke-width:3;vector-effect:non-scaling-stroke !important">A)svg"
        R"svg(<flowRegionBreak id="br"/>B</flowPara></flowRoot></svg>)svg");
    ASSERT_TRUE(region_doc);
    settle(*region_doc);
    auto const region_before = serialize(*region_doc);
    auto *region_flow = item(*region_doc, "f");
    auto *region_p = region_doc->getObjectById("p");
    auto *region_br = region_doc->getObjectById("br");
    ASSERT_TRUE(region_flow);
    ASSERT_TRUE(region_p && region_p->style);
    ASSERT_TRUE(region_br);

    ASSERT_EQ(native_char_count(region_flow), 3u);
    RecordProperty("regionbreak.svg", region_before);
    expect_flow_string_char("regionbreak_a", *region_doc, region_flow, 0, 'A', region_p,
                            "non-scaling-stroke", true, 3.0, 3.0,
                            SW::StrokeWidthConvention::NonScaling);
    expect_flow_break(region_flow, region_br, 1, "regionbreak_break");
    expect_flow_string_char("regionbreak_b", *region_doc, region_flow, 2, 'B', region_p,
                            "non-scaling-stroke", true, 3.0, 3.0,
                            SW::StrokeWidthConvention::NonScaling);

    auto *region_layout = te_get_layout(region_flow);
    ASSERT_TRUE(region_layout);
    EXPECT_EQ(region_layout->shapeIndex(region_layout->charIndexToIterator(0)), 0u);
    EXPECT_EQ(region_layout->shapeIndex(region_layout->charIndexToIterator(2)), 1u);

    // Independent native evidence for the region break: the two flowRegion
    // rectangles are separate native shapes with disjoint, stacked bounds, so A's
    // anchor must lie inside the first rectangle and B's inside the second. This
    // does not depend on shapeIndex alone.
    auto *region_r0 = item(*region_doc, "r0");
    auto *region_r1 = item(*region_doc, "r1");
    ASSERT_TRUE(region_r0 && region_r1);
    auto const r0_bounds = region_r0->documentGeometricBounds();
    auto const r1_bounds = region_r1->documentGeometricBounds();
    ASSERT_TRUE(r0_bounds);
    ASSERT_TRUE(r1_bounds);
    RecordProperty("regionbreak.r0_bounds", rect_text(r0_bounds));
    RecordProperty("regionbreak.r1_bounds", rect_text(r1_bounds));
    EXPECT_LE(r0_bounds->bottom(), r1_bounds->top());
    auto const region_anchor_a = region_layout->characterAnchorPoint(region_layout->charIndexToIterator(0));
    auto const region_anchor_b = region_layout->characterAnchorPoint(region_layout->charIndexToIterator(2));
    RecordProperty("regionbreak.a_anchor", point_text(region_anchor_a));
    RecordProperty("regionbreak.b_anchor", point_text(region_anchor_b));
    EXPECT_TRUE(r0_bounds->contains(region_anchor_a));
    EXPECT_TRUE(r1_bounds->contains(region_anchor_b));
    EXPECT_GE(region_anchor_b.y(), r0_bounds->bottom());

    EXPECT_EQ(serialize(*region_doc), region_before);

    // A trailing SHAPE_BREAK has nothing to follow it: the layout must finish (no
    // hanging), add no synthetic newline, keep A in the first region and leave the
    // source SVG byte-identical.
    auto trailing_doc = parse(std::string{svg_open} +
        R"svg(<flowRoot id="f" style="font-family:Arial;font-size:20px;stroke:black">)svg"
        R"svg(<flowRegion id="region"><rect id="tr0" width="400" height="200"/><rect id="tr1" y="200" width="400" height="200"/></flowRegion>)svg"
        R"svg(<flowPara id="p" style="stroke-width:3;vector-effect:non-scaling-stroke !important">A)svg"
        R"svg(<flowRegionBreak id="tbr"/></flowPara></flowRoot></svg>)svg");
    ASSERT_TRUE(trailing_doc);
    settle(*trailing_doc);
    auto const trailing_before = serialize(*trailing_doc);
    auto *trailing_flow = item(*trailing_doc, "f");
    auto *trailing_p = trailing_doc->getObjectById("p");
    auto *trailing_br = trailing_doc->getObjectById("tbr");
    ASSERT_TRUE(trailing_flow);
    ASSERT_TRUE(trailing_p && trailing_p->style);
    ASSERT_TRUE(trailing_br);
    // Nothing follows the trailing break, so it produces no character position at
    // all; that is exactly what keeps the multiline content free of a newline. The
    // break object itself must still exist and stay untouched.
    ASSERT_EQ(native_char_count(trailing_flow), 1u);
    RecordProperty("trailing_regionbreak.svg", trailing_before);
    expect_flow_string_char("trailing_regionbreak_a", *trailing_doc, trailing_flow, 0, 'A', trailing_p,
                            "non-scaling-stroke", true, 3.0, 3.0,
                            SW::StrokeWidthConvention::NonScaling);
    EXPECT_EQ(sp_te_get_string_multiline(trailing_flow).raw(), "A");
    auto *trailing_layout = te_get_layout(trailing_flow);
    ASSERT_TRUE(trailing_layout);
    EXPECT_EQ(trailing_layout->shapeIndex(trailing_layout->charIndexToIterator(0)), 0u);
    EXPECT_EQ(serialize(*trailing_doc), trailing_before);

    // Two consecutive SHAPE_BREAKs: the first terminates the A paragraph and the
    // second starts the following paragraph, so it is consumed by the loop-head
    // branch. That branch must advance the input index as well as the shape, or the
    // same control code would be read forever. Three stacked rectangles give A,
    // the two breaks and B their own regions to prove both advances happened.
    auto leading_doc = parse(std::string{svg_open} +
        R"svg(<flowRoot id="f" style="font-family:Arial;font-size:20px;stroke:black">)svg"
        R"svg(<flowRegion id="region"><rect id="lr0" width="400" height="200"/>)svg"
        R"svg(<rect id="lr1" y="200" width="400" height="200"/>)svg"
        R"svg(<rect id="lr2" y="400" width="400" height="200"/></flowRegion>)svg"
        R"svg(<flowPara id="p" style="stroke-width:3;vector-effect:non-scaling-stroke !important">A)svg"
        R"svg(<flowRegionBreak id="lb1"/><flowRegionBreak id="lb2"/>B</flowPara></flowRoot></svg>)svg");
    ASSERT_TRUE(leading_doc);
    settle(*leading_doc);
    auto const leading_before = serialize(*leading_doc);
    auto *leading_flow = item(*leading_doc, "f");
    auto *leading_p = leading_doc->getObjectById("p");
    auto *leading_b1 = leading_doc->getObjectById("lb1");
    auto *leading_b2 = leading_doc->getObjectById("lb2");
    ASSERT_TRUE(leading_flow);
    ASSERT_TRUE(leading_p && leading_p->style);
    ASSERT_TRUE(leading_b1 && leading_b2);
    ASSERT_EQ(native_char_count(leading_flow), 4u);
    RecordProperty("leading_regionbreak.svg", leading_before);
    expect_flow_string_char("leading_regionbreak_a", *leading_doc, leading_flow, 0, 'A', leading_p,
                            "non-scaling-stroke", true, 3.0, 3.0,
                            SW::StrokeWidthConvention::NonScaling);
    expect_flow_break(leading_flow, leading_b1, 1, "leading_regionbreak_break");
    expect_flow_break(leading_flow, leading_b2, 2, "leading_regionbreak_second_break");
    expect_flow_string_char("leading_regionbreak_b", *leading_doc, leading_flow, 3, 'B', leading_p,
                            "non-scaling-stroke", true, 3.0, 3.0,
                            SW::StrokeWidthConvention::NonScaling);
    EXPECT_EQ(sp_te_get_string_multiline(leading_flow).raw(), "A\n\nB");
    auto *leading_layout = te_get_layout(leading_flow);
    ASSERT_TRUE(leading_layout);
    EXPECT_EQ(leading_layout->shapeIndex(leading_layout->charIndexToIterator(0)), 0u);
    EXPECT_EQ(leading_layout->shapeIndex(leading_layout->charIndexToIterator(3)), 2u);
    auto *leading_r0 = item(*leading_doc, "lr0");
    auto *leading_r2 = item(*leading_doc, "lr2");
    ASSERT_TRUE(leading_r0 && leading_r2);
    auto const lr0_bounds = leading_r0->documentGeometricBounds();
    auto const lr2_bounds = leading_r2->documentGeometricBounds();
    ASSERT_TRUE(lr0_bounds);
    ASSERT_TRUE(lr2_bounds);
    auto const leading_anchor_a = leading_layout->characterAnchorPoint(leading_layout->charIndexToIterator(0));
    auto const leading_anchor_b = leading_layout->characterAnchorPoint(leading_layout->charIndexToIterator(3));
    RecordProperty("leading_regionbreak.a_anchor", point_text(leading_anchor_a));
    RecordProperty("leading_regionbreak.b_anchor", point_text(leading_anchor_b));
    EXPECT_TRUE(lr0_bounds->contains(leading_anchor_a));
    EXPECT_TRUE(lr2_bounds->contains(leading_anchor_b));
    EXPECT_GE(leading_anchor_b.y(), lr2_bounds->top());
    EXPECT_EQ(serialize(*leading_doc), leading_before);

    // The final region may already be exhausted when another explicit break is
    // read. It must be consumed without indexing beyond the available shapes.
    auto overflow_doc = parse(std::string{svg_open} +
        R"svg(<flowRoot id="f" style="font-family:Arial;font-size:20px;stroke:black">)svg"
        R"svg(<flowRegion id="region"><rect width="400" height="200"/></flowRegion>)svg"
        R"svg(<flowPara>A<flowRegionBreak/><flowRegionBreak/>B</flowPara></flowRoot></svg>)svg");
    ASSERT_TRUE(overflow_doc);
    settle(*overflow_doc);
    auto const overflow_before = serialize(*overflow_doc);
    auto *overflow_flow = item(*overflow_doc, "f");
    ASSERT_TRUE(overflow_flow);
    ASSERT_EQ(native_char_count(overflow_flow), 4u);
    EXPECT_EQ(sp_te_get_string_multiline(overflow_flow).raw(), "A\n\nB");
    EXPECT_EQ(serialize(*overflow_doc), overflow_before);
}

// SW3 A2 VE-META-1: two logically distinct runs with identical computed numeric
// width converge on the native non-scaling bit but differ in full vector-effect
// value and !important priority. The read-only controller must keep them as two
// runs, carry the exact native value/set/inherit/important/source per run, and
// leave the SVG XML byte-identical. Native SPStyle is the independent input
// oracle, asserted before the controller is queried.
TEST_F(StrokeWidthControllerTest, SW3VectorEffectRunMetadataDistinct)
{
    auto document = parse(std::string{svg_open} +
        R"svg(<text id="t" x="10" y="20" transform="scale(2)" style="font-family:Arial;font-size:20px;stroke:black;stroke-width:2">)svg"
        R"svg(<tspan id="s1" style="vector-effect:non-scaling-stroke">AB</tspan>)svg"
        R"svg(<tspan id="s2" style="vector-effect:non-scaling-stroke fixed-position !important">CD</tspan>)svg"
        R"svg(</text></svg>)svg");
    ASSERT_TRUE(document);
    settle(*document);
    auto const before = serialize(*document);
    auto *text = item(*document, "t");
    auto *s1 = document->getObjectById("s1");
    auto *s2 = document->getObjectById("s2");
    ASSERT_TRUE(text);
    ASSERT_TRUE(s1 && s1->style);
    ASSERT_TRUE(s2 && s2->style);
    ASSERT_EQ(native_char_count(text), 4u);

    // Independent native input oracle before any controller call: each element's
    // parsed vector-effect value/priority and the renderer's per-character source.
    ASSERT_STREQ(s1->style->vector_effect.get_value().c_str(), "non-scaling-stroke");
    EXPECT_FALSE(s1->style->vector_effect.important);
    EXPECT_TRUE(s1->style->vector_effect.stroke);
    ASSERT_STREQ(s2->style->vector_effect.get_value().c_str(), "non-scaling-stroke fixed-position");
    EXPECT_TRUE(s2->style->vector_effect.important);
    EXPECT_TRUE(s2->style->vector_effect.stroke);

    auto const r0 = native_render_style_at(text, 0);
    auto const r1 = native_render_style_at(text, 1);
    auto const r2 = native_render_style_at(text, 2);
    auto const r3 = native_render_style_at(text, 3);
    ASSERT_TRUE(r0.style && r1.style && r2.style && r3.style);
    EXPECT_EQ(r0.element, static_cast<SPObject *>(s1));
    EXPECT_EQ(r1.element, static_cast<SPObject *>(s1));
    EXPECT_EQ(r2.element, static_cast<SPObject *>(s2));
    EXPECT_EQ(r3.element, static_cast<SPObject *>(s2));
    ASSERT_STREQ(r0.style->vector_effect.get_value().c_str(), "non-scaling-stroke");
    EXPECT_FALSE(r0.style->vector_effect.important);
    ASSERT_STREQ(r2.style->vector_effect.get_value().c_str(), "non-scaling-stroke fixed-position");
    EXPECT_TRUE(r2.style->vector_effect.important);
    for (unsigned i = 0; i < 4; ++i) {
        EXPECT_DOUBLE_EQ(native_char_stroke(text, i).width, 2.0);
    }

    auto const result = SW::query_stroke_widths(*document, {text});
    ASSERT_EQ(result.eligible, 1u);
    ASSERT_EQ(result.targets.size(), 1u);
    auto const &target = result.targets[0];
    ASSERT_EQ(target.kind, SW::StrokeWidthTargetKind::TextOwner);
    ASSERT_EQ(target.runs.size(), 2u);

    auto const &run_a = target.runs[0];
    auto const &run_b = target.runs[1];
    EXPECT_EQ(run_a.first_char, 0u);
    EXPECT_EQ(run_a.last_char, 2u);
    EXPECT_EQ(run_b.first_char, 2u);
    EXPECT_EQ(run_b.last_char, 4u);
    EXPECT_EQ(run_a.style_source.get(), static_cast<SPObject *>(s1));
    EXPECT_EQ(run_b.style_source.get(), static_cast<SPObject *>(s2));

    // Identical computed numeric width, convention and raw non-scaling bit.
    EXPECT_DOUBLE_EQ(run_a.style.local_computed, 2.0);
    EXPECT_DOUBLE_EQ(run_b.style.local_computed, 2.0);
    ASSERT_TRUE(run_a.style.effective_px.has_value());
    ASSERT_TRUE(run_b.style.effective_px.has_value());
    EXPECT_DOUBLE_EQ(*run_a.style.effective_px, 2.0);
    EXPECT_DOUBLE_EQ(*run_b.style.effective_px, 2.0);
    EXPECT_EQ(run_a.style.convention, SW::StrokeWidthConvention::NonScaling);
    EXPECT_EQ(run_b.style.convention, SW::StrokeWidthConvention::NonScaling);
    EXPECT_TRUE(run_a.style.non_scaling);
    EXPECT_TRUE(run_b.style.non_scaling);

    // Full native value and independent source/priority survive per run.
    EXPECT_EQ(run_a.style.vector_effect_value, "non-scaling-stroke");
    EXPECT_TRUE(run_a.style.vector_effect_set);
    EXPECT_FALSE(run_a.style.vector_effect_inherit);
    EXPECT_FALSE(run_a.style.vector_effect_important);
    EXPECT_EQ(run_a.style.vector_effect_style_src, static_cast<unsigned char>(SPStyleSrc::STYLE_PROP));
    EXPECT_EQ(run_b.style.vector_effect_value, "non-scaling-stroke fixed-position");
    EXPECT_TRUE(run_b.style.vector_effect_set);
    EXPECT_FALSE(run_b.style.vector_effect_inherit);
    EXPECT_TRUE(run_b.style.vector_effect_important);
    EXPECT_EQ(run_b.style.vector_effect_style_src, static_cast<unsigned char>(SPStyleSrc::STYLE_PROP));
    EXPECT_NE(run_a.style.vector_effect_value, run_b.style.vector_effect_value);

    EXPECT_EQ(serialize(*document), before);
}

// ---------------------------------------------------------------------------
// SW3-A3-PLAN-1: read-only prospective per-run numeric plans on a TextOwner.
// Two selected tspans at scale 2 - ordinary local 2 and non-scaling local 3 -
// must prepare prospective local 5 and 10 for absolute 10, while the top-level
// member stays Excluded/TextAdapterPending with no parent patch, no planned
// change and byte-identical XML. Native SPStyle is the independent input oracle
// asserted before the controller is queried.
// ---------------------------------------------------------------------------

TEST_F(StrokeWidthControllerTest, SW3A3TextRunPlanOrdinaryAndNonScalingProspective)
{
    auto document = parse(std::string{svg_open} +
        R"svg(<text id="t" x="10" y="20" transform="scale(2)" style="font-family:Arial;font-size:20px;stroke:black">)svg"
        R"svg(<tspan id="s1" style="stroke-width:2">AB</tspan>)svg"
        R"svg(<tspan id="s2" style="stroke-width:3;vector-effect:non-scaling-stroke">CD</tspan>)svg"
        R"svg(</text></svg>)svg");
    ASSERT_TRUE(document);
    settle(*document);
    auto const before = serialize(*document);
    auto *text = item(*document, "t");
    auto *s1 = document->getObjectById("s1");
    auto *s2 = document->getObjectById("s2");
    ASSERT_TRUE(text);
    ASSERT_TRUE(s1 && s1->style);
    ASSERT_TRUE(s2 && s2->style);
    ASSERT_EQ(native_char_count(text), 4u);

    // Independent native oracle before any controller call: raw SPStyle at the
    // actual rendering source, never the controller query.
    auto const render_a = native_render_style_at(text, 0);
    auto const render_b = native_render_style_at(text, 2);
    ASSERT_TRUE(render_a.style && render_b.style);
    EXPECT_EQ(render_a.element, static_cast<SPObject *>(s1));
    EXPECT_EQ(render_b.element, static_cast<SPObject *>(s2));
    EXPECT_DOUBLE_EQ(render_a.style->stroke_width.computed, 2.0);
    EXPECT_FALSE(render_a.style->vector_effect.stroke);
    EXPECT_DOUBLE_EQ(render_b.style->stroke_width.computed, 3.0);
    EXPECT_TRUE(render_b.style->vector_effect.stroke);
    EXPECT_DOUBLE_EQ(native_char_stroke(text, 0).width, 2.0);
    EXPECT_DOUBLE_EQ(native_char_stroke(text, 2).width, 3.0);

    auto const plan = SW::prepare_stroke_widths(*document, {text}, absolute(10.0), 1);
    EXPECT_EQ(plan.state, SW::StrokeWidthPlanState::Prepared);
    EXPECT_EQ(plan.planned_changes, 0u);

    auto const *member = find_member(plan, text);
    ASSERT_TRUE(member);
    EXPECT_EQ(member->outcome, SW::StrokeWidthMemberOutcome::Excluded);
    EXPECT_EQ(member->reason, SW::StrokeWidthMemberReason::TextAdapterPending);
    // No parent-level patch, no run-level dash write.
    EXPECT_FALSE(member->local_width.has_value());
    EXPECT_FALSE(member->local_dasharray.has_value());
    EXPECT_FALSE(member->local_dashoffset.has_value());
    ASSERT_EQ(member->target.runs.size(), 2u);
    ASSERT_EQ(member->text_runs.size(), 2u);

    auto const &run_a = member->text_runs[0];
    EXPECT_EQ(run_a.first_char, 0u);
    EXPECT_EQ(run_a.last_char, 2u);
    EXPECT_EQ(run_a.style_source.get(), static_cast<SPObject *>(s1));
    EXPECT_EQ(run_a.eligibility, SW::StrokeWidthEligibility::Eligible);
    EXPECT_EQ(run_a.exclusion, SW::StrokeWidthExclusion::None);
    EXPECT_EQ(run_a.outcome, SW::StrokeWidthMemberOutcome::Change);
    EXPECT_EQ(run_a.reason, SW::StrokeWidthMemberReason::None);
    ASSERT_TRUE(run_a.local_width.has_value());
    EXPECT_DOUBLE_EQ(*run_a.local_width, 5.0);
    EXPECT_DOUBLE_EQ(run_a.style.local_computed, 2.0);
    EXPECT_EQ(run_a.style.convention, SW::StrokeWidthConvention::Ordinary);

    auto const &run_b = member->text_runs[1];
    EXPECT_EQ(run_b.first_char, 2u);
    EXPECT_EQ(run_b.last_char, 4u);
    EXPECT_EQ(run_b.style_source.get(), static_cast<SPObject *>(s2));
    EXPECT_EQ(run_b.eligibility, SW::StrokeWidthEligibility::Eligible);
    EXPECT_EQ(run_b.outcome, SW::StrokeWidthMemberOutcome::Change);
    ASSERT_TRUE(run_b.local_width.has_value());
    EXPECT_DOUBLE_EQ(*run_b.local_width, 10.0);
    EXPECT_DOUBLE_EQ(run_b.style.local_computed, 3.0);
    EXPECT_EQ(run_b.style.convention, SW::StrokeWidthConvention::NonScaling);
    EXPECT_EQ(run_b.style.vector_effect_value, "non-scaling-stroke");

    EXPECT_EQ(serialize(*document), before);
}

// ---------------------------------------------------------------------------
// SW3-A3-PLAN-2: relative 200% prepares prospective local 4 and 6 on the same
// two-run owner, and a separate stroke:none / zero-width run at absolute 1e-20
// is still a prospective Change, never excluded for paint_none. Both remain
// top-level Excluded/TextAdapterPending with no XML write.
// ---------------------------------------------------------------------------

TEST_F(StrokeWidthControllerTest, SW3A3TextRunPlanRelativeAndPaintNoneZeroWidth)
{
    auto document = parse(std::string{svg_open} +
        R"svg(<text id="t" x="10" y="20" transform="scale(2)" style="font-family:Arial;font-size:20px;stroke:black">)svg"
        R"svg(<tspan id="s1" style="stroke-width:2">AB</tspan>)svg"
        R"svg(<tspan id="s2" style="stroke-width:3;vector-effect:non-scaling-stroke">CD</tspan>)svg"
        R"svg(</text></svg>)svg");
    ASSERT_TRUE(document);
    settle(*document);
    auto const before = serialize(*document);
    auto *text = item(*document, "t");
    ASSERT_TRUE(text);
    ASSERT_EQ(native_char_count(text), 4u);

    auto const plan = SW::prepare_stroke_widths(*document, {text}, relative(200.0), 1);
    EXPECT_EQ(plan.state, SW::StrokeWidthPlanState::Prepared);
    EXPECT_EQ(plan.planned_changes, 0u);
    auto const *member = find_member(plan, text);
    ASSERT_TRUE(member);
    EXPECT_EQ(member->outcome, SW::StrokeWidthMemberOutcome::Excluded);
    EXPECT_EQ(member->reason, SW::StrokeWidthMemberReason::TextAdapterPending);
    EXPECT_FALSE(member->local_width.has_value());
    ASSERT_EQ(member->text_runs.size(), 2u);
    EXPECT_EQ(member->text_runs[0].outcome, SW::StrokeWidthMemberOutcome::Change);
    ASSERT_TRUE(member->text_runs[0].local_width.has_value());
    EXPECT_DOUBLE_EQ(*member->text_runs[0].local_width, 4.0);
    EXPECT_EQ(member->text_runs[1].outcome, SW::StrokeWidthMemberOutcome::Change);
    ASSERT_TRUE(member->text_runs[1].local_width.has_value());
    EXPECT_DOUBLE_EQ(*member->text_runs[1].local_width, 6.0);
    EXPECT_EQ(serialize(*document), before);

    // A separate stroke:none owner with zero width: paint_none is not an
    // eligibility or exclusion input, so a representable 0 -> 1e-20 effective
    // change is still prepared prospectively.
    auto zero_doc = parse(std::string{svg_open} +
        R"svg(<text id="z" x="0" y="20" style="font-family:Arial;font-size:20px;stroke:none;stroke-width:0">XY</text>)svg"
        R"svg(</svg>)svg");
    ASSERT_TRUE(zero_doc);
    settle(*zero_doc);
    auto const zero_before = serialize(*zero_doc);
    auto *zt = item(*zero_doc, "z");
    ASSERT_TRUE(zt && zt->style);
    EXPECT_TRUE(zt->style->stroke.isNone());
    EXPECT_DOUBLE_EQ(zt->style->stroke_width.computed, 0.0);

    auto const zero_plan = SW::prepare_stroke_widths(*zero_doc, {zt}, absolute(1e-20), 2);
    EXPECT_EQ(zero_plan.planned_changes, 0u);
    auto const *zmember = find_member(zero_plan, zt);
    ASSERT_TRUE(zmember);
    EXPECT_EQ(zmember->outcome, SW::StrokeWidthMemberOutcome::Excluded);
    EXPECT_EQ(zmember->reason, SW::StrokeWidthMemberReason::TextAdapterPending);
    EXPECT_FALSE(zmember->local_width.has_value());
    ASSERT_EQ(zmember->text_runs.size(), 1u);
    auto const &zrun = zmember->text_runs[0];
    EXPECT_TRUE(zrun.style.paint_none);
    EXPECT_EQ(zrun.eligibility, SW::StrokeWidthEligibility::Eligible);
    EXPECT_EQ(zrun.outcome, SW::StrokeWidthMemberOutcome::Change);
    EXPECT_EQ(zrun.reason, SW::StrokeWidthMemberReason::None);
    ASSERT_TRUE(zrun.local_width.has_value());
    EXPECT_DOUBLE_EQ(*zrun.local_width, 1e-20);
    EXPECT_FALSE(zrun.local_dasharray.has_value());
    EXPECT_FALSE(zrun.local_dashoffset.has_value());
    EXPECT_EQ(serialize(*zero_doc), zero_before);
}

// ---------------------------------------------------------------------------
// SW3-A3-PLAN-3: an existing mixed shape+text apply still changes only the
// shape. The pending text member (with a prospective Change run) writes no
// width or dash, its XML is untouched, and the caller sees exactly one
// transaction that undoes to the baseline.
// ---------------------------------------------------------------------------

TEST_F(StrokeWidthControllerTest, SW3A3MixedShapeTextApplyOnlyChangesShape)
{
    auto document = parse(std::string{svg_open} +
        R"(<rect id="r" width="10" height="10" style="stroke-width:2"/>)"
        R"(<text id="t" x="10" y="20" style="font-family:Arial;font-size:20px;stroke:black;stroke-width:2">AB</text>)"
        R"(</svg>)");
    ASSERT_TRUE(document);
    settle(*document);
    auto const before = serialize(*document);
    auto *rect = item(*document, "r");
    auto *text = item(*document, "t");
    ASSERT_TRUE(rect);
    ASSERT_TRUE(text);
    auto const text_attrs_before = authored_attributes(text);
    char const *text_style = text->getRepr()->attribute("style");
    ASSERT_TRUE(text_style);
    std::string const text_style_before = text_style;

    auto const plan = SW::prepare_stroke_widths(*document, {rect, text}, absolute(10.0), 21);
    EXPECT_EQ(plan.state, SW::StrokeWidthPlanState::Prepared);
    EXPECT_EQ(plan.planned_changes, 1u);
    auto const *text_member = find_member(plan, text);
    ASSERT_TRUE(text_member);
    EXPECT_EQ(text_member->outcome, SW::StrokeWidthMemberOutcome::Excluded);
    EXPECT_EQ(text_member->reason, SW::StrokeWidthMemberReason::TextAdapterPending);
    ASSERT_EQ(text_member->text_runs.size(), 1u);
    EXPECT_EQ(text_member->text_runs[0].outcome, SW::StrokeWidthMemberOutcome::Change);

    auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
    ASSERT_TRUE(token);
    auto const result = SW::apply_stroke_widths(*document, plan, 21, *token);
    EXPECT_EQ(result.state, SW::StrokeWidthApplyState::Applied);
    EXPECT_EQ(result.reason, SW::StrokeWidthMemberReason::None);
    EXPECT_EQ(result.changed, 1u);
    EXPECT_EQ(result.attempted_writes, 1u);
    EXPECT_DOUBLE_EQ(computed_width(*document, "r"), 10.0);

    // The text owner keeps its exact authored style/metadata, gains no span and
    // still reads its authored width: the prospective run was never written.
    char const *text_style_after = text->getRepr()->attribute("style");
    ASSERT_TRUE(text_style_after);
    EXPECT_EQ(std::string(text_style_after), text_style_before);
    EXPECT_TRUE(authored_attributes(text) == text_attrs_before);
    EXPECT_EQ(count_tspans(text), 0u);
    EXPECT_DOUBLE_EQ(computed_width(*document, "t"), 2.0);

    EXPECT_TRUE(SW::stroke_widths_output_ready(*document, plan, 21, *token));
    EXPECT_TRUE(token->commitAtomically(Inkscape::Util::Internal::ContextString("Stroke width"), "", [&] {
        return SW::stroke_widths_output_ready(*document, plan, 21, *token);
    }));

    // Exactly one history entry; the second Undo finds nothing.
    ASSERT_TRUE(Inkscape::DocumentUndo::undo(document.get()));
    EXPECT_EQ(serialize(*document), before);
    EXPECT_FALSE(Inkscape::DocumentUndo::undo(document.get()));
    EXPECT_EQ(serialize(*document), before);
    token.reset();
}

// ---------------------------------------------------------------------------
// SW3-A4-CONTENT-1: an explicit reversed partial range raw [3,1) normalizes to
// [1,3) on a two-tspan owner and freezes the whole-owner native text-only string
// and logical count, while preserving raw/normalized range and direction and
// leaving XML byte-identical. Editing one untouched character outside [1,3) to a
// same-length different character changes only the semantic content (not the
// selected range spans/style), so apply rejects StalePlan with zero writes and
// no new history.
// ---------------------------------------------------------------------------

TEST_F(StrokeWidthControllerTest, SW3A4TextContentStaleOnUntouchedEqualLengthEdit)
{
    auto document = parse(std::string{svg_open} +
        R"svg(<text id="t" x="10" y="20" style="font-family:Arial;font-size:20px;stroke:black">)svg"
        R"svg(<tspan id="s1" style="stroke-width:2">ABCD</tspan>)svg"
        R"svg(<tspan id="s2" style="stroke-width:3">EFGH</tspan>)svg"
        R"svg(</text></svg>)svg");
    ASSERT_TRUE(document);
    settle(*document);
    auto const before = serialize(*document);
    auto *text = item(*document, "t");
    ASSERT_TRUE(text);
    ASSERT_EQ(native_char_count(text), 8u);
    EXPECT_EQ(sp_te_get_string_multiline(text).raw(), "ABCDEFGH");

    SW::StrokeWidthTextRange range;
    range.owner = text;
    range.caret = false;
    range.first_char = 3; // raw caller order is reversed
    range.last_char = 1;

    auto const plan = SW::prepare_stroke_widths(*document, range, {text}, absolute(10.0), 70);
    ASSERT_EQ(plan.state, SW::StrokeWidthPlanState::Prepared);
    auto const *member = find_member(plan, text);
    ASSERT_TRUE(member);
    auto const &target = member->target;
    // Raw caller range and direction preserved; normalized [1,3) used for scope.
    EXPECT_EQ(target.raw_first_char, 3u);
    EXPECT_EQ(target.raw_last_char, 1u);
    EXPECT_TRUE(target.reversed);
    EXPECT_EQ(target.first_char, 1u);
    EXPECT_EQ(target.last_char, 3u);
    // Whole-owner semantic content/count frozen, not just the selected run.
    EXPECT_TRUE(target.text_content_available);
    EXPECT_EQ(target.char_count, 8u);
    EXPECT_EQ(target.text_content, "ABCDEFGH");
    EXPECT_EQ(serialize(*document), before);

    // Same-length edit of an untouched character at index 5 (F -> X), outside
    // [1,3). Native text edit; layout is rebuilt by the native helper.
    auto *layout = te_get_layout(text);
    ASSERT_TRUE(layout);
    auto const at5 = layout->charIndexToIterator(5);
    auto const at6 = layout->charIndexToIterator(6);
    sp_te_replace(text, at5, at6, "X");
    document->ensureUpToDate();
    EXPECT_EQ(native_char_count(text), 8u);
    EXPECT_EQ(sp_te_get_string_multiline(text).raw(), "ABCDEXGH");

    // Clean history baseline after the fixture edit, then attempt the stale plan.
    settle(*document);
    auto const stale_before = serialize(*document);

    auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
    ASSERT_TRUE(token);
    auto const result = SW::apply_stroke_widths(*document, plan, 70, *token);
    EXPECT_EQ(result.state, SW::StrokeWidthApplyState::Rejected);
    EXPECT_EQ(result.reason, SW::StrokeWidthMemberReason::StalePlan);
    EXPECT_EQ(result.attempted_writes, 0u);
    EXPECT_EQ(serialize(*document), stale_before);
    token->rollback();

    // The rejected apply added no undo entry.
    EXPECT_FALSE(Inkscape::DocumentUndo::undo(document.get()));
    EXPECT_EQ(serialize(*document), stale_before);
}

// ---------------------------------------------------------------------------
// SW3-A4-CONTENT-2: a native style-only range write splits one run into several
// spans but leaves the whole-owner semantic text-only string and logical count
// equal. This claims only that semantic content/count are span-structure
// independent; it makes no claim that every other authored attribute survives.
// ---------------------------------------------------------------------------

TEST_F(StrokeWidthControllerTest, SW3A4StyleSplitPreservesSemanticContentCount)
{
    auto document = parse(std::string{svg_open} +
        R"svg(<text id="t" x="10" y="20" style="font-family:Arial;font-size:20px;stroke:black">ABCD</text>)svg"
        R"svg(</svg>)svg");
    ASSERT_TRUE(document);
    settle(*document);
    auto *text = item(*document, "t");
    ASSERT_TRUE(text);
    ASSERT_EQ(native_char_count(text), 4u);
    EXPECT_EQ(count_tspans(text), 0u);

    auto const before = SW::query_stroke_widths(*document, {text});
    ASSERT_EQ(before.eligible, 1u);
    ASSERT_EQ(before.targets.size(), 1u);
    auto const &before_target = before.targets[0];
    ASSERT_TRUE(before_target.text_content_available);
    EXPECT_EQ(before_target.text_content, "ABCD");
    EXPECT_EQ(before_target.char_count, 4u);
    ASSERT_EQ(before_target.runs.size(), 1u);

    // Native style-only write over [1,3) splits the single run into spans.
    ASSERT_NO_FATAL_FAILURE(native_apply_width(*document, text, 1, 3, "10"));
    EXPECT_GT(count_tspans(text), 0u);
    ASSERT_EQ(native_char_count(text), 4u);

    auto const after = SW::query_stroke_widths(*document, {text});
    ASSERT_EQ(after.eligible, 1u);
    ASSERT_EQ(after.targets.size(), 1u);
    auto const &after_target = after.targets[0];
    // Span structure changed, but the frozen semantic facts are equal.
    EXPECT_GT(after_target.runs.size(), before_target.runs.size());
    EXPECT_EQ(after_target.text_content_available, before_target.text_content_available);
    EXPECT_EQ(after_target.text_content, before_target.text_content);
    EXPECT_EQ(after_target.char_count, before_target.char_count);
    EXPECT_EQ(after_target.text_content, "ABCD");
    EXPECT_EQ(after_target.char_count, 4u);
}

// ---------------------------------------------------------------------------
// SW3-B0 T1: native sp_te_apply_style feasibility on two ordinary/NS tspans at
// owner scale 2 with distinct fonts and coordinates. Apply prospective local
// widths in DESCENDING logical range ([2,4)=10, then [0,2)=5) through the native
// routine directly, never the controller's pending writer, reacquiring layout
// each call. One caller transaction; buffer save/reopen is parse(serialize), not
// a filesystem round trip.
// ---------------------------------------------------------------------------

TEST_F(StrokeWidthControllerTest, SW3B0NativeTwoTspanDescendingLocalWidth)
{
    auto document = parse(std::string{svg_open} +
        R"svg(<text id="t" x="10" y="20" transform="scale(2)" style="font-family:Arial;font-size:20px;stroke:black;fill:none">)svg"
        R"svg(<tspan id="s1" x="10" y="20" style="stroke-width:2;vector-effect:none">AB</tspan>)svg"
        R"svg(<tspan id="s2" x="30" y="40" style="font-family:'Courier New';font-size:30px;stroke-width:3;vector-effect:non-scaling-stroke !important">CD</tspan>)svg"
        R"svg(</text></svg>)svg");
    ASSERT_TRUE(document);
    settle(*document);
    auto const before = serialize(*document);
    auto *text = item(*document, "t");
    auto *s1 = item(*document, "s1");
    auto *s2 = item(*document, "s2");
    ASSERT_TRUE(text);
    ASSERT_TRUE(s1);
    ASSERT_TRUE(s2);
    auto const text_attrs_before = authored_attributes(text);
    auto const s1_attrs_before = authored_attributes(s1);
    auto const s2_attrs_before = authored_attributes(s2);
    ASSERT_EQ(native_char_count(text), 4u);
    ASSERT_EQ(sp_te_get_string_multiline(text).raw(), "ABCD");

    struct Anchor { double x = 0.0; double y = 0.0; };
    std::vector<Anchor> anchors_before;
    for (unsigned i = 0; i < 4; ++i) {
        auto *layout = te_get_layout(text);
        ASSERT_TRUE(layout);
        auto const p = layout->characterAnchorPoint(layout->charIndexToIterator(static_cast<int>(i)));
        anchors_before.push_back({p.x(), p.y()});
    }

    // Independent native baseline: two ordinary/NS runs, two font sizes/families.
    for (unsigned i = 0; i < 4; ++i) {
        auto const widths = native_char_widths(*document, text, i);
        auto const stroke = native_char_stroke(text, i);
        if (i < 2) {
            expect_required_widths("t1_baseline_s1", i, widths, 2.0, 4.0);
            EXPECT_EQ(widths.convention, SW::StrokeWidthConvention::Ordinary);
            EXPECT_STREQ(stroke.vector_effect.c_str(), "none");
            EXPECT_FALSE(stroke.vector_effect_important);
        } else {
            expect_required_widths("t1_baseline_s2", i, widths, 3.0, 3.0);
            EXPECT_EQ(widths.convention, SW::StrokeWidthConvention::NonScaling);
            EXPECT_STREQ(stroke.vector_effect.c_str(), "non-scaling-stroke");
            EXPECT_TRUE(stroke.vector_effect_important);
        }
    }
    auto const identity0 = native_char_identity(text, 0);
    auto const identity2 = native_char_identity(text, 2);
    EXPECT_DOUBLE_EQ(identity0.font_size, 20.0);
    EXPECT_DOUBLE_EQ(identity2.font_size, 30.0);
    EXPECT_TRUE(identity0.font_family == "Arial");
    EXPECT_TRUE(identity2.font_family.find("Courier New") != Glib::ustring::npos);
    EXPECT_NE(identity0.font_family, identity2.font_family);

    auto make_patch = [](char const *width, FrozenVectorEffect const &ve) {
        auto css = Util::delete_with<sp_repr_css_attr_unref>(sp_repr_css_attr_new());
        sp_repr_css_set_property(css.get(), "stroke-width", width);
        std::string const ve_value = ve.important ? std::string(ve.value.raw()) + " !important"
                                                  : ve.value.raw();
        sp_repr_css_set_property(css.get(), "vector-effect", ve_value.c_str());
        return css;
    };

    TextStyleLocalStroke width_only;
    width_only.stroke_width = true;

    auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
    ASSERT_TRUE(token);

    // DESCENDING: later logical run first, layout reacquired inside the helper.
    auto const frozen_s2 = frozen_vector_effect(text, 2, 4);
    ASSERT_TRUE(frozen_s2.important);
    ASSERT_STREQ(frozen_s2.value.c_str(), "non-scaling-stroke");
    auto patch_s2 = make_patch("10", frozen_s2);
    auto const patch_s2_before = css_attr_map(patch_s2.get());
    ASSERT_NO_FATAL_FAILURE(native_apply_local_stroke(*document, text, 2, 4, patch_s2.get(), width_only));
    EXPECT_TRUE(css_attr_map(patch_s2.get()) == patch_s2_before);

    auto const frozen_s1 = frozen_vector_effect(text, 0, 2);
    ASSERT_FALSE(frozen_s1.important);
    ASSERT_STREQ(frozen_s1.value.c_str(), "none");
    auto patch_s1 = make_patch("5", frozen_s1);
    auto const patch_s1_before = css_attr_map(patch_s1.get());
    ASSERT_NO_FATAL_FAILURE(native_apply_local_stroke(*document, text, 0, 2, patch_s1.get(), width_only));
    EXPECT_TRUE(css_attr_map(patch_s1.get()) == patch_s1_before);

    // Per-character native width/VE oracle after the two local writes.
    for (unsigned i = 0; i < 4; ++i) {
        auto const widths = native_char_widths(*document, text, i);
        auto const stroke = native_char_stroke(text, i);
        if (i < 2) {
            expect_required_widths("t1_applied_s1", i, widths, 5.0, 10.0);
            EXPECT_EQ(widths.convention, SW::StrokeWidthConvention::Ordinary);
            EXPECT_STREQ(stroke.vector_effect.c_str(), "none");
            EXPECT_FALSE(stroke.vector_effect_important);
        } else {
            expect_required_widths("t1_applied_s2", i, widths, 10.0, 10.0);
            EXPECT_EQ(widths.convention, SW::StrokeWidthConvention::NonScaling);
            EXPECT_STREQ(stroke.vector_effect.c_str(), "non-scaling-stroke");
            EXPECT_TRUE(stroke.vector_effect_important);
        }
    }
    // Text, font and coordinate preservation.
    EXPECT_EQ(native_char_count(text), 4u);
    EXPECT_EQ(sp_te_get_string_multiline(text).raw(), "ABCD");
    EXPECT_TRUE(authored_attributes(text) == text_attrs_before);
    // Re-fetch by id: a native normalization that deleted/recreated a tspan must
    // surface as a missing object, not a stale-pointer read.
    auto *s1_after = item(*document, "s1");
    auto *s2_after = item(*document, "s2");
    ASSERT_TRUE(s1_after);
    ASSERT_TRUE(s2_after);
    EXPECT_TRUE(authored_attributes(s1_after) == s1_attrs_before);
    EXPECT_TRUE(authored_attributes(s2_after) == s2_attrs_before);
    for (unsigned i = 0; i < 4; ++i) {
        auto *layout = te_get_layout(text);
        ASSERT_TRUE(layout);
        auto const p = layout->characterAnchorPoint(layout->charIndexToIterator(static_cast<int>(i)));
        EXPECT_DOUBLE_EQ(p.x(), anchors_before[i].x);
        EXPECT_DOUBLE_EQ(p.y(), anchors_before[i].y);
        EXPECT_DOUBLE_EQ(native_char_identity(text, i).font_size, i < 2 ? 20.0 : 30.0);
    }

    // Exactly one Undo/Redo transaction for the two native writes.
    auto const applied = serialize(*document);
    EXPECT_TRUE(token->commitAtomically(Inkscape::Util::Internal::ContextString("SW3-B0 native width"), "",
                                        [] { return true; }));
    ASSERT_TRUE(Inkscape::DocumentUndo::undo(document.get()));
    EXPECT_EQ(serialize(*document), before);
    EXPECT_FALSE(Inkscape::DocumentUndo::undo(document.get()));
    EXPECT_EQ(serialize(*document), before);
    ASSERT_TRUE(Inkscape::DocumentUndo::redo(document.get()));
    EXPECT_EQ(serialize(*document), applied);
    text = item(*document, "t");
    ASSERT_TRUE(text);
    for (unsigned i = 0; i < 4; ++i) {
        auto const widths = native_char_widths(*document, text, i);
        expect_required_widths("t1_redo", i, widths, i < 2 ? 5.0 : 10.0, i < 2 ? 10.0 : 10.0);
    }

    // Buffer round trip: parse of the saved XML buffer (not a filesystem reopen).
    auto reopened = parse(applied);
    ASSERT_TRUE(reopened);
    auto *reopened_text = item(*reopened, "t");
    ASSERT_TRUE(reopened_text);
    ASSERT_EQ(native_char_count(reopened_text), 4u);
    for (unsigned i = 0; i < 4; ++i) {
        auto const widths = native_char_widths(*reopened, reopened_text, i);
        expect_required_widths("t1_reopen", i, widths, i < 2 ? 5.0 : 10.0, i < 2 ? 10.0 : 10.0);
    }
    token.reset();
}

// ---------------------------------------------------------------------------
// SW3-B0 T2: native width write on flowLine / flowRegionBreak fixtures applied
// to rendered glyph ranges [0,1) and [2,3). Control index 1 is the exclusive END
// boundary of the [0,1) write, so the range engine may touch the break; the
// per-stage snapshots below prove the break's authored attributes and inline
// style are nonetheless preserved. Distinct widths (B=7, A=11) make an unsafe
// end-boundary advance across the break visible as a width failure. Asserts
// native source mapping/count/text are unchanged and that no synthetic glyph is
// written for the control. The pre-write sp_text_get_length baseline is 4 (the
// flowPara itself counts as a line break) and is captured, never hard-coded.
// The known failing shapeIndex diagnostic stays in the separate, untouched
// SW3FlowSourceEmptyBreakControlIndices test; here it is recorded non-fatally.
// ---------------------------------------------------------------------------

TEST_F(StrokeWidthControllerTest, SW3B0NativeFlowGlyphRangesExcludeControl)
{
    auto run_fixture = [&](char const *label, char const *region_svg, char const *control_svg) {
        SCOPED_TRACE(label);
        auto document = parse(std::string{svg_open} +
            R"svg(<flowRoot id="f" transform="scale(2)" style="font-family:Arial;font-size:20px;stroke:black">)svg"
            + region_svg +
            R"svg(<flowPara id="p" style="stroke-width:3;vector-effect:non-scaling-stroke !important">A)svg"
            + control_svg +
            R"svg(B</flowPara></flowRoot></svg>)svg");
        ASSERT_TRUE(document);
        settle(*document);
        auto const before = serialize(*document);
        auto *flow = item(*document, "f");
        auto *p = document->getObjectById("p");
        auto *br = document->getObjectById("br");
        ASSERT_TRUE(flow);
        ASSERT_TRUE(p && p->style);
        ASSERT_TRUE(br);
        ASSERT_EQ(native_char_count(flow), 3u);
        std::string const text_before = sp_te_get_string_multiline(flow).raw();
        RecordProperty(std::string(label) + ".text", text_before);

        // Read-only baseline: A/B are real glyphs from p, index 1 is the break.
        expect_flow_string_char("t2_baseline_a", *document, flow, 0, 'A', p, "non-scaling-stroke",
                                true, 3.0, 3.0, SW::StrokeWidthConvention::NonScaling);
        expect_flow_break(flow, br, 1, "t2_baseline_break");
        expect_flow_string_char("t2_baseline_b", *document, flow, 2, 'B', p, "non-scaling-stroke",
                                true, 3.0, 3.0, SW::StrokeWidthConvention::NonScaling);
        {
            auto *layout = te_get_layout(flow);
            ASSERT_TRUE(layout);
            RecordProperty(std::string(label) + ".shapeIndex.before",
                           std::to_string(layout->shapeIndex(layout->charIndexToIterator(0))));
            RecordProperty(std::string(label) + ".shapeIndex.before2",
                           std::to_string(layout->shapeIndex(layout->charIndexToIterator(2))));
        }

        // Break-preservation oracle: exact authored repr attributes, parsed
        // inline style, native multiline content, layout count and
        // sp_text_get_length, captured before any write and re-checked after
        // each write. Both flow-break classes are SPObject, not SPItem.
        struct BreakSnapshot {
            std::vector<std::pair<std::string, std::string>> attrs;
            std::map<std::string, std::string> style;
            std::string content;
            unsigned layout_count = 0;
            unsigned text_length = 0;
            bool captured = false;
        };
        auto capture_state = [&]() {
            BreakSnapshot snap;
            br = document->getObjectById("br");
            flow = item(*document, "f");
            EXPECT_TRUE(br);
            EXPECT_TRUE(flow);
            if (!br || !flow) return snap;
            snap.attrs = authored_attributes(br);
            snap.style = inline_style_map(br);
            snap.content = sp_te_get_string_multiline(flow).raw();
            snap.layout_count = native_char_count(flow);
            snap.text_length = sp_text_get_length(flow);
            snap.captured = true;
            return snap;
        };
        auto snapshot_text = [](BreakSnapshot const &snap) {
            std::string out = "attrs{";
            for (auto const &attr : snap.attrs) out += attr.first + "=" + attr.second + ";";
            out += "}style{";
            for (auto const &attr : snap.style) out += attr.first + ":" + attr.second + ";";
            out += "}content{" + snap.content + "}layoutCount{" + std::to_string(snap.layout_count)
                   + "}spTextGetLength{" + std::to_string(snap.text_length) + "}";
            return out;
        };
        auto expect_state = [&](char const *stage, BreakSnapshot const &baseline) {
            auto const observed = capture_state();
            EXPECT_TRUE(observed.captured);
            if (!baseline.captured || !observed.captured) return;
            EXPECT_TRUE(observed.attrs == baseline.attrs)
                << "stage " << stage << " break authored attributes changed: baseline="
                << snapshot_text(baseline) << " observed=" << snapshot_text(observed);
            EXPECT_TRUE(observed.style == baseline.style)
                << "stage " << stage << " break inline style changed: baseline="
                << snapshot_text(baseline) << " observed=" << snapshot_text(observed);
            EXPECT_EQ(observed.content, baseline.content);
            EXPECT_EQ(observed.layout_count, baseline.layout_count);
            EXPECT_EQ(observed.text_length, baseline.text_length);
            RecordProperty(std::string(label) + ".break." + stage + ".baseline", snapshot_text(baseline));
            RecordProperty(std::string(label) + ".break." + stage + ".observed", snapshot_text(observed));
        };

        BreakSnapshot const break_before = capture_state();
        ASSERT_TRUE(break_before.captured);
        // Pre-write native length oracle: flowPara + A + break + B = 4. Stop if
        // the baseline is not 4 before any write is attempted.
        ASSERT_EQ(break_before.text_length, 4u);
        ASSERT_EQ(break_before.layout_count, 3u);
        EXPECT_EQ(break_before.content, text_before);
        RecordProperty(std::string(label) + ".spTextGetLength.baseline",
                       std::to_string(break_before.text_length));
        RecordProperty(std::string(label) + ".break.baseline", snapshot_text(break_before));

        TextStyleLocalStroke width_only;
        width_only.stroke_width = true;
        // Opt in: the [0,1) write's exclusive end index is the break itself, so
        // keep the endpoint at that break instead of advancing into the next
        // sibling. This is the caller's request to leave the control unstyled.
        width_only.exclude_end_line_break = true;
        auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
        ASSERT_TRUE(token);

        auto apply_glyph_range = [&](unsigned lo, unsigned hi, char const *value) {
            flow = item(*document, "f");
            ASSERT_TRUE(flow);
            auto const frozen = frozen_vector_effect(flow, lo, hi);
            ASSERT_TRUE(frozen.important);
            ASSERT_STREQ(frozen.value.c_str(), "non-scaling-stroke");
            auto patch = Util::delete_with<sp_repr_css_attr_unref>(sp_repr_css_attr_new());
            sp_repr_css_set_property(patch.get(), "stroke-width", value);
            std::string const ve = std::string(frozen.value.raw()) + " !important";
            sp_repr_css_set_property(patch.get(), "vector-effect", ve.c_str());
            ASSERT_NO_FATAL_FAILURE(native_apply_local_stroke(*document, flow, lo, hi, patch.get(),
                                                              width_only));
        };

        // DESCENDING glyph ranges. Index 1 is the exclusive end boundary of the
        // [0,1) write, so the engine can touch break index 1; each stage
        // re-checks the break's authored attributes/inline style, content,
        // layout count and length against the pre-write baseline.
        apply_glyph_range(2, 3, "7");
        expect_state("after_first_write", break_before);
        apply_glyph_range(0, 1, "11");
        expect_state("after_second_write", break_before);
        flow = item(*document, "f");
        ASSERT_TRUE(flow);
        br = document->getObjectById("br");
        ASSERT_TRUE(br);

        // Count/text/source mapping preservation and no synthetic glyph.
        ASSERT_EQ(native_char_count(flow), 3u);
        EXPECT_EQ(sp_te_get_string_multiline(flow).raw(), text_before);
        EXPECT_EQ(sp_text_get_length(flow), break_before.text_length);
        auto const control_after = native_char_source(flow, 1);
        ASSERT_TRUE(control_after.valid);
        EXPECT_FALSE(control_after.has_glyph);
        ASSERT_TRUE(control_after.source);
        EXPECT_STREQ(control_after.source->getId(), "br");
        EXPECT_FALSE(is<SPString>(control_after.source));
        unsigned glyph_count = 0;
        for (unsigned i = 0; i < 3; ++i) {
            if (native_char_source(flow, i).has_glyph) ++glyph_count;
        }
        EXPECT_EQ(glyph_count, 2u);
        for (unsigned i : {0u, 2u}) {
            auto const glyph = native_char_source(flow, i);
            ASSERT_TRUE(glyph.has_glyph);
            EXPECT_TRUE(glyph.source && is<SPString>(glyph.source));
        }
        // Distinct per-glyph widths prove the control index separated the writes.
        for (unsigned i : {0u, 2u}) {
            auto const widths = native_char_widths(*document, flow, i);
            expect_required_widths(label, i, widths, i == 0 ? 11.0 : 7.0, i == 0 ? 11.0 : 7.0);
            EXPECT_EQ(widths.convention, SW::StrokeWidthConvention::NonScaling);
        }
        {
            auto *layout = te_get_layout(flow);
            ASSERT_TRUE(layout);
            RecordProperty(std::string(label) + ".shapeIndex.after",
                           std::to_string(layout->shapeIndex(layout->charIndexToIterator(0))));
            RecordProperty(std::string(label) + ".shapeIndex.after2",
                           std::to_string(layout->shapeIndex(layout->charIndexToIterator(2))));
        }

        token->rollback();
        EXPECT_EQ(serialize(*document), before);
    };

    run_fixture("flowline",
                R"sv(<flowRegion id="region"><rect id="r" width="400" height="400"/></flowRegion>)sv",
                R"sv(<flowLine id="br"/>)sv");
    run_fixture("flowregionbreak",
                R"sv(<flowRegion id="region"><rect id="r0" width="400" height="200"/><rect id="r1" y="200" width="400" height="200"/></flowRegion>)sv",
                R"sv(<flowRegionBreak id="br"/>)sv");
}

// SW3-B0 default-behavior guard: the exclude_end_line_break opt-in must not
// change the default for any other native caller. With the flag left false, the
// same [0,1) exclusive end on a flowLine / flowRegionBreak still advances to the
// next sibling and the control receives the inline style exactly as before.
// This separate guard freezes the default; the B0 test itself opts in.
TEST_F(StrokeWidthControllerTest, SW3B0DefaultEndBreakStillAdvanced)
{
    auto run_fixture = [&](char const *label, char const *region_svg, char const *control_svg) {
        SCOPED_TRACE(label);
        auto document = parse(std::string{svg_open} +
            R"svg(<flowRoot id="f" transform="scale(2)" style="font-family:Arial;font-size:20px;stroke:black">)svg"
            + region_svg +
            R"svg(<flowPara id="p" style="stroke-width:3;vector-effect:non-scaling-stroke !important">A)svg"
            + control_svg +
            R"svg(B</flowPara></flowRoot></svg>)svg");
        ASSERT_TRUE(document);
        settle(*document);
        auto const before = serialize(*document);
        auto *flow = item(*document, "f");
        auto *br = document->getObjectById("br");
        ASSERT_TRUE(flow);
        ASSERT_TRUE(br);

        // Default construction path: exclude_end_line_break is left false.
        TextStyleLocalStroke width_only;
        width_only.stroke_width = true;

        auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
        ASSERT_TRUE(token);
        auto const frozen = frozen_vector_effect(flow, 0, 1);
        ASSERT_TRUE(frozen.important);
        ASSERT_STREQ(frozen.value.c_str(), "non-scaling-stroke");
        auto patch = Util::delete_with<sp_repr_css_attr_unref>(sp_repr_css_attr_new());
        sp_repr_css_set_property(patch.get(), "stroke-width", "11");
        std::string const ve = std::string(frozen.value.raw()) + " !important";
        sp_repr_css_set_property(patch.get(), "vector-effect", ve.c_str());
        ASSERT_NO_FATAL_FAILURE(native_apply_local_stroke(*document, flow, 0, 1, patch.get(),
                                                          width_only));

        br = document->getObjectById("br");
        ASSERT_TRUE(br);
        auto const br_style = inline_style_map(br);
        auto const width = br_style.find("stroke-width");
        ASSERT_NE(width, br_style.end()) << "default behavior no longer advances past the break";
        EXPECT_EQ(width->second, "11");
        auto const ve_it = br_style.find("vector-effect");
        ASSERT_NE(ve_it, br_style.end());
        EXPECT_EQ(ve_it->second, "non-scaling-stroke !important");

        token->rollback();
        EXPECT_EQ(serialize(*document), before);
    };

    run_fixture("flowline",
                R"sv(<flowRegion id="region"><rect id="r" width="400" height="400"/></flowRegion>)sv",
                R"sv(<flowLine id="br"/>)sv");
    run_fixture("flowregionbreak",
                R"sv(<flowRegion id="region"><rect id="r0" width="400" height="200"/><rect id="r1" y="200" width="400" height="200"/></flowRegion>)sv",
                R"sv(<flowRegionBreak id="br"/>)sv");
}

// SW3-B1: the native multiline helper must flush a pending break before a direct
// SPString child, so it matches the already-working wrapped (flowSpan) form. This
// is read-only (no style write, no Undo, no controller query) and compares both
// flowLine and flowRegionBreak fixtures. It also guards that a trailing empty break
// adds no synthetic newline. The SW3-B0 fixed-length oracle was repaired in that
// test (pre-write sp_text_get_length baseline captured) and is not re-checked here.
TEST_F(StrokeWidthControllerTest, SW3B1MultilineBreakFlushDirectMatchesWrapped)
{
    char const *flowline_region =
        R"sv(<flowRegion id="region"><rect id="r" width="400" height="400"/></flowRegion>)sv";
    char const *regionbreak_region =
        R"sv(<flowRegion id="region"><rect id="r0" width="400" height="200"/><rect id="r1" y="200" width="400" height="200"/></flowRegion>)sv";

    // Builds `A` + break + optional trailing markup and records the native
    // multiline text. The document is serialized before and after the helper call
    // to prove no mutation. ASSERT_* needs a void-returning lambda.
    auto multiline = [&](char const *label, char const *region_svg, char const *break_svg,
                         char const *after_svg, std::string *out) {
        SCOPED_TRACE(label);
        auto document = parse(std::string{svg_open} +
            R"svg(<flowRoot id="f" style="font-family:Arial;font-size:20px;stroke:black">)svg"
            + region_svg +
            R"svg(<flowPara id="p">A)svg" + break_svg + after_svg +
            R"svg(</flowPara></flowRoot></svg>)svg");
        ASSERT_TRUE(document);
        settle(*document);
        auto *flow = item(*document, "f");
        ASSERT_TRUE(flow);
        auto const before = serialize(*document);
        *out = sp_te_get_string_multiline(flow).raw();
        EXPECT_EQ(serialize(*document), before);
    };

    std::string direct_line, wrapped_line, direct_region, wrapped_region, trailing_line, trailing_region;
    multiline("direct_flowline", flowline_region, R"sv(<flowLine id="br"/>)sv", R"sv(B)sv",
              &direct_line);
    multiline("wrapped_flowline", flowline_region,
              R"sv(<flowLine id="br"/><flowSpan>B</flowSpan>)sv", "", &wrapped_line);
    multiline("direct_regionbreak", regionbreak_region,
              R"sv(<flowRegionBreak id="br"/>)sv", R"sv(B)sv", &direct_region);
    multiline("wrapped_regionbreak", regionbreak_region,
              R"sv(<flowRegionBreak id="br"/><flowSpan>B</flowSpan>)sv", "", &wrapped_region);
    multiline("trailing_flowline", flowline_region, R"sv(<flowLine id="br"/>)sv", "",
              &trailing_line);
    multiline("trailing_regionbreak", regionbreak_region, R"sv(<flowRegionBreak id="br"/>)sv", "",
              &trailing_region);

    EXPECT_EQ(direct_line, "A\nB");
    EXPECT_EQ(wrapped_line, "A\nB");
    EXPECT_EQ(direct_line, wrapped_line);
    EXPECT_EQ(direct_region, "A\nB");
    EXPECT_EQ(wrapped_region, "A\nB");
    EXPECT_EQ(direct_region, wrapped_region);

    // A break with nothing after it must not emit a trailing synthetic newline.
    EXPECT_EQ(trailing_line, "A");
    EXPECT_EQ(trailing_region, "A");
}

// SW3-B1b: a retained but empty direct SPString must not consume a pending break.
// With xml:space="preserve" a trailing LF-only text node survives the XML reader
// as an SPString whose content collapses to empty. It contributes no text, so it
// must leave a preceding break pending: no trailing '\n' for the empty case
// (matching the no-child case), and exactly one '\n' before the next real text.
TEST_F(StrokeWidthControllerTest, SW3B1MultilineEmptyStringDoesNotConsumeBreak)
{
    char const *region =
        R"sv(<flowRegion id="region"><rect id="r" width="400" height="400"/></flowRegion>)sv";

    // `trailing_text` is appended inside flowPara after the break. "\n" is a
    // whitespace-only node kept by xml:space="preserve" whose content collapses
    // to empty because it is last; "B" is a normal direct nonempty SPString.
    // (An empty node followed by a nonempty direct sibling cannot be parsed:
    // adjacent XML text nodes merge, so the nonempty flush is a separate case.)
    auto build = [&](std::string const &trailing_text) {
        return parse(std::string{svg_open} +
            R"svg(<flowRoot id="f" style="font-family:Arial;font-size:20px;stroke:black">)svg"
            + region +
            R"svg(<flowPara id="p" xml:space="preserve">A<flowLine id="br"/>)svg" + trailing_text +
            R"svg(</flowPara></flowRoot></svg>)svg");
    };

    auto empty_doc = build("\n");
    auto plain_doc = build("");
    auto following_doc = build("B");
    ASSERT_TRUE(empty_doc && plain_doc && following_doc);
    settle(*empty_doc);
    settle(*plain_doc);
    settle(*following_doc);

    // Independent native objects before the multiline helper runs: the empty
    // fixture retains an SPString child whose text is genuinely empty, the
    // no-child fixture does not, and the following fixture holds real text.
    auto *empty_para = empty_doc->getObjectById("p");
    auto *plain_para = plain_doc->getObjectById("p");
    auto *following_para = following_doc->getObjectById("p");
    ASSERT_TRUE(empty_para && plain_para && following_para);
    ASSERT_TRUE(empty_para->lastChild());
    EXPECT_TRUE(is<SPString>(empty_para->lastChild()));
    EXPECT_TRUE(cast<SPString>(empty_para->lastChild())->string.empty());
    ASSERT_TRUE(plain_para->lastChild());
    EXPECT_FALSE(is<SPString>(plain_para->lastChild()));
    ASSERT_TRUE(following_para->lastChild());
    ASSERT_TRUE(is<SPString>(following_para->lastChild()));
    EXPECT_EQ(cast<SPString>(following_para->lastChild())->string.raw(), "B");

    auto *flow_empty = item(*empty_doc, "f");
    auto *flow_plain = item(*plain_doc, "f");
    auto *flow_following = item(*following_doc, "f");
    ASSERT_TRUE(flow_empty && flow_plain && flow_following);
    auto const before = serialize(*empty_doc);

    EXPECT_EQ(sp_te_get_string_multiline(flow_empty).raw(), "A");
    EXPECT_EQ(sp_te_get_string_multiline(flow_plain).raw(), "A");
    EXPECT_EQ(sp_te_get_string_multiline(flow_following).raw(), "A\nB");
    EXPECT_EQ(serialize(*empty_doc), before);
}

// SW3-B0 paragraph-endpoint diagnostic (test-only; root runs it). Hypothesis:
// with `exclude_end_line_break == true`, a half-open range that selects the last
// glyph of a middle paragraph (A and B in separate flowPara siblings) and ends
// exactly at B's paragraph-break index leaves `end_item` at the `flowPara` that
// CONTAINS B, so B itself is skipped instead of styled. This is the minimal
// three-paragraph flowRoot from the task. Native layout/source indices are
// discovered and asserted before any write; if B is skipped or an index/source
// changes, the width/content assertions fail loudly with recorded actuals.
TEST_F(StrokeWidthControllerTest, SW3B0NativeFlowParagraphEndpointExcludeDiagnostic)
{
    auto document = parse(std::string{svg_open} +
        R"svg(<flowRoot id="f" style="font-family:Arial;font-size:20px;stroke:black">)svg"
        R"svg(<flowRegion id="region"><rect id="r" width="400" height="400"/></flowRegion>)svg"
        R"svg(<flowPara id="pa" style="stroke-width:3">A</flowPara>)svg"
        R"svg(<flowPara id="pb" style="stroke-width:3">B</flowPara>)svg"
        R"svg(<flowPara id="pc" style="stroke-width:3">C</flowPara>)svg"
        R"svg(</flowRoot></svg>)svg");
    ASSERT_TRUE(document);
    settle(*document);
    auto const before = serialize(*document);
    RecordProperty("paragraph_endpoint.baseline.svg", before);

    auto *flow = item(*document, "f");
    auto *pa = document->getObjectById("pa");
    auto *pb = document->getObjectById("pb");
    auto *pc = document->getObjectById("pc");
    ASSERT_TRUE(flow);
    ASSERT_TRUE(pa && pa->style);
    ASSERT_TRUE(pb && pb->style);
    ASSERT_TRUE(pc && pc->style);

    // Independent native text oracle before any write.
    EXPECT_EQ(sp_te_get_string_multiline(flow).raw(), "A\nB\nC");
    unsigned const count = native_char_count(flow);
    RecordProperty("paragraph_endpoint.layoutCount", std::to_string(count));

    // 1. Discover the native layout indices (never guessed): three glyphs A/B/C
    //    parented by pa/pb/pc plus the two paragraph-break objects between them.
    struct CharFact {
        unsigned index = 0;
        bool has_glyph = false;
        gunichar value = 0;
        SPObject *source = nullptr;
        SPObject *parent = nullptr;
    };
    std::vector<CharFact> facts;
    for (unsigned i = 0; i < count; ++i) {
        auto const s = native_char_source(flow, i);
        ASSERT_TRUE(s.valid);
        facts.push_back({i, s.has_glyph, s.value, s.source, s.parent});
        record_native_char("paragraph_endpoint.baseline", i, s);
    }
    std::vector<CharFact const *> glyphs;
    std::vector<CharFact const *> breaks;
    for (auto const &f : facts) {
        (f.has_glyph ? glyphs : breaks).push_back(&f);
    }
    ASSERT_EQ(glyphs.size(), 3u);
    // At least one break per paragraph boundary; a trailing break on the last
    // paragraph is tolerated and simply not used by this diagnostic.
    ASSERT_GE(breaks.size(), 2u);
    EXPECT_EQ(glyphs[0]->value, 'A');
    EXPECT_EQ(glyphs[1]->value, 'B');
    EXPECT_EQ(glyphs[2]->value, 'C');
    EXPECT_EQ(glyphs[0]->parent, pa);
    EXPECT_EQ(glyphs[1]->parent, pb);
    EXPECT_EQ(glyphs[2]->parent, pc);
    for (auto const *g : glyphs) {
        ASSERT_TRUE(g->source && is<SPString>(g->source));
    }
    // Structural index expectation for the minimal A/B/C paragraph flow.
    EXPECT_EQ(count, 5u);
    EXPECT_EQ(glyphs[0]->index, 0u);
    EXPECT_EQ(glyphs[1]->index, 2u);
    EXPECT_EQ(glyphs[2]->index, 4u);
    EXPECT_EQ(breaks[0]->index, 1u);
    EXPECT_EQ(breaks[1]->index, 3u);

    // 2. The exclusive end is B's paragraph-break index: the first non-glyph
    //    between B's glyph and C's glyph. Establish its native source identity.
    std::optional<unsigned> break_after_b;
    for (unsigned i = glyphs[1]->index + 1; i < glyphs[2]->index; ++i) {
        if (!facts[i].has_glyph) {
            break_after_b = i;
            break;
        }
    }
    ASSERT_TRUE(break_after_b.has_value())
        << "no paragraph-break index between B and C; fixture cannot establish the endpoint";
    unsigned const hi = *break_after_b;
    ASSERT_LT(hi, count);
    auto const b_break = native_char_source(flow, hi);
    ASSERT_TRUE(b_break.valid);
    EXPECT_FALSE(b_break.has_glyph);
    ASSERT_TRUE(b_break.source);
    EXPECT_FALSE(is<SPString>(b_break.source));
    // Independent index/source identity for A's paragraph break too.
    ASSERT_TRUE(breaks[0]->source);
    EXPECT_FALSE(is<SPString>(breaks[0]->source));
    RecordProperty("paragraph_endpoint.aBreakIndex", std::to_string(breaks[0]->index));
    RecordProperty("paragraph_endpoint.aBreakSourceId",
                   breaks[0]->source->getId() ? breaks[0]->source->getId() : "none");
    EXPECT_EQ(breaks[0]->source, static_cast<SPObject *>(pa));
    RecordProperty("paragraph_endpoint.bBreakIndex", std::to_string(hi));
    RecordProperty("paragraph_endpoint.bBreakSourceId",
                   b_break.source->getId() ? b_break.source->getId() : "none");
    RecordProperty("paragraph_endpoint.bBreakSourceIsPb", b_break.source == pb ? "true" : "false");
    // Hypothesis-critical identity: the endpoint break is the flowPara containing B.
    EXPECT_EQ(b_break.source, static_cast<SPObject *>(pb));

    unsigned const lo = glyphs[0]->index;

    // 3. Baseline widths: all three authored 3px ordinary.
    for (unsigned i : {glyphs[0]->index, glyphs[1]->index, glyphs[2]->index}) {
        auto const w = native_char_widths(*document, flow, i);
        ASSERT_DOUBLE_EQ(w.local, 3.0);
        ASSERT_TRUE(w.effective.has_value());
        ASSERT_DOUBLE_EQ(*w.effective, 3.0);
        ASSERT_EQ(w.convention, SW::StrokeWidthConvention::Ordinary);
    }

    auto const pa_attrs = authored_attributes(pa);
    auto const pb_attrs = authored_attributes(pb);
    auto const pc_attrs = authored_attributes(pc);
    std::string const text_before = sp_te_get_string_multiline(flow).raw();

    // 4. One atomic interaction; local stroke write on the half-open range that
    //    selects A and B and ends exactly at B's paragraph-break index.
    TextStyleLocalStroke width_only;
    width_only.stroke_width = true;
    width_only.exclude_end_line_break = true;
    auto patch = Util::delete_with<sp_repr_css_attr_unref>(sp_repr_css_attr_new());
    sp_repr_css_set_property(patch.get(), "stroke-width", "11");
    auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
    ASSERT_TRUE(token);
    ASSERT_NO_FATAL_FAILURE(native_apply_local_stroke(*document, flow, lo, hi, patch.get(),
                                                      width_only));

    // 5. Outcome: A and B must both get 11; C stays 3. If B is skipped, the
    //    width assertion below fails with the actual width recorded; nothing is
    //    relaxed.
    flow = item(*document, "f");
    ASSERT_TRUE(flow);
    EXPECT_EQ(sp_te_get_string_multiline(flow).raw(), text_before);
    EXPECT_EQ(native_char_count(flow), count);
    for (unsigned i : {glyphs[0]->index, glyphs[1]->index}) {
        auto const w = native_char_widths(*document, flow, i);
        record_char_widths("paragraph_endpoint.after", i, w);
        EXPECT_DOUBLE_EQ(w.local, 11.0) << "selected paragraph char " << i << " was not styled";
        ASSERT_TRUE(w.effective.has_value());
        EXPECT_DOUBLE_EQ(*w.effective, 11.0);
    }
    {
        auto const w = native_char_widths(*document, flow, glyphs[2]->index);
        record_char_widths("paragraph_endpoint.after", glyphs[2]->index, w);
        EXPECT_DOUBLE_EQ(w.local, 3.0) << "unselected third paragraph changed";
        ASSERT_TRUE(w.effective.has_value());
        EXPECT_DOUBLE_EQ(*w.effective, 3.0);
    }
    EXPECT_TRUE(authored_attributes(pa) == pa_attrs);
    EXPECT_TRUE(authored_attributes(pb) == pb_attrs);
    EXPECT_TRUE(authored_attributes(pc) == pc_attrs);

    // 6. One rollback restores the authored XML exactly.
    token->rollback();
    EXPECT_EQ(serialize(*document), before);
}

// ---------------------------------------------------------------------------
// SW3-B1-APPLY-1: the first production text-only writer. Two ordinary tspans
// with distinct font/size at owner scale 2, absolute 10 -> both local 5. The
// controller writes runs in DESCENDING logical order through sp_te_apply_style,
// then the post-write verifier proves per-character local 5/ordinary, semantic
// content/count and format preserved. One caller-owned token commits, Undo/Redo
// round-trips, and a serialized buffer reopens with the same rendered widths.
// ---------------------------------------------------------------------------

TEST_F(StrokeWidthControllerTest, SW3B1ApplyTextTwoRunDescendingLocalWidth)
{
    auto document = parse(std::string{svg_open} +
        R"svg(<text id="t" x="10" y="20" transform="scale(2)" style="font-family:Arial;font-size:20px;stroke:black;fill:none">)svg"
        R"svg(<tspan id="s1" x="10" y="20" style="stroke-width:2;vector-effect:none">AB</tspan>)svg"
        R"svg(<tspan id="s2" x="30" y="40" style="font-family:'Courier New';font-size:30px;stroke-width:3">CD</tspan>)svg"
        R"svg(</text></svg>)svg");
    ASSERT_TRUE(document);
    settle(*document);
    auto const before = serialize(*document);
    auto *text = item(*document, "t");
    ASSERT_TRUE(text);
    ASSERT_EQ(native_char_count(text), 4u);
    ASSERT_EQ(sp_te_get_string_multiline(text).raw(), "ABCD");

    for (unsigned i = 0; i < 4; ++i) {
        EXPECT_EQ(native_char_widths(*document, text, i).convention,
                  SW::StrokeWidthConvention::Ordinary);
    }
    EXPECT_DOUBLE_EQ(native_char_widths(*document, text, 0).local, 2.0);
    EXPECT_DOUBLE_EQ(native_char_widths(*document, text, 2).local, 3.0);
    auto const id0 = native_char_identity(text, 0);
    auto const id2 = native_char_identity(text, 2);
    EXPECT_DOUBLE_EQ(id0.font_size, 20.0);
    EXPECT_DOUBLE_EQ(id2.font_size, 30.0);
    EXPECT_NE(id0.font_family, id2.font_family);

    auto const plan = SW::prepare_stroke_widths(*document, {text}, absolute(10.0), 501);
    ASSERT_EQ(plan.state, SW::StrokeWidthPlanState::Prepared);
    EXPECT_EQ(plan.planned_changes, 0u);
    auto const *member = find_member(plan, text);
    ASSERT_TRUE(member);
    EXPECT_EQ(member->outcome, SW::StrokeWidthMemberOutcome::Excluded);
    EXPECT_EQ(member->reason, SW::StrokeWidthMemberReason::TextAdapterPending);
    ASSERT_EQ(member->text_runs.size(), 2u);
    EXPECT_EQ(member->text_runs[0].outcome, SW::StrokeWidthMemberOutcome::Change);
    ASSERT_TRUE(member->text_runs[0].local_width.has_value());
    EXPECT_DOUBLE_EQ(*member->text_runs[0].local_width, 5.0);
    EXPECT_EQ(member->text_runs[1].outcome, SW::StrokeWidthMemberOutcome::Change);
    ASSERT_TRUE(member->text_runs[1].local_width.has_value());
    EXPECT_DOUBLE_EQ(*member->text_runs[1].local_width, 5.0);
    EXPECT_EQ(serialize(*document), before);

    auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
    ASSERT_TRUE(token);
    auto const result = SW::apply_stroke_widths_text(*document, plan, 501, *token);
    ASSERT_EQ(result.state, SW::StrokeWidthApplyState::Applied);
    EXPECT_EQ(result.reason, SW::StrokeWidthMemberReason::None);
    EXPECT_EQ(result.changed, 1u);
    EXPECT_EQ(result.attempted_writes, 2u);

    text = item(*document, "t");
    ASSERT_TRUE(text);
    EXPECT_EQ(native_char_count(text), 4u);
    EXPECT_EQ(sp_te_get_string_multiline(text).raw(), "ABCD");
    for (unsigned i = 0; i < 4; ++i) {
        auto const widths = native_char_widths(*document, text, i);
        EXPECT_DOUBLE_EQ(widths.local, 5.0);
        ASSERT_TRUE(widths.effective.has_value());
        EXPECT_DOUBLE_EQ(*widths.effective, 10.0);
        EXPECT_EQ(widths.convention, SW::StrokeWidthConvention::Ordinary);
    }
    EXPECT_EQ(native_char_identity(text, 0), id0);
    EXPECT_EQ(native_char_identity(text, 2), id2);
    EXPECT_STREQ(native_char_stroke(text, 0).vector_effect.c_str(), "none");
    EXPECT_TRUE(SW::stroke_widths_text_output_ready(*document, plan, 501, *token));

    auto const applied = serialize(*document);
    EXPECT_TRUE(token->commitAtomically(Inkscape::Util::Internal::ContextString("SW3-B1 text width"), "",
                                        [&] {
        return SW::stroke_widths_text_output_ready(*document, plan, 501, *token);
    }));
    ASSERT_TRUE(Inkscape::DocumentUndo::undo(document.get()));
    EXPECT_EQ(serialize(*document), before);
    EXPECT_FALSE(Inkscape::DocumentUndo::undo(document.get()));
    ASSERT_TRUE(Inkscape::DocumentUndo::redo(document.get()));
    EXPECT_EQ(serialize(*document), applied);

    // Buffer round trip: parse of the saved XML buffer (not a filesystem reopen).
    auto reopened = parse(applied);
    ASSERT_TRUE(reopened);
    auto *reopened_text = item(*reopened, "t");
    ASSERT_TRUE(reopened_text);
    ASSERT_EQ(native_char_count(reopened_text), 4u);
    for (unsigned i = 0; i < 4; ++i) {
        auto const widths = native_char_widths(*reopened, reopened_text, i);
        EXPECT_DOUBLE_EQ(widths.local, 5.0);
        EXPECT_EQ(widths.convention, SW::StrokeWidthConvention::Ordinary);
    }
    token.reset();
}

// ---------------------------------------------------------------------------
// SW3-B1-APPLY-2: an explicit reversed raw range [3,1) normalizes to [1,3) over
// chars B and C of two differently styled tspans. Only B/C change (absolute 8 at
// scale 2 -> local 4); glyphs A and D outside the range keep their frozen width,
// dash and vector-effect, and the whole-owner semantic content/count survive.
// ---------------------------------------------------------------------------

TEST_F(StrokeWidthControllerTest, SW3B1ApplyTextRangeOutsideGlyphsUntouched)
{
    auto document = parse(std::string{svg_open} +
        R"svg(<text id="t" x="10" y="20" transform="scale(2)" style="font-family:Arial;font-size:20px;stroke:black">)svg"
        R"svg(<tspan id="s1" style="stroke-width:2">AB</tspan>)svg"
        R"svg(<tspan id="s2" style="stroke-width:3">CD</tspan>)svg"
        R"svg(</text></svg>)svg");
    ASSERT_TRUE(document);
    settle(*document);
    auto const before = serialize(*document);
    auto *text = item(*document, "t");
    ASSERT_TRUE(text);
    ASSERT_EQ(native_char_count(text), 4u);

    auto const outside_a = native_char_stroke(text, 0);
    auto const outside_d = native_char_stroke(text, 3);
    ASSERT_DOUBLE_EQ(outside_a.width, 2.0);
    ASSERT_DOUBLE_EQ(outside_d.width, 3.0);

    SW::StrokeWidthTextRange range;
    range.owner = text;
    range.caret = false;
    range.first_char = 3; // raw caller order is reversed
    range.last_char = 1;
    auto const plan = SW::prepare_stroke_widths(*document, range, {text}, absolute(8.0), 502);
    ASSERT_EQ(plan.state, SW::StrokeWidthPlanState::Prepared);
    auto const *member = find_member(plan, text);
    ASSERT_TRUE(member);
    ASSERT_EQ(member->target.first_char, 1u);
    ASSERT_EQ(member->target.last_char, 3u);
    EXPECT_TRUE(member->target.reversed);
    ASSERT_EQ(member->target.text_content, "ABCD");
    ASSERT_EQ(member->text_runs.size(), 2u);
    EXPECT_EQ(member->text_runs[0].first_char, 1u);
    EXPECT_EQ(member->text_runs[0].last_char, 2u);
    EXPECT_EQ(member->text_runs[1].first_char, 2u);
    EXPECT_EQ(member->text_runs[1].last_char, 3u);
    ASSERT_TRUE(member->text_runs[0].local_width.has_value());
    EXPECT_DOUBLE_EQ(*member->text_runs[0].local_width, 4.0);
    ASSERT_TRUE(member->text_runs[1].local_width.has_value());
    EXPECT_DOUBLE_EQ(*member->text_runs[1].local_width, 4.0);
    EXPECT_EQ(serialize(*document), before);

    auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
    ASSERT_TRUE(token);
    auto const result = SW::apply_stroke_widths_text(*document, plan, 502, *token);
    ASSERT_EQ(result.state, SW::StrokeWidthApplyState::Applied);
    EXPECT_EQ(result.changed, 1u);
    // WP1c: the two adjacent runs share one write, so one native call covers both.
    EXPECT_EQ(result.attempted_writes, 1u);

    text = item(*document, "t");
    ASSERT_TRUE(text);
    EXPECT_EQ(native_char_count(text), 4u);
    EXPECT_EQ(sp_te_get_string_multiline(text).raw(), "ABCD");
    for (unsigned i = 0; i < 4; ++i) {
        auto const widths = native_char_widths(*document, text, i);
        EXPECT_EQ(widths.convention, SW::StrokeWidthConvention::Ordinary);
        EXPECT_DOUBLE_EQ(widths.local, (i == 1 || i == 2) ? 4.0 : (i == 0 ? 2.0 : 3.0));
    }
    auto const after_a = native_char_stroke(text, 0);
    auto const after_d = native_char_stroke(text, 3);
    EXPECT_DOUBLE_EQ(after_a.width, outside_a.width);
    EXPECT_EQ(after_a.width_important, outside_a.width_important);
    EXPECT_EQ(after_a.dasharray, outside_a.dasharray);
    EXPECT_DOUBLE_EQ(after_a.dashoffset, outside_a.dashoffset);
    EXPECT_EQ(after_a.dasharray_important, outside_a.dasharray_important);
    EXPECT_EQ(after_a.vector_effect, outside_a.vector_effect);
    EXPECT_EQ(after_a.vector_effect_important, outside_a.vector_effect_important);
    EXPECT_DOUBLE_EQ(after_d.width, outside_d.width);
    EXPECT_EQ(after_d.width_important, outside_d.width_important);
    EXPECT_EQ(after_d.dasharray, outside_d.dasharray);
    EXPECT_EQ(after_d.vector_effect, outside_d.vector_effect);
    EXPECT_TRUE(SW::stroke_widths_text_output_ready(*document, plan, 502, *token));

    token->rollback();
    EXPECT_EQ(serialize(*document), before);
    token.reset();
}

// ---------------------------------------------------------------------------
// Owner decision 2026-09-24: a mixed shape and text edit is one compatible action.
// ---------------------------------------------------------------------------

TEST_F(StrokeWidthControllerTest, SW3B1ApplyMixedShapeTextOneAction)
{
    auto document = parse(std::string{svg_open} +
        R"(<rect id="r" width="10" height="10" style="stroke-width:2"/>)"
        R"svg(<text id="t" x="10" y="20" transform="scale(2)" style="font-family:Arial;font-size:20px;stroke:black">)svg"
        R"svg(<tspan id="s1" style="stroke-width:2">AB</tspan>)svg"
        R"svg(</text></svg>)svg");
    ASSERT_TRUE(document);
    settle(*document);
    auto const before = serialize(*document);
    auto *rect = item(*document, "r");
    auto *text = item(*document, "t");
    ASSERT_TRUE(rect && text);

    auto const plan = SW::prepare_stroke_widths(*document, {rect, text}, absolute(10.0), 503);
    ASSERT_EQ(plan.state, SW::StrokeWidthPlanState::Prepared);
    ASSERT_EQ(plan.planned_changes, 1u);
    auto const *text_member = find_member(plan, text);
    ASSERT_TRUE(text_member);
    EXPECT_EQ(text_member->outcome, SW::StrokeWidthMemberOutcome::Excluded);
    EXPECT_EQ(text_member->reason, SW::StrokeWidthMemberReason::TextAdapterPending);
    ASSERT_EQ(text_member->text_runs.size(), 1u);
    EXPECT_EQ(text_member->text_runs[0].outcome, SW::StrokeWidthMemberOutcome::Change);

    auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
    ASSERT_TRUE(token);
    auto const applied = SW::apply_stroke_widths_compatible(*document, plan, 503, *token);
    // Owner decision 2026-09-24: both compatible members change in one Undo.
    EXPECT_EQ(applied.state, SW::StrokeWidthApplyState::Applied);
    EXPECT_EQ(applied.changed, 2u);
    EXPECT_EQ(applied.attempted_writes, 2u);
    EXPECT_DOUBLE_EQ(computed_width(*document, "r"), 10.0);
    auto const text_glyph_widths = native_char_widths(*document, text, 0);
    EXPECT_EQ(text_glyph_widths.convention, SW::StrokeWidthConvention::Ordinary);
    EXPECT_DOUBLE_EQ(text_glyph_widths.local, 5.0);
    EXPECT_TRUE(token->commitAtomically(
        Inkscape::Util::Internal::ContextString("Combined width"), "",
        [&] { return SW::stroke_widths_compatible_output_ready(*document, plan, 503, *token); }));
    EXPECT_TRUE(Inkscape::DocumentUndo::undo(document.get()));
    EXPECT_EQ(serialize(*document), before);

    // The legacy text-only entry point still refuses the shape with no writes.
    auto legacy_token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
    ASSERT_TRUE(legacy_token);
    auto const legacy = SW::apply_stroke_widths_text(*document, plan, 503, *legacy_token);
    EXPECT_EQ(legacy.state, SW::StrokeWidthApplyState::Rejected);
    EXPECT_EQ(legacy.reason, SW::StrokeWidthMemberReason::UnsupportedIntent);
    EXPECT_EQ(legacy.attempted_writes, 0u);
    EXPECT_EQ(serialize(*document), before);
    legacy_token->rollback();
}

// ---------------------------------------------------------------------------
// SW3-B1-APPLY-4: a same-length native edit of a character outside the explicit
// [0,2) range (D -> X) changes the frozen whole-owner semantic content, so the
// text applier rejects StalePlan before any native write and adds no history.
// ---------------------------------------------------------------------------

TEST_F(StrokeWidthControllerTest, SW3B1ApplyTextStaleSameLengthOutsideMutation)
{
    auto document = parse(std::string{svg_open} +
        R"svg(<text id="t" x="10" y="20" transform="scale(2)" style="font-family:Arial;font-size:20px;stroke:black">)svg"
        R"svg(<tspan id="s1" style="stroke-width:2">AB</tspan>)svg"
        R"svg(<tspan id="s2" style="stroke-width:3">CD</tspan>)svg"
        R"svg(</text></svg>)svg");
    ASSERT_TRUE(document);
    settle(*document);
    auto *text = item(*document, "t");
    ASSERT_TRUE(text);
    ASSERT_EQ(native_char_count(text), 4u);

    SW::StrokeWidthTextRange range;
    range.owner = text;
    range.caret = false;
    range.first_char = 0;
    range.last_char = 2;
    auto const plan = SW::prepare_stroke_widths(*document, range, {text}, absolute(10.0), 504);
    ASSERT_EQ(plan.state, SW::StrokeWidthPlanState::Prepared);
    auto const *member = find_member(plan, text);
    ASSERT_TRUE(member);
    ASSERT_EQ(member->target.text_content, "ABCD");
    ASSERT_EQ(member->target.char_count, 4u);
    ASSERT_EQ(member->text_runs.size(), 1u);
    EXPECT_EQ(member->text_runs[0].outcome, SW::StrokeWidthMemberOutcome::Change);

    // Native same-length edit at index 3 (outside [0,2)): D -> X.
    auto *layout = te_get_layout(text);
    ASSERT_TRUE(layout);
    sp_te_replace(text, layout->charIndexToIterator(3), layout->charIndexToIterator(4), "X");
    document->ensureUpToDate();
    EXPECT_EQ(native_char_count(text), 4u);
    EXPECT_EQ(sp_te_get_string_multiline(text).raw(), "ABCX");

    // Clean history baseline after the fixture edit, then attempt the stale plan.
    settle(*document);
    auto const stale_before = serialize(*document);

    auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
    ASSERT_TRUE(token);
    auto const result = SW::apply_stroke_widths_text(*document, plan, 504, *token);
    EXPECT_EQ(result.state, SW::StrokeWidthApplyState::Rejected);
    EXPECT_EQ(result.reason, SW::StrokeWidthMemberReason::StalePlan);
    EXPECT_EQ(result.attempted_writes, 0u);
    EXPECT_EQ(serialize(*document), stale_before);
    token->rollback();
    EXPECT_FALSE(Inkscape::DocumentUndo::undo(document.get()));
    EXPECT_EQ(serialize(*document), stale_before);
}

// ---------------------------------------------------------------------------
// SW3-B1-APPLY-5: a real native single-fire observer mutates the text owner's
// transform on the first (descending) native run call. The next run's pre-write
// binding check must fail, so the result is Failed/changed=0 with one attempted
// write; one caller rollback restores the exact baseline.
// ---------------------------------------------------------------------------

TEST_F(StrokeWidthControllerTest, SW3B1ApplyTextObserverMutationAfterFirstWriteFailsAndRollsBack)
{
    auto document = parse(std::string{svg_open} +
        R"svg(<text id="t" x="10" y="20" transform="scale(2)" style="font-family:Arial;font-size:20px;stroke:black">)svg"
        R"svg(<tspan id="s1" style="stroke-width:2">AB</tspan>)svg"
        R"svg(<tspan id="s2" style="stroke-width:3">CD</tspan>)svg"
        R"svg(</text></svg>)svg");
    ASSERT_TRUE(document);
    settle(*document);
    auto const before = serialize(*document);
    auto *text = item(*document, "t");
    ASSERT_TRUE(text);
    auto const plan = SW::prepare_stroke_widths(*document, {text}, absolute(10.0), 505);
    auto const *member = find_member(plan, text);
    ASSERT_TRUE(member);
    ASSERT_EQ(member->text_runs.size(), 2u);
    EXPECT_EQ(member->text_runs[1].outcome, SW::StrokeWidthMemberOutcome::Change);

    auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
    ASSERT_TRUE(token);
    // The descending-first native run is [2,4) on s2; this observer fires on that
    // style write and mutates the text owner's transform, so the next run's
    // pre-write affine check must fail.
    SingleFireStyleObserver observer(*item(*document, "s2")->getRepr(), [&] {
        text->setAttribute("transform", "scale(3)");
    });
    auto const result = SW::apply_stroke_widths_text(*document, plan, 505, *token);

    EXPECT_TRUE(observer.fired());
    EXPECT_EQ(result.state, SW::StrokeWidthApplyState::Failed);
    EXPECT_EQ(result.reason, SW::StrokeWidthMemberReason::PostconditionMismatch);
    EXPECT_EQ(result.changed, 0u);
    EXPECT_EQ(result.attempted_writes, 1u);
    EXPECT_FALSE(SW::stroke_widths_text_output_ready(*document, plan, 505, *token));

    observer.detach();
    token->rollback();
    EXPECT_EQ(serialize(*document), before);
    // Rollback restores the styled tspan's per-glyph width; observe the native
    // glyph at index 0 ('A' in s1), not the owner-inherited style.
    auto const text_glyph_widths = native_char_widths(*document, text, 0);
    EXPECT_EQ(text_glyph_widths.convention, SW::StrokeWidthConvention::Ordinary);
    EXPECT_DOUBLE_EQ(text_glyph_widths.local, 2.0);
}

// ---------------------------------------------------------------------------
// SW3-B1-APPLY-A (reviewer diagnostic): an explicit partial range over B/C of
// ABCD applies successfully, then the unselected glyph A is restyled natively in
// the same transaction. `stroke_widths_text_output_ready` must refuse because the
// frozen plan no longer matches the rendered document. The control call before
// the mutation is ready TRUE and A's native width is observed to move 2 -> 9, so
// the refusal cannot be vacuous or caused by an unrelated token problem.
// No commit; one rollback restores the exact baseline.
// ---------------------------------------------------------------------------

TEST_F(StrokeWidthControllerTest, SW3B1ApplyTextOutputReadyRejectsChangedUnselectedGlyph)
{
    auto document = parse(std::string{svg_open} +
        R"svg(<text id="t" x="10" y="20" transform="scale(2)" style="font-family:Arial;font-size:20px;stroke:black">)svg"
        R"svg(<tspan id="s1" style="stroke-width:2">AB</tspan>)svg"
        R"svg(<tspan id="s2" style="stroke-width:3">CD</tspan>)svg"
        R"svg(</text></svg>)svg");
    ASSERT_TRUE(document);
    settle(*document);
    auto const before = serialize(*document);
    auto *text = item(*document, "t");
    ASSERT_TRUE(text);
    ASSERT_EQ(native_char_count(text), 4u);

    SW::StrokeWidthTextRange range;
    range.owner = text;
    range.caret = false;
    range.first_char = 1;
    range.last_char = 3;
    auto const plan = SW::prepare_stroke_widths(*document, range, {text}, absolute(8.0), 506);
    ASSERT_EQ(plan.state, SW::StrokeWidthPlanState::Prepared);
    auto const *member = find_member(plan, text);
    ASSERT_TRUE(member);
    ASSERT_EQ(member->target.first_char, 1u);
    ASSERT_EQ(member->target.last_char, 3u);
    ASSERT_EQ(member->text_runs.size(), 2u);
    EXPECT_EQ(member->text_runs[0].outcome, SW::StrokeWidthMemberOutcome::Change);
    EXPECT_EQ(member->text_runs[1].outcome, SW::StrokeWidthMemberOutcome::Change);

    auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
    ASSERT_TRUE(token);
    auto const result = SW::apply_stroke_widths_text(*document, plan, 506, *token);
    ASSERT_EQ(result.state, SW::StrokeWidthApplyState::Applied);
    // WP1c: the two adjacent runs share one write, so one native call covers both.
    ASSERT_EQ(result.attempted_writes, 1u);

    text = item(*document, "t");
    ASSERT_TRUE(text);
    // Control: the clean applied state is ready. The later FALSE must come from
    // the unselected-glyph mutation, not from the token or the covered runs.
    EXPECT_TRUE(SW::stroke_widths_text_output_ready(*document, plan, 506, *token));
    auto const a_before = native_char_stroke(text, 0);
    EXPECT_DOUBLE_EQ(a_before.width, 2.0);

    // Native style mutation of unselected A (index 0) in the same transaction.
    auto patch = Util::delete_with<sp_repr_css_attr_unref>(sp_repr_css_attr_new());
    sp_repr_css_set_property(patch.get(), "stroke-width", "9");
    TextStyleLocalStroke width_only;
    width_only.stroke_width = true;
    width_only.exclude_end_line_break = true;
    ASSERT_NO_FATAL_FAILURE(native_apply_local_stroke(*document, text, 0, 1, patch.get(), width_only));
    document->ensureUpToDate();

    // Non-vacuous oracle: A actually moved, while the covered B/C and the other
    // unselected D are unchanged, so the refusal below is about A.
    text = item(*document, "t");
    ASSERT_TRUE(text);
    auto const a_after = native_char_stroke(text, 0);
    EXPECT_DOUBLE_EQ(a_after.width, 9.0) << "unselected A must be observed changed";
    EXPECT_NE(a_after.width, a_before.width);
    EXPECT_DOUBLE_EQ(native_char_stroke(text, 1).width, 4.0);
    EXPECT_DOUBLE_EQ(native_char_stroke(text, 2).width, 4.0);
    EXPECT_DOUBLE_EQ(native_char_stroke(text, 3).width, 3.0);

    // The frozen plan no longer matches the document; the commit-readiness check
    // must refuse even though A is outside every frozen run.
    EXPECT_FALSE(SW::stroke_widths_text_output_ready(*document, plan, 506, *token));

    token->rollback();
    EXPECT_EQ(serialize(*document), before);
    text = item(*document, "t");
    ASSERT_TRUE(text);
    EXPECT_DOUBLE_EQ(native_char_stroke(text, 0).width, 2.0);
    EXPECT_DOUBLE_EQ(native_char_stroke(text, 2).width, 3.0);
}

// ---------------------------------------------------------------------------
// SW3-B1-APPLY-B (reviewer diagnostic): two changed text runs; the descending
// first native write lands on s2, and a real single-fire observer then edits the
// still-unwritten earlier run s1's stroke style. The applier must fail before
// overwriting s1 (exactly one attempted write), so the callback edit survives
// until the caller's rollback restores the exact baseline. This is the text
// analogue of SW2BApplyCallbackFrozenStyleAndScopeFails subcase A.
// ---------------------------------------------------------------------------

TEST_F(StrokeWidthControllerTest, SW3B1ApplyTextObserverEarlierRunMutationFailsBeforeOverwrite)
{
    auto document = parse(std::string{svg_open} +
        R"svg(<text id="t" x="10" y="20" transform="scale(2)" style="font-family:Arial;font-size:20px;stroke:black">)svg"
        R"svg(<tspan id="s1" style="stroke-width:2">AB</tspan>)svg"
        R"svg(<tspan id="s2" style="stroke-width:3">CD</tspan>)svg"
        R"svg(</text></svg>)svg");
    ASSERT_TRUE(document);
    settle(*document);
    auto const before = serialize(*document);
    auto *text = item(*document, "t");
    auto *s1 = item(*document, "s1");
    ASSERT_TRUE(text);
    ASSERT_TRUE(s1);
    ASSERT_EQ(native_char_count(text), 4u);
    EXPECT_DOUBLE_EQ(native_char_stroke(text, 0).width, 2.0);
    EXPECT_DOUBLE_EQ(native_char_stroke(text, 2).width, 3.0);

    auto const plan = SW::prepare_stroke_widths(*document, {text}, absolute(10.0), 507);
    ASSERT_EQ(plan.state, SW::StrokeWidthPlanState::Prepared);
    auto const *member = find_member(plan, text);
    ASSERT_TRUE(member);
    ASSERT_EQ(member->text_runs.size(), 2u);
    ASSERT_EQ(member->text_runs[0].outcome, SW::StrokeWidthMemberOutcome::Change);
    ASSERT_EQ(member->text_runs[1].outcome, SW::StrokeWidthMemberOutcome::Change);

    auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
    ASSERT_TRUE(token);
    // Descending first run is [2,4) on s2; this callback fires on that style
    // write and edits the earlier, not-yet-written run s1's stroke style.
    SingleFireStyleObserver observer(*item(*document, "s2")->getRepr(), [&] {
        s1->setAttribute("style", "stroke-width:7");
    });
    auto const result = SW::apply_stroke_widths_text(*document, plan, 507, *token);

    EXPECT_TRUE(observer.fired());
    EXPECT_EQ(result.state, SW::StrokeWidthApplyState::Failed);
    EXPECT_EQ(result.reason, SW::StrokeWidthMemberReason::PostconditionMismatch);
    EXPECT_EQ(result.changed, 0u);
    EXPECT_EQ(result.attempted_writes, 1u);
    EXPECT_FALSE(SW::stroke_widths_text_output_ready(*document, plan, 507, *token));

    // s1 was never overwritten by the applier: the callback's 7 survives, not the
    // intended local 5. s2 was the single attempted write.
    text = item(*document, "t");
    ASSERT_TRUE(text);
    EXPECT_DOUBLE_EQ(native_char_stroke(text, 0).width, 7.0);
    EXPECT_DOUBLE_EQ(native_char_stroke(text, 1).width, 7.0);

    observer.detach();
    token->rollback();
    EXPECT_EQ(serialize(*document), before);
    text = item(*document, "t");
    ASSERT_TRUE(text);
    EXPECT_DOUBLE_EQ(native_char_stroke(text, 0).width, 2.0);
    EXPECT_DOUBLE_EQ(native_char_stroke(text, 2).width, 3.0);
}

// ---------------------------------------------------------------------------
// SW3-B1-APPLY-C (reviewer diagnostic): a selected text source whose runs would
// change plus a selected clone referencing it. The clone is not an eligible
// width target (a `use` of a text source is unsupported); it follows its
// original (owner decision 2026-09-28), so the text change applies.
// ---------------------------------------------------------------------------

TEST_F(StrokeWidthControllerTest, SW3B1ApplyTextWithSelectedCloneAppliesToTheSource)
{
    auto document = parse(std::string{svg_open} +
        R"svg(<text id="src" x="10" y="20" style="font-family:Arial;font-size:20px;stroke:black">)svg"
        R"svg(<tspan id="s1" style="stroke-width:2">AB</tspan>)svg"
        R"svg(</text>)svg"
        R"(<use id="clone" xlink:href="#src" x="40"/>)"
        R"(</svg>)");
    ASSERT_TRUE(document);
    settle(*document);
    auto const before = serialize(*document);
    auto *src = item(*document, "src");
    auto *clone = item(*document, "clone");
    ASSERT_TRUE(src);
    ASSERT_TRUE(clone);
    ASSERT_EQ(native_char_count(src), 2u);

    SW::StrokeWidthTextRange range;
    range.owner = src;
    range.caret = false;
    range.first_char = 0;
    range.last_char = 2;
    auto const plan = SW::prepare_stroke_widths(*document, range, {src, clone}, absolute(10.0), 508);
    ASSERT_EQ(plan.state, SW::StrokeWidthPlanState::Prepared);
    EXPECT_EQ(plan.following_clones, 1u);

    auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
    ASSERT_TRUE(token);
    auto const result = SW::apply_stroke_widths_text(*document, plan, 508, *token);
    ASSERT_EQ(result.state, SW::StrokeWidthApplyState::Applied);
    EXPECT_EQ(result.changed, 1u);
    EXPECT_FALSE(item(*document, "clone")->getRepr()->attribute("style")) << "the clone is never written";
    EXPECT_TRUE(token->commitAtomically(Inkscape::Util::Internal::ContextString("Stroke width"), "", [&] {
        return SW::stroke_widths_text_output_ready(*document, plan, 508, *token);
    }));
    src = item(*document, "src");
    EXPECT_DOUBLE_EQ(native_char_stroke(src, 0).width, 10.0);
    ASSERT_TRUE(Inkscape::DocumentUndo::undo(document.get()));
    EXPECT_EQ(serialize(*document), before);
}

// ---------------------------------------------------------------------------
// SW3-B1-APPLY-VE (rendering-source repair): a non-inheriting
// `vector-effect:none !important` authored on the rendered span. The SW1 query
// freezes each character's style from its actual rendering-source element (the
// span), which carries the independent priority. The postwrite verifier and the
// whole-owner baseline must read that same source, not the SPString/nearest
// style object where a non-inherited priority is lost. The explicit [0,2) range
// changes only AB; the unselected CD is proven unchanged from the frozen
// baseline. Before the repair the changed-run observation read `important` false
// and apply reported Failed/PostconditionMismatch, so the test is non-vacuous.
// ---------------------------------------------------------------------------

TEST_F(StrokeWidthControllerTest, SW3B1ApplyTextNonInheritingVectorEffectPriority)
{
    auto document = parse(std::string{svg_open} +
        R"svg(<text id="t" x="10" y="20" transform="scale(2)" )svg"
        R"svg(style="font-family:Arial;font-size:20px;stroke:black;fill:none">)svg"
        R"svg(<tspan id="s1" style="stroke-width:2;vector-effect:none !important">AB</tspan>)svg"
        R"svg(<tspan id="s2" style="stroke-width:3;vector-effect:none !important">CD</tspan>)svg"
        R"svg(</text></svg>)svg");
    ASSERT_TRUE(document);
    settle(*document);
    auto const before = serialize(*document);
    auto *text = item(*document, "t");
    ASSERT_TRUE(text);
    ASSERT_EQ(native_char_count(text), 4u);

    // Rendering-source oracle (span element): the non-inheriting value and its
    // !important priority are visible there and must be what the query freezes.
    ASSERT_STREQ(native_char_stroke(text, 0).vector_effect.c_str(), "none");
    ASSERT_TRUE(native_char_stroke(text, 0).vector_effect_important);
    ASSERT_STREQ(native_char_stroke(text, 2).vector_effect.c_str(), "none");
    ASSERT_TRUE(native_char_stroke(text, 2).vector_effect_important);

    SW::StrokeWidthTextRange range;
    range.owner = text;
    range.caret = false;
    range.first_char = 0;
    range.last_char = 2;
    auto const plan = SW::prepare_stroke_widths(*document, range, {text}, absolute(8.0), 509);
    ASSERT_EQ(plan.state, SW::StrokeWidthPlanState::Prepared);
    auto const *member = find_member(plan, text);
    ASSERT_TRUE(member);
    ASSERT_EQ(member->target.first_char, 0u);
    ASSERT_EQ(member->target.last_char, 2u);
    ASSERT_EQ(member->target.char_count, 4u);
    // Whole-owner baseline includes the unselected CD and keeps the priority.
    ASSERT_EQ(member->target.char_baseline.size(), 4u);
    EXPECT_TRUE(member->target.char_baseline[2].glyph);
    EXPECT_TRUE(member->target.char_baseline[2].style.vector_effect_important);
    ASSERT_EQ(member->text_runs.size(), 1u);
    ASSERT_EQ(member->text_runs[0].outcome, SW::StrokeWidthMemberOutcome::Change);
    ASSERT_EQ(member->text_runs[0].style.convention, SW::StrokeWidthConvention::Ordinary);
    EXPECT_STREQ(member->text_runs[0].style.vector_effect_value.c_str(), "none");
    EXPECT_TRUE(member->text_runs[0].style.vector_effect_important);

    auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
    ASSERT_TRUE(token);
    auto const result = SW::apply_stroke_widths_text(*document, plan, 509, *token);
    ASSERT_EQ(result.state, SW::StrokeWidthApplyState::Applied);
    EXPECT_EQ(result.changed, 1u);
    EXPECT_EQ(result.attempted_writes, 1u);

    text = item(*document, "t");
    ASSERT_TRUE(text);
    EXPECT_EQ(native_char_count(text), 4u);
    EXPECT_DOUBLE_EQ(native_char_stroke(text, 0).width, 4.0);
    EXPECT_DOUBLE_EQ(native_char_stroke(text, 1).width, 4.0);
    EXPECT_DOUBLE_EQ(native_char_stroke(text, 2).width, 3.0);
    EXPECT_DOUBLE_EQ(native_char_stroke(text, 3).width, 3.0);
    // Postwrite render observation and frozen run/baseline agree field-for-field
    // on the non-inheriting value and priority (changed and unselected glyphs).
    for (unsigned index = 0; index < 4; ++index) {
        auto const rendered = native_char_stroke(text, index);
        EXPECT_STREQ(rendered.vector_effect.c_str(), "none");
        EXPECT_TRUE(rendered.vector_effect_important);
        EXPECT_STREQ(rendered.vector_effect.c_str(),
                     member->target.char_baseline[index].style.vector_effect_value.c_str());
        EXPECT_EQ(rendered.vector_effect_important,
                  member->target.char_baseline[index].style.vector_effect_important);
    }
    EXPECT_TRUE(SW::stroke_widths_text_output_ready(*document, plan, 509, *token));

    token->rollback();
    EXPECT_EQ(serialize(*document), before);
    text = item(*document, "t");
    ASSERT_TRUE(text);
    EXPECT_TRUE(native_char_stroke(text, 0).vector_effect_important);
    EXPECT_DOUBLE_EQ(native_char_stroke(text, 0).width, 2.0);
}

// ---------------------------------------------------------------------------
// SW3-B1-APPLY-D (final readiness guard): a selected text source with changing
// runs plus a selected clone that initially references an unrelated text. The
// plan is Prepared because the clone's source graph is clean, and the text-only
// apply succeeds. A post-apply, pre-commit retarget of the clone to the changed
// text source must be refused by the live dependency recheck during
// intended-output readiness; without that recheck the frozen plan no longer
// matches the document. The retarget is asserted so the refusal cannot be the
// initial preflight or an unrelated token problem, and one caller rollback
// restores the exact baseline XML.
// ---------------------------------------------------------------------------

TEST_F(StrokeWidthControllerTest, SW3B1ApplyTextOutputReadyRejectsRetargetedCloneDependency)
{
    auto document = parse(std::string{svg_open} +
        R"svg(<text id="src" x="10" y="20" style="font-family:Arial;font-size:20px;stroke:black">)svg"
        R"svg(<tspan id="s1" style="stroke-width:2">AB</tspan>)svg"
        R"svg(</text>)svg"
        R"svg(<text id="other" x="10" y="60" style="font-family:Arial;font-size:20px;stroke:black">)svg"
        R"svg(<tspan id="o1" style="stroke-width:5">XY</tspan>)svg"
        R"svg(</text>)svg"
        R"(<use id="clone" xlink:href="#other" x="40"/>)"
        R"(</svg>)");
    ASSERT_TRUE(document);
    settle(*document);
    auto const before = serialize(*document);
    auto *src = item(*document, "src");
    auto *clone = item(*document, "clone");
    ASSERT_TRUE(src);
    ASSERT_TRUE(clone);
    ASSERT_EQ(native_char_count(src), 2u);
    auto *clone_use = cast<SPUse>(clone);
    ASSERT_TRUE(clone_use);
    ASSERT_EQ(clone_use->get_original(), item(*document, "other"));

    SW::StrokeWidthTextRange range;
    range.owner = src;
    range.caret = false;
    range.first_char = 0;
    range.last_char = 2;
    auto const plan = SW::prepare_stroke_widths(*document, range, {src, clone}, absolute(10.0), 520);
    // Clean initial dependency: the selected clone references an unrelated text,
    // so the plan is Prepared and the text-only writer may proceed.
    ASSERT_EQ(plan.state, SW::StrokeWidthPlanState::Prepared);
    auto const *member = find_member(plan, src);
    ASSERT_TRUE(member);
    ASSERT_EQ(member->text_runs.size(), 1u);
    ASSERT_EQ(member->text_runs[0].outcome, SW::StrokeWidthMemberOutcome::Change);

    auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
    ASSERT_TRUE(token);
    auto const result = SW::apply_stroke_widths_text(*document, plan, 520, *token);
    ASSERT_EQ(result.state, SW::StrokeWidthApplyState::Applied);
    ASSERT_EQ(result.changed, 1u);
    ASSERT_EQ(result.attempted_writes, 1u);

    src = item(*document, "src");
    ASSERT_TRUE(src);
    // Control: the clean applied state is ready before the retarget, so the
    // later FALSE can only come from the live dependency guard.
    EXPECT_TRUE(SW::stroke_widths_text_output_ready(*document, plan, 520, *token));

    // Retarget the selected clone to the changed text source after apply but
    // before commit.
    clone = item(*document, "clone");
    ASSERT_TRUE(clone);
    clone->setAttribute("xlink:href", "#src");
    clone_use = cast<SPUse>(item(*document, "clone"));
    ASSERT_TRUE(clone_use);
    EXPECT_STREQ(item(*document, "clone")->getRepr()->attribute("xlink:href"), "#src");
    EXPECT_EQ(clone_use->get_original(), src) << "clone retarget must actually take effect";

    // The frozen plan no longer matches the live dependency graph.
    EXPECT_FALSE(SW::stroke_widths_text_output_ready(*document, plan, 520, *token));

    token->rollback();
    EXPECT_EQ(serialize(*document), before);
    clone_use = cast<SPUse>(item(*document, "clone"));
    ASSERT_TRUE(clone_use);
    EXPECT_EQ(clone_use->get_original(), item(*document, "other"));
    src = item(*document, "src");
    ASSERT_TRUE(src);
    EXPECT_DOUBLE_EQ(native_char_stroke(src, 0).width, 2.0);
}

// ---------------------------------------------------------------------------
// SW3-B1-APPLY-E (final readiness guard): a partial text range applies
// successfully; the unselected glyph carries no authored vector-effect, so the
// frozen whole-owner baseline has vector_effect_set false there. Adding
// vector-effect:fixed-position to that unselected glyph's authored style after
// apply, without touching any stroke width, must be observed as a real native
// vector-effect value change and refused during intended-output readiness. The
// current preserved-stroke comparison skips the raw value whenever the frozen
// style did not author one, so this oracle is non-vacuous: the native value is
// asserted to move to fixed-position while the width stays put.
// ---------------------------------------------------------------------------

TEST_F(StrokeWidthControllerTest, SW3B1ApplyTextOutputReadyRejectsNewUnselectedVectorEffect)
{
    auto document = parse(std::string{svg_open} +
        R"svg(<text id="t" x="10" y="20" transform="scale(2)" style="font-family:Arial;font-size:20px;stroke:black;fill:none">)svg"
        R"svg(<tspan id="s1" style="stroke-width:2">AB</tspan>)svg"
        R"svg(<tspan id="s2" style="stroke-width:3">CD</tspan>)svg"
        R"svg(</text></svg>)svg");
    ASSERT_TRUE(document);
    settle(*document);
    auto const before = serialize(*document);
    auto *text = item(*document, "t");
    ASSERT_TRUE(text);
    ASSERT_EQ(native_char_count(text), 4u);

    SW::StrokeWidthTextRange range;
    range.owner = text;
    range.caret = false;
    range.first_char = 0;
    range.last_char = 2;
    auto const plan = SW::prepare_stroke_widths(*document, range, {text}, absolute(8.0), 521);
    ASSERT_EQ(plan.state, SW::StrokeWidthPlanState::Prepared);
    auto const *member = find_member(plan, text);
    ASSERT_TRUE(member);
    ASSERT_EQ(member->text_runs.size(), 1u);
    ASSERT_EQ(member->text_runs[0].outcome, SW::StrokeWidthMemberOutcome::Change);
    ASSERT_EQ(member->target.char_baseline.size(), 4u);
    // The frozen unselected glyph authored no vector-effect: the guard below is
    // about a genuinely new value, not an existing important one.
    ASSERT_TRUE(member->target.char_baseline[2].glyph);
    EXPECT_FALSE(member->target.char_baseline[2].style.vector_effect_set);
    EXPECT_FALSE(member->target.char_baseline[2].style.vector_effect_important);

    auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
    ASSERT_TRUE(token);
    auto const result = SW::apply_stroke_widths_text(*document, plan, 521, *token);
    ASSERT_EQ(result.state, SW::StrokeWidthApplyState::Applied);
    ASSERT_EQ(result.changed, 1u);
    ASSERT_EQ(result.attempted_writes, 1u);

    text = item(*document, "t");
    ASSERT_TRUE(text);
    EXPECT_DOUBLE_EQ(native_char_stroke(text, 0).width, 4.0);
    EXPECT_DOUBLE_EQ(native_char_stroke(text, 2).width, 3.0);
    // Control: the clean applied state is ready before the unselected mutation.
    EXPECT_TRUE(SW::stroke_widths_text_output_ready(*document, plan, 521, *token));

    auto const unselected_before = native_char_stroke(text, 2);
    ASSERT_STRNE(unselected_before.vector_effect.c_str(), "fixed-position");

    // Add only a non-inheriting vector-effect to the unselected CD span. No
    // stroke-width property is supplied, so the numeric width must not move.
    auto patch = Util::delete_with<sp_repr_css_attr_unref>(sp_repr_css_attr_new());
    sp_repr_css_set_property(patch.get(), "vector-effect", "fixed-position");
    TextStyleLocalStroke vector_effect_only;
    ASSERT_NO_FATAL_FAILURE(
        native_apply_local_stroke(*document, text, 2, 4, patch.get(), vector_effect_only));
    document->ensureUpToDate();

    text = item(*document, "t");
    ASSERT_TRUE(text);
    auto const unselected_after = native_char_stroke(text, 2);
    // Non-vacuous oracle: the native parser must expose the new full value while
    // the stroke width stays at the unselected baseline.
    EXPECT_STREQ(unselected_after.vector_effect.c_str(), "fixed-position")
        << "native parser must not ignore the authored vector-effect";
    EXPECT_NE(unselected_after.vector_effect, unselected_before.vector_effect);
    EXPECT_FALSE(unselected_after.vector_effect_important);
    EXPECT_DOUBLE_EQ(unselected_after.width, 3.0);

    // The frozen baseline had no vector-effect value to compare, but the live
    // rendered value changed; commit readiness must refuse.
    EXPECT_FALSE(SW::stroke_widths_text_output_ready(*document, plan, 521, *token));

    token->rollback();
    EXPECT_EQ(serialize(*document), before);
    text = item(*document, "t");
    ASSERT_TRUE(text);
    EXPECT_STREQ(native_char_stroke(text, 2).vector_effect.c_str(), "none");
    EXPECT_DOUBLE_EQ(native_char_stroke(text, 2).width, 3.0);
    EXPECT_DOUBLE_EQ(native_char_stroke(text, 0).width, 2.0);
}

// ---------------------------------------------------------------------------
// SW3-B2 (multi-text-owner outcome 1): two separately selected TextOwner roots
// plus an excluded bitmap are one user action. The prepared query must keep both
// text owners as eligible TextOwner members (one prospective Change run each)
// and report the bitmap as excluded. The text applier must then write both
// owners under one caller-owned atomic interaction -- one native run call each,
// changed==2, attempted_writes==native run calls -- leave the bitmap XML
// byte-identical, commit one Undo/Redo, and survive a serialized buffer reopen
// with the exact per-glyph widths. At the current text_count>1 gate the applier
// rejects the whole plan before any write, so this test fails at the apply
// result; the excluded bitmap is never a target and is never written.
// ---------------------------------------------------------------------------

TEST_F(StrokeWidthControllerTest, SW3B2ApplyTextTwoOwnersAndBitmapAtomic)
{
    auto document = parse(std::string{svg_open} +
        R"svg(<text id="ta" x="10" y="20" transform="scale(2)" style="font-family:Arial;font-size:20px;stroke:black;fill:none">)svg"
        R"svg(<tspan id="sa" style="stroke-width:2">AB</tspan>)svg"
        R"svg(</text>)svg"
        R"svg(<text id="tb" x="10" y="60" transform="scale(2)" style="font-family:Arial;font-size:20px;stroke:black;fill:none">)svg"
        R"svg(<tspan id="sb" style="stroke-width:3">CD</tspan>)svg"
        R"svg(</text>)svg"
        R"(<image id="bmp" width="10" height="10"/>)"
        R"(</svg>)");
    ASSERT_TRUE(document);
    settle(*document);
    auto const before = serialize(*document);
    auto *ta = item(*document, "ta");
    auto *tb = item(*document, "tb");
    auto *bmp = item(*document, "bmp");
    ASSERT_TRUE(ta);
    ASSERT_TRUE(tb);
    ASSERT_TRUE(bmp);
    ASSERT_EQ(native_char_count(ta), 2u);
    ASSERT_EQ(native_char_count(tb), 2u);
    EXPECT_DOUBLE_EQ(native_char_widths(*document, ta, 0).local, 2.0);
    EXPECT_DOUBLE_EQ(native_char_widths(*document, tb, 0).local, 3.0);
    auto const bmp_before = sp_repr_write_buf(bmp->getRepr(), 0, false, GQuark(0), 0, 0).raw();

    auto const plan = SW::prepare_stroke_widths(*document, {ta, tb, bmp}, absolute(10.0), 601);
    ASSERT_EQ(plan.state, SW::StrokeWidthPlanState::Prepared);
    // Both separately selected text owners survive preparation as eligible
    // TextOwner targets; the bitmap is excluded and is never a member.
    EXPECT_EQ(plan.query.eligible, 2u);
    EXPECT_EQ(plan.query.incompatible, 1u);
    ASSERT_TRUE(find_excluded(plan.query, SW::StrokeWidthExclusion::Bitmap));
    ASSERT_EQ(plan.query.targets.size(), 2u);
    for (auto const &target : plan.query.targets) {
        EXPECT_EQ(target.kind, SW::StrokeWidthTargetKind::TextOwner);
    }
    auto const *ma = find_member(plan, ta);
    auto const *mb = find_member(plan, tb);
    ASSERT_TRUE(ma);
    ASSERT_TRUE(mb);
    EXPECT_EQ(ma->target.kind, SW::StrokeWidthTargetKind::TextOwner);
    EXPECT_EQ(mb->target.kind, SW::StrokeWidthTargetKind::TextOwner);
    ASSERT_EQ(ma->text_runs.size(), 1u);
    ASSERT_EQ(mb->text_runs.size(), 1u);
    EXPECT_EQ(ma->text_runs[0].outcome, SW::StrokeWidthMemberOutcome::Change);
    EXPECT_EQ(mb->text_runs[0].outcome, SW::StrokeWidthMemberOutcome::Change);
    ASSERT_TRUE(ma->text_runs[0].local_width.has_value());
    ASSERT_TRUE(mb->text_runs[0].local_width.has_value());
    EXPECT_DOUBLE_EQ(*ma->text_runs[0].local_width, 5.0);
    EXPECT_DOUBLE_EQ(*mb->text_runs[0].local_width, 5.0);
    EXPECT_EQ(find_member(plan, bmp), nullptr);
    EXPECT_EQ(serialize(*document), before);

    // Native run calls = the eligible Change runs across both owners.
    std::size_t run_calls = 0;
    for (auto const &member : plan.members) {
        for (auto const &run : member.text_runs) {
            if (run.eligibility == SW::StrokeWidthEligibility::Eligible &&
                run.outcome == SW::StrokeWidthMemberOutcome::Change) {
                ++run_calls;
            }
        }
    }
    EXPECT_EQ(run_calls, 2u);

    auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
    ASSERT_TRUE(token);
    auto const result = SW::apply_stroke_widths_text(*document, plan, 601, *token);
    ASSERT_EQ(result.state, SW::StrokeWidthApplyState::Applied);
    EXPECT_EQ(result.reason, SW::StrokeWidthMemberReason::None);
    EXPECT_EQ(result.changed, 2u);
    EXPECT_EQ(result.attempted_writes, run_calls);
    // The two text owners were changed; only the incompatible bitmap counts as
    // excluded in the observed result, despite their pending plan metadata.
    EXPECT_EQ(result.excluded, 1u);
    EXPECT_TRUE(SW::stroke_widths_text_output_ready(*document, plan, 601, *token));

    // Per-glyph outcome on both owners: exact local 5 / effective 10, Ordinary.
    for (char const *id : {"ta", "tb"}) {
        auto *owner = item(*document, id);
        ASSERT_TRUE(owner);
        EXPECT_EQ(native_char_count(owner), 2u);
        for (unsigned i = 0; i < 2; ++i) {
            auto const widths = native_char_widths(*document, owner, i);
            EXPECT_DOUBLE_EQ(widths.local, 5.0);
            ASSERT_TRUE(widths.effective.has_value());
            EXPECT_DOUBLE_EQ(*widths.effective, 10.0);
            EXPECT_EQ(widths.convention, SW::StrokeWidthConvention::Ordinary);
        }
    }
    // The excluded bitmap was not a target and its XML is byte-identical.
    bmp = item(*document, "bmp");
    ASSERT_TRUE(bmp);
    EXPECT_EQ(sp_repr_write_buf(bmp->getRepr(), 0, false, GQuark(0), 0, 0).raw(), bmp_before);

    auto const applied = serialize(*document);
    EXPECT_TRUE(token->commitAtomically(
        Inkscape::Util::Internal::ContextString("SW3-B2 two text owners"), "",
        [&] { return SW::stroke_widths_text_output_ready(*document, plan, 601, *token); }));
    ASSERT_TRUE(Inkscape::DocumentUndo::undo(document.get()));
    EXPECT_EQ(serialize(*document), before);
    EXPECT_FALSE(Inkscape::DocumentUndo::undo(document.get()));
    ASSERT_TRUE(Inkscape::DocumentUndo::redo(document.get()));
    EXPECT_EQ(serialize(*document), applied);

    // Buffer round trip: both owners reopen with the same rendered widths.
    auto reopened = parse(applied);
    ASSERT_TRUE(reopened);
    for (char const *id : {"ta", "tb"}) {
        auto *owner = item(*reopened, id);
        ASSERT_TRUE(owner);
        ASSERT_EQ(native_char_count(owner), 2u);
        for (unsigned i = 0; i < 2; ++i) {
            auto const widths = native_char_widths(*reopened, owner, i);
            EXPECT_DOUBLE_EQ(widths.local, 5.0);
            EXPECT_EQ(widths.convention, SW::StrokeWidthConvention::Ordinary);
        }
    }
    token.reset();
}

// ---------------------------------------------------------------------------
// SW3-B2 (multi-text-owner outcome 2): two TextOwner roots in one plan; a real
// single-fire observer on the owner that is written first mutates the
// still-pending second owner's width. The applier must refuse before
// overwriting the pending owner: Failed with changed==0 and exactly the first
// owner's single native write counted. The observer is attached to both owners'
// tspan reprs with one shared first-fire guard, so the outcome does not depend
// on which owner the repair writes first. Per-glyph native observation proves
// one owner reached the intended local 5 while the pending owner kept the
// callback's 7; one caller rollback restores both authored baselines exactly.
// ---------------------------------------------------------------------------

TEST_F(StrokeWidthControllerTest, SW3B2ApplyTextTwoOwnersObserverMutationFails)
{
    auto document = parse(std::string{svg_open} +
        R"svg(<text id="ta" x="10" y="20" transform="scale(2)" style="font-family:Arial;font-size:20px;stroke:black;fill:none">)svg"
        R"svg(<tspan id="sa" style="stroke-width:2">AB</tspan>)svg"
        R"svg(</text>)svg"
        R"svg(<text id="tb" x="10" y="60" transform="scale(2)" style="font-family:Arial;font-size:20px;stroke:black;fill:none">)svg"
        R"svg(<tspan id="sb" style="stroke-width:3">CD</tspan>)svg"
        R"svg(</text>)svg"
        R"(</svg>)");
    ASSERT_TRUE(document);
    settle(*document);
    auto const before = serialize(*document);
    auto *ta = item(*document, "ta");
    auto *tb = item(*document, "tb");
    ASSERT_TRUE(ta);
    ASSERT_TRUE(tb);
    ASSERT_EQ(native_char_count(ta), 2u);
    ASSERT_EQ(native_char_count(tb), 2u);
    // Baseline per-glyph widths from the rendered layout source, never the
    // owner-inherited style.
    EXPECT_DOUBLE_EQ(native_char_stroke(ta, 0).width, 2.0);
    EXPECT_DOUBLE_EQ(native_char_stroke(ta, 1).width, 2.0);
    EXPECT_DOUBLE_EQ(native_char_stroke(tb, 0).width, 3.0);
    EXPECT_DOUBLE_EQ(native_char_stroke(tb, 1).width, 3.0);

    auto const plan = SW::prepare_stroke_widths(*document, {ta, tb}, absolute(10.0), 602);
    ASSERT_EQ(plan.state, SW::StrokeWidthPlanState::Prepared);
    auto const *ma = find_member(plan, ta);
    auto const *mb = find_member(plan, tb);
    ASSERT_TRUE(ma);
    ASSERT_TRUE(mb);
    ASSERT_EQ(ma->text_runs.size(), 1u);
    ASSERT_EQ(mb->text_runs.size(), 1u);
    ASSERT_EQ(ma->text_runs[0].outcome, SW::StrokeWidthMemberOutcome::Change);
    ASSERT_EQ(mb->text_runs[0].outcome, SW::StrokeWidthMemberOutcome::Change);

    auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
    ASSERT_TRUE(token);
    // Whichever owner is written first, its tspan observer mutates the other,
    // still-pending owner exactly once; the shared guard turns the re-triggered
    // second observer into a no-op even though the callback's own write fires it.
    bool handled = false;
    SingleFireStyleObserver obs_a(*item(*document, "sa")->getRepr(), [&] {
        if (handled) return;
        handled = true;
        item(*document, "sb")->setAttribute("style", "stroke-width:7");
    });
    SingleFireStyleObserver obs_b(*item(*document, "sb")->getRepr(), [&] {
        if (handled) return;
        handled = true;
        item(*document, "sa")->setAttribute("style", "stroke-width:7");
    });
    auto const result = SW::apply_stroke_widths_text(*document, plan, 602, *token);

    EXPECT_TRUE(handled) << "no native owner write reached the style observers";
    EXPECT_TRUE(obs_a.fired() || obs_b.fired());
    EXPECT_EQ(result.state, SW::StrokeWidthApplyState::Failed);
    EXPECT_EQ(result.reason, SW::StrokeWidthMemberReason::PostconditionMismatch);
    EXPECT_EQ(result.changed, 0u);
    EXPECT_EQ(result.attempted_writes, 1u);
    EXPECT_FALSE(SW::stroke_widths_text_output_ready(*document, plan, 602, *token));

    // Per-glyph native observation: exactly one owner reached the intended
    // local 5 (the single attempted write) and the other kept the callback's 7.
    ta = item(*document, "ta");
    tb = item(*document, "tb");
    ASSERT_TRUE(ta);
    ASSERT_TRUE(tb);
    double const ta0 = native_char_stroke(ta, 0).width;
    double const ta1 = native_char_stroke(ta, 1).width;
    double const tb0 = native_char_stroke(tb, 0).width;
    double const tb1 = native_char_stroke(tb, 1).width;
    EXPECT_DOUBLE_EQ(ta1, ta0);
    EXPECT_DOUBLE_EQ(tb1, tb0);
    EXPECT_TRUE((ta0 == 5.0 && tb0 == 7.0) || (ta0 == 7.0 && tb0 == 5.0))
        << "expected one written owner local 5 and one callback-mutated local 7; got ta="
        << ta0 << " tb=" << tb0;
    EXPECT_NE(ta0, tb0);

    obs_a.detach();
    obs_b.detach();
    token->rollback();
    EXPECT_EQ(serialize(*document), before);
    ta = item(*document, "ta");
    tb = item(*document, "tb");
    ASSERT_TRUE(ta);
    ASSERT_TRUE(tb);
    EXPECT_DOUBLE_EQ(native_char_stroke(ta, 0).width, 2.0);
    EXPECT_DOUBLE_EQ(native_char_stroke(ta, 1).width, 2.0);
    EXPECT_DOUBLE_EQ(native_char_stroke(tb, 0).width, 3.0);
    EXPECT_DOUBLE_EQ(native_char_stroke(tb, 1).width, 3.0);
}

// An excluded bitmap changing classification during a native text write must
// invalidate the frozen selection outcome before the caller commits it.
TEST_F(StrokeWidthControllerTest, SW3B2TextWriteRejectsChangedExcludedBitmap)
{
    auto document = parse(std::string{svg_open} +
        R"svg(<text id="t" x="10" y="20" transform="scale(2)" style="font-family:Arial;font-size:20px;stroke:black">)svg"
        R"svg(<tspan id="s" style="stroke-width:2">AB</tspan></text>)svg"
        R"(<image id="bmp" width="10" height="10"/>)"
        R"(</svg>)");
    ASSERT_TRUE(document);
    settle(*document);
    auto const before = serialize(*document);
    auto *text = item(*document, "t");
    auto *bitmap = item(*document, "bmp");
    ASSERT_TRUE(text);
    ASSERT_TRUE(bitmap);
    auto const plan = SW::prepare_stroke_widths(*document, {text, bitmap}, absolute(10.0), 603);
    ASSERT_EQ(plan.state, SW::StrokeWidthPlanState::Prepared);
    ASSERT_TRUE(find_excluded(plan.query, SW::StrokeWidthExclusion::Bitmap));

    auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
    ASSERT_TRUE(token);
    SingleFireStyleObserver observer(*item(*document, "s")->getRepr(), [&] {
        bitmap->setAttribute("style", "display:none");
    });
    auto const result = SW::apply_stroke_widths_text(*document, plan, 603, *token);
    EXPECT_TRUE(observer.fired());
    EXPECT_NE(serialize(*document), before);
    EXPECT_EQ(result.state, SW::StrokeWidthApplyState::Failed);
    EXPECT_EQ(result.reason, SW::StrokeWidthMemberReason::PostconditionMismatch);
    EXPECT_EQ(result.changed, 0u);
    EXPECT_EQ(result.attempted_writes, 1u);
    EXPECT_FALSE(SW::stroke_widths_text_output_ready(*document, plan, 603, *token));

    observer.detach();
    token->rollback();
    EXPECT_EQ(serialize(*document), before);
}

// Mixed compatible-member editing uses one frozen plan and one transaction for
// shapes and text; a bitmap child remains outside the write set.
TEST_F(StrokeWidthControllerTest, SW3CCompatibleShapeTextBitmapAtomic)
{
    auto document = parse(std::string{svg_open} +
        R"(<g id="g">)"
        R"(<rect id="r" width="10" height="10" style="stroke:black;stroke-width:2;fill:none"/>)"
        R"svg(<text id="t" x="10" y="40" transform="scale(2)" style="font-family:Arial;font-size:20px;stroke:black;fill:none">)svg"
        R"svg(<tspan id="s" style="stroke-width:3">AB</tspan></text>)svg"
        R"(<image id="bmp" width="10" height="10"/>)"
        R"(</g></svg>)");
    ASSERT_TRUE(document);
    settle(*document);
    auto const before = serialize(*document);
    auto *group = item(*document, "g");
    auto *bitmap = item(*document, "bmp");
    ASSERT_TRUE(group);
    ASSERT_TRUE(bitmap);
    auto const bmp_before = sp_repr_write_buf(bitmap->getRepr(), 0, false, GQuark(0), 0, 0).raw();
    auto const plan = SW::prepare_stroke_widths(*document, {group}, absolute(10.0), 604);
    ASSERT_EQ(plan.state, SW::StrokeWidthPlanState::Prepared);
    EXPECT_EQ(plan.query.eligible, 2u);
    EXPECT_EQ(plan.query.incompatible, 1u);
    ASSERT_TRUE(find_excluded(plan.query, SW::StrokeWidthExclusion::Bitmap));
    EXPECT_EQ(serialize(*document), before);

    auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
    ASSERT_TRUE(token);
    auto const result = SW::apply_stroke_widths_compatible(*document, plan, 604, *token);
    ASSERT_EQ(result.state, SW::StrokeWidthApplyState::Applied);
    EXPECT_EQ(result.reason, SW::StrokeWidthMemberReason::None);
    EXPECT_EQ(result.changed, 2u);
    EXPECT_EQ(result.excluded, 1u);
    EXPECT_EQ(result.attempted_writes, 2u);
    EXPECT_DOUBLE_EQ(computed_width(*document, "r"), 10.0);
    auto *text = item(*document, "t");
    ASSERT_TRUE(text);
    ASSERT_EQ(native_char_count(text), 2u);
    for (unsigned index = 0; index < 2; ++index) {
        auto const width = native_char_widths(*document, text, index);
        EXPECT_DOUBLE_EQ(width.local, 5.0);
        ASSERT_TRUE(width.effective.has_value());
        EXPECT_DOUBLE_EQ(*width.effective, 10.0);
    }
    bitmap = item(*document, "bmp");
    ASSERT_TRUE(bitmap);
    EXPECT_EQ(sp_repr_write_buf(bitmap->getRepr(), 0, false, GQuark(0), 0, 0).raw(), bmp_before);
    EXPECT_TRUE(SW::stroke_widths_compatible_output_ready(*document, plan, 604, *token));

    auto const applied = serialize(*document);
    EXPECT_TRUE(token->commitAtomically(
        Inkscape::Util::Internal::ContextString("SW3-C compatible width"), "",
        [&] { return SW::stroke_widths_compatible_output_ready(*document, plan, 604, *token); }));
    ASSERT_TRUE(Inkscape::DocumentUndo::undo(document.get()));
    EXPECT_EQ(serialize(*document), before);
    ASSERT_TRUE(Inkscape::DocumentUndo::redo(document.get()));
    EXPECT_EQ(serialize(*document), applied);
    token.reset();
}

// A callback from the first native text write changes the still-pending shape.
// The combined writer must refuse to overwrite it and leave rollback to the
// caller's one transaction.
TEST_F(StrokeWidthControllerTest, SW3CCompatibleRejectsPendingShapeMutation)
{
    auto document = parse(std::string{svg_open} +
        R"(<rect id="r" width="10" height="10" style="stroke:black;stroke-width:2"/>)"
        R"svg(<text id="t" x="10" y="40" transform="scale(2)" style="font-family:Arial;font-size:20px;stroke:black">)svg"
        R"svg(<tspan id="s" style="stroke-width:3">AB</tspan></text></svg>)svg");
    ASSERT_TRUE(document);
    settle(*document);
    auto const before = serialize(*document);
    auto *rect = item(*document, "r");
    auto *text = item(*document, "t");
    ASSERT_TRUE(rect);
    ASSERT_TRUE(text);
    auto const plan = SW::prepare_stroke_widths(*document, {rect, text}, absolute(10.0), 605);
    ASSERT_EQ(plan.state, SW::StrokeWidthPlanState::Prepared);

    auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
    ASSERT_TRUE(token);
    SingleFireStyleObserver observer(*item(*document, "s")->getRepr(), [&] {
        rect->setAttribute("style", "stroke:black;stroke-width:7");
    });
    auto const result = SW::apply_stroke_widths_compatible(*document, plan, 605, *token);
    EXPECT_TRUE(observer.fired());
    EXPECT_EQ(result.state, SW::StrokeWidthApplyState::Failed);
    EXPECT_EQ(result.reason, SW::StrokeWidthMemberReason::PostconditionMismatch);
    EXPECT_EQ(result.changed, 0u);
    EXPECT_EQ(result.attempted_writes, 1u);
    EXPECT_FALSE(SW::stroke_widths_compatible_output_ready(*document, plan, 605, *token));

    observer.detach();
    token->rollback();
    EXPECT_EQ(serialize(*document), before);
}

TEST_F(StrokeWidthControllerTest, SW3CCompatibleExplicitTextRangeAndShape)
{
    auto document = parse(std::string{svg_open} +
        R"(<rect id="r" width="10" height="10" style="stroke-width:2"/>)"
        R"svg(<text id="t" x="10" y="40" style="font-family:Arial;font-size:20px;stroke:black;stroke-width:2">AB</text></svg>)svg");
    ASSERT_TRUE(document);
    settle(*document);
    auto const before = serialize(*document);
    auto *rect = item(*document, "r");
    auto *text = item(*document, "t");
    ASSERT_TRUE(rect);
    ASSERT_TRUE(text);
    SW::StrokeWidthTextRange range;
    range.owner = text;
    range.caret = false;
    range.first_char = 0;
    range.last_char = 1;
    // Owner decision 2026-09-24: a text subrange and separate shape both change.
    auto const plan = SW::prepare_stroke_widths_combined(*document, range, {rect, text}, absolute(10.0), 606);
    ASSERT_EQ(plan.state, SW::StrokeWidthPlanState::Prepared);
    ASSERT_TRUE(plan.text_scope.has_value());

    // The legacy range is intentionally never broadened by the compatible writer.
    auto legacy_plan = SW::prepare_stroke_widths(*document, range, {rect, text}, absolute(10.0), 606);
    ASSERT_EQ(legacy_plan.state, SW::StrokeWidthPlanState::Prepared);
    auto legacy_token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
    ASSERT_TRUE(legacy_token);
    auto const legacy = SW::apply_stroke_widths_compatible(*document, legacy_plan, 606, *legacy_token);
    EXPECT_EQ(legacy.state, SW::StrokeWidthApplyState::Rejected);
    EXPECT_EQ(legacy.reason, SW::StrokeWidthMemberReason::UnsupportedIntent);
    EXPECT_EQ(legacy.attempted_writes, 0u);
    EXPECT_EQ(serialize(*document), before);
    legacy_token->rollback();

    auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
    ASSERT_TRUE(token);
    auto const result = SW::apply_stroke_widths_compatible(*document, plan, 606, *token);
    // Owner decision 2026-09-24: compatible changes commit in one Undo.
    ASSERT_EQ(result.state, SW::StrokeWidthApplyState::Applied);
    EXPECT_EQ(result.changed, 2u);
    EXPECT_DOUBLE_EQ(computed_width(*document, "r"), 10.0);
    EXPECT_DOUBLE_EQ(native_char_widths(*document, text, 0).local, 10.0);
    EXPECT_DOUBLE_EQ(native_char_widths(*document, text, 1).local, 2.0);
    EXPECT_TRUE(token->commitAtomically(
        Inkscape::Util::Internal::ContextString("Combined range width"), "",
        [&] { return SW::stroke_widths_compatible_output_ready(*document, plan, 606, *token); }));
    EXPECT_TRUE(Inkscape::DocumentUndo::undo(document.get()));
    EXPECT_EQ(serialize(*document), before);
}

TEST_F(StrokeWidthControllerTest, SW3CCombinedTextSubrangeShapesBitmapOneUndo)
{
    auto document = parse(std::string{svg_open} +
        R"(<rect id="r" width="10" height="10" style="stroke:black;stroke-width:2"/>)"
        R"svg(<text id="t" x="10" y="40" style="font-family:Arial;font-size:20px;stroke:black">)svg"
        R"svg(<tspan id="a" style="stroke-width:2">AB</tspan><tspan id="b" style="stroke-width:8">CD</tspan></text>)svg"
        R"(<image id="bmp" width="10" height="10"/></svg>)");
    ASSERT_TRUE(document);
    settle(*document);
    auto const before = serialize(*document);
    auto *rect = item(*document, "r");
    auto *owner = item(*document, "t");
    auto *bitmap = item(*document, "bmp");
    ASSERT_TRUE(rect && owner && bitmap);
    auto const bitmap_before = sp_repr_write_buf(bitmap->getRepr(), 0, false, GQuark(0), 0, 0).raw();
    SW::StrokeWidthTextRange range;
    range.owner = owner;
    range.first_char = 1;
    range.last_char = 3;
    // Owner decision 2026-09-24: explicit text subrange and separate shapes share one edit.
    auto const plan = SW::prepare_stroke_widths_combined(
        *document, range, {rect, owner, bitmap}, absolute(10.0), 608);
    ASSERT_EQ(plan.state, SW::StrokeWidthPlanState::Prepared);
    // r: eligible shape; t: eligible range owner; bmp: incompatible bitmap.
    EXPECT_EQ(plan.query.eligible, 2u);
    EXPECT_EQ(plan.query.incompatible, 1u);
    EXPECT_EQ(plan.query.unavailable, 0u);
    EXPECT_EQ(plan.query.excluded.size(), 1u);
    EXPECT_EQ(plan.excluded, 2u); // bitmap plus pending text owner
    EXPECT_EQ(plan.query.paint_none, 0u);
    EXPECT_EQ(plan.query.hairline, 0u);
    EXPECT_EQ(plan.query.non_scaling, 0u);
    EXPECT_TRUE(find_excluded(plan.query, SW::StrokeWidthExclusion::Bitmap));

    auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
    ASSERT_TRUE(token);
    auto const result = SW::apply_stroke_widths_compatible(*document, plan, 608, *token);
    ASSERT_EQ(result.state, SW::StrokeWidthApplyState::Applied)
        << "reason=" << static_cast<int>(result.reason) << " attempted=" << result.attempted_writes;
    EXPECT_EQ(result.changed, 2u);
    EXPECT_EQ(result.excluded, 1u);
    // WP1c: adjacent runs with an identical write are covered by one native call (3 runs, 2 calls).
    EXPECT_EQ(result.attempted_writes, 2u);
    EXPECT_DOUBLE_EQ(computed_width(*document, "r"), 10.0);
    EXPECT_DOUBLE_EQ(native_char_widths(*document, owner, 0).local, 2.0);
    EXPECT_DOUBLE_EQ(native_char_widths(*document, owner, 1).local, 10.0);
    EXPECT_DOUBLE_EQ(native_char_widths(*document, owner, 2).local, 10.0);
    EXPECT_DOUBLE_EQ(native_char_widths(*document, owner, 3).local, 8.0);
    EXPECT_EQ(sp_repr_write_buf(bitmap->getRepr(), 0, false, GQuark(0), 0, 0).raw(), bitmap_before);
    EXPECT_TRUE(SW::stroke_widths_compatible_output_ready(*document, plan, 608, *token));
    auto const applied = serialize(*document);
    EXPECT_TRUE(token->commitAtomically(
        Inkscape::Util::Internal::ContextString("Combined stroke width"), "",
        [&] { return SW::stroke_widths_compatible_output_ready(*document, plan, 608, *token); }));
    ASSERT_TRUE(Inkscape::DocumentUndo::undo(document.get()));
    EXPECT_EQ(serialize(*document), before);
    ASSERT_TRUE(Inkscape::DocumentUndo::redo(document.get()));
    EXPECT_EQ(serialize(*document), applied);
}

TEST_F(StrokeWidthControllerTest, HairlineMixedGroupBitmapOneUndo)
{
    auto document = mixed_fixture();
    ASSERT_TRUE(document);
    settle(*document);
    auto const before = serialize(*document);
    auto *group = item(*document, "mixed");
    auto *bitmap = item(*document, "bitmap");
    auto *protected_shape = item(*document, "protected");
    ASSERT_TRUE(group && bitmap && protected_shape);
    auto const bitmap_before = sp_repr_write_buf(bitmap->getRepr(), 0, false, GQuark(0), 0, 0).raw();
    auto const protected_before = sp_repr_write_buf(protected_shape->getRepr(), 0, false, GQuark(0), 0, 0).raw();
    auto const outside_before = sp_repr_write_buf(item(*document, "outside")->getRepr(),
                                                  0, false, GQuark(0), 0, 0).raw();
    SW::StrokeWidthIntent hairline;
    hairline.kind = SW::StrokeWidthIntentKind::Hairline;
    hairline.scale_dashes = true;
    auto const plan = SW::prepare_stroke_widths(*document,
                                                {group, item(*document, "thin")}, hairline, 609);
    ASSERT_EQ(plan.state, SW::StrokeWidthPlanState::Prepared);
    EXPECT_EQ(plan.query.covered, 1u);
    EXPECT_TRUE(find_excluded(plan.query, SW::StrokeWidthExclusion::Bitmap));
    EXPECT_TRUE(find_excluded(plan.query, SW::StrokeWidthExclusion::LockedAncestor));
    auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
    ASSERT_TRUE(token);
    auto const result = SW::apply_stroke_widths_compatible(*document, plan, 609, *token);
    ASSERT_EQ(result.state, SW::StrokeWidthApplyState::Applied)
        << "reason=" << static_cast<int>(result.reason) << " attempted=" << result.attempted_writes;
    EXPECT_EQ(result.changed, 4u);
    EXPECT_EQ(result.excluded, 2u);
    // Whole-owner edits retain the four source elements plus two shapes.
    EXPECT_EQ(result.attempted_writes, 6u);
    EXPECT_EQ(SW::query_stroke_widths(*document, {item(*document, "thin")}).hairline, 1u);
    EXPECT_EQ(SW::query_stroke_widths(*document, {item(*document, "thick")}).hairline, 1u);
    EXPECT_EQ(SW::query_stroke_widths(*document, {item(*document, "text")}).hairline, 1u);
    EXPECT_EQ(SW::query_stroke_widths(*document, {item(*document, "non-scaling")}).hairline, 1u);
    EXPECT_EQ(SW::query_stroke_widths(*document, {item(*document, "non-scaling")}).non_scaling, 0u);
    // Each dashed member scales its own authored pattern to the 1px fallback.
    ASSERT_EQ(item(*document, "thin")->style->stroke_dasharray.get_computed().size(), 2u);
    EXPECT_DOUBLE_EQ(item(*document, "thin")->style->stroke_dasharray.get_computed()[0], 2.0);
    EXPECT_DOUBLE_EQ(item(*document, "thin")->style->stroke_dasharray.get_computed()[1], 1.0);
    EXPECT_EQ(sp_repr_write_buf(bitmap->getRepr(), 0, false, GQuark(0), 0, 0).raw(), bitmap_before);
    EXPECT_EQ(sp_repr_write_buf(protected_shape->getRepr(), 0, false, GQuark(0), 0, 0).raw(), protected_before);
    EXPECT_EQ(sp_repr_write_buf(item(*document, "outside")->getRepr(),
                                0, false, GQuark(0), 0, 0).raw(), outside_before);
    EXPECT_TRUE(SW::stroke_widths_compatible_output_ready(*document, plan, 609, *token));
    auto const applied = serialize(*document);
    EXPECT_TRUE(token->commitAtomically(
        Inkscape::Util::Internal::ContextString("Hairline width"), "",
        [&] { return SW::stroke_widths_compatible_output_ready(*document, plan, 609, *token); }));
    ASSERT_TRUE(Inkscape::DocumentUndo::undo(document.get()));
    EXPECT_EQ(serialize(*document), before);
    ASSERT_TRUE(Inkscape::DocumentUndo::redo(document.get()));
    EXPECT_EQ(serialize(*document), applied);
}

TEST_F(StrokeWidthControllerTest, RemoveStrokeSkipsBitmapAndProtected)
{
    auto document = mixed_fixture();
    ASSERT_TRUE(document);
    settle(*document);
    auto const before = serialize(*document);
    auto *group = item(*document, "mixed");
    auto *bitmap = item(*document, "bitmap");
    auto *protected_shape = item(*document, "protected");
    ASSERT_TRUE(group && bitmap && protected_shape);
    auto const bitmap_before = sp_repr_write_buf(bitmap->getRepr(), 0, false, GQuark(0), 0, 0).raw();
    auto const protected_before = sp_repr_write_buf(protected_shape->getRepr(), 0, false, GQuark(0), 0, 0).raw();
    auto const thin_width = computed_width(*document, "thin");
    auto const thin_dash = item(*document, "thin")->style->stroke_dasharray.get_computed();
    auto const ns_width = computed_width(*document, "non-scaling");
    auto const ns_vector = item(*document, "non-scaling")->style->vector_effect.get_value().raw();
    auto const hair_width = computed_width(*document, "hairline");
    auto const hair_vector = item(*document, "hairline")->style->vector_effect.get_value().raw();
    auto const hair_extension = item(*document, "hairline")->style->stroke_extensions.hairline;
    SW::StrokeWidthIntent remove;
    remove.kind = SW::StrokeWidthIntentKind::RemoveStroke;
    auto const plan = SW::prepare_stroke_widths(*document, {group}, remove, 610);
    ASSERT_EQ(plan.state, SW::StrokeWidthPlanState::Prepared);
    auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
    ASSERT_TRUE(token);
    auto const result = SW::apply_stroke_widths_compatible(*document, plan, 610, *token);
    ASSERT_EQ(result.state, SW::StrokeWidthApplyState::Applied)
        << "reason=" << static_cast<int>(result.reason) << " attempted=" << result.attempted_writes;
    EXPECT_TRUE(item(*document, "thin")->style->stroke.isNone());
    EXPECT_TRUE(item(*document, "thick")->style->stroke.isNone());
    EXPECT_DOUBLE_EQ(computed_width(*document, "thin"), thin_width);
    EXPECT_EQ(item(*document, "thin")->style->stroke_dasharray.get_computed(), thin_dash);
    EXPECT_DOUBLE_EQ(computed_width(*document, "non-scaling"), ns_width);
    EXPECT_EQ(item(*document, "non-scaling")->style->vector_effect.get_value().raw(), ns_vector);
    EXPECT_DOUBLE_EQ(computed_width(*document, "hairline"), hair_width);
    EXPECT_EQ(item(*document, "hairline")->style->vector_effect.get_value().raw(), hair_vector);
    EXPECT_EQ(item(*document, "hairline")->style->stroke_extensions.hairline, hair_extension);
    for (unsigned index = 0; index < 3; ++index) {
        SW::StrokeWidthTextRange glyph;
        glyph.owner = item(*document, "text");
        glyph.first_char = index;
        glyph.last_char = index + 1;
        auto const rendered = SW::query_stroke_widths(*document, glyph, {item(*document, "text")});
        EXPECT_EQ(rendered.paint_none, 1u) << "character " << index;
    }
    EXPECT_EQ(sp_repr_write_buf(bitmap->getRepr(), 0, false, GQuark(0), 0, 0).raw(), bitmap_before);
    EXPECT_EQ(sp_repr_write_buf(protected_shape->getRepr(), 0, false, GQuark(0), 0, 0).raw(), protected_before);
    EXPECT_TRUE(SW::stroke_widths_compatible_output_ready(*document, plan, 610, *token));
    auto const applied = serialize(*document);
    EXPECT_TRUE(token->commitAtomically(
        Inkscape::Util::Internal::ContextString("Remove stroke"), "",
        [&] { return SW::stroke_widths_compatible_output_ready(*document, plan, 610, *token); }));
    ASSERT_TRUE(Inkscape::DocumentUndo::undo(document.get()));
    EXPECT_EQ(serialize(*document), before);
    ASSERT_TRUE(Inkscape::DocumentUndo::redo(document.get()));
    EXPECT_EQ(serialize(*document), applied);
}

TEST_F(StrokeWidthControllerTest, AbsoluteConvertsHairlineAndPreservesPaintDash)
{
    auto document = mixed_fixture();
    ASSERT_TRUE(document);
    settle(*document);
    auto *hair = item(*document, "hairline");
    auto *thin = item(*document, "thin");
    auto *non_scaling = item(*document, "non-scaling");
    ASSERT_TRUE(hair && thin && non_scaling);
    auto const before = serialize(*document);
    auto const hair_paint_before = hair->style->stroke.isNone();
    auto const thin_dash_before = thin->style->stroke_dasharray.get_computed();
    auto const plan = SW::prepare_stroke_widths(*document, {hair, thin, non_scaling}, absolute(6.0), 611);
    ASSERT_EQ(plan.state, SW::StrokeWidthPlanState::Prepared);
    ASSERT_EQ(plan.planned_changes, 3u);
    auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
    ASSERT_TRUE(token);
    auto const result = SW::apply_stroke_widths_compatible(*document, plan, 611, *token);
    ASSERT_EQ(result.state, SW::StrokeWidthApplyState::Applied)
        << "reason=" << static_cast<int>(result.reason) << " attempted=" << result.attempted_writes;
    EXPECT_EQ(SW::query_stroke_widths(*document, {hair}).hairline, 0u);
    EXPECT_EQ(SW::query_stroke_widths(*document, {hair}).non_scaling, 0u);
    EXPECT_EQ(hair->style->vector_effect.get_value().raw(), "none");
    EXPECT_EQ(thin->style->vector_effect.get_value().raw(), "none");
    EXPECT_EQ(non_scaling->style->vector_effect.get_value().raw(), "non-scaling-stroke");
    EXPECT_EQ(SW::query_stroke_widths(*document, {non_scaling}).non_scaling, 1u);
    EXPECT_NEAR(computed_width(*document, "hairline"), 6.0, 1e-9);
    EXPECT_DOUBLE_EQ(computed_width(*document, "thin"), 6.0);
    EXPECT_EQ(hair->style->stroke.isNone(), hair_paint_before);
    EXPECT_EQ(thin->style->stroke_dasharray.get_computed(), thin_dash_before);
    EXPECT_TRUE(token->commitAtomically(
        Inkscape::Util::Internal::ContextString("Absolute width"), "",
        [&] { return SW::stroke_widths_compatible_output_ready(*document, plan, 611, *token); }));
    EXPECT_TRUE(Inkscape::DocumentUndo::undo(document.get()));
    EXPECT_EQ(serialize(*document), before);
}

TEST_F(StrokeWidthControllerTest, AbsoluteHairlineUsesScalingWidthAfterConversion)
{
    auto document = parse(std::string{svg_open} +
        R"svg(<rect id="h" width="10" height="10" transform="scale(2)" )svg"
        R"svg(style="stroke:black;stroke-width:1;vector-effect:non-scaling-stroke;-inkscape-stroke:hairline"/>)svg"
        R"svg(<rect id="n" x="20" width="10" height="10" transform="scale(2)" )svg"
        R"svg(style="stroke:black;stroke-width:2;vector-effect:non-scaling-stroke"/></svg>)svg");
    ASSERT_TRUE(document);
    settle(*document);
    auto *hair = item(*document, "h");
    auto *ordinary_ns = item(*document, "n");
    ASSERT_TRUE(hair && ordinary_ns);
    auto const plan = SW::prepare_stroke_widths(*document, {hair, ordinary_ns}, absolute(6.0), 620);
    ASSERT_EQ(plan.state, SW::StrokeWidthPlanState::Prepared);
    auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
    ASSERT_TRUE(token);
    auto const result = SW::apply_stroke_widths_compatible(*document, plan, 620, *token);
    ASSERT_EQ(result.state, SW::StrokeWidthApplyState::Applied)
        << static_cast<int>(result.reason);
    EXPECT_DOUBLE_EQ(hair->style->stroke_width.computed, 3.0);
    auto const converted = SW::query_stroke_widths(*document, {hair});
    ASSERT_TRUE(converted.uniform_px.has_value());
    EXPECT_DOUBLE_EQ(*converted.uniform_px, 6.0);
    EXPECT_EQ(hair->style->vector_effect.get_value().raw(), "none");
    EXPECT_DOUBLE_EQ(ordinary_ns->style->stroke_width.computed, 6.0);
    EXPECT_EQ(ordinary_ns->style->vector_effect.get_value().raw(), "non-scaling-stroke");
    token->rollback();
}

TEST_F(StrokeWidthControllerTest, SW3CCombinedRangeInsideGroupCountsOnce)
{
    auto document = mixed_fixture();
    ASSERT_TRUE(document);
    settle(*document);
    auto const before = serialize(*document);
    auto *group = item(*document, "mixed");
    auto *owner = item(*document, "text");
    auto *outside = item(*document, "outside");
    ASSERT_TRUE(group && owner && outside);
    auto const outside_before = sp_repr_write_buf(outside->getRepr(), 0, false, GQuark(0), 0, 0).raw();
    SW::StrokeWidthTextRange range;
    range.owner = owner;
    range.first_char = 1;
    range.last_char = 2;
    auto const plan = SW::prepare_stroke_widths_combined(
        *document, range, {group, item(*document, "thin")}, absolute(6.0), 612);
    ASSERT_EQ(plan.state, SW::StrokeWidthPlanState::Prepared);
    // mixed/nested: traversal wrappers; thin/thick/non-scaling/hairline:
    // eligible shapes; text: eligible range owner; bitmap: incompatible;
    // protected: unavailable; repeated selected thin: covered.
    EXPECT_EQ(plan.query.eligible, 5u);
    EXPECT_EQ(plan.query.incompatible, 1u);
    EXPECT_EQ(plan.query.unavailable, 1u);
    EXPECT_EQ(plan.query.covered, 1u);
    EXPECT_EQ(plan.query.excluded.size(), 3u);
    EXPECT_EQ(plan.query.hairline, 1u);
    EXPECT_EQ(plan.query.non_scaling, 1u);
    EXPECT_EQ(plan.excluded, 3u); // bitmap, protected shape, pending text owner
    auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
    ASSERT_TRUE(token);
    auto const result = SW::apply_stroke_widths_compatible(*document, plan, 612, *token);
    ASSERT_EQ(result.state, SW::StrokeWidthApplyState::Applied)
        << static_cast<int>(result.reason);
    EXPECT_EQ(result.changed, 5u);
    EXPECT_EQ(result.excluded, 2u);
    EXPECT_EQ(result.attempted_writes, 5u);
    EXPECT_DOUBLE_EQ(native_char_widths(*document, owner, 0).local, 0.0);
    EXPECT_DOUBLE_EQ(native_char_widths(*document, owner, 1).local, 6.0);
    EXPECT_DOUBLE_EQ(native_char_widths(*document, owner, 2).local, 8.0);
    EXPECT_EQ(sp_repr_write_buf(outside->getRepr(), 0, false, GQuark(0), 0, 0).raw(), outside_before);
    EXPECT_TRUE(token->commitAtomically(
        Inkscape::Util::Internal::ContextString("Combined group range"), "",
        [&] { return SW::stroke_widths_compatible_output_ready(*document, plan, 612, *token); }));
    ASSERT_TRUE(Inkscape::DocumentUndo::undo(document.get()));
    EXPECT_EQ(serialize(*document), before);
}

TEST_F(StrokeWidthControllerTest, SW3CCombinedQueryCountsPaintNoneOnce)
{
    auto document = parse(std::string{svg_open} +
        R"(<rect id="r" width="10" height="10" style="stroke:none;stroke-width:2"/>)"
        R"(<image id="bmp" width="10" height="10"/>)"
        R"(<text id="t" x="10" y="40" style="font-family:Arial;font-size:20px;stroke:black;stroke-width:2">AB</text></svg>)");
    ASSERT_TRUE(document);
    SW::StrokeWidthTextRange range;
    range.owner = item(*document, "t");
    range.first_char = 0;
    range.last_char = 1;
    auto const plan = SW::prepare_stroke_widths_combined(
        *document, range, {item(*document, "r"), item(*document, "bmp"), item(*document, "t")},
        absolute(6.0), 621);
    auto const &query = plan.query;
    // r: eligible shape with paint none; t: eligible range owner;
    // bmp: incompatible bitmap.
    EXPECT_FALSE(query.range_rejected);
    EXPECT_EQ(query.eligible, 2u);
    EXPECT_EQ(query.paint_none, 1u);
    EXPECT_EQ(query.incompatible, 1u);
    EXPECT_EQ(query.unavailable, 0u);
    EXPECT_EQ(query.excluded.size(), 1u);
}

TEST_F(StrokeWidthControllerTest, SW3CProtectedAndStaleRangeSafety)
{
    auto document = parse(std::string{svg_open} +
        R"(<g id="g"><rect id="r" width="10" height="10" style="stroke:black;stroke-width:2"/>)"
        R"(<g id="locked" sodipodi:insensitive="true"><text id="t" x="10" y="40" )"
        R"(style="font-family:Arial;font-size:20px;stroke:black;stroke-width:2">AB</text></g></g></svg>)");
    ASSERT_TRUE(document);
    settle(*document);
    auto *group = item(*document, "g");
    auto *owner = item(*document, "t");
    ASSERT_TRUE(group && owner);
    auto const protected_before = sp_repr_write_buf(owner->getRepr(), 0, false, GQuark(0), 0, 0).raw();
    SW::StrokeWidthTextRange range;
    range.owner = owner;
    range.first_char = 0;
    range.last_char = 1;
    auto const plan = SW::prepare_stroke_widths_combined(*document, range, {group}, absolute(6.0), 613);
    ASSERT_EQ(plan.state, SW::StrokeWidthPlanState::Prepared);
    // g: traversal wrapper; r: eligible shape; locked: unavailable protected
    // group; t: unavailable protected range owner. The stale range below rejects
    // before traversal and contributes no target record.
    EXPECT_EQ(plan.query.eligible, 1u);
    EXPECT_EQ(plan.query.unavailable, 2u);
    auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
    ASSERT_TRUE(token);
    auto const applied = SW::apply_stroke_widths_compatible(*document, plan, 613, *token);
    EXPECT_EQ(applied.state, SW::StrokeWidthApplyState::Applied);
    EXPECT_EQ(applied.changed, 1u);
    EXPECT_EQ(applied.excluded, 2u);
    EXPECT_EQ(sp_repr_write_buf(owner->getRepr(), 0, false, GQuark(0), 0, 0).raw(), protected_before);
    token->rollback();

    range.last_char = 3;
    auto const stale = SW::prepare_stroke_widths_combined(*document, range, {group}, absolute(6.0), 614);
    EXPECT_EQ(stale.state, SW::StrokeWidthPlanState::Rejected);
    EXPECT_EQ(stale.query.range_exclusion, SW::StrokeWidthExclusion::TextRangeOutOfBounds);
}

TEST_F(StrokeWidthControllerTest, SW3CTextOnlyCompatiblePathPreservesUnselectedRun)
{
    auto document = parse(std::string{svg_open} +
        R"(<text id="t" x="10" y="40" style="font-family:Arial;font-size:20px;stroke:black">)"
        R"(<tspan id="a" style="stroke-width:2">AB</tspan>)"
        R"(<tspan id="b" style="stroke-width:8">CD</tspan></text></svg>)");
    ASSERT_TRUE(document);
    settle(*document);
    auto const before = serialize(*document);
    auto *owner = item(*document, "t");
    ASSERT_TRUE(owner);
    SW::StrokeWidthTextRange range;
    range.owner = owner;
    range.first_char = 0;
    range.last_char = 2;
    auto const plan = SW::prepare_stroke_widths_combined(*document, range, {owner}, absolute(6.0), 615);
    ASSERT_EQ(plan.state, SW::StrokeWidthPlanState::Prepared);
    EXPECT_EQ(plan.query.eligible, 1u);
    auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
    ASSERT_TRUE(token);
    auto const result = SW::apply_stroke_widths_compatible(*document, plan, 615, *token);
    ASSERT_EQ(result.state, SW::StrokeWidthApplyState::Applied);
    EXPECT_EQ(result.changed, 1u);
    EXPECT_EQ(result.excluded, 0u);
    EXPECT_EQ(result.attempted_writes, 1u);
    EXPECT_DOUBLE_EQ(native_char_widths(*document, owner, 0).local, 6.0);
    EXPECT_DOUBLE_EQ(native_char_widths(*document, owner, 2).local, 8.0);
    EXPECT_TRUE(token->commitAtomically(
        Inkscape::Util::Internal::ContextString("Text only compatible"), "",
        [&] { return SW::stroke_widths_compatible_output_ready(*document, plan, 615, *token); }));
    ASSERT_TRUE(Inkscape::DocumentUndo::undo(document.get()));
    EXPECT_EQ(serialize(*document), before);
}

TEST_F(StrokeWidthControllerTest, SW3CTextOnlyCompatibleRejectsStaleContent)
{
    auto document = parse(std::string{svg_open} +
        R"(<text id="t" x="10" y="40" style="font-family:Arial;font-size:20px;stroke:black">)"
        R"(<tspan style="stroke-width:2">AB</tspan><tspan style="stroke-width:8">CD</tspan>)"
        R"(</text></svg>)");
    ASSERT_TRUE(document);
    settle(*document);
    auto *owner = item(*document, "t");
    ASSERT_TRUE(owner);
    SW::StrokeWidthTextRange range;
    range.owner = owner;
    range.first_char = 0;
    range.last_char = 2;
    auto const plan = SW::prepare_stroke_widths_combined(*document, range, {owner}, absolute(6.0), 619);
    ASSERT_EQ(plan.state, SW::StrokeWidthPlanState::Prepared);
    auto const *layout = te_get_layout(owner);
    ASSERT_TRUE(layout);
    sp_te_replace(owner, layout->charIndexToIterator(3), layout->charIndexToIterator(4), "X");
    document->ensureUpToDate();
    settle(*document);
    auto const stale_before = serialize(*document);
    auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
    ASSERT_TRUE(token);
    auto const result = SW::apply_stroke_widths_compatible(*document, plan, 619, *token);
    EXPECT_EQ(result.state, SW::StrokeWidthApplyState::Rejected);
    EXPECT_EQ(result.reason, SW::StrokeWidthMemberReason::StalePlan);
    EXPECT_EQ(result.attempted_writes, 0u);
    EXPECT_EQ(serialize(*document), stale_before);
    token->rollback();
}

TEST_F(StrokeWidthControllerTest, SW3CNumericTextRunsPreserveConventionsAndScaleDashes)
{
    auto document = parse(std::string{svg_open} +
        R"(<rect id="r" width="10" height="10" style="stroke:black;stroke-width:2"/>)"
        R"(<text id="t" x="10" y="40" style="font-family:Arial;font-size:20px;stroke:black">)"
        R"(<tspan id="a" style="stroke-width:2">A</tspan>)"
        R"(<tspan id="h" style="stroke-width:1;vector-effect:non-scaling-stroke;-inkscape-stroke:hairline">B</tspan>)"
        R"(<tspan id="d" style="stroke-width:2;stroke-dasharray:4,2">C</tspan></text></svg>)");
    ASSERT_TRUE(document);
    settle(*document);
    auto *rect = item(*document, "r");
    auto *owner = item(*document, "t");
    ASSERT_TRUE(rect && owner);
    auto const plan = SW::prepare_stroke_widths(*document, {rect, owner}, absolute(6.0, true), 616);
    ASSERT_EQ(plan.state, SW::StrokeWidthPlanState::Prepared);
    auto const *text_member = find_member(plan, owner);
    ASSERT_TRUE(text_member);
    ASSERT_EQ(text_member->text_runs.size(), 3u);
    EXPECT_EQ(text_member->text_runs[0].outcome, SW::StrokeWidthMemberOutcome::Change);
    EXPECT_EQ(text_member->text_runs[1].outcome, SW::StrokeWidthMemberOutcome::Change);
    EXPECT_EQ(text_member->text_runs[2].outcome, SW::StrokeWidthMemberOutcome::Change);
    auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
    ASSERT_TRUE(token);
    auto const result = SW::apply_stroke_widths_compatible(*document, plan, 616, *token);
    ASSERT_EQ(result.state, SW::StrokeWidthApplyState::Applied)
        << static_cast<int>(result.reason);
    EXPECT_EQ(result.changed, 2u);
    EXPECT_EQ(result.attempted_writes, 4u);
    EXPECT_DOUBLE_EQ(computed_width(*document, "r"), 6.0);
    EXPECT_DOUBLE_EQ(native_char_widths(*document, owner, 0).local, 6.0);
    EXPECT_EQ(native_char_widths(*document, owner, 1).convention, SW::StrokeWidthConvention::Ordinary);
    EXPECT_EQ(native_char_widths(*document, owner, 1).style_source->style->vector_effect.get_value().raw(),
              "none");
    EXPECT_DOUBLE_EQ(native_char_widths(*document, owner, 2).local, 6.0);
    EXPECT_EQ(native_char_widths(*document, owner, 2).style_source->style->stroke_dasharray.get_computed(),
              (std::vector<double>{12.0, 6.0}));
    token->rollback();
}

TEST_F(StrokeWidthControllerTest, SW3CCombinedTextPathKeepsReferencedGeometry)
{
    auto document = parse(std::string{svg_open} +
        R"(<defs><path id="path" d="M0,0 L100,0"/></defs>)"
        R"(<rect id="r" width="10" height="10" style="stroke:black;stroke-width:2"/>)"
        R"(<text id="t" style="font-family:Arial;font-size:20px;stroke:black;stroke-width:2">)"
        R"(<textPath xlink:href="#path">AB</textPath></text></svg>)");
    ASSERT_TRUE(document);
    settle(*document);
    auto *owner = item(*document, "t");
    auto *path = item(*document, "path");
    ASSERT_TRUE(owner && path);
    auto const path_before = sp_repr_write_buf(path->getRepr(), 0, false, GQuark(0), 0, 0).raw();
    SW::StrokeWidthTextRange range;
    range.owner = owner;
    range.first_char = 0;
    range.last_char = 1;
    auto const plan = SW::prepare_stroke_widths_combined(
        *document, range, {owner, item(*document, "r")}, absolute(6.0), 617);
    ASSERT_EQ(plan.state, SW::StrokeWidthPlanState::Prepared);
    EXPECT_EQ(plan.query.eligible, 2u);
    auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
    ASSERT_TRUE(token);
    auto const result = SW::apply_stroke_widths_compatible(*document, plan, 617, *token);
    ASSERT_EQ(result.state, SW::StrokeWidthApplyState::Applied)
        << static_cast<int>(result.reason);
    EXPECT_EQ(result.changed, 2u);
    EXPECT_DOUBLE_EQ(native_char_widths(*document, owner, 0).local, 6.0);
    EXPECT_DOUBLE_EQ(native_char_widths(*document, owner, 1).local, 2.0);
    EXPECT_EQ(sp_repr_write_buf(path->getRepr(), 0, false, GQuark(0), 0, 0).raw(), path_before);
    token->rollback();
}

TEST_F(StrokeWidthControllerTest, SW3CCombinedFlowedTextAndShape)
{
    auto document = parse(std::string{svg_open} +
        R"(<rect id="r" width="10" height="10" style="stroke:black;stroke-width:2"/>)"
        R"(<flowRoot id="f" style="font-family:Arial;font-size:20px;stroke:black">)"
        R"(<flowRegion><rect width="200" height="100"/></flowRegion>)"
        R"(<flowPara style="stroke-width:2">AB</flowPara></flowRoot></svg>)");
    ASSERT_TRUE(document);
    settle(*document);
    auto *owner = item(*document, "f");
    ASSERT_TRUE(owner);
    SW::StrokeWidthTextRange range;
    range.owner = owner;
    range.first_char = 0;
    range.last_char = 1;
    auto const plan = SW::prepare_stroke_widths_combined(
        *document, range, {owner, item(*document, "r")}, absolute(6.0), 618);
    ASSERT_EQ(plan.state, SW::StrokeWidthPlanState::Prepared);
    EXPECT_EQ(plan.query.eligible, 2u);
    auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
    ASSERT_TRUE(token);
    auto const result = SW::apply_stroke_widths_compatible(*document, plan, 618, *token);
    ASSERT_EQ(result.state, SW::StrokeWidthApplyState::Applied)
        << static_cast<int>(result.reason);
    EXPECT_EQ(result.changed, 2u);
    EXPECT_DOUBLE_EQ(native_char_widths(*document, owner, 0).local, 6.0);
    EXPECT_DOUBLE_EQ(native_char_widths(*document, owner, 1).local, 2.0);
    token->rollback();
}

// Live Fill and Stroke regression: a plain text owner may use presentation
// attributes rather than an inline style or tspan. A mixed selection must
// update that text and a shape while preserving the bitmap and authored XML.
TEST_F(StrokeWidthControllerTest, SW3CPlainPresentationTextWidth)
{
    auto document = parse(std::string{svg_open} +
        R"svg(<rect id="r" x="5" y="5" width="10" height="10" style="stroke:black;stroke-width:2"/>)svg"
        R"svg(<text id="t" x="20" y="180" font-family="Arial" font-size="42" fill="#111" stroke="#e22" stroke-width="3">Width test</text>)svg"
        R"svg(<image id="bmp" x="200" y="0" width="10" height="10"/></svg>)svg");
    ASSERT_TRUE(document);
    settle(*document);
    auto const before = serialize(*document);
    auto *text = item(*document, "t");
    auto *rect = item(*document, "r");
    auto *bitmap = item(*document, "bmp");
    ASSERT_TRUE(text);
    ASSERT_TRUE(rect);
    ASSERT_TRUE(bitmap);
    auto const bitmap_before = sp_repr_write_buf(bitmap->getRepr(), 0, false, GQuark(0), 0, 0).raw();
    auto const plan = SW::prepare_stroke_widths(*document, {rect, text, bitmap}, absolute(10.0), 607);
    ASSERT_EQ(plan.state, SW::StrokeWidthPlanState::Prepared);
    ASSERT_EQ(plan.query.eligible, 2u);
    ASSERT_TRUE(find_excluded(plan.query, SW::StrokeWidthExclusion::Bitmap));

    auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
    ASSERT_TRUE(token);
    auto const result = SW::apply_stroke_widths_compatible(*document, plan, 607, *token);
    ASSERT_EQ(result.state, SW::StrokeWidthApplyState::Applied)
        << "attempted=" << result.attempted_writes << " output=" << serialize(*document);
    EXPECT_EQ(result.changed, 2u);
    EXPECT_EQ(result.excluded, 1u);
    EXPECT_DOUBLE_EQ(computed_width(*document, "r"), 10.0);
    EXPECT_DOUBLE_EQ(native_char_widths(*document, text, 0).local, 10.0);
    EXPECT_STREQ(text->getRepr()->attribute("font-size"), "42");
    EXPECT_STREQ(text->getRepr()->attribute("fill"), "#111");
    EXPECT_STREQ(text->getRepr()->attribute("stroke"), "#e22");
    EXPECT_EQ(sp_repr_write_buf(bitmap->getRepr(), 0, false, GQuark(0), 0, 0).raw(), bitmap_before);
    EXPECT_TRUE(SW::stroke_widths_compatible_output_ready(*document, plan, 607, *token));
    auto const applied = serialize(*document);
    EXPECT_TRUE(token->commitAtomically(
        Inkscape::Util::Internal::ContextString("SW3-C plain text width"), "",
        [&] { return SW::stroke_widths_compatible_output_ready(*document, plan, 607, *token); }));
    ASSERT_TRUE(Inkscape::DocumentUndo::undo(document.get()));
    EXPECT_EQ(serialize(*document), before);
    ASSERT_TRUE(Inkscape::DocumentUndo::redo(document.get()));
    EXPECT_EQ(serialize(*document), applied);
    token.reset();
}

} // namespace


// Owner report 2026-09-28 (failedgroupstroke.svg): in a millimetre document
// every absolute width failed with "the change failed and was rolled back".
// 2 px is 0.52916666666666656 user units there; the native CSS reader reads
// that text back as 0.52916666666666701 and the exact postcondition failed.
TEST_F(StrokeWidthControllerTest, MillimetreDocumentAbsoluteWidthsApply)
{
    auto document = parse(R"(<svg xmlns="http://www.w3.org/2000/svg" width="210mm" height="297mm" viewBox="0 0 210 297">
  <path id="p" d="M 10,10 H 100" style="fill:none;stroke:#000;stroke-width:0.19999"/>
  <path id="dashed" d="M 10,20 H 100" style="fill:none;stroke:#000;stroke-width:0.352778;stroke-dasharray:1.3,0.7;stroke-dashoffset:0.3"/>
  <g id="grp"><text id="t" x="10" y="40" style="font-size:4.23333px;fill:#808000;stroke:#808000;stroke-width:0.352778"><tspan id="ts" style="stroke-width:0.185208">Batista</tspan></text></g>
  <text id="plain" x="10" y="60" style="font-size:4.23333px;stroke:#000;stroke-width:0.352778">Plain</text>
  <text id="dashtext" x="10" y="80" style="font-size:4.23333px;stroke:#000;stroke-width:0.3;stroke-dasharray:6,2;stroke-dashoffset:1">Dash</text>
</svg>)");
    ASSERT_TRUE(document);
    auto const before = serialize(*document);
    for (double px : {2.0, 0.5, 1.0 / 3.0, 7.25}) {
        for (auto const *id : {"p", "dashed", "grp", "plain", "dashtext"}) {
            SCOPED_TRACE(std::string(id) + " " + std::to_string(px));
            std::vector<SPItem *> roots{item(*document, id)};
            auto const plan = SW::prepare_stroke_widths(*document, roots, absolute(px, true), 21);
            ASSERT_EQ(plan.state, SW::StrokeWidthPlanState::Prepared);
            auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
            ASSERT_TRUE(token);
            auto const result = SW::apply_stroke_widths_compatible(*document, plan, 21, *token);
            EXPECT_EQ(result.state, SW::StrokeWidthApplyState::Applied) << "reason " << int(result.reason);
            EXPECT_TRUE(token->commitAtomically(Inkscape::Util::Internal::ContextString("Stroke width"), "", [&] {
                return SW::stroke_widths_compatible_output_ready(*document, plan, 21, *token);
            }));
            ASSERT_TRUE(Inkscape::DocumentUndo::undo(document.get()));
            EXPECT_EQ(serialize(*document), before);
        }
    }
    // 100% is a no-op for every member: no write, no Undo step.
    {
        std::vector<SPItem *> all;
        for (auto const *id : {"p", "dashed", "grp", "plain", "dashtext"}) all.push_back(item(*document, id));
        auto const plan = SW::prepare_stroke_widths(*document, all, relative(100.0, true), 23);
        EXPECT_EQ(plan.planned_changes, 0u);
        auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
        ASSERT_TRUE(token);
        EXPECT_EQ(SW::apply_stroke_widths_compatible(*document, plan, 23, *token).state,
                  SW::StrokeWidthApplyState::Unchanged);
        token->rollback();
        EXPECT_EQ(serialize(*document), before);
    }
    // The applied width is the requested one, to the reader's last digit.
    std::vector<SPItem *> roots{item(*document, "p")};
    auto const plan = SW::prepare_stroke_widths(*document, roots, absolute(2.0), 22);
    auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
    ASSERT_TRUE(token);
    ASSERT_EQ(SW::apply_stroke_widths_compatible(*document, plan, 22, *token).state, SW::StrokeWidthApplyState::Applied);
    EXPECT_NEAR(computed_width(*document, "p"), 2.0 / (96.0 / 25.4), 1e-12);
    token->rollback();
}

// Review of 44ed019a7: dashed text in a pixel document with a ratio that gives
// long dash values (1/3) must still apply through every text write path.
TEST_F(StrokeWidthControllerTest, PixelDocumentDashedTextScalesAndApplies)
{
    auto document = parse(R"(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200">
  <text id="dashtext" x="10" y="40" style="font-size:20px;stroke:#000;stroke-width:3;stroke-dasharray:6,2;stroke-dashoffset:1">Dash</text>
  <text id="spans" x="10" y="80" style="font-size:20px;stroke:#000;stroke-width:3;stroke-dasharray:6,2"><tspan style="stroke-width:2">A</tspan>B</text>
</svg>)");
    ASSERT_TRUE(document);
    auto const before = serialize(*document);
    for (double px : {1.0, 1.0 / 3.0, 7.0 / 9.0}) {
        for (auto const *id : {"dashtext", "spans"}) {
            SCOPED_TRACE(std::string(id) + " " + std::to_string(px));
            std::vector<SPItem *> roots{item(*document, id)};
            auto const plan = SW::prepare_stroke_widths(*document, roots, absolute(px, true), 31);
            ASSERT_EQ(plan.state, SW::StrokeWidthPlanState::Prepared);
            auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
            ASSERT_TRUE(token);
            auto const result = SW::apply_stroke_widths_compatible(*document, plan, 31, *token);
            EXPECT_EQ(result.state, SW::StrokeWidthApplyState::Applied) << "reason " << int(result.reason);
            EXPECT_TRUE(token->commitAtomically(Inkscape::Util::Internal::ContextString("Stroke width"), "", [&] {
                return SW::stroke_widths_compatible_output_ready(*document, plan, 31, *token);
            }));
            ASSERT_TRUE(Inkscape::DocumentUndo::undo(document.get()));
            EXPECT_EQ(serialize(*document), before);
        }
    }
}


// Owner request 2026-09-28: a bullet-proof stroke width. Sweep absolute widths
// in 0.05 mm steps over text (plain, with its own tspan widths, dashed, in a
// group, in a CorelDRAW-style scaled group, rotated) and paths, in documents
// whose user unit is mm, px, pt, in, cm or 0.1 mm. Every step must apply,
// confirm, show the requested width, be a no-op when repeated, and undo to
// the exact original file.
namespace {
struct SweepDocument {
    char const *name;
    char const *root; // <svg ...> attributes giving the user unit
    double user_px;   // CSS px per user unit
};

std::string sweep_svg(SweepDocument const &doc)
{
    // Geometry in user units; a width of 1 user unit is the starting stroke.
    return std::string("<svg xmlns=\"http://www.w3.org/2000/svg\" ") + doc.root + R"svg(>
  <path id="p" d="M 1,1 H 20" style="fill:none;stroke:#000;stroke-width:0.2"/>
  <path id="dp" d="M 1,3 H 20" style="fill:none;stroke:#000;stroke-width:0.3;stroke-dasharray:1.3,0.7;stroke-dashoffset:0.3"/>
  <text id="t" x="1" y="6" style="font-size:2px;stroke:#808000;stroke-width:0.352778">Plain</text>
  <text id="ts" x="1" y="9" style="font-size:2px;fill:#808000;stroke:#808000;stroke-width:0.352778"><tspan id="ts1" style="stroke-width:0.185208">Recuerdo</tspan><tspan id="ts2" dy="1" style="stroke:#808000;stroke-width:0.319583">de</tspan> </text>
  <text id="dt" x="1" y="12" style="font-size:2px;stroke:#000;stroke-width:0.3;stroke-dasharray:0.6,0.2;stroke-dashoffset:0.1">Dash</text>
  <g id="g"><text id="g1" x="1" y="15" style="font-size:2px;stroke:#808000;stroke-width:0.352778">A</text><text id="g2" x="5" y="15" style="font-size:2px;stroke:#808000;stroke-width:0.185208"><tspan style="stroke-width:0.185208">B</tspan></text><path id="g3" d="M 1,16 H 9" style="fill:none;stroke:#000;stroke-width:0.19999"/></g>
  <g id="cdr" transform="matrix(0.3527777777,0,0,0.3527777777,-3.3,1.2)"><text id="c1" x="10" y="60" style="font-size:6px;stroke:#000;stroke-width:1">Scaled</text></g>
  <text id="rot" x="1" y="22" transform="rotate(-19.8753,5,22)" style="font-size:2px;stroke:#000;stroke-width:0.25">Rotated</text>
</svg>)svg";
}

// ---------------------------------------------------------------------------
// Owner report 2026-09-28 (build 21): a CorelDRAW-imported text whose lines
// are tspans positioned with x and dy="1.2em" (presentation attributes, no
// sodipodi:role) lost its line positions when a stroke width was applied:
// the lines flowed one after another. The tspans' x/dy and the text's
// geometry must survive a whole-object width write.
// ---------------------------------------------------------------------------
// Review finding on BUG-009: two style runs in one line, coloured through
// presentation attributes, end the width write with identical style="..."
// text; merging them would give the second run the first run's colour.
TEST_F(StrokeWidthControllerTest, ColouredRunsInOneLineKeepTheirColoursAfterAWidthWrite)
{
    auto document = parse(std::string{svg_open} +
        R"svg(<text x="10" y="40" xml:space="preserve" id="text1">)svg"
        R"svg(<tspan fill="#ff0000" font-size="20" id="red">Rojo </tspan>)svg"
        R"svg(<tspan fill="#0000ff" font-size="20" id="blue">Azul</tspan>)svg"
        R"svg(</text>)svg"
        R"(</svg>)");
    ASSERT_TRUE(document);
    settle(*document);
    auto const plan = SW::prepare_stroke_widths(*document, {item(*document, "text1")}, absolute(2.0), 911);
    ASSERT_EQ(plan.state, SW::StrokeWidthPlanState::Prepared);
    auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
    ASSERT_TRUE(token);
    ASSERT_EQ(SW::apply_stroke_widths_compatible(*document, plan, 911, *token).state, SW::StrokeWidthApplyState::Applied);
    ASSERT_TRUE(token->commitAtomically(Inkscape::Util::Internal::ContextString("Stroke width"), "", [&] {
        return SW::stroke_widths_compatible_output_ready(*document, plan, 911, *token);
    }));
    auto *red = document->getObjectById("red");
    auto *blue = document->getObjectById("blue");
    ASSERT_TRUE(red && blue) << "both runs still exist: " << serialize(*document);
    EXPECT_EQ(red->style->fill.get_value(), "#ff0000");
    EXPECT_EQ(blue->style->fill.get_value(), "#0000ff");
    auto *text = item(*document, "text1");
    EXPECT_DOUBLE_EQ(native_char_stroke(text, 0).width, 2.0);
    EXPECT_DOUBLE_EQ(native_char_stroke(text, native_char_count(text) - 1).width, 2.0);
}

TEST_F(StrokeWidthControllerTest, CorelLinesKeepTheirPositionsAfterAWidthWrite)
{
    auto document = parse(std::string{svg_open} +
        R"svg(<text x="875.2619" y="873.7452" xml:space="preserve" id="text29">)svg"
        R"svg(<tspan x="1027.6414" text-anchor="middle" font-family="sans-serif" font-size="30.5" fill="#1b1918" id="l1">María Victoria </tspan>)svg"
        R"svg(<tspan x="1027.6414" dy="1.2em" text-anchor="middle" font-family="sans-serif" font-size="30.5" fill="#1b1918" id="l2">Castillo Barría</tspan>)svg"
        R"svg(</text>)svg"
        R"svg(<text x="916.3166" y="923.1361" xml:space="preserve" id="text20">)svg"
        R"svg(<tspan x="1033.69" text-anchor="middle" font-family="sans-serif" font-size="10" fill="#1b1918" id="p1">Parroquia Nuestra Señora </tspan>)svg"
        R"svg(<tspan x="1033.69" dy="1.2em" text-anchor="middle" font-family="sans-serif" font-size="10" fill="#1b1918" id="p2">De La Asunción</tspan>)svg"
        R"svg(<tspan x="1033.69" dy="1.2em" text-anchor="middle" font-family="sans-serif" font-size="10" fill="#1b1918" id="p3">4 de octubre de 2026</tspan>)svg"
        R"svg(<tspan x="1033.69" dy="1.2em" text-anchor="middle" font-family="sans-serif" font-size="10" fill="#1b1918" id="p4" />)svg"
        R"svg(<tspan x="1033.69" dy="1.2em" text-anchor="middle" font-family="sans-serif" font-size="10" fill="#1b1918" id="p5" />)svg"
        R"svg(</text>)svg"
        R"(</svg>)");
    ASSERT_TRUE(document);
    settle(*document);
    std::vector<SPItem *> roots = {item(*document, "text29"), item(*document, "text20")};
    std::map<std::string, Geom::OptRect> before_bounds;
    std::map<std::string, std::pair<std::string, std::string>> before_pos;
    for (auto const *id : {"text29", "text20"}) before_bounds[id] = item(*document, id)->documentVisualBounds();
    for (auto const *id : {"l1", "l2", "p1", "p2", "p3", "p4", "p5"}) {
        auto *repr = document->getObjectById(id)->getRepr();
        before_pos[id] = {repr->attribute("x") ?: "", repr->attribute("dy") ?: ""};
    }

    auto const plan = SW::prepare_stroke_widths(*document, roots, absolute(2.0), 910);
    ASSERT_EQ(plan.state, SW::StrokeWidthPlanState::Prepared);
    auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
    ASSERT_TRUE(token);
    auto const result = SW::apply_stroke_widths_compatible(*document, plan, 910, *token);
    ASSERT_EQ(result.state, SW::StrokeWidthApplyState::Applied) << static_cast<int>(result.reason);
    ASSERT_TRUE(SW::stroke_widths_compatible_output_ready(*document, plan, 910, *token));
    ASSERT_TRUE(token->commitAtomically(Inkscape::Util::Internal::ContextString("Stroke width"), "", [&] {
        return SW::stroke_widths_compatible_output_ready(*document, plan, 910, *token);
    }));
    settle(*document);

    for (auto const *id : {"l1", "l2", "p1", "p2", "p3", "p4", "p5"}) {
        auto *object = document->getObjectById(id);
        ASSERT_TRUE(object) << id << " was removed";
        auto *repr = object->getRepr();
        EXPECT_EQ(std::string(repr->attribute("x") ?: ""), before_pos[id].first) << id << " lost its x";
        EXPECT_EQ(std::string(repr->attribute("dy") ?: ""), before_pos[id].second) << id << " lost its dy";
    }
    for (auto const *id : {"text29", "text20"}) {
        auto const after = item(*document, id)->documentVisualBounds();
        ASSERT_TRUE(after && before_bounds[id]);
        // A 2 px stroke widens the box by at most 2 px; flowing lines widen it by a line.
        EXPECT_NEAR(after->width(), before_bounds[id]->width(), 2.5) << id << " lines moved sideways";
        EXPECT_NEAR(after->height(), before_bounds[id]->height(), 2.5) << id << " lines moved vertically";
        EXPECT_NEAR(after->left(), before_bounds[id]->left(), 2.5) << id;
        EXPECT_NEAR(after->top(), before_bounds[id]->top(), 2.5) << id;
    }
    // The width is written per line (tspan), as the run writer does.
    auto *text29 = item(*document, "text29");
    EXPECT_DOUBLE_EQ(native_char_stroke(text29, 0).width, 2.0);
    EXPECT_DOUBLE_EQ(native_char_stroke(text29, native_char_count(text29) - 1).width, 2.0);
}

TEST_F(StrokeWidthControllerTest, SpanWidthPreservesTrailingZeroOwnerX)
{
    auto document = parse(std::string{svg_open} +
        R"(<text id="t" x="10.5000" y="40" style="font-size:20px;stroke:black;stroke-width:1"><tspan id="s">A</tspan></text></svg>)");
    ASSERT_TRUE(document);
    settle(*document);
    auto *owner = item(*document, "t");
    auto const before = serialize(*document);
    auto const attrs = authored_attributes(owner);
    auto const plan = SW::prepare_stroke_widths(*document, {owner}, absolute(2.5), 915);
    ASSERT_EQ(plan.state, SW::StrokeWidthPlanState::Prepared);
    auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
    ASSERT_TRUE(token);
    auto const result = SW::apply_stroke_widths_compatible(*document, plan, 915, *token);
    ASSERT_EQ(result.state, SW::StrokeWidthApplyState::Applied) << static_cast<int>(result.reason);
    ASSERT_TRUE(token->commitAtomically(Inkscape::Util::Internal::ContextString("Stroke width"), "", [&] {
        return SW::stroke_widths_compatible_output_ready(*document, plan, 915, *token);
    }));
    EXPECT_DOUBLE_EQ(native_char_stroke(owner, 0).width, 2.5);
    EXPECT_EQ(authored_attributes(owner), attrs);
    ASSERT_TRUE(Inkscape::DocumentUndo::undo(document.get()));
    EXPECT_EQ(serialize(*document), before);
}

TEST_F(StrokeWidthControllerTest, SpanWidthPreservesReserializedOwnerPositionsAndTransforms)
{
    struct Variant { char const *name; char const *attributes; };
    Variant const variants[] = {
        {"x5000", "x=\"10.5000\" y=\"40\""},
        {"y2500", "x=\"10\" y=\"40.2500\""},
        {"x0", "x=\"10.0\" y=\"40\""},
        {"matrix_spaces", "x=\"10\" y=\"40\" transform=\"matrix(0.7071 0.7071 -0.7071 0.7071 10 20)\""},
        {"translate_spaces", "x=\"10\" y=\"40\" transform=\"translate(10 20)\""},
        {"matrix_precision", "x=\"10\" y=\"40\" transform=\"matrix(0.866025403784,0.5,-0.5,0.866025403784,0,0)\""},
    };
    for (auto const &variant : variants) {
        SCOPED_TRACE(variant.name);
        auto document = parse(std::string{svg_open} + "<text id=\"t\" " + variant.attributes +
            R"( style="font-size:20px;stroke:black;stroke-width:1"><tspan id="s">A</tspan></text></svg>)");
        ASSERT_TRUE(document);
        settle(*document);
        auto *owner = item(*document, "t");
        auto const before = serialize(*document);
        auto const attrs = authored_attributes(owner);
        auto const plan = SW::prepare_stroke_widths(*document, {owner}, absolute(2.5), 916);
        ASSERT_EQ(plan.state, SW::StrokeWidthPlanState::Prepared);
        auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
        ASSERT_TRUE(token);
        auto const result = SW::apply_stroke_widths_compatible(*document, plan, 916, *token);
        ASSERT_EQ(result.state, SW::StrokeWidthApplyState::Applied) << static_cast<int>(result.reason);
        ASSERT_TRUE(token->commitAtomically(Inkscape::Util::Internal::ContextString("Stroke width"), "", [&] {
            return SW::stroke_widths_compatible_output_ready(*document, plan, 916, *token);
        }));
        auto const width = native_char_widths(*document, owner, 0).effective;
        ASSERT_TRUE(width);
        EXPECT_NEAR(*width, 2.5, 1e-5);
        EXPECT_EQ(authored_attributes(owner), attrs);
        ASSERT_TRUE(Inkscape::DocumentUndo::undo(document.get()));
        EXPECT_EQ(serialize(*document), before);
    }
}

TEST_F(StrokeWidthControllerTest, TwoSpanWidthPreservesOwnerYAfterEachWrite)
{
    auto document = parse(std::string{svg_open} +
        R"(<text id="t" x="10" y="40.2500" style="font-size:20px;stroke:black"><tspan id="a" style="stroke-width:1">A</tspan><tspan id="b" style="stroke-width:3">B</tspan></text></svg>)");
    ASSERT_TRUE(document);
    settle(*document);
    auto *owner = item(*document, "t");
    auto const before = serialize(*document);
    auto const attrs = authored_attributes(owner);
    auto const plan = SW::prepare_stroke_widths(*document, {owner}, absolute(2.5), 917);
    ASSERT_EQ(plan.state, SW::StrokeWidthPlanState::Prepared);
    auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
    ASSERT_TRUE(token);
    auto const result = SW::apply_stroke_widths_compatible(*document, plan, 917, *token);
    ASSERT_EQ(result.state, SW::StrokeWidthApplyState::Applied) << static_cast<int>(result.reason);
    ASSERT_TRUE(token->commitAtomically(Inkscape::Util::Internal::ContextString("Stroke width"), "", [&] {
        return SW::stroke_widths_compatible_output_ready(*document, plan, 917, *token);
    }));
    EXPECT_DOUBLE_EQ(native_char_stroke(owner, 0).width, 2.5);
    EXPECT_DOUBLE_EQ(native_char_stroke(owner, 1).width, 2.5);
    EXPECT_EQ(authored_attributes(owner), attrs);
    ASSERT_TRUE(Inkscape::DocumentUndo::undo(document.get()));
    EXPECT_EQ(serialize(*document), before);
}

TEST_F(StrokeWidthControllerTest, RectAndNoncanonicalTextOwnersApplyTogether)
{
    char const *positions[] = {
        "x=\"10.5000\" y=\"40\"",
        "x=\"10\" y=\"40.2500\"",
        "x=\"10.0\" y=\"40\"",
        "x=\"10\" y=\"40\" transform=\"matrix(0.7071 0.7071 -0.7071 0.7071 10 20)\"",
        "x=\"10\" y=\"40\" transform=\"translate(10 20)\"",
        "x=\"10\" y=\"40\" transform=\"matrix(0.866025403784,0.5,-0.5,0.866025403784,0,0)\"",
    };
    std::string svg = std::string{svg_open} +
        R"(<rect id="r" width="10" height="10" style="stroke:black;stroke-width:1"/>)";
    for (unsigned i = 0; i < std::size(positions); ++i) {
        svg += "<text id=\"t" + std::to_string(i) + "\" " + positions[i] +
            R"( style="font-size:20px;stroke:black;stroke-width:1"><tspan>A</tspan></text>)";
    }
    auto document = parse(svg + "</svg>");
    ASSERT_TRUE(document);
    settle(*document);
    auto *rect = item(*document, "r");
    auto const before = serialize(*document);
    std::vector<SPItem *> roots{rect};
    std::vector<std::vector<std::pair<std::string, std::string>>> attrs;
    for (unsigned i = 0; i < std::size(positions); ++i) {
        auto *owner = item(*document, ("t" + std::to_string(i)).c_str());
        ASSERT_TRUE(owner);
        roots.push_back(owner);
        attrs.push_back(authored_attributes(owner));
    }
    auto const plan = SW::prepare_stroke_widths(*document, roots, absolute(2.5), 918);
    ASSERT_EQ(plan.state, SW::StrokeWidthPlanState::Prepared);
    auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
    ASSERT_TRUE(token);
    auto const result = SW::apply_stroke_widths_compatible(*document, plan, 918, *token);
    ASSERT_EQ(result.state, SW::StrokeWidthApplyState::Applied) << static_cast<int>(result.reason);
    EXPECT_EQ(result.changed, roots.size());
    ASSERT_TRUE(token->commitAtomically(Inkscape::Util::Internal::ContextString("Stroke width"), "", [&] {
        return SW::stroke_widths_compatible_output_ready(*document, plan, 918, *token);
    }));
    EXPECT_NEAR(computed_width(*document, "r"), 2.5, 1e-5);
    for (unsigned i = 0; i < attrs.size(); ++i) {
        auto *owner = roots[i + 1];
        auto const width = native_char_widths(*document, owner, 0).effective;
        ASSERT_TRUE(width);
        EXPECT_NEAR(*width, 2.5, 1e-5) << i;
        EXPECT_EQ(authored_attributes(owner), attrs[i]) << i;
    }
    ASSERT_TRUE(Inkscape::DocumentUndo::undo(document.get()));
    EXPECT_EQ(serialize(*document), before);
}

TEST_F(StrokeWidthControllerTest, RealOwnerPositionChangeStillRejectsSpanPlan)
{
    auto document = parse(std::string{svg_open} +
        R"(<text id="t" x="10.5000" y="40" style="font-size:20px;stroke:black;stroke-width:1"><tspan>A</tspan></text></svg>)");
    ASSERT_TRUE(document);
    settle(*document);
    auto *owner = item(*document, "t");
    auto const plan = SW::prepare_stroke_widths(*document, {owner}, absolute(2.5), 919);
    ASSERT_EQ(plan.state, SW::StrokeWidthPlanState::Prepared);
    auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
    ASSERT_TRUE(token);
    owner->setAttribute("x", "11.5000");
    auto const changed = serialize(*document);
    auto const result = SW::apply_stroke_widths_compatible(*document, plan, 919, *token);
    EXPECT_EQ(result.state, SW::StrokeWidthApplyState::Rejected);
    EXPECT_EQ(result.reason, SW::StrokeWidthMemberReason::StalePlan);
    EXPECT_EQ(result.attempted_writes, 0u);
    EXPECT_EQ(serialize(*document), changed);
    token->rollback();
}

// Identical spans must still merge (the tidy pass keeps the XML small):
// computed styles are not compared, so an em/%-unit length such as
// line-height:125% does not stop the merge.
TEST_F(StrokeWidthControllerTest, IdenticalSpansStaySeparateUnderPercentLineHeight)
{
    auto document = parse(std::string{svg_open} +
        R"svg(<text x="10" y="40" xml:space="preserve" style="line-height:125%;font-size:20px" id="text1">)svg"
        R"svg(<tspan style="font-weight:bold" id="a">ab</tspan><tspan style="font-weight:bold" id="b">cd</tspan>)svg"
        R"svg(</text>)svg"
        R"(</svg>)");
    ASSERT_TRUE(document);
    settle(*document);
    auto const plan = SW::prepare_stroke_widths(*document, {item(*document, "text1")}, absolute(2.0), 912);
    ASSERT_EQ(plan.state, SW::StrokeWidthPlanState::Prepared);
    auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
    ASSERT_TRUE(token);
    ASSERT_EQ(SW::apply_stroke_widths_compatible(*document, plan, 912, *token).state, SW::StrokeWidthApplyState::Applied);
    ASSERT_TRUE(token->commitAtomically(Inkscape::Util::Internal::ContextString("Stroke width"), "", [&] {
        return SW::stroke_widths_compatible_output_ready(*document, plan, 912, *token);
    }));
    auto *text = item(*document, "text1");
    std::size_t spans = 0;
    for (auto &child : text->children) {
        if (is<SPTSpan>(&child)) ++spans;
    }
    EXPECT_EQ(spans, 2u) << "whole-owner edits preserve source identity: " << serialize(*document);
}


// Corel runs with identical presentation attributes (as the text tool leaves
// them after splitting one) must still merge, or the XML grows with every edit.
TEST_F(StrokeWidthControllerTest, CorelRunsWithEqualPresentationAttributesKeepSources)
{
    auto document = parse(std::string{svg_open} +
        R"svg(<text x="10" y="40" xml:space="preserve" id="text1">)svg"
        R"svg(<tspan fill="#ff0000" font-size="20" id="a">ab</tspan><tspan fill="#ff0000" font-size="20" id="b">cd</tspan>)svg"
        R"svg(</text>)svg"
        R"(</svg>)");
    ASSERT_TRUE(document);
    settle(*document);
    auto const plan = SW::prepare_stroke_widths(*document, {item(*document, "text1")}, absolute(2.0), 913);
    ASSERT_EQ(plan.state, SW::StrokeWidthPlanState::Prepared);
    auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
    ASSERT_TRUE(token);
    ASSERT_EQ(SW::apply_stroke_widths_compatible(*document, plan, 913, *token).state, SW::StrokeWidthApplyState::Applied);
    ASSERT_TRUE(token->commitAtomically(Inkscape::Util::Internal::ContextString("Stroke width"), "", [&] {
        return SW::stroke_widths_compatible_output_ready(*document, plan, 913, *token);
    }));
    auto *text = item(*document, "text1");
    std::size_t spans = 0;
    for (auto &child : text->children) {
        if (is<SPTSpan>(&child)) ++spans;
    }
    EXPECT_EQ(spans, 2u) << serialize(*document);
    EXPECT_EQ(sp_te_get_string_multiline(text).raw(), "abcd");
}

// A short rotate list on the first span would rotate the merged glyphs.
TEST_F(StrokeWidthControllerTest, RotateOnTheFirstSpanKeepsBothSpans)
{
    auto document = parse(std::string{svg_open} +
        R"svg(<text x="10" y="40" xml:space="preserve" style="font-size:20px" id="text1">)svg"
        R"svg(<tspan rotate="30" id="a">ab</tspan><tspan id="b">cd</tspan>)svg"
        R"svg(</text>)svg"
        R"(</svg>)");
    ASSERT_TRUE(document);
    settle(*document);
    auto const plan = SW::prepare_stroke_widths(*document, {item(*document, "text1")}, absolute(2.0), 914);
    ASSERT_EQ(plan.state, SW::StrokeWidthPlanState::Prepared);
    auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
    ASSERT_TRUE(token);
    ASSERT_EQ(SW::apply_stroke_widths_compatible(*document, plan, 914, *token).state, SW::StrokeWidthApplyState::Applied);
    ASSERT_TRUE(token->commitAtomically(Inkscape::Util::Internal::ContextString("Stroke width"), "", [&] {
        return SW::stroke_widths_compatible_output_ready(*document, plan, 914, *token);
    }));
    EXPECT_TRUE(document->getObjectById("a") && document->getObjectById("b")) << serialize(*document);
    EXPECT_STREQ(document->getObjectById("a")->getRepr()->attribute("rotate"), "30");
}

} // namespace

TEST_F(StrokeWidthControllerTest, SweepTenthMillimetreStepsAcrossUnitsAndTargets)
{
    SweepDocument const documents[] = {
        {"mm", R"(width="210mm" height="297mm" viewBox="0 0 210 297")", 96.0 / 25.4},
        {"px", R"(width="800" height="600")", 1.0},
        {"pt", R"(width="595pt" height="842pt" viewBox="0 0 595 842")", 96.0 / 72.0},
        {"in", R"(width="8.5in" height="11in" viewBox="0 0 8.5 11")", 96.0},
        {"cm", R"(width="21cm" height="29.7cm" viewBox="0 0 21 29.7")", 96.0 / 2.54},
        {"0.1mm", R"(width="100mm" height="100mm" viewBox="0 0 1000 1000")", 96.0 / 25.4 / 10.0},
    };
    std::vector<std::vector<char const *>> const selections = {
        {"p"}, {"dp"}, {"t"}, {"ts"}, {"dt"}, {"g"}, {"cdr"}, {"rot"},
        {"p", "dp", "t", "ts", "dt", "g", "cdr", "rot"},
    };
    std::size_t applied = 0;
    std::vector<std::string> failures;
    for (auto const &doc : documents) {
        auto document = parse(sweep_svg(doc));
        ASSERT_TRUE(document) << doc.name;
        auto const before = serialize(*document);
        for (auto const &ids : selections) {
            std::vector<SPItem *> roots;
            for (auto const *id : ids) roots.push_back(item(*document, id));
            std::string const label = std::string(doc.name) + " " + ids.front() + (ids.size() > 1 ? "+all" : "");
            for (int step = 1; step <= 60; ++step) { // 0.05 mm .. 3.00 mm
                double const mm = 0.05 * step;
                double const px = mm * 96.0 / 25.4;
                bool const scale_dashes = step % 2 == 0;
                auto const where = label + " " + std::to_string(mm) + "mm";
                auto const plan = SW::prepare_stroke_widths(*document, roots, absolute(px, scale_dashes), 41);
                if (plan.state != SW::StrokeWidthPlanState::Prepared || plan.planned_changes == 0 && plan.query.eligible == 0) {
                    failures.push_back(where + ": not prepared");
                    continue;
                }
                auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
                ASSERT_TRUE(token);
                auto const result = SW::apply_stroke_widths_compatible(*document, plan, 41, *token);
                if (result.state == SW::StrokeWidthApplyState::Unchanged) {
                    token->rollback(); // already this width (the authored one)
                    auto const query = SW::query_stroke_widths(*document, roots);
                    if (!query.uniform_px || std::fabs(*query.uniform_px - px) > 1e-3) {
                        failures.push_back(where + ": unchanged but not at this width");
                    }
                    continue;
                }
                if (result.state != SW::StrokeWidthApplyState::Applied) {
                    failures.push_back(where + ": apply state " + std::to_string(int(result.state)) + " reason " +
                                       std::to_string(int(result.reason)));
                    token->rollback();
                    continue;
                }
                bool const committed = token->commitAtomically(
                    Inkscape::Util::Internal::ContextString("Stroke width"), "",
                    [&] { return SW::stroke_widths_compatible_output_ready(*document, plan, 41, *token); });
                if (!committed) {
                    failures.push_back(where + ": not confirmed");
                    token->rollback();
                    continue;
                }
                ++applied;
                // The selection now shows the requested width (the toolbar rounds to 0.001 mm).
                auto const query = SW::query_stroke_widths(*document, roots);
                // Stored text keeps 6 decimals: within 0.001 px, and the same at 0.001 mm.
                if (!query.uniform_px || std::fabs(*query.uniform_px - px) > 1e-3 ||
                    std::lround(*query.uniform_px * 25.4 / 96.0 * 1000) != std::lround(mm * 1000)) {
                    failures.push_back(where + ": shows " +
                                       (query.uniform_px ? std::to_string(*query.uniform_px * 25.4 / 96.0) : "mixed"));
                }
                // The same width again is a no-op.
                auto const again = SW::prepare_stroke_widths(*document, roots, absolute(px, scale_dashes), 42);
                if (again.planned_changes != 0) {
                    failures.push_back(where + ": repeating it would change " + std::to_string(again.planned_changes));
                }
                if (!Inkscape::DocumentUndo::undo(document.get())) {
                    failures.push_back(where + ": no Undo step");
                } else if (serialize(*document) != before) {
                    failures.push_back(where + ": Undo did not restore the file");
                    return;
                }
            }
        }
    }
    std::cout << "stroke sweep: " << applied << " steps applied, " << failures.size() << " failures\n";
    for (std::size_t i = 0; i < failures.size() && i < 40; ++i) std::cout << "  " << failures[i] << "\n";
    EXPECT_TRUE(failures.empty());
}

// The user nudging the width up step by step without undoing: every step
// applies on top of the previous one, and Undo walks back one step at a time.
TEST_F(StrokeWidthControllerTest, SweepCumulativeNudgesUndoOneStepAtATime)
{
    for (auto const *root : {R"(width="210mm" height="297mm" viewBox="0 0 210 297")", R"(width="800" height="600")"}) {
        SweepDocument const doc{"cumulative", root, 0};
        auto document = parse(sweep_svg(doc));
        ASSERT_TRUE(document);
        auto const before = serialize(*document);
        std::vector<SPItem *> roots{item(*document, "ts"), item(*document, "g"), item(*document, "dt"),
                                    item(*document, "cdr")};
        std::vector<std::string> states{before};
        for (int step = 1; step <= 40; ++step) {
            double const px = 0.05 * step * 96.0 / 25.4;
            auto const plan = SW::prepare_stroke_widths(*document, roots, absolute(px, true), 50 + step);
            auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
            ASSERT_TRUE(token);
            auto const result = SW::apply_stroke_widths_compatible(*document, plan, 50 + step, *token);
            ASSERT_EQ(result.state, SW::StrokeWidthApplyState::Applied) << root << " step " << step;
            ASSERT_TRUE(token->commitAtomically(Inkscape::Util::Internal::ContextString("Stroke width"), "", [&] {
                return SW::stroke_widths_compatible_output_ready(*document, plan, 50 + step, *token);
            })) << root << " step " << step;
            states.push_back(serialize(*document));
        }
        for (int step = 40; step >= 1; --step) {
            ASSERT_TRUE(Inkscape::DocumentUndo::undo(document.get())) << step;
            ASSERT_EQ(serialize(*document), states[step - 1]) << root << " undo to step " << step - 1;
        }
    }
}

// Owner request 2026-09-28: hostile cases. Groups mixing vectors and a
// bitmap, clipped and masked groups, clones, text on a path, flowed text,
// presentation attributes, stylesheet classes, !important, em and percent
// widths, non-scaling and hairline strokes, gradients, locked, hidden and
// empty objects, nested transforms. Any of them may be skipped with a
// reason, but no request may fail and roll back, a confirmed change must
// read back as requested (repeating it is a no-op), and Undo restores the
// exact file.
TEST_F(StrokeWidthControllerTest, SweepHostileDocumentNeverFails)
{
    auto document = parse(R"svg(<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink"
     xmlns:sodipodi="http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd" width="210mm" height="297mm" viewBox="0 0 210 297">
  <defs>
    <style>.cls{stroke:#00f;stroke-width:0.4}.imp{stroke:#0f0;stroke-width:0.6 !important}</style>
    <clipPath id="clip" clipPathUnits="userSpaceOnUse"><rect x="0" y="0" width="30" height="10"/></clipPath>
    <mask id="mask" maskUnits="userSpaceOnUse" x="0" y="0" width="40" height="40"><rect width="40" height="40" fill="white"/></mask>
    <path id="curve" d="M 5,100 C 20,90 40,110 60,100"/>
    <symbol id="sym"><path d="M0,0 H5" style="stroke:#000;stroke-width:0.3"/></symbol>
    <linearGradient id="lg"><stop offset="0" stop-color="#f00"/><stop offset="1" stop-color="#00f"/></linearGradient>
  </defs>
  <g id="mixed"><image id="img" x="0" y="20" width="10" height="10" preserveAspectRatio="none"
      xlink:href="data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVQIHWP4z8DwHwAFgAI/ScLx9QAAAABJRU5ErkJggg=="/>
    <path id="mp" d="M 12,20 H 30" style="fill:none;stroke:#000;stroke-width:0.3"/>
    <text id="mt" x="12" y="28" style="font-size:3px;stroke:#000;stroke-width:0.2">Mix</text></g>
  <g id="clipped" clip-path="url(#clip)"><path id="cp" d="M 1,5 H 29" style="stroke:#000;stroke-width:0.5"/>
    <text id="ct" x="2" y="8" style="font-size:3px;stroke:#000;stroke-width:0.2">Clip</text></g>
  <g id="masked" mask="url(#mask)"><rect id="mr" x="2" y="2" width="10" height="10" style="fill:#ccc;stroke:#000;stroke-width:0.3"/></g>
  <g id="cdrclip" clip-path="url(#clip)" transform="matrix(0.3527777777,0,0,0.3527777777,1,1)">
    <image x="0" y="0" width="30" height="30" xlink:href="data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVQIHWP4z8DwHwAFgAI/ScLx9QAAAABJRU5ErkJggg=="/></g>
  <text id="orig" x="5" y="50" style="font-size:3px;stroke:#000;stroke-width:0.3">Original</text>
  <use id="clone" xlink:href="#orig" x="0" y="10"/>
  <text id="onpath" style="font-size:3px;stroke:#000;stroke-width:0.2"><textPath xlink:href="#curve">On a path</textPath></text>
  <rect id="frame" x="80" y="10" width="40" height="20" style="fill:none"/>
  <text id="flowed" style="font-size:3px;shape-inside:url(#frame);stroke:#000;stroke-width:0.2">Flowed text inside a frame</text>
  <path id="attr" d="M 5,60 H 40" stroke="#000" stroke-width="0.4" fill="none"/>
  <path id="classed" class="cls" d="M 5,65 H 40" fill="none"/>
  <path id="important" class="imp" d="M 5,70 H 40" fill="none"/>
  <path id="inlineimp" d="M 5,75 H 40" style="fill:none;stroke:#000;stroke-width:0.3 !important"/>
  <text id="em" x="5" y="80" style="font-size:4px;stroke:#000;stroke-width:0.05em">Em</text>
  <path id="pct" d="M 5,85 H 40" style="fill:none;stroke:#000;stroke-width:1%"/>
  <path id="nonscaling" d="M 5,88 H 40" transform="scale(2)" style="fill:none;stroke:#000;stroke-width:0.3;vector-effect:non-scaling-stroke"/>
  <path id="hairline" d="M 5,92 H 40" style="fill:none;stroke:#000;stroke-width:1px;vector-effect:non-scaling-stroke;-inkscape-stroke:hairline"/>
  <path id="nostroke" d="M 5,95 H 40" style="fill:#000;stroke:none"/>
  <path id="gradstroke" d="M 5,97 H 40" style="fill:none;stroke:url(#lg);stroke-width:0.3"/>
  <path id="locked" sodipodi:insensitive="true" d="M 5,99 H 40" style="stroke:#000;stroke-width:0.3"/>
  <path id="hidden" d="M 5,101 H 40" style="display:none;stroke:#000;stroke-width:0.3"/>
  <g id="nested" transform="translate(5,110) scale(0.5)"><g transform="rotate(30) scale(3,1)">
    <path id="skewed" d="M 0,0 H 20" style="stroke:#000;stroke-width:0.3"/>
    <text id="nt" x="0" y="5" style="font-size:3px;stroke:#000;stroke-width:0.2">Nested</text></g></g>
  <use id="symuse" xlink:href="#sym" x="100" y="100"/>
  <text id="emptytext" x="5" y="125" style="stroke:#000;stroke-width:0.2"></text>
  <g id="emptygroup"/>
  <path id="zero" d="M 5,128 H 40" style="stroke:#000;stroke-width:0"/>
  <text id="multi" x="5" y="135" style="font-size:3px;stroke:#000;stroke-width:0.2"><tspan style="stroke-width:0.1;stroke-dasharray:0.5,0.25">a</tspan><tspan style="stroke:none">b</tspan><tspan style="stroke-width:0.4 !important">c</tspan>d</text>
</svg>)svg");
    ASSERT_TRUE(document);
    auto const before = serialize(*document);
    std::vector<std::vector<char const *>> selections;
    char const *const singles[] = {"mixed", "clipped", "masked", "cdrclip", "orig", "clone", "onpath", "flowed",
                                   "attr", "classed", "important", "inlineimp", "em", "pct", "nonscaling",
                                   "hairline", "nostroke", "gradstroke", "locked", "hidden", "nested",
                                   "symuse", "emptytext", "emptygroup", "zero", "multi"};
    for (auto const *id : singles) selections.push_back({id});
    selections.push_back({"orig", "clone"}); // a clone and its original together
    selections.push_back(std::vector<char const *>(std::begin(singles), std::end(singles)));

    std::vector<std::pair<char const *, SW::StrokeWidthIntent>> intents;
    for (int step = 1; step <= 20; ++step) intents.push_back({"abs", absolute(0.05 * step * 96.0 / 25.4, step % 2)});
    intents.push_back({"150%", relative(150.0, true)});
    intents.push_back({"50%", relative(50.0, false)});
    intents.push_back({"100%", relative(100.0, true)});

    std::size_t applied = 0, unchanged = 0, rejected = 0;
    std::vector<std::string> failures;
    for (auto const &ids : selections) {
        std::vector<SPItem *> roots;
        for (auto const *id : ids) roots.push_back(item(*document, id));
        std::string const label = ids.size() > 2 ? "everything" : std::string(ids.front()) + (ids.size() > 1 ? "+clone" : "");
        for (auto const &[name, intent] : intents) {
            auto const where = label + " " + name + " " + std::to_string(intent.value);
            auto const plan = SW::prepare_stroke_widths(*document, roots, intent, 61);
            if (plan.state != SW::StrokeWidthPlanState::Prepared) {
                ++rejected; // a whole-plan refusal is reported to the user, never a failure
                continue;
            }
            auto token = Inkscape::DocumentUndo::beginAtomicInteraction(document.get());
            ASSERT_TRUE(token);
            auto const result = SW::apply_stroke_widths_compatible(*document, plan, 61, *token);
            if (result.state == SW::StrokeWidthApplyState::Failed) {
                failures.push_back(where + ": FAILED reason " + std::to_string(int(result.reason)));
                token->rollback();
                if (serialize(*document) != before) {
                    failures.push_back(where + ": rollback did not restore the file");
                    break;
                }
                continue;
            }
            if (result.state != SW::StrokeWidthApplyState::Applied) {
                ++(result.state == SW::StrokeWidthApplyState::Unchanged ? unchanged : rejected);
                token->rollback();
                continue;
            }
            bool const committed = token->commitAtomically(
                Inkscape::Util::Internal::ContextString("Stroke width"), "",
                [&] { return SW::stroke_widths_compatible_output_ready(*document, plan, 61, *token); });
            if (!committed) {
                failures.push_back(where + ": not confirmed");
                token->rollback();
                continue;
            }
            ++applied;
            if (intent.kind == SW::StrokeWidthIntentKind::AbsoluteCssPx) {
                auto const again = SW::prepare_stroke_widths(*document, roots, intent, 62);
                if (again.state == SW::StrokeWidthPlanState::Prepared && again.planned_changes != 0) {
                    failures.push_back(where + ": repeating it would change " +
                                       std::to_string(again.planned_changes));
                }
            }
            if (!Inkscape::DocumentUndo::undo(document.get()) || serialize(*document) != before) {
                failures.push_back(where + ": Undo did not restore the file");
                break;
            }
        }
    }
    std::cout << "hostile sweep: " << applied << " applied, " << unchanged << " unchanged, " << rejected
              << " refused, " << failures.size() << " failures\n";
    for (std::size_t i = 0; i < failures.size() && i < 40; ++i) std::cout << "  " << failures[i] << "\n";
    EXPECT_TRUE(failures.empty());
}

TEST_F(StrokeWidthControllerTest, SweepHostileFixtureFilesNeverFail)
{
    auto const directory = std::filesystem::path(__FILE__).parent_path().parent_path() /
                           "data/stroke-width/hostile";
    std::vector<std::filesystem::path> files;
    for (auto const &entry : std::filesystem::directory_iterator(directory)) {
        if (entry.path().extension() == ".svg") files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    std::vector<std::string> all_failures;
    for (auto const &file : files) {
        gchar *bytes = nullptr;
        gsize size = 0;
        if (!g_file_get_contents(file.string().c_str(), &bytes, &size, nullptr)) {
            all_failures.push_back(file.filename().string() + ": unreadable");
            continue;
        }
        std::string svg(bytes, size);
        g_free(bytes);
        auto document = parse(svg);
        if (!document) {
            all_failures.push_back(file.filename().string() + ": parse failed");
            continue;
        }
        // Path effects rewrite their path in a normalized form on the first
        // update after a change; settle that before the baseline snapshot.
        std::function<void(SPObject &)> settle = [&](SPObject &object) {
            for (auto &child : object.children) {
                if (auto *lpe_item = cast<SPLPEItem>(&child); lpe_item && lpe_item->hasPathEffect()) {
                    sp_lpe_item_update_patheffect(lpe_item, true, true);
                }
                settle(child);
            }
        };
        settle(*document->getRoot());
        document->ensureUpToDate();
        Inkscape::DocumentUndo::done(document.get(), Inkscape::Util::Internal::ContextString("Fixture settle"), "");
        Inkscape::DocumentUndo::clearUndo(document.get());
        auto const before = serialize(*document);
        std::vector<std::vector<std::string>> selections;
        std::vector<std::string> top_level;
        std::function<void(SPObject *, bool)> collect = [&](SPObject *object, bool in_defs) {
            for (auto *child = object->firstChild(); child; child = child->getNext()) {
                auto const *repr = child->getRepr();
                auto const *name = repr ? repr->name() : "";
                bool const defs = in_defs || std::strcmp(name, "svg:defs") == 0 ||
                                  std::strcmp(name, "defs") == 0;
                if (!defs) {
                    auto const *id = repr ? repr->attribute("id") : nullptr;
                    if (id && cast<SPItem>(child)) {
                        selections.push_back({id});
                        if (object == reinterpret_cast<SPObject *>(document->getRoot())) top_level.emplace_back(id);
                    }
                    collect(child, false);
                }
            }
        };
        collect(reinterpret_cast<SPObject *>(document->getRoot()), false);
        if (!top_level.empty()) selections.push_back(top_level);
        std::vector<std::pair<char const *, SW::StrokeWidthIntent>> intents;
        for (int step = 1; step <= 20; ++step) intents.push_back({"abs", absolute(0.05 * step * 96.0 / 25.4, step % 2)});
        intents.push_back({"150%", relative(150.0, true)});
        intents.push_back({"50%", relative(50.0, false)});
        intents.push_back({"100%", relative(100.0, true)});
        std::size_t applied = 0, unchanged = 0, refused = 0;
        std::vector<std::string> failures;
        bool stop_file = false;
        for (auto const &ids : selections) {
            if (stop_file) break;
            std::vector<SPItem *> roots;
            for (auto const &id : ids) roots.push_back(item(*document, id.c_str()));
            auto const label = ids.size() == 1 ? ids.front() : "all-top-level";
            for (auto const &[name, intent] : intents) {
                auto const where = label + " " + name + " " + std::to_string(intent.value);
                auto const plan = SW::prepare_stroke_widths(*document, roots, intent, 61);
                if (plan.state != SW::StrokeWidthPlanState::Prepared) { ++refused; continue; }
                auto token = SW::begin_stroke_width_interaction(*document, plan);
                if (!token) { failures.push_back(where + ": no interaction token"); stop_file = true; break; }
                auto const result = SW::apply_stroke_widths_compatible(*document, plan, 61, *token);
                if (result.state == SW::StrokeWidthApplyState::Failed) {
                    failures.push_back(where + ": FAILED reason " + std::to_string(int(result.reason)));
                    token->rollback();
                    if (serialize(*document) != before) {
                        failures.push_back(where + ": rollback did not restore the file");
                        stop_file = true;
                        break;
                    }
                    continue;
                }
                if (result.state != SW::StrokeWidthApplyState::Applied) {
                    ++(result.state == SW::StrokeWidthApplyState::Unchanged ? unchanged : refused);
                    token->rollback();
                    continue;
                }
                bool const committed = token->commitAtomically(
                    Inkscape::Util::Internal::ContextString("Stroke width"), "",
                    [&] { return SW::stroke_widths_compatible_output_ready(*document, plan, 61, *token); });
                if (!committed) {
                    failures.push_back(where + ": not confirmed");
                    token->rollback();
                    continue;
                }
                ++applied;
                if (intent.kind == SW::StrokeWidthIntentKind::AbsoluteCssPx) {
                    auto const again = SW::prepare_stroke_widths(*document, roots, intent, 62);
                    if (again.state == SW::StrokeWidthPlanState::Prepared && again.planned_changes != 0) {
                        failures.push_back(where + ": repeating it would change " + std::to_string(again.planned_changes));
                    }
                }
                if (!Inkscape::DocumentUndo::undo(document.get()) || serialize(*document) != before) {
                    failures.push_back(where + ": Undo did not restore the file");
                    stop_file = true;
                    break;
                }
            }
        }
        std::cout << "fixture " << file.filename().string() << ": " << applied << " applied, " << unchanged
                  << " unchanged, " << refused << " refused, " << failures.size() << " failures\n";
        for (std::size_t i = 0; i < failures.size() && i < 20; ++i) std::cout << "  " << failures[i] << "\n";
        for (auto const &failure : failures) all_failures.push_back(file.filename().string() + ": " + failure);
    }
    EXPECT_TRUE(all_failures.empty());
}

// Found by the adversarial fixture sweep (2026-09-28): a filter on a
// horizontal line wrote an infinite region (y="-inf" height="inf"), and that
// automatic write, left outside the history, made the next stroke change
// refuse ("another edit is in progress").
TEST_F(StrokeWidthControllerTest, FilteredLineHasAFiniteRegionAndTakesAStrokeWidth)
{
    auto document = parse(R"x(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="50"><defs><filter id="blur"><feGaussianBlur stdDeviation="0.2"/></filter></defs><g id="filtered" filter="url(#blur)" style="stroke:black;stroke-width:0.5"><path d="M2 8 H40"/></g></svg>)x");
    ASSERT_TRUE(document);
    auto const *filter = document->getObjectById("blur")->getRepr();
    for (auto const *name : {"x", "y", "width", "height"}) {
        auto const *value = filter->attribute(name);
        EXPECT_TRUE(!value || std::isfinite(g_ascii_strtod(value, nullptr))) << name << "=" << value;
    }
    std::vector<SPItem *> roots{item(*document, "filtered")};
    auto const plan = SW::prepare_stroke_widths(*document, roots, absolute(2.0), 71);
    auto token = SW::begin_stroke_width_interaction(*document, plan);
    ASSERT_TRUE(token);
    EXPECT_EQ(SW::apply_stroke_widths_compatible(*document, plan, 71, *token).state, SW::StrokeWidthApplyState::Applied);
    EXPECT_TRUE(token->commitAtomically(Inkscape::Util::Internal::ContextString("Stroke width"), "", [&] {
        return SW::stroke_widths_compatible_output_ready(*document, plan, 71, *token);
    }));
}

// A path effect rewrites its path after Undo, outside the history; the next
// stroke change settles that and applies instead of refusing.
TEST_F(StrokeWidthControllerTest, StrokeWidthAppliesAgainAfterUndoOnAPathEffect)
{
    auto document = parse(R"x(<svg xmlns="http://www.w3.org/2000/svg" xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape" width="100" height="50"><defs><inkscape:path-effect effect="spiro" id="effect"/></defs><path id="lpepath" inkscape:path-effect="#effect" inkscape:original-d="M2 8 H40" d="M2 8 H40" stroke="black" stroke-width="0.5"/></svg>)x");
    ASSERT_TRUE(document);
    std::vector<SPItem *> roots{item(*document, "lpepath")};
    for (double px : {0.2, 0.4, 0.6}) {
        SCOPED_TRACE(px);
        auto const plan = SW::prepare_stroke_widths(*document, roots, absolute(px), 72);
        auto token = SW::begin_stroke_width_interaction(*document, plan);
        ASSERT_TRUE(token);
        ASSERT_EQ(SW::apply_stroke_widths_compatible(*document, plan, 72, *token).state,
                  SW::StrokeWidthApplyState::Applied);
        ASSERT_TRUE(token->commitAtomically(Inkscape::Util::Internal::ContextString("Stroke width"), "", [&] {
            return SW::stroke_widths_compatible_output_ready(*document, plan, 72, *token);
        }));
        ASSERT_TRUE(Inkscape::DocumentUndo::undo(document.get()));
        document->ensureUpToDate();
        EXPECT_DOUBLE_EQ(computed_width(*document, "lpepath"), 0.5);
    }
}

// Review of dbee44c1b: a request that changes nothing never settles pending
// automatic changes into a step (the Redo branch survives).
TEST_F(StrokeWidthControllerTest, NoOpStrokeWidthKeepsRedoWithPendingAutomaticChanges)
{
    auto document = parse(R"x(<svg xmlns="http://www.w3.org/2000/svg" xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape" width="100" height="50"><defs><inkscape:path-effect effect="spiro" id="effect"/></defs><path id="lpepath" inkscape:path-effect="#effect" inkscape:original-d="M2 8 H40" d="M2 8 H40" stroke="black" stroke-width="0.5"/></svg>)x");
    ASSERT_TRUE(document);
    std::vector<SPItem *> roots{item(*document, "lpepath")};
    {
        auto const plan = SW::prepare_stroke_widths(*document, roots, absolute(0.8), 81);
        auto token = SW::begin_stroke_width_interaction(*document, plan);
        ASSERT_TRUE(token);
        ASSERT_EQ(SW::apply_stroke_widths_compatible(*document, plan, 81, *token).state,
                  SW::StrokeWidthApplyState::Applied);
        ASSERT_TRUE(token->commitAtomically(Inkscape::Util::Internal::ContextString("Stroke width"), "", [&] {
            return SW::stroke_widths_compatible_output_ready(*document, plan, 81, *token);
        }));
    }
    ASSERT_TRUE(Inkscape::DocumentUndo::undo(document.get()));
    document->ensureUpToDate(); // the path effect may rewrite its path now
    // The width it already has: nothing to change, nothing settled.
    auto const plan = SW::prepare_stroke_widths(*document, roots, absolute(0.5), 82);
    EXPECT_EQ(plan.planned_changes, 0u);
    auto token = SW::begin_stroke_width_interaction(*document, plan);
    if (token) token->rollback();
    EXPECT_TRUE(Inkscape::DocumentUndo::redo(document.get())) << "the Redo branch survives";
}

// WP1 A14 is intentionally runnable with the old controller API.
TEST_F(StrokeWidthControllerTest, WP1A14AbsoluteEmptyTextDoesNotBlockShape)
{
    auto document = parse(std::string{svg_open} +
        R"(<rect id="r" width="10" height="10" style="stroke:black;stroke-width:2"/>)"
        R"(<text id="empty" style="stroke:black;stroke-width:2"/>)"
        R"(</svg>)");
    ASSERT_TRUE(document);
    settle(*document);
    auto const before = serialize(*document);
    auto const plan = SW::prepare_stroke_widths(
        *document, {item(*document, "r"), item(*document, "empty")}, absolute(4), 1001);
    EXPECT_EQ(plan.query.eligible, 1u);
    EXPECT_EQ(plan.query.unavailable, 1u);
    EXPECT_EQ(serialize(*document), before);
    auto token = DocumentUndo::beginAtomicInteraction(document.get());
    ASSERT_TRUE(token);
    auto const result = SW::apply_stroke_widths_compatible(*document, plan, 1001, *token);
    EXPECT_EQ(result.state, SW::StrokeWidthApplyState::Applied);
    EXPECT_EQ(result.changed, 1u);
    EXPECT_EQ(result.excluded, 1u);
    if (result.state != SW::StrokeWidthApplyState::Applied) {
        token->rollback();
        return;
    }
    EXPECT_DOUBLE_EQ(computed_width(*document, "r"), 4);
    EXPECT_DOUBLE_EQ(computed_width(*document, "empty"), 2);
    auto const after = serialize(*document);
    EXPECT_NE(after, before);
    ASSERT_TRUE(token->commitAtomically(Util::Internal::ContextString("WP1 A14"), "",
        [&] { return SW::stroke_widths_compatible_output_ready(*document, plan, 1001, *token); }));
    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    EXPECT_EQ(serialize(*document), before);
    ASSERT_TRUE(DocumentUndo::redo(document.get()));
    EXPECT_EQ(serialize(*document), after);
}

TEST_F(StrokeWidthControllerTest, WP1A01MixedPointIncreaseOneUndo)
{
    auto document = parse(std::string{svg_open} +
        R"(<rect id="a" width="10" height="10" style="stroke:black;stroke-width:1pt"/>)"
        R"(<rect id="b" width="10" height="10" style="stroke:black;stroke-width:2pt"/>)"
        R"(<rect id="c" width="10" height="10" style="stroke:black;stroke-width:8pt"/></svg>)");
    ASSERT_TRUE(document);
    settle(*document);
    auto const before = serialize(*document);
    auto const plan = SW::prepare_stroke_widths(*document,
        {item(*document, "a"), item(*document, "b"), item(*document, "c")}, additive(0.1 * 96 / 72), 1002);
    EXPECT_EQ(plan.query.state, SW::StrokeWidthQuery::Mixed);
    wp1_roundtrip(*document, plan, before, [&](auto const &r) {
        ASSERT_EQ(r.state, SW::StrokeWidthApplyState::Applied);
        EXPECT_EQ(r.changed, 3u); EXPECT_EQ(r.excluded, 0u); EXPECT_EQ(r.skipped_runs, 0u);
        wp1_width(*document, "a", 1.1 * 96 / 72, 1.1 * 96 / 72);
        wp1_width(*document, "b", 2.1 * 96 / 72, 2.1 * 96 / 72);
        wp1_width(*document, "c", 8.1 * 96 / 72, 8.1 * 96 / 72);
        EXPECT_EQ(SW::stroke_width_applied_note(plan, r), "");
    });
    EXPECT_FALSE(DocumentUndo::undo(document.get())) << "exactly one action";
}

TEST_F(StrokeWidthControllerTest, WP1A02MixedMillimetreDecreaseUsesSection21Floor)
{
    auto document = parse(std::string{svg_open} +
        R"(<rect id="a" width="10" height="10" style="stroke:black;stroke-width:0.05mm"/>)"
        R"(<rect id="b" width="10" height="10" style="stroke:black;stroke-width:0.1mm"/>)"
        R"(<rect id="c" width="10" height="10" style="stroke:black;stroke-width:0.15mm"/>)"
        R"(<rect id="d" width="10" height="10" style="stroke:black;stroke-width:1mm"/>)"
        R"(<rect id="e" width="10" height="10" style="stroke:black;stroke-width:2mm"/></svg>)");
    ASSERT_TRUE(document); settle(*document);
    auto const before = serialize(*document);
    std::vector<SPItem *> roots;
    for (auto id : {"a", "b", "c", "d", "e"}) roots.push_back(item(*document, id));
    auto const a_xml = sp_repr_write_buf(roots[0]->getRepr(), 0, false, GQuark(0), 0, 0).raw();
    auto const plan = SW::prepare_stroke_widths(*document, roots, additive(-0.05 * 96 / 25.4), 1003);
    wp1_roundtrip(*document, plan, before, [&](auto const &r) {
        ASSERT_EQ(r.state, SW::StrokeWidthApplyState::Applied);
        EXPECT_EQ(r.changed, 4u); EXPECT_EQ(r.unchanged, 1u); EXPECT_EQ(r.excluded, 0u);
        char const *ids[] = {"a", "b", "c", "d", "e"};
        double mm[] = {0.05, 0.05, 0.10, 0.95, 1.95};
        for (unsigned i = 0; i < 5; ++i) wp1_width(*document, ids[i], mm[i] * 96 / 25.4, mm[i] * 96 / 25.4);
        EXPECT_EQ(sp_repr_write_buf(roots[0]->getRepr(), 0, false, GQuark(0), 0, 0).raw(), a_xml);
    });
}

TEST_F(StrokeWidthControllerTest, WP1A03FloorBoundariesAndPositiveZero)
{
    auto document = parse(std::string{svg_open} +
        R"(<rect id="a" width="10" height="10" style="stroke:black;stroke-width:1"/>)"
        R"(<rect id="b" width="10" height="10" style="stroke:black;stroke-width:1.5"/>)"
        R"(<rect id="c" width="10" height="10" style="stroke:black;stroke-width:2"/>)"
        R"(<rect id="d" width="10" height="10" style="stroke:black;stroke-width:1.9999999995"/>)"
        R"(<rect id="e" width="10" height="10" style="stroke:black;stroke-width:1.999999996"/>)"
        R"(<rect id="z" width="10" height="10" style="stroke:black;stroke-width:0"/></svg>)");
    ASSERT_TRUE(document); settle(*document);
    auto const before = serialize(*document);
    std::vector<SPItem *> roots;
    for (auto id : {"a", "b", "c", "d", "e"}) roots.push_back(item(*document, id));
    auto const plan = SW::prepare_stroke_widths(*document, roots, additive(-1), 1004);
    wp1_roundtrip(*document, plan, before, [&](auto const &r) {
        ASSERT_EQ(r.state, SW::StrokeWidthApplyState::Applied);
        EXPECT_EQ(r.changed, 2u); EXPECT_EQ(r.unchanged, 3u);
        wp1_width(*document, "a", 1, 1); wp1_width(*document, "b", 1.5, 1.5);
        wp1_width(*document, "c", 1, 1); wp1_width(*document, "d", 0.9999999995, 0.9999999995);
        wp1_width(*document, "e", 1.999999996, 1.999999996);
    });
    auto const zero = SW::prepare_stroke_widths(*document, {item(*document, "z")}, additive(1), 1005);
    wp1_roundtrip(*document, zero, before, [&](auto const &r) {
        ASSERT_EQ(r.state, SW::StrokeWidthApplyState::Applied);
        wp1_width(*document, "z", 1, 1);
    });
}

TEST_F(StrokeWidthControllerTest, WP1A04HairlineExcluded)
{
    auto document = parse(std::string{svg_open} +
        R"(<rect id="h" width="10" height="10" style="stroke:black;stroke-width:1;vector-effect:non-scaling-stroke;-inkscape-stroke:hairline"/>)"
        R"(<rect id="r" width="10" height="10" style="stroke:black;stroke-width:1pt"/></svg>)");
    ASSERT_TRUE(document); settle(*document);
    auto const before = serialize(*document);
    auto *hair = item(*document, "h");
    auto const hair_xml = sp_repr_write_buf(hair->getRepr(), 0, false, GQuark(0), 0, 0).raw();
    auto const plan = SW::prepare_stroke_widths(*document, {hair, item(*document, "r")}, additive(0.1 * 96 / 72), 1006);
    ASSERT_TRUE(find_member(plan, hair));
    EXPECT_EQ(find_member(plan, hair)->reason, SW::StrokeWidthMemberReason::UnsupportedIntent);
    wp1_roundtrip(*document, plan, before, [&](auto const &r) {
        ASSERT_EQ(r.state, SW::StrokeWidthApplyState::Applied);
        EXPECT_EQ(r.changed, 1u); EXPECT_EQ(r.excluded, 1u); EXPECT_EQ(r.skipped_runs, 0u);
        wp1_width(*document, "r", 1.1 * 96 / 72, 1.1 * 96 / 72);
        EXPECT_EQ(sp_repr_write_buf(hair->getRepr(), 0, false, GQuark(0), 0, 0).raw(), hair_xml);
        EXPECT_EQ(SW::stroke_width_applied_note(plan, r), "Stroke width applied; incompatible or protected items were skipped");
    });
}

TEST_F(StrokeWidthControllerTest, WP1A05InheritedGroupChildrenOnly)
{
    auto document = parse(std::string{svg_open} +
        R"(<g id="g" style="stroke:black;stroke-width:2">)"
        R"(<rect id="a" width="10" height="10"/>)"
        R"(<rect id="b" width="10" height="10"/>)"
        R"(<rect id="locked" width="10" height="10" sodipodi:insensitive="true"/>)"
        R"(<image id="bitmap" width="10" height="10"/></g></svg>)");
    ASSERT_TRUE(document); settle(*document);
    auto const before = serialize(*document);
    auto const group_style = std::string(item(*document, "g")->getRepr()->attribute("style"));
    std::vector<std::string> untouched;
    for (auto id : {"locked", "bitmap"}) untouched.push_back(sp_repr_write_buf(item(*document, id)->getRepr(), 0, false, GQuark(0), 0, 0).raw());
    auto const plan = SW::prepare_stroke_widths(*document, {item(*document, "g")}, additive(1), 1007);
    wp1_roundtrip(*document, plan, before, [&](auto const &r) {
        ASSERT_EQ(r.state, SW::StrokeWidthApplyState::Applied);
        EXPECT_EQ(r.changed, 2u); EXPECT_EQ(r.excluded, 2u);
        wp1_width(*document, "a", 3, 3); wp1_width(*document, "b", 3, 3);
        EXPECT_EQ(inline_style_map(item(*document, "a"))["stroke-width"], "3");
        EXPECT_EQ(inline_style_map(item(*document, "b"))["stroke-width"], "3");
        EXPECT_EQ(item(*document, "g")->getRepr()->attribute("style"), group_style);
        unsigned i = 0;
        for (auto id : {"locked", "bitmap"}) EXPECT_EQ(sp_repr_write_buf(item(*document, id)->getRepr(), 0, false, GQuark(0), 0, 0).raw(), untouched[i++]);
    });
}

TEST_F(StrokeWidthControllerTest, WP1A06ReversedTextRangeAndCaret)
{
    auto document = parse(std::string{svg_open} +
        R"(<text id="t" x="0" y="20" style="font-family:sans-serif;font-size:20px;stroke:black">)"
        R"(<tspan style="stroke-width:1pt">AB</tspan><tspan style="stroke-width:3pt">CD</tspan></text></svg>)");
    ASSERT_TRUE(document); settle(*document);
    auto *owner = item(*document, "t");
    ASSERT_EQ(native_char_count(owner), 4u);
    auto const before = serialize(*document);
    SW::StrokeWidthTextRange range; range.owner = owner; range.first_char = 3; range.last_char = 1;
    auto const plan = SW::prepare_stroke_widths_combined(*document, range, {owner}, additive(0.1 * 96 / 72), 1008);
    ASSERT_EQ(plan.query.targets.size(), 1u);
    EXPECT_TRUE(plan.query.targets[0].reversed);
    wp1_roundtrip(*document, plan, before, [&](auto const &r) {
        ASSERT_EQ(r.state, SW::StrokeWidthApplyState::Applied);
        EXPECT_EQ(r.changed, 1u); EXPECT_EQ(r.excluded, 0u); EXPECT_EQ(r.skipped_runs, 0u);
        double const widths[] = {96.0/72, 1.46667, 4.13333, 4};
        for (unsigned i = 0; i < 4; ++i) {
            auto w = native_char_widths(*document, owner, i);
            EXPECT_NEAR(w.local, widths[i], 1e-6);
            ASSERT_TRUE(w.effective); EXPECT_NEAR(*w.effective, widths[i], 1e-6);
        }
        EXPECT_EQ(range.first_char, 3u); EXPECT_EQ(range.last_char, 1u); EXPECT_FALSE(range.caret);
        EXPECT_EQ(sp_te_get_string_multiline(owner), "ABCD");
    });
    range.caret = true; range.first_char = range.last_char = 2;
    auto const caret = SW::prepare_stroke_widths_combined(*document, range, {owner}, additive(1), 1009);
    EXPECT_TRUE(caret.query.targets[0].caret_scope);
    EXPECT_EQ(caret.query.targets[0].raw_first_char, 2u);
    wp1_roundtrip(*document, caret, before, [&](auto const &r) {
        ASSERT_EQ(r.state, SW::StrokeWidthApplyState::Applied);
        for (unsigned i = 0; i < 4; ++i) EXPECT_NEAR(native_char_widths(*document, owner, i).local, i < 2 ? 2.33333 : 5, 1e-6);
        EXPECT_TRUE(range.caret); EXPECT_EQ(range.first_char, 2u); EXPECT_EQ(range.last_char, 2u);
    });
}

TEST_F(StrokeWidthControllerTest, WP1A07SkippedTextRunsSeparateFromOwner)
{
    auto document = parse(std::string{svg_open} +
        R"(<style>.fixed {stroke-width:3 !important}</style>)"
        R"(<text id="t" x="0" y="20" style="font-family:sans-serif;font-size:20px;stroke:black">)"
        R"(<tspan style="stroke-width:1">A</tspan>)"
        R"(<tspan style="stroke-width:1;vector-effect:non-scaling-stroke;-inkscape-stroke:hairline">B</tspan>)"
        R"(<tspan class="fixed">C</tspan></text></svg>)");
    ASSERT_TRUE(document); settle(*document);
    auto const before = serialize(*document);
    auto *owner = item(*document, "t");
    auto const plan = SW::prepare_stroke_widths(*document, {owner}, additive(1), 1010);
    EXPECT_EQ(plan.skipped_runs, 2u);
    wp1_roundtrip(*document, plan, before, [&](auto const &r) {
        EXPECT_EQ(r.state, SW::StrokeWidthApplyState::Applied) << serialize(*document);
        EXPECT_EQ(r.changed, 1u); EXPECT_EQ(r.excluded, 0u); EXPECT_EQ(r.skipped_runs, 2u);
        EXPECT_DOUBLE_EQ(native_char_widths(*document, owner, 0).local, 2);
        EXPECT_EQ(native_char_widths(*document, owner, 1).convention, SW::StrokeWidthConvention::Hairline);
        EXPECT_DOUBLE_EQ(native_char_widths(*document, owner, 2).local, 3);
        EXPECT_EQ(SW::stroke_width_applied_note(plan, r), "Stroke width applied; incompatible or protected items were skipped");
    }, 2);
}

TEST_F(StrokeWidthControllerTest, WP1A07AbsolutePreservesStylesheetProtectedSpan)
{
    auto document = parse(std::string{svg_open} +
        R"(<style>.fixed {stroke-width:3 !important}</style>)"
        R"(<text id="t" x="0" y="20" style="font-family:sans-serif;font-size:20px;stroke:black">)"
        R"(<tspan style="stroke-width:1">A</tspan>)"
        R"(<tspan style="stroke-width:1;vector-effect:non-scaling-stroke;-inkscape-stroke:hairline">B</tspan>)"
        R"(<tspan class="fixed">C</tspan></text></svg>)");
    ASSERT_TRUE(document); settle(*document);
    auto const before = serialize(*document);
    auto *owner = item(*document, "t");
    auto *protected_span = native_char_widths(*document, owner, 2).style_source;
    ASSERT_TRUE(protected_span);
    auto const protected_xml = sp_repr_write_buf(protected_span->getRepr(), 0, false, GQuark(0), 0, 0).raw();
    std::vector<NativeCharIdentity> identities;
    std::vector<Geom::Point> positions;
    auto const *layout = te_get_layout(owner); ASSERT_TRUE(layout);
    for (unsigned i = 0; i < 3; ++i) {
        identities.push_back(native_char_identity(owner, i));
        positions.push_back(layout->characterAnchorPoint(layout->charIndexToIterator(i)));
    }
    auto const plan = SW::prepare_stroke_widths(*document, {owner}, absolute(2), 1037);
    EXPECT_EQ(plan.skipped_runs, 1u);
    wp1_roundtrip(*document, plan, before, [&](auto const &r) {
        ASSERT_EQ(r.state, SW::StrokeWidthApplyState::Applied) << serialize(*document);
        EXPECT_EQ(r.changed, 1u); EXPECT_EQ(r.excluded, 0u); EXPECT_EQ(r.skipped_runs, 1u);
        // Absolute also converts the hairline to the requested numeric width.
        for (unsigned i = 0; i < 3; ++i) {
            auto const widths = native_char_widths(*document, owner, i);
            EXPECT_DOUBLE_EQ(widths.local, i == 2 ? 3 : 2);
            ASSERT_TRUE(widths.effective);
            EXPECT_DOUBLE_EQ(*widths.effective, i == 2 ? 3 : 2);
            EXPECT_EQ(widths.convention, SW::StrokeWidthConvention::Ordinary);
            EXPECT_EQ(native_char_identity(owner, i), identities[i]);
            auto const *current_layout = te_get_layout(owner); ASSERT_TRUE(current_layout);
            auto const position = current_layout->characterAnchorPoint(current_layout->charIndexToIterator(i));
            EXPECT_EQ(position, positions[i]);
        }
        EXPECT_EQ(native_char_widths(*document, owner, 2).style_source, protected_span);
        EXPECT_EQ(sp_repr_write_buf(protected_span->getRepr(), 0, false, GQuark(0), 0, 0).raw(), protected_xml);
        EXPECT_EQ(count_tspans(owner), 3u);
        EXPECT_STREQ(owner->getRepr()->attribute("x"), "0");
        EXPECT_STREQ(owner->getRepr()->attribute("y"), "20");
        EXPECT_EQ(SW::stroke_width_applied_note(plan, r), "Stroke width applied; incompatible or protected items were skipped");
    }, 1);
}

TEST_F(StrokeWidthControllerTest, WP1A08CloneAloneFollowingScaledAndFloor)
{
    auto document = parse(std::string{svg_open} +
        R"(<rect id="src" width="10" height="10" style="stroke:black;stroke-width:2"/>)"
        R"svg(<use id="clone" xlink:href="#src" x="20" transform="scale(3)"/>)svg"
        R"(</svg>)");
    ASSERT_TRUE(document); settle(*document);
    auto const before = serialize(*document);
    auto *source = item(*document, "src"); auto *clone = item(*document, "clone");
    auto const clone_xml = sp_repr_write_buf(clone->getRepr(), 0, false, GQuark(0), 0, 0).raw();
    auto const alone = SW::prepare_stroke_widths(*document, {clone}, additive(1), 1011);
    wp1_roundtrip(*document, alone, before, [&](auto const &r) {
        EXPECT_EQ(r.state, SW::StrokeWidthApplyState::Unchanged); EXPECT_EQ(r.excluded, 1u);
        EXPECT_EQ(SW::stroke_width_applied_note(alone, r), "Stroke width: incompatible or protected items were skipped");
    });
    auto const both = SW::prepare_stroke_widths(*document, {source, clone}, additive(1), 1012);
    EXPECT_EQ(both.following_clones, 1u);
    wp1_roundtrip(*document, both, before, [&](auto const &r) {
        ASSERT_EQ(r.state, SW::StrokeWidthApplyState::Applied);
        EXPECT_EQ(r.changed, 1u); EXPECT_EQ(r.excluded, 1u);
        wp1_width(*document, "src", 3, 3);
        auto *shadow = cast<SPItem>(clone->firstChild()); ASSERT_TRUE(shadow && shadow->style);
        EXPECT_DOUBLE_EQ(shadow->style->stroke_width.computed * shadow->i2doc_affine().descrim(), 9);
        EXPECT_EQ(sp_repr_write_buf(clone->getRepr(), 0, false, GQuark(0), 0, 0).raw(), clone_xml);
        EXPECT_EQ(SW::stroke_width_applied_note(both, r), "Stroke width applied; 1 linked clone follows its original");
    });
    auto const floor = SW::prepare_stroke_widths(*document, {source, clone}, additive(-2), 1013);
    EXPECT_EQ(floor.following_clones, 0u);
    wp1_roundtrip(*document, floor, before, [&](auto const &r) {
        EXPECT_EQ(r.state, SW::StrokeWidthApplyState::Unchanged); EXPECT_EQ(r.unchanged, 1u);
        EXPECT_EQ(r.excluded, 1u); EXPECT_EQ(SW::stroke_width_applied_note(floor, r), "Already at the minimum step");
    });
}

TEST_F(StrokeWidthControllerTest, WP1A09TransformsAndMillimetreViewBox)
{
    struct Case { char const *transform; double scale; };
    for (auto c : {Case{"scale(2)", 2}, Case{"scale(2,8)", 4}, Case{"rotate(30)", 1}, Case{"scale(-2,2)", 2}}) {
        SCOPED_TRACE(c.transform);
        auto document = parse(std::string{svg_open} +
            "<g transform=\"" + c.transform + "\"><rect id=\"r\" width=\"10\" height=\"10\" style=\"stroke:black;stroke-width:2\"/></g></svg>");
        ASSERT_TRUE(document); settle(*document);
        auto const before = serialize(*document);
        auto const plan = SW::prepare_stroke_widths(*document, {item(*document, "r")}, additive(1), 1014);
        wp1_roundtrip(*document, plan, before, [&](auto const &r) {
            ASSERT_EQ(r.state, SW::StrokeWidthApplyState::Applied);
            wp1_width(*document, "r", 2 + 1 / c.scale, 2 * c.scale + 1);
            EXPECT_GT(computed_width(*document, "r"), 0);
        });
    }
    auto document = parse(R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="100mm" height="100mm" viewBox="0 0 100 100"><g transform="scale(2)"><rect id="r" width="10" height="10" style="stroke:black;stroke-width:0.5"/></g><text id="t" x="0" y="20" style="font-family:sans-serif;font-size:10px;stroke:black;stroke-width:1">AB</text></svg>)svg");
    ASSERT_TRUE(document); settle(*document);
    auto const before = serialize(*document);
    auto const plan = SW::prepare_stroke_widths(*document, {item(*document, "r"), item(*document, "t")}, additive(0.05 * 96 / 25.4), 1015);
    wp1_roundtrip(*document, plan, before, [&](auto const &r) {
        ASSERT_EQ(r.state, SW::StrokeWidthApplyState::Applied); EXPECT_EQ(r.changed, 2u);
        wp1_width(*document, "r", 0.525, 1.05 * 96 / 25.4);
        auto w = native_char_widths(*document, item(*document, "t"), 0);
        EXPECT_NEAR(w.local, 1.05, 1e-9); ASSERT_TRUE(w.effective);
        EXPECT_NEAR(*w.effective, 1.05 * 96 / 25.4, 1e-9);
    });
}

TEST_F(StrokeWidthControllerTest, WP1A10UnsafeCandidatesDoNotBlockOthers)
{
    auto document = parse(std::string{svg_open} +
        R"(<rect id="good" width="10" height="10" style="stroke:black;stroke-width:2"/>)"
        R"svg(<rect id="singular" width="10" height="10" transform="scale(0)" style="stroke:black;stroke-width:2"/>)svg"
        R"(<rect id="nonfinite" width="10" height="10" style="stroke:black;stroke-width:2"/>)"
        R"(<rect id="overflow" width="10" height="10" style="stroke:black;stroke-width:2"/></svg>)");
    ASSERT_TRUE(document); settle(*document);
    item(*document, "nonfinite")->transform = Geom::Affine(std::numeric_limits<double>::infinity(), 0, 0, 1, 0, 0);
    item(*document, "overflow")->transform = Geom::Affine(1e200, 0, 0, 1e200, 0, 0);
    auto const before = serialize(*document);
    auto const plan = SW::prepare_stroke_widths(*document,
        {item(*document, "good"), item(*document, "singular"), item(*document, "nonfinite"), item(*document, "overflow")}, additive(1), 1016);
    EXPECT_TRUE(find_excluded(plan.query, SW::StrokeWidthExclusion::SingularTransform));
    EXPECT_TRUE(find_excluded(plan.query, SW::StrokeWidthExclusion::NonFiniteTransform));
    wp1_roundtrip(*document, plan, before, [&](auto const &r) {
        ASSERT_EQ(r.state, SW::StrokeWidthApplyState::Applied); EXPECT_EQ(r.changed, 1u); EXPECT_EQ(r.excluded, 3u);
        wp1_width(*document, "good", 3, 3);
        EXPECT_DOUBLE_EQ(computed_width(*document, "singular"), 2);
        EXPECT_DOUBLE_EQ(computed_width(*document, "nonfinite"), 2);
        EXPECT_DOUBLE_EQ(computed_width(*document, "overflow"), 2);
    });
}

TEST_F(StrokeWidthControllerTest, WP1A10FiniteTransformLocalUnderflowExcluded)
{
    auto document = parse(std::string{svg_open} +
        R"svg(<rect id="good" width="10" height="10" transform="scale(1e-150)" style="stroke:black;stroke-width:0"/>)svg"
        R"svg(<rect id="underflow" width="10" height="10" transform="scale(1e150)" style="stroke:black;stroke-width:0"/>)svg"
        R"(</svg>)");
    ASSERT_TRUE(document); settle(*document);
    auto const before = serialize(*document);
    auto const plan = SW::prepare_stroke_widths(*document,
        {item(*document, "good"), item(*document, "underflow")}, additive(1e-160), 1017);
    auto const *excluded = find_member(plan, item(*document, "underflow")); ASSERT_TRUE(excluded);
    EXPECT_TRUE(excluded->target.safe_transform);
    EXPECT_EQ(excluded->outcome, SW::StrokeWidthMemberOutcome::Excluded);
    EXPECT_EQ(excluded->reason, SW::StrokeWidthMemberReason::InvalidIntent);
    wp1_roundtrip(*document, plan, before, [&](auto const &r) {
        ASSERT_EQ(r.state, SW::StrokeWidthApplyState::Applied); EXPECT_EQ(r.changed, 1u); EXPECT_EQ(r.excluded, 1u);
        EXPECT_NEAR(computed_width(*document, "good"), 1e-10, 1e-23);
        EXPECT_NEAR(computed_width(*document, "good") * item(*document, "good")->i2doc_affine().descrim(), 1e-160, 1e-173);
        EXPECT_DOUBLE_EQ(computed_width(*document, "underflow"), 0);
    });
}

TEST_F(StrokeWidthControllerTest, WP1A11NonScalingShapeAndTextKeepVectorEffect)
{
    auto document = parse(std::string{svg_open} +
        R"svg(<g transform="scale(5)"><rect id="r" width="10" height="10" style="stroke:black;stroke-width:2;vector-effect:non-scaling-stroke !important"/>)svg"
        R"(<text id="t" x="0" y="20" style="font-family:sans-serif;font-size:20px;stroke:black;stroke-width:2;vector-effect:non-scaling-stroke !important">AB</text></g></svg>)");
    ASSERT_TRUE(document); settle(*document);
    auto const before = serialize(*document);
    auto const plan = SW::prepare_stroke_widths(*document, {item(*document, "r"), item(*document, "t")}, additive(1), 1018);
    wp1_roundtrip(*document, plan, before, [&](auto const &r) {
        ASSERT_EQ(r.state, SW::StrokeWidthApplyState::Applied); EXPECT_EQ(r.changed, 2u);
        wp1_width(*document, "r", 3, 3);
        auto w = native_char_widths(*document, item(*document, "t"), 0);
        EXPECT_DOUBLE_EQ(w.local, 3); ASSERT_TRUE(w.effective); EXPECT_DOUBLE_EQ(*w.effective, 3);
        EXPECT_EQ(w.convention, SW::StrokeWidthConvention::NonScaling);
        EXPECT_TRUE(item(*document, "r")->style->vector_effect.important);
        EXPECT_EQ(inline_style_map(item(*document, "r"))["vector-effect"], "non-scaling-stroke !important");
    });
}

TEST_F(StrokeWidthControllerTest, WP1A12DashPolicyAndIndependentPriorities)
{
    for (bool scale : {false, true}) {
        SCOPED_TRACE(scale);
        auto document = parse(std::string{svg_open} +
            R"(<g style="stroke:black;stroke-dasharray:4 2;stroke-dashoffset:-2">)"
            R"(<rect id="inherited" width="10" height="10" style="stroke-width:2"/>)"
            R"(<rect id="zero" width="10" height="10" style="stroke-width:0"/>)"
            R"(<rect id="array" width="10" height="10" style="stroke-width:2;stroke-dasharray:4 2 !important;stroke-dashoffset:-2"/>)"
            R"(<rect id="offset" width="10" height="10" style="stroke-width:2;stroke-dasharray:4 2;stroke-dashoffset:-2 !important"/>)"
            R"(<text id="text" x="0" y="20" style="font-family:sans-serif;font-size:20px;stroke-width:2;stroke-dasharray:4 2 !important;stroke-dashoffset:-2">AB</text></g></svg>)");
        ASSERT_TRUE(document); settle(*document);
        auto const before = serialize(*document);
        std::vector<SPItem *> roots;
        for (auto id : {"inherited", "zero", "array", "offset", "text"}) roots.push_back(item(*document, id));
        auto const intent = additive(2, scale);
        auto const plan = SW::prepare_stroke_widths(*document, roots, intent, 1019);
        EXPECT_EQ(plan.intent.scale_dashes, intent.scale_dashes);
        for (auto id : {"inherited", "array", "offset", "text"}) {
            auto const *member = find_member(plan, item(*document, id)); ASSERT_TRUE(member);
            if (member->target.kind == SW::StrokeWidthTargetKind::TextOwner) {
                ASSERT_EQ(member->text_runs.size(), 1u);
                auto const &run = member->text_runs[0];
                EXPECT_EQ(run.native_dasharray_computed.has_value(), intent.scale_dashes);
                EXPECT_EQ(run.local_dashoffset.has_value(), intent.scale_dashes);
                if (intent.scale_dashes) {
                    ASSERT_TRUE(run.native_dasharray_computed); ASSERT_TRUE(run.local_dashoffset);
                    EXPECT_EQ(*run.native_dasharray_computed, (std::vector<double>{8,4}));
                    EXPECT_DOUBLE_EQ(*run.local_dashoffset, -4);
                }
            } else {
                EXPECT_EQ(member->native_dasharray_computed.has_value(), intent.scale_dashes);
                EXPECT_EQ(member->local_dashoffset.has_value(), intent.scale_dashes);
                if (intent.scale_dashes) {
                    ASSERT_TRUE(member->native_dasharray_computed); ASSERT_TRUE(member->local_dashoffset);
                    EXPECT_EQ(*member->native_dasharray_computed, (std::vector<double>{8,4}));
                    EXPECT_DOUBLE_EQ(*member->local_dashoffset, -4);
                }
            }
        }
        auto const *zero = find_member(plan, item(*document, "zero")); ASSERT_TRUE(zero);
        EXPECT_FALSE(zero->local_dasharray); EXPECT_FALSE(zero->local_dashoffset);
        wp1_roundtrip(*document, plan, before, [&](auto const &r) {
            ASSERT_EQ(r.state, SW::StrokeWidthApplyState::Applied); EXPECT_EQ(r.changed, 5u); EXPECT_EQ(r.excluded, 0u);
            for (auto id : {"inherited", "array", "offset", "text"}) {
                auto *owner = item(*document, id);
                wp1_width(*document, id, 4, 4);
                EXPECT_EQ(owner->style->stroke_dasharray.get_computed(), (scale ? std::vector<double>{8,4} : std::vector<double>{4,2}));
                EXPECT_DOUBLE_EQ(owner->style->stroke_dashoffset.computed, scale ? -4 : -2);
                EXPECT_EQ(owner->style->stroke_dasharray.important, std::string(id) == "array" || std::string(id) == "text");
                EXPECT_EQ(owner->style->stroke_dashoffset.important, std::string(id) == "offset");
            }
            wp1_width(*document, "zero", 2, 2);
            EXPECT_EQ(item(*document, "zero")->style->stroke_dasharray.get_computed(), (std::vector<double>{4,2}));
            EXPECT_DOUBLE_EQ(item(*document, "zero")->style->stroke_dashoffset.computed, -2);
            if (!scale) EXPECT_EQ(inline_style_map(item(*document, "inherited")).count("stroke-dasharray"), 0u);
        });
    }
}

TEST_F(StrokeWidthControllerTest, WP1A13CanonicalNoOpShapeAndText)
{
    auto document = parse(std::string{svg_open} +
        R"(<rect id="r" width="10" height="10" style="stroke:black;stroke-width:100000000000000000000"/>)"
        R"(<rect id="small" width="10" height="10" style="stroke:black;stroke-width:3"/>)"
        R"(<text id="t" x="0" y="20" style="font-family:sans-serif;font-size:20px;stroke:black;stroke-width:2">AB</text></svg>)");
    ASSERT_TRUE(document); settle(*document); document->setVirgin(true);
    auto const before = serialize(*document);
    ASSERT_DOUBLE_EQ(computed_width(*document, "r"), 1e20);
    auto const huge = SW::prepare_stroke_widths(*document, {item(*document, "r")}, additive(0.1), 1020);
    wp1_roundtrip(*document, huge, before, [&](auto const &r) {
        EXPECT_EQ(r.state, SW::StrokeWidthApplyState::Unchanged); EXPECT_EQ(r.unchanged, 1u); EXPECT_EQ(r.attempted_writes, 0u);
        EXPECT_EQ(SW::stroke_width_applied_note(huge, r), "");
    });
    // Unlike the huge-double case, arithmetic changes this value. The native
    // CSS reader's storage precision must still make the shape a no-op.
    double const delta = 1e-15;
    ASSERT_NE(3.0 + delta, 3.0);
    auto const shape_rounded = SW::prepare_stroke_widths(*document, {item(*document, "small")}, additive(delta), 1036);
    ASSERT_EQ(shape_rounded.members.size(), 1u);
    EXPECT_EQ(shape_rounded.members[0].outcome, SW::StrokeWidthMemberOutcome::Unchanged);
    EXPECT_FALSE(shape_rounded.members[0].local_width);
    wp1_roundtrip(*document, shape_rounded, before, [&](auto const &r) {
        EXPECT_EQ(r.state, SW::StrokeWidthApplyState::Unchanged);
        EXPECT_EQ(r.unchanged, 1u); EXPECT_EQ(r.attempted_writes, 0u);
        EXPECT_EQ(SW::stroke_width_applied_note(shape_rounded, r), "");
        EXPECT_DOUBLE_EQ(computed_width(*document, "small"), 3);
        EXPECT_EQ(serialize(*document), before);
        EXPECT_FALSE(document->isModifiedSinceSave()); EXPECT_TRUE(document->getVirgin());
    });
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
    // The addition is representable as a double but lost by native text CSS precision.
    auto const rounded = SW::prepare_stroke_widths(*document, {item(*document, "t")}, additive(1e-8), 1021);
    ASSERT_EQ(rounded.members.size(), 1u);
    ASSERT_EQ(rounded.members[0].text_runs.size(), 1u);
    EXPECT_EQ(rounded.members[0].text_runs[0].outcome, SW::StrokeWidthMemberOutcome::Unchanged);
    wp1_roundtrip(*document, rounded, before, [&](auto const &r) {
        EXPECT_EQ(r.state, SW::StrokeWidthApplyState::Unchanged); EXPECT_EQ(r.unchanged, 1u); EXPECT_EQ(r.attempted_writes, 0u);
        EXPECT_EQ(SW::stroke_width_applied_note(rounded, r), "");
    });
    EXPECT_FALSE(document->isModifiedSinceSave()); EXPECT_TRUE(document->getVirgin());
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

TEST_F(StrokeWidthControllerTest, WP1A14AllIntentsGlyphlessTextAndRanges)
{
    for (auto intent : {absolute(4), relative(200), additive(2),
        SW::StrokeWidthIntent{SW::StrokeWidthIntentKind::Hairline, 0, false},
        SW::StrokeWidthIntent{SW::StrokeWidthIntentKind::RemoveStroke, 0, false}}) {
        SCOPED_TRACE(static_cast<int>(intent.kind));
        auto document = parse(std::string{svg_open} +
            R"(<rect id="r" width="10" height="10" style="stroke:black;stroke-width:2"/>)"
            R"(<text id="empty" style="stroke:black;stroke-width:2"/>)"
            R"(<text id="space" xml:space="preserve" style="stroke:black;stroke-width:2">   </text>)"
            R"(<text id="control" xml:space="preserve" style="stroke:black;stroke-width:2">&#9;&#10;</text>)"
            R"(<text id="cleared" style="stroke:black;stroke-width:2">AB</text></svg>)");
        ASSERT_TRUE(document); settle(*document);
        // Native text/flowtext embed their Layout, so a null layout cannot be
        // instantiated by the public model. Clear it to exercise absent output.
        cast<SPText>(item(*document, "cleared"))->layout.clear();
        auto const before = serialize(*document);
        std::vector<SPItem *> roots;
        std::vector<std::string> text_xml;
        roots.push_back(item(*document, "r"));
        for (auto id : {"empty", "space", "control", "cleared"}) {
            roots.push_back(item(*document, id));
            text_xml.push_back(sp_repr_write_buf(item(*document, id)->getRepr(), 0, false, GQuark(0), 0, 0).raw());
        }
        auto const plan = SW::prepare_stroke_widths(*document, roots, intent, 1022);
        EXPECT_EQ(plan.query.eligible, 3u); ASSERT_EQ(plan.query.excluded.size(), 2u);
        for (auto const &target : plan.query.excluded) EXPECT_EQ(target.exclusion, SW::StrokeWidthExclusion::EmptyText);
        wp1_roundtrip(*document, plan, before, [&](auto const &r) {
            ASSERT_EQ(r.state, SW::StrokeWidthApplyState::Applied); EXPECT_EQ(r.changed, 3u); EXPECT_EQ(r.excluded, 2u);
            unsigned i = 0;
            for (auto id : {"empty", "space", "control", "cleared"}) {
                if (i == 0 || i == 3) EXPECT_EQ(sp_repr_write_buf(item(*document, id)->getRepr(), 0, false, GQuark(0), 0, 0).raw(), text_xml[i]);
                ++i;
            }
            EXPECT_EQ(sp_te_get_string_multiline(item(*document, "space")), "   ");
            EXPECT_EQ(sp_te_get_string_multiline(item(*document, "control")), "\t");
            EXPECT_EQ(item(*document, "control")->getRepr()->firstChild()->content(), std::string("\t\n"));
            for (auto id : {"space", "control"}) {
                auto *text = item(*document, id);
                // Independent native rendering-source oracle, including glyphless characters.
                auto const *layout = te_get_layout(text); ASSERT_TRUE(layout);
                unsigned const count = layout->iteratorToCharIndex(layout->end());
                EXPECT_GT(count, 0u);
                for (unsigned index = 0; index < count; ++index) {
                    auto const observed = native_char_source(text, index);
                    ASSERT_TRUE(is<SPString>(observed.source));
                    auto *style = observed.source->parent->style; ASSERT_TRUE(style);
                    EXPECT_DOUBLE_EQ(style->stroke_width.computed,
                        intent.kind == SW::StrokeWidthIntentKind::RemoveStroke ? 2 :
                        intent.kind == SW::StrokeWidthIntentKind::Hairline ? 1 : 4);
                    EXPECT_EQ(style->stroke.isNone(), intent.kind == SW::StrokeWidthIntentKind::RemoveStroke);
                    EXPECT_EQ(style->stroke_extensions.hairline, intent.kind == SW::StrokeWidthIntentKind::Hairline);
                    EXPECT_EQ(style->vector_effect.stroke, intent.kind == SW::StrokeWidthIntentKind::Hairline);
                }
            }
            if (intent.kind == SW::StrokeWidthIntentKind::Hairline) EXPECT_EQ(SW::query_stroke_widths(*document, {roots[0]}).hairline, 1u);
            else if (intent.kind == SW::StrokeWidthIntentKind::RemoveStroke) EXPECT_TRUE(roots[0]->style->stroke.isNone());
            else wp1_width(*document, "r", 4, 4);
        });
    }
    auto document = parse(std::string{svg_open} + R"(<text id="t" xml:space="preserve" style="font-size:20px;stroke:black">A  B</text><rect id="r" width="10" height="10" style="stroke:black;stroke-width:2"/></svg>)");
    ASSERT_TRUE(document); settle(*document);
    auto const before = serialize(*document);
    SW::StrokeWidthTextRange range; range.owner = item(*document, "t"); range.first_char = 1; range.last_char = 3;
    auto const plan = SW::prepare_stroke_widths_combined(*document, range, {item(*document, "t"), item(*document, "r")}, additive(1), 1023);
    EXPECT_FALSE(find_excluded(plan.query, SW::StrokeWidthExclusion::EmptyText));
    wp1_roundtrip(*document, plan, before, [&](auto const &r) {
        ASSERT_EQ(r.state, SW::StrokeWidthApplyState::Applied); EXPECT_EQ(r.changed, 2u); EXPECT_EQ(r.excluded, 0u);
        wp1_width(*document, "r", 3, 3); EXPECT_EQ(sp_te_get_string_multiline(range.owner.get()), "A  B");
        for (unsigned index = 0; index < 4; ++index) {
            auto const observed = native_char_source(range.owner.get(), index);
            ASSERT_TRUE(is<SPString>(observed.source));
            EXPECT_DOUBLE_EQ(observed.source->parent->style->stroke_width.computed,
                             index == 1 || index == 2 ? 2 : 1);
        }
    });
}

TEST_F(StrokeWidthControllerTest, WP1A15StylesheetPriorityAndInlineImportant)
{
    auto document = parse(std::string{svg_open} +
        R"(<style>.fixed {stroke-width:2 !important}</style><rect id="fixed" class="fixed" width="10" height="10" style="stroke:black"/>)"
        R"(<rect id="inline" width="10" height="10" style="stroke:black;stroke-width:2 !important"/></svg>)");
    ASSERT_TRUE(document); settle(*document);
    auto const before = serialize(*document);
    auto const plan = SW::prepare_stroke_widths(*document, {item(*document, "fixed"), item(*document, "inline")}, additive(1), 1024);
    auto const *fixed = find_member(plan, item(*document, "fixed")); ASSERT_TRUE(fixed);
    EXPECT_EQ(fixed->outcome, SW::StrokeWidthMemberOutcome::Excluded); EXPECT_EQ(fixed->reason, SW::StrokeWidthMemberReason::StylePriority);
    wp1_roundtrip(*document, plan, before, [&](auto const &r) {
        ASSERT_EQ(r.state, SW::StrokeWidthApplyState::Applied); EXPECT_EQ(r.changed, 1u); EXPECT_EQ(r.excluded, 1u);
        wp1_width(*document, "fixed", 2, 2); wp1_width(*document, "inline", 3, 3);
        EXPECT_EQ(inline_style_map(item(*document, "inline"))["stroke-width"], "3 !important");
        EXPECT_EQ(SW::stroke_width_applied_note(plan, r), "Stroke width applied; incompatible or protected items were skipped");
    });
}

TEST_F(StrokeWidthControllerTest, WP1A17InvalidAdditiveIntentNeverWrites)
{
    for (double value : {std::numeric_limits<double>::quiet_NaN(), 0.0, -0.0,
        std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(), 1000000.1, -1000000.1}) {
        auto document = parse(std::string{svg_open} + R"(<rect id="r" width="10" height="10" style="stroke:black;stroke-width:2"/></svg>)");
        ASSERT_TRUE(document); settle(*document); document->setVirgin(true);
        auto const before = serialize(*document);
        auto const plan = SW::prepare_stroke_widths(*document, {item(*document, "r")}, additive(value), 1025);
        EXPECT_EQ(plan.state, SW::StrokeWidthPlanState::Rejected); EXPECT_EQ(plan.rejection, SW::StrokeWidthMemberReason::InvalidIntent);
        wp1_roundtrip(*document, plan, before, [&](auto const &r) {
            EXPECT_EQ(r.state, SW::StrokeWidthApplyState::Rejected); EXPECT_EQ(r.reason, SW::StrokeWidthMemberReason::InvalidIntent);
            EXPECT_EQ(r.attempted_writes, 0u); EXPECT_EQ(r.paint_none, 0u);
        });
        EXPECT_FALSE(DocumentUndo::undo(document.get()));
    }
}

TEST_F(StrokeWidthControllerTest, WP1A16NoOpExcludedRejectedAndFailedKeepRedoAndState)
{
    for (bool virgin : {false, true}) for (bool dirty : {false, true}) for (unsigned mode = 0; mode < 5; ++mode) {
        if (virgin && dirty) continue;
        SCOPED_TRACE(mode);
        SCOPED_TRACE(dirty);
        SCOPED_TRACE(virgin);
        auto document = parse(std::string{svg_open} +
            R"(<rect id="a" width="10" height="10" style="stroke:none;stroke-width:2"/>)"
            R"(<rect id="b" width="10" height="10" style="stroke:black;stroke-width:2"/>)"
            R"(<image id="bitmap" width="10" height="10"/></svg>)");
        ASSERT_TRUE(document); settle(*document);
        auto const before = serialize(*document);
        std::string redo_xml;
        auto const seed = SW::prepare_stroke_widths(*document, {item(*document, "a")}, additive(1), 1026);
        wp1_roundtrip(*document, seed, before, [&](auto const &r) {
            ASSERT_EQ(r.state, SW::StrokeWidthApplyState::Applied); redo_xml = serialize(*document);
        });
        document->setModifiedSinceSave(dirty); document->setVirgin(virgin);
        auto const defaults = Preferences::get()->getString("/desktop/style");
        std::vector<SPItem *> roots{item(*document, "a")};
        auto intent = absolute(2);
        if (mode == 1) intent = additive(-2);
        if (mode == 2) { intent = additive(1); roots = {item(*document, "bitmap")}; }
        if (mode == 3) { intent = additive(1); roots.push_back(item(*document, "b")); }
        if (mode == 4) intent = additive(0);
        auto const plan = SW::prepare_stroke_widths(*document, roots, intent, 1027);
        auto token = DocumentUndo::beginAtomicInteraction(document.get()); ASSERT_TRUE(token);
        std::unique_ptr<SingleFireStyleObserver> observer;
        if (mode == 3) observer = std::make_unique<SingleFireStyleObserver>(*roots[0]->getRepr(), [&] {
            item(*document, "b")->setAttribute("x", "999");
        });
        auto const result = SW::apply_stroke_widths_compatible(*document, plan, 1027, *token);
        if (observer) { EXPECT_TRUE(observer->fired()); observer->detach(); }
        EXPECT_EQ(result.state, mode == 3 ? SW::StrokeWidthApplyState::Failed : mode == 4 ? SW::StrokeWidthApplyState::Rejected : SW::StrokeWidthApplyState::Unchanged);
        EXPECT_EQ(result.changed, 0u); EXPECT_EQ(result.paint_none, 0u);
        EXPECT_EQ(result.excluded, mode == 2 ? 1u : 0u);
        EXPECT_EQ(result.skipped_runs, 0u);
        auto const note = mode == 1 ? "Already at the minimum step" :
                          mode == 2 ? "Stroke width: incompatible or protected items were skipped" : "";
        EXPECT_EQ(SW::stroke_width_applied_note(plan, result), note);
        if (mode < 3) EXPECT_EQ(SW::stroke_width_applied_note(plan, result.excluded), note);
        if (mode != 3) {
            EXPECT_EQ(serialize(*document), before);
            EXPECT_EQ(document->isModifiedSinceSave(), dirty);
            EXPECT_EQ(document->getVirgin(), virgin);
        }
        token->rollback();
        EXPECT_EQ(serialize(*document), before);
        EXPECT_EQ(document->isModifiedSinceSave(), dirty); EXPECT_EQ(document->getVirgin(), virgin);
        EXPECT_EQ(Preferences::get()->getString("/desktop/style"), defaults);
        ASSERT_TRUE(DocumentUndo::redo(document.get())); EXPECT_EQ(serialize(*document), redo_xml);
        ASSERT_TRUE(DocumentUndo::undo(document.get())); EXPECT_EQ(serialize(*document), before);
        EXPECT_FALSE(DocumentUndo::undo(document.get())) << "no added history entry";
    }
}

TEST_F(StrokeWidthControllerTest, WP1NotesComposePostwritePaintCloneAndSkippedOwners)
{
    auto document = parse(std::string{svg_open} +
        R"(<rect id="src" width="10" height="10" style="stroke:none;stroke-width:2"/>)"
        R"(<use id="clone" xlink:href="#src" x="20"/>)"
        R"(<text id="text" x="0" y="20" style="font-family:sans-serif;font-size:20px;stroke:none;stroke-width:2"><tspan>AB</tspan><tspan style="font-weight:bold">CD</tspan></text>)"
        R"(<image id="bitmap" width="10" height="10"/></svg>)");
    ASSERT_TRUE(document); settle(*document);
    auto const before = serialize(*document);
    auto const plan = SW::prepare_stroke_widths(*document,
        {item(*document, "src"), item(*document, "clone"), item(*document, "text"), item(*document, "bitmap")}, additive(1), 1028);
    wp1_roundtrip(*document, plan, before, [&](auto const &r) {
        ASSERT_EQ(r.state, SW::StrokeWidthApplyState::Applied); EXPECT_EQ(r.changed, 2u); EXPECT_EQ(r.paint_none, 2u);
        EXPECT_EQ(r.excluded, 2u); EXPECT_EQ(r.skipped_runs, 0u);
        EXPECT_EQ(SW::stroke_width_applied_note(plan, r), "Stroke width applied; 2 objects have no stroke colour; 1 linked clone follows its original; incompatible or protected items were skipped");
        EXPECT_EQ(SW::stroke_width_applied_note(plan, r.excluded), SW::stroke_width_applied_note(plan, r));
    });
    // Removal counts the resulting none paint, even though the input was black,
    // but that requested outcome is not a warning: a plain removal is silent.
    auto painted = parse(std::string{svg_open} + R"(<rect id="r" width="10" height="10" style="stroke:black;stroke-width:2"/></svg>)");
    ASSERT_TRUE(painted); settle(*painted);
    auto const remove = SW::prepare_stroke_widths(*painted, {item(*painted, "r")}, {SW::StrokeWidthIntentKind::RemoveStroke, 0, false}, 1029);
    wp1_roundtrip(*painted, remove, serialize(*painted), [&](auto const &r) {
        ASSERT_EQ(r.state, SW::StrokeWidthApplyState::Applied); EXPECT_EQ(r.paint_none, 1u);
        EXPECT_EQ(SW::stroke_width_applied_note(remove, r), "");
        EXPECT_EQ(SW::stroke_width_applied_note(remove, r.excluded), "");
    });
    // A removal that also has clones and skipped items names the removal.
    auto mixed = parse(std::string{svg_open} +
        R"(<rect id="src" width="10" height="10" style="stroke:black;stroke-width:2"/>)"
        R"(<use id="clone" xlink:href="#src" x="20"/>)"
        R"(<image id="bitmap" width="10" height="10"/></svg>)");
    ASSERT_TRUE(mixed); settle(*mixed);
    auto const remove_mixed = SW::prepare_stroke_widths(*mixed,
        {item(*mixed, "src"), item(*mixed, "clone"), item(*mixed, "bitmap")}, {SW::StrokeWidthIntentKind::RemoveStroke, 0, false}, 1030);
    wp1_roundtrip(*mixed, remove_mixed, serialize(*mixed), [&](auto const &r) {
        ASSERT_EQ(r.state, SW::StrokeWidthApplyState::Applied); EXPECT_EQ(r.paint_none, 1u);
        EXPECT_EQ(SW::stroke_width_applied_note(remove_mixed, r),
                  "Stroke removed; 1 linked clone follows its original; incompatible or protected items were skipped");
        EXPECT_EQ(SW::stroke_width_applied_note(remove_mixed, r.excluded), SW::stroke_width_applied_note(remove_mixed, r));
    });
}

TEST_F(StrokeWidthControllerTest, WP1NotesIgnoreNonePaintOnUnchangedTextRuns)
{
    for (auto intent : {absolute(2), additive(-2)}) {
        SCOPED_TRACE(static_cast<int>(intent.kind));
        auto document = parse(std::string{svg_open} +
            R"(<text id="t" x="0" y="20" style="font-family:sans-serif;font-size:20px;stroke:black">)"
            R"(<tspan style="stroke:none;stroke-width:2">A</tspan>)"
            R"(<tspan style="stroke-width:5">B</tspan></text></svg>)");
        ASSERT_TRUE(document); settle(*document);
        auto const before = serialize(*document);
        auto *owner = item(*document, "t");
        auto const plan = SW::prepare_stroke_widths(*document, {owner}, intent, 1038);
        ASSERT_EQ(plan.members.size(), 1u);
        ASSERT_EQ(plan.members[0].text_runs.size(), 2u);
        EXPECT_EQ(plan.members[0].text_runs[0].outcome, SW::StrokeWidthMemberOutcome::Unchanged);
        EXPECT_EQ(plan.members[0].text_runs[1].outcome, SW::StrokeWidthMemberOutcome::Change);
        wp1_roundtrip(*document, plan, before, [&](auto const &r) {
            ASSERT_EQ(r.state, SW::StrokeWidthApplyState::Applied);
            EXPECT_EQ(r.paint_none, 0u);
            EXPECT_EQ(SW::stroke_width_applied_note(plan, r), "");
            EXPECT_EQ(SW::stroke_width_applied_note(plan, r.excluded), "");
            EXPECT_DOUBLE_EQ(native_char_widths(*document, owner, 0).local, 2);
            EXPECT_DOUBLE_EQ(native_char_widths(*document, owner, 1).local,
                             intent.kind == SW::StrokeWidthIntentKind::AbsoluteCssPx ? 2 : 3);
        });
        // All eligible runs at the floor explain the no-op for text too.
        auto const floor = SW::prepare_stroke_widths(*document, {owner}, additive(-3), 1039);
        wp1_roundtrip(*document, floor, before, [&](auto const &r) {
            EXPECT_EQ(r.state, SW::StrokeWidthApplyState::Unchanged);
            EXPECT_EQ(r.attempted_writes, 0u); EXPECT_EQ(r.paint_none, 0u);
            EXPECT_EQ(SW::stroke_width_applied_note(floor, r), "Already at the minimum step");
            EXPECT_EQ(SW::stroke_width_applied_note(floor, r.excluded), "Already at the minimum step");
        });
    }
}

TEST_F(StrokeWidthControllerTest, WP1PureHelpersEveryUnitsXmlUnit)
{
    auto const path = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() / "share/ui/units.xml";
    Util::UnitTable units(path.string());
    std::map<std::string, double> const steps{{"px",0.1}, {"pt",0.1}, {"pc",0.01}, {"mm",0.05}, {"cm",0.005}, {"in",0.001}, {"m",0.00005}, {"ft",0.0001}};
    unsigned tested = 0;
    for (auto type : {Util::UNIT_TYPE_LINEAR, Util::UNIT_TYPE_RADIAL, Util::UNIT_TYPE_FONT_HEIGHT, Util::UNIT_TYPE_DIMENSIONLESS}) {
        for (auto unit : units.units(type)) {
            SCOPED_TRACE(unit->abbr.raw()); ++tested;
            auto const expected = steps.find(unit->abbr.raw());
            EXPECT_DOUBLE_EQ(SW::stroke_width_step(*unit), expected == steps.end() ? 0 : expected->second);
            EXPECT_EQ(SW::stroke_width_digits(*unit), unit->abbr == "m" ? 5u : unit->abbr == "ft" ? 6u : 3u);
            EXPECT_EQ(SW::stroke_width_list_unit(*unit)->abbr, unit->abbr == "mm" || unit->abbr == "cm" || unit->abbr == "m" ? "mm" : "pt");
        }
    }
    EXPECT_EQ(tested, 16u);
}

TEST_F(StrokeWidthControllerTest, WP1PureHelpersBothPresetDescriptorLists)
{
    struct Entry { char const *label; double value; };
    std::vector<Entry> const mm{{"0.1",0.1},{"0.2",0.2},{"0.25",0.25},{"0.35",0.35},{"0.5",0.5},{"0.75",0.75},{"1",1},{"1.5",1.5},{"2",2},{"2.5",2.5},{"3",3}};
    std::vector<Entry> const pt{{"0.25",0.25},{"0.5",0.5},{"0.75",0.75},{"1",1},{"1.5",1.5},{"2",2},{"3",3},{"4",4},{"6",6},{"8",8},{"10",10},{"12",12}};
    for (auto unit : {"mm", "pt"}) {
        auto const descriptors = SW::stroke_width_presets(*Util::UnitTable::get().getUnit(unit));
        auto const &entries = std::string(unit) == "mm" ? mm : pt;
        ASSERT_EQ(descriptors.size(), entries.size() + 1);
        EXPECT_EQ(descriptors[0].label, "Hairline"); EXPECT_EQ(descriptors[0].kind, SW::StrokeWidthPresetKind::Hairline);
        EXPECT_DOUBLE_EQ(SW::stroke_width_preset_px(descriptors[0]), 0);
        for (unsigned i = 0; i < entries.size(); ++i) {
            auto const &preset = descriptors[i + 1];
            EXPECT_EQ(preset.label, entries[i].label); EXPECT_EQ(preset.unit->abbr, unit);
            EXPECT_DOUBLE_EQ(preset.value, entries[i].value); EXPECT_EQ(preset.kind, SW::StrokeWidthPresetKind::Absolute);
            EXPECT_NEAR(SW::stroke_width_preset_px(preset), entries[i].value * (std::string(unit) == "mm" ? 96/25.4 : 96.0/72), 1e-12);
        }
    }
}

TEST_F(StrokeWidthControllerTest, WP1PureHelpersSameWidthAndFloor)
{
    EXPECT_TRUE(SW::stroke_width_same_width(0, 0.001));
    EXPECT_FALSE(SW::stroke_width_same_width(0, 0.00101));
    EXPECT_TRUE(SW::stroke_width_same_width(1e9, 1e9 + 0.2)); // existing relative equality
    EXPECT_FALSE(SW::stroke_width_same_width(1e9, 1e9 + 2));
    EXPECT_FALSE(SW::stroke_width_same_width(std::numeric_limits<double>::quiet_NaN(), 1));
    EXPECT_FALSE(SW::stroke_width_can_decrease(0, 1)); EXPECT_FALSE(SW::stroke_width_can_decrease(1, 1));
    EXPECT_FALSE(SW::stroke_width_can_decrease(1.5, 1)); EXPECT_TRUE(SW::stroke_width_can_decrease(2, 1));
    EXPECT_TRUE(SW::stroke_width_can_decrease(2 - 0.5e-9, 1)); EXPECT_FALSE(SW::stroke_width_can_decrease(2 - 4e-9, 1));
    for (double invalid : {0.0, -1.0, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()}) EXPECT_FALSE(SW::stroke_width_can_decrease(2, invalid));
    EXPECT_FALSE(SW::stroke_width_can_decrease(std::numeric_limits<double>::infinity(), 1));
    EXPECT_FALSE(SW::stroke_width_can_decrease(-1, 1));
}

TEST_F(StrokeWidthControllerTest, WP1A10FiniteTinyScaleWouldOverflowLocalIsExcludedEarly)
{
    ASSERT_TRUE(std::isinf(1e6 / 1e-310));
    auto document = parse(std::string{svg_open} +
        R"(<rect id="good" width="10" height="10" style="stroke:black;stroke-width:2"/>)"
        R"svg(<rect id="overflow" width="10" height="10" transform="scale(1e-310)" style="stroke:black;stroke-width:0"/>)svg"
        R"(</svg>)");
    ASSERT_TRUE(document); settle(*document);
    auto const before = serialize(*document);
    auto const plan = SW::prepare_stroke_widths(*document,
        {item(*document, "good"), item(*document, "overflow")}, additive(1e6), 1030);
    // The finite matrix's determinant underflows, so the shared transform
    // gate excludes it before the overflowing local division is attempted.
    EXPECT_TRUE(find_excluded(plan.query, SW::StrokeWidthExclusion::SingularTransform));
    wp1_roundtrip(*document, plan, before, [&](auto const &r) {
        ASSERT_EQ(r.state, SW::StrokeWidthApplyState::Applied); EXPECT_EQ(r.changed, 1u); EXPECT_EQ(r.excluded, 1u);
        wp1_width(*document, "good", 1000002, 1000002);
        EXPECT_DOUBLE_EQ(computed_width(*document, "overflow"), 0);
    });
}

TEST_F(StrokeWidthControllerTest, WP1A12UnsafeDashExcludesOnlyItsOwner)
{
    auto document = parse(std::string{svg_open} +
        R"(<rect id="good" width="10" height="10" style="stroke:black;stroke-width:1"/>)"
        R"(<rect id="unsafe" width="10" height="10" style="stroke:black;stroke-width:1;stroke-dasharray:4 2;stroke-dashoffset:1e307"/></svg>)");
    ASSERT_TRUE(document); settle(*document);
    // The native CSS parser caps exponents at 38. Isolate unsafe arithmetic
    // using the public computed style, as the existing effective-width tests do.
    item(*document, "unsafe")->style->stroke_dashoffset.computed = 1e307;
    ASSERT_DOUBLE_EQ(item(*document, "unsafe")->style->stroke_dashoffset.computed, 1e307);
    auto const before = serialize(*document);
    auto const unsafe_xml = sp_repr_write_buf(item(*document, "unsafe")->getRepr(), 0, false, GQuark(0), 0, 0).raw();
    auto const plan = SW::prepare_stroke_widths(*document, {item(*document, "good"), item(*document, "unsafe")}, additive(20, true), 1031);
    auto const *unsafe = find_member(plan, item(*document, "unsafe")); ASSERT_TRUE(unsafe);
    EXPECT_EQ(unsafe->outcome, SW::StrokeWidthMemberOutcome::Excluded); EXPECT_EQ(unsafe->reason, SW::StrokeWidthMemberReason::InvalidDash);
    wp1_roundtrip(*document, plan, before, [&](auto const &r) {
        ASSERT_EQ(r.state, SW::StrokeWidthApplyState::Applied); EXPECT_EQ(r.changed, 1u); EXPECT_EQ(r.excluded, 1u);
        wp1_width(*document, "good", 21, 21);
        EXPECT_EQ(sp_repr_write_buf(item(*document, "unsafe")->getRepr(), 0, false, GQuark(0), 0, 0).raw(), unsafe_xml);
    });
}

TEST_F(StrokeWidthControllerTest, WP1A08BrokenCloneGraphRejectsOnlyWithChanges)
{
    auto document = parse(std::string{svg_open} + R"(<rect id="r" width="10" height="10" style="stroke:black;stroke-width:2"/><use id="broken" xlink:href="#missing"/></svg>)");
    ASSERT_TRUE(document); settle(*document);
    auto const before = serialize(*document);
    auto roots = std::vector<SPItem *>{item(*document, "r"), item(*document, "broken")};
    auto const rejected = SW::prepare_stroke_widths(*document, roots, additive(1), 1032);
    EXPECT_EQ(rejected.rejection, SW::StrokeWidthMemberReason::DependentCloneUncertain);
    wp1_roundtrip(*document, rejected, before, [&](auto const &r) {
        EXPECT_EQ(r.state, SW::StrokeWidthApplyState::Rejected); EXPECT_EQ(r.attempted_writes, 0u);
    });
    auto const floor = SW::prepare_stroke_widths(*document, roots, additive(-2), 1033);
    wp1_roundtrip(*document, floor, before, [&](auto const &r) {
        EXPECT_EQ(r.state, SW::StrokeWidthApplyState::Unchanged); EXPECT_EQ(r.unchanged, 1u); EXPECT_EQ(r.excluded, 1u);
    });
}

TEST_F(StrokeWidthControllerTest, WP1A14EmptyCaretKeepsDefaultsAndRedo)
{
    auto document = parse(std::string{svg_open} + R"(<rect id="r" width="10" height="10" style="stroke:black;stroke-width:2"/><text id="empty" style="stroke:black;stroke-width:2"/></svg>)");
    ASSERT_TRUE(document); settle(*document);
    auto const before = serialize(*document);
    auto const seed = SW::prepare_stroke_widths(*document, {item(*document, "r")}, additive(1), 1034);
    std::string redo;
    wp1_roundtrip(*document, seed, before, [&](auto const &r) {
        ASSERT_EQ(r.state, SW::StrokeWidthApplyState::Applied); redo = serialize(*document);
    });
    document->setModifiedSinceSave(false); document->setVirgin(true);
    auto const defaults = Preferences::get()->getString("/desktop/style");
    SW::StrokeWidthTextRange range; range.owner = item(*document, "empty"); range.caret = true;
    auto const plan = SW::prepare_stroke_widths_combined(*document, range, {range.owner.get()}, additive(1), 1035);
    ASSERT_EQ(plan.query.excluded.size(), 1u);
    EXPECT_EQ(plan.query.excluded[0].exclusion, SW::StrokeWidthExclusion::EmptyText);
    EXPECT_TRUE(plan.query.excluded[0].caret_scope);
    wp1_roundtrip(*document, plan, before, [&](auto const &r) {
        EXPECT_EQ(r.state, SW::StrokeWidthApplyState::Unchanged); EXPECT_EQ(r.excluded, 1u);
        EXPECT_EQ(SW::stroke_width_applied_note(plan, r), "Stroke width: incompatible or protected items were skipped");
    });
    EXPECT_EQ(Preferences::get()->getString("/desktop/style"), defaults);
    EXPECT_FALSE(document->isModifiedSinceSave()); EXPECT_TRUE(document->getVirgin());
    ASSERT_TRUE(DocumentUndo::redo(document.get())); EXPECT_EQ(serialize(*document), redo);
}

TEST_F(StrokeWidthControllerTest, WP1A17NegativeBoundaryAndInvalidRangeIntent)
{
    auto document = parse(std::string{svg_open} + R"(<rect id="r" width="10" height="10" style="stroke:black;stroke-width:2000000"/></svg>)");
    ASSERT_TRUE(document); settle(*document);
    auto const before = serialize(*document);
    auto const valid = SW::prepare_stroke_widths(*document, {item(*document, "r")}, additive(-1e6), 1036);
    wp1_roundtrip(*document, valid, before, [&](auto const &r) {
        ASSERT_EQ(r.state, SW::StrokeWidthApplyState::Applied); wp1_width(*document, "r", 1e6, 1e6);
    });
    SW::StrokeWidthTextRange invalid_range;
    auto const invalid = SW::prepare_stroke_widths_combined(*document, invalid_range, {item(*document, "r")}, additive(0), 1037);
    EXPECT_EQ(invalid.state, SW::StrokeWidthPlanState::Rejected); EXPECT_EQ(invalid.rejection, SW::StrokeWidthMemberReason::InvalidIntent);
    EXPECT_EQ(serialize(*document), before);
}

// ---------------------------------------------------------------------------
// WP1c: source-and-clone verification and linear-time text writes.
// Complexity is asserted through the deterministic work counters of the apply
// result, never through wall-clock time.
// ---------------------------------------------------------------------------

namespace {

class StrokeWidthWp1cTest : public StrokeWidthControllerTest
{
protected:
    static std::vector<SPItem *> top_items(SPDocument &document)
    {
        std::vector<SPItem *> roots;
        for (auto &child : document.getRoot()->children) {
            if (auto *entry = cast<SPItem>(&child)) roots.push_back(entry);
        }
        return roots;
    }

    /// Prepare + apply against a fresh token, then roll back. Returns the plan and result.
    static std::pair<SW::StrokeWidthPlan, SW::StrokeWidthApplyResult> prepare_apply_rollback(
        SPDocument &document, std::vector<SPItem *> const &roots, SW::StrokeWidthIntent const &intent)
    {
        auto plan = SW::prepare_stroke_widths(document, roots, intent, 9100);
        EXPECT_EQ(plan.state, SW::StrokeWidthPlanState::Prepared);
        auto token = DocumentUndo::beginAtomicInteraction(&document);
        EXPECT_TRUE(token);
        if (!token) return {std::move(plan), {}};
        auto result = SW::apply_stroke_widths_compatible(document, plan, 9100, *token);
        token->rollback();
        return {std::move(plan), result};
    }

    static std::string text_doc(int n)
    {
        std::string s = std::string{svg_open};
        for (int i = 0; i < n; ++i) {
            s += "<text id=\"t" + std::to_string(i) + "\" x=\"" + std::to_string(i % 100) + ".5000\" y=\"" +
                 std::to_string(i / 50) + "\" xml:space=\"preserve\"><tspan x=\"" + std::to_string(i % 100) +
                 ".5000\" font-family=\"Arial\" font-size=\"4.0000\" fill=\"#000066\">Ab " + std::to_string(i) +
                 "</tspan></text>";
        }
        return s + "</svg>";
    }

    /// One text whose tspans carry only a style attribute (stroke-width 1 + i % 5) and two
    /// characters each: "w<digit>". An optional tspan index gets a nested child tspan.
    static std::string longtext_doc(int n, int nested_at = -1)
    {
        std::string s = std::string{svg_open} +
            "<text id=\"long\" x=\"10\" y=\"20\" xml:space=\"preserve\" "
            "style=\"font-family:Arial;font-size:4px;stroke:black\">";
        for (int i = 0; i < n; ++i) {
            s += "<tspan style=\"stroke-width:" + std::to_string(1 + i % 5) + "\">w" + std::to_string(i % 10);
            if (i == nested_at) s += "<tspan style=\"stroke-width:4\">xy</tspan>";
            s += "</tspan>";
        }
        return s + "</text></svg>";
    }

    static std::vector<double> local_widths(SPDocument &document, SPItem *owner)
    {
        auto const query = SW::query_stroke_widths(document, {owner});
        std::vector<double> widths;
        EXPECT_EQ(query.targets.size(), 1u);
        if (query.targets.size() != 1) return widths;
        for (auto const &character : query.targets[0].char_baseline) {
            widths.push_back(character.style.local_computed);
        }
        return widths;
    }

    static std::string unset_clone_fixture(std::string const &source_style, std::string const &clone_extra = {})
    {
        return std::string{svg_open} +
            R"(<rect id="src" width="10" height="10" style=")" + source_style + R"("/>)" +
            R"(<use id="u0" xlink:href="#src" x="20"/>)" +
            R"(<use id="u1" xlink:href="#src" x="40" )" + clone_extra + R"(/>)" +
            R"(<use id="u2" xlink:href="#src" x="60"/>)" +
            R"(</svg>)";
    }

    /// Every selected clone keeps its exact authored attributes and inline style.
    void expect_clones_untouched(SPDocument &document, std::vector<std::string> const &ids,
                                 std::map<std::string, std::vector<std::pair<std::string, std::string>>> const &attributes,
                                 std::map<std::string, std::map<std::string, std::string>> const &styles)
    {
        for (auto const &id : ids) {
            auto *clone = item(document, id.c_str());
            ASSERT_TRUE(clone) << id;
            EXPECT_EQ(authored_attributes(clone), attributes.at(id)) << id;
            EXPECT_EQ(inline_style_map(clone), styles.at(id)) << id;
        }
    }

    /// Roundtrip a source with `n` clones (three named ones plus extras), checking the source width
    /// and that no clone was written.
    void clone_case(std::string const &svg, std::vector<std::string> const &clone_ids,
                    SW::StrokeWidthIntent const &intent, double expected_source_width,
                    std::size_t expected_following, bool expect_none_stroke = false)
    {
        auto document = parse(svg);
        ASSERT_TRUE(document);
        settle(*document);
        auto const before = serialize(*document);
        std::map<std::string, std::vector<std::pair<std::string, std::string>>> attributes;
        std::map<std::string, std::map<std::string, std::string>> styles;
        std::vector<SPItem *> roots{item(*document, "src")};
        for (auto const &id : clone_ids) {
            attributes[id] = authored_attributes(item(*document, id.c_str()));
            styles[id] = inline_style_map(item(*document, id.c_str()));
            roots.push_back(item(*document, id.c_str()));
        }
        auto const plan = SW::prepare_stroke_widths(*document, roots, intent, 9101);
        ASSERT_EQ(plan.state, SW::StrokeWidthPlanState::Prepared);
        ASSERT_EQ(plan.planned_changes, 1u);
        ASSERT_EQ(plan.following_clones, expected_following);
        wp1_roundtrip(*document, plan, before, [&](SW::StrokeWidthApplyResult const &result) {
            ASSERT_EQ(result.state, SW::StrokeWidthApplyState::Applied)
                << "reason " << static_cast<int>(result.reason) << " attempted " << result.attempted_writes;
            EXPECT_EQ(result.changed, 1u);
            EXPECT_EQ(result.attempted_writes, 1u);
            EXPECT_EQ(result.work.char_captures, 0u);
            if (expect_none_stroke) {
                EXPECT_TRUE(item(*document, "src")->style->stroke.isNone());
            } else {
                EXPECT_DOUBLE_EQ(computed_width(*document, "src"), expected_source_width);
            }
            expect_clones_untouched(*document, clone_ids, attributes, styles);
        });
    }
};

} // namespace

// The clone class after the write, not the frozen one: a source with an UNSET width gains an
// explicit width, so its clones move from "follows an unset source" to CloneSourceOverrides.
TEST_F(StrokeWidthWp1cTest, WP1cUnsetSourceWidthAndClonesAbsoluteAndAdditive)
{
    // Absolute 3 sets 3; additive +1.5 from the unset default width 1 gives 2.5.
    clone_case(unset_clone_fixture("fill:red;stroke:black"), {"u0", "u1", "u2"}, absolute(3.0), 3.0, 3u);
    clone_case(unset_clone_fixture("fill:red;stroke:black"), {"u0", "u1", "u2"}, additive(1.5), 2.5, 3u);
}

TEST_F(StrokeWidthWp1cTest, WP1cExplicitSourceWidthClonesUnchangedBehaviour)
{
    clone_case(unset_clone_fixture("fill:red;stroke:black;stroke-width:2"), {"u0", "u1", "u2"}, absolute(3.0), 3.0, 3u);
}

TEST_F(StrokeWidthWp1cTest, WP1cCloneOfCloneFollowsAndNestedCloneStaysUnsupported)
{
    auto const svg = std::string{svg_open} +
        R"(<rect id="src" width="10" height="10" style="fill:red;stroke:black"/>)"
        R"(<use id="u0" xlink:href="#src" x="20"/>)"
        R"(<use id="u1" xlink:href="#u0" x="40"/>)"
        R"(</svg>)";
    auto document = parse(svg);
    ASSERT_TRUE(document);
    settle(*document);
    auto const before = serialize(*document);
    auto const attributes_u1 = authored_attributes(item(*document, "u1"));
    auto const plan = SW::prepare_stroke_widths(
        *document, {item(*document, "src"), item(*document, "u0"), item(*document, "u1")}, absolute(3.0), 9102);
    ASSERT_EQ(plan.state, SW::StrokeWidthPlanState::Prepared);
    EXPECT_EQ(plan.following_clones, 2u);
    ASSERT_TRUE(find_excluded(plan.query, SW::StrokeWidthExclusion::CloneUnsupportedChild));
    wp1_roundtrip(*document, plan, before, [&](SW::StrokeWidthApplyResult const &result) {
        ASSERT_EQ(result.state, SW::StrokeWidthApplyState::Applied);
        EXPECT_EQ(authored_attributes(item(*document, "u1")), attributes_u1);
        EXPECT_DOUBLE_EQ(computed_width(*document, "src"), 3.0);
        // The nested clone keeps its unsupported record after the write.
        auto const fresh = SW::query_stroke_widths(
            *document, {item(*document, "src"), item(*document, "u0"), item(*document, "u1")});
        EXPECT_TRUE(find_excluded(fresh, SW::StrokeWidthExclusion::CloneUnsupportedChild));
        EXPECT_TRUE(find_excluded(fresh, SW::StrokeWidthExclusion::CloneSourceOverrides));
    });
}

TEST_F(StrokeWidthWp1cTest, WP1cCloneWithOwnStyleKeepsItVerbatim)
{
    clone_case(unset_clone_fixture("fill:red;stroke:black", R"(style="stroke-width:5;opacity:0.5")"),
               {"u0", "u1", "u2"}, absolute(3.0), 3.0, 3u);
}

TEST_F(StrokeWidthWp1cTest, WP1cProtectedCloneOfUnsetSourceIsPreserved)
{
    clone_case(unset_clone_fixture("fill:red;stroke:black", R"(sodipodi:insensitive="true")"),
               {"u0", "u1", "u2"}, absolute(3.0), 3.0, 3u);
}

TEST_F(StrokeWidthWp1cTest, WP1cRemoveStrokeOnUnsetSourceWithClones)
{
    SW::StrokeWidthIntent intent;
    intent.kind = SW::StrokeWidthIntentKind::RemoveStroke;
    clone_case(unset_clone_fixture("fill:red;stroke:black"), {"u0", "u1", "u2"}, intent, 0.0, 3u, true);
}

// Strictness: only clones following a changed source may change class. An observer that flips
// another selected clone (whose source was not changed) during the write must still fail.
TEST_F(StrokeWidthWp1cTest, WP1cNegativeClassFlipOfAnUnrelatedCloneStillFails)
{
    auto document = parse(std::string{svg_open} +
        R"(<rect id="src" width="10" height="10" style="fill:red;stroke:black"/>)"
        R"(<rect id="src2" width="10" height="10" style="fill:red;stroke:black"/>)"
        R"(<use id="c" xlink:href="#src" x="20"/>)"
        R"(<use id="c2" xlink:href="#src2" x="40"/>)"
        R"(</svg>)");
    ASSERT_TRUE(document);
    settle(*document);
    auto const before = serialize(*document);
    auto const plan = SW::prepare_stroke_widths(
        *document, {item(*document, "src"), item(*document, "c"), item(*document, "c2")}, absolute(3.0), 9103);
    ASSERT_EQ(plan.state, SW::StrokeWidthPlanState::Prepared);
    ASSERT_EQ(plan.following_clones, 1u);
    auto token = DocumentUndo::beginAtomicInteraction(document.get());
    ASSERT_TRUE(token);
    SingleFireStyleObserver observer(*item(*document, "src")->getRepr(), [&] {
        item(*document, "src2")->setAttribute("style", "fill:red;stroke:black;stroke-width:9");
    });
    auto const result = SW::apply_stroke_widths_compatible(*document, plan, 9103, *token);
    EXPECT_TRUE(observer.fired());
    EXPECT_EQ(result.state, SW::StrokeWidthApplyState::Failed);
    EXPECT_EQ(result.reason, SW::StrokeWidthMemberReason::PostconditionMismatch);
    EXPECT_EQ(result.changed, 0u);
    observer.detach();
    token->rollback();
    EXPECT_EQ(serialize(*document), before);
}

// A following clone's own attributes are still verified: a callback that protects it fails.
TEST_F(StrokeWidthWp1cTest, WP1cNegativeFollowingCloneAttributeChangeStillFails)
{
    auto document = parse(unset_clone_fixture("fill:red;stroke:black"));
    ASSERT_TRUE(document);
    settle(*document);
    auto const before = serialize(*document);
    auto const plan = SW::prepare_stroke_widths(
        *document, {item(*document, "src"), item(*document, "u0"), item(*document, "u1")}, absolute(3.0), 9104);
    ASSERT_EQ(plan.state, SW::StrokeWidthPlanState::Prepared);
    auto token = DocumentUndo::beginAtomicInteraction(document.get());
    ASSERT_TRUE(token);
    SingleFireStyleObserver observer(*item(*document, "src")->getRepr(), [&] {
        item(*document, "u1")->setAttribute("sodipodi:insensitive", "true");
    });
    auto const result = SW::apply_stroke_widths_compatible(*document, plan, 9104, *token);
    EXPECT_TRUE(observer.fired());
    EXPECT_EQ(result.state, SW::StrokeWidthApplyState::Failed);
    EXPECT_EQ(result.reason, SW::StrokeWidthMemberReason::PostconditionMismatch);
    observer.detach();
    token->rollback();
    EXPECT_EQ(serialize(*document), before);
}

TEST_F(StrokeWidthWp1cTest, WP1cFiveHundredClonesOfAnUnsetSourceApply)
{
    std::string svg = std::string{svg_open} +
        R"(<rect id="src" width="5" height="5" style="fill:red;stroke:black"/>)";
    for (int i = 0; i < 500; ++i) {
        svg += "<use id=\"u" + std::to_string(i) + "\" xlink:href=\"#src\" x=\"" + std::to_string(i % 100 * 6) +
               "\" y=\"" + std::to_string(i / 100 * 6) + "\"/>";
    }
    svg += "</svg>";
    for (bool additive_intent : {false, true}) {
        auto document = parse(svg);
        ASSERT_TRUE(document);
        settle(*document);
        auto const before = serialize(*document);
        auto const roots = top_items(*document);
        auto const plan = SW::prepare_stroke_widths(*document, roots, additive_intent ? additive(1.5) : absolute(3.0), 9105);
        ASSERT_EQ(plan.state, SW::StrokeWidthPlanState::Prepared);
        EXPECT_EQ(plan.following_clones, 500u);
        wp1_roundtrip(*document, plan, before, [&](SW::StrokeWidthApplyResult const &result) {
            ASSERT_EQ(result.state, SW::StrokeWidthApplyState::Applied);
            EXPECT_EQ(result.changed, 1u);
            EXPECT_EQ(result.attempted_writes, 1u);
            EXPECT_DOUBLE_EQ(computed_width(*document, "src"), additive_intent ? 2.5 : 3.0);
        });
    }
}

// Text: the between-write scan must not re-scan every pending owner before every write.
TEST_F(StrokeWidthWp1cTest, WP1cTextPathIsLinearInOwners)
{
    std::size_t checks[2] = {0, 0};
    std::size_t captures[2] = {0, 0};
    int const counts[2] = {64, 256};
    for (int k = 0; k < 2; ++k) {
        auto document = parse(text_doc(counts[k]));
        ASSERT_TRUE(document);
        settle(*document);
        auto const [plan, result] = prepare_apply_rollback(*document, top_items(*document), absolute(2.5));
        ASSERT_EQ(result.state, SW::StrokeWidthApplyState::Applied);
        ASSERT_EQ(result.changed, static_cast<std::size_t>(counts[k]));
        checks[k] = result.work.pending_owner_checks;
        captures[k] = result.work.char_captures;
    }
    EXPECT_LE(checks[1], 4u * 256u) << "pending scan grew with owners x pending owners";
    EXPECT_LE(checks[1], 5u * std::max<std::size_t>(checks[0], 1u));
    EXPECT_LE(captures[1], 5u * captures[0]);
}

// Runs of one owner that share one target are written by one native call.
TEST_F(StrokeWidthWp1cTest, WP1cUniformRunsOfOneOwnerKeepSourceWrites)
{
    auto document = parse(longtext_doc(60));
    ASSERT_TRUE(document);
    settle(*document);
    auto *text = item(*document, "long");
    ASSERT_TRUE(text);
    auto const before = serialize(*document);
    auto const content = std::string(sp_te_get_string_multiline(text).raw());
    auto const plan = SW::prepare_stroke_widths(*document, {text}, absolute(2.5), 9106);
    ASSERT_EQ(plan.state, SW::StrokeWidthPlanState::Prepared);
    wp1_roundtrip(*document, plan, before, [&](SW::StrokeWidthApplyResult const &result) {
        ASSERT_EQ(result.state, SW::StrokeWidthApplyState::Applied);
        EXPECT_EQ(result.work.native_range_writes, 0u);
        EXPECT_EQ(result.work.direct_source_writes, 60u);
        EXPECT_EQ(result.attempted_writes, 60u);
        auto *owner = item(*document, "long");
        EXPECT_EQ(std::string(sp_te_get_string_multiline(owner).raw()), content);
        auto const widths = local_widths(*document, owner);
        ASSERT_EQ(widths.size(), 120u);
        for (double width : widths) EXPECT_DOUBLE_EQ(width, 2.5);
    });
}

// Distinct per-run targets: work per run must not depend on the run count of the owner.
TEST_F(StrokeWidthWp1cTest, WP1cManyDistinctRunsAreLinearAndRenderCorrectly)
{
    std::size_t captures[2] = {0, 0};
    int const counts[2] = {50, 200};
    for (int k = 0; k < 2; ++k) {
        auto document = parse(longtext_doc(counts[k]));
        ASSERT_TRUE(document);
        settle(*document);
        auto *text = item(*document, "long");
        ASSERT_TRUE(text);
        auto const before = serialize(*document);
        auto const plan = SW::prepare_stroke_widths(*document, {text}, additive(0.5), 9107);
        ASSERT_EQ(plan.state, SW::StrokeWidthPlanState::Prepared);
        int const n = counts[k];
        wp1_roundtrip(*document, plan, before, [&](SW::StrokeWidthApplyResult const &result) {
            ASSERT_EQ(result.state, SW::StrokeWidthApplyState::Applied);
            captures[k] = result.work.char_captures;
            EXPECT_LE(result.work.layout_rebuilds, 3u);
            EXPECT_EQ(result.work.native_range_writes, 0u) << "large owners write complete spans directly";
            // The span map is built once per owner: one lookup per character plus one per checked
            // character. Rebuilding it per write would need n times as many.
            EXPECT_GT(result.work.source_lookups, 0u);
            EXPECT_LE(result.work.source_lookups, 3u * 2u * static_cast<std::size_t>(n))
                << "source-map work grew with runs x characters";
            auto const widths = local_widths(*document, item(*document, "long"));
            ASSERT_EQ(widths.size(), static_cast<std::size_t>(2 * n));
            for (int i = 0; i < n; ++i) {
                double const expected = 1 + i % 5 + 0.5;
                EXPECT_DOUBLE_EQ(widths[2 * i], expected) << "run " << i;
                EXPECT_DOUBLE_EQ(widths[2 * i + 1], expected) << "run " << i;
            }
        });
    }
    EXPECT_LE(captures[1], 5u * captures[0]) << "per-run work grew with the run count";
}

// Whole owners keep their source identity even below the partial-range bulk threshold.
TEST_F(StrokeWidthWp1cTest, WP1cSmallOwnerUsesDirectSourceWrites)
{
    auto document = parse(longtext_doc(8));
    ASSERT_TRUE(document);
    settle(*document);
    auto const before = serialize(*document);
    auto const plan = SW::prepare_stroke_widths(*document, {item(*document, "long")}, additive(0.5), 9108);
    ASSERT_EQ(plan.state, SW::StrokeWidthPlanState::Prepared);
    wp1_roundtrip(*document, plan, before, [&](SW::StrokeWidthApplyResult const &result) {
        ASSERT_EQ(result.state, SW::StrokeWidthApplyState::Applied);
        EXPECT_EQ(result.work.native_range_writes, 0u);
        EXPECT_EQ(result.work.direct_source_writes, 8u);
        EXPECT_EQ(result.attempted_writes, 8u);
    });
}

// A bounded rendering parent and its flat child each receive their frozen direct patch.
TEST_F(StrokeWidthWp1cTest, WP1cOneLevelRenderingParentUsesOnlyDirectWrites)
{
    auto document = parse(longtext_doc(20, 10));
    ASSERT_TRUE(document); settle(*document);
    auto const before = serialize(*document);
    auto const plan = SW::prepare_stroke_widths(*document, {item(*document, "long")}, additive(0.5), 9109);
    wp1_roundtrip(*document, plan, before, [&](auto const &result) {
        ASSERT_EQ(result.state, SW::StrokeWidthApplyState::Applied);
        EXPECT_EQ(result.work.native_range_writes, 0u);
        EXPECT_EQ(result.work.direct_source_writes, 21u);
        auto widths = local_widths(*document, item(*document, "long"));
        ASSERT_EQ(widths.size(), 42u);
        unsigned index = 0;
        for (int i = 0; i < 20; ++i) {
            EXPECT_DOUBLE_EQ(widths[index++], 1 + i % 5 + 0.5);
            EXPECT_DOUBLE_EQ(widths[index++], 1 + i % 5 + 0.5);
            if (i == 10) { EXPECT_DOUBLE_EQ(widths[index++], 4.5); EXPECT_DOUBLE_EQ(widths[index++], 4.5); }
        }
    });
}

// The pending-owner watch: a callback that changes a still-pending owner's ancestors or
// provenance during the first owner's write must still refuse the next write.
TEST_F(StrokeWidthWp1cTest, WP1cPendingOwnerAncestorMutationStillRefused)
{
    struct Mutation { char const *name; char const *attribute; char const *value; };
    for (auto const &mutation : {Mutation{"lock the group", "sodipodi:insensitive", "true"},
                                 Mutation{"move the group", "transform", "translate(5,5)"}}) {
        auto document = parse(std::string{svg_open} +
            R"svg(<g id="ga"><text id="ta" x="10" y="20" style="font-family:Arial;font-size:20px;stroke:black;fill:none">)svg"
            R"svg(<tspan id="sa" style="stroke-width:2">AB</tspan></text></g>)svg"
            R"svg(<g id="gb"><text id="tb" x="10" y="60" style="font-family:Arial;font-size:20px;stroke:black;fill:none">)svg"
            R"svg(<tspan id="sb" style="stroke-width:3">CD</tspan></text></g>)svg"
            R"(</svg>)");
        ASSERT_TRUE(document) << mutation.name;
        settle(*document);
        auto const before = serialize(*document);
        auto const plan = SW::prepare_stroke_widths(
            *document, {item(*document, "ta"), item(*document, "tb")}, absolute(10.0), 9110);
        ASSERT_EQ(plan.state, SW::StrokeWidthPlanState::Prepared) << mutation.name;
        auto token = DocumentUndo::beginAtomicInteraction(document.get());
        ASSERT_TRUE(token);
        bool handled = false;
        SingleFireStyleObserver obs_a(*item(*document, "sa")->getRepr(), [&] {
            if (handled) return;
            handled = true;
            item(*document, "gb")->setAttribute(mutation.attribute, mutation.value);
        });
        SingleFireStyleObserver obs_b(*item(*document, "sb")->getRepr(), [&] {
            if (handled) return;
            handled = true;
            item(*document, "ga")->setAttribute(mutation.attribute, mutation.value);
        });
        auto const result = SW::apply_stroke_widths_compatible(*document, plan, 9110, *token);
        EXPECT_TRUE(handled) << mutation.name;
        EXPECT_EQ(result.state, SW::StrokeWidthApplyState::Failed) << mutation.name;
        EXPECT_EQ(result.reason, SW::StrokeWidthMemberReason::PostconditionMismatch) << mutation.name;
        EXPECT_EQ(result.attempted_writes, 1u) << mutation.name << ": the pending owner must not be overwritten";
        obs_a.detach();
        obs_b.detach();
        token->rollback();
        EXPECT_EQ(serialize(*document), before) << mutation.name;
    }
}

// No false refusal: an unrelated change during a write only costs a re-check.
TEST_F(StrokeWidthWp1cTest, WP1cUnrelatedChangeDuringTheWriteDoesNotRefuse)
{
    auto document = parse(std::string{svg_open} +
        R"svg(<text id="ta" x="10" y="20" style="font-family:Arial;font-size:20px;stroke:black;fill:none">)svg"
        R"svg(<tspan id="sa" style="stroke-width:2">AB</tspan></text>)svg"
        R"svg(<text id="tb" x="10" y="60" style="font-family:Arial;font-size:20px;stroke:black;fill:none">)svg"
        R"svg(<tspan id="sb" style="stroke-width:3">CD</tspan></text>)svg"
        R"svg(<rect id="other" width="5" height="5"/>)svg"
        R"(</svg>)");
    ASSERT_TRUE(document);
    settle(*document);
    auto const plan = SW::prepare_stroke_widths(
        *document, {item(*document, "ta"), item(*document, "tb")}, absolute(10.0), 9111);
    ASSERT_EQ(plan.state, SW::StrokeWidthPlanState::Prepared);
    auto token = DocumentUndo::beginAtomicInteraction(document.get());
    ASSERT_TRUE(token);
    SingleFireStyleObserver obs_a(*item(*document, "sa")->getRepr(), [&] {
        item(*document, "other")->setAttribute("data-noise", "1");
    });
    SingleFireStyleObserver obs_b(*item(*document, "sb")->getRepr(), [&] {
        item(*document, "other")->setAttribute("data-noise", "2");
    });
    auto const result = SW::apply_stroke_widths_compatible(*document, plan, 9111, *token);
    EXPECT_EQ(result.state, SW::StrokeWidthApplyState::Applied);
    EXPECT_EQ(result.changed, 2u);
    obs_a.detach();
    obs_b.detach();
    token->rollback();
}

// The caller's live-scope predicate (WP2b) is still checked before and after every write unit of
// the batched and direct text writers: an ended scope stops the apply and rollback restores.
TEST_F(StrokeWidthWp1cTest, WP1cLiveScopeStopsTheDirectTextWrites)
{
    auto document = parse(longtext_doc(20));
    ASSERT_TRUE(document);
    settle(*document);
    auto const before = serialize(*document);
    auto const plan = SW::prepare_stroke_widths(*document, {item(*document, "long")}, additive(0.5), 9112);
    ASSERT_EQ(plan.state, SW::StrokeWidthPlanState::Prepared);
    auto token = DocumentUndo::beginAtomicInteraction(document.get());
    ASSERT_TRUE(token);
    int calls = 0;
    // Two predicate calls per write unit (before, after): the scope ends before the 4th write.
    auto const result = SW::apply_stroke_widths_compatible(*document, plan, 9112, *token,
                                                           [&] { return ++calls <= 6; });
    EXPECT_EQ(result.state, SW::StrokeWidthApplyState::Failed);
    EXPECT_EQ(result.reason, SW::StrokeWidthMemberReason::PostconditionMismatch);
    EXPECT_EQ(result.attempted_writes, 3u);
    EXPECT_EQ(result.changed, 0u);
    token->rollback();
    EXPECT_EQ(serialize(*document), before);
}

// A group of runs with the identical write is one unit: the scope check is per unit.
TEST_F(StrokeWidthWp1cTest, WP1cLiveScopeStopsBeforeTheFirstBatchedWrite)
{
    auto document = parse(longtext_doc(30));
    ASSERT_TRUE(document);
    settle(*document);
    auto const before = serialize(*document);
    auto const plan = SW::prepare_stroke_widths(*document, {item(*document, "long")}, absolute(2.5), 9113);
    ASSERT_EQ(plan.state, SW::StrokeWidthPlanState::Prepared);
    auto token = DocumentUndo::beginAtomicInteraction(document.get());
    ASSERT_TRUE(token);
    auto const result = SW::apply_stroke_widths_text(*document, plan, 9113, *token, [] { return false; });
    EXPECT_EQ(result.state, SW::StrokeWidthApplyState::Failed);
    EXPECT_EQ(result.attempted_writes, 0u);
    token->rollback();
    EXPECT_EQ(serialize(*document), before);
}

// ---------------------------------------------------------------------------
// WP1c fix round (independent review R3).
// ---------------------------------------------------------------------------

// R3-1: a clone selected twice has a Covered duplicate while it is eligible; once the source gains
// a width the duplicate is no longer recorded. The transition must account for it.
TEST_F(StrokeWidthWp1cTest, WP1cR3DuplicateCloneRecordsFollowTheTransition)
{
    auto const svg = unset_clone_fixture("fill:red;stroke:black");
    {
        auto document = parse(svg);
        ASSERT_TRUE(document);
        auto const query = SW::query_stroke_widths(
            *document, {item(*document, "src"), item(*document, "u0"), item(*document, "u0")});
        ASSERT_EQ(query.covered, 1u) << "fixture: the repeated clone is a Covered duplicate";
    }
    clone_case(svg, {"u0", "u0"}, absolute(3.0), 3.0, 1u);
    clone_case(svg, {"u0", "u1", "u0", "u1"}, additive(1.5), 2.5, 2u);
}

// R3-2: a flipped clone keeps its ORIGINAL source; excluded clones keep their binding and authored
// attributes whatever class change a source write causes.
TEST_F(StrokeWidthWp1cTest, WP1cR3ExcludedCloneRetargetedDuringTheWriteFails)
{
    struct Tamper { char const *name; bool retarget; bool drop_transform; };
    for (auto const &tamper : {Tamper{"retarget and drop the singular transform", true, true},
                               Tamper{"drop the singular transform only", false, true},
                               Tamper{"retarget only", true, false}}) {
        auto document = parse(std::string{svg_open} +
            R"(<rect id="a" width="10" height="10" style="fill:red;stroke:black"/>)"
            R"(<rect id="b" width="10" height="10" style="fill:red;stroke:black"/>)"
            R"svg(<use id="cs" xlink:href="#a" x="30" transform="scale(0)"/>)svg"
            R"(</svg>)");
        ASSERT_TRUE(document) << tamper.name;
        settle(*document);
        auto const before = serialize(*document);
        auto const plan = SW::prepare_stroke_widths(
            *document, {item(*document, "a"), item(*document, "b"), item(*document, "cs")}, absolute(3.0), 9120);
        ASSERT_EQ(plan.state, SW::StrokeWidthPlanState::Prepared) << tamper.name;
        ASSERT_TRUE(find_excluded(plan.query, SW::StrokeWidthExclusion::SingularTransform)) << tamper.name;
        ASSERT_EQ(plan.planned_changes, 2u) << tamper.name;
        auto token = DocumentUndo::beginAtomicInteraction(document.get());
        ASSERT_TRUE(token);
        SingleFireStyleObserver observer(*item(*document, "a")->getRepr(), [&] {
            auto *clone = item(*document, "cs");
            if (tamper.retarget) clone->setAttribute("xlink:href", "#b");
            if (tamper.drop_transform) clone->setAttribute("transform", nullptr);
        });
        auto const result = SW::apply_stroke_widths_compatible(*document, plan, 9120, *token);
        EXPECT_TRUE(observer.fired()) << tamper.name;
        EXPECT_EQ(result.state, SW::StrokeWidthApplyState::Failed) << tamper.name;
        EXPECT_EQ(result.reason, SW::StrokeWidthMemberReason::PostconditionMismatch) << tamper.name;
        EXPECT_FALSE(SW::stroke_widths_compatible_output_ready(*document, plan, 9120, *token)) << tamper.name;
        observer.detach();
        token->rollback();
        EXPECT_EQ(serialize(*document), before) << tamper.name;
    }
}

// R3-3: a callback splits a pending plain span into two siblings with the same text and style. The
// cached span map must not survive the full-scan fallback; the run is then written natively.
TEST_F(StrokeWidthWp1cTest, WP1cR3SpanSplitByACallbackRollsBack)
{
    auto document = parse(longtext_doc(20));
    ASSERT_TRUE(document);
    settle(*document);
    auto *text = item(*document, "long");
    ASSERT_TRUE(text);
    auto const before = serialize(*document);
    auto const content = std::string(sp_te_get_string_multiline(text).raw());
    auto const plan = SW::prepare_stroke_widths(*document, {text}, additive(0.5), 9121);
    ASSERT_EQ(plan.state, SW::StrokeWidthPlanState::Prepared);
    // Descending order: the last tspan is written first, tspan 5 is still pending.
    auto *repr = text->getRepr();
    Inkscape::XML::Node *last = repr->lastChild();
    Inkscape::XML::Node *pending = repr->firstChild();
    for (int i = 0; i < 5; ++i) pending = pending->next();
    ASSERT_TRUE(last && pending && pending->firstChild());
    std::string const pending_style = pending->attribute("style");
    auto token = DocumentUndo::beginAtomicInteraction(document.get());
    ASSERT_TRUE(token);
    SingleFireStyleObserver observer(*last, [&] {
        auto *xml = document->getReprDoc();
        pending->firstChild()->setContent("w");
        auto *sibling = xml->createElement("svg:tspan");
        sibling->setAttribute("style", pending_style.c_str());
        auto *digits = xml->createTextNode("5");
        sibling->appendChild(digits);
        pending->parent()->addChild(sibling, pending);
        Inkscape::GC::release(digits);
        Inkscape::GC::release(sibling);
    });
    auto const result = SW::apply_stroke_widths_compatible(*document, plan, 9121, *token);
    EXPECT_TRUE(observer.fired());
    // A foreign split invalidates frozen source coverage; no whole-owner native fallback.
    EXPECT_EQ(result.state, SW::StrokeWidthApplyState::Failed);
    EXPECT_EQ(std::string(sp_te_get_string_multiline(item(*document, "long")).raw()), content);
    observer.detach();
    token->rollback();
    EXPECT_EQ(serialize(*document), before);
}

// R3-4: when the document is destroyed the watch leaves retained XML (no dangling observer).
TEST_F(StrokeWidthWp1cTest, WP1cR3WatchDetachesWhenTheDocumentIsDestroyed)
{
    auto document = parse(longtext_doc(2));
    ASSERT_TRUE(document);
    auto *rdoc = document->getReprDoc();
    auto *root = document->getReprRoot();
    Inkscape::GC::anchor(rdoc);
    Inkscape::GC::anchor(root);
    {
        SW::PendingOwnerWatch watch(*document);
        watch.acknowledge();
        ASSERT_FALSE(watch.foreign());
        root->setAttribute("data-live", "1");
        EXPECT_TRUE(watch.foreign()) << "a live document's changes are seen";
        watch.acknowledge();
        document.reset();
        root->setAttribute("data-late", "1");
        EXPECT_FALSE(watch.foreign()) << "the destroyed document's XML no longer notifies the watch";
    }
    Inkscape::GC::release(root);
    Inkscape::GC::release(rdoc);
}

// R3-5: a direct write is exact: a callback that rewrites the same element's style again (here it
// keeps the numbers and drops the dash priority) is refused right after that write.
TEST_F(StrokeWidthWp1cTest, WP1cR3SecondRewriteOfTheActiveSpanIsRefused)
{
    std::string svg = std::string{svg_open} +
        "<text id=\"long\" x=\"10\" y=\"20\" style=\"font-family:Arial;font-size:4px;stroke:black\">";
    for (int i = 0; i < 20; ++i) {
        svg += "<tspan style=\"stroke-width:" + std::to_string(1 + i % 5) +
               ";stroke-dasharray:4 2 !important\">w" + std::to_string(i % 10) + "</tspan>";
    }
    svg += "</text></svg>";
    auto document = parse(svg);
    ASSERT_TRUE(document);
    settle(*document);
    auto *text = item(*document, "long");
    auto const before = serialize(*document);
    auto const plan = SW::prepare_stroke_widths(*document, {text}, additive(0.5, true), 9122);
    ASSERT_EQ(plan.state, SW::StrokeWidthPlanState::Prepared);
    auto const *member = find_member(plan, text);
    ASSERT_TRUE(member);
    ASSERT_EQ(member->text_runs.size(), 20u);
    ASSERT_TRUE(member->text_runs[0].native_dasharray_css) << "fixture: the dash pattern is patched";
    ASSERT_TRUE(member->text_runs[0].dasharray_important);
    auto token = DocumentUndo::beginAtomicInteraction(document.get());
    ASSERT_TRUE(token);
    SingleFireStyleObserver observer(*text->getRepr()->lastChild(), [&] {
        std::string style = text->getRepr()->lastChild()->attribute("style");
        for (std::size_t at; (at = style.find(" !important")) != std::string::npos;) style.erase(at, 11);
        text->getRepr()->lastChild()->setAttribute("style", style.c_str());
    });
    auto const result = SW::apply_stroke_widths_compatible(*document, plan, 9122, *token);
    EXPECT_TRUE(observer.fired());
    EXPECT_EQ(result.state, SW::StrokeWidthApplyState::Failed);
    EXPECT_EQ(result.attempted_writes, 1u) << "refused right after the rewritten write, not at the end";
    observer.detach();
    token->rollback();
    EXPECT_EQ(serialize(*document), before);
}

// R3-5b: the changed-text verifier compares dash priorities: a persisted result with the numbers
// right and the priority wrong is not accepted, also on the native path.
TEST_F(StrokeWidthWp1cTest, WP1cR3DashPriorityIsVerifiedOnTheNativePath)
{
    auto document = parse(std::string{svg_open} +
        R"(<text id="t" x="10" y="20" style="font-family:Arial;font-size:20px;stroke:black">)"
        R"(<tspan id="s" style="stroke-width:2;stroke-dasharray:4 2 !important">AB</tspan></text></svg>)");
    ASSERT_TRUE(document);
    settle(*document);
    auto *text = item(*document, "t");
    auto const plan = SW::prepare_stroke_widths(*document, {text}, additive(0.5, true), 9123);
    ASSERT_EQ(plan.state, SW::StrokeWidthPlanState::Prepared);
    auto token = DocumentUndo::beginAtomicInteraction(document.get());
    ASSERT_TRUE(token);
    ASSERT_EQ(SW::apply_stroke_widths_compatible(*document, plan, 9123, *token).state,
              SW::StrokeWidthApplyState::Applied);
    ASSERT_TRUE(SW::stroke_widths_compatible_output_ready(*document, plan, 9123, *token));
    // The same numbers without the priority (the writer keeps the frozen priority).
    Inkscape::XML::Node *span = item(*document, "t")->getRepr()->firstChild();
    std::string style = span->attribute("style");
    ASSERT_NE(style.find("!important"), std::string::npos) << style;
    for (std::size_t at; (at = style.find(" !important")) != std::string::npos;) style.erase(at, 11);
    span->setAttribute("style", style.c_str());
    document->ensureUpToDate();
    EXPECT_FALSE(SW::stroke_widths_compatible_output_ready(*document, plan, 9123, *token));
    token->rollback();
}

// ---------------------------------------------------------------------------
// R3-7: native versus direct large-owner writes. The same document is written with the native
// range write forced everywhere and with the direct writer forced everywhere. Complete stroke
// snapshots of every character, glyph anchor positions and the text content must be equal, also
// after the XML is serialized and parsed again.
// ---------------------------------------------------------------------------

namespace {

class StrokeWidthDifferentialTest : public StrokeWidthWp1cTest
{
protected:
    struct TextSnapshot {
        std::string content;
        std::vector<SW::StrokeWidthCharBaseline> chars;
        std::vector<Geom::Point> anchors;
    };
    struct RunOutput {
        SW::StrokeWidthApplyResult result;
        TextSnapshot before;
        TextSnapshot after;
        TextSnapshot reopened;
        std::string xml;
    };

    static TextSnapshot snapshot(SPDocument &document, char const *id)
    {
        TextSnapshot out;
        document.ensureUpToDate();
        auto *owner = item(document, id);
        EXPECT_TRUE(owner) << id;
        if (!owner) return out;
        out.content = std::string(sp_te_get_string_multiline(owner).raw());
        auto const query = SW::query_stroke_widths(document, {owner});
        if (query.targets.size() == 1) out.chars = query.targets[0].char_baseline;
        auto const *layout = te_get_layout(owner);
        if (layout) {
            auto const count = layout->iteratorToCharIndex(layout->end());
            for (int i = 0; i < count; ++i) {
                out.anchors.push_back(layout->characterAnchorPoint(layout->charIndexToIterator(i)));
            }
        }
        return out;
    }

    static void expect_same_style(SW::StrokeWidthStyle const &a, SW::StrokeWidthStyle const &b,
                                  std::string const &where)
    {
        EXPECT_DOUBLE_EQ(a.local_computed, b.local_computed) << where << " width";
        EXPECT_EQ(a.effective_px.has_value(), b.effective_px.has_value()) << where;
        if (a.effective_px && b.effective_px) EXPECT_DOUBLE_EQ(*a.effective_px, *b.effective_px) << where;
        EXPECT_EQ(static_cast<int>(a.convention), static_cast<int>(b.convention)) << where << " convention";
        EXPECT_EQ(a.paint_none, b.paint_none) << where;
        EXPECT_EQ(a.paint_important, b.paint_important) << where;
        EXPECT_EQ(a.paint_style_src, b.paint_style_src) << where;
        EXPECT_EQ(a.hairline_important, b.hairline_important) << where;
        EXPECT_EQ(a.hairline_style_src, b.hairline_style_src) << where << " hairline source";
        EXPECT_EQ(a.dash_set, b.dash_set) << where;
        EXPECT_EQ(a.dash_computed, b.dash_computed) << where << " dash";
        EXPECT_DOUBLE_EQ(a.dash_offset_computed, b.dash_offset_computed) << where << " dash offset";
        EXPECT_EQ(a.width_set, b.width_set) << where;
        EXPECT_EQ(a.width_inherit, b.width_inherit) << where;
        EXPECT_EQ(a.width_important, b.width_important) << where << " width priority";
        EXPECT_EQ(a.width_style_src, b.width_style_src) << where << " width source";
        EXPECT_EQ(a.dash_style_src, b.dash_style_src) << where << " dash source";
        EXPECT_EQ(a.dashoffset_style_src, b.dashoffset_style_src) << where << " dash offset source";
        EXPECT_EQ(a.dasharray_important, b.dasharray_important) << where << " dash priority";
        EXPECT_EQ(a.dashoffset_important, b.dashoffset_important) << where << " dash offset priority";
        EXPECT_EQ(a.non_scaling, b.non_scaling) << where;
        EXPECT_EQ(a.vector_effect_value, b.vector_effect_value) << where;
        EXPECT_EQ(a.vector_effect_set, b.vector_effect_set) << where;
        EXPECT_EQ(a.vector_effect_important, b.vector_effect_important) << where;
        EXPECT_EQ(a.vector_effect_inherit, b.vector_effect_inherit) << where;
        EXPECT_EQ(a.vector_effect_style_src, b.vector_effect_style_src) << where << " vector-effect source";
    }

    static void expect_same(TextSnapshot const &a, TextSnapshot const &b, std::string const &label)
    {
        EXPECT_EQ(a.content, b.content) << label << " content";
        ASSERT_EQ(a.chars.size(), b.chars.size()) << label;
        for (std::size_t i = 0; i < a.chars.size(); ++i) {
            EXPECT_EQ(a.chars[i].glyph, b.chars[i].glyph) << label << " glyph " << i;
            expect_same_style(a.chars[i].style, b.chars[i].style, label + " char " + std::to_string(i));
        }
        ASSERT_EQ(a.anchors.size(), b.anchors.size()) << label;
        for (std::size_t i = 0; i < a.anchors.size(); ++i) {
            EXPECT_NEAR(a.anchors[i].x(), b.anchors[i].x(), 1e-6) << label << " x " << i;
            EXPECT_NEAR(a.anchors[i].y(), b.anchors[i].y(), 1e-6) << label << " y " << i;
        }
    }

    /// Apply `intent` to owner `id` with the given bulk threshold, commit, and snapshot before, after
    /// and after a serialize/parse round trip. A range limits the write to that part of the text.
    RunOutput run(std::string const &svg, char const *id, SW::StrokeWidthIntent const &intent,
                  std::size_t threshold, std::optional<std::pair<unsigned, unsigned>> range = {})
    {
        RunOutput out;
        auto const previous = SW::stroke_width_set_bulk_text_threshold(threshold);
        {
            auto document = parse(svg);
            EXPECT_TRUE(document);
            if (!document) {
                SW::stroke_width_set_bulk_text_threshold(previous);
                return out;
            }
            settle(*document);
            out.before = snapshot(*document, id);
            auto *owner = item(*document, id);
            SW::StrokeWidthPlan plan;
            if (range) {
                SW::StrokeWidthTextRange text_range;
                text_range.owner = owner;
                text_range.first_char = range->first;
                text_range.last_char = range->second;
                plan = SW::prepare_stroke_widths_combined(*document, text_range, {owner}, intent, 9200);
            } else {
                plan = SW::prepare_stroke_widths(*document, {owner}, intent, 9200);
            }
            EXPECT_EQ(plan.state, SW::StrokeWidthPlanState::Prepared);
            auto token = DocumentUndo::beginAtomicInteraction(document.get());
            EXPECT_TRUE(token);
            if (token) {
                out.result = SW::apply_stroke_widths_compatible(*document, plan, 9200, *token);
                if (out.result.state == SW::StrokeWidthApplyState::Applied) {
                    EXPECT_TRUE(token->commitAtomically(Util::Internal::ContextString("R3-7"), "", [&] {
                        return SW::stroke_widths_compatible_output_ready(*document, plan, 9200, *token);
                    }));
                } else {
                    token->rollback();
                }
            }
            out.after = snapshot(*document, id);
            out.xml = serialize(*document);
        }
        SW::stroke_width_set_bulk_text_threshold(previous);
        auto reopened = parse(out.xml);
        EXPECT_TRUE(reopened);
        if (reopened) out.reopened = snapshot(*reopened, id);
        return out;
    }

    /// Native versus direct, before and after reopening. Returns the direct run for extra checks.
    RunOutput compare(std::string const &svg, char const *id, SW::StrokeWidthIntent const &intent,
                      std::string const &label, std::optional<std::pair<unsigned, unsigned>> range = {},
                      bool native_may_write_directly = false)
    {
        auto native = run(svg, id, intent, std::numeric_limits<std::size_t>::max(), range);
        auto direct = run(svg, id, intent, 1, range);
        EXPECT_EQ(native.result.state, SW::StrokeWidthApplyState::Applied)
            << label << " native, reason " << static_cast<int>(native.result.reason) << " after "
            << native.result.attempted_writes << " writes";
        EXPECT_EQ(direct.result.state, SW::StrokeWidthApplyState::Applied)
            << label << " direct, reason " << static_cast<int>(direct.result.reason) << " after "
            << direct.result.attempted_writes << " writes";
        if (range && !native_may_write_directly) {
            EXPECT_EQ(native.result.work.direct_source_writes, 0u) << label << ": the native run must be native";
        }
        expect_same(native.after, direct.after, label + " native vs direct");
        expect_same(native.reopened, direct.reopened, label + " reopened native vs direct");
        expect_same(direct.after, direct.reopened, label + " direct vs reopened direct");
        EXPECT_EQ(native.before.content, direct.after.content) << label << " content unchanged";
        // The write really changed something.
        bool changed = false;
        for (std::size_t i = 0; i < direct.after.chars.size() && i < direct.before.chars.size(); ++i) {
            if (direct.after.chars[i].style.local_computed != direct.before.chars[i].style.local_computed ||
                direct.after.chars[i].style.dash_computed != direct.before.chars[i].style.dash_computed) {
                changed = true;
            }
        }
        EXPECT_TRUE(changed) << label << ": nothing was written";
        return direct;
    }

    static std::string spans(int n, std::function<std::string(int)> const &attributes,
                             std::function<std::string(int)> const &content)
    {
        std::string out;
        for (int i = 0; i < n; ++i) out += "<tspan " + attributes(i) + ">" + content(i) + "</tspan>";
        return out;
    }

    static std::string doc(std::string const &head, std::string const &body)
    {
        return std::string{svg_open} + head + body + "</svg>";
    }
};

} // namespace

TEST_F(StrokeWidthDifferentialTest, DashPatchesAndPriorities)
{
    auto const body = std::string{"<text id=\"t\" x=\"10\" y=\"20\" style=\"font-family:Arial;font-size:4px;stroke:black\">"} +
        spans(24, [](int i) {
            std::string style = "stroke-width:" + std::to_string(1 + i % 5);
            if (i % 2 == 0) style += ";stroke-dasharray:4 2" + std::string(i % 3 == 0 ? " !important" : "");
            if (i % 4 == 0) style += ";stroke-dashoffset:1" + std::string(i % 8 == 0 ? " !important" : "");
            return "style=\"" + style + "\"";
        }, [](int i) { return "w" + std::to_string(i % 10); }) + "</text>";
    auto const svg = doc("", body);
    auto const direct = compare(svg, "t", additive(0.5, true), "dash additive");
    EXPECT_GT(direct.result.work.direct_source_writes, 0u);
    compare(svg, "t", absolute(3.0, true), "dash absolute");
    // A scaled owner: the native write converts document units; the direct write keeps local units.
    std::string scaled = svg;
    scaled.replace(scaled.find("<text id=\"t\""), 12, "<text id=\"t\" transform=\"scale(2)\"");
    compare(scaled, "t", additive(0.5, true), "dash additive, scaled owner");
    compare(scaled, "t", absolute(3.0, true), "dash absolute, scaled owner");
}

TEST_F(StrokeWidthDifferentialTest, InheritedAndPresentationStyles)
{
    auto const body = std::string{"<text id=\"t\" x=\"10\" y=\"20\" style=\"font-family:Arial;font-size:4px;stroke:black;stroke-width:2\">"} +
        spans(24, [](int i) {
            switch (i % 4) {
            case 0: return std::string{"fill=\"#0000ff\""};                       // inherits the width
            case 1: return "stroke-width=\"" + std::to_string(1 + i % 3) + "\" font-size=\"5\"";
            case 2: return "style=\"stroke-width:" + std::to_string(1 + i % 5) + "\" fill=\"#ff0000\"";
            default: return std::string{"stroke=\"#333333\" stroke-linejoin=\"round\""};
            }
        }, [](int i) { return "w" + std::to_string(i % 10); }) + "</text>";
    auto const svg = doc("", body);
    compare(svg, "t", additive(0.5), "inherited/presentation additive");
    compare(svg, "t", absolute(3.0), "inherited/presentation absolute");
}

TEST_F(StrokeWidthDifferentialTest, StylesheetWidthIsProtected)
{
    auto const body = std::string{"<style type=\"text/css\">.w{stroke-width:3}</style>"} +
        "<text id=\"t\" x=\"10\" y=\"20\" style=\"font-family:Arial;font-size:4px;stroke:black\">" +
        spans(24, [](int i) { return i % 3 == 0 ? std::string{"class=\"w\""} : "style=\"stroke-width:" + std::to_string(1 + i % 5) + "\""; },
              [](int i) { return "w" + std::to_string(i % 10); }) + "</text>";
    auto const svg = doc("", body);
    // The native path already writes complete stylesheet-width spans directly (protection).
    compare(svg, "t", absolute(5.0), "stylesheet absolute", {}, true);
    compare(svg, "t", additive(0.5), "stylesheet additive", {}, true);
}

TEST_F(StrokeWidthDifferentialTest, PositionedLinesAndBlankLines)
{
    // CorelDRAW-like output (BUG-009): every line has its own x/y; blank lines are empty positioned spans.
    std::string body = "<text id=\"t\" x=\"10.5\" y=\"10\" xml:space=\"preserve\" style=\"font-family:Arial;font-size:4px;stroke:black\">";
    for (int i = 0; i < 24; ++i) {
        body += "<tspan x=\"10.5\" y=\"" + std::to_string(10 + i * 6) + "\" style=\"stroke-width:" +
                std::to_string(1 + i % 5) + "\">line " + std::to_string(i) + "</tspan>";
        if (i % 5 == 4) body += "<tspan x=\"10.5\" y=\"" + std::to_string(13 + i * 6) + "\"></tspan>";
    }
    body += "</text>";
    auto const svg = doc("", body);
    auto const direct = compare(svg, "t", additive(0.5), "positioned lines");
    EXPECT_GT(direct.result.work.direct_source_writes, 0u);
    // The blank positioned lines are still in the XML.
    EXPECT_NE(direct.xml.find("y=\"37\""), std::string::npos);
}

TEST_F(StrokeWidthDifferentialTest, WhitespaceAndXmlSpace)
{
    for (bool preserve : {true, false}) {
        auto const body = std::string{"<text id=\"t\" x=\"10\" y=\"20\" "} + (preserve ? "xml:space=\"preserve\"" : "") +
            " style=\"font-family:Arial;font-size:4px;stroke:black\">" +
            spans(24, [](int i) { return "style=\"stroke-width:" + std::to_string(1 + i % 5) + "\""; },
                  [](int i) {
                      switch (i % 4) {
                      case 0: return "  a" + std::to_string(i % 10) + "  ";
                      case 1: return " b" + std::to_string(i % 10);
                      case 2: return "c" + std::to_string(i % 10) + " ";
                      default: return "d\n   e" + std::to_string(i % 10);
                      }
                  }) + "</text>";
        auto const svg = doc("", body);
        std::string const label = preserve ? "whitespace preserve" : "whitespace collapse";
        auto const direct = compare(svg, "t", additive(0.5), label);
        EXPECT_EQ(direct.result.work.native_range_writes, 0u);
        EXPECT_EQ(direct.before.content, direct.after.content);

    }
}

TEST_F(StrokeWidthDifferentialTest, TextOnAPath)
{
    auto const body = std::string{"<defs><path id=\"p\" d=\"M10,60 C60,0 120,120 200,60\"/></defs>"} +
        "<text id=\"t\" style=\"font-family:Arial;font-size:4px;stroke:black\"><textPath xlink:href=\"#p\">" +
        spans(24, [](int i) { return "style=\"stroke-width:" + std::to_string(1 + i % 5) + "\""; },
              [](int i) { return "w" + std::to_string(i % 10); }) + "</textPath></text>";
    auto const svg = doc("", body);
    auto const native = run(svg, "t", additive(0.5), std::numeric_limits<std::size_t>::max());
    auto const direct = run(svg, "t", additive(0.5), 1);
    EXPECT_EQ(native.result.state, SW::StrokeWidthApplyState::Applied);
    EXPECT_EQ(direct.result.state, SW::StrokeWidthApplyState::Applied);
    expect_same(native.after, direct.after, "text path native vs direct");
    expect_same(native.reopened, direct.reopened, "text path reopened");
    expect_same(direct.after, direct.reopened, "text path direct vs reopened");
    EXPECT_GT(direct.result.work.direct_source_writes, 0u);
    EXPECT_NE(native.after.chars.front().style.local_computed, native.before.chars.front().style.local_computed)
        << "the text on the path was written";
}

TEST_F(StrokeWidthDifferentialTest, FlowedText)
{
    auto const body = std::string{"<flowRoot id=\"t\" style=\"font-family:Arial;font-size:4px;stroke:black\">"} +
        "<flowRegion><rect x=\"10\" y=\"10\" width=\"150\" height=\"80\"/></flowRegion><flowPara>" +
        spans(24, [](int i) { return "style=\"stroke-width:" + std::to_string(1 + i % 5) + "\""; },
              [](int i) { return "w" + std::to_string(i % 10); }) + "</flowPara></flowRoot>";
    auto svg = doc("", body);
    // flowSpan is the flow element: rename the generated tspans.
    for (std::size_t at; (at = svg.find("<tspan")) != std::string::npos;) svg.replace(at, 6, "<flowSpan");
    for (std::size_t at; (at = svg.find("</tspan>")) != std::string::npos;) svg.replace(at, 8, "</flowSpan>");
    auto const native = run(svg, "t", additive(0.5), std::numeric_limits<std::size_t>::max());
    auto const direct = run(svg, "t", additive(0.5), 1);
    EXPECT_EQ(native.result.state, SW::StrokeWidthApplyState::Applied)
        << "reason " << static_cast<int>(native.result.reason) << " after " << native.result.attempted_writes;
    EXPECT_EQ(direct.result.state, SW::StrokeWidthApplyState::Applied)
        << "reason " << static_cast<int>(direct.result.reason) << " after " << direct.result.attempted_writes;
    expect_same(native.after, direct.after, "flowed native vs direct");
    expect_same(native.reopened, direct.reopened, "flowed reopened");
    expect_same(direct.after, direct.reopened, "flowed direct vs reopened");
    EXPECT_GT(direct.result.work.direct_source_writes, 0u);
}

TEST_F(StrokeWidthDifferentialTest, PartialSourcesFallBackToTheNativeWrite)
{
    // A range that starts and ends inside a span: the two partial spans need native splitting, the
    // spans between are complete and may be written directly.
    auto const body = std::string{"<text id=\"t\" x=\"10\" y=\"20\" style=\"font-family:Arial;font-size:4px;stroke:black\">"} +
        spans(24, [](int i) { return "style=\"stroke-width:" + std::to_string(1 + i % 5) + "\""; },
              [](int i) { return "w" + std::to_string(i % 10); }) + "</text>";
    auto const svg = doc("", body);
    auto const direct = compare(svg, "t", additive(0.5), "partial range", std::make_pair(1u, 45u));
    EXPECT_GT(direct.result.work.direct_source_writes, 0u);
    EXPECT_GT(direct.result.work.native_range_writes, 0u) << "the two partial spans are written natively";
    // The characters outside the range keep their width.
    EXPECT_DOUBLE_EQ(direct.after.chars.front().style.local_computed, direct.before.chars.front().style.local_computed);
    EXPECT_DOUBLE_EQ(direct.after.chars.back().style.local_computed, direct.before.chars.back().style.local_computed);
}

TEST_F(StrokeWidthDifferentialTest, InkscapeLineSpans)
{
    std::string body = "<text id=\"t\" x=\"10\" y=\"10\" xml:space=\"preserve\" style=\"font-family:Arial;font-size:4px;stroke:black\">";
    for (int i = 0; i < 24; ++i) {
        body += "<tspan sodipodi:role=\"line\" x=\"10\" y=\"" + std::to_string(10 + i * 6) + "\" style=\"stroke-width:" +
                std::to_string(1 + i % 5) + "\">line " + std::to_string(i) + "</tspan>";
    }
    body += "</text>";
    auto const direct = compare(doc("", body), "t", additive(0.5), "sodipodi:role=line");
    EXPECT_EQ(direct.result.work.native_range_writes, 0u);
    EXPECT_EQ(direct.result.work.direct_source_writes, 24u);
    for (std::size_t i = 0; i < direct.after.chars.size(); ++i) {
        if (direct.before.chars[i].authored)
            EXPECT_DOUBLE_EQ(direct.after.chars[i].style.local_computed,
                             direct.before.chars[i].style.local_computed + 0.5);
        EXPECT_EQ(direct.after.anchors[i], direct.before.anchors[i]);
    }
}


// R3b-1: the excluded clones' binding snapshots are part of both plan comparators: replacing an
// excluded singular clone's authored style or attributes keeps its query record but is a different
// plan, before an apply (reprepare) and at settlement (`stroke_width_plans_match`).
TEST_F(StrokeWidthWp1cTest, WP1cR3bPlanComparatorsSeeAChangedExcludedClone)
{
    struct Edit { char const *name; char const *attribute; char const *value; };
    for (auto const &edit : {Edit{"authored style", "style", "opacity:0.5"},
                             Edit{"authored attribute", "data-note", "changed"}}) {
        auto document = parse(std::string{svg_open} +
            R"(<rect id="a" width="10" height="10" style="fill:red;stroke:black"/>)"
            R"svg(<use id="cs" xlink:href="#a" x="30" transform="scale(0)"/>)svg"
            R"(</svg>)");
        ASSERT_TRUE(document) << edit.name;
        settle(*document);
        std::vector<SPItem *> const roots{item(*document, "a"), item(*document, "cs")};
        auto const first = SW::prepare_stroke_widths(*document, roots, absolute(3.0), 9300);
        ASSERT_EQ(first.state, SW::StrokeWidthPlanState::Prepared) << edit.name;
        ASSERT_EQ(first.preserved_clones.size(), 1u) << edit.name;
        EXPECT_TRUE(SW::stroke_width_plans_match(first, SW::prepare_stroke_widths(*document, roots, absolute(3.0), 9300)));

        item(*document, "cs")->setAttribute(edit.attribute, edit.value);
        settle(*document);
        auto const second = SW::prepare_stroke_widths(*document, roots, absolute(3.0), 9300);
        ASSERT_EQ(second.state, SW::StrokeWidthPlanState::Prepared) << edit.name;
        // Same query record, same members, different snapshot.
        EXPECT_EQ(second.query.excluded.size(), first.query.excluded.size()) << edit.name;
        EXPECT_FALSE(SW::stroke_width_plans_match(first, second)) << edit.name << ": settlement must not accept it";
        EXPECT_FALSE(SW::stroke_width_plans_match(second, first)) << edit.name;

        // Apply with the stale plan: the pre-apply re-derivation rejects it without a write.
        auto const before = serialize(*document);
        auto token = DocumentUndo::beginAtomicInteraction(document.get());
        ASSERT_TRUE(token);
        auto const result = SW::apply_stroke_widths_compatible(*document, first, 9300, *token);
        EXPECT_EQ(result.state, SW::StrokeWidthApplyState::Rejected) << edit.name;
        EXPECT_EQ(result.reason, SW::StrokeWidthMemberReason::StalePlan) << edit.name;
        EXPECT_EQ(result.attempted_writes, 0u) << edit.name;
        token->rollback();
        EXPECT_EQ(serialize(*document), before) << edit.name;
    }
}

// R3b-2: an unchanged malformed (NaN) affine of an excluded clone is unchanged.
TEST_F(StrokeWidthWp1cTest, WP1cR3bUnchangedNonFiniteCloneIsPreserved)
{
    auto document = parse(unset_clone_fixture("fill:red;stroke:black"));
    ASSERT_TRUE(document);
    settle(*document);
    auto *clone = item(*document, "u1");
    ASSERT_TRUE(clone);
    clone->transform = Geom::Affine(std::numeric_limits<double>::quiet_NaN(), 0, 0, 1, 0, 0);
    auto const before = serialize(*document);
    std::vector<SPItem *> const roots{item(*document, "src"), item(*document, "u0"), clone};
    auto const plan = SW::prepare_stroke_widths(*document, roots, absolute(3.0), 9301);
    ASSERT_EQ(plan.state, SW::StrokeWidthPlanState::Prepared);
    ASSERT_TRUE(find_excluded(plan.query, SW::StrokeWidthExclusion::NonFiniteTransform));
    ASSERT_EQ(plan.planned_changes, 1u);
    ASSERT_EQ(plan.preserved_clones.size(), 1u);
    wp1_roundtrip(*document, plan, before, [&](SW::StrokeWidthApplyResult const &result) {
        ASSERT_EQ(result.state, SW::StrokeWidthApplyState::Applied)
            << "reason " << static_cast<int>(result.reason);
        EXPECT_DOUBLE_EQ(computed_width(*document, "src"), 3.0);
        EXPECT_TRUE(std::isnan(item(*document, "u1")->transform[0])) << "the clone was not touched";
    });
}

// F1: independent computed-width and exact geometry/XML outcomes.
TEST_F(StrokeWidthControllerTest, F1WholeOwnersInvisibleAndLineSourcesAllIntents)
{
    struct Case { std::string body; std::vector<char const *> sources; std::vector<double> widths{}; bool dashed = false; };
    std::vector<Case> cases = {
        {R"svg(<text id="t" style="stroke-dasharray:4 2;stroke-dashoffset:1">Hello <tspan id="a" style="fill:red;stroke-width:3">world</tspan></text>)svg", {"t", "a"}, {2, 3}, true},
        {R"svg(<flowRoot id="t"><flowRegion><rect width="200" height="200"/></flowRegion><flowPara id="a" style="stroke-dasharray:4 2;stroke-dashoffset:1">Hello <flowSpan id="b" style="fill:red;stroke-width:3">world</flowSpan></flowPara></flowRoot>)svg", {"a", "b"}, {2, 3}, true},
        {R"svg(<flowRoot id="t"><flowRegion><rect width="200" height="200"/></flowRegion><flowDiv id="a" style="stroke-dasharray:4 2;stroke-dashoffset:1">Hello <flowSpan id="b" style="fill:red;stroke-width:3">world</flowSpan></flowDiv></flowRoot>)svg", {"a", "b"}, {2, 3}, true},
        {R"svg(<defs><path id="p" d="M0,30 H200"/></defs><text id="t"><textPath style="stroke-dasharray:4 2;stroke-dashoffset:1" id="a" xlink:href="#p">Hello <tspan id="b" style="fill:red;stroke-width:3">world</tspan></textPath></text>)svg", {"a", "b"}, {2, 3}, true},
        {R"svg(<flowRoot id="t"><flowRegion><rect width="200" height="200"/></flowRegion><flowDiv><flowPara id="a">AB</flowPara><flowPara id="b">CD</flowPara></flowDiv></flowRoot>)svg", {"a", "b"}},
        {R"svg(<flowRoot id="t"><flowRegion><rect width="100" height="40"/><rect y="100" width="100" height="40"/></flowRegion><flowPara id="a">AB</flowPara><flowRegionBreak/><flowPara id="b">CD</flowPara></flowRoot>)svg", {"a", "b"}},
        {R"svg(<text id="t"><tspan sodipodi:role="line"/><tspan id="a" sodipodi:role="line" x="10" y="60">AB</tspan></text>)svg", {"a"}},
        {R"svg(<text id="t"><tspan id="a" sodipodi:role="line" x="10" y="30">AB</tspan><tspan id="b" sodipodi:role="line" x="10" y="60">CD</tspan></text>)svg", {"a", "b"}},
        {R"svg(<text id="t"><tspan id="a" sodipodi:role="line">A</tspan><tspan sodipodi:role="line" id="blank"/></text>)svg", {"a"}},
        {R"svg(<text id="t"><tspan sodipodi:role="line"><tspan id="a">A</tspan></tspan><tspan sodipodi:role="line"><tspan id="b">B</tspan></tspan></text>)svg", {"a", "b"}},
        {R"svg(<text id="t"><tspan id="a">  A  </tspan><tspan id="b"> B  C </tspan></text>)svg", {"a", "b"}},
        {R"svg(<text id="t"><tspan id="a" x="10.1234" y="30.5678" rotate="10">  A </tspan><tspan id="b" x="10.1234" dy="1.2em"> B  C </tspan><tspan x="10.1234" dy="1.2em"/></text>)svg", {"a", "b"}},
        {R"svg(<text id="t"><tspan id="a">A&#173;B</tspan></text>)svg", {"a"}},
        {R"svg(<text id="t"><tspan id="a">&#173;</tspan></text>)svg", {"a"}},
        {R"svg(<defs><path id="p" d="M0,30 H20"/></defs><text id="t"><textPath id="a" xlink:href="#p">ABCD EFGH</textPath></text>)svg", {"a"}},
        {R"svg(<defs><path id="p" d="M0,30 H20"/></defs><text id="t"><textPath id="a" xlink:href="#p" startOffset="200">ABCD</textPath></text>)svg", {"a"}},
        {R"svg(<flowRoot id="t"><flowRegion><rect id="frame" width="30" height="22"/></flowRegion><flowPara id="a">ABCD EFGH IJKL</flowPara></flowRoot>)svg", {"a"}},
        {R"svg(<flowRoot id="t"><flowRegion><rect id="frame" width="1" height="1"/></flowRegion><flowPara id="a">ABCD</flowPara></flowRoot>)svg", {"a"}}
    };
    for (std::size_t c = 0; c < cases.size(); ++c) {
        for (auto intent : {absolute(4, true), absolute(0, true), relative(200, true), additive(1, true), additive(-1, true),
                            SW::StrokeWidthIntent{SW::StrokeWidthIntentKind::Hairline, 0, true},
                            SW::StrokeWidthIntent{SW::StrokeWidthIntentKind::RemoveStroke, 0, true}}) {
            for (bool caret : {false, true}) {
                SCOPED_TRACE(std::to_string(c) + ":" + std::to_string(int(intent.kind)) + ":" + std::to_string(caret));
                auto document = parse(std::string{svg_open} +
                    R"svg(<g style="font-family:Arial;font-size:20px;stroke:black;stroke-width:2">)svg" + cases[c].body + "</g></svg>");
                ASSERT_TRUE(document); settle(*document);
                auto *owner = item(*document, "t");
                auto const *layout = te_get_layout(owner);
                ASSERT_TRUE(layout);
                auto const count = layout->iteratorToCharIndex(layout->end());
                ASSERT_GT(count, 0);
                if (cases[c].body.find("flowRegionBreak") != std::string::npos) {
                    EXPECT_TRUE(std::any_of(layout->input_stream().begin(), layout->input_stream().end(), [](auto *input) {
                        return input->Type() == Text::Layout::CONTROL_CODE && input->source->getRepr() &&
                            std::string(input->source->getRepr()->name()) == "svg:flowRegionBreak";
                    }));
                    std::set<unsigned> shapes;
                    for (auto const &glyph : layout->glyphs()) shapes.insert(glyph.span(layout).line(layout).in_shape);
                    EXPECT_EQ(shapes.size(), 2u);
                }
                std::vector<Geom::Point> anchors;
                for (int i = 0; i < count; ++i) anchors.push_back(layout->characterAnchorPoint(layout->charIndexToIterator(i)));
                std::vector<Geom::Affine> glyphs;
                std::vector<unsigned> identities;
                for (auto const &glyph : layout->glyphs()) { glyphs.push_back(glyph.transform(*layout)); identities.push_back(glyph.glyph); }
                std::vector<std::pair<XML::Node *, std::string>> raw;
                std::function<void(XML::Node *)> capture = [&](XML::Node *node) {
                    if (node->content()) raw.emplace_back(node, node->content());
                    for (auto *child = node->firstChild(); child; child = child->next()) capture(child);
                };
                capture(owner->getRepr());
                auto const before = serialize(*document);
                SW::StrokeWidthTextRange range; range.owner = owner; range.caret = true;
                auto const plan = caret ? SW::prepare_stroke_widths_combined(*document, range, {owner}, intent, 7001)
                                        : SW::prepare_stroke_widths(*document, {owner}, intent, 7001);
                wp1_roundtrip(*document, plan, before, [&](auto const &result) {
                    ASSERT_EQ(result.state, SW::StrokeWidthApplyState::Applied);
                    EXPECT_EQ(result.changed, 1u); EXPECT_EQ(result.skipped_runs, 0u);
                    EXPECT_EQ(result.work.native_range_writes, 0u);
                    EXPECT_EQ(result.work.direct_source_writes, cases[c].sources.size());
                    for (unsigned j = 0; j < cases[c].sources.size(); ++j) {
                        auto id = cases[c].sources[j];
                        double const old = cases[c].widths.empty() ? 2 : cases[c].widths[j];
                        auto *source = document->getObjectById(id); ASSERT_TRUE(source && source->style);
                        double expected = intent.kind == SW::StrokeWidthIntentKind::Hairline ? 1 :
                            intent.kind == SW::StrokeWidthIntentKind::RemoveStroke ? old :
                            intent.kind == SW::StrokeWidthIntentKind::AdditiveCssPx ? old + intent.value :
                            intent.kind == SW::StrokeWidthIntentKind::RelativePercent ? old * 2 : intent.value;
                        EXPECT_DOUBLE_EQ(source->style->stroke_width.computed, expected);
                        if (cases[c].dashed) {
                            // Existing text CSS output stores six significant digits.
                            // Derive the oracle independently of the planner/native reader.
                            auto stored = [](double value) {
                                char buffer[64]; g_ascii_formatd(buffer, sizeof(buffer), "%.6g", value);
                                return g_ascii_strtod(buffer, nullptr);
                            };
                            double const ratio = intent.kind == SW::StrokeWidthIntentKind::RemoveStroke ? 1 : expected / old;
                            if (ratio == 0) EXPECT_TRUE(source->style->stroke_dasharray.values.empty());
                            else {
                                ASSERT_EQ(source->style->stroke_dasharray.values.size(), 2u);
                                EXPECT_DOUBLE_EQ(source->style->stroke_dasharray.values[0].computed, stored(4 * ratio));
                                EXPECT_DOUBLE_EQ(source->style->stroke_dasharray.values[1].computed, stored(2 * ratio));
                            }
                            EXPECT_DOUBLE_EQ(source->style->stroke_dashoffset.computed, stored(ratio));
                        }
                        if (intent.kind == SW::StrokeWidthIntentKind::Hairline) {
                            EXPECT_TRUE(source->style->stroke_extensions.hairline);
                            EXPECT_EQ(source->style->vector_effect.stroke, true);
                        }
                        if (intent.kind == SW::StrokeWidthIntentKind::RemoveStroke) EXPECT_TRUE(source->style->stroke.isNone());
                    }
                    auto const *after = te_get_layout(owner);
                    ASSERT_EQ(after->iteratorToCharIndex(after->end()), count);
                    for (int i = 0; i < count; ++i) EXPECT_EQ(after->characterAnchorPoint(after->charIndexToIterator(i)), anchors[i]);
                    ASSERT_EQ(after->glyphs().size(), glyphs.size());
                    for (std::size_t i = 0; i < glyphs.size(); ++i) {
                        EXPECT_EQ(after->glyphs()[i].glyph, identities[i]);
                        for (unsigned j = 0; j < 6; ++j) EXPECT_EQ(after->glyphs()[i].transform(*after)[j], glyphs[i][j]);
                    }
                    for (auto const &[node, content] : raw) EXPECT_EQ(node->content(), content);
                });
            }
        }
    }
}

TEST_F(StrokeWidthControllerTest, F1RangeCrossesStructuralBreak)
{
    auto document = parse(std::string{svg_open} + R"svg(<text id="t" xml:space="preserve" style="font-size:20px;stroke:black;stroke-width:2"><tspan sodipodi:role="line" x="10" y="30">AB</tspan><tspan sodipodi:role="line" x="10" y="60">CD</tspan></text></svg>)svg");
    ASSERT_TRUE(document); settle(*document);
    auto *owner = item(*document, "t");
    SW::StrokeWidthTextRange range; range.owner = owner; range.first_char = 1; range.last_char = 4;
    auto const before = serialize(*document);
    auto const anchors = SW::query_stroke_widths(*document, {owner}).targets.front().char_baseline;
    auto const plan = SW::prepare_stroke_widths_combined(*document, range, {owner}, absolute(5), 7002);
    ASSERT_EQ(plan.members.front().text_runs.size(), 2u);
    EXPECT_EQ(plan.members.front().text_runs[0].first_char, 1u);
    EXPECT_EQ(plan.members.front().text_runs[1].first_char, 3u);
    wp1_roundtrip(*document, plan, before, [&](auto const &result) {
        ASSERT_EQ(result.state, SW::StrokeWidthApplyState::Applied);
        auto query = SW::query_stroke_widths(*document, {owner});
        ASSERT_EQ(query.targets.front().char_baseline.size(), 5u);
        for (unsigned i : {0u, 1u, 3u, 4u}) EXPECT_DOUBLE_EQ(query.targets.front().char_baseline[i].style.local_computed, i == 1 || i == 3 ? 5 : 2);
        EXPECT_FALSE(query.targets.front().char_baseline[2].authored);
        for (unsigned i = 0; i < anchors.size(); ++i) EXPECT_EQ(query.targets.front().char_baseline[i].anchor, anchors[i].anchor);
    });
}

TEST_F(StrokeWidthWp1cTest, F1SourceThresholdsAndProtectedCascade)
{
    for (int n : {1, 15, 16, 17}) {
        auto document = parse(longtext_doc(n)); ASSERT_TRUE(document); settle(*document);
        auto const before = serialize(*document);
        auto const plan = SW::prepare_stroke_widths(*document, {item(*document, "long")}, absolute(6), 7003);
        wp1_roundtrip(*document, plan, before, [&](auto const &result) {
            ASSERT_EQ(result.state, SW::StrokeWidthApplyState::Applied);
            EXPECT_EQ(result.work.native_range_writes, 0u); EXPECT_EQ(result.work.direct_source_writes, unsigned(n));
            for (double width : local_widths(*document, item(*document, "long"))) EXPECT_DOUBLE_EQ(width, 6);
        });
    }
    auto document = parse(std::string{svg_open} + R"svg(<style>.styled {stroke-width:2;fill:red;font-size:20px}.important {stroke-width:7 !important}</style><text id="t" transform="scale(2)" style="stroke:black;stroke-width:2"><tspan id="a" class="styled" style="stroke-width:2 !important;fill:blue">A</tspan><tspan id="b" style="vector-effect:non-scaling-stroke">B</tspan><tspan id="h" style="display:none">hidden</tspan><tspan id="l" sodipodi:insensitive="true">locked</tspan><tspan id="i" class="important">I</tspan><tref id="ref" xlink:href="#referenced"/></text><text id="referenced">R</text><rect id="r" width="10" height="10" style="stroke:black;stroke-width:2"/></svg>)svg");
    ASSERT_TRUE(document); settle(*document);
    auto const before = serialize(*document);
    std::vector<std::string> protected_xml;
    for (auto id : {"h", "l", "i", "ref", "referenced"}) protected_xml.push_back(sp_repr_write_buf(document->getObjectById(id)->getRepr(), 0, false, GQuark(0), 0, 0).raw());
    for (auto intent : {absolute(4), SW::StrokeWidthIntent{SW::StrokeWidthIntentKind::Hairline, 0, false}}) {
        auto const plan = SW::prepare_stroke_widths(*document, {item(*document, "t"), item(*document, "r")}, intent, 7004);
        wp1_roundtrip(*document, plan, before, [&](auto const &result) {
            ASSERT_EQ(result.state, SW::StrokeWidthApplyState::Applied); EXPECT_EQ(result.changed, 2u);
            auto *a = document->getObjectById("a"); auto *b = document->getObjectById("b");
            EXPECT_DOUBLE_EQ(a->style->stroke_width.computed, intent.kind == SW::StrokeWidthIntentKind::Hairline ? 1 : 2);
            EXPECT_DOUBLE_EQ(b->style->stroke_width.computed, intent.kind == SW::StrokeWidthIntentKind::Hairline ? 1 : 4);
            EXPECT_EQ(a->getRepr()->attribute("class"), std::string("styled"));
            EXPECT_NE(std::string(a->getRepr()->attribute("style")).find("!important"), std::string::npos);
            unsigned i = 0;
            for (auto id : {"h", "l", "i", "ref", "referenced"}) EXPECT_EQ(sp_repr_write_buf(document->getObjectById(id)->getRepr(), 0, false, GQuark(0), 0, 0).raw(), protected_xml[i++]);
        }, plan.skipped_runs);
    }
}

TEST_F(StrokeWidthWp1cTest, F1UnprovedOwnerDoesNotBlockShape)
{
    for (auto text_fixture : {
        R"svg(<text id="t" style="stroke:black;stroke-width:2">A<tspan style="display:none">protected</tspan><tspan>B</tspan></text>)svg",
        R"svg(<text id="t" style="stroke:black;stroke-width:2">A<tref xlink:href="#other"/></text><text id="other">R</text>)svg",
        R"svg(<text id="t" style="stroke:black;stroke-width:2"><tspan>A<tspan/></tspan></text>)svg",
        R"svg(<text id="t" style="stroke:black;stroke-width:2"><tspan>A<title>metadata</title></tspan></text>)svg",
        R"svg(<text id="t" style="stroke:black"><tspan style="stroke-width:2;stroke-dasharray:4 2;stroke-dashoffset:1">A<tspan style="stroke-width:0">B</tspan></tspan></text>)svg"}) {
    auto document = parse(std::string{svg_open} + text_fixture + R"svg(<text id="good" style="font-size:20px;stroke:black;stroke-width:2">OK</text><rect id="r" width="10" height="10" style="stroke:black;stroke-width:2"/></svg>)svg");
    ASSERT_TRUE(document); settle(*document);
    auto const before = serialize(*document);
    auto const text = sp_repr_write_buf(item(*document, "t")->getRepr(), 0, false, GQuark(0), 0, 0).raw();
    auto const plan = SW::prepare_stroke_widths(*document, {item(*document, "t"), item(*document, "good"), item(*document, "r")}, absolute(4, true), 7005);
    if (std::string(text_fixture).find("tref") != std::string::npos) {
        auto const query = SW::query_stroke_widths(*document, {item(*document, "t")});
        ASSERT_TRUE(std::any_of(query.targets.front().runs.begin(), query.targets.front().runs.end(), [](auto const &run) {
            return run.exclusion == SW::StrokeWidthExclusion::UnsupportedTref;
        }));
    }
    wp1_roundtrip(*document, plan, before, [&](auto const &result) {
        ASSERT_EQ(result.state, SW::StrokeWidthApplyState::Applied); EXPECT_EQ(result.changed, 2u); EXPECT_EQ(result.excluded, 1u);
        EXPECT_DOUBLE_EQ(computed_width(*document, "r"), 4);
        EXPECT_DOUBLE_EQ(computed_width(*document, "good"), 4);
        EXPECT_EQ(sp_repr_write_buf(item(*document, "t")->getRepr(), 0, false, GQuark(0), 0, 0).raw(), text);
        EXPECT_NE(SW::stroke_width_applied_note(plan, result).find("direct source coverage"), std::string::npos);
    }, plan.skipped_runs);
    }
}

TEST_F(StrokeWidthControllerTest, F1StyleSelectorSideEffectsRollback)
{
    for (auto rule : {"tspan[style] {fill:red}", "tspan[style] {opacity:0.5}",
                      "tspan[style] {font-size:40px}", "tspan[style] + tspan {stroke-width:9}"}) {
        auto document = parse(std::string{svg_open} + "<style>" + rule + "</style>" +
            R"svg(<text id="t" style="stroke:black;stroke-width:2;font-size:20px"><tspan id="a">A</tspan><tspan id="h" style="display:none">hidden</tspan></text></svg>)svg");
        ASSERT_TRUE(document); settle(*document);
        auto const before = serialize(*document);
        auto const plan = SW::prepare_stroke_widths(*document, {item(*document, "t")}, absolute(4), 7007);
        SingleFireStyleObserver observer(*document->getObjectById("a")->getRepr(), [&] {
            // Force the existing CSS matcher to refresh the layout-absent sibling.
            auto *hidden = document->getObjectById("h");
            hidden->style->readFromObject(hidden);
            if (std::string(rule).find(" + ") != std::string::npos) EXPECT_DOUBLE_EQ(hidden->style->stroke_width.computed, 9);
        });
        wp1_roundtrip(*document, plan, before, [&](auto const &result) {
            EXPECT_EQ(result.state, SW::StrokeWidthApplyState::Failed);
            EXPECT_EQ(result.reason, SW::StrokeWidthMemberReason::PostconditionMismatch);
        });
        observer.detach();
        EXPECT_EQ(serialize(*document), before);
    }
}

// Private input is read in place, never saved/copied. Emit counts only.
TEST_F(StrokeWidthWp1cTest, F1PrivateStrokeProbe)
{
    auto const *path = g_getenv("VA_STROKE_PROBE"); if (!path) return;
    auto document = SPDocument::createNewDoc(path, false); ASSERT_TRUE(document); settle(*document);
    std::vector<SPItem *> texts;
    std::function<void(SPObject *)> collect = [&](SPObject *node) {
        if (auto *text = cast<SPText>(node)) texts.push_back(text);
        for (auto &child : node->children) collect(&child);
    };
    collect(document->getRoot()); ASSERT_EQ(texts.size(), 8u);
    unsigned applied = 0, cases = 0;
    auto const before = serialize(*document);
    auto all = top_items(*document);
    for (auto intent : {absolute(3), relative(200), additive(0.1), absolute(2), SW::StrokeWidthIntent{SW::StrokeWidthIntentKind::Hairline, 0, false}}) {
        for (unsigned i = 0; i <= texts.size(); ++i) {
            for (bool caret : {false, true}) {
                if (i == texts.size() && caret) continue;
                std::vector<SPItem *> selected = i < texts.size() ? std::vector<SPItem *>{texts[i]} : all;
                SW::StrokeWidthTextRange range; range.owner = selected.front(); range.caret = true;
                auto plan = caret ? SW::prepare_stroke_widths_combined(*document, range, selected, intent, 7006)
                                  : SW::prepare_stroke_widths(*document, selected, intent, 7006);
                unsigned planned_texts = 0;
                for (auto const &member : plan.members) if (!member.text_runs.empty()) {
                    ++planned_texts;
                    EXPECT_TRUE(std::any_of(member.text_runs.begin(), member.text_runs.end(), [](auto const &run) {
                        return run.outcome == SW::StrokeWidthMemberOutcome::Change;
                    }));
                }
                EXPECT_EQ(planned_texts, i < texts.size() ? 1u : 8u);
                auto token = SW::begin_stroke_width_interaction(*document, plan); ASSERT_TRUE(token);
                auto result = SW::apply_stroke_widths_compatible(*document, plan, 7006, *token); ++cases;
                EXPECT_EQ(result.state, SW::StrokeWidthApplyState::Applied);
                if (result.state == SW::StrokeWidthApplyState::Applied) { ++applied; EXPECT_TRUE(SW::stroke_widths_compatible_output_ready(*document, plan, 7006, *token)); }
                token->rollback(); document->ensureUpToDate(); EXPECT_TRUE(serialize(*document) == before);
            }
        }
    }
    std::cout << "F1_PRIVATE owners=" << texts.size() << " cases=" << cases << " applied=" << applied << '\n';
    EXPECT_EQ(applied, cases);
}

TEST_F(StrokeWidthControllerTest, F1bCascadeAndLineDashFamilies)
{
    for (auto declarations : {"class=\"w\"", "style=\"stroke-width:2 !important\"", "class=\"important\""}) {
        for (auto intent : {absolute(4, true), absolute(0, true), relative(200, true), additive(1, true), additive(-1, true),
                            SW::StrokeWidthIntent{SW::StrokeWidthIntentKind::Hairline, 0, true},
                            SW::StrokeWidthIntent{SW::StrokeWidthIntentKind::RemoveStroke, 0, true}}) {
            auto document = parse(std::string{svg_open} +
                "<style>.w {stroke-width:2;fill:red}.important {stroke-width:2 !important}</style>"
                "<text id=\"t\" style=\"font-size:20px;stroke:black;stroke-width:2\"><tspan id=\"a\" sodipodi:role=\"line\" x=\"10\" y=\"30\" " + declarations +
                ">AB</tspan><tspan id=\"b\" sodipodi:role=\"line\" x=\"10\" y=\"60\" style=\"stroke-width:3;stroke-dasharray:6 3;stroke-dashoffset:-3\">CD</tspan></text></svg>");
            ASSERT_TRUE(document); settle(*document);
            auto *owner = item(*document, "t"); auto *a = document->getObjectById("a");
            auto const before = serialize(*document);
            auto const raw_a = sp_repr_write_buf(a->getRepr(), 0, false, GQuark(0), 0, 0).raw();
            auto const *layout = te_get_layout(owner);
            std::vector<Geom::Point> anchors;
            for (int i = 0; i < layout->iteratorToCharIndex(layout->end()); ++i) anchors.push_back(layout->characterAnchorPoint(layout->charIndexToIterator(i)));
            bool const excluded = std::string(declarations).find("class=\"important") != std::string::npos &&
                intent.kind != SW::StrokeWidthIntentKind::RemoveStroke;
            auto const plan = SW::prepare_stroke_widths(*document, {owner}, intent, 7010);
            wp1_roundtrip(*document, plan, before, [&](auto const &result) {
                ASSERT_EQ(result.state, SW::StrokeWidthApplyState::Applied);
                auto *b = document->getObjectById("b");
                auto expected = [&](double old) {
                    return intent.kind == SW::StrokeWidthIntentKind::RemoveStroke ? old :
                        intent.kind == SW::StrokeWidthIntentKind::Hairline ? 1 :
                        intent.kind == SW::StrokeWidthIntentKind::RelativePercent ? old * 2 :
                        intent.kind == SW::StrokeWidthIntentKind::AdditiveCssPx ? old + intent.value : intent.value;
                };
                EXPECT_DOUBLE_EQ(a->style->stroke_width.computed, excluded ? 2 : expected(2));
                EXPECT_DOUBLE_EQ(b->style->stroke_width.computed, expected(3));
                double const factor = intent.kind == SW::StrokeWidthIntentKind::RemoveStroke ? 1 : expected(3) / 3;
                if (factor == 0) EXPECT_TRUE(b->style->stroke_dasharray.values.empty());
                else { ASSERT_EQ(b->style->stroke_dasharray.values.size(), 2u);
                EXPECT_DOUBLE_EQ(b->style->stroke_dasharray.values[0].computed, 6 * factor);
                EXPECT_DOUBLE_EQ(b->style->stroke_dasharray.values[1].computed, 3 * factor); }
                EXPECT_DOUBLE_EQ(b->style->stroke_dashoffset.computed, -3 * factor);
                if (excluded) EXPECT_EQ(sp_repr_write_buf(a->getRepr(), 0, false, GQuark(0), 0, 0).raw(), raw_a);
                else if (a->getRepr()->attribute("class")) EXPECT_EQ(a->getRepr()->attribute("class"), std::string(std::string(declarations).find("important") != std::string::npos ? "important" : "w"));
                else if (intent.kind != SW::StrokeWidthIntentKind::RemoveStroke) EXPECT_TRUE(a->style->stroke_width.important);
                auto const *after = te_get_layout(owner);
                for (unsigned i = 0; i < anchors.size(); ++i) EXPECT_EQ(after->characterAnchorPoint(after->charIndexToIterator(i)), anchors[i]);
            }, plan.skipped_runs);
        }
    }
}

TEST_F(StrokeWidthControllerTest, F1bPositionedClassLineRanges)
{
    for (bool blank : {false, true}) for (auto endpoints : {std::pair{1u, 2u}, std::pair{1u, 4u}, std::pair{4u, 1u}}) {
        auto document = parse(std::string{svg_open} + R"svg(<style>.line {fill:red;font-size:20px}.outside {fill:blue}</style><text id="t" xml:space="preserve" style="stroke:black;stroke-width:2"><tspan id="line1" class="line" sodipodi:role="line" x="10" y="30"><tspan id="a" class="outside">A</tspan><tspan id="b" style="fill:green" dy="2.5">B</tspan></tspan>)svg" +
            (blank ? R"svg(<tspan id="blank" sodipodi:role="line" x="10" y="45"/>)svg" : "") +
            R"svg(<tspan id="line2" class="line" sodipodi:role="line" x="10" y="60"><tspan id="c" dy="1.25" style="fill:purple">C</tspan><tspan id="d" class="outside">D</tspan></tspan></text></svg>)svg");
        ASSERT_TRUE(document); settle(*document);
        auto *owner = item(*document, "t"); auto const before = serialize(*document);
        auto const query = SW::query_stroke_widths(*document, {owner});
        auto const baseline = query.targets.front().char_baseline;
        unsigned const last = endpoints.first == 1 && endpoints.second == 2 ? 2 : (blank ? 5 : 4);
        SW::StrokeWidthTextRange range; range.owner = owner;
        range.first_char = endpoints.first > endpoints.second ? last : 1;
        range.last_char = endpoints.first > endpoints.second ? 1 : last;
        std::vector<std::pair<char const *, std::string>> untouched;
        for (auto id : {"a", "d"}) untouched.emplace_back(id, sp_repr_write_buf(document->getObjectById(id)->getRepr(), 0, false, GQuark(0), 0, 0).raw());
        std::vector<std::pair<std::string, std::string>> positions;
        for (auto id : {"line1", "line2"}) {
            auto *node = document->getObjectById(id)->getRepr();
            positions.emplace_back(node->attribute("x"), node->attribute("y"));
        }
        auto const plan = SW::prepare_stroke_widths_combined(*document, range, {owner}, absolute(5), 7011);
        EXPECT_EQ(plan.members.front().target.reversed, endpoints.first > endpoints.second);
        wp1_roundtrip(*document, plan, before, [&](auto const &result) {
            ASSERT_EQ(result.state, SW::StrokeWidthApplyState::Applied);
            auto const after = SW::query_stroke_widths(*document, {owner}).targets.front().char_baseline;
            ASSERT_EQ(after.size(), baseline.size());
            for (unsigned i = 0; i < after.size(); ++i) {
                EXPECT_EQ(after[i].anchor, baseline[i].anchor);
                if (after[i].authored) EXPECT_DOUBLE_EQ(after[i].style.local_computed, i >= 1 && i < last ? 5 : 2);
            }
            for (auto id : {"b", "c"}) {
                auto *source = document->getObjectById(id); ASSERT_TRUE(source);
                EXPECT_EQ(source->getRepr()->attribute("dy"), std::string(id == std::string("b") ? "2.5" : "1.25"));
            }
            for (auto const &[id, xml] : untouched) EXPECT_EQ(sp_repr_write_buf(document->getObjectById(id)->getRepr(), 0, false, GQuark(0), 0, 0).raw(), xml);
            for (auto id : {"line1", "line2"}) {
                auto *object = document->getObjectById(id); ASSERT_TRUE(object);
                auto *node = object->getRepr();
                EXPECT_EQ(node->attribute("class"), std::string("line"));
                auto const index = id == std::string("line1") ? 0 : 1;
                EXPECT_EQ(node->attribute("x"), positions[index].first);
                EXPECT_EQ(node->attribute("y"), positions[index].second);
            }
        });
    }
}

TEST_F(StrokeWidthDifferentialTest, F1bRoleLinePartialDifferential)
{
    auto const svg = doc("", R"svg(<text id="t" xml:space="preserve" style="font-family:Arial;font-size:20px;stroke:black;stroke-width:2"><tspan sodipodi:role="line" x="10" y="30"><tspan>AB</tspan></tspan><tspan sodipodi:role="line" x="10" y="60"><tspan>CD</tspan></tspan></text>)svg");
    auto const output = compare(svg, "t", additive(1), "role=line partial", std::make_pair(0u, 4u));
    EXPECT_GT(output.result.work.native_range_writes, 0u);
    EXPECT_GT(output.result.work.direct_source_writes, 0u);
    for (unsigned i = 0; i < output.after.chars.size(); ++i) {
        if (output.after.chars[i].authored) EXPECT_DOUBLE_EQ(output.after.chars[i].style.local_computed, i < 4 ? 3 : 2);
        EXPECT_EQ(output.after.anchors[i], output.before.anchors[i]);
    }
}

TEST_F(StrokeWidthControllerTest, F1cPositionedClassPartialLeaf)
{
    for (bool reversed : {false, true}) for (bool classed : {false, true}) {
        SCOPED_TRACE(reversed);
        SCOPED_TRACE(classed);
        auto document = parse(std::string{svg_open} +
            R"svg(<style>.line {font-size:20px;fill:red}.leaf {fill:green}</style><text id="t" xml:space="preserve" style="font-size:20px;stroke:black;stroke-width:2"><tspan id="line" sodipodi:role="line" x="10" y="30")svg" +
            (classed ? " class=\"line\"" : "") + R"svg(><tspan id="leaf" x="10" dy="2.5")svg" +
            (classed ? " class=\"leaf\"" : " style=\"fill:green\"") +
            R"svg(>ABC</tspan><tspan id="outside" style="fill:blue">D</tspan></tspan></text></svg>)svg");
        ASSERT_TRUE(document); settle(*document);
        auto *owner = item(*document, "t"); auto const before = serialize(*document);
        std::vector<Geom::Point> anchors;
        std::vector<NativeCharIdentity> identities;
        auto const *layout = te_get_layout(owner); ASSERT_TRUE(layout);
        for (unsigned index = 0; index < 4; ++index) {
            anchors.push_back(layout->characterAnchorPoint(layout->charIndexToIterator(index)));
            identities.push_back(native_char_identity(owner, index));
        }
        auto const outside = sp_repr_write_buf(document->getObjectById("outside")->getRepr(), 0, false, GQuark(0), 0, 0).raw();
        SW::StrokeWidthTextRange range; range.owner = owner;
        range.first_char = reversed ? 2 : 1; range.last_char = reversed ? 1 : 2;
        auto const plan = SW::prepare_stroke_widths_combined(*document, range, {owner}, absolute(5), 7012);
        EXPECT_EQ(plan.members.front().target.reversed, reversed);
        wp1_roundtrip(*document, plan, before, [&](auto const &result) {
            ASSERT_EQ(result.state, SW::StrokeWidthApplyState::Applied);
            EXPECT_GT(result.work.native_range_writes, 0u);
            EXPECT_EQ(sp_te_get_string_multiline(owner), "ABCD");
            auto const *after = te_get_layout(owner); ASSERT_TRUE(after);
            for (unsigned index = 0; index < 4; ++index) {
                EXPECT_EQ(after->characterAnchorPoint(after->charIndexToIterator(index)), anchors[index]);
                auto const identity = native_char_identity(owner, index);
                EXPECT_EQ(identity.character, identities[index].character);
                EXPECT_EQ(identity.fill, identities[index].fill);
                EXPECT_EQ(identity.font_size, identities[index].font_size);
                EXPECT_EQ(identity.font_family, identities[index].font_family);
                auto const observed = native_char_source(owner, index);
                ASSERT_TRUE(is<SPString>(observed.source));
                EXPECT_DOUBLE_EQ(observed.source->parent->style->stroke_width.computed, index == 1 ? 5 : 2);
            }
            for (auto id : {"line", "leaf"}) {
                auto *object = document->getObjectById(id); ASSERT_TRUE(object);
                auto *node = object->getRepr();
                EXPECT_EQ(node->attribute("x"), std::string("10"));
                if (classed) EXPECT_EQ(node->attribute("class"), std::string(id));
                if (std::string(id) == "line") {
                    EXPECT_EQ(node->attribute("y"), std::string("30"));
                    EXPECT_EQ(node->attribute("sodipodi:role"), std::string("line"));
                } else EXPECT_EQ(node->attribute("dy"), std::string("2.5"));
            }
            EXPECT_EQ(sp_repr_write_buf(document->getObjectById("outside")->getRepr(), 0, false, GQuark(0), 0, 0).raw(), outside);
        }); // helper requires exact XML Undo and Redo as well as output readiness
    }
}

TEST_F(StrokeWidthControllerTest, F1dUnanchoredPositionListsPartialRanges)
{
    for (bool reversed : {false, true}) for (bool across_lines : {false, true}) {
        SCOPED_TRACE(reversed);
        SCOPED_TRACE(across_lines);
        auto document = parse(std::string{svg_open} + R"svg(<text id="t" xml:space="preserve" style="font-size:20px;stroke:black;stroke-width:2"><tspan id="line1" sodipodi:role="line" y="30"><tspan id="a" x="10, 25 40" dx="0, 0.25 0.5" dy="2.50, 0 1.25" rotate="0, 5 0">ABC</tspan></tspan><tspan id="line2" sodipodi:role="line" x="10, 25 40" y="60" dy="1.50, 0 0"><tspan id="b" x="10 25,40" dx="0 0.25,0.5" dy="0, 1.25 0" rotate="0 0,10">DEF</tspan></tspan></text></svg>)svg");
        ASSERT_TRUE(document); settle(*document);
        auto *owner = item(*document, "t");
        auto const before = serialize(*document);
        auto const baseline = SW::query_stroke_widths(*document, {owner}).targets.front().char_baseline;
        using Positions = std::array<std::optional<std::string>, 5>;
        auto positions = [](SPObject *object) {
            Positions values;
            unsigned i = 0;
            for (auto name : {"x", "y", "dx", "dy", "rotate"}) {
                if (auto value = object->getRepr()->attribute(name)) values[i] = value;
                ++i;
            }
            return values;
        };
        std::map<std::string, Positions> frozen;
        for (auto id : {"t", "line1", "a", "line2", "b"}) frozen[id] = positions(document->getObjectById(id));
        ASSERT_FALSE(frozen["t"][0]); ASSERT_FALSE(frozen["t"][1]);
        ASSERT_EQ(frozen["line1"][1], std::optional<std::string>{"30"});
        SW::StrokeWidthTextRange range; range.owner = owner;
        unsigned const end = across_lines ? 6 : 2; // B through E, or strict substring B
        range.first_char = reversed ? end : 1; range.last_char = reversed ? 1 : end;
        auto const plan = SW::prepare_stroke_widths_combined(*document, range, {owner}, absolute(5), 7013);
        EXPECT_EQ(plan.members.front().target.reversed, reversed);
        wp1_roundtrip(*document, plan, before, [&](auto const &result) {
            ASSERT_EQ(result.state, SW::StrokeWidthApplyState::Applied);
            EXPECT_GT(result.work.native_range_writes, 0u);
            for (auto const &[id, expected] : frozen) {
                auto *object = document->getObjectById(id); ASSERT_TRUE(object);
                EXPECT_EQ(positions(object), expected) << id;
            }
            auto const after = SW::query_stroke_widths(*document, {owner}).targets.front().char_baseline;
            ASSERT_EQ(after.size(), baseline.size());
            for (unsigned i = 0; i < after.size(); ++i) {
                EXPECT_EQ(after[i].anchor, baseline[i].anchor);
                if (after[i].authored) EXPECT_DOUBLE_EQ(after[i].style.local_computed, i >= 1 && i < end ? 5 : 2);
            }
        }); // exact XML, one Undo/Redo, and independent commit verification
    }
}

TEST_F(StrokeWidthControllerTest, F1dUnrestorablePositionRefusedBeforeMutation)
{
    auto document = parse(std::string{svg_open} + R"svg(<text id="t" style="font-size:20px;stroke:black;stroke-width:2"><tspan dy="1">ABC</tspan><tspan dy="2">DEF</tspan></text></svg>)svg");
    ASSERT_TRUE(document); settle(*document);
    auto *owner = item(*document, "t"); auto const before = serialize(*document);
    SW::StrokeWidthTextRange range; range.owner = owner; range.first_char = 1; range.last_char = 2;
    auto const plan = SW::prepare_stroke_widths_combined(*document, range, {owner}, absolute(5), 7014);
    wp1_roundtrip(*document, plan, before, [&](auto const &result) {
        EXPECT_EQ(result.state, SW::StrokeWidthApplyState::Rejected);
        EXPECT_EQ(result.reason, SW::StrokeWidthMemberReason::UnrestorableTextPosition);
        EXPECT_EQ(result.attempted_writes, 0u);
        EXPECT_EQ(result.work.native_range_writes, 0u);
    });
}

// F5: authored XML (including whitespace rules), geometry and computed output
// are separate oracles. No raw string is reconstructed from logical indices.
TEST_F(StrokeWidthControllerTest, F5CorelCollapsingWholeOwnersAllIntents)
{
    for (auto body : {
        R"svg(<text id="t" x="10.1234" y="30.5678" style="font-family:Arial;font-size:20px;stroke:black;stroke-width:2"><tspan id="a" x="10.1234" dy="0.0000" fill="#112233">  A  B&#9;C&#10;D  </tspan><tspan id="b" x="10.1234" dy="24.5678" fill="#ff0000"> E  F </tspan><tspan id="empty" x="10.1234" dy="24.5678"/></text>)svg",
        R"svg(<text id="t" x="10.1234" y="30.5678" style="font-family:Arial;font-size:20px;stroke:black;stroke-width:2"><tspan xml:space="preserve"><tspan id="a" fill="#112233">  A  B&#9;C </tspan><tspan xml:space="default"><tspan id="b" fill="#ff0000">  D  E&#10;F </tspan></tspan></tspan></text>)svg",
        R"svg(<text id="t" x="10.1234" y="30.5678" style="font-family:Arial;font-size:20px;stroke:black;stroke-width:2"><tspan style="white-space:pre"><tspan id="a" fill="#112233"> A  B&#9;C&#10;D </tspan><tspan style="white-space:normal"><tspan id="b" fill="#ff0000"> E  F </tspan></tspan></tspan></text>)svg"
    }) for (auto intent : {absolute(4), absolute(0), absolute(2), relative(200), relative(100), additive(1), additive(-1), additive(-3),
            SW::StrokeWidthIntent{SW::StrokeWidthIntentKind::Hairline, 0, false},
            SW::StrokeWidthIntent{SW::StrokeWidthIntentKind::RemoveStroke, 0, false}}) {
        for (bool caret : {false, true}) {
            SCOPED_TRACE(body);
            SCOPED_TRACE(int(intent.kind));
            SCOPED_TRACE(intent.value);
            SCOPED_TRACE(caret);
            auto document = parse(std::string{svg_open} + body + "</svg>");
            ASSERT_TRUE(document); settle(*document);
            auto *owner = item(*document, "t");
            auto const before = serialize(*document);
            auto const content = sp_te_get_string_multiline(owner);
            auto const baseline = SW::query_stroke_widths(*document, {owner}).targets.front().char_baseline;
            auto const *layout = te_get_layout(owner); ASSERT_TRUE(layout);
            std::vector<Geom::Affine> glyph_transforms;
            for (auto const &glyph : layout->glyphs()) glyph_transforms.push_back(glyph.transform(*layout));
            std::vector<NativeCharIdentity> identities;
            for (unsigned i = 0; i < baseline.size(); ++i) identities.push_back(native_char_identity(owner, i));
            using Attributes = std::map<std::string, std::string>;
            auto attrs = [](XML::Node *node) {
                Attributes result;
                for (auto const &attr : node->attributeList()) {
                    auto const name = std::string(g_quark_to_string(attr.key));
                    if (name != "style") result[name] = attr.value.pointer();
                }
                return result;
            };
            auto declarations = [&](XML::Node *node) {
                Attributes result;
                auto css = Util::delete_with<sp_repr_css_attr_unref>(sp_repr_css_attr(node, "style"));
                for (auto const &attr : css->attributeList()) {
                    auto const name = std::string(g_quark_to_string(attr.key));
                    bool const allowed = intent.kind == SW::StrokeWidthIntentKind::RemoveStroke ? name == "stroke" :
                        name == "stroke-width" || (intent.kind == SW::StrokeWidthIntentKind::Hairline &&
                        (name == "vector-effect" || name == "-inkscape-stroke"));
                    if (!allowed) result[name] = attr.value.pointer();
                }
                return result;
            };
            struct Frozen { XML::Node *node; Attributes attributes, declarations; std::optional<std::string> raw; };
            std::vector<Frozen> frozen;
            std::function<void(XML::Node *)> capture = [&](XML::Node *node) {
                frozen.push_back({node, attrs(node), declarations(node), node->content() ? std::optional<std::string>{node->content()} : std::nullopt});
                for (auto *child = node->firstChild(); child; child = child->next()) capture(child);
            };
            capture(owner->getRepr());
            SW::StrokeWidthTextRange range; range.owner = owner; range.caret = true;
            auto const plan = caret ? SW::prepare_stroke_widths_combined(*document, range, {owner}, intent, 7050)
                                    : SW::prepare_stroke_widths(*document, {owner}, intent, 7050);
            // The established decrement floor retains width 2 for a step of 3;
            // it does not clamp to zero. Derive this fixture's oracle directly.
            double const expected = intent.kind == SW::StrokeWidthIntentKind::Hairline ? 1 :
                intent.kind == SW::StrokeWidthIntentKind::RemoveStroke ? 2 :
                intent.kind == SW::StrokeWidthIntentKind::RelativePercent ? 2 * intent.value / 100 :
                intent.kind == SW::StrokeWidthIntentKind::AdditiveCssPx ? (intent.value == -3 ? 2 : 2 + intent.value) : intent.value;
            bool const changing = expected != 2 || intent.kind == SW::StrokeWidthIntentKind::RemoveStroke;
            wp1_roundtrip(*document, plan, before, [&](auto const &result) {
                ASSERT_EQ(result.state, changing ? SW::StrokeWidthApplyState::Applied : SW::StrokeWidthApplyState::Unchanged);
                EXPECT_EQ(result.changed, changing ? 1u : 0u); EXPECT_EQ(result.excluded, 0u);
                EXPECT_EQ(result.work.native_range_writes, 0u);
                EXPECT_EQ(sp_te_get_string_multiline(owner), content);
                for (auto const &f : frozen) {
                    EXPECT_EQ(attrs(f.node), f.attributes);
                    EXPECT_EQ(declarations(f.node), f.declarations);
                    EXPECT_EQ(f.node->content() ? std::optional<std::string>{f.node->content()} : std::nullopt, f.raw);
                }
                auto reopened = parse(serialize(*document)); ASSERT_TRUE(reopened);
                unsigned raw_index = 0;
                std::function<void(XML::Node *)> check_reopened = [&](XML::Node *node) {
                    ASSERT_LT(raw_index, frozen.size()); auto const &f = frozen[raw_index++];
                    EXPECT_EQ(attrs(node), f.attributes); EXPECT_EQ(declarations(node), f.declarations);
                    EXPECT_EQ(node->content() ? std::optional<std::string>{node->content()} : std::nullopt, f.raw);
                    for (auto *child = node->firstChild(); child; child = child->next()) check_reopened(child);
                };
                check_reopened(item(*reopened, "t")->getRepr()); EXPECT_EQ(raw_index, frozen.size());
                auto const *after_layout = te_get_layout(owner); ASSERT_TRUE(after_layout);
                ASSERT_EQ(after_layout->glyphs().size(), glyph_transforms.size());
                for (unsigned i = 0; i < glyph_transforms.size(); ++i)
                    for (unsigned j = 0; j < 6; ++j) EXPECT_EQ(after_layout->glyphs()[i].transform(*after_layout)[j], glyph_transforms[i][j]);
                for (auto id : {"a", "b"}) {
                    auto *source = document->getObjectById(id); ASSERT_TRUE(source);
                    EXPECT_DOUBLE_EQ(source->style->stroke_width.computed, expected);
                    if (intent.kind == SW::StrokeWidthIntentKind::Hairline) EXPECT_TRUE(source->style->stroke_extensions.hairline);
                    if (intent.kind == SW::StrokeWidthIntentKind::RemoveStroke) EXPECT_TRUE(source->style->stroke.isNone());
                }
                auto const after = SW::query_stroke_widths(*document, {owner}).targets.front().char_baseline;
                ASSERT_EQ(after.size(), baseline.size());
                for (unsigned i = 0; i < after.size(); ++i) {
                    EXPECT_EQ(after[i].anchor, baseline[i].anchor);
                    EXPECT_EQ(after[i].glyph_index, baseline[i].glyph_index);
                    auto const identity = native_char_identity(owner, i);
                    EXPECT_EQ(identity.character, identities[i].character);
                    EXPECT_EQ(identity.fill, identities[i].fill);
                    EXPECT_EQ(identity.font_family, identities[i].font_family);
                    EXPECT_EQ(identity.font_size, identities[i].font_size);
                }
            });
        }
    }
}

TEST_F(StrokeWidthControllerTest, F5CollapsingRangesSafeAndUnsafeBothDirections)
{
    struct Case { char const *text; unsigned first, last; bool safe, direct; };
    for (auto c : {Case{"  A  B&#9;C&#10;D  ", 1, 2, false, false},
                   Case{"A B", 1, 2, false, false}, // raw == normalized; split removes the space
                   Case{"ABC", 1, 2, true, false},
                   Case{"  A  B&#9;C&#10;D  ", 0, 6, true, true}}) {
        for (bool reversed : {false, true}) {
            SCOPED_TRACE(c.text);
            SCOPED_TRACE(reversed);
            auto document = parse(std::string{svg_open} +
                R"svg(<text id="t" x="10.1234" y="30.5678" style="font-family:Arial;font-size:20px;stroke:black;stroke-width:2"><tspan id="a" x="10.1234" fill="#112233">)svg" + c.text +
                R"svg(</tspan><tspan id="b" x="10.1234" dy="24.5678" fill="#ff0000">XYZ</tspan></text></svg>)svg");
            ASSERT_TRUE(document); settle(*document);
            auto *owner = item(*document, "t"); auto const before = serialize(*document);
            auto const content = sp_te_get_string_multiline(owner);
            auto const baseline = SW::query_stroke_widths(*document, {owner}).targets.front().char_baseline;
            auto const outside = sp_repr_write_buf(document->getObjectById("b")->getRepr(), 0, false, GQuark(0), 0, 0).raw();
            SW::StrokeWidthTextRange range; range.owner = owner;
            range.first_char = reversed ? c.last : c.first; range.last_char = reversed ? c.first : c.last;
            auto const plan = SW::prepare_stroke_widths_combined(*document, range, {owner}, absolute(5), 7051);
            EXPECT_EQ(plan.members.front().target.reversed, reversed);
            if (!c.safe) {
                EXPECT_TRUE(std::any_of(plan.members.front().text_runs.begin(), plan.members.front().text_runs.end(), [](auto const &run) {
                    return run.reason == SW::StrokeWidthMemberReason::UnsafeTextWhitespace;
                }));
                SW::StrokeWidthApplyResult none; none.excluded = 1;
                EXPECT_NE(SW::stroke_width_applied_note(plan, none).find("partial range could rewrite raw text or change collapsed whitespace"), std::string::npos);
            }
            wp1_roundtrip(*document, plan, before, [&](auto const &result) {
                EXPECT_EQ(result.state, c.safe ? SW::StrokeWidthApplyState::Applied : SW::StrokeWidthApplyState::Unchanged);
                if (!c.safe) { EXPECT_EQ(result.attempted_writes, 0u); return; }
                EXPECT_EQ(result.work.native_range_writes == 0, c.direct);
                EXPECT_EQ(sp_te_get_string_multiline(owner), content);
                EXPECT_EQ(sp_repr_write_buf(document->getObjectById("b")->getRepr(), 0, false, GQuark(0), 0, 0).raw(), outside);
                auto const after = SW::query_stroke_widths(*document, {owner}).targets.front().char_baseline;
                ASSERT_EQ(after.size(), baseline.size());
                for (unsigned i = 0; i < after.size(); ++i) {
                    EXPECT_EQ(after[i].anchor, baseline[i].anchor);
                    EXPECT_DOUBLE_EQ(after[i].style.local_computed, i >= c.first && i < c.last ? 5 : 2);
                }
            }, c.safe ? 0 : 1);
        }
    }
}

TEST_F(StrokeWidthControllerTest, F5RefusalKeepsRedoAndIndependentShapeApplies)
{
    for (bool shape : {false, true}) {
        auto document = parse(std::string{svg_open} + R"svg(<text id="t" x="10" y="30" style="stroke:black;stroke-width:2">  A  B  </text><rect id="r" width="10" height="10" style="stroke:black;stroke-width:2"/></svg>)svg");
        ASSERT_TRUE(document); settle(*document);
        auto *owner = item(*document, "t"); auto *rect = item(*document, "r");
        rect->setAttribute("data-seed", "redo");
        DocumentUndo::done(document.get(), Util::Internal::ContextString("F5 seed"), "");
        auto const redo = serialize(*document);
        ASSERT_TRUE(DocumentUndo::undo(document.get()));
        document->setModifiedSinceSave(false);
        auto const before = serialize(*document);
        auto const text_xml = sp_repr_write_buf(owner->getRepr(), 0, false, GQuark(0), 0, 0).raw();
        SW::StrokeWidthTextRange range; range.owner = owner; range.first_char = 1; range.last_char = 2;
        auto const plan = SW::prepare_stroke_widths_combined(*document, range, shape ? std::vector<SPItem *>{owner, rect} : std::vector<SPItem *>{owner}, absolute(5), 7052);
        wp1_roundtrip(*document, plan, before, [&](auto const &result) {
            EXPECT_EQ(result.state, shape ? SW::StrokeWidthApplyState::Applied : SW::StrokeWidthApplyState::Unchanged);
            EXPECT_EQ(result.changed, shape ? 1u : 0u); EXPECT_EQ(result.excluded, 1u);
            EXPECT_EQ(result.work.native_range_writes, 0u);
            EXPECT_EQ(sp_repr_write_buf(owner->getRepr(), 0, false, GQuark(0), 0, 0).raw(), text_xml);
            if (shape) EXPECT_DOUBLE_EQ(rect->style->stroke_width.computed, 5);
        }, 1);
        if (!shape) {
            EXPECT_FALSE(DocumentUndo::undo(document.get()));
            ASSERT_TRUE(DocumentUndo::redo(document.get()));
            EXPECT_EQ(serialize(*document), redo);
        }
    }
}

TEST_F(StrokeWidthControllerTest, F5NestedWhitespaceRangesAndNumericConversion)
{
    for (auto rule : {"xml:space=\"preserve\"", "style=\"white-space:pre\""}) for (bool complete : {false, true}) for (bool reversed : {false, true}) {
        auto document = parse(std::string{svg_open} + R"svg(<text id="t" x="10.1234" y="30.5678" style="font-family:Arial;font-size:20px;stroke:black;stroke-width:2"><tspan id="a" x="10.1234" )svg" + rule +
            R"svg(>A  B</tspan><tspan id="b" x="10.1234" dy="24.5678">XYZ</tspan></text></svg>)svg");
        ASSERT_TRUE(document); settle(*document);
        auto *owner = item(*document, "t"); auto const before = serialize(*document);
        auto const raw = document->getObjectById("a")->getRepr()->firstChild()->content();
        std::string const raw_text = raw;
        unsigned const start = complete ? 0 : 1, end = complete ? 4 : 2;
        SW::StrokeWidthTextRange range; range.owner = owner;
        range.first_char = reversed ? end : start; range.last_char = reversed ? start : end;
        auto const plan = SW::prepare_stroke_widths_combined(*document, range, {owner}, absolute(5), 7053);
        wp1_roundtrip(*document, plan, before, [&](auto const &result) {
            EXPECT_EQ(result.state, complete ? SW::StrokeWidthApplyState::Applied : SW::StrokeWidthApplyState::Unchanged);
            EXPECT_EQ(result.work.native_range_writes, 0u);
            EXPECT_EQ(document->getObjectById("a")->getRepr()->firstChild()->content(), raw_text);
            EXPECT_EQ(document->getObjectById("a")->getRepr()->attribute("x"), std::string("10.1234"));
        }, complete ? 0 : 1);
        // Existing hairline converted to ordinary numeric width, still in place.
        if (complete) {
            auto *source = document->getObjectById("a");
            source->setAttribute("style", "stroke-width:1;vector-effect:non-scaling-stroke;-inkscape-stroke:hairline;white-space:pre");
            document->ensureUpToDate(); settle(*document);
            auto const numeric_before = serialize(*document);
            auto const numeric = SW::prepare_stroke_widths(*document, {owner}, absolute(4), 7054);
            wp1_roundtrip(*document, numeric, numeric_before, [&](auto const &result) {
                ASSERT_EQ(result.state, SW::StrokeWidthApplyState::Applied);
                EXPECT_EQ(result.work.native_range_writes, 0u);
                EXPECT_FALSE(source->style->stroke_extensions.hairline);
                EXPECT_FALSE(source->style->vector_effect.stroke);
                EXPECT_DOUBLE_EQ(source->style->stroke_width.computed, 4);
                EXPECT_EQ(source->getRepr()->firstChild()->content(), raw_text);
            });
        }
    }
}
