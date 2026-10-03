// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_UTIL_CRASH_HANDLER_THREAD_H
#define INKSCAPE_UTIL_CRASH_HANDLER_THREAD_H
#include <thread>
namespace Inkscape::Util {
inline bool crash_handler_on_main_thread(std::thread::id main_thread) noexcept
{
    return std::this_thread::get_id() == main_thread;
}
}
#endif
