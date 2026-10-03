// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Auto-save
 *
 * Copyright (C) 2020 Tavmjong Bah
 *
 * Re-write of code formerly in inkscape.cpp and originally written by Jon Cruz and others.
 *
 * The contents of this file may be used under the GNU General Public License Version 2 or later.
 *
 */

#ifndef INKSCAPE_AUTOSAVE_H
#define INKSCAPE_AUTOSAVE_H

#include <cstdint>
#include <map>
#include <string>
#include <vector>

class InkscapeApplication;

namespace Inkscape {
namespace IO {
namespace DocumentTransaction {
class SystemCalls;
}
}

class AutoSave final {
private:
    AutoSave() = default;

public:
    AutoSave(const AutoSave &) = delete;
    AutoSave &operator=(const AutoSave &) = delete;
    AutoSave(AutoSave &&) = delete;
    AutoSave &operator=(AutoSave &&) = delete;

    static AutoSave &getInstance()
    {
        static AutoSave theInstance;
        return theInstance;
    }

    static void restart();
    void init(InkscapeApplication *app);
    void start(); // Includes restarting.
    bool save();

    // Production/test dependency-injection overload. save() builds the
    // compile-time platform system calls and forwards here; tests supply a
    // fault-injecting subclass. Same timer bool contract as save().
    bool save(IO::DocumentTransaction::SystemCalls &calls);

private:
    InkscapeApplication* _app = nullptr;

    // Re-entry guard for the whole real tick. RAII scope resets it on every
    // return/exception.
    bool _saving = false;
    // One lazily generated GLib session token for recovery basenames.
    std::string _session;
    // Global per-instance monotonic attempt generation (uint64, never wraps).
    std::uint64_t _generation = 0;
    // Recovery files THIS session confirmed Published, oldest first, per
    // document serial. Only these are ever pruned (/options/autosave/max);
    // other sessions' and legacy files are never touched.
    std::map<unsigned long, std::vector<std::string>> _published;
};

} // namespace Inkscape

#endif // INKSCAPE_AUTOSAVE_H

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
