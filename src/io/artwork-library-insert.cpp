// SPDX-License-Identifier: GPL-2.0-or-later
#include "artwork-library-insert.h"
#include "document.h"
#include "document-undo.h"
#include "object/sp-item-group.h"
#include "object/sp-root.h"
#include "object/sp-text.h"
#include "object/sp-clippath.h"
#include "object/sp-mask.h"
#include "style.h"
#include "xml/attribute-record.h"
#include "xml/subtree-revision.h"
#include "xml/repr.h"
#include "util/units.h"
#include <2geom/transforms.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <thread>
#include <utility>

namespace Inkscape::IO::ArtworkLibrary {
namespace {
using F = InsertionFailure;
[[noreturn]] void fail(F code, char const *message) { throw InsertionError(code, message); }
void require(bool ok, F code, char const *message) { if (!ok) fail(code, message); }
using NodeOwner = std::shared_ptr<XML::Node>;
NodeOwner pin(XML::Node *n)
{
    GC::anchor(n);
    return NodeOwner(n, [](auto *p) { GC::release(p); });
}
void charge(std::size_t &total, std::size_t count, std::size_t cap)
{
    require(total <= cap && count <= cap - total, F::LimitExceeded, "Insertion byte budget");
    total += count;
}
void validate(InsertionLimits const &l)
{
    InsertionLimits hard;
    require(l.nodes && l.nodes <= hard.nodes && l.depth && l.depth <= hard.depth &&
        l.materialized_style_bytes && l.materialized_style_bytes <= hard.materialized_style_bytes &&
        l.target_snapshot_bytes && l.target_snapshot_bytes <= hard.target_snapshot_bytes &&
        std::isfinite(l.coordinate_magnitude) && l.coordinate_magnitude > 0 &&
        l.coordinate_magnitude <= hard.coordinate_magnitude, F::LimitExceeded, "Invalid insertion limits");
}
void affine(Geom::Affine const &a, double cap)
{
    for (unsigned i = 0; i < 6; ++i)
        require(std::isfinite(a[i]) && std::abs(a[i]) <= cap, F::Ineligible, "Nonfinite/extreme target transform");
    auto determinant = a[0] * a[3] - a[1] * a[2];
    require(std::isfinite(determinant) && std::abs(determinant) >= 1e-12,
            F::Ineligible, "Singular target transform");
}
std::string attributes(XML::Node const *n, std::size_t &bytes, std::size_t cap)
{
    std::string out;
    auto field = [&](char const *value) {
        if (!value) { out += "-;"; return; }
        auto length = strnlen(value, cap + 1);
        charge(bytes, length, cap);
        out += std::to_string(length); out += ':'; out.append(value, length);
    };
    field(n->name()); field(n->content());
    for (auto const &a : n->attributeList()) {
        field(g_quark_to_string(a.key)); field(a.value);
    }
    return out;
}
struct Ancestor {
    NodeOwner node;
    std::string attrs;
    Geom::Affine to_document;
};
std::vector<Ancestor> ancestry(SPDocument &doc, SPObject &parent, InsertionLimits const &l)
{
    require(parent.document == &doc, F::Ineligible, "Parent belongs to another document");
    require(parent.getRepr() == doc.getReprRoot() || !std::strcmp(parent.getRepr()->name(), "svg:g"),
            F::Ineligible, "Insertion parent must be the document root or an ordinary group/layer");
    std::vector<Ancestor> chain;
    std::size_t bytes = 0;
    for (auto *p = &parent; p; p = p->parent) {
        require(chain.size() < l.depth, F::LimitExceeded, "Target ancestor depth");
        auto item = cast<SPItem>(p);
        require(item && !item->isLocked() && !item->isHidden(), F::Ineligible, "Hidden/locked/ineligible ancestor");
        require(!item->getClipObject() && !item->getMaskObject() && !item->style->getFilter() &&
            item->style->opacity.get_value() == "1" && item->style->mix_blend_mode.get_value() == "normal",
            F::Ineligible, "Ancestor compositing would change imported appearance");
        auto matrix = item->i2doc_affine(); affine(matrix, l.coordinate_magnitude);
        affine(matrix.inverse(), l.coordinate_magnitude);
        chain.push_back({pin(p->getRepr()), attributes(p->getRepr(), bytes, l.target_snapshot_bytes), matrix});
        if (p == doc.getRoot()) return chain;
    }
    fail(F::Ineligible, "Detached target");
}
std::string number(double n)
{
    char buffer[G_ASCII_DTOSTR_BUF_SIZE];
    return g_ascii_dtostr(buffer, sizeof(buffer), n);
}
void walk(XML::Node *node, InsertionLimits const &limits, std::function<void(XML::Node *)> const &visit)
{
    std::size_t nodes = 0;
    std::function<void(XML::Node *, std::size_t)> descend = [&](auto *n, auto depth) {
        require(++nodes <= limits.nodes && depth <= limits.depth, F::LimitExceeded, "Insertion tree budget");
        visit(n);
        for (auto *child = n->firstChild(); child; child = child->next()) descend(child, depth + 1);
    };
    descend(node, 1);
}
std::string fingerprint(XML::Node *root, InsertionLimits const &limits)
{
    std::string result; std::size_t bytes = 0;
    walk(root, limits, [&](auto *node) {
        auto a = attributes(node, bytes, limits.materialized_style_bytes + 32u * 1024 * 1024);
        result += std::to_string(static_cast<int>(node->type())) + ":" +
                  std::to_string(node->childCount()) + ":" + std::to_string(a.size()) + ":" + a;
    });
    return result;
}
void validate_frame_owners(SPDocument &doc, InsertionLimits const &limits)
{
    // V3 validates metadata spelling, not this ownership association. A frame
    // must bind to a generated SPText in THIS private document and be one of its
    // real native shape-inside dependencies. Never resolve against destination.
    walk(doc.getReprRoot(), limits, [&](auto *node) {
        if (auto id = node->attribute("inkscape:text-frame-owner")) {
            auto owner = cast<SPText>(doc.getObjectById(id));
            require(!std::strcmp(node->name(), "svg:rect") && node->parent() &&
                    !std::strcmp(node->parent()->name(), "svg:defs") && owner && owner->style &&
                    !g_strcmp0(owner->getRepr()->attribute("inkscape:text-frame-generated"), "true"),
                    F::Unsupported, "Generated frame has no private generated-text owner");
            require(std::any_of(owner->style->shape_inside.hrefs.begin(), owner->style->shape_inside.hrefs.end(),
                [&](auto *href) { auto item = href->getObject(); return item && item->getRepr() == node; }),
                F::Unsupported, "Generated frame is not referenced by its declared owner");
        }
        auto text = cast<SPText>(doc.getObjectByRepr(node));
        if (!text || !node->attribute("inkscape:text-frame-generated")) return;
        require(text->style && text->getId() && !text->style->shape_inside.hrefs.empty() &&
                text->style->shape_inside.hrefs.size() <= 20, F::Unsupported, "Generated text needs bounded frame references");
        for (auto *href : text->style->shape_inside.hrefs) {
            auto item = href->getObject();
            require(item && !g_strcmp0(item->getRepr()->attribute("inkscape:text-frame-owner"), text->getId()),
                    F::Unsupported, "Generated text references a differently owned frame");
        }
    });
}
void freeze_style(SPDocument &doc, InsertionLimits const &limits, std::function<void()> const &check)
{
    // Read all original native values before modifying any ancestor's style.
    std::vector<std::pair<NodeOwner, std::string>> prepared;
    std::size_t total = 0;
    walk(doc.getReprRoot(), limits, [&](auto *node) {
        check();
        auto object = doc.getObjectByRepr(node);
        if (!object || !object->style || std::strncmp(node->name(), "svg:", 4)) return;
        auto style = std::string(node->attribute("style") ? node->attribute("style") : "");
        style += ';'; // Preserve properties outside SPStyle, e.g. authored clip/mask.
        for (auto *property : object->style->properties()) {
            auto key = property->name().raw();
            auto value = property->get_value().raw();
            // Preserve native inheritance rather than applying a parent's relative
            // font metric again at every descendant.
            if (object != doc.getRoot() && property->inherits && !property->set) value = "inherit";
            if (key == "d" && !property->set) value = "none"; // Keep authored d attribute authoritative.
            if (value.empty() && (key.starts_with("marker") || key == "shape-inside" ||
                                  key == "shape-subtract")) value = "none";
            if (value.empty()) continue;
            if (value.ends_with("!important")) value.resize(value.size() - 10);
            auto declaration = key + ":" + value + " !important;";
            require(style.size() + declaration.size() <= 512u * 1024, F::LimitExceeded, "One materialized style exceeds 512 KiB");
            style += declaration;
        }
        // Native clip/mask live outside SPStyle::properties(). Materialize
        // both positive references and neutral values; otherwise destination
        // selectors can silently composite the AlwaysGroup wrapper.
        if (auto item = cast<SPItem>(object)) {
            auto clip = item->getClipObject();
            auto mask = item->getMaskObject();
            style += std::string("clip-path:") +
                     (clip ? std::string("url(#") + clip->getId() + ")" : "none") + " !important;";
            style += std::string("mask:") +
                     (mask ? std::string("url(#") + mask->getId() + ")" : "none") + " !important;";
        }
        require(style.size() <= 512u * 1024, F::LimitExceeded, "One materialized style exceeds 512 KiB");
        charge(total, style.size(), limits.materialized_style_bytes);
        prepared.emplace_back(pin(node), std::move(style));
    });
    for (auto const &[node, style] : prepared) {
        check(); node->setAttribute("style", style);
        require(node->attribute("style") && style == node->attribute("style"), F::Unsupported,
                "Native editing preferences changed the appearance shield");
    }
    doc.ensureUpToDate(); check();
}
}
struct InsertionTarget::State {
    unsigned long serial;
    std::thread::id thread;
    std::vector<Ancestor> chain;
    bool matches(SPDocument &doc, InsertionLimits const &limits) const
    {
        if (doc.serial() != serial || std::this_thread::get_id() != thread || chain.empty()) return false;
        auto *parent = doc.getObjectByRepr(chain.front().node.get());
        if (!parent) return false;
        try {
            auto now = ancestry(doc, *parent, limits);
            if (now.size() != chain.size()) return false;
            for (std::size_t i = 0; i < now.size(); ++i) {
                if (now[i].node != chain[i].node || now[i].attrs != chain[i].attrs) return false;
                for (unsigned j = 0; j < 6; ++j) if (now[i].to_document[j] != chain[i].to_document[j]) return false;
            }
            return true;
        } catch (InsertionError const &) { return false; }
    }
};
InsertionTarget capture_insertion_target(SPDocument &doc, SPObject &parent, InsertionLimits limits)
{
    validate(limits);
    require(!DocumentUndo::interactionCloseRequested(&doc), F::Ineligible, "Document is closing");
    return InsertionTarget(std::make_shared<InsertionTarget::State const>(
        InsertionTarget::State{doc.serial(), std::this_thread::get_id(), ancestry(doc, parent, limits)}));
}
struct InsertionAccess {
    static InsertedArtwork run(SPDocument &doc, InsertionTarget target, ValidatedSvg asset,
                               Geom::Point origin, InsertionLimits limits, Cancelled cancelled)
    {
        validate(limits);
        require(asset.policy_version() == 3, F::Unsupported, "Insertion requires the frozen v3 preflight policy");
        auto state = target._state;
        require(state->matches(doc, limits), F::StaleTarget, "Target identity/context changed");
        for (unsigned i = 0; i < 2; ++i)
            require(std::isfinite(origin[i]) && std::abs(origin[i]) <= limits.coordinate_magnitude,
                    F::Ineligible, "Invalid document-coordinate placement");
        auto check = [&] {
            if (cancelled && cancelled()) fail(F::Cancelled, "Library insertion cancelled");
            require(!DocumentUndo::interactionCloseRequested(&doc), F::Interrupted, "Document close requested");
            require(state->matches(doc, limits), F::StaleTarget, "Target identity/context changed");
        };
        // Private owners obey the public synchronous ownership contract.
        auto preparation_lease = DocumentUndo::holdInteractionOperation(&doc);
        require(preparation_lease != nullptr, F::Ineligible, "Cannot retain destination operation");
        std::unique_ptr<SPDocument> source;
        NodeOwner payload;
        auto immutable_bytes = asset.svg_bytes(); // Owner stays live over native parse.
        {
            XML::SubtreeRevision revision(*doc.getReprRoot());
            check();
            auto original = SPDocument::createNewDocFromMem(
                std::span<char const>(immutable_bytes->data(), immutable_bytes->size()));
            require(original != nullptr, F::Unsupported, "Native SVG parser refused validated bytes");
            original->ensureUpToDate(); check();
            validate_frame_owners(*original, limits);
            auto bounds = original->getRoot()->documentVisualBounds();
            require(bounds && bounds->width() > 0 && bounds->height() > 0 &&
                std::isfinite(bounds->width()) && std::isfinite(bounds->height()), F::Unsupported, "Asset has no finite visible artwork");
            // Native width values avoid introducing an extra px -> mm -> px ULP.
            auto width = original->getWidth().value("px"), height = original->getHeight().value("px");
            require(std::isfinite(width) && std::isfinite(height) && width > 0 && height > 0 &&
                    width <= limits.coordinate_magnitude && height <= limits.coordinate_magnitude,
                    F::LimitExceeded, "Native viewport exceeds insertion bounds");
            auto shell = "<svg xmlns='http://www.w3.org/2000/svg' width='" + number(width) +
                "px' height='" + number(height) + "px' viewBox='0 0 " + number(width) + " " + number(height) + "'/>";
            source = SPDocument::createNewDocFromMem(std::span<char const>(shell.data(), shell.size()));
            require(source != nullptr, F::Unsupported, "Cannot create private import envelope");
            auto root = source->getReprRoot();
            std::string envelope_id = "vacards-import-envelope";
            while (original->getObjectById(envelope_id.c_str())) envelope_id += "_";
            root->setAttribute("id", envelope_id);
            auto *copy = original->getReprRoot()->duplicate(source->getReprDoc());
            payload = NodeOwner(copy, [](auto *n) { GC::release(n); }); // Factory anchor.
            copy->setAttribute("x", "0"); copy->setAttribute("y", "0");
            copy->setAttribute("width", number(width) + "px");
            copy->setAttribute("height", number(height) + "px");
            root->appendChild(copy);
            source->ensureUpToDate(); check();
            freeze_style(*source, limits, check);
            validate_frame_owners(*source, limits);
            require(!revision.changed(), F::StaleTarget, "Destination changed during private preparation");
        }
        check();
        // No callback/event dispatch between releasing this lease and acquiring
        // the atomic interaction's settlement lease. begin refuses pending close.
        preparation_lease.reset();
        auto transaction = DocumentUndo::beginAtomicInteraction(&doc);
        require(transaction.has_value(), F::Busy, "Destination has pending XML/history work or another interaction");
        try {
            check();
            std::vector<XML::Node *> inserted;
            doc.import(*source, state->chain.front().node.get(), nullptr, Geom::Translate(origin), &inserted,
                       SPDocument::ImportRoot::AlwaysGroup, SPDocument::ImportLayersMode::None,
                       SPDocument::ImportResources::Independent);
            // The native result vector is ordinary (unscanned) storage. Pin the
            // returned XML before any callback can collect a detached result.
            require(inserted.size() == 1, F::Interrupted, "Import did not produce one owned group");
            auto group = pin(inserted.front());
            // Native id-clash's REF_PLAIN_ID adapter updates ownership before
            // the payload is attached. Validate the remapped private association.
            validate_frame_owners(*source, limits);
            check();
            require(!transaction->interrupted(), F::Interrupted, "Nested history operation interrupted insertion");
            auto *object = doc.getObjectByRepr(group.get());
            require(object && object->parent && object->parent->getRepr() == state->chain.front().node.get() &&
                    object->getId(), F::StaleTarget, "Imported group was detached");
            auto *inner = group->firstChild();
            require(inner && !inner->next(), F::Unsupported, "Unexpected import envelope structure");
            // Independent import may rename IDs in source, so compare AFTER import.
            // Geometry, resources, root metadata, and inline shields must all agree.
            require(fingerprint(inner, limits) == fingerprint(payload.get(), limits), F::Unsupported,
                    "Native insertion altered the prepared payload");
            auto neutral_wrapper = [&] {
                auto item = cast<SPItem>(doc.getObjectByRepr(group.get()));
                return item && !item->getClipObject() && !item->getMaskObject() &&
                    !item->isHidden() && !item->style->getFilter() &&
                    item->style->opacity.get_value() == "1" &&
                    item->style->mix_blend_mode.get_value() == "normal";
            };
            require(neutral_wrapper(), F::Unsupported, "Destination compositing changed the import wrapper");
            auto published = fingerprint(group.get(), limits);
            InsertedArtwork result{doc.serial(), asset.asset_id(), asset.sha256(), {object->getId()},
                                   asset.width_mm(), asset.height_mm(), origin, asset.warnings()};
            result.warnings.emplace_back("Appearance is pinned with inline important styles; remove those overrides explicitly to restyle from destination CSS.");
            auto ready = [&] {
                check();
                auto *current = doc.getObjectByRepr(group.get());
                return !transaction->interrupted() && neutral_wrapper() && current && current->parent &&
                    current->parent->getRepr() == state->chain.front().node.get() &&
                    fingerprint(group.get(), limits) == published;
            };
            require(transaction->commitAtomically(Util::Internal::ContextString("Insert library artwork"),
                                                  "dialog-symbols", ready), F::Interrupted,
                    "Insertion changed or closed before history publication");
            return result;
        } catch (...) {
            transaction->rollback(); // Settlement was admitted before a close request.
            throw;
        }
    }
};
InsertedArtwork insert_artwork(SPDocument &doc, InsertionTarget target, ValidatedSvg asset,
                               Geom::Point origin, InsertionLimits limits, Cancelled cancelled)
{
    return InsertionAccess::run(doc, std::move(target), std::move(asset), origin, limits, std::move(cancelled));
}
}
