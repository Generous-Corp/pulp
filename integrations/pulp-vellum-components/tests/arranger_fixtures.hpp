#pragma once

// Fixtures shared by the arranger tests: the unit layout, the view wiring and
// the canonical-JSON helpers that make a document edit observable.

#include "timeline_command_test_helpers.hpp"

#include <pulp/timeline/schema_registry.hpp>
#include <pulp/timeline/serialize.hpp>
#include <pulp/timeline_editor/edit_intent.hpp>
#include <pulp/vellum_components/arranger_view.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

namespace timeline_view_test {

using namespace pulp::timeline;
using namespace pulp::timeline_editor;
using namespace pulp::vellum_components;
using namespace timeline_test;
using ::vellum::view::HitMetrics;
using ::vellum::view::PointerType;

/// One pixel per tick, with the lane column starting at x=120, so a pixel
/// delta and a tick delta are the same number and every expected value in
/// these tests is readable without arithmetic. The fixture clip is one quarter
/// (kTicksPerQuarter ticks) long, so it occupies x=[120, 120+kTicksPerQuarter)
/// and the view is given bounds wide enough to hold all of it -- otherwise
/// paint clips it at the right edge and empty-lane clicks land inside it.
inline ArrangerLayout unit_layout() {
    ArrangerLayout layout;
    layout.origin_tick = {0};
    layout.px_per_tick = 1.0;
    layout.track_height_px = 40.0f;
    layout.lane_left_px = 120.0f;
    layout.ruler_height_px = 0.0f;
    return layout;
}

/// The fixture project places one clip at tick 0 with a quarter-note duration
/// on track {4} of sequence {3}.
inline ArrangerView& configure(ArrangerView& view, const Project& project, EditIntentHost& host) {
    view.set_bounds({0, 0, 120.0f + kTicksPerQuarter + 800.0f, 200});
    view.set_project(&project, {3});
    view.set_layout(unit_layout());
    view.set_host(&host);
    view.set_hit_metrics(HitMetrics::for_pointer(PointerType::mouse));
    return view;
}

/// The builtin schema registry, which every serialize path needs and which
/// cannot fail for the builtin set.
inline SchemaRegistry builtin_registry() {
    auto registry = make_builtin_timeline_registry();
    REQUIRE(registry);
    return std::move(registry).value();
}

inline std::string canonical_project(const Project& project) {
    const auto registry = builtin_registry();
    auto encoded = serialize_project(project, registry);
    REQUIRE(encoded);
    return std::move(encoded).value().json;
}

inline std::vector<ItemId> track_order_after_round_trip(const Project& project) {
    const auto registry = builtin_registry();
    auto encoded = serialize_project(project, registry);
    REQUIRE(encoded);
    auto decoded = deserialize_project(encoded.value().json, registry);
    REQUIRE(decoded);
    const auto order = decoded.value().find_sequence({3})->track_order();
    return {order.begin(), order.end()};
}

inline Project make_authored_track_project() {
    auto first = Track::create({4}, "first", {});
    auto second = Track::create({7}, "second", {});
    auto third = Track::create({8}, "third", {});
    REQUIRE(first);
    REQUIRE(second);
    REQUIRE(third);

    SequenceInput sequence_input;
    sequence_input.id = {3};
    sequence_input.name = "sequence";
    sequence_input.musical_duration = TickDuration{8 * kTicksPerQuarter};
    sequence_input.tracks.push_back(std::move(first).value());
    sequence_input.tracks.push_back(std::move(second).value());
    sequence_input.tracks.push_back(std::move(third).value());
    sequence_input.track_order = {{8}, {4}, {7}};
    auto sequence = Sequence::create(std::move(sequence_input));
    REQUIRE(sequence);

    ProjectInput project_input;
    project_input.id = {1};
    project_input.name = "project";
    project_input.next_item_id = 9;
    project_input.root_sequence_id = {3};
    project_input.sequences.push_back(std::move(sequence).value());
    auto project = Project::create(std::move(project_input));
    REQUIRE(project);
    return std::move(project).value();
}

} // namespace timeline_view_test
