// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Gio::Actions for selection tied to the application and without GUI.
 *
 * Copyright (C) 2023 Martin owens
 *
 * The contents of this file may be used under the GNU General Public License Version 2 or later.
 *
 */

#include "actions-helper-gui.h"

#include <iostream>

#include "actions/vacards-cli-result.h"
#include "document.h"
#include "inkscape-window.h"
#include "inkscape-application.h"

/**
 * This activates all actions, which are called even if you don't know or care about which action group it's in.
 */
void activate_any_actions(action_vector_t const &actions, Glib::RefPtr<Gio::Application> app, InkscapeWindow *win, SPDocument *doc)
{
    Inkscape::VACardsCli::begin_chain();
    Inkscape::VACardsCli::CommandLineChainScope const chain_scope;
    for (std::size_t i = 0; i < actions.size(); ++i) {
        auto const &[name, param] = actions[i];
        if (Inkscape::VACardsCli::halt_pending()) {
            for (std::size_t j = i; j < actions.size(); ++j) {
                auto const &skipped = actions[j];
                std::string text;
                if (skipped.second.gobj()) {
                    text = skipped.second.get_type_string() == "s"
                         ? Glib::VariantBase::cast_dynamic<Glib::Variant<Glib::ustring>>(skipped.second).get().raw()
                         : skipped.second.print().raw();
                }
                Inkscape::VACardsCli::emit(Inkscape::VACardsCli::make_skipped_record(skipped.first, text));
            }
            break;
        }
        Gio::ActionGroup *group = nullptr;
        if (app->has_action(name)) {
            group = app.operator->();
        } else if (win && win->has_action(name)) {
            group = win;
        } else if (doc && doc->getActionGroup()->has_action(name)) {
            group = doc->getActionGroup().operator->();
        }
        if (!group) {
            std::cerr << "ActionsHelper::activate_actions: Unknown action name: " << name << std::endl;
            Inkscape::VACardsCli::note_rejection();
            continue;
        }
        if (!group->get_action_enabled(name)) {
            std::cerr << "ActionsHelper::activate_actions: Action '" << name
                      << "' is disabled for the current document or selection; it was not run." << std::endl;
            Inkscape::VACardsCli::note_rejection();
            continue;
        }
        group->activate_action(name, param);
    }
}

/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0)(case-label . +))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4 :
