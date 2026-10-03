// SPDX-License-Identifier: GPL-2.0-or-later
#define BOOST_JSON_NO_LIB
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <future>
#include <numbers>
#include <numeric>
#include <thread>
#include <boost/json.hpp>
#include <glib.h>
#include <gtest/gtest.h>

#include "document-undo.h"
#include "document.h"
#include "inkscape.h"
#include "nesting/nesting-document.h"
#include "nesting/sparrow-adapter.h"
#include "object/sp-item.h"
#include "xml/repr.h"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
namespace Inkscape::Nesting::SparrowInternal {
void runWindowsForTest(std::filesystem::path const &, std::vector<std::string> const &,
                       std::chrono::steady_clock::time_point, std::stop_token, unsigned);
}
#endif
using namespace Inkscape;
using namespace Inkscape::Nesting;
namespace {
namespace json = boost::json;
std::string read(std::filesystem::path const &p)
{
    std::ifstream in(p);
    return {std::istreambuf_iterator<char>(in), {}};
}
#ifdef _WIN32
std::string probeUtf8(std::filesystem::path const &path)
{
    auto bytes = path.u8string();
    return {reinterpret_cast<char const *>(bytes.data()), bytes.size()};
}
// CMake builds these tests as a GUI-subsystem executable on Windows. Make a
// temporary adjacent copy with only its PE subsystem set to Windows console;
// the probe code and DLL resolution stay identical, without changing CMake.
struct ConsoleProbeImage
{
    struct FileOwner {
        std::filesystem::path path;
        ~FileOwner() { std::error_code ec; std::filesystem::remove(path, ec); }
    } file;
    ConsoleProbeImage()
    {
        wchar_t original[32768]{}, temporary[MAX_PATH]{};
        if (!GetModuleFileNameW(nullptr, original, 32768) ||
            !GetTempFileNameW(std::filesystem::path(original).parent_path().c_str(), L"spw", 0, temporary))
            throw std::runtime_error("Cannot locate console probe image");
        file.path = temporary;
        auto exe = file.path;
        exe.replace_extension(L".exe");
        std::filesystem::rename(file.path, exe);
        file.path = exe;
        if (!CopyFileW(original, file.path.c_str(), FALSE))
            throw std::runtime_error("Cannot copy console probe image");
        std::fstream image(file.path, std::ios::binary | std::ios::in | std::ios::out);
        std::uint32_t offset = 0, signature = 0;
        image.seekg(0x3c);
        image.read(reinterpret_cast<char *>(&offset), sizeof(offset));
        image.seekg(offset);
        image.read(reinterpret_cast<char *>(&signature), sizeof(signature));
        if (!image || signature != 0x00004550)
            throw std::runtime_error("Invalid console probe PE image");
        std::uint16_t const console = IMAGE_SUBSYSTEM_WINDOWS_CUI;
        image.seekp(offset + 24 + 68); // OptionalHeader.Subsystem, PE32 and PE32+
        image.write(reinterpret_cast<char const *>(&console), sizeof(console));
        image.flush();
        if (!image) throw std::runtime_error("Cannot set console probe subsystem");
    }
};
std::filesystem::path consoleProbeExecutable()
{
    static ConsoleProbeImage image;
    return image.file.path;
}
// Only the explicitly spawned console image has the marker argument;
// ordinary suite runs do not create a marker.
TEST(SparrowWindowsSpawn, ConsoleChildProbe)
{
    int count = 0;
    auto argv = CommandLineToArgvW(GetCommandLineW(), &count);
    ASSERT_NE(argv, nullptr);
    struct ArgvOwner { LPWSTR *p; ~ArgvOwner() { LocalFree(p); } } owner{argv};
    std::filesystem::path marker;
    std::wstring mode;
    int index = 0;
    for (int i = 0; i < count; ++i) {
        std::wstring_view arg(argv[i]);
        if (arg.starts_with(L"--sparrow-marker=")) {
            marker = std::wstring(arg.substr(17));
            index = i;
        }
        if (arg.starts_with(L"--sparrow-mode=")) mode = arg.substr(15);
    }
    if (marker.empty()) return;
    std::ifstream image(std::filesystem::path(argv[0]), std::ios::binary);
    std::uint32_t offset = 0;
    std::uint16_t subsystem = 0;
    image.seekg(0x3c);
    image.read(reinterpret_cast<char *>(&offset), sizeof(offset));
    image.seekg(offset + 24 + 68);
    image.read(reinterpret_cast<char *>(&subsystem), sizeof(subsystem));
    ASSERT_TRUE(image.good());
    EXPECT_EQ(subsystem, IMAGE_SUBSYSTEM_WINDOWS_CUI);
    ASSERT_GE(count, index + 5);
    EXPECT_EQ(std::wstring(argv[index + 1]), L"");
    EXPECT_EQ(std::wstring(argv[index + 2]), L"space and \"quote\"");
    EXPECT_EQ(std::wstring(argv[index + 3]), L"trailing slash\\");
    EXPECT_EQ(std::wstring(argv[index + 4]), L"two\\\\\"quotes\"\\");
    EXPECT_EQ(GetConsoleWindow(), nullptr);
    wchar_t rayon[32]{};
    ASSERT_GT(GetEnvironmentVariableW(L"RAYON_NUM_THREADS", rayon, 32), 0u);
    EXPECT_EQ(std::wstring(rayon), L"3");
    wchar_t system[32768]{};
    EXPECT_GT(GetEnvironmentVariableW(L"SystemRoot", system, 32768), 0u);
    EXPECT_EQ(std::filesystem::current_path(), marker.parent_path());
    for (auto id : {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE})
        EXPECT_EQ(GetFileType(GetStdHandle(id)), static_cast<DWORD>(FILE_TYPE_CHAR));
    std::ofstream report(marker);
    report << "console=" << (GetConsoleWindow() ? "present" : "absent") << "\n";
    report.close();
    // Stay alive across multiple desktop window snapshots. Deadline/cancel
    // modes instead prove the parent interrupts a running console child.
    std::this_thread::sleep_for(std::chrono::milliseconds(mode == L"wait" ? 10000 : 700));
    if (mode == L"fail") ADD_FAILURE() << "deliberate nonzero child exit";
}
struct WindowsProbe
{
    std::filesystem::path dir;
    std::vector<std::string> args;
    WindowsProbe(std::string mode = "pass")
    {
        auto raw = g_dir_make_tmp("sparrow-console-XXXXXX", nullptr);
        if (!raw) throw std::runtime_error("Cannot create console probe directory");
        auto base = std::filesystem::path(std::u8string(reinterpret_cast<char8_t const *>(raw)));
        g_free(raw);
        dir = base / L"space and \u00f1";
        std::filesystem::create_directory(dir);
        args = {probeUtf8(consoleProbeExecutable()), "--gtest_filter=SparrowWindowsSpawn.ConsoleChildProbe",
                "--sparrow-mode=" + mode, "--sparrow-marker=" + probeUtf8(dir / L"console marker.txt"),
                "", "space and \"quote\"", "trailing slash\\", "two\\\\\"quotes\"\\"};
    }
    ~WindowsProbe() { std::error_code ec; std::filesystem::remove_all(dir.parent_path(), ec); }
    void launch(std::chrono::steady_clock::time_point deadline, std::stop_token stop = {})
    {
        SparrowInternal::runWindowsForTest(dir, args, deadline, stop, 3);
    }
};
TEST(SparrowWindowsSpawn, ConsoleProgramHasNoConsoleAndReceivesQuotedArguments)
{
    WindowsProbe probe;
    ASSERT_NO_THROW(probe.launch(std::chrono::steady_clock::now() + std::chrono::seconds(15)));
    EXPECT_EQ(read(probe.dir / L"console marker.txt"), "console=absent\n");
}
TEST(SparrowWindowsSpawn, NonzeroConsoleChildIsRejected)
{
    WindowsProbe probe("fail");
    EXPECT_THROW(probe.launch(std::chrono::steady_clock::now() + std::chrono::seconds(15)), std::runtime_error);
    EXPECT_EQ(read(probe.dir / L"console marker.txt"), "console=absent\n");
}
TEST(SparrowWindowsSpawn, DeadlineReapsRunningConsoleChild)
{
    WindowsProbe probe("wait");
    auto const started = std::chrono::steady_clock::now();
    EXPECT_THROW(probe.launch(started + std::chrono::seconds(2)), std::runtime_error);
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(4));
    EXPECT_EQ(read(probe.dir / L"console marker.txt"), "console=absent\n");
}
TEST(SparrowWindowsSpawn, CancellationReapsRunningConsoleChild)
{
    WindowsProbe probe("wait");
    std::stop_source stop;
    auto const started = std::chrono::steady_clock::now();
    auto future = std::async(std::launch::async, [&] {
        probe.launch(started + std::chrono::seconds(15), stop.get_token());
    });
    for (int i = 0; i < 400 && !std::filesystem::exists(probe.dir / L"console marker.txt"); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    stop.request_stop();
    EXPECT_THROW(future.get(), std::runtime_error);
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(4));
    EXPECT_EQ(read(probe.dir / L"console marker.txt"), "console=absent\n");
}
#endif
json::array points(std::vector<Point> const &p)
{
    json::array out;
    for (auto v : p)
        out.push_back(json::array{v.x, v.y});
    return out;
}
json::value snapshotJson(PreparedDocumentNesting const &s)
{
    json::array parts, holes;
    for (auto const &h : s.container_holes)
        holes.push_back(points(h));
    for (auto const &p : s.parts) {
        json::array components;
        for (auto const &c : p.components) {
            json::array inner;
            for (auto const &h : c.holes)
                inner.push_back(points(h));
            components.push_back(json::object{{"outer", points(c.outer)}, {"holes", std::move(inner)}});
        }
        parts.push_back(json::object{{"id", p.id}, {"svg_id", p.item->getId()}, {"components", std::move(components)}});
    }
    return json::object{{"container", points(s.container_outline)},
                        {"container_holes", std::move(holes)},
                        {"parts", std::move(parts)}};
}
json::value posesJson(SolveResult const &r)
{
    json::array rows;
    for (auto p : r.placements)
        rows.push_back(json::object{{"id", p.part_id},
                                    {"x", p.translation_x},
                                    {"y", p.translation_y},
                                    {"angle", p.rotation_degrees},
                                    {"placed", p.placed}});
    return json::object{{"status", "ok"},
                        {"backend", r.backend},
                        {"backend_detail", r.backend_detail},
                        {"elapsed", r.metrics.elapsed_seconds},
                        {"placements", std::move(rows)}};
}
struct Fixture
{
    std::unique_ptr<SPDocument> document;
    PreparedDocumentNesting snapshot;
    Options options;
    std::vector<std::string> ids;
    Fixture(std::string const &name)
    {
        if (!Application::exists())
            Application::create(false);
        auto dir = std::filesystem::path(NESTING_SPARROW_FIXTURES) / name;
        document = SPDocument::createNewDoc((dir / "input.svg").string().c_str());
        if (!document)
            throw std::runtime_error("Fixture SVG could not open");
        document->ensureUpToDate();
        DocumentUndo::done(document.get(), Util::Internal::ContextString{"Fixture"}, "");
        DocumentUndo::clearUndo(document.get());
        DocumentUndo::clearRedo(document.get());
        document->setModifiedSinceSave(false);
        std::ifstream input(dir / "ids.txt");
        std::string id;
        std::vector<SPItem *> parts;
        while (input >> id) {
            ids.push_back(id);
            parts.push_back(cast<SPItem>(document->getObjectById(id)));
        }
        auto prepared = prepareDocumentNesting(cast<SPItem>(document->getObjectById("container")), parts);
        if (!prepared)
            throw std::runtime_error(prepared.error);
        snapshot = std::move(*prepared.snapshot);
        options.time_limit_ms = 15000;
        auto extra = json::parse(read(dir / "options.json")).as_object();
        if (auto v = extra.if_contains("time_limit_ms"))
            options.time_limit_ms = v->to_number<std::uint64_t>();
        if (auto v = extra.if_contains("rotation_mode"))
            options.rotation_mode = RotationMode(v->to_number<int>());
        if (auto v = extra.if_contains("rotation_step_degrees"))
            options.rotation_step_degrees = v->to_number<double>();
        if (auto v = extra.if_contains("part_spacing"))
            options.part_spacing = v->to_number<double>();
        if (auto v = extra.if_contains("container_margin"))
            options.container_margin = v->to_number<double>();
    }
};
void writeJson(std::filesystem::path const &p, json::value const &value)
{
    std::ofstream out(p);
    out << json::serialize(value);
    ASSERT_TRUE(out.good());
}
void runCase(std::string const &name, int seed, unsigned expected)
{
    Fixture f(name);
    f.options.random_seed = seed;
    auto before = sp_repr_save_buf(f.document->getReprDoc()).raw();
    auto solved = solvePreparedNesting(f.snapshot, f.options);
    ASSERT_TRUE(solved) << solved.error;
    EXPECT_EQ(sp_repr_save_buf(f.document->getReprDoc()).raw(), before);
    auto placed = std::count_if(solved.placements.begin(), solved.placements.end(), [](auto p) { return p.placed; });
    EXPECT_GE(placed, expected) << name << " via " << solved.backend;
    if (name == "circles80") {
        EXPECT_EQ(placed, 80);
        EXPECT_EQ(solved.backend, "sparrow");
    }
    auto apply = applyNestingPlacements(f.snapshot, solved.placements);
    ASSERT_TRUE(apply.status == ApplyStatus::Applied || apply.status == ApplyStatus::NoChange) << apply.error;
    auto after = sp_repr_save_buf(f.document->getReprDoc()).raw();
    if (apply.changed()) {
        DocumentUndo::undo(f.document.get());
        f.document->ensureUpToDate();
        for (auto const &part : f.snapshot.parts)
            for (int i = 0; i < 6; ++i)
                EXPECT_NEAR(part.item->i2doc_affine()[i], part.original_item_to_document[i], 1e-7);
        DocumentUndo::redo(f.document.get());
        f.document->ensureUpToDate();
        EXPECT_EQ(sp_repr_save_buf(f.document->getReprDoc()).raw(), after);
    }
    auto root = g_getenv("VACARDS_SPARROW_EVIDENCE");
    std::filesystem::path dir;
    if (root)
        dir = std::filesystem::path(root) / (name + "-seed" + std::to_string(seed));
    else {
        auto tmp = g_dir_make_tmp("vacards-sparrow-acceptance-XXXXXX", nullptr);
        ASSERT_TRUE(tmp);
        dir = tmp;
        g_free(tmp);
    }
    std::filesystem::create_directories(dir);
    ::testing::Test::RecordProperty("evidence_directory", dir.string());
    writeJson(dir / "snapshot.json", snapshotJson(f.snapshot));
    writeJson(dir / "result.json", posesJson(solved));
    writeJson(dir / "options.json", json::object{{"time_limit_ms", f.options.time_limit_ms},
                                                 {"rotation_mode", int(f.options.rotation_mode)},
                                                 {"rotation_step_degrees", f.options.rotation_step_degrees},
                                                 {"part_spacing", f.options.part_spacing},
                                                 {"container_margin", f.options.container_margin},
                                                 {"flatten_tolerance", .05},
                                                 {"allow_part_holes", false}});
    {
        std::ofstream svg(dir / "applied.svg");
        svg << after;
        ASSERT_TRUE(svg.good());
    }
    auto reopened = SPDocument::createNewDoc((dir / "applied.svg").string().c_str());
    ASSERT_TRUE(reopened);
    reopened->ensureUpToDate();
    std::vector<SPItem *> parts;
    for (auto const &id : f.ids)
        parts.push_back(cast<SPItem>(reopened->getObjectById(id)));
    auto prepared = prepareDocumentNesting(cast<SPItem>(reopened->getObjectById("container")), parts);
    ASSERT_TRUE(prepared) << prepared.error;
    writeJson(dir / "recaptured.json", snapshotJson(*prepared.snapshot));
}
std::vector<Point> detailedRing(std::size_t count, double radius)
{
    std::vector<Point> ring;
    for (std::size_t i = 0; i < count; ++i) {
        auto a = 2 * std::numbers::pi * static_cast<double>(i) / static_cast<double>(count);
        ring.push_back({radius * std::cos(a), radius * std::sin(a)});
    }
    return ring;
}
TEST(SparrowAcceptance, EligibilitySkipsRunsThatCannotHelp)
{
    {
        Fixture f("rectangles");
        ASSERT_TRUE(sparrowEligible(f.snapshot, f.options));
        f.options.time_limit_ms = 2999;
        EXPECT_FALSE(sparrowEligible(f.snapshot, f.options)) << "Sparrow would get 0 search seconds";
        f.options.time_limit_ms = 3000;
        EXPECT_TRUE(sparrowEligible(f.snapshot, f.options));
    }
    {
        Fixture f("rectangles");
        f.snapshot.parts.front().components.front().outer = detailedRing(4000, 10);
        EXPECT_TRUE(sparrowEligible(f.snapshot, f.options)) << "detail is fine: Sparrow gets a proxy";
        f.options.part_spacing = 1;
        EXPECT_TRUE(sparrowEligible(f.snapshot, f.options)) << "and so is spacing";
        f.snapshot.parts.front().components.front().outer = detailedRing(10001, 10);
        EXPECT_FALSE(sparrowEligible(f.snapshot, f.options)) << "beyond the contour cap";
    }
    {
        Fixture f("rectangles");
        auto const &sheet = f.snapshot.container_outline;
        auto const [x0, x1] = std::minmax({sheet[0].x, sheet[1].x, sheet[2].x, sheet[3].x});
        auto const [y0, y1] = std::minmax({sheet[0].y, sheet[1].y, sheet[2].y, sheet[3].y});
        // One part larger than the sheet: Sparrow still runs (it fills the
        // sheet with what fits) and leaves that part unplaced.
        f.snapshot.parts.front().components.front().outer = {
            {0, 0}, {x1 - x0, 0}, {x1 - x0, y1 - y0 + 1}, {0, y1 - y0 + 1}};
        EXPECT_TRUE(sparrowEligible(f.snapshot, f.options)) << "overflow fills the sheet";
        auto solved = solveSparrow(f.snapshot, f.options,
                                   std::chrono::steady_clock::now() + std::chrono::milliseconds(f.options.time_limit_ms), {});
        ASSERT_TRUE(solved) << solved.error;
        EXPECT_FALSE(solved.placements.front().placed);
    }
}
std::size_t placedCount(std::vector<Placement> const &placements)
{
    return std::count_if(placements.begin(), placements.end(), [](auto p) { return p.placed; });
}
bool validLayout(PreparedDocumentNesting const &s, Options const &o, std::vector<Placement> const &placements)
{
    Job job(o);
    if (job.setContainer(s.container_outline) != Status::Ok)
        return false;
    for (auto const &part : s.parts)
        if (job.addPart(part.id, part.components) != Status::Ok)
            return false;
    if (job.validate(placements) == Status::Ok)
        return true;
    ::testing::Test::RecordProperty("validation_error", job.error());
    std::cerr << "validation: " << job.error() << "\n";
    // Keep the failing layout for offline inspection.
    if (auto dir = g_dir_make_tmp("vacards-sparrow-invalid-XXXXXX", nullptr)) {
        json::array rows;
        for (auto const &placement : placements)
            rows.push_back(json::object{{"id", placement.part_id},
                                        {"x", placement.translation_x},
                                        {"y", placement.translation_y},
                                        {"angle", placement.rotation_degrees},
                                        {"placed", placement.placed}});
        std::ofstream(std::filesystem::path(dir) / "snapshot.json") << json::serialize(snapshotJson(s));
        std::ofstream(std::filesystem::path(dir) / "placements.json") << json::serialize(rows);
        std::cerr << "invalid layout kept in " << dir << "\n";
        g_free(dir);
    }
    return false;
}
TEST(SparrowAcceptance, OverfilledSheetIsFilledBySparrow)
{
    // 90 circles of r=50 (a hexagonal packing of this sheet holds 84) and 20
    // of r=30. Their area fits the sheet, but they cannot all fit: Sparrow,
    // which packs every copy it is given into a strip, must search the count
    // and keep only copies wholly on the sheet, across two part types.
    Fixture f("circles80");
    f.options.time_limit_ms = 20000;
    auto const originals = f.snapshot.parts.size();
    std::uint64_t next_id = 0;
    for (auto const &part : f.snapshot.parts)
        next_id = std::max(next_id, part.id + 1);
    for (std::size_t i = 0; i < 30; ++i) {
        auto copy = f.snapshot.parts[i % originals];
        copy.id = next_id++;
        if (i >= 10) {
            for (auto &component : copy.components) {
                double cx = 0, cy = 0;
                for (auto const &point : component.outer) {
                    cx += point.x / component.outer.size();
                    cy += point.y / component.outer.size();
                }
                for (auto &point : component.outer) {
                    point.x = cx + 0.6 * (point.x - cx);
                    point.y = cy + 0.6 * (point.y - cy);
                }
            }
        }
        f.snapshot.parts.push_back(std::move(copy));
    }
    ASSERT_TRUE(sparrowEligible(f.snapshot, f.options));
    auto solved = solveSparrow(f.snapshot, f.options,
                               std::chrono::steady_clock::now() + std::chrono::milliseconds(f.options.time_limit_ms), {});
    ASSERT_TRUE(solved) << solved.error;
    ASSERT_EQ(solved.placements.size(), f.snapshot.parts.size());
    EXPECT_TRUE(validLayout(f.snapshot, f.options, solved.placements));
    auto const placed = placedCount(solved.placements);
    EXPECT_GE(placed, 76u);
    EXPECT_LT(placed, f.snapshot.parts.size());
    // The trace lists "offered>on sheet" per run; the search ran again after
    // the first run left copies off the sheet.
    auto const runs = std::count(solved.backend_detail.begin(), solved.backend_detail.end(), '>');
    EXPECT_GE(runs, 2) << solved.backend_detail;
    ::testing::Test::RecordProperty("placed", static_cast<int>(placed));
    ::testing::Test::RecordProperty("runs", solved.backend_detail);
}
TEST(SparrowAcceptance, DetailedSpacedCopiesReachSparrowAndKeepTheirSpacing)
{
    // 40 copies of a 3000-vertex outline with 2 px spacing. Sparrow's own
    // separation offset would take tens of seconds at this detail and the
    // copies exceed the old 50000-vertex total; Sparrow now gets one
    // simplified proxy per outline and must finish with a layout whose exact
    // outlines keep the spacing.
    Fixture f("circles80");
    f.options.time_limit_ms = 15000;
    f.options.part_spacing = 2;
    f.snapshot.parts.resize(40);
    for (auto &part : f.snapshot.parts) {
        auto &outer = part.components.front().outer;
        double cx = 0, cy = 0;
        for (auto const &point : outer) {
            cx += point.x / outer.size();
            cy += point.y / outer.size();
        }
        outer.clear();
        for (int i = 0; i < 3000; ++i) {
            auto const a = 2 * std::numbers::pi * i / 3000;
            auto const r = 45 * (1 + 0.2 * std::sin(7 * a) + 0.08 * std::sin(23 * a));
            outer.push_back({cx + r * std::cos(a), cy + r * std::sin(a)});
        }
    }
    ASSERT_TRUE(sparrowEligible(f.snapshot, f.options));
    auto const started = std::chrono::steady_clock::now();
    auto solved = solveSparrow(f.snapshot, f.options,
                               started + std::chrono::milliseconds(f.options.time_limit_ms), {});
    auto const elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    ASSERT_TRUE(solved) << solved.error;
    EXPECT_TRUE(validLayout(f.snapshot, f.options, solved.placements)) << "exact outlines keep 2 px";
    EXPECT_GE(placedCount(solved.placements), 30u);
    EXPECT_LT(elapsed, f.options.time_limit_ms / 1000.0 + 2.0);
    ::testing::Test::RecordProperty("placed", static_cast<int>(placedCount(solved.placements)));
}
/// `distinct` 3000-vertex outlines, `copies` of each, on the circles80 sheet
/// moved `shift` px from the origin.
void useDistinctDetailedShapes(Fixture &f, std::size_t distinct, std::size_t copies, double shift)
{
    for (auto &point : f.snapshot.container_outline)
        point.x += shift;
    f.snapshot.parts.resize(distinct * copies);
    for (std::size_t index = 0; index < f.snapshot.parts.size(); ++index) {
        auto &outer = f.snapshot.parts[index].components.front().outer;
        double cx = 0, cy = 0;
        for (auto const &point : outer) {
            cx += point.x / outer.size();
            cy += point.y / outer.size();
        }
        auto const shape = static_cast<double>(index % distinct);
        outer.clear();
        for (int i = 0; i < 3000; ++i) {
            auto const a = 2 * std::numbers::pi * i / 3000;
            auto const r = 40 * (1 + 0.15 * std::sin((5 + shape) * a) + 0.06 * std::sin((19 + 2 * shape) * a));
            outer.push_back({shift + cx + r * std::cos(a), cy + r * std::sin(a)});
        }
    }
}
TEST(SparrowAcceptance, ZeroSpacingDetailedShapesNestValidlyFarFromTheOrigin)
{
    // Spacing 0 (a tiny proxy offset, review of a8103456a) on a sheet 30000 px
    // from the origin, where the validator's float32 composition loses most.
    Fixture f("circles80");
    f.options.time_limit_ms = 30000;
    f.options.part_spacing = 0;
    useDistinctDetailedShapes(f, 4, 8, 30000);
    ASSERT_TRUE(sparrowEligible(f.snapshot, f.options));
    auto const started = std::chrono::steady_clock::now();
    auto solved = solveSparrow(f.snapshot, f.options,
                               started + std::chrono::milliseconds(f.options.time_limit_ms), {});
    auto const elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    ASSERT_TRUE(solved) << solved.error;
    EXPECT_TRUE(validLayout(f.snapshot, f.options, solved.placements));
    EXPECT_GE(placedCount(solved.placements), 28u);
    EXPECT_LT(elapsed, f.options.time_limit_ms / 1000.0 + 2.0);
    ::testing::Test::RecordProperty("placed", static_cast<int>(placedCount(solved.placements)));
    ::testing::Test::RecordProperty("detail", solved.backend_detail);
}
TEST(SparrowAcceptance, ProxyPreparationOverBudgetGivesUpPromptly)
{
    // 12 distinct detailed outlines need several seconds of envelope
    // preparation; with 5 s the Sparrow lane must give up within its budget
    // (native nests alone) instead of running past the deadline.
    Fixture f("circles80");
    f.options.time_limit_ms = 5000;
    f.options.part_spacing = 0;
    useDistinctDetailedShapes(f, 12, 3, 0);
    auto const started = std::chrono::steady_clock::now();
    auto solved = solveSparrow(f.snapshot, f.options,
                               started + std::chrono::milliseconds(f.options.time_limit_ms), {});
    auto const elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    EXPECT_LT(elapsed, f.options.time_limit_ms / 1000.0 + 2.0) << solved.error;
    ::testing::Test::RecordProperty("outcome", solved ? solved.backend_detail : solved.error);
    if (solved) {
        // A machine fast enough to prepare these within 2 s nests normally.
        EXPECT_TRUE(validLayout(f.snapshot, f.options, solved.placements));
        GTEST_SKIP() << "proxies were prepared within the budget: " << solved.backend_detail;
    }
    EXPECT_NE(solved.error.find("ran out of its time budget"), std::string::npos) << solved.error;
}
TEST(SparrowAcceptance, SparrowIsNotUsedWithObstacles)
{
    Fixture f("rectangles");
    ASSERT_TRUE(sparrowEligible(f.snapshot, f.options));
    auto const &sheet = f.snapshot.container_outline;
    auto const x0 = std::min({sheet[0].x, sheet[1].x, sheet[2].x, sheet[3].x});
    auto const y0 = std::min({sheet[0].y, sheet[1].y, sheet[2].y, sheet[3].y});
    PreparedObstacle obstacle;
    CollisionComponent mark;
    mark.outer = {{x0, y0}, {x0 + 2, y0}, {x0 + 2, y0 + 2}, {x0, y0 + 2}};
    obstacle.components.push_back(mark);
    f.snapshot.obstacles.push_back(obstacle);
    EXPECT_FALSE(sparrowEligible(f.snapshot, f.options));
    auto solved = solvePreparedNesting(f.snapshot, f.options);
    ASSERT_TRUE(solved) << solved.error;
    EXPECT_EQ(solved.backend, "native");
}
TEST(SparrowAcceptance, SpacedDetailedPartsAreNestedWithinTheBudget)
{
    Fixture f("spaced_curves");
    EXPECT_TRUE(sparrowEligible(f.snapshot, f.options));
    auto started = std::chrono::steady_clock::now();
    auto solved = solvePreparedNesting(f.snapshot, f.options);
    auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    ASSERT_TRUE(solved) << solved.error;
    EXPECT_EQ(std::count_if(solved.placements.begin(), solved.placements.end(), [](auto p) { return p.placed; }), 6);
    EXPECT_LT(elapsed, f.options.time_limit_ms / 1000.0 + 3.0);
}
TEST(SparrowAcceptance, PinnedHelperAvailable)
{
    ASSERT_TRUE(sparrowAvailable());
}
TEST(SparrowAcceptance, CirclesSeed0)
{
    runCase("circles80", 0, 80);
}
TEST(SparrowAcceptance, CirclesSeed1)
{
    runCase("circles80", 1, 80);
}
TEST(SparrowAcceptance, CirclesSeed2)
{
    runCase("circles80", 2, 80);
}
class SparrowCorpus : public ::testing::TestWithParam<std::tuple<char const *, unsigned>>
{
};
TEST_P(SparrowCorpus, GeometryAndSvgRoundtrip)
{
    auto [name, count] = GetParam();
    runCase(name, 0, count);
}
INSTANTIATE_TEST_SUITE_P(Shapes, SparrowCorpus,
                         ::testing::Values(std::tuple{"rectangles", 32u}, std::tuple{"triangles", 35u},
                                           std::tuple{"concave_l", 24u}, std::tuple{"mixed_curves", 36u},
                                           std::tuple{"clearance", 20u}, std::tuple{"nested_transforms", 20u},
                                           std::tuple{"container_hole", 30u}, std::tuple{"part_holes_filled", 18u},
                                           std::tuple{"compound_rigid", 15u}, std::tuple{"oversized_part", 15u},
                                           std::tuple{"concave_container", 24u}, std::tuple{"discrete_rotations", 25u},
                                           std::tuple{"rotation_required", 7u}, std::tuple{"rotation_forbidden", 6u},
                                           std::tuple{"spaced_curves", 6u}));
TEST(SparrowAcceptance, ActiveCancellationLeavesDocumentUnchanged)
{
    Fixture f("circles80");
    std::stop_source stop;
    auto before = sp_repr_save_buf(f.document->getReprDoc()).raw();
    auto started = std::chrono::steady_clock::now();
    auto future = std::async(std::launch::async,
                             [&] { return solvePreparedNesting(f.snapshot, f.options, {}, stop.get_token()); });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    stop.request_stop();
    auto result = future.get();
    EXPECT_EQ(result.status, Status::Cancelled);
    EXPECT_TRUE(result.placements.empty());
    EXPECT_EQ(sp_repr_save_buf(f.document->getReprDoc()).raw(), before);
    EXPECT_LT(std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count(), 3.0);
}
TEST(SparrowAcceptance, GeometryValidationRejectsInvalidExternalCandidates)
{
    Options options;
    options.rotation_mode = RotationMode::None;
    options.part_spacing = 1;
    options.container_margin = 1;
    std::vector<Point> sheet{{0, 0}, {30, 0}, {30, 30}, {0, 30}}, square{{0, 0}, {5, 0}, {5, 5}, {0, 5}};
    auto validate = [&](std::vector<Placement> const &p) {
        Job job(options);
        EXPECT_EQ(job.setContainer(sheet), Status::Ok);
        EXPECT_EQ(job.addPart(1, square), Status::Ok);
        EXPECT_EQ(job.addPart(2, square), Status::Ok);
        return job.validate(p);
    };
    std::vector<Placement> valid{{1, 3, 3, 0, true}, {2, 10, 3, 0, true}};
    EXPECT_EQ(validate(valid), Status::Ok);
    auto bad = valid;
    bad[1].translation_x = 2;
    EXPECT_NE(validate(bad), Status::Ok);
    bad = valid;
    bad[0].translation_x = -2;
    EXPECT_NE(validate(bad), Status::Ok);
    bad = valid;
    bad[1].translation_x = 8.5;
    EXPECT_NE(validate(bad), Status::Ok);
    bad = valid;
    bad[1].rotation_degrees = 90;
    EXPECT_NE(validate(bad), Status::Ok);
    bad = valid;
    bad[1].part_id = 1;
    EXPECT_NE(validate(bad), Status::Ok);
}
TEST(SparrowAcceptance, ExpiredBudgetStillPublishesTheFirstLayout)
{
    Fixture f("spaced_curves");
    f.options.time_limit_ms = 300;
    auto solved = solvePreparedNesting(f.snapshot, f.options);
    ASSERT_TRUE(solved) << solved.error;
    EXPECT_GE(std::count_if(solved.placements.begin(), solved.placements.end(), [](auto p) { return p.placed; }), 1);
}
TEST(SparrowAcceptance, ComplexShapesKeepNativeAndTiesKeepIncumbent)
{
    for (auto name : {"container_hole", "concave_container", "compound_rigid"}) {
        Fixture f(name);
        EXPECT_FALSE(sparrowEligible(f.snapshot, f.options));
    }
    Fixture f("rectangles");
    EXPECT_TRUE(sparrowEligible(f.snapshot, f.options));
    std::vector<Placement> poses;
    for (auto const &p : f.snapshot.parts)
        poses.push_back({.part_id = p.id});
    EXPECT_FALSE(betterNestingCandidate(f.snapshot, poses, poses));
    f.options.time_limit_ms = 0;
    EXPECT_FALSE(sparrowEligible(f.snapshot, f.options));
}
} // namespace
