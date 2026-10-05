// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_OBJECT_TRANSFORM_POLICY_SCOPE_H
#define INKSCAPE_OBJECT_TRANSFORM_POLICY_SCOPE_H
namespace Inkscape {
// Shipped transform defaults. Stroke baking also scales dashes natively.
struct TransformPolicy {
    bool stroke = true;
    bool rectcorners = true;
    bool pattern = true;
    bool gradient = true;
    bool hatch = true;
    bool preserve = false;
};
// Request-local, same-thread lifetime; no preference writes. Nested scopes restore.
class ScopedTransformPolicy final {
public:
    explicit ScopedTransformPolicy(TransformPolicy policy = {});
    ~ScopedTransformPolicy();
    ScopedTransformPolicy(ScopedTransformPolicy const &) = delete;
    ScopedTransformPolicy &operator=(ScopedTransformPolicy const &) = delete;
    static TransformPolicy const *active() noexcept;
private:
    TransformPolicy const _policy;
    TransformPolicy const *_previous;
};
}
#endif
