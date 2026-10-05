// SPDX-License-Identifier: GPL-2.0-or-later
#include <gtest/gtest.h>
#include "actions/vacards-cli-production.h"
#include "actions/vacards-cli-fault-testing.h"




#include <numeric>
#include <iostream>
#include <filesystem>
#include <fstream>
#include "actions/actions-vacards-bitmap.h"
#include "actions/vacards-cli-edit-services.h"
#include "bitmap-adjustment-chemistry.h"
#include "display/cairo-utils.h"
#include "document.h"
#include "event-log.h"
#include "inkscape.h"
#include "inkscape-application.h"
#include "io/vacards-cli-files.h"
#include "io/vacards-cli-resources.h"
#include "object/sp-image.h"
#include "selection.h"
#include "ui/bitmap-tone-member-targets.h"
#include "ui/explode-bitmap-undo.h"
#include "xml/repr.h"
#include "xml/node.h"
#include "xml/attribute-record.h"
using namespace Inkscape;
using namespace Inkscape::VACardsCli;
namespace {
// Observe the native EventLog even while a test holds an atomic interaction.
// The public history command correctly refuses that admission state.
HistorySnapshotResult observeHistory(SPDocument &document) {
    auto log=document.get_event_log();
    if(!log) return {{},ParseError{"internal-error",{},"Missing native EventLog"}};
    std::vector<std::pair<std::uint64_t,std::string>> events;
    auto const &columns=EventLog::getColumns();
    std::function<void(Gtk::TreeModel::Children)> walk=[&](auto children) {
        for(auto row=children.begin();row!=children.end();++row) {
            Glib::ustring label=(*row)[columns.description];
            events.emplace_back((*row)[columns.serial],label.raw());walk(row->children());
        }
    };
    walk(log->getEventListStore()->children());
    for(std::size_t i=0;i<events.size();++i) if(events[i].first==log->getCurrEventSerial()) {
        HistorySnapshot result;result.can_undo=i>0;result.can_redo=i+1<events.size();
        if(result.can_undo)result.next_undo_label=events[i].second;
        if(result.can_redo)result.next_redo_label=events[i+1].second;
        return {result,{}};
    }
    return {{},ParseError{"internal-error",{},"Missing current native EventLog row"}};
}
class M3BitmapNative : public testing::Test {
protected:
    void SetUp() override {
        static auto *app = new InkscapeApplication;
        ASSERT_TRUE(app->gtk_app()); if (!Application::exists()) Application::create(false);
        auto gdk = gdk_pixbuf_new(GDK_COLORSPACE_RGB, true, 8, 8, 8); gdk_pixbuf_fill(gdk, 0x80808096);
        Pixbuf pixels(gdk); auto href = sp_image_encode_png_data_uri(pixels); ASSERT_TRUE(href);
        auto image = [&](char const *id) { return "<image id='" + std::string(id) + "' width='8' height='8' href='" + *href + "'/>"; };
        doc = SPDocument::createNewDocFromMem("<svg xmlns='http://www.w3.org/2000/svg' xmlns:xlink='http://www.w3.org/1999/xlink'><g id='g'>" +
            image("a") + "<g id='nested'>" + image("b") + "</g><rect id='vector' width='4' height='4'/><use id='clone' xlink:href='#a'/></g><rect id='untouched' x='20' width='2' height='2'/></svg>");
        ASSERT_TRUE(doc); doc->ensureUpToDate();
        doc->getSelection()->set(cast<SPItem>(doc->getObjectById("untouched")));
        DocumentUndo::setUndoSensitive(doc.get(), true); DocumentUndo::clearUndo(doc.get()); DocumentUndo::clearRedo(doc.get());
    }
    std::string xml() { return sp_repr_save_buf(doc->getReprDoc()).raw(); }
    Record call(std::string command, boost::json::object params, bool dry = false) {
        Request request; request.command = command; request.params = std::move(params); request.dry_run = dry;
        EditServices edits{*doc, *doc->getSelection(), document_stamp(doc.get()), {}};
        ProductionContext production{files, grants, edits, tokens, {}};
        production.session_id="p6-test"; production.catalog_identity="p6-test-catalog";
        production.incarnation=17; production.target_generation=23; production.bitmap=&bitmap;
        context.document = doc.get(); context.selection = doc->getSelection();
        auto before=xml();auto revision=document_stamp(doc.get()).revision;auto sessionRevision=context.session_revision;
        std::vector<std::string> selected;for(auto item:doc->getSelection()->items())selected.emplace_back(item->getId());
        auto history=observeHistory(*doc);EXPECT_TRUE(history.value.has_value());
        auto result=execute_bitmap(request, context, production);
        if(dry || command=="bitmap.tone-query" || command=="bitmap.histogram" || command=="bitmap.explode.analyze" ||
           command=="bitmap.explode.contour" || result.status!=Status::Changed) {
            EXPECT_EQ(xml(),before);EXPECT_EQ(document_stamp(doc.get()).revision,revision);EXPECT_EQ(context.session_revision,sessionRevision);
            std::vector<std::string> now;for(auto item:doc->getSelection()->items())now.emplace_back(item->getId());EXPECT_EQ(now,selected);
            auto after=observeHistory(*doc);EXPECT_TRUE(after.value.has_value());EXPECT_EQ(bool(after.value),bool(history.value));
            if(history.value && after.value) {
                EXPECT_EQ(after.value->can_undo,history.value->can_undo);EXPECT_EQ(after.value->can_redo,history.value->can_redo);
                EXPECT_EQ(after.value->next_undo_label,history.value->next_undo_label);EXPECT_EQ(after.value->next_redo_label,history.value->next_redo_label);
            }
        }
        return result;
    }
    std::unique_ptr<SPDocument> doc;
    FileState files; Grants grants; TokenStore tokens; DispatchContext context; Bitmap::CliSession bitmap;
};
TEST_F(M3BitmapNative, ToneQueryPaginationMixedAndReadOnly) {
    BitmapAdjustments::apply_tone(cast<SPItem>(doc->getObjectById("b")), Filters::BitmapToneSettings{.brightness=20});
    DocumentUndo::done(doc.get(), Util::Internal::ContextString("Fixture tone"), ""); doc->ensureUpToDate();
    auto before = xml(); auto revision = document_stamp(doc.get()).revision;
    auto r = call("bitmap.tone-query", {{"ids", boost::json::array{"g", "a"}}, {"mode", "compatible-members"}, {"limit", 1}});
    ASSERT_EQ(r.status, Status::Ok); EXPECT_EQ(r.data.at("total-count"), 2); EXPECT_EQ(r.data.at("excluded-count"), 2);
    EXPECT_EQ(r.covered, 1); EXPECT_TRUE(r.data.at("mixed").as_object().at("brightness").as_bool());
    ASSERT_TRUE(r.data.at("next-cursor").is_string()); auto cursor = r.data.at("next-cursor");
    auto second = call("bitmap.tone-query", {{"ids", boost::json::array{"g", "a"}}, {"mode", "compatible-members"}, {"limit", 1}, {"cursor", cursor}});
    EXPECT_EQ(second.data.at("members").as_array().front().as_object().at("id"), "b"); EXPECT_TRUE(second.data.at("next-cursor").is_null());
    auto wrong = call("bitmap.tone-query", {{"ids", boost::json::array{"g"}}, {"mode", "legacy-roots"}, {"cursor", cursor}});
    EXPECT_EQ(wrong.reason, "invalid-cursor"); EXPECT_EQ(xml(), before); EXPECT_EQ(document_stamp(doc.get()).revision, revision);
    EXPECT_EQ(doc->getSelection()->singleItem()->getId(), std::string("untouched"));
}
TEST_F(M3BitmapNative, HistogramIdentityAndOverrideAreNativeSampleCounts) {
    auto before = xml(); auto r = call("bitmap.histogram", {{"ids", boost::json::array{"a"}}, {"remap", "none"}});
    ASSERT_EQ(r.status, Status::Ok); auto const &counts = r.data.at("counts").as_array(); ASSERT_EQ(counts.size(), 256u);
    EXPECT_EQ(counts[128], 64); EXPECT_EQ(r.data.at("total-samples"), 64); EXPECT_FALSE(r.data.at("remapped").as_bool());
    auto changed = call("bitmap.histogram", {{"ids", boost::json::array{"a"}}, {"remap", "none"}, {"brightness", 100}}, true);
    EXPECT_TRUE(changed.data.at("remapped").as_bool()); EXPECT_NE(changed.data.at("counts"), r.data.at("counts"));
    EXPECT_EQ(xml(), before); EXPECT_EQ(tokens.snapshot().active_roots, 0u);
}
TEST_F(M3BitmapNative, ToneMembersPreviewCancelOneUndoAndPreservation) {
    auto b = cast<SPItem>(doc->getObjectById("b"));
    BitmapAdjustments::apply_tone(b, Filters::BitmapToneSettings{.contrast=23});
    DocumentUndo::done(doc.get(), Util::Internal::ContextString("Fixture tone"), ""); doc->ensureUpToDate();
    auto before = xml(); auto params = boost::json::object{{"ids", boost::json::array{"g", "a"}}, {"patch", boost::json::object{{"brightness", 10}}}};
    auto preview = call("bitmap.tone-set", params, true); ASSERT_EQ(preview.status, Status::Ok); EXPECT_EQ(xml(), before);
    context.cancelled = [] { return true; }; EXPECT_EQ(call("bitmap.tone-set", params).status, Status::Cancelled); EXPECT_EQ(xml(), before);
    context.cancelled = {}; auto commit = call("bitmap.tone-set", params); ASSERT_EQ(commit.status, Status::Changed) << commit.message;
    EXPECT_EQ(commit.data.at("changed-count"), 2); EXPECT_EQ(commit.data.at("excluded-count"), 2);
    EXPECT_EQ(BitmapAdjustments::canonical_tone(b).contrast, 23); auto after = xml();
    EXPECT_EQ(doc->getSelection()->singleItem()->getId(), std::string("untouched"));
    ASSERT_TRUE(DocumentUndo::undo(doc.get())); doc->ensureUpToDate(); EXPECT_EQ(xml(), before);
    ASSERT_TRUE(DocumentUndo::redo(doc.get())); doc->ensureUpToDate(); EXPECT_EQ(xml(), after);
    auto reopened = SPDocument::createNewDocFromMem(after); ASSERT_TRUE(reopened); reopened->ensureUpToDate();
    EXPECT_EQ(BitmapAdjustments::canonical_tone(cast<SPItem>(reopened->getObjectById("b"))).contrast, 23);
    EXPECT_EQ(call("bitmap.tone-set", params).status, Status::Unchanged);
}
}

namespace {
TEST_F(M3BitmapNative, ExplodeAnalyzeRetainsOnlyOnRequestAndCancelPreservesDocument) {
    auto before=xml();auto params=boost::json::object{{"id","b"},{"recipe",boost::json::object{}},{"retain",true}};
    auto preview=call("bitmap.explode.analyze",params,true);ASSERT_EQ(preview.status,Status::Ok)<<preview.message;
    EXPECT_TRUE(preview.data.at("analysis-token").is_null());EXPECT_EQ(tokens.snapshot().active_roots,0u);EXPECT_EQ(xml(),before);
    auto retained=call("bitmap.explode.analyze",params);ASSERT_EQ(retained.status,Status::Ok)<<retained.message;
    EXPECT_TRUE(retained.data.at("analysis-token").is_string());EXPECT_EQ(tokens.snapshot().active_roots,1u);
    EXPECT_EQ(retained.data.at("piece-count"),1);EXPECT_EQ(xml(),before);
    context.cancelled=[] {return true;};EXPECT_EQ(call("bitmap.explode.analyze",params).status,Status::Cancelled);
    EXPECT_EQ(tokens.snapshot().active_roots,1u);EXPECT_EQ(xml(),before);
}
TEST_F(M3BitmapNative, ExplodeContourUsesAnalysisParentWithoutMutation) {
    auto before=xml();auto a=call("bitmap.explode.analyze",{{"id","b"},{"recipe",boost::json::object{}}});ASSERT_EQ(a.status,Status::Ok)<<a.message;
    auto params=boost::json::object{{"analysis-token",a.data.at("analysis-token")},{"contour",boost::json::object{}},{"retain",true}};
    unsigned checks=0;context.cancelled=[&] {return ++checks>=4;};
    auto cancelled=call("bitmap.explode.contour",params);EXPECT_EQ(cancelled.status,Status::Cancelled);
    EXPECT_EQ(cancelled.error_details.at("stage"),"Contour");EXPECT_EQ(tokens.snapshot().active_roots,1u);context.cancelled={};
    auto result=call("bitmap.explode.contour",params);ASSERT_EQ(result.status,Status::Ok)<<result.message;
    EXPECT_EQ(result.data.at("parent-token"),a.data.at("analysis-token"));EXPECT_TRUE(result.data.at("contour-token").is_string());
    EXPECT_EQ(tokens.snapshot().active_tokens.size(),2u);EXPECT_EQ(tokens.snapshot().active_roots,1u);EXPECT_EQ(xml(),before);
}
TEST_F(M3BitmapNative, ExplodeTokenPublishesOnceWithUndoRedo) {
    auto before=xml();auto a=call("bitmap.explode.analyze",{{"id","b"},{"recipe",boost::json::object{}}});ASSERT_EQ(a.status,Status::Ok)<<a.message;
    auto params=boost::json::object{{"analysis-token",a.data.at("analysis-token")},{"contours",false}};
    auto preview=call("bitmap.explode.explode",params,true);ASSERT_EQ(preview.status,Status::Ok)<<preview.message;
    EXPECT_EQ(xml(),before);EXPECT_EQ(tokens.snapshot().active_roots,1u);
    auto result=call("bitmap.explode.explode",params);ASSERT_EQ(result.status,Status::Changed)<<result.message;
    EXPECT_EQ(result.selection_after.size(),1u);EXPECT_EQ(tokens.snapshot().active_roots,0u);auto after=xml();
    ASSERT_TRUE(DocumentUndo::undo(doc.get()));doc->ensureUpToDate();EXPECT_EQ(xml(),before);
    ASSERT_TRUE(DocumentUndo::redo(doc.get()));doc->ensureUpToDate();EXPECT_EQ(xml(),after);
    EXPECT_EQ(call("bitmap.explode.explode",params).reason,"invalid-token");
}
TEST_F(M3BitmapNative, ContourOnlyFullRecipePreservesPixelsAndHasOneUndo) {
    auto before=xml();auto params=boost::json::object{{"id","b"},{"recipe",boost::json::object{}},{"contour",boost::json::object{}}};
    auto preview=call("bitmap.explode.create-contour-only",params,true);ASSERT_EQ(preview.status,Status::Ok)<<preview.message;EXPECT_EQ(xml(),before);
    auto result=call("bitmap.explode.create-contour-only",params);ASSERT_EQ(result.status,Status::Changed)<<result.message;
    auto const &data=result.data;EXPECT_TRUE(data.at("source-retained").as_bool());
    auto const &piece=data.at("publication-map").as_array().front().as_object();EXPECT_EQ(piece.at("pixel-sha256"),data.at("source-pixel-sha256"));
    auto after=xml();ASSERT_TRUE(DocumentUndo::undo(doc.get()));doc->ensureUpToDate();EXPECT_EQ(xml(),before);
    ASSERT_TRUE(DocumentUndo::redo(doc.get()));doc->ensureUpToDate();EXPECT_EQ(xml(),after);
}
TEST_F(M3BitmapNative, AlphaIdentityIsNoOpAndRetainsToken) {
    auto before=xml();auto a=call("bitmap.explode.analyze",{{"id","b"},{"recipe",boost::json::object{{"refine",false},{"faint-floor",0}}}});
    ASSERT_EQ(a.status,Status::Ok)<<a.message;
    auto preview=call("bitmap.explode.apply-adjustment",{{"analysis-token",a.data.at("analysis-token")}},true);
    EXPECT_EQ(preview.status,Status::Ok);EXPECT_EQ(preview.data.at("variant"),"computed-dry-run");
    EXPECT_TRUE(preview.data.contains("alpha-sha256"));EXPECT_EQ(xml(),before);EXPECT_EQ(tokens.snapshot().active_roots,1u);
    auto result=call("bitmap.explode.apply-adjustment",{{"analysis-token",a.data.at("analysis-token")}});
    EXPECT_EQ(result.status,Status::Unchanged)<<result.message;EXPECT_EQ(result.data.at("unchanged-reason"),"identity-adjustment");
    EXPECT_EQ(tokens.snapshot().active_roots,1u);EXPECT_EQ(xml(),before);
}
}

namespace {
TEST_F(M3BitmapNative, AlphaPublicationRefinesOnceAndRestoresThroughHistory) {
    auto before=xml();auto params=boost::json::object{{"id","b"},{"recipe",boost::json::object{{"threshold",128},{"softness",40},{"faint-floor",0},{"refine",true}}}};
    auto preview=call("bitmap.explode.apply-adjustment",params,true);ASSERT_EQ(preview.status,Status::Ok)<<preview.message;EXPECT_EQ(xml(),before);
    auto result=call("bitmap.explode.apply-adjustment",params);ASSERT_EQ(result.status,Status::Changed)<<result.message;
    auto image=cast<SPImage>(doc->getObjectById("b"));ASSERT_TRUE(image && image->pixbuf);
    Pixbuf pixels(*image->pixbuf);pixels.ensurePixelFormat(Pixbuf::PF_GDK);auto raw=pixels.getPixbufRaw();
    EXPECT_EQ(gdk_pixbuf_get_pixels(raw)[3],222);EXPECT_EQ(gdk_pixbuf_get_pixels(raw)[0],128);
    auto after=xml();auto again=call("bitmap.explode.apply-adjustment",params);EXPECT_EQ(again.status,Status::Unchanged)<<again.message;EXPECT_EQ(xml(),after);
    ASSERT_TRUE(DocumentUndo::undo(doc.get()));doc->ensureUpToDate();EXPECT_EQ(xml(),before);
    ASSERT_TRUE(DocumentUndo::redo(doc.get()));doc->ensureUpToDate();EXPECT_EQ(xml(),after);
}
}

namespace {
TEST_F(M3BitmapNative, EncodeRefusalKeepsTypedStageAndHistory) {
    auto gdk=gdk_pixbuf_new(GDK_COLORSPACE_RGB,true,8,4,4);gdk_pixbuf_fill(gdk,0x80808096);
    Pixbuf pixels(gdk);auto href=sp_image_encode_png_data_uri(pixels);ASSERT_TRUE(href);
    doc->getObjectById("b")->getRepr()->setAttribute("href",*href);
    DocumentUndo::done(doc.get(),Util::Internal::ContextString("Small raster fixture"),"");doc->ensureUpToDate();
    auto before=xml();auto revision=document_stamp(doc.get()).revision;
    auto result=call("bitmap.explode.analyze",{{"id","b"},{"recipe",boost::json::object{}},{"retain",true}});
    EXPECT_EQ(result.reason,"analysis-failed");EXPECT_EQ(result.error_details.at("stage"),"Encode");
    EXPECT_EQ(result.error_details.at("reason"),"context reason EncodingFailed during analysis encoding");
    EXPECT_EQ(result.error_details.at("mutation_state"),"none");EXPECT_EQ(result.error_retryable,false);
    EXPECT_EQ(xml(),before);EXPECT_EQ(document_stamp(doc.get()).revision,revision);EXPECT_EQ(tokens.snapshot().active_roots,0u);
}
}

namespace {
TEST_F(M3BitmapNative, BitmapCopyExactPixelsPreservesRectangleBackgroundAndHistory) {
    auto before=xml();auto params=boost::json::object{{"ids",boost::json::array{"vector","untouched"}},
        {"size",boost::json::object{{"width",13},{"height",7}}},{"background",boost::json::array{0,0,1,1}},
        {"bbox","geometric"},{"replace",false}};
    auto preview=call("bitmap.copy",params,true);ASSERT_EQ(preview.status,Status::Ok)<<preview.message;
    EXPECT_EQ(preview.data.at("width"),13);EXPECT_EQ(preview.data.at("height"),7);EXPECT_EQ(xml(),before);
    EXPECT_TRUE(preview.data.at("image-id").is_null());EXPECT_DOUBLE_EQ(preview.data.at("dpi-x").to_number<double>(),13.0*96/22);
    EXPECT_DOUBLE_EQ(preview.data.at("dpi-y").to_number<double>(),7.0*96/4);
    auto result=call("bitmap.copy",params);ASSERT_EQ(result.status,Status::Changed)<<result.message;
    auto image=cast<SPImage>(doc->getObjectById(std::string(result.data.at("image-id").as_string())));ASSERT_TRUE(image);
    EXPECT_DOUBLE_EQ(image->width.computed,22);EXPECT_DOUBLE_EQ(image->height.computed,4);ASSERT_TRUE(image->pixbuf);
    EXPECT_EQ(image->pixbuf->width(),13);EXPECT_EQ(image->pixbuf->height(),7);
    Pixbuf pixels(*image->pixbuf);pixels.ensurePixelFormat(Pixbuf::PF_GDK);auto raw=pixels.getPixbufRaw();
    auto center=gdk_pixbuf_get_pixels(raw)+3*gdk_pixbuf_get_rowstride(raw)+6*4;
    EXPECT_EQ(center[0],0);EXPECT_EQ(center[1],0);EXPECT_EQ(center[2],255);EXPECT_EQ(center[3],255);
    EXPECT_TRUE(doc->getObjectById("vector"));EXPECT_TRUE(doc->getObjectById("untouched"));auto after=xml();
    ASSERT_TRUE(DocumentUndo::undo(doc.get()));doc->ensureUpToDate();EXPECT_EQ(xml(),before);
    ASSERT_TRUE(DocumentUndo::redo(doc.get()));doc->ensureUpToDate();EXPECT_EQ(xml(),after);
    auto reopened=SPDocument::createNewDocFromMem(after);ASSERT_TRUE(reopened);reopened->ensureUpToDate();
    auto restored=cast<SPImage>(reopened->getObjectById(std::string(result.data.at("image-id").as_string())));
    ASSERT_TRUE(restored && restored->pixbuf);EXPECT_EQ(restored->pixbuf->width(),13);EXPECT_DOUBLE_EQ(restored->width.computed,22);
}
TEST_F(M3BitmapNative, BitmapCopyReplacementAndDomainRefusalAreAtomic) {
    auto before=xml();auto params=boost::json::object{{"ids",boost::json::array{"vector"}},
        {"size",boost::json::object{{"width",13},{"height",7}}},{"replace",true}};
    context.cancelled=[] {return true;};EXPECT_EQ(call("bitmap.copy",params).status,Status::Cancelled);EXPECT_EQ(xml(),before);context.cancelled={};
    auto huge=params;huge["size"]=boost::json::object{{"width",32000000},{"height",7}};
    auto refused=call("bitmap.copy",huge);EXPECT_EQ(refused.reason,"engine-limit");EXPECT_EQ(xml(),before);
    EXPECT_EQ(refused.error_details.at("reason"),"prepare: raster budget exceeded");EXPECT_EQ(refused.error_details.at("mutation_state"),"none");
    auto result=call("bitmap.copy",params);ASSERT_EQ(result.status,Status::Changed)<<result.message;EXPECT_FALSE(doc->getObjectById("vector"));
    auto output=doc->getObjectById(std::string(result.data.at("image-id").as_string()));ASSERT_TRUE(output);EXPECT_EQ(output->parent->getId(),std::string("g"));
    auto after=xml();ASSERT_TRUE(DocumentUndo::undo(doc.get()));doc->ensureUpToDate();EXPECT_EQ(xml(),before);
    ASSERT_TRUE(DocumentUndo::redo(doc.get()));doc->ensureUpToDate();EXPECT_EQ(xml(),after);
}
}

namespace {
TEST_F(M3BitmapNative, ContourTokenCommitConsumesOnlyItsOwnFamily) {
    auto params=boost::json::object{{"id","b"},{"recipe",boost::json::object{}}};
    auto a=call("bitmap.explode.analyze",params);ASSERT_EQ(a.status,Status::Ok)<<a.message;
    auto other=call("bitmap.explode.analyze",params);ASSERT_EQ(other.status,Status::Ok)<<other.message;
    auto cp=boost::json::object{{"analysis-token",a.data.at("analysis-token")},{"contour",boost::json::object{}}};
    auto preview=call("bitmap.explode.contour",cp,true);ASSERT_EQ(preview.status,Status::Ok)<<preview.message;
    EXPECT_TRUE(preview.data.at("contour-token").is_null());EXPECT_EQ(tokens.snapshot().active_roots,2u);
    auto c=call("bitmap.explode.contour",cp);ASSERT_EQ(c.status,Status::Ok)<<c.message;
    auto publication=boost::json::object{{"contour-token",c.data.at("contour-token")},{"contours",true}};
    auto before=xml();context.cancelled=[] {return true;};
    EXPECT_EQ(call("bitmap.explode.explode",publication).status,Status::Cancelled);context.cancelled={};
    EXPECT_EQ(tokens.snapshot().active_tokens.size(),3u);
    auto result=call("bitmap.explode.explode",publication);ASSERT_EQ(result.status,Status::Changed)<<result.message;
    EXPECT_EQ(tokens.snapshot().active_roots,1u);EXPECT_EQ(tokens.snapshot().active_tokens.size(),1u);
    EXPECT_EQ(tokens.snapshot().active_tokens.front(),other.data.at("analysis-token").as_string());
    auto after=xml();ASSERT_TRUE(DocumentUndo::undo(doc.get()));doc->ensureUpToDate();EXPECT_EQ(xml(),before);
    ASSERT_TRUE(DocumentUndo::redo(doc.get()));doc->ensureUpToDate();EXPECT_EQ(xml(),after);
}
TEST_F(M3BitmapNative, ZeroPiecesPreviewAndUnsupportedAndStaleRefusalsPreserveState) {
    auto gdk=gdk_pixbuf_new(GDK_COLORSPACE_RGB,true,8,8,8);gdk_pixbuf_fill(gdk,0);
    Pixbuf pixels(gdk);auto href=sp_image_encode_png_data_uri(pixels);ASSERT_TRUE(href);
    doc->getObjectById("b")->getRepr()->setAttribute("href",*href);
    DocumentUndo::done(doc.get(),Util::Internal::ContextString("P6 transparent fixture"),"");doc->ensureUpToDate();
    auto params=boost::json::object{{"id","b"},{"recipe",boost::json::object{{"threshold",255},{"softness",0},{"faint-floor",0},{"refine",false}}}};
    auto analyzed=call("bitmap.explode.analyze",params);ASSERT_EQ(analyzed.status,Status::Ok)<<analyzed.message;
    ASSERT_EQ(analyzed.data.at("piece-count"),0);
    auto publication=boost::json::object{{"analysis-token",analyzed.data.at("analysis-token")},{"contours",false}};
    auto preview=call("bitmap.explode.explode",publication,true);EXPECT_EQ(preview.status,Status::Ok)<<preview.message;
    EXPECT_EQ(preview.data.at("variant"),"computed-dry-run");EXPECT_EQ(preview.data.at("piece-count"),0);
    EXPECT_EQ(call("bitmap.explode.explode",publication).status,Status::Unchanged);EXPECT_EQ(tokens.snapshot().active_roots,1u);
    EXPECT_EQ(call("bitmap.explode.analyze",{{"id","vector"},{"recipe",boost::json::object{}}}).reason,"requires-single-bitmap");
    doc->getObjectById("b")->getRepr()->setAttribute("x","2");
    DocumentUndo::done(doc.get(),Util::Internal::ContextString("P6 stale fixture"),"");doc->ensureUpToDate();
    EXPECT_EQ(call("bitmap.explode.explode",publication).reason,"stale-plan");EXPECT_EQ(tokens.snapshot().active_roots,1u);
}
TEST_F(M3BitmapNative, BitmapCopyFractionalDpiAndNoBoundsPreserveState) {
    auto result=call("bitmap.copy",{{"ids",boost::json::array{"vector"}},{"dpi",97.5}},true);
    ASSERT_EQ(result.status,Status::Ok)<<result.message;EXPECT_EQ(result.data.at("width"),5);EXPECT_EQ(result.data.at("height"),5);
    EXPECT_DOUBLE_EQ(result.data.at("dpi-x").to_number<double>(),120);
    doc->getObjectById("vector")->getRepr()->setAttribute("width","0");
    DocumentUndo::done(doc.get(),Util::Internal::ContextString("P6 no bounds fixture"),"");doc->ensureUpToDate();
    EXPECT_EQ(call("bitmap.copy",{{"ids",boost::json::array{"vector"}}}).reason,"no-bounds");
}
}

namespace {
TEST_F(M3BitmapNative, IndependentThreeIslandMetricsAndContourTopology) {
    bitmap.retire();
    auto path=std::filesystem::path(__FILE__).parent_path().parent_path()/"cli_tests/vacards-agent/fixtures/m3/explode.svg";
    std::ifstream stream(path);ASSERT_TRUE(stream);std::string bytes((std::istreambuf_iterator<char>(stream)),{});
    doc=SPDocument::createNewDocFromMem(bytes);ASSERT_TRUE(doc);doc->ensureUpToDate();
    DocumentUndo::setUndoSensitive(doc.get(),true);DocumentUndo::clearUndo(doc.get());DocumentUndo::clearRedo(doc.get());
    auto a=call("bitmap.explode.analyze",{{"id","image1"},{"recipe",boost::json::object{}}});ASSERT_EQ(a.status,Status::Ok)<<a.message;
    ASSERT_EQ(a.data.at("piece-count"),3);auto const &pieces=a.data.at("pieces").as_array();ASSERT_EQ(pieces.size(),3u);
    for(unsigned i=0;i<3;++i) {
        auto const &piece=pieces[i].as_object();EXPECT_EQ(piece.at("index"),i);EXPECT_EQ(piece.at("pixel-count"),400);EXPECT_EQ(piece.at("alpha-sum"),102000);
    }
    auto c=call("bitmap.explode.contour",{{"analysis-token",a.data.at("analysis-token")},{"contour",boost::json::object{}}});
    ASSERT_EQ(c.status,Status::Ok)<<c.message;EXPECT_EQ(c.data.at("no-contour-count"),0);
    for(auto const &value:c.data.at("pieces").as_array()) {auto const &piece=value.as_object();EXPECT_EQ(piece.at("topology-count"),1);EXPECT_FALSE(piece.at("noContour").as_bool());}
}
}

namespace {
TEST_F(M3BitmapNative, EveryPreviewPreservesPreexistingUndoAndRedoLabels) {
    doc->getObjectById("untouched")->getRepr()->setAttribute("y","2");
    DocumentUndo::done(doc.get(),Util::Internal::ContextString("P6 history fixture"),"");doc->ensureUpToDate();
    auto seeded=call("bitmap.tone-set",{{"ids",boost::json::array{"b"}},{"patch",boost::json::object{{"brightness",10}}}});
    ASSERT_EQ(seeded.status,Status::Changed);ASSERT_TRUE(DocumentUndo::undo(doc.get()));doc->ensureUpToDate();
    EditServices edits{*doc,*doc->getSelection(),document_stamp(doc.get()),{}};
    auto history=history_snapshot(edits);ASSERT_TRUE(history.value);
    ASSERT_TRUE(history.value->can_undo);ASSERT_TRUE(history.value->can_redo);
    EXPECT_EQ(history.value->next_undo_label,"P6 history fixture");EXPECT_EQ(history.value->next_redo_label,"Adjust object tone");
    EXPECT_EQ(call("bitmap.tone-query",{{"ids",boost::json::array{"b"}}},true).status,Status::Ok);
    EXPECT_EQ(call("bitmap.histogram",{{"ids",boost::json::array{"b"}}},true).status,Status::Ok);
    EXPECT_EQ(call("bitmap.tone-set",{{"ids",boost::json::array{"b"}},{"patch",boost::json::object{{"brightness",20}}}},true).status,Status::Ok);
    EXPECT_EQ(call("bitmap.copy",{{"ids",boost::json::array{"vector"}}},true).status,Status::Ok);
    auto full=boost::json::object{{"id","b"},{"recipe",boost::json::object{}}};
    EXPECT_EQ(call("bitmap.explode.analyze",full,true).status,Status::Ok);
    EXPECT_EQ(call("bitmap.explode.apply-adjustment",full,true).status,Status::Ok);
    auto explode=full;explode["contours"]=false;EXPECT_EQ(call("bitmap.explode.explode",explode,true).status,Status::Ok);
    auto only=full;only["contour"]=boost::json::object{};EXPECT_EQ(call("bitmap.explode.create-contour-only",only,true).status,Status::Ok);
    auto a=call("bitmap.explode.analyze",full);ASSERT_EQ(a.status,Status::Ok);
    EXPECT_EQ(call("bitmap.explode.contour",{{"analysis-token",a.data.at("analysis-token")},{"contour",boost::json::object{}}},true).status,Status::Ok);
    context.cancelled=[] {return true;};EXPECT_EQ(call("bitmap.explode.explode",explode).status,Status::Cancelled);context.cancelled={};
    auto after=history_snapshot(edits);ASSERT_TRUE(after.value);EXPECT_EQ(after.value->next_undo_label,history.value->next_undo_label);EXPECT_EQ(after.value->next_redo_label,history.value->next_redo_label);
    EXPECT_EQ(tokens.snapshot().active_roots,1u);
}
}


namespace {
// Independent inputs/expectations: vacards-cli-m3-cases.py:76-98,173-189,240-251.
// No expected value is obtained from the implementation under test.
class P9Independent : public M3BitmapNative {
protected:
    void loadP9(bool explode = false) {
        bitmap.retire();
        auto path=std::filesystem::path(__FILE__).parent_path().parent_path()/"cli_tests/vacards-agent/fixtures/m3"/(explode ? "explode.svg":"tone.svg");
        std::ifstream stream(path); ASSERT_TRUE(stream);
        doc=SPDocument::createNewDocFromMem(std::string((std::istreambuf_iterator<char>(stream)),{}));
        ASSERT_TRUE(doc); doc->ensureUpToDate();
        doc->getSelection()->set(cast<SPItem>(doc->getObjectById("untouched")));
        DocumentUndo::setUndoSensitive(doc.get(),true);
        // Seed both history directions so preservation cannot pass on empty history.
        doc->getObjectById("untouched")->getRepr()->setAttribute("x","151");
        DocumentUndo::done(doc.get(),Util::Internal::ContextString("P9 seed undo"),"");
        doc->getObjectById("untouched")->getRepr()->setAttribute("x","152");
        DocumentUndo::done(doc.get(),Util::Internal::ContextString("P9 seed redo"),"");
        ASSERT_TRUE(DocumentUndo::undo(doc.get())); doc->ensureUpToDate();
        auto retained=call("bitmap.explode.analyze",{{"id","image1"},{"recipe",boost::json::object{{"refine",false}}}});
        ASSERT_EQ(retained.status,Status::Ok)<<retained.message;
        ASSERT_EQ(tokens.snapshot().active_roots,1u);
        ASSERT_GT(tokens.snapshot().charged_bytes,0u);
    }
    boost::json::object state() {
        EditServices edits{*doc,*doc->getSelection(),document_stamp(doc.get()),{}};
        auto h=observeHistory(*doc); EXPECT_TRUE(h.value);
        boost::json::array selected,active;
        for(auto item:doc->getSelection()->items()) selected.emplace_back(item->getId());
        auto t=tokens.snapshot(); for(auto const &id:t.active_tokens) active.emplace_back(id);
        auto label=[](auto const &v)->boost::json::value {return v ? boost::json::value(*v):boost::json::value(nullptr);};
        return {{"xml",xml()},{"selection",selected},{"document_revision",document_stamp(doc.get()).revision},
            {"session_revision",context.session_revision},
            {"undo",boost::json::object{{"available",h.value->can_undo},{"label",label(h.value->next_undo_label)}}},
            {"redo",boost::json::object{{"available",h.value->can_redo},{"label",label(h.value->next_redo_label)}}},
            {"tokens",boost::json::object{{"active",active},{"roots",t.active_roots},{"charged_bytes",t.charged_bytes}}}};
    }
    boost::json::object untouched() {
        boost::json::object result;
        for(auto const &a:doc->getObjectById("untouched")->getRepr()->attributeList())
            result[g_quark_to_string(a.key)]=a.value.pointer();
        return result;
    }
    Record independent(std::string command,boost::json::object params) {
        auto before=state(), attrs=untouched();
        auto preview=call(command,params,true); EXPECT_EQ(preview.status,Status::Ok)<<preview.message;
        EXPECT_EQ(state(),before); EXPECT_EQ(untouched(),attrs);
        context.cancelled=[] {return true;}; auto cancelled=call(command,params); context.cancelled={};
        EXPECT_EQ(cancelled.status,Status::Cancelled); EXPECT_EQ(state(),before);
        auto result=call(command,params); EXPECT_EQ(untouched(),attrs);
        auto expectedStatus = command=="bitmap.explode.apply-adjustment" ? Status::Unchanged :
            command=="bitmap.tone-query" || command=="bitmap.histogram" || command=="bitmap.explode.analyze" || command=="bitmap.explode.contour" ? Status::Ok : Status::Changed;
        EXPECT_EQ(result.status,expectedStatus)<<result.message;
        if(command!="bitmap.explode.analyze" && command!="bitmap.explode.contour")
            EXPECT_EQ(state().at("tokens"),before.at("tokens"));
        if(result.status==Status::Changed) {
            EXPECT_EQ(result.publication,"committed"); EXPECT_TRUE(result.one_undo_step);
            auto after=state();
            EXPECT_NE(after.at("document_revision"),before.at("document_revision"));
            EXPECT_EQ(after.at("session_revision"),before.at("session_revision"));
            EXPECT_EQ(after.at("selection"),before.at("selection"));
            ASSERT_HISTORY(result,before,after);
        } else {
            auto after=state(); before.erase("tokens");after.erase("tokens");EXPECT_EQ(after,before);
        }
        return result;
    }
    void ASSERT_HISTORY(Record const &,boost::json::object const &before,boost::json::object const &after) {
        ASSERT_TRUE(DocumentUndo::undo(doc.get()));doc->ensureUpToDate();
        EXPECT_EQ(xml(),before.at("xml").as_string());EXPECT_EQ(state().at("selection"),before.at("selection"));
        ASSERT_TRUE(DocumentUndo::redo(doc.get()));doc->ensureUpToDate();EXPECT_EQ(xml(),after.at("xml").as_string());
        EXPECT_EQ(state().at("selection"),after.at("selection"));
    }
    void refusal(std::string command,boost::json::object params,std::string code,std::string branch,std::string layer="command-service") {
        auto before=state(); auto result=call(command,params);auto after=state();
        ASSERT_EQ(result.action,command);ASSERT_TRUE(result.error);
        ASSERT_EQ(result.error->code,code);ASSERT_EQ(result.reason,code)<<result.message;
        ASSERT_EQ(result.status,Status::Rejected); ASSERT_EQ(before,after);
        ASSERT_NE(result.publication,"committed");
        if (layer == "identity/admission") {
            ASSERT_TRUE(result.error_details.if_contains("reason"));
            ASSERT_EQ(result.error_details.at("mutation_state"), "none");
            ASSERT_TRUE(result.error_retryable);
        }
        // Exact branch is established by the fixture and the typed native result.
        if(auto reason=result.error_details.if_contains("reason")) ASSERT_EQ(*reason,boost::json::value(branch));
        boost::json::object observation{{"obligation","P9:"+command+":"+layer+":"+code+":"+branch},
            {"command",result.action},{"code",result.error->code},{"branch",branch},{"settled",result.status==Status::Rejected && result.publication!="committed"},
            {"before",before},{"after",after}};
        std::cout<<"P9-OBSERVATION "<<boost::json::serialize(observation)<<std::endl;
    }
};
TEST_F(P9Independent, bitmap_copy) {
    loadP9();auto r=independent("bitmap.copy",{{"ids",boost::json::array{"shape1"}},{"size",boost::json::object{{"width",10},{"height",10}}},{"bbox","geometric"},{"background",boost::json::array{0,0,0,0}},{"replace",false}});
    ASSERT_EQ(r.status,Status::Changed);EXPECT_EQ(r.data.at("width"),10);EXPECT_EQ(r.data.at("height"),10);
    EXPECT_DOUBLE_EQ(r.data.at("dpi-x").to_number<double>(),96);EXPECT_DOUBLE_EQ(r.data.at("dpi-y").to_number<double>(),96);EXPECT_EQ(r.data.at("source-retained"),true);
    EXPECT_EQ(r.data.at("pixel-sha256"),"93c67b2c71b023b1ca3c61d62499944cdc98c83e90a4389f895beff846d30c43");
    EXPECT_EQ(r.data.at("profile-sha256"),"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}
TEST_F(P9Independent, bitmap_tone_query) {
    loadP9();auto r=independent("bitmap.tone-query",{{"ids",boost::json::array{"image1"}},{"mode","compatible-members"}});
    EXPECT_EQ(r.data.at("total-count"),1);EXPECT_EQ(r.data.at("excluded-count"),0);EXPECT_TRUE(r.data.at("next-cursor").is_null());
    auto expected=boost::json::parse(R"({"brightness":0.0,"contrast":0.0,"intensity":0.0,"highlights":0.0,"shadows":0.0,"midtones":0.0})");
    ASSERT_EQ(r.data.at("members").as_array().size(),1u);
    EXPECT_EQ(r.data.at("members").as_array()[0].at("id"),"image1");
    EXPECT_EQ(r.data.at("members").as_array()[0].at("values"),expected);
    for(auto const &entry:r.data.at("mixed").as_object())EXPECT_EQ(entry.value(),false);
}
TEST_F(P9Independent, bitmap_histogram) {
    loadP9();auto r=independent("bitmap.histogram",{{"ids",boost::json::array{"image1"}},{"channel","luminance"},{"bins",256},{"remap","none"}});
    EXPECT_EQ(r.data.at("width"),120);EXPECT_EQ(r.data.at("height"),80);EXPECT_EQ(r.data.at("remapped"),false);
    EXPECT_EQ(r.data.at("counts").as_array().size(),256u);
}
TEST_F(P9Independent, bitmap_tone_set) {
    loadP9();auto r=independent("bitmap.tone-set",{{"ids",boost::json::array{"image1"}},{"patch",boost::json::object{{"brightness",10}}}});
    ASSERT_EQ(r.status,Status::Changed);EXPECT_EQ(r.data.at("changed-count"),1);EXPECT_EQ(r.data.at("unchanged-count"),0);EXPECT_EQ(r.data.at("excluded-count"),0);EXPECT_EQ(r.data.at("total-count"),1);
    auto const &m=r.data.at("members").as_array()[0];EXPECT_EQ(m.at("id"),"image1");
    auto expected=boost::json::parse(R"({"brightness":0.0,"contrast":0.0,"intensity":0.0,"highlights":0.0,"shadows":0.0,"midtones":0.0})");
    EXPECT_EQ(m.at("before"),expected);expected.as_object()["brightness"]=10.0;EXPECT_EQ(m.at("after"),expected);
}
TEST_F(P9Independent, bitmap_explode_analyze) {
    loadP9(true);auto r=independent("bitmap.explode.analyze",{{"id","image1"},{"recipe",boost::json::object{{"refine",false}}}});
    EXPECT_EQ(tokens.snapshot().active_roots,2u);EXPECT_EQ(tokens.snapshot().active_tokens.size(),2u);
    ASSERT_EQ(r.data.at("piece-count"),3);auto const &pieces=r.data.at("pieces").as_array();ASSERT_EQ(pieces.size(),3u);
    for(unsigned i=0;i<3;++i){EXPECT_EQ(pieces[i].at("index"),i);EXPECT_EQ(pieces[i].at("pixel-count"),400);EXPECT_EQ(pieces[i].at("alpha-sum"),102000);}
}
TEST_F(P9Independent, bitmap_explode_contour) {
    loadP9(true);auto a=call("bitmap.explode.analyze",{{"id","image1"},{"recipe",boost::json::object{{"refine",false}}}});ASSERT_EQ(a.status,Status::Ok);
    auto r=independent("bitmap.explode.contour",{{"analysis-token",a.data.at("analysis-token")},{"contour",boost::json::object{{"offset",boost::json::object{{"value",0},{"unit","px"}}},{"gap-tolerance",boost::json::object{{"value",0},{"unit","px"}}}}}});
    EXPECT_EQ(tokens.snapshot().active_roots,2u);EXPECT_EQ(tokens.snapshot().active_tokens.size(),3u);
    EXPECT_EQ(r.data.at("parent-token"),a.data.at("analysis-token"));
    EXPECT_EQ(r.data.at("no-contour-count"),0);auto const &pieces=r.data.at("pieces").as_array();ASSERT_EQ(pieces.size(),3u);
    for(unsigned i=0;i<3;++i){EXPECT_EQ(pieces[i].at("index"),i);EXPECT_EQ(pieces[i].at("noContour"),false);EXPECT_EQ(pieces[i].at("topology-count"),1);}
}
TEST_F(P9Independent, bitmap_explode_explode) {
    loadP9(true);auto r=independent("bitmap.explode.explode",{{"id","image1"},{"recipe",boost::json::object{{"refine",false}}}});
    ASSERT_EQ(r.status,Status::Changed);EXPECT_EQ(r.data.at("piece-count"),3);
}
TEST_F(P9Independent, bitmap_explode_create_contour_only) {
    loadP9(true);auto r=independent("bitmap.explode.create-contour-only",{{"id","image1"},{"recipe",boost::json::object{{"refine",false}}},{"contour",boost::json::object{{"offset",boost::json::object{{"value",0},{"unit","px"}}},{"gap-tolerance",boost::json::object{{"value",0},{"unit","px"}}}}}});
    ASSERT_EQ(r.status,Status::Changed);EXPECT_EQ(r.data.at("source-retained"),true);
    EXPECT_EQ(r.data.at("source-pixel-sha256"),"da4c43687e951c39761c898dcad75315d352c3df7caf3f549c30154df40a213a");
}
TEST_F(P9Independent, bitmap_explode_apply_adjustment) {
    loadP9(true);auto r=independent("bitmap.explode.apply-adjustment",{{"id","image1"},{"recipe",boost::json::object{{"refine",false}}}});
    ASSERT_EQ(r.status,Status::Unchanged);EXPECT_EQ(r.data.at("source-retained"),true);
    EXPECT_EQ(r.data.at("source-pixel-sha256"),"da4c43687e951c39761c898dcad75315d352c3df7caf3f549c30154df40a213a");
    auto preview=call("bitmap.explode.apply-adjustment",{{"id","image1"},{"recipe",boost::json::object{{"refine",false}}}},true);
    EXPECT_EQ(preview.data.at("alpha-sha256"),"02ba3808f41e3226e8da312d9c11b4b35baae0e88f62f0e6886e49257b5084bb");
}
}

namespace {
class P6D12 : public P9Independent {};
TEST_F(P6D12, ToneCursor) {
    loadP9();
    doc->ensureUpToDate();
    refusal("bitmap.tone-query",{{"ids",boost::json::array{"image1"}},{"mode","compatible-members"},{"cursor","different:0"}},"invalid-cursor","cursor binding differs");
}
TEST_F(P6D12, HistogramAdmission) {
    loadP9();
    doc->ensureUpToDate();
    refusal("bitmap.histogram",{{"ids",boost::json::array{"shape1"}}},"requires-single-bitmap","single usable image admission");
}
TEST_F(P6D12, HistogramDecode) {
    loadP9();doc->getObjectById("image1")->getRepr()->setAttribute("xlink:href","data:image/png;base64,AAAA");
    doc->ensureUpToDate();
    refusal("bitmap.histogram",{{"ids",boost::json::array{"image1"}}},"missing-source","build_bitmap_histogram: decode unavailable");
}
TEST_F(P6D12, ToneEmpty) {
    loadP9();
    doc->ensureUpToDate();
    refusal("bitmap.tone-set",{{"ids",boost::json::array{"shape1"}},{"patch",boost::json::object{{"brightness",10}}}},"no-eligible-targets","resolved member set empty");
}
TEST_F(P6D12, CopyBudget) {
    loadP9();
    doc->ensureUpToDate();
    refusal("bitmap.copy",{{"ids",boost::json::array{"shape1"}},{"size",boost::json::object{{"width",32000000},{"height",10}}}},"engine-limit","prepare: raster budget exceeded");
}
TEST_F(P6D12, CopyBounds) {
    loadP9();doc->getObjectById("child1")->getRepr()->setAttribute("width","0");
    doc->ensureUpToDate();
    refusal("bitmap.copy",{{"ids",boost::json::array{"child1"}}},"no-bounds","prepare: empty visual/geometric rectangle");
}
TEST_F(P6D12, CopyDecode) {
    loadP9();doc->getObjectById("image1")->getRepr()->setAttribute("xlink:href","data:image/png;base64,AAAA");
    doc->ensureUpToDate();
    refusal("bitmap.copy",{{"ids",boost::json::array{"image1"}}},"missing-source","prepare: decode missing");
}
TEST_F(P6D12, analyze_Admission) {
    loadP9(true);doc->ensureUpToDate();
    refusal("bitmap.explode.analyze",{{"id","shape1"},{"recipe",boost::json::object{}}},"requires-single-bitmap","context resolve: not one embedded bitmap");
}
TEST_F(P6D12, analyze_Unsupported) {
    loadP9(true);doc->getObjectById("image1")->getRepr()->setAttribute("transform","scale(0)");doc->ensureUpToDate();
    refusal("bitmap.explode.analyze",{{"id","image1"},{"recipe",boost::json::object{}}},"unsupported-target","context reason UnsupportedTarget");
}
TEST_F(P6D12, explode_Admission) {
    loadP9(true);doc->ensureUpToDate();
    refusal("bitmap.explode.explode",{{"id","shape1"},{"recipe",boost::json::object{}}},"requires-single-bitmap","context resolve: not one embedded bitmap");
}
TEST_F(P6D12, explode_Unsupported) {
    loadP9(true);doc->getObjectById("image1")->getRepr()->setAttribute("transform","scale(0)");doc->ensureUpToDate();
    refusal("bitmap.explode.explode",{{"id","image1"},{"recipe",boost::json::object{}}},"unsupported-target","context reason UnsupportedTarget");
}
TEST_F(P6D12, create_contour_only_Admission) {
    loadP9(true);doc->ensureUpToDate();
    refusal("bitmap.explode.create-contour-only",{{"id","shape1"},{"recipe",boost::json::object{}},{"contour",boost::json::object{}}},"requires-single-bitmap","context resolve: not one embedded bitmap");
}
TEST_F(P6D12, create_contour_only_Unsupported) {
    loadP9(true);doc->getObjectById("image1")->getRepr()->setAttribute("transform","scale(0)");doc->ensureUpToDate();
    refusal("bitmap.explode.create-contour-only",{{"id","image1"},{"recipe",boost::json::object{}},{"contour",boost::json::object{}}},"unsupported-target","context reason UnsupportedTarget");
}
TEST_F(P6D12, apply_adjustment_Admission) {
    loadP9(true);doc->ensureUpToDate();
    refusal("bitmap.explode.apply-adjustment",{{"id","shape1"},{"recipe",boost::json::object{}}},"requires-single-bitmap","context resolve: not one embedded bitmap");
}
TEST_F(P6D12, apply_adjustment_Unsupported) {
    loadP9(true);doc->getObjectById("image1")->getRepr()->setAttribute("transform","scale(0)");doc->ensureUpToDate();
    refusal("bitmap.explode.apply-adjustment",{{"id","image1"},{"recipe",boost::json::object{}}},"unsupported-target","context reason UnsupportedTarget");
}
TEST_F(P6D12, AnalyzeEncoding) {
    loadP9(true);
    auto raw=gdk_pixbuf_new(GDK_COLORSPACE_RGB,true,8,4,4);gdk_pixbuf_fill(raw,0x80808096);
    Pixbuf pixels(raw);auto href=sp_image_encode_png_data_uri(pixels);ASSERT_TRUE(href);
    doc->getObjectById("image1")->getRepr()->setAttribute("xlink:href",*href);doc->ensureUpToDate();
    refusal("bitmap.explode.analyze",{{"id","image1"},{"recipe",boost::json::object{}}},"analysis-failed","context reason EncodingFailed during analysis encoding");
}
}

namespace {
TEST_F(P6D12, CopyUnsupported) {
    loadP9();doc->getObjectById("shape1")->getRepr()->setAttribute("transform","scale(0)");doc->ensureUpToDate();
    refusal("bitmap.copy",{{"ids",boost::json::array{"shape1"}}},"unsupported-target","context reason UnsupportedTarget");
}
TEST_F(P6D12, CopySettlementRollback) {
    loadP9();unsigned checks=0;context.cancelled=[&] {return ++checks>=4;};
    refusal("bitmap.copy",{{"ids",boost::json::array{"shape1"}}},"publication-failed","publish failed; caller rollback");
    context.cancelled={};EXPECT_EQ(checks,4u);
}
TEST_F(P6D12, ToneSettlementRollback) {
    loadP9();unsigned checks=0;context.cancelled=[&] {return ++checks>=4;};
    refusal("bitmap.tone-set",{{"ids",boost::json::array{"image1"}},{"patch",boost::json::object{{"brightness",10}}}},"internal-error","unexpected service exception; rollback before return");
    context.cancelled={};EXPECT_EQ(checks,4u);
}
}

namespace {
TEST_F(P6D12, explode_Busy) {
    loadP9(true);
    auto guard=DocumentUndo::beginAtomicInteraction(doc.get());ASSERT_TRUE(guard);
    refusal("bitmap.explode.explode",{{"id","image1"},{"recipe",boost::json::object{}}},"document-busy","context reason DocumentBusy");
    guard->rollback();
}
TEST_F(P6D12, create_contour_only_Busy) {
    loadP9(true);
    auto guard=DocumentUndo::beginAtomicInteraction(doc.get());ASSERT_TRUE(guard);
    refusal("bitmap.explode.create-contour-only",{{"id","image1"},{"recipe",boost::json::object{}},{"contour",boost::json::object{}}},"document-busy","context reason DocumentBusy");
    guard->rollback();
}
TEST_F(P6D12, apply_adjustment_Busy) {
    loadP9(true);
    auto raw=gdk_pixbuf_new(GDK_COLORSPACE_RGB,true,8,8,8);gdk_pixbuf_fill(raw,0x80808096);
    Pixbuf pixels(raw);auto href=sp_image_encode_png_data_uri(pixels);ASSERT_TRUE(href);
    doc->getObjectById("image1")->getRepr()->setAttribute("xlink:href",*href);
    DocumentUndo::done(doc.get(),Util::Internal::ContextString("P6 alpha busy fixture"),"");doc->ensureUpToDate();
    auto guard=DocumentUndo::beginAtomicInteraction(doc.get());ASSERT_TRUE(guard);
    refusal("bitmap.explode.apply-adjustment",{{"id","image1"},{"recipe",boost::json::object{}}},"document-busy","context reason DocumentBusy");
    guard->rollback();
}
}

namespace {
TEST_F(P6D12, ToneQueryEmptyD15) {
    loadP9();
    auto before=state();
    auto params=boost::json::object{{"ids",boost::json::array{"shape1"}},{"mode","compatible-members"}};
    auto result=call("bitmap.tone-query",params);
    ASSERT_EQ(result.status,Status::Rejected);ASSERT_TRUE(result.error);
    EXPECT_EQ(result.error->code,"no-eligible-targets");EXPECT_EQ(result.eligible,0u);
    ASSERT_EQ(result.excluded.size(),1u);
    EXPECT_EQ(result.excluded[0].id,"shape1");EXPECT_EQ(result.excluded[0].reason,"unsupported-target");
    EXPECT_EQ(result.error_details.at("exclusions"),boost::json::parse(R"([{"id":"shape1","reason":"unsupported-target"}])"));
    EXPECT_EQ(result.error_details.at("mutation_state"),"none");EXPECT_EQ(state(),before);
    auto preview=call("bitmap.tone-query",params,true);
    EXPECT_EQ(preview.status,Status::Rejected);EXPECT_EQ(preview.error_details,result.error_details);EXPECT_EQ(state(),before);
    refusal("bitmap.tone-query",params,"no-eligible-targets","resolved member set empty");
}
TEST_F(P6D12, CopyAtomicAdmission) {
    loadP9();DocumentUndo::setUndoSensitive(doc.get(),false);
    refusal("bitmap.copy",{{"ids",boost::json::array{"shape1"}}},"transaction-unavailable","EditTransaction inactive","identity/admission");
    DocumentUndo::setUndoSensitive(doc.get(),true);
}
TEST_F(P6D12, ToneAtomicAdmission) {
    loadP9();DocumentUndo::setUndoSensitive(doc.get(),false);
    refusal("bitmap.tone-set",{{"ids",boost::json::array{"image1"}},{"patch",boost::json::object{{"brightness",10}}}},"transaction-unavailable","EditTransaction inactive","identity/admission");
    DocumentUndo::setUndoSensitive(doc.get(),true);
}
TEST_F(P6D12, ExplodeEncoding) {
    loadP9(true);
    auto raw=gdk_pixbuf_new(GDK_COLORSPACE_RGB,true,8,4,4);gdk_pixbuf_fill(raw,0x80808096);
    Pixbuf pixels(raw);auto href=sp_image_encode_png_data_uri(pixels);ASSERT_TRUE(href);
    doc->getObjectById("image1")->getRepr()->setAttribute("xlink:href",*href);doc->ensureUpToDate();
    refusal("bitmap.explode.explode",{{"id","image1"},{"recipe",boost::json::object{}},{"contour",boost::json::object{}}},"encoding-failed","context reason EncodingFailed");
}
TEST_F(P6D12, ContourOnlyEncoding) {
    loadP9(true);
    auto raw=gdk_pixbuf_new(GDK_COLORSPACE_RGB,true,8,4,4);gdk_pixbuf_fill(raw,0x80808096);
    Pixbuf pixels(raw);auto href=sp_image_encode_png_data_uri(pixels);ASSERT_TRUE(href);
    doc->getObjectById("image1")->getRepr()->setAttribute("xlink:href",*href);doc->ensureUpToDate();
    refusal("bitmap.explode.create-contour-only",{{"id","image1"},{"recipe",boost::json::object{}},{"contour",boost::json::object{}}},"encoding-failed","context reason EncodingFailed");
}
}

namespace {
class P6Fault : public P6D12 {
protected:
    void alphaFixture() {
        auto raw=gdk_pixbuf_new(GDK_COLORSPACE_RGB,true,8,64,64);gdk_pixbuf_fill(raw,0x80808096);
        Pixbuf pixels(raw);auto href=sp_image_encode_png_data_uri(pixels);ASSERT_TRUE(href);
        doc->getObjectById("image1")->getRepr()->setAttribute("xlink:href",*href);
        DocumentUndo::done(doc.get(),Util::Internal::ContextString("fault alpha fixture"),"");doc->ensureUpToDate();
    }
    void faultRefusal(std::string command,boost::json::object params,std::string code,std::string branch,
                      std::string point,CliFaultKind kind,std::string mutation) {
        auto before=state();
        ScopedCliFaultPlanForTesting scope({{{point,kind}}, {}});
        auto result=call(command,params);auto after=state();
        ASSERT_EQ(scope.plan().faults[0].visits,1u);
        ASSERT_EQ(result.status,Status::Rejected)<<result.message;
        ASSERT_TRUE(result.error);ASSERT_EQ(result.error->code,code);ASSERT_EQ(before,after);
        ASSERT_EQ(result.error_details.at("mutation_state"),boost::json::value(mutation));
        ASSERT_FALSE(DocumentUndo::interactionActive(doc.get()));
        std::string native=branch.substr(0,branch.find(" ("));
        if(code=="publication-failed")native="context reason PublicationFailed";
        ASSERT_EQ(result.error_details.at("reason"),boost::json::value(native));
        std::cout<<"P9-OBSERVATION "<<boost::json::serialize(boost::json::object{
            {"obligation","P9:"+command+":command-service:"+code+":"+branch},{"command",command},{"code",code},{"branch",branch},
            {"mutation_state",mutation},{"before",before},{"after",after},{"settled",true},
            {"actual",boost::json::object{{"fault_point",point},{"visits",scope.plan().faults[0].visits},{"interaction_active",false}}}})<<std::endl;
    }
};
TEST_F(P6Fault, ToneCountedLimit) {
    loadP9();auto before=state();
    ScopedCliFaultPlanForTesting scope({{},{{"bitmap.tone-set.members",0}}});
    auto result=call("bitmap.tone-set",{{"ids",boost::json::array{"image1"}},{"patch",boost::json::object{{"brightness",10}}}});
    ASSERT_TRUE(result.error);ASSERT_EQ(result.error->code,"engine-limit");ASSERT_EQ(state(),before);
    ASSERT_FALSE(DocumentUndo::interactionActive(doc.get()));
    std::cout<<"P9-OBSERVATION "<<boost::json::serialize(boost::json::object{{"obligation","P9:bitmap.tone-set:command-service:engine-limit:resolved member count exceeds 100000 before mutation"},{"command","bitmap.tone-set"},{"code","engine-limit"},
      {"branch","resolved member count exceeds 100000 before mutation"},{"mutation_state","none"},{"before",before},{"after",state()},{"settled",true},
      {"actual",boost::json::object{{"resolved_members",1},{"test_ceiling",0},{"production_ceiling",100000}}}})<<std::endl;
}
TEST_F(P6Fault, Branch3) {
    loadP9();
    faultRefusal("bitmap.copy",boost::json::parse(R"params({"ids": ["shape1"]})params").as_object(),"rasterization-failed","prepare: renderer failed","bitmap.copy.before-render-surface",CliFaultKind::Renderer,"none");
}
TEST_F(P6Fault, Branch4) {
    loadP9();
    faultRefusal("bitmap.copy",boost::json::parse(R"params({"ids": ["shape1"]})params").as_object(),"encoding-failed","publish: encode failed","bitmap.copy.after-prepare-before-publication",CliFaultKind::Encoding,"none");
}
TEST_F(P6Fault, Branch6) {
    loadP9();
    faultRefusal("bitmap.copy",boost::json::parse(R"params({"ids": ["shape1"]})params").as_object(),"stale-dependency","context reason StaleCapture","bitmap.copy.before-publication-validation",CliFaultKind::StaleCapture,"none");
}
TEST_F(P6Fault, Branch7) {
    loadP9(true);
    faultRefusal("bitmap.explode.analyze",boost::json::parse(R"params({"id": "image1", "recipe": {"refine": false}})params").as_object(),"analysis-failed","context reason AnalysisFailed (full recipe only for publication)","bitmap.explode.analyze.before-analysis",CliFaultKind::Analysis,"none");
}
TEST_F(P6Fault, Branch8) {
    loadP9(true);
    faultRefusal("bitmap.explode.analyze",boost::json::parse(R"params({"id": "image1", "recipe": {"refine": false}})params").as_object(),"memory-admission-failed","context reason MemoryAdmissionFailed","bitmap.explode.analyze.before-memory-admission",CliFaultKind::MemoryAdmission,"none");
}
TEST_F(P6Fault, Branch10) {
    loadP9(true);auto retained=call("bitmap.explode.analyze",{{"id","image1"},{"recipe",boost::json::object{{"refine",false}}}});ASSERT_EQ(retained.status,Status::Ok);
    faultRefusal("bitmap.explode.contour",boost::json::object{{"analysis-token",retained.data.at("analysis-token")},{"contour",boost::json::object{}}},"memory-admission-failed","context reason MemoryAdmissionFailed","bitmap.explode.contour.before-contour-memory-admission",CliFaultKind::MemoryAdmission,"none");
}
TEST_F(P6Fault, Branch11) {
    loadP9(true);auto retained=call("bitmap.explode.analyze",{{"id","image1"},{"recipe",boost::json::object{{"refine",false}}}});ASSERT_EQ(retained.status,Status::Ok);
    faultRefusal("bitmap.explode.contour",boost::json::object{{"analysis-token",retained.data.at("analysis-token")},{"contour",boost::json::object{}}},"contour-failed","context reason ContourFailed (requested contour stage)","bitmap.explode.contour.before-contour",CliFaultKind::Contour,"none");
}
TEST_F(P6Fault, Branch13) {
    loadP9(true);
    faultRefusal("bitmap.explode.explode",boost::json::parse(R"params({"id": "image1", "recipe": {"refine": false}})params").as_object(),"analysis-failed","context reason AnalysisFailed (full recipe only for publication)","bitmap.explode.explode.before-analysis",CliFaultKind::Analysis,"none");
}
TEST_F(P6Fault, Branch14) {
    loadP9(true);
    faultRefusal("bitmap.explode.explode",boost::json::parse(R"params({"id": "image1", "recipe": {"refine": false}})params").as_object(),"memory-admission-failed","context reason MemoryAdmissionFailed","bitmap.explode.explode.before-memory-admission",CliFaultKind::MemoryAdmission,"none");
}
TEST_F(P6Fault, Branch15) {
    loadP9(true);
    faultRefusal("bitmap.explode.explode",boost::json::parse(R"params({"id": "image1", "recipe": {"refine": false}, "contour": {}, "contours": true})params").as_object(),"contour-failed","context reason ContourFailed (requested contour stage)","bitmap.explode.explode.before-contour",CliFaultKind::Contour,"none");
}
TEST_F(P6Fault, Branch16) {
    loadP9(true);
    faultRefusal("bitmap.explode.explode",boost::json::parse(R"params({"id": "image1", "recipe": {"refine": false}})params").as_object(),"publication-failed","context reason PublicationFailed; native rollback","bitmap.explode.explode.after-native-before-settlement",CliFaultKind::Publication,"rolled-back");
}
TEST_F(P6Fault, Branch17) {
    loadP9(true);
    faultRefusal("bitmap.explode.explode",boost::json::parse(R"params({"id": "image1", "recipe": {"refine": false}})params").as_object(),"stale-dependency","context reason StaleCapture","bitmap.explode.explode.before-publication-validation",CliFaultKind::StaleCapture,"none");
}
TEST_F(P6Fault, Branch19) {
    loadP9(true);
    faultRefusal("bitmap.explode.create-contour-only",boost::json::parse(R"params({"id": "image1", "recipe": {"refine": false}, "contour": {}})params").as_object(),"analysis-failed","context reason AnalysisFailed (full recipe only for publication)","bitmap.explode.create-contour-only.before-analysis",CliFaultKind::Analysis,"none");
}
TEST_F(P6Fault, Branch20) {
    loadP9(true);
    faultRefusal("bitmap.explode.create-contour-only",boost::json::parse(R"params({"id": "image1", "recipe": {"refine": false}, "contour": {}})params").as_object(),"memory-admission-failed","context reason MemoryAdmissionFailed","bitmap.explode.create-contour-only.before-memory-admission",CliFaultKind::MemoryAdmission,"none");
}
TEST_F(P6Fault, Branch21) {
    loadP9(true);
    faultRefusal("bitmap.explode.create-contour-only",boost::json::parse(R"params({"id": "image1", "recipe": {"refine": false}, "contour": {}})params").as_object(),"contour-failed","context reason ContourFailed (requested contour stage)","bitmap.explode.create-contour-only.before-contour",CliFaultKind::Contour,"none");
}
TEST_F(P6Fault, Branch22) {
    loadP9(true);
    faultRefusal("bitmap.explode.create-contour-only",boost::json::parse(R"params({"id": "image1", "recipe": {"refine": false}, "contour": {}})params").as_object(),"publication-failed","context reason PublicationFailed; native rollback","bitmap.explode.create-contour-only.after-native-before-settlement",CliFaultKind::Publication,"rolled-back");
}
TEST_F(P6Fault, Branch23) {
    loadP9(true);
    faultRefusal("bitmap.explode.create-contour-only",boost::json::parse(R"params({"id": "image1", "recipe": {"refine": false}, "contour": {}})params").as_object(),"stale-dependency","context reason StaleCapture","bitmap.explode.create-contour-only.before-publication-validation",CliFaultKind::StaleCapture,"none");
}
TEST_F(P6Fault, Branch25) {
    loadP9(true);alphaFixture();
    faultRefusal("bitmap.explode.apply-adjustment",boost::json::parse(R"params({"id": "image1", "recipe": {}})params").as_object(),"analysis-failed","context reason AnalysisFailed (full recipe only for publication)","bitmap.explode.apply-adjustment.before-analysis",CliFaultKind::Analysis,"none");
}
TEST_F(P6Fault, Branch26) {
    loadP9(true);alphaFixture();
    faultRefusal("bitmap.explode.apply-adjustment",boost::json::parse(R"params({"id": "image1", "recipe": {}})params").as_object(),"memory-admission-failed","context reason MemoryAdmissionFailed","bitmap.explode.apply-adjustment.before-memory-admission",CliFaultKind::MemoryAdmission,"none");
}
TEST_F(P6Fault, Branch27) {
    loadP9(true);alphaFixture();
    faultRefusal("bitmap.explode.apply-adjustment",boost::json::parse(R"params({"id": "image1", "recipe": {}})params").as_object(),"encoding-failed","context reason EncodingFailed","bitmap.explode.apply-adjustment.after-analysis-before-encoding-receipt",CliFaultKind::Encoding,"none");
}
TEST_F(P6Fault, Branch28) {
    loadP9(true);alphaFixture();
    faultRefusal("bitmap.explode.apply-adjustment",boost::json::parse(R"params({"id": "image1", "recipe": {}})params").as_object(),"publication-failed","context reason PublicationFailed; native rollback","bitmap.explode.apply-adjustment.after-native-before-settlement",CliFaultKind::Publication,"rolled-back");
}
TEST_F(P6Fault, Branch29) {
    loadP9(true);alphaFixture();
    faultRefusal("bitmap.explode.apply-adjustment",boost::json::parse(R"params({"id": "image1", "recipe": {}})params").as_object(),"stale-dependency","context reason StaleCapture","bitmap.explode.apply-adjustment.before-publication-validation",CliFaultKind::StaleCapture,"none");
}
}

namespace {
boost::json::object faultEngineState(DispatchContext &ctx,TokenStore &tokens) {
    auto h=observeHistory(*ctx.document);EXPECT_TRUE(h.value);
    boost::json::array selection,active;for(auto item:ctx.selection->items())selection.emplace_back(item->getId());
    auto t=tokens.snapshot();for(auto const &id:t.active_tokens)active.emplace_back(id);
    auto label=[](auto const &v)->boost::json::value{return v ? boost::json::value(*v):boost::json::value(nullptr);};
    return {{"xml",sp_repr_save_buf(ctx.document->getReprDoc()).raw()},{"selection",selection},
      {"document_revision",document_stamp(ctx.document).revision},{"session_revision",ctx.session_revision},
      {"undo",boost::json::object{{"available",h.value->can_undo},{"label",label(h.value->next_undo_label)}}},
      {"redo",boost::json::object{{"available",h.value->can_redo},{"label",label(h.value->next_redo_label)}}},
      {"tokens",boost::json::object{{"active",active},{"roots",t.active_roots},{"charged_bytes",t.charged_bytes}}}};
}
TEST_F(P6Fault, EngineException0) {
    SessionOptions options;options.document_path=(std::filesystem::path(__FILE__).parent_path().parent_path()/"cli_tests/vacards-agent/fixtures/m3/explode.svg").string();options.grants.read_files={options.document_path};
    with_cli_engine_for_testing(options,[&](auto &ctx,auto &files,auto &store,auto const &execute) {
        auto &d=*ctx.document;DocumentUndo::setUndoSensitive(&d,true);DocumentUndo::clearUndo(&d);DocumentUndo::clearRedo(&d);
        d.getReprRoot()->setAttribute("data-fault-seed","undo");DocumentUndo::done(&d,Util::Internal::ContextString("fault undo"),"");
        d.getReprRoot()->setAttribute("data-fault-seed","redo");DocumentUndo::done(&d,Util::Internal::ContextString("fault redo"),"");ASSERT_TRUE(DocumentUndo::undo(&d));d.ensureUpToDate();
        ctx.selection->set(cast<SPItem>(d.getObjectById("untouched")));
        auto requestFor=[&](std::string cmd,boost::json::object params) {Request req;req.id="p6-fault";req.command=cmd;req.params=std::move(params);req.document=document_stamp(&d).id;req.if_revision=document_stamp(&d).revision;return req;};
        auto analysis=execute(requestFor("bitmap.explode.analyze",{{"id","image1"},{"recipe",boost::json::object{{"refine",false}}}}));ASSERT_EQ(analysis.status,Status::Ok)<<analysis.message;
        ASSERT_GT(store.snapshot().charged_bytes,0u);
        auto request=requestFor("bitmap.tone-query",boost::json::parse(R"params({"ids": ["image1"]})params").as_object());auto before=faultEngineState(ctx,store);
        ScopedCliFaultPlanForTesting scope({{{"bitmap.tone-query.before-aggregate",CliFaultKind::ServiceException}}, {}});
        auto result=execute(request);auto after=faultEngineState(ctx,store);
        ASSERT_EQ(scope.plan().faults[0].visits,1u);ASSERT_EQ(result.status,Status::Failed)<<result.message;
        ASSERT_EQ(result.reason,"internal-error");ASSERT_EQ(before,after);ASSERT_FALSE(DocumentUndo::interactionActive(&d));
        std::cout<<"P9-OBSERVATION "<<boost::json::serialize(boost::json::object{{"obligation","P9:bitmap.tone-query:command-service:internal-error:unexpected service exception; rollback before return"},{"command","bitmap.tone-query"},{"code",result.reason},{"branch","unexpected service exception; rollback before return"},
          {"mutation_state","none"},{"before",before},{"after",after},{"settled",true},
          {"actual",boost::json::object{{"fault_point","bitmap.tone-query.before-aggregate"},{"visits",scope.plan().faults[0].visits},{"interaction_active",false},{"result",typed_result(result,request.id,1)}}}})<<std::endl;
    });
}
TEST_F(P6Fault, EngineException1) {
    SessionOptions options;options.document_path=(std::filesystem::path(__FILE__).parent_path().parent_path()/"cli_tests/vacards-agent/fixtures/m3/explode.svg").string();options.grants.read_files={options.document_path};
    with_cli_engine_for_testing(options,[&](auto &ctx,auto &files,auto &store,auto const &execute) {
        auto &d=*ctx.document;DocumentUndo::setUndoSensitive(&d,true);DocumentUndo::clearUndo(&d);DocumentUndo::clearRedo(&d);
        d.getReprRoot()->setAttribute("data-fault-seed","undo");DocumentUndo::done(&d,Util::Internal::ContextString("fault undo"),"");
        d.getReprRoot()->setAttribute("data-fault-seed","redo");DocumentUndo::done(&d,Util::Internal::ContextString("fault redo"),"");ASSERT_TRUE(DocumentUndo::undo(&d));d.ensureUpToDate();
        ctx.selection->set(cast<SPItem>(d.getObjectById("untouched")));
        auto requestFor=[&](std::string cmd,boost::json::object params) {Request req;req.id="p6-fault";req.command=cmd;req.params=std::move(params);req.document=document_stamp(&d).id;req.if_revision=document_stamp(&d).revision;return req;};
        auto analysis=execute(requestFor("bitmap.explode.analyze",{{"id","image1"},{"recipe",boost::json::object{{"refine",false}}}}));ASSERT_EQ(analysis.status,Status::Ok)<<analysis.message;
        ASSERT_GT(store.snapshot().charged_bytes,0u);
        auto request=requestFor("bitmap.histogram",boost::json::parse(R"params({"ids": ["image1"]})params").as_object());auto before=faultEngineState(ctx,store);
        ScopedCliFaultPlanForTesting scope({{{"bitmap.histogram.before-histogram",CliFaultKind::ServiceException}}, {}});
        auto result=execute(request);auto after=faultEngineState(ctx,store);
        ASSERT_EQ(scope.plan().faults[0].visits,1u);ASSERT_EQ(result.status,Status::Failed)<<result.message;
        ASSERT_EQ(result.reason,"internal-error");ASSERT_EQ(before,after);ASSERT_FALSE(DocumentUndo::interactionActive(&d));
        std::cout<<"P9-OBSERVATION "<<boost::json::serialize(boost::json::object{{"obligation","P9:bitmap.histogram:command-service:internal-error:unexpected service exception; rollback before return"},{"command","bitmap.histogram"},{"code",result.reason},{"branch","unexpected service exception; rollback before return"},
          {"mutation_state","none"},{"before",before},{"after",after},{"settled",true},
          {"actual",boost::json::object{{"fault_point","bitmap.histogram.before-histogram"},{"visits",scope.plan().faults[0].visits},{"interaction_active",false},{"result",typed_result(result,request.id,1)}}}})<<std::endl;
    });
}
TEST_F(P6Fault, EngineException2) {
    SessionOptions options;options.document_path=(std::filesystem::path(__FILE__).parent_path().parent_path()/"cli_tests/vacards-agent/fixtures/m3/explode.svg").string();options.grants.read_files={options.document_path};
    with_cli_engine_for_testing(options,[&](auto &ctx,auto &files,auto &store,auto const &execute) {
        auto &d=*ctx.document;DocumentUndo::setUndoSensitive(&d,true);DocumentUndo::clearUndo(&d);DocumentUndo::clearRedo(&d);
        d.getReprRoot()->setAttribute("data-fault-seed","undo");DocumentUndo::done(&d,Util::Internal::ContextString("fault undo"),"");
        d.getReprRoot()->setAttribute("data-fault-seed","redo");DocumentUndo::done(&d,Util::Internal::ContextString("fault redo"),"");ASSERT_TRUE(DocumentUndo::undo(&d));d.ensureUpToDate();
        ctx.selection->set(cast<SPItem>(d.getObjectById("untouched")));
        auto requestFor=[&](std::string cmd,boost::json::object params) {Request req;req.id="p6-fault";req.command=cmd;req.params=std::move(params);req.document=document_stamp(&d).id;req.if_revision=document_stamp(&d).revision;return req;};
        auto analysis=execute(requestFor("bitmap.explode.analyze",{{"id","image1"},{"recipe",boost::json::object{{"refine",false}}}}));ASSERT_EQ(analysis.status,Status::Ok)<<analysis.message;
        ASSERT_GT(store.snapshot().charged_bytes,0u);
        auto request=requestFor("bitmap.copy",boost::json::parse(R"params({"ids": ["untouched"]})params").as_object());auto before=faultEngineState(ctx,store);
        ScopedCliFaultPlanForTesting scope({{{"bitmap.copy.after-publication-before-settlement",CliFaultKind::ServiceException}}, {}});
        auto result=execute(request);auto after=faultEngineState(ctx,store);
        ASSERT_EQ(scope.plan().faults[0].visits,1u);ASSERT_EQ(result.status,Status::Failed)<<result.message;
        ASSERT_EQ(result.reason,"internal-error");ASSERT_EQ(before,after);ASSERT_FALSE(DocumentUndo::interactionActive(&d));
        std::cout<<"P9-OBSERVATION "<<boost::json::serialize(boost::json::object{{"obligation","P9:bitmap.copy:command-service:internal-error:unexpected service exception; rollback before return"},{"command","bitmap.copy"},{"code",result.reason},{"branch","unexpected service exception; rollback before return"},
          {"mutation_state","rolled-back"},{"before",before},{"after",after},{"settled",true},
          {"actual",boost::json::object{{"fault_point","bitmap.copy.after-publication-before-settlement"},{"visits",scope.plan().faults[0].visits},{"interaction_active",false},{"result",typed_result(result,request.id,1)}}}})<<std::endl;
    });
}
TEST_F(P6Fault, EngineException3) {
    SessionOptions options;options.document_path=(std::filesystem::path(__FILE__).parent_path().parent_path()/"cli_tests/vacards-agent/fixtures/m3/explode.svg").string();options.grants.read_files={options.document_path};
    with_cli_engine_for_testing(options,[&](auto &ctx,auto &files,auto &store,auto const &execute) {
        auto &d=*ctx.document;DocumentUndo::setUndoSensitive(&d,true);DocumentUndo::clearUndo(&d);DocumentUndo::clearRedo(&d);
        d.getReprRoot()->setAttribute("data-fault-seed","undo");DocumentUndo::done(&d,Util::Internal::ContextString("fault undo"),"");
        d.getReprRoot()->setAttribute("data-fault-seed","redo");DocumentUndo::done(&d,Util::Internal::ContextString("fault redo"),"");ASSERT_TRUE(DocumentUndo::undo(&d));d.ensureUpToDate();
        ctx.selection->set(cast<SPItem>(d.getObjectById("untouched")));
        auto requestFor=[&](std::string cmd,boost::json::object params) {Request req;req.id="p6-fault";req.command=cmd;req.params=std::move(params);req.document=document_stamp(&d).id;req.if_revision=document_stamp(&d).revision;return req;};
        auto analysis=execute(requestFor("bitmap.explode.analyze",{{"id","image1"},{"recipe",boost::json::object{{"refine",false}}}}));ASSERT_EQ(analysis.status,Status::Ok)<<analysis.message;
        ASSERT_GT(store.snapshot().charged_bytes,0u);
        auto request=requestFor("bitmap.explode.analyze",boost::json::parse(R"params({"id": "image1", "recipe": {"refine": false}})params").as_object());auto before=faultEngineState(ctx,store);
        ScopedCliFaultPlanForTesting scope({{{"bitmap.explode.analyze.after-analysis",CliFaultKind::ServiceException}}, {}});
        auto result=execute(request);auto after=faultEngineState(ctx,store);
        ASSERT_EQ(scope.plan().faults[0].visits,1u);ASSERT_EQ(result.status,Status::Failed)<<result.message;
        ASSERT_EQ(result.reason,"internal-error");ASSERT_EQ(before,after);ASSERT_FALSE(DocumentUndo::interactionActive(&d));
        std::cout<<"P9-OBSERVATION "<<boost::json::serialize(boost::json::object{{"obligation","P9:bitmap.explode.analyze:command-service:internal-error:unexpected service exception; rollback before return"},{"command","bitmap.explode.analyze"},{"code",result.reason},{"branch","unexpected service exception; rollback before return"},
          {"mutation_state","none"},{"before",before},{"after",after},{"settled",true},
          {"actual",boost::json::object{{"fault_point","bitmap.explode.analyze.after-analysis"},{"visits",scope.plan().faults[0].visits},{"interaction_active",false},{"result",typed_result(result,request.id,1)}}}})<<std::endl;
    });
}
TEST_F(P6Fault, EngineException4) {
    SessionOptions options;options.document_path=(std::filesystem::path(__FILE__).parent_path().parent_path()/"cli_tests/vacards-agent/fixtures/m3/explode.svg").string();options.grants.read_files={options.document_path};
    with_cli_engine_for_testing(options,[&](auto &ctx,auto &files,auto &store,auto const &execute) {
        auto &d=*ctx.document;DocumentUndo::setUndoSensitive(&d,true);DocumentUndo::clearUndo(&d);DocumentUndo::clearRedo(&d);
        d.getReprRoot()->setAttribute("data-fault-seed","undo");DocumentUndo::done(&d,Util::Internal::ContextString("fault undo"),"");
        d.getReprRoot()->setAttribute("data-fault-seed","redo");DocumentUndo::done(&d,Util::Internal::ContextString("fault redo"),"");ASSERT_TRUE(DocumentUndo::undo(&d));d.ensureUpToDate();
        ctx.selection->set(cast<SPItem>(d.getObjectById("untouched")));
        auto requestFor=[&](std::string cmd,boost::json::object params) {Request req;req.id="p6-fault";req.command=cmd;req.params=std::move(params);req.document=document_stamp(&d).id;req.if_revision=document_stamp(&d).revision;return req;};
        auto analysis=execute(requestFor("bitmap.explode.analyze",{{"id","image1"},{"recipe",boost::json::object{{"refine",false}}}}));ASSERT_EQ(analysis.status,Status::Ok)<<analysis.message;
        ASSERT_GT(store.snapshot().charged_bytes,0u);
        auto request=requestFor("bitmap.explode.contour",boost::json::object{{"analysis-token",analysis.data.at("analysis-token")},{"contour",boost::json::object{}}});auto before=faultEngineState(ctx,store);
        ScopedCliFaultPlanForTesting scope({{{"bitmap.explode.contour.after-contour",CliFaultKind::ServiceException}}, {}});
        auto result=execute(request);auto after=faultEngineState(ctx,store);
        ASSERT_EQ(scope.plan().faults[0].visits,1u);ASSERT_EQ(result.status,Status::Failed)<<result.message;
        ASSERT_EQ(result.reason,"internal-error");ASSERT_EQ(before,after);ASSERT_FALSE(DocumentUndo::interactionActive(&d));
        std::cout<<"P9-OBSERVATION "<<boost::json::serialize(boost::json::object{{"obligation","P9:bitmap.explode.contour:command-service:internal-error:unexpected service exception; rollback before return"},{"command","bitmap.explode.contour"},{"code",result.reason},{"branch","unexpected service exception; rollback before return"},
          {"mutation_state","none"},{"before",before},{"after",after},{"settled",true},
          {"actual",boost::json::object{{"fault_point","bitmap.explode.contour.after-contour"},{"visits",scope.plan().faults[0].visits},{"interaction_active",false},{"result",typed_result(result,request.id,1)}}}})<<std::endl;
    });
}
TEST_F(P6Fault, EngineException5) {
    SessionOptions options;options.document_path=(std::filesystem::path(__FILE__).parent_path().parent_path()/"cli_tests/vacards-agent/fixtures/m3/explode.svg").string();options.grants.read_files={options.document_path};
    with_cli_engine_for_testing(options,[&](auto &ctx,auto &files,auto &store,auto const &execute) {
        auto &d=*ctx.document;DocumentUndo::setUndoSensitive(&d,true);DocumentUndo::clearUndo(&d);DocumentUndo::clearRedo(&d);
        d.getReprRoot()->setAttribute("data-fault-seed","undo");DocumentUndo::done(&d,Util::Internal::ContextString("fault undo"),"");
        d.getReprRoot()->setAttribute("data-fault-seed","redo");DocumentUndo::done(&d,Util::Internal::ContextString("fault redo"),"");ASSERT_TRUE(DocumentUndo::undo(&d));d.ensureUpToDate();
        ctx.selection->set(cast<SPItem>(d.getObjectById("untouched")));
        auto requestFor=[&](std::string cmd,boost::json::object params) {Request req;req.id="p6-fault";req.command=cmd;req.params=std::move(params);req.document=document_stamp(&d).id;req.if_revision=document_stamp(&d).revision;return req;};
        auto analysis=execute(requestFor("bitmap.explode.analyze",{{"id","image1"},{"recipe",boost::json::object{{"refine",false}}}}));ASSERT_EQ(analysis.status,Status::Ok)<<analysis.message;
        ASSERT_GT(store.snapshot().charged_bytes,0u);
        auto request=requestFor("bitmap.explode.explode",boost::json::parse(R"params({"id": "image1", "recipe": {"refine": false}})params").as_object());auto before=faultEngineState(ctx,store);
        ScopedCliFaultPlanForTesting scope({{{"bitmap.explode.explode.after-native-before-settlement",CliFaultKind::ServiceException}}, {}});
        auto result=execute(request);auto after=faultEngineState(ctx,store);
        ASSERT_EQ(scope.plan().faults[0].visits,1u);ASSERT_EQ(result.status,Status::Failed)<<result.message;
        ASSERT_EQ(result.reason,"internal-error");ASSERT_EQ(before,after);ASSERT_FALSE(DocumentUndo::interactionActive(&d));
        std::cout<<"P9-OBSERVATION "<<boost::json::serialize(boost::json::object{{"obligation","P9:bitmap.explode.explode:command-service:internal-error:unexpected service exception; rollback before return"},{"command","bitmap.explode.explode"},{"code",result.reason},{"branch","unexpected service exception; rollback before return"},
          {"mutation_state","rolled-back"},{"before",before},{"after",after},{"settled",true},
          {"actual",boost::json::object{{"fault_point","bitmap.explode.explode.after-native-before-settlement"},{"visits",scope.plan().faults[0].visits},{"interaction_active",false},{"result",typed_result(result,request.id,1)}}}})<<std::endl;
    });
}
TEST_F(P6Fault, EngineException6) {
    SessionOptions options;options.document_path=(std::filesystem::path(__FILE__).parent_path().parent_path()/"cli_tests/vacards-agent/fixtures/m3/explode.svg").string();options.grants.read_files={options.document_path};
    with_cli_engine_for_testing(options,[&](auto &ctx,auto &files,auto &store,auto const &execute) {
        auto &d=*ctx.document;DocumentUndo::setUndoSensitive(&d,true);DocumentUndo::clearUndo(&d);DocumentUndo::clearRedo(&d);
        d.getReprRoot()->setAttribute("data-fault-seed","undo");DocumentUndo::done(&d,Util::Internal::ContextString("fault undo"),"");
        d.getReprRoot()->setAttribute("data-fault-seed","redo");DocumentUndo::done(&d,Util::Internal::ContextString("fault redo"),"");ASSERT_TRUE(DocumentUndo::undo(&d));d.ensureUpToDate();
        ctx.selection->set(cast<SPItem>(d.getObjectById("untouched")));
        auto requestFor=[&](std::string cmd,boost::json::object params) {Request req;req.id="p6-fault";req.command=cmd;req.params=std::move(params);req.document=document_stamp(&d).id;req.if_revision=document_stamp(&d).revision;return req;};
        auto analysis=execute(requestFor("bitmap.explode.analyze",{{"id","image1"},{"recipe",boost::json::object{{"refine",false}}}}));ASSERT_EQ(analysis.status,Status::Ok)<<analysis.message;
        ASSERT_GT(store.snapshot().charged_bytes,0u);
        auto request=requestFor("bitmap.explode.create-contour-only",boost::json::parse(R"params({"id": "image1", "recipe": {"refine": false}, "contour": {}})params").as_object());auto before=faultEngineState(ctx,store);
        ScopedCliFaultPlanForTesting scope({{{"bitmap.explode.create-contour-only.after-native-before-settlement",CliFaultKind::ServiceException}}, {}});
        auto result=execute(request);auto after=faultEngineState(ctx,store);
        ASSERT_EQ(scope.plan().faults[0].visits,1u);ASSERT_EQ(result.status,Status::Failed)<<result.message;
        ASSERT_EQ(result.reason,"internal-error");ASSERT_EQ(before,after);ASSERT_FALSE(DocumentUndo::interactionActive(&d));
        std::cout<<"P9-OBSERVATION "<<boost::json::serialize(boost::json::object{{"obligation","P9:bitmap.explode.create-contour-only:command-service:internal-error:unexpected service exception; rollback before return"},{"command","bitmap.explode.create-contour-only"},{"code",result.reason},{"branch","unexpected service exception; rollback before return"},
          {"mutation_state","rolled-back"},{"before",before},{"after",after},{"settled",true},
          {"actual",boost::json::object{{"fault_point","bitmap.explode.create-contour-only.after-native-before-settlement"},{"visits",scope.plan().faults[0].visits},{"interaction_active",false},{"result",typed_result(result,request.id,1)}}}})<<std::endl;
    });
}
TEST_F(P6Fault, EngineException7) {
    SessionOptions options;options.document_path=(std::filesystem::path(__FILE__).parent_path().parent_path()/"cli_tests/vacards-agent/fixtures/m3/explode.svg").string();options.grants.read_files={options.document_path};
    with_cli_engine_for_testing(options,[&](auto &ctx,auto &files,auto &store,auto const &execute) {
        auto &d=*ctx.document;DocumentUndo::setUndoSensitive(&d,true);DocumentUndo::clearUndo(&d);DocumentUndo::clearRedo(&d);
        auto raw=gdk_pixbuf_new(GDK_COLORSPACE_RGB,true,8,64,64);gdk_pixbuf_fill(raw,0x80808096);Pixbuf pixels(raw);auto href=sp_image_encode_png_data_uri(pixels);ASSERT_TRUE(href);d.getObjectById("image1")->getRepr()->setAttribute("xlink:href",*href);d.ensureUpToDate();
        d.getReprRoot()->setAttribute("data-fault-seed","undo");DocumentUndo::done(&d,Util::Internal::ContextString("fault undo"),"");
        d.getReprRoot()->setAttribute("data-fault-seed","redo");DocumentUndo::done(&d,Util::Internal::ContextString("fault redo"),"");ASSERT_TRUE(DocumentUndo::undo(&d));d.ensureUpToDate();
        ctx.selection->set(cast<SPItem>(d.getObjectById("untouched")));
        auto requestFor=[&](std::string cmd,boost::json::object params) {Request req;req.id="p6-fault";req.command=cmd;req.params=std::move(params);req.document=document_stamp(&d).id;req.if_revision=document_stamp(&d).revision;return req;};
        auto analysis=execute(requestFor("bitmap.explode.analyze",{{"id","image1"},{"recipe",boost::json::object{{"refine",false}}}}));ASSERT_EQ(analysis.status,Status::Ok)<<analysis.message;
        ASSERT_GT(store.snapshot().charged_bytes,0u);
        auto request=requestFor("bitmap.explode.apply-adjustment",boost::json::parse(R"params({"id": "image1", "recipe": {}})params").as_object());auto before=faultEngineState(ctx,store);
        ScopedCliFaultPlanForTesting scope({{{"bitmap.explode.apply-adjustment.after-native-before-settlement",CliFaultKind::ServiceException}}, {}});
        auto result=execute(request);auto after=faultEngineState(ctx,store);
        ASSERT_EQ(scope.plan().faults[0].visits,1u);ASSERT_EQ(result.status,Status::Failed)<<result.message;
        ASSERT_EQ(result.reason,"internal-error");ASSERT_EQ(before,after);ASSERT_FALSE(DocumentUndo::interactionActive(&d));
        std::cout<<"P9-OBSERVATION "<<boost::json::serialize(boost::json::object{{"obligation","P9:bitmap.explode.apply-adjustment:command-service:internal-error:unexpected service exception; rollback before return"},{"command","bitmap.explode.apply-adjustment"},{"code",result.reason},{"branch","unexpected service exception; rollback before return"},
          {"mutation_state","rolled-back"},{"before",before},{"after",after},{"settled",true},
          {"actual",boost::json::object{{"fault_point","bitmap.explode.apply-adjustment.after-native-before-settlement"},{"visits",scope.plan().faults[0].visits},{"interaction_active",false},{"result",typed_result(result,request.id,1)}}}})<<std::endl;
    });
}
}
