// SPDX-License-Identifier: GPL-2.0-or-later

#include "nesting-ffi.h"
#include "vacards_nesting.h"
#include "nesting-test-geometry.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <concepts>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numbers>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using Inkscape::Nesting::Job;
using Inkscape::Nesting::JobState;
using Inkscape::Nesting::Options;
using Inkscape::Nesting::Point;
using Inkscape::Nesting::Status;
using Inkscape::Nesting::CollisionComponent;
using Inkscape::Nesting::Placement;
using Inkscape::Nesting::Quality;
using Inkscape::Nesting::RotationMode;
using Inkscape::Nesting::Test::CanonicalRank;
using Inkscape::Nesting::Test::FixedContainerFixture;
using Inkscape::Nesting::Test::PartGeometry;
using Inkscape::Nesting::Test::canonical_rank;
using Inkscape::Nesting::Test::rank_not_worse;
using Inkscape::Nesting::Test::ranks_equivalent;
using Inkscape::Nesting::Test::transformed;
using Inkscape::Nesting::Test::validate_placements;

auto square(double size) -> std::vector<Point>
{
    return {{0.0, 0.0}, {size, 0.0}, {size, size}, {0.0, size}};
}

auto check(bool condition, char const *message) -> bool
{
    if (!condition) {
        std::cerr << message << '\n';
    }
    return condition;
}

auto rectangle(double x, double y, double width, double height) -> std::vector<Point>
{
    return {{x, y}, {x + width, y}, {x + width, y + height}, {x, y + height}};
}

auto component(std::vector<Point> outer, std::vector<std::vector<Point>> holes = {}) -> CollisionComponent
{
    return {.outer = std::move(outer), .holes = std::move(holes)};
}

auto part(std::uint64_t id, std::vector<Point> outer) -> PartGeometry
{
    return {.id = id, .components = {component(std::move(outer))}};
}

auto placed_part(std::uint64_t id, double x, double y, double rotation = 0.0) -> Placement
{
    return {
        .part_id = id,
        .translation_x = x,
        .translation_y = y,
        .rotation_degrees = rotation,
        .placed = true,
    };
}

auto unplaced_part(std::uint64_t id) -> Placement
{
    return {.part_id = id};
}

auto gate_options(RotationMode rotation = RotationMode::None) -> Options
{
    Options options;
    options.quality = Quality::Draft;
    options.rotation_mode = rotation;
    options.random_seed = 0x5eed'1234'89ab'cdefULL;
    options.time_limit_ms = 0;
    options.worker_count = 1;
    return options;
}

struct AdversarialFixture
{
    std::string_view trap_class;
    FixedContainerFixture fixture;
    std::vector<Placement> known_better;
    std::vector<Placement> known_blocked;
};

auto adversarial_fixtures() -> std::vector<AdversarialFixture>
{
    std::vector<AdversarialFixture> fixtures;

    // Six mutually exclusive, high-priority blockers precede the two parts
    // whose combined area is greater. Size-, area-, and input-order passes all
    // admit a blocker first; an insertion/ejection neighborhood can replace
    // it with the two useful parts.
    fixtures.push_back({
        .trap_class = "order-trap",
        .fixture = {
            .id = "adversarial-order-trap",
            .container = rectangle(0, 0, 30, 20),
            .parts = {
                part(91'001, rectangle(0, 0, 18, 12)),
                part(91'002, rectangle(0, 0, 18, 12)),
                part(91'003, rectangle(0, 0, 18, 12)),
                part(91'004, rectangle(0, 0, 18, 12)),
                part(91'005, rectangle(0, 0, 18, 12)),
                part(91'006, rectangle(0, 0, 18, 12)),
                part(91'007, rectangle(0, 0, 14, 10)),
                part(91'008, rectangle(0, 0, 14, 10)),
            },
            .options = gate_options(),
            .minimum_placed = 1,
        },
        .known_better = {
            unplaced_part(91'001),
            unplaced_part(91'002),
            unplaced_part(91'003),
            unplaced_part(91'004),
            unplaced_part(91'005),
            unplaced_part(91'006),
            placed_part(91'007, 0.5, 0.5),
            placed_part(91'008, 15.5, 0.5),
        },
        .known_blocked = {
            placed_part(91'001, 0.5, 0.5),
            unplaced_part(91'002),
            unplaced_part(91'003),
            unplaced_part(91'004),
            unplaced_part(91'005),
            unplaced_part(91'006),
            unplaced_part(91'007),
            unplaced_part(91'008),
        },
    });

    // The narrow orientation wins the constructive translation score, but it
    // consumes the full-height lane. Rotating the first part into the wider,
    // shorter orientation creates a second lane for the other part.
    fixtures.push_back({
        .trap_class = "rotation-trap",
        .fixture = {
            .id = "adversarial-rotation-trap",
            .container = rectangle(0, 0, 24, 30),
            .parts = {
                part(92'001, rectangle(0, 0, 14, 24)),
                part(92'002, rectangle(0, 0, 22, 15)),
            },
            .options = gate_options(RotationMode::RightAngles),
            .minimum_placed = 1,
        },
        .known_better = {
            placed_part(92'001, 24.0, 0.5, 90.0),
            placed_part(92'002, 1.0, 15.0),
        },
        .known_blocked = {
            placed_part(92'001, 0.5, 0.5),
            unplaced_part(92'002),
        },
    });

    // A rectangular hole creates four narrow lanes inside a concave outer
    // boundary. Vertical parts greedily occupy the two lower corners and
    // block the bottom horizontal lane. Ruin/recreate can move two verticals
    // beside the hole and recover both horizontal lanes.
    fixtures.push_back({
        .trap_class = "concave-hole-blocker-trap",
        .fixture = {
            .id = "adversarial-concave-hole-blocker-trap",
            .container = {{0, 0}, {30, 0}, {30, 20}, {34, 20},
                          {34, 30}, {30, 30}, {30, 50}, {0, 50}},
            .container_holes = {rectangle(10, 10, 10, 30)},
            .parts = {
                part(93'001, rectangle(0, 0, 9, 29)),
                part(93'002, rectangle(0, 0, 9, 29)),
                part(93'003, rectangle(0, 0, 9, 29)),
                part(93'004, rectangle(0, 0, 9, 29)),
                part(93'005, rectangle(0, 0, 9, 29)),
                part(93'006, rectangle(0, 0, 9, 29)),
                part(93'007, rectangle(0, 0, 29, 9)),
                part(93'008, rectangle(0, 0, 29, 9)),
            },
            .options = gate_options(),
            .minimum_placed = 3,
        },
        .known_better = {
            placed_part(93'001, 0.5, 10.5),
            placed_part(93'002, 20.5, 10.5),
            unplaced_part(93'003),
            unplaced_part(93'004),
            unplaced_part(93'005),
            unplaced_part(93'006),
            placed_part(93'007, 0.5, 0.5),
            placed_part(93'008, 0.5, 40.5),
        },
        .known_blocked = {
            placed_part(93'001, 0.5, 0.5),
            placed_part(93'002, 20.5, 0.5),
            unplaced_part(93'003),
            unplaced_part(93'004),
            unplaced_part(93'005),
            unplaced_part(93'006),
            unplaced_part(93'007),
            placed_part(93'008, 0.5, 40.5),
        },
    });

    // Each compound is one rigid payload with two disconnected islands. Its
    // bottom-left placement blocks a long part in the U's lower corridor;
    // moving one intact compound into the two upper arms admits both payloads.
    auto rigid_compound = [](std::uint64_t id) {
        return PartGeometry{
            .id = id,
            .components = {
                component(rectangle(0, 0, 9, 18)),
                component(rectangle(30, 0, 9, 18)),
            },
        };
    };
    fixtures.push_back({
        .trap_class = "rigid-compound-trap",
        .fixture = {
            .id = "adversarial-rigid-compound-trap",
            .container = {{0, 0}, {40, 0}, {40, 30}, {30, 30},
                          {30, 10}, {10, 10}, {10, 30}, {0, 30}},
            .parts = {
                rigid_compound(94'001),
                rigid_compound(94'002),
                rigid_compound(94'003),
                rigid_compound(94'004),
                rigid_compound(94'005),
                part(94'006, rectangle(0, 0, 39, 8)),
            },
            .options = gate_options(),
            .minimum_placed = 1,
        },
        .known_better = {
            placed_part(94'001, 0.5, 11.0),
            unplaced_part(94'002),
            unplaced_part(94'003),
            unplaced_part(94'004),
            unplaced_part(94'005),
            placed_part(94'006, 0.5, 0.5),
        },
        .known_blocked = {
            placed_part(94'001, 0.5, 0.5),
            unplaced_part(94'002),
            unplaced_part(94'003),
            unplaced_part(94'004),
            unplaced_part(94'005),
            unplaced_part(94'006),
        },
    });

    return fixtures;
}

auto difficult_fixtures() -> std::vector<FixedContainerFixture>
{
    std::vector<FixedContainerFixture> fixtures;

    fixtures.push_back({
        .id = "concave-l-container",
        .container = {{0, 0}, {90, 0}, {90, 24}, {38, 24}, {38, 82}, {0, 82}},
        .parts = {
            part(101, rectangle(120, 0, 30, 18)),
            part(102, {{170, 0}, {194, 0}, {194, 8}, {181, 8}, {181, 24}, {170, 24}}),
            part(103, rectangle(215, 0, 16, 30)),
        },
        .options = gate_options(),
        .minimum_placed = 3,
    });

    fixtures.push_back({
        .id = "irregular-container-hole",
        .container = rectangle(0, 0, 100, 80),
        .container_holes = {rectangle(36, 20, 28, 40)},
        .parts = {
            part(201, rectangle(120, 0, 28, 16)),
            part(202, rectangle(160, 0, 18, 18)),
            part(203, rectangle(190, 0, 12, 32)),
        },
        .options = gate_options(),
        .minimum_placed = 3,
    });

    fixtures.push_back({
        .id = "narrow-right-angle-fit",
        .container = rectangle(0, 0, 52, 22),
        .parts = {
            part(301, rectangle(100, 0, 18, 36)),
            part(302, rectangle(140, 0, 10, 10)),
        },
        .options = gate_options(RotationMode::RightAngles),
        .minimum_placed = 2,
    });

    auto compound_options = gate_options();
    fixtures.push_back({
        .id = "rigid-disconnected-compound",
        .container = rectangle(0, 0, 86, 60),
        .parts = {
            {.id = 401,
             .components = {
                 component(rectangle(110, 0, 10, 10)),
                 component(rectangle(140, 12, 11, 9)),
             }},
            {.id = 402,
             .components = {
                 component(rectangle(180, 0, 24, 24), {rectangle(188, 8, 8, 8)}),
             }},
            part(403, rectangle(220, 0, 17, 14)),
        },
        .options = compound_options,
        .minimum_placed = 3,
    });

    auto clearance_options = gate_options();
    clearance_options.part_spacing = 4.0;
    clearance_options.container_margin = 3.0;
    fixtures.push_back({
        .id = "physical-clearance",
        .container = rectangle(0, 0, 80, 50),
        .parts = {
            part(501, rectangle(100, 0, 20, 15)),
            part(502, rectangle(130, 0, 20, 15)),
        },
        .options = clearance_options,
        .minimum_placed = 2,
    });

    fixtures.push_back({
        .id = "all-unplaced",
        .container = rectangle(0, 0, 10, 10),
        .parts = {
            part(601, rectangle(100, 0, 20, 20)),
            part(602, rectangle(130, 0, 12, 18)),
        },
        .options = gate_options(),
        .minimum_placed = 0,
    });

    for (auto &adversarial : adversarial_fixtures()) {
        fixtures.emplace_back(std::move(adversarial.fixture));
    }

    return fixtures;
}

enum class FixturePartOrder { Original, Reversed };

auto part_order_name(FixturePartOrder order) -> std::string_view
{
    switch (order) {
        case FixturePartOrder::Original:
            return "original";
        case FixturePartOrder::Reversed:
            return "reversed";
    }
    throw std::logic_error("unrecognized fixture part order");
}

auto parse_part_order(std::string_view value) -> FixturePartOrder
{
    if (value == "original") {
        return FixturePartOrder::Original;
    }
    if (value == "reversed") {
        return FixturePartOrder::Reversed;
    }
    throw std::invalid_argument("part order must be 'original' or 'reversed'");
}

void apply_part_order(FixedContainerFixture &fixture, FixturePartOrder order)
{
    if (order == FixturePartOrder::Reversed) {
        std::ranges::reverse(fixture.parts);
    }
}

auto adversarial_trap_classes() -> std::map<std::string, std::string>
{
    std::map<std::string, std::string> result;
    for (auto const &adversarial : adversarial_fixtures()) {
        result.emplace(adversarial.fixture.id, adversarial.trap_class);
    }
    return result;
}

auto is_approved_adversarial_trap(std::string_view trap_class) -> bool
{
    return trap_class == "order-trap" || trap_class == "rotation-trap" ||
           trap_class == "concave-hole-blocker-trap" || trap_class == "rigid-compound-trap";
}

auto configure(Job &job, FixedContainerFixture const &fixture) -> bool
{
    if (job.setContainer(fixture.container) != Status::Ok) {
        std::cerr << fixture.id << ": container rejected: " << job.error() << '\n';
        return false;
    }
    for (auto const &hole : fixture.container_holes) {
        if (job.addContainerHole(hole) != Status::Ok) {
            std::cerr << fixture.id << ": container hole rejected: " << job.error() << '\n';
            return false;
        }
    }
    for (auto const &candidate : fixture.parts) {
        if (job.addPart(candidate.id, candidate.components) != Status::Ok) {
            std::cerr << fixture.id << ": part " << candidate.id << " rejected: " << job.error() << '\n';
            return false;
        }
    }
    return true;
}

struct FirstIncumbent
{
    Status status = Status::InternalError;
    std::vector<Placement> placements;
    CanonicalRank rank;
    std::chrono::steady_clock::duration wall_time{};
    double first_incumbent_ms = std::numeric_limits<double>::quiet_NaN();
    std::uint64_t last_iteration = 0;
    unsigned callback_count = 0;
    bool progress_valid = true;
};

auto first_incumbent(FixedContainerFixture fixture) -> FirstIncumbent
{
    fixture.options.time_limit_ms = 0;
    Job job(fixture.options);
    FirstIncumbent result;
    if (!configure(job, fixture)) {
        result.progress_valid = false;
        return result;
    }

    double last_elapsed = 0.0;
    std::optional<CanonicalRank> last_rank;
    auto const started = std::chrono::steady_clock::now();
    result.status = job.run([&](auto const &progress) {
        ++result.callback_count;
        if (progress.iteration < result.last_iteration || progress.elapsed_seconds < last_elapsed ||
            progress.total_count != fixture.parts.size() || progress.placed_count > progress.total_count ||
            progress.stage > 2) {
            result.progress_valid = false;
        }
        result.last_iteration = progress.iteration;
        last_elapsed = progress.elapsed_seconds;
        if (progress.placements.empty()) {
            return;
        }
        if (auto error = validate_placements(fixture, progress.placements)) {
            std::cerr << fixture.id << ": invalid ABI progress snapshot: " << *error << '\n';
            result.progress_valid = false;
            job.cancel();
            return;
        }
        auto const rank = canonical_rank(fixture, progress.placements);
        if (rank.placed_count != progress.placed_count ||
            std::abs(rank.placed_area - progress.best_score) >
                std::max(1.0, std::abs(rank.placed_area)) * 1.0e-8 ||
            (last_rank && !rank_not_worse(rank, *last_rank))) {
            result.progress_valid = false;
        }
        last_rank = rank;
        if (result.placements.empty()) {
            result.first_incumbent_ms = std::chrono::duration<double, std::milli>(
                                            std::chrono::steady_clock::now() - started)
                                            .count();
            result.placements = progress.placements;
            result.rank = rank;
            job.cancel();
        }
    });
    result.wall_time = std::chrono::steady_clock::now() - started;
    // Keep the callback-owned copy as the object under test. Whether a
    // cancelled job retains a terminal incumbent belongs to the explicit job
    // contract test below, not to these snapshot/metamorphic helpers.
    return result;
}

void transform_points(std::vector<Point> &points, double scale, double dx, double dy)
{
    for (auto &point : points) {
        point.x = point.x * scale + dx;
        point.y = point.y * scale + dy;
    }
}

auto transformed_fixture(FixedContainerFixture fixture, double scale, double dx, double dy)
    -> FixedContainerFixture
{
    transform_points(fixture.container, scale, dx, dy);
    for (auto &hole : fixture.container_holes) {
        transform_points(hole, scale, dx, dy);
    }
    for (auto &candidate : fixture.parts) {
        for (auto &part_component : candidate.components) {
            transform_points(part_component.outer, scale, dx, dy);
            for (auto &hole : part_component.holes) {
                transform_points(hole, scale, dx, dy);
            }
        }
    }
    fixture.options.part_spacing *= scale;
    fixture.options.container_margin *= scale;
    return fixture;
}

auto reversed_winding(FixedContainerFixture fixture) -> FixedContainerFixture
{
    std::ranges::reverse(fixture.container);
    for (auto &hole : fixture.container_holes) {
        std::ranges::reverse(hole);
    }
    for (auto &candidate : fixture.parts) {
        for (auto &part_component : candidate.components) {
            std::ranges::reverse(part_component.outer);
            for (auto &hole : part_component.holes) {
                std::ranges::reverse(hole);
            }
        }
    }
    return fixture;
}

auto placements_by_id(std::span<Placement const> placements) -> std::map<std::uint64_t, Placement>
{
    std::map<std::uint64_t, Placement> result;
    for (auto const &placement : placements) {
        result.emplace(placement.part_id, placement);
    }
    return result;
}

auto placement_near(Placement const &left, Placement const &right, double tolerance) -> bool
{
    return left.part_id == right.part_id && left.placed == right.placed &&
           std::abs(left.translation_x - right.translation_x) <= tolerance &&
           std::abs(left.translation_y - right.translation_y) <= tolerance &&
           std::abs(left.rotation_degrees - right.rotation_degrees) <= tolerance;
}

auto check_authoritative_fixture_gates() -> bool
{
    bool passed = true;
    for (auto const &fixture : difficult_fixtures()) {
        auto const result = first_incumbent(fixture);
        passed &= check(result.status == Status::Cancelled, "first-incumbent run did not cancel transactionally");
        passed &= check(result.progress_valid, "ABI progress failed authoritative validation");
        passed &= check(!result.placements.empty(), "solver did not publish a complete first incumbent");
        if (result.placements.empty()) {
            continue;
        }
        if (auto error = validate_placements(fixture, result.placements)) {
            std::cerr << fixture.id << ": authoritative post-validation failed: " << *error << '\n';
            passed = false;
        }
        auto const placed = static_cast<std::size_t>(std::ranges::count_if(
            result.placements, [](auto const &placement) { return placement.placed; }));
        if (placed < fixture.minimum_placed) {
            std::cerr << fixture.id << ": placed " << placed << ", expected at least " << fixture.minimum_placed
                      << '\n';
            passed = false;
        }
        if (fixture.id == "all-unplaced" && placed != 0) {
            std::cerr << "oversized all-unplaced fixture unexpectedly placed a part\n";
            passed = false;
        }
    }
    return passed;
}

auto check_validator_rejects_malformed_output() -> bool
{
    auto fixture = FixedContainerFixture{
        .id = "validator-negative-control",
        .container = rectangle(0, 0, 50, 50),
        .parts = {
            part(701, rectangle(100, 0, 10, 10)),
            part(702, rectangle(120, 0, 10, 10)),
        },
        .options = gate_options(),
        .minimum_placed = 2,
    };
    std::vector<Placement> overlap{
        {.part_id = 701, .translation_x = -95, .translation_y = 5, .placed = true},
        {.part_id = 702, .translation_x = -115, .translation_y = 5, .placed = true},
    };
    auto outside = overlap;
    outside[0].translation_x = -111;
    auto wrong_id = overlap;
    wrong_id[1].part_id = 999;
    auto non_finite = overlap;
    non_finite[0].translation_x = std::numeric_limits<double>::infinity();

    bool passed = true;
    passed &= check(validate_placements(fixture, overlap).has_value(), "validator accepted overlapping placements");
    passed &= check(validate_placements(fixture, outside).has_value(), "validator accepted a boundary violation");
    passed &= check(validate_placements(fixture, wrong_id).has_value(), "validator accepted reordered/unknown IDs");
    passed &= check(validate_placements(fixture, non_finite).has_value(), "validator accepted a non-finite transform");
    return passed;
}

auto check_legal_contact_and_clearance_semantics() -> bool
{
    bool passed = true;

    auto exact_fill = FixedContainerFixture{
        .id = "legal-container-contact",
        .container = rectangle(0, 0, 20, 10),
        .parts = {part(751, rectangle(0, 0, 20, 10))},
        .options = gate_options(),
        .minimum_placed = 1,
    };
    std::vector<Placement> exact_fill_placement{{.part_id = 751, .placed = true}};
    passed &= check(!validate_placements(exact_fill, exact_fill_placement).has_value(),
                    "zero-clearance part touching the container boundary was rejected");

    auto touching_parts = FixedContainerFixture{
        .id = "legal-part-contact",
        .container = rectangle(0, 0, 20, 10),
        .parts = {
            part(752, rectangle(0, 0, 10, 10)),
            part(753, rectangle(10, 0, 10, 10)),
        },
        .options = gate_options(),
        .minimum_placed = 2,
    };
    std::vector<Placement> touching_placements{
        {.part_id = 752, .placed = true},
        {.part_id = 753, .placed = true},
    };
    passed &= check(!validate_placements(touching_parts, touching_placements).has_value(),
                    "zero-spacing parts sharing an edge were rejected");

    auto overlapping_placements = touching_placements;
    overlapping_placements[1].translation_x = -1.0;
    passed &= check(validate_placements(touching_parts, overlapping_placements).has_value(),
                    "positive-area overlap was mistaken for legal contact");

    auto spaced_parts = touching_parts;
    spaced_parts.options.part_spacing = 2.0;
    passed &= check(validate_placements(spaced_parts, touching_placements).has_value(),
                    "contact was accepted despite non-zero part spacing");

    auto hole_contact = FixedContainerFixture{
        .id = "legal-hole-contact",
        .container = rectangle(0, 0, 30, 20),
        .container_holes = {rectangle(10, 5, 10, 10)},
        .parts = {part(754, rectangle(0, 5, 10, 10))},
        .options = gate_options(),
        .minimum_placed = 1,
    };
    std::vector<Placement> hole_contact_placement{{.part_id = 754, .placed = true}};
    passed &= check(!validate_placements(hole_contact, hole_contact_placement).has_value(),
                    "zero-clearance contact with a hole boundary was rejected");

    auto covering_hole = hole_contact;
    covering_hole.parts.front() = part(754, rectangle(10, 5, 10, 10));
    passed &= check(validate_placements(covering_hole, hole_contact_placement).has_value(),
                    "a part occupying a container hole was accepted as boundary contact");

    auto margin_wall = FixedContainerFixture{
        .id = "container-margin-wall-contract",
        .container = rectangle(0, 0, 20, 20),
        .parts = {part(755, rectangle(3, 5, 5, 5))},
        .options = gate_options(),
        .minimum_placed = 1,
    };
    margin_wall.options.part_spacing = 4.0;
    margin_wall.options.container_margin = 3.0;
    std::vector<Placement> margin_wall_placement{{.part_id = 755, .placed = true}};
    passed &= check(!validate_placements(margin_wall, margin_wall_placement).has_value(),
                    "a part exactly at the sheet margin was rejected");
    margin_wall.parts.front() = part(755, rectangle(2.5, 5, 5, 5));
    passed &= check(validate_placements(margin_wall, margin_wall_placement).has_value(),
                    "a part inside the sheet margin was accepted");

    return passed;
}

auto check_metamorphic_gates() -> bool
{
    auto fixture = FixedContainerFixture{
        .id = "metamorphic-base",
        .container = rectangle(0, 0, 84, 54),
        .parts = {
            part(801, rectangle(110, 5, 24, 13)),
            part(802, rectangle(150, 7, 19, 11)),
            part(803, {{190, 4}, {208, 4}, {208, 10}, {200, 10}, {200, 22}, {190, 22}}),
        },
        .options = gate_options(),
        .minimum_placed = 3,
    };
    auto const baseline = first_incumbent(fixture);
    bool passed = check(baseline.status == Status::Cancelled && baseline.progress_valid &&
                            baseline.placements.size() == fixture.parts.size(),
                        "metamorphic baseline did not produce a valid incumbent");
    if (baseline.placements.size() != fixture.parts.size()) {
        return false;
    }

    auto const repeated = first_incumbent(fixture);
    passed &= check(repeated.placements.size() == baseline.placements.size(),
                    "seeded repeatability changed result cardinality");
    if (repeated.placements.size() == baseline.placements.size()) {
        for (std::size_t index = 0; index < baseline.placements.size(); ++index) {
            passed &= check(placement_near(baseline.placements[index], repeated.placements[index], 1.0e-9),
                            "same seed and single worker changed the first incumbent");
        }
    }

    constexpr double dx = 137.0;
    constexpr double dy = -61.0;
    auto translated_input = transformed_fixture(fixture, 1.0, dx, dy);
    auto const translated_result = first_incumbent(translated_input);
    passed &= check(ranks_equivalent(baseline.rank, translated_result.rank),
                    "translation changed canonical objective rank");
    if (translated_result.placements.size() == baseline.placements.size()) {
        for (std::size_t part_index = 0; part_index < fixture.parts.size(); ++part_index) {
            auto const &base_placement = baseline.placements[part_index];
            auto const &translated_placement = translated_result.placements[part_index];
            passed &= check(base_placement.placed == translated_placement.placed,
                            "translation changed placement admission");
            if (!base_placement.placed) {
                continue;
            }
            auto const base_point = transformed(fixture.parts[part_index].components[0].outer[0], base_placement);
            auto const shifted_point =
                transformed(translated_input.parts[part_index].components[0].outer[0], translated_placement);
            passed &= check(std::abs(shifted_point.x - (base_point.x + dx)) <= 2.0e-3 &&
                                std::abs(shifted_point.y - (base_point.y + dy)) <= 2.0e-3,
                            "translation metamorphism changed physical placement");
        }
    } else {
        passed = false;
    }

    constexpr double scale = 10.0;
    auto scaled_input = transformed_fixture(fixture, scale, 0.0, 0.0);
    auto const scaled_result = first_incumbent(scaled_input);
    passed &= check(scaled_result.placements.size() == baseline.placements.size(),
                    "uniform scale changed result cardinality");
    passed &= check(scaled_result.rank.placed_count == baseline.rank.placed_count &&
                        std::abs(scaled_result.rank.placed_area - baseline.rank.placed_area * scale * scale) <= 0.05,
                    "uniform scale changed admission or area rank");
    if (scaled_result.placements.size() == baseline.placements.size()) {
        for (std::size_t part_index = 0; part_index < fixture.parts.size(); ++part_index) {
            auto const &base_placement = baseline.placements[part_index];
            auto const &scaled_placement = scaled_result.placements[part_index];
            if (!base_placement.placed || !scaled_placement.placed) {
                passed &= check(base_placement.placed == scaled_placement.placed,
                                "uniform scale changed placement admission");
                continue;
            }
            auto const base_point = transformed(fixture.parts[part_index].components[0].outer[0], base_placement);
            auto const scaled_point = transformed(scaled_input.parts[part_index].components[0].outer[0], scaled_placement);
            passed &= check(std::abs(scaled_point.x - base_point.x * scale) <= 0.03 &&
                                std::abs(scaled_point.y - base_point.y * scale) <= 0.03,
                            "uniform scale metamorphism changed physical placement");
        }
    }

    auto reversed_input = reversed_winding(fixture);
    auto const reversed_result = first_incumbent(reversed_input);
    passed &= check(reversed_result.progress_valid && ranks_equivalent(baseline.rank, reversed_result.rank),
                    "winding reversal changed validity or canonical rank");

    auto reordered_input = fixture;
    std::ranges::reverse(reordered_input.parts);
    auto const reordered_result = first_incumbent(reordered_input);
    passed &= check(reordered_result.progress_valid && ranks_equivalent(baseline.rank, reordered_result.rank),
                    "input order changed canonical rank");
    auto const baseline_by_id = placements_by_id(baseline.placements);
    auto const reordered_by_id = placements_by_id(reordered_result.placements);
    passed &= check(baseline_by_id.size() == reordered_by_id.size(), "input order lost or duplicated stable IDs");
    for (auto const &[id, placement] : baseline_by_id) {
        auto const found = reordered_by_id.find(id);
        passed &= check(found != reordered_by_id.end() && found->second.placed == placement.placed,
                        "input order changed the admitted stable-ID set");
    }

    auto duplicate_input = fixture;
    auto duplicate = duplicate_input.parts.front();
    duplicate.id = 899;
    duplicate_input.parts.push_back(std::move(duplicate));
    auto const duplicate_result = first_incumbent(duplicate_input);
    passed &= check(duplicate_result.progress_valid &&
                        duplicate_result.placements.size() == duplicate_input.parts.size(),
                    "duplicate geometry collapsed result identity or cardinality");
    if (auto error = validate_placements(duplicate_input, duplicate_result.placements)) {
        std::cerr << "duplicate-geometry metamorphism failed validation: " << *error << '\n';
        passed = false;
    }

    return passed;
}

struct CompletionAudit
{
    Status status = Status::InternalError;
    std::vector<Placement> placements;
    std::vector<CanonicalRank> published_ranks;
    std::chrono::steady_clock::duration wall_time{};
    double first_incumbent_ms = std::numeric_limits<double>::quiet_NaN();
    std::uint64_t last_iteration = 0;
    double last_elapsed = 0.0;
    double last_best_score = 0.0;
    unsigned callback_count = 0;
    unsigned finalizing_count = 0;
    bool saw_validating = false;
    bool saw_solving = false;
    bool valid = true;
};

auto run_to_deadline(FixedContainerFixture fixture, std::uint64_t budget_ms) -> CompletionAudit
{
    fixture.options.time_limit_ms = budget_ms;
    Job job(fixture.options);
    CompletionAudit audit;
    if (!configure(job, fixture)) {
        audit.valid = false;
        return audit;
    }

    unsigned last_stage = 0;
    auto const started = std::chrono::steady_clock::now();
    audit.status = job.run([&](auto const &progress) {
        ++audit.callback_count;
        if (progress.stage > 2 || progress.stage < last_stage || progress.iteration < audit.last_iteration ||
            progress.elapsed_seconds < audit.last_elapsed || progress.best_score < audit.last_best_score ||
            progress.total_count != fixture.parts.size() || progress.placed_count > progress.total_count) {
            audit.valid = false;
        }
        last_stage = progress.stage;
        audit.last_iteration = progress.iteration;
        audit.last_elapsed = progress.elapsed_seconds;
        audit.last_best_score = progress.best_score;
        audit.saw_validating |= progress.stage == 0;
        audit.saw_solving |= progress.stage == 1;
        audit.finalizing_count += progress.stage == 2 ? 1U : 0U;

        if (progress.placements.empty()) {
            return;
        }
        if (!std::isfinite(audit.first_incumbent_ms)) {
            audit.first_incumbent_ms = std::chrono::duration<double, std::milli>(
                                           std::chrono::steady_clock::now() - started)
                                           .count();
        }
        if (auto error = validate_placements(fixture, progress.placements)) {
            std::cerr << fixture.id << ": invalid published placement snapshot: " << *error << '\n';
            audit.valid = false;
            return;
        }
        auto const rank = canonical_rank(fixture, progress.placements);
        if (rank.placed_count != progress.placed_count ||
            std::abs(rank.placed_area - progress.best_score) >
                std::max(1.0, std::abs(rank.placed_area)) * 1.0e-8 ||
            (!audit.published_ranks.empty() && !rank_not_worse(rank, audit.published_ranks.back()))) {
            audit.valid = false;
        }
        audit.published_ranks.emplace_back(rank);
    });
    audit.wall_time = std::chrono::steady_clock::now() - started;
    audit.placements = job.results();

    if (audit.status == Status::Ok) {
        if (auto error = validate_placements(fixture, audit.placements)) {
            std::cerr << fixture.id << ": invalid final ABI placements: " << *error << '\n';
            audit.valid = false;
        }
        if (!audit.published_ranks.empty() &&
            !rank_not_worse(canonical_rank(fixture, audit.placements), audit.published_ranks.front())) {
            audit.valid = false;
        }
    }
    return audit;
}

auto check_deadline_progress_and_monotonicity() -> bool
{
    auto fixture = FixedContainerFixture{
        .id = "deadline-progress",
        .container = rectangle(0, 0, 90, 54),
        .parts = {
            part(901, rectangle(110, 0, 27, 14)),
            part(902, rectangle(150, 0, 19, 17)),
            part(903, {{185, 0}, {207, 0}, {207, 8}, {198, 8}, {198, 24}, {185, 24}}),
        },
        .options = gate_options(),
        .minimum_placed = 3,
    };
    constexpr std::uint64_t budget_ms = 25;
    std::vector<double> elapsed_ms;
    bool passed = true;
    for (unsigned repetition = 0; repetition < 5; ++repetition) {
        auto const audit = run_to_deadline(fixture, budget_ms);
        auto const wall_ms = std::chrono::duration<double, std::milli>(audit.wall_time).count();
        elapsed_ms.emplace_back(wall_ms);
        passed &= check(audit.status == Status::Ok, "finite deadline did not complete successfully");
        passed &= check(audit.valid, "deadline run violated progress or placement invariants");
        passed &= check(audit.saw_validating && audit.saw_solving && audit.finalizing_count == 1,
                        "progress stages were missing, reordered, or finalized more than once");
        passed &= check(!audit.published_ranks.empty(), "deadline run never published a complete incumbent");
        passed &= check(audit.placements.size() == fixture.parts.size(),
                        "deadline run did not return one explicit result per part");
        passed &= check(wall_ms + 5.0 >= static_cast<double>(budget_ms),
                        "finite wall-clock budget terminated materially early");
        passed &= check(wall_ms <= static_cast<double>(budget_ms) + 250.0,
                        "finite wall-clock budget exceeded the 250 ms overrun gate");
        passed &= check(audit.callback_count <= audit.last_iteration / 64 + 64,
                        "raw progress callback traffic is not bounded relative to solver work");
    }
    std::ranges::sort(elapsed_ms);
    auto const p95_ms = elapsed_ms[static_cast<std::size_t>(std::ceil(elapsed_ms.size() * 0.95)) - 1];
    passed &= check(p95_ms <= static_cast<double>(budget_ms) + 250.0,
                    "p95 deadline completion exceeded the release-gate allowance");
    return passed;
}

auto cancellation_fixture() -> FixedContainerFixture
{
    auto fixture = FixedContainerFixture{
        .id = "cancellation-phases",
        .container = rectangle(0, 0, 120, 80),
        .options = gate_options(),
        .minimum_placed = 0,
    };
    for (std::uint64_t index = 0; index < 12; ++index) {
        fixture.parts.emplace_back(part(1000 + index, rectangle(160 + static_cast<double>(index) * 15.0, 0, 11, 9)));
    }
    return fixture;
}

auto check_cancellation_phase_and_latency_gates() -> bool
{
    bool passed = true;
    auto fixture = cancellation_fixture();

    for (unsigned cancel_stage : {0U, 2U}) {
        auto staged_fixture = fixture;
        staged_fixture.options.time_limit_ms = cancel_stage == 2 ? 10 : 0;
        Job job(staged_fixture.options);
        passed &= configure(job, staged_fixture);
        bool saw_stage = false;
        auto const status = job.run([&](auto const &progress) {
            if (progress.stage == cancel_stage) {
                saw_stage = true;
                job.cancel();
            }
        });
        passed &= check(saw_stage, "requested cancellation phase was never reached");
        passed &= check(status == Status::Cancelled && job.state() == JobState::Cancelled,
                        "phase cancellation did not produce a cancelled terminal job");
    }

    fixture.options.time_limit_ms = 0;
    Job job(fixture.options);
    passed &= configure(job, fixture);
    std::mutex progress_mutex;
    std::condition_variable progress_condition;
    bool solving_started = false;
    Status terminal_status = Status::InternalError;
    std::thread worker([&] {
        terminal_status = job.run([&](auto const &progress) {
            if (progress.stage == 1 && progress.iteration >= 256) {
                {
                    std::lock_guard lock(progress_mutex);
                    solving_started = true;
                }
                progress_condition.notify_one();
            }
        });
    });

    {
        std::unique_lock lock(progress_mutex);
        if (!progress_condition.wait_for(lock, std::chrono::seconds(5), [&] { return solving_started; })) {
            passed &= check(false, "unlimited solver never reached cancellable solving work");
        }
    }
    auto const cancellation_started = std::chrono::steady_clock::now();
    job.cancel();
    worker.join();
    auto const cancellation_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - cancellation_started).count();
    passed &= check(terminal_status == Status::Cancelled && job.state() == JobState::Cancelled,
                    "cross-thread cancellation did not stop the job");
    passed &= check(cancellation_ms <= 200.0, "cross-thread cancellation latency exceeded 200 ms");
    return passed;
}

class StableHash
{
public:
    template <std::integral Integer>
    void add(Integer value)
    {
        add_unsigned(static_cast<std::uint64_t>(value));
    }

    void add(double value)
    {
        static_assert(sizeof(double) == sizeof(std::uint64_t));
        add_unsigned(std::bit_cast<std::uint64_t>(value));
    }

    void add(std::string_view value)
    {
        add(value.size());
        for (auto const byte : value) {
            _value ^= static_cast<unsigned char>(byte);
            _value *= 1099511628211ULL;
        }
    }

    [[nodiscard]] std::uint64_t value() const { return _value; }

private:
    void add_unsigned(std::uint64_t value)
    {
        for (unsigned shift = 0; shift < 64; shift += 8) {
            _value ^= (value >> shift) & 0xffU;
            _value *= 1099511628211ULL;
        }
    }
    std::uint64_t _value = 14695981039346656037ULL;
};

void hash_ring(StableHash &hash, std::span<Point const> ring)
{
    hash.add(ring.size());
    for (auto const &point : ring) {
        hash.add(point.x);
        hash.add(point.y);
    }
}

void hash_part(StableHash &hash, PartGeometry const &candidate)
{
    hash.add(candidate.id);
    hash.add(candidate.components.size());
    for (auto const &candidate_component : candidate.components) {
        hash_ring(hash, candidate_component.outer);
        hash.add(candidate_component.holes.size());
        for (auto const &hole : candidate_component.holes) {
            hash_ring(hash, hole);
        }
    }
}

auto fixture_fingerprint(FixedContainerFixture const &fixture, bool canonicalize_part_order) -> std::string
{
    StableHash hash;
    hash.add(fixture.id);
    hash_ring(hash, fixture.container);
    hash.add(fixture.container_holes.size());
    for (auto const &hole : fixture.container_holes) {
        hash_ring(hash, hole);
    }
    hash.add(fixture.parts.size());
    if (canonicalize_part_order) {
        std::vector<PartGeometry const *> ordered_parts;
        ordered_parts.reserve(fixture.parts.size());
        for (auto const &candidate : fixture.parts) {
            ordered_parts.emplace_back(&candidate);
        }
        std::ranges::sort(ordered_parts, {}, [](auto const *candidate) { return candidate->id; });
        for (auto const *candidate : ordered_parts) {
            hash_part(hash, *candidate);
        }
    } else {
        for (auto const &candidate : fixture.parts) {
            hash_part(hash, candidate);
        }
    }
    hash.add(fixture.options.part_spacing);
    hash.add(fixture.options.container_margin);
    hash.add(fixture.options.rotation_step_degrees);
    hash.add(fixture.options.random_seed);
    hash.add(fixture.options.time_limit_ms);
    hash.add(static_cast<std::uint64_t>(fixture.options.rotation_mode));
    hash.add(static_cast<std::uint64_t>(fixture.options.quality));

    // Worker count is the one deliberate execution-policy difference between
    // baseline and portfolio runs. Keep it out of both immutable-input hashes
    // and record it explicitly in report metadata instead.

    std::ostringstream stream;
    stream << std::hex << std::setfill('0') << std::setw(16) << hash.value();
    return stream.str();
}

auto ordered_fixture_fingerprint(FixedContainerFixture const &fixture) -> std::string
{
    return fixture_fingerprint(fixture, false);
}

auto semantic_fixture_fingerprint(FixedContainerFixture const &fixture) -> std::string
{
    return fixture_fingerprint(fixture, true);
}

auto placement_signature(std::span<Placement const> placements) -> std::string
{
    StableHash hash;
    hash.add(placements.size());
    for (auto const &placement : placements) {
        hash.add(placement.part_id);
        hash.add(placement.translation_x);
        hash.add(placement.translation_y);
        hash.add(placement.rotation_degrees);
        hash.add(placement.placed ? 1U : 0U);
    }
    std::ostringstream stream;
    stream << std::hex << std::setfill('0') << std::setw(16) << hash.value();
    return stream.str();
}

auto check_adversarial_fixture_gates() -> bool
{
    auto const fixtures = adversarial_fixtures();
    auto const repeated_definitions = adversarial_fixtures();
    bool passed = true;
    auto verify = [&passed](bool condition, std::string const &message) {
        if (!condition) {
            std::cerr << message << '\n';
            passed = false;
        }
    };

    verify(fixtures.size() == 4, "adversarial corpus must contain exactly four required trap classes");
    verify(repeated_definitions.size() == fixtures.size(),
           "adversarial corpus construction changed fixture cardinality");

    std::map<std::string, bool> case_ids;
    std::map<std::string, bool> trap_classes;
    for (std::size_t fixture_index = 0; fixture_index < fixtures.size(); ++fixture_index) {
        auto const &adversarial = fixtures[fixture_index];
        auto const &fixture = adversarial.fixture;
        auto const prefix = fixture.id + ": ";

        verify(case_ids.emplace(fixture.id, true).second, prefix + "duplicate fixture ID");
        verify(!adversarial.trap_class.empty(), prefix + "missing trap class");
        verify(trap_classes.emplace(std::string(adversarial.trap_class), true).second,
               prefix + "duplicate adversarial trap class");
        verify(fixture_index < repeated_definitions.size() &&
                   ordered_fixture_fingerprint(fixture) ==
                       ordered_fixture_fingerprint(repeated_definitions[fixture_index].fixture),
               prefix + "fixture fingerprint is not reproducible");
        verify(adversarial.known_better.size() == fixture.parts.size(),
               prefix + "known-better placement cardinality differs from the input");
        verify(adversarial.known_blocked.size() == fixture.parts.size(),
               prefix + "known-blocked placement cardinality differs from the input");

        std::map<std::uint64_t, bool> stable_ids;
        for (auto const &candidate : fixture.parts) {
            verify(candidate.id != 0, prefix + "part has the reserved zero stable ID");
            verify(stable_ids.emplace(candidate.id, true).second, prefix + "part IDs are not unique");
        }

        auto const better_error = validate_placements(fixture, adversarial.known_better);
        auto const blocked_error = validate_placements(fixture, adversarial.known_blocked);
        if (better_error) {
            verify(false, prefix + "known-better placement is invalid: " + *better_error);
        }
        if (blocked_error) {
            verify(false, prefix + "known-blocked placement is invalid: " + *blocked_error);
        }
        if (better_error || blocked_error) {
            continue;
        }

        auto const better_rank = canonical_rank(fixture, adversarial.known_better);
        auto const blocked_rank = canonical_rank(fixture, adversarial.known_blocked);
        verify(better_rank.placed_count >= 2, prefix + "reference solution is too trivial");
        verify(blocked_rank.placed_count < fixture.parts.size(),
               prefix + "blocked state does not leave an insertion opportunity");
        verify(rank_not_worse(better_rank, blocked_rank) && !ranks_equivalent(better_rank, blocked_rank),
               prefix + "reference solution does not strictly improve the canonical rank");

        auto const first = first_incumbent(fixture);
        auto const repeated = first_incumbent(fixture);
        verify(first.status == Status::Cancelled && repeated.status == Status::Cancelled,
               prefix + "first-incumbent reproducibility runs did not cancel transactionally");
        verify(first.progress_valid && repeated.progress_valid,
               prefix + "first-incumbent reproducibility run published invalid progress");
        verify(first.placements.size() == fixture.parts.size() &&
                   repeated.placements.size() == fixture.parts.size(),
               prefix + "first-incumbent reproducibility run lost result cardinality");
        if (first.placements.size() == fixture.parts.size() &&
            repeated.placements.size() == fixture.parts.size()) {
            verify(placement_signature(first.placements) == placement_signature(repeated.placements),
                   prefix + "same seed and worker count changed the first incumbent");
            verify(rank_not_worse(better_rank, first.rank),
                   prefix + "reference placement is worse than the constructive incumbent");
        }
    }
    for (auto const required : {"order-trap", "rotation-trap", "concave-hole-blocker-trap",
                                "rigid-compound-trap"}) {
        verify(trap_classes.contains(required), std::string("adversarial corpus is missing ") + required);
    }
    return passed;
}

auto fixture_selected_area(FixedContainerFixture const &fixture) -> double
{
    double area = 0.0;
    for (auto const &candidate : fixture.parts) {
        area += Inkscape::Nesting::Test::part_area(candidate);
    }
    return area;
}

auto fixture_container_area(FixedContainerFixture const &fixture) -> double
{
    auto area = Inkscape::Nesting::Test::ring_area(fixture.container);
    for (auto const &hole : fixture.container_holes) {
        area -= Inkscape::Nesting::Test::ring_area(hole);
    }
    return std::max(0.0, area);
}

struct DifferentialCaseRecord
{
    std::string id;
    std::string trap_class;
    std::string semantic_input_fingerprint;
    std::string ordered_input_fingerprint;
    std::uint64_t random_seed = 0;
    bool valid = false;
    std::uint64_t status_code = 0;
    std::uint64_t selected_count = 0;
    double selected_area = 0.0;
    std::uint64_t placed_count = 0;
    double placed_area = 0.0;
    double container_usable_area = 0.0;
    double utilization_percent = 0.0;
    std::uint64_t unplaced_count = 0;
    std::string unplaced_reason;
    std::optional<double> preparation_ms{};
    std::optional<double> initial_incumbent_ms{};
    std::optional<double> exploration_refinement_ms{};
    double wall_ms = 0.0;
    std::uint64_t iterations = 0;
    std::uint64_t callbacks = 0;
    std::optional<double> occupied_bounds_area;
    std::optional<double> deadline_overrun_ms{};
    std::optional<std::uint64_t> peak_rss_bytes{};
    std::optional<double> cancellation_latency_ms{};
    std::string contour_source;
    std::string contour_fidelity;
    std::string placement_signature;
};

struct DifferentialReport
{
    std::string backend;
    std::string mode;
    std::uint64_t budget_ms = 0;
    std::uint32_t configured_worker_count = 0;
    std::optional<std::uint32_t> runtime_observed_worker_count{};
    std::string input_order;
    std::vector<DifferentialCaseRecord> cases{};
};

constexpr std::string_view DIFFERENTIAL_COLUMNS =
    "case_id\ttrap_class\tsemantic_input_fingerprint\tordered_input_fingerprint\trandom_seed\tvalid\t"
    "status_code\tselected_count\tselected_area\tplaced_count\tplaced_area\tcontainer_usable_area\t"
    "utilization_percent\tunplaced_count\tunplaced_reason\tpreparation_ms\tinitial_incumbent_ms\t"
    "exploration_refinement_ms\twall_ms\titerations\tcallbacks\toccupied_bounds_area\tdeadline_overrun_ms\t"
    "peak_rss_bytes\tcancellation_latency_ms\tcontour_source\tcontour_fidelity\tplacement_signature";

auto token_is_safe(std::string_view token) -> bool
{
    return !token.empty() && std::ranges::all_of(token, [](unsigned char character) {
               return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
                      (character >= '0' && character <= '9') || character == '-' || character == '_' ||
                      character == '.';
           });
}

auto make_differential_record(FixedContainerFixture fixture, std::string trap_class, std::uint64_t budget_ms)
    -> DifferentialCaseRecord
{
    fixture.options.time_limit_ms = budget_ms;
    DifferentialCaseRecord record;
    record.id = fixture.id;
    record.trap_class = std::move(trap_class);
    record.semantic_input_fingerprint = semantic_fixture_fingerprint(fixture);
    record.ordered_input_fingerprint = ordered_fixture_fingerprint(fixture);
    record.random_seed = fixture.options.random_seed;
    record.selected_count = fixture.parts.size();
    record.selected_area = fixture_selected_area(fixture);
    record.container_usable_area = fixture_container_area(fixture);
    record.contour_source = "synthetic";
    record.contour_fidelity = "exact";

    std::vector<Placement> placements;
    if (budget_ms == 0) {
        auto const result = first_incumbent(fixture);
        record.status_code = static_cast<std::uint64_t>(result.status);
        record.wall_ms = std::chrono::duration<double, std::milli>(result.wall_time).count();
        record.initial_incumbent_ms = result.first_incumbent_ms;
        record.iterations = result.last_iteration;
        record.callbacks = result.callback_count;
        placements = result.placements;
        record.valid = result.status == Status::Cancelled && result.progress_valid &&
                       placements.size() == fixture.parts.size() &&
                       !validate_placements(fixture, placements).has_value();
    } else {
        auto const audit = run_to_deadline(fixture, budget_ms);
        record.status_code = static_cast<std::uint64_t>(audit.status);
        record.wall_ms = std::chrono::duration<double, std::milli>(audit.wall_time).count();
        if (std::isfinite(audit.first_incumbent_ms)) {
            record.initial_incumbent_ms = audit.first_incumbent_ms;
            record.exploration_refinement_ms = std::max(0.0, record.wall_ms - audit.first_incumbent_ms);
        }
        record.iterations = audit.last_iteration;
        record.callbacks = audit.callback_count;
        record.deadline_overrun_ms = std::max(0.0, record.wall_ms - static_cast<double>(budget_ms));
        placements = audit.placements;
        record.valid = audit.status == Status::Ok && audit.valid && placements.size() == fixture.parts.size() &&
                       !validate_placements(fixture, placements).has_value();
    }

    auto const rank = canonical_rank(fixture, placements);
    record.placed_count = rank.placed_count;
    record.placed_area = rank.placed_area;
    if (rank.placed_count != 0) {
        record.occupied_bounds_area = rank.occupied_area;
    }
    record.utilization_percent = record.container_usable_area > 0.0
                                     ? record.placed_area * 100.0 / record.container_usable_area
                                     : 0.0;
    record.unplaced_count = record.selected_count - record.placed_count;
    record.unplaced_reason = record.unplaced_count == 0 ? "none" : "not_exposed_by_current_abi";
    record.placement_signature = placement_signature(placements);
    return record;
}

template <typename Value>
void write_optional(std::ostream &stream, std::optional<Value> const &value)
{
    if (value) {
        stream << *value;
    } else {
        stream << "NA";
    }
}

auto write_differential_report(std::string const &path, std::string const &backend, std::uint64_t budget_ms,
                               std::uint32_t worker_count, FixturePartOrder part_order) -> bool
{
    if (!token_is_safe(backend)) {
        throw std::invalid_argument("backend label must use only ASCII letters, digits, '.', '_' or '-'");
    }
    if (worker_count == 0 || worker_count > 1024) {
        throw std::invalid_argument("differential worker count must be in [1, 1024]");
    }

    DifferentialReport report{
        .backend = backend,
        .mode = budget_ms == 0 ? "first-incumbent" : "finite-deadline",
        .budget_ms = budget_ms,
        .configured_worker_count = worker_count,
        // The current ABI does not expose runtime worker telemetry. Do not
        // infer it from the requested count; strict adversarial improvement is
        // the independent activation proof until telemetry is added.
        .runtime_observed_worker_count = std::nullopt,
        .input_order = std::string(part_order_name(part_order)),
    };
    bool all_valid = true;
    auto const trap_classes = adversarial_trap_classes();
    for (auto fixture : difficult_fixtures()) {
        fixture.options.worker_count = worker_count;
        apply_part_order(fixture, part_order);
        auto const found_trap = trap_classes.find(fixture.id);
        auto record = make_differential_record(
            std::move(fixture), found_trap == trap_classes.end() ? "general" : found_trap->second, budget_ms);
        all_valid &= record.valid;
        report.cases.emplace_back(std::move(record));
    }

    std::ofstream stream(path, std::ios::trunc);
    if (!stream) {
        throw std::runtime_error("unable to create differential report: " + path);
    }
    stream << std::setprecision(17);
    stream << "VACARDS_NESTING_DIFFERENTIAL\t2\n";
    stream << "backend\t" << report.backend << '\n';
    stream << "mode\t" << report.mode << '\n';
    stream << "budget_ms\t" << report.budget_ms << '\n';
    stream << "configured_worker_count\t" << report.configured_worker_count << '\n';
    stream << "runtime_observed_worker_count\t";
    write_optional(stream, report.runtime_observed_worker_count);
    stream << '\n';
    stream << "input_order\t" << report.input_order << '\n';
    stream << "columns\t" << DIFFERENTIAL_COLUMNS << '\n';
    for (auto const &record : report.cases) {
        stream << "case\t" << record.id << '\t' << record.trap_class << '\t'
               << record.semantic_input_fingerprint << '\t' << record.ordered_input_fingerprint << '\t'
               << record.random_seed << '\t' << (record.valid ? 1 : 0) << '\t' << record.status_code << '\t'
               << record.selected_count << '\t' << record.selected_area << '\t' << record.placed_count << '\t'
               << record.placed_area << '\t' << record.container_usable_area << '\t' << record.utilization_percent
               << '\t' << record.unplaced_count << '\t' << record.unplaced_reason << '\t';
        write_optional(stream, record.preparation_ms);
        stream << '\t';
        write_optional(stream, record.initial_incumbent_ms);
        stream << '\t';
        write_optional(stream, record.exploration_refinement_ms);
        stream << '\t' << record.wall_ms << '\t' << record.iterations << '\t' << record.callbacks << '\t';
        write_optional(stream, record.occupied_bounds_area);
        stream << '\t';
        write_optional(stream, record.deadline_overrun_ms);
        stream << '\t';
        write_optional(stream, record.peak_rss_bytes);
        stream << '\t';
        write_optional(stream, record.cancellation_latency_ms);
        stream << '\t' << record.contour_source << '\t' << record.contour_fidelity << '\t'
               << record.placement_signature << '\n';
    }
    if (!stream) {
        throw std::runtime_error("unable to finish differential report: " + path);
    }
    return all_valid;
}

auto split_tabs(std::string const &line) -> std::vector<std::string>
{
    std::vector<std::string> fields;
    std::size_t start = 0;
    while (true) {
        auto const separator = line.find('\t', start);
        fields.emplace_back(line.substr(start, separator - start));
        if (separator == std::string::npos) {
            break;
        }
        start = separator + 1;
    }
    return fields;
}

auto parse_unsigned(std::string const &value, std::string_view field) -> std::uint64_t
{
    if (value.empty() || !std::ranges::all_of(value, [](unsigned char character) {
            return character >= '0' && character <= '9';
        })) {
        throw std::runtime_error("invalid unsigned value for " + std::string(field));
    }
    std::size_t consumed = 0;
    auto const parsed = std::stoull(value, &consumed);
    if (consumed != value.size()) {
        throw std::runtime_error("invalid unsigned value for " + std::string(field));
    }
    return parsed;
}

auto parse_number(std::string const &value, std::string_view field) -> double
{
    std::size_t consumed = 0;
    auto const parsed = std::stod(value, &consumed);
    if (consumed != value.size() || !std::isfinite(parsed)) {
        throw std::runtime_error("invalid finite number for " + std::string(field));
    }
    return parsed;
}

auto parse_optional_number(std::string const &value, std::string_view field) -> std::optional<double>
{
    return value == "NA" ? std::nullopt : std::optional<double>{parse_number(value, field)};
}

auto parse_optional_unsigned(std::string const &value, std::string_view field) -> std::optional<std::uint64_t>
{
    return value == "NA" ? std::nullopt : std::optional<std::uint64_t>{parse_unsigned(value, field)};
}

auto read_required_fields(std::istream &stream, std::string_view expected_key) -> std::vector<std::string>
{
    std::string line;
    if (!std::getline(stream, line)) {
        throw std::runtime_error("truncated differential report before " + std::string(expected_key));
    }
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    auto fields = split_tabs(line);
    if (fields.empty() || fields.front() != expected_key) {
        throw std::runtime_error("expected differential report field " + std::string(expected_key));
    }
    return fields;
}

auto read_differential_report(std::string const &path) -> DifferentialReport
{
    std::ifstream stream(path);
    if (!stream) {
        throw std::runtime_error("unable to open differential report: " + path);
    }

    auto schema = read_required_fields(stream, "VACARDS_NESTING_DIFFERENTIAL");
    if (schema.size() != 2 || schema[1] != "2") {
        throw std::runtime_error("unsupported differential report schema");
    }
    auto backend = read_required_fields(stream, "backend");
    auto mode = read_required_fields(stream, "mode");
    auto budget = read_required_fields(stream, "budget_ms");
    auto configured_workers = read_required_fields(stream, "configured_worker_count");
    auto observed_workers = read_required_fields(stream, "runtime_observed_worker_count");
    auto input_order = read_required_fields(stream, "input_order");
    auto columns = read_required_fields(stream, "columns");
    if (backend.size() != 2 || !token_is_safe(backend[1]) || mode.size() != 2 ||
        (mode[1] != "first-incumbent" && mode[1] != "finite-deadline") || budget.size() != 2 ||
        configured_workers.size() != 2 || observed_workers.size() != 2 || input_order.size() != 2 ||
        (input_order[1] != "original" && input_order[1] != "reversed") || columns.size() != 29) {
        throw std::runtime_error("malformed differential report metadata");
    }
    std::string serialized_columns;
    for (std::size_t index = 1; index < columns.size(); ++index) {
        if (index != 1) {
            serialized_columns += '\t';
        }
        serialized_columns += columns[index];
    }
    if (serialized_columns != DIFFERENTIAL_COLUMNS) {
        throw std::runtime_error("differential report columns do not match schema version 2");
    }

    auto const configured_worker_count = parse_unsigned(configured_workers[1], "configured_worker_count");
    auto const observed_worker_count =
        parse_optional_unsigned(observed_workers[1], "runtime_observed_worker_count");
    if (configured_worker_count == 0 || configured_worker_count > 1024 ||
        (observed_worker_count && (*observed_worker_count == 0 || *observed_worker_count > 1024))) {
        throw std::runtime_error("differential report worker-count metadata is invalid");
    }

    DifferentialReport report{
        .backend = backend[1],
        .mode = mode[1],
        .budget_ms = parse_unsigned(budget[1], "budget_ms"),
        .configured_worker_count = static_cast<std::uint32_t>(configured_worker_count),
        .runtime_observed_worker_count =
            observed_worker_count
                ? std::optional<std::uint32_t>{static_cast<std::uint32_t>(*observed_worker_count)}
                : std::nullopt,
        .input_order = input_order[1],
    };
    if ((report.mode == "first-incumbent") != (report.budget_ms == 0)) {
        throw std::runtime_error("differential report mode and budget disagree");
    }
    while (stream.peek() != std::char_traits<char>::eof()) {
        auto fields = read_required_fields(stream, "case");
        if (fields.size() != 29) {
            throw std::runtime_error("differential case row has the wrong field count");
        }
        DifferentialCaseRecord record;
        record.id = fields[1];
        record.trap_class = fields[2];
        record.semantic_input_fingerprint = fields[3];
        record.ordered_input_fingerprint = fields[4];
        record.random_seed = parse_unsigned(fields[5], "random_seed");
        if (!token_is_safe(record.id) || !token_is_safe(record.trap_class) ||
            !token_is_safe(record.semantic_input_fingerprint) || !token_is_safe(record.ordered_input_fingerprint) ||
            (fields[6] != "0" && fields[6] != "1")) {
            throw std::runtime_error("differential case identity or validity is malformed");
        }
        record.valid = fields[6] == "1";
        record.status_code = parse_unsigned(fields[7], "status_code");
        record.selected_count = parse_unsigned(fields[8], "selected_count");
        record.selected_area = parse_number(fields[9], "selected_area");
        record.placed_count = parse_unsigned(fields[10], "placed_count");
        record.placed_area = parse_number(fields[11], "placed_area");
        record.container_usable_area = parse_number(fields[12], "container_usable_area");
        record.utilization_percent = parse_number(fields[13], "utilization_percent");
        record.unplaced_count = parse_unsigned(fields[14], "unplaced_count");
        record.unplaced_reason = fields[15];
        record.preparation_ms = parse_optional_number(fields[16], "preparation_ms");
        record.initial_incumbent_ms = parse_optional_number(fields[17], "initial_incumbent_ms");
        record.exploration_refinement_ms = parse_optional_number(fields[18], "exploration_refinement_ms");
        record.wall_ms = parse_number(fields[19], "wall_ms");
        record.iterations = parse_unsigned(fields[20], "iterations");
        record.callbacks = parse_unsigned(fields[21], "callbacks");
        record.occupied_bounds_area = parse_optional_number(fields[22], "occupied_bounds_area");
        record.deadline_overrun_ms = parse_optional_number(fields[23], "deadline_overrun_ms");
        record.peak_rss_bytes = parse_optional_unsigned(fields[24], "peak_rss_bytes");
        record.cancellation_latency_ms = parse_optional_number(fields[25], "cancellation_latency_ms");
        record.contour_source = fields[26];
        record.contour_fidelity = fields[27];
        record.placement_signature = fields[28];
        auto const expected_utilization = record.container_usable_area > 0.0
                                              ? record.placed_area * 100.0 / record.container_usable_area
                                              : 0.0;
        auto const utilization_tolerance = std::max(1.0, expected_utilization) * 1.0e-9;
        auto const optional_is_negative = [](std::optional<double> const &value) {
            return value && *value < 0.0;
        };
        auto const expected_status = report.mode == "first-incumbent" ? Status::Cancelled : Status::Ok;
        if (record.selected_count == 0 || record.placed_count > record.selected_count ||
            record.placed_count > std::numeric_limits<std::uint32_t>::max() ||
            record.unplaced_count != record.selected_count - record.placed_count || record.selected_area < 0.0 ||
            record.placed_area < 0.0 || record.placed_area > record.selected_area + 1.0e-8 ||
            record.container_usable_area < 0.0 || record.utilization_percent < 0.0 || record.wall_ms < 0.0 ||
            std::abs(record.utilization_percent - expected_utilization) > utilization_tolerance ||
            optional_is_negative(record.preparation_ms) || optional_is_negative(record.initial_incumbent_ms) ||
            optional_is_negative(record.exploration_refinement_ms) ||
            optional_is_negative(record.occupied_bounds_area) || optional_is_negative(record.deadline_overrun_ms) ||
            optional_is_negative(record.cancellation_latency_ms) ||
            (record.valid && record.status_code != static_cast<std::uint64_t>(expected_status)) ||
            (record.valid && !record.initial_incumbent_ms) || (record.valid && record.callbacks == 0) ||
            (record.valid && report.budget_ms != 0 && !record.deadline_overrun_ms) ||
            (report.budget_ms == 0 && record.deadline_overrun_ms) ||
            (record.placed_count == 0 && record.occupied_bounds_area) ||
            (record.placed_count != 0 && !record.occupied_bounds_area) || record.unplaced_reason.empty() ||
            record.contour_source.empty() || record.contour_fidelity.empty() ||
            !token_is_safe(record.unplaced_reason) || !token_is_safe(record.contour_source) ||
            !token_is_safe(record.contour_fidelity) || record.semantic_input_fingerprint.size() != 16 ||
            record.ordered_input_fingerprint.size() != 16 ||
            record.placement_signature.size() != 16 || !token_is_safe(record.placement_signature)) {
            throw std::runtime_error("differential case metrics violate schema invariants: " + record.id);
        }
        report.cases.emplace_back(std::move(record));
    }
    if (report.cases.empty()) {
        throw std::runtime_error("differential report contains no cases");
    }
    return report;
}

struct DifferentialComparison
{
    struct EvidenceRow
    {
        DifferentialCaseRecord baseline;
        DifferentialCaseRecord portfolio;
        std::string rank_relation;
    };

    std::vector<std::string> errors;
    std::size_t strict_improvements = 0;
    std::size_t strict_adversarial_improvements = 0;
    std::vector<EvidenceRow> evidence;
};

struct DifferentialComparisonPolicy
{
    bool require_identical = false;
    bool require_rank_equivalent = false;
    bool allow_input_order_permutation = false;
    bool require_strict_improvement = false;
    bool require_strict_adversarial_improvement = false;
    std::optional<std::uint32_t> expected_baseline_worker_count{};
    std::optional<std::uint32_t> expected_portfolio_worker_count{};
};

auto check_aggregate_defaults() -> bool
{
    static_assert(std::is_aggregate_v<CollisionComponent>);
    static_assert(std::is_aggregate_v<FixedContainerFixture>);
    static_assert(std::is_aggregate_v<DifferentialCaseRecord>);
    static_assert(std::is_aggregate_v<DifferentialReport>);
    static_assert(std::is_aggregate_v<DifferentialComparisonPolicy>);

    // Keep intentionally omitted fields in these designated initializers:
    // GNU's warning-as-error build must accept their empty defaults.
    FixedContainerFixture const fixture{
        .id = "aggregate-defaults",
        .container = square(10.0),
        .options = gate_options(),
    };
    DifferentialReport const report{
        .backend = "baseline",
        .mode = "first-incumbent",
        .input_order = "original",
    };
    DifferentialComparisonPolicy const policy{.require_identical = true};
    bool passed = check(fixture.container_holes.empty() && fixture.parts.empty() && report.cases.empty(),
                        "omitted aggregate collections were not empty");
    passed &= check(!policy.expected_baseline_worker_count && !policy.expected_portfolio_worker_count,
                    "omitted worker expectations imposed a worker-count constraint");

    DifferentialCaseRecord const record{};
    std::ostringstream telemetry;
    auto append_telemetry = [&telemetry](auto const &value) {
        write_optional(telemetry, value);
        telemetry << '\t';
    };
    append_telemetry(record.preparation_ms);
    append_telemetry(record.initial_incumbent_ms);
    append_telemetry(record.exploration_refinement_ms);
    append_telemetry(record.deadline_overrun_ms);
    append_telemetry(record.peak_rss_bytes);
    append_telemetry(record.cancellation_latency_ms);
    append_telemetry(report.runtime_observed_worker_count);
    passed &= check(telemetry.str() == "NA\tNA\tNA\tNA\tNA\tNA\tNA\t",
                    "unavailable default telemetry was reported as a measurement");
    return passed;
}

auto record_rank(DifferentialCaseRecord const &record) -> CanonicalRank
{
    return {
        .placed_area = record.placed_area,
        .placed_count = static_cast<std::uint32_t>(record.placed_count),
        .occupied_area = record.occupied_bounds_area.value_or(std::numeric_limits<double>::infinity()),
    };
}

void check_deadline_overrun(DifferentialReport const &report, std::string_view role,
                            DifferentialComparison &comparison)
{
    if (report.budget_ms == 0) {
        return;
    }

    std::vector<double> overruns;
    overruns.reserve(report.cases.size());
    auto const allowed_ms = std::max(250.0, static_cast<double>(report.budget_ms) * 0.05);
    for (auto const &record : report.cases) {
        if (!record.deadline_overrun_ms) {
            comparison.errors.emplace_back(std::string(role) + " report omits deadline overrun for " + record.id);
            continue;
        }
        auto const overrun_ms = *record.deadline_overrun_ms;
        if (!std::isfinite(overrun_ms) || overrun_ms < 0.0) {
            comparison.errors.emplace_back(std::string(role) + " report has invalid deadline overrun for " +
                                           record.id);
            continue;
        }
        overruns.emplace_back(overrun_ms);
        if (overrun_ms > allowed_ms) {
            std::ostringstream message;
            message << std::setprecision(17) << role << " deadline overrun for " << record.id << " is "
                    << overrun_ms << " ms; allowed " << allowed_ms << " ms";
            comparison.errors.emplace_back(message.str());
        }
    }
    if (overruns.empty()) {
        return;
    }
    std::ranges::sort(overruns);
    auto const p95_index = static_cast<std::size_t>(std::ceil(overruns.size() * 0.95)) - 1;
    if (overruns[p95_index] > allowed_ms) {
        std::ostringstream message;
        message << role << " deadline-overrun p95 is " << overruns[p95_index] << " ms; allowed " << allowed_ms
                << " ms";
        comparison.errors.emplace_back(message.str());
    }
}

auto compare_differential_reports(DifferentialReport const &baseline, DifferentialReport const &experimental,
                                  DifferentialComparisonPolicy const &policy) -> DifferentialComparison
{
    DifferentialComparison comparison;
    if (baseline.mode != experimental.mode || baseline.budget_ms != experimental.budget_ms) {
        comparison.errors.emplace_back("reports were not produced with the same mode and budget");
    }
    if (!policy.allow_input_order_permutation && baseline.input_order != experimental.input_order) {
        comparison.errors.emplace_back("reports use different input orders without a permutation comparison");
    }
    auto check_workers = [&comparison](DifferentialReport const &report, std::string_view role,
                                       std::optional<std::uint32_t> expected) {
        if (expected && report.configured_worker_count != *expected) {
            comparison.errors.emplace_back(std::string(role) + " report did not use the expected worker count");
        }
        if (expected && report.runtime_observed_worker_count &&
            *report.runtime_observed_worker_count != *expected) {
            comparison.errors.emplace_back(std::string(role) + " runtime worker telemetry disagrees with the gate");
        }
    };
    check_workers(baseline, "baseline", policy.expected_baseline_worker_count);
    check_workers(experimental, "portfolio", policy.expected_portfolio_worker_count);
    if ((policy.require_strict_improvement || policy.require_strict_adversarial_improvement) &&
        (baseline.budget_ms == 0 || baseline.configured_worker_count != 1 ||
         experimental.configured_worker_count != 2)) {
        comparison.errors.emplace_back(
            "activation evidence requires a positive budget, baseline worker_count=1 and portfolio worker_count=2");
    }
    check_deadline_overrun(baseline, "baseline", comparison);
    check_deadline_overrun(experimental, "portfolio", comparison);

    std::map<std::string, DifferentialCaseRecord const *> experimental_cases;
    for (auto const &record : experimental.cases) {
        if (!experimental_cases.emplace(record.id, &record).second) {
            comparison.errors.emplace_back("experimental report repeats case " + record.id);
        }
    }
    std::map<std::string, bool> baseline_ids;
    for (auto const &baseline_record : baseline.cases) {
        if (!baseline_ids.emplace(baseline_record.id, true).second) {
            comparison.errors.emplace_back("baseline report repeats case " + baseline_record.id);
            continue;
        }
        auto const found = experimental_cases.find(baseline_record.id);
        if (found == experimental_cases.end()) {
            comparison.errors.emplace_back("experimental report is missing case " + baseline_record.id);
            continue;
        }
        auto const &experimental_record = *found->second;
        DifferentialComparison::EvidenceRow evidence{
            .baseline = baseline_record,
            .portfolio = experimental_record,
            .rank_relation = "invalid",
        };
        if (!baseline_record.valid || !experimental_record.valid) {
            comparison.errors.emplace_back("invalid authoritative placement in case " + baseline_record.id);
            comparison.evidence.emplace_back(std::move(evidence));
            continue;
        }
        auto const area_tolerance = std::max(1.0, std::abs(baseline_record.selected_area)) * 1.0e-9;
        if (baseline_record.trap_class != experimental_record.trap_class ||
            baseline_record.semantic_input_fingerprint != experimental_record.semantic_input_fingerprint ||
            (!policy.allow_input_order_permutation &&
             baseline_record.ordered_input_fingerprint != experimental_record.ordered_input_fingerprint) ||
            baseline_record.random_seed != experimental_record.random_seed ||
            baseline_record.selected_count != experimental_record.selected_count ||
            std::abs(baseline_record.selected_area - experimental_record.selected_area) > area_tolerance ||
            std::abs(baseline_record.container_usable_area - experimental_record.container_usable_area) >
                area_tolerance) {
            comparison.errors.emplace_back("case inputs differ for " + baseline_record.id);
            comparison.evidence.emplace_back(std::move(evidence));
            continue;
        }
        auto const baseline_rank = record_rank(baseline_record);
        auto const experimental_rank = record_rank(experimental_record);
        if (!rank_not_worse(experimental_rank, baseline_rank)) {
            evidence.rank_relation = "worse";
            comparison.errors.emplace_back("experimental canonical rank regressed for " + baseline_record.id);
        } else if (!ranks_equivalent(experimental_rank, baseline_rank)) {
            evidence.rank_relation = "strictly-better";
            ++comparison.strict_improvements;
            if (is_approved_adversarial_trap(baseline_record.trap_class)) {
                ++comparison.strict_adversarial_improvements;
            }
        } else {
            evidence.rank_relation = "equivalent";
        }
        if (policy.require_rank_equivalent && !ranks_equivalent(experimental_rank, baseline_rank)) {
            comparison.errors.emplace_back("canonical rank changed in parity comparison for " + baseline_record.id);
        }
        if (policy.require_identical &&
            (baseline_record.ordered_input_fingerprint != experimental_record.ordered_input_fingerprint ||
             baseline_record.placement_signature != experimental_record.placement_signature)) {
            comparison.errors.emplace_back("placement signature changed for deterministic case " +
                                           baseline_record.id);
        }
        comparison.evidence.emplace_back(std::move(evidence));
    }
    for (auto const &[id, record] : experimental_cases) {
        static_cast<void>(record);
        if (!baseline_ids.contains(id)) {
            comparison.errors.emplace_back("experimental report adds unmatched case " + id);
        }
    }
    if (policy.require_strict_improvement && comparison.strict_improvements == 0) {
        comparison.errors.emplace_back(
            "portfolio produced zero strict canonical improvements on the approved corpus");
    }
    if (policy.require_strict_adversarial_improvement && comparison.strict_adversarial_improvements == 0) {
        comparison.errors.emplace_back(
            "portfolio produced zero strict canonical improvements on the approved adversarial corpus");
    }
    return comparison;
}

constexpr std::string_view COMPARISON_COLUMNS =
    "case_id\ttrap_class\tbudget_ms\tsemantic_input_fingerprint\tbaseline_ordered_input_fingerprint\t"
    "portfolio_ordered_input_fingerprint\trandom_seed\tbaseline_configured_worker_count\t"
    "baseline_runtime_observed_worker_count\tportfolio_configured_worker_count\t"
    "portfolio_runtime_observed_worker_count\t"
    "baseline_wall_ms\tportfolio_wall_ms\tbaseline_initial_incumbent_ms\tportfolio_initial_incumbent_ms\t"
    "baseline_iterations\tportfolio_iterations\tbaseline_placed_area\tportfolio_placed_area\t"
    "baseline_placed_count\tportfolio_placed_count\tbaseline_occupied_bounds_area\t"
    "portfolio_occupied_bounds_area\tbaseline_deadline_overrun_ms\tportfolio_deadline_overrun_ms\t"
    "baseline_valid\tportfolio_valid\trank_relation\tbaseline_placement_signature\t"
    "portfolio_placement_signature";

void write_comparison_report(std::string const &path, DifferentialReport const &baseline,
                             DifferentialReport const &portfolio, DifferentialComparison const &comparison)
{
    std::ofstream stream(path, std::ios::trunc);
    if (!stream) {
        throw std::runtime_error("unable to create portfolio comparison report: " + path);
    }
    stream << std::setprecision(17);
    stream << "VACARDS_NESTING_PORTFOLIO_DIFFERENTIAL\t1\n";
    stream << "baseline_backend\t" << baseline.backend << '\n';
    stream << "portfolio_backend\t" << portfolio.backend << '\n';
    stream << "budget_ms\t" << baseline.budget_ms << '\n';
    stream << "gate_passed\t" << (comparison.errors.empty() ? 1 : 0) << '\n';
    stream << "strict_improvements\t" << comparison.strict_improvements << '\n';
    stream << "strict_adversarial_improvements\t" << comparison.strict_adversarial_improvements << '\n';
    stream << "error_count\t" << comparison.errors.size() << '\n';
    stream << "columns\t" << COMPARISON_COLUMNS << '\n';
    for (auto const &row : comparison.evidence) {
        auto const &left = row.baseline;
        auto const &right = row.portfolio;
        stream << "case\t" << left.id << '\t' << left.trap_class << '\t' << baseline.budget_ms << '\t'
               << left.semantic_input_fingerprint << '\t' << left.ordered_input_fingerprint << '\t'
               << right.ordered_input_fingerprint << '\t' << left.random_seed << '\t'
               << baseline.configured_worker_count << '\t';
        write_optional(stream, baseline.runtime_observed_worker_count);
        stream << '\t' << portfolio.configured_worker_count << '\t';
        write_optional(stream, portfolio.runtime_observed_worker_count);
        stream << '\t' << left.wall_ms << '\t' << right.wall_ms << '\t';
        write_optional(stream, left.initial_incumbent_ms);
        stream << '\t';
        write_optional(stream, right.initial_incumbent_ms);
        stream << '\t' << left.iterations << '\t' << right.iterations << '\t' << left.placed_area << '\t'
               << right.placed_area << '\t' << left.placed_count << '\t' << right.placed_count << '\t';
        write_optional(stream, left.occupied_bounds_area);
        stream << '\t';
        write_optional(stream, right.occupied_bounds_area);
        stream << '\t';
        write_optional(stream, left.deadline_overrun_ms);
        stream << '\t';
        write_optional(stream, right.deadline_overrun_ms);
        stream << '\t' << (left.valid ? 1 : 0) << '\t' << (right.valid ? 1 : 0) << '\t'
               << row.rank_relation << '\t' << left.placement_signature << '\t' << right.placement_signature << '\n';
    }
    if (!stream) {
        throw std::runtime_error("unable to finish portfolio comparison report: " + path);
    }
}

auto check_differential_comparator_gates() -> bool
{
    DifferentialCaseRecord record{
        .id = "comparator-negative-control",
        .trap_class = "order-trap",
        .semantic_input_fingerprint = "0123456789abcdef",
        .ordered_input_fingerprint = "0123456789abcdef",
        .random_seed = 1234,
        .valid = true,
        .selected_count = 2,
        .selected_area = 100.0,
        .placed_count = 1,
        .placed_area = 60.0,
        .container_usable_area = 120.0,
        .utilization_percent = 50.0,
        .unplaced_count = 1,
        .unplaced_reason = "not_exposed_by_current_abi",
        .wall_ms = 1.0,
        .occupied_bounds_area = 80.0,
        .contour_source = "synthetic",
        .contour_fidelity = "exact",
        .placement_signature = "1111111111111111",
    };
    DifferentialReport baseline{
        .backend = "baseline",
        .mode = "first-incumbent",
        .budget_ms = 0,
        .configured_worker_count = 1,
        .input_order = "original",
        .cases = {record},
    };
    auto experimental = baseline;
    experimental.backend = "experimental";

    bool passed = true;
    passed &= check(compare_differential_reports(
                        baseline, experimental, {.require_identical = true,
                                                 .expected_baseline_worker_count = 1,
                                                 .expected_portfolio_worker_count = 1})
                        .errors.empty(),
                    "differential comparator rejected identical reports");

    auto worse = experimental;
    worse.cases.front().placed_area = 59.0;
    passed &= check(!compare_differential_reports(baseline, worse, {}).errors.empty(),
                    "differential comparator accepted a worse canonical rank");

    auto improved = experimental;
    improved.cases.front().placed_count = 2;
    improved.cases.front().unplaced_count = 0;
    improved.cases.front().unplaced_reason = "none";
    auto const improvement = compare_differential_reports(baseline, improved, {});
    passed &= check(improvement.errors.empty() && improvement.strict_improvements == 1 &&
                        improvement.strict_adversarial_improvements == 1,
                    "differential comparator did not record a strict canonical improvement");

    auto changed_input = experimental;
    changed_input.cases.front().semantic_input_fingerprint = "fedcba9876543210";
    passed &= check(!compare_differential_reports(baseline, changed_input, {}).errors.empty(),
                    "differential comparator accepted non-identical immutable inputs");

    auto permuted_input = experimental;
    permuted_input.input_order = "reversed";
    permuted_input.cases.front().ordered_input_fingerprint = "fedcba9876543210";
    passed &= check(!compare_differential_reports(baseline, permuted_input, {}).errors.empty() &&
                        compare_differential_reports(
                            baseline, permuted_input,
                            {.require_rank_equivalent = true, .allow_input_order_permutation = true})
                            .errors.empty(),
                    "input-order permutation policy did not preserve semantic input and canonical rank");

    auto changed_placement = experimental;
    changed_placement.cases.front().placement_signature = "2222222222222222";
    passed &= check(compare_differential_reports(baseline, changed_placement, {}).errors.empty() &&
                        !compare_differential_reports(baseline, changed_placement, {.require_identical = true})
                             .errors.empty(),
                    "deterministic differential mode did not enforce placement identity");

    auto activation_baseline = baseline;
    activation_baseline.mode = "finite-deadline";
    activation_baseline.budget_ms = 100;
    activation_baseline.cases.front().deadline_overrun_ms = 0.0;
    auto activation_portfolio = improved;
    activation_portfolio.mode = "finite-deadline";
    activation_portfolio.budget_ms = 100;
    activation_portfolio.configured_worker_count = 2;
    activation_portfolio.cases.front().deadline_overrun_ms = 0.0;
    passed &= check(compare_differential_reports(
                        activation_baseline, activation_portfolio,
                        {.require_strict_adversarial_improvement = true,
                         .expected_baseline_worker_count = 1,
                         .expected_portfolio_worker_count = 2})
                        .errors.empty(),
                    "activation comparator rejected valid strict adversarial improvement evidence");

    auto no_improvement = activation_portfolio;
    no_improvement.cases = activation_baseline.cases;
    passed &= check(!compare_differential_reports(
                         activation_baseline, no_improvement,
                         {.require_strict_improvement = true,
                          .expected_baseline_worker_count = 1,
                          .expected_portfolio_worker_count = 2})
                         .errors.empty(),
                    "activation comparator accepted zero strict improvements");
    passed &= check(compare_differential_reports(
                        activation_baseline, activation_portfolio,
                        {.require_strict_improvement = true,
                         .expected_baseline_worker_count = 1,
                         .expected_portfolio_worker_count = 2})
                        .errors.empty(),
                    "activation comparator rejected valid strict improvement evidence");
    passed &= check(!compare_differential_reports(
                         activation_baseline, no_improvement,
                         {.require_strict_adversarial_improvement = true,
                          .expected_baseline_worker_count = 1,
                          .expected_portfolio_worker_count = 2})
                         .errors.empty(),
                    "activation comparator accepted zero strict adversarial improvements");

    // With 21 cases a single outlier is above the nearest-rank p95. It must
    // still fail, for either role, at each of the existing budget boundaries.
    for (auto const &[budget_ms, allowed_ms] :
         {std::pair<std::uint64_t, double>{100, 250.0}, {1000, 250.0}, {5000, 250.0}, {60000, 3000.0}}) {
        auto timely = activation_baseline;
        timely.budget_ms = budget_ms;
        timely.cases.clear();
        for (unsigned index = 0; index < 21; ++index) {
            auto measured = activation_baseline.cases.front();
            measured.id = "deadline-case-" + std::to_string(index);
            measured.deadline_overrun_ms = 0.0;
            timely.cases.push_back(std::move(measured));
        }
        timely.cases.back().deadline_overrun_ms = allowed_ms;
        passed &= check(compare_differential_reports(timely, timely, {}).errors.empty(),
                        "deadline comparator rejected the inclusive per-case limit");

        for (auto invalid_overrun : {
                 std::optional<double>{std::nextafter(allowed_ms, std::numeric_limits<double>::infinity())},
                 std::optional<double>{-1.0},
                 std::optional<double>{std::numeric_limits<double>::quiet_NaN()},
                 std::optional<double>{std::numeric_limits<double>::infinity()},
                 std::optional<double>{-std::numeric_limits<double>::infinity()},
                 std::optional<double>{},
             }) {
            auto invalid = timely;
            invalid.cases.back().deadline_overrun_ms = invalid_overrun;
            auto const baseline_failure = compare_differential_reports(invalid, timely, {});
            auto const portfolio_failure = compare_differential_reports(timely, invalid, {});
            auto names_case_and_role = [](auto const &comparison, std::string_view role) {
                return std::ranges::any_of(comparison.errors, [role](auto const &error) {
                    return error.find(role) != std::string::npos &&
                           error.find("deadline-case-20") != std::string::npos;
                });
            };
            passed &= check(names_case_and_role(baseline_failure, "baseline") &&
                                names_case_and_role(portfolio_failure, "portfolio"),
                            "deadline comparator hid an invalid individual case below p95");
        }
        auto all_late = timely;
        for (auto &measured : all_late.cases) {
            measured.deadline_overrun_ms = allowed_ms + 1.0;
        }
        auto const late_comparison = compare_differential_reports(timely, all_late, {});
        passed &= check(std::ranges::any_of(late_comparison.errors, [](auto const &error) {
                            return error.find("portfolio deadline-overrun p95") != std::string::npos;
                        }),
                        "deadline comparator lost the aggregate p95 diagnostic");
    }
    return passed;
}

auto compare_differential_report_files(std::string const &baseline_path, std::string const &experimental_path,
                                       DifferentialComparisonPolicy const &policy,
                                       std::optional<std::string> const &comparison_report_path) -> bool
{
    auto const baseline = read_differential_report(baseline_path);
    auto const experimental = read_differential_report(experimental_path);
    auto const comparison = compare_differential_reports(baseline, experimental, policy);
    if (comparison_report_path) {
        write_comparison_report(*comparison_report_path, baseline, experimental, comparison);
    }
    for (auto const &error : comparison.errors) {
        std::cerr << "differential gate: " << error << '\n';
    }
    if (comparison.errors.empty()) {
        std::cout << "Compared " << baseline.cases.size() << " cases at " << baseline.budget_ms
                  << " ms: no canonical-rank regressions; " << comparison.strict_improvements
                  << " strict improvement(s), " << comparison.strict_adversarial_improvements
                  << " on the approved adversarial corpus.\n";
    }
    return comparison.errors.empty();
}

auto fixed_work_contract_tests() -> bool
{
    using Inkscape::Nesting::StopReason;
    bool passed = true;
    static_assert(sizeof(VacNestingTerminal) == 16);
    static_assert(offsetof(VacNestingTerminal, completed_work) == 8);
    VacNestingTerminal untouched{VAC_NESTING_STOP_TIME_LIMIT, 0, 123};
    passed &= check(vac_nesting_job_set_work_limit(nullptr, 17) == VAC_NESTING_STATUS_INVALID_ARGUMENT &&
                    vac_nesting_job_get_terminal(nullptr, &untouched) == VAC_NESTING_STATUS_INVALID_ARGUMENT &&
                    untouched.completed_work == 123, "null C ABI admission/output preservation failed");
    auto raw = vac_nesting_job_new(nullptr);
    passed &= check(raw && vac_nesting_job_get_terminal(raw, nullptr) == VAC_NESTING_STATUS_INVALID_ARGUMENT &&
                    vac_nesting_job_get_terminal(raw, &untouched) == VAC_NESTING_STATUS_INVALID_STATE &&
                    untouched.completed_work == 123, "nonterminal C ABI query failed");
    vac_nesting_job_free(raw);

    Options options;
    options.worker_count = 1;
    options.time_limit_ms = 0;
    options.random_seed = 0xfedcba9876543210ULL;
    options.rotation_mode = RotationMode::None;
    options.quality = Quality::Draft;
    std::vector<Placement> first;
    for (int run = 0; run < 2; ++run) {
        Job job(options);
        passed &= check(!job.terminalResult(), "terminal query accepted configuring job");
        passed &= check(job.setContainer(square(100)) == Status::Ok &&
                        job.addPart(1, square(10)) == Status::Ok && job.addPart(2, square(12)) == Status::Ok,
                        "fixed-work fixture setup failed");
        passed &= check(job.setWorkLimit(1027) == Status::Ok, "fixed-work setter failed");
        passed &= check(job.run() == Status::Ok, "fixed-work solve failed");
        auto terminal = job.terminalResult();
        passed &= check(terminal && terminal->stop_reason == StopReason::WorkLimit && terminal->completed_work == 1027,
                        "fixed-work terminal reason/count differs");
        auto result = job.results();
        if (!run) first = result;
        else {
            passed &= check(first.size() == result.size(), "deterministic result count differs");
            for (std::size_t i = 0; i < std::min(first.size(), result.size()); ++i)
                passed &= check(first[i].part_id == result[i].part_id && first[i].placed == result[i].placed &&
                    first[i].translation_x == result[i].translation_x && first[i].translation_y == result[i].translation_y &&
                    first[i].rotation_degrees == result[i].rotation_degrees, "fixed-work placements differ");
        }
        Job validator(options);
        passed &= check(validator.setContainer(square(100)) == Status::Ok &&
            validator.addPart(1, square(10)) == Status::Ok && validator.addPart(2, square(12)) == Status::Ok &&
            validator.validate(result) == Status::Ok, "fixed-work result violates independent geometry validation");
        auto validated = validator.terminalResult();
        passed &= check(validated && validated->stop_reason == StopReason::Completed && validated->completed_work == 0,
                        "natural validation completion should not count preparation as work");
        passed &= check(job.setWorkLimit(0) == Status::InvalidState, "terminal setter accepted");
    }
    Job cancelled(options);
    passed &= check(cancelled.setContainer(square(100)) == Status::Ok && cancelled.addPart(1, square(10)) == Status::Ok &&
                    cancelled.setWorkLimit(100000) == Status::Ok, "cancel fixture setup failed");
    auto started = std::chrono::steady_clock::now();
    passed &= check(cancelled.run([&](auto const &p) { if (p.iteration) cancelled.cancel(); }) == Status::Cancelled,
                    "limited job failed cancellation");
    auto terminal = cancelled.terminalResult();
    passed &= check(terminal && terminal->stop_reason == StopReason::Cancelled && terminal->completed_work < 100000,
                    "cancellation terminal query failed");
    passed &= check(std::chrono::steady_clock::now() - started < std::chrono::seconds(2), "cancellation was not prompt");
    Job legacy;
    passed &= check(legacy.setWorkLimit(1) == Status::InvalidArgument && legacy.setWorkLimit(0) == Status::Ok,
                    "work-limit option admission failed");
    options.time_limit_ms = 1;
    Job timed(options);
    passed &= check(timed.setWorkLimit(0) == Status::Ok && timed.setContainer(square(100)) == Status::Ok &&
        timed.addPart(1, square(10)) == Status::Ok && timed.run() == Status::Ok, "zero limit changed legacy run");
    auto timed_terminal = timed.terminalResult();
    passed &= check(timed_terminal && timed_terminal->stop_reason == StopReason::TimeLimit,
                    "time-limit terminal result failed");
    std::cout << "SEAM-NEST fixed-work C++ contract: " << (passed ? "PASS" : "FAIL") << '\n';
    return passed;
}

auto run_contract_tests() -> int
{
    try {
        bool passed = fixed_work_contract_tests();

        passed &= check(std::string(Inkscape::Nesting::statusMessage(Status::InternalError)) == "internal solver error",
                        "status message table is out of sync");

        Job validation;
        auto validation_shape = square(20.0);
        passed &= check(validation.addContainerHole(validation_shape) == Status::InvalidState,
                        "a hole was accepted before its container");
        std::vector<Point> line = {{0.0, 0.0}, {1.0, 1.0}, {2.0, 2.0}};
        passed &= check(validation.setContainer(line) == Status::InvalidArgument, "zero-area container was accepted");
        passed &= check(validation.setContainer(validation_shape) == Status::Ok,
                        "job did not recover from a correctable input error");
        passed &= check(validation.run() == Status::InvalidArgument, "job without parts was allowed to run");
        passed &= check(validation.state() == JobState::Configuring, "correctable run error made the job terminal");

        // JobAddObstacleForwardsToEngine (add-to-sheet work order 10.1).
        {
            Job obstacle_job;
            auto const obstacle = square(20.0);
            passed &= check(obstacle_job.addObstacle(obstacle) == Status::InvalidState,
                            "an obstacle was accepted before its container");
            std::vector<Point> const sheet = {{0.0, 0.0}, {50.0, 0.0}, {50.0, 30.0}, {0.0, 30.0}};
            passed &= check(obstacle_job.setContainer(sheet) == Status::Ok, "obstacle test container failed");
            passed &= check(obstacle_job.addObstacle(obstacle) == Status::Ok, "obstacle was rejected");
            passed &= check(obstacle_job.addPart(1, square(10.0)) == Status::Ok, "obstacle test part failed");
            passed &= check(obstacle_job.run() == Status::Ok, "obstacle run failed");
            auto const results = obstacle_job.results();
            bool beside = false;
            if (results.size() == 1 && results[0].placed) {
                // The part's corners after rotation and translation.
                auto const radians = results[0].rotation_degrees * std::numbers::pi / 180.0;
                double min_x = std::numeric_limits<double>::infinity();
                double min_y = std::numeric_limits<double>::infinity();
                for (auto const &corner : square(10.0)) {
                    auto const x = corner.x * std::cos(radians) - corner.y * std::sin(radians) + results[0].translation_x;
                    auto const y = corner.x * std::sin(radians) + corner.y * std::cos(radians) + results[0].translation_y;
                    min_x = std::min(min_x, x);
                    min_y = std::min(min_y, y);
                }
                beside = min_x >= 20.0 - 1e-3 || min_y >= 20.0 - 1e-3;
            }
            if (!beside) {
                std::cerr << "obstacle result count=" << results.size();
                for (auto const &r : results)
                    std::cerr << " placed=" << r.placed << " t=(" << r.translation_x << "," << r.translation_y
                              << ") rot=" << r.rotation_degrees;
                std::cerr << '\n';
            }
            passed &= check(beside, "the part was not placed beside the obstacle");
        }

        Job pre_cancelled;
        pre_cancelled.cancel();
        passed &= check(pre_cancelled.run() == Status::Cancelled, "pre-run cancellation was not preserved");
        passed &=
            check(pre_cancelled.state() == JobState::Cancelled, "pre-run cancellation has the wrong terminal state");

        auto container = square(100.0);
        auto part = square(10.0);
        Job job;
        passed &= check(job.state() == JobState::Configuring, "new job was not configurable");
        passed &= check(job.setContainer(container) == Status::Ok, "container was rejected");
        passed &= check(job.addPart(11, part) == Status::Ok, "part was rejected");
        passed &= check(job.addPart(11, part) == Status::InvalidArgument, "duplicate part ID was accepted");

        // The Rust boundary owns copies; caller memory can change immediately.
        for (auto &point : container) {
            point = {NAN, NAN};
        }
        for (auto &point : part) {
            point = {NAN, NAN};
        }

        unsigned callbacks = 0;
        bool saw_best_snapshot = false;
        double last_best_score = 0.0;
        auto status = job.run([&](auto const &progress) {
            ++callbacks;
            passed &= check(progress.total_count == 1, "progress lost the part count");
            passed &= check(progress.best_score >= last_best_score, "best-so-far progress regressed");
            last_best_score = progress.best_score;
            if (!progress.placements.empty()) {
                saw_best_snapshot = true;
                passed &= check(progress.placements.size() == 1 && progress.placements[0].part_id == 11 &&
                                    progress.placements[0].placed,
                                "progress snapshot did not contain the current best placement");
            }
        });
        passed &= check(status == Status::Ok, "the Jagua solver did not complete");
        passed &= check(job.state() == JobState::Completed, "successful solver run was not terminal");
        passed &= check(callbacks >= 1, "run did not publish progress");
        passed &= check(saw_best_snapshot, "run did not publish a best-so-far placement snapshot");
        auto results = job.results();
        passed &= check(results.size() == 1 && results[0].part_id == 11 && results[0].placed,
                        "successful run did not expose the placement");
        passed &= check(job.error().empty(), "successful run retained a stale error");

        Job cancellable;
        auto shape = square(25.0);
        passed &= check(cancellable.setContainer(square(100.0)) == Status::Ok, "cancellation test container failed");
        passed &= check(cancellable.addPart(1, square(10.0)) == Status::Ok, "cancellation test part failed");
        status = cancellable.run([&](auto const &progress) {
            if (progress.stage == 1 && progress.iteration >= 256) {
                cancellable.cancel();
            }
        });
        passed &= check(status == Status::Cancelled, "callback cancellation was ignored");
        passed &= check(cancellable.state() == JobState::Cancelled, "cancelled job has the wrong state");
        passed &= check(cancellable.results().empty(), "cancelled job published partial results");

        Job partial;
        passed &= check(partial.setContainer(square(30.0)) == Status::Ok, "partial test container failed");
        passed &= check(partial.addPart(20, square(40.0)) == Status::Ok, "oversized part was rejected early");
        passed &= check(partial.addPart(21, square(10.0)) == Status::Ok, "small part setup failed");
        passed &= check(partial.run() == Status::Ok, "partial placement run failed");
        auto partial_results = partial.results();
        passed &= check(partial_results.size() == 2, "partial placement lost an input part");
        passed &= check(partial_results.size() == 2 && partial_results[0].part_id == 20 && !partial_results[0].placed,
                        "oversized part was not explicitly unplaced");
        passed &= check(partial_results.size() == 2 && partial_results[1].part_id == 21 && partial_results[1].placed,
                        "solver stopped instead of placing the later fitting part");

        Job throwing_callback;
        passed &= check(throwing_callback.setContainer(shape) == Status::Ok, "throwing callback container failed");
        passed &= check(throwing_callback.addPart(2, shape) == Status::Ok, "throwing callback part failed");
        bool exception_was_contained = false;
        try {
            (void)throwing_callback.run([](auto const &) { throw std::runtime_error("controlled callback failure"); });
        } catch (std::runtime_error const &error) {
            exception_was_contained = std::string(error.what()) == "controlled callback failure";
        }
        passed &= check(exception_was_contained, "C++ exception crossed the C/Rust boundary or was lost");
        passed &= check(throwing_callback.state() == JobState::Cancelled, "callback failure did not cancel its job");

        Job moved_from;
        Job moved_to = std::move(moved_from);
        passed &= check(!moved_from, "moved-from job retained ownership");
        passed &= check(moved_to.state() == JobState::Configuring, "move construction lost the native job");
        passed &=
            check(moved_from.setContainer(shape) == Status::InvalidState, "moved-from job accepted configuration");

        bool invalid_options_rejected = false;
        try {
            Options invalid;
            invalid.part_spacing = NAN;
            Job invalid_job(invalid);
        } catch (std::invalid_argument const &) {
            invalid_options_rejected = true;
        }
        passed &= check(invalid_options_rejected, "invalid options created a job");

        passed &= check_aggregate_defaults();
        passed &= check_authoritative_fixture_gates();
        passed &= check_validator_rejects_malformed_output();
        passed &= check_legal_contact_and_clearance_semantics();
        passed &= check_metamorphic_gates();
        passed &= check_deadline_progress_and_monotonicity();
        passed &= check_cancellation_phase_and_latency_gates();
        passed &= check_adversarial_fixture_gates();
        passed &= check_differential_comparator_gates();

        return passed ? 0 : 1;
    } catch (std::exception const &error) {
        std::cerr << "unexpected exception: " << error.what() << '\n';
        return 2;
    }
}

void print_usage(char const *program)
{
    std::cerr << "Usage:\n"
              << "  " << program << "\n"
              << "  " << program
              << " --benchmark-report FILE --backend-label LABEL [--budget-ms MILLISECONDS]"
                 " [--worker-count COUNT] [--part-order original|reversed]\n"
              << "  " << program
              << " --compare-reports BASELINE PORTFOLIO [--comparison-report FILE]"
                 " [--expected-baseline-workers COUNT] [--expected-portfolio-workers COUNT]"
                 " [--require-identical] [--require-rank-equivalent]"
                 " [--allow-input-order-permutation] [--require-strict-improvement]"
                 " [--require-strict-adversarial-improvement]\n\n"
              << "Opt-in CTest gates:\n"
              << "  VACARDS_NESTING_RUN_PORTFOLIO_DIFFERENTIAL=1 ctest --test-dir BUILD"
                 " -R vacards-nesting-portfolio-differential\n"
              << "  VACARDS_NESTING_RUN_PORTFOLIO_DIFFERENTIAL_60S=1 ctest --test-dir BUILD"
                 " -R vacards-nesting-portfolio-differential-60000ms\n";
}

auto parse_worker_count(std::string const &value, std::string_view field) -> std::uint32_t
{
    auto const parsed = parse_unsigned(value, field);
    if (parsed == 0 || parsed > 1024) {
        throw std::invalid_argument(std::string(field) + " must be in [1, 1024]");
    }
    return static_cast<std::uint32_t>(parsed);
}

} // namespace

int main(int argc, char **argv)
{
    if (argc == 1) {
        return run_contract_tests();
    }
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        print_usage(argv[0]);
        return 0;
    }

    try {
        if (std::string_view(argv[1]) == "--compare-reports") {
            if (argc < 4) {
                print_usage(argv[0]);
                return 64;
            }
            DifferentialComparisonPolicy policy;
            std::optional<std::string> comparison_report_path;
            for (int index = 4; index < argc;) {
                auto const option = std::string_view(argv[index]);
                if (option == "--require-identical") {
                    policy.require_identical = true;
                    ++index;
                } else if (option == "--require-rank-equivalent") {
                    policy.require_rank_equivalent = true;
                    ++index;
                } else if (option == "--allow-input-order-permutation") {
                    policy.allow_input_order_permutation = true;
                    ++index;
                } else if (option == "--require-strict-improvement") {
                    policy.require_strict_improvement = true;
                    ++index;
                } else if (option == "--require-strict-adversarial-improvement") {
                    policy.require_strict_adversarial_improvement = true;
                    ++index;
                } else if (index + 1 < argc && option == "--comparison-report" && !comparison_report_path) {
                    comparison_report_path = argv[index + 1];
                    index += 2;
                } else if (index + 1 < argc && option == "--expected-baseline-workers" &&
                           !policy.expected_baseline_worker_count) {
                    policy.expected_baseline_worker_count =
                        parse_worker_count(argv[index + 1], "expected_baseline_workers");
                    index += 2;
                } else if (index + 1 < argc && option == "--expected-portfolio-workers" &&
                           !policy.expected_portfolio_worker_count) {
                    policy.expected_portfolio_worker_count =
                        parse_worker_count(argv[index + 1], "expected_portfolio_workers");
                    index += 2;
                } else {
                    print_usage(argv[0]);
                    return 64;
                }
            }
            if (policy.require_identical && policy.allow_input_order_permutation) {
                throw std::invalid_argument("identical placement and input-order permutation are mutually exclusive");
            }
            return compare_differential_report_files(argv[2], argv[3], policy, comparison_report_path) ? 0 : 1;
        }

        std::optional<std::string> report_path;
        std::optional<std::string> backend_label;
        std::optional<std::uint64_t> budget_ms;
        std::optional<std::uint32_t> worker_count;
        FixturePartOrder part_order = FixturePartOrder::Original;
        bool part_order_set = false;
        for (int index = 1; index < argc; index += 2) {
            if (index + 1 >= argc) {
                print_usage(argv[0]);
                return 64;
            }
            auto const option = std::string_view(argv[index]);
            auto const value = std::string(argv[index + 1]);
            if (option == "--benchmark-report" && !report_path) {
                report_path = value;
            } else if (option == "--backend-label" && !backend_label) {
                backend_label = value;
            } else if (option == "--budget-ms" && !budget_ms) {
                budget_ms = parse_unsigned(value, "budget_ms");
            } else if (option == "--worker-count" && !worker_count) {
                worker_count = parse_worker_count(value, "worker_count");
            } else if (option == "--part-order" && !part_order_set) {
                part_order = parse_part_order(value);
                part_order_set = true;
            } else {
                print_usage(argv[0]);
                return 64;
            }
        }
        if (!report_path || !backend_label) {
            print_usage(argv[0]);
            return 64;
        }
        return write_differential_report(*report_path, *backend_label, budget_ms.value_or(0),
                                         worker_count.value_or(1), part_order)
                   ? 0
                   : 1;
    } catch (std::exception const &error) {
        std::cerr << "differential harness error: " << error.what() << '\n';
        return 2;
    }
}
