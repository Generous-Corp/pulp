#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <utility>

#include <pulp/playback/transport.hpp>
#include <pulp/timebase/compiled_tempo_map.hpp>
#include <pulp/timebase/loop_region.hpp>
#include <pulp/timeline_editor/loop_range.hpp>
#include <pulp/timeline_editor/sequencer_ui_host.hpp>
#include <pulp/timeline_editor/snap_grid.hpp>

using namespace pulp;

namespace {

constexpr std::int64_t quarters(std::int64_t count) {
    return count * timebase::kTicksPerQuarter;
}

/// Stands where a session shell stands: above both rungs, describing a loop in
/// the only vocabulary either of them has. Neither call site below converts,
/// because there is nothing left to convert between.
constexpr timebase::TickDuration loop_length(const timebase::LoopRegion& loop) {
    return timebase::TickDuration{loop.enabled ? loop.end.value - loop.start.value : 0};
}

template <typename T, typename E> T take(runtime::Result<T, E> result) {
    REQUIRE(result);
    return std::move(result).value();
}

} // namespace

// This suite is one of the few places allowed to name both rungs at once. It
// links pulp::playback and pulp::timeline-editor deliberately, standing where a
// front end stands, which is exactly what neither rung may do for itself — and
// is what makes "both rungs describe the same loop" a statement a build can
// check rather than a claim two headers make separately.
TEST_CASE("Transport and editor rungs describe a loop with one type",
          "[timebase][timeline-editor][playback][layering]") {
    // Each rung's own spelling names the timebase type, so a loop crossing the
    // seam is the same object rather than a field-for-field copy that a later
    // edit to one struct could silently desynchronise.
    REQUIRE(std::is_same_v<playback::LoopRegion, timebase::LoopRegion>);
    REQUIRE(std::is_same_v<decltype(playback::TransportPlayhead::loop), timebase::LoopRegion>);
    REQUIRE(std::is_same_v<decltype(timeline_editor::UiPlayhead::loop), timebase::LoopRegion>);
    REQUIRE(std::is_same_v<decltype(playback::TransportSnapshot::loop), timebase::LoopRegion>);
}

TEST_CASE("A loop set on the transport reaches an editor reading unconverted",
          "[timebase][timeline-editor][playback]") {
    const playback::LoopRegion bars_one_to_three{true, timebase::TickPosition{quarters(4)},
                                                 timebase::TickPosition{quarters(12)}};

    playback::TransportPlayhead transport_reading;
    transport_reading.loop = bars_one_to_three;

    // No conversion, no field-by-field assignment, no adapter: the editor's
    // reading takes the transport's loop whole.
    timeline_editor::UiPlayhead ui_reading;
    ui_reading.loop = transport_reading.loop;

    REQUIRE(ui_reading.loop == transport_reading.loop);
    // The shared vocabulary is what lets one measurement serve both readings.
    REQUIRE(loop_length(transport_reading.loop).value == quarters(8));
    REQUIRE(loop_length(ui_reading.loop).value == quarters(8));

    // A disabled loop keeps its bounds, so a view still has a region to draw
    // and re-enabling returns the user to what they set up.
    ui_reading.loop.enabled = false;
    REQUIRE(ui_reading.loop.start.value == quarters(4));
    REQUIRE(ui_reading.loop.end.value == quarters(12));
    REQUIRE(loop_length(ui_reading.loop).value == 0);
    REQUIRE_FALSE(ui_reading.loop == transport_reading.loop);
}

TEST_CASE("Loop regions order, not merely compare", "[timebase]") {
    // The transport rung's ordering is what survived the move; an editor
    // consumer inherits it rather than being limited to equality.
    constexpr timebase::LoopRegion earlier{true, timebase::TickPosition{0},
                                           timebase::TickPosition{quarters(4)}};
    constexpr timebase::LoopRegion later{true, timebase::TickPosition{quarters(4)},
                                         timebase::TickPosition{quarters(8)}};

    static_assert(earlier < later);
    static_assert(earlier <= earlier);
    REQUIRE(earlier < later);
    REQUIRE((earlier <=> later) < 0);
    REQUIRE((earlier <=> earlier) == 0);

    // Disabled sorts before enabled at the same bounds, which is the field
    // order the transport already relied on to detect a loop change.
    constexpr timebase::LoopRegion disabled{false, timebase::TickPosition{0},
                                            timebase::TickPosition{quarters(4)}};
    static_assert(disabled < earlier);
    REQUIRE(disabled < earlier);
}

TEST_CASE("Loop range edits canonicalize already-snapped endpoints",
          "[timebase][timeline-editor][loop-range]") {
    using timeline_editor::loop_region_from_snapped_endpoints;

    const auto forward = loop_region_from_snapped_endpoints(
        timebase::TickPosition{-quarters(3)}, timebase::TickPosition{quarters(2)});
    REQUIRE(forward);
    CHECK(*forward == timebase::LoopRegion{true, {-quarters(3)}, {quarters(2)}});

    const auto reverse = loop_region_from_snapped_endpoints(
        timebase::TickPosition{quarters(2)}, timebase::TickPosition{-quarters(3)});
    REQUIRE(reverse);
    CHECK(*reverse == *forward);

    const auto negative_reverse = loop_region_from_snapped_endpoints(
        timebase::TickPosition{-quarters(1)}, timebase::TickPosition{-quarters(4)});
    REQUIRE(negative_reverse);
    CHECK(*negative_reverse == timebase::LoopRegion{true, {-quarters(4)}, {-quarters(1)}});

    constexpr auto minimum = std::numeric_limits<std::int64_t>::min();
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    const auto rails = loop_region_from_snapped_endpoints(timebase::TickPosition{maximum},
                                                         timebase::TickPosition{minimum});
    REQUIRE(rails);
    CHECK(*rails == timebase::LoopRegion{true, {minimum}, {maximum}});
}

TEST_CASE("Loop range edits preserve the caller's snap result and reject collapse",
          "[timebase][timeline-editor][loop-range][snap-grid]") {
    const std::array meter_points{timebase::MeterPoint{{0}, {4, 4}}};
    const auto meter = take(timebase::CompiledMeterMap::compile(meter_points));
    const auto grid = take(timeline_editor::SnapGrid::create(
        timebase::TickDuration{timebase::kTicksPerQuarter / 2}));

    const auto midpoint = timebase::TickPosition{timebase::kTicksPerQuarter / 4};
    const auto tied_later = grid.snap(meter, midpoint, timeline_editor::SnapDirection::Nearest);
    REQUIRE(tied_later == timebase::TickPosition{timebase::kTicksPerQuarter / 2});

    const auto tied_range = timeline_editor::loop_region_from_snapped_endpoints(
        timebase::TickPosition{0}, tied_later);
    REQUIRE(tied_range);
    CHECK(tied_range->end == tied_later);

    const auto snapped_first = grid.snap(meter, timebase::TickPosition{1});
    const auto snapped_second = grid.snap(meter, timebase::TickPosition{2});
    REQUIRE(snapped_first == snapped_second);
    const auto collapsed =
        timeline_editor::loop_region_from_snapped_endpoints(snapped_first, snapped_second);
    REQUIRE_FALSE(collapsed);
    CHECK(collapsed.error() == timeline_editor::LoopRangeError::CollapsedSpan);
}

TEST_CASE("Loop enable edits preserve both authored bounds",
          "[timebase][timeline-editor][loop-range]") {
    const auto active = timeline_editor::loop_region_from_snapped_endpoints(
        timebase::TickPosition{-quarters(2)}, timebase::TickPosition{quarters(5)});
    REQUIRE(active);

    const auto disabled = timeline_editor::with_loop_enabled(*active, false);
    CHECK_FALSE(disabled.enabled);
    CHECK(disabled.start == active->start);
    CHECK(disabled.end == active->end);

    const auto reenabled = timeline_editor::with_loop_enabled(disabled, true);
    CHECK(reenabled == *active);
}

TEST_CASE("A canonical editor loop candidate is consumed by the transport unchanged",
          "[timebase][timeline-editor][playback][loop-range]") {
    const std::array tempo_points{timebase::TempoPoint{{0}, 120.0}};
    const auto tempo = take(timebase::CompiledTempoMap::compile(
        tempo_points, timebase::RationalRate{48'000, 1}));
    const auto candidate = timeline_editor::loop_region_from_snapped_endpoints(
        timebase::TickPosition{quarters(4)}, timebase::TickPosition{quarters(8)});
    REQUIRE(candidate);

    playback::MasterTransportConfig config;
    config.max_buffer_size = 64;
    playback::MasterTransport transport;
    REQUIRE(transport.prepare(tempo, config) == playback::TransportError::None);
    REQUIRE(transport.set_loop(*candidate) == playback::TransportError::None);

    playback::TransportSnapshot snapshot;
    REQUIRE(transport.begin_block(64, snapshot) == playback::TransportError::None);
    CHECK(snapshot.loop == *candidate);
    CHECK(transport.playhead().loop == *candidate);
}
