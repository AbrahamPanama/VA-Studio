// SPDX-License-Identifier: GPL-2.0-or-later
#include "artwork-library-thumbnail.h"
#include "document.h"
#include "display/drawing.h"
#include "display/drawing-context.h"
#include "display/drawing-surface.h"
#include "object/sp-root.h"
#include "util/units.h"
#include <2geom/transforms.h>
#include <cairomm/surface.h>
#include <gdk/gdk.h>
#include <glibmm/wrap.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <thread>
#include <tuple>
#include <utility>

namespace Inkscape::UI::Cache {
namespace {
constexpr unsigned renderer_policy = 2; // Fixed dithering off; bump for framing, color, quality/fence changes.
void size_ok(ThumbnailSize s)
{
    if (!s.width || s.width > 512 || !s.height || s.height > 512 ||
        !s.device_scale || s.device_scale > 4)
        throw PreviewRenderLimit("Thumbnail dimensions/device scale out of range");
}
std::size_t add(std::size_t a, std::size_t b)
{
    if (b > std::numeric_limits<std::size_t>::max() - a)
        throw PreviewRenderLimit("Thumbnail allocation sum overflow");
    return a + b;
}
std::size_t multiply(std::size_t a, std::size_t b)
{
    if (a && b > std::numeric_limits<std::size_t>::max() / a)
        throw PreviewRenderLimit("Thumbnail allocation product overflow");
    return a * b;
}
struct Key {
    std::array<char, 64> hash;
    unsigned svg_policy, renderer;
    ThumbnailSize size;
    // A hit must not bypass a service's stricter native render policy.
    std::size_t bytes, work;
    unsigned dimension;
    double kernel;
    bool operator==(Key const &) const = default;
};
Key key(IO::ArtworkLibrary::ValidatedSvg const &svg, ThumbnailSize size,
        PreviewRenderBudget::Limits limits)
{
    auto hash = svg.sha256();
    if (hash.size() != 64) throw PreviewRenderLimit("Invalid admitted thumbnail hash");
    Key result{{}, svg.policy_version(), renderer_policy, size,
               limits.bytes, limits.work, limits.dimension, limits.kernel};
    std::copy(hash.begin(), hash.end(), result.hash.begin());
    return result;
}

// Explicit retained-allocation envelopes include the C++ owners/control blocks
// and opaque Cairo/GBytes/GObject bookkeeping; not malloc arena or GPU memory.
constexpr std::size_t pool_overhead = 4096;
constexpr std::size_t backing_overhead = 2048;
constexpr std::size_t texture_overhead = 2048;
constexpr std::size_t result_overhead = 1024;
constexpr std::size_t handle_overhead = 1024;
constexpr std::size_t entry_overhead = 1024;
constexpr std::size_t maximum_entries = 2048;
using Stats = ArtworkLibraryThumbnailPool::Stats;
struct Accounting {
    explicit Accounting(std::size_t capacity) {
        value.capacity = capacity; value.charged = value.resident = pool_overhead;
    }
    mutable std::mutex mutex;
    Stats value;
};
// No native/UI pointers: the last owner can release on another thread after
// the application, pool, context and service have all gone.
class Charge {
public:
    Charge(std::shared_ptr<Accounting> a, std::size_t n) : account(std::move(a)), amount(n) {}
    Charge(Charge const &) = delete;
    ~Charge() {
        std::lock_guard lock(account->mutex);
        auto &s = account->value;
        (committed ? s.resident : s.reserved) -= amount;
        s.charged -= amount;
        if (committed && pinned) s.externally_pinned -= amount;
        if (pixels) { s.pixel_bytes -= pixels; --s.backings; ++s.backing_releases; }
        if (texture) ++s.texture_releases;
    }
    void commit(bool external) {
        std::lock_guard lock(account->mutex);
        if (committed) return;
        committed = true; pinned = external;
        auto &s = account->value;
        s.reserved -= amount; s.resident += amount;
        if (pinned) s.externally_pinned += amount;
    }
    void pin(bool external) noexcept {
        std::lock_guard lock(account->mutex);
        if (committed && pinned != external) {
            if (external) account->value.externally_pinned += amount;
            else account->value.externally_pinned -= amount;
        }
        pinned = external;
    }
    bool is_pinned() const {
        std::lock_guard lock(account->mutex);
        return pinned;
    }
    void allocated_pixels(std::size_t n) {
        std::lock_guard lock(account->mutex);
        pixels = n; account->value.pixel_bytes += n;
        ++account->value.backings; ++account->value.backing_allocations;
    }
    void allocated_texture() {
        std::lock_guard lock(account->mutex);
        texture = true; ++account->value.texture_allocations;
    }
    void owns_bytes(bool held) {
        std::lock_guard lock(account->mutex);
        if (held) ++account->value.backing_owners;
        else --account->value.backing_owners;
        bytes_held = held;
    }
    bool owns_bytes() const {
        std::lock_guard lock(account->mutex);
        return bytes_held;
    }
    std::size_t size() const { return amount; }
private:
    std::shared_ptr<Accounting> account;
    std::size_t amount, pixels = 0;
    bool committed = false, pinned = false, texture = false, bytes_held = false;
};
class Reservation {
public:
    Reservation(std::shared_ptr<Accounting> a, std::size_t n) : account(std::move(a)), remaining(n) {
        std::lock_guard lock(account->mutex);
        auto &s = account->value;
        if (n > s.capacity - s.charged) throw PreviewRenderLimit("Thumbnail pool exhausted");
        s.reserved += n; s.charged += n;
    }
    Reservation(Reservation const &) = delete;
    Reservation &operator=(Reservation const &) = delete;
    Reservation(Reservation &&other) noexcept
        : account(other.account), remaining(std::exchange(other.remaining, 0)) {}
    ~Reservation() {
        std::lock_guard lock(account->mutex);
        account->value.reserved -= remaining; account->value.charged -= remaining;
    }
    std::shared_ptr<Charge> take(std::size_t n) {
        if (n > remaining) throw std::logic_error("Thumbnail reservation partition");
        // Allocation of this owner/control block is covered by its envelope.
        auto result = std::make_shared<Charge>(account, n);
        remaining -= n;
        return result;
    }
private:
    std::shared_ptr<Accounting> account;
    std::size_t remaining;
};
// allocate_shared retains this allocator through weak_ptr control-block life.
// A weak result therefore cannot leave an uncharged live control block.
template <typename T> struct ResultAllocator {
    using value_type = T;
    std::shared_ptr<Charge> charge;
    explicit ResultAllocator(std::shared_ptr<Charge> c) : charge(std::move(c)) {}
    template <typename U> ResultAllocator(ResultAllocator<U> const &a) : charge(a.charge) {}
    T *allocate(std::size_t n) {
        if (multiply(n, sizeof(T)) > charge->size() / 2)
            throw PreviewRenderLimit("Thumbnail retained control block exceeds envelope");
        return std::allocator<T>{}.allocate(n);
    }
    void deallocate(T *p, std::size_t n) { std::allocator<T>{}.deallocate(p, n); }
    template <typename U> bool operator==(ResultAllocator<U> const &a) const { return charge == a.charge; }
};
struct TextureUnref {
    void operator()(GdkTexture *p) const { if (p) g_object_unref(p); }
};
using TextureOwner = std::unique_ptr<GdkTexture, TextureUnref>;
Glib::RefPtr<Gdk::Texture> charged_texture(TextureOwner raw, std::shared_ptr<Charge> const &charge)
{
    // Same wrapper operation as Glib::wrap, but its shared control block also
    // has a charged allocator. Copying just this RefPtr (or a weak_ptr to it)
    // cannot outlive its metadata reservation. ObjectBase itself is attached
    // to the GObject and covered by the TextureRecord envelope.
    auto wrapped = dynamic_cast<Gdk::Texture *>(Glib::wrap_auto(G_OBJECT(raw.get()), false));
    if (!wrapped) throw std::runtime_error("Native thumbnail wrapper unavailable");
    raw.release();
    return Glib::RefPtr<Gdk::Texture>(wrapped, [](Gdk::Texture *p) { p->unreference(); },
                                     ResultAllocator<Gdk::Texture>{charge});
}
struct BackingOwner {
    std::shared_ptr<Charge> charge;
    cairo_surface_t *surface;
    BackingOwner(std::shared_ptr<Charge> c, cairo_surface_t *s)
        : charge(std::move(c)), surface(cairo_surface_reference(s)) { charge->owns_bytes(true); }
    BackingOwner(BackingOwner const &) = delete;
    ~BackingOwner() {
        cairo_surface_destroy(surface); // Before charge destruction.
        charge->owns_bytes(false);
    }
};
struct TextureRecord;
// GLib explicitly allows a delayed toggle notification after removal, even
// with a dangling object argument. The callback therefore receives a never-
// reused integer token, not any object's address. This registry has no heap
// nodes of its own: links are part of already-charged TextureRecord metadata.
// The mutex also fences record destruction against callbacks already running.
// Zero-initialized GMutex needs no C++ static destructor; late texture
// finalizers cannot race the destruction of an application-owned mutex.
GMutex records_mutex;
struct RecordsLock {
    RecordsLock() { g_mutex_lock(&records_mutex); }
    ~RecordsLock() { g_mutex_unlock(&records_mutex); }
};
TextureRecord *records_first = nullptr;
guint next_record_token = 0;
struct TextureRecord {
    std::shared_ptr<Charge> metadata;
    std::weak_ptr<Charge> backing;
    std::size_t bytes = 0, warning_count = 0;
    PreviewRenderBudget::Stats render_stats;
    std::unique_ptr<char[]> warning_bytes;
    std::unique_ptr<std::string_view[]> warnings;
    TextureRecord *previous = nullptr, *next = nullptr;
    guint token = 0;
    std::int64_t external_transitions = 1;
    void unregister();
    ~TextureRecord() {
        unregister();
        // A GBytes owner may survive the GdkTexture itself. It is then not
        // reclaimable by this cache, and stays pinned until its real finalizer.
        if (auto b = backing.lock()) b->pin(true);
    }
    void pin(bool external) noexcept {
        metadata->pin(external);
        if (auto b = backing.lock()) b->pin(external);
    }
};
void TextureRecord::unregister()
{
    RecordsLock lock;
    if (!token) return;
    // Once removed, any late notification becomes a lookup miss. A surviving
    // texture/backing is wholly external and cannot be reclaimed by the LRU.
    pin(true);
    if (previous) previous->next = next; else records_first = next;
    if (next) next->previous = previous;
    token = 0;
}
guint allocate_record_token()
{
    RecordsLock lock;
    if (next_record_token == std::numeric_limits<guint>::max())
        throw PreviewRenderLimit("Thumbnail notification token space exhausted");
    return ++next_record_token;
}
void register_record(TextureRecord &r, guint token)
{
    RecordsLock lock;
    r.token = token;
    r.next = records_first;
    if (records_first) records_first->previous = &r;
    records_first = &r;
}
GQuark record_key()
{
    static auto value = g_quark_from_static_string("vacards-thumbnail-pool-record");
    return value;
}
TextureRecord &record(GdkTexture *texture)
{
    return *static_cast<TextureRecord *>(g_object_get_qdata(G_OBJECT(texture), record_key()));
}
void toggled(gpointer data, GObject *, gboolean last) noexcept
{
    RecordsLock lock;
    for (auto r = records_first; r; r = r->next) {
        if (r->token != GPOINTER_TO_UINT(data)) continue;
        // Delta accounting, rather than assigning the last delivered boolean,
        // handles notifications that finish out of order across ref/unref
        // threads. Allocation charge never depends on this eviction hint.
        r->external_transitions += last ? -1 : 1;
        r->pin(r->external_transitions != 0);
        break;
    }
}
// One native operation process-wide, even if an unrelated DrawingScope masks
// the thread-local PreviewRenderBudget or an isolated test creates two pools.
std::atomic_flag native_active = ATOMIC_FLAG_INIT;
struct NativeGate {
    explicit NativeGate(std::shared_ptr<Accounting> a) : account(std::move(a)) {
        if (native_active.test_and_set()) throw PreviewRenderLimit("Thumbnail native render already active");
        std::lock_guard lock(account->mutex); ++account->value.active_jobs;
    }
    ~NativeGate() {
        { std::lock_guard lock(account->mutex); --account->value.active_jobs; }
        native_active.clear();
    }
    std::shared_ptr<Accounting> account;
};
// Drawing outlives the shown tree; hide executes before either document or
// drawing destruction, including exception paths. No borrowed SPItem escapes.
struct Shown {
    SPRoot *root;
    unsigned key;
    ~Shown() { root->invoke_hide(key); }
};
}

struct ArtworkLibraryThumbnailPool::State {
    explicit State(GMainContext *c, std::shared_ptr<Accounting> a)
        : context(g_main_context_ref(c ? c : g_main_context_default())),
          owner(std::this_thread::get_id()), account(std::move(a)) {}
    ~State() { clear(); g_main_context_unref(context); }
    GMainContext *context;
    std::thread::id owner;
    std::shared_ptr<Accounting> account;
    bool closed = false;
    std::uint64_t epoch = 0;
    struct Entry {
        Key key;
        GdkTexture *texture = nullptr;
        std::shared_ptr<Charge> metadata;
        Entry *previous = nullptr, *next = nullptr;
    };
    Entry *first = nullptr, *last = nullptr;
    std::size_t count = 0;
    void thread() const {
        if (owner != std::this_thread::get_id())
            throw std::logic_error("Thumbnail pool owning-thread violation");
    }
    void unlink(Entry *e) {
        if (e->previous) e->previous->next = e->next; else first = e->next;
        if (e->next) e->next->previous = e->previous; else last = e->previous;
    }
    void front(Entry *e) {
        e->previous = nullptr; e->next = first;
        if (first) first->previous = e; else last = e;
        first = e;
    }
    void erase(Entry *e) {
        unlink(e); --count;
        { std::lock_guard lock(account->mutex); --account->value.cache_entries; }
        // No registry/accounting lock spans a GObject operation.
        auto texture = e->texture;
        auto &r = record(texture);
        g_object_ref(texture);
        auto token = r.token;
        r.unregister();
        g_object_remove_toggle_ref(G_OBJECT(texture), toggled, GUINT_TO_POINTER(token));
        delete e;
        g_object_unref(texture);
    }
    void clear() { while (last) erase(last); }
    Entry *find(Key const &k) {
        for (auto e = first; e; e = e->next) if (e->key == k) {
            unlink(e); front(e); return e;
        }
        return nullptr;
    }
    void admit(std::size_t amount, bool new_entry) {
        for (;;) {
            bool fits;
            { std::lock_guard lock(account->mutex);
              fits = amount <= account->value.capacity - account->value.charged; }
            if (fits && (!new_entry || count < maximum_entries)) return;
            auto e = last;
            while (e && record(e->texture).metadata->is_pinned()) e = e->previous;
            if (!e) throw PreviewRenderLimit("Thumbnail pool exhausted by live allocations");
            erase(e);
        }
    }
    void insert(std::unique_ptr<Entry> e) {
        auto &r = record(e->texture);
        g_object_add_toggle_ref(G_OBJECT(e->texture), toggled, GUINT_TO_POINTER(r.token));
        front(e.release()); ++count;
        std::lock_guard lock(account->mutex); ++account->value.cache_entries;
    }
};

std::shared_ptr<ArtworkLibraryThumbnailPool> ArtworkLibraryThumbnailPool::create(
    GMainContext *context, std::size_t capacity)
{
    // Check the bootstrap envelope before allocating its bookkeeping.
    if (capacity < pool_overhead || capacity > hard_limit)
        throw PreviewRenderLimit("Thumbnail pool capacity outside bookkeeping/128 MiB bounds");
    static_assert(sizeof(State) + sizeof(Accounting) + sizeof(ArtworkLibraryThumbnailPool) < pool_overhead / 2);
    auto account = std::make_shared<Accounting>(capacity);
    auto pool = std::shared_ptr<ArtworkLibraryThumbnailPool>(new ArtworkLibraryThumbnailPool);
    pool->_state = std::make_shared<State>(context, std::move(account));
    return pool;
}
ArtworkLibraryThumbnailPool::~ArtworkLibraryThumbnailPool() = default;
ArtworkLibraryThumbnailPool::Stats ArtworkLibraryThumbnailPool::stats() const
{
    std::lock_guard lock(_state->account->mutex);
    return _state->account->value;
}
void ArtworkLibraryThumbnailPool::invalidate_all(ThumbnailInvalidation reason)
{
    auto s = _state; s->thread();
    if (s->closed) return;
    if (s->epoch == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("Thumbnail pool epoch exhausted");
    ++s->epoch;
    if (reason == ThumbnailInvalidation::Close) s->closed = true;
    s->clear(); // Dropping cache ownership cannot uncharge consumer ownership.
}

class ThumbnailAllocation {
public:
static ThumbnailResult render(std::shared_ptr<ArtworkLibraryThumbnailPool> pool,
                              IO::ArtworkLibrary::ValidatedSvg svg, ThumbnailSize size,
                              PreviewRenderBudget::Limits limits, std::function<bool()> cancelled)
{
    try {
        if (!pool) throw std::invalid_argument("Thumbnail requires the application pool");
        auto state = pool->_state; // Pin before the first caller callback.
        state->thread();
        NativeGate gate(state->account);
        size_ok(size);
        auto epoch = state->epoch;
        PreviewRenderBudget budget(limits, [state, epoch, cancelled = std::move(cancelled)] {
            if (state->closed || state->epoch != epoch) return true;
            auto requested = cancelled && cancelled();
            return requested || state->closed || state->epoch != epoch;
        });
        budget.checkpoint();
        auto k = key(svg, size, limits);
        if (auto found = state->find(k)) {
            // Pin before admission may evict entries. This is still the same
            // native texture/backing, not a newly allocated CPU pixel copy.
            TextureOwner raw(GDK_TEXTURE(g_object_ref(found->texture)));
            state->admit(add(result_overhead, handle_overhead), false);
            Reservation reservation(state->account, add(result_overhead, handle_overhead));
            budget.checkpoint();
            auto metadata = reservation.take(result_overhead);
            auto handle = reservation.take(handle_overhead);
            auto texture = charged_texture(std::move(raw), handle);
            auto image = std::allocate_shared<Thumbnail>(ResultAllocator<Thumbnail>{metadata});
            auto &r = record(texture->gobj());
            image->texture = std::move(texture); image->bytes = r.bytes;
            image->render_stats = r.render_stats;
            image->warnings = {r.warnings.get(), r.warning_count};
            budget.checkpoint();
            metadata->commit(true); handle->commit(true);
            return {std::move(image), ThumbnailFailure::None, ""};
        }
        auto w = multiply(size.width, size.device_scale), h = multiply(size.height, size.device_scale);
        auto stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, static_cast<int>(w));
        if (stride <= 0) throw PreviewRenderLimit("Thumbnail stride overflow");
        auto pixel_bytes = multiply(static_cast<std::size_t>(stride), h);
        // This temporary copy is admitted-input metadata, not retained output.
        // The output stores exact-sized immutable views/characters, never a
        // caller's arbitrary vector or string capacity.
        auto warnings = svg.warnings();
        std::size_t warning_bytes = 0;
        for (auto const &warning : warnings) warning_bytes = add(warning_bytes, add(warning.size(), 1));
        auto text_cost = add(warning_bytes, multiply(warnings.size(), sizeof(std::string_view)));
        auto backing_cost = add(pixel_bytes, backing_overhead);
        auto texture_cost = add(texture_overhead, text_cost);
        auto total = add(add(backing_cost, texture_cost), add(add(result_overhead, handle_overhead), entry_overhead));
        state->admit(total, true);
        Reservation reservation(state->account, total);
        auto notification_token = allocate_record_token(); // Never wrap/reuse, before native allocation.
        budget.checkpoint(); // Reservation visible; no output backing allocated.
        auto backing_charge = reservation.take(backing_cost);
        auto texture_charge = reservation.take(texture_cost);
        auto result_charge = reservation.take(result_overhead);
        auto handle_charge = reservation.take(handle_overhead);
        auto entry_charge = reservation.take(entry_overhead);
        auto bytes = svg.svg_bytes(); // Retained across native parsing.
        auto doc = SPDocument::createNewDocFromMem(std::span<char const>(bytes->data(), bytes->size()));
        if (!doc) throw std::runtime_error("Native thumbnail document could not be opened");
        budget.checkpoint();
        doc->ensureUpToDate();
        budget.checkpoint();
        // Reject degenerate filtered targets even when native Drawing would
        // cull them before reaching Filter::render. Use the native bbox engine,
        // never an independently reconstructed path/filter geometry.
        std::vector<SPObject *> todo{doc->getRoot()};
        std::size_t visited = 0;
        while (!todo.empty()) {
            auto object = todo.back(); todo.pop_back();
            budget.checkpoint();
            if (++visited > 100000) throw PreviewRenderLimit("Native thumbnail object limit");
            if (auto item = cast<SPItem>(object); item && item->isFiltered()) {
                auto box = item->bbox(Geom::identity(), SPItem::GEOMETRIC_BBOX);
                if (!box) throw PreviewRenderLimit("Filtered native item has no geometry");
                PreviewRenderBudget::rect(*box, true);
                PreviewRenderBudget::affine(item->i2doc_affine());
            }
            for (auto &child : object->children) todo.push_back(&child);
        }

        auto page_w = doc->getWidth().value("px"), page_h = doc->getHeight().value("px");
        if (!std::isfinite(page_w) || !std::isfinite(page_h) || page_w <= 0 || page_h <= 0)
            throw PreviewRenderLimit("Invalid native physical viewport");
        // Preserve the admitted root page and its authored overflow/clipping.
        // Fit without changing physical SVG dimensions or authored filterRes.
        double scale = std::min(w / page_w, h / page_h);
        Geom::Affine frame = Geom::Scale(scale) *
            Geom::Translate((w - page_w * scale) / 2, (h - page_h * scale) / 2);
        PreviewRenderBudget::affine(frame);
        Drawing drawing;
        budget.bind_drawing(&drawing);
        drawing.setCacheBudget(0);
        auto display_key = SPItem::display_key_new(1);
        auto root = doc->getRoot();
        auto shown = root->invoke_show(drawing, display_key, SP_ITEM_SHOW_DISPLAY);
        Shown hide{root, display_key};
        if (!shown) throw std::runtime_error("Native thumbnail drawing is unavailable");
        drawing.setRoot(shown);
        drawing.setExact(); // Native quality setters require the shown root.
        drawing.setDithering(false); // Fixed preview policy; this setter also requires the root.
        shown->setTransform(frame);
        // Update the full private tree, not only a clipped subset whose hidden
        // filter state would escape the post-native checks.
        drawing.update();
        budget.checkpoint();
        PreviewRenderBudget::surface(w, h);
        auto surface = Cairo::ImageSurface::create(Cairo::Surface::Format::ARGB32, w, h);
        backing_charge->allocated_pixels(pixel_bytes);
        if (surface->get_stride() != stride || surface->get_height() != static_cast<int>(h))
            throw PreviewRenderLimit("Native thumbnail allocation differs from reservation");
        {
            // Artwork previews are presented on an opaque white tile. Paint
            // the presentation background into the backing surface so it is
            // reliable even when the source SVG has transparent pixels and
            // the GTK Picture widget does not paint CSS below its texture.
            auto background = Cairo::Context::create(surface);
            background->set_source_rgb(1.0, 1.0, 1.0);
            background->paint();
            DrawingSurface target(surface->cobj(), Geom::Point(0, 0));
            DrawingContext dc(target);
            drawing.render(dc, Geom::IntRect::from_xywh(0, 0, w, h), DrawingItem::RENDER_BYPASS_CACHE);
        }
        surface->flush();
        if (cairo_surface_status(surface->cobj()) != CAIRO_STATUS_SUCCESS)
            throw std::runtime_error("Native thumbnail surface failed");
        budget.checkpoint();
        static_assert(sizeof(TextureRecord) + sizeof(Charge) * 2 < texture_overhead / 2);
        static_assert(sizeof(BackingOwner) + sizeof(Charge) < backing_overhead / 2);
        static_assert(sizeof(ArtworkLibraryThumbnailPool::State::Entry) + sizeof(Charge) < entry_overhead / 2);
        auto native_record = std::make_unique<TextureRecord>();
        native_record->metadata = texture_charge; native_record->backing = backing_charge;
        native_record->bytes = pixel_bytes; native_record->render_stats = budget.stats();
        native_record->warning_count = warnings.size();
        native_record->warning_bytes = std::make_unique<char[]>(warning_bytes);
        native_record->warnings = std::make_unique<std::string_view[]>(warnings.size());
        std::size_t offset = 0;
        for (std::size_t i = 0; i < warnings.size(); ++i) {
            auto n = warnings[i].size();
            std::memcpy(native_record->warning_bytes.get() + offset, warnings[i].c_str(), n + 1);
            native_record->warnings[i] = {native_record->warning_bytes.get() + offset, n};
            offset += n + 1;
        }
        auto owner = std::make_unique<BackingOwner>(backing_charge, surface->cobj());
        auto buffer = g_bytes_new_with_free_func(surface->get_data(), pixel_bytes,
            [](gpointer p) { delete static_cast<BackingOwner *>(p); }, owner.release());
        auto raw_texture = gdk_memory_texture_new(w, h, GDK_MEMORY_DEFAULT, buffer, stride);
        g_bytes_unref(buffer);
        if (!raw_texture) throw std::runtime_error("Native thumbnail texture failed");
        TextureOwner native_texture(raw_texture);
        texture_charge->allocated_texture();
        // GDK_MEMORY_DEFAULT is native 8-bit premultiplied Cairo layout (byte
        // alignment); GdkMemoryTexture retains the supplied GBytes, not a
        // converted backing. Never publish if a backend violates that path.
        if (!backing_charge->owns_bytes())
            throw PreviewRenderLimit("Native texture did not retain the charged GBytes");
        auto record_ptr = native_record.release();
        g_object_set_qdata_full(G_OBJECT(raw_texture), record_key(), record_ptr,
            [](gpointer p) { delete static_cast<TextureRecord *>(p); });
        auto texture = charged_texture(std::move(native_texture), handle_charge);
        auto image = std::allocate_shared<Thumbnail>(ResultAllocator<Thumbnail>{result_charge});
        image->bytes = pixel_bytes;
        image->texture = std::move(texture); // Zero-copy GBytes/Cairo backing.
        image->render_stats = budget.stats();
        image->warnings = {record_ptr->warnings.get(), record_ptr->warning_count};
        auto entry = std::make_unique<ArtworkLibraryThumbnailPool::State::Entry>(
            ArtworkLibraryThumbnailPool::State::Entry{k, raw_texture, entry_charge});
        register_record(*record_ptr, notification_token); // Charged intrusive links; no new allocation.
        budget.checkpoint(); // Actual allocation, before publication/lease commitment.
        backing_charge->commit(true); texture_charge->commit(true);
        result_charge->commit(true); handle_charge->commit(true); entry_charge->commit(false);
        state->insert(std::move(entry)); // No allocating container or caller callback.
        return {std::move(image), ThumbnailFailure::None, ""};
    } catch (PreviewRenderCancelled const &) {
        return {{}, ThumbnailFailure::Cancelled, "Thumbnail generation cancelled"};
    } catch (PreviewRenderLimit const &) {
        return {{}, ThumbnailFailure::Limits, "Thumbnail retained-pool or native-render limit exceeded"};
    } catch (std::exception const &) {
        return {{}, ThumbnailFailure::Native, "Native thumbnail operation failed"};
    }
}
};

ThumbnailResult render_library_thumbnail(std::shared_ptr<ArtworkLibraryThumbnailPool> pool,
                                         IO::ArtworkLibrary::ValidatedSvg svg, ThumbnailSize size,
                                         PreviewRenderBudget::Limits limits,
                                         std::function<bool()> cancelled)
{
    return ThumbnailAllocation::render(std::move(pool), std::move(svg), size, limits, std::move(cancelled));
}

struct ArtworkLibraryThumbnails::State : std::enable_shared_from_this<State> {
    explicit State(std::shared_ptr<ArtworkLibraryThumbnailPool> p, PreviewRenderBudget::Limits limits)
        : pool(std::move(p)), context(g_main_context_ref(pool->_state->context)),
          owner(std::this_thread::get_id()), render_limits(limits), pool_epoch(pool->_state->epoch) {}
    ~State() { stop_source(); g_main_context_unref(context); }
    std::shared_ptr<ArtworkLibraryThumbnailPool> pool;
    GMainContext *context;
    std::thread::id owner;
    PreviewRenderBudget::Limits render_limits;
    bool closed = false, dispatching = false;
    std::uint64_t generation = 0;
    std::uint64_t pool_epoch;
    GSource *source = nullptr;
    std::vector<ThumbnailDemand> visible;
    Ready ready;
    std::size_t next = 0;
    void thread() const {
        if (owner != std::this_thread::get_id()) throw std::logic_error("Thumbnail service owning-thread violation");
    }
    void stop_source() {
        if (auto s = std::exchange(source, nullptr)) { g_source_destroy(s); g_source_unref(s); }
    }
    void sync_pool() {
        if (pool_epoch == pool->_state->epoch) return;
        pool_epoch = pool->_state->epoch;
        advance(); stop_source(); visible.clear(); ready = {}; next = 0;
        if (pool->_state->closed) closed = true;
    }
    void advance() {
        if (generation == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("Thumbnail generation exhausted");
        ++generation;
    }
    void close() {
        closed = true; stop_source(); visible.clear(); ready = {};
    }
    void schedule() {
        sync_pool();
        if (closed || dispatching || source || next >= visible.size()) return;
        source = g_idle_source_new();
        g_source_set_priority(source, G_PRIORITY_DEFAULT_IDLE);
        auto weak = new std::weak_ptr<State>(shared_from_this());
        g_source_set_callback(source, [](gpointer p) -> gboolean {
            auto state = static_cast<std::weak_ptr<State> *>(p)->lock();
            if (state) state->tick();
            return G_SOURCE_REMOVE;
        }, weak, [](gpointer p) { delete static_cast<std::weak_ptr<State> *>(p); });
        g_source_attach(source, context);
    }
    void tick() noexcept {
        // Pin State before any callback. Never touch the service wrapper here.
        auto fired = std::exchange(source, nullptr);
        if (fired) g_source_unref(fired); // GLib retains the executing source.
        if (owner != std::this_thread::get_id()) { close(); return; }
        try { sync_pool(); } catch (...) { close(); return; }
        if (closed || dispatching || next >= visible.size()) return;
        dispatching = true;
        auto epoch = generation;
        auto finish = [this] { dispatching = false; schedule(); };
        try {
            auto index = next++;
            auto demand = visible[index]; // Immutable payload + value size pin.
            auto callback = ready;
            auto result = render_library_thumbnail(pool, demand.svg, demand.size, render_limits,
                [this, epoch] { return closed || generation != epoch; });
            sync_pool();
            if (!closed && generation == epoch && callback) callback(epoch, index, std::move(result));
        } catch (...) {
            // A caller callback must not unwind through the GLib C boundary.
            // Stop this demand generation rather than recursively notifying it.
            if (generation == epoch) { visible.clear(); ready = {}; next = 0; }
        }
        try { finish(); } catch (...) { close(); }
    }
};

ArtworkLibraryThumbnails::ArtworkLibraryThumbnails(std::shared_ptr<ArtworkLibraryThumbnailPool> pool,
                                                   PreviewRenderBudget::Limits limits)
{
    if (!pool) throw std::invalid_argument("Thumbnail service requires the application pool");
    pool->_state->thread();
    if (pool->_state->closed) throw std::logic_error("Thumbnail pool closed");
    // Validate policy immediately without performing any native work.
    { PreviewRenderBudget check(limits); }
    _state = std::make_shared<State>(std::move(pool), limits);
}
ArtworkLibraryThumbnails::~ArtworkLibraryThumbnails() { _state->close(); }
std::uint64_t ArtworkLibraryThumbnails::set_visible(std::vector<ThumbnailDemand> demand, Ready ready)
{
    auto s = _state; s->thread();
    s->sync_pool();
    if (s->closed) throw std::logic_error("Thumbnail service closed");
    if (demand.size() > 128) throw PreviewRenderLimit("Thumbnail visible-page count limit");
    std::size_t bytes = 0;
    std::vector<ThumbnailDemand> canonical;
    canonical.reserve(demand.size());
    for (auto const &d : demand) {
        size_ok(d.size);
        auto n = d.svg.svg_bytes()->size();
        if (n > 64u * 1024 * 1024 - bytes) throw PreviewRenderLimit("Thumbnail visible-page byte limit");
        bytes += n;
        canonical.push_back(d); // Do not retain an oversized caller vector capacity.
    }
    s->advance(); s->stop_source();
    s->visible = std::move(canonical); s->ready = std::move(ready); s->next = 0;
    s->schedule(); return s->generation;
}
void ArtworkLibraryThumbnails::invalidate(ThumbnailInvalidation reason)
{
    auto s = _state; s->thread();
    if (s->closed) return;
    s->advance(); s->stop_source(); s->visible.clear(); s->ready = {}; s->next = 0;
    if (reason == ThumbnailInvalidation::Close) s->close();
    if (reason == ThumbnailInvalidation::FontsChanged) {
        s->pool->invalidate_all(reason);
        s->pool_epoch = s->pool->_state->epoch;
    }
}
std::uint64_t ArtworkLibraryThumbnails::generation() const { auto s = _state; s->thread(); s->sync_pool(); return s->generation; }
std::size_t ArtworkLibraryThumbnails::cache_bytes() const { auto s = _state; s->thread(); return s->pool->stats().charged; }
std::size_t ArtworkLibraryThumbnails::pending_sources() const { auto s = _state; s->thread(); s->sync_pool(); return bool(s->source); }
} // namespace Inkscape::UI::Cache
