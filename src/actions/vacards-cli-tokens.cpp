// SPDX-License-Identifier: GPL-2.0-or-later
#include "vacards-cli-tokens.h"
#include <algorithm>
#include <map>
#include <mutex>
#include <set>
#include <glib.h>
namespace Inkscape::VACardsCli {
namespace {
TokenError invalid() { return {"invalid-token", "Token identity, lineage or allocation is invalid.", "Use a token from this session and the declared token family.", false}; }
TokenError stale() { return {"stale-plan", "Token capture is no longer current.", "Prepare a new token against the current document.", false}; }
bool same_owner(std::shared_ptr<void const> const &a, std::shared_ptr<void const> const &b) {
    return !a.owner_before(b) && !b.owner_before(a) && a.get() == b.get();
}
std::optional<TokenError> validate(TokenLease const &lease, TokenValidationContext const &c) {
    auto const &b = lease.binding;
    if (b.session != c.session) return invalid();
    if (b.document != c.document || b.incarnation != c.incarnation || b.revision != c.current_document_revision ||
        b.target_generation != c.target_generation || b.engine != c.engine || b.catalog_hash != c.catalog_hash) return stale();
    return {};
}
TokenValidationContext context(TokenBinding const &b) {
    return {b.session, b.document, b.engine, b.catalog_hash, b.incarnation, b.revision, b.target_generation};
}
}
struct TokenStore::Impl {
    struct Charge { std::uint64_t bytes; std::shared_ptr<void const> owner; };
    struct PayloadHold {
        std::shared_ptr<TokenPayload const> payload;
        std::vector<TokenAllocation> allocations;
    };
    TokenBudget limits;
    mutable std::mutex mutex;
    std::map<std::string, TokenLease> active;
    // This is the single allocation ledger. Weak entries never prolong a charge;
    // active and retired leases own it, including after this store is destroyed.
    struct Allocation {
        std::uint64_t bytes;
        std::weak_ptr<void const> owner;
        std::weak_ptr<Charge> charge;
    };
    // Weak identity tombstones prevent reuse with another owner/size, but retain
    // no storage. The same still-live original allocation can be charged again.
    std::map<std::string, Allocation> allocations;
    explicit Impl(TokenBudget b) : limits(b) {}
    TokenSnapshot snapshot() const {
        TokenSnapshot out;
        for (auto const &[id, lease] : active) {
            out.active_tokens.push_back(id);
            if (!lease.parent) ++out.active_roots;
        }
        for (auto const &[id, allocation] : allocations)
            if (auto charge = allocation.charge.lock()) out.charged_bytes += charge->bytes;
        return out;
    }
    TokenRelease release(std::string const &id) {
        if (!active.contains(id)) return {{}, {id}, {}};
        std::set<std::string> retired{id};
        bool added;
        do {
            added = false;
            for (auto const &[key, lease] : active)
                if (lease.parent && retired.contains(lease.parent->id)) added |= retired.insert(key).second;
        } while (added);
        TokenRelease out;
        out.released.assign(retired.begin(), retired.end());
        for (auto const &key : retired) active.erase(key);
        return out;
    }
};
TokenStore::TokenStore(TokenBudget limits) : _impl(std::make_unique<Impl>(limits)) {}
TokenStore::~TokenStore() = default;
TokenSnapshot TokenStore::snapshot() const {
    std::lock_guard lock(_impl->mutex);
    return _impl->snapshot();
}
TokenResult TokenStore::retain(TokenKind kind, TokenBinding const &binding, std::shared_ptr<TokenPayload const> payload,
                               std::vector<TokenAllocation> allocations, std::optional<TokenParent> parent) {
    std::lock_guard lock(_impl->mutex);
    bool root = kind == TokenKind::ExplodeAnalysis || kind == TokenKind::NestAnalysis;
    if (!payload || payload->kind() != kind || binding.session.empty() || binding.document.empty() ||
        binding.engine.empty() || binding.catalog_hash.empty() || !binding.incarnation || root == bool(parent))
        return {{}, invalid()};
    if (parent) {
        auto expected = kind == TokenKind::ExplodeContour ? TokenKind::ExplodeAnalysis : TokenKind::NestAnalysis;
        auto found = _impl->active.find(parent->id);
        if (parent->kind != expected || found == _impl->active.end() || found->second.kind != expected ||
            validate(found->second, context(binding))) return {{}, invalid()};
    }
    auto current = _impl->snapshot();
    if (root && current.active_roots >= _impl->limits.roots)
        return {{}, TokenError{"token-capacity", "Active token root limit reached.", "Release an unused token family.", false}};
    // Validate and budget the entire request before changing membership or charges.
    std::map<std::string, std::shared_ptr<Impl::Charge>> charges;
    auto bytes = current.charged_bytes;
    for (auto const &a : allocations) {
        if (a.identity.empty() || !a.owner) return {{}, invalid()};
        std::shared_ptr<Impl::Charge> charge;
        if (auto it = charges.find(a.identity); it != charges.end()) charge = it->second;
        else if (auto it = _impl->allocations.find(a.identity); it != _impl->allocations.end()) {
            charge = it->second.charge.lock();
            if (!charge && (it->second.bytes != a.bytes || !same_owner(it->second.owner.lock(), a.owner)))
                return {{}, invalid()};
        }
        if (charge) {
            std::shared_ptr<void const> alias(charge, charge->owner.get());
            if (charge->bytes != a.bytes || (!same_owner(charge->owner, a.owner) && !same_owner(alias, a.owner)))
                return {{}, invalid()};
        } else {
            if (bytes > _impl->limits.bytes || a.bytes > _impl->limits.bytes - bytes)
                return {{}, TokenError{"token-capacity", "Retained storage limit reached.", "Release unused tokens and wait for held work to settle.", false}};
            bytes += a.bytes;
            charge = std::make_shared<Impl::Charge>(Impl::Charge{a.bytes, a.owner});
        }
        charges[a.identity] = std::move(charge);
    }
    for (auto &a : allocations) {
        auto const &charge = charges.at(a.identity);
        a.owner = std::shared_ptr<void const>(charge, charge->owner.get());
    }
    auto uuid = g_uuid_string_random();
    std::string id = uuid; g_free(uuid);
    // A worker copying just the immutable payload still holds its allocation charge.
    auto hold = std::make_shared<Impl::PayloadHold>(Impl::PayloadHold{payload, allocations});
    payload = std::shared_ptr<TokenPayload const>(hold, payload.get());
    TokenLease lease{id, kind, binding, std::move(parent), std::move(payload), std::move(allocations)};
    for (auto const &[key, charge] : charges)
        _impl->allocations[key] = {charge->bytes, charge->owner, charge};
    _impl->active.emplace(id, lease);
    return {std::move(lease), {}};
}
TokenResult TokenStore::lookup(std::string const &id, TokenKind kind, TokenValidationContext const &c,
                               std::optional<TokenParent> parent) const {
    std::lock_guard lock(_impl->mutex);
    auto it = _impl->active.find(id);
    if (it == _impl->active.end() || it->second.kind != kind) return {{}, invalid()};
    auto const &lease = it->second;
    if (parent && (!lease.parent || parent->id != lease.parent->id || parent->kind != lease.parent->kind)) return {{}, invalid()};
    if (auto error = validate(lease, c)) return {{}, error};
    return {lease, {}};
}
TokenRelease TokenStore::release_subtree(std::string const &id) {
    std::lock_guard lock(_impl->mutex);
    return _impl->release(id);
}
TokenRelease TokenStore::consume_after_commit(std::string const &id, TokenValidationContext const &c) {
    std::lock_guard lock(_impl->mutex);
    auto it = _impl->active.find(id);
    if (it == _impl->active.end()) return {{}, {}, invalid()};
    if (auto error = validate(it->second, c)) return {{}, {}, error};
    auto root = id;
    while (_impl->active.at(root).parent) root = _impl->active.at(root).parent->id;
    return _impl->release(root);
}
TokenRelease TokenStore::invalidate_document(std::string const &document, std::uint64_t incarnation) {
    std::lock_guard lock(_impl->mutex);
    TokenRelease out;
    for (auto const &[id, lease] : _impl->active)
        if (lease.binding.document == document && lease.binding.incarnation == incarnation) out.released.push_back(id);
    for (auto const &id : out.released) _impl->active.erase(id);
    return out;
}
}
