#include <catch2/catch_test_macros.hpp>

#include <type_traits>

#include <pulp/playback/transport.hpp>
#include <pulp/timebase/loop_region.hpp>
#include <pulp/timeline_editor/sequencer_ui_host.hpp>

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
