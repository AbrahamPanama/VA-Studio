// SPDX-License-Identifier: GPL-2.0-or-later

#include "bitmap-adjustments-controller.h"
#include "ui/bitmap-preview-composer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>
#include <ranges>
#include <utility>

#include <glibmm/i18n.h>
#include <glibmm/main.h>

#include "bitmap-adjustment-chemistry.h"
#include "desktop.h"
#include "display/drawing-item.h"
#include "display/nr-filter.h"
#include "document.h"
#include "document-undo.h"
#include "object/sp-filter.h"
#include "object/sp-image.h"
#include "object/sp-item.h"
#include "object/sp-use.h"
#include "object/weakptr.h"
#include "selection.h"
#include "style.h"
#include "ui/icon-names.h"
#include "util/operation-targets.h"

namespace Inkscape::UI {
namespace {

char const *undo_key(Filters::BitmapTonePatch const &patch)
{
    unsigned count = 0;
    char const *key = "bitmap-adjustments:tone:all";
    auto visit = [&](std::optional<double> const &value, char const *candidate) {
        if (value) {
            ++count;
            key = candidate;
        }
    };
    visit(patch.brightness, "bitmap-adjustments:tone:brightness");
    visit(patch.contrast, "bitmap-adjustments:tone:contrast");
    visit(patch.intensity, "bitmap-adjustments:tone:intensity");
    visit(patch.highlights, "bitmap-adjustments:tone:highlights");
    visit(patch.shadows, "bitmap-adjustments:tone:shadows");
    visit(patch.midtones, "bitmap-adjustments:tone:midtones");
    return count == 1 ? key : "bitmap-adjustments:tone:all";
}

} // namespace

using BitmapAdjustments::usable_bitmap;
using BitmapAdjustments::is_embedded;
using BitmapAdjustments::canonical_tone;

struct BitmapAdjustmentsController::ToneTarget {
    explicit ToneTarget(SPItem *target)
        : item(target)
        , baseline(canonical_tone(target))
        , requested(baseline)
    {}

    SPWeakPtr<SPItem> item;
    Filters::BitmapToneSettings baseline;
    Filters::BitmapToneSettings requested;
    Bitmap::ClientLease preview;
    sigc::scoped_connection released;
    sigc::scoped_connection modified;
};

BitmapAdjustmentsController::BitmapAdjustmentsController(SPDesktop *desktop)
    : _desktop(desktop)
{
    if (!_desktop) return;
    _tool_changed = _desktop->connectEventContextChanged([this](auto, auto) {
        if (!_committing) cancelPreview();
    });
    _document_replaced = _desktop->connectDocumentReplaced([this](auto, auto) {
        if (!_committing) cancelPreview();
        reconnectSelection();
    });
    reconnectSelection();
}

BitmapAdjustmentsController::~BitmapAdjustmentsController()
{
    cancelPreview();
    _selection_changed.disconnect();
    _selection_modified.disconnect();
    _tool_changed.disconnect();
    _document_replaced.disconnect();
    _desktop = nullptr;
}

void BitmapAdjustmentsController::reconnectSelection()
{
    _selection_changed.disconnect();
    _selection_modified.disconnect();
    if (!_desktop || !_desktop->getSelection()) return;
    _selection_changed = _desktop->getSelection()->connectChanged([this](auto *) {
        if (!_committing) cancelPreview();
    });
    _selection_modified = _desktop->getSelection()->connectModified([this](auto *, auto) {
        if (!_committing) cancelPreview();
    });
}

std::vector<SPItem *> BitmapAdjustmentsController::collectTargets(
    std::size_t *selected_count, std::size_t *unavailable_count,
    std::size_t *missing_count) const
{
    std::vector<SPItem *> selected;
    if (_desktop && _desktop->getSelection()) {
        for (auto item : _desktop->getSelection()->items()) selected.push_back(item);
    }
    if (selected_count) *selected_count = selected.size();
    auto resolved = BitmapAdjustments::resolve_tone_targets(selected, [this](SPItem *item) {
        return _desktop && item->isVisibleAndUnlocked(_desktop->dkey) &&
               item->get_arenaitem(_desktop->dkey);
    });
    if (unavailable_count) *unavailable_count = resolved.unavailable;
    if (missing_count) *missing_count = resolved.missing_sources;
    return std::move(resolved.items);
}

SPImage *BitmapAdjustmentsController::targetImage() const
{
    if (!_targets.empty()) {
        if (_targets.size() != 1) return nullptr;
        auto image = cast<SPImage>(_targets.front()->item.get());
        return usable_bitmap(image) ? image : nullptr;
    }
    std::size_t selected_count = 0;
    auto targets = collectTargets(&selected_count);
    if (selected_count != 1 || targets.size() != 1) return nullptr;
    auto image = cast<SPImage>(targets.front());
    return usable_bitmap(image) ? image : nullptr;
}

BitmapAdjustmentSnapshot BitmapAdjustmentsController::query() const
{
    BitmapAdjustmentSnapshot result;
    auto targets = collectTargets(&result.selected_count, &result.unavailable_count,
                                  &result.missing_source_count);
    result.has_selection = result.selected_count != 0;
    result.target_count = targets.size();
    result.ignored_count = result.unavailable_count + result.missing_source_count;
    result.has_targets = !targets.empty();

    if (result.selected_count == 1 && _desktop && _desktop->getSelection()) {
        auto selected = _desktop->getSelection()->items();
        auto it = selected.begin();
        if (it != selected.end()) {
            if (auto image = cast<SPImage>(*it)) result.missing = !usable_bitmap(image);
        }
    }

    if (result.selected_count == 1 && targets.size() == 1) {
        if (auto image = cast<SPImage>(targets.front()); usable_bitmap(image)) {
            result.has_single_bitmap = true;
            result.embedded = is_embedded(image);
            result.pixel_width = image->pixbuf->width();
            result.pixel_height = image->pixbuf->height();
        }
    }

    if (targets.empty()) return result;
    result.tone = BitmapAdjustments::aggregate_tone(targets);
    return result;
}

void BitmapAdjustmentsController::clearTargets() noexcept
{
    _publish_idle.disconnect();
    ++_generation;
    _targets.clear();
    _pending_patch = {};
}

bool BitmapAdjustmentsController::captureTargets()
{
    if (!_targets.empty()) return true;
    auto items = collectTargets();
    if (items.empty()) return false;

    _targets.reserve(items.size());
    for (auto item : items) {
        auto target = std::make_unique<ToneTarget>(item);
        target->preview = Bitmap::contribute({item, _desktop->dkey}, {});
        if (!target->preview) { clearTargets(); return false; }
        target->released = item->connectRelease([this](auto *) {
            if (!_committing) cancelPreview();
        });
        target->modified = item->connectModified([this](auto *, auto) {
            if (!_committing) cancelPreview();
        });
        _targets.emplace_back(std::move(target));
    }
    return true;
}

bool BitmapAdjustmentsController::schedulePublish()
{
    if (_targets.empty()) return false;
    _publish_idle.disconnect();
    auto const generation = ++_generation;
    _publish_idle = Glib::signal_idle().connect([this, generation] {
        publish(generation);
        return false;
    }, Glib::PRIORITY_HIGH_IDLE);
    return true;
}

bool BitmapAdjustmentsController::publish(uint64_t generation)
{
    if (!_desktop || generation != _generation || _targets.empty()) return false;

    for (auto const &target : _targets) {
        Bitmap::Contribution contribution;
        if (_mode == BitmapPreviewMode::Preview) contribution.tone = target->requested;
        contribution.clipping_warning = _clipping_warning;
        if (target->preview.prepare(std::move(contribution), generation).status != Bitmap::Status::unchanged) {
            cancelPreview();
            return false;
        }
    }
    if (generation != _generation) return false;
    for (auto const &target : _targets) {
        if (Bitmap::update(target->preview, generation).status != Bitmap::Status::changed) {
            cancelPreview();
            return false;
        }
    }
    _visible = true;
    return true;
}

bool BitmapAdjustmentsController::preview(Filters::BitmapToneSettings const &settings)
{
    return previewPatch(Filters::BitmapTonePatch::all(settings));
}

bool BitmapAdjustmentsController::previewPatch(Filters::BitmapTonePatch const &patch)
{
    if (patch.empty() || !captureTargets()) return false;
    _pending_patch = patch;
    for (auto &target : _targets) {
        target->requested = Filters::apply_bitmap_tone_patch(target->baseline, patch);
    }
    _mode = BitmapPreviewMode::Preview;
    return schedulePublish();
}

bool BitmapAdjustmentsController::setPreviewMode(BitmapPreviewMode mode)
{
    if (!captureTargets()) return false;
    _mode = mode;
    return schedulePublish();
}

bool BitmapAdjustmentsController::setClippingWarning(bool enabled)
{
    if (!captureTargets()) return false;
    _clipping_warning = enabled;
    return schedulePublish();
}

void BitmapAdjustmentsController::restoreCanonical() noexcept
{
    // Releasing tone cannot overwrite another panel's source-only preview.
    for (auto const &target : _targets) target->preview.reset();
}

bool BitmapAdjustmentsController::commit(Filters::BitmapToneSettings const &settings,
                                         bool continuous)
{
    return commitPatch(Filters::BitmapTonePatch::all(settings), continuous);
}

bool BitmapAdjustmentsController::commitPatch(Filters::BitmapTonePatch const &patch,
                                              bool continuous)
{
    if (patch.empty() || !captureTargets()) return false;
    auto document = _desktop ? _desktop->getDocument() : nullptr;
    if (!document) return false;

    std::vector<ToneTarget *> changed;
    for (auto &target : _targets) {
        auto item = target->item.get();
        if (!item || item->document != document || !item->getRepr()) {
            cancelPreview();
            return false;
        }
        target->baseline = canonical_tone(item);
        target->requested = Filters::apply_bitmap_tone_patch(target->baseline, patch);
        if (!Filters::bitmap_tone_settings_equal(target->baseline, target->requested)) {
            changed.push_back(target.get());
        }
    }
    if (changed.empty()) {
        cancelPreview();
        return false;
    }

    _publish_idle.disconnect();
    ++_generation;
    _committing = true;
    if (_mode == BitmapPreviewMode::Preview && !_visible) publish(_generation);

    std::vector<ToneTarget *> applied;
    bool succeeded = true;
    for (auto target : changed) {
        if (!BitmapAdjustments::apply_tone(target->item.get(), target->requested)) {
            succeeded = false;
            break;
        }
        applied.push_back(target);
    }
    if (!succeeded) {
        for (auto target : applied) {
            BitmapAdjustments::apply_tone(target->item.get(), target->baseline);
        }
        document->ensureUpToDate();
        restoreCanonical();
        _committing = false;
        clearTargets();
        _visible = false;
        return false;
    }

    if (continuous) {
        DocumentUndo::maybeDone(document, undo_key(patch), RC_("Undo", "Adjust object tone"),
                                INKSCAPE_ICON("shape-image"));
    } else {
        DocumentUndo::done(document, RC_("Undo", "Adjust object tone"),
                           INKSCAPE_ICON("shape-image"));
    }
    document->ensureUpToDate();
    restoreCanonical();
    _visible = false;
    _committing = false;
    clearTargets();
    _mode = BitmapPreviewMode::Preview;
    _clipping_warning = false;
    return true;
}

void BitmapAdjustmentsController::cancelPreview() noexcept
{
    if (_visible) restoreCanonical();
    _visible = false;
    clearTargets();
    _mode = BitmapPreviewMode::Preview;
    _clipping_warning = false;
}

} // namespace Inkscape::UI
