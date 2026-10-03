// SPDX-License-Identifier: GPL-2.0-or-later
#include <gtest/gtest.h>
#include <algorithm>
#include <memory>
#include <thread>
#include "bitmap-adjustment-chemistry.h"
#include "desktop.h"
#include "document.h"
#include "document-undo.h"
#include "inkscape.h"
#include "inkscape-application.h"
#include "object/sp-image.h"
#include "object/sp-filter.h"
#include "inkgc/gc-core.h"
#include "object/filters/sp-filter-primitive.h"
#include "style.h"
#include "object/sp-root.h"
#include "selection.h"
#include "ui/explode-bitmap-target.h"
#include "xml/node.h"
#include "xml/repr.h"
using namespace Inkscape;
using namespace Inkscape::Bitmap;
namespace {
constexpr auto png = "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP4z8DwHwAFAAH/iZk9HQAAAABJRU5ErkJggg==";
std::string image(char const *id = "im") {
    return std::string("<image id='") + id + "' width='10' height='10' href='" + png + "'/>";
}
class ExplodeBitmapTargetTest : public ::testing::Test {
protected:
    void SetUp() override {
        static auto *app = [] {
            g_setenv("INKSCAPE_APP_ID_TAG", "explodebitmaptargettest", TRUE);
            return new InkscapeApplication;
        }();
        ASSERT_TRUE(app->gtk_app());
        if (!Application::exists()) Application::create(false);
    }
    void open(std::string const &body) {
        desktop.reset();
        document = SPDocument::createNewDocFromMem("<svg xmlns='http://www.w3.org/2000/svg' xmlns:xlink='http://www.w3.org/1999/xlink' xmlns:inkscape='http://www.inkscape.org/namespaces/inkscape' xmlns:sodipodi='http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd' width='40' height='30'>" + body + "</svg>");
        ASSERT_TRUE(document); document->ensureUpToDate();
        desktop = std::make_unique<SPDesktop>(document->getNamedView());
        document->setModifiedSinceSave(false);
    }
    SPItem *item(char const *id) { return cast<SPItem>(document->getObjectById(id)); }
    void select(char const *id) { ASSERT_TRUE(item(id)); desktop->getSelection()->set(item(id)); }
    Result<TargetSnapshot> query() { return resolve(*desktop, Intent::Explode); }
    void reason(Result<TargetSnapshot> const &r, Refusal code) {
        EXPECT_FALSE(r.ok()); EXPECT_EQ(r.value.supportability, Supportability::Refused);
        EXPECT_TRUE(std::any_of(r.value.refusals.begin(), r.value.refusals.end(), [=](auto const &x) { return x.reason == code && *x.diagnostic; }));
    }
    std::unique_ptr<SPDocument> document;
    std::unique_ptr<SPDesktop> desktop;
};
TEST_F(ExplodeBitmapTargetTest, IdenticalSelectionAndCompatibleToolsPreserveCapture) {
    open(image()); select("im"); auto t = query().value; ASSERT_TRUE(valid(t, *document));
    auto selected = desktop->getSelection()->items_vector();
    desktop->getSelection()->addList(selected); EXPECT_TRUE(valid(t, *document));
    for (auto tool : {"/tools/zoom", "/tools/select", "/tools/shapes/rect", "/tools/select"}) {
        desktop->setTool(tool); EXPECT_TRUE(valid(t, *document)) << tool;
    }
    desktop->setTool("/tools/text"); EXPECT_FALSE(valid(t, *document));
    desktop->setTool("/tools/select"); EXPECT_FALSE(valid(t, *document));
}
TEST_F(ExplodeBitmapTargetTest, SelectionDepartureAndReturnInvalidatesCapture) {
    open(image()); select("im"); auto t = query().value;
    auto selected = desktop->getSelection()->items_vector();
    desktop->getSelection()->clear(); desktop->getSelection()->addList(selected);
    EXPECT_FALSE(valid(t, *document));
}
TEST_F(ExplodeBitmapTargetTest, T26SingleBitmapReadOnlyAndContexts) {
    open("<g id='g' transform='translate(3,4)'>" + image() + "</g>"); select("im");
    auto before = sp_repr_save_buf(document->getReprDoc()).raw();
    auto r = query(); ASSERT_TRUE(r.ok()) << r.outcome.diagnostic;
    EXPECT_EQ(r.value.mode, TargetMode::SingleBitmap); EXPECT_EQ(r.value.supportability, Supportability::Supported);
    EXPECT_EQ(r.value.roots.size(), 1u); EXPECT_NE(r.value.bitmap, 0u); EXPECT_TRUE(valid(r.value, *document));
    EXPECT_EQ(r.value.destinationParent, reinterpret_cast<std::uintptr_t>(item("g")));
    EXPECT_EQ(r.value.destinationSlot, 0u);
    EXPECT_EQ(r.value.contexts.front().itemToDocument[4], 3); EXPECT_EQ(r.value.contexts.front().itemToDocument[5], 4);
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before); EXPECT_FALSE(document->isModifiedSinceSave());
}
TEST_F(ExplodeBitmapTargetTest, T26GroupsNestedAndParentChildRoots) {
    open("<g id='outer'><g id='inner'>" + image() + "</g></g>");
    for (auto id : {"outer", "inner"}) {
        select(id); desktop->getSelection()->add(item("im"));
        ASSERT_EQ(desktop->getSelection()->items_vector().size(), 1u); // native single-add protects the parent
        auto ordinary = query(); ASSERT_TRUE(ordinary.ok()); EXPECT_EQ(ordinary.value.covered, 0u);
        EXPECT_EQ(ordinary.value.supportability, Supportability::ConversionRequired);
        desktop->getSelection()->clear();
        std::vector<SPItem *> overlap{item(id), item("im")};
        desktop->getSelection()->add(overlap.begin(), overlap.end()); // native bulk API preserves overlap
        ASSERT_EQ(desktop->getSelection()->items_vector().size(), 2u);
        auto r = query(); ASSERT_TRUE(r.ok()) << r.outcome.diagnostic;
        EXPECT_EQ(r.value.roots.size(), 1u); EXPECT_EQ(r.value.mode, TargetMode::CollectiveConversion);
        EXPECT_EQ(r.value.covered, 1u); EXPECT_EQ(r.value.roots.front(), reinterpret_cast<std::uintptr_t>(item(id)));
        EXPECT_EQ(r.value.supportability, Supportability::ConversionRequired); EXPECT_EQ(r.value.bitmap, 0u);
        ASSERT_FALSE(r.value.conversionReasons.empty()); EXPECT_NE(r.value.conversionReasons.front().find("Undo"), std::string::npos);
    }
}
TEST_F(ExplodeBitmapTargetTest, T26MultipleMixedVectorAndCloneUnits) {
    open(image() + image("two") + "<rect id='v' width='5' height='5'/><use id='clone' href='#im'/>");
    select("two"); desktop->getSelection()->add(item("v")); auto mixed = query(); ASSERT_TRUE(mixed.ok()) << mixed.outcome.diagnostic;
    EXPECT_EQ(mixed.value.roots.size(), 2u); EXPECT_EQ(mixed.value.mode, TargetMode::CollectiveConversion);
    EXPECT_EQ(mixed.value.supportability, Supportability::ConversionRequired);
    select("v"); auto vector = query(); ASSERT_TRUE(vector.ok());
    EXPECT_EQ(vector.value.supportability, Supportability::ConversionRequired);
    select("clone"); auto clone = query(); EXPECT_TRUE(clone.ok()) << clone.outcome.diagnostic;
    EXPECT_EQ(clone.value.mode, TargetMode::CollectiveConversion);
    EXPECT_EQ(clone.value.supportability, Supportability::ConversionRequired);
    select("im"); desktop->getSelection()->add(item("two")); reason(query(), Refusal::HrefReference); // live clone blocks replacement
}
TEST_F(ExplodeBitmapTargetTest, T26ContiguousBitmapsAndUnsafePlacement) {
    open("<g id='g'><rect id='before' width='1' height='1'/>" + image() + image("two") +
         "<rect id='after' width='1' height='1'/></g>");
    select("im"); desktop->getSelection()->add(item("two"));
    auto before = sp_repr_save_buf(document->getReprDoc()).raw();
    auto r = query(); ASSERT_TRUE(r.ok()); EXPECT_EQ(r.value.supportability, Supportability::ConversionRequired);
    EXPECT_EQ(r.value.destinationParent, reinterpret_cast<std::uintptr_t>(item("g")));
    EXPECT_EQ(r.value.destinationSlot, 1u); EXPECT_EQ(r.value.roots.size(), 2u);
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before);
    open(image() + "<rect id='v' width='5' height='5'/>" + image("two"));
    select("im"); desktop->getSelection()->add(item("two")); reason(query(), Refusal::Interleaved);
    open("<g>" + image() + "</g><g>" + image("two") + "</g>");
    select("im"); desktop->getSelection()->add(item("two")); reason(query(), Refusal::CrossParent);
}
TEST_F(ExplodeBitmapTargetTest, T26ResourceAndTextEditingRefusal) {
    open("<defs><clipPath id='clip'>" + image() + "</clipPath></defs>"); select("im"); reason(query(), Refusal::ResourceTarget);
    open(image()); select("im"); auto before = query(); desktop->setTool("/tools/text");
    reason(query(), Refusal::TextTool); EXPECT_FALSE(valid(before.value, *document));
    desktop->setTool("/tools/select"); desktop->getSelection()->clear(); reason(query(), Refusal::EmptySelection);
}
TEST_F(ExplodeBitmapTargetTest, T24ProtectedAncestorsAndDescendants) {
    for (auto const &[attribute, code] : std::initializer_list<std::pair<char const *, Refusal>>{
        {"style='display:none'", Refusal::Hidden}, {"sodipodi:insensitive='true'", Refusal::Locked},
        {"style='opacity:0'", Refusal::ZeroOpacity},
        {"style='mix-blend-mode:multiply'", Refusal::AncestorEffect}, {"style='isolation:isolate'", Refusal::AncestorEffect}}) {
        open(std::string("<g id='g' ") + attribute + ">" + image() + "</g>"); select("im"); reason(query(), code);
    }
    open("<g id='g'><g style='display:none'>" + image() + "</g></g>"); select("g"); reason(query(), Refusal::Hidden);
}
TEST_F(ExplodeBitmapTargetTest, T24OwnAndAncestorEffects) {
    auto defs = "<defs><clipPath id='c'><rect width='5' height='5'/></clipPath><mask id='m'><rect width='5' height='5'/></mask><filter id='f'><feGaussianBlur stdDeviation='1'/></filter></defs>";
    for (auto const &[style, code] : std::initializer_list<std::pair<char const *, Refusal>>{
        {"mask:url(#m)", Refusal::OwnMask}, {"filter:url(#f)", Refusal::OwnFilter}}) {
        open(defs + image()); select("im"); item("im")->getRepr()->setAttribute("style", style); reason(query(), code);
    }
    open(defs + image()); select("im"); item("im")->getRepr()->setAttribute("style", "clip-path:url(#c)");
    auto clipped = query(); ASSERT_TRUE(clipped.ok());
    EXPECT_EQ(clipped.value.supportability, Supportability::Supported);
    for (auto effect : {"clip-path:url(#c)", "mask:url(#m)", "filter:url(#f)"}) {
        open(std::string(defs) + "<g style='" + effect + "'>" + image() + "</g>");
        select("im"); reason(query(), Refusal::AncestorEffect);
    }
    open("<defs><clipPath id='c'><text>unsafe</text></clipPath></defs>" + image()); select("im");
    item("im")->getRepr()->setAttribute("style", "clip-path:url(#c)"); reason(query(), Refusal::UnsupportedClip);
}
TEST_F(ExplodeBitmapTargetTest, T24CanonicalToneAndMalformedGraph) {
    open(image()); select("im"); Filters::BitmapToneSettings tone; tone.brightness = 0.25;
    ASSERT_TRUE(BitmapAdjustments::apply_tone(item("im"), tone)); document->ensureUpToDate();
    auto r = query(); ASSERT_TRUE(r.ok()) << r.outcome.diagnostic;
    EXPECT_EQ(r.value.supportability, Supportability::Supported);
    EXPECT_EQ(r.value.contexts.front().tone.brightness, 0.25);
    auto primitive = BitmapAdjustments::find_managed_tone_primitive(item("im")->style->getFilter());
    primitive->getRepr()->firstChild()->setAttribute("tableValues", "0 1"); reason(query(), Refusal::MalformedTone);
    EXPECT_FALSE(valid(r.value, *document));
}
TEST_F(ExplodeBitmapTargetTest, T16EveryReferenceKind) {
    for (auto const &[reference, code] : std::initializer_list<std::pair<char const *, Refusal>>{
        {"<use href='#im'/>", Refusal::HrefReference},
        {"<defs><pattern id='p'><use href='#im'/></pattern></defs>", Refusal::HrefReference},
        {"<defs><clipPath id='c'><use href='#im'/></clipPath></defs>", Refusal::HrefReference},
        {"<defs><mask id='m'><use href='#im'/></mask></defs>", Refusal::HrefReference},
        {"<rect style='fill:url(#im)'/>", Refusal::UrlReference},
        {"<style>#im { opacity:0.5 }</style>", Refusal::CssDependency},
        {"<style>image { opacity:0.5 }</style>", Refusal::CssDependency},
        {"<rect aria-labelledby='im'/>", Refusal::IdDependency},
        {"<rect custom-ref='#im'/>", Refusal::IdDependency}}) {
        open(image() + reference); select("im"); auto before = sp_repr_save_buf(document->getReprDoc()).raw();
        reason(query(), code); EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before);
    }
    open("<g id='g'>" + image() + "</g><use href='#g'/>"); select("im"); reason(query(), Refusal::CloneReference);
    open("<g id='g'>" + image() + "</g><use href='#im'/>"); select("g"); reason(query(), Refusal::HrefReference);
}
TEST_F(ExplodeBitmapTargetTest, EmbeddedOnlyIntakeAndConversionDependencies) {
    open(image()); select("im"); item("im")->getRepr()->setAttribute("href", "external.png"); reason(query(), Refusal::LinkedSource);
    item("im")->getRepr()->setAttribute("href", "data:image/png;base64,AAAA"); reason(query(), Refusal::InvalidIntake);
    item("im")->getRepr()->removeAttribute("href"); reason(query(), Refusal::MissingSource);
    open("<g id='g'><image width='5' height='5' href='external.png'/></g>"); select("g"); reason(query(), Refusal::LinkedSource);
}
TEST_F(ExplodeBitmapTargetTest, StalenessXmlSelectionRevertDeletionAndLifetime) {
    open(image() + "<rect id='v' width='2' height='2'/>"); select("im"); auto s = query().value;
    EXPECT_TRUE(valid(s, *document)); item("im")->getRepr()->setAttribute("opacity", "0.5");
    item("im")->getRepr()->removeAttribute("opacity"); EXPECT_FALSE(valid(s, *document));
    s = query().value; select("v"); select("im"); EXPECT_FALSE(valid(s, *document));
    s = query().value; document->getReprRoot()->setAttribute("viewBox", "0 0 80 60"); EXPECT_FALSE(valid(s, *document));
    s = query().value; item("im")->deleteObject(); EXPECT_FALSE(valid(s, *document));
    s = query().value; desktop.reset(); EXPECT_FALSE(valid(s, *document));
}
TEST_F(ExplodeBitmapTargetTest, T24MalformedToneInputAlphaAndExtraPrimitive) {
    for (int variant = 0; variant < 7; ++variant) {
        open(image()); select("im"); Filters::BitmapToneSettings tone; tone.contrast = 0.2;
        ASSERT_TRUE(BitmapAdjustments::apply_tone(item("im"), tone));
        auto p = BitmapAdjustments::find_managed_tone_primitive(item("im")->style->getFilter())->getRepr();
        if (variant == 0) p->setAttribute("in", "SourceAlpha");
        if (variant == 1) p->lastChild()->setAttribute("type", "linear");
        if (variant == 2) p->setAttribute("inkscape:contrast", "1000");
        if (variant == 3) {
            auto extra = document->getReprDoc()->createElement("svg:feGaussianBlur");
            p->parent()->appendChild(extra); GC::release(extra);
        }
        if (variant == 4) p->setAttribute("x", "0.5");
        if (variant == 5) p->setAttribute("result", "SourceGraphic");
        if (variant == 6) p->setAttribute("style", "color-interpolation-filters:linearRGB");
        reason(query(), Refusal::MalformedTone);
    }
}
TEST_F(ExplodeBitmapTargetTest, T24UnqualifiedResourcesAndSingularTransform) {
    open(image()); select("im"); item("im")->getRepr()->setAttribute("transform", "scale(0)");
    reason(query(), Refusal::InvalidTransform);
    open("<defs><pattern id='p' width='1' height='1'>" + image() + "</pattern></defs><rect id='v' width='5' height='5' fill='url(#p)'/>");
    select("v"); reason(query(), Refusal::ResourceDependency);
    open(image()); select("im"); item("im")->getRepr()->setAttribute("clip-path", "url(#missing)");
    reason(query(), Refusal::UnsupportedClip);
}
TEST_F(ExplodeBitmapTargetTest, T16PercentEncodedAndPrefixIds) {
    open(image() + "<use href='#%69m'/>"); select("im"); reason(query(), Refusal::HrefReference);
    open(image() + "<rect custom-ref='#im-other #im'/>"); select("im"); reason(query(), Refusal::IdDependency);
    open(image() + "<rect custom-ref='#im-other'/>"); select("im");
    auto prefix = query(); ASSERT_TRUE(prefix.ok()); EXPECT_EQ(prefix.value.supportability, Supportability::Supported);
}
TEST_F(ExplodeBitmapTargetTest, StalenessSameIdReplacementAndDocumentBinding) {
    open(image()); select("im"); auto s = query().value;
    item("im")->getRepr()->setAttribute("id", "other");
    item("other")->getRepr()->setAttribute("id", "im"); EXPECT_FALSE(valid(s, *document));
    s = query().value;
    auto oldIdentity = s.bitmap;
    // Keep the old representation alive to prevent allocator address reuse.
    auto oldObject = item("im"); sp_object_ref(oldObject);
    auto oldRepr = oldObject->getRepr(); GC::anchor(oldRepr);
    auto newRepr = oldRepr->duplicate(document->getReprDoc());
    item("im")->deleteObject(); document->getReprRoot()->appendChild(newRepr);
    GC::release(newRepr); GC::release(oldRepr); document->ensureUpToDate(); select("im");
    EXPECT_FALSE(valid(s, *document));
    auto current = query(); ASSERT_TRUE(current.ok()); EXPECT_NE(current.value.bitmap, oldIdentity);
    sp_object_unref(oldObject);
    s = current.value;
    auto replacement = SPDocument::createNewDocFromMem("<svg xmlns='http://www.w3.org/2000/svg'/> ");
    ASSERT_TRUE(replacement); EXPECT_FALSE(valid(s, *replacement));
    desktop->setDocument(replacement.get()); desktop->setDocument(document.get());
    EXPECT_FALSE(valid(s, *document));
}
TEST_F(ExplodeBitmapTargetTest, StalenessResourceAndMainThreadOnly) {
    open(image() + "<defs><clipPath id='c'><rect id='r' width='5' height='5'/></clipPath></defs>"); select("im");
    auto s = query().value; item("r")->getRepr()->setAttribute("width", "3"); EXPECT_FALSE(valid(s, *document));
    Result<TargetSnapshot> worker; bool fresh = true;
    std::thread thread([&] { worker = query(); fresh = valid(s, *document); }); thread.join();
    reason(worker, Refusal::MainThreadOnly); EXPECT_FALSE(fresh);
}
TEST_F(ExplodeBitmapTargetTest, StalenessNativeTransformStyleAndAncestor) {
    open("<g id='g'>" + image() + "</g>"); select("im"); document->ensureUpToDate();
    for (auto id : {"im", "g"}) {
        auto s = query().value; ASSERT_TRUE(valid(s, *document));
        auto before = sp_repr_save_buf(document->getReprDoc()).raw();
        auto original = item(id)->i2dt_affine();
        item(id)->set_i2d_affine(original * Geom::Translate(2, 3));
        EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before);
        EXPECT_FALSE(valid(s, *document)); // before deferred modified delivery
        item(id)->set_i2d_affine(original); document->ensureUpToDate();
        EXPECT_FALSE(valid(s, *document)); // native change-and-revert still invalid
    }
    auto s = query().value; auto before = sp_repr_save_buf(document->getReprDoc()).raw();
    item("im")->style->opacity.read("0.5");
    item("im")->requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG | SP_OBJECT_STYLE_MODIFIED_FLAG);
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before); EXPECT_FALSE(valid(s, *document));
    document->ensureUpToDate(); EXPECT_FALSE(valid(s, *document));
    s = query().value;
    // Direct native signal delivery also invalidates, without pending XML edits.
    item("g")->emitModified(SP_OBJECT_STYLE_MODIFIED_FLAG); EXPECT_FALSE(valid(s, *document));
}
TEST_F(ExplodeBitmapTargetTest, T24VisibilityAndLockedDescendants) {
    for (auto visibility : {"hidden", "collapse"}) {
        auto style = std::string("visibility:") + visibility;
        open(image()); select("im"); item("im")->getRepr()->setAttribute("style", style.c_str());
        document->ensureUpToDate(); reason(query(), Refusal::Hidden);
        open("<g id='g' style='" + style + "'>" + image() + "</g>"); select("im"); reason(query(), Refusal::Hidden);
        open("<g id='g'>" + image() + "</g>"); select("g");
        item("im")->getRepr()->setAttribute("style", style.c_str()); document->ensureUpToDate(); reason(query(), Refusal::Hidden);
    }
    open("<g id='g'><g sodipodi:insensitive='true'>" + image() + "</g></g>");
    select("g"); reason(query(), Refusal::Locked);
}
TEST_F(ExplodeBitmapTargetTest, T24RetainedAncestorOpacityVersusConversion) {
    open("<g id='outer' opacity='0.5'><g id='inner' opacity='0.5'>" + image() + "</g></g>"); select("im");
    item("im")->getRepr()->setAttribute("opacity", "0.5"); document->ensureUpToDate();
    auto before = sp_repr_save_buf(document->getReprDoc()).raw();
    auto r = query(); ASSERT_TRUE(r.ok()) << r.outcome.diagnostic;
    EXPECT_EQ(r.value.supportability, Supportability::Supported);
    EXPECT_EQ(r.value.effectiveOpacity, 0.125); EXPECT_EQ(r.value.retainedAncestorOpacity, 0.25);
    EXPECT_EQ(r.value.contexts.front().opacity, 0.5);
    EXPECT_EQ(r.value.destinationParent, reinterpret_cast<std::uintptr_t>(item("inner")));
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before);
    select("inner"); reason(query(), Refusal::AncestorEffect); // opacity would be baked/lifted
    select("outer"); auto collective = query(); ASSERT_TRUE(collective.ok());
    EXPECT_EQ(collective.value.supportability, Supportability::ConversionRequired); // own opacity rendered once
}
TEST_F(ExplodeBitmapTargetTest, T24EnclosingToneFilterQualification) {
    for (auto const &[attribute, value] : {std::pair{"inkscape:auto-region", "false"},
            std::pair{"x", "0.5"}, std::pair{"y", "0.5"}, std::pair{"width", "0.1"},
            std::pair{"height", "0.1"}, std::pair{"filterUnits", "userSpaceOnUse"},
            std::pair{"primitiveUnits", "objectBoundingBox"}, std::pair{"filterRes", "1 1"},
            std::pair{"href", "#other"}, std::pair{"xlink:href", "#other"},
            std::pair{"style", "color-interpolation-filters:linearRGB"}, std::pair{"x", "NaN"}}) {
        SCOPED_TRACE(attribute);
        SCOPED_TRACE(value);
        open(image()); select("im"); Filters::BitmapToneSettings tone; tone.brightness = 0.25;
        ASSERT_TRUE(BitmapAdjustments::apply_tone(item("im"), tone)); document->ensureUpToDate();
        auto canonical = query(); ASSERT_TRUE(canonical.ok());
        EXPECT_EQ(canonical.value.supportability, Supportability::Supported);
        auto filter = item("im")->style->getFilter()->getRepr(); filter->setAttribute(attribute, value);
        // Do not let an automatic region recomputation conceal the modified graph.
        auto before = sp_repr_save_buf(document->getReprDoc()).raw(); reason(query(), Refusal::MalformedTone);
        EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before);
    }
}
TEST_F(ExplodeBitmapTargetTest, T24CroppingSliceVersusFullSourceViewport) {
    // Square 1x1 source, rectangular viewport: slice removes half the source rows.
    for (auto alignment : {"xMinYMin", "xMidYMid", "xMaxYMax"}) {
        open(image()); select("im"); auto repr = item("im")->getRepr();
        repr->setAttribute("width", "20");
        repr->setAttribute("preserveAspectRatio", (std::string(alignment) + " slice").c_str());
        document->ensureUpToDate(); reason(query(), Refusal::CroppingSlice);
        repr->setAttribute("height", "20"); document->ensureUpToDate();
        auto full = query(); ASSERT_TRUE(full.ok()); EXPECT_EQ(full.value.supportability, Supportability::Supported);
        repr->setAttribute("height", "10"); repr->setAttribute("preserveAspectRatio", (std::string(alignment) + " meet").c_str());
        document->ensureUpToDate(); auto meet = query(); ASSERT_TRUE(meet.ok());
        EXPECT_EQ(meet.value.supportability, Supportability::Supported);
    }
}
TEST_F(ExplodeBitmapTargetTest, NoneIgnoresSliceKeywordAndStretches) {
    open(image()); select("im"); auto repr = item("im")->getRepr();
    repr->setAttribute("width", "20"); repr->setAttribute("height", "10");
    repr->setAttribute("preserveAspectRatio", "none slice"); document->ensureUpToDate();
    auto full = query(); ASSERT_TRUE(full.ok());
    EXPECT_EQ(full.value.supportability, Supportability::Supported);
}
TEST_F(ExplodeBitmapTargetTest, T16ExactWhitespaceIdLists) {
    for (auto attribute : {"aria-labelledby", "aria-describedby", "aria-controls", "aria-owns", "aria-flowto", "headers"}) {
        open(image() + "<rect " + attribute + "='im-other prefix-im x#im %69m'/>"); select("im");
        auto r = query(); ASSERT_TRUE(r.ok()); EXPECT_EQ(r.value.supportability, Supportability::Supported);
        auto rect = document->getReprRoot()->lastChild(); rect->setAttribute(attribute, "other\tim\nlast");
        reason(query(), Refusal::IdDependency);
    }
}
} // namespace
