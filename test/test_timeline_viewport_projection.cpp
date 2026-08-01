#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <type_traits>

#include <pulp/timeline_editor/viewport_projection.hpp>

using namespace pulp;
using namespace pulp::timeline_editor;

TEST_CASE("Tick projection maps a signed musical viewport in both directions",
          "[timeline-editor][viewport-projection]") {
    constexpr auto quarter = timebase::kTicksPerQuarter;
    auto projection = TickProjection::create({-quarter}, {4 * quarter}, {25.0f, 800.0f});
    REQUIRE(projection);

    CHECK(projection->x_at({-quarter}) == 25.0f);
    CHECK(projection->x_at({quarter}) == 425.0f);
    CHECK(projection->x_at({3 * quarter}) == 825.0f);
    CHECK(projection->tick_at(25.0f) == timebase::TickPosition{-quarter});
    CHECK(projection->tick_at(425.0f) == timebase::TickPosition{quarter});
    CHECK(projection->tick_at(825.0f) == timebase::TickPosition{3 * quarter});

    // Rendering remains continuous across the viewport boundary, while pointer
    // lookup stays inside the range the user can see.
    CHECK(projection->x_at({-2 * quarter}) == -175.0f);
    CHECK(projection->tick_at(-175.0f) == timebase::TickPosition{-quarter});
    CHECK(projection->tick_at(1025.0f) == timebase::TickPosition{3 * quarter});
}

TEST_CASE("Tick projection rounds to the nearest tick without overflowing",
          "[timeline-editor][viewport-projection]") {
    auto projection = TickProjection::create({-10}, {20}, {0.0f, 80.0f});
    REQUIRE(projection);

    CHECK(projection->tick_at(42.0f) == timebase::TickPosition{1});
    CHECK(projection->tick_at(38.0f) == timebase::TickPosition{-1});

    auto extreme =
        TickProjection::create({std::numeric_limits<std::int64_t>::min()},
                               {std::numeric_limits<std::int64_t>::max()}, {-100.0f, 200.0f});
    REQUIRE(extreme);
    CHECK(std::isfinite(extreme->x_at({std::numeric_limits<std::int64_t>::max()})));
    CHECK(extreme->tick_at(-std::numeric_limits<float>::infinity()) == extreme->visible_start());
    CHECK(extreme->tick_at(std::numeric_limits<float>::infinity()) == extreme->visible_end());
}

TEST_CASE("Invalid tick viewports fail before projection math can become ambiguous",
          "[timeline-editor][viewport-projection]") {
    using Error = ViewportProjectionError;
    const auto maximum = std::numeric_limits<std::int64_t>::max();

    auto zero_duration = TickProjection::create({0}, {0}, {0.0f, 100.0f});
    REQUIRE_FALSE(zero_duration);
    CHECK(zero_duration.error() == Error::NonPositiveTickDuration);

    auto negative_extent = TickProjection::create({0}, {1}, {0.0f, -1.0f});
    REQUIRE_FALSE(negative_extent);
    CHECK(negative_extent.error() == Error::NonPositivePixelExtent);

    auto indistinguishable_extent =
        TickProjection::create({0}, {1}, {std::numeric_limits<float>::max() / 2.0f, 1.0f});
    REQUIRE_FALSE(indistinguishable_extent);
    CHECK(indistinguishable_extent.error() == Error::NonPositivePixelExtent);

    auto non_finite =
        TickProjection::create({0}, {1}, {std::numeric_limits<float>::quiet_NaN(), 100.0f});
    REQUIRE_FALSE(non_finite);
    CHECK(non_finite.error() == Error::NonFinitePixelSpan);

    auto overflow = TickProjection::create({maximum}, {1}, {0.0f, 100.0f});
    REQUIRE_FALSE(overflow);
    CHECK(overflow.error() == Error::TickRangeOverflow);
}

TEST_CASE("Pitch projection assigns higher notes to higher piano-roll rows",
          "[timeline-editor][viewport-projection]") {
    auto projection = PitchProjection::create(60, 71, {10.0f, 240.0f});
    REQUIRE(projection);
    CHECK(projection->row_height() == 20.0f);
    CHECK(projection->y_at(71) == 20.0f);
    CHECK(projection->y_at(70) == 40.0f);
    CHECK(projection->y_at(60) == 240.0f);
    CHECK(projection->y_at(72) == 0.0f);
    CHECK(projection->y_at(59) == 260.0f);

    for (std::uint8_t pitch = 60; pitch <= 71; ++pitch)
        CHECK(projection->pitch_at(projection->y_at(pitch)) == pitch);

    CHECK(projection->pitch_at(10.0f) == 71);
    CHECK(projection->pitch_at(29.999f) == 71);
    CHECK(projection->pitch_at(30.0f) == 70);
    CHECK(projection->pitch_at(-100.0f) == 71);
    CHECK(projection->pitch_at(1000.0f) == 60);
    CHECK(projection->pitch_at(std::numeric_limits<float>::quiet_NaN()) == 71);

    auto full_range = PitchProjection::create(0, 127, {0.0f, 128.0f});
    REQUIRE(full_range);
    for (std::int32_t pitch = 0; pitch <= 127; ++pitch) {
        const auto midi_pitch = static_cast<std::uint8_t>(pitch);
        CHECK(full_range->pitch_at(full_range->y_at(midi_pitch)) == midi_pitch);
    }
}

TEST_CASE("Pitch projection rejects invalid MIDI and pixel ranges",
          "[timeline-editor][viewport-projection]") {
    using Error = ViewportProjectionError;

    auto reversed = PitchProjection::create(72, 60, {0.0f, 100.0f});
    REQUIRE_FALSE(reversed);
    CHECK(reversed.error() == Error::InvalidPitchRange);

    auto outside_midi = PitchProjection::create(0, 128, {0.0f, 100.0f});
    REQUIRE_FALSE(outside_midi);
    CHECK(outside_midi.error() == Error::InvalidPitchRange);

    auto infinite = PitchProjection::create(0, 127, {0.0f, std::numeric_limits<float>::infinity()});
    REQUIRE_FALSE(infinite);
    CHECK(infinite.error() == Error::NonFinitePixelSpan);

    static_assert(noexcept(std::declval<const PitchProjection&>().pitch_at(0.0f)));
    static_assert(noexcept(std::declval<const TickProjection&>().tick_at(0.0f)));
}
