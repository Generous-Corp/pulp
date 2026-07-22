#pragma once

#include <pulp/timebase/compiled_meter_map.hpp>
#include <pulp/timebase/tick.hpp>
#include <pulp/timeline/item_id.hpp>

#include <compare>
#include <cstdint>
#include <string>
#include <vector>

// Authored, non-linear launch model. Where the linear model (model.hpp) places
// clips at absolute timeline positions, a launch surface exposes clips that are
// triggered on demand and quantized to a musical boundary. These are lightweight
// value types describing WHAT is launchable and HOW it quantizes; the sample-
// accurate resolution of a trigger to a boundary lives in the playback engine
// (pulp/playback/clip_launch.hpp). They are deliberately not yet wired into the
// persistent Project identity/serialization graph.
namespace pulp::timeline {

// How a launch snaps to a musical boundary. The boundary set is
// { phase + k * grid : k in Z } measured on the transport's monotonic clock.
// A non-positive grid means "immediate": launch at the current position with no
// quantization. `phase` lets a slot align to an offset other than the monotonic
// origin (for example, a launch grid anchored to a section start).
struct LaunchQuantize {
    timebase::TickDuration grid{0};
    timebase::TickPosition phase{0};

    constexpr bool immediate() const noexcept {
        return grid.value <= 0;
    }
    constexpr auto operator<=>(const LaunchQuantize&) const = default;
};

// Launch immediately, without quantization.
constexpr LaunchQuantize launch_immediate() noexcept {
    return {timebase::TickDuration{0}, timebase::TickPosition{0}};
}

// Quantize to a whole number of quarter notes.
constexpr LaunchQuantize launch_every_quarters(std::int64_t count) noexcept {
    return {timebase::TickDuration{count * timebase::kTicksPerQuarter}, timebase::TickPosition{0}};
}

// Quantize to a whole number of bars under the supplied meter. A bar spans
// numerator * (4 / denominator) quarter notes; the arithmetic stays in exact
// canonical ticks (kTicksPerQuarter is divisible by every supported denominator).
constexpr LaunchQuantize launch_every_bars(std::int64_t count,
                                           timebase::MeterSignature meter) noexcept {
    const std::int64_t ticks_per_bar = timebase::kTicksPerQuarter * meter.numerator * 4 /
                                       meter.denominator;
    return {timebase::TickDuration{count * ticks_per_bar}, timebase::TickPosition{0}};
}

// A single launchable clip in a track lane. `clip_id` references a clip stored in
// the linear model (or a launch-owned clip pool in a later slice); a null id is
// an empty slot. Foundation slices carry identity and quantization only.
struct Slot {
    ItemId id;
    ItemId clip_id;
    LaunchQuantize launch_quantize{};

    constexpr bool empty() const noexcept {
        return !clip_id.valid();
    }
};

// A column of slots launched as a unit. The linear model reserves "scene" for no
// other concept, so the name is safe here. Track-to-slot arbitration when a whole
// scene is launched is a later slice; a Scene is just the grouping at this stage.
struct Scene {
    ItemId id;
    std::string name;
    std::vector<Slot> slots;
};

} // namespace pulp::timeline
