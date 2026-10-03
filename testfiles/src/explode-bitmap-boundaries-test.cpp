// SPDX-License-Identifier: GPL-2.0-or-later
#include <gtest/gtest.h>
#include <cstring>
#include <csignal>
#include <chrono>
#include <functional>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <glibmm/main.h>
#include "colors/color.h"
#include "display/cairo-utils.h"
#include "extension/internal/image-resolution.h"
#include "helper/png-write.h"
#include "ui/explode-bitmap-panel-preparation.h"
#include "bitmap-copy-outcome.h"
#include "bitmap-explode-chemistry.h"
#include "desktop.h"
#include "file.h"
#include "print.h"
#include "inkscape-window.h"
#include "seltrans.h"
#include "ui/clipboard.h"
#include "ui/dialog/print.h"
#include "ui/dialog/save-template-dialog.h"
#include "ui/dialog/artwork-library-controller.h"
#include "undo-stack-observer.h"
#include <filesystem>
#include "document.h"
#include "inkscape.h"
#include "inkscape-application.h"
#include "message-stack.h"
#include "object/sp-image.h"
#include "object/sp-root.h"
#include "preferences.h"
#include "selection.h"
#include "ui/explode-bitmap-publication.h"
#include "ui/explode-bitmap-overlay.h"
#include "ui/explode-bitmap-undo.h"
#include "util/bitmap-island-specks.h"
#include "xml/document.h"
#include "xml/repr.h"
using namespace Inkscape;
using namespace Inkscape::Bitmap;
void document_save_as(InkscapeWindow *);
void document_save_copy(InkscapeWindow *);
namespace {
using Print = UI::Dialog::Print;
struct Observer : UndoStackObserver {
    std::function<void()> callback;
    void notifyUndoCommitEvent(Event *) override { callback(); }
    void notifyUndoEvent(Event *) override {} void notifyRedoEvent(Event *) override {}
    void notifyUndoExpired(Event *) override {} void notifyClearUndoEvent() override {}
    void notifyClearRedoEvent() override {}
};
struct Room : MemoryProbe {
    bool read(RawMemory &m) const noexcept override { m={16*1024*MiB,8*1024*MiB,128*MiB}; return true; }
} room;
std::function<void()> conversionCallback;
void afterInsert() { if (conversionCallback) conversionCallback(); }
class Boundaries : public ::testing::Test {
protected:
    void SetUp() override {
        static auto app=[] { Gtk::Application::wrap_in_search_entry2(); g_setenv("INKSCAPE_APP_ID_TAG","eb5boundaries",TRUE); return new InkscapeApplication; }();
        ASSERT_TRUE(app->gtk_app()); app->gio_app()->register_application(); if (!Application::exists()) Application::create(false);
        for (auto signal:{SIGSEGV,SIGABRT,SIGFPE,SIGILL}) std::signal(signal,SIG_DFL);
    }
    void TearDown() override { Print::setRunForTesting(nullptr); conversionCallback={}; DocumentUndo::setAtomicSettlementFaultForTesting(nullptr); }
    void open(bool nested=false) {
        lease.reset(); desktop.reset(); doc.reset(); candidate={}; pieces={}; grid={}; input={};
        std::string body="<rect id='a' width='12' height='8' fill='#369'/><rect id='b' x='50' width='12' height='8' fill='#936'/>";
        if (nested) body="<g transform='translate(7,9) scale(2,3)'>"+body+"</g>";
        doc=SPDocument::createNewDocFromMem("<svg xmlns='http://www.w3.org/2000/svg' width='240' height='200'>"+body+"</svg>");
        ASSERT_TRUE(doc); doc->ensureUpToDate(); desktop=std::make_unique<SPDesktop>(doc->getNamedView());
        desktop->getSelection()->add(doc->getObjectById("a")); desktop->getSelection()->add(doc->getObjectById("b"));
        DocumentUndo::setUndoSensitive(doc.get(),true);
        DocumentUndo::done(doc.get(),Util::Internal::ContextString("Fixture"),""); DocumentUndo::clearUndo(doc.get());
        doc->ensureUpToDate(); doc->setModifiedSinceSave(false);
        PlatformEvidence e; e.platform=EvidencePlatform::Mac; e.observationNs=1;
        lease=std::make_unique<DependencyLease>(*desktop,e);
        auto c=prepareBitmapCopy(*desktop->getSelection(),{},PlacementPolicy::ContiguousReplacement,budget,{&room,nullptr,nullptr,afterInsert});
        ASSERT_TRUE(c.ok()) << c.outcome.diagnostic; candidate=std::move(c.candidate);
        auto captured=candidate.gridInput(budget); ASSERT_TRUE(captured.ok()) << captured.outcome.diagnostic;
        input=std::move(captured.value); Recipe recipe; recipe.bypassAlpha=true;
        auto g=prepareGrid(input,recipe,budget); ASSERT_TRUE(g.ok()) << g.outcome.diagnostic; grid=std::move(g.value);
        JobWork work(grid.width*grid.height); AlphaLut lut; for (unsigned i=0;i<256;++i) lut[i]=i;
        auto regions=label(grid.view(),lut,budget,work); ASSERT_TRUE(regions.ok());
        auto partition=enclose(regions.value,budget,work); ASSERT_TRUE(partition.ok());
        auto outlines=prepareOutlines(partition.value,grid,budget,work);
        ASSERT_TRUE(outlines.ok()) << outlines.outcome.diagnostic;
        ASSERT_EQ(outlines.value.storage->pieceCount,2u); ASSERT_GT(outlines.value.storage->count,0u);
        EXPECT_EQ(outlines.value.storage->pixelToDocument,grid.pixelToDocument);
        auto encoded=encode(grid,partition.value,budget); ASSERT_TRUE(encoded.ok()) << encoded.outcome.diagnostic;
        pieces=std::move(encoded.value); ASSERT_EQ(pieces.count(),2u);
        p={}; p.target=candidate.metadata().token; p.dependencies=capture(p.target); ASSERT_TRUE(p.dependencies);
        p.grid=&grid; p.pieces=&pieces; p.budget=&budget; p.probe=&room;
        p.activation=std::make_shared<DependencyRequest>();
        p.limits.publicationUnits=p.limits.rollbackUnits=p.limits.historyUnits=20000;
        before=xml();
    }
    void openDirect() {
        open(); auto token=DocumentUndo::beginAtomicInteraction(doc.get()); ASSERT_TRUE(token);
        ASSERT_TRUE(publishBitmapCopy(*desktop->getSelection(),candidate,&*token).ok());
        ASSERT_TRUE(token->commitAtomically(Util::Internal::ContextString("Fixture bitmap"),"",[] { return true; }));
        refreshDirect();
    }
    void refreshDirect() {
        doc->ensureUpToDate();
        DocumentUndo::done(doc.get(),Util::Internal::ContextString("Fixture view"),"");
        DocumentUndo::clearUndo(doc.get()); lease.reset();
        PlatformEvidence e; e.platform=EvidencePlatform::Mac; e.observationNs=1;
        lease=std::make_unique<DependencyLease>(*desktop,e);
        auto target=resolve(*desktop,Intent::Explode); ASSERT_TRUE(target.ok());
        p.target=target.value; p.dependencies=capture(p.target); ASSERT_TRUE(p.dependencies);
        p.session=sessionJobIdentity(logicalImageIdentity(*cast<SPImage>(desktop->getSelection()->single())),p.target);
        p.activation=std::make_shared<DependencyRequest>(); before=xml();
    }
    std::string xml() { return sp_repr_save_buf(doc->getReprDoc()).raw(); }
    Outcome run() {
        auto start=std::chrono::steady_clock::now();
        auto out=convertAndExplode(*desktop,{candidate,p},1);
        RecordProperty("pair_us",std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now()-start).count()));
        return out;
    }
    HistoryUsage history() { return preflightUndo(*doc,{false,0,1}).usage; }
    void original() {
        EXPECT_EQ(xml(),before); EXPECT_TRUE(doc->getObjectById("a")); EXPECT_TRUE(doc->getObjectById("b"));
        EXPECT_EQ(desktop->getSelection()->size(),2u); EXPECT_FALSE(doc->isModifiedSinceSave());
        EXPECT_EQ(history().undoCount,0u);
    }
    void drain() { auto context=Glib::MainContext::get_default(); for (unsigned i=0;i<100 && context->pending();++i) context->iteration(false); }
    std::unique_ptr<SPDocument> doc;
    std::unique_ptr<SPDesktop> desktop;
    std::unique_ptr<DependencyLease> lease;
    Budget budget{Budget::FixedLimitForTest{},1536*MiB};
    PreparedBitmapCopy candidate;
    CandidateGridInput input;
    FinalGrid grid;
    EncodedPieces pieces;
    Prepared p;
    std::string before;
};
TEST_F(Boundaries,CandidateGridIsReadOnlyOwnedAndMatchesPublishedGeometry) {
    open(true); original();
    EXPECT_EQ(grid.candidateIdentity,candidate.metadata().candidateIdentity);
    auto originalInput=input.raster.view(); auto candidatePixels=candidate.rgba();
    for (unsigned y=0;y<originalInput.height;++y)
        EXPECT_EQ(std::memcmp(originalInput.data+y*originalInput.stride,candidatePixels.data+y*candidatePixels.stride,4*originalInput.width),0);
    auto token=DocumentUndo::beginAtomicInteraction(doc.get()); ASSERT_TRUE(token);
    auto created=publishBitmapCopy(*desktop->getSelection(),candidate,&*token); ASSERT_TRUE(created.ok());
    auto image=cast<SPImage>(desktop->getSelection()->single()); ASSERT_TRUE(image);
    auto map=image->c2p*image->i2doc_affine();
    for (unsigned i=0;i<6;++i) EXPECT_NEAR(map[i],grid.pixelToDocument[i],1e-10);
    EXPECT_EQ(grid.width,candidate.metadata().width); EXPECT_EQ(grid.height,candidate.metadata().height);
    EXPECT_EQ(candidate.gridInput(budget).outcome.status,Status::unavailable);
    token->rollback(); EXPECT_EQ(xml(),before);
}
TEST_F(Boundaries,CandidateCaptureCancelAndWrongGridLeaveOriginals) {
    open(); auto canceled=std::make_shared<std::atomic_bool>(true);
    EXPECT_EQ(candidate.gridInput(budget,Stop{canceled}).outcome.status,Status::canceled);
    grid.candidateIdentity++; EXPECT_EQ(run().status,Status::unavailable); original();
}
TEST_F(Boundaries,PartAPairPublishesAndConsumedTicketCannotReplay) {
    open(); bool reached=false; conversionCallback=[&] { reached=true; };
    auto out=run(); ASSERT_EQ(out.status,Status::changed) << out.diagnostic;
    EXPECT_TRUE(reached); EXPECT_EQ(desktop->getSelection()->size(),pieces.count());
    EXPECT_EQ(history().undoCount,2u);
    auto after=xml(); auto selected=desktop->getSelection()->items_vector(); auto usage=history();
    EXPECT_NE(run().status,Status::changed);
    EXPECT_EQ(xml(),after); EXPECT_EQ(desktop->getSelection()->items_vector(),selected);
    EXPECT_EQ(history().undoCount,usage.undoCount); EXPECT_EQ(history().undoBytes,usage.undoBytes);
}
TEST_F(Boundaries,T19ConversionOutputCountAdmittedAndOneLessRefusesBeforeMutation) {
    for(unsigned which=0;which<3;++which) for(bool fits : {false,true}) {
        open(); auto value=pieces.count()-(fits ? 0 : 1);
        if (!which) p.limits.publicationUnits=value;
        else if (which==1) p.limits.rollbackUnits=value;
        else p.limits.historyUnits=value;
        bool reached=false; conversionCallback=[&] { reached=true; };
        auto out=run(); EXPECT_EQ(out.status,fits ? Status::changed : Status::unavailable) << out.diagnostic;
        EXPECT_EQ(reached,fits);
        if(!fits) original();
        else { EXPECT_EQ(desktop->getSelection()->size(),pieces.count()); EXPECT_EQ(history().undoCount,2u); }
    }
}
TEST_F(Boundaries,T14T30DirectUndoAndCommitCallbacksGuardRealEntries) {
    openDirect(); INKSCAPE.add_desktop(desktop.get());
    auto registered=std::shared_ptr<void>(desktop.get(),[](void *d) { INKSCAPE.remove_desktop(static_cast<SPDesktop *>(d)); });
    auto window=std::make_unique<InkscapeWindow>(desktop.get()); refreshDirect();
    auto workspace=UI::Dialog::ArtworkLibraryWorkspace::create(); workspace->new_collection("Boundary test");
    auto library=std::make_unique<UI::Dialog::ArtworkLibraryController>(workspace); library->setDesktop(desktop.get());
    Gtk::Window legacy; unsigned undoCalls=0,commitCalls=0,ran=0;
    auto boundary=[&] {
        // This idle exposes an accidental chooser/clipboard/print nested loop.
        bool pumped=false; auto idle=Glib::signal_idle().connect([&] { pumped=true; return false; });
        EXPECT_FALSE(DocumentUndo::interactionActive(doc.get()));
        EXPECT_TRUE(publicationBoundaryPending(doc.get()));
        auto snapshot=xml();
        ObjectSet borrowed(doc.get()); borrowed.setList(desktop->getSelection()->items_vector());
        auto clipboard=UI::ClipboardManager::get();
        clipboard->copy(&borrowed); borrowed.cut();
        EXPECT_FALSE(clipboard->pasteSize(&borrowed,false,true,true));
        EXPECT_THROW(UI::copy_selection_detached(borrowed),std::runtime_error);
        clipboard->copy(desktop->getSelection());
        EXPECT_FALSE(clipboard->paste(desktop.get(),true,false));
        EXPECT_FALSE(sp_file_save_document(legacy,doc.get(),false,desktop.get()));
        EXPECT_FALSE(sp_file_save_dialog(legacy,doc.get(),Extension::FILE_SAVE_METHOD_SAVE_AS));
        document_save_as(window.get()); document_save_copy(window.get());
        EXPECT_EQ(SP_ACTIVE_DOCUMENT,doc.get());
        EXPECT_FALSE(sp_file_save_template(legacy,"eb5-existing","","","",false));
        UI::Dialog::SaveTemplate::save_document_as_template(legacy);
        EXPECT_TRUE(library->actions()->get_action_enabled("add-selection"));
        library->actions()->activate_action("add-selection");
        sp_print_document(legacy,doc.get());
        Print print(doc.get(),doc->getRoot());
        EXPECT_EQ(print.run(Gtk::PrintOperation::Action::PRINT_DIALOG,legacy),Gtk::PrintOperation::Result::CANCEL);
        EXPECT_EQ(xml(),snapshot); EXPECT_FALSE(pumped);
        EXPECT_TRUE(deferPublicationBoundary(doc.get(),[&](SPDocument &) { ++ran; }));
        // Deliberately pump: queued commands must still not execute under the lease.
        drain(); EXPECT_EQ(ran,0u); EXPECT_TRUE(pumped); idle.disconnect();
    };
    Observer observer; observer.callback=[&] { ++undoCalls; boundary(); }; doc->addUndoObserver(observer);
    auto commit=doc->connectCommit([&] { ++commitCalls; boundary(); });
    auto out=publishExplode(*desktop,p,1);
    commit.disconnect(); doc->removeUndoObserver(observer);
    ASSERT_EQ(out.status,Status::changed) << out.diagnostic;
    EXPECT_EQ(undoCalls,1u); EXPECT_EQ(commitCalls,1u); EXPECT_EQ(history().undoCount,1u);
    // Pending native copy/paste are tied to the originating view.
    library.reset(); window.reset(); registered.reset(); lease.reset(); desktop.reset(); drain(); EXPECT_EQ(ran,2u);
}
TEST_F(Boundaries,T14T30EveryExplodeCallbackDefersSerialization) {
    for (auto stage:{PublishStage::Guard,PublishStage::Delete,PublishStage::Resources,PublishStage::Insert,
                    PublishStage::Binding,PublishStage::Native,PublishStage::Selection,PublishStage::Bake,
                    PublishStage::Settlement,PublishStage::Readiness,PublishStage::Rollback}) {
        openDirect(); using Self=decltype(this); struct Callback { Self self; PublishStage stage; unsigned queued=0,ran=0; std::string output; } cb{this,stage};
        p.hooks={[](PublishStage at,unsigned,void *data) {
            auto &c=*static_cast<Callback *>(data);
            if (c.stage==PublishStage::Rollback && at==PublishStage::Insert) throw std::bad_alloc();
            if (at!=c.stage) return;
            auto nested=c.self->p; nested.activation=std::make_shared<DependencyRequest>(); nested.hooks={};
            EXPECT_NE(publishExplode(*c.self->desktop,nested,2).status,Status::changed);
            EXPECT_TRUE(deferPublicationBoundary(c.self->doc.get(),[&c](SPDocument &) { ++c.ran; c.output=c.self->xml(); }));
            ++c.queued; EXPECT_EQ(c.ran,0u);
        },&cb};
        auto out=publishExplode(*desktop,p,1); EXPECT_EQ(out.status,stage==PublishStage::Rollback ? Status::failed : Status::changed) << out.diagnostic;
        EXPECT_GT(cb.queued,0u); EXPECT_EQ(cb.ran,0u); auto final=xml(); drain();
        EXPECT_EQ(cb.ran,cb.queued); EXPECT_EQ(cb.output,final);
    }
}
TEST_F(Boundaries,T30DirectPublicationReplaysNativeCopyThenPasteOnce) {
    openDirect(); unsigned callbacks=0;
    auto connection=doc->connectCommit([&] {
        ++callbacks; if (callbacks!=1) return;
        desktop->getSelection()->copy();
        EXPECT_FALSE(UI::ClipboardManager::get()->paste(desktop.get(),true,false));
    });
    ASSERT_EQ(publishExplode(*desktop,p,1).status,Status::changed);
    EXPECT_EQ(history().undoCount,1u); drain(); connection.disconnect();
    EXPECT_EQ(callbacks,2u); EXPECT_EQ(history().undoCount,2u);
    unsigned images=0; for (auto &child:doc->getRoot()->children) if (is<SPImage>(&child)) ++images;
    EXPECT_EQ(images,4u); EXPECT_EQ(desktop->getSelection()->size(),2u);
}
TEST_F(Boundaries,T14DestroyedViewDoesNotRunDeferredAction) {
    open(); bool ran=false; auto token=DocumentUndo::beginAtomicInteraction(doc.get()); ASSERT_TRUE(token);
    EXPECT_TRUE(deferPublicationBoundary(*desktop,[&](SPDesktop &) { ran=true; }));
    token->rollback(); lease.reset(); desktop.reset(); drain(); EXPECT_FALSE(ran);
}
TEST_F(Boundaries,T14DestroyedDocumentDiscardsContinuation) {
    openDirect(); bool ran=false;
    auto connection=doc->connectCommit([&] {
        EXPECT_TRUE(deferPublicationBoundary(doc.get(),[&](SPDocument &) { ran=true; }));
    });
    ASSERT_EQ(publishExplode(*desktop,p,1).status,Status::changed); connection.disconnect();
    lease.reset(); desktop.reset(); doc.reset(); drain(); EXPECT_FALSE(ran);
}
TEST_F(Boundaries,T14WindowDestroyedOrDocumentReplacedDropsSaveReplay) {
    for (bool replace:{false,true}) {
        openDirect(); INKSCAPE.add_desktop(desktop.get());
        auto registered=std::shared_ptr<void>(desktop.get(),[](void *d) { INKSCAPE.remove_desktop(static_cast<SPDesktop *>(d)); });
        auto window=std::make_unique<InkscapeWindow>(desktop.get()); refreshDirect();
        auto other=SPDocument::createNewDocFromMem("<svg xmlns='http://www.w3.org/2000/svg'/>");
        auto connection=doc->connectCommit([&] { EXPECT_FALSE(sp_file_save_dialog(*window,doc.get(),Extension::FILE_SAVE_METHOD_SAVE_AS)); });
        ASSERT_EQ(publishExplode(*desktop,p,1).status,Status::changed); connection.disconnect();
        // Keep a status update queued across native window teardown. The desktop
        // deliberately survives so deferred Save replay must also be dropped.
        desktop->messageStack()->push(NORMAL_MESSAGE, "Pending status before closing the window");
        if (replace) {
            desktop->setDocument(other.get());
            EXPECT_NE(desktop->getDesktopWidget(), nullptr);
        } else {
            window.reset();
            EXPECT_EQ(desktop->getDesktopWidget(), nullptr);
        }
        drain();
        if (replace) desktop->setDocument(doc.get());
        window.reset();
        EXPECT_EQ(desktop->getDesktopWidget(), nullptr);
        desktop->messageStack()->push(NORMAL_MESSAGE, "Status after closing the window");
        drain();
    }
}
TEST_F(Boundaries,T14TrailingPublicationScopeRefusesWithoutRequeue) {
    open(); unsigned ran=0;
    auto token=DocumentUndo::beginAtomicInteraction(doc.get()); ASSERT_TRUE(token);
    auto publication=DocumentUndo::holdPublication(doc.get());
    EXPECT_TRUE(deferPublicationBoundary(doc.get(),[&](SPDocument &) { ++ran; }));
    token->rollback();
    drain(); EXPECT_EQ(ran,0u); publication.reset(); drain(); EXPECT_EQ(ran,0u);
}
TEST_F(Boundaries,OrdinaryCopyCutPasteAndPrintIncludingSelectorDrag) {
    openDirect(); auto clipboard=UI::ClipboardManager::get();
    clipboard->copy(desktop->getSelection());
    auto count=desktop->getSelection()->size(); desktop->getSelection()->cut();
    EXPECT_TRUE(desktop->getSelection()->isEmpty());
    ASSERT_TRUE(clipboard->paste(desktop.get(),true,false));
    EXPECT_EQ(desktop->getSelection()->size(),count);
    DocumentUndo::done(doc.get(),Util::Internal::ContextString("Paste"),"");
    INKSCAPE.add_desktop(desktop.get());
    auto registered=std::shared_ptr<void>(desktop.get(),[](void *d) { INKSCAPE.remove_desktop(static_cast<SPDesktop *>(d)); });
    InkscapeWindow parent(desktop.get());
    static unsigned printed; printed=0;
    auto temp=g_dir_make_tmp("eb5-boundaries-XXXXXX",nullptr); ASSERT_TRUE(temp);
    auto path=std::filesystem::path(temp)/"print.pdf"; g_free(temp);
    auto pathString=path.string(); // narrow and outliving the call; path::c_str() is wchar_t on Windows
    g_object_set_data(G_OBJECT(parent.gobj()),"eb5-print-path",const_cast<char *>(pathString.c_str()));
    Print::setRunForTesting(+[](Glib::RefPtr<Gtk::PrintOperation> const &op,Gtk::Window &w) {
        ++printed;
        op->set_export_filename(static_cast<char *>(g_object_get_data(G_OBJECT(w.gobj()),"eb5-print-path")));
        auto result=op->run(Gtk::PrintOperation::Action::EXPORT,w);
        EXPECT_EQ(result,Gtk::PrintOperation::Result::APPLY); return result;
    });
    for (bool drag:{false,true}) {
        SelTrans selector(desktop.get());
        if (drag) { selector.grab({0,0},-1,-1,false,true); selector.moveTo({2,2},0); }
        EXPECT_EQ(DocumentUndo::interactionActive(doc.get()),drag);
        { auto printLease=DocumentUndo::holdInteractionOperation(doc.get());
          auto detachedFilePublication=DocumentUndo::holdPublication(doc.get());
          EXPECT_FALSE(publicationBoundaryPending(doc.get())); }
        std::filesystem::remove(path); sp_print_document(parent,doc.get());
        EXPECT_TRUE(std::filesystem::exists(path));
        if (drag) EXPECT_TRUE(selector.cancel());
        drain();
    }
    EXPECT_EQ(printed,2u); std::filesystem::remove(path); std::filesystem::remove(path.parent_path());
}
// Owner-only picture generator: real EB-REAL1 intake, preparation and publication.
// No panel widget or GUI automation; the normal suite never runs this case.
TEST_F(Boundaries, DISABLED_OwnerDemoExplodeArrangeExport) {
    auto directory = g_getenv("VACARDS_EXPLODE_DEMO_DIR");
    if (!directory || !*directory) GTEST_SKIP() << "VACARDS_EXPLODE_DEMO_DIR is required";
    auto images = g_getenv("VACARDS_EXPLODE_DEMO_IMAGES");
    ASSERT_TRUE(images && *images);
    std::filesystem::create_directories(directory);
    std::ofstream notes(std::filesystem::path(directory) / "notes.md");
    ASSERT_TRUE(notes);
    notes << "# Demo 3 — real Explode Bitmap engine\n\n"
          << "Refine on, T128/S40; embedded PNGs at default import size. "
          << "White background, 1600 px wide. Bboxes use document SVG px [x,y,width,height]. "
          << "Original order: top-to-bottom, then left-to-right. Grid rows have ceil(sqrt(count)) pieces; "
          << "each piece reserves its width plus 15%, and each row its tallest height plus 15%.\n\n"
          << "| Image | Pieces | Result |\n|---|---:|---|\n";
    auto require = [](bool ok, std::string const &reason) { if (!ok) throw std::runtime_error(reason); };
    auto quoted = [](std::string const &s) {
        std::ostringstream out; out << '"';
        for (unsigned char c : s) {
            if (c == '"' || c == '\\') out << '\\' << c;
            else if (c < 32) out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << unsigned(c);
            else out << c;
        }
        out << '"'; return out.str();
    };
    auto bbox = [](Geom::Rect const &r) {
        std::ostringstream s; s << std::setprecision(17) << '[' << r.left() << ',' << r.top()
            << ',' << r.width() << ',' << r.height() << ']'; return s.str();
    };
    recordBitmapMainThread();
    std::istringstream list(images); std::string path;
    while (std::getline(list, path, ';')) {
        if (path.empty()) continue;
        auto name = std::filesystem::path(path).stem().string();
        auto base = (std::filesystem::path(directory) / name).string();
        unsigned count = 0;
        std::ofstream json(base + ".json"); ASSERT_TRUE(json);
        SCOPED_TRACE(path);
        try {
            lease.reset(); desktop.reset(); doc.reset();
            gchar *data = nullptr; gsize size = 0;
            require(g_file_get_contents(path.c_str(), &data, &size, nullptr), "Cannot read input PNG");
            auto encoded = g_base64_encode(reinterpret_cast<guchar const *>(data), size);
            std::string href = std::string("data:image/png;base64,") + encoded;
            g_free(data); g_free(encoded);
            doc = SPDocument::createNewDocFromMem("<svg xmlns='http://www.w3.org/2000/svg' width='100' height='100'>"
                "<image id='original' width='100' height='100' preserveAspectRatio='none' href='" + href + "'/></svg>");
            require(bool(doc), "Cannot create imported document"); doc->ensureUpToDate();
            auto original = cast<SPImage>(doc->getObjectById("original"));
            require(original && original->pixbuf, "Cannot decode imported PNG");
            // Same resolution rounding and fallback as GdkpixbufInput::open.
            Extension::Internal::ImageResolution resolution(path.c_str());
            auto scale = [](double dpi) { return dpi <= .05 ? 960.0 : 960.0 / std::round(10.0 * dpi); };
            double width = original->pixbuf->width() * (resolution.ok() ? scale(resolution.x()) : 1);
            double height = original->pixbuf->height() * (resolution.ok() ? scale(resolution.y()) : 1);
            original->getRepr()->setAttributeSvgDouble("width", width);
            original->getRepr()->setAttributeSvgDouble("height", height);
            doc->getReprRoot()->setAttributeSvgDouble("width", width);
            doc->getReprRoot()->setAttributeSvgDouble("height", height);
            doc->ensureUpToDate();
            desktop = std::make_unique<SPDesktop>(doc->getNamedView());
            desktop->getSelection()->set(original);
            DocumentUndo::setUndoSensitive(doc.get(), true);
            DocumentUndo::done(doc.get(), Util::Internal::ContextString("Imported demo PNG"), "");
            DocumentUndo::clearUndo(doc.get());
            auto exportPng = [&](std::string const &suffix, Geom::Rect area) {
                doc->ensureUpToDate();
                // Small white margin also keeps bbox strokes inside the exported image.
                area.expandBy(std::max(area.width(), area.height()) * .025);
                auto h = std::max(1ul, static_cast<unsigned long>(std::lround(1600 * area.height() / area.width())));
                require(sp_export_png_file(doc.get(), (base + suffix).c_str(), area, 1600, h,
                    96 * 1600 / area.width(), 96 * 1600 / area.width(), Colors::Color(0xffffffff),
                    nullptr, nullptr, true) == EXPORT_OK, "PNG export failed: " + suffix);
            };
            auto originalBounds = original->documentVisualBounds();
            require(bool(originalBounds), "Missing original bbox");
            exportPng("-before.png", *originalBounds);
            auto evidence = diagnosticDependencyEvidence();
            lease = std::make_unique<DependencyLease>(*desktop, evidence);
            auto target = resolve(*desktop, Intent::Explode); require(target.ok(), target.outcome.diagnostic);
            Prepared prepared; prepared.target = target.value; prepared.dependencies = capture(target.value);
            prepared.session = sessionJobIdentity(logicalImageIdentity(*original), target.value);
            prepared.activation = std::make_shared<DependencyRequest>(); prepared.limits = measuredLimits(evidence);
            require(bool(prepared.dependencies), "Dependency observation unavailable");
            require(prepared.limits.workerQualified, "Bitmap worker limits are not qualified on this platform.");
            auto memory = sampleMemory(); require(memory.ok(), memory.outcome.diagnostic);
            auto ledger = std::make_shared<Budget>(1536 * MiB);
            auto checked = ledger->recheck(memory.value); require(checked.ok(), checked.diagnostic);
            auto header = inspectHref(href, {}, *ledger); require(header.ok(), header.outcome.diagnostic);
            if (header.value.width > MaxExplodeSourceAxis || header.value.height > MaxExplodeSourceAxis)
                throw std::runtime_error(explodeSourceSizeMessage(header.value.width, header.value.height));
            auto uri = inspectUri(href, {}); require(uri.ok(), uri.outcome.diagnostic);
            auto admission = admit(PanelPreparation::resources(header.value.width, header.value.height,
                uri.value.decodedBytes, ledger->reserved()), memory.value);
            require(admission.ok(), admission.outcome.diagnostic);
            JobInput job; job.storage.budget = ledger;
            std::uint64_t envelope = sizeof(PanelPreparation::Input) + 64 + uri.value.payloadLength
                + target.value.contexts.size() * sizeof(TargetContext);
            for (auto const &context : target.value.contexts) envelope += context.id.size() + 1;
            envelope += (target.value.selection.size() + target.value.roots.size()) * sizeof(std::uintptr_t);
            checked = ledger->acquire(Stage::input, envelope, job.storage.reservation);
            require(checked.ok(), checked.diagnostic);
            auto input = std::make_shared<PanelPreparation::Input>(); input->target = target.value;
            input->recipe = query(logicalImageIdentity(*original));
            require(input->recipe.threshold == 128 && input->recipe.softness == 40 && !input->recipe.bypassAlpha,
                "Unexpected default recipe");
            input->decodedBytes = uri.value.decodedBytes;
            job.storage.bytes.assign(href.data() + uri.value.payloadOffset,
                href.data() + uri.value.payloadOffset + uri.value.payloadLength);
            job.storage.payload = input; job.pixels = std::uint64_t(header.value.width) * header.value.height;
            job.work = PanelPreparation::calculate;
            bool delivered = false; JobResult result;
            BitmapJobs jobs([&](Ticket, JobResult r) { result = std::move(r); delivered = true; }, {}, JobClock::now, false);
            checked = prepared.activation->check(prepared.dependencies, DependencyCheck::Dispatch);
            require(checked.ok(), checked.diagnostic);
            auto ticket = jobs.request(std::move(job));
            auto deadline = JobClock::now() + std::chrono::seconds(120);
            while (!delivered && JobClock::now() < deadline) {
                jobs.poll(); drain(); std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            require(delivered, "Preparation timed out after 120 seconds");
            require(result.ok(), result.outcome.diagnostic);
            checked = prepared.activation->check(prepared.dependencies, DependencyCheck::Delivery);
            require(checked.ok(), checked.diagnostic); require(valid(prepared.session), "Stale recipe session");
            auto const &out = static_cast<PanelPreparation::Output const &>(*result.value.payload);
            count = out.count; require(out.explodeOutcome.ok(), out.explodeOutcome.diagnostic);
            require(!out.opaque, "Source is fully opaque"); require(out.visible != 0, "Source has no visible pixels");
            require(count && count <= MaxExplodePieces, "Piece count outside panel publication range (1–150)");
            out.omitOutlines(); prepared.grid = &out.grid; prepared.pieces = &out.pieces; prepared.budget = ledger.get();
            auto publication = publishExplode(*desktop, prepared, ticket);
            require(publication.status == Status::changed, publication.diagnostic);
            doc->ensureUpToDate();
            auto selected = desktop->getSelection()->items_vector(); require(selected.size() == count, "Published count mismatch");
            struct Piece { SPItem *item; Geom::Rect before, after; };
            std::vector<Piece> ordered;
            for (auto item : selected) {
                auto bounds = item->documentVisualBounds(); require(bool(bounds), "Missing piece bbox");
                ordered.push_back({item, *bounds, *bounds});
            }
            std::stable_sort(ordered.begin(), ordered.end(), [](auto const &a, auto const &b) {
                if (a.before.top() != b.before.top()) return a.before.top() < b.before.top();
                return a.before.left() < b.before.left();
            });
            // Annotation objects exist only for the in-place export, never in the moved SVG.
            auto annotations = doc->getReprDoc()->createElement("svg:g");
            doc->getReprRoot()->appendChild(annotations);
            char const *colours[] = {"#e63946", "#1565c0", "#00865a", "#9c27b0", "#ed7900"};
            for (unsigned i = 0; i < count; ++i) {
                auto rect = doc->getReprDoc()->createElement("svg:rect"); auto const &b = ordered[i].before;
                rect->setAttributeSvgDouble("x", b.left()); rect->setAttributeSvgDouble("y", b.top());
                rect->setAttributeSvgDouble("width", b.width()); rect->setAttributeSvgDouble("height", b.height());
                rect->setAttribute("fill", "none"); rect->setAttribute("stroke", colours[i % 5]);
                rect->setAttributeSvgDouble("stroke-width", originalBounds->width() / 1200);
                annotations->appendChild(rect); GC::release(rect);
            }
            exportPng("-exploded.png", *originalBounds);
            annotations->parent()->removeChild(annotations); GC::release(annotations);
            unsigned columns = static_cast<unsigned>(std::ceil(std::sqrt(count)));
            double x = 0, y = 0, rowHeight = 0;
            for (unsigned i = 0; i < count; ++i) {
                auto &piece = ordered[i];
                if (i && i % columns == 0) { x = 0; y += rowHeight * 1.15; rowHeight = 0; }
                // All imported pieces have the root as parent; root coordinates are SVG px.
                piece.item->doWriteTransform(piece.item->transform * Geom::Translate(x - piece.before.left(), y - piece.before.top()));
                x += piece.before.width() * 1.15; rowHeight = std::max(rowHeight, piece.before.height());
            }
            doc->ensureUpToDate(); Geom::OptRect movedBounds;
            for (auto &piece : ordered) {
                auto b = piece.item->documentVisualBounds(); require(bool(b), "Missing moved bbox");
                piece.after = *b; if (movedBounds) movedBounds->unionWith(*b); else movedBounds = b;
            }
            for (unsigned i = 0; i < count; ++i) for (unsigned j = i + 1; j < count; ++j)
                require(!ordered[i].after.intersects(ordered[j].after), "Moved pieces overlap");
            exportPng("-moved.png", *movedBounds);
            // Keep coordinates intact while fitting the saved SVG page with a white margin.
            auto page = *movedBounds; page.expandBy(std::max(page.width(), page.height()) * .025);
            doc->getReprRoot()->setAttributeSvgDouble("width", page.width());
            doc->getReprRoot()->setAttributeSvgDouble("height", page.height());
            std::ostringstream viewbox; viewbox << std::setprecision(17) << page.left() << ' ' << page.top() << ' ' << page.width() << ' ' << page.height();
            doc->getReprRoot()->setAttribute("viewBox", viewbox.str());
            require(sp_repr_save_file(doc->getReprDoc(), (base + "-moved.svg").c_str()), "SVG save failed");
            json << "{\"status\":\"published\",\"count\":" << count << ",\"threshold\":128,\"softness\":40,\"refine\":true,\"pieces\":[";
            for (unsigned i = 0; i < count; ++i) {
                auto const &piece = ordered[i]; if (i) json << ',';
                json << "{\"id\":" << quoted(piece.item->getId()) << ",\"before\":" << bbox(piece.before)
                     << ",\"after\":" << bbox(piece.after) << '}';
            }
            json << "]}\n";
            notes << "| " << name << " | " << count << " | Published; all three PNGs and moved SVG exported |\n";
            std::cout << "DEMO3 " << name << " count=" << count << " exported" << std::endl;
        } catch (std::exception const &error) {
            json << "{\"status\":\"refused\",\"count\":" << count << ",\"reason\":" << quoted(error.what()) << "}\n";
            notes << "| " << name << " | " << count << " | Refused: " << error.what() << " |\n";
            std::cout << "DEMO3 " << name << " REFUSED: " << error.what() << std::endl;
        }
        notes.flush(); json.flush();
        require(waitForBitmapReaper(std::chrono::seconds(3)), "Worker did not retire");
    }
}
} // namespace
