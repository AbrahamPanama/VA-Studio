// SPDX-License-Identifier: GPL-2.0-or-later
#include "io/artwork-library-insert.h"
#include "io/artwork-library-package.h"
#include "io/artwork-library-document.h"
#include "id-clash.h"
#include "document.h"
#include "document-undo.h"
#include "inkscape.h"
#include "inkscape-application.h"
#include "inkscape-version.h"
#include "object/object-set.h"
#include "object/sp-item-group.h"
#include "object/sp-root.h"
#include "object/sp-text.h"
#include "object/sp-textpath.h"
#include "ui/text-frame-tools.h"
#include "object/sp-paint-server.h"
#include "object/sp-gradient.h"
#include "object/sp-gradient-reference.h"
#include "object/sp-clippath.h"
#include "object/sp-mask.h"
#include "style.h"
#include "preferences.h"
#include "undo-stack-observer.h"
#include "xml/attribute-record.h"
#include "xml/node-observer.h"
#include "xml/repr.h"
#include "xml/document.h"
#include "display/cairo-utils.h"
#include "display/drawing.h"
#include "display/drawing-item.h"
#include "display/drawing-context.h"
#include "display/drawing-surface.h"
#include <gtest/gtest.h>
#include <png.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <thread>

using namespace Inkscape;
namespace Library = IO::ArtworkLibrary;
namespace {
std::string svg(std::string const &body, std::string const &dimensions =
    "width='25.4mm' height='25.4mm' viewBox='0 0 96 96'", std::string const &extra = "")
{
    return "<svg xmlns='http://www.w3.org/2000/svg' xmlns:xlink='http://www.w3.org/1999/xlink' "
           "xmlns:inkscape='http://www.inkscape.org/namespaces/inkscape' " +
           dimensions + " " + extra + ">" + body + "</svg>";
}
Library::ValidatedSvg token(std::string const &xml, double w = 25.4, double h = 25.4)
{
    Library::Bytes bytes(xml.begin(), xml.end());
    return Library::preflight_svg(bytes, {"b203e8e9-640c-4195-b0ea-f9b790245678",
                                         Library::artwork_sha256(bytes), w, h});
}
std::unique_ptr<SPDocument> document(std::string const &xml)
{
    auto doc = SPDocument::createNewDocFromMem(std::span<char const>(xml.data(), xml.size()));
    if (!doc) throw std::runtime_error("Native document fixture");
    doc->ensureUpToDate();
    DocumentUndo::done(doc.get(), Util::Internal::ContextString("Fixture"), "");
    DocumentUndo::clearUndo(doc.get()); DocumentUndo::clearRedo(doc.get());
    doc->setModifiedSinceSave(false);
    return doc;
}
void snapshot(XML::Node const *node, std::string &out)
{
    auto field = [&](char const *s) {
        if (!s) { out += "-;"; return; }
        out += std::to_string(std::strlen(s)) + ":" + s;
    };
    out += "[" + std::to_string(static_cast<int>(node->type()));
    field(node->name()); field(node->content());
    for (auto const &a : node->attributeList()) { field(g_quark_to_string(a.key)); field(a.value); }
    out += ";";
    for (auto *c = node->firstChild(); c; c = c->next()) snapshot(c, out);
    out += "]";
}
std::string snapshot(SPDocument &doc) { std::string out; snapshot(doc.getReprDoc(), out); return out; }
XML::Node *find(XML::Node *root, char const *name)
{
    if (!std::strcmp(root->name(), name)) return root;
    for (auto *c = root->firstChild(); c; c = c->next()) if (auto n = find(c, name)) return n;
    return nullptr;
}
SPItem *group(SPDocument &doc, Library::InsertedArtwork const &result)
{
    return cast<SPItem>(doc.getObjectById(result.inserted_ids.at(0).c_str()));
}
struct History final : UndoStackObserver {
    SPDocument &doc;
    unsigned commits = 0, undos = 0, redos = 0, clears = 0;
    explicit History(SPDocument &d) : doc(d) { doc.addUndoObserver(*this); }
    ~History() override { doc.removeUndoObserver(*this); }
    void notifyUndoEvent(Event *) override { ++undos; }
    void notifyRedoEvent(Event *) override { ++redos; }
    void notifyUndoCommitEvent(Event *) override { ++commits; }
    void notifyUndoExpired(Event *) override {}
    void notifyClearUndoEvent() override { ++clears; }
    void notifyClearRedoEvent() override { ++clears; }
};
struct Added final : XML::NodeObserver {
    XML::Node &root;
    std::function<void(XML::Node &)> action;
    bool fired = false;
    explicit Added(XML::Node &r) : root(r) { root.addObserver(*this); }
    ~Added() override { root.removeObserver(*this); }
    void notifyChildAdded(XML::Node &, XML::Node &child, XML::Node *) override {
        if (fired || std::strcmp(child.name(), "svg:g")) return;
        fired = true; action(child);
    }
};
Cairo::RefPtr<Cairo::ImageSurface> render(SPDocument &doc, Geom::Point origin = {}, double zoom = 1)
{
    Drawing drawing;
    auto key = SPItem::display_key_new(1);
    auto root = doc.getRoot();
    auto shown = root->invoke_show(drawing, key, SP_ITEM_SHOW_DISPLAY);
    drawing.setRoot(shown);
    shown->setTransform(Geom::Translate(-origin) * Geom::Scale(zoom) * Geom::Translate(8.25, 9.375));
    drawing.update();
    auto surface = Cairo::ImageSurface::create(Cairo::Surface::Format::ARGB32, 192, 160);
    DrawingSurface target(surface->cobj(), Geom::IntPoint(0, 0));
    DrawingContext context(target);
    drawing.render(context, Geom::IntRect::from_xywh(0, 0, 192, 160));
    surface->flush(); root->invoke_hide(key);
    return surface;
}
std::size_t differences(Cairo::RefPtr<Cairo::ImageSurface> const &a,
                        Cairo::RefPtr<Cairo::ImageSurface> const &b)
{
    std::size_t result = 0;
    for (int y = 0; y < a->get_height(); ++y) for (int x = 0; x < a->get_width(); ++x)
        result += std::memcmp(a->get_data() + y*a->get_stride() + 4*x,
                              b->get_data() + y*b->get_stride() + 4*x, 4) != 0;
    return result;
}
void same_render(SPDocument &source, SPDocument &dest, Geom::Point origin)
{
    for (double zoom : {0.75, 1., 1.375}) {
        SCOPED_TRACE(zoom);
        EXPECT_EQ(differences(render(source, {}, zoom), render(dest, origin, zoom)), 0u);
    }
}
class ArtworkLibraryInsert : public ::testing::Test {
    void SetUp() override { if (!Application::exists()) Application::create(false); }
};
constexpr char art[] = "<path id='art' d='M8 8L48 8L48 48L8 48Z' fill='#cf2468'/>";
}

TEST_F(ArtworkLibraryInsert, RealTokenOneUndoExactRestorationAndNoSelectionChange)
{
    auto source_xml = svg(art); auto asset = token(source_xml);
    auto bytes_owner = asset.svg_bytes();
    auto doc = document(svg("<rect id='existing' x='150' width='8' height='8'/>"));
    ObjectSet selected(doc.get()); selected.add(doc->getObjectById("existing"));
    auto before_selection = selected.items_vector();
    auto before = snapshot(*doc);
    auto target = Library::capture_insertion_target(*doc, *doc->getRoot());
    History history(*doc);
    auto result = Library::insert_artwork(*doc, target, asset, {12, 13});
    EXPECT_EQ(history.commits, 1u);
    ASSERT_EQ(result.inserted_ids.size(), 1u);
    ASSERT_TRUE(group(*doc, result));
    EXPECT_EQ(selected.items_vector(), before_selection);
    EXPECT_EQ(*bytes_owner, source_xml);
    auto after = snapshot(*doc);
    EXPECT_TRUE(DocumentUndo::undo(doc.get()));
    EXPECT_EQ(snapshot(*doc), before);
    EXPECT_EQ(selected.items_vector(), before_selection);
    EXPECT_TRUE(DocumentUndo::redo(doc.get()));
    EXPECT_EQ(snapshot(*doc), after);
    EXPECT_EQ(history.commits, 1u);
}
TEST_F(ArtworkLibraryInsert, UngroupMoveRotateAndReopenPreserveArtworkScale)
{
    // LightBurn libraries use millimetre geometry in a physical SVG viewport.
    // Keep a genuine source group: one ungroup removes our insertion envelope,
    // another removes the SVG viewport, and a third exposes the artwork.
    auto xml = svg("<g><path d='M2 2L18 2L18 13L2 13Z' fill='#cf2468'/></g>",
                   "width='25.4mm' height='25.4mm' viewBox='0 0 25.4 25.4'");
    auto doc = document(svg(""));
    auto result = Library::insert_artwork(*doc,
        Library::capture_insertion_target(*doc, *doc->getRoot()), token(xml), {12, 13});
    ObjectSet selected(doc.get());
    selected.add(group(*doc, result));
    for (unsigned level = 0; level < 3; ++level) {
        SCOPED_TRACE(level);
        doc->ensureUpToDate();
        auto before = render(*doc);
        selected.ungroup();
        doc->ensureUpToDate();
        ASSERT_EQ(selected.size(), 1u);
        EXPECT_EQ(differences(before, render(*doc)), 0u);
        auto item = selected.singleItem();
        auto bounds = item->documentVisualBounds();
        ASSERT_TRUE(bounds);
        selected.moveRelative({7, 9});
        doc->ensureUpToDate();
        auto moved = item->documentVisualBounds();
        ASSERT_TRUE(moved);
        EXPECT_NEAR(moved->left(), bounds->left() + 7, 1e-7);
        EXPECT_NEAR(moved->top(), bounds->top() + 9, 1e-7);
        EXPECT_NEAR(moved->width(), bounds->width(), 1e-7);
        EXPECT_NEAR(moved->height(), bounds->height(), 1e-7);
        selected.rotateRelative(moved->midpoint(), 90);
        doc->ensureUpToDate();
        auto rotated = item->documentVisualBounds();
        ASSERT_TRUE(rotated);
        EXPECT_NEAR(rotated->width(), bounds->height(), 1e-7);
        EXPECT_NEAR(rotated->height(), bounds->width(), 1e-7);
        std::string selected_id = item->getId();
        DocumentUndo::done(doc.get(), Util::Internal::ContextString("Move and rotate"), "");
        auto after = render(*doc);
        auto reopened = document(sp_repr_save_buf(doc->getReprDoc()).raw());
        EXPECT_EQ(differences(after, render(*reopened)), 0u);
        ASSERT_TRUE(DocumentUndo::undo(doc.get()));
        ASSERT_TRUE(DocumentUndo::redo(doc.get()));
        doc->ensureUpToDate();
        EXPECT_EQ(differences(after, render(*doc)), 0u);
        selected.clear();
        selected.add(doc->getObjectById(selected_id.c_str()));
    }
}

TEST_F(ArtworkLibraryInsert, UngroupKeepsNonScalingLibraryOutlines)
{
    auto doc = document(svg("<g id='group' transform='scale(3.779527559)'>"
        "<path id='outline' d='M2 2L18 2L18 13L2 13Z' fill='none' "
        "stroke='black' stroke-width='0.1' vector-effect='non-scaling-stroke'/></g>"));
    auto before = render(*doc);
    ObjectSet selected(doc.get()); selected.add(doc->getObjectById("group"));
    selected.ungroup();
    doc->ensureUpToDate();
    auto outline = selected.singleItem(); ASSERT_TRUE(outline);
    EXPECT_DOUBLE_EQ(outline->style->stroke_width.computed, .1);
    EXPECT_EQ(differences(before, render(*doc)), 0u);
    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    doc->ensureUpToDate();
    EXPECT_EQ(differences(before, render(*doc)), 0u);
    ASSERT_TRUE(DocumentUndo::redo(doc.get()));
    doc->ensureUpToDate();
    EXPECT_EQ(differences(before, render(*doc)), 0u);
}

TEST_F(ArtworkLibraryInsert, PhysicalUnitsViewBoxAspectRatioAndTransformedParent)
{
    for (auto par : {"none", "xMidYMid meet", "xMidYMid slice"}) {
        SCOPED_TRACE(par);
        auto xml = svg("<rect x='-10' y='-20' width='48' height='96' fill='red'/>",
            "width='1in' height='72pt' viewBox='-10 -20 48 96'",
            std::string("preserveAspectRatio='") + par + "'");
        auto source = document(xml);
        auto dest = document(svg("<g id='parent' transform='translate(37,19) rotate(17) scale(1.3,.7)'/>",
                                 "width='400' height='200' viewBox='0 0 200 100'"));
        auto target = Library::capture_insertion_target(*dest, *dest->getObjectById("parent"));
        Geom::Point origin(20, 25);
        auto result = Library::insert_artwork(*dest, target, token(xml), origin);
        auto actual = group(*dest, result)->documentVisualBounds();
        auto expected = source->getRoot()->documentVisualBounds();
        ASSERT_TRUE(actual); ASSERT_TRUE(expected);
        EXPECT_NEAR(actual->left(), expected->left() + origin[0], 1e-8);
        EXPECT_NEAR(actual->top(), expected->top() + origin[1], 1e-8);
        EXPECT_NEAR(actual->width() * 25.4/96, expected->width() * 25.4/96, .01);
        EXPECT_NEAR(actual->height() * 25.4/96, expected->height() * 25.4/96, .01);
        same_render(*source, *dest, origin);
    }
}
TEST_F(ArtworkLibraryInsert, DestinationImportantCssCannotRestylePathsTextOrStops)
{
    auto xml = svg("<defs><linearGradient id='paint'><stop offset='0' stop-color='red'/>"
        "<stop offset='1' stop-color='blue'/></linearGradient></defs>"
        "<path class='incoming' id='art' d='M8 8L88 8L88 48L8 48Z' fill='url(#paint)'/>"
        "<text x='10' y='70' style='font-size:12px;font-family:sans-serif'>Editable</text>");
    auto source = document(xml);
    auto dest = document(svg("<style>path, text, tspan, stop, .incoming {fill:lime!important;"
        "stroke:black!important;stroke-width:8!important;opacity:.2!important;display:none!important;"
        "font-size:50px!important;stop-color:yellow!important} "
        "path {d:path(&quot;M0 0L1 1&quot;)!important}</style><g id='parent'/>"));
    auto style_before = std::string(find(dest->getReprRoot(), "svg:style")->firstChild()->content());
    auto result = Library::insert_artwork(*dest,
        Library::capture_insertion_target(*dest, *dest->getObjectById("parent")), token(xml), {5, 7});
    same_render(*source, *dest, {5, 7});
    EXPECT_EQ(find(dest->getReprRoot(), "svg:style")->firstChild()->content(), style_before);
    auto path = find(group(*dest, result)->getRepr(), "svg:path");
    ASSERT_TRUE(path); EXPECT_STREQ(path->attribute("d"), "M8 8L88 8L88 48L8 48Z");
    path->setAttribute("d", "M0 0L30 0L30 30Z"); dest->ensureUpToDate();
    EXPECT_GT(differences(render(*source), render(*dest, {5, 7})), 0u); // Positive oracle control.
}
TEST_F(ArtworkLibraryInsert, TwoInsertionsDoNotShareEvenEquivalentGradientResources)
{
    auto xml = svg("<defs><linearGradient id='paint'><stop offset='0' stop-color='red'/>"
        "<stop offset='1' stop-color='blue'/></linearGradient></defs>"
        "<rect id='art' width='40' height='40' fill='url(#paint)'/>");
    auto asset = token(xml);
    auto dest = document(svg("<defs><linearGradient id='paint'><stop offset='0' stop-color='red'/>"
        "<stop offset='1' stop-color='blue'/></linearGradient></defs>"));
    auto first = Library::insert_artwork(*dest, Library::capture_insertion_target(*dest, *dest->getRoot()), asset, {0, 0});
    auto second = Library::insert_artwork(*dest, Library::capture_insertion_target(*dest, *dest->getRoot()), asset, {90, 0});
    auto a = find(group(*dest, first)->getRepr(), "svg:linearGradient");
    auto b = find(group(*dest, second)->getRepr(), "svg:linearGradient");
    ASSERT_TRUE(a); ASSERT_TRUE(b);
    EXPECT_STRNE(a->attribute("id"), b->attribute("id"));
    EXPECT_STRNE(a->attribute("id"), "paint");
    auto second_before = std::string(); snapshot(group(*dest, second)->getRepr(), second_before);
    a->firstChild()->setAttribute("style", "stop-color:lime !important;stop-opacity:1 !important");
    dest->ensureUpToDate();
    auto second_after = std::string(); snapshot(group(*dest, second)->getRepr(), second_after);
    EXPECT_EQ(second_before, second_after);
    auto rect_a = cast<SPItem>(dest->getObjectByRepr(find(group(*dest, first)->getRepr(), "svg:rect")));
    auto rect_b = cast<SPItem>(dest->getObjectByRepr(find(group(*dest, second)->getRepr(), "svg:rect")));
    ASSERT_TRUE(rect_a); ASSERT_TRUE(rect_b);
    EXPECT_NE(rect_a->style->fill.href->getObject(), rect_b->style->fill.href->getObject());
}
TEST_F(ArtworkLibraryInsert, RootNestingAndMetadataSurviveNativeSaveReopen)
{
    auto xml = svg("<metadata xmlns:rdf='http://www.w3.org/1999/02/22-rdf-syntax-ns#' "
        "xmlns:dc='http://purl.org/dc/elements/1.1/'><rdf:RDF><rdf:Description><dc:title>Native asset</dc:title>"
        "</rdf:Description></rdf:RDF></metadata>" + std::string(art),
        "width='25.4mm' height='25.4mm' viewBox='0 0 96 96'",
        "inkscape:nesting-contour-version='1'");
    auto dest = document(svg(""));
    auto result = Library::insert_artwork(*dest, Library::capture_insertion_target(*dest, *dest->getRoot()), token(xml), {});
    auto inner = find(group(*dest, result)->getRepr(), "svg:svg");
    ASSERT_TRUE(inner); EXPECT_STREQ(inner->attribute("inkscape:nesting-contour-version"), "1");
    EXPECT_TRUE(find(inner, "svg:metadata"));
    auto saved = sp_repr_save_buf(dest->getReprDoc());
    auto reopened = document(saved.raw());
    auto restored = group(*reopened, result); ASSERT_TRUE(restored);
    EXPECT_TRUE(find(restored->getRepr(), "svg:metadata"));
    EXPECT_STREQ(find(restored->getRepr(), "svg:svg")->attribute("inkscape:nesting-contour-version"), "1");
    same_render(*dest, *reopened, {});
}
TEST_F(ArtworkLibraryInsert, TrustedSmallFilterClipMaskFixturePreservesNativeRender)
{
    // Small original fixture only; not an untrusted filter allocation qualification.
    auto xml = svg("<defs><filter id='f' x='-.2' y='-.2' width='1.4' height='1.4'>"
        "<feGaussianBlur stdDeviation='.5'/></filter>"
        "<clipPath id='c'><rect width='70' height='70'/></clipPath>"
        "<mask id='m'><rect width='96' height='96' fill='white'/></mask></defs>"
        "<g clip-path='url(#c)' mask='url(#m)'><rect x='10' y='10' width='50' height='50' "
        "fill='red' filter='url(#f)'/></g>");
    auto source = document(xml); auto dest = document(svg(""));
    Library::insert_artwork(*dest, Library::capture_insertion_target(*dest, *dest->getRoot()), token(xml), {3, 4});
    same_render(*source, *dest, {3, 4});
}
TEST_F(ArtworkLibraryInsert, HiddenLockedAndNonneutralAncestorsRefuseWithoutXmlChanges)
{
    for (auto attrs : {"style='display:none'", "style='opacity:.5'",
                      "xmlns:sodipodi='http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd' sodipodi:insensitive='true'"}) {
        auto dest = document(svg("<g " + std::string(attrs) + "><g id='parent'/></g>"));
        auto before = snapshot(*dest);
        EXPECT_THROW(Library::capture_insertion_target(*dest, *dest->getObjectById("parent")), Library::InsertionError);
        EXPECT_EQ(snapshot(*dest), before); EXPECT_FALSE(dest->isModifiedSinceSave());
    }
}
TEST_F(ArtworkLibraryInsert, StaleParentAndWrongDocumentCannotReuseTargetIdentity)
{
    auto dest = document(svg("<g id='parent'/>")); auto other = document(svg("<g id='parent'/>"));
    auto target = Library::capture_insertion_target(*dest, *dest->getObjectById("parent"));
    auto before = snapshot(*other);
    EXPECT_THROW(Library::insert_artwork(*other, target, token(svg(art)), {}), Library::InsertionError);
    EXPECT_EQ(snapshot(*other), before);
    dest->getObjectById("parent")->getRepr()->setAttribute("transform", "translate(1)");
    dest->ensureUpToDate(); auto changed = snapshot(*dest);
    EXPECT_THROW(Library::insert_artwork(*dest, target, token(svg(art)), {}), Library::InsertionError);
    EXPECT_EQ(snapshot(*dest), changed);
}
TEST_F(ArtworkLibraryInsert, PendingXmlRefusedAndItsLaterUndoRemainsIntact)
{
    auto dest = document(svg("<g id='parent'/>")); auto before = snapshot(*dest);
    dest->getObjectById("parent")->getRepr()->setAttribute("data-pending", "preserve");
    auto pending = snapshot(*dest);
    auto target = Library::capture_insertion_target(*dest, *dest->getRoot());
    History history(*dest);
    EXPECT_THROW(Library::insert_artwork(*dest, target, token(svg(art)), {}), Library::InsertionError);
    EXPECT_EQ(snapshot(*dest), pending); EXPECT_EQ(history.commits, 0u);
    DocumentUndo::done(dest.get(), Util::Internal::ContextString("Pending"), "");
    EXPECT_TRUE(DocumentUndo::undo(dest.get())); EXPECT_EQ(snapshot(*dest), before);
}
TEST_F(ArtworkLibraryInsert, RawOpenVersionStampDoesNotBlockFirstInsertion)
{
    for (auto version : {"", "inkscape:version='1.4'"}) {
        SCOPED_TRACE(version);
        auto xml = svg("<rect id='existing' width='5' height='5'/>",
                       "width='25.4mm' height='25.4mm' viewBox='0 0 96 96'", version);
        // Deliberately bypass document(): its Fixture done()/clearUndo() used
        // to consume the startup version stamp and hide the native-open failure.
        auto dest = SPDocument::createNewDocFromMem(xml); ASSERT_TRUE(dest);
        dest->ensureUpToDate();
        ASSERT_STREQ(dest->getReprRoot()->attribute("inkscape:version"), Inkscape::version_string);
        auto before = snapshot(*dest);
        auto dirty = dest->isModifiedSinceSave();
        History history(*dest);
        auto result = Library::insert_artwork(*dest,
            Library::capture_insertion_target(*dest, *dest->getRoot()), token(svg(art)), {3, 4});
        ASSERT_EQ(result.inserted_ids.size(), 1u);
        EXPECT_EQ(history.commits, 1u);
        EXPECT_TRUE(DocumentUndo::undo(dest.get()));
        EXPECT_EQ(snapshot(*dest), before);
        EXPECT_EQ(dest->isModifiedSinceSave(), dirty);
        EXPECT_FALSE(DocumentUndo::undo(dest.get())); // no hidden startup Undo
        EXPECT_TRUE(DocumentUndo::redo(dest.get()));
        EXPECT_TRUE(dest->getObjectById(result.inserted_ids.front()));
    }
}
TEST_F(ArtworkLibraryInsert, RawOpenStillRefusesPendingUserXml)
{
    auto dest = SPDocument::createNewDocFromMem(svg("<rect id='existing' width='5' height='5'/>"));
    ASSERT_TRUE(dest);
    dest->ensureUpToDate(); // no Fixture done()/clearUndo()
    auto existing = dest->getObjectById("existing")->getRepr();
    auto before = snapshot(*dest);
    History history(*dest);
    existing->setAttribute("data-pending", "preserve");
    auto pending = snapshot(*dest);
    try {
        Library::insert_artwork(*dest, Library::capture_insertion_target(*dest, *dest->getRoot()),
                                token(svg(art)), {});
        FAIL() << "Load metadata must not consume pending user XML";
    } catch (Library::InsertionError const &e) {
        EXPECT_EQ(e.failure(), Library::InsertionFailure::Busy);
    }
    EXPECT_EQ(snapshot(*dest), pending);
    EXPECT_EQ(history.commits, 0u); EXPECT_EQ(history.clears, 0u);
    // The metadata persists, while the user's pending edit still owns one Undo.
    DocumentUndo::done(dest.get(), Util::Internal::ContextString("Pending user edit"), "");
    ASSERT_TRUE(DocumentUndo::undo(dest.get()));
    EXPECT_EQ(snapshot(*dest), before);
    EXPECT_TRUE(DocumentUndo::redo(dest.get()));
    EXPECT_EQ(snapshot(*dest), pending);
}
TEST_F(ArtworkLibraryInsert, EarlyCancellationPreservesXmlDirtySelectionAndRedo)
{
    auto dest = document(svg("<rect id='existing' width='5' height='5'/>"));
    dest->getObjectById("existing")->getRepr()->setAttribute("width", "9");
    DocumentUndo::done(dest.get(), Util::Internal::ContextString("Existing"), "");
    EXPECT_TRUE(DocumentUndo::undo(dest.get()));
    auto before = snapshot(*dest); auto dirty = dest->isModifiedSinceSave();
    ObjectSet selection(dest.get()); selection.add(dest->getObjectById("existing"));
    auto selected = selection.items_vector();
    History history(*dest);
    EXPECT_THROW(Library::insert_artwork(*dest, Library::capture_insertion_target(*dest, *dest->getRoot()),
                                        token(svg(art)), {}, {}, [] { return true; }), Library::InsertionError);
    EXPECT_EQ(snapshot(*dest), before); EXPECT_EQ(dest->isModifiedSinceSave(), dirty);
    EXPECT_EQ(selection.items_vector(), selected); EXPECT_EQ(history.commits, 0u);
    EXPECT_TRUE(DocumentUndo::redo(dest.get()));
    EXPECT_STREQ(dest->getObjectById("existing")->getRepr()->attribute("width"), "9");
}
TEST_F(ArtworkLibraryInsert, NativeObserverCancellationRollsBackImportedXml)
{
    auto dest = document(svg("")); auto before = snapshot(*dest); bool cancel = false;
    Added observer(*dest->getReprRoot()); observer.action = [&](auto &) { cancel = true; };
    History history(*dest);
    EXPECT_THROW(Library::insert_artwork(*dest, Library::capture_insertion_target(*dest, *dest->getRoot()),
                                        token(svg(art)), {}, {}, [&] { return cancel; }), Library::InsertionError);
    EXPECT_TRUE(observer.fired); EXPECT_EQ(snapshot(*dest), before);
    EXPECT_FALSE(dest->isModifiedSinceSave()); EXPECT_EQ(history.commits, 0u);
}
TEST_F(ArtworkLibraryInsert, ForeignDoneDuringImportCannotPublishPartialInsertion)
{
    auto dest = document(svg("")); auto before = snapshot(*dest);
    Added observer(*dest->getReprRoot());
    observer.action = [&](auto &) { DocumentUndo::done(dest.get(), Util::Internal::ContextString("Foreign"), ""); };
    History history(*dest);
    EXPECT_THROW(Library::insert_artwork(*dest, Library::capture_insertion_target(*dest, *dest->getRoot()),
                                        token(svg(art)), {}), Library::InsertionError);
    EXPECT_TRUE(observer.fired); EXPECT_EQ(snapshot(*dest), before);
    EXPECT_FALSE(dest->isModifiedSinceSave()); EXPECT_EQ(history.commits, 0u);
}
TEST_F(ArtworkLibraryInsert, DetachAndForcedGcDuringNativeImportRollBackExactly)
{
    auto dest = document(svg("")); auto before = snapshot(*dest);
    Added observer(*dest->getReprRoot());
    observer.action = [&](auto &child) {
        dest->getReprRoot()->removeChild(&child);
        GC::Core::gcollect();
    };
    EXPECT_THROW(Library::insert_artwork(*dest, Library::capture_insertion_target(*dest, *dest->getRoot()),
                                        token(svg(art)), {}), Library::InsertionError);
    EXPECT_TRUE(observer.fired); EXPECT_EQ(snapshot(*dest), before);
}
TEST_F(ArtworkLibraryInsert, CloseBeforeCommitRetainsOwnerUntilExactRollback)
{
    auto dest = document(svg("")); auto before = snapshot(*dest);
    auto closed = std::make_shared<bool>(false);
    auto connection = dest->connectBeforeCommit([&] {
        EXPECT_TRUE(DocumentUndo::deferUntilInteractionQuiescent(dest.get(),
            [closed](SPDocument &) { *closed = true; }, true));
    });
    History history(*dest);
    EXPECT_THROW(Library::insert_artwork(*dest, Library::capture_insertion_target(*dest, *dest->getRoot()),
                                        token(svg(art)), {}), Library::InsertionError);
    EXPECT_EQ(snapshot(*dest), before); EXPECT_FALSE(*closed); EXPECT_EQ(history.commits, 0u);
    EXPECT_FALSE(dest->isModifiedSinceSave());
    connection.disconnect(); // Owner is destroyed only after inspection, without dispatching idle.
}
TEST_F(ArtworkLibraryInsert, CloseAfterPublicationIsCommittedNotRetryableFailure)
{
    auto dest = document(svg("")); auto closed = std::make_shared<bool>(false);
    auto connection = dest->connectCommit([&] {
        EXPECT_TRUE(DocumentUndo::deferUntilInteractionQuiescent(dest.get(),
            [closed](SPDocument &) { *closed = true; }, true));
    });
    History history(*dest);
    auto result = Library::insert_artwork(*dest, Library::capture_insertion_target(*dest, *dest->getRoot()),
                                         token(svg(art)), {});
    EXPECT_EQ(history.commits, 1u); EXPECT_TRUE(group(*dest, result)); EXPECT_FALSE(*closed);
    connection.disconnect();
}
TEST_F(ArtworkLibraryInsert, BeforeCommitMutationTriggersRollbackWithoutHistory)
{
    auto dest = document(svg("")); auto before = snapshot(*dest);
    auto connection = dest->connectBeforeCommit([&] { dest->getReprRoot()->setAttribute("width", "200"); });
    History history(*dest);
    EXPECT_THROW(Library::insert_artwork(*dest, Library::capture_insertion_target(*dest, *dest->getRoot()),
                                        token(svg(art)), {}), Library::InsertionError);
    EXPECT_EQ(snapshot(*dest), before); EXPECT_EQ(history.commits, 0u);
    connection.disconnect();
}
TEST_F(ArtworkLibraryInsert, LoweredMaterializationBudgetRefusesBeforeDestinationMutation)
{
    auto dest = document(svg("")); auto before = snapshot(*dest);
    Library::InsertionLimits limits; limits.materialized_style_bytes = 1;
    EXPECT_THROW(Library::insert_artwork(*dest, Library::capture_insertion_target(*dest, *dest->getRoot()),
                                        token(svg(art)), {}, limits), Library::InsertionError);
    EXPECT_EQ(snapshot(*dest), before);
}
TEST_F(ArtworkLibraryInsert, OriginalLiveInteractionStillAllowsExternalDoneToRetireIt)
{
    auto dest = document(svg(""));
    auto live = DocumentUndo::beginRollbackableInteraction(dest.get()); ASSERT_TRUE(live);
    dest->getReprRoot()->setAttribute("data-live", "keep");
    DocumentUndo::done(dest.get(), Util::Internal::ContextString("Existing behavior"), "");
    EXPECT_FALSE(live->active());
    live->rollback();
    EXPECT_STREQ(dest->getReprRoot()->attribute("data-live"), "keep");
    EXPECT_TRUE(DocumentUndo::undo(dest.get()));
    EXPECT_EQ(dest->getReprRoot()->attribute("data-live"), nullptr);
}

TEST_F(ArtworkLibraryInsert, EmbeddedAlphaPngRemainsSelfContainedAndEditable)
{
    png_image image{}; image.version = PNG_IMAGE_VERSION;
    image.width = 1; image.height = 1; image.format = PNG_FORMAT_RGBA;
    unsigned char pixel[] = {255, 0, 128, 127};
    png_alloc_size_t size = 0;
    ASSERT_TRUE(png_image_write_to_memory(&image, nullptr, &size, 0, pixel, 0, nullptr));
    Library::Bytes data(size);
    ASSERT_TRUE(png_image_write_to_memory(&image, data.data(), &size, 0, pixel, 0, nullptr));
    data.resize(size); png_image_free(&image);
    auto encoded = g_base64_encode(data.data(), data.size());
    std::string uri = std::string("data:image/png;base64,") + encoded; g_free(encoded);
    auto xml = svg("<image width='40' height='40' href='" + uri + "'/>");
    auto source = document(xml); auto dest = document(svg(""));
    auto asset = token(xml);
    auto outcome = Library::insert_artwork(*dest,
        Library::capture_insertion_target(*dest, *dest->getRoot()), asset, {4, 5});
    auto raster = find(group(*dest, outcome)->getRepr(), "svg:image");
    ASSERT_TRUE(raster);
    auto href = raster->attribute("href");
    if (!href) href = raster->attribute("xlink:href");
    ASSERT_TRUE(href); EXPECT_EQ(std::string(href), uri);
    same_render(*source, *dest, {4, 5});
}
TEST_F(ArtworkLibraryInsert, TargetCanOutliveOriginalDocumentButNotBindToReplacement)
{
    auto original = document(svg("<g id='parent'/>"));
    auto target = Library::capture_insertion_target(*original, *original->getObjectById("parent"));
    original.reset();
    GC::Core::gcollect();
    auto replacement = document(svg("<g id='parent'/>"));
    auto before = snapshot(*replacement);
    EXPECT_THROW(Library::insert_artwork(*replacement, target, token(svg(art)), {}), Library::InsertionError);
    EXPECT_EQ(snapshot(*replacement), before);
}

namespace {
std::unique_ptr<SPDocument> framed_document(double width = 256)
{
    auto doc = document(svg("<text id='art' x='5' y='20' style='font-family:sans-serif;font-size:10px'>"
                            "Independent generated text frames retain editable text in two columns.</text>"));
    auto text = cast<SPText>(doc->getObjectById("art"));
    if (!text) throw std::runtime_error("Native text fixture missing");
    UI::TextFrameSettings frame;
    // Positive fixture: 124-unit columns, then 80 units after a three-column edit.
    // The old total width 80 (36-unit columns) is retained in the overset test.
    frame.width = width; frame.height = 60; frame.columns = 2; frame.gap = 8;
    if (!UI::setTextFrameSettings(*doc, {text}, frame)) throw std::runtime_error("Native frame writer failed");
    doc->ensureUpToDate();
    DocumentUndo::done(doc.get(), Util::Internal::ContextString("Frame fixture"), "");
    DocumentUndo::clearUndo(doc.get()); DocumentUndo::clearRedo(doc.get());
    doc->setModifiedSinceSave(false);
    return doc;
}
Library::ValidatedSvg staged_frame_token(SPDocument &doc)
{
    ObjectSet selection(&doc); selection.add(doc.getObjectById("art"));
    auto staged = Library::stage_selection(selection);
    return Library::preflight_svg(staged.svg, {"b203e8e9-640c-4195-b0ea-f9b790245678",
        Library::artwork_sha256(staged.svg), staged.width_mm, staged.height_mm});
}
std::vector<std::shared_ptr<XML::Node>> frame_nodes(SPText &text)
{
    std::vector<std::shared_ptr<XML::Node>> result;
    for (auto *href : text.style->shape_inside.hrefs) {
        auto object = href->getObject();
        if (!object) throw std::runtime_error("Missing real native frame dependency");
        auto node = object->getRepr(); GC::anchor(node);
        result.emplace_back(node, [](auto *p) { GC::release(p); });
    }
    return result;
}
std::string node_snapshot(XML::Node *node) { std::string out; snapshot(node, out); return out; }

double independent_word_advance()
{
    auto doc = document(svg("<text id='word' style='font-family:sans-serif;font-size:10px'>Independent</text>"));
    auto text = cast<SPText>(doc->getObjectById("word"));
    if (!text || !text->layout.outputExists() || !text->layout.bounds(Geom::Affine())) {
        throw std::runtime_error("Independent word measurement has no native glyphs");
    }
    return text->layout.characterAnchorPoint(text->layout.end())[Geom::X] -
           text->layout.characterAnchorPoint(text->layout.begin())[Geom::X];
}
}

TEST_F(ArtworkLibraryInsert, V3GeneratedFrameCollisionsEditOnlyTheOwningCopyAndUndoExactly)
{
    auto source = framed_document(); auto source_before = snapshot(*source);
    auto const word_advance = independent_word_advance();
    RecordProperty("independent_first_word_advance", std::to_string(word_advance));
    ASSERT_GT(word_advance, 0.);
    auto source_text = cast<SPText>(source->getObjectById("art")); ASSERT_TRUE(source_text);
    auto source_settings = UI::textFrameSettings(*source_text);
    ASSERT_LT(word_advance, (source_settings.width - source_settings.gap) / 2);
    ASSERT_TRUE(source_text->layout.bounds(source_text->i2doc_affine()));
    auto asset = staged_frame_token(*source); ASSERT_EQ(asset.policy_version(), 3u);
    auto asset_before = *asset.svg_bytes();
    auto dest = framed_document(); auto dest_before = snapshot(*dest);
    auto existing = cast<SPText>(dest->getObjectById("art")); ASSERT_TRUE(existing);
    auto existing_text = node_snapshot(existing->getRepr());
    auto existing_frames = frame_nodes(*existing); ASSERT_EQ(existing_frames.size(), 2u);
    std::vector<std::string> existing_xml;
    for (auto const &n : existing_frames) existing_xml.push_back(node_snapshot(n.get()));
    History history(*dest);
    auto first = Library::insert_artwork(*dest, Library::capture_insertion_target(*dest, *dest->getRoot()), asset, {10, 12});
    auto first_state = snapshot(*dest);
    auto second = Library::insert_artwork(*dest, Library::capture_insertion_target(*dest, *dest->getRoot()), asset, {100, 12});
    auto inserted_state = snapshot(*dest); EXPECT_EQ(history.commits, 2u);
    auto a = cast<SPText>(dest->getObjectByRepr(find(group(*dest, first)->getRepr(), "svg:text")));
    auto b = cast<SPText>(dest->getObjectByRepr(find(group(*dest, second)->getRepr(), "svg:text")));
    ASSERT_TRUE(a); ASSERT_TRUE(b);
    std::string aid = a->getId(), bid = b->getId();
    EXPECT_NE(aid, "art"); EXPECT_NE(bid, "art"); EXPECT_NE(aid, bid);
    auto old_a = frame_nodes(*a), old_b = frame_nodes(*b);
    ASSERT_EQ(old_a.size(), 2u); ASSERT_EQ(old_b.size(), 2u);
    for (auto const &n : old_a) EXPECT_STREQ(n->attribute("inkscape:text-frame-owner"), aid.c_str());
    for (auto const &n : old_b) EXPECT_STREQ(n->attribute("inkscape:text-frame-owner"), bid.c_str());
    auto b_before = node_snapshot(group(*dest, second)->getRepr());
    auto settings = UI::textFrameSettings(*a); settings.columns = 3;
    ASSERT_LT(word_advance, (settings.width - 2 * settings.gap) / 3);
    ASSERT_TRUE(UI::setTextFrameSettings(*dest, {a}, settings)); dest->ensureUpToDate();
    ASSERT_TRUE(a->layout.bounds(a->i2doc_affine()));
    DocumentUndo::done(dest.get(), Util::Internal::ContextString("Edit inserted frame"), "");
    EXPECT_EQ(history.commits, 3u);
    for (auto const &n : old_a) EXPECT_EQ(n->parent(), nullptr); // Old XML, even if an ID was reused.
    EXPECT_EQ(frame_nodes(*a).size(), 3u);
    EXPECT_EQ(node_snapshot(group(*dest, second)->getRepr()), b_before);
    EXPECT_EQ(node_snapshot(existing->getRepr()), existing_text);
    for (std::size_t i = 0; i < existing_frames.size(); ++i) {
        EXPECT_TRUE(existing_frames[i]->parent());
        EXPECT_EQ(node_snapshot(existing_frames[i].get()), existing_xml[i]);
    }
    auto edited_state = snapshot(*dest);
    EXPECT_TRUE(DocumentUndo::undo(dest.get())); EXPECT_EQ(snapshot(*dest), inserted_state);
    EXPECT_TRUE(DocumentUndo::redo(dest.get())); EXPECT_EQ(snapshot(*dest), edited_state);
    // Edit the other copy with the actual native editor, not a metadata mock.
    b = cast<SPText>(dest->getObjectById(bid.c_str())); ASSERT_TRUE(b);
    a = cast<SPText>(dest->getObjectById(aid.c_str())); ASSERT_TRUE(a);
    auto edited_a_frames = frame_nodes(*a);
    std::vector<std::string> edited_a_xml;
    for (auto const &n : edited_a_frames) edited_a_xml.push_back(node_snapshot(n.get()));
    auto a_after = node_snapshot(group(*dest, first)->getRepr());
    auto settings_b = UI::textFrameSettings(*b); settings_b.columns = 1;
    ASSERT_LT(word_advance, settings_b.width);
    ASSERT_TRUE(UI::setTextFrameSettings(*dest, {b}, settings_b)); dest->ensureUpToDate();
    ASSERT_TRUE(b->layout.bounds(b->i2doc_affine()));
    DocumentUndo::done(dest.get(), Util::Internal::ContextString("Edit second frame"), "");
    for (auto const &n : old_b) EXPECT_EQ(n->parent(), nullptr);
    EXPECT_EQ(frame_nodes(*b).size(), 1u);
    EXPECT_EQ(node_snapshot(group(*dest, first)->getRepr()), a_after);
    for (std::size_t i = 0; i < edited_a_frames.size(); ++i) {
        EXPECT_TRUE(edited_a_frames[i]->parent());
        EXPECT_EQ(node_snapshot(edited_a_frames[i].get()), edited_a_xml[i]);
    }
    EXPECT_EQ(node_snapshot(existing->getRepr()), existing_text);
    for (std::size_t i = 0; i < existing_frames.size(); ++i)
        EXPECT_EQ(node_snapshot(existing_frames[i].get()), existing_xml[i]);
    EXPECT_TRUE(DocumentUndo::undo(dest.get())); EXPECT_EQ(snapshot(*dest), edited_state);
    EXPECT_TRUE(DocumentUndo::undo(dest.get())); EXPECT_EQ(snapshot(*dest), inserted_state);
    EXPECT_TRUE(DocumentUndo::undo(dest.get())); EXPECT_EQ(snapshot(*dest), first_state);
    EXPECT_TRUE(DocumentUndo::undo(dest.get())); EXPECT_EQ(snapshot(*dest), dest_before);
    EXPECT_EQ(snapshot(*source), source_before); EXPECT_EQ(*asset.svg_bytes(), asset_before);
}

TEST_F(ArtworkLibraryInsert, NarrowIndependentFramesRefuseOversetStagingWithoutXmlChanges)
{
    auto const word_advance = independent_word_advance();
    RecordProperty("independent_first_word_advance", std::to_string(word_advance));
    ASSERT_GT(word_advance, 36.);
    auto source = framed_document(80); // Original failing positive geometry.
    auto text = cast<SPText>(source->getObjectById("art")); ASSERT_TRUE(text);
    auto frames = frame_nodes(*text); ASSERT_EQ(frames.size(), 2u);
    for (auto const &node : frames) {
        ASSERT_TRUE(node->parent());
        EXPECT_STREQ(node->name(), "svg:rect");
        EXPECT_STREQ(node->parent()->name(), "svg:defs");
        EXPECT_STREQ(node->attribute("inkscape:text-frame-owner"), "art");
        EXPECT_DOUBLE_EQ(node->getAttributeDouble("width", -1), 36.);
        EXPECT_DOUBLE_EQ(node->getAttributeDouble("height", -1), 60.);
        auto object = source->getObjectByRepr(node.get()); ASSERT_TRUE(object);
        EXPECT_EQ(object->document, source.get());
    }
    EXPECT_NE(frames[0].get(), frames[1].get());
    EXPECT_FALSE(text->layout.bounds(text->i2doc_affine()));
    ObjectSet selected(source.get()); selected.add(text);
    EXPECT_FALSE(selected.documentBounds(SPItem::VISUAL_BBOX));
    auto before = snapshot(*source);
    bool const dirty = source->isModifiedSinceSave();
    try {
        (void)staged_frame_token(*source);
        ADD_FAILURE() << "Fully overset text must not create an insertable staged asset";
    } catch (std::runtime_error const &error) {
        EXPECT_STREQ(error.what(), "Selection has no finite positive physical extent");
    }
    EXPECT_EQ(snapshot(*source), before);
    EXPECT_EQ(source->isModifiedSinceSave(), dirty);
}

TEST_F(ArtworkLibraryInsert, V3MissingOrWrongFrameOwnerRefusesBeforeDestinationMutation)
{
    for (auto owner : {"destination-only", "frame", "ordinary"}) {
        auto xml = svg("<defs><rect id='frame' width='30' height='20' inkscape:text-frame-owner='" +
            std::string(owner) + "'/></defs><text id='ordinary' x='5' y='15'>Not generated</text>");
        auto asset = token(xml); // The real v3 factory admits spelling, not ownership.
        auto dest = framed_document(); auto before = snapshot(*dest);
        History history(*dest);
        EXPECT_THROW(Library::insert_artwork(*dest, Library::capture_insertion_target(*dest, *dest->getRoot()),
                                            asset, {}), Library::InsertionError);
        EXPECT_EQ(snapshot(*dest), before); EXPECT_EQ(history.commits, 0u);
        EXPECT_FALSE(dest->isModifiedSinceSave());
    }
}

TEST_F(ArtworkLibraryInsert, NativeFrameEditPreservesUnreferencedNamesakesAndCollidingNewIds)
{
    auto doc = framed_document(); auto text = cast<SPText>(doc->getObjectById("art")); ASSERT_TRUE(text);
    auto extra = doc->getReprDoc()->createElement("svg:rect");
    extra->setAttribute("id", "art-text-frame-3");
    extra->setAttribute("inkscape:text-frame-owner", "art");
    extra->setAttribute("width", "9"); extra->setAttribute("height", "9");
    doc->getObjectById("art-text-frame-1")->getRepr()->parent()->appendChild(extra);
    auto extra_before = node_snapshot(extra);
    auto old = frame_nodes(*text);
    auto frame = UI::textFrameSettings(*text); frame.columns = 3;
    ASSERT_TRUE(UI::setTextFrameSettings(*doc, {text}, frame)); doc->ensureUpToDate();
    EXPECT_TRUE(extra->parent()); EXPECT_EQ(node_snapshot(extra), extra_before);
    for (auto const &n : old) EXPECT_EQ(n->parent(), nullptr);
    auto current = frame_nodes(*text); ASSERT_EQ(current.size(), 3u);
    for (auto const &n : current) {
        EXPECT_NE(n.get(), extra);
        EXPECT_STREQ(n->attribute("inkscape:text-frame-owner"), "art");
    }
    GC::release(extra);
}

TEST_F(ArtworkLibraryInsert, NativeRenameRemapsOnlyExactPlainFrameOwners)
{
    auto doc = framed_document(); auto text = cast<SPText>(doc->getObjectById("art")); ASSERT_TRUE(text);
    auto frames = frame_nodes(*text);
    auto unrelated = doc->getReprDoc()->createElement("svg:rect");
    unrelated->setAttribute("id", "unrelated");
    unrelated->setAttribute("inkscape:text-frame-owner", "art-long");
    frames.front()->parent()->appendChild(unrelated);
    rename_id(text, "renamed");
    for (auto const &n : frames) EXPECT_STREQ(n->attribute("inkscape:text-frame-owner"), "renamed");
    EXPECT_STREQ(unrelated->attribute("inkscape:text-frame-owner"), "art-long");
    GC::release(unrelated);
}


TEST_F(ArtworkLibraryInsert, NativeClashPolicyPreservesDefaultAndForcesEquivalentResourceRefs)
{
    auto xml = svg("<defs><linearGradient id='paint'><stop offset='0' stop-color='red'/>"
        "<stop offset='1' stop-color='blue'/></linearGradient>"
        "<linearGradient id='derived' xlink:href='#paint'/></defs>"
        "<rect id='shape' width='40' height='40' style='fill:url(#derived) !important;opacity:.7 !important'/>");
    auto destination = document(xml);
    auto legacy = document(xml);
    auto forced = document(xml);
    auto destination_before = snapshot(*destination);
    auto legacy_paint = cast<SPGradient>(legacy->getObjectById("paint"));
    auto forced_paint = cast<SPGradient>(forced->getObjectById("paint"));
    auto target_paint = cast<SPGradient>(destination->getObjectById("paint"));
    ASSERT_TRUE(legacy_paint); ASSERT_TRUE(forced_paint); ASSERT_TRUE(target_paint);
    ASSERT_TRUE(legacy_paint->isEquivalent(target_paint)); // Actual native exemption witness.
    ASSERT_TRUE(forced_paint->isEquivalent(target_paint));
    prevent_id_clashes(legacy.get(), destination.get()); // Default signature unchanged.
    EXPECT_STREQ(legacy_paint->getId(), "paint");

    auto derived = cast<SPGradient>(forced->getObjectById("derived"));
    auto shape = cast<SPItem>(forced->getObjectById("shape"));
    ASSERT_TRUE(derived); ASSERT_TRUE(shape);
    prevent_id_clashes(forced.get(), destination.get(), IdClashPolicy::RenameAllCollisions);
    forced->ensureUpToDate();
    EXPECT_STRNE(forced_paint->getId(), "paint");
    EXPECT_STRNE(derived->getId(), "derived");
    ASSERT_TRUE(derived->ref);
    EXPECT_EQ(derived->ref->getObject(), forced_paint);
    ASSERT_TRUE(shape->style->fill.href);
    EXPECT_EQ(shape->style->fill.href->getObject(), derived);
    EXPECT_TRUE(shape->style->fill.important);
    EXPECT_TRUE(shape->style->opacity.important);
    EXPECT_EQ(snapshot(*destination), destination_before);
}

TEST_F(ArtworkLibraryInsert, IndependentReferencesAndPrioritiesSurviveTargetCssAndUndo)
{
    std::string defs = "<defs><linearGradient id='paint'><stop offset='0' stop-color='red'/>"
        "<stop offset='1' stop-color='blue'/></linearGradient>"
        "<linearGradient id='derived' xlink:href='#paint'/>"
        "<clipPath id='sourceClip'><rect width='44' height='44'/></clipPath></defs>";
    auto xml = svg(defs + "<rect id='shape' x='8' y='8' width='48' height='48' "
                   "clip-path='url(#sourceClip)' fill='url(#derived)'/>");
    auto source = document(xml);
    auto dest = document(svg(defs + "<style>rect{fill:lime!important;opacity:.2!important}</style>"));
    auto target_before = std::string();
    snapshot(dest->getObjectById("paint")->getRepr(), target_before);
    auto before = snapshot(*dest);
    History history(*dest);
    auto first = Library::insert_artwork(*dest, Library::capture_insertion_target(*dest, *dest->getRoot()),
                                        token(xml), {});
    same_render(*source, *dest, {}); // Positive strict render, not successful refusal.
    auto after_first = snapshot(*dest);
    auto second = Library::insert_artwork(*dest, Library::capture_insertion_target(*dest, *dest->getRoot()),
                                         token(xml), {240, 0});
    auto after_second = snapshot(*dest);
    // The first rect is the clipPath rect; find the visible shape by traversal.
    auto visible = [&](Library::InsertedArtwork const &result) {
        SPItem *out = nullptr;
        std::function<void(XML::Node *)> visit = [&](auto *n) {
            if (auto item = cast<SPItem>(dest->getObjectByRepr(n)); item && item->getClipObject()) out = item;
            for (auto *c = n->firstChild(); c; c = c->next()) visit(c);
        };
        visit(group(*dest, result)->getRepr()); return out;
    };
    auto a = visible(first);
    auto b = visible(second);
    ASSERT_TRUE(a); ASSERT_TRUE(b);
    ASSERT_TRUE(a->style->fill.href); ASSERT_TRUE(b->style->fill.href);
    auto da = cast<SPGradient>(a->style->fill.href->getObject());
    auto db = cast<SPGradient>(b->style->fill.href->getObject());
    ASSERT_TRUE(da); ASSERT_TRUE(db); ASSERT_TRUE(da->ref); ASSERT_TRUE(db->ref);
    EXPECT_NE(da, db); EXPECT_NE(da->ref->getObject(), db->ref->getObject());
    EXPECT_NE(da->ref->getObject(), dest->getObjectById("paint"));
    EXPECT_NE(db->ref->getObject(), dest->getObjectById("paint"));
    EXPECT_NE(a->getClipObject(), b->getClipObject());
    EXPECT_NE(a->getClipObject(), dest->getObjectById("sourceClip"));
    EXPECT_TRUE(a->style->fill.important); EXPECT_TRUE(b->style->fill.important);
    a->style->readFromObject(a); b->style->readFromObject(b); dest->ensureUpToDate();
    EXPECT_EQ(a->style->fill.href->getObject(), da);
    EXPECT_EQ(b->style->fill.href->getObject(), db);
    auto target_after = std::string();
    snapshot(dest->getObjectById("paint")->getRepr(), target_after);
    EXPECT_EQ(target_after, target_before);
    EXPECT_EQ(history.commits, 2u);
    EXPECT_TRUE(DocumentUndo::undo(dest.get())); EXPECT_EQ(snapshot(*dest), after_first);
    EXPECT_TRUE(DocumentUndo::undo(dest.get())); EXPECT_EQ(snapshot(*dest), before);
    EXPECT_TRUE(DocumentUndo::redo(dest.get()));
    EXPECT_TRUE(DocumentUndo::redo(dest.get())); EXPECT_EQ(snapshot(*dest), after_second);
}

namespace {
void wrapper_compositing_case(bool mask)
{
    auto id = mask ? "targetMask" : "targetClip";
    auto property = mask ? "mask" : "clip-path";
    auto defs = mask ?
        "<defs><mask id='targetMask' maskUnits='userSpaceOnUse' x='0' y='0' width='96' height='96'>"
        "<rect width='14' height='96' fill='white'/></mask></defs>" :
        "<defs><clipPath id='targetClip'><rect width='14' height='96'/></clipPath></defs>";
    auto xml = svg(std::string("<g>") + art + "</g>");
    auto source = document(xml);
    for (auto importance : {"", "!important"}) {
        SCOPED_TRACE(property);
        SCOPED_TRACE(importance);
        auto css = std::string("<style>g{") + property + ":url(#" + id + ")" + importance + "}</style>";
        // Existing unshielded native behavior is a positive control: this rule
        // must attach the target resource and materially change actual pixels.
        auto control = document(svg(std::string(defs) + css + "<g id='control'>" + art + "</g>"));
        auto control_group = cast<SPItem>(control->getObjectById("control"));
        ASSERT_TRUE(control_group);
        if (mask) ASSERT_TRUE(control_group->getMaskObject());
        else ASSERT_TRUE(control_group->getClipObject());
        EXPECT_GT(differences(render(*source), render(*control)), 0u);

        auto dest = document(svg(std::string(defs) + css));
        auto before = snapshot(*dest);
        History history(*dest);
        auto result = Library::insert_artwork(*dest,
            Library::capture_insertion_target(*dest, *dest->getRoot()), token(xml), {});
        auto wrapper = group(*dest, result);
        ASSERT_TRUE(wrapper);
        EXPECT_FALSE(wrapper->getClipObject()); EXPECT_FALSE(wrapper->getMaskObject());
        EXPECT_STREQ(wrapper->getRepr()->attribute("clip-path"), "none");
        EXPECT_STREQ(wrapper->getRepr()->attribute("mask"), "none");
        same_render(*source, *dest, {});
        auto after = snapshot(*dest);
        // A native style rebuild must not reattach the destination resource.
        wrapper->style->readFromObject(wrapper);
        dest->ensureUpToDate();
        EXPECT_FALSE(wrapper->getClipObject()); EXPECT_FALSE(wrapper->getMaskObject());
        same_render(*source, *dest, {});
        EXPECT_EQ(snapshot(*dest), after);
        EXPECT_EQ(history.commits, 1u);
        EXPECT_TRUE(DocumentUndo::undo(dest.get())); EXPECT_EQ(snapshot(*dest), before);
        EXPECT_TRUE(DocumentUndo::redo(dest.get())); EXPECT_EQ(snapshot(*dest), after);
        same_render(*source, *dest, {});
    }
}
}
TEST_F(ArtworkLibraryInsert, DestinationCssClipCannotCompositeIndependentWrapper)
{
    wrapper_compositing_case(false);
}
TEST_F(ArtworkLibraryInsert, DestinationCssMaskCannotCompositeIndependentWrapper)
{
    wrapper_compositing_case(true);
}

TEST_F(ArtworkLibraryInsert, AtomicRetiredMutationIsAfterPublicationAndHasSeparateUndo)
{
    auto dest = document(svg(""));
    auto before = snapshot(*dest);
    History history(*dest);
    auto atomic = DocumentUndo::beginAtomicInteraction(dest.get()); ASSERT_TRUE(atomic);
    dest->getReprRoot()->setAttribute("data-atomic", "published");
    unsigned retired = 0, ready_calls = 0, commits_seen = 0;
    auto committed = dest->connectCommit([&] {
        ++commits_seen;
        EXPECT_EQ(retired, 0u);
        EXPECT_EQ(dest->getReprRoot()->attribute("data-retired"), nullptr);
    });
    auto connection = atomic->connectRetired([&] {
        ++retired;
        EXPECT_EQ(history.commits, 1u); EXPECT_EQ(commits_seen, 1u);
        EXPECT_FALSE(atomic->active());
        EXPECT_TRUE(dest->getReprDoc()->inTransaction());
        dest->getReprRoot()->setAttribute("data-retired", "subsequent");
    });
    EXPECT_TRUE(atomic->commitAtomically(Util::Internal::ContextString("Atomic publish"), "", [&] {
        ++ready_calls;
        EXPECT_EQ(retired, 0u); EXPECT_EQ(history.commits, 0u);
        return dest->getReprRoot()->attribute("data-retired") == nullptr;
    }));
    EXPECT_EQ(retired, 1u); EXPECT_EQ(ready_calls, 1u);
    connection.disconnect(); committed.disconnect(); atomic.reset();
    DocumentUndo::done(dest.get(), Util::Internal::ContextString("Later observer edit"), "");
    EXPECT_EQ(history.commits, 2u);
    auto after = snapshot(*dest);
    EXPECT_TRUE(DocumentUndo::undo(dest.get()));
    EXPECT_EQ(dest->getReprRoot()->attribute("data-retired"), nullptr);
    EXPECT_STREQ(dest->getReprRoot()->attribute("data-atomic"), "published");
    EXPECT_TRUE(DocumentUndo::undo(dest.get())); EXPECT_EQ(snapshot(*dest), before);
    EXPECT_TRUE(DocumentUndo::redo(dest.get()));
    EXPECT_TRUE(DocumentUndo::redo(dest.get())); EXPECT_EQ(snapshot(*dest), after);
}

TEST_F(ArtworkLibraryInsert, AtomicRetiredCloseIsPostPublicationCommittedOutcome)
{
    auto dest = document(svg(""));
    History history(*dest);
    auto closed = std::make_shared<bool>(false);
    auto atomic = DocumentUndo::beginAtomicInteraction(dest.get()); ASSERT_TRUE(atomic);
    dest->getReprRoot()->setAttribute("data-atomic", "published");
    unsigned retired = 0;
    auto connection = atomic->connectRetired([&] {
        ++retired;
        EXPECT_EQ(history.commits, 1u);
        EXPECT_TRUE(dest->getReprDoc()->inTransaction());
        EXPECT_TRUE(DocumentUndo::deferUntilInteractionQuiescent(dest.get(),
            [closed](SPDocument &) { *closed = true; }, true));
    });
    EXPECT_TRUE(atomic->commitAtomically(Util::Internal::ContextString("Atomic close"), "", [] { return true; }));
    EXPECT_EQ(retired, 1u); EXPECT_EQ(history.commits, 1u);
    EXPECT_FALSE(atomic->active()); EXPECT_FALSE(*closed);
    EXPECT_TRUE(DocumentUndo::interactionCloseRequested(dest.get()));
    EXPECT_STREQ(dest->getReprRoot()->attribute("data-atomic"), "published");
    connection.disconnect(); atomic.reset();
    EXPECT_FALSE(*closed); // No owner continuation is dispatched inline.
}

TEST_F(ArtworkLibraryInsert, AtomicNoLogDoesNotClaimPublicationOrConsumeRedo)
{
    auto dest = document(svg(""));
    dest->getReprRoot()->setAttribute("data-future", "redo");
    DocumentUndo::done(dest.get(), Util::Internal::ContextString("Future"), "");
    ASSERT_TRUE(DocumentUndo::undo(dest.get()));
    auto before = snapshot(*dest);
    History history(*dest);
    auto atomic = DocumentUndo::beginAtomicInteraction(dest.get()); ASSERT_TRUE(atomic);
    unsigned retired = 0;
    auto connection = atomic->connectRetired([&] { ++retired; });
    EXPECT_FALSE(atomic->commitAtomically(Util::Internal::ContextString("No log"), "", [] { return true; }));
    EXPECT_EQ(retired, 0u); EXPECT_EQ(history.commits, 0u);
    EXPECT_EQ(snapshot(*dest), before);
    connection.disconnect(); atomic.reset();
    EXPECT_TRUE(DocumentUndo::redo(dest.get()));
    EXPECT_STREQ(dest->getReprRoot()->attribute("data-future"), "redo");
}


// These tests deliberately retain settled tokens. Destroying the token before
// done/Undo/Redo would hide a stale atomic_owner or retained operation lease.
TEST_F(ArtworkLibraryInsert, RetainedCommittedAtomicTokenAllowsLaterHistory)
{
    auto dest = document(svg(""));
    auto before = snapshot(*dest);
    History history(*dest);
    auto atomic = DocumentUndo::beginAtomicInteraction(dest.get()); ASSERT_TRUE(atomic);
    dest->getReprRoot()->setAttribute("data-atomic", "first");
    ASSERT_TRUE(atomic->commitAtomically(Util::Internal::ContextString("First atomic"), "", [] { return true; }));
    EXPECT_FALSE(atomic->active());
    EXPECT_TRUE(DocumentUndo::interactionIsQuiescent(dest.get()));
    auto first = snapshot(*dest);
    dest->getReprRoot()->setAttribute("data-later", "second");
    DocumentUndo::done(dest.get(), Util::Internal::ContextString("Later ordinary edit"), "");
    EXPECT_EQ(history.commits, 2u);
    auto second = snapshot(*dest);
    EXPECT_TRUE(DocumentUndo::undo(dest.get())); EXPECT_EQ(snapshot(*dest), first);
    EXPECT_TRUE(DocumentUndo::undo(dest.get())); EXPECT_EQ(snapshot(*dest), before);
    EXPECT_TRUE(DocumentUndo::redo(dest.get())); EXPECT_EQ(snapshot(*dest), first);
    EXPECT_TRUE(DocumentUndo::redo(dest.get())); EXPECT_EQ(snapshot(*dest), second);
    EXPECT_FALSE(atomic->interrupted());
}

TEST_F(ArtworkLibraryInsert, AtomicRetirementCanPublishItsOwnSeparateUndo)
{
    auto dest = document(svg(""));
    auto before = snapshot(*dest);
    History history(*dest);
    auto atomic = DocumentUndo::beginAtomicInteraction(dest.get()); ASSERT_TRUE(atomic);
    dest->getReprRoot()->setAttribute("data-atomic", "first");
    std::string first;
    unsigned retired = 0;
    auto connection = atomic->connectRetired([&] {
        ++retired;
        EXPECT_FALSE(atomic->active());
        EXPECT_EQ(history.commits, 1u);
        EXPECT_TRUE(dest->getReprDoc()->inTransaction());
        EXPECT_FALSE(DocumentUndo::interactionIsQuiescent(dest.get())); // Lease, not history fence.
        first = snapshot(*dest);
        dest->getReprRoot()->setAttribute("data-retired", "second");
        DocumentUndo::done(dest.get(), Util::Internal::ContextString("Retirement edit"), "");
        EXPECT_EQ(history.commits, 2u);
    });
    ASSERT_TRUE(atomic->commitAtomically(Util::Internal::ContextString("First atomic"), "", [] { return true; }));
    EXPECT_EQ(retired, 1u);
    EXPECT_TRUE(DocumentUndo::interactionIsQuiescent(dest.get()));
    auto second = snapshot(*dest);
    connection.disconnect();
    EXPECT_TRUE(DocumentUndo::undo(dest.get())); EXPECT_EQ(snapshot(*dest), first);
    EXPECT_TRUE(DocumentUndo::undo(dest.get())); EXPECT_EQ(snapshot(*dest), before);
    EXPECT_TRUE(DocumentUndo::redo(dest.get())); EXPECT_EQ(snapshot(*dest), first);
    EXPECT_TRUE(DocumentUndo::redo(dest.get())); EXPECT_EQ(snapshot(*dest), second);
    EXPECT_FALSE(atomic->interrupted());
}

TEST_F(ArtworkLibraryInsert, RetainedNoLogAtomicTokenAllowsRedoAndNewInteraction)
{
    auto dest = document(svg(""));
    dest->getReprRoot()->setAttribute("data-future", "redo");
    DocumentUndo::done(dest.get(), Util::Internal::ContextString("Future"), "");
    auto future = snapshot(*dest);
    ASSERT_TRUE(DocumentUndo::undo(dest.get()));
    auto before = snapshot(*dest);
    History history(*dest);
    auto atomic = DocumentUndo::beginAtomicInteraction(dest.get()); ASSERT_TRUE(atomic);
    unsigned retired = 0;
    auto connection = atomic->connectRetired([&] { ++retired; });
    EXPECT_FALSE(atomic->commitAtomically(Util::Internal::ContextString("No log"), "", [] { return true; }));
    EXPECT_FALSE(atomic->active());
    EXPECT_EQ(retired, 0u); EXPECT_EQ(history.commits, 0u);
    EXPECT_EQ(snapshot(*dest), before);
    EXPECT_TRUE(DocumentUndo::interactionIsQuiescent(dest.get()));
    EXPECT_TRUE(DocumentUndo::redo(dest.get())); EXPECT_EQ(snapshot(*dest), future);
    EXPECT_TRUE(DocumentUndo::undo(dest.get())); EXPECT_EQ(snapshot(*dest), before);
    auto next = DocumentUndo::beginAtomicInteraction(dest.get()); ASSERT_TRUE(next);
    next->rollback();
    EXPECT_TRUE(DocumentUndo::interactionIsQuiescent(dest.get()));
    connection.disconnect();
}

TEST_F(ArtworkLibraryInsert, RetainedRolledBackAtomicTokenAllowsHistoryAndNewInteraction)
{
    auto dest = document(svg(""));
    dest->getReprRoot()->setAttribute("data-future", "redo");
    DocumentUndo::done(dest.get(), Util::Internal::ContextString("Future"), "");
    auto future = snapshot(*dest);
    ASSERT_TRUE(DocumentUndo::undo(dest.get()));
    auto before = snapshot(*dest);
    bool const dirty = dest->isModifiedSinceSave();
    History history(*dest);
    auto atomic = DocumentUndo::beginAtomicInteraction(dest.get()); ASSERT_TRUE(atomic);
    dest->getReprRoot()->setAttribute("data-aborted", "discard");
    atomic->rollback();
    EXPECT_FALSE(atomic->active());
    EXPECT_EQ(snapshot(*dest), before);
    EXPECT_EQ(dest->isModifiedSinceSave(), dirty);
    EXPECT_EQ(history.commits, 0u);
    EXPECT_TRUE(DocumentUndo::interactionIsQuiescent(dest.get()));
    EXPECT_TRUE(DocumentUndo::redo(dest.get())); EXPECT_EQ(snapshot(*dest), future);
    EXPECT_TRUE(DocumentUndo::undo(dest.get())); EXPECT_EQ(snapshot(*dest), before);
    auto next = DocumentUndo::beginAtomicInteraction(dest.get()); ASSERT_TRUE(next);
    dest->getReprRoot()->setAttribute("data-next", "publish");
    ASSERT_TRUE(next->commitAtomically(Util::Internal::ContextString("Next atomic"), "", [] { return true; }));
    EXPECT_EQ(history.commits, 1u);
    auto after = snapshot(*dest);
    EXPECT_TRUE(DocumentUndo::undo(dest.get())); EXPECT_EQ(snapshot(*dest), before);
    EXPECT_TRUE(DocumentUndo::redo(dest.get())); EXPECT_EQ(snapshot(*dest), after);
}

TEST_F(ArtworkLibraryInsert, InactiveButPublishingAtomicTokenStillGuardsHistory)
{
    auto dest = document(svg(""));
    auto before = snapshot(*dest);
    History history(*dest);
    auto atomic = DocumentUndo::beginAtomicInteraction(dest.get()); ASSERT_TRUE(atomic);
    dest->getReprRoot()->setAttribute("data-atomic", "publish");
    unsigned callbacks = 0;
    auto connection = dest->connectCommit([&] {
        ++callbacks;
        EXPECT_FALSE(atomic->active()); // sealInteraction already ran.
        EXPECT_EQ(history.commits, 1u);
        auto const clears = history.clears;
        EXPECT_FALSE(DocumentUndo::interactionIsQuiescent(dest.get()));
        EXPECT_FALSE(DocumentUndo::undo(dest.get()));
        EXPECT_FALSE(DocumentUndo::redo(dest.get()));
        DocumentUndo::clearUndo(dest.get());
        DocumentUndo::clearRedo(dest.get());
        DocumentUndo::done(dest.get(), Util::Internal::ContextString("Forbidden nested commit"), "");
        EXPECT_EQ(history.commits, 1u); EXPECT_EQ(history.clears, clears);
    });
    EXPECT_TRUE(atomic->commitAtomically(Util::Internal::ContextString("Atomic publication"), "", [] { return true; }));
    connection.disconnect();
    EXPECT_EQ(callbacks, 1u);
    EXPECT_TRUE(atomic->interrupted()); // Reentry was refused, publication still succeeded.
    EXPECT_TRUE(DocumentUndo::interactionIsQuiescent(dest.get()));
    auto after = snapshot(*dest);
    EXPECT_TRUE(DocumentUndo::undo(dest.get())); EXPECT_EQ(snapshot(*dest), before);
    EXPECT_TRUE(DocumentUndo::redo(dest.get())); EXPECT_EQ(snapshot(*dest), after);
}

TEST_F(ArtworkLibraryInsert, ActiveAtomicTokenStillRejectsForeignDone)
{
    auto dest = document(svg(""));
    auto before = snapshot(*dest);
    History history(*dest);
    auto atomic = DocumentUndo::beginAtomicInteraction(dest.get()); ASSERT_TRUE(atomic);
    dest->getReprRoot()->setAttribute("data-atomic", "discard");
    DocumentUndo::done(dest.get(), Util::Internal::ContextString("Foreign done"), "");
    EXPECT_TRUE(atomic->active()); EXPECT_TRUE(atomic->interrupted());
    EXPECT_EQ(history.commits, 0u);
    EXPECT_FALSE(DocumentUndo::interactionIsQuiescent(dest.get()));
    unsigned ready = 0;
    EXPECT_FALSE(atomic->commitAtomically(Util::Internal::ContextString("Interrupted atomic"), "", [&] {
        ++ready; return true;
    }));
    EXPECT_EQ(ready, 0u);
    EXPECT_TRUE(atomic->active());
    atomic->rollback();
    EXPECT_EQ(snapshot(*dest), before);
    EXPECT_TRUE(DocumentUndo::interactionIsQuiescent(dest.get()));
}

TEST_F(ArtworkLibraryInsert, DestroyingSettledTokenDoesNotReleaseNewAtomicGuard)
{
    auto dest = document(svg(""));
    auto before = snapshot(*dest);
    History history(*dest);
    auto old = DocumentUndo::beginAtomicInteraction(dest.get()); ASSERT_TRUE(old);
    old->rollback();
    auto next = DocumentUndo::beginAtomicInteraction(dest.get()); ASSERT_TRUE(next);
    dest->getReprRoot()->setAttribute("data-next", "discard");
    old.reset();
    DocumentUndo::done(dest.get(), Util::Internal::ContextString("Foreign next done"), "");
    EXPECT_TRUE(next->active()); EXPECT_TRUE(next->interrupted());
    EXPECT_EQ(history.commits, 0u);
    EXPECT_FALSE(DocumentUndo::interactionIsQuiescent(dest.get()));
    next->rollback();
    EXPECT_EQ(snapshot(*dest), before);
    EXPECT_TRUE(DocumentUndo::interactionIsQuiescent(dest.get()));
}

namespace {
// Bounded, nonblocking dispatch. No assumption that unrelated application
// sources become idle; the tests assert only their own deferred continuation.
void dispatch_atomic_test_events()
{
    for (unsigned n = 0; n != 128 && g_main_context_iteration(nullptr, false); ++n) {}
}
struct AtomicRollbackAttributeObserver final : XML::NodeObserver {
    XML::Node &root;
    std::function<void()> action;
    bool fired = false;
    explicit AtomicRollbackAttributeObserver(XML::Node &node) : root(node) { root.addObserver(*this); }
    ~AtomicRollbackAttributeObserver() override { root.removeObserver(*this); }
    void notifyAttributeChanged(XML::Node &, GQuark name, Util::ptr_shared,
                                Util::ptr_shared value) override {
        if (fired || std::strcmp(g_quark_to_string(name), "data-atomic") ||
            static_cast<char const *>(value)) return;
        fired = true;
        action();
    }
};
}

TEST_F(ArtworkLibraryInsert, RollbackRemainsGuardedAndLeasedThroughNativeCallbacks)
{
    auto dest = document(svg(""));
    dest->getReprRoot()->setAttribute("data-prior", "keep");
    DocumentUndo::done(dest.get(), Util::Internal::ContextString("Prior edit"), "");
    auto before = snapshot(*dest);
    History history(*dest);
    auto atomic = DocumentUndo::beginAtomicInteraction(dest.get()); ASSERT_TRUE(atomic);
    dest->getReprRoot()->setAttribute("data-atomic", "discard");
    bool continued = false;
    {
        AtomicRollbackAttributeObserver observer(*dest->getReprRoot());
        observer.action = [&] {
            EXPECT_FALSE(atomic->active()); // Inactive does not mean fully reconstructed.
            auto const clears = history.clears;
            EXPECT_FALSE(DocumentUndo::undo(dest.get()));
            EXPECT_FALSE(DocumentUndo::redo(dest.get()));
            DocumentUndo::clearUndo(dest.get()); DocumentUndo::clearRedo(dest.get());
            EXPECT_EQ(history.clears, clears);
            EXPECT_TRUE(DocumentUndo::deferUntilInteractionQuiescent(dest.get(),
                [&](SPDocument &doc) {
                    EXPECT_EQ(snapshot(doc), before); // All restoration precedes close continuation.
                    continued = true;
                }, true));
            dispatch_atomic_test_events();
            EXPECT_FALSE(continued);
        };
        atomic->rollback();
        EXPECT_TRUE(observer.fired);
    }
    EXPECT_FALSE(atomic->active()); EXPECT_TRUE(atomic->interrupted());
    EXPECT_EQ(snapshot(*dest), before);
    EXPECT_EQ(history.commits, 0u);
    EXPECT_FALSE(continued); // Neither lease destruction nor rollback closes inline.
    dispatch_atomic_test_events();
    EXPECT_TRUE(continued); // Retaining the inactive token no longer stalls the owner.
}

TEST_F(ArtworkLibraryInsert, RetirementCloseLeaseSurvivesTokenResetAndEndsAfterCallback)
{
    for (bool destroy_token : {false, true}) {
        SCOPED_TRACE(destroy_token);
        auto dest = document(svg(""));
        auto atomic = DocumentUndo::beginAtomicInteraction(dest.get()); ASSERT_TRUE(atomic);
        dest->getReprRoot()->setAttribute("data-atomic", "published");
        bool closed = false, retired = false;
        auto connection = atomic->connectRetired([&] {
            retired = true;
            EXPECT_FALSE(atomic->active());
            EXPECT_TRUE(DocumentUndo::deferUntilInteractionQuiescent(dest.get(),
                [&](SPDocument &doc) {
                    EXPECT_EQ(&doc, dest.get());
                    EXPECT_STREQ(doc.getReprRoot()->attribute("data-atomic"), "published");
                    // Model the deferred owner, rather than destroying a private
                    // source inline while a synchronous operation still uses it.
                    dest.reset();
                    closed = true;
                }, true));
            if (destroy_token) atomic.reset();
            dispatch_atomic_test_events();
            EXPECT_FALSE(closed);
            EXPECT_NE(dest, nullptr);
        });
        EXPECT_TRUE(atomic->commitAtomically(Util::Internal::ContextString("Close after retirement"), "",
                                             [] { return true; }));
        EXPECT_TRUE(retired);
        EXPECT_FALSE(closed); EXPECT_NE(dest, nullptr);
        dispatch_atomic_test_events();
        EXPECT_TRUE(closed); EXPECT_EQ(dest, nullptr);
        connection.disconnect();
        if (!destroy_token) {
            ASSERT_TRUE(atomic);
            EXPECT_FALSE(atomic->active());
            atomic->rollback(); // Inert even after its document was destroyed.
        }
    }
}

TEST_F(ArtworkLibraryInsert, AtomicNativeGeometryUpdateCommitsAndUndoesExactly)
{
    auto dest = document(svg("<rect id='native' x='2' y='3' width='16' height='12'/>"));
    auto before = snapshot(*dest);
    History history(*dest);
    auto atomic = DocumentUndo::beginAtomicInteraction(dest.get()); ASSERT_TRUE(atomic);
    auto item = cast<SPItem>(dest->getObjectById("native")); ASSERT_TRUE(item);
    item->getRepr()->setAttribute("width", "31.25");
    EXPECT_TRUE(dest->ensureUpToDate());
    EXPECT_TRUE(atomic->active()); EXPECT_FALSE(atomic->interrupted());
    EXPECT_TRUE(DocumentUndo::getUndoSensitive(dest.get()));
    EXPECT_TRUE(dest->getReprDoc()->inTransaction());
    auto bounds = item->documentGeometricBounds(); ASSERT_TRUE(bounds);
    EXPECT_DOUBLE_EQ(bounds->width(), 31.25);
    EXPECT_TRUE(atomic->commitAtomically(Util::Internal::ContextString("Native geometry"), "",
                                        [] { return true; }));
    EXPECT_EQ(history.commits, 1u);
    auto after = snapshot(*dest);
    EXPECT_TRUE(DocumentUndo::undo(dest.get())); EXPECT_EQ(snapshot(*dest), before);
    EXPECT_TRUE(DocumentUndo::redo(dest.get())); EXPECT_EQ(snapshot(*dest), after);
}

TEST_F(ArtworkLibraryInsert, AtomicNativeGeometryUpdateRollbackPreservesRedo)
{
    auto dest = document(svg("<rect id='native' width='16' height='12'/>"));
    dest->getObjectById("native")->getRepr()->setAttribute("width", "20");
    DocumentUndo::done(dest.get(), Util::Internal::ContextString("Future width"), "");
    auto future = snapshot(*dest);
    ASSERT_TRUE(DocumentUndo::undo(dest.get()));
    auto before = snapshot(*dest);
    bool const dirty = dest->isModifiedSinceSave();
    History history(*dest);
    auto atomic = DocumentUndo::beginAtomicInteraction(dest.get()); ASSERT_TRUE(atomic);
    dest->getObjectById("native")->getRepr()->setAttribute("width", "31.25");
    EXPECT_TRUE(dest->ensureUpToDate());
    EXPECT_FALSE(atomic->interrupted());
    atomic->rollback();
    EXPECT_EQ(snapshot(*dest), before);
    EXPECT_EQ(dest->isModifiedSinceSave(), dirty);
    EXPECT_EQ(history.commits, 0u);
    EXPECT_TRUE(DocumentUndo::interactionIsQuiescent(dest.get()));
    EXPECT_TRUE(DocumentUndo::redo(dest.get())); EXPECT_EQ(snapshot(*dest), future);
    EXPECT_TRUE(DocumentUndo::undo(dest.get())); EXPECT_EQ(snapshot(*dest), before);
}

TEST_F(ArtworkLibraryInsert, AtomicNativeUpdateCallbackXmlRemainsInSameUndo)
{
    auto dest = document(svg("<rect id='native' width='16' height='12'/>"));
    auto before = snapshot(*dest);
    History history(*dest);
    auto atomic = DocumentUndo::beginAtomicInteraction(dest.get()); ASSERT_TRUE(atomic);
    bool updated = false;
    auto connection = dest->connectModified([&](unsigned) {
        if (updated) return;
        updated = true;
        EXPECT_TRUE(atomic->active());
        EXPECT_TRUE(DocumentUndo::getUndoSensitive(dest.get()));
        EXPECT_TRUE(dest->getReprDoc()->inTransaction());
        dest->getReprRoot()->setAttribute("data-native-update", "recorded");
    });
    dest->getObjectById("native")->getRepr()->setAttribute("width", "31.25");
    EXPECT_TRUE(dest->ensureUpToDate());
    EXPECT_TRUE(updated); EXPECT_FALSE(atomic->interrupted());
    connection.disconnect();
    EXPECT_TRUE(atomic->commitAtomically(Util::Internal::ContextString("Native update and callback"), "",
                                        [] { return true; }));
    EXPECT_EQ(history.commits, 1u);
    auto after = snapshot(*dest);
    EXPECT_TRUE(DocumentUndo::undo(dest.get())); EXPECT_EQ(snapshot(*dest), before);
    EXPECT_TRUE(DocumentUndo::redo(dest.get())); EXPECT_EQ(snapshot(*dest), after);
    EXPECT_STREQ(dest->getReprRoot()->attribute("data-native-update"), "recorded");
}

TEST_F(ArtworkLibraryInsert, AtomicNativeUpdateCallbacksCannotPublishOrDisableHistory)
{
    auto dest = document(svg("<rect id='native' width='16' height='12'/>"));
    dest->getReprRoot()->setAttribute("data-prior", "keep");
    DocumentUndo::done(dest.get(), Util::Internal::ContextString("Prior edit"), "");
    auto before = snapshot(*dest);
    History history(*dest);
    auto atomic = DocumentUndo::beginAtomicInteraction(dest.get()); ASSERT_TRUE(atomic);
    bool updated = false;
    auto connection = dest->connectModified([&](unsigned) {
        if (updated) return;
        updated = true;
        EXPECT_TRUE(atomic->active());
        EXPECT_FALSE(atomic->interrupted()); // Native update itself is not foreign history.
        auto const clears = history.clears;
        dest->getReprRoot()->setAttribute("data-foreign-update", "must rollback");
        DocumentUndo::done(dest.get(), Util::Internal::ContextString("Foreign update done"), "");
        EXPECT_FALSE(DocumentUndo::undo(dest.get()));
        EXPECT_FALSE(DocumentUndo::redo(dest.get()));
        DocumentUndo::clearUndo(dest.get()); DocumentUndo::clearRedo(dest.get());
        DocumentUndo::setUndoSensitive(dest.get(), false);
        EXPECT_TRUE(DocumentUndo::getUndoSensitive(dest.get()));
        EXPECT_TRUE(dest->getReprDoc()->inTransaction());
        EXPECT_TRUE(atomic->active()); EXPECT_TRUE(atomic->interrupted());
        EXPECT_EQ(history.commits, 0u); EXPECT_EQ(history.clears, clears);
    });
    dest->getObjectById("native")->getRepr()->setAttribute("width", "31.25");
    EXPECT_TRUE(dest->ensureUpToDate());
    EXPECT_TRUE(updated);
    connection.disconnect();
    unsigned ready = 0;
    EXPECT_FALSE(atomic->commitAtomically(Util::Internal::ContextString("Interrupted native update"), "", [&] {
        ++ready; return true;
    }));
    EXPECT_EQ(ready, 0u);
    atomic->rollback();
    EXPECT_EQ(snapshot(*dest), before);
    EXPECT_EQ(history.commits, 0u);
    // The attempted clearUndo did not remove the pre-existing event.
    EXPECT_TRUE(DocumentUndo::undo(dest.get()));
    EXPECT_EQ(dest->getReprRoot()->attribute("data-prior"), nullptr);
    EXPECT_TRUE(DocumentUndo::redo(dest.get())); EXPECT_EQ(snapshot(*dest), before);
}

TEST_F(ArtworkLibraryInsert, OrdinaryInsensitiveScopeStillInterruptsAtomicHistory)
{
    auto dest = document(svg("<rect id='native' width='16' height='12'/>"));
    auto before = snapshot(*dest);
    History history(*dest);
    auto atomic = DocumentUndo::beginAtomicInteraction(dest.get()); ASSERT_TRUE(atomic);
    {
        DocumentUndo::ScopedInsensitive foreign(dest.get());
        EXPECT_TRUE(DocumentUndo::getUndoSensitive(dest.get()));
        EXPECT_TRUE(atomic->interrupted()); EXPECT_TRUE(atomic->active());
        dest->getReprRoot()->setAttribute("data-foreign", "recorded for rollback");
    }
    atomic->rollback();
    EXPECT_EQ(snapshot(*dest), before);
    EXPECT_EQ(history.commits, 0u);
}

namespace {
InkscapeApplication &atomic_test_application()
{
    if (auto app = InkscapeApplication::instance()) return *app;
    Gtk::Application::wrap_in_search_entry2();
    g_setenv("INKSCAPE_APP_ID_TAG", "atomicpreparationtest", true);
    return *new InkscapeApplication(); // Process-lifetime, as in the export fixture.
}

// No private-owner reset: these tests exercise registered application close.
struct AtomicRegisteredDocument {
    InkscapeApplication &app = atomic_test_application();
    SPDocument *doc = app.document_add(document(svg("")));
    unsigned destroyed = 0;
    sigc::connection destroy_connection = doc->connectDestroy([this] {
        ++destroyed;
        doc = nullptr;
    });
    ~AtomicRegisteredDocument() {
        if (doc) app.document_close(doc);
        dispatch_atomic_test_events();
        destroy_connection.disconnect();
    }
};
}

TEST_F(ArtworkLibraryInsert, AtomicPreparationRollbackDefersRegisteredCloseThroughOuterCall)
{
    for (bool rollback_in_ready : {false, true}) {
        SCOPED_TRACE(rollback_in_ready);
        AtomicRegisteredDocument owned;
        auto raw = owned.doc;
        auto before = snapshot(*raw);
        auto const initial_operations = owned.app.documentOperationCount();
        auto atomic = DocumentUndo::beginAtomicInteraction(raw); ASSERT_TRUE(atomic);
        raw->getReprRoot()->setAttribute("data-atomic", "discard");
        unsigned callbacks = 0, ready_calls = 0, ready_destroyed = 0, commits = 0;
        auto commit_connection = raw->connectCommit([&] { ++commits; });
        auto rollback_and_close = [&] {
            ++callbacks;
            atomic->rollback();
            EXPECT_FALSE(atomic->active());
            EXPECT_EQ(snapshot(*raw), before);
            EXPECT_FALSE(DocumentUndo::interactionIsQuiescent(raw));
            owned.app.document_close(raw);
            owned.app.document_close(raw); // Idempotent while pending.
            EXPECT_TRUE(owned.app.documentClosePending(raw));
            dispatch_atomic_test_events(); // Nested loop must not admit close.
            EXPECT_EQ(owned.destroyed, 0u);
            EXPECT_EQ(owned.doc, raw);
        };
        auto before_connection = raw->connectBeforeCommit([&] {
            if (!rollback_in_ready) rollback_and_close();
        });
        // A callable's captured owner may itself dispatch during destruction.
        // It must be destroyed under the outer lease, even on pre-ready return.
        auto ready_capture = std::shared_ptr<int>(new int(0), [&](int *value) {
            delete value;
            ++ready_destroyed;
            dispatch_atomic_test_events();
            EXPECT_EQ(owned.destroyed, 0u);
            EXPECT_EQ(owned.doc, raw);
        });
        EXPECT_FALSE(atomic->commitAtomically(Util::Internal::ContextString("Aborted preparation"), "",
            [&, pin = std::move(ready_capture)] {
                (void)pin;
                ++ready_calls;
                rollback_and_close();
                return true; // Stale readiness must not publish after rollback.
            }));
        before_connection.disconnect();
        commit_connection.disconnect();
        EXPECT_EQ(callbacks, 1u);
        EXPECT_EQ(ready_calls, rollback_in_ready ? 1u : 0u);
        EXPECT_EQ(ready_destroyed, 1u);
        EXPECT_EQ(commits, 0u);
        EXPECT_EQ(owned.destroyed, 0u);
        EXPECT_FALSE(atomic->active());
        dispatch_atomic_test_events();
        EXPECT_EQ(owned.destroyed, 1u);
        EXPECT_EQ(owned.doc, nullptr);
        EXPECT_EQ(owned.app.documentOperationCount(), initial_operations);
        atomic->rollback(); // Retained inactive token is harmless after real close.
    }
}

TEST_F(ArtworkLibraryInsert, AtomicPreparationRollbackCannotPublishFreshCallbackXml)
{
    for (bool rollback_in_ready : {false, true}) {
        SCOPED_TRACE(rollback_in_ready);
        auto dest = document(svg(""));
        auto before = snapshot(*dest);
        History history(*dest);
        auto atomic = DocumentUndo::beginAtomicInteraction(dest.get()); ASSERT_TRUE(atomic);
        dest->getReprRoot()->setAttribute("data-atomic", "discard");
        unsigned callbacks = 0, ready_calls = 0, retired = 0;
        auto retired_connection = atomic->connectRetired([&] { ++retired; });
        std::string fresh;
        auto rollback_and_edit = [&] {
            ++callbacks;
            atomic->rollback();
            EXPECT_FALSE(atomic->active());
            EXPECT_EQ(snapshot(*dest), before);
            // Fresh caller-owned transaction: neither stale done nor stale
            // rollback may consume it when the outer call resumes.
            dest->getReprRoot()->setAttribute("data-later", "keep");
            fresh = snapshot(*dest);
        };
        auto before_connection = dest->connectBeforeCommit([&] {
            if (!rollback_in_ready) rollback_and_edit();
        });
        EXPECT_FALSE(atomic->commitAtomically(Util::Internal::ContextString("Aborted preparation"), "", [&] {
            ++ready_calls;
            rollback_and_edit();
            return true;
        }));
        before_connection.disconnect();
        retired_connection.disconnect();
        EXPECT_EQ(callbacks, 1u);
        EXPECT_EQ(ready_calls, rollback_in_ready ? 1u : 0u);
        EXPECT_EQ(retired, 0u);
        EXPECT_EQ(history.commits, 0u);
        EXPECT_FALSE(atomic->active());
        EXPECT_TRUE(DocumentUndo::interactionIsQuiescent(dest.get()));
        EXPECT_EQ(snapshot(*dest), fresh);
        atomic->rollback();
        EXPECT_EQ(snapshot(*dest), fresh);
        DocumentUndo::done(dest.get(), Util::Internal::ContextString("Fresh callback edit"), "");
        EXPECT_EQ(history.commits, 1u);
        EXPECT_TRUE(DocumentUndo::undo(dest.get())); EXPECT_EQ(snapshot(*dest), before);
        EXPECT_TRUE(DocumentUndo::redo(dest.get())); EXPECT_EQ(snapshot(*dest), fresh);
    }
}


TEST_F(ArtworkLibraryInsert, NestedViewportPlacementPersistsWithoutNormalizingPayload)
{
    struct Case { char const *par; double left, top, width, height; };
    for (auto const &c : {
            Case{"none", 20, 25, 96, 96},
            Case{"xMidYMid meet", 44, 25, 48, 96},
            Case{"xMidYMid slice", 20, -23, 96, 192}}) {
        SCOPED_TRACE(c.par);
        auto xml = svg("<rect id='viewport-art' x='-10' y='-20' width='48' height='96' fill='red'/>",
            "width='1in' height='72pt' viewBox='-10 -20 48 96'",
            std::string("preserveAspectRatio='") + c.par + "'");
        auto source = document(xml);
        auto dest = document(svg("<g id='parent' transform='translate(37,19) rotate(17) scale(1.3,.7)'/>",
                                 "width='400' height='200' viewBox='0 0 200 100'"));
        auto before = snapshot(*dest);
        auto result = Library::insert_artwork(*dest,
            Library::capture_insertion_target(*dest, *dest->getObjectById("parent")),
            token(xml), {20, 25});
        auto verify = [&](SPDocument &d) {
            d.ensureUpToDate();
            auto wrapper = group(d, result);
            ASSERT_TRUE(wrapper);
            auto inner = wrapper->getRepr()->firstChild();
            ASSERT_TRUE(inner);
            auto viewport = cast<SPRoot>(d.getObjectByRepr(inner));
            ASSERT_TRUE(viewport);
            EXPECT_STREQ(inner->attribute("viewBox"), "-10 -20 48 96");
            EXPECT_STREQ(inner->attribute("preserveAspectRatio"), c.par);
            for (unsigned i = 0; i < 6; ++i)
                EXPECT_NEAR(viewport->c2p[i], source->getRoot()->c2p[i], 1e-12);
            auto leaf = cast<SPItem>(d.getObjectById("viewport-art"));
            ASSERT_TRUE(leaf);
            for (auto item : {wrapper, static_cast<SPItem *>(viewport), leaf}) {
                auto box = item->documentVisualBounds();
                ASSERT_TRUE(box);
                EXPECT_NEAR(box->left(), c.left, 1e-8);
                EXPECT_NEAR(box->top(), c.top, 1e-8);
                EXPECT_NEAR(box->width(), c.width, 1e-8);
                EXPECT_NEAR(box->height(), c.height, 1e-8);
            }
            same_render(*source, d, {20, 25});
        };
        verify(*dest);
        auto after = snapshot(*dest);
        auto saved = sp_repr_save_buf(dest->getReprDoc()).raw();
        auto reopened = document(saved);
        verify(*reopened);
        EXPECT_TRUE(DocumentUndo::undo(dest.get()));
        EXPECT_EQ(snapshot(*dest), before);
        EXPECT_TRUE(DocumentUndo::redo(dest.get()));
        EXPECT_EQ(snapshot(*dest), after);
        verify(*dest);
    }
}

TEST_F(ArtworkLibraryInsert, IndependentWrapperMatrixKeepsRoundTripPrecision)
{
    auto xml = svg(art);
    auto source = document(xml);
    auto dest = document(svg("<g id='parent' transform='translate(37,19) rotate(17) scale(1.3,.7)'/>",
                             "width='400' height='200' viewBox='0 0 200 100'"));
    auto parent = cast<SPItem>(dest->getObjectById("parent"));
    ASSERT_TRUE(parent);
    Geom::Point origin(20, 25);
    auto expected = Geom::Translate(origin) * parent->i2doc_affine().inverse();
    auto prefs = Preferences::get();
    char const *path = "/options/svgoutput/numericprecision";
    struct Restore {
        Preferences *prefs;
        char const *path;
        Preferences::Entry old;
        ~Restore() {
            if (old.isSet()) prefs->setString(path, old.getString());
            else prefs->remove(path);
        }
    } restore{prefs, path, prefs->getEntry(path)};
    prefs->setInt(path, 3); // User serialization preference must not degrade placement.
    auto result = Library::insert_artwork(*dest,
        Library::capture_insertion_target(*dest, *parent), token(xml), origin);
    auto verify = [&](SPDocument &d) {
        d.ensureUpToDate();
        auto wrapper = group(d, result);
        ASSERT_TRUE(wrapper);
        for (unsigned i = 0; i < 6; ++i)
            EXPECT_DOUBLE_EQ(wrapper->transform[i], expected[i]);
        auto effective = wrapper->i2doc_affine();
        Geom::Affine translation = Geom::Translate(origin);
        for (unsigned i = 0; i < 6; ++i)
            EXPECT_NEAR(effective[i], translation[i], 1e-12);
        same_render(*source, d, origin);
    };
    verify(*dest);
    auto saved = sp_repr_save_buf(dest->getReprDoc()).raw();
    auto reopened = document(saved);
    verify(*reopened);
}


TEST_F(ArtworkLibraryInsert, IndependentWrapperCannotStealAnonymousPayloadGroupIdWithoutCss)
{
    auto xml = svg(std::string("<g>") + art + "</g>");
    auto source = document(xml);
    auto dest = document(svg(""));
    auto source_group = find(source->getReprRoot(), "svg:g");
    ASSERT_TRUE(source_group);
    auto source_id = std::string(source_group->attribute("id"));
    // Explicitly establish the native allocator collision, independent of CSS.
    ASSERT_EQ(dest->generate_unique_id("g"), source_id);
    auto before = snapshot(*dest);
    History history(*dest);
    auto result = Library::insert_artwork(*dest,
        Library::capture_insertion_target(*dest, *dest->getRoot()), token(xml), {});
    auto wrapper = group(*dest, result);
    ASSERT_TRUE(wrapper);
    EXPECT_NE(std::string(wrapper->getId()), source_id);
    auto inner_group = find(wrapper->getRepr()->firstChild(), "svg:g");
    ASSERT_TRUE(inner_group);
    EXPECT_STREQ(inner_group->attribute("id"), source_id.c_str());
    same_render(*source, *dest, {});
    auto after = snapshot(*dest);
    EXPECT_EQ(history.commits, 1u);
    EXPECT_TRUE(DocumentUndo::undo(dest.get()));
    EXPECT_EQ(snapshot(*dest), before);
    EXPECT_TRUE(DocumentUndo::redo(dest.get()));
    EXPECT_EQ(snapshot(*dest), after);
    same_render(*source, *dest, {});
}


namespace {
std::string neutral_text_art(bool on_path)
{
    std::string const guide =
        "<path id='neutral-baseline' d='M8 60C30 25 65 80 88 45' fill='none' stroke='none'/>";
    std::string const content = on_path
        ? "<textPath xlink:href='#neutral-baseline'>Editable</textPath>"
        : "Editable";
    return svg((on_path ? guide : "") +
        "<text id='neutral-label' class='incoming' x='8' y='56' "
        "style='font-family:sans-serif;font-size:12px;fill:#cf2468'>" +
        content + "</text>");
}

void expect_neutral_priorities(SPText &text)
{
    EXPECT_FALSE(text.has_shape_inside());
    EXPECT_TRUE(text.style->shape_inside.set);
    EXPECT_TRUE(text.style->shape_inside.important);
    EXPECT_FALSE(text.style->shape_inside.inherit);
    EXPECT_TRUE(text.style->shape_inside.hrefs.empty());
    EXPECT_TRUE(text.style->font_size.important);
    EXPECT_TRUE(text.style->fill.important);
    EXPECT_EQ(text.style->shape_inside.write(SP_STYLE_FLAG_IFSET), "shape-inside:none !important;");
}

void expect_baseline_dependency(SPDocument &doc)
{
    auto text = cast<SPText>(doc.getObjectById("neutral-label"));
    auto path = cast<SPItem>(doc.getObjectById("neutral-baseline"));
    ASSERT_TRUE(text);
    ASSERT_TRUE(path);
    ASSERT_TRUE(text->firstChild());
    auto textpath = cast<SPTextPath>(text->firstChild());
    ASSERT_TRUE(textpath);
    EXPECT_EQ(sp_textpath_get_path_item(textpath), path); // Native reference, not only href spelling.
    EXPECT_EQ(text->get_first_shape_dependency(), path);
    EXPECT_EQ(text->get_all_shape_dependencies(), (std::vector<SPItem *>{path}));
    std::vector<SPObject *> links;
    text->getLinked(links, SPObject::LinkedObjectNature::DEPENDENCY);
    EXPECT_EQ(links, (std::vector<SPObject *>{path}));
    links.clear();
    text->getLinkedRecursive(links, SPObject::LinkedObjectNature::DEPENDENCY);
    EXPECT_NE(std::find(links.begin(), links.end(), path), links.end());
    EXPECT_STREQ(textpath->getRepr()->attribute("xlink:href"), "#neutral-baseline");
}
} // namespace

TEST_F(ArtworkLibraryInsert, NeutralHideShowPreservesWholeXmlPriorityAndNativePixels)
{
    for (bool on_path : {false, true}) {
        SCOPED_TRACE(on_path);
        auto xml = neutral_text_art(on_path);
        auto source = document(xml);
        auto empty = document(svg(""));
        EXPECT_GT(differences(render(*source), render(*empty)), 0u); // Nonempty render oracle.
        auto dest = document(svg(
            "<defs><rect id='foreign-frame' x='1' y='1' width='90' height='90'/></defs>"
            "<style>.incoming{shape-inside:url(#foreign-frame)!important;"
            "font-size:32px!important;fill:lime!important;opacity:.2!important}</style>"));
        auto result = Library::insert_artwork(*dest,
            Library::capture_insertion_target(*dest, *dest->getRoot()), token(xml), {});
        ASSERT_TRUE(group(*dest, result));
        auto text = cast<SPText>(dest->getObjectById("neutral-label"));
        ASSERT_TRUE(text);
        ASSERT_TRUE(text->layout.outputExists());
        ASSERT_NO_FATAL_FAILURE(expect_neutral_priorities(*text));
        same_render(*source, *dest, {});
        auto before = snapshot(*dest);
        auto dirty = dest->isModifiedSinceSave();
        History history(*dest);
        auto selected = ObjectSet(dest.get());
        selected.add(text);
        auto selection_before = selected.items_vector();

        // These are the actual methods called around inline paste and toolbar
        // range work. Neutral text must never enter the lossy temporary CSS path.
        for (unsigned i = 0; i < 2; ++i) {
            text->hide_shape_inside();
            dest->ensureUpToDate();
            EXPECT_EQ(snapshot(*dest), before);
            ASSERT_NO_FATAL_FAILURE(expect_neutral_priorities(*text));
            same_render(*source, *dest, {});
            text->show_shape_inside();
            dest->ensureUpToDate();
            EXPECT_EQ(snapshot(*dest), before);
            ASSERT_NO_FATAL_FAILURE(expect_neutral_priorities(*text));
            same_render(*source, *dest, {});
        }
        EXPECT_EQ(selected.items_vector(), selection_before);
        EXPECT_EQ(dest->isModifiedSinceSave(), dirty);
        EXPECT_EQ(history.commits, 0u);
        EXPECT_EQ(history.clears, 0u);

        // Positive control: deliberately remove the inline shield in a private
        // copy. The real destination important rules must change pixels.
        auto control = document(sp_repr_save_buf(dest->getReprDoc()).raw());
        auto unshielded = cast<SPText>(control->getObjectById("neutral-label"));
        ASSERT_TRUE(unshielded);
        unshielded->getRepr()->setAttribute("style", nullptr);
        control->ensureUpToDate();
        EXPECT_TRUE(unshielded->has_shape_inside());
        EXPECT_TRUE(unshielded->style->shape_inside.important);
        EXPECT_NE(unshielded->style->font_size.computed, text->style->font_size.computed);
        EXPECT_GT(differences(render(*dest), render(*control)), 0u);
        EXPECT_NE(snapshot(*control), before);
        EXPECT_EQ(snapshot(*dest), before);
    }
}

TEST_F(ArtworkLibraryInsert, NeutralTextPathSelectedCropRetainsNativeDependencyThroughSvgRoundTrip)
{
    auto xml = neutral_text_art(true);
    auto source = document(xml);
    auto empty = document(svg(""));
    EXPECT_GT(differences(render(*source), render(*empty)), 0u);
    auto dest = document(svg("<rect id='unselected-decoy' x='2' y='2' width='12' height='12' fill='blue'/>"));
    auto result = Library::insert_artwork(*dest,
        Library::capture_insertion_target(*dest, *dest->getRoot()), token(xml), {});
    ASSERT_TRUE(group(*dest, result));
    ASSERT_NO_FATAL_FAILURE(expect_baseline_dependency(*dest));
    auto original_text = cast<SPText>(dest->getObjectById("neutral-label"));
    ASSERT_TRUE(original_text);
    auto selected = ObjectSet(dest.get());
    selected.add(original_text); // The path is deliberately NOT selected.
    auto selected_before = selected.items_vector();
    auto original_before = snapshot(*dest);
    auto source_before = snapshot(*source);
    auto dirty_before = dest->isModifiedSinceSave();
    History original_history(*dest);
    EXPECT_GT(differences(render(*source), render(*dest)), 0u); // The decoy is genuinely visible.

    // This is the same native dependency-preserving crop used by selected
    // vector export. Work on an independent native copy, never the live source.
    auto copy = document(sp_repr_save_buf(dest->getReprDoc()).raw());
    auto text = cast<SPText>(copy->getObjectById("neutral-label"));
    auto path = copy->getObjectById("neutral-baseline");
    ASSERT_TRUE(text);
    ASSERT_TRUE(path);
    auto before = snapshot(*copy);
    History history(*copy);
    copy->getRoot()->cropToObjects({text});
    copy->ensureUpToDate();
    EXPECT_EQ(copy->getObjectById("neutral-label"), text);
    EXPECT_EQ(copy->getObjectById("neutral-baseline"), path);
    EXPECT_FALSE(copy->getObjectById("unselected-decoy"));
    ASSERT_NO_FATAL_FAILURE(expect_baseline_dependency(*copy));
    ASSERT_NO_FATAL_FAILURE(expect_neutral_priorities(*text));
    same_render(*source, *copy, {});
    DocumentUndo::done(copy.get(), Util::Internal::ContextString("Selected SVG crop"), "");
    EXPECT_EQ(history.commits, 1u);
    auto after = snapshot(*copy);
    EXPECT_NE(after, before); // Crop must do real work, not preserve everything.

    auto saved = sp_repr_save_buf(copy->getReprDoc());
    auto reopened = document(saved.raw());
    ASSERT_NO_FATAL_FAILURE(expect_baseline_dependency(*reopened));
    auto reopened_text = cast<SPText>(reopened->getObjectById("neutral-label"));
    ASSERT_TRUE(reopened_text);
    ASSERT_NO_FATAL_FAILURE(expect_neutral_priorities(*reopened_text));
    EXPECT_FALSE(reopened->getObjectById("unselected-decoy"));
    same_render(*source, *reopened, {});
    same_render(*copy, *reopened, {});

    EXPECT_TRUE(DocumentUndo::undo(copy.get()));
    EXPECT_EQ(snapshot(*copy), before);
    EXPECT_TRUE(copy->getObjectById("unselected-decoy"));
    ASSERT_NO_FATAL_FAILURE(expect_baseline_dependency(*copy));
    EXPECT_TRUE(DocumentUndo::redo(copy.get()));
    EXPECT_EQ(snapshot(*copy), after);
    ASSERT_NO_FATAL_FAILURE(expect_baseline_dependency(*copy));
    same_render(*source, *copy, {});
    EXPECT_EQ(snapshot(*dest), original_before);
    EXPECT_EQ(snapshot(*source), source_before);
    EXPECT_EQ(selected.items_vector(), selected_before);
    EXPECT_EQ(dest->isModifiedSinceSave(), dirty_before);
    EXPECT_EQ(original_history.commits, 0u);
}

TEST_F(ArtworkLibraryInsert, ShapeDependencyGateKeepsUrlsAndExplicitInheritDistinctFromNone)
{
    for (auto value : {"url(#real-frame)", "url(#missing-frame)", "inherit"}) {
        SCOPED_TRACE(value);
        auto doc = document(svg(
            std::string("<defs><rect id='real-frame' width='90' height='90'/></defs>") +
            "<path id='baseline-control' d='M2 60H90'/>"
            "<text id='probe' style='font-size:12px;shape-inside:" + value + "'>"
            "<textPath xlink:href='#baseline-control'>Control</textPath></text>"));
        auto text = cast<SPText>(doc->getObjectById("probe"));
        ASSERT_TRUE(text);
        EXPECT_TRUE(text->has_shape_inside());
        EXPECT_TRUE(text->style->shape_inside.set);
        EXPECT_EQ(bool(text->style->shape_inside.inherit), std::string(value) == "inherit");
        auto dependencies = text->get_all_shape_dependencies();
        if (std::string(value) == "url(#real-frame)") {
            EXPECT_EQ(dependencies,
                      (std::vector<SPItem *>{cast<SPItem>(doc->getObjectById("real-frame"))}));
            // Preserve the existing genuine-flow hide/show route. No important
            // declarations here: this is not a claim that route preserves them.
            text->hide_shape_inside();
            doc->ensureUpToDate();
            EXPECT_FALSE(text->has_shape_inside());
            text->show_shape_inside();
            doc->ensureUpToDate();
            EXPECT_TRUE(text->has_shape_inside());
            EXPECT_EQ(text->get_all_shape_dependencies(), dependencies);
        } else {
            // Missing URL and inherit remain the legacy flow branch, not a
            // new textPath fallback. Full inherit resolution is not implemented.
            EXPECT_TRUE(dependencies.empty());
            EXPECT_EQ(text->get_first_shape_dependency(), nullptr);
        }
    }
}

TEST_F(ArtworkLibraryInsert, IndependentWrapperIdDoesNotCaptureIncomingUseReference)
{
    auto xml = svg(std::string("<g id='g1'>") + art +
                   "</g><use id='incoming-use' xlink:href='#g1' x='44'/>");
    auto source = document(xml);
    // Exercise deconfliction of the private envelope ID too. The existing
    // destination object must remain untouched and must not become the wrapper.
    auto dest = document(svg("<g id='vacards-import-envelope'/>"));
    ASSERT_EQ(dest->generate_unique_id("g"), "g1");
    auto existing = dest->getObjectById("vacards-import-envelope");
    ASSERT_TRUE(existing);
    auto before = snapshot(*dest);
    History history(*dest);
    auto result = Library::insert_artwork(*dest,
        Library::capture_insertion_target(*dest, *dest->getRoot()), token(xml), {});
    auto wrapper = group(*dest, result);
    ASSERT_TRUE(wrapper);
    EXPECT_NE(std::string(wrapper->getId()), "g1");
    EXPECT_NE(std::string(wrapper->getId()), "vacards-import-envelope");
    EXPECT_EQ(dest->getObjectById("vacards-import-envelope"), existing);
    auto incoming = dest->getObjectById("incoming-use");
    ASSERT_TRUE(incoming);
    EXPECT_STREQ(incoming->getRepr()->attribute("xlink:href"), "#g1");
    auto target = dest->getObjectById("g1");
    ASSERT_TRUE(target);
    EXPECT_NE(target, wrapper);
    EXPECT_EQ(incoming->parent, target->parent);
    same_render(*source, *dest, {});
    auto after = snapshot(*dest);
    EXPECT_EQ(history.commits, 1u);
    EXPECT_TRUE(DocumentUndo::undo(dest.get()));
    EXPECT_EQ(snapshot(*dest), before);
    EXPECT_TRUE(DocumentUndo::redo(dest.get()));
    EXPECT_EQ(snapshot(*dest), after);
    same_render(*source, *dest, {});
}


// Bounded native validFor coverage. These use only public application APIs and
// existing fixtures; no private SPDocument state is exposed or inspected.
TEST_F(ArtworkLibraryInsert, ValidForLiveAndAtomicTokensOnlyOwnTheirDocument)
{
    auto dest = document(svg("<g id='own'/>"));
    auto other = document(svg("<g id='other'/>"));
    auto untouched = document(svg("<g id='untouched'/>"));
    auto dest_before = snapshot(*dest);
    auto other_before = snapshot(*other);
    auto untouched_before = snapshot(*untouched);
    History dest_history(*dest), other_history(*other), untouched_history(*untouched);

    auto live = DocumentUndo::beginRollbackableInteraction(dest.get()); ASSERT_TRUE(live);
    EXPECT_TRUE(live->validFor(dest.get()));
    EXPECT_FALSE(live->validFor(nullptr));
    EXPECT_FALSE(live->validFor(other.get()));
    EXPECT_FALSE(live->validFor(untouched.get()));
    // Invalid queries are side-effect free: every document is unchanged and the
    // third document can still start its own interaction afterwards.
    EXPECT_EQ(snapshot(*dest), dest_before);
    EXPECT_EQ(snapshot(*other), other_before);
    EXPECT_EQ(snapshot(*untouched), untouched_before);
    live->rollback();
    EXPECT_FALSE(live->validFor(dest.get()));

    auto fresh = DocumentUndo::beginRollbackableInteraction(untouched.get()); ASSERT_TRUE(fresh);
    EXPECT_TRUE(fresh->validFor(untouched.get()));
    fresh->rollback();
    EXPECT_FALSE(fresh->validFor(untouched.get()));

    auto atomic = DocumentUndo::beginAtomicInteraction(other.get()); ASSERT_TRUE(atomic);
    EXPECT_TRUE(atomic->validFor(other.get()));
    EXPECT_FALSE(atomic->validFor(nullptr));
    EXPECT_FALSE(atomic->validFor(dest.get()));
    EXPECT_FALSE(atomic->validFor(untouched.get()));
    // The admitted settlement lease keeps operation_depth nonzero; the predicate
    // must accept that valid atomic state.
    EXPECT_FALSE(DocumentUndo::interactionIsQuiescent(other.get()));
    EXPECT_EQ(snapshot(*other), other_before);
    EXPECT_EQ(snapshot(*untouched), untouched_before);
    atomic->rollback();
    EXPECT_FALSE(atomic->validFor(other.get()));

    EXPECT_EQ(dest_history.commits, 0u);
    EXPECT_EQ(other_history.commits, 0u);
    EXPECT_EQ(untouched_history.commits, 0u);
}

TEST_F(ArtworkLibraryInsert, ValidForFollowsMoveRollbackCommitAndNoLogSettlement)
{
    auto dest = document(svg(""));
    auto foreign = document(svg(""));
    auto before = snapshot(*dest);

    auto live = DocumentUndo::beginRollbackableInteraction(dest.get()); ASSERT_TRUE(live);
    EXPECT_TRUE(live->validFor(dest.get()));
    auto moved = std::move(*live);
    EXPECT_FALSE(live->validFor(dest.get())); // Moved-from token owns no state.
    EXPECT_TRUE(moved.validFor(dest.get()));
    EXPECT_FALSE(moved.validFor(foreign.get()));
    moved.rollback();
    EXPECT_FALSE(moved.validFor(dest.get()));
    EXPECT_FALSE(moved.validFor(foreign.get()));
    EXPECT_EQ(snapshot(*dest), before);

    auto committed = DocumentUndo::beginRollbackableInteraction(dest.get()); ASSERT_TRUE(committed);
    EXPECT_TRUE(committed->validFor(dest.get()));
    dest->getReprRoot()->setAttribute("data-commit", "1");
    committed->commit(Util::Internal::ContextString("Committed"), "");
    EXPECT_FALSE(committed->validFor(dest.get()));
    EXPECT_FALSE(committed->validFor(foreign.get()));

    dest->getReprRoot()->setAttribute("data-future", "redo");
    DocumentUndo::done(dest.get(), Util::Internal::ContextString("Future"), "");
    ASSERT_TRUE(DocumentUndo::undo(dest.get()));
    auto seeded = snapshot(*dest);
    History history(*dest);
    auto atomic = DocumentUndo::beginAtomicInteraction(dest.get()); ASSERT_TRUE(atomic);
    EXPECT_TRUE(atomic->validFor(dest.get()));
    EXPECT_FALSE(atomic->validFor(foreign.get()));
    unsigned retired = 0;
    auto connection = atomic->connectRetired([&] { ++retired; });
    EXPECT_FALSE(atomic->commitAtomically(Util::Internal::ContextString("No log"), "", [] { return true; }));
    EXPECT_FALSE(atomic->validFor(dest.get()));
    EXPECT_FALSE(atomic->validFor(foreign.get()));
    EXPECT_EQ(retired, 0u);
    EXPECT_EQ(history.commits, 0u);
    connection.disconnect();
    EXPECT_EQ(snapshot(*dest), seeded);
    EXPECT_TRUE(DocumentUndo::redo(dest.get()));
    EXPECT_STREQ(dest->getReprRoot()->attribute("data-future"), "redo");
}

TEST_F(ArtworkLibraryInsert, ValidForRejectsInterruptedAtomicAndOwnRollbackStillWorks)
{
    auto dest = document(svg(""));
    auto before = snapshot(*dest);
    History history(*dest);
    auto atomic = DocumentUndo::beginAtomicInteraction(dest.get()); ASSERT_TRUE(atomic);
    EXPECT_TRUE(atomic->validFor(dest.get()));
    dest->getReprRoot()->setAttribute("data-atomic", "discard");
    DocumentUndo::done(dest.get(), Util::Internal::ContextString("Foreign done"), "");
    EXPECT_TRUE(atomic->active());
    EXPECT_TRUE(atomic->interrupted());
    EXPECT_FALSE(atomic->validFor(dest.get()));
    auto pending = snapshot(*dest);
    EXPECT_FALSE(atomic->validFor(nullptr));
    EXPECT_FALSE(atomic->validFor(dest.get()));
    EXPECT_EQ(snapshot(*dest), pending); // Predicate probe did not alter XML.
    EXPECT_EQ(history.commits, 0u);
    atomic->rollback();
    EXPECT_EQ(snapshot(*dest), before);
    EXPECT_FALSE(atomic->validFor(dest.get()));
    EXPECT_TRUE(DocumentUndo::interactionIsQuiescent(dest.get()));
}

TEST_F(ArtworkLibraryInsert, ValidForFalseWhenNativeCloseRequestedAndSettlesCleanly)
{
    auto dest = document(svg(""));
    auto before = snapshot(*dest);
    auto atomic = DocumentUndo::beginAtomicInteraction(dest.get()); ASSERT_TRUE(atomic);
    EXPECT_TRUE(atomic->validFor(dest.get()));
    bool continued = false;
    EXPECT_TRUE(DocumentUndo::deferUntilInteractionQuiescent(dest.get(),
        [&](SPDocument &) { continued = true; }, true));
    EXPECT_TRUE(DocumentUndo::interactionCloseRequested(dest.get()));
    EXPECT_FALSE(atomic->validFor(dest.get()));
    EXPECT_NE(dest, nullptr); // Close stays deferred while the settlement lease lives.
    EXPECT_EQ(snapshot(*dest), before);
    atomic->rollback();
    EXPECT_FALSE(atomic->validFor(dest.get()));
    EXPECT_EQ(snapshot(*dest), before);
    EXPECT_FALSE(continued); // No continuation is dispatched inline.
    dispatch_atomic_test_events();
    EXPECT_TRUE(continued);
    EXPECT_NE(dest, nullptr);
}

TEST_F(ArtworkLibraryInsert, ValidForRetainedLiveTokenRejectsDestroyedDocument)
{
    auto dest = document(svg(""));
    auto raw = dest.get();
    auto live = DocumentUndo::beginRollbackableInteraction(raw); ASSERT_TRUE(live);
    EXPECT_TRUE(live->validFor(raw));
    bool closed = false;
    {
        auto lease = DocumentUndo::holdInteractionOperation(raw);
        ASSERT_TRUE(lease);
        EXPECT_TRUE(DocumentUndo::deferUntilInteractionQuiescent(raw, [&](SPDocument &doc) {
            EXPECT_EQ(&doc, raw);
            dest.reset();
            closed = true;
        }, true));
        EXPECT_FALSE(live->validFor(raw)); // Close requested while still alive.
        EXPECT_NE(dest, nullptr);
    }
    dispatch_atomic_test_events();
    EXPECT_TRUE(closed);
    EXPECT_EQ(dest, nullptr);
    // The argument is a stale raw pointer now; reject it without dereferencing.
    EXPECT_FALSE(live->validFor(raw));
    EXPECT_FALSE(live->validFor(nullptr));
    live->rollback(); // Inert after the owning document was destroyed.
    EXPECT_FALSE(live->validFor(raw));
}

TEST_F(ArtworkLibraryInsert, ValidForFalseInsideXmlMutationThenTrueAfter)
{
    auto dest = document(svg(""));
    History history(*dest);
    auto atomic = DocumentUndo::beginAtomicInteraction(dest.get()); ASSERT_TRUE(atomic);
    EXPECT_TRUE(atomic->validFor(dest.get()));
    dest->getReprRoot()->setAttribute("data-atomic", "present");
    bool inside = true;
    {
        AtomicRollbackAttributeObserver observer(*dest->getReprRoot());
        observer.action = [&] { inside = atomic->validFor(dest.get()); };
        dest->getReprRoot()->setAttribute("data-atomic", nullptr);
        EXPECT_TRUE(observer.fired);
    }
    EXPECT_FALSE(inside); // XML MutationScope is still active during the observer.
    EXPECT_TRUE(atomic->validFor(dest.get()));
    EXPECT_EQ(history.commits, 0u);
    atomic->rollback();
    EXPECT_TRUE(DocumentUndo::interactionIsQuiescent(dest.get()));
}
