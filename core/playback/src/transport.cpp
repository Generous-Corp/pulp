#include <pulp/playback/transport.hpp>
#include <pulp/runtime/scoped_no_alloc.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace pulp::playback {
namespace {

bool valid_meter(MeterSignature meter) noexcept {
    if (meter.numerator <= 0 || meter.denominator <= 0)
        return false;
    const auto denominator = static_cast<std::uint32_t>(meter.denominator);
    return (denominator & (denominator - 1)) == 0;
}

std::int64_t saturating_add(std::int64_t value, std::uint32_t increment) noexcept {
    const auto maximum = std::numeric_limits<std::int64_t>::max();
    if (value > maximum - static_cast<std::int64_t>(increment))
        return maximum;
    return value + static_cast<std::int64_t>(increment);
}

std::uint64_t distance(timebase::SamplePosition start,
                       timebase::SamplePosition end) noexcept {
    if (end.value <= start.value)
        return 0;
    return static_cast<std::uint64_t>(end.value) - static_cast<std::uint64_t>(start.value);
}

timebase::BarPosition bar_at_tick(timebase::TickPosition tick,
                                  timebase::TickPosition anchor_tick,
                                  timebase::BarPosition anchor_bar,
                                  MeterSignature meter) noexcept {
    const long double ticks_per_bar =
        static_cast<long double>(timebase::kTicksPerQuarter) *
        static_cast<long double>(meter.numerator) * 4.0L /
        static_cast<long double>(meter.denominator);
    const long double relative_tick = static_cast<long double>(tick.value) -
                                      static_cast<long double>(anchor_tick.value);
    const long double projected = static_cast<long double>(anchor_bar.value) +
                                  std::floor(relative_tick / ticks_per_bar);
    if (projected >= static_cast<long double>(std::numeric_limits<std::int64_t>::max()))
        return {std::numeric_limits<std::int64_t>::max()};
    if (projected <= static_cast<long double>(std::numeric_limits<std::int64_t>::min()))
        return {std::numeric_limits<std::int64_t>::min()};
    return {static_cast<std::int64_t>(projected)};
}

} // namespace

bool valid_transport_ranges(const TransportSnapshot& transport) noexcept {
    if (transport.tempo_map == nullptr || transport.frame_count == 0 ||
        transport.frame_count >
            static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()) ||
        transport.range_count == 0 || transport.range_count > transport.ranges.size())
        return false;
    std::uint64_t expected_offset = 0;
    for (std::uint8_t index = 0; index < transport.range_count; ++index) {
        const auto& range = transport.ranges[index];
        if (range.frame_count == 0 || range.sample_offset != expected_offset)
            return false;
        expected_offset += range.frame_count;
        if (expected_offset > transport.frame_count || (index != 0 && !range.discontinuity))
            return false;
    }
    return expected_offset == transport.frame_count;
}

TransportError MasterTransport::prepare(const timebase::CompiledTempoMap& tempo_map,
                                        const MasterTransportConfig& config) noexcept {
    reset();
    tempo_map_ = &tempo_map;
    max_buffer_size_ = config.max_buffer_size;
    if (max_buffer_size_ == 0 ||
        max_buffer_size_ > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
        reset();
        return TransportError::InvalidFrameCount;
    }
    if (!valid_meter(config.meter)) {
        reset();
        return TransportError::InvalidMeter;
    }

    control_state_.meter = config.meter;
    control_state_.loop = config.loop;
    control_state_.position = config.initial_position;
    control_state_.playing = config.initially_playing;
    const auto loop_error = validate_loop(config.loop);
    if (loop_error != TransportError::None) {
        reset();
        return loop_error;
    }

    tempo_cursor_.reset(tempo_map);
    timeline_sample_ = tempo_map.ticks_to_samples(config.initial_position);
    tempo_cursor_.seek(timeline_sample_);
    timeline_tick_ = config.initial_position;
    monotonic_ = {config.initial_position};
    meter_anchor_tick_ = {};
    meter_anchor_bar_ = {};
    meter_anchor_signature_ = config.meter;
    previous_playing_ = false;
    previous_meter_ = config.meter;
    previous_loop_ = config.loop;
    previous_tempo_bpm_ = tempo_cursor_.tempo_at_tick(config.initial_position);
    publish_desired();
    return TransportError::None;
}

TransportError MasterTransport::set_playing(bool playing) noexcept {
    if (tempo_map_ == nullptr)
        return TransportError::NotPrepared;
    control_state_.playing = playing;
    publish_desired();
    return TransportError::None;
}

TransportError MasterTransport::seek(timebase::TickPosition position) noexcept {
    if (tempo_map_ == nullptr)
        return TransportError::NotPrepared;
    control_state_.position = position;
    ++control_state_.seek_generation;
    publish_desired();
    return TransportError::None;
}

TransportError MasterTransport::set_loop(LoopRegion loop) noexcept {
    if (tempo_map_ == nullptr)
        return TransportError::NotPrepared;
    const auto error = validate_loop(loop);
    if (error != TransportError::None)
        return error;
    control_state_.loop = loop;
    publish_desired();
    return TransportError::None;
}

TransportError MasterTransport::set_meter(MeterSignature meter) noexcept {
    if (tempo_map_ == nullptr)
        return TransportError::NotPrepared;
    if (!valid_meter(meter))
        return TransportError::InvalidMeter;
    control_state_.meter = meter;
    publish_desired();
    return TransportError::None;
}

TransportError MasterTransport::validate_loop(LoopRegion loop) const noexcept {
    if (!loop.enabled)
        return TransportError::None;
    if (!(loop.start < loop.end))
        return TransportError::InvalidLoop;

    const auto start = tempo_map_->ticks_to_samples(loop.start);
    const auto end = tempo_map_->ticks_to_samples(loop.end);
    const auto length = distance(start, end);
    if (length == 0)
        return TransportError::InvalidLoop;
    if (length < max_buffer_size_)
        return TransportError::LoopTooShortForMaximumBlock;
    return TransportError::None;
}

void MasterTransport::publish_desired() noexcept {
    desired_.write(control_state_);
}

TransportError MasterTransport::begin_block(std::uint32_t frame_count,
                                            TransportSnapshot& snapshot) noexcept {
    runtime::ScopedNoAlloc no_alloc_guard;
    if (tempo_map_ == nullptr)
        return TransportError::NotPrepared;
    if (frame_count == 0 || frame_count > max_buffer_size_)
        return TransportError::InvalidFrameCount;

    const auto desired = desired_.read();
    const bool seeked = desired.seek_generation != applied_seek_generation_;
    if (seeked) {
        timeline_sample_ = tempo_map_->ticks_to_samples(desired.position);
        tempo_cursor_.seek(timeline_sample_);
        timeline_tick_ = desired.position;
        applied_seek_generation_ = desired.seek_generation;
        pending_discontinuity_ = true;
    }

    if (desired.meter != meter_anchor_signature_) {
        if (first_block_) {
            meter_anchor_tick_ = {};
            meter_anchor_bar_ = {};
        } else {
            meter_anchor_bar_ = bar_at_tick(timeline_tick_, meter_anchor_tick_,
                                            meter_anchor_bar_, meter_anchor_signature_);
            meter_anchor_tick_ = timeline_tick_;
        }
        meter_anchor_signature_ = desired.meter;
    }

    snapshot = {};
    snapshot.tempo_map = tempo_map_;
    snapshot.sample_rate = tempo_map_->sample_rate();
    snapshot.block_index = block_index_++;
    snapshot.frame_count = frame_count;
    snapshot.meter = desired.meter;
    snapshot.loop = desired.loop;
    snapshot.is_playing = desired.playing;
    snapshot.transport_changed = !first_block_ &&
                                 (desired.playing != previous_playing_ ||
                                  desired.loop.enabled != previous_loop_.enabled);
    snapshot.transport_started = desired.playing &&
                                 (first_block_ || !previous_playing_);
    snapshot.reset_requested = seeked;
    snapshot.time_sig_changed = !first_block_ && desired.meter != previous_meter_;

    auto make_range = [&](std::uint8_t index, std::uint32_t offset,
                          std::uint32_t count, bool discontinuity,
                          const timebase::TickPosition* forced_end_tick = nullptr) {
        auto& range = snapshot.ranges[index];
        range.sample_offset = offset;
        range.frame_count = count;
        range.timeline_sample_start = timeline_sample_;
        range.timeline_tick_start = timeline_tick_;
        range.monotonic_start = monotonic_;
        range.bar_start = bar_at_tick(range.timeline_tick_start, meter_anchor_tick_,
                                      meter_anchor_bar_, meter_anchor_signature_);
        range.tempo_bpm = tempo_cursor_.tempo_at_tick(range.timeline_tick_start);
        range.tempo_changed = index == 0
                                  ? !first_block_ &&
                                        range.tempo_bpm != previous_tempo_bpm_
                                  : range.tempo_bpm != snapshot.ranges[index - 1].tempo_bpm;
        range.discontinuity = discontinuity;
        if (desired.playing) {
            const timebase::SamplePosition end_sample{
                saturating_add(timeline_sample_.value, count)};
            range.timeline_tick_end = forced_end_tick != nullptr
                                          ? *forced_end_tick
                                          : tempo_cursor_.advance(end_sample).tick;
            if (range.timeline_tick_end < range.timeline_tick_start)
                range.timeline_tick_end = range.timeline_tick_start;
            const auto duration = range.timeline_tick_end - range.timeline_tick_start;
            range.monotonic_end = monotonic_ + duration;
            timeline_sample_ = end_sample;
            timeline_tick_ = range.timeline_tick_end;
            monotonic_ = range.monotonic_end;
        } else {
            range.timeline_tick_end = range.timeline_tick_start;
            range.monotonic_end = range.monotonic_start;
        }
    };

    if (!desired.playing) {
        make_range(0, 0, frame_count, pending_discontinuity_);
        snapshot.range_count = 1;
        pending_discontinuity_ = false;
    } else if (!desired.loop.enabled) {
        make_range(0, 0, frame_count, pending_discontinuity_);
        snapshot.range_count = 1;
        pending_discontinuity_ = false;
    } else {
        const auto loop_start = tempo_map_->ticks_to_samples(desired.loop.start);
        const auto loop_end = tempo_map_->ticks_to_samples(desired.loop.end);
        if (timeline_sample_.value >= loop_end.value) {
            timeline_sample_ = loop_start;
            timeline_tick_ = desired.loop.start;
            tempo_cursor_.seek(loop_start);
            pending_discontinuity_ = true;
        }

        const auto until_wrap = distance(timeline_sample_, loop_end);
        const auto first_count = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(frame_count, until_wrap));
        if (first_count > 0) {
            const auto* forced_end = static_cast<std::uint64_t>(first_count) == until_wrap
                                         ? &desired.loop.end
                                         : nullptr;
            make_range(0, 0, first_count, pending_discontinuity_, forced_end);
            snapshot.range_count = 1;
            pending_discontinuity_ = false;
        }

        const auto remaining = frame_count - first_count;
        if (remaining > 0) {
            timeline_sample_ = loop_start;
            timeline_tick_ = desired.loop.start;
            tempo_cursor_.seek(loop_start);
            make_range(snapshot.range_count, first_count, remaining, true);
            ++snapshot.range_count;
        } else if (timeline_sample_ == loop_end) {
            timeline_sample_ = loop_start;
            timeline_tick_ = desired.loop.start;
            tempo_cursor_.seek(loop_start);
            pending_discontinuity_ = true;
        }
    }

    snapshot.tempo_bpm = snapshot.ranges[0].tempo_bpm;
    previous_tempo_bpm_ = snapshot.ranges[snapshot.range_count - 1].tempo_bpm;
    previous_playing_ = desired.playing;
    previous_meter_ = desired.meter;
    previous_loop_ = desired.loop;
    first_block_ = false;
    return TransportError::None;
}

void MasterTransport::reset() noexcept {
    tempo_map_ = nullptr;
    tempo_cursor_ = {};
    max_buffer_size_ = 0;
    control_state_ = {};
    desired_.write(control_state_);
    timeline_sample_ = {};
    timeline_tick_ = {};
    monotonic_ = {};
    meter_anchor_tick_ = {};
    meter_anchor_bar_ = {};
    meter_anchor_signature_ = {};
    applied_seek_generation_ = 0;
    block_index_ = 0;
    previous_playing_ = false;
    previous_meter_ = {};
    previous_loop_ = {};
    previous_tempo_bpm_ = 120.0;
    first_block_ = true;
    pending_discontinuity_ = false;
}

} // namespace pulp::playback
