// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "vacards-cli-transaction.h"
#include <memory>
#include <optional>
namespace Inkscape::VACardsCli {
enum class TokenKind { ExplodeAnalysis, ExplodeContour, NestAnalysis, NestSolution };
// Roots: ExplodeAnalysis/NestAnalysis have no parent. ExplodeContour requires
// ExplodeAnalysis; NestSolution requires NestAnalysis. Cross-family lineage,
// cross-session/document/incarnation and cycles are refused as invalid-token.
struct TokenParent { std::string id; TokenKind kind; };
// Lookup compares current identity/freshness, never reconstructs creation params.
// Session revision is provenance only; unrelated status/options do not invalidate.
struct TokenValidationContext {
    std::string session, document, engine, catalog_hash;
    std::uint64_t incarnation = 0, current_document_revision = 0, target_generation = 0;
};
// Unique stable allocation identity. Same identity must have the same bytes and
// owner; shared buffers count once across all tokens AND held retired leases.
// New child storage uses a new identity. Admission charges only unseen identities;
// the final lease releases the charge. No eviction, TTL or second registry.
struct TokenAllocation {
    std::string identity;
    std::uint64_t bytes = 0;
    std::shared_ptr<void const> owner;
};
struct TokenError { std::string code, message, hint; bool retryable = false; };
// Payload is immutable and pointer-free with respect to document/XML. Shared
// root budget owns buffers once. Child tokens share that charge, never a TTL.
struct TokenPayload { virtual ~TokenPayload() = default; virtual TokenKind kind() const noexcept = 0; };
struct TokenBudget { std::size_t roots = 4; std::uint64_t bytes = 512ULL * 1024 * 1024; };
struct TokenLease {
    std::string id;
    TokenKind kind = TokenKind::ExplodeAnalysis;
    TokenBinding binding;
    std::optional<TokenParent> parent;
    std::shared_ptr<TokenPayload const> payload;
    std::vector<TokenAllocation> allocations;
};
struct TokenResult { std::optional<TokenLease> value; std::optional<TokenError> error; };
struct TokenRelease { std::vector<std::string> released, not_found; std::optional<TokenError> error; };
struct TokenSnapshot {
    std::vector<std::string> active_tokens;
    std::size_t active_roots = 0;
    std::uint64_t charged_bytes = 0;
};
class TokenStore {
public:
    explicit TokenStore(TokenBudget limits = {});
    ~TokenStore();
    TokenStore(TokenStore const &) = delete;
    TokenStore &operator=(TokenStore const &) = delete;
    TokenResult retain(TokenKind, TokenBinding const &, std::shared_ptr<TokenPayload const>,
                       std::vector<TokenAllocation> allocations,
                       std::optional<TokenParent> parent = {});
    // Returned binding is immutable creation evidence; revalidate stored ordered
    // targets/dependencies on the owner thread before use. Parent must match ID
    // AND kind; a child recipe need not equal its parent's normalized params.
    TokenResult lookup(std::string const &, TokenKind, TokenValidationContext const &,
                       std::optional<TokenParent> expected_parent = {}) const;
    TokenRelease release_subtree(std::string const &);
    // Only native committed settlement authorizes consumption. Failed preflight
    // is retryable with a fresh admission; dry-run never calls retain/consume.
    // A descendant ID consumes its entire root family after native commit only.
    // Pass the successfully validated PRE-commit context; native commit may
    // already have advanced document revision. Only the settlement owner calls.
    // Failed/unchanged/preview/cancelled work retains the family and all leases.
    TokenRelease consume_after_commit(std::string const &, TokenValidationContext const &);
    TokenRelease invalidate_document(std::string const &document_id, std::uint64_t incarnation);
    // Value-only view; includes unique allocations held by retired leases.
    TokenSnapshot snapshot() const;
    // Retain/release/consume changes session revision once if membership changes;
    // status reports active roots/tokens and unique charged bytes (incl. retired
    // held leases). Document revision and target generation do not change.
private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};
}
