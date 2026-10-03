// SPDX-License-Identifier: GPL-2.0-or-later
// Real widgets/controllers; signal emission tests handlers, not native event propagation.
#include <gtest/gtest.h>
#include <clocale>
#include <cmath>
#include <memory>
#include <chrono>
#include <thread>
#include <string>
#include <vector>
#include <glibmm/main.h>
#include <giomm/simpleactiongroup.h>
#include <gtkmm/box.h>
#include <gtkmm/builder.h>
#include "document.h"
#include "object/sp-object.h"
#include "document-undo.h"
#include "inkscape.h"
#include "xml/repr.h"
#include <gtkmm/entry.h>
#include <gtkmm/label.h>
#include <gtkmm/window.h>
#include "inkscape-application.h"
#include "ui/widget/gtk-registry.h"
#include "ui/widget/spinbutton.h"
#include "ui/widget/unit-menu.h"

using Inkscape::UI::Widget::InkSpinButton;
using Inkscape::UI::Widget::SpinButton;
namespace {
Gtk::Widget *child(Gtk::Widget &root, char const *name) {
    for (auto *w = root.get_first_child(); w; w = w->get_next_sibling()) {
        if (w->get_name() == name) return w;
        if (auto *found = child(*w, name)) return found;
    }
    return nullptr;
}
// Hold a reference while emitting: handlers can alter widgets/controllers.
GObject *controller(Gtk::Widget &w, GType type) {
    auto *list = gtk_widget_observe_controllers(w.gobj());
    GObject *result = nullptr;
    for (guint i = 0; i < g_list_model_get_n_items(list); ++i) {
        auto *item = G_OBJECT(g_list_model_get_item(list, i));
        if (g_type_is_a(G_OBJECT_TYPE(item), type)) { result = item; break; }
        g_object_unref(item);
    }
    g_object_unref(list);
    return result;
}
void emit(Gtk::Widget &w, GType type, char const *signal) {
    auto *list = gtk_widget_observe_controllers(w.gobj());
    int n = 0;
    for (guint i = 0; i < g_list_model_get_n_items(list); ++i) {
        auto *item = G_OBJECT(g_list_model_get_item(list, i));
        if (g_type_is_a(G_OBJECT_TYPE(item), type)) {
            ++n;
            g_signal_emit_by_name(item, signal);
        }
        g_object_unref(item);
    }
    g_object_unref(list);
    ASSERT_GT(n, 0);
}
bool key(Gtk::Widget &w, guint value, GdkModifierType mods = GdkModifierType(0)) {
    auto *c = controller(w, GTK_TYPE_EVENT_CONTROLLER_KEY);
    EXPECT_NE(c, nullptr);
    gboolean handled = false;
    if (c) {
        GdkKeymapKey *keys = nullptr;
        int count = 0;
        gdk_display_map_keyval(gdk_display_get_default(), value, &keys, &count);
        guint const code = count ? keys[0].keycode : 0;
        g_free(keys);
        g_signal_emit_by_name(c, "key-pressed", value, code, mods, &handled);
        g_object_unref(c);
    }
    return handled;
}
bool wheel(Gtk::Widget &w) {
    auto *c = controller(w, GTK_TYPE_EVENT_CONTROLLER_SCROLL);
    EXPECT_NE(c, nullptr);
    gboolean handled = false;
    if (c) { g_signal_emit_by_name(c, "scroll", 0.0, -5.0, &handled); g_object_unref(c); }
    return handled;
}
void click(InkSpinButton &spin) {
    auto *label = child(spin, "InkSpinButton-Value");
    ASSERT_NE(label, nullptr);
    auto *c = controller(*label, GTK_TYPE_GESTURE_DRAG);
    ASSERT_NE(c, nullptr);
    // No movement: the production drag-end handler's click path.
    g_signal_emit_by_name(c, "end", nullptr);
    g_object_unref(c);
}
Gtk::Entry &entry(InkSpinButton &spin) {
    return *dynamic_cast<Gtk::Entry *>(child(spin, "InkSpinButton-Entry"));
}
void activate(InkSpinButton &spin, std::string const &text) {
    click(spin);
    entry(spin).set_text(text);
    g_signal_emit_by_name(entry(spin).gobj(), "activate");
}
class SpinPolicies : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        static auto *app = [] {
            g_setenv("INKSCAPE_APP_ID_TAG", "spinButtonPoliciesTest", TRUE);
            auto *result = new InkscapeApplication;
            if (result->gtk_app()) Inkscape::UI::Widget::register_all();
            return result;
        }();
        ASSERT_TRUE(app->gtk_app()) << "GTK desktop required";
    }
    void SetUp() override { window.set_child(box); }
    // Only visibility/focus commit routes need a mapped window. Parser,
    // pending-edit and action tests use the existing hidden-window pattern.
    // Mapping failures remain failures; none of these gates are skipped.
    void present() {
        char const *stage = "present";
        try {
            window.present();
            stage = "main-context iteration";
            auto context = Glib::MainContext::get_default();
            while (context->pending()) context->iteration(false);
        } catch (...) {
            ADD_FAILURE() << "GTK window setup failed during " << stage;
            throw;
        }
    }
    void TearDown() override {
        // Drain enter_edit's idle focus requests while the widgets still live.
        auto context = Glib::MainContext::get_default();
        while (context->pending()) context->iteration(false);
        for (auto &s : spins) box.remove(*s);
        spins.clear();
        window.unset_child();
    }
    template <typename T> T &make() {
        auto p = std::make_unique<T>();
        auto &s = *p;
        s.set_adjustment(Gtk::Adjustment::create(50, 0, 1000, 1, 10));
        s.set_digits(3);
        box.append(s);
        spins.push_back(std::move(p));
        return s;
    }
    Gtk::Window window;
    Gtk::Box box;
    std::vector<std::unique_ptr<InkSpinButton>> spins;
};
void hover(InkSpinButton &s) {
    auto *c = controller(s, GTK_TYPE_EVENT_CONTROLLER_MOTION);
    ASSERT_NE(c, nullptr);
    g_signal_emit_by_name(c, "enter", 0.0, 0.0);
    g_object_unref(c);
}
void default_behavior(InkSpinButton &s) {
    hover(s);
    EXPECT_TRUE(child(s, "InkSpinButton-Plus")->get_visible());
    EXPECT_TRUE(child(s, "InkSpinButton-Minus")->get_visible());
    EXPECT_TRUE(wheel(s));
    EXPECT_GT(s.get_value(), 50);
    s.set_value(50);
    click(s);
    EXPECT_TRUE(entry(s).get_visible());
    EXPECT_TRUE(key(entry(s), GDK_KEY_Up)); EXPECT_EQ(s.get_value(), 51);
    EXPECT_TRUE(key(entry(s), GDK_KEY_Down)); EXPECT_EQ(s.get_value(), 50);
    EXPECT_TRUE(key(entry(s), GDK_KEY_Page_Up)); EXPECT_EQ(s.get_value(), 60);
    EXPECT_TRUE(key(entry(s), GDK_KEY_Page_Down)); EXPECT_EQ(s.get_value(), 50);
    entry(s).set_text("2.5");
    emit(s, GTK_TYPE_EVENT_CONTROLLER_FOCUS, "leave");
    EXPECT_EQ(s.get_value(), 2.5);
    s.set_mixed();
    EXPECT_EQ(entry(s).get_text(), "");
    // Compare UTF-8 bytes: ustring == const char* collates with the process
    // locale, which fails for U+2014 on Windows although the text is right.
    EXPECT_EQ(dynamic_cast<Gtk::Label *>(child(s, "InkSpinButton-Value"))->get_text().raw(),
              std::string("\xE2\x80\x94"));
    click(s);
    EXPECT_TRUE(key(entry(s), GDK_KEY_Up));
    EXPECT_FALSE(s.is_mixed());
    EXPECT_EQ(s.get_value(), 3.5);
}


// Preserve LC_NUMERIC even on ASSERT/GTEST_SKIP.
class NumericLocale {
public:
    NumericLocale() : saved(std::setlocale(LC_NUMERIC, nullptr)) {}
    ~NumericLocale() { std::setlocale(LC_NUMERIC, saved.c_str()); }
    bool select(bool comma) {
        for (auto name : comma ? std::vector<char const *>{"es_ES.UTF-8", "es_ES", "de_DE.UTF-8", "de_DE", "Spanish_Spain.1252"}
                               : std::vector<char const *>{"en_US.UTF-8", "en_US", "C"}) {
            if (std::setlocale(LC_NUMERIC, name) &&
                std::string(std::localeconv()->decimal_point) == (comma ? "," : ".")) return true;
        }
        return false;
    }
private:
    std::string saved;
};
void decimal_matrix(InkSpinButton &s) {
    for (auto text : {"0.5", ".5", "0,5", ",5"}) {
        SCOPED_TRACE(text);
        s.set_value(50);
        activate(s, text);
        EXPECT_DOUBLE_EQ(s.get_value(), 0.5);
        EXPECT_EQ(entry(s).get_text(), "0.5");
    }
    // Previously accepted inputs, including signs, exponents, expressions and
    // hex, must retain the evaluator's numerical result.
    for (auto const &[text, expected] : std::vector<std::pair<std::string, double>>{
             {"1,25", 1.25}, {"1,5", 1.5}, {"10,50", 10.5}, {"1,0000", 1.0},
             {"2.5", 2.5}, {"+2.5", 2.5}, {" 2.5 ", 2.5}, {"2e1", 20},
             {"2+3*4", 14}, {"2*(3+4)", 14}, {"2.5/2", 1.25}, {"0x10", 16}}) {
        SCOPED_TRACE(text);
        activate(s, text);
        EXPECT_DOUBLE_EQ(s.get_value(), expected);
    }
}
void rejected_comma_matrix(InkSpinButton &s, bool numeric_only = false) {
    s.set_dont_evaluate(numeric_only);
    s.get_adjustment()->set_upper(2000); // 1234,567 must parse without clamping.
    for (bool validation : {false, true}) {
        SCOPED_TRACE(validation);
        s.set_input_validation(validation);
        // The ambiguity check precedes all evaluators, including numeric-only
        // parsing, and must reject signed and unit-suffixed input globally.
        for (auto text : {"1,000", "12,345", "123,456", "1,234,567", "-1,000",
                          "10,500 mm", "1,000 mm", " \t+12,345 ", "1,000mm"}) {
            SCOPED_TRACE(text);
            s.set_value(50);
            click(s);
            entry(s).set_text(text);
            int invalid = 0;
            auto c = s.signal_invalid_text().connect([&] { ++invalid; });
            EXPECT_EQ(s.commit_pending_edit(), InkSpinButton::CommitResult::Invalid);
            EXPECT_DOUBLE_EQ(s.get_value(), 50);
            EXPECT_EQ(invalid, validation ? 1 : 0);
            EXPECT_EQ(entry(s).get_text(), text);
            c.disconnect();
            s.discard_pending_edit();
        }
        for (auto const &[text, expected] : std::vector<std::pair<std::string, double>>{
                 {"0,125", 0.125}, {",125", 0.125}, {"1234,567", 1234.567},
                 {"1,5", 1.5}, {"10,50", 10.5}, {"0,5", 0.5}, {",5", 0.5},
                 {",500", 0.5}, {"01,000", 1.0}, {"1,000e2", 100.0},
                 {"1,000+2", numeric_only ? 1.0 : 3.0}}) {
            SCOPED_TRACE(text);
            s.set_value(50);
            click(s);
            entry(s).set_text(text);
            bool const invalid_prefix = numeric_only && validation && text == "1,000+2";
            EXPECT_EQ(s.commit_pending_edit(), invalid_prefix
                      ? InkSpinButton::CommitResult::Invalid : InkSpinButton::CommitResult::Committed);
            EXPECT_DOUBLE_EQ(s.get_value(), invalid_prefix ? 50.0 : expected);
            s.discard_pending_edit();
        }
        // Point-containing input still follows the existing parser policies.
        s.set_value(50);
        click(s);
        entry(s).set_text("1,000.5");
        EXPECT_EQ(s.commit_pending_edit(), numeric_only && !validation
                  ? InkSpinButton::CommitResult::Committed : InkSpinButton::CommitResult::Invalid);
        EXPECT_DOUBLE_EQ(s.get_value(), numeric_only && !validation ? 1.0 : 50.0);
        s.discard_pending_edit();
        if (numeric_only) continue; // Legacy numeric-prefix parsing is unchanged.
        for (auto text : {"1,2,3", "1.000,5"}) {
            SCOPED_TRACE(text);
            s.set_value(50);
            click(s);
            entry(s).set_text(text);
            EXPECT_EQ(s.commit_pending_edit(), InkSpinButton::CommitResult::Invalid);
            EXPECT_DOUBLE_EQ(s.get_value(), 50);
            s.discard_pending_edit();
        }
    }
}
TEST_F(SpinPolicies, E09_PointDecimalInputRegression) {
    NumericLocale locale;
    if (!locale.select(false)) GTEST_SKIP() << "Missing point LC_NUMERIC: en_US.UTF-8, en_US, C";
    decimal_matrix(make<InkSpinButton>());
    decimal_matrix(make<SpinButton>());
}
TEST_F(SpinPolicies, E09_CommaDecimalInputRegression) {
    NumericLocale locale;
    if (!locale.select(true)) GTEST_SKIP() << "Missing comma LC_NUMERIC: es_ES.UTF-8, es_ES, de_DE.UTF-8, de_DE, Spanish_Spain.1252";
    decimal_matrix(make<InkSpinButton>());
    decimal_matrix(make<SpinButton>());
}
TEST_F(SpinPolicies, E09_AmbiguousCommasBothLocalesAndValidationPolicies) {
    NumericLocale locale;
    for (bool comma : {false, true}) {
        SCOPED_TRACE(comma);
        if (!locale.select(comma)) GTEST_SKIP() << "Missing " << (comma ? "comma es_ES/de_DE" : "point en_US/C") << " LC_NUMERIC";
        rejected_comma_matrix(make<InkSpinButton>());
        rejected_comma_matrix(make<SpinButton>());
        rejected_comma_matrix(make<InkSpinButton>(), true);
    }
}
TEST_F(SpinPolicies, E09_UnitAwareBothLocales) {
    NumericLocale locale;
    for (bool comma : {false, true}) {
        if (!locale.select(comma)) GTEST_SKIP() << "Missing " << (comma ? "comma (es_ES/de_DE/Spanish_Spain.1252)" : "point (en_US/C)") << " LC_NUMERIC";
        Inkscape::UI::Widget::UnitMenu units;
        units.setUnitType(Inkscape::Util::UNIT_TYPE_LINEAR);
        units.setUnit("mm");
        auto &s = make<SpinButton>();
        s.setUnitMenu(&units);
        rejected_comma_matrix(s);
        for (auto const &[text, expected] : std::vector<std::pair<std::string, double>>{
                 {"1,25 mm", 1.25}, {"0,125 mm", 0.125}, {",125 mm", 0.125},
                 {"1234,567 mm", 1234.567}, {"1.25 mm", 1.25}, {"2.54 cm", 25.4}, {"1 in", 25.4}}) {
            SCOPED_TRACE(text);
            activate(s, text);
            EXPECT_NEAR(s.get_value(), expected, 1e-10);
        }
        s.setUnitMenu(nullptr);
    }
}
TEST_F(SpinPolicies, E09_NumericOnlyBothSeparatorsAndFailureSignal) {
    NumericLocale locale;
    for (bool comma : {false, true}) {
        if (!locale.select(comma)) GTEST_SKIP() << "Missing " << (comma ? "comma es_ES/de_DE" : "point en_US/C") << " LC_NUMERIC";
        auto &s = make<InkSpinButton>();
        s.set_dont_evaluate(true);
        s.set_input_validation();
        int invalid = 0;
        auto connection = s.signal_invalid_text().connect([&] { ++invalid; });
        for (auto text : {"0.5", ".5", "0,5", ",5"}) {
            activate(s, text);
            EXPECT_DOUBLE_EQ(s.get_value(), 0.5);
        }
        activate(s, "abc");
        EXPECT_EQ(invalid, 1);
        EXPECT_DOUBLE_EQ(s.get_value(), 0.5);
        connection.disconnect();
    }
}

TEST_F(SpinPolicies, B04_PendingMixedDiscardAndProgrammaticRefresh) {
    auto &s = make<SpinButton>();
    s.set_commit_only_if_changed();
    s.set_input_validation();
    s.set_mixed();
    click(s);
    int invalid = 0;
    auto c = s.signal_invalid_text().connect([&] { ++invalid; });
    EXPECT_FALSE(s.has_pending_edit());
    EXPECT_EQ(s.commit_pending_edit(), InkSpinButton::CommitResult::NoEdit);
    EXPECT_TRUE(s.is_mixed());
    entry(s).set_text("50");
    EXPECT_TRUE(s.has_pending_edit());
    // A genuine typed commit of the hidden value resolves Mixed once.
    EXPECT_EQ(s.commit_pending_edit(), InkSpinButton::CommitResult::Committed);
    EXPECT_FALSE(s.is_mixed());
    EXPECT_FALSE(s.has_pending_edit());
    s.set_mixed(); click(s);
    entry(s).set_text("abc"); entry(s).set_text("");
    EXPECT_FALSE(s.has_pending_edit());
    entry(s).set_text("2.5");
    s.discard_pending_edit();
    EXPECT_TRUE(s.is_mixed());
    EXPECT_EQ(s.get_value(), 50);
    EXPECT_FALSE(entry(s).get_visible());
    EXPECT_FALSE(s.has_pending_edit());
    click(s); entry(s).set_text("2.5");
    s.set_value(4.12345); // widget refresh is never a user edit
    EXPECT_FALSE(s.has_pending_edit());
    EXPECT_EQ(invalid, 0);
    c.disconnect();
}

TEST_F(SpinPolicies, B05_ValidationOffKeepsLegacyClampAndSuppressesFailureSignal) {
    auto &s = make<SpinButton>();
    activate(s, "-2"); EXPECT_EQ(s.get_value(), 0);
    activate(s, "5000000"); EXPECT_EQ(s.get_value(), 1000);
    int invalid = 0;
    auto c = s.signal_invalid_text().connect([&] { ++invalid; });
    activate(s, "abc");
    EXPECT_EQ(s.get_value(), 1000); EXPECT_EQ(invalid, 0);
    c.disconnect();
}
TEST_F(SpinPolicies, B05_AdjustmentAndCallerMaximumAfterTransform) {
    auto &s = make<InkSpinButton>();
    s.set_input_validation();
    s.set_input_maximum(100);
    s.set_transformers([](double v) { return v * 10; }, [](double v) { return v / 10; });
    activate(s, "10"); EXPECT_EQ(s.get_value(), 100);
    click(s); entry(s).set_text("10.01");
    EXPECT_EQ(s.commit_pending_edit(), InkSpinButton::CommitResult::Invalid);
    EXPECT_EQ(s.get_value(), 100);
    s.get_adjustment()->set_upper(10000);
    EXPECT_EQ(s.commit_pending_edit(), InkSpinButton::CommitResult::Invalid);
    s.set_input_maximum(20000);
    s.get_adjustment()->set_upper(100);
    EXPECT_EQ(s.commit_pending_edit(), InkSpinButton::CommitResult::Invalid);
    s.get_adjustment()->set_lower(20);
    entry(s).set_text("1");
    EXPECT_EQ(s.commit_pending_edit(), InkSpinButton::CommitResult::Invalid);
}
TEST_F(SpinPolicies, B05_NumericOnlyStrictValidationAndDefaultPrefixParsing) {
    auto &s = make<InkSpinButton>();
    s.set_dont_evaluate(true);
    activate(s, "2tail"); // stod's historical numeric-prefix behavior
    EXPECT_EQ(s.get_value(), 2);
    s.set_input_validation();
    for (auto text : {"2tail", "1,2,3", "1.2.3"}) {
        click(s); entry(s).set_text(text);
        EXPECT_EQ(s.commit_pending_edit(), InkSpinButton::CommitResult::Invalid);
        EXPECT_EQ(s.get_value(), 2);
        EXPECT_TRUE(entry(s).get_visible());
    }
    entry(s).set_text("2.5 ");
    EXPECT_EQ(s.commit_pending_edit(), InkSpinButton::CommitResult::Committed);
    EXPECT_EQ(s.get_value(), 2.5);
}
TEST_F(SpinPolicies, B05_TransformerCannotHideNegativeOrNonfiniteInput) {
    auto &s = make<InkSpinButton>();
    s.set_input_validation();
    s.set_transformers([](double value) { return std::isfinite(value) ? std::abs(value) : 1; }, {});
    for (auto text : {"-2", "1e999"}) {
        click(s); entry(s).set_text(text);
        EXPECT_EQ(s.commit_pending_edit(), InkSpinButton::CommitResult::Invalid);
        EXPECT_EQ(s.get_value(), 50);
    }
}
TEST_F(SpinPolicies, B05_AdjustmentPageSizeIsAnEffectiveLimit) {
    auto &s = make<InkSpinButton>();
    s.set_input_validation();
    s.get_adjustment()->set_page_size(100);
    click(s); entry(s).set_text("950");
    EXPECT_EQ(s.commit_pending_edit(), InkSpinButton::CommitResult::Invalid);
    EXPECT_EQ(s.get_value(), 50);
    entry(s).set_text("900");
    EXPECT_EQ(s.commit_pending_edit(), InkSpinButton::CommitResult::Committed);
    EXPECT_EQ(s.get_value(), 900);
}
TEST_F(SpinPolicies, B05_InkEvaluatorEmptyAndNonfiniteStayOpen) {
    auto &s = make<InkSpinButton>();
    s.set_input_validation();
    for (auto text : {"", " ", "1e999", "-2", "1001"}) {
        SCOPED_TRACE(text);
        click(s); entry(s).set_text(text);
        EXPECT_EQ(s.commit_pending_edit(), InkSpinButton::CommitResult::Invalid);
        EXPECT_EQ(s.get_value(), 50);
        EXPECT_EQ(entry(s).get_text(), text);
        EXPECT_TRUE(entry(s).get_visible());
    }
}
TEST_F(SpinPolicies, B11_KeySteppingOnAndOffAllModifiersAndMixed) {
    for (bool enabled : {false, true}) for (bool mixed : {false, true}) {
        auto &s = make<SpinButton>();
        s.set_key_stepping(enabled);
        for (auto mods : {GdkModifierType(0), GDK_SHIFT_MASK, GDK_CONTROL_MASK, GDK_ALT_MASK, GDK_META_MASK}) {
            for (auto value : {GDK_KEY_Up, GDK_KEY_KP_Up, GDK_KEY_Down, GDK_KEY_KP_Down, GDK_KEY_Page_Up, GDK_KEY_Page_Down}) {
                SCOPED_TRACE(enabled);
                SCOPED_TRACE(value);
                SCOPED_TRACE(mods);
                s.set_value(50); s.set_mixed(mixed); click(s);
                EXPECT_EQ(key(entry(s), value, mods), enabled);
                EXPECT_EQ(s.is_mixed(), !enabled && mixed);
                if (enabled) EXPECT_NE(s.get_value(), 50); else EXPECT_EQ(s.get_value(), 50);
            }
        }
        // Wrapper's optional increment path also respects the policy.
        s.set_increment(2);
        s.set_value(50);
        EXPECT_EQ(key(s, GDK_KEY_Up), enabled);
        EXPECT_EQ(s.get_value(), enabled ? 52 : 50);
    }
}
TEST_F(SpinPolicies, B12_NoDragWheelArrowsKeepsClickAndStopsSpinning) {
    auto &s = make<InkSpinButton>();
    s.set_drag_sensitivity(0);
    s.set_has_arrows(false);
    hover(s);
    EXPECT_FALSE(child(s, "InkSpinButton-Plus")->get_visible());
    EXPECT_FALSE(child(s, "InkSpinButton-Minus")->get_visible());
    EXPECT_FALSE(wheel(s)); EXPECT_EQ(s.get_value(), 50);
    auto *label = child(s, "InkSpinButton-Value");
    auto *drag = controller(*label, GTK_TYPE_GESTURE_DRAG);
    g_signal_emit_by_name(drag, "begin", nullptr);
    g_signal_emit_by_name(drag, "update", nullptr);
    g_signal_emit_by_name(drag, "end", nullptr);
    g_object_unref(drag);
    EXPECT_EQ(s.get_value(), 50);
    EXPECT_TRUE(entry(s).get_visible());
    // With the arrows hidden, click-edit/disabled drag must start no timer.
    auto context = Glib::MainContext::get_default();
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(550);
    while (std::chrono::steady_clock::now() < deadline) {
        while (context->pending()) context->iteration(false);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_EQ(s.get_value(), 50);
}
TEST_F(SpinPolicies, B09_FieldUndoDefaultRestoresFocusValue) {
    auto &s = make<SpinButton>();
    emit(s, GTK_TYPE_EVENT_CONTROLLER_FOCUS, "enter");
    s.set_value(60);
    EXPECT_TRUE(key(s, GDK_KEY_z, GDK_CONTROL_MASK));
    EXPECT_EQ(s.get_value(), 50);
}
TEST_F(SpinPolicies, B09_FieldUndoOptOutForwardsDocumentActionsAndDiscards) {
    if (!Inkscape::Application::exists()) Inkscape::Application::create(false);
    auto doc = SPDocument::createNewDocFromMem(R"(<svg xmlns="http://www.w3.org/2000/svg"><path id="p" d="M0,0L1,1"/></svg>)");
    ASSERT_TRUE(doc);
    doc->ensureUpToDate();
    window.insert_action_group("doc", doc->getActionGroup());
    auto before = sp_repr_save_buf(doc->getReprDoc());
    doc->getObjectById("p")->getRepr()->setAttribute("stroke-width", "2");
    Inkscape::DocumentUndo::done(doc.get(), Inkscape::Util::Internal::ContextString("Test change"), "");
    auto after = sp_repr_save_buf(doc->getReprDoc());
    ASSERT_NE(before, after);
    auto &s = make<SpinButton>();
    s.set_field_undo(false);
    int changes = 0;
    auto c = s.signal_value_changed().connect([&] { ++changes; });
    for (bool mixed : {false, true}) for (bool pending : {false, true}) {
        for (auto mods : {GDK_CONTROL_MASK,
#ifdef __APPLE__
                          GDK_META_MASK,
#endif
                          }) {
            s.set_mixed(mixed); click(s);
            if (pending) entry(s).set_text("2.5");
            EXPECT_TRUE(key(s, GDK_KEY_z, mods));
            EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()), before);
            EXPECT_FALSE(s.has_pending_edit());
            EXPECT_EQ(s.is_mixed(), mixed);
            EXPECT_TRUE(key(s, GDK_KEY_z, GdkModifierType(mods | GDK_SHIFT_MASK)));
            EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()), after);
            EXPECT_TRUE(key(s, GDK_KEY_z, mods));
            EXPECT_TRUE(key(s, GDK_KEY_y, mods));
            EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()), after);
        }
    }
    EXPECT_TRUE(key(s, GDK_KEY_Undo));
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()), before);
    EXPECT_TRUE(key(s, GDK_KEY_Redo));
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()), after);
    EXPECT_EQ(changes, 0);
    c.disconnect();
    window.insert_action_group("doc", {});
}
TEST_F(SpinPolicies, DestroyBeforeDeferredEditFocusIsSafe) {
    auto &s = make<InkSpinButton>();
    click(s);
    ASSERT_TRUE(entry(s).get_visible());
    box.remove(s);
    spins.pop_back();
    auto context = Glib::MainContext::get_default();
    while (context->pending()) context->iteration(false);
}
// Keep tests requiring native window presentation together, after the direct
// handler tests. A failed native presentation must not invalidate their evidence.
TEST_F(SpinPolicies, B22_InkDefaults) { present(); default_behavior(make<InkSpinButton>()); }
TEST_F(SpinPolicies, B22_WrapperDefaults) { present(); default_behavior(make<SpinButton>()); }
TEST_F(SpinPolicies, B03_ChangedOnlyEveryCommitRouteOnAndOff) {
    present();
    for (bool policy : {false, true}) for (int route : {0, 1, 2, 3}) {
        SCOPED_TRACE(policy);
        SCOPED_TRACE(route);
        auto &s = make<InkSpinButton>();
        s.set_commit_only_if_changed(policy);
        s.set_value(0.006944444444444444);
        window.unset_focus();
        click(s);
        EXPECT_FALSE(s.has_pending_edit());
        auto commit = [&] {
            switch (route) {
                case 0: emit(s, GTK_TYPE_EVENT_CONTROLLER_FOCUS, "leave"); break;
                case 1: EXPECT_TRUE(key(entry(s), GDK_KEY_Return)); break;
                case 2: emit(s, GTK_TYPE_EVENT_CONTROLLER_MOTION, "leave"); break;
                case 3: g_signal_emit_by_name(entry(s).gobj(), "activate"); break;
            }
        };
        commit();
        EXPECT_DOUBLE_EQ(s.get_value(), policy ? 0.006944444444444444 : 0.007);
        click(s);
        entry(s).set_text("2.5");
        EXPECT_TRUE(s.has_pending_edit());
        commit();
        EXPECT_DOUBLE_EQ(s.get_value(), 2.5);
        EXPECT_FALSE(s.has_pending_edit());
    }
}
TEST_F(SpinPolicies, B05_ValidationRejectsWithoutClampingEveryRoute) {
    present();
    for (int route : {0, 1, 2, 3, 4}) for (auto text : {"abc", "", ".", "1e999", "-2", "5000000", "1.2.3", "1,2,3", "1.000,5"}) {
        SCOPED_TRACE(route);
        SCOPED_TRACE(text);
        auto &s = make<SpinButton>();
        s.set_input_validation();
        s.set_commit_only_if_changed();
        window.unset_focus();
        click(s);
        entry(s).set_text(text);
        int invalid = 0;
        auto c = s.signal_invalid_text().connect([&] { ++invalid; });
        switch (route) {
            case 0: EXPECT_EQ(s.commit_pending_edit(), InkSpinButton::CommitResult::Invalid); break;
            case 1: emit(s, GTK_TYPE_EVENT_CONTROLLER_FOCUS, "leave"); break;
            case 2: EXPECT_TRUE(key(entry(s), GDK_KEY_Return)); break;
            case 3: emit(s, GTK_TYPE_EVENT_CONTROLLER_MOTION, "leave"); break;
            case 4: g_signal_emit_by_name(entry(s).gobj(), "activate"); break;
        }
        EXPECT_EQ(s.get_value(), 50);
        EXPECT_EQ(invalid, 1);
        EXPECT_TRUE(entry(s).get_visible());
        EXPECT_EQ(entry(s).get_text(), text);
        EXPECT_TRUE(s.has_pending_edit());
        c.disconnect();
        s.discard_pending_edit();
    }
}
// Native drag offsets and GTK propagation cannot be created by signal emission.
// Supervisor desktop-session gate must cover B22 drag stepping, B12 real
// drags/panel scrolling and B09 native Ctrl/Cmd routing before qualification.
} // namespace
