// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_UI_BITMAP_ADJUSTMENTS_CONTROLLER_H
#define INKSCAPE_UI_BITMAP_ADJUSTMENTS_CONTROLLER_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <sigc++/scoped_connection.h>

#include "display/bitmap-tone.h"

class SPDesktop;
class SPImage;
class SPItem;

namespace Inkscape::UI {

enum class BitmapPreviewMode { Before, Preview };

struct BitmapAdjustmentSnapshot {
    bool has_selection = false;
    bool has_targets = false;
    std::size_t selected_count = 0;
    std::size_t target_count = 0;
    std::size_t ignored_count = 0; // Excludes descendants/clones already covered.
    std::size_t unavailable_count = 0;
    std::size_t missing_source_count = 0;

    // Histogram and source metadata are deliberately available only when the
    // complete selection consists of one usable bitmap.
    bool has_single_bitmap = false;
    bool missing = false;
    bool embedded = false;
    unsigned pixel_width = 0;
    unsigned pixel_height = 0;

    std::array<Filters::BitmapToneAggregateValue,
               Filters::BITMAP_TONE_PROPERTY_COUNT> tone;
};

/** Per-desktop tone client of the shared, non-persistent preview composer. */
class BitmapAdjustmentsController final {
public:
    explicit BitmapAdjustmentsController(SPDesktop *desktop);
    ~BitmapAdjustmentsController();

    BitmapAdjustmentSnapshot query() const;
    SPImage *targetImage() const;

    bool preview(Filters::BitmapToneSettings const &settings);
    bool previewPatch(Filters::BitmapTonePatch const &patch);
    bool setPreviewMode(BitmapPreviewMode mode);
    bool setClippingWarning(bool enabled);
    bool commit(Filters::BitmapToneSettings const &settings, bool continuous = false);
    bool commitPatch(Filters::BitmapTonePatch const &patch, bool continuous = false);
    void cancelPreview() noexcept;

    BitmapPreviewMode previewMode() const { return _mode; }
    bool clippingWarning() const { return _clipping_warning; }
    bool previewActive() const { return _visible; }
    std::size_t targetCount() const { return _targets.size(); }

private:
    struct ToneTarget;

    std::vector<SPItem *> collectTargets(std::size_t *selected_count = nullptr,
                                         std::size_t *unavailable_count = nullptr,
                                         std::size_t *missing_count = nullptr) const;
    bool captureTargets();
    bool schedulePublish();
    bool publish(uint64_t generation);
    void restoreCanonical() noexcept;
    void clearTargets() noexcept;
    void reconnectSelection();

    SPDesktop *_desktop = nullptr;
    std::vector<std::unique_ptr<ToneTarget>> _targets;
    Filters::BitmapTonePatch _pending_patch;
    BitmapPreviewMode _mode = BitmapPreviewMode::Preview;
    bool _clipping_warning = false;
    bool _visible = false;
    bool _committing = false;
    uint64_t _generation = 0;

    sigc::scoped_connection _publish_idle;
    sigc::scoped_connection _selection_changed;
    sigc::scoped_connection _selection_modified;
    sigc::scoped_connection _tool_changed;
    sigc::scoped_connection _document_replaced;
};

} // namespace Inkscape::UI

#endif // INKSCAPE_UI_BITMAP_ADJUSTMENTS_CONTROLLER_H
