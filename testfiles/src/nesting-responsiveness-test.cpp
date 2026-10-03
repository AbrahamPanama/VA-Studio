// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Nesting responsiveness measurements (consolidated nesting work order, R0 and
 * R5a). Records timings with RecordProperty; assertions are status and generous
 * ceilings only. Label nesting-perf; not a critical test. Set
 * VACARDS_NESTING_PERF_DIR to also write one TSV per measurement.
 */

#include "nesting/nesting-document.h"
#include "nesting/sparrow-adapter.h"
#include "ui/tools/nesting-tool.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <glibmm/main.h>
#include <gtest/gtest.h>

#include "desktop.h"
#include "document-undo.h"
#include "document.h"
#include "inkscape-application.h"
#include "inkscape.h"
#include "object/sp-item.h"
#include "preferences.h"
#include "selection.h"
#include "ui/widget/events/canvas-event.h"
#include "xml/repr.h"

using namespace Inkscape;
using namespace Inkscape::Nesting;
using namespace Inkscape::UI::Tools;

namespace {

using Clock = std::chrono::steady_clock;

double ms_since(Clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

std::filesystem::path fixtures()
{
    return NESTING_RESPONSIVENESS_FIXTURES;
}

double percentile(std::vector<double> values, double p)
{
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    // Nearest rank.
    auto const rank = static_cast<std::size_t>(std::ceil(p * values.size()));
    return values[std::clamp<std::size_t>(rank, 1, values.size()) - 1];
}

void write_tsv(std::string const &name, std::string const &text)
{
    auto const *dir = g_getenv("VACARDS_NESTING_PERF_DIR");
    if (!dir || !*dir) {
        return;
    }
    std::filesystem::create_directories(dir);
    std::ofstream(std::filesystem::path(dir) / (name + ".tsv")) << text;
}

std::string format(double value)
{
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(1);
    out << value;
    return out.str();
}

// --- R0: document-level prepare / solve / apply --------------------------------

Options measurement_options()
{
    Options options;
    options.quality = Quality::Draft;
    options.time_limit_ms = 3000;
    options.part_spacing = 3.78;
    options.container_margin = 18.9;
    options.random_seed = 17;
    return options;
}

struct Measurement
{
    double prepare_ms = 0.0;
    double capture_ms = 0.0; // R1: GTK-thread share of prepare_ms
    double solve_ms = 0.0;
    double apply_ms = 0.0;
    double validation_ms = 0.0;
    std::size_t revalidated = 0;
    std::size_t placed = 0;
    std::size_t parts = 0;
    bool ok = false;
};

Measurement measure_once(std::string const &name)
{
    Measurement run;
    auto const path = fixtures() / (name + ".svg");
    if (!std::filesystem::exists(path)) {
        ADD_FAILURE() << "fixture " << path << " is missing: run through ctest (fixture setup "
                      << "test_nesting-responsiveness-fixtures) or testfiles/nesting/responsiveness/make_fixtures.py";
        return run;
    }
    auto document = SPDocument::createNewDoc(path.string().c_str());
    if (!document) {
        ADD_FAILURE() << "cannot open " << path;
        return run;
    }
    document->ensureUpToDate();
    std::vector<SPItem *> parts;
    std::ifstream ids(fixtures() / (name + ".ids.txt"));
    for (std::string id; ids >> id;) {
        if (auto *part = cast<SPItem>(document->getObjectById(id))) {
            parts.push_back(part);
        }
    }
    run.parts = parts.size();
    auto *sheet = cast<SPItem>(document->getObjectById("sheet"));
    if (!sheet || parts.empty()) {
        ADD_FAILURE() << name << ": missing sheet or parts";
        return run;
    }

    auto started = Clock::now();
    auto prepared = prepareDocumentNesting(sheet, parts);
    run.prepare_ms = ms_since(started);
    if (!prepared) {
        ADD_FAILURE() << name << ": preparation failed: " << prepared.error;
        return run;
    }
    run.capture_ms = prepared.snapshot->metrics.capture_seconds * 1000.0;

    started = Clock::now();
    auto solved = solvePreparedNesting(*prepared.snapshot, measurement_options());
    run.solve_ms = ms_since(started);
    if (!solved) {
        ADD_FAILURE() << name << ": solve failed: " << solved.error;
        return run;
    }
    run.placed = static_cast<std::size_t>(
        std::count_if(solved.placements.begin(), solved.placements.end(), [](auto const &p) { return p.placed; }));

    started = Clock::now();
    auto applied = applyNestingPlacements(*prepared.snapshot, solved.placements);
    run.apply_ms = ms_since(started);
    run.validation_ms = applied.validation_seconds * 1000.0;
    run.revalidated = applied.revalidated_count;
    run.ok = applied.status == ApplyStatus::Applied;
    if (!run.ok) {
        ADD_FAILURE() << name << ": apply failed: " << applied.error;
        return run;
    }
    // R2: nothing changed during the run and no fixture has text or external
    // references, so nothing is re-prepared (M4 measured parts + 1 here).
    EXPECT_EQ(applied.revalidated_count, 0u) << name;
    return run;
}

class NestingResponsivenessTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        if (!Application::exists()) {
            Application::create(false);
        }
    }

    void measure(std::string const &name)
    {
        std::vector<Measurement> runs;
        for (int i = 0; i < 3; ++i) {
            runs.push_back(measure_once(name));
            if (!runs.back().ok) {
                return;
            }
        }
        auto median = [&](auto field) {
            std::vector<double> values;
            for (auto const &run : runs) {
                values.push_back(static_cast<double>(run.*field));
            }
            return percentile(values, 0.5);
        };
        RecordProperty("parts", std::to_string(runs.front().parts));
        RecordProperty("placed", std::to_string(runs.front().placed));
        RecordProperty("prepare_ms", format(median(&Measurement::prepare_ms)));
        RecordProperty("capture_ms", format(median(&Measurement::capture_ms)));
        RecordProperty("solve_ms", format(median(&Measurement::solve_ms)));
        RecordProperty("apply_ms", format(median(&Measurement::apply_ms)));
        RecordProperty("validation_ms", format(median(&Measurement::validation_ms)));
        RecordProperty("revalidated_count", std::to_string(runs.front().revalidated));

        std::string tsv =
            "run\tparts\tplaced\tprepare_ms\tsolve_ms\tapply_ms\tvalidation_ms\trevalidated_count\tcapture_ms\n";
        for (std::size_t i = 0; i < runs.size(); ++i) {
            auto const &r = runs[i];
            tsv += std::to_string(i + 1) + "\t" + std::to_string(r.parts) + "\t" + std::to_string(r.placed) + "\t" +
                   format(r.prepare_ms) + "\t" + format(r.solve_ms) + "\t" + format(r.apply_ms) + "\t" +
                   format(r.validation_ms) + "\t" + std::to_string(r.revalidated) + "\t" + format(r.capture_ms) + "\n";
        }
        tsv += "median\t\t\t" + format(median(&Measurement::prepare_ms)) + "\t" + format(median(&Measurement::solve_ms)) + "\t" +
               format(median(&Measurement::apply_ms)) + "\t" + format(median(&Measurement::validation_ms)) + "\t\n";
        write_tsv(name, tsv);
        std::cout << "R0 " << name << "\n" << tsv;
    }
};

TEST_F(NestingResponsivenessTest, VectorCards200)
{
    measure("vector_cards_200");
}

TEST_F(NestingResponsivenessTest, DetailedCurves240)
{
    measure("detailed_curves_240");
}

TEST_F(NestingResponsivenessTest, StickersBitmap50)
{
    measure("stickers_bitmap_50");
}

TEST_F(NestingResponsivenessTest, StrokedPaths100)
{
    measure("stroked_paths_100");
}

// --- R5a: tool switch / stop latency ------------------------------------------

InkscapeApplication *initialize_gui()
{
    static auto *application = [] {
        g_setenv("INKSCAPE_APP_ID_TAG", "nestingresponsivenesstest", TRUE);
        return new InkscapeApplication(); // Process-lifetime test fixture.
    }();
    return application->gtk_app() ? application : nullptr;
}

void drain_main_context()
{
    auto context = Glib::MainContext::get_default();
    while (context->iteration(false)) {
    }
}

bool wait_until(std::function<bool()> const &predicate, std::chrono::milliseconds timeout)
{
    auto const deadline = Clock::now() + timeout;
    do {
        drain_main_context();
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (Clock::now() < deadline);
    drain_main_context();
    return predicate();
}

// 40 unique 1500-vertex closed curves (the make_spaced_curves.py formula) beside a
// 1500 x 1500 sheet.
std::string unique_curves_svg()
{
    std::string svg = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="4000" height="1600">)svg"
                      R"svg(<rect id="sheet" x="0" y="0" width="1500" height="1500" fill="none" stroke="#888"/>)svg";
    char buffer[64];
    for (int index = 0; index < 40; ++index) {
        double const radius = 40.0 * (1.0 + 0.002 * index);
        double const cx = 1700.0 + 200.0 * (index % 10);
        double const cy = 100.0 + 200.0 * (index / 10);
        svg += "<path id=\"part-" + std::to_string(index + 1) + "\" fill=\"#000\" d=\"M";
        for (int k = 0; k < 1500; ++k) {
            double const a = 2.0 * M_PI * k / 1500;
            double const r =
                radius * (1.0 + 0.15 * std::sin(5 * a) + 0.04 * std::cos(23 * a) + 0.01 * std::sin(71 * a));
            std::snprintf(buffer, sizeof buffer, " %.3f,%.3f", cx + r * std::cos(a), cy + r * std::sin(a));
            svg += buffer;
        }
        svg += " Z\"/>";
    }
    return svg + "</svg>";
}

class NestingStopLatencyTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        if (!initialize_gui()) {
            GTEST_SKIP() << "GTK display unavailable; stop-latency measurement skipped";
        }
        if (!Application::exists()) {
            Application::create(false);
        }
        auto *preferences = Preferences::get();
        saved_time_limit_ms = preferences->getInt("/tools/nesting/time_limit_ms", 5000);
        saved_part_spacing = preferences->getDouble("/tools/nesting/part_spacing", 0.0);
    }

    void TearDown() override
    {
        desktop.reset();
        document.reset();
        drain_main_context();
        auto *preferences = Preferences::get();
        preferences->setInt("/tools/nesting/time_limit_ms", saved_time_limit_ms);
        preferences->setDouble("/tools/nesting/part_spacing", saved_part_spacing);
    }

    void load(std::unique_ptr<SPDocument> doc, std::vector<std::string> const &ids)
    {
        desktop.reset();
        document = std::move(doc);
        ASSERT_TRUE(document);
        document->ensureUpToDate();
        desktop = std::make_unique<SPDesktop>(document->getNamedView());
        std::vector<SPItem *> parts;
        for (auto const &id : ids) {
            auto *part = cast<SPItem>(document->getObjectById(id));
            ASSERT_TRUE(part) << id;
            parts.push_back(part);
        }
        desktop->getSelection()->setList(parts);
    }

    /// One trial: start nesting, wait for `ready` (given the time nest_into
    /// returned), then switch to the Select tool and return how long the switch
    /// (including the nesting tool's destructor) took, in milliseconds.
    /// Negative when the trial could not reach its switch point.
    double trial(SPItem *container, std::function<bool(NestingTool &, Clock::time_point)> const &ready)
    {
        desktop->setTool("/tools/nesting");
        auto *tool = dynamic_cast<NestingTool *>(desktop->getTool());
        if (!tool || !tool->nest_into(container)) {
            return -1.0;
        }
        auto const started_nesting = Clock::now();
        // Re-fetch the tool on every poll: a solve that ends on its own returns to
        // the Select tool and destroys the nesting tool.
        auto const reached = wait_until(
            [&] {
                auto *current = dynamic_cast<NestingTool *>(desktop->getTool());
                return current == tool && current->is_solving() && ready(*current, started_nesting);
            },
            std::chrono::seconds(60));
        if (!reached) {
            desktop->setTool("/tools/select");
            return -1.0;
        }
        // R3: the longest single preview update of this run, before it ends.
        preview_ms.push_back(tool->longest_preview_update_seconds() * 1000.0);
        auto const started = Clock::now();
        desktop->setTool("/tools/select");
        double const elapsed = ms_since(started);
        drain_main_context();
        return elapsed;
    }

    void record(std::string const &name, std::vector<double> const &values)
    {
        double const p50 = percentile(values, 0.5);
        double const p95 = percentile(values, 0.95);
        RecordProperty("trials", std::to_string(values.size()));
        RecordProperty("p50_ms", format(p50));
        RecordProperty("p95_ms", format(p95));
        RecordProperty("max_ms", format(*std::max_element(values.begin(), values.end())));
        auto const longest_preview = preview_ms.empty() ? 0.0 : *std::max_element(preview_ms.begin(), preview_ms.end());
        RecordProperty("longest_preview_update_ms", format(longest_preview));
        std::string tsv = "trial\tswitch_ms\tlongest_preview_update_ms\n";
        for (std::size_t i = 0; i < values.size(); ++i) {
            tsv += std::to_string(i + 1) + "\t" + format(values[i]) + "\t" +
                   (i < preview_ms.size() ? format(preview_ms[i]) : std::string{}) + "\n";
        }
        tsv += "p50\t" + format(p50) + "\np95\t" + format(p95) + "\n";
        write_tsv(name, tsv);
        std::cout << "R5a " << name << " p50=" << format(p50) << " ms p95=" << format(p95)
                  << " ms; R3 longest preview update " << format(longest_preview) << " ms\n";
        for (double value : values) {
            EXPECT_LT(value, 1000.0) << name << ": a tool switch took longer than the 1000 ms ceiling";
        }
    }

    void run_unique_curves(std::string const &name,
                           std::function<bool(NestingTool &, Clock::time_point)> const &ready)
    {
        auto *preferences = Preferences::get();
        preferences->setInt("/tools/nesting/time_limit_ms", 30000);
        preferences->setDouble("/tools/nesting/part_spacing", 3.78);
        std::vector<std::string> ids;
        for (int index = 0; index < 40; ++index) {
            ids.push_back("part-" + std::to_string(index + 1));
        }
        auto const svg = unique_curves_svg();
        std::vector<double> values;
        for (int i = 0; i < 20; ++i) {
            load(SPDocument::createNewDocFromMem(std::span<char const>{svg.data(), svg.size()}), ids);
            double const elapsed = trial(cast<SPItem>(document->getObjectById("sheet")), ready);
            ASSERT_GE(elapsed, 0.0) << name << ": trial " << i + 1 << " did not reach its switch point";
            values.push_back(elapsed);
        }
        record(name, values);
    }

    std::unique_ptr<SPDocument> document;
    std::unique_ptr<SPDesktop> desktop;
    std::vector<double> preview_ms;
    int saved_time_limit_ms = 5000;
    double saved_part_spacing = 0.0;
};

/// The engine is searching: a stage-1 report arrived. Stage 0 ("validating")
/// reports are forwarded unthrottled and come earlier. Placements are not
/// required: the tool's 100 ms throttle can drop the one report that carries the
/// first layout, and later non-improving reports carry none.
bool searching(NestingTool const &tool)
{
    auto const &progress = tool.last_progress();
    return progress && progress->stage == 1;
}

TEST_F(NestingStopLatencyTest, ToolSwitchDuringSolveReturnsPromptly)
{
    run_unique_curves("switch_during_solve", [](NestingTool &tool, Clock::time_point) { return searching(tool); });
}

TEST_F(NestingStopLatencyTest, ToolSwitchDuringEnginePreparationReturnsPromptly)
{
    // 20 ms after nest_into returned: inside engine preparation (40 unique
    // 1500-vertex parts take seconds to prepare), before the search starts.
    run_unique_curves("switch_during_preparation", [](NestingTool &tool, Clock::time_point started) {
        EXPECT_FALSE(searching(tool)) << "the search started before the 20 ms switch point";
        return Clock::now() - started >= std::chrono::milliseconds(20);
    });
}

TEST_F(NestingStopLatencyTest, ToolSwitchDuringSparrowRunReturnsPromptly)
{
    if (!sparrowAvailable()) {
        GTEST_SKIP() << "Sparrow helper not bundled in this build; Sparrow stop latency not measured";
    }
    auto const dir = std::filesystem::path(NESTING_SPARROW_CORPUS) / "circles80";
    std::vector<std::string> ids;
    {
        std::ifstream file(dir / "ids.txt");
        for (std::string id; file >> id;) {
            ids.push_back(id);
        }
    }
    ASSERT_EQ(ids.size(), 80u);
    Preferences::get()->setInt("/tools/nesting/time_limit_ms", 15000);
    std::vector<double> values;
    for (int i = 0; i < 20; ++i) {
        load(SPDocument::createNewDoc((dir / "input.svg").string().c_str()), ids);
        // Progress comes only from the native lane; the Sparrow helper is launched
        // before it. 500 ms after the native search starts the helper is running
        // (not observed directly: recorded as a caveat with the evidence).
        std::optional<Clock::time_point> searching_since;
        double const elapsed = trial(cast<SPItem>(document->getObjectById("container")),
                                     [&](NestingTool &tool, Clock::time_point) {
                                         if (!searching_since && searching(tool)) {
                                             searching_since = Clock::now();
                                         }
                                         return searching_since &&
                                                Clock::now() - *searching_since >= std::chrono::milliseconds(500);
                                     });
        ASSERT_GE(elapsed, 0.0) << "trial " << i + 1 << " did not reach its switch point";
        values.push_back(elapsed);
    }
    record("switch_during_sparrow", values);
}

TEST_F(NestingStopLatencyTest, PreviewUpdatesDuringASolveStayShort)
{
    // R3: 80 parts under the point budget (full outlines), 3 s into the search.
    auto const dir = std::filesystem::path(NESTING_SPARROW_CORPUS) / "circles80";
    std::vector<std::string> ids;
    {
        std::ifstream file(dir / "ids.txt");
        for (std::string id; file >> id;) {
            ids.push_back(id);
        }
    }
    ASSERT_EQ(ids.size(), 80u);
    Preferences::get()->setInt("/tools/nesting/time_limit_ms", 15000);
    load(SPDocument::createNewDoc((dir / "input.svg").string().c_str()), ids);
    preview_ms.clear();
    double const elapsed = trial(cast<SPItem>(document->getObjectById("container")),
                                 [](NestingTool &tool, Clock::time_point started) {
                                     return searching(tool) && tool.preview_document_bounds() &&
                                            Clock::now() - started >= std::chrono::seconds(3);
                                 });
    ASSERT_GE(elapsed, 0.0) << "no preview within 3 s of search (layouts lost to the progress throttle?)";
    ASSERT_EQ(preview_ms.size(), 1u);
    RecordProperty("longest_preview_update_ms", format(preview_ms.front()));
    write_tsv("preview_update_circles80", "longest_preview_update_ms\n" + format(preview_ms.front()) + "\n");
    std::cout << "R3 circles80 longest preview update " << format(preview_ms.front()) << " ms\n";
    EXPECT_LT(preview_ms.front(), 1000.0) << "generous ceiling; the target is 50 ms";
}

TEST_F(NestingStopLatencyTest, NestReturnsBeforeGeometryIsPrepared)
{
    // R1 on the bitmap fixture: nest() only captures; the geometry follows on
    // the worker. Measured against the P1 capture target (100 ms); the ceiling
    // is generous. Esc then cancels within 250 ms without a document change.
    std::vector<std::string> ids;
    {
        std::ifstream file(fixtures() / "stickers_bitmap_50.ids.txt");
        for (std::string id; file >> id;) {
            ids.push_back(id);
        }
    }
    ASSERT_EQ(ids.size(), 50u);
    load(SPDocument::createNewDoc((fixtures() / "stickers_bitmap_50.svg").string().c_str()), ids);
    auto const before = sp_repr_save_buf(document->getReprDoc()).raw();
    Preferences::get()->setInt("/tools/nesting/time_limit_ms", 30000);
    desktop->setTool("/tools/nesting");
    auto *tool = dynamic_cast<NestingTool *>(desktop->getTool());
    ASSERT_TRUE(tool);
    auto const started = Clock::now();
    ASSERT_TRUE(tool->nest_into(cast<SPItem>(document->getObjectById("sheet"))));
    auto const nest_ms = ms_since(started);
    EXPECT_TRUE(tool->is_solving());
    // nest() returned before the geometry existed: the run is still preparing.
    EXPECT_FALSE(tool->preview_document_bounds());
    EXPECT_NE(tool->label_text(NestingTool::Label::Progress).raw().find("Preparing"), std::string::npos)
        << tool->label_text(NestingTool::Label::Progress);
    RecordProperty("nest_return_ms", format(nest_ms));
    std::cout << "R1 stickers_bitmap_50 nest() returned in " << format(nest_ms) << " ms (P1 target 100 ms)\n";
    EXPECT_LT(nest_ms, 1000.0) << "generous ceiling";

    KeyPressEvent escape;
    escape.keyval = GDK_KEY_Escape;
    GdkKeymapKey *keys = nullptr;
    int key_count = 0;
    if (gdk_display_map_keyval(gdk_display_get_default(), GDK_KEY_Escape, &keys, &key_count) && key_count > 0) {
        escape.keycode = keys[0].keycode;
        escape.group = keys[0].group;
    }
    g_free(keys);
    auto const escaped = Clock::now();
    tool->root_handler(escape);
    auto const cancel_ms = ms_since(escaped);
    RecordProperty("cancel_ms", format(cancel_ms));
    std::cout << "R1 stickers_bitmap_50 Esc during preparation cancelled in " << format(cancel_ms) << " ms\n";
    EXPECT_LT(cancel_ms, 250.0);
    EXPECT_FALSE(tool->is_solving());
    drain_main_context();
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before);
    write_tsv("r1_stickers_nest", "nest_return_ms\tcancel_ms\n" + format(nest_ms) + "\t" + format(cancel_ms) + "\n");
}

} // namespace
