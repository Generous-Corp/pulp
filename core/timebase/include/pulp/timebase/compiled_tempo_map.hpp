#pragma once

#include <pulp/runtime/result.hpp>
#include <pulp/timebase/rational_time.hpp>
#include <pulp/timebase/tick.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace pulp::timebase {

inline constexpr std::uint32_t kMaximumCompiledSampleRate = 768'000;

enum class TempoCurve {
    Constant,
    LinearInTicks,
};

struct TempoPoint {
    TickPosition tick;
    double bpm = 120.0;
    TempoCurve curve_to_next = TempoCurve::Constant;
    constexpr auto operator<=>(const TempoPoint&) const = default;
};

enum class TempoMapError {
    Empty,
    MissingTickZero,
    InvalidBpm,
    InvalidCurve,
    InvalidFinalCurve,
    UnorderedPoints,
    InvalidSampleRate,
    SampleRangeExceeded,
};

// Editable, sample-rate-independent document value. Validation is shared with
// compilation so malformed maps never reach the playback graph.
class TempoMap {
  public:
    TempoMap() : points_{{{0}, 120.0, TempoCurve::Constant}} {}

    static runtime::Result<TempoMap, TempoMapError>
    create(std::span<const TempoPoint> points) noexcept;

    runtime::Result<TempoMap, TempoMapError>
    replacing_points(std::span<const TempoPoint> points) const noexcept {
        return create(points);
    }

    std::span<const TempoPoint> points() const noexcept {
        return points_;
    }
    constexpr auto operator<=>(const TempoMap&) const = default;

  private:
    explicit TempoMap(std::vector<TempoPoint> points) : points_(std::move(points)) {}
    std::vector<TempoPoint> points_;
};

struct SampleToTickResult {
    TickPosition tick;
    SamplePosition represented_sample;
    std::uint64_t absolute_error_samples = 0;
    bool exact = true;
};

// Immutable, sample-rate-specific tempo lookup. Tempo ramps are linear in
// musical tick position; their tick-to-sample integral and inverse are
// analytic. Integer sample anchors at every tempo point prevent cumulative
// floating-point drift across segments. When the integer tick grid is at least
// as dense as the sample grid, every sample within TickPosition's representable
// range has an exact canonical tick. A sparser grid cannot represent every
// sample; resolve_sample() then returns the nearest tick and reports its
// explicit sample error. Both position types use their full signed 64-bit
// range. ticks_to_samples() saturates if its mathematical result exceeds the
// sample domain. Resolving a sample outside the image of TickPosition returns
// a nearest canonical edge representation with exact=false and the actual
// sample error.
class CompiledTempoMap {
  public:
    static runtime::Result<CompiledTempoMap, TempoMapError>
    compile(std::span<const TempoPoint> points, RationalRate sample_rate) noexcept;
    static runtime::Result<CompiledTempoMap, TempoMapError>
    compile(const TempoMap& map, RationalRate sample_rate) noexcept {
        return compile(map.points(), sample_rate);
    }

    TickPosition samples_to_ticks(SamplePosition sample) const noexcept;
    SampleToTickResult resolve_sample(SamplePosition sample) const noexcept;
    SamplePosition ticks_to_samples(TickPosition tick) const noexcept;
    double tempo_at_tick(TickPosition tick) const noexcept;

    RationalRate sample_rate() const noexcept {
        return sample_rate_;
    }
    std::size_t segment_count() const noexcept {
        return segments_.size();
    }

  private:
    friend class TempoCursor;
    struct Segment {
        TickPosition start_tick;
        TickPosition end_tick;
        SamplePosition start_sample;
        double start_bpm = 120.0;
        double end_bpm = 120.0;
        TempoCurve curve = TempoCurve::Constant;
    };

    long double samples_from_segment_start(const Segment& segment,
                                           std::int64_t delta_ticks) const noexcept;
    long double ticks_from_segment_start(const Segment& segment,
                                         long double delta_samples) const noexcept;
    std::size_t segment_for_tick(TickPosition tick) const noexcept;
    std::size_t segment_for_sample(SamplePosition sample) const noexcept;
    SamplePosition ticks_to_samples_in_segment(TickPosition tick,
                                               std::size_t segment) const noexcept;
    SampleToTickResult resolve_sample_in_segment(SamplePosition sample,
                                                 std::size_t segment) const noexcept;

    CompiledTempoMap(RationalRate sample_rate, std::vector<Segment> segments) noexcept
        : sample_rate_(sample_rate), segments_(std::move(segments)) {}

    RationalRate sample_rate_;
    std::vector<Segment> segments_;
};

// Allocation-free streaming resolver. Forward segment changes are consumed
// once, giving amortized O(1) segment selection for monotonic playback. A
// backward sample is an explicit discontinuity and performs one cold seek.
class TempoCursor {
  public:
    TempoCursor() = default;
    explicit TempoCursor(const CompiledTempoMap& map) noexcept {
        reset(map);
    }

    void reset(const CompiledTempoMap& map) noexcept;
    SampleToTickResult seek(SamplePosition sample) noexcept;
    SampleToTickResult advance(SamplePosition sample) noexcept;
    double tempo_at_tick(TickPosition tick) noexcept;

    std::size_t segment_index() const noexcept {
        return segment_index_;
    }

  private:
    const CompiledTempoMap* map_ = nullptr;
    std::size_t segment_index_ = 0;
    SamplePosition sample_{};
    bool positioned_ = false;
};

} // namespace pulp::timebase
