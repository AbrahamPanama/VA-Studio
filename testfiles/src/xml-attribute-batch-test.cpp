// SPDX-License-Identifier: GPL-2.0-or-later
#include <gtest/gtest.h>
#include <functional>
#include <memory>
#include <string>
#include "xml/repr.h"
#include "xml/node-observer.h"
#include "xml/event-fns.h"
#include "preferences.h"

namespace {
namespace XML = Inkscape::XML;
using Shared = Inkscape::Util::ptr_shared;
auto document()
{
    return std::shared_ptr<XML::Document>(sp_repr_read_buf("<svg><g id='art'/></svg>", SP_SVG_NS_URI));
}
auto hints()
{
    return std::vector<XML::Node::AttributeUpdate>{
        {"inkscape:export-filename", "art.png"},
        {"inkscape:export-xdpi", "300"},
        {"inkscape:export-ydpi", "300"}};
}
void expect_hints(XML::Node &node)
{
    EXPECT_STREQ(node.attribute("inkscape:export-filename"), "art.png");
    EXPECT_STREQ(node.attribute("inkscape:export-xdpi"), "300");
    EXPECT_STREQ(node.attribute("inkscape:export-ydpi"), "300");
}
struct Observer : XML::NodeObserver {
    XML::Node &node;
    std::function<void(XML::Node &, GQuark, Shared, Shared)> callback;
    unsigned calls = 0;
    explicit Observer(XML::Node &node) : node(node) { node.addObserver(*this); }
    ~Observer() override { node.removeObserver(*this); }
    void notifyAttributeChanged(XML::Node &n, GQuark key, Shared before, Shared after) override {
        ++calls;
        if (callback) callback(n, key, before, after);
    }
};
using Log = std::unique_ptr<XML::Event, decltype(&sp_repr_free_log)>;

TEST(XmlAttributeBatch, ObserversSeeCompleteTupleAndUndoRedoRestoresIt)
{
    auto doc = document(); auto node = doc->root()->firstChild();
    auto original = sp_repr_save_buf(doc.get());
    Observer observer(*node);
    observer.callback = [](auto &n, auto, auto, auto) { expect_hints(n); };
    doc->beginTransaction();
    EXPECT_TRUE(node->setAttributesAtomically(hints()));
    EXPECT_EQ(observer.calls, 3u);
    auto updated = sp_repr_save_buf(doc.get());
    Log log(doc->commitUndoable(), sp_repr_free_log); ASSERT_TRUE(log);
    observer.callback = {};
    sp_repr_undo_log(log.get());
    EXPECT_EQ(sp_repr_save_buf(doc.get()), original);
    sp_repr_replay_log(log.get());
    EXPECT_EQ(sp_repr_save_buf(doc.get()), updated);
}

TEST(XmlAttributeBatch, NestedCommitCannotSplitTupleOrConsumeFreshTransaction)
{
    auto doc = document(); auto node = doc->root()->firstChild();
    Log first(nullptr, sp_repr_free_log);
    Observer observer(*node);
    bool committed = false;
    observer.callback = [&](auto &n, auto, auto, auto) {
        if (committed) return;
        committed = true;
        expect_hints(n);
        first.reset(doc->commitUndoable());
        doc->beginTransaction();
        n.setAttribute("fresh", "preserve");
    };
    doc->beginTransaction();
    EXPECT_TRUE(node->setAttributesAtomically(hints()));
    Log fresh(doc->commitUndoable(), sp_repr_free_log);
    ASSERT_TRUE(first); ASSERT_TRUE(fresh);
    sp_repr_undo_log(fresh.get());
    EXPECT_EQ(node->attribute("fresh"), nullptr);
    expect_hints(*node);
    sp_repr_undo_log(first.get());
    for (auto const &hint : hints()) EXPECT_EQ(node->attribute(hint.name.c_str()), nullptr);
    sp_repr_replay_log(first.get()); expect_hints(*node);
    sp_repr_replay_log(fresh.get()); EXPECT_STREQ(node->attribute("fresh"), "preserve");
}

TEST(XmlAttributeBatch, RemoveReattachAndCollectionRetainCompletePublication)
{
    auto doc = document(); auto parent = doc->root(); auto node = parent->firstChild();
    Observer observer(*node);
    bool rebound = false;
    observer.callback = [&](auto &n, auto, auto, auto) {
        if (rebound) return;
        rebound = true;
        parent->removeChild(&n);
        Inkscape::GC::Core::gcollect();
        expect_hints(n);
        parent->appendChild(&n);
        expect_hints(*parent->firstChild());
    };
    EXPECT_TRUE(node->setAttributesAtomically(hints()));
    EXPECT_EQ(parent->firstChild(), node);
    expect_hints(*node);
    EXPECT_EQ(observer.calls, 3u);
}

TEST(XmlAttributeBatch, NestedOverwriteDoesNotReceiveStalePendingValue)
{
    auto doc = document(); auto node = doc->root()->firstChild();
    Observer observer(*node);
    bool changed = false;
    std::vector<std::string> y_events;
    observer.callback = [&](auto &n, auto key, auto, auto after) {
        if (std::string(g_quark_to_string(key)) == "inkscape:export-ydpi") y_events.emplace_back(after);
        if (!changed) {
            changed = true;
            expect_hints(n);
            n.setAttribute("inkscape:export-ydpi", "600");
        }
    };
    EXPECT_TRUE(node->setAttributesAtomically(hints()));
    EXPECT_STREQ(node->attribute("inkscape:export-ydpi"), "600");
    EXPECT_EQ(y_events, (std::vector<std::string>{"600"}));
}

TEST(XmlAttributeBatch, NoopEmitsNeitherEventsNorMutationCompletion)
{
    auto doc = document(); auto node = doc->root()->firstChild();
    node->setAttributesAtomically(hints());
    Observer observer(*node);
    unsigned completions = 0;
    auto connection = doc->signalMutationFinished().connect([&] { ++completions; });
    doc->beginTransaction();
    EXPECT_FALSE(node->setAttributesAtomically(hints()));
    EXPECT_FALSE(node->setAttributesAtomically({}));
    EXPECT_FALSE(node->setAttributesAtomically({{"absent", std::nullopt}}));
    EXPECT_EQ(observer.calls, 0u); EXPECT_EQ(completions, 0u);
    Log log(doc->commitUndoable(), sp_repr_free_log); EXPECT_FALSE(log);
    connection.disconnect();
}

TEST(XmlAttributeBatch, InvalidInputRefusesBeforeAnyPublication)
{
    auto doc = document(); auto node = doc->root()->firstChild();
    auto original = sp_repr_save_buf(doc.get());
    Observer observer(*node);
    for (auto bad : {XML::Node::AttributeUpdate{"", "x"}, {"bad name", "x"},
                    {"bad=name", "x"}, {"1starts-with-digit", "x"}, {"a:b:c", "x"},
                    {std::string("bad\xff", 4), "x"},
                    {std::string("bad\0name", 8), "x"}, {"name", std::string("bad\0value", 9)},
                    {"inkscape:export-filename", "duplicate"}}) {
        auto updates = hints(); updates.push_back(bad);
        EXPECT_THROW(node->setAttributesAtomically(std::move(updates)), std::invalid_argument);
        EXPECT_EQ(sp_repr_save_buf(doc.get()), original);
    }
    EXPECT_EQ(observer.calls, 0u);
}

TEST(XmlAttributeBatch, MixedAdditionRemovalAndReplacement)
{
    auto doc = document(); auto node = doc->root()->firstChild();
    node->setAttribute("remove", "old"); node->setAttribute("replace", "before");
    Observer observer(*node);
    observer.callback = [](auto &n, auto, auto, auto) {
        EXPECT_EQ(n.attribute("remove"), nullptr);
        EXPECT_STREQ(n.attribute("replace"), "after");
        EXPECT_STREQ(n.attribute("add"), "new");
        EXPECT_STREQ(n.attribute("id"), "art");
    };
    EXPECT_TRUE(node->setAttributesAtomically({{"remove", std::nullopt}, {"replace", "after"}, {"add", "new"}}));
    EXPECT_EQ(observer.calls, 3u);
}

TEST(XmlAttributeBatch, SavedEventStringsSurviveEqualNestedReplacementAndCollection)
{
    auto doc = document(); auto node = doc->root()->firstChild();
    std::vector<XML::Node::AttributeUpdate> updates;
    std::vector<std::string> old;
    for (unsigned i = 0; i < 4; ++i) {
        auto key = "data-batch-" + std::to_string(i);
        old.emplace_back(8192, 'a' + i);
        node->setAttribute(key, old.back());
        updates.push_back({key, std::string(8192, 'A' + i)});
    }
    Observer observer(*node);
    bool replaced = false;
    observer.callback = [&](auto &n, auto key, auto before, auto after) {
        if (!replaced) {
            replaced = true;
            // Equal single-attribute writes replace the string storage without
            // emitting attribute events. No transaction/log retains the batch's
            // new strings here; pending Change records must retain them safely.
            for (auto const &update : updates) n.setAttribute(update.name, *update.value);
            Inkscape::GC::Core::gcollect();
        }
        auto index = unsigned(g_quark_to_string(key)[11] - '0');
        ASSERT_LT(index, updates.size());
        EXPECT_EQ(std::string(before), old[index]);
        EXPECT_EQ(std::string(after), *updates[index].value);
    };
    EXPECT_TRUE(node->setAttributesAtomically(updates));
    EXPECT_EQ(observer.calls, 4u);
}

TEST(XmlAttributeBatch, EditingCleanupMatchesSingleAttributePolicy)
{
    auto doc = document(); auto reference = document();
    auto prefs = Inkscape::Preferences::get();
    struct RestorePreferences {
        Inkscape::Preferences *prefs;
        std::vector<std::pair<std::string, bool>> values;
        ~RestorePreferences() { for (auto const &[key, value] : values) prefs->setBool(key, value); }
        void set(char const *suffix, bool value) {
            auto key = std::string("/options/svgoutput/") + suffix;
            values.emplace_back(key, prefs->getBool(key));
            prefs->setBool(key, value);
        }
    } restore{prefs, {}};
    restore.set("check_on_editing", true);
    restore.set("disable_optimizations", false);
    restore.set("incorrect_attributes_remove", true);
    restore.set("incorrect_attributes_warn", false);
    restore.set("incorrect_style_properties_remove", true);
    restore.set("incorrect_style_properties_warn", false);
    restore.set("style_defaults_remove", false);
    restore.set("style_defaults_warn", false);
    auto node = doc->root()->firstChild(); auto single = reference->root()->firstChild();
    std::vector<XML::Node::AttributeUpdate> updates = {
        {"zzy-not-an-svg-attribute", "reject"},
        {"style", "fill:red;zzy-not-a-css-property:1"},
        {"inkscape:export-xdpi", "300"}};
    node->setAttributesAtomically(updates);
    for (auto const &update : updates) single->setAttribute(update.name, *update.value);
    EXPECT_EQ(sp_repr_save_buf(doc.get()), sp_repr_save_buf(reference.get()));
    EXPECT_EQ(node->attribute("zzy-not-an-svg-attribute"), nullptr);
}
} // namespace
