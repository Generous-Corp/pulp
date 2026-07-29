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

bool beats_to_ticks(double beats, timebase::TickPosition& result) noexcept {
    if (!std::isfinite(beats))
        return false;
    const auto scaled =
        static_cast<long double>(beats) * static_cast<long double>(timebase::kTicksPerQuarter);
    const auto rounded = std::round(scaled);
    const auto minimum = static_cast<long double>(std::numeric_limits<std::int64_t>::min());
    // Use an exclusive, exactly representable 2^63 upper bound. On platforms
    // where long double aliases double, converting INT64_MAX to long double can
    // itself round to 2^63 and make an inclusive comparison unsafe.
    const auto maximum_exclusive = -minimum;
    if (rounded < minimum || !(rounded < maximum_exclusive))
        return false;
    result.value = static_cast<std::int64_t>(rounded);
    return true;
}

double ticks_to_beats(timebase::TickPosition ticks) noexcept {
    return static_cast<double>(ticks.value) / static_cast<double>(timebase::kTicksPerQuarter);
}

double wrap_loop_beat(double beat, double loop_start, double loop_end) noexcept {
    if (beat < loop_end)
        return beat;
    const auto length = loop_end - loop_start;
    auto remainder = std::fmod(beat - loop_start, length);
    if (remainder < 0.0)
        remainder += length;
    return loop_start + remainder;
}

bool beat_fits_tick_domain(double beat) noexcept {
    timebase::TickPosition ignored;
    return beats_to_ticks(beat, ignored);
}

bool beats_nearly_equal(double lhs, double rhs, double absolute_floor) noexcept {
    const auto magnitude = std::max({1.0, std::abs(lhs), std::abs(rhs)});
    const auto numeric_floor = 8.0 * std::numeric_limits<double>::epsilon() * magnitude;
    return std::abs(lhs - rhs) <= std::max(absolute_floor, numeric_floor);
}

std::int64_t saturating_add(std::int64_t value, std::uint32_t increment) noexcept {
    const auto maximum = std::numeric_limits<std::int64_t>::max();
    if (value > maximum - static_cast<std::int64_t>(increment))
        return maximum;
    return value + static_cast<std::int64_t>(increment);
}

std::uint64_t distance(timebase::SamplePosition start, timebase::SamplePosition end) noexcept {
    if (end.value <= start.value)
        return 0;
    return static_cast<std::uint64_t>(end.value) - static_cast<std::uint64_t>(start.value);
}

timebase::BarPosition bar_at_tick(timebase::TickPosition tick, timebase::TickPosition anchor_tick,
                                  timebase::BarPosition anchor_bar, MeterSignature meter) noexcept {
    const long double ticks_per_bar = static_cast<long double>(timebase::kTicksPerQuarter) *
                                      static_cast<long double>(meter.numerator) * 4.0L /
                                      static_cast<long double>(meter.denominator);
    const long double relative_tick =
        static_cast<long double>(tick.value) - static_cast<long double>(anchor_tick.value);
    const long double projected =
        static_cast<long double>(anchor_bar.value) + std::floor(relative_tick / ticks_per_bar);
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
        transport.range_count == 0 || transport.range_count > transport.ranges.size() ||
        (transport.has_precise_host_loop &&
         (!std::isfinite(transport.host_loop_start_beats) ||
          !std::isfinite(transport.host_loop_end_beats) ||
          !(transport.host_loop_start_beats < transport.host_loop_end_beats))))
        return false;
    std::uint64_t expected_offset = 0;
    for (std::uint8_t index = 0; index < transport.range_count; ++index) {
        const auto& range = transport.ranges[index];
        if (range.frame_count == 0 || range.sample_offset != expected_offset)
            return false;
        if (range.has_precise_host_ticks &&
            (!range.host_beat_mapping || !std::isfinite(range.host_tick_start) ||
             !std::isfinite(range.host_tick_end) ||
             (transport.is_playing ? range.host_tick_end < range.host_tick_start
                                   : range.host_tick_end != range.host_tick_start)))
            return false;
        expected_offset += range.frame_count;
        if (expected_offset > transport.frame_count || (index != 0 && !range.discontinuity))
            return false;
    }
    return expected_offset == transport.frame_count;
}

long double
host_mapped_document_sample_at_output_offset(const TransportRange& range,
                                             const timebase::CompiledTempoMap& tempo_map,
                                             std::uint32_t output_offset) noexcept {
    if (range.frame_count == 0)
        return static_cast<long double>(
            tempo_map.ticks_to_samples(range.timeline_tick_start).value);
    const auto clamped = std::min(output_offset, range.frame_count);
    const auto fraction =
        static_cast<long double>(clamped) / static_cast<long double>(range.frame_count);
    const auto tick_start = range.has_precise_host_ticks
                                ? static_cast<long double>(range.host_tick_start)
                                : static_cast<long double>(range.timeline_tick_start.value);
    const auto tick_end = range.has_precise_host_ticks
                              ? static_cast<long double>(range.host_tick_end)
                              : static_cast<long double>(range.timeline_tick_end.value);
    const auto fractional_tick = tick_start + (tick_end - tick_start) * fraction;
    if (fractional_tick <= static_cast<long double>(std::numeric_limits<std::int64_t>::min()))
        return static_cast<long double>(
            tempo_map.ticks_to_samples({std::numeric_limits<std::int64_t>::min()}).value);
    if (fractional_tick >= static_cast<long double>(std::numeric_limits<std::int64_t>::max()))
        return static_cast<long double>(
            tempo_map.ticks_to_samples({std::numeric_limits<std::int64_t>::max()}).value);
    return tempo_map.fractional_ticks_to_samples(fractional_tick);
}

bool host_mapped_output_offset_for_tick(const TransportRange& range,
                                        timebase::TickPosition document_tick,
                                        std::uint32_t& output_offset) noexcept {
    const auto tick_start = range.has_precise_host_ticks
                                ? static_cast<long double>(range.host_tick_start)
                                : static_cast<long double>(range.timeline_tick_start.value);
    const auto tick_end = range.has_precise_host_ticks
                              ? static_cast<long double>(range.host_tick_end)
                              : static_cast<long double>(range.timeline_tick_end.value);
    if (!range.host_beat_mapping || range.frame_count == 0 || !(tick_start < tick_end))
        return false;
    const auto document = static_cast<long double>(document_tick.value);
    if (document < tick_start || !(document < tick_end))
        return false;
    const auto projected = (document - tick_start) * static_cast<long double>(range.frame_count) /
                           (tick_end - tick_start);
    const auto floored = std::floor(projected);
    output_offset = floored <= 0.0L
                        ? 0u
                        : static_cast<std::uint32_t>(std::min<long double>(
                              floored, static_cast<long double>(range.frame_count - 1)));
    return true;
}

TransportError MasterTransport::prepare(const timebase::CompiledTempoMap& tempo_map,
                                        const MasterTransportConfig& config) noexcept {
    reset();
    if (config.tempo_sync_source != nullptr &&
        (!std::isfinite(config.tempo_sync_quantum_beats) || config.tempo_sync_quantum_beats <= 0.0))
        return TransportError::InvalidTempoSyncConfig;
    tempo_map_ = &tempo_map;
    tempo_sync_source_ = config.tempo_sync_source;
    tempo_sync_quantum_beats_ = config.tempo_sync_quantum_beats;
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
    ++control_state_.playing_generation;
    publish_desired();
    return TransportError::None;
}

TransportError MasterTransport::set_tempo_sync_tempo(double tempo_bpm) noexcept {
    if (tempo_map_ == nullptr)
        return TransportError::NotPrepared;
    if (tempo_sync_source_ == nullptr)
        return TransportError::InvalidTempoSyncConfig;
    if (!std::isfinite(tempo_bpm) || tempo_bpm <= 0.0)
        return TransportError::InvalidTempo;
    control_state_.tempo_sync_tempo_bpm = tempo_bpm;
    ++control_state_.tempo_sync_tempo_generation;
    publish_desired();
    return TransportError::None;
}

TransportError MasterTransport::seek(timebase::TickPosition position) noexcept {
    if (tempo_map_ == nullptr)
        return TransportError::NotPrepared;
    control_state_.position = position;
    // An explicit reposition during a drag is the same intent as moving the
    // anchor, so the two positions never disagree.
    if (control_state_.scrubbing)
        control_state_.scrub_position = position;
    ++control_state_.seek_generation;
    publish_desired();
    return TransportError::None;
}

TransportError MasterTransport::begin_scrub(std::uint32_t window_frames,
                                            timebase::TickPosition position) noexcept {
    if (tempo_map_ == nullptr)
        return TransportError::NotPrepared;
    if (tempo_sync_source_ != nullptr)
        return TransportError::InvalidTempoSyncConfig;
    if (window_frames == 0)
        return TransportError::InvalidScrubWindow;
    if (window_frames < max_buffer_size_)
        return TransportError::ScrubWindowTooShortForMaximumBlock;
    control_state_.scrubbing = true;
    control_state_.scrub_window_frames = window_frames;
    control_state_.scrub_position = position;
    ++control_state_.scrub_generation;
    publish_desired();
    return TransportError::None;
}

TransportError MasterTransport::scrub_to(timebase::TickPosition position) noexcept {
    if (tempo_map_ == nullptr)
        return TransportError::NotPrepared;
    if (!control_state_.scrubbing)
        return TransportError::NotScrubbing;
    control_state_.scrub_position = position;
    publish_desired();
    return TransportError::None;
}

TransportError MasterTransport::end_scrub() noexcept {
    if (tempo_map_ == nullptr)
        return TransportError::NotPrepared;
    // scrub_position is deliberately retained: begin_block parks the playhead
    // on it as the drag releases.
    control_state_.scrubbing = false;
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
    if (tempo_sync_source_ != nullptr)
        return TransportError::TempoSyncHostTimeRequired;
    return begin_internal_block(frame_count, snapshot);
}

TransportError MasterTransport::begin_block(std::uint32_t frame_count,
                                            std::int64_t output_host_time_micros,
                                            TransportSnapshot& snapshot) noexcept {
    if (tempo_sync_source_ == nullptr)
        return begin_internal_block(frame_count, snapshot);
    return begin_tempo_synced_block(frame_count, output_host_time_micros, snapshot);
}

TransportError MasterTransport::begin_internal_block(std::uint32_t frame_count,
                                                     TransportSnapshot& snapshot) noexcept {
    runtime::ScopedNoAlloc no_alloc_guard;
    if (tempo_map_ == nullptr)
        return TransportError::NotPrepared;
    if (frame_count == 0 || frame_count > max_buffer_size_)
        return TransportError::InvalidFrameCount;

    const auto desired = desired_.read();

    // Leaving scrub parks the playhead on the anchor the drag released on,
    // never mid-window. A seek published in the same control window is the
    // later, more explicit intent, so it is applied afterwards and wins.
    const bool scrub_entered = desired.scrubbing && !previous_scrubbing_;
    const bool scrub_exited = !desired.scrubbing && previous_scrubbing_;
    if (scrub_exited) {
        timeline_sample_ = tempo_map_->ticks_to_samples(desired.scrub_position);
        tempo_cursor_.seek(timeline_sample_);
        timeline_tick_ = desired.scrub_position;
        pending_discontinuity_ = true;
        scrub_window_remaining_ = 0;
    }

    // A fresh drag abandons the window in flight; moving an existing drag does
    // not, which is what keeps the anchor latched to window boundaries.
    if (desired.scrub_generation != applied_scrub_generation_) {
        applied_scrub_generation_ = desired.scrub_generation;
        scrub_window_remaining_ = 0;
    }

    const bool seeked = desired.seek_generation != applied_seek_generation_;
    if (seeked) {
        timeline_sample_ = tempo_map_->ticks_to_samples(desired.position);
        tempo_cursor_.seek(timeline_sample_);
        timeline_tick_ = desired.position;
        applied_seek_generation_ = desired.seek_generation;
        pending_discontinuity_ = true;
        // An explicit reposition outranks the window in flight: the next scrub
        // window starts at the seek target instead of finishing the old one.
        scrub_window_remaining_ = 0;
    }

    // Scrubbing moves the playhead whether or not the musical transport rolls.
    const bool advancing = desired.scrubbing || desired.playing;

    if (desired.meter != meter_anchor_signature_) {
        if (first_block_) {
            meter_anchor_tick_ = {};
            meter_anchor_bar_ = {};
        } else {
            meter_anchor_bar_ = bar_at_tick(timeline_tick_, meter_anchor_tick_, meter_anchor_bar_,
                                            meter_anchor_signature_);
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
    snapshot.is_playing = advancing;
    snapshot.scrubbing = desired.scrubbing;
    snapshot.transport_changed = !first_block_ && (advancing != previous_playing_ ||
                                                   desired.loop.enabled != previous_loop_.enabled);
    snapshot.transport_started = advancing && (first_block_ || !previous_playing_);
    // Entering and leaving scrub are hard repositions. The window restarts in
    // between are not: they recur many times a second, and a per-grain state
    // reset would wipe consumer state that the plain discontinuity already
    // describes correctly.
    snapshot.reset_requested =
        seeked || scrub_entered || scrub_exited || desired.loop != previous_loop_;
    snapshot.time_sig_changed = !first_block_ && desired.meter != previous_meter_;
    if (seeked || snapshot.transport_started || scrub_entered || scrub_exited ||
        desired.loop != previous_loop_ || !advancing || desired.scrubbing || !desired.loop.enabled)
        loop_pass_index_ = 0;

    auto make_range = [&](std::uint8_t index, std::uint32_t offset, std::uint32_t count,
                          bool discontinuity,
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
        range.tempo_changed = index == 0 ? !first_block_ && range.tempo_bpm != previous_tempo_bpm_
                                         : range.tempo_bpm != snapshot.ranges[index - 1].tempo_bpm;
        range.discontinuity = discontinuity;
        range.loop_pass_index =
            advancing && !desired.scrubbing && desired.loop.enabled ? loop_pass_index_ : 0;
        if (advancing) {
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

    // Restarting the window is structurally a loop wrap: reposition, mark a
    // discontinuity, and split the block. Consumers that already release notes
    // and reset readers on a wrap therefore need no scrub-specific handling.
    auto start_scrub_window = [&]() {
        timeline_sample_ = tempo_map_->ticks_to_samples(desired.scrub_position);
        tempo_cursor_.seek(timeline_sample_);
        timeline_tick_ = desired.scrub_position;
        // Clamping to a full maximum block keeps "a block spans at most two
        // windows" a structural property of begin_block rather than something
        // only begin_scrub's validation guarantees.
        scrub_window_remaining_ = std::max(desired.scrub_window_frames, max_buffer_size_);
        pending_discontinuity_ = true;
    };

    if (desired.scrubbing) {
        if (scrub_window_remaining_ == 0)
            start_scrub_window();
        const auto first_count = std::min(frame_count, scrub_window_remaining_);
        make_range(0, 0, first_count, pending_discontinuity_);
        snapshot.range_count = 1;
        pending_discontinuity_ = false;
        scrub_window_remaining_ -= first_count;

        const auto remaining = frame_count - first_count;
        if (remaining > 0) {
            start_scrub_window();
            make_range(1, first_count, remaining, true);
            snapshot.range_count = 2;
            pending_discontinuity_ = false;
            scrub_window_remaining_ -= remaining;
        }
    } else if (!desired.playing) {
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
        const auto first_count =
            static_cast<std::uint32_t>(std::min<std::uint64_t>(frame_count, until_wrap));
        if (first_count > 0) {
            const auto* forced_end =
                static_cast<std::uint64_t>(first_count) == until_wrap ? &desired.loop.end : nullptr;
            make_range(0, 0, first_count, pending_discontinuity_, forced_end);
            snapshot.range_count = 1;
            pending_discontinuity_ = false;
        }

        const auto remaining = frame_count - first_count;
        if (remaining > 0) {
            timeline_sample_ = loop_start;
            timeline_tick_ = desired.loop.start;
            tempo_cursor_.seek(loop_start);
            ++loop_pass_index_;
            make_range(snapshot.range_count, first_count, remaining, true);
            ++snapshot.range_count;
        } else if (timeline_sample_ == loop_end) {
            timeline_sample_ = loop_start;
            timeline_tick_ = desired.loop.start;
            tempo_cursor_.seek(loop_start);
            ++loop_pass_index_;
            pending_discontinuity_ = true;
        }
    }

    snapshot.tempo_bpm = snapshot.ranges[0].tempo_bpm;
    previous_tempo_bpm_ = snapshot.ranges[snapshot.range_count - 1].tempo_bpm;
    previous_playing_ = advancing;
    previous_scrubbing_ = desired.scrubbing;
    previous_meter_ = desired.meter;
    previous_loop_ = desired.loop;
    first_block_ = false;
    return TransportError::None;
}

TransportError MasterTransport::begin_tempo_synced_block(std::uint32_t frame_count,
                                                         std::int64_t output_host_time_micros,
                                                         TransportSnapshot& snapshot) noexcept {
    runtime::ScopedNoAlloc no_alloc_guard;
    if (tempo_map_ == nullptr)
        return TransportError::NotPrepared;
    if (frame_count == 0 || frame_count > max_buffer_size_)
        return TransportError::InvalidFrameCount;
    if (tempo_sync_source_ == nullptr || !std::isfinite(tempo_sync_quantum_beats_) ||
        tempo_sync_quantum_beats_ <= 0.0)
        return TransportError::InvalidTempoSyncConfig;

    const auto desired = desired_.read();
    // A scrub window deliberately repositions on a private clock. Mixing that
    // with an active shared beat mapping would make neither clock authoritative.
    if (desired.scrubbing)
        return TransportError::InvalidTempoSyncConfig;

    TempoSyncBlockRequest request;
    request.output_host_time_micros = output_host_time_micros;
    request.frame_count = frame_count;
    request.sample_rate = static_cast<double>(tempo_map_->sample_rate().as_long_double());
    request.quantum_beats = tempo_sync_quantum_beats_;
    request.command.request_playing =
        desired.playing_generation != applied_tempo_sync_playing_generation_;
    request.command.playing = desired.playing;
    request.command.request_beat = desired.seek_generation != applied_tempo_sync_seek_generation_;
    request.command.beat = ticks_to_beats(desired.position);
    request.command.request_tempo =
        desired.tempo_sync_tempo_generation != applied_tempo_sync_tempo_generation_;
    request.command.tempo_bpm = desired.tempo_sync_tempo_bpm;
    if (!valid_tempo_sync_request(request))
        return TransportError::InvalidTempoSyncState;

    TempoSyncBlockState state;
    const auto source_error = tempo_sync_source_->capture_audio_block(request, state);
    if (source_error == TempoSyncError::Disabled || source_error == TempoSyncError::BackendFailure)
        return TransportError::TempoSyncUnavailable;
    if (source_error != TempoSyncError::None || !valid_tempo_sync_state(state))
        return TransportError::InvalidTempoSyncState;
    const bool block_playing =
        project_tempo_sync_playing(request, state, previous_playing_).playing_for_block;
    const auto source_beat_start = state.beat_start;
    const auto source_beat_end = block_playing ? state.beat_end : state.beat_start;
    if (!beat_fits_tick_domain(source_beat_start) || !beat_fits_tick_domain(source_beat_end))
        return TransportError::InvalidTempoSyncState;
    const bool explicit_seek = request.command.request_beat;
    const auto continuity_tolerance =
        std::max(1.0e-9, 2.0 * state.tempo_bpm / (60.0 * request.sample_rate));
    const bool inferred_jump =
        has_expected_tempo_sync_beat_ && block_playing &&
        !beats_nearly_equal(source_beat_start, expected_tempo_sync_beat_, continuity_tolerance);
    const bool loop_changed = !first_block_ && desired.loop != previous_loop_;
    const bool transport_started = block_playing && (first_block_ || !previous_playing_);
    if (!block_playing || explicit_seek || inferred_jump || transport_started || loop_changed)
        loop_pass_index_ = 0;

    double local_beat_start = source_beat_start;
    double local_beat_end = source_beat_end;
    double loop_start_beat = 0.0;
    double loop_end_beat = 0.0;
    double next_loop_boundary = 0.0;
    bool crosses_loop = false;
    bool ends_at_loop = false;
    if (block_playing && desired.loop.enabled) {
        loop_start_beat = ticks_to_beats(desired.loop.start);
        loop_end_beat = ticks_to_beats(desired.loop.end);
        const auto loop_length = loop_end_beat - loop_start_beat;
        const auto source_span = source_beat_end - source_beat_start;
        if (!(loop_length > 0.0) ||
            (source_span > loop_length && !beats_nearly_equal(source_span, loop_length, 1.0e-9)))
            return TransportError::LoopTooShortForMaximumBlock;

        local_beat_start = wrap_loop_beat(source_beat_start, loop_start_beat, loop_end_beat);
        if (source_beat_start < loop_start_beat) {
            next_loop_boundary = loop_end_beat;
        } else {
            const auto cycle = std::floor((source_beat_start - loop_start_beat) / loop_length);
            next_loop_boundary = loop_start_beat + (cycle + 1.0) * loop_length;
        }
        if (source_beat_end > next_loop_boundary &&
            !beats_nearly_equal(source_beat_end, next_loop_boundary, 1.0e-9)) {
            if (source_beat_end > next_loop_boundary + loop_length &&
                !beats_nearly_equal(source_beat_end, next_loop_boundary + loop_length, 1.0e-9))
                return TransportError::LoopTooShortForMaximumBlock;
            crosses_loop = true;
        } else if (beats_nearly_equal(source_beat_end, next_loop_boundary, 1.0e-9)) {
            ends_at_loop = true;
        }
        local_beat_end = crosses_loop || ends_at_loop
                             ? loop_end_beat
                             : wrap_loop_beat(source_beat_end, loop_start_beat, loop_end_beat);
    }

    timebase::TickPosition local_start_tick;
    if (!beats_to_ticks(local_beat_start, local_start_tick))
        return TransportError::InvalidTempoSyncState;
    if (desired.meter != meter_anchor_signature_) {
        if (first_block_) {
            meter_anchor_tick_ = {};
            meter_anchor_bar_ = {};
        } else {
            meter_anchor_bar_ = bar_at_tick(local_start_tick, meter_anchor_tick_, meter_anchor_bar_,
                                            meter_anchor_signature_);
            meter_anchor_tick_ = local_start_tick;
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
    snapshot.is_playing = block_playing;
    snapshot.transport_changed =
        !first_block_ && (block_playing != previous_playing_ || loop_changed);
    snapshot.transport_started = transport_started;
    snapshot.reset_requested = explicit_seek || inferred_jump || loop_changed;
    snapshot.time_sig_changed = !first_block_ && desired.meter != previous_meter_;

    auto make_range = [&](std::uint8_t index, std::uint32_t offset, std::uint32_t count,
                          double beat_start, double beat_end, bool discontinuity) {
        auto& range = snapshot.ranges[index];
        range.sample_offset = offset;
        range.frame_count = count;
        beats_to_ticks(beat_start, range.timeline_tick_start);
        beats_to_ticks(block_playing ? beat_end : beat_start, range.timeline_tick_end);
        range.timeline_sample_start = tempo_map_->ticks_to_samples(range.timeline_tick_start);
        range.monotonic_start = monotonic_;
        range.monotonic_end =
            block_playing ? monotonic_ + (range.timeline_tick_end - range.timeline_tick_start)
                          : monotonic_;
        range.bar_start = bar_at_tick(range.timeline_tick_start, meter_anchor_tick_,
                                      meter_anchor_bar_, meter_anchor_signature_);
        range.tempo_bpm = state.tempo_bpm;
        range.tempo_changed = index == 0 ? !first_block_ && state.tempo_bpm != previous_tempo_bpm_
                                         : state.tempo_bpm != snapshot.ranges[index - 1].tempo_bpm;
        range.discontinuity = discontinuity;
        range.host_beat_mapping = true;
        range.host_tick_start = beat_start * static_cast<double>(timebase::kTicksPerQuarter);
        range.host_tick_end = (block_playing ? beat_end : beat_start) *
                              static_cast<double>(timebase::kTicksPerQuarter);
        range.has_precise_host_ticks = true;
        range.loop_pass_index = block_playing && desired.loop.enabled ? loop_pass_index_ : 0;
        monotonic_ = range.monotonic_end;
        timeline_tick_ = range.timeline_tick_end;
        timeline_sample_ = tempo_map_->ticks_to_samples(timeline_tick_);
    };

    const bool first_discontinuity = pending_discontinuity_ || explicit_seek || inferred_jump;
    pending_discontinuity_ = false;
    if (!crosses_loop) {
        make_range(0, 0, frame_count, local_beat_start, local_beat_end, first_discontinuity);
        snapshot.range_count = 1;
        if (ends_at_loop) {
            ++loop_pass_index_;
            pending_discontinuity_ = true;
        }
    } else {
        const auto source_span = source_beat_end - source_beat_start;
        const auto boundary_fraction = (next_loop_boundary - source_beat_start) / source_span;
        auto first_count = static_cast<std::uint32_t>(
            std::ceil(boundary_fraction * static_cast<double>(frame_count)));
        first_count = std::clamp(first_count, std::uint32_t{1}, frame_count);
        make_range(0, 0, first_count, local_beat_start, loop_end_beat, first_discontinuity);
        snapshot.range_count = 1;
        const auto remaining = frame_count - first_count;
        ++loop_pass_index_;
        if (remaining > 0) {
            const auto second_end = loop_start_beat + (source_beat_end - next_loop_boundary);
            make_range(1, first_count, remaining, loop_start_beat, second_end, true);
            snapshot.range_count = 2;
        } else {
            pending_discontinuity_ = true;
        }
    }

    snapshot.tempo_bpm = state.tempo_bpm;
    previous_tempo_bpm_ = state.tempo_bpm;
    previous_playing_ = block_playing;
    previous_scrubbing_ = false;
    previous_meter_ = desired.meter;
    previous_loop_ = desired.loop;
    first_block_ = false;
    has_expected_tempo_sync_beat_ = block_playing;
    expected_tempo_sync_beat_ = state.beat_end;
    applied_tempo_sync_playing_generation_ = desired.playing_generation;
    applied_tempo_sync_seek_generation_ = desired.seek_generation;
    applied_tempo_sync_tempo_generation_ = desired.tempo_sync_tempo_generation;
    return valid_transport_ranges(snapshot) ? TransportError::None
                                            : TransportError::InvalidTempoSyncState;
}

void MasterTransport::reset() noexcept {
    tempo_map_ = nullptr;
    tempo_cursor_ = {};
    tempo_sync_source_ = nullptr;
    tempo_sync_quantum_beats_ = 4.0;
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
    applied_scrub_generation_ = 0;
    applied_tempo_sync_playing_generation_ = 0;
    applied_tempo_sync_seek_generation_ = 0;
    applied_tempo_sync_tempo_generation_ = 0;
    block_index_ = 0;
    loop_pass_index_ = 0;
    scrub_window_remaining_ = 0;
    previous_scrubbing_ = false;
    previous_playing_ = false;
    previous_meter_ = {};
    previous_loop_ = {};
    previous_tempo_bpm_ = 120.0;
    first_block_ = true;
    pending_discontinuity_ = false;
    has_expected_tempo_sync_beat_ = false;
    expected_tempo_sync_beat_ = 0.0;
}

} // namespace pulp::playback
