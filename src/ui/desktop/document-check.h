// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Check for data loss when closing a document window.
 *
 * Copyright (C) 2021 Tavmjong Bah
 *
 * The contents of this file may be used under the GNU General Public License Version 2 or later.
 *
 */

#ifndef DOCUMENT_CHECK_H

class SPDesktop;
class SPDocument;
namespace Gtk { class Window; }

bool document_check_for_data_loss(SPDesktop *desktop);
bool document_check_save_for_close(Gtk::Window &window, SPDocument *document);
void set_document_check_context_for_testing(SPDocument *document, Gtk::Window *window);
void set_document_check_response_for_testing(int response);
unsigned document_check_prompt_count_for_testing();
void reset_document_check_prompt_count_for_testing();

#endif // DOCUMENT_CHECK_H

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
