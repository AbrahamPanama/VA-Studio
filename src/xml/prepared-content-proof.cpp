// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * @brief Read-only exact proof of prepared XML content
 *
 * See xml/prepared-content-proof.h for the contract.
 */

#include "xml/prepared-content-proof.h"

#include <cstring>
#include <unordered_map>
#include <utility>

#include "gc-anchored.h"
#include "xml/attribute-record.h"
#include "xml/document.h"
#include "xml/text-node.h"

namespace Inkscape {
namespace XML {

namespace {

/// Exact C-string comparison with nullness; pointer identity is a fast path.
bool exactStringEqual(char const *a, char const *b)
{
    if (a == b) {
        return true;
    }
    if (!a || !b) {
        return false;
    }
    return std::strcmp(a, b) == 0;
}

/// Exact single-node fields: type, name, content, child count, attributes.
bool nodeFieldsEqual(Node const &a, Node const &b)
{
    if (a.type() != b.type()) {
        return false;
    }
    if (a.type() == NodeType::TEXT_NODE) {
        auto const *a_text = dynamic_cast<TextNode const *>(&a);
        auto const *b_text = dynamic_cast<TextNode const *>(&b);
        if (!a_text || !b_text || a_text->is_CData() != b_text->is_CData()) {
            return false;
        }
    }
    if (!exactStringEqual(a.name(), b.name())) {
        return false;
    }
    if (!exactStringEqual(a.content(), b.content())) {
        return false;
    }
    if (a.childCount() != b.childCount()) {
        return false;
    }

    const AttributeVector &av = a.attributeList();
    const AttributeVector &bv = b.attributeList();
    if (av.size() != bv.size()) {
        return false;
    }

    // Order-independent exact key/value mapping; a small local index only,
    // never an XML copy and never a sort of live values.
    std::unordered_map<GQuark, char const *> bvalues;
    bvalues.reserve(bv.size());
    for (auto const &record : bv) {
        bvalues.emplace(record.key, record.value.pointer());
    }

    if (bvalues.size() != bv.size()) {
        return false; // Native attributes are a map; duplicate keys are invalid.
    }

    for (auto const &ra : av) {
        auto it = bvalues.find(ra.key);
        if (it == bvalues.end()) {
            return false;
        }
        if (!exactStringEqual(ra.value.pointer(), it->second)) {
            return false;
        }
        bvalues.erase(it); // A repeated key in a would otherwise match twice.
    }
    return bvalues.empty();
}

/// Exact ordered preorder comparison, iterative and bounded at the roots.
bool exactTreeEqual(Node const &a_root, Node const &b_root)
{
    Node const *a = &a_root;
    Node const *b = &b_root;

    for (;;) {
        if (!nodeFieldsEqual(*a, *b)) {
            return false;
        }

        if (a->firstChild()) {
            a = a->firstChild();
            b = b->firstChild();
            continue;
        }

        // No children: advance to the next node in preorder.
        for (;;) {
            if (a == &a_root) {
                return b == &b_root;
            }
            Node const *next_a = a->next();
            Node const *next_b = b->next();
            if (next_a || next_b) {
                if (!next_a || !next_b) {
                    return false;
                }
                a = next_a;
                b = next_b;
                break;
            }
            a = a->parent();
            b = b->parent();
            if (!a || !b) {
                return false;
            }
        }
    }
}

} // namespace

void PreparedContentProof::DocumentRelease::operator()(Document *document) const
{
    if (document) {
        GC::release(document);
    }
}

PreparedContentProof::PreparedContentProof(std::unique_ptr<Document, DocumentRelease> snapshot,
                                           std::optional<std::string> filename,
                                           std::optional<std::string> base)
    : _snapshot(std::move(snapshot))
    , _filename(std::move(filename))
    , _base(std::move(base))
{
}

PreparedContentProof::~PreparedContentProof() = default;

std::unique_ptr<PreparedContentProof> PreparedContentProof::capture(Document const &source,
                                                                    char const *filename,
                                                                    char const *base)
{
    // Wrap the native duplicate result immediately so every failure path below
    // releases it through GC::release RAII.
    std::unique_ptr<Document, DocumentRelease> snapshot(source.duplicate(nullptr));
    if (!snapshot) {
        return nullptr;
    }

    // SimpleDocument::duplicate does not copy noncanonical DOCUMENT fields, so
    // prove exact input-vs-snapshot equality here and report unavailable on any
    // mismatch rather than normalizing or repairing the native copy.
    if (!exactTreeEqual(source, *snapshot)) {
        return nullptr;
    }

    std::optional<std::string> owned_filename;
    if (filename) {
        owned_filename = filename;
    }
    std::optional<std::string> owned_base;
    if (base) {
        owned_base = base;
    }

    return std::unique_ptr<PreparedContentProof>(
        new PreparedContentProof(std::move(snapshot), std::move(owned_filename), std::move(owned_base)));
}

bool PreparedContentProof::matches(Document const &source, char const *filename, char const *base) const
{
    if (!_snapshot) {
        return false;
    }
    if (!exactTreeEqual(source, *_snapshot)) {
        return false;
    }
    if (filename) {
        if (!_filename || *_filename != filename) {
            return false;
        }
    } else if (_filename) {
        return false;
    }
    if (base) {
        if (!_base || *_base != base) {
            return false;
        }
    } else if (_base) {
        return false;
    }
    return true;
}

Document const &PreparedContentProof::preparedDocument() const
{
    return *_snapshot;
}

} // namespace XML
} // namespace Inkscape
