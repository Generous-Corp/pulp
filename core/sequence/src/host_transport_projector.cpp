#include <pulp/sequence/host_transport_projector.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace pulp::sequence {
namespace {

bool valid_meter(playback::MeterSignature meter) noexcept {
    if (meter.numerator <= 0 || meter.denominator <= 0)
        return false;
    const auto denominator = static_cast<std::uint32_t>(meter.denominator);
    return (denominator & (denominator - 1)) == 0;
}

bool same_sample_rate(double sample_rate, timebase::RationalRate expected) noexcept {
    if (!std::isfinite(sample_rate) || sample_rate <= 0.0 || !expected.valid())
        return false;
    const auto difference =
        std::abs(static_cast<long double>(sample_rate) - expected.as_long_double());
    return difference <= 1.0e-9L;
}

bool beats_to_ticks(double beats, timebase::TickPosition& result) noexcept {
    if (!std::isfinite(beats))
        return false;
    const auto scaled =
        static_cast<long double>(beats) * static_cast<long double>(timebase::kTicksPerQuarter);
    if (scaled < static_cast<long double>(std::numeric_limits<std::int64_t>::min()) ||
        scaled > static_cast<long double>(std::numeric_limits<std::int64_t>::max()))
        return false;
    result.value = static_cast<std::int64_t>(std::llround(scaled));
    return true;
}

bool valid_host_beat_clock(const format::ProcessContext& context) noexcept {
    return context.has_transport(format::TransportField::BeatPosition) &&
           context.has_transport(format::TransportField::Tempo) &&
           context.has_transport(format::TransportField::SamplePosition) &&
           std::isfinite(context.position_beats) && std::isfinite(context.tempo_bpm) &&
           context.tempo_bpm > 0.0;
}

timebase::TickPosition tick_at_host_frame(double start_beat, std::uint32_t frames,
                                          double sample_rate, double tempo_bpm) noexcept {
    const auto beat = static_cast<long double>(start_beat) +
                      static_cast<long double>(frames) * static_cast<long double>(tempo_bpm) /
                          (60.0L * static_cast<long double>(sample_rate));
    const auto tick = beat * static_cast<long double>(timebase::kTicksPerQuarter);
    if (tick >= static_cast<long double>(std::numeric_limits<std::int64_t>::max()))
        return {std::numeric_limits<std::int64_t>::max()};
    if (tick <= static_cast<long double>(std::numeric_limits<std::int64_t>::min()))
        return {std::numeric_limits<std::int64_t>::min()};
    return {static_cast<std::int64_t>(std::llround(tick))};
}

bool sample_at_host_beat(timebase::SamplePosition anchor_sample, double anchor_beat,
                         double target_beat, double sample_rate, double tempo_bpm,
                         timebase::SamplePosition& result) noexcept {
    const auto delta = static_cast<long double>(target_beat - anchor_beat) *
                       static_cast<long double>(sample_rate) * 60.0L /
                       static_cast<long double>(tempo_bpm);
    const auto sample = static_cast<long double>(anchor_sample.value) + delta;
    if (!std::isfinite(sample) ||
        sample < static_cast<long double>(std::numeric_limits<std::int64_t>::min()) ||
        sample > static_cast<long double>(std::numeric_limits<std::int64_t>::max()))
        return false;
    result.value = static_cast<std::int64_t>(std::llround(sample));
    return true;
}

std::uint64_t frames_between_host_beats(double start, double end, double sample_rate,
                                        double tempo_bpm) noexcept {
    if (!(end > start))
        return 0;
    const auto frames = static_cast<long double>(end - start) *
                        static_cast<long double>(sample_rate) * 60.0L /
                        static_cast<long double>(tempo_bpm);
    if (!std::isfinite(frames) || frames <= 0.0L)
        return 0;
    if (frames >= static_cast<long double>(std::numeric_limits<std::uint64_t>::max()))
        return std::numeric_limits<std::uint64_t>::max();
    return static_cast<std::uint64_t>(std::ceil(frames));
}

timebase::SamplePosition add_frames(timebase::SamplePosition position,
                                    std::uint32_t frames) noexcept {
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    const auto signed_frames = static_cast<std::int64_t>(frames);
    if (position.value > maximum - signed_frames)
        return {maximum};
    return {position.value + signed_frames};
}

std::uint64_t sample_distance(timebase::SamplePosition start,
                              timebase::SamplePosition end) noexcept {
    if (end.value <= start.value)
        return 0;
    return static_cast<std::uint64_t>(end.value) - static_cast<std::uint64_t>(start.value);
}

timebase::BarPosition bar_at_tick(timebase::TickPosition tick,
                                  playback::MeterSignature meter) noexcept {
    const auto ticks_per_bar = static_cast<long double>(timebase::kTicksPerQuarter) *
                               static_cast<long double>(meter.numerator) * 4.0L /
                               static_cast<long double>(meter.denominator);
    const auto value = std::floor(static_cast<long double>(tick.value) / ticks_per_bar);
    if (value >= static_cast<long double>(std::numeric_limits<std::int64_t>::max()))
        return {std::numeric_limits<std::int64_t>::max()};
    if (value <= static_cast<long double>(std::numeric_limits<std::int64_t>::min()))
        return {std::numeric_limits<std::int64_t>::min()};
    return {static_cast<std::int64_t>(value)};
}

} // namespace

HostTransportProjectionError
HostTransportProjector::prepare(const timebase::CompiledTempoMap& tempo_map,
                                std::uint32_t maximum_block_size) noexcept {
    reset();
    if (maximum_block_size == 0 ||
        maximum_block_size > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())) {
        return HostTransportProjectionError::InvalidFrameCount;
    }
    tempo_map_ = &tempo_map;
    maximum_block_size_ = maximum_block_size;
    return HostTransportProjectionError::None;
}

HostTransportProjectionError
HostTransportProjector::project(const format::ProcessContext& context,
                                playback::TransportSnapshot& snapshot) noexcept {
    if (tempo_map_ == nullptr)
        return HostTransportProjectionError::NotPrepared;
    if (context.num_samples <= 0 ||
        static_cast<std::uint32_t>(context.num_samples) > maximum_block_size_) {
        return HostTransportProjectionError::InvalidFrameCount;
    }
    if (!same_sample_rate(context.sample_rate, tempo_map_->sample_rate()))
        return HostTransportProjectionError::InvalidSampleRate;

    const playback::MeterSignature meter{context.time_sig_numerator, context.time_sig_denominator};
    if (!valid_meter(meter))
        return HostTransportProjectionError::InvalidMeter;

    playback::LoopRegion loop;
    loop.enabled = context.is_looping;
    if (loop.enabled) {
        if (!beats_to_ticks(context.loop_start_beats, loop.start) ||
            !beats_to_ticks(context.loop_end_beats, loop.end) || !(loop.start < loop.end)) {
            return HostTransportProjectionError::InvalidLoop;
        }
    }

    const auto frames = static_cast<std::uint32_t>(context.num_samples);
    const timebase::SamplePosition host_start{context.position_samples};
    const bool use_host_beat_clock = valid_host_beat_clock(context);
    timebase::TickPosition host_tick_start;
    if (use_host_beat_clock && !beats_to_ticks(context.position_beats, host_tick_start)) {
        return HostTransportProjectionError::BeatPositionOutOfRange;
    }
    if (use_host_beat_clock && context.is_playing) {
        const auto host_beat_end =
            context.position_beats + static_cast<double>(frames) * context.tempo_bpm /
                                         (60.0 * context.sample_rate);
        if (!(host_beat_end > context.position_beats))
            return HostTransportProjectionError::BeatPositionOutOfRange;
    }
    const bool mapping_transition =
        !first_block_ && use_host_beat_clock != previous_host_beat_mapping_;
    const bool inferred_jump =
        has_expected_sample_ && context.is_playing && host_start != expected_next_sample_;
    const bool discontinuity = context.reset_requested || context.transport_jump || inferred_jump ||
                               mapping_transition || pending_discontinuity_;
    bool next_pending_discontinuity = false;

    snapshot = {};
    snapshot.tempo_map = tempo_map_;
    snapshot.sample_rate = tempo_map_->sample_rate();
    snapshot.block_index = block_index_++;
    snapshot.frame_count = frames;
    snapshot.meter = meter;
    snapshot.loop = loop;
    if (loop.enabled && use_host_beat_clock) {
        snapshot.host_loop_start_beats = context.loop_start_beats;
        snapshot.host_loop_end_beats = context.loop_end_beats;
        snapshot.has_precise_host_loop = true;
    }
    snapshot.is_playing = context.is_playing;
    snapshot.transport_changed = context.transport_changed;
    snapshot.transport_started =
        context.transport_started || (context.is_playing && (first_block_ || !previous_playing_));
    snapshot.reset_requested = context.reset_requested || inferred_jump || mapping_transition;
    snapshot.time_sig_changed =
        context.time_sig_changed || (!first_block_ && meter != previous_meter_);

    auto make_range = [&](std::uint8_t index, std::uint32_t offset, std::uint32_t count,
                          timebase::SamplePosition timeline_start, bool range_discontinuity,
                          const timebase::TickPosition* forced_start = nullptr,
                          const timebase::TickPosition* forced_end = nullptr,
                          double host_beat_start = std::numeric_limits<double>::quiet_NaN(),
                          double forced_end_beat = std::numeric_limits<double>::quiet_NaN()) {
        auto& range = snapshot.ranges[index];
        range.sample_offset = offset;
        range.frame_count = count;
        range.timeline_sample_start = timeline_start;
        range.timeline_tick_start =
            forced_start != nullptr ? *forced_start : tempo_map_->samples_to_ticks(timeline_start);
        range.timeline_tick_end =
            context.is_playing
                ? (forced_end != nullptr ? *forced_end
                   : use_host_beat_clock
                       ? tick_at_host_frame(host_beat_start, count, context.sample_rate,
                                            context.tempo_bpm)
                       : tempo_map_->samples_to_ticks(add_frames(timeline_start, count)))
                : range.timeline_tick_start;
        if (use_host_beat_clock) {
            range.has_precise_host_ticks = true;
            range.host_tick_start =
                host_beat_start * static_cast<double>(timebase::kTicksPerQuarter);
            const auto end_beat = std::isfinite(forced_end_beat)
                                      ? forced_end_beat
                                      : host_beat_start + static_cast<double>(count) *
                                                              context.tempo_bpm /
                                                              (60.0 * context.sample_rate);
            range.host_tick_end = context.is_playing
                                      ? end_beat * static_cast<double>(timebase::kTicksPerQuarter)
                                      : range.host_tick_start;
        }
        if (range.timeline_tick_end < range.timeline_tick_start)
            range.timeline_tick_end = range.timeline_tick_start;
        range.monotonic_start = monotonic_;
        range.monotonic_end =
            context.is_playing ? monotonic_ + (range.timeline_tick_end - range.timeline_tick_start)
                               : monotonic_;
        range.bar_start = bar_at_tick(range.timeline_tick_start, meter);
        range.tempo_bpm = use_host_beat_clock
                              ? context.tempo_bpm
                              : tempo_map_->tempo_at_tick(range.timeline_tick_start);
        range.tempo_changed = index == 0 ? context.tempo_changed
                                         : range.tempo_bpm != snapshot.ranges[index - 1].tempo_bpm;
        range.discontinuity = range_discontinuity;
        range.host_beat_mapping = use_host_beat_clock;
        monotonic_ = range.monotonic_end;
    };

    if (!context.is_playing || !loop.enabled) {
        make_range(0, 0, frames, host_start, discontinuity,
                   use_host_beat_clock ? &host_tick_start : nullptr, nullptr,
                   context.position_beats);
        snapshot.range_count = 1;
        expected_next_sample_ = context.is_playing ? add_frames(host_start, frames) : host_start;
    } else {
        auto loop_start = tempo_map_->ticks_to_samples(loop.start);
        auto loop_end = tempo_map_->ticks_to_samples(loop.end);
        std::uint64_t loop_length = sample_distance(loop_start, loop_end);
        if (use_host_beat_clock) {
            if (!sample_at_host_beat(host_start, context.position_beats, context.loop_start_beats,
                                     context.sample_rate, context.tempo_bpm, loop_start) ||
                !sample_at_host_beat(host_start, context.position_beats, context.loop_end_beats,
                                     context.sample_rate, context.tempo_bpm, loop_end))
                return HostTransportProjectionError::InvalidLoop;
            loop_length =
                frames_between_host_beats(context.loop_start_beats, context.loop_end_beats,
                                          context.sample_rate, context.tempo_bpm);
        }
        if (loop_length == 0)
            return HostTransportProjectionError::InvalidLoop;
        if ((use_host_beat_clock && !(context.position_beats < context.loop_end_beats)) ||
            (!use_host_beat_clock && host_start.value >= loop_end.value)) {
            make_range(0, 0, frames, host_start, discontinuity,
                       use_host_beat_clock ? &host_tick_start : nullptr, nullptr,
                       context.position_beats);
            snapshot.range_count = 1;
            expected_next_sample_ = add_frames(host_start, frames);
        } else {
            const auto until_wrap =
                use_host_beat_clock
                    ? frames_between_host_beats(context.position_beats, context.loop_end_beats,
                                                context.sample_rate, context.tempo_bpm)
                    : sample_distance(host_start, loop_end);
            const auto first_count =
                static_cast<std::uint32_t>(std::min<std::uint64_t>(frames, until_wrap));
            const auto remaining = frames - first_count;
            if (remaining > loop_length)
                return HostTransportProjectionError::LoopTooShortForBlock;
            if (first_count > 0) {
                const auto* forced_end =
                    static_cast<std::uint64_t>(first_count) == until_wrap ? &loop.end : nullptr;
                make_range(0, 0, first_count, host_start, discontinuity,
                           use_host_beat_clock ? &host_tick_start : nullptr, forced_end,
                           context.position_beats,
                           forced_end != nullptr ? context.loop_end_beats
                                                 : std::numeric_limits<double>::quiet_NaN());
                snapshot.range_count = 1;
            }
            if (remaining > 0) {
                make_range(snapshot.range_count, first_count, remaining, loop_start, true,
                           &loop.start, nullptr, context.loop_start_beats);
                ++snapshot.range_count;
                expected_next_sample_ =
                    remaining == loop_length ? loop_start : add_frames(loop_start, remaining);
                next_pending_discontinuity = remaining == loop_length;
            } else if (first_count > 0 && static_cast<std::uint64_t>(first_count) == until_wrap) {
                expected_next_sample_ = loop_start;
                next_pending_discontinuity = true;
            } else {
                expected_next_sample_ = add_frames(host_start, first_count);
            }
        }
    }

    snapshot.tempo_bpm = snapshot.ranges[0].tempo_bpm;
    has_expected_sample_ = true;
    previous_meter_ = meter;
    previous_loop_ = loop;
    previous_playing_ = context.is_playing;
    previous_host_beat_mapping_ = use_host_beat_clock;
    pending_discontinuity_ = next_pending_discontinuity;
    first_block_ = false;
    return HostTransportProjectionError::None;
}

void HostTransportProjector::reset() noexcept {
    tempo_map_ = nullptr;
    maximum_block_size_ = 0;
    monotonic_ = {};
    expected_next_sample_ = {};
    previous_meter_ = {};
    previous_loop_ = {};
    block_index_ = 0;
    has_expected_sample_ = false;
    first_block_ = true;
    previous_playing_ = false;
    previous_host_beat_mapping_ = false;
    pending_discontinuity_ = false;
}

} // namespace pulp::sequence
