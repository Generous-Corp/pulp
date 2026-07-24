#include <pulp/timebase/compiled_tempo_map.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace pulp::timebase {
namespace {

constexpr double kMinimumBpm = 1.0;
constexpr double kMaximumBpm = 1'000.0;
constexpr long double kInt64MaximumRoundingBoundary = 9'223'372'036'854'775'807.5L;

std::int64_t rounded_and_clamped(long double value) noexcept {
    constexpr auto minimum = static_cast<long double>(std::numeric_limits<std::int64_t>::min());
    constexpr auto maximum = static_cast<long double>(std::numeric_limits<std::int64_t>::max());
    if (value <= minimum)
        return std::numeric_limits<std::int64_t>::min();
    if (value >= maximum)
        return std::numeric_limits<std::int64_t>::max();
    return static_cast<std::int64_t>(std::llround(value));
}

std::int64_t saturating_add(std::int64_t lhs, std::int64_t rhs) noexcept {
    if (rhs > 0 && lhs > std::numeric_limits<std::int64_t>::max() - rhs) {
        return std::numeric_limits<std::int64_t>::max();
    }
    if (rhs < 0 && lhs < std::numeric_limits<std::int64_t>::min() - rhs) {
        return std::numeric_limits<std::int64_t>::min();
    }
    return lhs + rhs;
}

std::uint64_t integer_distance(std::int64_t lhs, std::int64_t rhs) noexcept {
    if (lhs >= rhs) {
        return static_cast<std::uint64_t>(lhs) - static_cast<std::uint64_t>(rhs);
    }
    return static_cast<std::uint64_t>(rhs) - static_cast<std::uint64_t>(lhs);
}

bool valid_bpm(double bpm) noexcept {
    return std::isfinite(bpm) && bpm >= kMinimumBpm && bpm <= kMaximumBpm;
}

} // namespace

runtime::Result<TempoMap, TempoMapError>
TempoMap::create(std::span<const TempoPoint> points) noexcept {
    if (points.empty())
        return runtime::Err(TempoMapError::Empty);
    if (points.front().tick.value != 0)
        return runtime::Err(TempoMapError::MissingTickZero);
    for (std::size_t index = 0; index < points.size(); ++index) {
        if (!valid_bpm(points[index].bpm))
            return runtime::Err(TempoMapError::InvalidBpm);
        if (points[index].curve_to_next != TempoCurve::Constant &&
            points[index].curve_to_next != TempoCurve::LinearInTicks)
            return runtime::Err(TempoMapError::InvalidCurve);
        if (index > 0 && points[index].tick.value <= points[index - 1].tick.value)
            return runtime::Err(TempoMapError::UnorderedPoints);
    }
    if (points.back().curve_to_next != TempoCurve::Constant)
        return runtime::Err(TempoMapError::InvalidFinalCurve);
    return runtime::Ok(TempoMap(std::vector<TempoPoint>(points.begin(), points.end())));
}

runtime::Result<CompiledTempoMap, TempoMapError>
CompiledTempoMap::compile(std::span<const TempoPoint> points, RationalRate sample_rate) noexcept {
    auto editable = TempoMap::create(points);
    if (!editable)
        return runtime::Err(editable.error());
    const auto normalized_rate = sample_rate.normalized();
    if (!normalized_rate.valid() ||
        normalized_rate.as_long_double() > static_cast<long double>(kMaximumCompiledSampleRate))
        return runtime::Err(TempoMapError::InvalidSampleRate);

    std::vector<Segment> segments;
    segments.reserve(points.size());
    CompiledTempoMap calculator(normalized_rate, {});
    SamplePosition anchor{};
    for (std::size_t index = 0; index < points.size(); ++index) {
        const auto& point = points[index];
        const auto has_next = index + 1 < points.size();
        const auto end_tick = has_next ? points[index + 1].tick
                                       : TickPosition{std::numeric_limits<std::int64_t>::max()};
        const auto end_bpm = has_next && point.curve_to_next == TempoCurve::LinearInTicks
                                 ? points[index + 1].bpm
                                 : point.bpm;
        segments.push_back({point.tick, end_tick, anchor, point.bpm, end_bpm,
                            has_next ? point.curve_to_next : TempoCurve::Constant});
        if (has_next) {
            const auto delta = end_tick.value - point.tick.value;
            const auto duration = calculator.samples_from_segment_start(segments.back(), delta);
            if (!std::isfinite(duration) || duration < 0.0L ||
                duration >= kInt64MaximumRoundingBoundary)
                return runtime::Err(TempoMapError::SampleRangeExceeded);
            const auto rounded_duration = static_cast<std::int64_t>(std::llround(duration));
            if (anchor.value > std::numeric_limits<std::int64_t>::max() - rounded_duration)
                return runtime::Err(TempoMapError::SampleRangeExceeded);
            anchor.value += rounded_duration;
        }
    }
    return runtime::Ok(CompiledTempoMap(normalized_rate, std::move(segments)));
}

long double CompiledTempoMap::samples_from_segment_start(const Segment& segment,
                                                         std::int64_t delta_ticks) const noexcept {
    const auto ticks = static_cast<long double>(delta_ticks);
    const auto scale =
        sample_rate_.as_long_double() * 60.0L / static_cast<long double>(kTicksPerQuarter);
    if (delta_ticks < 0 || segment.curve == TempoCurve::Constant ||
        segment.end_tick.value == segment.start_tick.value ||
        segment.start_bpm == segment.end_bpm) {
        return ticks * scale / static_cast<long double>(segment.start_bpm);
    }

    const auto length = static_cast<long double>(segment.end_tick.value - segment.start_tick.value);
    const auto slope =
        (static_cast<long double>(segment.end_bpm) - static_cast<long double>(segment.start_bpm)) /
        length;
    const auto relative_tempo_change = slope * ticks / static_cast<long double>(segment.start_bpm);
    return scale * std::log1p(relative_tempo_change) / slope;
}

long double CompiledTempoMap::ticks_from_segment_start(const Segment& segment,
                                                       long double delta_samples) const noexcept {
    const auto samples = delta_samples;
    const auto inverse_scale =
        static_cast<long double>(kTicksPerQuarter) / (sample_rate_.as_long_double() * 60.0L);
    if (delta_samples < 0.0L || segment.curve == TempoCurve::Constant ||
        segment.end_tick.value == segment.start_tick.value ||
        segment.start_bpm == segment.end_bpm) {
        return samples * inverse_scale * static_cast<long double>(segment.start_bpm);
    }

    const auto length = static_cast<long double>(segment.end_tick.value - segment.start_tick.value);
    const auto slope =
        (static_cast<long double>(segment.end_bpm) - static_cast<long double>(segment.start_bpm)) /
        length;
    return static_cast<long double>(segment.start_bpm) *
           std::expm1(samples * inverse_scale * slope) / slope;
}

std::size_t CompiledTempoMap::segment_for_tick(TickPosition tick) const noexcept {
    const auto it = std::upper_bound(
        segments_.begin(), segments_.end(), tick,
        [](TickPosition value, const Segment& segment) { return value < segment.start_tick; });
    return it == segments_.begin() ? 0 : static_cast<std::size_t>(it - segments_.begin() - 1);
}

std::size_t CompiledTempoMap::segment_for_sample(SamplePosition sample) const noexcept {
    const auto it = std::upper_bound(
        segments_.begin(), segments_.end(), sample,
        [](SamplePosition value, const Segment& segment) { return value < segment.start_sample; });
    return it == segments_.begin() ? 0 : static_cast<std::size_t>(it - segments_.begin() - 1);
}

SamplePosition CompiledTempoMap::ticks_to_samples(TickPosition tick) const noexcept {
    return ticks_to_samples_in_segment(tick, segment_for_tick(tick));
}

SamplePosition
CompiledTempoMap::ticks_to_samples_in_segment(TickPosition tick,
                                              std::size_t segment_index) const noexcept {
    while (segment_index + 1 < segments_.size() && tick >= segments_[segment_index + 1].start_tick)
        ++segment_index;
    while (segment_index > 0 && tick < segments_[segment_index].start_tick)
        --segment_index;
    const auto& segment = segments_[segment_index];
    const auto delta = tick.value - segment.start_tick.value;
    const auto offset = rounded_and_clamped(samples_from_segment_start(segment, delta));
    return {saturating_add(segment.start_sample.value, offset)};
}

double CompiledTempoMap::tempo_at_tick(TickPosition tick) const noexcept {
    const auto& segment = segments_[segment_for_tick(tick)];
    if (tick < segment.start_tick || segment.curve == TempoCurve::Constant ||
        segment.start_bpm == segment.end_bpm || segment.end_tick == segment.start_tick) {
        return segment.start_bpm;
    }

    const auto offset =
        static_cast<long double>(tick.value) - static_cast<long double>(segment.start_tick.value);
    const auto length = static_cast<long double>(segment.end_tick.value) -
                        static_cast<long double>(segment.start_tick.value);
    const auto fraction = std::clamp(offset / length, 0.0L, 1.0L);
    return static_cast<double>(static_cast<long double>(segment.start_bpm) +
                               fraction * (static_cast<long double>(segment.end_bpm) -
                                           static_cast<long double>(segment.start_bpm)));
}

TickPosition CompiledTempoMap::samples_to_ticks(SamplePosition sample) const noexcept {
    return resolve_sample(sample).tick;
}

SampleToTickResult CompiledTempoMap::resolve_sample(SamplePosition sample) const noexcept {
    const auto segment_index = segment_for_sample(sample);
    return resolve_sample_in_segment(sample, segment_index);
}

SampleToTickResult
CompiledTempoMap::resolve_sample_in_segment(SamplePosition sample,
                                            std::size_t segment_index) const noexcept {
    const auto& segment = segments_[segment_index];
    const auto delta_samples = static_cast<long double>(sample.value) -
                               static_cast<long double>(segment.start_sample.value);
    const auto estimate_offset =
        rounded_and_clamped(ticks_from_segment_start(segment, delta_samples));
    TickPosition estimate{saturating_add(segment.start_tick.value, estimate_offset)};

    // The analytic inverse lands within a few ticks. Correct it with a monotonic
    // lower-bound search so the returned tick is canonical: the first tick that
    // maps to this integer sample. This makes sample -> tick -> sample exact.
    auto low = estimate.value;
    auto high = estimate.value;
    std::int64_t step = 1;
    while (ticks_to_samples_in_segment({low}, segment_index).value >= sample.value) {
        high = low;
        if (low == std::numeric_limits<std::int64_t>::min())
            break;
        const auto distance_to_min =
            integer_distance(low, std::numeric_limits<std::int64_t>::min());
        const auto decrement = static_cast<std::int64_t>(
            std::min<std::uint64_t>(static_cast<std::uint64_t>(step), distance_to_min));
        low -= decrement;
        step = std::min<std::int64_t>(step * 2, std::numeric_limits<std::int64_t>::max() / 2);
    }
    step = 1;
    while (ticks_to_samples_in_segment({high}, segment_index).value < sample.value) {
        low = high;
        if (high == std::numeric_limits<std::int64_t>::max())
            break;
        const auto distance_to_max =
            integer_distance(std::numeric_limits<std::int64_t>::max(), high);
        const auto increment = static_cast<std::int64_t>(
            std::min<std::uint64_t>(static_cast<std::uint64_t>(step), distance_to_max));
        high += increment;
        step = std::min<std::int64_t>(step * 2, std::numeric_limits<std::int64_t>::max() / 2);
    }
    while (integer_distance(low, high) > 1) {
        const auto midpoint = std::midpoint(low, high);
        if (ticks_to_samples_in_segment({midpoint}, segment_index).value < sample.value) {
            low = midpoint;
        } else {
            high = midpoint;
        }
    }
    const TickPosition upper_tick{
        ticks_to_samples_in_segment({low}, segment_index).value >= sample.value ? low : high};
    const auto upper_sample = ticks_to_samples_in_segment(upper_tick, segment_index);
    if (upper_sample == sample) {
        return {upper_tick, upper_sample, 0, true};
    }

    const TickPosition lower_tick{upper_tick.value == std::numeric_limits<std::int64_t>::min()
                                      ? upper_tick.value
                                      : upper_tick.value - 1};
    const auto lower_sample = ticks_to_samples_in_segment(lower_tick, segment_index);
    const auto lower_error = integer_distance(sample.value, lower_sample.value);
    const auto upper_error = integer_distance(upper_sample.value, sample.value);
    auto nearest_tick = lower_error <= upper_error ? lower_tick : upper_tick;
    const auto nearest_sample = lower_error <= upper_error ? lower_sample : upper_sample;
    const auto nearest_error = std::min(lower_error, upper_error);

    // A clamped edge can contain many ticks with the same represented sample.
    // Canonicalize only when the selected tick is not already the first one.
    if (nearest_tick.value != std::numeric_limits<std::int64_t>::min() &&
        ticks_to_samples({nearest_tick.value - 1}) == nearest_sample) {
        nearest_tick = resolve_sample(nearest_sample).tick;
    }
    return {nearest_tick, nearest_sample, nearest_error, false};
}

void TempoCursor::reset(const CompiledTempoMap& map) noexcept {
    map_ = &map;
    segment_index_ = 0;
    sample_ = {};
    positioned_ = false;
}

SampleToTickResult TempoCursor::seek(SamplePosition sample) noexcept {
    if (map_ == nullptr)
        return {{}, {}, integer_distance(sample.value, 0), sample.value == 0};
    segment_index_ = map_->segment_for_sample(sample);
    sample_ = sample;
    positioned_ = true;
    return map_->resolve_sample_in_segment(sample, segment_index_);
}

SampleToTickResult TempoCursor::advance(SamplePosition sample) noexcept {
    if (map_ == nullptr)
        return {{}, {}, integer_distance(sample.value, 0), sample.value == 0};
    if (!positioned_ || sample < sample_)
        return seek(sample);
    while (segment_index_ + 1 < map_->segments_.size() &&
           sample >= map_->segments_[segment_index_ + 1].start_sample)
        ++segment_index_;
    sample_ = sample;
    return map_->resolve_sample_in_segment(sample, segment_index_);
}

double TempoCursor::tempo_at_tick(TickPosition tick) noexcept {
    if (map_ == nullptr)
        return 120.0;
    while (segment_index_ + 1 < map_->segments_.size() &&
           tick >= map_->segments_[segment_index_ + 1].start_tick)
        ++segment_index_;
    while (segment_index_ > 0 && tick < map_->segments_[segment_index_].start_tick)
        --segment_index_;
    const auto& segment = map_->segments_[segment_index_];
    if (tick < segment.start_tick || segment.curve == TempoCurve::Constant ||
        segment.start_bpm == segment.end_bpm || segment.end_tick == segment.start_tick)
        return segment.start_bpm;
    const auto offset = static_cast<long double>(tick.value - segment.start_tick.value);
    const auto length = static_cast<long double>(segment.end_tick.value - segment.start_tick.value);
    const auto fraction = std::clamp(offset / length, 0.0L, 1.0L);
    return static_cast<double>(static_cast<long double>(segment.start_bpm) +
                               fraction * (static_cast<long double>(segment.end_bpm) -
                                           static_cast<long double>(segment.start_bpm)));
}

} // namespace pulp::timebase
