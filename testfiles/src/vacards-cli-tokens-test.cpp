// SPDX-License-Identifier: GPL-2.0-or-later
#include <gtest/gtest.h>
#include "actions/vacards-cli-tokens.h"
#include <thread>
using namespace Inkscape::VACardsCli;
namespace {
struct Payload : TokenPayload {
    TokenKind type;
    explicit Payload(TokenKind k) : type(k) {}
    TokenKind kind() const noexcept override { return type; }
};
TokenBinding binding(std::uint64_t incarnation = 1) {
    TokenBinding b; b.session="s"; b.document="d"; b.engine="native"; b.catalog_hash="hash";
    b.incarnation=incarnation; b.revision=7; b.target_generation=3; return b;
}
TokenValidationContext context() { return {"s","d","native","hash",1,7,3}; }
TokenResult retain(TokenStore &s, TokenKind k = TokenKind::ExplodeAnalysis,
                   std::vector<TokenAllocation> a = {}, std::optional<TokenParent> parent = {}, TokenBinding b = binding()) {
    return s.retain(k,b,std::make_shared<Payload>(k),std::move(a),std::move(parent));
}
}
TEST(M3Tokens, RejectsInvalidPayloadWithoutMutation) {
    TokenStore s;
    auto r=s.retain(TokenKind::ExplodeAnalysis,binding(),{},{});
    ASSERT_TRUE(r.error); EXPECT_EQ(r.error->code,"invalid-token"); EXPECT_TRUE(s.snapshot().active_tokens.empty());
    EXPECT_TRUE(s.retain(TokenKind::NestAnalysis,binding(),std::make_shared<Payload>(TokenKind::ExplodeAnalysis),{}).error);
}
TEST(M3Tokens, RootBudgetDoesNotEvictAndChildDoesNotChargeRoot) {
    TokenStore s({1,100}); auto root=retain(s); ASSERT_TRUE(root.value);
    auto refused=retain(s); ASSERT_TRUE(refused.error); EXPECT_EQ(refused.error->code,"token-capacity");
    auto child=retain(s,TokenKind::ExplodeContour,{},TokenParent{root.value->id,TokenKind::ExplodeAnalysis});
    ASSERT_TRUE(child.value); EXPECT_EQ(s.snapshot().active_roots,1u); EXPECT_EQ(s.snapshot().active_tokens.size(),2u);
    EXPECT_TRUE(s.lookup(root.value->id,TokenKind::ExplodeAnalysis,context()).value);
}
TEST(M3Tokens, UniqueStorageAndRetiredLeasesRemainChargedUntilLastHold) {
    TokenStore s({4,100}); auto buffer=std::make_shared<int>(1);
    auto root=retain(s,TokenKind::ExplodeAnalysis,{{"pixels",80,buffer}}); ASSERT_TRUE(root.value);
    auto child=retain(s,TokenKind::ExplodeContour,{{"pixels",80,buffer},{"contour",20,std::make_shared<int>(2)}},
                      TokenParent{root.value->id,TokenKind::ExplodeAnalysis}); ASSERT_TRUE(child.value);
    EXPECT_EQ(s.snapshot().charged_bytes,100u);
    auto release=s.release_subtree(root.value->id); EXPECT_EQ(release.released.size(),2u);
    EXPECT_TRUE(s.snapshot().active_tokens.empty()); EXPECT_EQ(s.snapshot().charged_bytes,100u);
    EXPECT_EQ(retain(s,TokenKind::NestAnalysis,{{"new",1,std::make_shared<int>(3)}}).error->code,"token-capacity");
    child.value.reset(); EXPECT_EQ(s.snapshot().charged_bytes,80u);
    root.value.reset(); EXPECT_EQ(s.snapshot().charged_bytes,0u);
    EXPECT_TRUE(retain(s,TokenKind::NestAnalysis,{{"new",100,buffer}}).value);
}
TEST(M3Tokens, AllocationIdentityRejectsSizeOrOwnerChangesAtomically) {
    TokenStore s; auto buffer=std::make_shared<int>(1);
    auto root=retain(s,TokenKind::ExplodeAnalysis,{{"pixels",80,buffer}}); ASSERT_TRUE(root.value);
    EXPECT_EQ(retain(s,TokenKind::NestAnalysis,{{"pixels",81,buffer}}).error->code,"invalid-token");
    EXPECT_EQ(retain(s,TokenKind::NestAnalysis,{{"pixels",80,std::make_shared<int>(1)}}).error->code,"invalid-token");
    EXPECT_EQ(s.snapshot().active_roots,1u); EXPECT_EQ(s.snapshot().charged_bytes,80u);
    auto reused=retain(s,TokenKind::NestAnalysis,root.value->allocations); ASSERT_TRUE(reused.value);
    EXPECT_EQ(s.snapshot().charged_bytes,80u);
}
TEST(M3Tokens, LineageKindSessionAndFreshnessAreDistinct) {
    TokenStore s; auto root=retain(s); ASSERT_TRUE(root.value);
    EXPECT_TRUE(retain(s,TokenKind::NestSolution,{},TokenParent{root.value->id,TokenKind::ExplodeAnalysis}).error);
    auto child=retain(s,TokenKind::ExplodeContour,{},TokenParent{root.value->id,TokenKind::ExplodeAnalysis}); ASSERT_TRUE(child.value);
    auto c=context(); c.session="other";
    EXPECT_EQ(s.lookup(child.value->id,TokenKind::ExplodeContour,c).error->code,"invalid-token");
    for (unsigned changed=0; changed<6; ++changed) {
        c=context();
        switch(changed) {
            case 0: c.document="other"; break;
            case 1: ++c.incarnation; break;
            case 2: ++c.current_document_revision; break;
            case 3: ++c.target_generation; break;
            case 4: c.engine="other"; break;
            case 5: c.catalog_hash="other"; break;
        }
        auto stale=s.lookup(child.value->id,TokenKind::ExplodeContour,c);
        ASSERT_TRUE(stale.error); EXPECT_EQ(stale.error->code,"stale-plan");
    }
    EXPECT_EQ(s.lookup(child.value->id,TokenKind::ExplodeContour,context(),TokenParent{root.value->id,TokenKind::NestAnalysis}).error->code,"invalid-token");
    auto b=binding(); b.session_revision=900;
    EXPECT_TRUE(retain(s,TokenKind::ExplodeContour,{},TokenParent{root.value->id,TokenKind::ExplodeAnalysis},b).value);
}
TEST(M3Tokens, DescendantCommitConsumesWholeFamilyButStaleCommitDoesNot) {
    TokenStore s; auto root=retain(s); ASSERT_TRUE(root.value);
    auto child=retain(s,TokenKind::ExplodeContour,{},TokenParent{root.value->id,TokenKind::ExplodeAnalysis}); ASSERT_TRUE(child.value);
    auto sibling=retain(s,TokenKind::ExplodeContour,{},TokenParent{root.value->id,TokenKind::ExplodeAnalysis}); ASSERT_TRUE(sibling.value);
    auto c=context(); ++c.incarnation;
    EXPECT_TRUE(s.consume_after_commit(child.value->id,c).error); EXPECT_EQ(s.snapshot().active_tokens.size(),3u);
    EXPECT_EQ(s.consume_after_commit(child.value->id,context()).released.size(),3u);
    EXPECT_TRUE(s.consume_after_commit(child.value->id,context()).error); EXPECT_TRUE(s.snapshot().active_tokens.empty());
}
TEST(M3Tokens, ReleaseSubtreeAndInvalidationRespectIncarnation) {
    TokenStore s; auto root=retain(s); auto other=retain(s,TokenKind::NestAnalysis,{}, {},binding(2));
    ASSERT_TRUE(root.value); ASSERT_TRUE(other.value);
    auto child=retain(s,TokenKind::ExplodeContour,{},TokenParent{root.value->id,TokenKind::ExplodeAnalysis}); ASSERT_TRUE(child.value);
    EXPECT_EQ(s.release_subtree(child.value->id).released.size(),1u); EXPECT_EQ(s.snapshot().active_roots,2u);
    EXPECT_EQ(s.release_subtree("missing").not_found,std::vector<std::string>{"missing"});
    EXPECT_EQ(s.invalidate_document("d",1).released,std::vector<std::string>{root.value->id});
    EXPECT_EQ(s.snapshot().active_tokens,std::vector<std::string>{other.value->id});
}
TEST(M3Tokens, ConcurrentReleaseConsumeAndSnapshotSettleOnce) {
    TokenStore s; auto root=retain(s,TokenKind::ExplodeAnalysis,{{"a",50,std::make_shared<int>(1)}}); ASSERT_TRUE(root.value);
    TokenRelease a,b;
    std::thread first([&]{ a=s.release_subtree(root.value->id); });
    std::thread second([&]{ b=s.consume_after_commit(root.value->id,context()); });
    auto during=s.snapshot(); EXPECT_LE(during.active_roots,1u); EXPECT_EQ(during.charged_bytes,50u);
    first.join(); second.join(); EXPECT_EQ(a.released.size()+b.released.size(),1u);
    EXPECT_EQ(s.snapshot().charged_bytes,50u); root.value.reset(); EXPECT_EQ(s.snapshot().charged_bytes,0u);
}

TEST(M3Tokens, LeaseOutlivesStoreWithoutDocumentOrDanglingStorage) {
    std::optional<TokenLease> held;
    std::weak_ptr<int> witness;
    {
        TokenStore store; auto owner=std::make_shared<int>(42); witness=owner;
        auto result=retain(store,TokenKind::ExplodeAnalysis,{{"storage",4,owner}});
        ASSERT_TRUE(result.value); held=std::move(result.value);
    }
    ASSERT_FALSE(witness.expired()); EXPECT_EQ(*witness.lock(),42);
    held.reset(); EXPECT_TRUE(witness.expired());
}

TEST(M3Tokens, RetiredIdentityCannotBeReboundToDifferentStorage) {
    TokenStore store; auto original=std::make_shared<int>(1);
    auto root=retain(store,TokenKind::ExplodeAnalysis,{{"identity",4,original}}); ASSERT_TRUE(root.value);
    store.release_subtree(root.value->id); root.value.reset(); EXPECT_EQ(store.snapshot().charged_bytes,0u);
    auto rebound=retain(store,TokenKind::ExplodeAnalysis,{{"identity",4,std::make_shared<int>(2)}});
    ASSERT_TRUE(rebound.error); EXPECT_EQ(rebound.error->code,"invalid-token");
    EXPECT_TRUE(retain(store,TokenKind::ExplodeAnalysis,{{"identity",4,original}}).value);
    EXPECT_EQ(store.snapshot().charged_bytes,4u);
}

TEST(M3Tokens, CopiedWorkerPayloadKeepsRetiredCharge) {
    TokenStore store;
    auto root=retain(store,TokenKind::ExplodeAnalysis,{{"worker",70,std::make_shared<int>(1)}}); ASSERT_TRUE(root.value);
    auto worker=root.value->payload;
    store.release_subtree(root.value->id); root.value.reset();
    EXPECT_EQ(store.snapshot().charged_bytes,70u); EXPECT_TRUE(store.snapshot().active_tokens.empty());
    worker.reset(); EXPECT_EQ(store.snapshot().charged_bytes,0u);
}
