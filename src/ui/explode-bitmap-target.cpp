// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/explode-bitmap-target.h"
#include <algorithm>
#include <cmath>
#include <memory>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include "bitmap-adjustment-chemistry.h"
#include "desktop.h"
#include "inkgc/gc-core.h"
#include "document.h"
#include "object/sp-clippath.h"
#include "object/sp-filter.h"
#include "object/sp-image.h"
#include "object/sp-mask.h"
#include "object/sp-root.h"
#include "object/sp-pattern.h"
#include "object/sp-use.h"
#include "selection.h"
#include "ui/explode-bitmap-context.h"
#include "style.h"
#include "ui/tools/text-tool.h"
#include "util/bitmap-input-header.h"
#include "util/operation-targets.h"
#include "xml/attribute-record.h"
#include "xml/node.h"
#include "xml/node-observer.h"
namespace Inkscape::Bitmap {
bool compatibleExplodeBitmapTool(UI::Tools::ToolBase const *tool) {
    return !dynamic_cast<UI::Tools::TextTool const *>(tool);
}
namespace {
using XML::Node;
std::thread::id const mainThread = std::this_thread::get_id();
std::uintptr_t identity(void const *p) { return reinterpret_cast<std::uintptr_t>(p); }
std::string id(SPObject *o) { return o && o->getId() ? o->getId() : ""; }
std::string_view attr(Node const *n, char const *key)
{ auto v = n->attribute(key); return v ? v : ""; }

// Tracker is main-thread state, deliberately separate from the plain-data snapshot.
// Conservative subtree invalidation covers CSS, defs, href bytes, ancestors and slots.
// Selection/tool signals invalidate even a change followed by a revert. No save revision.
struct Tracker : XML::NodeObserver {
    SPDesktop *desktop;
    Selection &selected;
    DocumentPublicationContext *context = nullptr;
    SPDocument &document;
    Node *root;
    std::uint64_t incarnation, generation = 1;
    std::vector<SPItem *> selection;
    void selectionChanged() {
        auto next = selected.items_vector();
        if (next != selection) { selection = std::move(next); bump(); }
    }
    std::vector<sigc::connection> connections, nativeConnections;
    void observeNative() {
        for (auto &c : nativeConnections) c.disconnect();
        nativeConnections.clear();
        auto observe = [&](auto const &self, SPObject *o) -> void {
            nativeConnections.emplace_back(); // allocate tracking storage before attaching the callback
            nativeConnections.back() = o->connectModified([this](SPObject *, unsigned) { bump(); });
            for (auto &child : o->children) self(self, &child);
        };
        observe(observe, document.getRoot());
    }
    Tracker(SPDesktop &d, std::uint64_t epoch) : desktop(&d), selected(*d.getSelection()), document(*d.getDocument()),
        root(document.getReprRoot()), incarnation(epoch) { GC::anchor(root); root->addSubtreeObserver(*this); }
    Tracker(DocumentPublicationContext &c) : desktop(nullptr), selected(*c.getSelection()), context(&c),
        document(*c.getDocument()), root(document.getReprRoot()), incarnation(c.incarnation()) {
        generation = c.targetGeneration(); GC::anchor(root); root->addSubtreeObserver(*this);
        selection = selected.items_vector();
        connections.push_back(selected.connectChanged([this](Selection *) { selectionChanged(); }));
    }
    ~Tracker() override {
        for (auto &c : nativeConnections) c.disconnect();
        for (auto &c : connections) c.disconnect();
        root->removeSubtreeObserver(*this); GC::release(root);
    }
    void bump() { ++generation; }
    void notifyChildAdded(Node &, Node &, Node *) override { bump(); }
    void notifyChildRemoved(Node &, Node &, Node *) override { bump(); }
    void notifyChildOrderChanged(Node &, Node &, Node *, Node *) override { bump(); }
    void notifyContentChanged(Node &, Util::ptr_shared, Util::ptr_shared) override { bump(); }
    void notifyAttributeChanged(Node &, GQuark, Util::ptr_shared, Util::ptr_shared) override { bump(); }
    void notifyElementNameChanged(Node &, GQuark, GQuark) override { bump(); }
};
auto &trackers() {
    // Destroy callbacks erase entries; process-lifetime container avoids shutdown ordering.
    static auto *map = new std::unordered_map<std::uintptr_t, std::unique_ptr<Tracker>>;
    return *map;
}
Tracker &tracker(SPDesktop &desktop) {
    auto key = identity(&desktop);
    auto &entry = trackers()[key];
    if (entry && (&entry->document != desktop.getDocument() || entry->root != desktop.getDocument()->getReprRoot())) entry.reset();
    if (!entry) {
        static std::uint64_t epoch = 0;
        entry = std::make_unique<Tracker>(desktop, ++epoch);
        auto *t = entry.get();
        t->selection = desktop.getSelection()->items_vector();
        t->connections.push_back(desktop.connectDestroy([key](SPDesktop *) { trackers().erase(key); }));
        t->connections.push_back(t->document.connectDestroy([key] { trackers().erase(key); }));
        t->connections.push_back(desktop.connectDocumentReplaced([t](SPDesktop *, SPDocument *) { t->bump(); }));
        t->connections.push_back(desktop.getSelection()->connectChanged([t](Selection *) { t->selectionChanged(); }));
        t->connections.push_back(desktop.connectEventContextChanged([t](SPDesktop *, UI::Tools::ToolBase *tool) { if (!compatibleExplodeBitmapTool(tool)) t->bump(); }));
    }
    return *entry;
}
void refuse(TargetSnapshot &s, SPObject *o, Refusal r, char const *message) {
    s.refusals.push_back({r, identity(o), id(o), message});
}
void walk(Node *node, auto const &visit) {
    visit(node);
    for (auto child = node->firstChild(); child; child = child->next()) walk(child, visit);
}
// Admit only known graph attributes, including the enclosing filter's style.
bool attributesOnly(Node const *n, std::initializer_list<std::string_view> allowed) {
    for (auto const &a : n->attributeList()) {
        auto key = std::string_view(g_quark_to_string(a.key));
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end()) return false;
        if (key == "style") {
            std::string style;
            for (char c : attr(n, "style")) if (!g_ascii_isspace(c)) style += c;
            if (style != "color-interpolation-filters:sRGB" && style != "color-interpolation-filters:sRGB;") return false;
        }
    }
    return true;
}
bool canonicalTone(SPItem *item) {
    auto settings = BitmapAdjustments::query_tone(item);
    auto filter = item->style->getFilter();
    if (!settings || !filter) return false;
    auto repr = filter->getRepr();
    if (!attributesOnly(repr, {"id", "inkscape:label", "inkscape:collect", "style", "color-interpolation-filters",
            "inkscape:auto-region", "filterUnits", "primitiveUnits", "x", "y", "width", "height"}) ||
        !filter->auto_region || filter->filterUnits != SP_FILTER_UNITS_OBJECTBOUNDINGBOX ||
        filter->primitiveUnits != SP_FILTER_UNITS_USERSPACEONUSE || filter->filterRes.getNumber() >= 0)
        return false;
    for (auto const &[key, expected] : {std::pair{"filterUnits", "objectBoundingBox"},
            std::pair{"primitiveUnits", "userSpaceOnUse"}, std::pair{"inkscape:auto-region", "true"},
            std::pair{"color-interpolation-filters", "sRGB"}})
        if (repr->attribute(key) && attr(repr, key) != expected) return false;
    for (auto key : {"x", "y", "width", "height"}) {
        SVGLength parsed;
        if (repr->attribute(key) && !parsed.read(repr->attribute(key))) return false;
    }
    // In bounding-box units the canonical LUT must cover the full [0,1] image.
    // Missing attributes use SVG defaults (-10%, -10%, 120%, 120%).
    auto extent = [](SVGLength const &v, double fallback) {
        if (!v._set) return fallback;
        return v.unit == SVGLength::NONE || v.unit == SVGLength::PERCENT ? v.value : NAN;
    };
    double x = extent(filter->x, -0.1), y = extent(filter->y, -0.1);
    double w = extent(filter->width, 1.2), h = extent(filter->height, 1.2);
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(w) || !std::isfinite(h) ||
        x > 0 || y > 0 || w <= 0 || h <= 0 || x + w < 1 || y + h < 1) return false;
    auto primitive = repr->firstChild();
    if (!primitive || primitive->next() || std::string_view(primitive->name()) != "svg:feComponentTransfer" ||
        attr(primitive, BitmapAdjustments::TONE_MARKER_ATTRIBUTE) != BitmapAdjustments::TONE_MARKER_VALUE ||
        (!attr(primitive, "in").empty() && attr(primitive, "in") != "SourceGraphic")) return false;
    if (!attributesOnly(primitive, {"id", "style", "color-interpolation-filters", "in", "result",
            "inkscape:bitmap-adjustment", "inkscape:brightness", "inkscape:contrast", "inkscape:intensity",
            "inkscape:highlights", "inkscape:shadows", "inkscape:midtones"})) return false;
    // A result cannot shadow reserved filter inputs, even in a one-primitive graph.
    for (auto reserved : {"SourceGraphic", "SourceAlpha", "BackgroundImage", "BackgroundAlpha", "FillPaint", "StrokePaint"})
        if (attr(primitive, "result") == reserved) return false;
    for (auto name : {"x", "y", "width", "height", "href", "xlink:href"})
        if (!attr(primitive, name).empty()) return false;
    auto primitiveObject = item->document->getObjectByRepr(primitive);
    if (!primitiveObject || !primitiveObject->style || primitiveObject->style->color_interpolation_filters.get_value() != "sRGB")
        return false;
    // Settings cannot be silently clamped; LUT and alpha must match canonical tone math.
    for (unsigned i = 0; i < Filters::BITMAP_TONE_PROPERTY_COUNT; ++i) {
        static char const *names[] = {"inkscape:brightness", "inkscape:contrast", "inkscape:intensity",
            "inkscape:highlights", "inkscape:shadows", "inkscape:midtones"};
        char *end = nullptr;
        auto raw = primitive->attribute(names[i]);
        if (!raw) return false;
        auto value = g_ascii_strtod(raw, &end);
        if (!std::isfinite(value) || *end || value != Filters::get_bitmap_tone_property(*settings, static_cast<Filters::BitmapToneProperty>(i)))
            return false;
    }
    auto table = Filters::build_bitmap_tone_table(*settings);
    auto function = primitive->firstChild();
    for (auto name : {"svg:feFuncR", "svg:feFuncG", "svg:feFuncB", "svg:feFuncA"}) {
        if (!function || std::string_view(function->name()) != name ||
            !attributesOnly(function, {"id", "type", "tableValues"})) return false;
        if (std::string_view(name) == "svg:feFuncA") {
            if (attr(function, "type") != "identity") return false;
        } else {
            if (attr(function, "type") != "table") return false;
            auto text = function->attribute("tableValues");
            if (!text) return false;
            for (auto expected : table) {
                while (g_ascii_isspace(*text) || *text == ',') ++text;
                char *end = nullptr;
                auto value = g_ascii_strtod(text, &end);
                if (end == text || !std::isfinite(value) || std::abs(value - expected) > 1e-8) return false;
                text = end;
            }
            while (g_ascii_isspace(*text)) ++text;
            if (*text) return false;
        }
        function = function->next();
    }
    return !function;
}
bool safeClip(SPItem *item) {
    auto clip = item->getClipObject();
    if (!clip) return false;
    bool safe = true;
    walk(clip->getRepr(), [&](Node *n) {
        auto name = std::string_view(n->name() ? n->name() : "");
        if (name != "svg:clipPath" && name != "svg:g" && name != "svg:path" && name != "svg:rect" &&
            name != "svg:circle" && name != "svg:ellipse" && name != "svg:polygon" && name != "svg:polyline") safe = false;
        auto o = item->document->getObjectByRepr(n);
        if (auto member = cast<SPItem>(o); member && (member->isHidden() || member->isLocked() ||
            member->isFiltered() || member->getMaskObject() || member->getClipObject())) safe = false;
        if (n->attribute("href") || n->attribute("xlink:href")) safe = false;
    });
    return safe;
}
void intake(TargetSnapshot &s, SPImage *image) {
    auto n = image->getRepr();
    auto href = n->attribute("href");
    if (!href) href = n->attribute("xlink:href");
    if (!href || !*href) { refuse(s, image, Refusal::MissingSource, "Bitmap source is missing."); return; }
    auto uri = inspectUri(href, HeaderLimits{});
    if (uri.ok() && uri.value.kind == UriKind::Linked) {
        refuse(s, image, Refusal::LinkedSource, "Embed linked images before using Explode Bitmap."); return;
    }
    if (!uri.ok()) { refuse(s, image, Refusal::InvalidIntake, uri.outcome.diagnostic); return; }
    // Header parsing is bounded, before controls; pixel decoding belongs to EB2-decode.
    // The ledger ceiling matches the existing encoded bound; only the actual header
    // prefix is reserved, including permitted profile/text metadata beyond 1 MiB.
    Budget budget(Budget::FixedLimitForTest{}, HeaderLimits{}.maxEncodedBytes);
    auto header = inspectHref(href, HeaderLimits{}, budget);
    if (!header.ok()) refuse(s, image, Refusal::InvalidIntake, header.outcome.diagnostic);
    else if (!BitmapAdjustments::usable_bitmap(image)) refuse(s, image, Refusal::MissingSource, "Bitmap pixels are unavailable.");
}
void context(TargetSnapshot &s, SPObject *o, bool ancestor, std::unordered_set<SPObject *> &seen, bool replaced = true) {
    if (!seen.insert(o).second) return;
    auto item = cast<SPItem>(o);
    if (!item) return;
    TargetContext c;
    c.identity = identity(o); c.parent = identity(o->parent); c.id = id(o);
    auto affine = item->i2doc_affine();
    for (unsigned i = 0; i < 6; ++i) c.itemToDocument[i] = affine[i];
    if (affine.isSingular() || std::any_of(c.itemToDocument.begin(), c.itemToDocument.end(), [](double v) { return !std::isfinite(v); }))
        refuse(s, o, Refusal::InvalidTransform, "Target transform is singular or non-finite.");
    c.hidden = item->isHidden() || item->style->visibility.computed != SP_CSS_VISIBILITY_VISIBLE; c.locked = item->isLocked(); c.clone = is<SPUse>(item);
    c.referenced = o->hrefcount || o->cloned;
    c.clip = item->getClipObject(); c.mask = item->getMaskObject(); c.filter = item->isFiltered() || (item->style->filter.set && item->style->filter.get_value() != "none");
    c.clipResource = identity(item->getClipObject()); c.maskResource = identity(item->getMaskObject());
    c.filterResource = identity(item->style->getFilter());
    c.opacity = item->style->opacity.as_double();
    if (s.intent == Intent::BitmapCopy) {
        // Non-destructive Copy uses the native composite renderer; no source or
        // resource is removed. Protection and decoded-source admission still apply.
        if (c.hidden) refuse(s, o, Refusal::Hidden, "Hidden targets and ancestors are protected.");
        if (c.locked) refuse(s, o, Refusal::Locked, "Locked targets and ancestors are protected.");
        if (auto image = cast<SPImage>(item); image && !BitmapAdjustments::usable_bitmap(image))
            refuse(s, o, Refusal::MissingSource, "Bitmap pixels are unavailable.");
        s.contexts.push_back(std::move(c)); return;
    }
    // Pattern/marker rendering has a separate resource graph (possibly linked raster,
    // clones or recursive dependencies). Do not admit an unqualified graph as vectors.
    if (is<SPPattern>(item->style->getFillPaintServer()) || is<SPPattern>(item->style->getStrokePaintServer()))
        refuse(s, o, Refusal::ResourceDependency, "Pattern paint dependencies require qualified conversion preflight.");
    for (auto const &a : o->getRepr()->attributeList()) {
        auto key = std::string_view(g_quark_to_string(a.key));
        auto value = std::string_view(static_cast<char const *>(a.value));
        if ((key == "style" || key.starts_with("marker")) && value.find("url(") != std::string_view::npos &&
            (key.starts_with("marker") || value.find("marker") != std::string_view::npos))
            refuse(s, o, Refusal::ResourceDependency, "Marker resource dependencies are unsupported.");
        if (value.find("url(") != std::string_view::npos &&
            ((key == "clip-path" || (key == "style" && value.find("clip-path") != std::string_view::npos)) && !c.clip))
            refuse(s, o, Refusal::UnsupportedClip, "Clip dependency is unresolved.");
        if (value.find("url(") != std::string_view::npos &&
            (key == "mask" || (key == "style" && value.find("mask:") != std::string_view::npos)))
            refuse(s, o, Refusal::OwnMask, "Mask dependency is unsupported.");
        if (key == "style" && value.find('\\') != std::string_view::npos)
            refuse(s, o, Refusal::CssDependency, "Escaped inline CSS dependencies cannot be proven safe.");
    }
    if (c.hidden) refuse(s, o, Refusal::Hidden, "Hidden targets and ancestors are protected.");
    if (c.locked) refuse(s, o, Refusal::Locked, "Locked targets and ancestors are protected.");
    if (c.opacity <= 0) refuse(s, o, Refusal::ZeroOpacity, "Target has zero effective opacity.");
    if (c.referenced && replaced) refuse(s, o, Refusal::CloneReference, "Target or ancestor has live references/clones (T16).");
    bool blend = item->style->mix_blend_mode.value != SP_CSS_BLEND_NORMAL || item->style->isolation.value != SP_CSS_ISOLATION_AUTO;
    if (ancestor && (c.clip || c.mask || c.filter || blend ||
        (c.opacity != 1 && s.mode == TargetMode::CollectiveConversion)))
        refuse(s, o, Refusal::AncestorEffect, "Ancestor compositing cannot be safely baked and replaced.");
    if (!ancestor) {
        if (c.mask) refuse(s, o, Refusal::OwnMask, "Masks are unsupported; preserve the original target.");
        if (blend) refuse(s, o, Refusal::OwnFilter, "Blend/isolation context is unsupported.");
        if (c.clip && !safeClip(item)) refuse(s, o, Refusal::UnsupportedClip, "Only simple geometric own clips are supported.");
        if (c.filter) {
            auto filter = item->style->getFilter();
            bool managed = filter && BitmapAdjustments::find_managed_tone_primitive(filter);
            if (!managed) refuse(s, o, Refusal::OwnFilter, "Only canonical managed tone filters are supported.");
            else if (!canonicalTone(item)) refuse(s, o, Refusal::MalformedTone, "Managed tone graph does not match canonical tone math.");
            else c.tone = *BitmapAdjustments::query_tone(item);
        }
        if (auto image = cast<SPImage>(item)) {
            intake(s, image);
            for (unsigned i = 0; i < 6; ++i) c.pixelToItem[i] = image->c2p[i];
            if (image->aspect_align != SP_ASPECT_NONE && image->aspect_clip == SP_ASPECT_SLICE && image->viewBox_set) {
                auto source = image->viewBox * image->c2p;
                auto const &viewport = image->clipbox;
                double tolerance = 1e-12 * std::max({1.0, source.width(), source.height()});
                if (source.left() < viewport.left() - tolerance || source.top() < viewport.top() - tolerance ||
                    source.right() > viewport.right() + tolerance || source.bottom() > viewport.bottom() + tolerance)
                    refuse(s, o, Refusal::CroppingSlice, "preserveAspectRatio slice crops source pixels; use a noncropping image viewport.");
            }
            c.viewport = {image->clipbox.left(), image->clipbox.top(), image->clipbox.width(), image->clipbox.height()};
            if (image->c2p.isSingular() || !image->clipbox.isFinite() || image->clipbox.width() <= 0 || image->clipbox.height() <= 0 ||
                std::any_of(c.pixelToItem.begin(), c.pixelToItem.end(), [](double v) { return !std::isfinite(v); }))
                refuse(s, o, Refusal::InvalidTransform, "Bitmap mapping or viewport is invalid.");
        }
    }
    s.contexts.push_back(c);
}
// XML graph checks complement hrefcount, which misses CSS, attributes and unresolved urls.
void references(TargetSnapshot &s, SPDocument &doc, std::unordered_set<SPObject *> const &protectedObjects) {
    std::unordered_set<std::string> ids;
    for (auto o : protectedObjects) if (!id(o).empty()) ids.insert(id(o));
    walk(doc.getReprRoot(), [&](Node *n) {
        auto o = doc.getObjectByRepr(n);
        if (std::string_view(n->name() ? n->name() : "") == "svg:style")
            refuse(s, o, Refusal::CssDependency, "Stylesheet dependencies on replacement ids/classes require explicit conversion (T16).");
        for (auto const &a : n->attributeList()) {
            auto key = std::string_view(g_quark_to_string(a.key));
            auto raw = static_cast<char const *>(a.value);
            if ((key == "href" || key == "xlink:href") && std::string_view(raw).starts_with("data:")) continue;
            auto value = std::string_view(raw);
            if (key == "aria-labelledby" || key == "aria-describedby" || key == "aria-controls" ||
                key == "aria-owns" || key == "aria-flowto" || key == "aria-activedescendant" ||
                key == "aria-details" || key == "aria-errormessage" || key == "headers") {
                for (std::size_t begin = 0, end; begin < value.size(); begin = end) {
                    while (begin < value.size() && g_ascii_isspace(value[begin])) ++begin;
                    end = begin;
                    while (end < value.size() && !g_ascii_isspace(value[end])) ++end;
                    if (ids.count(std::string(value.substr(begin, end - begin))))
                        refuse(s, o, Refusal::IdDependency, "An id-list dependency would be changed (T16).");
                }
                continue; // IDREF(S) values are tokens, not URL fragments.
            }
            auto decoded = std::unique_ptr<char, decltype(&g_free)>(
                std::string_view(raw).find('%') == std::string_view::npos ? nullptr : g_uri_unescape_string(raw, nullptr), g_free);
            value = decoded ? decoded.get() : raw;
            for (auto const &target : ids) {
                auto fragment = "#" + target;
                bool found = false;
                for (auto pos = value.find(fragment); pos != std::string_view::npos; pos = value.find(fragment, pos + 1)) {
                    auto end = pos + fragment.size();
                    if (end == value.size() || !(g_ascii_isalnum(value[end]) || value[end] == '_' || value[end] == '-' || value[end] == '.' || value[end] == ':')) found = true;
                }
                if (!found) continue;
                if (key == "href" || key == "xlink:href")
                    refuse(s, o, Refusal::HrefReference, "An href/use/resource points to the target or its context (T16).");
                else if (value.find("url(") != std::string_view::npos)
                    refuse(s, o, Refusal::UrlReference, "A pattern, clip, mask or effect url depends on the target (T16).");
                else refuse(s, o, Refusal::IdDependency, "An attribute depends on a replacement/context id (T16).");
            }
        }
    });
}
} // namespace
// Both entry points share all target policy and native observation.
static Result<TargetSnapshot> resolveOwner(SPDocument *doc, Selection &selection,
    Tracker &t, std::uintptr_t owner, Intent intent, bool toolCompatible) try {
    Result<TargetSnapshot> result;
    auto &s = result.value; s.intent = intent;
    if (std::this_thread::get_id() != mainThread) {
        refuse(s, nullptr, Refusal::MainThreadOnly, "Resolve Explode Bitmap on the main thread.");
        result.outcome = {Status::unavailable, s.refusals.front().diagnostic}; return result;
    }
    if (!doc) { result.outcome = {Status::unavailable, "No document."}; return result; }
    t.observeNative();
    s.document = identity(doc); s.documentSerial = doc->serial(); s.desktop = owner;
    s.incarnation = t.incarnation; s.generation = t.generation;
    auto selected = selection.items_vector();
    for (auto item : selected) s.selection.push_back(identity(item));
    if (selected.empty()) refuse(s, nullptr, Refusal::EmptySelection, "Select one bitmap or a conversion unit.");
    if (!toolCompatible)
        refuse(s, nullptr, Refusal::TextTool, "Leave text editing before using Explode Bitmap.");
    // Do not suppress clone instances through source coverage: conversion renders appearance.
    auto normalized = Util::resolve_composite_targets<SPItem>(selected,
        [](SPItem *) { return Util::TargetAvailability::Eligible; },
        [](SPItem *i) { return cast<SPItem>(i->parent); }, [](SPItem *) -> SPItem * { return nullptr; });
    s.covered = normalized.covered;
    auto &roots = normalized.items;
    bool direct = roots.size() == 1 && is<SPImage>(roots.front());
    s.mode = direct ? TargetMode::SingleBitmap : TargetMode::CollectiveConversion;
    if (direct) s.bitmap = identity(roots.front());
    else s.conversionReasons.emplace_back("Convert the whole selection into ONE bitmap first. Only Undo restores its editable objects.");
    std::unordered_set<SPObject *> seen, protectedObjects;
    for (auto root : roots) {
        s.roots.push_back(identity(root));
        auto subtree = [&](auto const &self, SPObject *o) -> void {
            protectedObjects.insert(o); context(s, o, false, seen);
            // SPUse children are generated clone shadow objects, never editable roots.
            if (!is<SPUse>(o)) for (auto &child : o->children) self(self, &child);
            // A clone may be converted, but its source/raster dependencies must pass intake/protection.
            if (auto use = cast<SPUse>(o)) {
                auto source = use->get_original();
                if (!source) refuse(s, o, Refusal::MissingSource, "Clone source is missing.");
                else {
                    auto dependencies = [&](auto const &self, SPObject *dep) -> void {
                        if (seen.count(dep)) return;
                        context(s, dep, false, seen, false);
                        if (!is<SPUse>(dep)) for (auto &child : dep->children) self(self, &child);
                        if (auto nested = cast<SPUse>(dep)) {
                            if (auto original = nested->get_original()) self(self, original);
                            else refuse(s, dep, Refusal::MissingSource, "Nested clone source is missing.");
                        }
                    };
                    dependencies(dependencies, source);
                }
            }
        };
        subtree(subtree, root);
        for (auto p = root->parent; p; p = p->parent) {
            protectedObjects.insert(p); context(s, p, true, seen);
            auto name = std::string_view(p->getRepr()->name());
            if (name == "svg:defs" || name == "svg:clipPath" || name == "svg:mask" || name == "svg:pattern")
                refuse(s, root, Refusal::ResourceTarget, "Resource/clip/mask members are not editable targets.");
        }
    }
    if (!roots.empty() && intent != Intent::BitmapCopy) {
        auto parent = roots.front()->parent;
        s.destinationParent = identity(parent);
        std::unordered_set<SPObject *> span(roots.begin(), roots.end());
        for (auto root : roots) if (root->parent != parent)
            refuse(s, root, Refusal::CrossParent, "Conversion requires contiguous siblings in one parent/layer.");
        bool started = false, ended = false; std::size_t slot = 0;
        if (parent) for (auto &child : parent->children) {
            if (span.count(&child)) {
                if (!started) s.destinationSlot = slot;
                if (ended) refuse(s, &child, Refusal::Interleaved, "Unselected siblings interleave the conversion unit.");
                started = true;
            } else if (started && is<SPItem>(&child)) ended = true;
            ++slot;
        }
    }
    if (direct) for (auto const &c : s.contexts) {
        s.effectiveOpacity *= c.opacity;
        if (c.identity != s.bitmap) s.retainedAncestorOpacity *= c.opacity;
    }
    if (intent != Intent::BitmapCopy) references(s, *doc, protectedObjects);
    s.supportability = !s.refusals.empty() ? Supportability::Refused : direct ? Supportability::Supported : Supportability::ConversionRequired;
    result.outcome = {s.refusals.empty() ? Status::unchanged : Status::incompatible,
        s.refusals.empty() ? (direct ? "Single embedded bitmap." : "Explicit collective conversion required.") : s.refusals.front().diagnostic};
    return result;
}
catch (std::bad_alloc const &) { return {{Status::failed, "Target preflight allocation failed."}, {}, 0}; }
catch (...) { return {{Status::failed, "Target preflight failed."}, {}, 0}; }
Result<TargetSnapshot> resolve(SPDesktop &desktop, Intent intent) try {
    if (std::this_thread::get_id() != mainThread) {
        Result<TargetSnapshot> result; result.value.intent = intent;
        refuse(result.value, nullptr, Refusal::MainThreadOnly, "Resolve Explode Bitmap on the main thread.");
        result.outcome = {Status::unavailable, result.value.refusals.front().diagnostic}; return result;
    }
    if (!desktop.getDocument()) return {{Status::unavailable, "No document."}, {}};
    return resolveOwner(desktop.getDocument(), *desktop.getSelection(), tracker(desktop),
                        identity(&desktop), intent, compatibleExplodeBitmapTool(desktop.getTool()));
}
catch (...) { return {{Status::failed, "Target owner observation failed."}, {}}; }
Result<TargetSnapshot> resolve(DocumentPublicationContext &context, Intent intent) try {
    if (!context.ownerThread() || !context.getDocument() || context.canceled())
        return {{Status::unavailable, "No owner-thread document."}, {}};
    if (auto desktop = context.desktopView()) return resolve(*desktop, intent);
    auto &entry = trackers()[context.identity()];
    if (!entry) entry = std::make_unique<Tracker>(context);
    if (entry->generation != context.targetGeneration())
        return {{Status::unavailable, "Context target generation has retired."}, {}};
    return resolveOwner(context.getDocument(), *context.getSelection(), *entry,
                        context.identity(), intent, true);
}
catch (...) { return {{Status::failed, "Target context observation failed."}, {}}; }
void initializePublicationTarget(DocumentPublicationContext &context) {
    if (context.ownerThread() && context.getDocument() && !context.desktopView())
        trackers().try_emplace(context.identity(), std::make_unique<Tracker>(context));
}
void retirePublicationTarget(DocumentPublicationContext &context) {
    if (!context.desktopView()) trackers().erase(context.identity());
}
bool valid(TargetSnapshot const &s, SPDocument &doc) {
    if (std::this_thread::get_id() != mainThread || s.document != identity(&doc) || s.documentSerial != doc.serial()) return false;
    auto found = trackers().find(s.desktop);
    if (found == trackers().end()) return false;
    auto &t = *found->second;
    // Native edits queue updates before modified signals are delivered. Fail closed
    // while pending; after delivery the monotonic generation preserves change/revert.
    if (doc.getRoot()->uflags || doc.getRoot()->mflags) { t.bump(); return false; }
    return &t.document == &doc && t.root == doc.getReprRoot() && (t.desktop ? t.desktop->getDocument() == &doc : t.context && t.context->getDocument() == &doc) &&
        t.incarnation == s.incarnation && t.generation == s.generation;
}
} // namespace Inkscape::Bitmap
