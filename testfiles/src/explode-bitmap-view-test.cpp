// SPDX-License-Identifier: GPL-2.0-or-later
#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <future>
#include <thread>
#include <vector>
#include <cairomm/surface.h>
#include "async/bitmap-job-reaper.h"
#include "bitmap-adjustment-chemistry.h"
#include "display/cairo-utils.h"
#include "display/drawing-context.h"
#include "display/drawing-image.h"
#include "display/drawing-surface.h"
#include "display/drawing.h"
#include "document.h"
#include "document-undo.h"
#include "inkgc/gc-core.h"
#include "inkscape.h"
#include "inkscape-application.h"
#include "object/sp-image.h"
#include "object/sp-root.h"
#include "ui/bitmap-preview-composer.h"
#include "xml/repr.h"

// Windows test executables use the GUI subsystem, so gtest cannot capture a death-test child's stderr (it reaches the
// parent's log instead): on Windows these tests check the death and exit code only, elsewhere the message too.
#ifdef _WIN32
#define VA_DEATH_MESSAGE(text) ""
#else
#define VA_DEATH_MESSAGE(text) text
#endif
using namespace Inkscape;
using namespace Inkscape::Bitmap;
namespace {
constexpr auto all_effects = SuppressedOwnEffects::Tone | SuppressedOwnEffects::Clip | SuppressedOwnEffects::Opacity;
bool contains(SuppressedOwnEffects effects, SuppressedOwnEffects bit)
{
    return (static_cast<unsigned>(effects) & static_cast<unsigned>(bit)) != 0;
}
class View {
public:
    explicit View(SPDocument *document) : root(document->getRoot()), key(SPItem::display_key_new(1)) { show(); }
    ~View() { hide(); }
    void show()
    {
        drawing.setRoot(root->invoke_show(drawing, key, SP_ITEM_SHOW_DISPLAY));
        active = true;
        drawing.update();
    }
    void hide() { if (active) { root->invoke_hide(key); active = false; } }
    std::vector<unsigned char> render(bool update = true)
    {
        if (update) drawing.update();
        auto surface = Cairo::ImageSurface::create(Cairo::Surface::Format::ARGB32, 8, 8);
        DrawingSurface target(surface->cobj(), Geom::IntPoint(0, 0));
        DrawingContext context(target);
        drawing.render(context, Geom::IntRect::from_xywh(0, 0, 8, 8));
        surface->flush();
        return {surface->get_data(), surface->get_data() + surface->get_stride() * 8};
    }
    Drawing drawing;
    SPRoot *root;
    unsigned key;
    bool active = false;
};
class BitmapViewTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        static auto *app = [] {
            g_setenv("INKSCAPE_APP_ID_TAG", "bitmapviewtest", TRUE);
            return new InkscapeApplication();
        }();
        ASSERT_TRUE(app->gtk_app());
        if (!Application::exists()) Application::create(false);
        recordBitmapMainThread();
        auto source = solid(90);
        auto uri = sp_image_encode_png_data_uri(*source);
        ASSERT_TRUE(uri);
        auto svg = Glib::ustring::compose(R"(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
          <defs><clipPath id="own-clip"><rect width="4" height="8"/></clipPath>
          <clipPath id="parent-clip"><rect width="8" height="6"/></clipPath>
          <filter id="parent-tone" color-interpolation-filters="sRGB" x="0" y="0" width="1" height="1">
          <feComponentTransfer><feFuncR type="linear" slope="0.8"/><feFuncG type="linear" slope="0.8"/>
          <feFuncB type="linear" slope="0.8"/></feComponentTransfer></filter></defs>
          <g id="parent"><image id="image" width="8" height="8" preserveAspectRatio="none"
          style="image-rendering:pixelated" href="%1"/></g></svg>)", *uri);
        doc = SPDocument::createNewDocFromMem(svg.raw());
        ASSERT_TRUE(doc);
        doc->ensureUpToDate();
        image = cast<SPImage>(doc->getObjectById("image"));
        ASSERT_TRUE(image);
        ASSERT_TRUE(image->pixbuf);
        doc->setModifiedSinceSave(false);
    }
    static std::shared_ptr<Pixbuf> solid(unsigned gray)
    {
        auto gdk = gdk_pixbuf_new(GDK_COLORSPACE_RGB, true, 8, 8, 8);
        gdk_pixbuf_fill(gdk, (gray << 24) | (gray << 16) | (gray << 8) | 255);
        auto result = std::make_shared<Pixbuf>(gdk);
        result->ensurePixelFormat(Pixbuf::PF_CAIRO);
        return result;
    }
    Filters::BitmapToneSettings tone(double brightness = 25)
    {
        Filters::BitmapToneSettings result;
        result.brightness = brightness;
        return result;
    }
    void ownEffects()
    {
        image->getRepr()->setAttribute("opacity", "0.5");
        image->getRepr()->setAttribute("clip-path", "url(#own-clip)");
        ASSERT_TRUE(BitmapAdjustments::apply_tone(image, tone()));
        doc->ensureUpToDate();
    }
    void ancestors()
    {
        auto parent = doc->getObjectById("parent")->getRepr();
        parent->setAttribute("opacity", "0.6");
        parent->setAttribute("clip-path", "url(#parent-clip)");
        parent->setAttribute("filter", "url(#parent-tone)");
        doc->ensureUpToDate();
    }
    std::shared_ptr<Pixbuf> baked(SuppressedOwnEffects effects)
    {
        auto result = std::make_shared<Pixbuf>(*image->pixbuf);
        result->ensurePixelFormat(Pixbuf::PF_GDK);
        auto table = Filters::build_bitmap_tone_table(tone());
        for (int y = 0; y < 8; ++y) for (int x = 0; x < 8; ++x) {
            auto p = result->pixels() + y * result->rowstride() + 4 * x;
            if (contains(effects, SuppressedOwnEffects::Tone)) {
                for (int c = 0; c < 3; ++c) p[c] = std::lround(255 * table[p[c]]);
            }
            if (contains(effects, SuppressedOwnEffects::Opacity)) p[3] = 128;
            if (contains(effects, SuppressedOwnEffects::Clip) && x >= 4) p[3] = 0;
        }
        result->markDirty();
        result->ensurePixelFormat(Pixbuf::PF_CAIRO);
        return result;
    }
    ViewPixels pixels(View &view, std::shared_ptr<Pixbuf const> value)
    {
        return {std::move(value), image->pixbuf, SPItem::ensure_key(image->get_arenaitem(view.key))};
    }
    bool install(View &view, std::shared_ptr<Pixbuf const> value, SuppressedOwnEffects effects)
    {
        auto p = pixels(view, std::move(value));
        return image->setComposedView(view.key, p, effects, image->composedViewGeneration(view.key));
    }
    std::unique_ptr<SPDocument> oracle(std::shared_ptr<Pixbuf const> value, SuppressedOwnEffects effects)
    {
        auto result = SPDocument::createNewDocFromMem(sp_repr_save_buf(doc->getReprDoc()).raw());
        auto target = cast<SPImage>(result->getObjectById("image"));
        auto uri = sp_image_encode_png_data_uri(*value);
        EXPECT_TRUE(uri);
        target->getRepr()->setAttribute("href", uri->c_str());
        if (contains(effects, SuppressedOwnEffects::Tone)) target->getRepr()->setAttribute("style", "image-rendering:pixelated");
        if (contains(effects, SuppressedOwnEffects::Clip)) target->getRepr()->setAttribute("clip-path", nullptr);
        if (contains(effects, SuppressedOwnEffects::Opacity)) target->getRepr()->setAttribute("opacity", nullptr);
        result->ensureUpToDate();
        return result;
    }
    Contribution alpha(std::shared_ptr<Pixbuf const> value)
    {
        Contribution c;
        c.layer = PreviewLayer::Alpha;
        Pixbuf straight(*value); straight.ensurePixelFormat(Pixbuf::PF_GDK);
        auto budget = std::make_shared<Budget>(Budget::FixedLimitForTest{}, 100000000);
        auto backing = reserveAlphaDisplay(straight.width(), straight.height(), budget);
        EXPECT_TRUE(backing.ok());
        EXPECT_TRUE(prepareAlphaDisplay({straight.pixels(), std::uint64_t(straight.rowstride())*straight.height(),
            unsigned(straight.rowstride()), unsigned(straight.width()), unsigned(straight.height())}, *backing.value).ok());
        c.backing = backing.value; c.pixels = wrapAlphaDisplay(c.backing);
        c.source = image->pixbuf;
        return c;
    }
    std::unique_ptr<SPDocument> doc;
    SPImage *image = nullptr;
};
}
TEST_F(BitmapViewTest, T25EverySuppressionCombinationIsLocalAndAncestorsApplyOnce)
{
    ownEffects(); ancestors();
    auto xml = sp_repr_save_buf(doc->getReprDoc());
    View canonical(doc.get()), preview(doc.get());
    auto baseline = canonical.render();
    for (unsigned bits = 0; bits < 8; ++bits) {
        auto effects = static_cast<SuppressedOwnEffects>(bits);
        SCOPED_TRACE(bits);
        auto value = baked(effects);
        auto reference = oracle(value, effects);
        View expected(reference.get());
        ASSERT_TRUE(install(preview, value, effects));
        EXPECT_EQ(preview.render(), expected.render());
        EXPECT_EQ(canonical.render(), baseline);
        EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()), xml);
        EXPECT_FALSE(doc->isModifiedSinceSave());
        auto actual = preview.render();
        // Independent LUT/half-opacity/hard-clip bake matches committed output.
        ASSERT_EQ(actual.size(), baseline.size());
        for (std::size_t i = 0; i < actual.size(); ++i) EXPECT_LE(std::abs(int(actual[i]) - int(baseline[i])), 2);
    }
    ASSERT_TRUE(install(preview, nullptr, SuppressedOwnEffects::None));
    EXPECT_EQ(preview.render(), baseline);
}
TEST_F(BitmapViewTest, T25PixelOnlyApiDoesNotSuppressEffects)
{
    ownEffects();
    View preview(doc.get()), reference(doc.get());
    auto source = solid(160);
    auto expected_doc = oracle(source, SuppressedOwnEffects::None);
    View expected(expected_doc.get());
    ASSERT_TRUE(image->setViewPixbuf(preview.key, source));
    EXPECT_EQ(preview.render(), expected.render());
    EXPECT_NE(preview.render(), reference.render());
    ASSERT_TRUE(install(preview, baked(all_effects), all_effects));
    auto drawing_image = cast<DrawingImage>(image->get_arenaitem(preview.key));
    ASSERT_TRUE(image->setViewPixbuf(preview.key, source));
    EXPECT_FALSE(drawing_image->suppressesOwnEffect(SuppressedOwnEffects::Opacity));
    EXPECT_FALSE(drawing_image->suppressesOwnEffect(SuppressedOwnEffects::Clip));
    EXPECT_FALSE(drawing_image->suppressesOwnEffect(SuppressedOwnEffects::Tone));
    EXPECT_EQ(preview.render(), expected.render());
    // The same legacy operation must supersede a queued composed installation.
    preview.drawing.snapshot();
    ASSERT_TRUE(install(preview, baked(all_effects), all_effects));
    ASSERT_TRUE(image->setViewPixbuf(preview.key, source));
    preview.drawing.unsnapshot();
    EXPECT_EQ(preview.render(), expected.render());
}
TEST_F(BitmapViewTest, T29StaleConsumedAndAbsentViewTicketsCannotInstall)
{
    View view(doc.get());
    auto value = pixels(view, solid(180));
    auto old = image->composedViewGeneration(view.key);
    auto current = image->composedViewGeneration(view.key);
    EXPECT_FALSE(image->setComposedView(view.key, value, all_effects, old));
    ASSERT_TRUE(image->setComposedView(view.key, value, SuppressedOwnEffects::None, current));
    auto baseline = view.render();
    EXPECT_FALSE(image->setComposedView(view.key, value, all_effects, current));
    EXPECT_EQ(view.render(), baseline);
    auto ticket = image->composedViewGeneration(view.key);
    view.hide();
    EXPECT_EQ(image->composedViewGeneration(view.key), 0u);
    EXPECT_FALSE(image->setComposedView(view.key, value, all_effects, ticket));
    view.show();
    // Even a numerically equal ticket in a recreated view cannot reuse its key.
    auto recreated = image->composedViewGeneration(view.key);
    EXPECT_FALSE(image->setComposedView(view.key, value, all_effects, recreated));
    EXPECT_FALSE(image->setComposedView(SPItem::display_key_new(1), value, all_effects, recreated));
    auto fresh = pixels(view, solid(210));
    ASSERT_TRUE(image->setComposedView(view.key, fresh, SuppressedOwnEffects::None, recreated));
}
TEST_F(BitmapViewTest, T29RefreshRetiresTicketAndRemovesSuppression)
{
    ownEffects();
    View preview(doc.get()), canonical(doc.get());
    ASSERT_TRUE(install(preview, baked(all_effects), all_effects));
    auto value = pixels(preview, solid(200));
    auto ticket = image->composedViewGeneration(preview.key);
    auto uri = sp_image_encode_png_data_uri(*solid(150)); ASSERT_TRUE(uri);
    image->getRepr()->setAttribute("href", uri->c_str());
    doc->ensureUpToDate();
    EXPECT_FALSE(image->setComposedView(preview.key, value, all_effects, ticket));
    auto fresh_ticket = image->composedViewGeneration(preview.key);
    EXPECT_FALSE(image->setComposedView(preview.key, value, all_effects, fresh_ticket));
    EXPECT_EQ(preview.render(), canonical.render());
    auto drawing_image = cast<DrawingImage>(image->get_arenaitem(preview.key));
    EXPECT_FALSE(drawing_image->suppressesOwnEffect(SuppressedOwnEffects::Tone));
    EXPECT_FALSE(drawing_image->suppressesOwnEffect(SuppressedOwnEffects::Clip));
    EXPECT_FALSE(drawing_image->suppressesOwnEffect(SuppressedOwnEffects::Opacity));
}
TEST_F(BitmapViewTest, T29InvalidSourceAndShapeNeverChangeView)
{
    View view(doc.get());
    auto baseline = view.render();
    auto value = pixels(view, solid(200));
    auto generation = image->composedViewGeneration(view.key);
    value.source = solid(90); // expired/wrong source incarnation
    EXPECT_FALSE(image->setComposedView(view.key, value, all_effects, generation));
    value.source = image->pixbuf;
    auto gdk = gdk_pixbuf_new(GDK_COLORSPACE_RGB, true, 8, 1, 1);
    value.pixels = std::make_shared<Pixbuf>(gdk);
    EXPECT_FALSE(image->setComposedView(view.key, value, all_effects, generation));
    value.pixels.reset();
    EXPECT_FALSE(image->setComposedView(view.key, value, all_effects, generation));
    EXPECT_FALSE(image->setComposedView(view.key, value, static_cast<SuppressedOwnEffects>(8), generation));
    EXPECT_EQ(view.render(), baseline);
    ASSERT_TRUE(image->setComposedView(view.key, value, SuppressedOwnEffects::None, generation));
}
TEST_F(BitmapViewTest, T29ComposerRefreshUndoAndLastDepartureUseCurrentAppearance)
{
    ownEffects();
    DocumentUndo::done(doc.get(), Util::Internal::ContextString{"Initial own effects"}, "");
    View preview(doc.get()), canonical(doc.get());
    Contribution tc; tc.tone = tone(40);
    auto tone_client = contribute({image, preview.key}, tc);
    ASSERT_EQ(update(tone_client, 1).status, Status::changed);
    auto alpha_client = contribute({image, preview.key}, alpha(solid(210)));
    ASSERT_EQ(update(alpha_client, 1).status, Status::changed);
    auto stale = alpha(solid(170));
    ASSERT_EQ(alpha_client.prepare(stale, 2).status, Status::unchanged);
    auto uri = sp_image_encode_png_data_uri(*solid(130)); ASSERT_TRUE(uri);
    image->getRepr()->setAttribute("href", uri->c_str());
    ASSERT_TRUE(BitmapAdjustments::apply_tone(image, tone(-20)));
    image->getRepr()->setAttribute("opacity", "0.75");
    doc->ensureUpToDate();
    DocumentUndo::done(doc.get(), Util::Internal::ContextString{"Change current source/style"}, "");
    EXPECT_EQ(update(alpha_client, 2).status, Status::canceled);
    auto reference = oracle(image->pixbuf, SuppressedOwnEffects::None);
    auto ri = cast<SPImage>(reference->getObjectById("image"));
    ASSERT_TRUE(BitmapAdjustments::apply_tone(ri, *tc.tone)); reference->ensureUpToDate();
    View expected(reference.get());
    EXPECT_EQ(preview.render(), expected.render()); // live tone recomposed on new source
    ASSERT_TRUE(DocumentUndo::undo(doc.get())); doc->ensureUpToDate();
    EXPECT_EQ(update(alpha_client, 2).status, Status::canceled);
    ASSERT_TRUE(DocumentUndo::redo(doc.get())); doc->ensureUpToDate();
    alpha_client.reset(); tone_client.reset();
    EXPECT_EQ(preview.render(), canonical.render());
    EXPECT_EQ(std::string(image->getRepr()->attribute("href")), *uri);
}
TEST_F(BitmapViewTest, T29ComposerUnavailableNeverReportsPreviewSuccess)
{
    View view(doc.get());
    auto client = contribute({image, view.key}, alpha(solid(180)));
    ASSERT_EQ(client.prepare(alpha(solid(200)), 1).status, Status::unchanged);
    view.hide();
    EXPECT_EQ(update(client, 1).status, Status::unavailable);
    view.show();
    EXPECT_EQ(update(client, 1).status, Status::canceled);
    ASSERT_EQ(client.prepare(alpha(solid(220)), 2).status, Status::unchanged);
    ASSERT_EQ(update(client, 2).status, Status::changed);
    view.hide(); client.reset(); // closed window restoration must be safe
}
TEST_F(BitmapViewTest, T08SnapshotPixelsAndEffectsPublishTogetherAcrossGcAndRefresh)
{
    ownEffects();
    View view(doc.get()), canonical(doc.get());
    auto original = view.render();
    auto value = pixels(view, baked(all_effects));
    std::weak_ptr<Pixbuf const> lifetime = value.pixels;
    view.drawing.snapshot();
    auto ticket = image->composedViewGeneration(view.key);
    ASSERT_TRUE(image->setComposedView(view.key, value, all_effects, ticket));
    value.pixels.reset();
    GC::Core::gcollect();
    // Worker reads only the frozen drawing, never SPImage or ticket state.
    std::promise<void> started, resume;
    auto ready = resume.get_future();
    auto worker = std::async(std::launch::async, [&] {
        started.set_value(); ready.wait();
        return view.render(false);
    });
    started.get_future().wait();
    EXPECT_FALSE(lifetime.expired());
    auto uri = sp_image_encode_png_data_uri(*solid(180)); ASSERT_TRUE(uri);
    image->getRepr()->setAttribute("href", uri->c_str()); doc->ensureUpToDate();
    GC::Core::gcollect();
    resume.set_value(); // render the old immutable snapshot after live XML/source changes
    EXPECT_EQ(worker.get(), original);
    view.drawing.unsnapshot(); // retired composed operation cannot resurrect
    EXPECT_EQ(view.render(), canonical.render());
    EXPECT_TRUE(lifetime.expired());
    auto fresh = baked(all_effects);
    auto reference = oracle(fresh, all_effects); View expected(reference.get());
    auto before = view.render();
    view.drawing.snapshot();
    ASSERT_TRUE(install(view, fresh, all_effects));
    EXPECT_EQ(view.render(false), before);
    view.drawing.unsnapshot();
    EXPECT_EQ(view.render(), expected.render());
}
TEST_F(BitmapViewTest, T08NewestQueuedInstallationWinsAndHideReleasesBacking)
{
    View view(doc.get());
    auto first = solid(150), second = solid(210);
    std::weak_ptr<Pixbuf const> old = first, live = second;
    view.drawing.snapshot();
    ASSERT_TRUE(install(view, first, all_effects));
    ASSERT_TRUE(install(view, second, SuppressedOwnEffects::None));
    first.reset(); second.reset();
    GC::Core::gcollect();
    view.drawing.unsnapshot();
    EXPECT_TRUE(old.expired()); EXPECT_FALSE(live.expired());
    auto reference = oracle(live.lock(), SuppressedOwnEffects::None); View expected(reference.get());
    EXPECT_EQ(view.render(), expected.render());
    view.hide(); GC::Core::gcollect(); EXPECT_TRUE(live.expired());
}

TEST_F(BitmapViewTest, T08ViewApiRejectsOffThreadBeforeTouchingObjects)
{
    View view(doc.get());
    auto value = pixels(view, solid(200));
    auto ticket = image->composedViewGeneration(view.key);
    EXPECT_DEATH({
        std::thread wrong([&] { image->composedViewGeneration(view.key); }); wrong.join();
    }, VA_DEATH_MESSAGE("main-thread affinity"));
    EXPECT_DEATH({
        std::thread wrong([&] { image->setComposedView(view.key, value, all_effects, ticket); }); wrong.join();
    }, VA_DEATH_MESSAGE("main-thread affinity"));
}
TEST_F(BitmapViewTest, T29HiddenImageAndHiddenAncestorReportUnavailable)
{
    View view(doc.get());
    auto value = pixels(view, solid(200));
    auto ticket = image->composedViewGeneration(view.key);
    auto client = contribute({image, view.key}, alpha(solid(180)));
    ASSERT_EQ(client.prepare(alpha(solid(190)), 1).status, Status::unchanged);
    image->setHidden(true); doc->ensureUpToDate();
    EXPECT_EQ(image->composedViewGeneration(view.key), 0u);
    EXPECT_FALSE(image->setComposedView(view.key, value, all_effects, ticket));
    EXPECT_EQ(update(client, 1).status, Status::unavailable);
    image->setHidden(false); doc->ensureUpToDate();
    auto parent = cast<SPItem>(doc->getObjectById("parent")); ASSERT_TRUE(parent);
    parent->setHidden(true); doc->ensureUpToDate();
    EXPECT_EQ(image->composedViewGeneration(view.key), 0u);
    EXPECT_EQ(update(client, 2).status, Status::unavailable);
    parent->setHidden(false); doc->ensureUpToDate();
    ASSERT_EQ(client.prepare(alpha(solid(220)), 3).status, Status::unchanged);
    ASSERT_EQ(update(client, 3).status, Status::changed);
}
TEST_F(BitmapViewTest, T29TenThousandTicketsReleaseSupersededBuffers)
{
    View view(doc.get()), canonical(doc.get());
    auto original = view.render();
    std::weak_ptr<Pixbuf const> previous;
    auto current = pixels(view, solid(140));
    Generation retired = 0, latest = 0;
    for (unsigned i = 0; i < 10000; ++i) {
        retired = latest;
        latest = image->composedViewGeneration(view.key);
        previous = current.pixels;
        current = pixels(view, solid(90 + i % 150));
        if (retired) EXPECT_FALSE(image->setComposedView(view.key, current, all_effects, retired));
        EXPECT_TRUE(previous.expired());
    }
    EXPECT_EQ(view.render(), original); // preparation never publishes
    ASSERT_TRUE(image->setComposedView(view.key, current, SuppressedOwnEffects::None, latest));
    auto reference = oracle(current.pixels, SuppressedOwnEffects::None); View expected(reference.get());
    EXPECT_EQ(view.render(), expected.render());
    std::weak_ptr<Pixbuf const> installed = current.pixels;
    current.pixels.reset();
    ASSERT_TRUE(install(view, nullptr, SuppressedOwnEffects::None));
    EXPECT_TRUE(installed.expired());
    EXPECT_EQ(view.render(), canonical.render());
}
TEST_F(BitmapViewTest, T29RestoreReadsCurrentPixelsRatherThanDepartingSource)
{
    ownEffects();
    View view(doc.get()), canonical(doc.get());
    ASSERT_TRUE(install(view, baked(all_effects), all_effects));
    auto restore = pixels(view, nullptr); // carries deliberately obsolete source
    auto uri = sp_image_encode_png_data_uri(*solid(190)); ASSERT_TRUE(uri);
    image->getRepr()->setAttribute("href", uri->c_str());
    image->getRepr()->setAttribute("opacity", "0.9");
    ASSERT_TRUE(BitmapAdjustments::apply_tone(image, tone(-30)));
    doc->ensureUpToDate();
    auto ticket = image->composedViewGeneration(view.key);
    ASSERT_TRUE(image->setComposedView(view.key, restore, SuppressedOwnEffects::None, ticket));
    EXPECT_EQ(view.render(), canonical.render());
    image->getRepr()->setAttribute("href", nullptr); doc->ensureUpToDate();
    EXPECT_FALSE(image->pixbuf);
    ASSERT_TRUE(install(view, nullptr, SuppressedOwnEffects::None));
    EXPECT_EQ(view.render(), canonical.render());
}

TEST_F(BitmapViewTest, T08NewPreparationRetiresInstallationQueuedBehindSnapshot)
{
    View view(doc.get());
    auto baseline = view.render();
    auto pending = solid(200);
    std::weak_ptr<Pixbuf const> retired = pending;
    view.drawing.snapshot();
    ASSERT_TRUE(install(view, pending, all_effects));
    pending.reset();
    auto next = image->composedViewGeneration(view.key);
    ASSERT_NE(next, 0u);
    EXPECT_TRUE(retired.expired()); // retirement releases payload before snapshot ends
    view.drawing.unsnapshot();
    EXPECT_TRUE(retired.expired());
    EXPECT_EQ(view.render(), baseline);
    auto value = pixels(view, solid(230));
    ASSERT_TRUE(image->setComposedView(view.key, value, SuppressedOwnEffects::None, next));
    auto reference = oracle(value.pixels, SuppressedOwnEffects::None); View expected(reference.get());
    EXPECT_EQ(view.render(), expected.render());
}
TEST_F(BitmapViewTest, T29RejectedInstallationPropagatesUnavailableThroughComposer)
{
    View view(doc.get());
    auto baseline = view.render();
    auto client = contribute({image, view.key}, alpha(solid(180)));
    ASSERT_EQ(client.prepare(alpha(solid(200)), 1).status, Status::unchanged);
    // Retire only the SPImage ticket, leaving the owner's revision and view
    // unchanged. This reaches install(false), rather than the absent-view gate.
    ASSERT_NE(image->composedViewGeneration(view.key), 0u);
    EXPECT_EQ(update(client, 1).status, Status::unavailable);
    EXPECT_EQ(view.render(), baseline);
    EXPECT_EQ(update(client, 1).status, Status::canceled);
    ASSERT_EQ(client.prepare(alpha(solid(220)), 2).status, Status::unchanged);
    EXPECT_EQ(update(client, 2).status, Status::changed);
    client.reset(); EXPECT_EQ(view.render(), baseline);
}

TEST_F(BitmapViewTest, T29RetiredToneOnlyTicketCannotInstallRendererOrReportChanged)
{
    ownEffects();
    View view(doc.get());
    auto legacy = solid(160);
    ASSERT_TRUE(image->setViewPixbuf(view.key, legacy));
    auto baseline = view.render();
    Contribution tc; tc.tone = tone(40);
    auto client = contribute({image, view.key}, tc);
    ASSERT_EQ(client.prepare(tc, 1).status, Status::unchanged);
    ASSERT_NE(image->composedViewGeneration(view.key), 0u);
    EXPECT_EQ(update(client, 1).status, Status::unavailable);
    EXPECT_EQ(view.render(), baseline);
    ASSERT_EQ(client.prepare(tc, 2).status, Status::unchanged);
    ASSERT_EQ(update(client, 2).status, Status::changed);
    auto reference = oracle(legacy, SuppressedOwnEffects::None);
    auto target = cast<SPImage>(reference->getObjectById("image"));
    ASSERT_TRUE(BitmapAdjustments::apply_tone(target, *tc.tone));
    reference->ensureUpToDate(); View expected(reference.get());
    EXPECT_EQ(view.render(), expected.render()); // tone alone preserves legacy pixels
    EXPECT_NE(view.render(), baseline);
}
TEST_F(BitmapViewTest, T29ToneOnlyAfterBakedToneRestoresCanonicalPixelsAndRendersNewTone)
{
    ownEffects(); ancestors();
    auto xml = sp_repr_save_buf(doc->getReprDoc()).raw();
    for (auto effects : {SuppressedOwnEffects::Tone, all_effects}) {
        for (bool snapshot : {false, true}) {
            SCOPED_TRACE(static_cast<unsigned>(effects));
            SCOPED_TRACE(snapshot);
            View view(doc.get()), canonical(doc.get());
            auto canonical_pixels = canonical.render();
            ASSERT_TRUE(install(view, baked(effects), effects));
            auto before = view.render();
            auto drawing_image = cast<DrawingImage>(image->get_arenaitem(view.key));
            ASSERT_TRUE(drawing_image->suppressesOwnEffect(SuppressedOwnEffects::Tone));
            Contribution tc; tc.tone = tone(60);
            auto client = contribute({image, view.key}, tc);
            ASSERT_EQ(client.prepare(tc, 1).status, Status::unchanged);
            auto reference = oracle(image->pixbuf, SuppressedOwnEffects::None);
            auto target = cast<SPImage>(reference->getObjectById("image"));
            ASSERT_TRUE(BitmapAdjustments::apply_tone(target, *tc.tone));
            reference->ensureUpToDate(); View expected(reference.get());
            auto expected_pixels = expected.render();
            ASSERT_NE(expected_pixels, before);
            if (snapshot) view.drawing.snapshot();
            ASSERT_EQ(update(client, 1).status, Status::changed);
            if (snapshot) {
                EXPECT_EQ(view.render(false), before);
                EXPECT_TRUE(drawing_image->suppressesOwnEffect(SuppressedOwnEffects::Tone));
                view.drawing.unsnapshot();
            }
            EXPECT_EQ(view.render(), expected_pixels);
            for (auto bit : {SuppressedOwnEffects::Tone, SuppressedOwnEffects::Clip, SuppressedOwnEffects::Opacity}) {
                EXPECT_FALSE(drawing_image->suppressesOwnEffect(bit));
            }
            EXPECT_EQ(canonical.render(), canonical_pixels);
            EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(), xml);
            client.reset();
            EXPECT_EQ(view.render(), canonical_pixels);
        }
    }
}
TEST_F(BitmapViewTest, T08QueuedRendererAndPixelsRetireTogetherOnNewPreparation)
{
    ownEffects();
    for (bool with_alpha : {false, true}) {
        SCOPED_TRACE(with_alpha);
        View view(doc.get());
        auto baseline = view.render();
        Contribution tc; tc.tone = tone(40);
        auto client = contribute({image, view.key}, tc);
        ClientLease ac;
        if (with_alpha) ac = contribute({image, view.key}, alpha(solid(200)));
        view.drawing.snapshot();
        ASSERT_EQ(update(client, 1).status, Status::changed);
        if (with_alpha) ASSERT_EQ(update(ac, 1).status, Status::changed);
        EXPECT_EQ(view.render(false), baseline);
        tc.tone = tone(60);
        ASSERT_EQ(client.prepare(tc, 2).status, Status::unchanged);
        view.drawing.unsnapshot();
        EXPECT_EQ(view.render(), baseline); // neither queued renderer nor pixels publish
        view.drawing.snapshot();
        ASSERT_EQ(update(client, 2).status, Status::changed);
        EXPECT_EQ(view.render(false), baseline);
        view.drawing.unsnapshot();
        auto reference = oracle(with_alpha ? solid(200) : image->pixbuf, SuppressedOwnEffects::None);
        auto target = cast<SPImage>(reference->getObjectById("image"));
        ASSERT_TRUE(BitmapAdjustments::apply_tone(target, *tc.tone));
        reference->ensureUpToDate(); View expected(reference.get());
        EXPECT_EQ(view.render(), expected.render()); // both current parts publish coherently
        EXPECT_NE(view.render(), baseline);
    }
}
TEST_F(BitmapViewTest, T08SuccessfulQueuedInstallationsRetainOnlyLatestBuffer)
{
    View view(doc.get());
    auto baseline = view.render();
    std::vector<std::weak_ptr<Pixbuf const>> buffers;
    view.drawing.snapshot();
    for (unsigned i = 0; i < 1000; ++i) {
        auto value = solid(100 + i % 150);
        buffers.emplace_back(value);
        ASSERT_TRUE(install(view, value, SuppressedOwnEffects::None));
        value.reset();
        // Count actual surviving payloads before replay, not just generation flags.
        EXPECT_EQ(std::count_if(buffers.begin(), buffers.end(), [](auto const &w) { return !w.expired(); }), 1);
    }
    GC::Core::gcollect();
    EXPECT_EQ(view.render(false), baseline);
    auto reference = oracle(buffers.back().lock(), SuppressedOwnEffects::None);
    View expected(reference.get());
    view.drawing.unsnapshot();
    EXPECT_EQ(view.render(), expected.render());
    view.hide(); GC::Core::gcollect();
    EXPECT_EQ(std::count_if(buffers.begin(), buffers.end(), [](auto const &w) { return !w.expired(); }), 0);
}
TEST_F(BitmapViewTest, T25BakedFivePercentOpacityPicksAtRenderedAlpha)
{
    image->getRepr()->setAttribute("opacity", "0.05"); doc->ensureUpToDate();
    View view(doc.get()), canonical(doc.get());
    auto baseline = canonical.render();
    auto value = solid(90);
    value->ensurePixelFormat(Pixbuf::PF_GDK);
    for (int y = 0; y < 8; ++y) for (int x = 0; x < 8; ++x) {
        value->pixels()[y * value->rowstride() + 4 * x + 3] = 12; // Cairo quantizes native 5% opacity to this alpha byte
    }
    value->markDirty(); value->ensurePixelFormat(Pixbuf::PF_CAIRO);
    ASSERT_TRUE(install(view, value, SuppressedOwnEffects::Opacity));
    auto rendered = view.render();
    EXPECT_EQ(rendered, baseline);
    EXPECT_EQ(rendered[3], 12);
    auto item = image->get_arenaitem(view.key);
    EXPECT_EQ(item->pick(Geom::Point(2, 2), 0, {}), item);
    EXPECT_NE(view.drawing.pick(Geom::Point(2, 2), 0, {}, 0), nullptr);
    auto canonical_item = image->get_arenaitem(canonical.key);
    EXPECT_EQ(canonical_item->pick(Geom::Point(2, 2), 0, {}), canonical_item);
}
TEST_F(BitmapViewTest, DependencyStampObservationIsReadOnlyAndTracksChanges)
{
    View view(doc.get());
    auto first = image->viewDependencyStamp(view.key);
    ASSERT_NE(first.incarnation, 0u);
    for (int i = 0; i < 5; ++i) EXPECT_EQ(image->viewDependencyStamp(view.key), first);

    // Observation must not retire a pending ticket or a queued installation.
    auto baseline = view.render();
    auto ticket = image->composedViewGeneration(view.key);
    auto ticketed = image->viewDependencyStamp(view.key);
    EXPECT_NE(ticketed, first); // ticket issuance
    EXPECT_EQ(ticketed.incarnation, first.incarnation);
    view.drawing.snapshot();
    auto value = pixels(view, solid(200));
    ASSERT_TRUE(image->setComposedView(view.key, value, SuppressedOwnEffects::None, ticket));
    auto accepted = image->viewDependencyStamp(view.key);
    EXPECT_NE(accepted, ticketed); // accepted installation
    for (int i = 0; i < 5; ++i) EXPECT_EQ(image->viewDependencyStamp(view.key), accepted);
    view.drawing.unsnapshot();
    auto reference = oracle(value.pixels, SuppressedOwnEffects::None); View expected(reference.get());
    EXPECT_EQ(view.render(), expected.render()); // queued pixels survived observation
    EXPECT_NE(view.render(), baseline);
    EXPECT_EQ(image->viewDependencyStamp(view.key), accepted);

    // A pending ticket stays installable after repeated observation.
    auto ticket2 = image->composedViewGeneration(view.key);
    auto retiring = image->viewDependencyStamp(view.key);
    EXPECT_NE(retiring, accepted); // retirement of the earlier ticket / new issuance
    image->viewDependencyStamp(view.key);
    EXPECT_TRUE(image->setComposedView(view.key, pixels(view, solid(150)), SuppressedOwnEffects::None, ticket2));
    EXPECT_NE(image->viewDependencyStamp(view.key), retiring);

    // Canonical replacement.
    auto before = image->viewDependencyStamp(view.key);
    auto uri = sp_image_encode_png_data_uri(*solid(30)); ASSERT_TRUE(uri);
    image->getRepr()->setAttribute("href", uri->c_str());
    doc->ensureUpToDate();
    auto replaced = image->viewDependencyStamp(view.key);
    EXPECT_NE(replaced, before);
    EXPECT_EQ(replaced.incarnation, before.incarnation);

    // Legacy view-pixel change.
    ASSERT_TRUE(image->setViewPixbuf(view.key, solid(60)));
    EXPECT_NE(image->viewDependencyStamp(view.key), replaced);

    // Hidden view is unavailable; a recreated view has a new incarnation.
    auto old = image->viewDependencyStamp(view.key);
    view.hide();
    EXPECT_FALSE(image->viewDependencyStamp(view.key).available());
    view.show();
    auto recreated = image->viewDependencyStamp(view.key);
    ASSERT_TRUE(recreated.available());
    EXPECT_NE(recreated.incarnation, old.incarnation);
    EXPECT_NE(recreated, old);
}
TEST_F(BitmapViewTest, DependencyChangedSignalsEveryRevisionEventIncludingSaturation)
{
    class Probe final : public DrawingImage {
    public:
        using DrawingImage::DrawingImage;
        void nearExhaustion() { _revision = UINT64_MAX - 1; }
    };
    Drawing drawing;
    auto *view = new Probe(drawing); drawing.setRoot(view);
    unsigned events = 0;
    auto connection = view->connectDependencyChanged([&] {
        ++events;
        // Emission follows the increment, before any deferred pixel installation.
        EXPECT_EQ(view->dependencyStamp().revision, events <= 4 ? events : 0u);
    });
    drawing.snapshot();
    view->setCanonicalPixbuf(solid(90)); EXPECT_EQ(events, 1u);
    auto ticket = view->composedGeneration(); EXPECT_EQ(events, 2u);
    for (int i = 0; i < 100; ++i) EXPECT_TRUE(view->dependencyStamp().available());
    EXPECT_EQ(events, 2u);
    EXPECT_TRUE(view->setComposedPixels(solid(100), SuppressedOwnEffects::None, ticket, nullptr, true));
    EXPECT_EQ(events, 3u);
    EXPECT_FALSE(view->setComposedPixels({}, SuppressedOwnEffects::None, ticket, nullptr, true));
    EXPECT_EQ(events, 3u); // rejected ticket is not a revision event
    view->setErasePreview({}); EXPECT_EQ(events, 4u);
    drawing.unsnapshot(); EXPECT_EQ(events, 4u); // deferred replay adds no revision
    view->nearExhaustion();
    view->setErasePreview({}); EXPECT_EQ(events, 5u);
    for (int i = 0; i < 100; ++i) EXPECT_FALSE(view->dependencyStamp().available());
    EXPECT_EQ(events, 5u);
    view->setCanonicalPixbuf({}); EXPECT_EQ(events, 6u);
    ticket = view->composedGeneration(); EXPECT_EQ(events, 7u);
    EXPECT_TRUE(view->setComposedPixels({}, SuppressedOwnEffects::None, ticket, nullptr, true));
    EXPECT_EQ(events, 8u);
    connection.disconnect(); view->setErasePreview({}); EXPECT_EQ(events, 8u);
}
