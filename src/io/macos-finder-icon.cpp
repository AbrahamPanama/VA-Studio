// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * VIEW-1: Finder custom icon for SVG files saved by VA Studio (macOS only).
 * Calls NSWorkspace through the Objective-C runtime so no Objective-C++ source
 * is needed in the build.
 *
 * Copyright 2026 VA Studio authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "macos-finder-icon.h"

#include <glib.h>

#include <condition_variable>
#include <cstring>
#include <deque>
#include <dlfcn.h>
#include <fcntl.h>
#include <mutex>
#include <objc/message.h>
#include <objc/objc.h>
#include <objc/runtime.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <thread>
#include <unistd.h>

namespace Inkscape::IO {

namespace {

id send(id receiver, char const *selector)
{
    return reinterpret_cast<id (*)(id, SEL)>(objc_msgSend)(receiver, sel_registerName(selector));
}

id class_object(char const *name)
{
    return reinterpret_cast<id>(objc_getClass(name));
}

bool same_file(struct stat const &a, struct stat const &b)
{
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino;
}

bool matches(struct stat const &file, FinderIconTarget const &target)
{
    return static_cast<std::uint64_t>(file.st_dev) == target.device &&
           static_cast<std::uint64_t>(file.st_ino) == target.inode && file.st_size == target.size &&
           file.st_mtimespec.tv_sec == target.modified_seconds &&
           static_cast<std::uint32_t>(file.st_mtimespec.tv_nsec / 1000) == target.modified_microseconds;
}

// FAT and exFAT keep Mac attributes in visible "._" files.
bool native_attributes(int fd)
{
    struct statfs volume {};
    if (fstatfs(fd, &volume) != 0) {
        return false;
    }
    return std::strcmp(volume.f_fstypename, "msdos") != 0 && std::strcmp(volume.f_fstypename, "exfat") != 0;
}

bool apply_icon(std::string const &path, std::span<unsigned char const> png)
{
    // AppKit is loaded by GTK in the application; a test process may need it.
    if (!objc_getClass("NSWorkspace")) {
        dlopen("/System/Library/Frameworks/AppKit.framework/AppKit", RTLD_LAZY | RTLD_GLOBAL);
    }
    id const workspace_class = class_object("NSWorkspace");
    id const image_class = class_object("NSImage");
    id const data_class = class_object("NSData");
    id const string_class = class_object("NSString");
    id const pool_class = class_object("NSAutoreleasePool");
    if (!workspace_class || !image_class || !data_class || !string_class || !pool_class) {
        return false;
    }
    id pool = send(pool_class, "new");
    id data = reinterpret_cast<id (*)(id, SEL, void const *, unsigned long)>(objc_msgSend)(
        data_class, sel_registerName("dataWithBytes:length:"), png.data(), png.size());
    id image = data ? reinterpret_cast<id (*)(id, SEL, id)>(objc_msgSend)(send(image_class, "alloc"),
                                                                          sel_registerName("initWithData:"), data)
                    : nullptr;
    id file = reinterpret_cast<id (*)(id, SEL, char const *)>(objc_msgSend)(
        string_class, sel_registerName("stringWithUTF8String:"), path.c_str());
    id workspace = send(workspace_class, "sharedWorkspace");
    BOOL set = NO;
    if (image && file && workspace) {
        set = reinterpret_cast<BOOL (*)(id, SEL, id, id, unsigned long)>(objc_msgSend)(
            workspace, sel_registerName("setIcon:forFile:options:"), image, file, 0UL);
    }
    if (image) {
        send(image, "release");
    }
    send(pool, "drain");
    return set;
}

struct Request {
    std::string path;
    std::vector<unsigned char> png;
    FinderIconTarget target;
    std::function<void(bool)> done;
};

class Worker {
public:
    static Worker &instance()
    {
        static auto *worker = new Worker; // lives for the process; never joined
        return *worker;
    }

    void push(Request request)
    {
        std::lock_guard lock(_mutex);
        _queue.push_back(std::move(request));
        if (!_started) {
            _started = true;
            std::thread([this] { run(); }).detach();
        }
        _wake.notify_one();
    }

private:
    void run()
    {
        for (;;) {
            Request request;
            {
                std::unique_lock lock(_mutex);
                _wake.wait(lock, [this] { return !_queue.empty(); });
                request = std::move(_queue.front());
                _queue.pop_front();
            }
            bool const set = set_finder_icon(request.path, request.png, &request.target);
            auto *reply = new std::function<void()>([done = std::move(request.done), set] {
                if (done) done(set);
            });
            g_idle_add_full(
                G_PRIORITY_DEFAULT,
                [](gpointer data) -> gboolean {
                    (*static_cast<std::function<void()> *>(data))();
                    return G_SOURCE_REMOVE;
                },
                reply, [](gpointer data) { delete static_cast<std::function<void()> *>(data); });
        }
    }

    std::mutex _mutex;
    std::condition_variable _wake;
    std::deque<Request> _queue;
    bool _started = false;
};

} // namespace

bool set_finder_icon(std::string const &path, std::span<unsigned char const> png, FinderIconTarget const *target)
{
    if (path.empty() || png.empty()) {
        return false;
    }
    int const fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    struct stat before {};
    bool set = false;
    if (fstat(fd, &before) == 0 && S_ISREG(before.st_mode) && (!target || matches(before, *target)) &&
        native_attributes(fd)) {
        set = apply_icon(path, png);
        // Keep the save time on this same file, and only while the path still
        // names it: a file saved over it meanwhile keeps its own time.
        struct stat now {};
        if (stat(path.c_str(), &now) == 0 && same_file(now, before)) {
            struct timespec const times[2] = {before.st_atimespec, before.st_mtimespec};
            if (futimens(fd, times) != 0) {
                g_warning("Finder icon: could not restore the modification time of %s", path.c_str());
            }
        }
    }
    close(fd);
    return set;
}

void set_finder_icon_async(std::string path, std::vector<unsigned char> png, FinderIconTarget target,
                           std::function<void(bool)> done)
{
    Worker::instance().push({std::move(path), std::move(png), target, std::move(done)});
}

void clear_finder_icon(int fd)
{
    fremovexattr(fd, XATTR_RESOURCEFORK_NAME, 0);
    unsigned char info[32] = {};
    if (fgetxattr(fd, XATTR_FINDERINFO_NAME, info, sizeof(info), 0, 0) != sizeof(info)) {
        return;
    }
    if (!(info[8] & 0x04)) {
        return; // no custom icon flag (kHasCustomIcon, 0x0400 in the big-endian flags)
    }
    info[8] &= ~0x04;
    unsigned char const empty[32] = {};
    if (std::memcmp(info, empty, sizeof(info)) == 0) {
        fremovexattr(fd, XATTR_FINDERINFO_NAME, 0);
    } else {
        fsetxattr(fd, XATTR_FINDERINFO_NAME, info, sizeof(info), 0, 0);
    }
}

} // namespace Inkscape::IO
