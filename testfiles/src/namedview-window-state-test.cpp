// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * Integration regression: capturing per-window namedview state and applying it
 * to a copied document must be a read-only operation on the source document.
 *
 * Fixture startup/teardown is the real GUI application/desktop path from
 * testfiles/src/dialog-notebook-test.cpp:45-145. The source document is edited
 * twice through the native Undo transaction path and undone once, so a retained
 * redo entry exists. capture_namedview_window_state() and
 * apply_namedview_window_state() are then exercised against a real document
 * copy while an UndoStackObserver and a whole-tree XML fingerprint prove that
 * neither the source undo history nor the source XML, identity, title, dirty
 * flag, undo sensitivity or transaction state changed.
 */

#include <gtest/gtest.h>
#include <gtk/gtk.h>
#include <gtkmm/application.h>
#include <gtkmm/window.h>
#include <giomm/simpleaction.h>

#include <2geom/transforms.h>

#include <cmath>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "desktop.h"
#include "document.h"
#include "document-undo.h"
#include "enums.h"
#include "inkscape-application.h"
#include "inkscape-window.h"
#include "layer-manager.h"
#include "object/sp-item-group.h"
#include "object/sp-namedview.h"
#include "object/sp-object.h"
#include "preferences.h"
#include "svg/stringstream.h"
#include "undo-stack-observer.h"
#include "util-string/context-string.h"
#include "xml/attribute-record.h"
#include "xml/document.h"
#include "xml/node.h"

namespace {

class TestApplication : public InkscapeApplication {};

InkscapeApplication &testApplication()
{
    static auto application = [] {
        Gtk::Application::wrap_in_search_entry2();
        g_setenv("INKSCAPE_APP_ID_TAG", "namedviewwindowstatetest", true);
        auto result = new TestApplication();
        result->gio_app()->register_application();
        // Preserve fatal failures as test failures rather than the application's
        // interactive emergency dialog.
        for (auto signal : {SIGSEGV, SIGABRT, SIGFPE, SIGILL}) {
            std::signal(signal, SIG_DFL);
        }
#ifndef _WIN32
        std::signal(SIGBUS, SIG_DFL);
#endif
        return result;
    }();
    return *application;
}

void drainMainContext()
{
    for (unsigned i = 0; i < 10000 && g_main_context_pending(nullptr); ++i) {
        g_main_context_iteration(nullptr, false);
    }
}

// Synthetic document: an svg:title, a real sodipodi:namedview, a real layer with
// the owned id "layer-original" and one rect at x='10'. No filesystem output.
constexpr char kSvg[] =
    "<svg xmlns='http://www.w3.org/2000/svg'"
    " xmlns:sodipodi='http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd'"
    " xmlns:inkscape='http://www.inkscape.org/namespaces/inkscape'"
    " width='100' height='100'>"
    "<title>Window state fixture</title>"
    "<sodipodi:namedview id='namedview1' inkscape:current-layer='layer-original'/>"
    "<g inkscape:groupmode='layer' inkscape:label='Layer' id='layer-original'>"
    "<rect id='rect' x='10' y='0' width='5' height='5'/>"
    "</g>"
    "</svg>";

// Counts every undo-stack notification the source document can emit. All must
// stay zero across capture/apply.
struct History final : Inkscape::UndoStackObserver {
    SPDocument &document;
    unsigned commits = 0, undos = 0, redos = 0, clears = 0, expires = 0;
    explicit History(SPDocument &doc) : document(doc) { document.addUndoObserver(*this); }
    ~History() override { document.removeUndoObserver(*this); }
    void notifyUndoEvent(Inkscape::Event *) override { ++undos; }
    void notifyRedoEvent(Inkscape::Event *) override { ++redos; }
    void notifyUndoCommitEvent(Inkscape::Event *) override { ++commits; }
    void notifyUndoExpired(Inkscape::Event *) override { ++expires; }
    void notifyClearUndoEvent() override { ++clears; }
    void notifyClearRedoEvent() override { ++clears; }
};

// Length-prefixed field. A null pointer is distinct from an empty string.
void appendField(std::string &out, char const *text)
{
    if (!text) {
        out += "-;";
        return;
    }
    out += std::to_string(std::strlen(text)) + ":" + text + ";";
}

// Raw, nonmutating XML fingerprint: node type, name and content (null distinct
// from empty), every attribute in its actual order and every child in order.
// Serialization (sp_repr_save_buf) mutates the live document, so it is never
// used here.
void fingerprintNode(Inkscape::XML::Node const *node, std::string &out)
{
    out += "[" + std::to_string(static_cast<int>(node->type()));
    appendField(out, node->name());
    appendField(out, node->content());
    for (auto const &attr : node->attributeList()) {
        appendField(out, g_quark_to_string(attr.key));
        appendField(out, static_cast<char const *>(attr.value));
    }
    out += ";";
    for (auto child = node->firstChild(); child; child = child->next()) {
        fingerprintNode(child, out);
    }
    out += "]";
}

std::string fingerprint(SPDocument &document)
{
    std::string out;
    fingerprintNode(document.getReprDoc(), out);
    return out;
}

bool actionEnabled(SPDocument &document, char const *name)
{
    auto group = document.getActionGroup();
    if (!group) {
        return false;
    }
    auto action = std::dynamic_pointer_cast<Gio::SimpleAction>(group->lookup_action(name));
    return action && action->get_enabled();
}

// Parses an attribute with g_ascii_strtoll and requires that the whole non-empty
// text was consumed.
long long parseIntAttribute(char const *text)
{
    EXPECT_NE(text, nullptr);
    if (!text) {
        return 0;
    }
    char *end = nullptr;
    long long const value = g_ascii_strtoll(text, &end, 10);
    EXPECT_GT(std::strlen(text), 0u);
    EXPECT_EQ(end, text + std::strlen(text));
    return value;
}

// Everything observable about the source outside its XML: identity, whole-tree
// XML, dirty flag, undo/redo sensitivity, transaction state and window title.
std::string sourceState(SPDocument &document, InkscapeWindow &window)
{
    std::string out;
    appendField(out, document.getDocumentFilename());
    appendField(out, document.getDocumentBase());
    appendField(out, document.getDocumentName());
    out += "modified=" + std::string(document.isModifiedSinceSave() ? "1" : "0");
    out += ";undo=" + std::string(actionEnabled(document, "undo") ? "1" : "0");
    out += ";redo=" + std::string(actionEnabled(document, "redo") ? "1" : "0");
    out += ";undo-sensitive=" + std::string(Inkscape::DocumentUndo::getUndoSensitive(&document) ? "1" : "0");
    out += ";transaction=" + std::string(document.getReprDoc()->inTransaction() ? "1" : "0");
    out += ";title=" + window.get_title().raw();
    out += ";" + fingerprint(document);
    return out;
}

class NamedViewWindowStateTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        auto const gui = std::getenv("INKSCAPE_TEST_GUI");
        if (!gui || std::string(gui) != "1") {
            GTEST_SKIP() << "GUI testing not enabled";
        }
        // Only a test that passed the gate may touch the application or prefs.
        started = true;

        // Initialize the real application before reading preferences.
        auto &application = testApplication();
        ASSERT_TRUE(application.gtk_app());

        auto *prefs = Inkscape::Preferences::get();
        saved_viewport = prefs->getBool("/options/savedocviewport/value", true);
        saved_geometry = prefs->getInt("/options/savewindowgeometry/value", PREFS_WINDOW_GEOMETRY_NONE);
        prefs_snapshotted = true;

        document = application.document_add(
            SPDocument::createNewDocFromMem(std::string_view{kSvg}));
        ASSERT_TRUE(document);
        desktop = application.createDesktop(document, false, true);
        ASSERT_TRUE(desktop);
        window = desktop->getInkscapeWindow();
        ASSERT_TRUE(window);
        drainMainContext();
    }

    void TearDown() override
    {
        // A skipped SetUp must not initialize preferences or pump events.
        if (!started) {
            return;
        }

        if (document) {
            document->setModifiedSinceSave(false);
        }
        if (desktop) {
            testApplication().destroyDesktop(desktop);
        } else if (document) {
            testApplication().document_close(document);
        }
        desktop = nullptr;
        document = nullptr;
        window = nullptr;
        drainMainContext();

        // Restore preferences only when SetUp actually snapshotted them.
        if (prefs_snapshotted) {
            auto *prefs = Inkscape::Preferences::get();
            prefs->setBool("/options/savedocviewport/value", saved_viewport);
            prefs->setInt("/options/savewindowgeometry/value", saved_geometry);
        }
    }

    SPDocument *document = nullptr;
    SPDesktop *desktop = nullptr;
    InkscapeWindow *window = nullptr;
    bool started = false;
    bool prefs_snapshotted = false;
    bool saved_viewport = true;
    int saved_geometry = 0;
};

// ONE substantive case: capture + apply are read-only for the source while the
// target copy receives the snapshot, and the source's original history survives.
TEST_F(NamedViewWindowStateTest, CaptureAndCopyApplicationAreReadOnly)
{
    auto *prefs = Inkscape::Preferences::get();
    prefs->setBool("/options/savedocviewport/value", true);
    prefs->setInt("/options/savewindowgeometry/value", PREFS_WINDOW_GEOMETRY_FILE);

    // Synthetic source identity; nothing is written to disk.
    document->setDocumentFilename("/synthetic/window-state-fixture.svg");
    document->setDocumentBase("/synthetic");

    // Copy BEFORE the source snapshot. The copy owns an independent namedview and
    // is the only document apply_namedview_window_state() is allowed to touch.
    auto target = document->copy();
    ASSERT_TRUE(target);
    ASSERT_TRUE(target->getNamedView());

    // Select the real layer through the native currentLayer setter.
    auto *layer = document->getObjectById("layer-original");
    ASSERT_TRUE(layer);
    desktop->layerManager().setCurrentLayer(layer);
    EXPECT_STREQ(layer->getId(), "layer-original");

    // Two real edits through the native Undo path, then undo one so a redo entry
    // is retained before the snapshots are taken.
    auto *rect = document->getObjectById("rect");
    ASSERT_TRUE(rect);
    auto *repr = rect->getRepr();
    ASSERT_TRUE(repr);

    repr->setAttribute("x", "20");
    Inkscape::DocumentUndo::done(document, Inkscape::Util::Internal::ContextString("Fixture"), "");
    repr->setAttribute("x", "30");
    Inkscape::DocumentUndo::done(document, Inkscape::Util::Internal::ContextString("Fixture"), "");
    ASSERT_TRUE(Inkscape::DocumentUndo::undo(document));
    EXPECT_STREQ(repr->attribute("x"), "20");

    History history{*document};

    std::string const before = sourceState(*document, *window);

    auto state = capture_namedview_window_state(*desktop);
    ASSERT_TRUE(state.viewport.has_value());
    ASSERT_TRUE(state.geometry.has_value());
    ASSERT_TRUE(state.current_layer_id.has_value());
    EXPECT_EQ(*state.current_layer_id, "layer-original");

    // Capture alone is read-only: the source is untouched before any apply.
    EXPECT_EQ(sourceState(*document, *window), before);

    apply_namedview_window_state(*target->getNamedView(), state);

    // The target copy received the snapshot (including the exact owned layer id).
    EXPECT_STREQ(target->getNamedView()->getRepr()->attribute("inkscape:current-layer"),
                 "layer-original");

    // The source is byte-for-byte and state-for-state unchanged.
    std::string const after = sourceState(*document, *window);
    EXPECT_EQ(before, after);

    // No undo-stack notification fired on the source during capture/apply.
    EXPECT_EQ(history.commits, 0u);
    EXPECT_EQ(history.undos, 0u);
    EXPECT_EQ(history.redos, 0u);
    EXPECT_EQ(history.clears, 0u);
    EXPECT_EQ(history.expires, 0u);

    // The original history is retained: redo -> x30, undo -> x20, undo -> x10.
    ASSERT_TRUE(Inkscape::DocumentUndo::redo(document));
    EXPECT_STREQ(repr->attribute("x"), "30");
    ASSERT_TRUE(Inkscape::DocumentUndo::undo(document));
    EXPECT_STREQ(repr->attribute("x"), "20");
    ASSERT_TRUE(Inkscape::DocumentUndo::undo(document));
    EXPECT_STREQ(repr->attribute("x"), "10");
}

// Native wrapper parity: the enabled capture must reproduce exactly what the
// existing sp_namedview_document_from_window() writes, and applying it to a
// copy must not touch the source.
TEST_F(NamedViewWindowStateTest, EnabledCaptureMatchesNativeWrapper)
{
    auto *prefs = Inkscape::Preferences::get();
    prefs->setBool("/options/savedocviewport/value", true);
    prefs->setInt("/options/savewindowgeometry/value", PREFS_WINDOW_GEOMETRY_FILE);

    // An unrelated namedview attribute is shared by source and copy and must be
    // neither read nor written by capture/apply or the native wrapper.
    auto *source_nv = document->getNamedView();
    ASSERT_TRUE(source_nv);
    source_nv->getRepr()->setAttribute("inkscape:window-x", "123");

    // Copy BEFORE the source snapshot.
    auto target = document->copy();
    ASSERT_TRUE(target);
    auto *target_nv = target->getNamedView();
    ASSERT_TRUE(target_nv);
    auto *target_repr = target_nv->getRepr();
    ASSERT_TRUE(target_repr);
    EXPECT_STREQ(target_repr->attribute("inkscape:window-x"), "123");

    auto *layer = document->getObjectById("layer-original");
    ASSERT_TRUE(layer);
    desktop->layerManager().setCurrentLayer(layer);

    // Drive the real desktop viewport through the native setters.
    desktop->zoom_absolute(Geom::Point(20, 30), 2.25);
    desktop->rotate_absolute_center_point(Geom::Point(20, 30), Geom::rad_from_deg(33.6));

    // Native read getters establish the independent expected values.
    double const expected_zoom = desktop->current_zoom();
    Geom::Point const expected_center = desktop->current_center();
    auto const [expected_w, expected_h] = desktop->getWindowSize();
    long long const expected_maximized = desktop->is_maximized() ? 1 : 0;
    double const expected_rotation =
        std::round(Geom::deg_from_rad(desktop->current_rotation().angle()));
    EXPECT_EQ(static_cast<int>(expected_rotation), 34);

    History history{*document};
    std::string const before = sourceState(*document, *window);

    auto state = capture_namedview_window_state(*desktop);
    ASSERT_TRUE(state.viewport.has_value());
    ASSERT_TRUE(state.geometry.has_value());
    ASSERT_TRUE(state.current_layer_id.has_value());
    EXPECT_EQ(*state.current_layer_id, "layer-original");

    // Capture alone is read-only: the source is untouched before any apply.
    EXPECT_EQ(sourceState(*document, *window), before);

    apply_namedview_window_state(*target_nv, state);

    // Exact owned layer id.
    EXPECT_STREQ(target_repr->attribute("inkscape:current-layer"), "layer-original");

    // The captured state must equal the independently measured native getters.
    ASSERT_TRUE(state.viewport.has_value());
    EXPECT_DOUBLE_EQ(state.viewport->zoom, expected_zoom);
    EXPECT_DOUBLE_EQ(state.viewport->zoom, 2.25);
    EXPECT_DOUBLE_EQ(state.viewport->rotation, expected_rotation);
    EXPECT_DOUBLE_EQ(state.viewport->rotation, 34.0);
    EXPECT_DOUBLE_EQ(state.viewport->current_center.x(), expected_center.x());
    EXPECT_DOUBLE_EQ(state.viewport->current_center.y(), expected_center.y());

    // Native SVG output uses the configured numeric precision, so the raw getter
    // doubles are checked in the captured state above, while the emitted output
    // strings are checked here against native formatting.
    Inkscape::SVGOStringStream zoom_text;
    zoom_text << expected_zoom;
    char const *zoom_attr = target_repr->attribute("inkscape:zoom");
    ASSERT_NE(zoom_attr, nullptr);
    EXPECT_EQ(std::string(zoom_attr), zoom_text.str());
    Inkscape::SVGOStringStream cx_text;
    cx_text << expected_center.x();
    char const *cx_attr = target_repr->attribute("inkscape:cx");
    ASSERT_NE(cx_attr, nullptr);
    EXPECT_EQ(std::string(cx_attr), cx_text.str());
    Inkscape::SVGOStringStream cy_text;
    cy_text << expected_center.y();
    char const *cy_attr = target_repr->attribute("inkscape:cy");
    ASSERT_NE(cy_attr, nullptr);
    EXPECT_EQ(std::string(cy_attr), cy_text.str());
    Inkscape::SVGOStringStream rotation_text;
    rotation_text << expected_rotation;
    char const *rotation_attr = target_repr->attribute("inkscape:rotation");
    ASSERT_NE(rotation_attr, nullptr);
    EXPECT_EQ(std::string(rotation_attr), rotation_text.str());

    // Geometry attributes.
    EXPECT_EQ(parseIntAttribute(target_repr->attribute("inkscape:window-width")), expected_w);
    EXPECT_EQ(parseIntAttribute(target_repr->attribute("inkscape:window-height")), expected_h);
    EXPECT_EQ(parseIntAttribute(target_repr->attribute("inkscape:window-maximized")), expected_maximized);

    // The unrelated sentinel survived the apply.
    EXPECT_STREQ(target_repr->attribute("inkscape:window-x"), "123");

    // Source unchanged by capture/apply.
    EXPECT_EQ(sourceState(*document, *window), before);

    // Native wrapper on the source, then compare both namedviews. This is
    // supplementary to the independent native-getter expectations above.
    sp_namedview_document_from_window(desktop);

    std::string source_fp, target_fp;
    fingerprintNode(source_nv->getRepr(), source_fp);
    fingerprintNode(target_repr, target_fp);
    EXPECT_EQ(source_fp, target_fp);

    // The wrapper's intentional source write is undo-insensitive and raises no
    // history notification; undo sensitivity is restored afterwards.
    EXPECT_EQ(history.commits, 0u);
    EXPECT_EQ(history.undos, 0u);
    EXPECT_EQ(history.redos, 0u);
    EXPECT_EQ(history.clears, 0u);
    EXPECT_EQ(history.expires, 0u);
    EXPECT_TRUE(Inkscape::DocumentUndo::getUndoSensitive(document));
}

// With viewport saving off and geometry NONE/LAST, capture omits both groups;
// apply must keep the target's stored attributes byte-for-byte and still set
// the current layer.
TEST_F(NamedViewWindowStateTest, DisabledPreferencesPreserveStoredAttributes)
{
    auto *layer = document->getObjectById("layer-original");
    ASSERT_TRUE(layer);
    desktop->layerManager().setCurrentLayer(layer);

    History history{*document};

    for (int const geometry_mode : {PREFS_WINDOW_GEOMETRY_NONE, PREFS_WINDOW_GEOMETRY_LAST}) {
        auto *prefs = Inkscape::Preferences::get();
        prefs->setBool("/options/savedocviewport/value", false);
        prefs->setInt("/options/savewindowgeometry/value", geometry_mode);

        auto target = document->copy();
        ASSERT_TRUE(target);
        auto *target_nv = target->getNamedView();
        ASSERT_TRUE(target_nv);
        auto *target_repr = target_nv->getRepr();
        ASSERT_TRUE(target_repr);

        // Explicit sentinels for both disabled groups plus unrelated attrs.
        target_repr->setAttribute("inkscape:zoom", "3.25");
        target_repr->setAttribute("inkscape:rotation", "11");
        target_repr->setAttribute("inkscape:cx", "-12.5");
        target_repr->setAttribute("inkscape:cy", "44.5");
        target_repr->setAttribute("inkscape:window-width", "333");
        target_repr->setAttribute("inkscape:window-height", "444");
        target_repr->setAttribute("inkscape:window-maximized", "1");
        target_repr->setAttribute("inkscape:window-x", "123");
        target_repr->setAttribute("inkscape:window-y", "456");
        target_repr->setAttribute("pagecolor", "#abcdef");
        // Non-owned current layer, so apply must overwrite it.
        target_repr->setAttribute("inkscape:current-layer", "old-id");

        document->setModifiedSinceSave(false);

        std::string const before = sourceState(*document, *window);

        auto state = capture_namedview_window_state(*desktop);
        EXPECT_FALSE(state.viewport.has_value());
        EXPECT_FALSE(state.geometry.has_value());
        ASSERT_TRUE(state.current_layer_id.has_value());
        EXPECT_EQ(*state.current_layer_id, "layer-original");

        // Capture alone is read-only: the source is untouched before any apply.
        EXPECT_EQ(sourceState(*document, *window), before);

        apply_namedview_window_state(*target_nv, state);

        // No pumping between snapshots: the source state is identical.
        EXPECT_EQ(sourceState(*document, *window), before);

        // Every stored sentinel is untouched.
        EXPECT_STREQ(target_repr->attribute("inkscape:zoom"), "3.25");
        EXPECT_STREQ(target_repr->attribute("inkscape:rotation"), "11");
        EXPECT_STREQ(target_repr->attribute("inkscape:cx"), "-12.5");
        EXPECT_STREQ(target_repr->attribute("inkscape:cy"), "44.5");
        EXPECT_STREQ(target_repr->attribute("inkscape:window-width"), "333");
        EXPECT_STREQ(target_repr->attribute("inkscape:window-height"), "444");
        EXPECT_STREQ(target_repr->attribute("inkscape:window-maximized"), "1");
        EXPECT_STREQ(target_repr->attribute("inkscape:window-x"), "123");
        EXPECT_STREQ(target_repr->attribute("inkscape:window-y"), "456");
        EXPECT_STREQ(target_repr->attribute("pagecolor"), "#abcdef");

        // The current layer is always applied and carries the owned id.
        EXPECT_STREQ(target_repr->attribute("inkscape:current-layer"), "layer-original");
    }

    EXPECT_EQ(history.commits, 0u);
    EXPECT_EQ(history.undos, 0u);
    EXPECT_EQ(history.redos, 0u);
    EXPECT_EQ(history.clears, 0u);
    EXPECT_EQ(history.expires, 0u);
    EXPECT_TRUE(Inkscape::DocumentUndo::getUndoSensitive(document));
}

// Removing the source layer id must not regenerate one: the owned id is absent
// from the capture, and apply removes both the stale rotation sentinel and the
// stale current-layer attribute on the target, proving no generated source ID.
TEST_F(NamedViewWindowStateTest, ZeroRotationAndMissingLayerIdRemoveAttributes)
{
    auto *prefs = Inkscape::Preferences::get();
    prefs->setBool("/options/savedocviewport/value", true);
    prefs->setInt("/options/savewindowgeometry/value", PREFS_WINDOW_GEOMETRY_NONE);

    // Valid current layer selected on the source before its id is removed.
    auto *layer = document->getObjectById("layer-original");
    ASSERT_TRUE(layer);
    desktop->layerManager().setCurrentLayer(layer);

    // Copy BEFORE the source id is removed; the copy owns its own binding.
    auto target = document->copy();
    ASSERT_TRUE(target);
    auto *target_nv = target->getNamedView();
    ASSERT_TRUE(target_nv);
    auto *target_repr = target_nv->getRepr();
    ASSERT_TRUE(target_repr);

    // Deliberately strip the bound id. SPObject ID removal must not regenerate.
    layer->getRepr()->setAttribute("id", nullptr);
    EXPECT_EQ(layer->getId(), nullptr);
    auto *current = desktop->layerManager().currentLayer();
    ASSERT_NE(current, nullptr);
    EXPECT_EQ(current, layer);

    // Native viewport rotation to 0 degrees.
    desktop->rotate_absolute_center_point(Geom::Point(20, 30), Geom::rad_from_deg(0.0));

    // Target sentinels that apply must remove or preserve.
    target_repr->setAttribute("inkscape:rotation", "17");
    target_repr->setAttribute("inkscape:current-layer", "old");
    target_repr->setAttribute("inkscape:window-x", "123");

    History history{*document};
    std::string const before = sourceState(*document, *window);

    auto state = capture_namedview_window_state(*desktop);
    ASSERT_TRUE(state.viewport.has_value());
    EXPECT_DOUBLE_EQ(state.viewport->rotation, 0.0);
    EXPECT_FALSE(state.geometry.has_value());
    EXPECT_FALSE(state.current_layer_id.has_value());

    // Capture alone is read-only.
    EXPECT_EQ(sourceState(*document, *window), before);

    apply_namedview_window_state(*target_nv, state);

    // Zero rotation removes the attribute; the absent layer id removes the
    // current-layer attribute; the unrelated window-x sentinel survives.
    EXPECT_EQ(target_repr->attribute("inkscape:rotation"), nullptr);
    EXPECT_EQ(target_repr->attribute("inkscape:current-layer"), nullptr);
    EXPECT_STREQ(target_repr->attribute("inkscape:window-x"), "123");

    // Source XML/identity/history untouched.
    EXPECT_EQ(sourceState(*document, *window), before);
    EXPECT_EQ(layer->getId(), nullptr);

    EXPECT_EQ(history.commits, 0u);
    EXPECT_EQ(history.undos, 0u);
    EXPECT_EQ(history.redos, 0u);
    EXPECT_EQ(history.clears, 0u);
    EXPECT_EQ(history.expires, 0u);
}

// The captured layer id is an owned copy: renaming the source layer after
// capture must not change what the target receives. A second identical apply is
// a full no-op for the target's raw XML, identity and Undo history.
TEST_F(NamedViewWindowStateTest, CapturedLayerIdIsOwnedAndRepeatedApplyIsNoOp)
{
    auto *prefs = Inkscape::Preferences::get();
    prefs->setBool("/options/savedocviewport/value", false);
    prefs->setInt("/options/savewindowgeometry/value", PREFS_WINDOW_GEOMETRY_FILE);

    auto *layer = document->getObjectById("layer-original");
    ASSERT_TRUE(layer);
    desktop->layerManager().setCurrentLayer(layer);

    // Copy BEFORE capture.
    auto target = document->copy();
    ASSERT_TRUE(target);
    auto *target_nv = target->getNamedView();
    ASSERT_TRUE(target_nv);
    auto *target_repr = target_nv->getRepr();
    ASSERT_TRUE(target_repr);

    // Unrelated sentinels; viewport is disabled so zoom stays untouched.
    target_repr->setAttribute("inkscape:window-x", "123");
    target_repr->setAttribute("inkscape:zoom", "3.25");

    std::string const before = sourceState(*document, *window);

    auto state = capture_namedview_window_state(*desktop);
    EXPECT_FALSE(state.viewport.has_value());
    ASSERT_TRUE(state.geometry.has_value());
    ASSERT_TRUE(state.current_layer_id.has_value());
    EXPECT_EQ(*state.current_layer_id, "layer-original");

    // Capture alone is read-only.
    EXPECT_EQ(sourceState(*document, *window), before);

    // Deliberately rename the source layer after capture; the captured value is
    // an owned original and must not follow the rename.
    layer->getRepr()->setAttribute("id", "layer-renamed");
    ASSERT_STREQ(layer->getId(), "layer-renamed");

    // Baseline taken AFTER the intentional rename; never compare across it.
    std::string const source_after_rename = sourceState(*document, *window);

    apply_namedview_window_state(*target_nv, state);
    EXPECT_STREQ(target_repr->attribute("inkscape:current-layer"), "layer-original");
    EXPECT_STREQ(target_repr->attribute("inkscape:window-x"), "123");
    EXPECT_STREQ(target_repr->attribute("inkscape:zoom"), "3.25");

    // Two real undoable edits on the target, then one undo so redo is retained.
    auto *target_rect = target->getObjectById("rect");
    ASSERT_TRUE(target_rect);
    auto *target_rect_repr = target_rect->getRepr();
    ASSERT_TRUE(target_rect_repr);
    target_rect_repr->setAttribute("x", "20");
    Inkscape::DocumentUndo::done(target.get(), Inkscape::Util::Internal::ContextString("Fixture"), "");
    target_rect_repr->setAttribute("x", "30");
    Inkscape::DocumentUndo::done(target.get(), Inkscape::Util::Internal::ContextString("Fixture"), "");
    ASSERT_TRUE(Inkscape::DocumentUndo::undo(target.get()));
    EXPECT_STREQ(target_rect_repr->attribute("x"), "20");

    // A clean target plus a full observable snapshot before the repeated apply.
    target->setModifiedSinceSave(false);
    History target_history{*target};

    auto snapshotTarget = [](SPDocument &doc) {
        std::string out;
        appendField(out, doc.getDocumentFilename());
        appendField(out, doc.getDocumentBase());
        appendField(out, doc.getDocumentName());
        out += "modified=" + std::string(doc.isModifiedSinceSave() ? "1" : "0");
        out += ";undo-sensitive=" + std::string(Inkscape::DocumentUndo::getUndoSensitive(&doc) ? "1" : "0");
        out += ";transaction=" + std::string(doc.getReprDoc()->inTransaction() ? "1" : "0");
        out += ";" + fingerprint(doc);
        return out;
    };
    std::string const target_before = snapshotTarget(*target);

    apply_namedview_window_state(*target_nv, state);

    // Identical state re-apply is a full no-op.
    EXPECT_EQ(snapshotTarget(*target), target_before);
    EXPECT_EQ(target_history.commits, 0u);
    EXPECT_EQ(target_history.undos, 0u);
    EXPECT_EQ(target_history.redos, 0u);
    EXPECT_EQ(target_history.clears, 0u);
    EXPECT_EQ(target_history.expires, 0u);

    // The retained target history survived both applies.
    ASSERT_TRUE(Inkscape::DocumentUndo::redo(target.get()));
    EXPECT_STREQ(target_rect_repr->attribute("x"), "30");
    ASSERT_TRUE(Inkscape::DocumentUndo::undo(target.get()));
    EXPECT_STREQ(target_rect_repr->attribute("x"), "20");
    ASSERT_TRUE(Inkscape::DocumentUndo::undo(target.get()));
    EXPECT_STREQ(target_rect_repr->attribute("x"), "10");

    // The source baseline taken after the deliberate rename is untouched by BOTH
    // target applies.
    EXPECT_EQ(sourceState(*document, *window), source_after_rename);
}

} // namespace
