// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_LPE_POWERCLIP_H
#define INKSCAPE_LPE_POWERCLIP_H

/*
 * Inkscape::LPEPowerClip
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include <optional>

#include "live_effects/effect.h"
#include "live_effects/parameter/message.h"

namespace Inkscape {
namespace LivePathEffect {

class LPEPowerClip : public Effect {
public:
    LPEPowerClip(LivePathEffectObject *lpeobject);
    ~LPEPowerClip() override;
    void doBeforeEffect (SPLPEItem const* lpeitem) override;
    Geom::PathVector doEffect_path (Geom::PathVector const & path_in) override;
    void doOnRemove(SPLPEItem const* /*lpeitem*/) override;
    // Request-local removal; bypasses the GUI onungroup preference.
    void removeFrom(SPLPEItem *item);
    void doOnVisibilityToggled(SPLPEItem const* lpeitem) override;
    Glib::ustring getId();
    void add();
    void upd();
    void del();
    Geom::PathVector getClipPathvector();

  private:
    BoolParam inverse;
    BoolParam flatten;
    BoolParam hide_clip;
    MessageParam message;
    bool _updating;
    bool _legacy;
    std::optional<bool> _request_onungroup;
};

void sp_remove_powerclip(Inkscape::Selection *sel);
void sp_inverse_powerclip(Inkscape::Selection *sel);

// Document-only overloads; caller validates the supported domain and settles.
void sp_inverse_powerclip(SPDocument *document, SPLPEItem *item);
void sp_remove_powerclip(SPLPEItem *item);

} //namespace LivePathEffect
} //namespace Inkscape
#endif
