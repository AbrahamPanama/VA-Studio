// SPDX-License-Identifier: GPL-2.0-or-later
#include "explode-bitmap-context.h"
#include "explode-bitmap-dependencies.h"
#include "desktop.h"
#include "display/drawing.h"
#include "document.h"
#include "object/sp-image.h"
#include "object/sp-root.h"
#include "selection.h"
#include <thread>
#include <algorithm>
#include <sigc++/scoped_connection.h>
namespace Inkscape::Bitmap {
namespace { auto const mainThread = std::this_thread::get_id(); }
struct DocumentPublicationContext::Impl {
    SPDocument *document = nullptr;
    SPDesktop *desktop = nullptr;
    std::uint64_t incarnation = 0, generation = 0;
    unsigned key = 0;
    bool canceled = false;
    std::vector<std::string> selectionAfter;
    std::unique_ptr<Drawing> drawing;
    std::unique_ptr<Selection> selection;
    std::shared_ptr<DependencyRequest> activation = std::make_shared<DependencyRequest>();
    std::unique_ptr<DependencyLease> lease;
    sigc::scoped_connection destroyed, replaced, closed;
    void hide() { if (drawing && document) document->getRoot()->invoke_hide(key); drawing.reset(); }
};
DocumentPublicationContext::DocumentPublicationContext() : _impl(std::make_unique<Impl>()) {}
DocumentPublicationContext::~DocumentPublicationContext() {
    _impl->lease.reset(); retirePublicationTarget(*this); _impl->selection.reset(); _impl->hide();
}
bool DocumentPublicationContext::ownerThread() const noexcept { return std::this_thread::get_id() == mainThread; }
std::uint64_t DocumentPublicationContext::identity() const noexcept {
    return reinterpret_cast<std::uintptr_t>(_impl->desktop ? static_cast<void *>(_impl->desktop) : const_cast<DocumentPublicationContext *>(this));
}
SPDocument *DocumentPublicationContext::getDocument() const { return ownerThread() ? _impl->document : nullptr; }
SPDesktop *DocumentPublicationContext::desktopView() const { return _impl->desktop; }
Selection *DocumentPublicationContext::getSelection() const { return _impl->desktop ? _impl->desktop->getSelection() : _impl->selection.get(); }
unsigned DocumentPublicationContext::drawingKey() const { return _impl->desktop ? _impl->desktop->dkey : _impl->key; }
std::uint64_t DocumentPublicationContext::incarnation() const { return _impl->incarnation; }
std::uint64_t DocumentPublicationContext::targetGeneration() const { return _impl->generation; }
std::shared_ptr<DependencyRequest> DocumentPublicationContext::activation() const { return _impl->activation; }
void DocumentPublicationContext::stageSelectionAfter(std::vector<std::string> ids) { _impl->selectionAfter = std::move(ids); }
std::vector<std::string> DocumentPublicationContext::takeSelectionAfter() { return std::move(_impl->selectionAfter); }
bool DocumentPublicationContext::canceled() const { return _impl->canceled; }
PublicationResult DocumentPublicationContext::cancel() {
    if (!ownerThread()) return {Status::unavailable, "Owner thread required.", PublicationRefusal::wrongThread};
    _impl->canceled = true;
    if (_impl->lease) _impl->lease->invalidate();
    return {Status::canceled, "Publication canceled without mutation.", PublicationRefusal::canceled};
}
ContextResult<std::unique_ptr<DocumentPublicationContext>> DocumentPublicationContext::headless(
    SPDocument &doc, std::vector<std::string> const &roots, std::uint64_t incarnation, std::uint64_t generation) {
    if (std::this_thread::get_id() != mainThread) return {{Status::unavailable, "Owner thread required.", PublicationRefusal::wrongThread}, {}};
    if (!incarnation || !generation)
        return {{Status::unavailable, "Invalid context identity.", PublicationRefusal::invalidIdentity}, {}};
    if (roots.size() != 1 || !is<SPImage>(doc.getObjectById(roots.front())))
        return {{Status::incompatible, "Exactly one direct embedded image is required.", PublicationRefusal::invalidRoots}, {}};
    return headlessBitmapCopy(doc, roots, incarnation, generation);
}
ContextResult<std::unique_ptr<DocumentPublicationContext>> DocumentPublicationContext::headlessBitmapCopy(
    SPDocument &doc, std::vector<std::string> const &roots, std::uint64_t incarnation, std::uint64_t generation) {
    if (std::this_thread::get_id() != mainThread)
        return {{Status::unavailable, "Owner thread required.", PublicationRefusal::wrongThread}, {}};
    if (!incarnation || !generation)
        return {{Status::unavailable, "Invalid context identity.", PublicationRefusal::invalidIdentity}, {}};
    std::vector<SPItem *> items;
    for (auto const &id : roots) {
        auto item = cast<SPItem>(doc.getObjectById(id));
        if (!item || std::find(items.begin(), items.end(), item) != items.end())
            return {{Status::incompatible, "Invalid explicit bitmap-copy roots.", PublicationRefusal::invalidRoots}, {}};
        items.push_back(item);
    }
    if (items.empty()) return {{Status::incompatible, "Bitmap Copy requires explicit roots.", PublicationRefusal::invalidRoots}, {}};
    auto context = std::unique_ptr<DocumentPublicationContext>(new DocumentPublicationContext);
    auto &s = *context->_impl; s.document = &doc; s.incarnation = incarnation; s.generation = generation;
    s.selection = std::make_unique<Selection>(&doc);
    s.selection->setList(items);
    s.drawing = std::make_unique<Drawing>(); s.key = SPItem::display_key_new(1);
    s.drawing->setRoot(doc.getRoot()->invoke_show(*s.drawing, s.key, SP_ITEM_SHOW_DISPLAY));
    doc.ensureUpToDate(); s.drawing->update();
    s.closed = doc.connectDestroy([c = context.get()] {
        c->_impl->lease.reset(); retirePublicationTarget(*c); c->_impl->selection.reset();
        c->_impl->hide(); c->_impl->document = nullptr;
    });
    initializePublicationTarget(*context);
    s.lease = std::make_unique<DependencyLease>(*context);
    return {{Status::unchanged, "Document publication context ready."}, std::move(context)};
}
ContextResult<std::unique_ptr<DocumentPublicationContext>> DocumentPublicationContext::desktop(SPDesktop &desktop) {
    if (std::this_thread::get_id() != mainThread || !desktop.getDocument())
        return {{Status::unavailable, "Owner-thread desktop document required."}, {}};
    auto context = std::unique_ptr<DocumentPublicationContext>(new DocumentPublicationContext);
    auto &s = *context->_impl; s.desktop = &desktop; s.document = desktop.getDocument();
    s.destroyed = desktop.connectDestroy([c = context.get()](auto) { c->_impl->document = nullptr; c->_impl->desktop = nullptr; });
    s.replaced = desktop.connectDocumentReplaced([c = context.get()](auto, auto) { c->_impl->document = nullptr; });
    auto target = resolve(desktop, Intent::Explode);
    s.incarnation = target.value.incarnation; s.generation = target.value.generation;
    s.lease = std::make_unique<DependencyLease>(*context);
    return {{Status::unchanged, "Desktop publication context ready."}, std::move(context)};
}
}
