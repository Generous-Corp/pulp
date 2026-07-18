#pragma once

#include <pulp/timebase/rational_time.hpp>
#include <pulp/timebase/tick.hpp>

#include <cstddef>
#include <span>
#include <vector>

namespace pulp::timebase {

enum class TempoCurve {
    Constant,
    LinearInTicks,
};

struct TempoPoint {
    TickPosition tick;
    double bpm = 120.0;
    TempoCurve curve_to_next = TempoCurve::Constant;
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
    explicit CompiledTempoMap(std::span<const TempoPoint> points, RationalRate sample_rate);

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

    RationalRate sample_rate_;
    std::vector<Segment> segments_;
};

} // namespace pulp::timebase
