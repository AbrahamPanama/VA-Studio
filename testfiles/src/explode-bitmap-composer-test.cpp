// SPDX-License-Identifier: GPL-2.0-or-later
#include <gtest/gtest.h>
#include <string>
#include <chrono>
#include <future>
#ifndef _WIN32
#include <sys/resource.h>
#endif
#include <vector>
#include <glibmm/main.h>
#include <cairomm/surface.h>
#include "async/bitmap-job-reaper.h"
#include "bitmap-adjustment-chemistry.h"
#include "desktop.h"
#include "display/cairo-utils.h"
#include "display/control/canvas-item-drawing.h"
#include "display/drawing-context.h"
#include "display/drawing-surface.h"
#include "display/drawing.h"
#include "display/drawing-item.h"
#include "display/nr-filter.h"
#include "document.h"
#include "document-undo.h"
#include "inkscape.h"
#include "inkscape-application.h"
#include "object/sp-image.h"
#include "object/object-set.h"
#include "object/sp-root.h"
#include "selection.h"
#include "ui/bitmap-adjustments-controller.h"
#include "ui/bitmap-preview-composer.h"
#include "ui/clipboard.h"
#include "xml/repr.h"
using namespace Inkscape;
using namespace Inkscape::Bitmap;
namespace {
constexpr auto valid_data =
    "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAIAAAACAgMAAAAP2OW3AAAAIGNIUk0AAHomAACAhAAA+gAAAIDoAAB1MAAA6mAAADqYAAAXcJy6UTwAAAAMUExURQAAAIBAIEBAQMDAwLgSRQ4AAAACdFJOUwCAmytOGAAAAAd0SU1FB+oIHREEEmM5K8wAAAAldEVYdGRhdGU6Y3JlYXRlADIwMjYtMDgtMjlUMTc6MDQ6MTgrMDA6MDBoWbZDAAAAJXRFWHRkYXRlOm1vZGlmeQAyMDI2LTA4LTI5VDE3OjA0OjE4KzAwOjAwGQQO/wAAACh0RVh0ZGF0ZTp0aW1lc3RhbXAAMjAyNi0wOC0yOVQxNzowNDoxOCswMDowME4RLyAAAAAMSURBVAjXY1BguAAAATQA8bBD2hMAAAAASUVORK5CYII=";
void drain() { while (Glib::MainContext::get_default()->iteration(false)) {} }
std::vector<unsigned char> render(Drawing &drawing)
{
    drawing.update();
    auto surface = Cairo::ImageSurface::create(Cairo::Surface::Format::ARGB32, 8, 8);
    DrawingSurface target(surface->cobj(), Geom::IntPoint(0, 0));
    DrawingContext context(target);
    drawing.render(context, Geom::IntRect::from_xywh(0, 0, 8, 8));
    surface->flush();
    return {surface->get_data(), surface->get_data() + surface->get_stride() * 8};
}
class View {
public:
    View(SPDocument *doc) : root(doc->getRoot()), key(SPItem::display_key_new(1)) { show(); }
    ~View() { hide(); }
    void show() { drawing.setRoot(root->invoke_show(drawing, key, SP_ITEM_SHOW_DISPLAY)); active = true; drawing.update(); }
    void hide() { if (active) { root->invoke_hide(key); active = false; } }
    Drawing drawing;
    SPRoot *root;
    unsigned key;
    bool active = false;
};
class ComposerTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        static auto *app = [] {
            g_setenv("INKSCAPE_APP_ID_TAG", "bitmapcomposertest", TRUE);
            return new InkscapeApplication();
        }();
        ASSERT_TRUE(app->gtk_app()); // a skipped fixture cannot qualify this package
        if (!Application::exists()) Application::create(false);
        recordBitmapMainThread();
        // The small PNG comes from the existing tone alpha regression fixture.
        auto svg = Glib::ustring::compose(R"(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8"><image id="image" width="8" height="8" style="image-rendering:pixelated" href="%1"/></svg>)", valid_data);
        doc = SPDocument::createNewDocFromMem(svg.raw());
        ASSERT_TRUE(doc);
        doc->ensureUpToDate();
        image = cast<SPImage>(doc->getObjectById("image"));
        ASSERT_TRUE(image);
        ASSERT_TRUE(image->pixbuf);
        ASSERT_EQ(image->pixbuf->width(), 2);
        doc->setModifiedSinceSave(false);
    }
    Contribution tone(double brightness = 35)
    {
        Contribution c;
        c.tone.emplace();
        c.tone->brightness = brightness;
        c.tone->contrast = 20;
        return c;
    }
    Contribution alpha()
    {
        Contribution c;
        c.layer = PreviewLayer::Alpha;
        c.source = image->pixbuf;
        auto pixels = std::make_shared<Pixbuf>(*image->pixbuf);
        pixels->ensurePixelFormat(Pixbuf::PF_GDK);
        for (int y = 0; y < pixels->height(); ++y) for (int x = 0; x < pixels->width(); ++x) {
            auto p = pixels->pixels() + y * pixels->rowstride() + 4 * x;
            if (p[3] < 200) p[3] = 0; // simulated explode threshold, RGB unchanged
        }
        pixels->markDirty();
        auto budget = std::make_shared<Budget>(Budget::FixedLimitForTest{}, 1024);
        auto backing = reserveAlphaDisplay(pixels->width(), pixels->height(), budget);
        EXPECT_TRUE(backing.ok());
        EXPECT_TRUE(prepareAlphaDisplay({pixels->pixels(), std::uint64_t(pixels->rowstride())*pixels->height(),
            std::uint64_t(pixels->rowstride()), unsigned(pixels->width()), unsigned(pixels->height())}, *backing.value).ok());
        c.backing = backing.value; c.pixels = wrapAlphaDisplay(c.backing);
        return c;
    }
    void install(ClientLease &lease, Generation g = 1) { ASSERT_EQ(update(lease, g).status, Status::changed); }
    std::unique_ptr<SPDocument> doc;
    SPImage *image = nullptr;
};
} // namespace
TEST_F(ComposerTest, T25BothOrdersCancelAndResetAreIndependent)
{
    for (bool alpha_first : {false, true}) for (bool alpha_leaves : {false, true}) {
        View view(doc.get()), canonical(doc.get());
        auto xml = sp_repr_save_buf(doc->getReprDoc());
        auto baseline = render(view.drawing);
        auto t = tone(); auto a = alpha();
        ClientLease tone_client, alpha_client;
        if (alpha_first) {
            alpha_client = contribute({image, view.key}, a); install(alpha_client);
            tone_client = contribute({image, view.key}, t); install(tone_client);
        } else {
            tone_client = contribute({image, view.key}, t); install(tone_client);
            alpha_client = contribute({image, view.key}, a); install(alpha_client);
        }
        auto both = render(view.drawing);
        EXPECT_NE(both, baseline);
        EXPECT_EQ(render(canonical.drawing), baseline);
        // Independent oracle: canonical tone filter + thresholded source bytes.
        ASSERT_TRUE(BitmapAdjustments::apply_tone(image, *t.tone));
        doc->ensureUpToDate();
        auto href = sp_image_encode_png_data_uri(*a.pixels);
        ASSERT_TRUE(href);
        auto original = std::string(image->getRepr()->attribute("href"));
        image->getRepr()->setAttribute("href", href->c_str());
        doc->ensureUpToDate();
        EXPECT_EQ(both, render(canonical.drawing));
        image->getRepr()->setAttribute("href", original.c_str());
        ASSERT_TRUE(BitmapAdjustments::reset_tone(image));
        doc->ensureUpToDate();
        // Refresh invalidates the old source generation, so publish fresh alpha.
        a = alpha();
        ASSERT_EQ(alpha_client.prepare(a, 2).status, Status::unchanged); install(alpha_client, 2);
        if (alpha_leaves) alpha_client.reset(); else tone_client.reset();
        View oracle(doc.get());
        auto expected = contribute({image, oracle.key}, alpha_leaves ? t : a); install(expected);
        EXPECT_EQ(render(view.drawing), render(oracle.drawing));
        tone_client.reset(); alpha_client.reset();
        EXPECT_EQ(render(view.drawing), render(canonical.drawing));
        EXPECT_EQ(std::string(image->getRepr()->attribute("href")), original);
        // All preview-only operations above must be nonpersistent.
        EXPECT_FALSE(xml.empty());
    }
}
TEST_F(ComposerTest, T25ToneMigrationBeforeWarningCommitUndoPasteAndClose)
{
    auto copy = image->getRepr()->duplicate(doc->getReprDoc());
    copy->setAttribute("id", "paste-source"); copy->setAttribute("x", "20");
    copy->setAttribute("opacity", "0.65");
    doc->getRoot()->getRepr()->appendChild(copy); Inkscape::GC::release(copy);
    doc->ensureUpToDate();
    auto source = cast<SPImage>(doc->getObjectById("paste-source")); ASSERT_TRUE(source);
    ASSERT_TRUE(BitmapAdjustments::apply_tone(source, *tone(10).tone)); doc->ensureUpToDate();
    DocumentUndo::done(doc.get(), Util::Internal::ContextString{"Prepare paste fixture"}, "");
    DocumentUndo::clearUndo(doc.get()); DocumentUndo::clearRedo(doc.get());
    doc->setModifiedSinceSave(false);
    for (bool alpha_first : {false, true}) {
        auto desktop = std::make_unique<SPDesktop>(doc->getNamedView());
        desktop->getSelection()->set(image);
        auto &controller = desktop->bitmapAdjustmentsController();
        auto drawing = desktop->getCanvasDrawing()->get_drawing();
        auto baseline = render(*drawing);
        auto xml = sp_repr_save_buf(doc->getReprDoc());
        auto a = alpha(); ClientLease ac;
        if (alpha_first) { ac = contribute({image, desktop->dkey}, a); install(ac); }
        ASSERT_TRUE(controller.preview(*tone().tone)); drain();
        ASSERT_TRUE(controller.previewActive());
        if (!alpha_first) { ac = contribute({image, desktop->dkey}, a); install(ac); }
        auto combined = render(*drawing);
        ASSERT_TRUE(controller.setPreviewMode(UI::BitmapPreviewMode::Before)); drain();
        auto before = render(*drawing);
        EXPECT_NE(before, combined);
        ASSERT_TRUE(controller.setClippingWarning(true)); drain();
        ASSERT_TRUE(controller.setClippingWarning(false)); drain();
        EXPECT_EQ(render(*drawing), before);
        EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()), xml);
        EXPECT_FALSE(doc->isModifiedSinceSave());
        controller.cancelPreview();
        EXPECT_EQ(render(*drawing), before); // alpha survives tone panel cancel
        ASSERT_TRUE(controller.preview(*tone().tone)); drain();
        ASSERT_TRUE(controller.commit(*tone().tone)); drain();
        EXPECT_EQ(render(*drawing), combined); // tone commit is not applied twice
        DocumentUndo::undo(doc.get()); doc->ensureUpToDate(); drain();
        EXPECT_EQ(render(*drawing), before);
        DocumentUndo::redo(doc.get()); doc->ensureUpToDate(); drain();
        EXPECT_EQ(render(*drawing), combined);
        // Native clipboard copy uses a detached ObjectSet so both panels stay live.
        ObjectSet copied(doc.get()); copied.set(source);
        UI::ClipboardManager::get()->copy(&copied); drain();
        ASSERT_TRUE(controller.preview(*tone(-40).tone)); drain();
        ASSERT_TRUE(UI::ClipboardManager::get()->pasteStyle(desktop->getSelection()));
        doc->ensureUpToDate(); drain();
        EXPECT_FALSE(controller.previewActive());
        auto pasted = BitmapAdjustments::query_tone(image); ASSERT_TRUE(pasted);
        EXPECT_DOUBLE_EQ(pasted->brightness, 35); // computed Paste Style preserves managed tone
        EXPECT_DOUBLE_EQ(pasted->contrast, 20);
        View expected_view(doc.get());
        auto expected_alpha = contribute({image, expected_view.key}, a); install(expected_alpha);
        EXPECT_EQ(render(*drawing), render(expected_view.drawing));
        EXPECT_NE(render(*drawing), baseline);
        desktop.reset(); // leases must safely outlive the destroyed panel/display
        ac.reset();
        ASSERT_TRUE(BitmapAdjustments::reset_tone(image));
        image->getRepr()->setAttribute("style", "image-rendering:pixelated");
        doc->ensureUpToDate();
        DocumentUndo::done(doc.get(), Util::Internal::ContextString{"Reset composer fixture"}, "");
        DocumentUndo::clearUndo(doc.get()); DocumentUndo::clearRedo(doc.get());
        doc->setModifiedSinceSave(false);
    }
}
TEST_F(ComposerTest, T29LatestGenerationAndPreparedRevision)
{
    View view(doc.get());
    auto t = contribute({image, view.key}, tone()); install(t);
    for (Generation g = 2; g <= 10001; ++g) EXPECT_EQ(t.prepare(tone(g % 80), g).status, Status::unchanged);
    EXPECT_EQ(update(t, 2).status, Status::canceled);
    // An out-of-order completion must not discard the replaceable latest slot.
    EXPECT_EQ(update(t, 10001).status, Status::changed);
    EXPECT_EQ(update(t, 10000).status, Status::canceled);
    auto a = contribute({image, view.key}, alpha()); install(a);
    EXPECT_EQ(t.prepare(tone(50), 10002).status, Status::unchanged);
    a.reset(); // composition changed after preparation
    EXPECT_EQ(update(t, 10002).status, Status::canceled);
    EXPECT_EQ(t.prepare(tone(50), 10003).status, Status::unchanged); install(t, 10003);
}
TEST_F(ComposerTest, T29LabelInvalidationNeverResurrectsPreparedGeneration)
{
    for (bool published : {false, true}) {
        SCOPED_TRACE(published ? "published" : "unpublished");
        View view(doc.get()), oracle(doc.get());
        auto client = contribute({image, view.key}, tone(10));
        Generation generation = 1;
        if (published) { install(client); ++generation; }
        auto baseline = render(view.drawing);
        ASSERT_EQ(client.prepare(tone(60), generation).status, Status::unchanged);
        image->setLabel(published ? "published invalidation" : "first publication invalidation");
        auto xml = sp_repr_save_buf(doc->getReprDoc());
        // Exact review sequence: tone 10, prepare tone 60 at generation 1,
        // setLabel, update. The published variant retires generation 2.
        EXPECT_EQ(update(client, generation).status, Status::canceled);
        EXPECT_EQ(update(client, generation).status, Status::canceled);
        EXPECT_EQ(render(view.drawing), baseline);
        EXPECT_EQ(client.prepare(tone(60), generation).status, Status::canceled);
        EXPECT_EQ(update(client, generation + 1).status, Status::canceled);
        EXPECT_EQ(render(view.drawing), baseline);
        ASSERT_EQ(client.prepare(tone(60), generation + 1).status, Status::unchanged);
        install(client, generation + 1);
        auto expected = contribute({image, oracle.key}, tone(60)); install(expected);
        EXPECT_EQ(render(view.drawing), render(oracle.drawing));
        EXPECT_NE(render(view.drawing), baseline);
        EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()), xml);
    }
}
TEST_F(ComposerTest, T29AbsentViewRefreshAndSourceIncarnation)
{
    View view(doc.get());
    auto a = alpha();
    auto lease = contribute({image, view.key}, a); install(lease);
    view.hide();
    EXPECT_EQ(lease.prepare(a, 2).status, Status::unavailable);
    view.show();
    EXPECT_EQ(lease.prepare(a, 3).status, Status::unchanged); install(lease, 3);
    auto old_pixels = std::weak_ptr(a.pixels);
    a.pixels.reset();
    auto replacement_uri = sp_image_encode_png_data_uri(*image->pixbuf); ASSERT_TRUE(replacement_uri);
    image->getRepr()->setAttribute("href", replacement_uri->c_str());
    doc->ensureUpToDate();
    EXPECT_TRUE(old_pixels.expired());
    a.pixels = image->pixbuf;
    EXPECT_EQ(lease.prepare(a, 4).status, Status::canceled);
    View oracle(doc.get());
    EXPECT_EQ(render(view.drawing), render(oracle.drawing));
}
TEST_F(ComposerTest, T02SupersededBuffersAndRepeatedPanelLifetimes)
{
    View view(doc.get());
    auto t = contribute({image, view.key}, tone()); install(t);
    for (int cycle = 0; cycle < 100; ++cycle) {
        auto a = alpha(); auto old = std::weak_ptr(a.pixels);
        auto lease = contribute({image, view.key}, a); install(lease);
        a.pixels.reset();
        auto next = alpha();
        EXPECT_EQ(lease.prepare(next, 2).status, Status::unchanged); install(lease, 2);
        EXPECT_TRUE(old.expired());
        auto final = std::weak_ptr(next.pixels); next.pixels.reset();
        lease.reset();
        EXPECT_TRUE(final.expired());
    }
    auto baseline = render(view.drawing);
    auto duplicate = contribute({image, view.key}, tone(-50));
    EXPECT_FALSE(duplicate);
    EXPECT_EQ(render(view.drawing), baseline);
}
TEST_F(ComposerTest, T02ReservedBackingSurvivesUntilLastReference)
{
    View view(doc.get());
    auto tc = contribute({image, view.key}, tone()); install(tc);
    auto bytes = std::uint64_t(image->pixbuf->rowstride()) * image->pixbuf->height();
    auto budget = std::make_shared<Budget>(Budget::FixedLimitForTest{}, 3 * bytes);
    auto make = [&] {
        auto backing = reserveAlphaDisplay(image->pixbuf->width(), image->pixbuf->height(), budget);
        EXPECT_TRUE(backing.ok());
        Pixbuf source(*image->pixbuf); source.ensurePixelFormat(Pixbuf::PF_GDK);
        EXPECT_TRUE(prepareAlphaDisplay({source.pixels(), bytes, unsigned(source.rowstride()),
            unsigned(source.width()), unsigned(source.height())}, *backing.value).ok());
        Contribution c; c.layer = PreviewLayer::Alpha; c.source = image->pixbuf;
        c.backing = backing.value; c.pixels = wrapAlphaDisplay(c.backing);
        return c;
    };
    auto ac = contribute({image, view.key}, make()); install(ac);
    EXPECT_EQ(budget->reserved(), bytes);
    for (Generation g = 2; g <= 10001; ++g) {
        auto next = make();
        EXPECT_LE(budget->reserved(), 3 * bytes); // current + prepared + incoming
        ASSERT_EQ(ac.prepare(std::move(next), g).status, Status::unchanged);
        EXPECT_EQ(budget->reserved(), 2 * bytes);
    }
    install(ac, 10001);
    EXPECT_EQ(budget->reserved(), bytes);
    ac.reset();
    EXPECT_EQ(budget->reserved(), 0);
    EXPECT_TRUE(tc); // departing alpha drops neither tone nor its owner
}
TEST_F(ComposerTest, T29SameIdReplacementAndLeaseMoves)
{
    View view(doc.get());
    auto old = contribute({image, view.key}, tone()); install(old);
    auto repr = image->getRepr();
    auto copy = repr->duplicate(doc->getReprDoc());
    image->deleteObject(); doc->ensureUpToDate();
    doc->getRoot()->getRepr()->appendChild(copy); Inkscape::GC::release(copy);
    doc->ensureUpToDate();
    image = cast<SPImage>(doc->getObjectById("image")); ASSERT_TRUE(image);
    auto current = contribute({image, view.key}, tone(-50)); install(current);
    auto baseline = render(view.drawing);
    EXPECT_EQ(update(old, 2).status, Status::unavailable);
    old.reset();
    EXPECT_EQ(render(view.drawing), baseline);
    ClientLease moved = std::move(current);
    EXPECT_FALSE(current); EXPECT_TRUE(moved);
    moved = std::move(moved); EXPECT_TRUE(moved);
    moved.reset();
    View oracle(doc.get()); EXPECT_EQ(render(view.drawing), render(oracle.drawing));
}
TEST_F(ComposerTest, T25PreviewOnlyXmlAndAlphaResetKeepTone)
{
    View view(doc.get());
    auto xml = sp_repr_save_buf(doc->getReprDoc());
    auto t = contribute({image, view.key}, tone()); install(t);
    auto tone_only = render(view.drawing);
    auto a = contribute({image, view.key}, alpha()); install(a);
    EXPECT_NE(render(view.drawing), tone_only);
    Contribution reset; reset.layer = PreviewLayer::Alpha;
    EXPECT_EQ(a.prepare(reset, 2).status, Status::unchanged); install(a, 2);
    EXPECT_EQ(render(view.drawing), tone_only);
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()), xml);
    EXPECT_FALSE(doc->isModifiedSinceSave());
    a.reset(); EXPECT_EQ(render(view.drawing), tone_only);
    t.reset();
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()), xml);
    EXPECT_FALSE(doc->isModifiedSinceSave());
}
TEST_F(ComposerTest, T29DocumentCloseDropsPreparedOutput)
{
    ClientLease tone_client, alpha_client;
    std::weak_ptr<Pixbuf const> pixels;
    {
        View view(doc.get());
        tone_client = contribute({image, view.key}, tone()); install(tone_client);
        auto a = alpha(); pixels = a.pixels;
        alpha_client = contribute({image, view.key}, a);
        ASSERT_EQ(alpha_client.prepare(a, 1).status, Status::unchanged);
    }
    doc.reset(); // weak document/item identities must not resurrect objects
    EXPECT_EQ(update(alpha_client, 1).status, Status::unavailable);
    tone_client.reset(); alpha_client.reset();
    EXPECT_TRUE(pixels.expired());
}
TEST_F(ComposerTest, T25CurrentCanonicalAfterLastDeparture)
{
    View view(doc.get()), oracle(doc.get());
    auto t = contribute({image, view.key}, tone(-30)); install(t);
    auto a = contribute({image, view.key}, alpha()); install(a);
    ASSERT_TRUE(BitmapAdjustments::apply_tone(image, *tone(50).tone));
    doc->ensureUpToDate();
    auto committed = render(oracle.drawing);
    EXPECT_NE(render(view.drawing), committed);
    a.reset(); t.reset();
    EXPECT_EQ(render(view.drawing), committed);
}
TEST_F(ComposerTest, T25OwnClipOpacityAndToneAreAppliedOnceInBothOrders)
{
    auto defs = doc->getReprDoc()->createElement("svg:defs");
    auto clip = doc->getReprDoc()->createElement("svg:clipPath");
    clip->setAttribute("id", "clip");
    auto rect = doc->getReprDoc()->createElement("svg:rect");
    rect->setAttribute("width", "4"); rect->setAttribute("height", "8");
    clip->appendChild(rect); defs->appendChild(clip); doc->getRoot()->getRepr()->appendChild(defs);
    Inkscape::GC::release(rect); Inkscape::GC::release(clip); Inkscape::GC::release(defs);
    image->getRepr()->setAttribute("clip-path", "url(#clip)");
    image->getRepr()->setAttribute("opacity", "0.65");
    ASSERT_TRUE(BitmapAdjustments::apply_tone(image, *tone(10).tone));
    doc->ensureUpToDate();
    auto xml = sp_repr_save_buf(doc->getReprDoc());
    for (bool alpha_first : {false, true}) {
        View view(doc.get());
        auto t = tone(50); auto a = alpha();
        auto reference = SPDocument::createNewDocFromMem(xml.raw()); ASSERT_TRUE(reference);
        auto reference_image = cast<SPImage>(reference->getObjectById("image")); ASSERT_TRUE(reference_image);
        auto href = sp_image_encode_png_data_uri(*a.pixels); ASSERT_TRUE(href);
        reference_image->getRepr()->setAttribute("href", href->c_str());
        ASSERT_TRUE(BitmapAdjustments::apply_tone(reference_image, *t.tone));
        reference->ensureUpToDate(); View oracle(reference.get());
        ClientLease tc, ac;
        if (alpha_first) {
            ac = contribute({image, view.key}, a); install(ac);
            tc = contribute({image, view.key}, t); install(tc);
        } else {
            tc = contribute({image, view.key}, t); install(tc);
            ac = contribute({image, view.key}, a); install(ac);
        }
        EXPECT_EQ(render(view.drawing), render(oracle.drawing));
        EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()), xml);
    }
}
TEST_F(ComposerTest, ToneAlonePreservesExistingPixelView)
{
    View view(doc.get()), oracle(doc.get());
    auto a = alpha();
    ASSERT_TRUE(image->setViewPixbuf(view.key, a.pixels));
    ASSERT_TRUE(image->setViewPixbuf(oracle.key, a.pixels));
    auto baseline = render(view.drawing);
    auto t = contribute({image, view.key}, tone()); install(t);
    EXPECT_NE(render(view.drawing), baseline);
    t.reset();
    EXPECT_EQ(render(view.drawing), baseline);
    EXPECT_EQ(render(view.drawing), render(oracle.drawing));
}

TEST_F(ComposerTest, UnpublishedLeaseDoesNotRestoreView)
{
    View view(doc.get());
    auto drawing_item = image->get_arenaitem(view.key); ASSERT_TRUE(drawing_item);
    drawing_item->setFilterRenderer(BitmapAdjustments::build_tone_preview_renderer(image, drawing_item, tone(50).tone));
    auto existing = render(view.drawing);
    auto client = contribute({image, view.key}, tone(-40));
    ASSERT_EQ(client.prepare(tone(-40), 1).status, Status::unchanged);
    client.reset();
    EXPECT_EQ(render(view.drawing), existing);
}

TEST_F(ComposerTest, T29SourceChangesBeforeRefreshNotification)
{
    View view(doc.get());
    auto a = alpha(); auto client = contribute({image, view.key}, a);
    ASSERT_EQ(client.prepare(a, 1).status, Status::unchanged);
    auto canonical = image->pixbuf;
    image->pixbuf = std::make_shared<Pixbuf>(*canonical); // refresh before modified delivery
    EXPECT_EQ(update(client, 1).status, Status::canceled);
    client.reset(); image->pixbuf = canonical;
}

TEST_F(ComposerTest, T29RecreatedViewRejectsPreparedUpdate)
{
    View view(doc.get()); auto baseline = render(view.drawing);
    auto client = contribute({image, view.key}, alpha());
    ASSERT_EQ(client.prepare(alpha(), 1).status, Status::unchanged);
    view.hide(); view.show();
    EXPECT_EQ(update(client, 1).status, Status::unavailable);
    EXPECT_EQ(render(view.drawing), baseline);
}

TEST_F(ComposerTest, P2FullSourceBackingMeasuredAndFrozenReaderRetirement)
{
    constexpr unsigned side = 5000;
    constexpr std::uint64_t bytes = 100000000;
    auto gdk = gdk_pixbuf_new(GDK_COLORSPACE_RGB, true, 8, side, side);
    gdk_pixbuf_fill(gdk, 0x4070b080);
    image->pixbuf = std::make_shared<Pixbuf>(gdk);
    View view(doc.get());
    auto budget = std::make_shared<Budget>(Budget::FixedLimitForTest{}, 2*bytes);
    auto reserved = reserveAlphaDisplay(side, side, budget); ASSERT_TRUE(reserved.ok());
    EXPECT_EQ(budget->reserved(), bytes);
    auto source = image->pixbuf;
    auto prepared = std::async(std::launch::async, [&] {
        return prepareAlphaDisplay({source->pixels(), bytes, side*4, side, side}, *reserved.value);
    }).get();
    ASSERT_TRUE(prepared.ok());
    auto start = std::chrono::steady_clock::now();
    Contribution a; a.layer = PreviewLayer::Alpha; a.source = source; a.backing = reserved.value;
    a.pixels = wrapAlphaDisplay(a.backing);
    ASSERT_TRUE(a.pixels); EXPECT_EQ(a.pixels->width(), side); EXPECT_EQ(a.pixels->height(), side);
    EXPECT_EQ(a.pixels->pixels(), reinterpret_cast<unsigned char const *>(a.backing->pixels.data()));
    auto client = contribute({image, view.key}, a); ASSERT_TRUE(update(client, 1).ok());
    auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()-start).count();
    long long peak = 0; // RSS measurement is emitted on the measuring POSIX host.
#ifndef _WIN32
    struct rusage usage{}; ASSERT_EQ(getrusage(RUSAGE_SELF, &usage), 0);
#ifdef __APPLE__
    peak = usage.ru_maxrss;
#else
    peak = usage.ru_maxrss * 1024;
#endif
#endif
    std::cout << "P2_MEASURE source=5000x5000 backing_bytes=" << bytes
              << " wrap_install_ms=" << ms << " process_peak_rss_bytes=" << peak << std::endl;
    view.drawing.update(); view.drawing.snapshot();
    std::weak_ptr<AlphaDisplayBacking const> retired = a.backing;
    a = {}; reserved.value.reset(); client.reset();
    EXPECT_FALSE(retired.expired()); EXPECT_EQ(budget->reserved(), bytes);
    auto overlap = reserveAlphaDisplay(side, side, budget); ASSERT_TRUE(overlap.ok());
    EXPECT_EQ(budget->reserved(), 2*bytes);
    EXPECT_FALSE(reserveAlphaDisplay(1, 1, budget).ok());
    view.drawing.unsnapshot(); view.drawing.update();
    EXPECT_TRUE(retired.expired()); EXPECT_EQ(budget->reserved(), bytes);
    overlap.value.reset(); EXPECT_EQ(budget->reserved(), 0u);
}
TEST_F(ComposerTest, P2DisplayReservationValidationAndCancellation)
{
    auto budget = std::make_shared<Budget>(Budget::FixedLimitForTest{}, 16);
    EXPECT_FALSE(reserveAlphaDisplay(5001, 1, budget).ok());
    auto backing = reserveAlphaDisplay(2, 2, budget); ASSERT_TRUE(backing.ok());
    unsigned char rgba[16] = {};
    EXPECT_FALSE(prepareAlphaDisplay({rgba, 16, 4, 2, 2}, *backing.value).ok());
    auto stop = std::make_shared<std::atomic<bool>>(true);
    EXPECT_EQ(prepareAlphaDisplay({rgba, 16, 8, 2, 2}, *backing.value, Stop(stop)).status, Status::canceled);
    EXPECT_FALSE(wrapAlphaDisplay(backing.value));
    EXPECT_TRUE(prepareAlphaDisplay({rgba, 16, 8, 2, 2}, *backing.value).ok());
    View view(doc.get()); auto a = alpha();
    a.backing = backing.value; // unrelated pixels cannot borrow another reservation
    auto client = contribute({image, view.key}, a);
    EXPECT_EQ(update(client, 1).status, Status::failed);
}
