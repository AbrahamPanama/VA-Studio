// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <memory>
#include <string>
#include <vector>
#include "ui/explode-bitmap-target.h"
#include "ui/explode-bitmap-jobs.h"
namespace Inkscape { class Selection; }
namespace Inkscape::Bitmap {
struct Prepared;
struct PreparedContourOnly;
struct PreparedAlpha;
class DependencyLease;
class DependencyRequest;
enum class PublicationRefusal {
    none, wrongThread, invalidIdentity, invalidRoots, staleTarget, staleDependency, staleSession,
    missingRecipe, wrongActivation, canceled, nativePublication
};
struct PublicationResult : Outcome {
    PublicationRefusal refusal = PublicationRefusal::none;
    std::optional<CliBitmapFailure> failure;
    bool rolledBack = false;
    // Values only. Integration may install these in its session after changed settlement.
    std::vector<std::string> selectionAfter;
    PublicationResult(Status s, char const *message, PublicationRefusal r = PublicationRefusal::none)
        : PublicationResult(Outcome{s, message}, r) {}
    PublicationResult(Outcome o = {}, PublicationRefusal r = PublicationRefusal::none)
        : Outcome(o), refusal(r) {
        if (!o.ok()) failure = bitmapFailure(o, CliBitmapStage::Resolve,
            r == PublicationRefusal::invalidRoots ? CliBitmapReason::UnsupportedTarget :
            r == PublicationRefusal::staleTarget || r == PublicationRefusal::staleDependency ||
            r == PublicationRefusal::staleSession ? CliBitmapReason::StaleCapture : CliBitmapReason::InternalError);
    }
};
template <typename T> struct ContextResult {
    PublicationResult outcome;
    T value{};
    bool ok() const noexcept { return outcome.ok(); }
};
// Owner-thread capability, immutable per target generation. Keep the document alive
// and retire/join jobs before destruction. The local Selection is never a desktop
// selection; it supplies native release notifications and exact mutation scripts.
class DocumentPublicationContext {
public:
    static ContextResult<std::unique_ptr<DocumentPublicationContext>> headless(
        SPDocument &, std::vector<std::string> const &explicit_roots,
        std::uint64_t incarnation, std::uint64_t target_generation);
    static ContextResult<std::unique_ptr<DocumentPublicationContext>> headlessBitmapCopy(
        SPDocument &, std::vector<std::string> const &explicit_roots,
        std::uint64_t incarnation, std::uint64_t target_generation);
    static ContextResult<std::unique_ptr<DocumentPublicationContext>> desktop(SPDesktop &);
    ~DocumentPublicationContext();
    DocumentPublicationContext(DocumentPublicationContext const &) = delete;
    DocumentPublicationContext &operator=(DocumentPublicationContext const &) = delete;
    std::uint64_t identity() const noexcept;
    bool ownerThread() const noexcept;
    SPDocument *getDocument() const;
    SPDesktop *desktopView() const;
    Selection *getSelection() const; // native publisher/lease accessor; never live GUI scratch headless
    unsigned drawingKey() const;
    std::uint64_t incarnation() const;
    std::uint64_t targetGeneration() const;
    std::shared_ptr<DependencyRequest> activation() const;
    // Native publisher accessor: allocate result IDs before the rollback boundary
    // closes; moving them to the result after settlement cannot allocate.
    void stageSelectionAfter(std::vector<std::string>);
    std::vector<std::string> takeSelectionAfter();
    bool canceled() const;
    PublicationResult cancel(); // invalidates all prepared dependencies, no recipe/document mutation
private:
    DocumentPublicationContext();
    struct Impl;
    std::unique_ptr<Impl> _impl;
};
Result<TargetSnapshot> resolve(DocumentPublicationContext &, Intent);
void initializePublicationTarget(DocumentPublicationContext &); // cheap lifetime/selection observation
void retirePublicationTarget(DocumentPublicationContext &); // lifetime tracker teardown
PublicationResult publishExplode(DocumentPublicationContext &, Prepared const &, Ticket);
PublicationResult publishContourOnly(DocumentPublicationContext &, PreparedContourOnly const &, Ticket);
PublicationResult publishAlpha(DocumentPublicationContext &, PreparedAlpha const &, Ticket);
PublicationResult publishAlpha(DocumentPublicationContext &, PreparedAlpha const &, Prepared const &request, Ticket);
}
