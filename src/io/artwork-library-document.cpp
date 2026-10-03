// SPDX-License-Identifier: GPL-2.0-or-later
#include "artwork-library-document.h"

#include <cmath>
#include <map>
#include <string_view>

#include "document.h"
#include "io/stream/inkscapestream.h"
#include "util/units.h"
#include "xml/attribute-record.h"
#include "xml/repr.h"

namespace Inkscape::IO::ArtworkLibrary {
namespace {

void checkpoint(Cancelled const &cancelled)
{
    if (cancelled && cancelled()) throw UI::SelectionCopyCancelled();
}

class BoundedWriter final : public IO::BasicWriter {
public:
    BoundedWriter(std::size_t limit, Cancelled const &cancelled) : limit(limit), cancelled(cancelled) {}
    void close() override { checkpoint(cancelled); }
    void flush() override { checkpoint(cancelled); }
    void put(char value) override
    {
        if (bytes.size() % 4096 == 0) checkpoint(cancelled);
        if (bytes.size() >= limit) throw std::runtime_error("Serialized artwork byte limit exceeded");
        bytes.push_back(static_cast<unsigned char>(value));
    }
    Bytes bytes;
private:
    std::size_t limit;
    Cancelled const &cancelled;
};

void namespaces(XML::Node *node, std::map<std::string, std::string> &result,
                UI::SelectionCopyLimits const &limits, Cancelled const &cancelled,
                std::size_t &nodes, std::size_t depth)
{
    checkpoint(cancelled);
    if (++nodes > limits.nodes || depth > limits.depth) {
        throw std::runtime_error("Serialized artwork tree limit exceeded");
    }
    auto add = [&](char const *name) {
        if (!name) return;
        std::string_view qname(name);
        auto pos = qname.find(':');
        if (pos == qname.npos) return;
        auto prefix = std::string(qname.substr(0, pos));
        if (prefix == "xml" || prefix == "xmlns") return;
        auto uri = sp_xml_ns_prefix_uri(prefix.c_str());
        if (!uri) throw std::runtime_error("Unknown artwork namespace");
        result.emplace(std::move(prefix), uri);
    };
    if (node->type() == XML::NodeType::ELEMENT_NODE) {
        add(node->name());
        for (auto const &attr : node->attributeList()) add(g_quark_to_string(attr.key));
    }
    for (auto child = node->firstChild(); child; child = child->next()) {
        namespaces(child, result, limits, cancelled, nodes, depth + 1);
    }
}

// Inkscape records an export's destination path on the exported object (and
// sometimes the root). It is a local path, not artwork, so it never enters a
// library asset (admission refuses it). The DPI hints are admitted and kept.
void strip_export_path(XML::Node *node)
{
    if (node->type() == XML::NodeType::ELEMENT_NODE) {
        node->removeAttribute("inkscape:export-filename");
    }
    for (auto child = node->firstChild(); child; child = child->next()) {
        strip_export_path(child);
    }
}

} // namespace

StagedSelection stage_selection(ObjectSet &source, UI::SelectionCopyLimits const &limits,
                                 Cancelled cancelled)
{
    auto const budgets = limits; // Pin options before any caller callback.
    auto detached = UI::copy_selection_detached(source, budgets, cancelled);
    checkpoint(cancelled);
    auto width = detached.document->getWidth().value("mm");
    auto height = detached.document->getHeight().value("mm");
    if (!std::isfinite(width) || !std::isfinite(height) || width <= 0 || height <= 0 ||
        width > 1e9 || height > 1e9) {
        throw std::runtime_error("Invalid staged physical dimensions");
    }

    auto root = detached.document->getReprRoot();
    strip_export_path(root);
    std::map<std::string, std::string> ns;
    std::size_t nodes = 0;
    namespaces(root, ns, budgets, cancelled, nodes, 1);
    for (auto const &[prefix, uri] : ns) {
        root->setAttribute(("xmlns:" + prefix).c_str(), uri.c_str());
    }
    // The low-level native XML writer does escaping but not preference-driven
    // cleanup/sorting. Do not call save_buf (which can discard editable metadata
    // under the user's export preferences). No temporary file or global setting.
    BoundedWriter writer(budgets.svg_bytes, cancelled);
    writer.writeString("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    sp_repr_write_stream(root, writer, 0, false, GQuark(0), 0, 0);
    writer.close();
    return {std::move(writer.bytes), width, height, std::move(detached.warnings)};
}

} // namespace Inkscape::IO::ArtworkLibrary
