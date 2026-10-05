// SPDX-License-Identifier: GPL-2.0-or-later
#include "vacards-cli-transport.h"
#include <algorithm>
#include <cerrno>
#include <condition_variable>
#include <deque>
#include <mutex>
#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <poll.h>
#include <unistd.h>
#endif
namespace Inkscape::VACardsCli {
struct BoundedFdWriter::SharedState {
    static constexpr std::size_t byte_limit = 16u * 1024u * 1024u;
    explicit SharedState(int output_fd) : fd(output_fd) {}
    int fd;
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<std::string> queue;
    std::size_t queued_bytes = 0;
    bool accepting = true, closing = false, failed = false, finished = false;
};

BoundedFdWriter::BoundedFdWriter(int fd) : state(std::make_shared<SharedState>(fd)) {
    auto shared = state;
    worker = std::thread([shared] {
        for (;;) {
            std::string line;
            {
                std::unique_lock lock(shared->mutex);
                shared->changed.wait(lock, [&] { return shared->closing || !shared->queue.empty(); });
                if (shared->queue.empty()) {
                    shared->finished = true;
                    shared->changed.notify_all();
                    return;
                }
                line = std::move(shared->queue.front());
            }
            std::size_t offset = 0;
            while (offset < line.size()) {
#ifdef _WIN32
                auto result = _write(shared->fd, line.data() + offset,
                                     static_cast<unsigned>(line.size() - offset));
#else
                auto result = ::write(shared->fd, line.data() + offset, line.size() - offset);
#endif
                if (result < 0 && errno == EINTR) continue;
                if (result <= 0) {
                    std::lock_guard lock(shared->mutex);
                    shared->failed = true;
                    shared->accepting = false;
                    shared->closing = true;
                    shared->queue.clear();
                    shared->queued_bytes = 0;
                    shared->finished = true;
                    shared->changed.notify_all();
                    return;
                }
                offset += static_cast<std::size_t>(result);
            }
            {
                std::lock_guard lock(shared->mutex);
                shared->queued_bytes -= line.size();
                shared->queue.pop_front();
                shared->changed.notify_all();
            }
        }
    });
}

BoundedFdWriter::~BoundedFdWriter() {
    close_and_flush(std::chrono::seconds(10));
}

bool BoundedFdWriter::push_line(std::string_view line) {
    std::lock_guard lock(state->mutex);
    if (!state->accepting || state->failed) return false;
    if (line.size() > SharedState::byte_limit - state->queued_bytes) {
        state->failed = true;
        state->accepting = false;
        state->closing = true;
        state->changed.notify_all();
        return false;
    }
    try {
        state->queue.emplace_back(line);
        state->queued_bytes += line.size();
    } catch (...) {
        state->failed = true;
        state->accepting = false;
        state->closing = true;
        state->changed.notify_all();
        return false;
    }
    state->changed.notify_one();
    return true;
}

void BoundedFdWriter::close_and_flush(std::chrono::milliseconds bound) {
    if (!worker.joinable()) return;
    {
        std::lock_guard lock(state->mutex);
        state->accepting = false;
        state->closing = true;
        state->changed.notify_all();
    }
    bool finished;
    {
        std::unique_lock lock(state->mutex);
        finished = state->changed.wait_for(lock, bound, [&] { return state->finished; });
    }
    if (finished) worker.join();
    else worker.detach(); // process exit; a blocked pipe write cannot be cancelled portably
}

ByteReader stream_reader(std::istream &s) {
    return [&s](char *p, std::size_t n) {
        s.read(p, n);
        if (s.bad()) return -1;
        return static_cast<int>(s.gcount());
    };
}
ByteReader descriptor_reader(int fd) {
    return [fd](char *p, std::size_t n) {
#ifdef _WIN32
        auto handle = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
        if (GetFileType(handle) == FILE_TYPE_PIPE) {
            DWORD available = 0;
            if (!PeekNamedPipe(handle, nullptr, 0, nullptr, &available, nullptr))
                return GetLastError() == ERROR_BROKEN_PIPE ? 0 : -1;
            if (!available) { Sleep(50); return -2; }
            n = std::min(n, static_cast<std::size_t>(available));
        } else if (GetFileType(handle) == FILE_TYPE_CHAR) {
            if (WaitForSingleObject(handle, 50) == WAIT_TIMEOUT) return -2;
        }
        auto result = _read(fd, p, static_cast<unsigned>(n));
#else
        pollfd poller{fd, POLLIN, 0};
        auto ready = poll(&poller, 1, 50);
        if (ready == 0 || (ready < 0 && errno == EINTR)) return -2;
        if (ready < 0) return -1;
        auto result = ::read(fd, p, n);
#endif
        if (result < 0 && errno == EINTR) return -2;
        return static_cast<int>(result);
    };
}
JsonLines::State JsonLines::next(ByteReader const &read, std::string &line) {
    for (;;) {
        if (pos == count) {
            int n = read(buffer, sizeof(buffer));
            if (n == -2) return State::Retry;
            if (n < 0) return State::Error;
            if (n == 0) {
                if (oversized) { oversized = false; pending.clear(); return State::Oversize; }
                if (pending.empty()) return State::End;
                line = std::move(pending); pending.clear();
                if (!line.empty() && line.back() == '\r') line.pop_back();
                return State::Line;
            }
            count = n; pos = 0;
        }
        char c = buffer[pos++];
        if (c == '\n') {
            if (oversized) { oversized = false; pending.clear(); return State::Oversize; }
            line = std::move(pending); pending.clear();
            if (!line.empty() && line.back() == '\r') line.pop_back();
            return line.size() > request_limit ? State::Oversize : State::Line;
        }
        if (!oversized) {
            pending += c;
            // One extra byte permits CRLF at the exact ceiling.
            if (pending.size() > request_limit + 1) { oversized = true; pending.clear(); }
        }
    }
}
}
