#pragma once

#include <pulp/runtime/result.hpp>
#include <pulp/timebase/tick.hpp>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace pulp::timebase {

struct MeterSignature {
    std::int32_t numerator = 4;
    std::int32_t denominator = 4;
    constexpr auto operator<=>(const MeterSignature&) const = default;
};

struct MeterPoint {
    TickPosition tick{};
    MeterSignature signature{};
    constexpr auto operator<=>(const MeterPoint&) const = default;
};

struct BarPosition {
    std::int64_t value = 0;
    constexpr auto operator<=>(const BarPosition&) const = default;
};

struct BarTickPosition {
    BarPosition bar{};
    TickDuration tick_in_bar{};
    constexpr auto operator<=>(const BarTickPosition&) const = default;
};

enum class MeterMapError {
    Empty,
    MissingTickZero,
    InvalidSignature,
    UnorderedPoints,
    ChangeNotOnBarBoundary,
    RangeExceeded,
};

class MeterMap {
  public:
    MeterMap() : points_{{{0}, {4, 4}}} {}

    static runtime::Result<MeterMap, MeterMapError>
    create(std::span<const MeterPoint> points) noexcept;
    runtime::Result<MeterMap, MeterMapError>
    replacing_points(std::span<const MeterPoint> points) const noexcept {
        return create(points);
    }
    std::span<const MeterPoint> points() const noexcept { return points_; }
    auto operator<=>(const MeterMap&) const = default;

  private:
    explicit MeterMap(std::vector<MeterPoint> points) : points_(std::move(points)) {}
    std::vector<MeterPoint> points_;
};

class CompiledMeterMap {
  public:
    static runtime::Result<CompiledMeterMap, MeterMapError>
    compile(const MeterMap& map) noexcept;
    static runtime::Result<CompiledMeterMap, MeterMapError>
    compile(std::span<const MeterPoint> points) noexcept;

    // Total over the signed 64-bit domain. Representable conversions are exact;
    // results outside TickPosition/BarPosition clamp to the nearest endpoint.
    BarTickPosition tick_to_bar(TickPosition tick) const noexcept;
    TickPosition bar_to_tick(BarPosition bar, TickDuration tick_in_bar = {}) const noexcept;
    MeterSignature meter_at_tick(TickPosition tick) const noexcept;
    std::size_t segment_count() const noexcept { return segments_.size(); }

  private:
    struct Segment {
        TickPosition start_tick{};
        BarPosition start_bar{};
        std::int64_t ticks_per_bar = 0;
        MeterSignature signature{};
    };

    explicit CompiledMeterMap(std::vector<Segment> segments) : segments_(std::move(segments)) {}
    std::vector<Segment> segments_;
};

} // namespace pulp::timebase
