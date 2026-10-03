// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef VACARDS_CRASH_REPORT_COMMON_H
#define VACARDS_CRASH_REPORT_COMMON_H
#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace vacards_crash {
constexpr std::uint64_t dump_limit = 128ull * 1024 * 1024;
constexpr std::uint64_t retention_limit = 256ull * 1024 * 1024;
constexpr std::size_t record_limit = 16 * 1024;
inline bool abnormal(std::uint32_t code) {
    return code != 0xC000013Au && ((code & 0xC0000000u) == 0xC0000000u ||
                                  (code >= 0x80000001u && code <= 0x80000004u));
}
inline char const *label(std::uint32_t code) {
    switch (code) {
        case 0xC0000005u: return "access violation";
        case 0xC000041Du: return "callback exception";
        case 0xC0000409u: return "fail-fast / stack buffer overrun";
        case 0xC00000FDu: return "stack overflow";
        case 0x80000003u: return "breakpoint";
        default: return "abnormal termination";
    }
}
inline std::string hex(std::uint32_t code) {
    std::ostringstream out;
    out << "0x" << std::uppercase << std::hex << std::setfill('0') << std::setw(8) << code;
    return out.str();
}
// Tokens exclude argv[0]. A lone '-' is an operand; '--' ends option parsing.
template<class String> bool gui_command(std::vector<String> const &tokens) {
    bool options = true;
    for (auto const &s : tokens) {
        if (!options || s.empty()) continue;
        String end{ '-', '-' }, short_gui{ '-', 'g' };
        String long_gui{ '-', '-', 'w', 'i', 't', 'h', '-', 'g', 'u', 'i' };
        if (s == end) { options = false; continue; }
        if (s.size() > 1 && s[0] == '-' && s != short_gui && s != long_gui) return false;
    }
    return true;
}
// Prevent control-character injection; build fields are bounded, printable ASCII.
inline std::string field(std::string const &s) {
    std::string out;
    for (unsigned char c : s.substr(0, 160)) out += c >= 32 && c <= 126 ? char(c) : '?';
    return out;
}
struct Record {
    std::string start, end;
    std::uint32_t pid, code;
    bool gui;
    std::string version, build, commit, dump;
};
inline std::string format(Record const &r) {
    return "schema=1\nutc_start=" + field(r.start) + "\nutc_end=" + field(r.end) +
        "\npid=" + std::to_string(r.pid) + "\nexit_code=" + hex(r.code) +
        "\ncommand_class=" + (r.gui ? "interactive" : "automation") +
        "\nversion=" + field(r.version) + "\nbuild_number=" + field(r.build) +
        "\nsource_commit=" + field(r.commit) + "\ndump_outcome=" + field(r.dump) + "\n";
}
// Owned completed basenames: YYYYMMDDTHHMMSSmmmZ-<decimal PID>.txt/.dmp.
inline std::string stem(std::string const &name) {
    if (name.size() < 25 || name.size() > 34 || (name.substr(name.size()-4) != ".txt" &&
                             name.substr(name.size()-4) != ".dmp")) return {};
    if (name[8] != 'T' || name[18] != 'Z' || name[19] != '-') return {};
    for (std::size_t i = 0; i < name.size()-4; ++i) {
        if (i == 8 || i == 18 || i == 19) continue;
        if (name[i] < '0' || name[i] > '9') return {};
    }
    if (name[20] == '0') return {};
    std::uint64_t pid = 0;
    for (std::size_t i = 20; i < name.size()-4; ++i) pid = pid * 10 + (name[i]-'0');
    if (pid > UINT32_MAX) return {};
    return name.substr(0, name.size()-4);
}
struct File { std::string name; std::uint64_t size, time; };
// Treat orphan attachments as pairs too. Never select unrelated/temp files.
inline std::vector<std::string> prune(std::vector<File> const &files) {
    struct Pair { std::string id; std::uint64_t size=0, time=0; std::vector<std::string> names; };
    std::vector<Pair> pairs;
    for (auto const &f : files) {
        auto id = stem(f.name);
        if (id.empty()) continue;
        auto p = std::find_if(pairs.begin(), pairs.end(), [&](Pair const &p){return p.id == id;});
        if (p == pairs.end()) { pairs.push_back({id, 0, 0, {}}); p = pairs.end()-1; }
        p->size = f.size > UINT64_MAX-p->size ? UINT64_MAX : p->size+f.size;
        p->time = std::max(p->time, f.time);
        p->names.push_back(f.name);
    }
    std::sort(pairs.begin(), pairs.end(), [](Pair const &a, Pair const &b){
        return a.time != b.time ? a.time > b.time : a.id > b.id;
    });
    std::size_t kept = 0;
    std::uint64_t bytes = 0;
    std::vector<std::string> remove;
    for (auto const &p : pairs) {
        if (kept < 10 && p.size <= retention_limit-bytes) { ++kept; bytes += p.size; }
        else remove.insert(remove.end(), p.names.begin(), p.names.end());
    }
    return remove;
}
}
#endif
