// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Unit tests migrated from cxxtest
 *
 * Authors:
 *   Martin Owens
 *
 * Copyright (C) 2024 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include <gtest/gtest.h>

#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <src/document.h>
#include <src/document-undo.h>
#include <src/inkscape.h>
#include <src/object/sp-root.h>
#include <src/object/sp-object.h>
#include <src/xml/repr.h>

using namespace Inkscape;
using namespace Inkscape::XML;
using Nature = SPObject::LinkedObjectNature;
using namespace std::literals;

class ObjectLinksTest : public ::testing::Test {
public:
    static void SetUpTestCase() {
        Inkscape::Application::create(false);
    }

    void SetUp() override {
        constexpr auto docString = R"A(<?xml version="1.0" encoding="UTF-8" standalone="no"?>
<svg version="1.1" id="svg2" width="245" height="110" xmlns:xlink="http://www.w3.org/1999/xlink" xmlns="http://www.w3.org/2000/svg">
  <g id="holder" style="fill:#a51d2d">
    <rect style="stroke:none;stroke-width:7.62315;stroke-linecap:round;stroke-linejoin:round" id="blueberry" width="50" height="50" x="10" y="10" />
    <use x="0" y="0" xlink:href="#blueberry" id="banana" transform="translate(60)" style="fill:#008000" />
    <use x="0" y="0" xlink:href="#banana" id="peach" transform="translate(60)" style="fill:#ff0000" />
    <text xml:space="preserve" style="fill:#a51d2d;" id="pathtext" transform="translate(5,105)"><textPath xlink:href="#textpath" startOffset="50%" id="subtext" style="font-size:11px;font-family:'Noto Sans';">Text from the blue path</textPath></text>
    <path style="fill:none;stroke:#1a5fb4;stroke-width:1;" d="M 20.493281,-6.8198204 C 44.533623,-28.1299 82.044808,-31.874126 109.5958,-15.089731 c 18.83597,10.2521826 40.69713,14.53112164 61.50635,8.1113336" id="textpath" />
    <text xml:space="preserve" style="font-size:6px;white-space:pre;shape-inside:url(#blueberry);fill:#3d3846;" x="200" y="10" id="boxedtext" transform="translate(177)"><tspan x="35" y="57.763855" id="tspan5" style="font-size:6px;font-family:'Noto Sans';">This text should flow into the rectangle and should continue to flow after the cropping function is completed</tspan></text>
    <a id="boat" href="#linked_to"><rect id="linked_from"/></a>
    <rect id="linked_to"/>
  </g>
</svg>)A"sv;
        doc = SPDocument::createNewDocFromMem(docString);

        ASSERT_TRUE(doc);
        ASSERT_TRUE(doc->getRoot());
    }

    std::vector<SPObject *> getObjects(std::vector<std::string> const &lst) {
        std::vector<SPObject *> ret;
        for (auto &id : lst) {
            ret.push_back(doc->getObjectById(id));
        }
        return ret;
    }

    std::unique_ptr<SPDocument> doc;
};

::testing::AssertionResult ObjectIdsEq(int i, std::set<std::string> const& a, std::set<std::string> const& b) {
    std::set<std::string> delta;
    std::set_difference(a.begin(), a.end(), b.begin(), b.end(),
                        std::inserter(delta, delta.begin()));
    std::set_difference(b.begin(), b.end(), a.begin(), a.end(),
                        std::inserter(delta, delta.begin()));

    if (delta.size()) {
        std::ostringstream oo;
        for (auto id : delta) {
            if (std::find(a.begin(), a.end(), id) == a.end()) {
                oo << i << ". unexpected linked object '" << id << "' found.\n";
            } else {
                oo << i << ". expected linked object '" << id << "' not found.\n";
            }
        }
        return ::testing::AssertionFailure() << oo.str();
    }
    return ::testing::AssertionSuccess();
}

::testing::AssertionResult ObjectListEq(int i, std::vector<SPObject *> const& a, std::set<std::string> const& b) {
    std::set<std::string> a_ids;
    for (auto obj : a) {
        a_ids.insert(obj->getId());
    }
    return ObjectIdsEq(i, a_ids, b);
}


TEST_F(ObjectLinksTest, getLinked)
{
    std::vector<std::pair<std::pair<std::string, Nature>, std::set<std::string>>> tests = {
        // clang-format off
        // Groups don't directly link to anything (nonrecursive)
        {{"holder",    Nature::ANY}, {}},

        // A clone is forward linked to it's source, and backwards linked to anything cloning it
        {{"banana",    Nature::DEPENDENT},    {"peach"}},
        {{"banana",    Nature::ANY},          {"blueberry", "peach"}},
        {{"banana",    Nature::DEPENDENCY},   {"blueberry"}},

        // A clone of a clone is forward linked to it's source
        {{"peach",     Nature::DEPENDENT},    {}},
        {{"peach",     Nature::ANY},          {"banana"}},
        {{"peach",     Nature::DEPENDENCY},   {"banana"}},

        // A shape used for clones and flowed text is back linked
        {{"blueberry", Nature::DEPENDENT},    {"banana", "boxedtext"}},
        {{"blueberry", Nature::ANY},          {"banana", "boxedtext"}},
        {{"blueberry", Nature::DEPENDENCY},   {}},

        // Text flowed into a shape has a forward link to that shape
        {{"boxedtext", Nature::DEPENDENT},    {}},
        {{"boxedtext", Nature::ANY},          {"blueberry"}},
        {{"boxedtext", Nature::DEPENDENCY},   {"blueberry"}},

        // A shape used to shape text has back links to the text
        {{"textpath",  Nature::DEPENDENT},    {"subtext"}},
        {{"textpath",  Nature::ANY},          {"subtext"}},
        {{"textpath",  Nature::DEPENDENCY},   {}},

        // Text on a path has a forward link to it's shape
        {{"pathtext",  Nature::DEPENDENT},    {}},
        {{"pathtext",  Nature::ANY},          {"textpath"}},
        {{"pathtext",  Nature::DEPENDENCY},   {"textpath"}},

        // Anchor tags are linked correctly
        {{"linked_to",   Nature::DEPENDENT},  {"boat"}},
        {{"linked_to",   Nature::DEPENDENCY}, {}},
        {{"boat",        Nature::DEPENDENT},  {}},
        {{"boat",        Nature::DEPENDENCY}, {"linked_to"}}
        // clang-format on
    };
    auto i = 0;
    for (auto &test : tests) {
        auto obj = doc->getObjectById(test.first.first);
        ASSERT_TRUE(obj);

        auto objects = obj->getLinked(test.first.second);
        ASSERT_TRUE(ObjectListEq(i, objects, test.second));
        i++;
    }
}

TEST_F(ObjectLinksTest, getLinkedRecursive)
{
    std::vector<std::pair<std::pair<std::string, Nature>, std::set<std::string>>> tests = {
        // clang-format off
        // Groups link to everything via recursion
        {{"holder", Nature::DEPENDENT},  {"peach", "banana", "boxedtext", "subtext", "boat"}},
        {{"holder", Nature::ANY},        {"blueberry", "peach", "banana", "boxedtext", "subtext", "textpath", "boat", "linked_to"}},
        {{"holder", Nature::DEPENDENCY}, {"blueberry", "banana", "textpath", "linked_to"}},

        // A clone is forward linked to it's source, and backwards linked to anything cloning it
        {{"banana", Nature::DEPENDENT},  {"peach"}},
        {{"banana", Nature::ANY},        {"blueberry", "peach", "boxedtext", "banana"}},
        {{"banana", Nature::DEPENDENCY}, {"blueberry",}},

        {{"peach",  Nature::DEPENDENCY}, {"banana", "blueberry"}}
        // clang-format on
    };
    auto i = 0;
    for (auto &test : tests) {
        auto obj = doc->getObjectById(test.first.first);
        ASSERT_TRUE(obj);

        std::vector<SPObject *> objects;
        obj->getLinkedRecursive(objects, test.first.second);
        ASSERT_TRUE(ObjectListEq(i, objects, test.second));
        i++;
    }
}

TEST_F(ObjectLinksTest, cropToObject)
{
    doc->getRoot()->cropToObjects({doc->getObjectById("peach")});

    ASSERT_TRUE(doc->getObjectById("peach"));
    ASSERT_TRUE(doc->getObjectById("blueberry"));
    ASSERT_TRUE(doc->getObjectById("banana"));
    ASSERT_FALSE(doc->getObjectById("nothing"));
    ASSERT_FALSE(doc->getObjectById("pathtext"));
    ASSERT_FALSE(doc->getObjectById("textpath"));
    ASSERT_FALSE(doc->getObjectById("boxedtext"));
}

namespace {

// CR-002 synthetic fixture: a fill_between_many LPE item (`effect_path`) whose
// `linkedpaths` dependency (`source_path`) is a sibling deletion target. The
// effect item deliberately precedes its linked source in document order, the
// order that produced the native cropToObjects fault. The source is nested so
// ancestor preservation is observable. No user artwork is used.
constexpr auto cr002_fixture = R"A(<?xml version="1.0" encoding="UTF-8" standalone="no"?>
<svg version="1.1" width="100" height="100" viewBox="0 0 100 100"
     xmlns="http://www.w3.org/2000/svg"
     xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape"
     xmlns:sodipodi="http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd">
  <defs>
    <inkscape:path-effect id="fx" effect="fill_between_many" method="originald"
        linkedpaths="#source_path,0,1|" is_visible="true" lpeversion="1.2" />
  </defs>
  <g id="layer1" inkscape:groupmode="layer" inkscape:label="Layer 1">
    <path id="effect_path" inkscape:path-effect="#fx" inkscape:original-d="M 0,0 L 10,0"
        d="M 0,0 L 10,0" style="fill:#ff00ff;stroke:none" transform="translate(1,2)" />
    <g id="source_group" transform="translate(10,20)">
      <path id="source_path" d="M 0,0 L 30,0 L 30,30 Z"
          style="fill:#00ff00;stroke:#000000;stroke-width:2" transform="translate(3,4)" />
    </g>
    <rect id="unrelated_rect" x="60" y="60" width="10" height="10" style="fill:#ff0000" />
  </g>
</svg>)A"sv;

// Group-selection fixture: members carry distinct transforms so both order and
// per-member transform preservation are observable after crop.
constexpr auto group_fixture = R"A(<?xml version="1.0" encoding="UTF-8" standalone="no"?>
<svg version="1.1" width="100" height="100" viewBox="0 0 100 100"
     xmlns="http://www.w3.org/2000/svg"
     xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape">
  <g id="layer1" inkscape:groupmode="layer" inkscape:label="Layer 1">
    <g id="keep_group" transform="translate(5,6)">
      <rect id="m1" x="0" y="0" width="4" height="4" transform="translate(1,1)" style="fill:#111111" />
      <rect id="m2" x="10" y="0" width="4" height="4" transform="translate(2,2)" style="fill:#222222" />
      <rect id="m3" x="20" y="0" width="4" height="4" transform="translate(3,3)" style="fill:#333333" />
    </g>
    <rect id="dropped" x="50" y="50" width="4" height="4" style="fill:#444444" />
  </g>
</svg>)A"sv;

// Multiple independently selected roots at the same level.
constexpr auto multi_root_fixture = R"A(<?xml version="1.0" encoding="UTF-8" standalone="no"?>
<svg version="1.1" width="100" height="100" viewBox="0 0 100 100"
     xmlns="http://www.w3.org/2000/svg">
  <g id="root_a" transform="translate(1,1)">
    <rect id="a1" x="0" y="0" width="2" height="2" style="fill:#aaaaaa" />
  </g>
  <g id="root_b" transform="translate(2,2)">
    <rect id="b1" x="5" y="5" width="2" height="2" style="fill:#bbbbbb" />
  </g>
  <rect id="dropped" x="20" y="20" width="2" height="2" style="fill:#cccccc" />
</svg>)A"sv;

// Two effects share one linked source.
constexpr auto shared_source_fixture = R"A(<?xml version="1.0" encoding="UTF-8" standalone="no"?>
<svg version="1.1" width="100" height="100" viewBox="0 0 100 100"
     xmlns="http://www.w3.org/2000/svg"
     xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape">
  <defs>
    <inkscape:path-effect id="fx_a" effect="fill_between_many" method="originald"
        linkedpaths="#shared_source,0,1|" is_visible="true" lpeversion="1.2" />
    <inkscape:path-effect id="fx_b" effect="fill_between_many" method="originald"
        linkedpaths="#shared_source,0,1|" is_visible="true" lpeversion="1.2" />
  </defs>
  <g id="layer1" inkscape:groupmode="layer" inkscape:label="Layer 1">
    <path id="effect_a" inkscape:path-effect="#fx_a" inkscape:original-d="M 0,0 L 10,0"
        d="M 0,0 L 10,0" style="fill:#ff00ff;stroke:none" />
    <path id="effect_b" inkscape:path-effect="#fx_b" inkscape:original-d="M 0,0 L 10,0"
        d="M 0,0 L 10,0" style="fill:#00ffff;stroke:none" />
    <path id="shared_source" d="M 0,0 L 20,0 L 20,20 Z" style="fill:#00ff00" />
    <rect id="dropped" x="70" y="70" width="6" height="6" style="fill:#ff0000" />
  </g>
</svg>)A"sv;

// A chain with no cycles: chain_top -> chain_mid -> chain_source.
constexpr auto transitive_fixture = R"A(<?xml version="1.0" encoding="UTF-8" standalone="no"?>
<svg version="1.1" width="100" height="100" viewBox="0 0 100 100"
     xmlns="http://www.w3.org/2000/svg"
     xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape">
  <defs>
    <inkscape:path-effect id="fx_chain" effect="fill_between_many" method="originald"
        linkedpaths="#chain_mid,0,1|" is_visible="true" lpeversion="1.2" />
    <inkscape:path-effect id="fx_leaf" effect="fill_between_many" method="originald"
        linkedpaths="#chain_source,0,1|" is_visible="true" lpeversion="1.2" />
  </defs>
  <g id="layer1" inkscape:groupmode="layer" inkscape:label="Layer 1">
    <path id="chain_top" inkscape:path-effect="#fx_chain" inkscape:original-d="M 0,0 L 10,0"
        d="M 0,0 L 10,0" style="fill:#ff00ff;stroke:none" />
    <path id="chain_mid" inkscape:path-effect="#fx_leaf" inkscape:original-d="M 0,0 L 12,0"
        d="M 0,0 L 12,0" style="fill:#00ffff;stroke:none" />
    <path id="chain_source" d="M 0,0 L 25,0 L 25,25 Z" style="fill:#00ff00" />
    <rect id="dropped" x="70" y="70" width="6" height="6" style="fill:#ff0000" />
  </g>
</svg>)A"sv;

// FLAT same-parent CR-002 fixture: the LPE holder (`effect_path`) precedes its
// linked source (`source_path`) and both are direct siblings. Retaining the
// unrelated rect therefore puts the freed source itself in the raw deletion
// list, which is the exact CR-002 UAF; the nested fixture only covers the
// ancestor case. No user artwork is used.
//
// `effect_path` stores the native *computed* LPE geometry in `d` (a saved
// Inkscape file never stores the authored input there; that lives in
// `inkscape:original-d`). For this exact geometry the native fill_between_many
// output is `M 2,2 H 32 V 32 Z`; authoring it keeps full-byte save snapshots
// stable across Undo/Redo (see CropUndoRedoRoundtripRestoresDocument).
constexpr auto flat_cr002_fixture = R"A(<?xml version="1.0" encoding="UTF-8" standalone="no"?>
<svg version="1.1" width="100" height="100" viewBox="0 0 100 100"
     xmlns="http://www.w3.org/2000/svg"
     xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape">
  <defs>
    <inkscape:path-effect id="fx" effect="fill_between_many" method="originald"
        linkedpaths="#source_path,0,1|" is_visible="true" lpeversion="1.2" />
  </defs>
  <path id="effect_path" inkscape:path-effect="#fx" inkscape:original-d="M 0,0 L 10,0"
      d="M 2,2 H 32 V 32 Z" style="fill:#ff00ff;stroke:none" transform="translate(1,2)" />
  <path id="source_path" d="M 0,0 L 30,0 L 30,30 Z"
      style="fill:#00ff00;stroke:#000000;stroke-width:2" transform="translate(3,4)" />
  <rect id="unrelated_rect" x="60" y="60" width="10" height="10" style="fill:#ff0000" />
</svg>)A"sv;

// Same objects as flat_cr002_fixture but the linked source precedes the effect
// holder. Deleting the source first and then revisiting the holder is the
// reversed-order variant of the same dangling-source fault.
constexpr auto flat_cr002_reversed_fixture = R"A(<?xml version="1.0" encoding="UTF-8" standalone="no"?>
<svg version="1.1" width="100" height="100" viewBox="0 0 100 100"
     xmlns="http://www.w3.org/2000/svg"
     xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape">
  <defs>
    <inkscape:path-effect id="fx" effect="fill_between_many" method="originald"
        linkedpaths="#source_path,0,1|" is_visible="true" lpeversion="1.2" />
  </defs>
  <path id="source_path" d="M 0,0 L 30,0 L 30,30 Z"
      style="fill:#00ff00;stroke:#000000;stroke-width:2" transform="translate(3,4)" />
  <path id="effect_path" inkscape:path-effect="#fx" inkscape:original-d="M 0,0 L 10,0"
      d="M 0,0 L 10,0" style="fill:#ff00ff;stroke:none" transform="translate(1,2)" />
  <rect id="unrelated_rect" x="60" y="60" width="10" height="10" style="fill:#ff0000" />
</svg>)A"sv;

// Clone native dependency: the `<use>` precedes its href source, both direct
// siblings, so the same crop dependency closure must protect the source.
constexpr auto flat_clone_fixture = R"A(<?xml version="1.0" encoding="UTF-8" standalone="no"?>
<svg version="1.1" width="100" height="100" viewBox="0 0 100 100"
     xmlns="http://www.w3.org/2000/svg"
     xmlns:xlink="http://www.w3.org/1999/xlink">
  <use id="clone_use" xlink:href="#clone_source" x="10" y="0" />
  <rect id="clone_source" x="0" y="0" width="5" height="5" style="fill:#00ff00" />
  <rect id="unrelated_rect" x="60" y="60" width="10" height="10" style="fill:#ff0000" />
</svg>)A"sv;

// A selected group whose member carries the LPE effect; the linked source is a
// sibling of the group, so dependency closure must descend into the selected
// group's children to keep it.
constexpr auto nested_effect_group_fixture = R"A(<?xml version="1.0" encoding="UTF-8" standalone="no"?>
<svg version="1.1" width="100" height="100" viewBox="0 0 100 100"
     xmlns="http://www.w3.org/2000/svg"
     xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape">
  <defs>
    <inkscape:path-effect id="fx" effect="fill_between_many" method="originald"
        linkedpaths="#source_path,0,1|" is_visible="true" lpeversion="1.2" />
  </defs>
  <g id="effect_group" transform="translate(7,8)">
    <path id="effect_path" inkscape:path-effect="#fx" inkscape:original-d="M 0,0 L 10,0"
        d="M 0,0 L 10,0" style="fill:#ff00ff;stroke:none" transform="translate(1,2)" />
  </g>
  <path id="source_path" d="M 0,0 L 30,0 L 30,30 Z"
      style="fill:#00ff00;stroke:#000000;stroke-width:2" transform="translate(3,4)" />
  <rect id="unrelated_rect" x="60" y="60" width="10" height="10" style="fill:#ff0000" />
</svg>)A"sv;

// M1 ancestor-dependency fixture: the *ancestor container itself* carries the
// LPE. `effect_group` holds an effect whose linked sources are `src_in` (the
// source sibling inside the ancestor) and `src_out` (the source sibling outside
// it). Selecting only `selected_child` must keep the ancestor as a container and
// collect the ancestor's own dependencies, while still dropping the unrelated
// sibling inside (`drop_in`) and outside (`drop_out`). Enqueuing the ancestor as
// a retained root would keep `drop_in` too and defeat the crop, so this fixture
// pins the difference. No user artwork is used.
constexpr auto ancestor_effect_fixture = R"A(<?xml version="1.0" encoding="UTF-8" standalone="no"?>
<svg version="1.1" width="100" height="100" viewBox="0 0 100 100"
     xmlns="http://www.w3.org/2000/svg"
     xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape">
  <defs>
    <inkscape:path-effect id="fx_ancestor" effect="fill_between_many" method="originald"
        linkedpaths="#src_in,0,1|#src_out,0,1|" is_visible="true" lpeversion="1.2" />
  </defs>
  <g id="layer1" inkscape:groupmode="layer" inkscape:label="Layer 1">
    <g id="effect_group" inkscape:path-effect="#fx_ancestor" transform="translate(7,8)">
      <rect id="selected_child" x="0" y="0" width="4" height="4"
          style="fill:#ff00ff" transform="translate(1,2)" />
      <path id="src_in" d="M 0,0 L 30,0 L 30,30 Z"
          style="fill:#00ff00;stroke:#000000;stroke-width:2" transform="translate(3,4)" />
      <rect id="drop_in" x="40" y="40" width="5" height="5" style="fill:#123456" />
    </g>
    <path id="src_out" d="M 0,0 L 20,0 L 20,20 Z"
        style="fill:#0000ff;stroke:#000000;stroke-width:2" transform="translate(5,6)" />
    <rect id="drop_out" x="60" y="60" width="10" height="10" style="fill:#ff0000" />
  </g>
</svg>)A"sv;

} // namespace

class CropToObjectsTest : public ::testing::Test {
public:
    static void SetUpTestCase() {
        if (!Inkscape::Application::exists()) {
            Inkscape::Application::create(false);
        }
    }

    // Mirrors Export: build the document, bring it up to date, then crop.
    std::unique_ptr<SPDocument> makeDoc(std::string_view svg) {
        auto document = SPDocument::createNewDocFromMem(svg);
        EXPECT_TRUE(document);
        if (document) {
            document->ensureUpToDate();
        }
        return document;
    }

    static std::vector<SPObject *> resolve(SPDocument *document, std::initializer_list<char const *> ids) {
        std::vector<SPObject *> objects;
        for (auto id : ids) {
            objects.push_back(document->getObjectById(id));
        }
        return objects;
    }

    static std::string attr(SPDocument *document, char const *id, char const *name) {
        auto *object = document->getObjectById(id);
        EXPECT_NE(object, nullptr) << id;
        if (!object) {
            return {};
        }
        auto const *value = object->getAttribute(name);
        return value ? std::string(value) : std::string();
    }

    static std::vector<std::string> childIds(SPObject *object) {
        std::vector<std::string> ids;
        for (auto &child : object->children) {
            ids.push_back(child.getId());
        }
        return ids;
    }

    static int countById(SPObject const *object, std::string const &id) {
        int count = (object->getId() && id == object->getId()) ? 1 : 0;
        for (auto &child : object->children) {
            count += countById(&child, id);
        }
        return count;
    }
};

// Retaining an unrelated object must drop the effect item and its linked source
// without revisiting a released object. Crashes on the CR-002 baseline.
TEST_F(CropToObjectsTest, RetainUnrelatedRectRemovesEffectAndSourceWithoutCrash)
{
    auto doc = makeDoc(cr002_fixture);
    ASSERT_TRUE(doc);

    doc->getRoot()->cropToObjects(resolve(doc.get(), {"unrelated_rect"}));

    ASSERT_NE(doc->getObjectById("unrelated_rect"), nullptr);
    EXPECT_EQ(doc->getObjectById("effect_path"), nullptr);
    EXPECT_EQ(doc->getObjectById("source_path"), nullptr);
    EXPECT_EQ(doc->getObjectById("source_group"), nullptr);
}

// Retaining the linked source must not let the effect's removal delete it, and
// must preserve its exact authored data and ancestor chain.
TEST_F(CropToObjectsTest, RetainLinkedSourcePreservesSourceAndAncestors)
{
    auto doc = makeDoc(cr002_fixture);
    ASSERT_TRUE(doc);

    auto const source_d = attr(doc.get(), "source_path", "d");
    auto const source_style = attr(doc.get(), "source_path", "style");
    auto const source_transform = attr(doc.get(), "source_path", "transform");
    auto const group_transform = attr(doc.get(), "source_group", "transform");
    ASSERT_FALSE(source_d.empty());
    ASSERT_FALSE(source_transform.empty());

    doc->getRoot()->cropToObjects(resolve(doc.get(), {"source_path"}));

    auto *source = doc->getObjectById("source_path");
    ASSERT_NE(source, nullptr);
    EXPECT_EQ(attr(doc.get(), "source_path", "d"), source_d);
    EXPECT_EQ(attr(doc.get(), "source_path", "style"), source_style);
    EXPECT_EQ(attr(doc.get(), "source_path", "transform"), source_transform);

    auto *group = doc->getObjectById("source_group");
    ASSERT_NE(group, nullptr);
    EXPECT_EQ(attr(doc.get(), "source_group", "transform"), group_transform);
    EXPECT_EQ(source->parent, group);
    ASSERT_NE(group->parent, nullptr);
    EXPECT_EQ(std::string(group->parent->getId()), "layer1");

    EXPECT_EQ(doc->getObjectById("effect_path"), nullptr);
    EXPECT_EQ(doc->getObjectById("unrelated_rect"), nullptr);
}

// Retaining the effect item must keep its effect reference and the linked source
// it depends on.
TEST_F(CropToObjectsTest, RetainEffectPreservesEffectAttributeAndSourceDependency)
{
    auto doc = makeDoc(cr002_fixture);
    ASSERT_TRUE(doc);

    doc->getRoot()->cropToObjects(resolve(doc.get(), {"effect_path"}));

    auto *effect = doc->getObjectById("effect_path");
    ASSERT_NE(effect, nullptr);
    EXPECT_EQ(attr(doc.get(), "effect_path", "inkscape:path-effect"), std::string("#fx"));

    ASSERT_NE(doc->getObjectById("source_path"), nullptr);
    EXPECT_EQ(attr(doc.get(), "source_path", "d"), std::string("M 0,0 L 30,0 L 30,30 Z"));
    EXPECT_EQ(attr(doc.get(), "source_path", "transform"), std::string("translate(3,4)"));

    EXPECT_EQ(doc->getObjectById("unrelated_rect"), nullptr);
}

// The retained effect and its source dependency must survive native
// serialization/reopen and a repeated ensureUpToDate/crop.
TEST_F(CropToObjectsTest, RetainEffectDependencySurvivesSerializeReopenAndEnsureUpToDate)
{
    auto doc = makeDoc(cr002_fixture);
    ASSERT_TRUE(doc);
    doc->getRoot()->cropToObjects(resolve(doc.get(), {"effect_path"}));
    ASSERT_NE(doc->getObjectById("source_path"), nullptr);

    auto const saved = sp_repr_save_buf(doc->getReprDoc()).raw();
    auto reopened = SPDocument::createNewDocFromMem(saved);
    ASSERT_TRUE(reopened);
    reopened->ensureUpToDate();

    auto *effect = reopened->getObjectById("effect_path");
    ASSERT_NE(effect, nullptr);
    EXPECT_EQ(std::string(effect->getAttribute("inkscape:path-effect")), "#fx");
    ASSERT_NE(reopened->getObjectById("source_path"), nullptr);
    EXPECT_EQ(attr(reopened.get(), "source_path", "d"), std::string("M 0,0 L 30,0 L 30,30 Z"));
    EXPECT_EQ(attr(reopened.get(), "source_path", "transform"), std::string("translate(3,4)"));

    // A second crop on the reopened document must still protect the dependency.
    reopened->getRoot()->cropToObjects(resolve(reopened.get(), {"effect_path"}));
    EXPECT_NE(reopened->getObjectById("effect_path"), nullptr);
    EXPECT_NE(reopened->getObjectById("source_path"), nullptr);
}

// A selected group is one target: all members and their order/transforms stay.
TEST_F(CropToObjectsTest, RetainSelectedGroupPreservesMembersOrderAndTransforms)
{
    auto doc = makeDoc(group_fixture);
    ASSERT_TRUE(doc);

    auto const group_transform = attr(doc.get(), "keep_group", "transform");
    auto const m1_transform = attr(doc.get(), "m1", "transform");
    auto const m2_transform = attr(doc.get(), "m2", "transform");
    auto const m3_transform = attr(doc.get(), "m3", "transform");

    doc->getRoot()->cropToObjects(resolve(doc.get(), {"keep_group"}));

    auto *group = doc->getObjectById("keep_group");
    ASSERT_NE(group, nullptr);
    EXPECT_EQ(attr(doc.get(), "keep_group", "transform"), group_transform);
    EXPECT_EQ(childIds(group), (std::vector<std::string>{"m1", "m2", "m3"}));
    EXPECT_EQ(attr(doc.get(), "m1", "transform"), m1_transform);
    EXPECT_EQ(attr(doc.get(), "m2", "transform"), m2_transform);
    EXPECT_EQ(attr(doc.get(), "m3", "transform"), m3_transform);
    EXPECT_EQ(doc->getObjectById("dropped"), nullptr);
}

// Multiple selected roots each stay exactly once.
TEST_F(CropToObjectsTest, RetainMultipleRootsPreservesEachExactlyOnce)
{
    auto doc = makeDoc(multi_root_fixture);
    ASSERT_TRUE(doc);

    doc->getRoot()->cropToObjects(resolve(doc.get(), {"root_a", "root_b"}));

    EXPECT_NE(doc->getObjectById("root_a"), nullptr);
    EXPECT_NE(doc->getObjectById("root_b"), nullptr);
    EXPECT_EQ(countById(doc->getRoot(), "root_a"), 1);
    EXPECT_EQ(countById(doc->getRoot(), "root_b"), 1);
    EXPECT_EQ(doc->getObjectById("dropped"), nullptr);
}

// Cropping a document copy must not mutate the original input.
TEST_F(CropToObjectsTest, CroppingCopyLeavesOriginalDocumentUnchanged)
{
    auto doc = makeDoc(cr002_fixture);
    ASSERT_TRUE(doc);
    auto const before = sp_repr_save_buf(doc->getReprDoc()).raw();

    auto copy = doc->copy();
    ASSERT_TRUE(copy);
    copy->ensureUpToDate();
    copy->getRoot()->cropToObjects(resolve(copy.get(), {"unrelated_rect"}));

    ASSERT_NE(copy->getObjectById("unrelated_rect"), nullptr);
    EXPECT_EQ(copy->getObjectById("effect_path"), nullptr);
    EXPECT_EQ(copy->getObjectById("source_path"), nullptr);

    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(), before);
    EXPECT_NE(doc->getObjectById("effect_path"), nullptr);
    EXPECT_NE(doc->getObjectById("source_path"), nullptr);
    EXPECT_NE(doc->getObjectById("unrelated_rect"), nullptr);
}

// An empty selection is a legitimate no-op.
TEST_F(CropToObjectsTest, EmptySelectionIsNoOp)
{
    auto doc = makeDoc(cr002_fixture);
    ASSERT_TRUE(doc);
    auto const before = sp_repr_save_buf(doc->getReprDoc()).raw();

    doc->getRoot()->cropToObjects({});

    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(), before);
    EXPECT_NE(doc->getObjectById("effect_path"), nullptr);
    EXPECT_NE(doc->getObjectById("source_path"), nullptr);
    EXPECT_NE(doc->getObjectById("unrelated_rect"), nullptr);
}

// Two effects sharing one source: keeping both keeps the source once.
TEST_F(CropToObjectsTest, SharedSourceTwoEffectsKeepsSourceAndBothEffects)
{
    auto doc = makeDoc(shared_source_fixture);
    ASSERT_TRUE(doc);

    doc->getRoot()->cropToObjects(resolve(doc.get(), {"effect_a", "effect_b"}));

    EXPECT_NE(doc->getObjectById("effect_a"), nullptr);
    EXPECT_NE(doc->getObjectById("effect_b"), nullptr);
    EXPECT_NE(doc->getObjectById("shared_source"), nullptr);
    EXPECT_EQ(countById(doc->getRoot(), "shared_source"), 1);
    EXPECT_EQ(doc->getObjectById("dropped"), nullptr);
}

// Dropping the other effect must not delete the shared source retained through
// the selected effect.
TEST_F(CropToObjectsTest, SharedSourceDroppingOtherEffectKeepsRetainedSource)
{
    auto doc = makeDoc(shared_source_fixture);
    ASSERT_TRUE(doc);

    doc->getRoot()->cropToObjects(resolve(doc.get(), {"effect_a"}));

    EXPECT_NE(doc->getObjectById("effect_a"), nullptr);
    EXPECT_EQ(doc->getObjectById("effect_b"), nullptr);
    EXPECT_NE(doc->getObjectById("shared_source"), nullptr);
    EXPECT_EQ(attr(doc.get(), "effect_a", "inkscape:path-effect"), std::string("#fx_a"));
}

// Keep the whole transitive effect chain (no cycles in the fixture).
TEST_F(CropToObjectsTest, TransitiveEffectDependencyKeepsWholeChain)
{
    auto doc = makeDoc(transitive_fixture);
    ASSERT_TRUE(doc);

    doc->getRoot()->cropToObjects(resolve(doc.get(), {"chain_top"}));

    EXPECT_NE(doc->getObjectById("chain_top"), nullptr);
    EXPECT_NE(doc->getObjectById("chain_mid"), nullptr);
    EXPECT_NE(doc->getObjectById("chain_source"), nullptr);
    EXPECT_EQ(attr(doc.get(), "chain_mid", "inkscape:path-effect"), std::string("#fx_leaf"));
    EXPECT_EQ(doc->getObjectById("dropped"), nullptr);
}

// FLAT same-parent retain-unrelated: the raw deletion list contains the linked
// source itself, so this is the exact CR-002 sibling UAF. Retaining the
// unrelated rect must remove the effect holder and its source once, without
// revisiting the released source. Crashes on the CR-002 baseline. The nested
// `RetainUnrelatedRectRemovesEffectAndSourceWithoutCrash` case above keeps its
// ancestor-coverage role and is deliberately preserved.
TEST_F(CropToObjectsTest, FlatRetainUnrelatedRectRemovesEffectAndSourceWithoutCrash)
{
    auto doc = makeDoc(flat_cr002_fixture);
    ASSERT_TRUE(doc);

    doc->getRoot()->cropToObjects(resolve(doc.get(), {"unrelated_rect"}));

    ASSERT_NE(doc->getObjectById("unrelated_rect"), nullptr);
    EXPECT_EQ(doc->getObjectById("effect_path"), nullptr);
    EXPECT_EQ(doc->getObjectById("source_path"), nullptr);
}

// Retaining the flat source must keep its exact authored data and drop the
// effect holder that would otherwise run its satellite cleanup over it.
TEST_F(CropToObjectsTest, FlatRetainSourcePreservesSourceDropsEffect)
{
    auto doc = makeDoc(flat_cr002_fixture);
    ASSERT_TRUE(doc);

    auto const source_d = attr(doc.get(), "source_path", "d");
    auto const source_style = attr(doc.get(), "source_path", "style");
    auto const source_transform = attr(doc.get(), "source_path", "transform");
    ASSERT_FALSE(source_d.empty());
    ASSERT_FALSE(source_transform.empty());

    doc->getRoot()->cropToObjects(resolve(doc.get(), {"source_path"}));

    auto *source = doc->getObjectById("source_path");
    ASSERT_NE(source, nullptr);
    EXPECT_EQ(attr(doc.get(), "source_path", "d"), source_d);
    EXPECT_EQ(attr(doc.get(), "source_path", "style"), source_style);
    EXPECT_EQ(attr(doc.get(), "source_path", "transform"), source_transform);
    EXPECT_EQ(doc->getObjectById("effect_path"), nullptr);
    EXPECT_EQ(doc->getObjectById("unrelated_rect"), nullptr);
}

// Retaining the flat effect holder must keep its linked source dependency.
TEST_F(CropToObjectsTest, FlatRetainEffectKeepsSourceDependency)
{
    auto doc = makeDoc(flat_cr002_fixture);
    ASSERT_TRUE(doc);

    doc->getRoot()->cropToObjects(resolve(doc.get(), {"effect_path"}));

    auto *effect = doc->getObjectById("effect_path");
    ASSERT_NE(effect, nullptr);
    EXPECT_EQ(attr(doc.get(), "effect_path", "inkscape:path-effect"), std::string("#fx"));
    ASSERT_NE(doc->getObjectById("source_path"), nullptr);
    EXPECT_EQ(attr(doc.get(), "source_path", "d"), std::string("M 0,0 L 30,0 L 30,30 Z"));
    EXPECT_EQ(attr(doc.get(), "source_path", "transform"), std::string("translate(3,4)"));
    EXPECT_EQ(doc->getObjectById("unrelated_rect"), nullptr);
}

// Reversed sibling order: the source precedes the effect holder, so the raw
// deletion order is the mirror of the CR-002 fault.
TEST_F(CropToObjectsTest, ReversedRetainUnrelatedRectRemovesEffectAndSourceWithoutCrash)
{
    auto doc = makeDoc(flat_cr002_reversed_fixture);
    ASSERT_TRUE(doc);

    doc->getRoot()->cropToObjects(resolve(doc.get(), {"unrelated_rect"}));

    ASSERT_NE(doc->getObjectById("unrelated_rect"), nullptr);
    EXPECT_EQ(doc->getObjectById("effect_path"), nullptr);
    EXPECT_EQ(doc->getObjectById("source_path"), nullptr);
}

// Reversed order must still protect the retained effect's source through native
// save/reopen and a repeated crop.
TEST_F(CropToObjectsTest, ReversedRetainEffectKeepsSourceDependencyThroughRoundTrip)
{
    auto doc = makeDoc(flat_cr002_reversed_fixture);
    ASSERT_TRUE(doc);

    doc->getRoot()->cropToObjects(resolve(doc.get(), {"effect_path"}));
    ASSERT_NE(doc->getObjectById("effect_path"), nullptr);
    ASSERT_NE(doc->getObjectById("source_path"), nullptr);

    auto const saved = sp_repr_save_buf(doc->getReprDoc()).raw();
    auto reopened = SPDocument::createNewDocFromMem(saved);
    ASSERT_TRUE(reopened);
    reopened->ensureUpToDate();

    ASSERT_NE(reopened->getObjectById("effect_path"), nullptr);
    ASSERT_NE(reopened->getObjectById("source_path"), nullptr);
    EXPECT_EQ(attr(reopened.get(), "source_path", "d"), std::string("M 0,0 L 30,0 L 30,30 Z"));

    reopened->getRoot()->cropToObjects(resolve(reopened.get(), {"effect_path"}));
    EXPECT_NE(reopened->getObjectById("effect_path"), nullptr);
    EXPECT_NE(reopened->getObjectById("source_path"), nullptr);
}

// A selected group containing the effect is one target: the group, its member
// and the member's linked source sibling must survive. This is the nested
// dependency check that was absent (the other group case has no effect member).
TEST_F(CropToObjectsTest, NestedSelectedEffectGroupKeepsEffectAndSourceDependency)
{
    auto doc = makeDoc(nested_effect_group_fixture);
    ASSERT_TRUE(doc);

    auto const group_transform = attr(doc.get(), "effect_group", "transform");

    doc->getRoot()->cropToObjects(resolve(doc.get(), {"effect_group"}));

    auto *group = doc->getObjectById("effect_group");
    ASSERT_NE(group, nullptr);
    EXPECT_EQ(attr(doc.get(), "effect_group", "transform"), group_transform);
    EXPECT_EQ(childIds(group), (std::vector<std::string>{"effect_path"}));
    EXPECT_NE(doc->getObjectById("effect_path"), nullptr);
    EXPECT_NE(doc->getObjectById("source_path"), nullptr);
    EXPECT_EQ(attr(doc.get(), "effect_path", "inkscape:path-effect"), std::string("#fx"));
    EXPECT_EQ(doc->getObjectById("unrelated_rect"), nullptr);
}

// A `<use>` clone is a native dependency of its href source; singular crop to
// the clone must retain both and survive save/reopen.
TEST_F(CropToObjectsTest, SingularCropToCloneRetainsNativeSourceThroughRoundTrip)
{
    auto doc = makeDoc(flat_clone_fixture);
    ASSERT_TRUE(doc);

    auto *clone = doc->getObjectById("clone_use");
    ASSERT_NE(clone, nullptr);
    doc->getRoot()->cropToObject(clone);

    ASSERT_NE(doc->getObjectById("clone_use"), nullptr);
    ASSERT_NE(doc->getObjectById("clone_source"), nullptr);
    ASSERT_NE(clone->getRepr(), nullptr);
    EXPECT_STREQ(clone->getRepr()->attribute("xlink:href"), "#clone_source");
    EXPECT_EQ(doc->getObjectById("unrelated_rect"), nullptr);

    auto reopened = SPDocument::createNewDocFromMem(sp_repr_save_buf(doc->getReprDoc()).raw());
    ASSERT_TRUE(reopened);
    reopened->ensureUpToDate();
    EXPECT_NE(reopened->getObjectById("clone_use"), nullptr);
    EXPECT_NE(reopened->getObjectById("clone_source"), nullptr);
}

// Plural crop retaining the unrelated rect must remove the clone and its source
// without crashing.
TEST_F(CropToObjectsTest, PluralRetainUnrelatedRemovesCloneAndSourceWithoutCrash)
{
    auto doc = makeDoc(flat_clone_fixture);
    ASSERT_TRUE(doc);

    doc->getRoot()->cropToObjects(resolve(doc.get(), {"unrelated_rect"}));

    ASSERT_NE(doc->getObjectById("unrelated_rect"), nullptr);
    EXPECT_EQ(doc->getObjectById("clone_use"), nullptr);
    EXPECT_EQ(doc->getObjectById("clone_source"), nullptr);
}

// Singular CLI crop (`--export-id-only`) to the linked source must keep the
// exact source and drop the effect holder and unrelated rect, then survive a
// native save/reopen.
TEST_F(CropToObjectsTest, SingularCropToSourceRetainsSourceThroughRoundTrip)
{
    auto doc = makeDoc(flat_cr002_fixture);
    ASSERT_TRUE(doc);

    auto const source_d = attr(doc.get(), "source_path", "d");
    auto const source_transform = attr(doc.get(), "source_path", "transform");
    ASSERT_FALSE(source_d.empty());
    ASSERT_FALSE(source_transform.empty());

    doc->getRoot()->cropToObject(doc->getObjectById("source_path"));

    auto *source = doc->getObjectById("source_path");
    ASSERT_NE(source, nullptr);
    EXPECT_EQ(attr(doc.get(), "source_path", "d"), source_d);
    EXPECT_EQ(attr(doc.get(), "source_path", "transform"), source_transform);
    EXPECT_EQ(doc->getObjectById("effect_path"), nullptr);
    EXPECT_EQ(doc->getObjectById("unrelated_rect"), nullptr);

    auto reopened = SPDocument::createNewDocFromMem(sp_repr_save_buf(doc->getReprDoc()).raw());
    ASSERT_TRUE(reopened);
    reopened->ensureUpToDate();
    ASSERT_NE(reopened->getObjectById("source_path"), nullptr);
    EXPECT_EQ(attr(reopened.get(), "source_path", "d"), source_d);
    EXPECT_EQ(reopened->getObjectById("effect_path"), nullptr);
}

// Singular CLI crop to the effect holder must retain its linked source
// dependency; baseline loses `source_path` here.
TEST_F(CropToObjectsTest, SingularCropToEffectKeepsSourceDependencyThroughRoundTrip)
{
    auto doc = makeDoc(flat_cr002_fixture);
    ASSERT_TRUE(doc);

    doc->getRoot()->cropToObject(doc->getObjectById("effect_path"));

    auto *effect = doc->getObjectById("effect_path");
    ASSERT_NE(effect, nullptr);
    EXPECT_EQ(attr(doc.get(), "effect_path", "inkscape:path-effect"), std::string("#fx"));
    ASSERT_NE(doc->getObjectById("source_path"), nullptr);
    EXPECT_EQ(attr(doc.get(), "source_path", "d"), std::string("M 0,0 L 30,0 L 30,30 Z"));
    EXPECT_EQ(doc->getObjectById("unrelated_rect"), nullptr);

    auto reopened = SPDocument::createNewDocFromMem(sp_repr_save_buf(doc->getReprDoc()).raw());
    ASSERT_TRUE(reopened);
    reopened->ensureUpToDate();
    EXPECT_NE(reopened->getObjectById("effect_path"), nullptr);
    ASSERT_NE(reopened->getObjectById("source_path"), nullptr);
    EXPECT_EQ(attr(reopened.get(), "source_path", "transform"), std::string("translate(3,4)"));
}

// Legacy singular null contract: a null target drops every renderable child.
// The product task explicitly preserves this old behavior; this case is
// behavior preservation, not a new feature. If the product worker intentionally
// escalates a different null path, the supervisor must reconcile this oracle
// rather than loosen it silently.
TEST_F(CropToObjectsTest, SingularCropToNullDropsAllRenderableChildren)
{
    auto doc = makeDoc(flat_cr002_fixture);
    ASSERT_TRUE(doc);

    doc->getRoot()->cropToObject(nullptr);

    ASSERT_NE(doc->getRoot(), nullptr);
    EXPECT_EQ(doc->getObjectById("effect_path"), nullptr);
    EXPECT_EQ(doc->getObjectById("source_path"), nullptr);
    EXPECT_EQ(doc->getObjectById("unrelated_rect"), nullptr);
}

// Ordinary editing Delete removes the holder, but linked input geometry belongs
// to the user. Generated satellites have separate removal coverage.
TEST_F(CropToObjectsTest, NormalDeleteKeepsUserOwnedEffectSource)
{
    auto doc = makeDoc(flat_cr002_fixture);
    ASSERT_TRUE(doc);

    auto *effect = doc->getObjectById("effect_path");
    auto *source = doc->getObjectById("source_path");
    ASSERT_NE(effect, nullptr);
    ASSERT_NE(source, nullptr);

    bool effect_notified = false;
    bool source_notified = false;
    effect->connectDelete(sigc::slot<void(SPObject *)>(
        [&](SPObject *deleted) { effect_notified = effect_notified || deleted == effect; }));
    source->connectDelete(sigc::slot<void(SPObject *)>(
        [&](SPObject *deleted) { source_notified = source_notified || deleted == source; }));

    effect->deleteObject(true, true);

    EXPECT_TRUE(effect_notified);
    EXPECT_FALSE(source_notified);
    EXPECT_EQ(doc->getObjectById("effect_path"), nullptr);
    EXPECT_NE(doc->getObjectById("source_path"), nullptr);
    EXPECT_NE(doc->getObjectById("unrelated_rect"), nullptr);
}

// Focused native Undo/Redo roundtrip for the crop on a live document copy. The
// crop mutation must be one undoable transaction; undo restores the pre-crop
// serialization and redo restores the post-crop serialization.
TEST_F(CropToObjectsTest, CropUndoRedoRoundtripRestoresDocument)
{
    auto doc = makeDoc(flat_cr002_fixture);
    ASSERT_TRUE(doc);

    // Positive control: the fixture must already be in native computed state, so
    // `d` is the LPE output (not the authored `inkscape:original-d`) and stays
    // byte-stable across a further update pass.
    auto const effect_d = attr(doc.get(), "effect_path", "d");
    auto const effect_original_d = attr(doc.get(), "effect_path", "inkscape:original-d");
    ASSERT_FALSE(effect_d.empty());
    ASSERT_FALSE(effect_original_d.empty());
    EXPECT_NE(effect_d, effect_original_d)
        << "fixture must store native computed LPE geometry, not the authored input";
    doc->ensureUpToDate();
    EXPECT_EQ(attr(doc.get(), "effect_path", "d"), effect_d);

    DocumentUndo::done(doc.get(), Util::Internal::ContextString("Fixture"), "");
    DocumentUndo::clearUndo(doc.get());
    DocumentUndo::clearRedo(doc.get());

    auto const before = sp_repr_save_buf(doc->getReprDoc()).raw();

    doc->getRoot()->cropToObjects(resolve(doc.get(), {"effect_path"}));
    DocumentUndo::done(doc.get(), Util::Internal::ContextString("Selected SVG crop"), "");
    auto const after = sp_repr_save_buf(doc->getReprDoc()).raw();

    ASSERT_NE(after, before);
    EXPECT_NE(doc->getObjectById("effect_path"), nullptr);
    EXPECT_NE(doc->getObjectById("source_path"), nullptr);
    EXPECT_EQ(doc->getObjectById("unrelated_rect"), nullptr);

    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(), before);
    EXPECT_NE(doc->getObjectById("unrelated_rect"), nullptr);
    EXPECT_NE(doc->getObjectById("effect_path"), nullptr);
    EXPECT_NE(doc->getObjectById("source_path"), nullptr);

    ASSERT_TRUE(DocumentUndo::redo(doc.get()));
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(), after);
    EXPECT_EQ(doc->getObjectById("unrelated_rect"), nullptr);
    EXPECT_NE(doc->getObjectById("source_path"), nullptr);
}

// M1: an ancestor container that is itself LPE-bearing owes its own linked
// sources. Selecting only a descendant must keep the ancestor as a container and
// collect the ancestor's linked sources, while still dropping the unrelated
// sibling inside the ancestor and the unrelated sibling outside it. Enqueuing the
// ancestor as a retained root would keep `drop_in` too, so this pins the
// difference: the group is not promoted to a selected whole-group target.
TEST_F(CropToObjectsTest, AncestorEffectKeepsLinkedSourcesWithoutPromotingGroup)
{
    auto doc = makeDoc(ancestor_effect_fixture);
    ASSERT_TRUE(doc);

    auto const group_transform = attr(doc.get(), "effect_group", "transform");
    auto const child_style = attr(doc.get(), "selected_child", "style");
    auto const child_transform = attr(doc.get(), "selected_child", "transform");
    auto const src_in_d = attr(doc.get(), "src_in", "d");
    auto const src_in_transform = attr(doc.get(), "src_in", "transform");
    auto const src_out_d = attr(doc.get(), "src_out", "d");
    auto const src_out_transform = attr(doc.get(), "src_out", "transform");
    ASSERT_FALSE(src_in_d.empty());
    ASSERT_FALSE(src_out_d.empty());

    doc->getRoot()->cropToObjects(resolve(doc.get(), {"selected_child"}));

    auto *group = doc->getObjectById("effect_group");
    ASSERT_NE(group, nullptr);
    EXPECT_EQ(attr(doc.get(), "effect_group", "transform"), group_transform);
    EXPECT_EQ(attr(doc.get(), "effect_group", "inkscape:path-effect"), std::string("#fx_ancestor"));

    // The ancestor kept its own dependency sources: one sibling inside it and one
    // sibling outside it. The selected child is still present under the group.
    auto *child = doc->getObjectById("selected_child");
    ASSERT_NE(child, nullptr);
    EXPECT_EQ(child->parent, group);
    EXPECT_EQ(attr(doc.get(), "selected_child", "style"), child_style);
    EXPECT_EQ(attr(doc.get(), "selected_child", "transform"), child_transform);
    EXPECT_NE(doc->getObjectById("src_in"), nullptr);
    EXPECT_NE(doc->getObjectById("src_out"), nullptr);

    // The group is NOT a retained whole-group target: its unrelated sibling is
    // still cropped and only the real retained members remain, in order.
    EXPECT_EQ(doc->getObjectById("drop_in"), nullptr);
    EXPECT_EQ(doc->getObjectById("drop_out"), nullptr);
    EXPECT_EQ(childIds(group), (std::vector<std::string>{"selected_child", "src_in"}));

    // Round trip: ancestor reference and both sources survive save/reopen.
    auto reopened = SPDocument::createNewDocFromMem(sp_repr_save_buf(doc->getReprDoc()).raw());
    ASSERT_TRUE(reopened);
    reopened->ensureUpToDate();

    ASSERT_NE(reopened->getObjectById("effect_group"), nullptr);
    EXPECT_EQ(attr(reopened.get(), "effect_group", "inkscape:path-effect"), std::string("#fx_ancestor"));
    ASSERT_NE(reopened->getObjectById("src_in"), nullptr);
    ASSERT_NE(reopened->getObjectById("src_out"), nullptr);
    EXPECT_EQ(attr(reopened.get(), "src_in", "d"), src_in_d);
    EXPECT_EQ(attr(reopened.get(), "src_in", "transform"), src_in_transform);
    EXPECT_EQ(attr(reopened.get(), "src_out", "d"), src_out_d);
    EXPECT_EQ(attr(reopened.get(), "src_out", "transform"), src_out_transform);
    EXPECT_EQ(reopened->getObjectById("drop_in"), nullptr);
    EXPECT_EQ(reopened->getObjectById("drop_out"), nullptr);

    // A repeated crop on the reopened document must stay stable.
    reopened->getRoot()->cropToObjects(resolve(reopened.get(), {"selected_child"}));
    EXPECT_NE(reopened->getObjectById("effect_group"), nullptr);
    EXPECT_NE(reopened->getObjectById("src_in"), nullptr);
    EXPECT_NE(reopened->getObjectById("src_out"), nullptr);
    EXPECT_EQ(reopened->getObjectById("drop_in"), nullptr);
}

// Whole-group selection is still one target: every member (including the
// unrelated sibling inside) is kept, while the ancestor's source dependency
// outside the group is retained and the unrelated sibling outside is dropped.
TEST_F(CropToObjectsTest, WholeAncestorEffectGroupSelectionKeepsAllMembers)
{
    auto doc = makeDoc(ancestor_effect_fixture);
    ASSERT_TRUE(doc);

    doc->getRoot()->cropToObjects(resolve(doc.get(), {"effect_group"}));

    auto *group = doc->getObjectById("effect_group");
    ASSERT_NE(group, nullptr);
    EXPECT_EQ(childIds(group), (std::vector<std::string>{"selected_child", "src_in", "drop_in"}));
    EXPECT_NE(doc->getObjectById("selected_child"), nullptr);
    EXPECT_NE(doc->getObjectById("src_in"), nullptr);
    EXPECT_NE(doc->getObjectById("src_out"), nullptr);
    EXPECT_EQ(doc->getObjectById("drop_out"), nullptr);
}
