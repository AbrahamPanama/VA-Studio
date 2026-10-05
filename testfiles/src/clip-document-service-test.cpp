// SPDX-License-Identifier: GPL-2.0-or-later
#include <gtest/gtest.h>
#include <algorithm>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include "document.h"
#include "document-undo.h"
#include "inkscape.h"
#include "object/object-set.h"
#include "object/clip-document-service.h"
#include "object/sp-clippath.h"
#include "object/sp-lpe-item.h"
#include "live_effects/lpe-powerclip.h"
#include "util-string/context-string.h"
#include <2geom/pathvector.h>
#include "object/sp-item.h"
#include "object/sp-root.h"
#include "preferences.h"
#include "xml/attribute-record.h"
#include "xml/node.h"
#include "xml/document.h"

using namespace Inkscape::ClipDocumentService;

namespace {
constexpr char fixture[] = R"(<svg xmlns='http://www.w3.org/2000/svg'
 xmlns:inkscape='http://www.inkscape.org/namespaces/inkscape'
 xmlns:sodipodi='http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd'
 xmlns:xlink='http://www.w3.org/1999/xlink' id='root' width='200' height='200' viewBox='0 0 200 200'>
 <defs id='defs'/><sodipodi:namedview id='namedview'/>
 <g id='layer' transform='translate(10,20)'>
 <path id='cutter' d='M 0,0 H 40 V 40 H 0 Z' transform='translate(20,10)'/>
 <path id='target' d='M 0,0 H 100 V 100 H 0 Z' transform='translate(5,3)' style='fill:#ff0000'/>
 <path id='other' d='M 120,120 H 150 V 150 Z'/></g></svg>)";
std::string canonical(Inkscape::XML::Node const *node)
{
    std::string out = std::string(node->name() ? node->name() : "") + '[';
    std::vector<std::string> attrs;
    for (auto const &a : node->attributeList()) {
        attrs.emplace_back(std::string(g_quark_to_string(a.key)) + '=' + a.value.pointer());
    }
    std::sort(attrs.begin(), attrs.end());
    for (auto const &a : attrs) out += std::to_string(a.size()) + ':' + a;
    out += ']';
    if (node->content()) out += std::string(node->content());
    for (auto child = node->firstChild(); child; child = child->next()) out += canonical(child);
    return out + '/';
}
std::string hash(SPDocument &doc)
{
    auto xml = canonical(doc.getReprRoot());
    auto h = g_compute_checksum_for_string(G_CHECKSUM_SHA256, xml.c_str(), xml.size());
    std::string result(h); g_free(h); return result;
}
class ClipDocumentServiceTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!Inkscape::Application::exists()) Inkscape::Application::create(false);
        prefs = Inkscape::Preferences::get();
    }
    std::unique_ptr<SPDocument> make(std::string_view xml = fixture) {
        auto doc = SPDocument::createNewDocFromMem(xml);
        doc->ensureUpToDate();
        Inkscape::DocumentUndo::setUndoSensitive(doc.get(), true);
        Inkscape::DocumentUndo::clearUndo(doc.get());
        return doc;
    }
    SPItem *item(SPDocument &doc, char const *id) { return cast<SPItem>(doc.getObjectById(id)); }
    Inkscape::Preferences *prefs;
};
TEST_F(ClipDocumentServiceTest, GuiBaseline) {
    // SHA256 of full canonical XML captured before extraction (24 fixture pairs).
    std::vector<std::string> const expected{
        "914e1fce807c95e76061362c09c0e164ce8711b915a18cddaaa745eaa68b43e0",
        "a7c9c92a13b23edc6acfc13e79f515448159a6e1f9f93ad3d4cf672b703d542e",
        "986fa70da774d473c6cad1f6273bcf918fb363e40f28150e3e8523faa4a3f4b4",
        "e14c55d22f2738209d3a67febb869572eafe25cf2f03ed0e632a37b684dbb2b7",
        "ac6cb056f2db522b4728229c8587fd603054be7f9aeaf879c6f92c1770be06db",
        "7f2a5612a55cd6ce57ec84f98bfea0814490e39ee899ef4df92656a851cb63a1",
        "5364e2222af7e5a6445ef7ccccc387e736f98cc8bea4434632409721f129794c",
        "36ec42253f88cc5725f5f01c90d0661ce6b06ada9a3660c5720387dbf9018211",
        "ac6cb056f2db522b4728229c8587fd603054be7f9aeaf879c6f92c1770be06db",
        "7f2a5612a55cd6ce57ec84f98bfea0814490e39ee899ef4df92656a851cb63a1",
        "5364e2222af7e5a6445ef7ccccc387e736f98cc8bea4434632409721f129794c",
        "36ec42253f88cc5725f5f01c90d0661ce6b06ada9a3660c5720387dbf9018211",
        "8c864dd2419e29ed55e742e3690d9b17cfe255443376bf942f0374f7d5eb12b2",
        "68d93eee77491a526bb8766a68691161ac38a9d6058ac1e9b00f355cb80d6169",
        "cb92ac2c8d0d8fd7ec1d0e7e5e8015a0a7494d5c7f6b7afe9ab6cf9314cf9959",
        "8b9263c826fc1909a105f5584a05ab1400ad899d10efe552ab9d459d693afc0a",
        "7c81fda07b29557a566181c38ed762419ef41c4a290867bd22ce7122c5f84dd5",
        "a78006c06659888cdbe15e01e842c4645cd4c6b34ebe564482005b395dde7e1b",
        "5fb4bff903648f50e9c43f11a7ce4822e6066358ccc009f5916ade7218c4b28f",
        "957be063a9a62028771835c2d681f046ee3159844ebc82bc7a1d6706d91595b8",
        "7c81fda07b29557a566181c38ed762419ef41c4a290867bd22ce7122c5f84dd5",
        "a78006c06659888cdbe15e01e842c4645cd4c6b34ebe564482005b395dde7e1b",
        "5fb4bff903648f50e9c43f11a7ce4822e6066358ccc009f5916ade7218c4b28f",
        "957be063a9a62028771835c2d681f046ee3159844ebc82bc7a1d6706d91595b8",
        "a8379fe88df54a4dd54275d7e77cd941cabff5b54ce18fbaa8459b023510a60d",
        "820ce782a09059221e7ffd4c0f70d1a49f088b29632ad1546e2a6447aeb82aec",
        "6ad7b90f9bef2c58030623bc11201daf8b0d89f300c7d424767e0202204e47e4",
        "385d3436a7659f7d16b597a0f66d6bcfad58b1bb9f63092ce899ab7434126d83",
        "de8dc463d7a42d6328d967a83accc4f4d398c4fa4941dfab1fa4a9ba45d78cdf",
        "c5c37b12fe12c8ddf9e5c7765ec694aaf0b54411231a77f45465a4e19f7c1799",
        "23f75b43d2057199eb592d7aa6cf51c425ca3fb088598ae8013e6a649278e8c5",
        "36ec42253f88cc5725f5f01c90d0661ce6b06ada9a3660c5720387dbf9018211",
        "de8dc463d7a42d6328d967a83accc4f4d398c4fa4941dfab1fa4a9ba45d78cdf",
        "c5c37b12fe12c8ddf9e5c7765ec694aaf0b54411231a77f45465a4e19f7c1799",
        "23f75b43d2057199eb592d7aa6cf51c425ca3fb088598ae8013e6a649278e8c5",
        "36ec42253f88cc5725f5f01c90d0661ce6b06ada9a3660c5720387dbf9018211",
        "99715eef0161c9e890762ae7305a5ede69bcafe4d7616dd297297c8514dd8b56",
        "6dc7580caf95de5070d1b4b29bf6419d8fab214651d8d684d150e647005863e1",
        "3e130fab66fe6f568d4bca19dd045083c1d634fe31969d0a21111bb989d17a89",
        "c0c1b6af9c419a27ef6c78077eb01cbce4a074fc0c8f67190fcaa77f13cf79d5",
        "87ed1bf781f6ceb535ebf413a878735281d09a892ad59addf82733b0a430511a",
        "fa41171dce185176067b37a88ce80191f6e99cac32e8a5b25f10b430c8ff11ff",
        "acea28e7afb00651edcbc719a4b96d9c5265bb04e2cf0a0367b689b73a11f39d",
        "957be063a9a62028771835c2d681f046ee3159844ebc82bc7a1d6706d91595b8",
        "87ed1bf781f6ceb535ebf413a878735281d09a892ad59addf82733b0a430511a",
        "fa41171dce185176067b37a88ce80191f6e99cac32e8a5b25f10b430c8ff11ff",
        "acea28e7afb00651edcbc719a4b96d9c5265bb04e2cf0a0367b689b73a11f39d",
        "957be063a9a62028771835c2d681f046ee3159844ebc82bc7a1d6706d91595b8",
    };
    size_t snapshot=0;
    prefs->setBool("/options/preservetransform/value",false);
    for (bool clip : {false,true}) for (bool top : {false,true}) for (int grouping : {0,1,2}) for (bool remove : {false,true}) {
        prefs->setBool("/options/maskobject/topmost", top);
        prefs->setInt("/options/maskobject/grouping", grouping);
        prefs->setBool("/options/maskobject/ungrouping", true);
        auto doc = make();
        // createDoc stamps the current build revision. Keep this fixed fixture's
        // metadata identical to the captured pre-extraction document even when
        // another change advances/rebuilds the shared tree. Do not omit metadata
        // from canonicalization or regenerate any of the 48 expected hashes.
        doc->getReprRoot()->setAttribute("inkscape:version", "1.5-dev (7bb90dde77, 2026-10-04, custom)");
        Inkscape::ObjectSet selection(doc.get());
        selection.add(item(*doc,"target")); selection.add(item(*doc,"cutter"));
        selection.setMask(clip,false,remove);
        doc->ensureUpToDate();
        EXPECT_EQ(hash(*doc),expected.at(snapshot++));
        std::cout << "GUI " << clip << top << grouping << remove << " set " << hash(*doc) << '\n';
        selection.unsetMask(clip,true,remove);
        doc->ensureUpToDate();
        EXPECT_EQ(hash(*doc),expected.at(snapshot++));
        std::cout << "GUI " << clip << top << grouping << remove << " release " << hash(*doc) << '\n';
    }
}
TEST_F(ClipDocumentServiceTest, LowerCutterAndPreferenceIndependence) {
    std::string expected_set, expected_release;
    for (bool top : {false,true}) for (int grouping : {0,1,2}) for (bool preserve : {false,true}) {
        prefs->setBool("/options/maskobject/topmost",top);
        prefs->setInt("/options/maskobject/grouping",grouping);
        prefs->setBool("/options/maskobject/ungrouping", !top);
        prefs->setBool("/options/preservetransform/value",preserve);
        auto doc = make(); auto target = item(*doc,"target"); auto cutter = item(*doc,"cutter");
        auto before = canonical(doc->getReprRoot());
        auto prepared = prepareSetClip(doc.get(),target,cutter,{.keep_cutter=false});
        EXPECT_EQ(prepared.status,Status::Prepared);
        EXPECT_EQ(canonical(doc->getReprRoot()),before);
        auto result = setClip(doc.get(),target,cutter,{.keep_cutter=false});
        ASSERT_EQ(result.status,Status::Applied);
        EXPECT_EQ(result.affected_ids,(std::vector<std::string>{"target","cutter"}));
        EXPECT_FALSE(doc->getObjectById("cutter"));
        ASSERT_TRUE(target->getClipObject());
        auto clip = target->getClipObject();
        auto child = cast<SPItem>(clip->firstChild()); ASSERT_TRUE(child);
        EXPECT_STREQ(child->getRepr()->attribute("d"),"M 0,0 H 40 V 40 H 0 Z");
        EXPECT_TRUE(Geom::are_near(child->transform * target->i2doc_affine(),Geom::Translate(30,30),1e-9));
        doc->ensureUpToDate();
        if (expected_set.empty()) expected_set = hash(*doc); else EXPECT_EQ(hash(*doc),expected_set);
        auto released = releaseClip(doc.get(),{target},{.keep_cutter=true});
        ASSERT_EQ(released.status,Status::Applied);
        ASSERT_EQ(released.restored_cutter_ids.size(),1u);
        EXPECT_FALSE(target->getClipObject());
        EXPECT_EQ(target->parent,doc->getObjectById("layer"));
        auto restored = item(*doc,released.restored_cutter_ids[0].c_str()); ASSERT_TRUE(restored);
        EXPECT_EQ(restored->parent,target->parent);
        EXPECT_TRUE(Geom::are_near(restored->i2doc_affine(),Geom::Translate(30,30),1e-9));
        doc->ensureUpToDate();
        if (expected_release.empty()) expected_release = hash(*doc); else EXPECT_EQ(hash(*doc),expected_release);
    }
}
TEST_F(ClipDocumentServiceTest, RetainOrConsumeAndReleasePolicy) {
    for (bool keep_set : {false,true}) for (bool keep_release : {false,true}) {
        auto doc = make(); auto target = item(*doc,"target"); auto cutter = item(*doc,"cutter");
        auto cutter_before = canonical(cutter->getRepr());
        ASSERT_EQ(setClip(doc.get(),target,cutter,{.keep_cutter=keep_set}).status,Status::Applied);
        EXPECT_EQ(bool(doc->getObjectById("cutter")),keep_set);
        if (keep_set) EXPECT_EQ(canonical(item(*doc,"cutter")->getRepr()),cutter_before);
        auto released = releaseClip(doc.get(),{target},{.keep_cutter=keep_release});
        EXPECT_EQ(released.status,Status::Applied);
        EXPECT_EQ(released.restored_cutter_ids.size(),keep_release ? 1u : 0u);
        EXPECT_FALSE(target->getClipObject());
        EXPECT_EQ(bool(doc->getObjectById("cutter")),keep_set);
    }
}
TEST_F(ClipDocumentServiceTest, NativePowerClipInverseAndRequestDefaults) {
    std::string expected;
    for (bool preference : {false,true}) {
        prefs->setString("/live_effects/powerclip/inverse",preference ? "false" : "true");
        prefs->setString("/live_effects/powerclip/hide_clip",preference ? "true" : "false");
        prefs->setString("/live_effects/powerclip/flatten",preference ? "true" : "false");
        prefs->setBool("/options/onungroup",preference);
        auto doc = make(); auto target = item(*doc,"target");
        ASSERT_EQ(setClip(doc.get(),target,item(*doc,"cutter"),{.inverse=true}).status,Status::Applied);
        doc->ensureUpToDate();
        auto lpeitem = cast<SPLPEItem>(target); ASSERT_TRUE(lpeitem);
        auto powerclip = dynamic_cast<Inkscape::LivePathEffect::LPEPowerClip *>(lpeitem->getCurrentLPE());
        ASSERT_TRUE(powerclip);
        EXPECT_STREQ(powerclip->getRepr()->attribute("inverse"),"true");
        EXPECT_STREQ(powerclip->getRepr()->attribute("hide_clip"),"false");
        EXPECT_STREQ(powerclip->getRepr()->attribute("flatten"),"false");
        auto generated = doc->getObjectById(powerclip->getId().c_str()); ASSERT_TRUE(generated);
        EXPECT_STREQ(generated->getRepr()->attribute("class"),"powerclip");
        auto paths = powerclip->getClipPathvector();
        ASSERT_EQ(paths.size(),2u) << "native inverse = expanded target bbox plus cutter";
        auto outer=Geom::bounds_fast(paths[0]); auto cut=Geom::bounds_fast(paths[1]);
        ASSERT_TRUE(outer); ASSERT_TRUE(cut);
        EXPECT_GT(outer->width(),100);
        EXPECT_EQ(paths.winding(cut->midpoint()),0) << "inverse hole excludes cutter interior";
        EXPECT_NE(paths.winding(outer->min()+Geom::Point(1,1)),0) << "inverse admits outside the cutter";
        EXPECT_NE(generated->getRepr()->attribute("d"),nullptr);
        if (expected.empty()) expected=hash(*doc); else EXPECT_EQ(hash(*doc),expected);
        auto released = releaseClip(doc.get(),{target},{.keep_cutter=true});
        ASSERT_EQ(released.status,Status::Applied);
        EXPECT_FALSE(lpeitem->hasPathEffectOfType(Inkscape::LivePathEffect::POWERCLIP));
        EXPECT_FALSE(target->getClipObject());
        EXPECT_EQ(released.restored_cutter_ids.size(),1u) << "PowerClip helper is never restored as artwork";
        auto restored = item(*doc,released.restored_cutter_ids[0].c_str()); ASSERT_TRUE(restored);
        auto restored_bounds=restored->documentGeometricBounds(); ASSERT_TRUE(restored_bounds);
        EXPECT_TRUE(Geom::are_near(restored_bounds->min(),Geom::Point(30,30),1e-9));
        EXPECT_TRUE(Geom::are_near(restored_bounds->max(),Geom::Point(70,70),1e-9));
    }
}
TEST_F(ClipDocumentServiceTest, UnsupportedInverseIsReadOnly) {
    for (bool bad_target : {false,true}) {
        auto doc = make();
        auto xml=doc->getReprDoc()->createElement(bad_target ? "svg:text" : "svg:use");
        xml->setAttribute("id","unsupported");
        if (!bad_target) xml->setAttribute("xlink:href","#other");
        doc->getObjectById("layer")->getRepr()->appendChild(xml); Inkscape::GC::release(xml);
        doc->ensureUpToDate();
        auto before = canonical(doc->getReprRoot());
        auto result = setClip(doc.get(),bad_target ? item(*doc,"unsupported") : item(*doc,"target"),
                              bad_target ? item(*doc,"cutter") : item(*doc,"unsupported"),{.inverse=true});
        EXPECT_EQ(result.status,Status::Refused);
        EXPECT_EQ(result.reason,bad_target ? Reason::UnsupportedInverseTarget : Reason::UnsupportedInverseCutter);
        EXPECT_EQ(canonical(doc->getReprRoot()),before);
    }
}
TEST_F(ClipDocumentServiceTest, PairAdmissionPreservesDocument) {
    auto doc = make(); auto target=item(*doc,"target"); auto cutter=item(*doc,"cutter");
    auto before=canonical(doc->getReprRoot());
    EXPECT_EQ(setClip(nullptr,target,cutter).reason,Reason::InvalidDocument);
    EXPECT_EQ(setClip(doc.get(),nullptr,cutter).reason,Reason::InvalidItem);
    EXPECT_EQ(setClip(doc.get(),target,target).reason,Reason::OverlappingRoles);
    EXPECT_EQ(setClip(doc.get(),item(*doc,"layer"),cutter).reason,Reason::OverlappingRoles);
    EXPECT_EQ(canonical(doc->getReprRoot()),before);
    target->setAttribute("sodipodi:insensitive","true");
    before=canonical(doc->getReprRoot());
    EXPECT_EQ(setClip(doc.get(),target,cutter).reason,Reason::ProtectedItem);
    EXPECT_EQ(canonical(doc->getReprRoot()),before);
    target->removeAttribute("sodipodi:insensitive"); target->setAttribute("transform","scale(0)");
    before=canonical(doc->getReprRoot());
    EXPECT_EQ(setClip(doc.get(),target,cutter).reason,Reason::SingularTransform);
    EXPECT_EQ(canonical(doc->getReprRoot()),before);
}
TEST_F(ClipDocumentServiceTest, ReleasePartialCompatibilityAndNoRecursiveTraversal) {
    auto doc=make(); auto target=item(*doc,"target"); auto other=item(*doc,"other");
    ASSERT_EQ(setClip(doc.get(),target,item(*doc,"cutter"),{.keep_cutter=false}).status,Status::Applied);
    auto before=canonical(doc->getReprRoot());
    EXPECT_EQ(releaseClip(doc.get(),{item(*doc,"layer")}).reason,Reason::NotClipped);
    EXPECT_EQ(canonical(doc->getReprRoot()),before);
    auto other_xml=canonical(other->getRepr());
    auto prepared=prepareReleaseClip(doc.get(),{target,other,target,nullptr},{.keep_cutter=false});
    EXPECT_EQ(prepared.status,Status::Prepared);
    EXPECT_EQ(canonical(doc->getReprRoot()),before);
    auto result=releaseClip(doc.get(),{target,other,target,nullptr},{.keep_cutter=false});
    EXPECT_EQ(result.status,Status::Applied);
    EXPECT_EQ(result.affected_ids,(std::vector<std::string>{"target"}));
    ASSERT_EQ(result.excluded.size(),2u);
    EXPECT_EQ(result.excluded[0].reason,Reason::NotClipped);
    EXPECT_EQ(result.excluded[1].reason,Reason::InvalidItem);
    EXPECT_EQ(result.covered_ids,(std::vector<std::string>{"target"}));
    EXPECT_EQ(canonical(other->getRepr()),other_xml);
    EXPECT_EQ(releaseClip(doc.get(),{}).status,Status::Unchanged);
}
TEST_F(ClipDocumentServiceTest, HelperGroupRemainsAndUngroupRequestRefused) {
    auto doc=make(); auto group=item(*doc,"layer");
    group->setAttribute("inkscape:groupmode","maskhelper");
    // Move cutter outside the target group, preserving explicit independent roles.
    auto cutter=item(*doc,"cutter"); auto repr=cutter->getRepr();
    Inkscape::GC::anchor(repr); repr->parent()->removeChild(repr); doc->getReprRoot()->appendChild(repr); Inkscape::GC::release(repr);
    cutter=item(*doc,"cutter");
    ASSERT_EQ(setClip(doc.get(),group,cutter).status,Status::Applied);
    auto before=canonical(doc->getReprRoot());
    EXPECT_EQ(releaseClip(doc.get(),{group},{.ungroup_helpers=true}).reason,Reason::UnsupportedHelperUngroup);
    EXPECT_EQ(canonical(doc->getReprRoot()),before);
    ASSERT_EQ(releaseClip(doc.get(),{group}).status,Status::Applied);
    EXPECT_EQ(item(*doc,"target")->parent,group);
    EXPECT_EQ(doc->getObjectById("layer"),group);
}
TEST_F(ClipDocumentServiceTest, CallerOwnsOneUndoAndExactXmlRestoration) {
    for (bool inverse : {false,true}) for (bool keep : {false,true}) for (bool release : {false,true}) {
        auto doc=make(); auto before=canonical(doc->getReprRoot());
        auto interaction=Inkscape::DocumentUndo::beginRollbackableInteraction(doc.get());
        ASSERT_TRUE(interaction);
        ASSERT_EQ(setClip(doc.get(),item(*doc,"target"),item(*doc,"cutter"),{inverse,keep}).status,Status::Applied);
        if (release) ASSERT_EQ(releaseClip(doc.get(),{item(*doc,"target")},{.keep_cutter=true}).status,Status::Applied);
        ASSERT_TRUE(Inkscape::DocumentUndo::interactionActive(doc.get()));
        // No helper settlement: the caller's interaction remains active.
        interaction->commit(Inkscape::Util::Internal::ContextString{"Caller clip"},"");
        doc->ensureUpToDate();
        auto after=canonical(doc->getReprRoot());
        ASSERT_TRUE(Inkscape::DocumentUndo::undo(doc.get())); doc->ensureUpToDate();
        EXPECT_EQ(canonical(doc->getReprRoot()),before);
        EXPECT_FALSE(Inkscape::DocumentUndo::undo(doc.get()));
        ASSERT_TRUE(Inkscape::DocumentUndo::redo(doc.get())); doc->ensureUpToDate();
        EXPECT_EQ(canonical(doc->getReprRoot()),after);
        EXPECT_FALSE(Inkscape::DocumentUndo::redo(doc.get()));
    }
}
TEST_F(ClipDocumentServiceTest, ReferencedCutterRefusedWhenConsumed) {
    for (int orphan_policy : {0,1}) {
        prefs->setInt("/options/cloneorphans/value",orphan_policy);
        auto doc=make();
        auto repr=doc->getReprDoc()->createElement("svg:use");
        repr->setAttribute("id","follower"); repr->setAttribute("xlink:href","#cutter");
        doc->getObjectById("layer")->getRepr()->appendChild(repr); Inkscape::GC::release(repr);
        doc->ensureUpToDate(); auto before=canonical(doc->getReprRoot());
        EXPECT_EQ(setClip(doc.get(),item(*doc,"target"),item(*doc,"cutter"),{.keep_cutter=false}).reason,Reason::ReferencedCutter);
        EXPECT_EQ(canonical(doc->getReprRoot()),before);
        auto follower=canonical(doc->getObjectById("follower")->getRepr());
        ASSERT_EQ(setClip(doc.get(),item(*doc,"target"),item(*doc,"cutter"),{.keep_cutter=true}).status,Status::Applied);
        EXPECT_EQ(canonical(doc->getObjectById("follower")->getRepr()),follower);
    }
}
TEST_F(ClipDocumentServiceTest, RootOverlapIsCoveredAndChildrenRetainTheirClips) {
    auto doc=make(); auto group=item(*doc,"layer"); auto target=item(*doc,"target");
    ASSERT_EQ(setClip(doc.get(),target,item(*doc,"cutter")).status,Status::Applied);
    group->setAttribute("clip-path",target->getRepr()->attribute("clip-path"));
    auto target_before=canonical(target->getRepr());
    auto result=releaseClip(doc.get(),{target,group,group},{.keep_cutter=false});
    EXPECT_EQ(result.status,Status::Applied);
    EXPECT_EQ(result.affected_ids,(std::vector<std::string>{"layer"}));
    EXPECT_EQ(result.covered_ids.size(),2u);
    EXPECT_FALSE(group->getClipObject()); EXPECT_TRUE(target->getClipObject());
    EXPECT_EQ(canonical(target->getRepr()),target_before);
}
TEST_F(ClipDocumentServiceTest, CallerRollbackLeavesCanonicalXmlAndHistoryIntact) {
    auto doc=make(); auto before=canonical(doc->getReprRoot());
    auto interaction=Inkscape::DocumentUndo::beginRollbackableInteraction(doc.get()); ASSERT_TRUE(interaction);
    ASSERT_EQ(setClip(doc.get(),item(*doc,"target"),item(*doc,"cutter"),{.inverse=true,.keep_cutter=false}).status,Status::Applied);
    interaction->rollback(); doc->ensureUpToDate();
    EXPECT_EQ(canonical(doc->getReprRoot()),before);
    EXPECT_FALSE(Inkscape::DocumentUndo::undo(doc.get()));
}
TEST_F(ClipDocumentServiceTest, GroupTargetAndNestedCutterStayStructured) {
    constexpr std::string_view xml=R"(<svg xmlns='http://www.w3.org/2000/svg' xmlns:inkscape='http://www.inkscape.org/namespaces/inkscape' id='root' width='200' height='200'>
      <defs id='defs'/><g id='cutter' transform='translate(10,20)'><g id='nested'><path id='cut-child' d='M 0,0 H 40 V 40 H 0 Z'/></g></g>
      <g id='target' transform='scale(2)'><path id='art-child' d='M 0,0 H 100 V 100 H 0 Z'/></g></svg>)";
    for (bool inverse : {false,true}) {
        auto doc=make(xml); auto target=item(*doc,"target"); auto child=item(*doc,"art-child");
        auto structure_before=canonical(child->getRepr());
        auto bounds_before=child->documentVisualBounds(); ASSERT_TRUE(bounds_before);
        ASSERT_EQ(setClip(doc.get(),target,item(*doc,"cutter"),{.inverse=inverse,.keep_cutter=false}).status,Status::Applied);
        EXPECT_EQ(child->parent,target);
        EXPECT_EQ(target->parent,doc->getRoot());
        ASSERT_TRUE(target->getClipObject());
        if (!inverse) EXPECT_EQ(canonical(child->getRepr()),structure_before);
        ASSERT_EQ(releaseClip(doc.get(),{target},{.keep_cutter=true}).status,Status::Applied);
        EXPECT_EQ(child->parent,target);
        if (!inverse) EXPECT_EQ(canonical(child->getRepr()),structure_before);
        doc->ensureUpToDate(); auto bounds_after=child->documentVisualBounds(); ASSERT_TRUE(bounds_after);
        EXPECT_TRUE(Geom::are_near(bounds_before->min(),bounds_after->min(),1e-9));
        EXPECT_TRUE(Geom::are_near(bounds_before->max(),bounds_after->max(),1e-9));
    }
}
TEST_F(ClipDocumentServiceTest, CloneSourceAndMissingSourceAreTypedRefusals) {
    auto doc=make();
    auto repr=doc->getReprDoc()->createElement("svg:use");
    repr->setAttribute("id","clone"); repr->setAttribute("xlink:href","#target");
    doc->getObjectById("layer")->getRepr()->appendChild(repr); Inkscape::GC::release(repr);
    doc->ensureUpToDate(); auto before=canonical(doc->getReprRoot());
    EXPECT_EQ(setClip(doc.get(),item(*doc,"target"),item(*doc,"clone")).reason,Reason::CloneWithSource);
    EXPECT_EQ(canonical(doc->getReprRoot()),before);
    item(*doc,"clone")->setAttribute("xlink:href","#missing");
    doc->ensureUpToDate(); before=canonical(doc->getReprRoot());
    EXPECT_EQ(setClip(doc.get(),item(*doc,"target"),item(*doc,"clone")).reason,Reason::MissingSource);
    EXPECT_EQ(canonical(doc->getReprRoot()),before);
}
}
