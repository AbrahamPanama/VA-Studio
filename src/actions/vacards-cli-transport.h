// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef VACARDS_CLI_TRANSPORT_H
#define VACARDS_CLI_TRANSPORT_H
#include <functional>
#include <chrono>
#include <memory>
#include <istream>
#include <string>
#include <string_view>
#include <thread>
namespace Inkscape::VACardsCli {
inline constexpr std::size_t request_limit = 1048576, response_limit = 8388608;
// Reads bytes: positive=count, 0=EOF, -1=fatal, -2=retry (allows shutdown).
using ByteReader = std::function<int(char *, std::size_t)>;
using LineWriter = std::function<bool(std::string_view)>;
ByteReader stream_reader(std::istream &input);
ByteReader descriptor_reader(int fd);
class BoundedFdWriter {
public:
    explicit BoundedFdWriter(int fd);
    ~BoundedFdWriter();
    BoundedFdWriter(BoundedFdWriter const &) = delete;
    BoundedFdWriter &operator=(BoundedFdWriter const &) = delete;
    // Not named write(): vacards-cli-entry.cpp maps write to _write on Windows.
    bool push_line(std::string_view line);
    void close_and_flush(std::chrono::milliseconds bound);
private:
    struct SharedState;
    std::shared_ptr<SharedState> state;
    std::thread worker;
};
class JsonLines {
public:
    enum class State { Line, Oversize, End, Error, Retry };
    State next(ByteReader const &read, std::string &line);
private:
    std::string pending;
    bool oversized = false;
    char buffer[4096];
    std::size_t pos = 0, count = 0;
};
}
#endif
