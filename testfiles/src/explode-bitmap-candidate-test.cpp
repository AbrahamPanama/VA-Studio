// SPDX-License-Identifier: GPL-2.0-or-later
#include <gtest/gtest.h>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include "message-stack.h"
#include "ui/icon-names.h"
#include <memory>
#include <stdexcept>
#include "bitmap-copy-outcome.h"
#include "desktop.h"
#include "document.h"
#include "display/cairo-utils.h"
#include "helper/pixbuf-ops.h"
#include "inkgc/gc-core.h"
#include "inkscape.h"
#include "inkscape-application.h"
#include "object/object-set.h"
#include "object/sp-image.h"
#include "object/sp-lpe-item.h"
#include "object/sp-namedview.h"
#include "object/sp-root.h"
#include "object/sp-use.h"
#include "selection.h"
#include "selection-chemistry.h"
#include "style.h"
#include "svg/svg.h"
#include "util/bitmap-memory-admission.h"
#include "util/scope_exit.h"
#include "util/units.h"
#include "xml/repr.h"
#include <glibmm/i18n.h>
using namespace Inkscape;
using namespace Inkscape::Bitmap;
namespace {
char const *png="data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP4z8DwHwAFAAH/iZk9HQAAAABJRU5ErkJggg==";
std::string rect(char const *id, int x=0) { return "<rect id='"+std::string(id)+"' x='"+std::to_string(x)+"' width='12' height='8' fill='#36c'/>"; }
std::string bitmap(double width=10) { return "<image id='im' width='"+std::to_string(width)+"' height='10' preserveAspectRatio='none' href='"+png+"'/>"; }
std::unique_ptr<SPDocument> document(std::string const &body) {
    auto doc=SPDocument::createNewDocFromMem("<svg xmlns='http://www.w3.org/2000/svg' xmlns:xlink='http://www.w3.org/1999/xlink' width='80' height='60'>"+body+"</svg>");
    doc->ensureUpToDate(); return doc;
}
struct Room : MemoryProbe { bool read(RawMemory &m) const noexcept override { m={8ull*1024*MiB,6ull*1024*MiB,128*MiB}; return true; } } room;
struct Missing : MemoryProbe { bool read(RawMemory &) const noexcept override { return false; } } missing;
cairo_surface_t *nullSurface(int,int) { return nullptr; }
cairo_surface_t *badSurface(int,int) { return cairo_image_surface_create(CAIRO_FORMAT_ARGB32,-1,-1); }
cairo_surface_t *oomSurface(int,int) { throw std::bad_alloc(); }
void failInsert() { throw std::bad_alloc(); }
SPDocument *callbackDocument=nullptr;
void commitDuringInsert() { DocumentUndo::done(callbackDocument,Util::Internal::ContextString("Callback commit"),""); }
void unrelatedEdit() { callbackDocument->getObjectById("outside")->getRepr()->setAttribute("fill","red"); }
bool failEncode(char const *stage) { return std::strcmp(stage,"png")!=0; }
class CandidateTest : public ::testing::Test {
protected:
    void SetUp() override {
        static auto app=[] { g_setenv("INKSCAPE_APP_ID_TAG","eb2candidate",TRUE); return new InkscapeApplication; }();
        ASSERT_TRUE(app->gtk_app()); if (!Application::exists()) Application::create(false);
    }
    void open(std::string const &body, std::vector<char const *> const &ids) {
        desktop.reset(); doc=document(body); desktop=std::make_unique<SPDesktop>(doc->getNamedView());
        for (auto id:ids) desktop->getSelection()->add(doc->getObjectById(id));
        DocumentUndo::setUndoSensitive(doc.get(),true);
        DocumentUndo::done(doc.get(),Util::Internal::ContextString("Fixture"),""); DocumentUndo::clearUndo(doc.get());
        doc->ensureUpToDate(); doc->setModifiedSinceSave(false);
    }
    BitmapCopyOutcome prepare(CandidateHooks hooks={&room}) { return prepareBitmapCopy(*desktop->getSelection(),{},PlacementPolicy::ContiguousReplacement,budget,hooks); }
    std::string xml() { return sp_repr_save_buf(doc->getReprDoc()).raw(); }
    std::unique_ptr<SPDocument> doc;
    std::unique_ptr<SPDesktop> desktop;
    Budget budget{1536*MiB};
};
TEST_F(CandidateTest,T26ReadOnlyGroupAndMultipleMixedPlacementUndo) {
    for (auto body : {"<g id='a'>"+rect("child")+"</g>"+rect("b",20),bitmap()+rect("b",20)}) {
        open(rect("before",50)+body+rect("after",60),{body.starts_with("<g") ? "a" : "im","b"});
        auto before=xml(); auto selection=desktop->getSelection()->items_vector();
        auto out=prepare(); ASSERT_TRUE(out.ok()) << out.outcome.diagnostic; ASSERT_TRUE(out.candidate.pixels());
        EXPECT_EQ(xml(),before); EXPECT_FALSE(doc->isModifiedSinceSave()); EXPECT_EQ(desktop->getSelection()->items_vector(),selection);
        auto const &m=out.candidate.metadata(); EXPECT_EQ(m.renderDpi,body.starts_with("<g") ? 300 : 9.6);
        EXPECT_GT(m.width,0u); EXPECT_NEAR(m.dpiX,m.width*25.4/m.widthMm,1e-10); EXPECT_EQ(m.token.roots.size(),2u);
        EXPECT_EQ(publishBitmapCopy(*desktop->getSelection(),out.candidate).outcome.status,Status::unavailable);
        EXPECT_EQ(xml(),before);
        auto token=DocumentUndo::beginAtomicInteraction(doc.get()); ASSERT_TRUE(token);
        auto published=publishBitmapCopy(*desktop->getSelection(),out.candidate,&*token); ASSERT_TRUE(published.ok()) << published.outcome.diagnostic;
        auto image=cast<SPImage>(desktop->getSelection()->single()); ASSERT_TRUE(image); EXPECT_EQ(published.image,reinterpret_cast<std::uintptr_t>(image));
        EXPECT_EQ(image->getRepr()->position(),m.slot); EXPECT_EQ(image->pixbuf->width(),int(m.width)); EXPECT_EQ(image->pixbuf->height(),int(m.height));
        auto matrix=image->c2p * image->i2doc_affine();
        EXPECT_NEAR(96/std::hypot(matrix[0],matrix[1]),m.dpiX,1e-9); EXPECT_NEAR(96/std::hypot(matrix[2],matrix[3]),m.dpiY,1e-9);
        auto rgba=out.candidate.rgba(); ASSERT_TRUE(rgba.validate().ok());
        std::unique_ptr<Pixbuf> decoded(Pixbuf::create_from_data_uri(image->getRepr()->attribute("xlink:href")+5));
        ASSERT_TRUE(decoded); decoded->ensurePixelFormat(Pixbuf::PF_GDK); auto actual=decoded->getPixbufRaw();
        ASSERT_EQ(gdk_pixbuf_get_n_channels(actual),4);
        for (unsigned row=0;row<m.height;++row) EXPECT_EQ(std::memcmp(rgba.data+row*rgba.stride,gdk_pixbuf_get_pixels(actual)+row*gdk_pixbuf_get_rowstride(actual),4*m.width),0);
        EXPECT_TRUE(token->commitAtomically(Util::Internal::ContextString("Convert selection to bitmap"),"",[] { return true; }));
        EXPECT_EQ(publishBitmapCopy(*desktop->getSelection(),out.candidate).outcome.status,Status::unavailable);
        DocumentUndo::undo(doc.get()); EXPECT_EQ(xml(),before); // one Undo restores editable roots
        DocumentUndo::redo(doc.get()); EXPECT_TRUE(cast<SPImage>(doc->getObjectById(published.imageId.c_str())));
    }
}
TEST_F(CandidateTest,T26MultipleBitmapsUseHighestNativeDpi) {
    open(bitmap()+"<image id='b' x='20' width='5' height='10' href='"+png+"'/>",{"im","b"}); auto before=xml(); auto out=prepare(); ASSERT_TRUE(out.ok()) << out.outcome.diagnostic;
    EXPECT_EQ(out.candidate.metadata().token.roots.size(),2u); EXPECT_NEAR(out.candidate.metadata().renderDpi,19.2,1e-10); EXPECT_EQ(xml(),before);
}
TEST_F(CandidateTest,T26TransformedParentPlacementUndo) {
    open("<g id='parent' transform='matrix(0,1,-1,.2,30,10)'>"+rect("a")+rect("b",20)+"</g>",{"a","b"});
    auto before=xml(); auto bounds=desktop->getSelection()->documentBounds(SPItem::VISUAL_BBOX); ASSERT_TRUE(bounds);
    auto out=prepare(); ASSERT_TRUE(out.ok()) << out.outcome.diagnostic; auto const &m=out.candidate.metadata();
    auto token=DocumentUndo::beginAtomicInteraction(doc.get()); ASSERT_TRUE(token);
    auto published=publishBitmapCopy(*desktop->getSelection(),out.candidate,&*token); ASSERT_TRUE(published.ok()) << published.outcome.diagnostic;
    auto image=cast<SPImage>(desktop->getSelection()->single()); ASSERT_TRUE(image);
    EXPECT_EQ(image->parent,doc->getObjectById("parent")); EXPECT_EQ(image->getRepr()->position(),m.slot);
    auto matrix=image->c2p*image->i2doc_affine();
    EXPECT_NEAR(matrix[0],96/m.dpiX,1e-10); EXPECT_NEAR(matrix[3],96/m.dpiY,1e-10);
    EXPECT_NEAR(matrix[1],0,1e-10); EXPECT_NEAR(matrix[2],0,1e-10);
    EXPECT_NEAR(matrix[4],bounds->left(),1e-10); EXPECT_NEAR(matrix[5],bounds->top(),1e-10);
    ASSERT_TRUE(token->commitAtomically(Util::Internal::ContextString("Convert selection to bitmap"),"",[] { return true; }));
    DocumentUndo::undo(doc.get()); EXPECT_EQ(xml(),before);
}
TEST_F(CandidateTest,T26RefusesInterleavedCrossParentAndAncestorEffects) {
    for (auto body : {rect("a")+rect("gap",15)+rect("b",30),"<g>"+rect("a")+"</g><g>"+rect("b",20)+"</g>","<g opacity='.5'>"+rect("a")+rect("b",20)+"</g>"}) {
        open(body,{"a","b"}); auto before=xml(); auto out=prepare(); EXPECT_FALSE(out.ok()); EXPECT_FALSE(out.candidate.pixels()); EXPECT_EQ(xml(),before); EXPECT_FALSE(doc->isModifiedSinceSave());
    }
}
TEST_F(CandidateTest,T26DpiFloorClampAndSkewUsesMinimumSingularValue) {
    open(rect("a"),{"a"}); auto v=prepare(); ASSERT_TRUE(v.ok()); EXPECT_EQ(v.candidate.metadata().renderDpi,300);
    v={};
    open("<g transform='matrix(1,0,2,1,0,0)'>"+bitmap(.1)+"</g>",{"im"}); auto b=prepare(); ASSERT_TRUE(b.ok()) << b.outcome.diagnostic;
    auto m=b.candidate.metadata(); EXPECT_GT(m.requestedDpi,960); EXPECT_EQ(m.renderDpi,600); EXPECT_TRUE(m.clamped); EXPECT_STRNE(m.resolutionReason,"");
    // Pixel affine is [0.1,0;20,10]. Independent eigenvalues of M^T M.
    double trace=500.01,det=.01*100;
    double small=2*det/(trace+std::sqrt(trace*trace-4*det));
    EXPECT_NEAR(m.requestedDpi,96/std::sqrt(small),1e-6);
    b={}; open(bitmap()+"<use id='clone' xlink:href='#im' transform='scale(.1)'/>",{"clone"});
    auto clone=prepare(); ASSERT_TRUE(clone.ok()) << clone.outcome.diagnostic; EXPECT_NEAR(clone.candidate.metadata().requestedDpi,96,1e-8);
}
TEST_F(CandidateTest,T18RenderFailuresLeaveXmlSelectionAndLedgerUntouched) {
    for(auto surface:{nullSurface,badSurface,oomSurface}) {
        open(rect("a"),{"a"}); auto before=xml(); auto selected=desktop->getSelection()->items_vector();
        auto out=prepare({&room,surface}); EXPECT_FALSE(out.ok()); EXPECT_FALSE(out.candidate.pixels()); EXPECT_EQ(xml(),before);
        EXPECT_EQ(desktop->getSelection()->items_vector(),selected); EXPECT_EQ(budget.reserved(),0u); EXPECT_FALSE(doc->isModifiedSinceSave());
        auto later=prepare(); EXPECT_TRUE(later.ok()) << later.outcome.diagnostic;
    }
}
TEST_F(CandidateTest,T15AdmissionEncodeAndStaleFailuresPreserveOriginals) {
    open(rect("a"),{"a"}); auto before=xml(); auto out=prepare({&missing}); EXPECT_FALSE(out.ok()); EXPECT_EQ(xml(),before); EXPECT_EQ(budget.reserved(),0u);
    sp_image_set_allocation_hook(failEncode); out=prepare(); sp_image_set_allocation_hook(nullptr);
    EXPECT_FALSE(out.ok()); EXPECT_EQ(xml(),before); EXPECT_EQ(budget.reserved(),0u);
    out=prepare(); ASSERT_TRUE(out.ok()); doc->getObjectById("a")->getRepr()->setAttribute("fill","red"); doc->ensureUpToDate();
    DocumentUndo::done(doc.get(),Util::Internal::ContextString("Other edit"),""); auto changed=xml();
    auto token=DocumentUndo::beginAtomicInteraction(doc.get()); ASSERT_TRUE(token);
    EXPECT_FALSE(publishBitmapCopy(*desktop->getSelection(),out.candidate,&*token).ok()); EXPECT_EQ(xml(),changed); token->rollback();
}
TEST_F(CandidateTest,T15PublicationFailureRollsBackXmlAndSelection) {
    open(rect("a")+rect("b",20),{"a","b"}); auto before=xml(); auto out=prepare({&room,nullptr,nullptr,failInsert}); ASSERT_TRUE(out.ok());
    auto token=DocumentUndo::beginAtomicInteraction(doc.get()); ASSERT_TRUE(token);
    EXPECT_FALSE(publishBitmapCopy(*desktop->getSelection(),out.candidate,&*token).ok()); EXPECT_EQ(xml(),before);
    EXPECT_EQ(desktop->getSelection()->items_vector().size(),2u); EXPECT_FALSE(doc->isModifiedSinceSave());
}

TEST_F(CandidateTest,T15UnexpectedCallbackEditRejectsSettlement) {
    open(rect("a")+rect("outside",30),{"a"}); auto before=xml(); callbackDocument=doc.get();
    auto out=prepare({&room,nullptr,nullptr,unrelatedEdit}); ASSERT_TRUE(out.ok()); auto token=DocumentUndo::beginAtomicInteraction(doc.get()); ASSERT_TRUE(token);
    EXPECT_FALSE(publishBitmapCopy(*desktop->getSelection(),out.candidate,&*token).ok()); EXPECT_EQ(xml(),before); EXPECT_FALSE(doc->isModifiedSinceSave()); callbackDocument=nullptr;
}
TEST_F(CandidateTest,T15SelectionCallbackDeletingRootRollsBack) {
    for (auto id:{"a","b"}) {
        open(rect("a")+rect("b",20),{"a","b"}); auto before=xml();
        auto out=prepare(); ASSERT_TRUE(out.ok());
        auto token=DocumentUndo::beginAtomicInteraction(doc.get()); ASSERT_TRUE(token);
        bool called=false;
        auto connection=desktop->getSelection()->connectChanged([&](Selection *selection) {
            if (!called && selection->isEmpty()) {
                called=true;
                doc->getObjectById(id)->deleteObject(true,true);
            }
        });
        auto published=publishBitmapCopy(*desktop->getSelection(),out.candidate,&*token);
        connection.disconnect();
        EXPECT_TRUE(called); EXPECT_EQ(published.outcome.status,Status::failed);
        EXPECT_EQ(xml(),before); EXPECT_FALSE(doc->isModifiedSinceSave());
        auto selected=desktop->getSelection()->items_vector(); ASSERT_EQ(selected.size(),2u);
        EXPECT_STREQ(selected[0]->getId(),"a"); EXPECT_STREQ(selected[1]->getId(),"b");
        EXPECT_FALSE(token->active());
    }
}
TEST_F(CandidateTest,T15NonAtomicGuardIsRefused) {
    open(rect("a")+rect("b",20),{"a","b"}); auto before=xml();
    auto out=prepare(); ASSERT_TRUE(out.ok());
    auto preview=DocumentUndo::beginRollbackableInteraction(doc.get()); ASSERT_TRUE(preview);
    ASSERT_TRUE(preview->validFor(doc.get())); // generic ownership alone is insufficient
    EXPECT_EQ(publishBitmapCopy(*desktop->getSelection(),out.candidate,&*preview).outcome.status,Status::unavailable);
    EXPECT_EQ(xml(),before); EXPECT_TRUE(preview->active()); preview->rollback();
}
TEST_F(CandidateTest,T15InsertionCallbackCannotCommitConversion) {
    open(rect("a")+rect("b",20),{"a","b"}); auto before=xml();
    callbackDocument=doc.get();
    auto out=prepare({&room,nullptr,nullptr,commitDuringInsert}); ASSERT_TRUE(out.ok());
    auto token=DocumentUndo::beginAtomicInteraction(doc.get()); ASSERT_TRUE(token);
    EXPECT_EQ(publishBitmapCopy(*desktop->getSelection(),out.candidate,&*token).outcome.status,Status::failed);
    callbackDocument=nullptr;
    EXPECT_EQ(xml(),before); EXPECT_FALSE(doc->isModifiedSinceSave()); EXPECT_FALSE(token->active());
    EXPECT_EQ(desktop->getSelection()->items_vector().size(),2u);
    DocumentUndo::undo(doc.get()); EXPECT_EQ(xml(),before); // no partial conversion history
    DocumentUndo::redo(doc.get()); EXPECT_EQ(xml(),before);
}
TEST_F(CandidateTest,T26EveryProtectedAndUnavailableObjectIsReported) {
    open("<g id='locked' xmlns:sodipodi='http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd' sodipodi:insensitive='true'>"+
         rect("a")+"</g><rect id='hidden' width='4' height='4' style='display:none'/>"
         "<image id='missing' width='4' height='4'/><image id='linked' width='4' height='4' href='/unavailable-eb2.png'/>",
         {"locked","hidden","missing","linked"});
    auto before=xml(); auto selected=desktop->getSelection()->items_vector();
    auto out=prepare(); EXPECT_FALSE(out.ok()); EXPECT_FALSE(out.candidate.pixels());
    for (auto expected:std::vector<std::pair<char const *,Refusal>>{
             {"locked",Refusal::Locked},{"hidden",Refusal::Hidden},
             {"missing",Refusal::MissingSource},{"linked",Refusal::LinkedSource}}) {
        auto reason=std::find_if(out.refusals.begin(),out.refusals.end(),[&](auto const &r) {
            return r.id==expected.first && r.reason==expected.second;
        });
        ASSERT_NE(reason,out.refusals.end()) << expected.first;
        EXPECT_EQ(reason->identity,reinterpret_cast<std::uintptr_t>(doc->getObjectById(expected.first)));
        EXPECT_STRNE(reason->diagnostic,"");
    }
    auto target=resolve(*desktop,Intent::ConversionCandidate);
    ASSERT_EQ(out.refusals.size(),target.value.refusals.size());
    for (unsigned k=0;k<out.refusals.size();++k) {
        EXPECT_EQ(out.refusals[k].id,target.value.refusals[k].id);
        EXPECT_EQ(out.refusals[k].reason,target.value.refusals[k].reason);
    }
    EXPECT_EQ(xml(),before); EXPECT_EQ(desktop->getSelection()->items_vector(),selected);
    EXPECT_FALSE(doc->isModifiedSinceSave());
}
TEST_F(CandidateTest,T26NormalizedOverlapAndCloneOnly) {
    open("<g id='g'>"+rect("a")+"</g>"+rect("b",20),{});
    auto set=desktop->getSelection(); std::vector<SPItem *> roots{cast<SPItem>(doc->getObjectById("g")),cast<SPItem>(doc->getObjectById("a")),cast<SPItem>(doc->getObjectById("b"))};
    set->add(roots.begin(),roots.end()); auto before=xml(); auto out=prepare(); ASSERT_TRUE(out.ok()) << out.outcome.diagnostic;
    EXPECT_EQ(out.candidate.metadata().token.roots.size(),2u); EXPECT_EQ(out.candidate.metadata().token.covered,1u); EXPECT_EQ(xml(),before);
    out={};
    open(rect("source")+"<use id='clone' xlink:href='#source' x='20'/>",{"clone"}); before=xml(); out=prepare(); ASSERT_TRUE(out.ok()) << out.outcome.diagnostic;
    EXPECT_EQ(xml(),before); auto token=DocumentUndo::beginAtomicInteraction(doc.get()); ASSERT_TRUE(token);
    auto published=publishBitmapCopy(*desktop->getSelection(),out.candidate,&*token); ASSERT_TRUE(published.ok()); EXPECT_TRUE(doc->getObjectById("source")); EXPECT_FALSE(doc->getObjectById("clone")); token->rollback();
}
TEST_F(CandidateTest,T15FailureAndEmptyCandidatePreserveRedo) {
    open(rect("a"),{"a"}); doc->getObjectById("a")->getRepr()->setAttribute("fill","red");
    DocumentUndo::done(doc.get(),Util::Internal::ContextString("Red"),""); DocumentUndo::undo(doc.get()); doc->ensureUpToDate();
    auto before=xml(); { auto out=prepare({&room,nullSurface}); EXPECT_FALSE(out.ok()); EXPECT_EQ(xml(),before); }
    DocumentUndo::redo(doc.get()); EXPECT_STREQ(doc->getObjectById("a")->getRepr()->attribute("fill"),"red");
    desktop->getSelection()->clear(); before=xml(); auto out=prepare(); EXPECT_EQ(out.outcome.status,Status::unchanged); EXPECT_FALSE(out.candidate.pixels()); EXPECT_EQ(xml(),before);
}

// Pre-change options implementation from HEAD 7604d83a3, with only ObjectSet receiver syntax adapted.
void sp_selection_delete_impl(std::vector<SPItem*> const &items) {
    for (auto item:items) sp_object_ref(item,nullptr);
    for (auto item:items) { item->deleteObject(true,true); sp_object_unref(item,nullptr); }
}
static bool has_rendering_effect(SPItem const *item) {
    if (item->isFiltered() || item->getClipObject() || item->getMaskObject()) return true;
    if (auto lpe=cast<SPLPEItem>(item); lpe && lpe->hasPathEffect()) return true;
    return item->style && item->style->opacity.value < SP_SCALE24_MAX;
}
void preChange(ObjectSet &set, BitmapCopyOptions const &options)
{
    SPDocument *doc = set.document();
    Inkscape::XML::Document *xml_doc = doc->getReprDoc();
    if (set.isEmpty()) {
        if (set.desktop())
            set.desktop()->messageStack()->flash(Inkscape::WARNING_MESSAGE, _("Select <b>object(s)</b> to make a bitmap copy."));
        return;
    }
    if (set.desktop()) {
        set.desktop()->messageStack()->flash(Inkscape::IMMEDIATE_MESSAGE, _("Rendering bitmap..."));
        set.desktop()->setWaitingCursor();
    }
    auto clear_cursor = scope_exit([&set] {
        if (set.desktop())
            set.desktop()->clearWaitingCursor();
    });
    doc->ensureUpToDate();
    Geom::OptRect bbox = set.documentBounds(SPItem::VISUAL_BBOX);
    if (!bbox) {
        return;
    }
    auto items_range = set.items();
    auto items_vec = std::vector<SPItem const *>(items_range.begin(), items_range.end());
    std::sort(items_vec.begin(), items_vec.end(), sp_item_repr_compare_position_bool);
    SPItem const *topmost = items_vec.back();
    SPObject *parent_object = topmost->parent;
    Inkscape::XML::Node *after = const_cast<Inkscape::XML::Node *>(topmost->getRepr());
    for (SPObject *ancestor = topmost->parent; ancestor && ancestor != doc->getRoot(); ancestor = ancestor->parent) {
        if (auto const *ancestor_item = cast<SPItem>(ancestor); ancestor_item && has_rendering_effect(ancestor_item)) {
            parent_object = ancestor->parent;
            after = ancestor->getRepr();
        }
    }
    Inkscape::XML::Node *parent = parent_object->getRepr();
    double const res = std::clamp(options.dpi, 1, BitmapCopyOptions::max_dpi);
    if (res == Inkscape::Util::Quantity::convert(1, "in", "px")) {
        bbox = bbox->roundOutwards();
    }
    std::optional<Antialiasing> antialias;
    if (!options.antialias) {
        antialias = Antialiasing::None;
    } else if (auto nv = doc->getNamedView(); nv && !nv->antialias_rendering) {
        antialias = Antialiasing::None;
    }
    std::unique_ptr<Inkscape::Pixbuf> pb(sp_generate_internal_bitmap(doc, *bbox, res, items_vec, false, nullptr, 1, antialias));
    if (!pb) {
        if (set.desktop())
            set.desktop()->messageStack()->flash(Inkscape::ERROR_MESSAGE, _("Could not render the bitmap copy (the image may be too large)."));
        return;
    }
    if (!options.transparent) {
        auto *source = pb->getSurfaceRaw();
        int const w = cairo_image_surface_get_width(source);
        int const h = cairo_image_surface_get_height(source);
        auto *white = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
        if (cairo_surface_status(white) != CAIRO_STATUS_SUCCESS) {
            cairo_surface_destroy(white);
            if (set.desktop())
                set.desktop()->messageStack()->flash(Inkscape::ERROR_MESSAGE, _("Could not render the bitmap copy (the image may be too large)."));
            return;
        }
        auto *cr = cairo_create(white);
        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_paint(cr);
        cairo_set_source_surface(cr, source, 0, 0);
        cairo_paint(cr);
        cairo_destroy(cr);
        pb = std::make_unique<Inkscape::Pixbuf>(white);
    }
    Inkscape::XML::Node *repr = xml_doc->createElement("svg:image");
    sp_embed_image(repr, pb.get());
    repr->setAttributeSvgDouble("width", bbox->width());
    repr->setAttributeSvgDouble("height", bbox->height());
    auto parentItem = cast<SPItem>(parent_object);
    Geom::Affine affine = Geom::Translate(bbox->left(), bbox->top()) * parentItem->i2doc_affine().inverse();
    repr->setAttributeOrRemoveIfEmpty("transform", sp_svg_transform_write(affine));
    parent->addChild(repr, after);
    if (!options.keep_original) {
        std::vector<SPItem *> originals(items_range.begin(), items_range.end());
        auto const clone_depth = [](SPItem *item) {
            int depth = 0;
            for (auto *use = cast<SPUse>(item); use && depth < 1000; use = cast<SPUse>(use->get_original())) {
                ++depth;
            }
            return depth;
        };
        std::stable_sort(originals.begin(), originals.end(),
                         [&](SPItem *a, SPItem *b) { return clone_depth(a) > clone_depth(b); });
        set.clear();
        sp_selection_delete_impl(originals);
    }
    set.clear();
    set.add(repr);
    Inkscape::GC::release(repr);
    if (options.commit_undo) {
        DocumentUndo::done(doc, RC_("Undo", "Create bitmap"), INKSCAPE_ICON("selection-make-bitmap-copy"));
    }
}
TEST_F(CandidateTest,NativeEmbeddedBytesAndAttributesMatchPreChange) {
    int comparisons=0;
    for(auto body:{rect("a")+rect("b",20),"<g id='a' transform='translate(2,3)'>"+rect("c")+"</g>"+rect("b",20),bitmap()+rect("b",20),"<g opacity='.5'>"+rect("a")+rect("b",20)+"</g>"}) {
        for(int dpi:{96,300,600}) for(bool transparent:{false,true}) for(bool keep:{false,true}) {
            auto old=document(body),now=document(body); ObjectSet a(old.get()),b(now.get());
            auto first=body.starts_with("<image") ? "im" : "a";
            a.add(old->getObjectById(first)); a.add(old->getObjectById("b"));
            b.add(now->getObjectById(first)); b.add(now->getObjectById("b"));
            BitmapCopyOptions options; options.dpi=dpi; options.transparent=transparent; options.keep_original=keep; options.antialias=dpi!=300;
            preChange(a,options); b.createBitmapCopy(options);
            auto x=cast<SPImage>(a.single()),y=cast<SPImage>(b.single()); ASSERT_TRUE(x); ASSERT_TRUE(y);
            EXPECT_STREQ(x->getRepr()->attribute("xlink:href"),y->getRepr()->attribute("xlink:href"));
            for(auto attr:{"width","height","transform"}) EXPECT_STREQ(x->getRepr()->attribute(attr),y->getRepr()->attribute(attr));
            EXPECT_EQ(x->getRepr()->position(),y->getRepr()->position()); EXPECT_EQ(x->parent->getId()!=nullptr,y->parent->getId()!=nullptr);
            ++comparisons;
        }
    }
    EXPECT_EQ(comparisons,48);
}
// These grids are fixed from the authored decimal boundary: 5.44 * 25/8 = 17,
// 2.72 * 25/4 = 17. Adjacent decimal fixtures lie below/above that boundary.
// No production unit, metadata or renderer helper computes these expectations.
struct FractionalFixture { char const *side; int dpi; unsigned pixels; };
FractionalFixture const fractional[]={{"5.439999999999999",300,17},{"5.44",300,17},
    {"5.440000000000001",300,18},{"2.72",600,17},{"2.720000000000001",600,18}};
std::string fractionalRect(char const *side) {
    return "<rect id='a' width='"+std::string(side)+"' height='"+side+"' fill='#36c'/>";
}
void expectPngGrid(SPImage *image,unsigned pixels) {
    auto href=image->getRepr()->attribute("xlink:href"); ASSERT_TRUE(href);
    auto base64=std::strchr(href,','); ASSERT_TRUE(base64); gsize size=0;
    auto bytes=g_base64_decode(base64+1,&size);
    auto cleanup=scope_exit([&] { g_free(bytes); }); ASSERT_GE(size,24u);
    EXPECT_EQ(std::memcmp(bytes,"\211PNG\r\n\032\n",8),0);
    // Read IHDR directly, independently of Pixbuf and shared rendering helpers.
    auto word=[&](unsigned k) { return (unsigned(bytes[k])<<24)|(unsigned(bytes[k+1])<<16)|
        (unsigned(bytes[k+2])<<8)|unsigned(bytes[k+3]); };
    EXPECT_EQ(word(16),pixels); EXPECT_EQ(word(20),pixels);
}
TEST_F(CandidateTest,NativeFractionalBoundaryBytesAndIndependentPngGrid) {
    for (auto f:fractional) for (bool transparent:{false,true}) {
        SCOPED_TRACE(std::string(f.side)+(transparent ? " transparent" : " white"));
        auto old=document(fractionalRect(f.side)),now=document(fractionalRect(f.side));
        ObjectSet a(old.get()),b(now.get()); a.add(old->getObjectById("a")); b.add(now->getObjectById("a"));
        BitmapCopyOptions options; options.dpi=f.dpi; options.transparent=transparent; options.antialias=false;
        preChange(a,options); b.createBitmapCopy(options);
        auto x=cast<SPImage>(a.single()),y=cast<SPImage>(b.single()); ASSERT_TRUE(x); ASSERT_TRUE(y);
        EXPECT_STREQ(x->getRepr()->attribute("xlink:href"),y->getRepr()->attribute("xlink:href"));
        expectPngGrid(x,f.pixels); expectPngGrid(y,f.pixels);
    }
}
TEST_F(CandidateTest,SafeFractionalBoundaryMetadataAndPublication) {
    for (auto f:fractional) {
        SCOPED_TRACE(f.side); open(fractionalRect(f.side),{"a"}); auto before=xml();
        BitmapCopyOptions options; options.dpi=f.dpi; options.antialias=false;
        auto out=prepareBitmapCopy(*desktop->getSelection(),options,PlacementPolicy::ContiguousReplacement,budget,{&room});
        ASSERT_TRUE(out.ok()) << out.outcome.diagnostic;
        EXPECT_EQ(out.candidate.metadata().width,f.pixels); EXPECT_EQ(out.candidate.metadata().height,f.pixels);
        ASSERT_TRUE(out.candidate.pixels()); EXPECT_EQ(out.candidate.pixels()->width(),int(f.pixels));
        EXPECT_EQ(out.candidate.pixels()->height(),int(f.pixels));
        auto token=DocumentUndo::beginAtomicInteraction(doc.get()); ASSERT_TRUE(token);
        auto published=publishBitmapCopy(*desktop->getSelection(),out.candidate,&*token);
        ASSERT_TRUE(published.ok()) << published.outcome.diagnostic;
        auto image=cast<SPImage>(desktop->getSelection()->single()); ASSERT_TRUE(image); expectPngGrid(image,f.pixels);
        ASSERT_TRUE(token->commitAtomically(Util::Internal::ContextString("Convert selection to bitmap"),"",[] { return true; }));
        DocumentUndo::undo(doc.get()); EXPECT_EQ(xml(),before);
    }
}
} // namespace
