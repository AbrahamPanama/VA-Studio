// SPDX-License-Identifier: GPL-2.0-or-later
// Standalone: clang++ -std=c++17 test-crash-report-common.cpp -o /tmp/st-e-common
#include "crash-report-common.h"
#include <cstdlib>
#include <iostream>
#include <set>
using namespace vacards_crash;
static unsigned checks = 0;
static void check(bool ok) { ++checks; if (!ok) { std::cerr << "FAIL check " << checks << '\n'; std::exit(1); } }
int main() {
    for (auto code : {0xC0000005u, 0xC000041Du, 0xC0000409u, 0xC00000FDu, 0xC0000374u,
                      0xC0000602u, 0xC1234567u, 0xFFFFFFFFu, 0x80000001u, 0x80000002u,
                      0x80000003u, 0x80000004u}) check(abnormal(code));
    for (auto code : {0u, 1u, 2u, 3u, 134u, 259u, 0xC000013Au, 0x80000000u,
                      0x80000005u, 0xBFFFFFFFu}) check(!abnormal(code));
    check(hex(static_cast<std::uint32_t>(std::int32_t(-1073741819))) == "0xC0000005");
    check(std::string(label(0xC1234567u)) == "abnormal termination");
    for (auto tokens : std::vector<std::vector<std::string>>{{}, {"document.svg"}, {"-g"},
            {"--with-gui", "file.svg"}, {"--", "--actions=quit"}, {"-"},
            {"--with-gui", "--", "-query"}}) check(gui_command(tokens));
    for (auto tokens : std::vector<std::vector<std::string>>{{"--version"}, {"-g", "--actions=quit"},
            {"--export-filename=x"}, {"-o", "x"}, {"--with-gui=yes"}, {"--unknown"},
            {"file.svg", "-q"}, {"--shell"}, {"--batch-process"}}) check(!gui_command(tokens));
    check(gui_command(std::vector<std::wstring>{L"á 文.svg", L"--", L"-文.svg"}));
    Record r{"2026-09-30T17:00:00.000Z", "2026-09-30T17:00:01.000Z", 123, 0xC0000005u,
             false, "1.0", "26", "abcdef", "copied"};
    auto text = format(r);
    check(text.size() <= record_limit);
    std::set<std::string> expected{"schema", "utc_start", "utc_end", "pid", "exit_code",
        "command_class", "version", "build_number", "source_commit", "dump_outcome"}, actual;
    std::istringstream lines(text); std::string line;
    while (std::getline(lines, line)) {
        auto eq = line.find('='); check(eq != std::string::npos);
        check(actual.insert(line.substr(0, eq)).second);
    }
    check(actual == expected);
    check(text.find("command_class=automation\n") != std::string::npos);
    r.gui = true; r.version = "v\npath=secret\r\t" + std::string(20000, 'x');
    text = format(r);
    check(text.find("\npath=") == std::string::npos && text.size() < record_limit);
    check(text.find("command_class=interactive\n") != std::string::npos);
    for (auto name : {"20260930T170000000Z-123.txt", "20260930T170000000Z-123.dmp"})
        check(stem(name) == "20260930T170000000Z-123");
    for (auto name : {"keep.txt", "20260930T170000000Z-123.txt.tmp", "20260930T170000000Z-0.txt",
            "20260930T170000000Z-.txt", "20260930T170000000Z-1/2.txt", "20260930T170000000Z-x.txt", "20260930T170000000Z-4294967296.txt",
            "20260930T170000000Z-123456789012345.txt"})
        check(stem(name).empty());
    std::vector<File> files{{"foreign.txt", UINT64_MAX, 0}, {"foreign.dmp", UINT64_MAX, 0}};
    for (unsigned i = 1; i <= 12; ++i) {
        auto id = "20260930T170000000Z-" + std::to_string(i);
        files.push_back({id + ".txt", 1000, i}); files.push_back({id + ".dmp", 1000, i});
    }
    auto remove = prune(files);
    check(remove.size() == 4);
    check(std::set<std::string>(remove.begin(), remove.end()) == std::set<std::string>{
        "20260930T170000000Z-1.txt", "20260930T170000000Z-1.dmp",
        "20260930T170000000Z-2.txt", "20260930T170000000Z-2.dmp"});
    files = {{"20260930T170000000Z-1.txt", 1000, 1}, {"20260930T170000000Z-1.dmp", dump_limit, 1},
             {"20260930T170000000Z-2.txt", 1000, 2}, {"20260930T170000000Z-2.dmp", dump_limit, 2},
             {"20260930T170000000Z-3.txt", 1000, 3}};
    remove = prune(files); check(remove.size() == 2);
    check(remove[0] == "20260930T170000000Z-1.txt");
    check(prune({{"20260930T170000000Z-1.dmp", UINT64_MAX, 1},
                 {"20260930T170000000Z-1.txt", 1, 1}}).size() == 2);
    check(prune({{"20260930T170000000Z-1.dmp", 1, 1}}).empty());
    std::cout << "PASS " << checks << " portable crash policy checks\n";
}
