// SPDX-License-Identifier: GPL-2.0-or-later
#include "artwork-library-workspace.h"
#include "io/artwork-library-lbart.h"
#include "io/artwork-library-lightburn.h"
#include "svg/svg-length.h"
#include <gio/gio.h>
#include <libxml/xmlreader.h>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <utility>

namespace Inkscape::UI::Dialog {
namespace {
void require(bool ok, char const *message) { if (!ok) throw std::runtime_error(message); }
void poll(Art::Cancelled const &c) { if (c && c()) throw std::runtime_error("Cancelled; unpublished work discarded"); }
std::string uuid() { auto p = g_uuid_string_random(); std::string s(p); g_free(p); return s; }
Art::Catalog fresh(std::string name) { Art::Manifest m; m.id = uuid(); m.name = std::move(name); return Art::Catalog::create(std::move(m)); }
std::string display_stem(std::string const &path, char const *fallback)
{
    auto display = g_filename_display_basename(path.c_str());
    std::string name(display); g_free(display);
    // The filename is a display label only; content still determines format.
    auto dot = name.rfind('.');
    if (dot != std::string::npos && dot != 0) name.resize(dot);
    for (char &ch : name) if (static_cast<unsigned char>(ch) < 32 || ch == 127) ch = '_';
    // Keep valid UTF-8 and room for a duplicate-name suffix within the manifest's
    // 1024-byte name limit. This does not change the source filename or path.
    if (name.size() > 1000) {
        auto end = name.c_str();
        while (*end && g_utf8_next_char(end) - name.c_str() <= 1000) end = g_utf8_next_char(end);
        name.resize(end - name.c_str());
    }
    if (name.empty() || name.find_first_not_of(' ') == std::string::npos) return fallback;
    return name;
}
std::string imported_collection_name(std::vector<std::string> const &paths)
{
    return paths.size() == 1 ? display_stem(paths.front(), "Imported artwork") : "Imported artwork";
}
std::string quoted(std::string const &s)
{
    std::string out = "\""; char const *hex = "0123456789abcdef";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (c < 32) { out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15]; }
        else out += c;
    }
    return out + '"';
}
bool zip(std::string const &path, Art::Cancelled c) { auto b = Art::read_library_input(path, 4, c, true); return b.size() == 4 && b[0] == 'P' && b[1] == 'K'; }
// Metadata discovery ONLY. No native document and no resource loading. The full
// immutable bytes still go through preflight, which validates every attribute,
// DTD/entity/namespace, physical dimensions and reference before any native use.
std::pair<double, double> dimensions(Art::Bytes const &b)
{
    require(b.size() <= 32u * 1024 * 1024, "SVG input byte limit exceeded");
    std::unique_ptr<xmlTextReader, decltype(&xmlFreeTextReader)> r(
        xmlReaderForMemory(reinterpret_cast<char const *>(b.data()), int(b.size()), nullptr, nullptr,
                           XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING), xmlFreeTextReader);
    require(bool(r), "Cannot read SVG root");
    while (xmlTextReaderRead(r.get()) == 1) {
        auto type = xmlTextReaderNodeType(r.get());
        require(type != XML_READER_TYPE_DOCUMENT_TYPE && type != XML_READER_TYPE_ENTITY_REFERENCE,
                "DTD/entities are not admitted");
        if (type != XML_READER_TYPE_ELEMENT) continue;
        require(xmlStrEqual(xmlTextReaderConstLocalName(r.get()), BAD_CAST "svg") &&
                xmlStrEqual(xmlTextReaderConstNamespaceUri(r.get()), BAD_CAST "http://www.w3.org/2000/svg"), "Expected SVG root");
        auto length = [&](char const *key) {
            auto raw = xmlTextReaderGetAttribute(r.get(), BAD_CAST key);
            require(raw, "SVG requires explicit physical width and height");
            std::string s(reinterpret_cast<char const *>(raw)); xmlFree(raw);
            SVGLength l;
            require(l.readAbsolute(s.c_str()),
                    "SVG dimensions require absolute units");
            require(std::isfinite(l.computed) && l.computed > 0, "Invalid SVG dimensions");
            return l.computed * 25.4 / 96;
        };
        return {length("width"), length("height")};
    }
    throw std::runtime_error("Missing SVG root");
}
void add_checked(Art::Catalog &catalog, Art::NewArtwork m, Art::Bytes const &b, Art::Cancelled c)
{
    if (m.id.empty()) m.id = uuid();
    (void)Art::preflight_svg(b, {m.id, Art::artwork_sha256(b, c), m.width_mm, m.height_mm}, {}, c);
    catalog.add(std::move(m), b, c);
}
// Admission for a library-level save. Names the artwork so the user can find
// and remove it. Cancellation keeps its own message.
Art::ValidatedSvg admit_named(Art::CatalogSnapshot const &snapshot, std::string const &id, Art::Cancelled const &c)
{
    try {
        return ArtworkLibraryWorkspace::admit(snapshot, id, c);
    } catch (std::exception const &e) {
        poll(c);
        std::string name = id;
        try { name = snapshot.asset(id).name; } catch (...) {}
        throw std::runtime_error("Artwork \"" + name + "\" cannot be saved: " + e.what() +
                                 ". Remove it from the collection, then save again.");
    }
}
}

LibraryCollection::LibraryCollection(Art::Catalog value, std::string filename,
    std::optional<Art::FileVersion> file_version, std::optional<std::uint64_t> saved)
    : catalog(std::move(value)), path(std::move(filename)), version(std::move(file_version)), saved_revision(saved) { sync_header(); }
void LibraryCollection::sync_header() { auto m = catalog.snapshot().manifest(); identity = m.id; label = m.name; revision = m.revision; }
bool LibraryCollection::dirty() const { return !saved_revision || *saved_revision != revision; }
ArtworkLibraryWorkspace::ArtworkLibraryWorkspace() : _owner(std::this_thread::get_id()) {}
ArtworkLibraryWorkspace::~ArtworkLibraryWorkspace() = default;
std::shared_ptr<ArtworkLibraryWorkspace> ArtworkLibraryWorkspace::create() { return std::shared_ptr<ArtworkLibraryWorkspace>(new ArtworkLibraryWorkspace); }
void ArtworkLibraryWorkspace::check_idle(bool closing) const {
    require(_owner == std::this_thread::get_id(), "Library workspace owning-thread violation");
    require(!_closing || closing, "Library is reserved for a close decision");
    require(!_busy && !_pending, "Finish/cancel the current library operation or import decision first");
}
LibraryCollection *ArtworkLibraryWorkspace::find(std::string const &id) {
    for (auto &c : _collections) if (c.identity == id) return &c;
    return nullptr;
}
LibraryCollection const *ArtworkLibraryWorkspace::active() const {
    for (auto const &c : _collections) if (c.identity == _active) return &c;
    return nullptr;
}
LibraryCollection &ArtworkLibraryWorkspace::current() { auto c = find(_active); require(c, "No active collection"); return *c; }
bool ArtworkLibraryWorkspace::can_close() const {
    return !_closing && !_busy && !_pending && std::none_of(_collections.begin(), _collections.end(), [](auto const &c) { return c.dirty() || c.uncertain; });
}
void ArtworkLibraryWorkspace::notify() { auto pin = shared_from_this(); changed.emit(); }
void ArtworkLibraryWorkspace::cancel() {
    require(!_closing, "Library is reserved for close; use the close dialog's cancellation"); cancel_impl();
}
void ArtworkLibraryWorkspace::cancel_impl() {
    if (_pending) { _pending.reset(); _message = "Import cancelled; original collection unchanged"; }
    else if (_cancel) { *_cancel = true; _message = "Cancellation requested; waiting for the final publication result"; }
    else _message = "No library operation is pending";
    notify();
}
void ArtworkLibraryWorkspace::start(std::function<Complete(Art::Cancelled)> work, bool retain_publication_result, bool closing)
{
    check_idle(closing);
    struct Result { std::mutex mutex; bool done = false; Complete complete; std::string error; };
    auto result = std::make_shared<Result>();
    auto cancellation = std::make_shared<std::atomic<bool>>(false);
    auto self = shared_from_this();
    _cancel = cancellation; _busy = true;
    try {
        std::thread([result, cancellation, work = std::move(work)] {
            Complete complete; std::string error;
            try { complete = work([cancellation] { return cancellation->load(); }); }
            catch (std::exception const &e) { error = e.what(); }
            catch (...) { error = "Unexpected library worker failure"; }
            std::lock_guard lock(result->mutex);
            result->complete = std::move(complete); result->error = std::move(error); result->done = true;
        }).detach();
    } catch (...) { _busy = false; _cancel.reset(); throw; }
    // The source retains the workspace, not a panel. Never lose a late storage
    // publication because a widget closed. No synchronous join on a NAS read.
    auto completion = new std::function<bool()>([self, result, cancellation, retain_publication_result] {
        Complete complete; std::string error;
        { std::lock_guard lock(result->mutex); if (!result->done) return true;
          complete = std::move(result->complete); error = std::move(result->error); }
        self->_busy = false; self->_cancel.reset();
        try {
            if (cancellation->load() && !retain_publication_result) self->_message = "Cancelled; unpublished work discarded";
            else if (complete) complete(*self); else self->_message = error;
        }
        catch (std::exception const &e) { self->_message = e.what(); }
        self->notify(); return false;
    });
    g_timeout_add_full(G_PRIORITY_DEFAULT, 25, [](void *p) -> gboolean {
        try { return (*static_cast<std::function<bool()> *>(p))(); } catch (...) { return false; }
    }, completion, [](void *p) { delete static_cast<std::function<bool()> *>(p); });
    notify();
}
void ArtworkLibraryWorkspace::invalidate_page() { ++_page_generation; _page.clear(); }
void ArtworkLibraryWorkspace::refresh_rows() {
    ++_rows_generation; _rows.clear(); invalidate_page();
    if (auto c = active()) { auto s = c->catalog.snapshot(); for (auto const &id : s.search(_query)) _rows.push_back(s.asset(id)); }
}
void ArtworkLibraryWorkspace::new_collection(std::string name) {
    check_idle(); require(_collections.size() < 16, "At most 16 open collections");
    _collections.push_back({fresh(std::move(name))}); _active = _collections.back().identity;
    refresh_rows(); notify();
}
void ArtworkLibraryWorkspace::select_collection(std::string id) {
    check_idle(); require(find(id), "Collection unloaded"); _active = std::move(id);
    remember_selection(); refresh_rows(); notify();
}
void ArtworkLibraryWorkspace::remember_selection() {
    auto c = active();
    auto path = c ? c->path : std::string{};
    if (_saved_session.active_path == path) return;
    _saved_session.active_path = std::move(path);
    session_changed.emit();
}
void ArtworkLibraryWorkspace::remember_saved(LibraryCollection const &collection, std::string const &previous_path) {
    // Merge durable paths rather than snapshotting the catalog: close/discard
    // must not forget saved files, nor should an offline disk erase its entries.
    auto c = &collection;
    if (c->path.empty()) return;
    auto &paths = _saved_session.paths;
    auto old = std::find(paths.begin(), paths.end(), previous_path);
    if (old != paths.end()) *old = c->path;
    else if (std::find(paths.begin(), paths.end(), c->path) == paths.end()) {
        if (paths.size() == 16) paths.erase(paths.begin());
        paths.push_back(c->path);
    }
    // Saving collections in a different order must not reorder the sidebar
    // on restart. Unavailable saved files remain eligible for the next reopen.
    std::vector<std::string> ordered;
    for (auto const &loaded : _collections) if (!loaded.path.empty()) ordered.push_back(loaded.path);
    for (auto const &path : paths)
        if (ordered.size() < 16 && std::find(ordered.begin(), ordered.end(), path) == ordered.end())
            ordered.push_back(path);
    paths = std::move(ordered);
    if (_active == c->identity || (!previous_path.empty() && _saved_session.active_path == previous_path))
        _saved_session.active_path = c->path;
    session_changed.emit();
}
void ArtworkLibraryWorkspace::restore_session(LibrarySavedSession session) {
    check_idle(); require(_collections.empty(), "Restore requires an empty library workspace");
    _saved_session = {};
    for (auto const &path : session.paths) {
        if (_saved_session.paths.size() == 16) break;
        try {
            auto canonical = Art::canonical_library_path(path);
            if (std::find(_saved_session.paths.begin(), _saved_session.paths.end(), canonical) == _saved_session.paths.end())
                _saved_session.paths.push_back(std::move(canonical));
        } catch (Art::StorageError const &) {} // Invalid saved input, not an unavailable file.
    }
    if (!session.active_path.empty()) {
        try {
            auto canonical = Art::canonical_library_path(session.active_path);
            if (std::find(_saved_session.paths.begin(), _saved_session.paths.end(), canonical) != _saved_session.paths.end())
                _saved_session.active_path = std::move(canonical);
        } catch (Art::StorageError const &) {}
    }
    if (_saved_session.paths.empty()) return;
    _message = "Reopening saved libraries…";
    start([session = _saved_session](Art::Cancelled cancelled) mutable -> Complete {
        std::vector<LibraryCollection> collections;
        std::string unavailable;
        for (auto &path : session.paths) {
            poll(cancelled);
            try {
                auto loaded = Art::load_library(path, {}, cancelled);
                if (session.active_path == path) session.active_path = loaded.version.path;
                path = loaded.version.path;
                LibraryCollection collection{Art::Catalog::from_package(std::move(loaded.package), {}, cancelled),
                                             path, loaded.version, loaded.version.revision};
                if (std::none_of(collections.begin(), collections.end(),
                    [&](auto const &other) { return other.identity == collection.identity; }))
                    collections.push_back(std::move(collection));
            } catch (std::exception const &) {
                poll(cancelled);
                auto name = g_filename_display_basename(path.c_str());
                if (!unavailable.empty()) unavailable += ", ";
                unavailable += name; g_free(name);
            }
        }
        return [collections = std::move(collections), session = std::move(session),
                unavailable = std::move(unavailable)](auto &w) mutable {
            w._collections = std::move(collections);
            w._saved_session = std::move(session);
            auto &paths = w._saved_session.paths;
            std::vector<std::string> unique;
            for (auto const &path : paths)
                if (std::find(unique.begin(), unique.end(), path) == unique.end()) unique.push_back(path);
            paths = std::move(unique);
            w._active = w._collections.empty() ? "" : w._collections.front().identity;
            for (auto const &c : w._collections) if (c.path == w._saved_session.active_path) w._active = c.identity;
            w.session_changed.emit();
            w._message = unavailable.empty() ? "Saved libraries reopened" :
                "Could not reopen: " + unavailable + ". Use Open when the files are available.";
            w.refresh_rows();
        };
    });
}
void ArtworkLibraryWorkspace::unload(bool discard) {
    check_idle(); auto &c = current(); require(!c.uncertain || discard, "Uncertain publication: retain/reconcile files or explicitly confirm unloading without retry");
    require(!c.dirty() || discard, "Unsaved library changes: Save, Discard or Cancel");
    auto id = _active;
    std::erase(_saved_session.paths, c.path);
    std::erase_if(_collections, [&](auto const &v) { return v.identity == id; });
    _active = _collections.empty() ? "" : _collections.front().identity;
    _saved_session.active_path = active() ? active()->path : "";
    session_changed.emit();
    refresh_rows(); notify();
}
void ArtworkLibraryWorkspace::open(std::string path) {
    check_idle(); require(_collections.size() < 16, "At most 16 open collections");
    // Content dispatch belongs on the worker; suffix never grants trust.
    start([path = std::move(path)](Art::Cancelled c) mutable -> Complete {
        poll(c);
        path = Art::resolved_library_path(path);
        if (!zip(path, c)) return [path](auto &w) { w.import_files_impl({path}, true, w._closing); };
        auto loaded = Art::load_library(path, {}, c);
        LibraryCollection collection{Art::Catalog::from_package(std::move(loaded.package), {}, c), loaded.version.path,
                                     loaded.version, loaded.version.revision};
        return [collection = std::move(collection)](auto &w) mutable {
            auto id = collection.identity;
            require(!w.find(id), "This library identity is already open; unload it before reopening/recovering");
            w._collections.push_back(std::move(collection)); w._active = id;
            w.remember_saved(w._collections.back());
            w._message = "Collection opened; artwork will be admitted only on demand"; w.refresh_rows();
        };
    });
}
void ArtworkLibraryWorkspace::import_files(std::vector<std::string> paths, bool into_new) {
    import_files_impl(std::move(paths), into_new);
}
void ArtworkLibraryWorkspace::import_files_impl(std::vector<std::string> paths, bool into_new, bool closing) {
    check_idle(closing); require(!paths.empty() && paths.size() <= 128, "Select 1..128 input files");
    require(!into_new || _collections.size() < 16, "At most 16 open collections");
    auto name = into_new ? imported_collection_name(paths) : std::string();
    if (into_new) {
        auto base = name;
        for (unsigned suffix = 2; std::any_of(_collections.begin(), _collections.end(),
             [&](auto const &collection) { return collection.label == name; }); ++suffix)
            name = base + " (" + std::to_string(suffix) + ")";
    }
    auto candidate = into_new ? fresh(std::move(name)) : current().catalog;
    auto target = into_new ? std::string() : _active;
    start([paths = std::move(paths), candidate = std::move(candidate), target](Art::Cancelled c) mutable -> Complete {
        std::vector<LibraryImportEntry> report;
        auto one = [&](std::string const &path, std::size_t ordinal, std::string name, auto make) {
            poll(c); require(report.size() < 10000, "Import report entry limit exceeded");
            try { auto message = make(); report.push_back({path, std::move(name), std::move(message), ordinal, true}); }
            catch (std::exception const &e) { poll(c); report.push_back({path, std::move(name), e.what(), ordinal, false}); }
        };
        for (auto const &path : paths) {
            poll(c);
            try {
                if (zip(path, c)) {
                    auto loaded = Art::load_library(path, {}, c);
                    for (auto const &a : loaded.package.manifest().assets) one(path, 0, a.name, [&] {
                        auto b = loaded.package.read_artwork(a.id, c);
                        add_checked(candidate, {"", a.name, a.tags, a.width_mm, a.height_mm, a.extra_json}, b, c);
                        return std::string("Editable SVG admitted; original optional metadata retained");
                    });
                    continue;
                }
                auto b = Art::read_library_input(path, 512u * 1024 * 1024, c);
                auto first = std::find_if_not(b.begin(), b.end(), [](unsigned char ch) { return g_ascii_isspace(ch); });
                bool xml = first != b.end() && (*first == '<' || *first == 0xef);
                if (xml) {
                    auto name = display_stem(path, "Artwork");
                    one(path, 0, name, [&] { auto [w, h] = dimensions(b); add_checked(candidate, {"", name, {}, w, h}, b, c); return std::string("Editable SVG admitted"); });
                } else {
                    auto archive = Art::LbartArchive::open(std::move(b), {}, c); auto entries = archive.entries();
                    for (std::size_t i = 0; i < entries.size(); ++i) one(path, i + 1, entries[i].name, [&] {
                        auto raw = archive.read_artwork(i, c);
                        auto converted = Art::convert_lightburn_shapes_v1_draft(std::string(raw.begin(), raw.end()), {}, c);
                        std::string extra = "{\"sourceFormat\":\"LightBurnShapes-v1\",\"sourceOrdinal\":" + std::to_string(i + 1) + ",\"diagnostics\":[";
                        bool comma = false;
                        for (auto const &q : converted.qualifications) { if (comma) extra += ','; extra += quoted(q); comma = true; }
                        extra += "],\"unappliedLaserTabs\":["; comma = false;
                        for (auto const &tab : converted.unapplied_laser_tabs) {
                            if (comma) extra += ',';
                            extra += "{\"shapePreorder1Based\":" + std::to_string(tab.shape_preorder_1based) +
                                ",\"pairCount\":" + std::to_string(tab.pair_count) + ",\"message\":" + quoted(tab.message) + ",\"sourceTextChunks\":[";
                            // Exact decoded text in bounded JSON strings. The converter's
                            // numeric Tabs grammar is ASCII; chunk boundaries do not split UTF-8.
                            for (std::size_t p = 0; p < tab.source_text.size(); p += 4000) {
                                if (p) extra += ','; extra += quoted(tab.source_text.substr(p, 4000));
                            }
                            extra += "]}"; comma = true;
                        }
                        extra += "]}";
                        add_checked(candidate, {"", entries[i].name, {}, converted.width_mm, converted.height_mm, extra},
                                    Art::Bytes(converted.svg.begin(), converted.svg.end()), c);
                        std::string message = "Editable geometry admitted. ";
                        for (auto const &q : converted.qualifications) message += q + " ";
                        for (auto const &tab : converted.unapplied_laser_tabs)
                            message += std::string(tab.message) + " shape=" + std::to_string(tab.shape_preorder_1based) + " pairs=" + std::to_string(tab.pair_count) + ". ";
                        return message;
                    });
                }
            } catch (std::exception const &e) { poll(c); require(report.size() < 10000, "Import report entry limit exceeded"); report.push_back({path, "", e.what(), 0, false}); }
        }
        poll(c);
        return [pending = LibraryPendingImport{std::move(candidate), target, std::move(report)}](auto &w) mutable {
            w._pending = std::move(pending); w._message = "Review every import result. Nothing has been added yet.";
        };
    }, false, closing);
}
void ArtworkLibraryWorkspace::accept_import(bool accept) {
    require(!_closing, "Library is reserved for a close decision");
    require(!_busy && bool(_pending), "No import decision pending");
    auto pending = std::move(*_pending); _pending.reset();
    if (accept) {
        require(std::any_of(pending.entries.begin(), pending.entries.end(), [](auto const &e) { return e.converted; }), "No editable entries to import");
        if (pending.target_id.empty()) { _collections.push_back({std::move(pending.candidate)}); _active = _collections.back().identity; }
        else { auto target = find(pending.target_id); require(target, "Import target unloaded"); target->catalog = std::move(pending.candidate); target->sync_header(); }
        _message = "Accepted entries added; save the native .valib independently of the document"; refresh_rows();
    } else _message = "Import cancelled; original collection unchanged";
    notify();
}
void ArtworkLibraryWorkspace::edit(std::function<void(Art::Catalog &, Art::Cancelled)> operation) {
    check_idle(); auto candidate = current().catalog; auto id = _active;
    start([candidate = std::move(candidate), id, operation = std::move(operation)](Art::Cancelled c) mutable -> Complete {
        operation(candidate, c); poll(c);
        return [candidate = std::move(candidate), id](auto &w) mutable { auto target = w.find(id); require(target, "Edit target unloaded"); target->catalog = std::move(candidate); target->sync_header(); w.refresh_rows(); w._message = "Library edit staged; document history unchanged"; };
    });
}
void ArtworkLibraryWorkspace::add(Art::NewArtwork m, Art::Bytes b) { edit([m = std::move(m), b = std::move(b)](auto &catalog, auto c) { add_checked(catalog, m, b, c); }); }
void ArtworkLibraryWorkspace::rename(std::string id, std::string name) { edit([id, name](auto &v, auto c) { v.rename(id, name, c); }); }
void ArtworkLibraryWorkspace::tags(std::string id, std::vector<std::string> tags) { edit([id, tags](auto &v, auto c) { v.set_tags(id, tags, c); }); }
void ArtworkLibraryWorkspace::remove(std::string id) {
    check_idle(); auto candidate = current().catalog; auto library = _active;
    auto name = candidate.snapshot().asset(id).name;
    start([candidate = std::move(candidate), library, id, name](Art::Cancelled c) mutable -> Complete {
        candidate.remove(id, c); poll(c);
        return [candidate = std::move(candidate), library, id, name](auto &w) mutable {
            auto target = w.find(library); require(target, "Removal target unloaded");
            target->catalog = std::move(candidate); target->sync_header(); target->removed_names[id] = name;
            w.refresh_rows(); w._message = "Artwork moved to session trash; next Save retains a recovery collection first";
        };
    });
}
void ArtworkLibraryWorkspace::restore(std::string id) {
    // A restored artwork may be edited; removing it again needs a fresh trash copy.
    if (auto c = find(_active)) c->trash_covered.erase(id);
    edit([id](auto &v, auto c) { v.restore(id, c); });
}
void ArtworkLibraryWorkspace::save(std::string destination) { save_impl(_active, std::move(destination)); }
void ArtworkLibraryWorkspace::save_impl(std::string const &id, std::string destination, bool closing) {
    check_idle(closing); auto target = find(id); require(target, "Save target unloaded"); auto &collection = *target; require(!collection.uncertain, "Publication uncertain: reconcile the exact file/recovery before retrying");
    if (destination.empty()) destination = collection.path;
    require(!destination.empty(), "Save As requires a destination");
    destination = Art::canonical_library_path(destination);
    auto expected = collection.version;
    auto snapshot = collection.catalog.snapshot(); auto revision = collection.revision;
    // Coverage is per destination folder: Save As elsewhere writes its own trash copy.
    auto covered = destination == collection.path ? collection.trash_covered : std::set<std::string>{};
    start([destination, expected, snapshot, covered, id, revision](Art::Cancelled c) mutable -> Complete {
        poll(c);
        destination = Art::resolved_library_path(destination);
        // Loaded/saved versions already carry the resolved path. Save As to a
        // different destination must not access an old, possibly offline share.
        if (expected && destination != expected->path) expected.reset();
        bool const create_only = !expected;
        // No unsafe payload is laundered through Save: every ACTIVE artwork is
        // admitted. Removed artwork is not; a trash copy is admitted again when
        // it is opened or recovered. This lets the user remove an artwork that
        // no longer passes admission and still save the collection.
        for (auto const &asset : snapshot.list(c)) (void)admit_named(snapshot, asset, c);
        Art::StorageOptions options; options.cancelled = c;
        auto removed = snapshot.removed_ids(c);
        bool const needs_trash = std::any_of(removed.begin(), removed.end(),
            [&](auto const &asset) { return !covered.contains(asset); });
        std::string trash_path;
        Art::StorageResult result;
        bool recovery_ready = true;
        if (needs_trash) {
            // Only the removed artwork, never a second copy of the whole library.
            trash_path = destination + ".trash-" + uuid() + ".valib";
            Art::StorageResult recovery;
            try { recovery = Art::save_library(trash_path, snapshot.removed_items_snapshot(c), {}, options); }
            catch (std::exception const &e) { recovery.publication = Art::Publication::Uncertain; recovery.message = e.what(); }
            recovery_ready = recovery.publication == Art::Publication::Published && bool(recovery.version);
            if (!recovery_ready) {
                // This result concerns only the recovery candidate, not target publication.
                result = std::move(recovery); result.publication = Art::Publication::NotPublished;
                result.message = "Target not saved: trash recovery was not conclusively published. " + result.message;
            }
        }
        if (recovery_ready) {
            try { result = Art::save_library(destination, snapshot, expected, options); }
            catch (std::exception const &e) { result.publication = Art::Publication::Uncertain; result.message = e.what(); }
        }
        if (create_only && result.failure == Art::StorageFailure::Conflict &&
            result.message.starts_with("Create destination")) {
            result.message = "Not saved: a file already exists at " + destination +
                             ". Save As never replaces an existing file; choose a new file name.";
        }
        // LIB-11 (owner decision D1): only after a conclusive publication that
        // produced a new prior-version copy. Never blocks or fails the save.
        Art::RecoveryPruneResult pruned;
        if (result.publication == Art::Publication::Published && result.version && !result.recovery_path.empty()) {
            try { pruned = Art::prune_library_recovery(result.version->path, result.version->library_id, 5,
                                                       result.recovery_path); }
            catch (std::exception const &e) { pruned.warnings.push_back(e.what()); }
        }
        bool const trash_written = needs_trash && recovery_ready;
        return [result = std::move(result), id, revision, trash_path, trash_written,
                removed = std::move(removed), pruned = std::move(pruned)](auto &w) {
            auto target = w.find(id); require(target, "Save target unloaded");
            // Paths retained by THIS save only. The collection keeps a cumulative,
            // de-duplicated list for the unload/close prompts.
            std::vector<std::string> retained;
            if (trash_written) {
                retained.push_back(trash_path);
                target->trash_covered.insert(removed.begin(), removed.end());
            }
            for (auto const &p : {result.recovery_path, result.staged_path, result.retained_lock_path})
                if (!p.empty()) retained.push_back(p);
            for (auto const &p : retained)
                if (std::find(target->recovery_paths.begin(), target->recovery_paths.end(), p) == target->recovery_paths.end())
                    target->recovery_paths.push_back(p);
            for (auto const &p : pruned.removed) std::erase(target->recovery_paths, p);
            target->uncertain = result.publication == Art::Publication::Uncertain ||
                (result.publication == Art::Publication::Published && !result.version);
            if ((result.publication == Art::Publication::Published || result.publication == Art::Publication::Unchanged) && result.version) {
                auto previous_path = target->path;
                target->path = result.version->path; target->version = result.version; target->saved_revision = revision;
                w.remember_saved(*target, previous_path);
            }
            w._message = result.message;
            if (target->uncertain) w._message += " — uncertain publication; do not retry.";
            if (!retained.empty()) {
                w._message += " Retained paths:";
                for (auto const &p : retained) w._message += "\n" + p;
            }
            if (!pruned.removed.empty())
                w._message += "\nRemoved " + std::to_string(pruned.removed.size()) + " older recovery " +
                              (pruned.removed.size() == 1 ? "copy" : "copies") +
                              "; the newest 5 of this library in this folder are kept.";
            for (auto const &warning : pruned.warnings) w._message += "\nRecovery retention: " + warning;
            if (result.cancellation_too_late) w._message += "\nCancellation was too late; publication was not rolled back.";
        };
    }, true, closing);
}
void ArtworkLibraryWorkspace::scan_recovery(std::string directory) {
    check_idle();
    _recovery_scan.reset();
    start([directory = std::move(directory)](Art::Cancelled c) -> Complete {
        auto scan = Art::scan_library_recovery(directory, {}, c);
        return [scan = std::move(scan)](auto &w) mutable {
            auto valid = std::count_if(scan.files.begin(), scan.files.end(), [](auto const &file) { return bool(file.version); });
            w._message = std::to_string(valid) + " verified recovery candidates; " +
                std::to_string(scan.files.size() - valid) + " candidates require attention." +
                (scan.limited ? " Discovery limits reached; results are incomplete." : "") +
                " Nothing has been restored. Use Recovery → Review recovery results.";
            w._recovery_scan = std::move(scan);
        };
    });
}
void ArtworkLibraryWorkspace::recover(std::string path, std::optional<Art::FileVersion> expected) {
    check_idle(); require(_collections.size() < 16, "At most 16 open collections");
    start([path, expected = std::move(expected)](Art::Cancelled c) -> Complete {
        auto loaded = Art::load_library(path, {}, c);
        require(!expected || *expected == loaded.version, "Recovery candidate changed since discovery; inspect it again");
        auto source = Art::Catalog::from_package(std::move(loaded.package), {}, c).snapshot();
        // Recovery copy has a new identity and no overwrite authority over the original.
        auto header = source.manifest(c);
        header.id = uuid(); header.revision = 0; header.assets.clear();
        // Keep collection-level optional metadata, just as for each asset.
        // Recovery changes identity/authority, not the user's opaque metadata.
        // A valid maximum-length name must not become invalid merely because
        // a UI suffix was appended; retain it intact in that case.
        std::string const suffix = " — recovered copy";
        if (header.name.size() + suffix.size() <= 1024) header.name += suffix;
        auto recovered = Art::Catalog::create(std::move(header), {}, c);
        for (auto const &id : source.list(c)) { auto a = source.asset(id); auto b = source.read_artwork(id, c); add_checked(recovered, {a.id, a.name, a.tags, a.width_mm, a.height_mm, a.extra_json}, *b, c); }
        return [recovered = std::move(recovered)](auto &w) mutable { w._collections.push_back({std::move(recovered)}); w._active = w._collections.back().identity; w._message = "Recovery opened as an unsaved independent collection; original retained"; w.refresh_rows(); };
    });
}
void ArtworkLibraryWorkspace::inspect_lock() {
    check_idle(); auto &c = current();
    require(!c.path.empty(), "Save this collection to a file before checking its lock");
    _inspected_lock.reset();
    start([path = c.path](Art::Cancelled cancelled) -> Complete {
        poll(cancelled);
        auto lock = Art::inspect_library_lock(path);
        return [lock = std::move(lock)](auto &w) mutable {
            if (!lock) { w._message = "No lock file exists for this library; saving is not blocked by a lock."; return; }
            w._inspected_lock = std::move(lock);
            w._inspected_lock_collection = w._active;
            w._message = "Lock file found: " + w._inspected_lock->path;
        };
    });
}
void ArtworkLibraryWorkspace::remove_lock(Art::LibraryLock lock) {
    check_idle(); _inspected_lock.reset();
    start([lock = std::move(lock)](Art::Cancelled cancelled) -> Complete {
        poll(cancelled);
        Art::remove_stale_library_lock(lock);
        return [path = lock.path](auto &w) { w._message = "Removed lock file " + path + ". You can save again."; };
    });
}
void ArtworkLibraryWorkspace::dismiss_lock(std::string message) {
    require(_owner == std::this_thread::get_id(), "Library workspace owning-thread violation");
    if (!_inspected_lock) return;
    _inspected_lock.reset(); _message = std::move(message); notify();
}
void ArtworkLibraryWorkspace::export_svg(std::string id, std::string destination) {
    check_idle(); auto snapshot = current().catalog.snapshot();
    start([snapshot, id, destination](Art::Cancelled c) -> Complete {
        auto token = admit(snapshot, id, c); auto bytes = token.svg_bytes(); poll(c);
        // CREATE ONLY; publication cannot overwrite existing art. Failure leaves
        // its exact path visible, never silently deletes user data or retries.
        std::string message;
        try {
            Art::write_library_export(destination, Art::Bytes(bytes->begin(), bytes->end()), c);
            message = "SVG exported: " + destination;
        } catch (std::exception const &e) {
            message = "SVG export incomplete: " + destination + ": " + e.what() +
                " — any partial file retained; nothing overwritten";
        }
        return [message](auto &w) { w._message = message; };
    }, true);
}
Art::ValidatedSvg ArtworkLibraryWorkspace::admit(Art::CatalogSnapshot s, std::string id, Art::Cancelled c) {
    auto a = s.asset(id); auto b = s.read_artwork(id, c);
    return Art::preflight_svg(*b, {a.id, a.sha256, a.width_mm, a.height_mm}, {}, c);
}
void ArtworkLibraryWorkspace::search(std::string query) {
    check_idle(); require(query.size() <= 4096, "Search query too long"); _query = std::move(query);
    auto snapshot = current().catalog.snapshot(); auto q = _query; invalidate_page();
    start([snapshot, q](Art::Cancelled c) -> Complete { std::vector<Art::Asset> rows; for (auto const &id : snapshot.search(q, c)) rows.push_back(snapshot.asset(id));
        return [rows = std::move(rows)](auto &w) mutable { w._rows = std::move(rows); ++w._rows_generation; }; });
}
void ArtworkLibraryWorkspace::visible(std::vector<std::string> ids) {
    check_idle(); require(ids.size() <= 128, "Visible page exceeds 128 cells"); auto snapshot = current().catalog.snapshot();
    auto generation = ++_page_generation; _page.clear();
    start([snapshot, ids = std::move(ids), generation](Art::Cancelled c) -> Complete {
        std::vector<LibraryPageEntry> page; std::size_t bytes = 0;
        for (auto const &id : ids) { poll(c); LibraryPageEntry entry{snapshot.asset(id)};
            try { auto n = snapshot.artwork_size(id); require(n <= 64u * 1024 * 1024 - bytes, "Visible-page input exceeds 64 MiB");
                entry.svg = admit(snapshot, id, c); bytes += n;
            } catch (std::exception const &e) { poll(c); entry.diagnostic = e.what(); }
            page.push_back(std::move(entry));
        }
        return [page = std::move(page), generation](auto &w) mutable { if (generation == w._page_generation) w._page = std::move(page); };
    });
}
}
