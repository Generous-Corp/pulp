#include <pulp/playback/realtime_stretch_renderer.hpp>

#include <pulp/playback/audio_renderer.hpp>
#include <pulp/playback/offline_stretch_artifact.hpp>
#include <pulp/playback/program.hpp>
#include <pulp/playback/track_mixer_program.hpp>
#include <pulp/runtime/scoped_no_alloc.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <new>
#include <vector>

namespace pulp::playback {
namespace {

bool live_stretch(const AudioClipRendererProgram& clip) noexcept {
    return clip.source_time_mapping ==
           AudioClipRendererProgram::SourceTimeMapping::OfflineStretchArtifact;
}

std::uint64_t checked_bytes(std::size_t elements) noexcept {
    return elements > std::numeric_limits<std::uint64_t>::max() / sizeof(float)
               ? std::numeric_limits<std::uint64_t>::max()
               : static_cast<std::uint64_t>(elements) * sizeof(float);
}

bool add_charge(std::uint64_t bytes, std::uint64_t limit, std::uint64_t& total) noexcept {
    if (bytes > limit || total > limit - bytes)
        return false;
    total += bytes;
    return true;
}

long double tick_start(const TransportRange& range) noexcept {
    return range.has_precise_host_ticks ? static_cast<long double>(range.host_tick_start)
                                        : static_cast<long double>(range.timeline_tick_start.value);
}

long double tick_end(const TransportRange& range) noexcept {
    return range.has_precise_host_ticks ? static_cast<long double>(range.host_tick_end)
                                        : static_cast<long double>(range.timeline_tick_end.value);
}

float clip_envelope(const AudioClipRendererProgram& clip, long double relative) noexcept {
    auto value = static_cast<long double>(clip.gain_linear);
    if (clip.fade_in_frames != 0 && relative < clip.fade_in_frames)
        value *= relative / static_cast<long double>(clip.fade_in_frames);
    const auto remaining =
        std::max(0.0L, static_cast<long double>(clip.timeline_frame_count - 1u) - relative);
    if (clip.fade_out_frames != 0 && remaining < clip.fade_out_frames)
        value *= remaining / static_cast<long double>(clip.fade_out_frames);
    return static_cast<float>(value);
}

float artifact_sample(const AudioClipRendererProgram& clip, std::size_t channel,
                      long double document_position) noexcept {
    const auto relative = document_position - static_cast<long double>(clip.timeline_start);
    if (!(relative >= 0.0L) || !(relative < static_cast<long double>(clip.timeline_frame_count)))
        return 0.0f;
    const auto& samples = clip.audio->channels[channel];
    const auto base = std::floor(relative);
    const auto first = static_cast<std::size_t>(base);
    const auto second = std::min(first + 1u, samples.size() - 1u);
    const auto fraction = static_cast<float>(relative - base);
    return (samples[first] + (samples[second] - samples[first]) * fraction) *
           clip_envelope(clip, relative);
}

long double host_output_for_document_span(const TransportRange& range,
                                          const timebase::CompiledTempoMap& map,
                                          long double document_start,
                                          long double document_end) noexcept {
    const auto host_ticks = tick_end(range) - tick_start(range);
    if (!(document_end > document_start) || !(host_ticks > 0.0L) || range.frame_count == 0)
        return std::numeric_limits<long double>::quiet_NaN();
    const auto document_ticks = map.fractional_samples_to_ticks(document_end) -
                                map.fractional_samples_to_ticks(document_start);
    return document_ticks * static_cast<long double>(range.frame_count) / host_ticks;
}

bool has_meaningful_document_span(long double start, long double end) noexcept {
    const auto scale = std::max({1.0L, std::abs(start), std::abs(end)});
    const auto tolerance = 8.0L * std::numeric_limits<double>::epsilon() * scale;
    return end - start > tolerance;
}

struct MixerRun {
    TrackMixerControlCursor gain;
    TrackMixerControlCursor pan;

    explicit MixerRun(const TrackMixerProgram& mixer) noexcept {
        gain.reset(mixer.gain_automation, mixer.gain_linear);
        pan.reset(mixer.pan_automation, mixer.pan);
    }

    float factor(long double tick, std::size_t channel, std::size_t channels) noexcept {
        return clamped_track_gain(gain.value_at_tick(tick)) *
               track_mixer_channel_gain(clamped_track_pan(pan.value_at_tick(tick)), channel,
                                        channels);
    }
};

} // namespace

struct RealtimeStretchProgramRuntime::Impl {
    struct ClipLane {
        timeline::ItemId id;
        const AudioClipRendererProgram* clip = nullptr;
        signal::RealtimePitchTimeProcessor processor;
        std::vector<float> fifo;
        std::uint32_t fifo_capacity = 0;
        std::uint32_t fifo_read = 0;
        std::uint32_t fifo_write = 0;
        std::uint32_t fifo_count = 0;
        std::uint64_t dequeued_total = 0;

        bool enqueue(float* const* source, std::uint32_t frames) noexcept {
            if (frames > fifo_capacity - fifo_count)
                return false;
            const auto channels = clip->audio->num_channels();
            for (std::uint32_t frame = 0; frame < frames; ++frame) {
                for (std::size_t channel = 0; channel < channels; ++channel)
                    fifo[channel * fifo_capacity + fifo_write] = source[channel][frame];
                fifo_write = (fifo_write + 1u) % fifo_capacity;
            }
            fifo_count += frames;
            return true;
        }

        bool enqueue_silence(std::uint32_t frames) noexcept {
            if (frames > fifo_capacity - fifo_count)
                return false;
            const auto channels = clip->audio->num_channels();
            for (std::uint32_t frame = 0; frame < frames; ++frame) {
                for (std::size_t channel = 0; channel < channels; ++channel)
                    fifo[channel * fifo_capacity + fifo_write] = 0.0f;
                fifo_write = (fifo_write + 1u) % fifo_capacity;
            }
            fifo_count += frames;
            return true;
        }

        bool dequeue(float* const* destination, std::uint32_t frames) noexcept {
            if (frames > fifo_count)
                return false;
            const auto channels = clip->audio->num_channels();
            for (std::uint32_t frame = 0; frame < frames; ++frame) {
                for (std::size_t channel = 0; channel < channels; ++channel)
                    destination[channel][frame] = fifo[channel * fifo_capacity + fifo_read];
                fifo_read = (fifo_read + 1u) % fifo_capacity;
            }
            fifo_count -= frames;
            return true;
        }

        void trim_newest(std::uint32_t frames) noexcept {
            frames = std::min(frames, fifo_count);
            fifo_write = (fifo_write + fifo_capacity - frames % fifo_capacity) % fifo_capacity;
            fifo_count -= frames;
        }
    };

    struct TrackLane {
        timeline::ItemId id;
        std::vector<ClipLane> clips;
        std::vector<float> input_scratch;
        std::vector<float> output_scratch;
        std::vector<float> audio_delay;
        std::vector<float> mixer_delay;
        std::array<const float*, signal::kRealtimePitchTimeMaximumChannels> input_ptrs{};
        std::array<float*, signal::kRealtimePitchTimeMaximumChannels> output_ptrs{};
        std::uint32_t delay_cursor = 0;
        std::uint64_t elapsed_output_frames = 0;
        bool poisoned = false;
        std::uint64_t playback_epoch = 0;
        std::uint64_t loop_pass_index = 0;
        long double next_document_input = 0.0L;
        long double expected_document_start = 0.0L;
        long double pending_host_output = 0.0L;
        long double analysis_host_output = 0.0L;
        std::uint32_t analysis_input_count = 0;
        bool has_identity = false;
        bool producer_initialized = false;
        bool host_mapped_producer = false;
    };

    TrackLane* find_track(timeline::ItemId id) noexcept {
        const auto found = std::find_if(tracks.begin(), tracks.end(),
                                        [id](const auto& lane) { return lane.id == id; });
        return found == tracks.end() ? nullptr : &*found;
    }
    const TrackLane* find_track(timeline::ItemId id) const noexcept {
        const auto found = std::find_if(tracks.begin(), tracks.end(),
                                        [id](const auto& lane) { return lane.id == id; });
        return found == tracks.end() ? nullptr : &*found;
    }

    void configure_output(TrackLane& lane, ClipLane& clip) noexcept {
        const auto channels = clip.clip->audio->num_channels();
        for (std::size_t channel = 0; channel < channels; ++channel) {
            auto* scratch = lane.output_scratch.data() + channel * maximum_block_frames;
            lane.output_ptrs[channel] = scratch;
        }
    }

    bool drain_processor(TrackLane& lane, ClipLane& clip) noexcept {
        configure_output(lane, clip);
        while (clip.processor.available_stretched() > 0) {
            const auto take = static_cast<std::uint32_t>(std::min(
                clip.processor.available_stretched(), static_cast<int>(maximum_block_frames)));
            if (take > clip.fifo_capacity - clip.fifo_count ||
                clip.processor.read_stretched(lane.output_ptrs.data(), static_cast<int>(take)) !=
                    static_cast<int>(take) ||
                !clip.enqueue(lane.output_ptrs.data(), take))
                return false;
        }
        return true;
    }

    bool finalize_segment(TrackLane& lane) noexcept {
        if (lane.has_identity) {
            const auto finalize_guard = static_cast<std::uint64_t>(latency) /
                                            std::max<std::uint32_t>(1, maximum_block_frames) +
                                        32u;
            for (auto& clip : lane.clips) {
                bool complete = false;
                for (std::uint64_t step = 0; step < finalize_guard; ++step) {
                    if (!drain_processor(lane, clip))
                        return false;
                    const auto plan =
                        clip.processor.plan_finalize(static_cast<int>(maximum_block_frames));
                    if (plan.status == signal::PitchTimeStreamFinalizePlanStatus::complete) {
                        complete = true;
                        break;
                    }
                    if (plan.status == signal::PitchTimeStreamFinalizePlanStatus::ready) {
                        const auto status = clip.processor.finalize(plan.samples);
                        if (status == signal::PitchTimeStreamFinalizeStatus::invalid_mode ||
                            status == signal::PitchTimeStreamFinalizeStatus::invalid_request)
                            return false;
                    } else if (plan.status !=
                               signal::PitchTimeStreamFinalizePlanStatus::needs_drain) {
                        return false;
                    }
                }
                if (!complete || !drain_processor(lane, clip))
                    return false;
                if (clip.dequeued_total > lane.elapsed_output_frames)
                    return false;
                const auto remaining64 = lane.elapsed_output_frames - clip.dequeued_total;
                if (remaining64 > clip.fifo_capacity)
                    return false;
                const auto old_remaining = static_cast<std::uint32_t>(remaining64);
                if (clip.fifo_count > old_remaining)
                    clip.trim_newest(clip.fifo_count - old_remaining);
                else if (!clip.enqueue_silence(old_remaining - clip.fifo_count))
                    return false;
            }
        }
        for (auto& clip : lane.clips)
            clip.processor.reset();
        lane.pending_host_output = 0.0L;
        lane.analysis_host_output = 0.0L;
        lane.analysis_input_count = 0;
        return true;
    }

    void reset_track_state(TrackLane& lane) noexcept {
        for (auto& clip : lane.clips) {
            clip.processor.reset();
            std::fill(clip.fifo.begin(), clip.fifo.end(), 0.0f);
            clip.fifo_read = 0;
            clip.fifo_write = 0;
            clip.fifo_count = 0;
            clip.dequeued_total = 0;
        }
        std::fill(lane.audio_delay.begin(), lane.audio_delay.end(), 0.0f);
        std::fill(lane.mixer_delay.begin(), lane.mixer_delay.end(), 1.0f);
        lane.has_identity = false;
        lane.delay_cursor = 0;
        lane.elapsed_output_frames = 0;
        lane.expected_document_start = 0.0L;
        lane.next_document_input = 0.0L;
        lane.pending_host_output = 0.0L;
        lane.analysis_host_output = 0.0L;
        lane.analysis_input_count = 0;
        lane.producer_initialized = false;
        lane.host_mapped_producer = false;
    }

    RealtimeStretchRenderCode poison(TrackLane& lane, audio::BufferView<float> output,
                                     RealtimeStretchRenderCode code) noexcept {
        reset_track_state(lane);
        lane.poisoned = true;
        output.clear();
        return code;
    }

    ProgramGeneration generation = 0;
    const PlaybackProgram* publication = nullptr;
    const timebase::CompiledTempoMap* tempo_map = nullptr;
    std::vector<TrackLane> tracks;
    std::uint32_t maximum_block_frames = 0;
    std::uint32_t output_channels = 0;
    std::uint32_t latency = 0;
    float maximum_ratio = 1.0f;
    std::uint64_t reserved_bytes = 0;
    bool fail_after_mutation_for_test = false;
    bool fail_prepare_allocation_for_test = false;
};

RealtimeStretchProgramRuntime::RealtimeStretchProgramRuntime() : impl_(std::make_unique<Impl>()) {}
RealtimeStretchProgramRuntime::~RealtimeStretchProgramRuntime() = default;
RealtimeStretchProgramRuntime::RealtimeStretchProgramRuntime(
    RealtimeStretchProgramRuntime&&) noexcept = default;
RealtimeStretchProgramRuntime&
RealtimeStretchProgramRuntime::operator=(RealtimeStretchProgramRuntime&&) noexcept = default;

RealtimeStretchStateBankAdmission RealtimeStretchProgramRuntime::prepare(
    const PlaybackProgram& program, double sample_rate, std::uint32_t maximum_block_frames,
    std::uint32_t output_channels, const AudioRendererLimits& limits) {
    auto aggregate =
        admit_realtime_stretch_program(program, sample_rate, maximum_block_frames, limits);
    if (!aggregate)
        return aggregate;
    if (sample_rate != static_cast<double>(program.tempo_map().sample_rate().as_long_double()))
        return {RealtimeStretchStateBankError::InvalidConfiguration,
                {},
                0,
                0,
                0,
                signal::PitchTimePrepareStatus::prepared};
    if (output_channels == 0 || output_channels > limits.max_channels)
        return {RealtimeStretchStateBankError::ChannelLimitExceeded,
                {},
                output_channels,
                limits.max_channels};

    const bool force_allocation_failure = impl_ && impl_->fail_prepare_allocation_for_test;
    if (impl_)
        impl_->fail_prepare_allocation_for_test = false;
#if defined(__cpp_exceptions)
    try {
        if (force_allocation_failure)
            throw std::bad_alloc{};
#else
    if (force_allocation_failure)
        return {RealtimeStretchStateBankError::AllocationFailed, {}, 0, 0, 0,
                signal::PitchTimePrepareStatus::prepared};
#endif
        auto candidate = std::make_unique<Impl>();
        candidate->generation = program.generation();
        candidate->publication = &program;
        candidate->tempo_map = &program.tempo_map();
        candidate->maximum_block_frames = maximum_block_frames;
        candidate->output_channels = output_channels;
        candidate->maximum_ratio = limits.realtime_stretch_max_time_ratio;
        if (!add_charge(sizeof(Impl), limits.max_realtime_stretch_state_bytes,
                        candidate->reserved_bytes))
            return {RealtimeStretchStateBankError::StateBytesExceeded, {}, 0, 0, 0,
                    signal::PitchTimePrepareStatus::prepared};

        signal::RealtimePitchTimeConfig geometry_config;
        geometry_config.mode = signal::PitchTimeMode::time_stretch;
        geometry_config.quality = signal::PitchTimeQuality::low_latency;
        geometry_config.channels = 1;
        geometry_config.max_block = static_cast<int>(maximum_block_frames);
        geometry_config.max_time_ratio = candidate->maximum_ratio;
        signal::RealtimePitchTimePreparedGeometry<float> geometry;
        const auto geometry_status = signal::checked_realtime_pitch_time_prepared_geometry<float>(
            geometry_config, 1.0, limits.max_realtime_stretch_allocation_bytes, geometry);
        if (geometry_status != signal::PitchTimePrepareStatus::prepared)
            return {RealtimeStretchStateBankError::ProcessorPrepareRejected,
                    {},
                    0,
                    limits.max_realtime_stretch_allocation_bytes,
                    0,
                    geometry_status};
        candidate->latency = static_cast<std::uint32_t>(geometry.maximum_stream_output_lag_samples);

        candidate->tracks.reserve(program.tracks().size());
        bool has_realtime_stretch = false;
        for (const auto& track : program.tracks()) {
            Impl::TrackLane lane;
            lane.id = track->id();
            if (const auto* audio = track->audio_program()) {
                const auto live_clip_count = static_cast<std::size_t>(
                    std::count_if(audio->clips().begin(), audio->clips().end(), live_stretch));
                lane.clips.reserve(live_clip_count);
                for (const auto& clip : audio->clips()) {
                    if (!live_stretch(clip))
                        continue;
                    signal::RealtimePitchTimeConfig config = geometry_config;
                    config.channels = static_cast<int>(clip.audio->num_channels());
                    Impl::ClipLane clip_lane;
                    clip_lane.id = clip.id;
                    clip_lane.clip = &clip;
                    const auto status = clip_lane.processor.prepare(
                        sample_rate, config, limits.max_realtime_stretch_allocation_bytes);
                    if (status != signal::PitchTimePrepareStatus::prepared)
                        return {RealtimeStretchStateBankError::ProcessorPrepareRejected,
                                clip.id,
                                0,
                                0,
                                0,
                                status};
                    lane.clips.push_back(std::move(clip_lane));
                }
            }
            if (lane.clips.capacity() >
                (std::numeric_limits<std::uint64_t>::max() - sizeof(Impl::TrackLane)) /
                    sizeof(Impl::ClipLane))
                return {RealtimeStretchStateBankError::StateBytesExceeded, {}, 0, 0, 0,
                        signal::PitchTimePrepareStatus::prepared};
            const auto metadata_bytes =
                static_cast<std::uint64_t>(sizeof(Impl::TrackLane)) +
                static_cast<std::uint64_t>(lane.clips.capacity()) * sizeof(Impl::ClipLane);
            if (!add_charge(metadata_bytes, limits.max_realtime_stretch_state_bytes,
                            candidate->reserved_bytes))
                return {RealtimeStretchStateBankError::StateBytesExceeded, {}, 0, 0, 0,
                        signal::PitchTimePrepareStatus::prepared};
            if (!lane.clips.empty()) {
                has_realtime_stretch = true;
                const auto fifo_capacity64 =
                    static_cast<std::uint64_t>(candidate->latency) * 2u + maximum_block_frames;
                if (fifo_capacity64 == 0 ||
                    fifo_capacity64 > std::numeric_limits<std::uint32_t>::max())
                    return {RealtimeStretchStateBankError::InvalidConfiguration,
                            {},
                            0,
                            0,
                            0,
                            signal::PitchTimePrepareStatus::prepared};
                const auto fifo_capacity = static_cast<std::uint32_t>(fifo_capacity64);
                const auto scratch_elements =
                    static_cast<std::size_t>(limits.max_channels) * maximum_block_frames;
                const auto delay_elements = static_cast<std::size_t>(output_channels) *
                                            std::max<std::uint32_t>(1, candidate->latency);
                lane.input_scratch.assign(scratch_elements, 0.0f);
                lane.output_scratch.assign(scratch_elements, 0.0f);
                lane.audio_delay.assign(delay_elements, 0.0f);
                lane.mixer_delay.assign(delay_elements, 1.0f);
                for (auto& clip : lane.clips) {
                    clip.fifo_capacity = fifo_capacity;
                    const auto fifo_elements =
                        static_cast<std::size_t>(clip.clip->audio->num_channels()) * fifo_capacity;
                    clip.fifo.assign(fifo_elements, 0.0f);
                    const auto fifo_bytes = checked_bytes(fifo_elements);
                    if (fifo_bytes == std::numeric_limits<std::uint64_t>::max() ||
                        !add_charge(fifo_bytes, limits.max_realtime_stretch_state_bytes,
                                    candidate->reserved_bytes))
                        return {RealtimeStretchStateBankError::StateBytesExceeded, {}, 0, 0, 0,
                                signal::PitchTimePrepareStatus::prepared};
                }
                const auto scratch_bytes = checked_bytes(scratch_elements);
                const auto delay_bytes = checked_bytes(delay_elements);
                if (scratch_bytes == std::numeric_limits<std::uint64_t>::max() ||
                    delay_bytes == std::numeric_limits<std::uint64_t>::max() ||
                    scratch_bytes > std::numeric_limits<std::uint64_t>::max() / 2u ||
                    delay_bytes > std::numeric_limits<std::uint64_t>::max() / 2u)
                    return {RealtimeStretchStateBankError::StateBytesExceeded, {}, 0, 0, 0,
                            signal::PitchTimePrepareStatus::prepared};
                if (!add_charge(scratch_bytes, limits.max_realtime_stretch_state_bytes,
                                candidate->reserved_bytes) ||
                    !add_charge(scratch_bytes, limits.max_realtime_stretch_state_bytes,
                                candidate->reserved_bytes) ||
                    !add_charge(delay_bytes, limits.max_realtime_stretch_state_bytes,
                                candidate->reserved_bytes) ||
                    !add_charge(delay_bytes, limits.max_realtime_stretch_state_bytes,
                                candidate->reserved_bytes))
                    return {RealtimeStretchStateBankError::StateBytesExceeded, {}, 0, 0, 0,
                            signal::PitchTimePrepareStatus::prepared};
            }
            candidate->tracks.push_back(std::move(lane));
        }
        if (!has_realtime_stretch)
            candidate->latency = 0;
        if (!add_charge(aggregate.reserved_state_bytes, limits.max_realtime_stretch_state_bytes,
                        candidate->reserved_bytes))
            return {RealtimeStretchStateBankError::StateBytesExceeded,
                    {},
                    aggregate.reserved_state_bytes,
                    limits.max_realtime_stretch_state_bytes};
        impl_ = std::move(candidate);
        aggregate.reserved_state_bytes = impl_->reserved_bytes;
        return aggregate;
#if defined(__cpp_exceptions)
    } catch (...) {
        return {RealtimeStretchStateBankError::AllocationFailed, {}, 0, 0, 0,
                signal::PitchTimePrepareStatus::prepared};
    }
#endif
}

RealtimeStretchRenderCode RealtimeStretchProgramRuntime::preflight_track(
    const PlaybackProgram& program, const TrackProgram& track, const TransportSnapshot& transport,
    audio::BufferView<float> output) const noexcept {
    runtime::ScopedNoAlloc no_alloc;
    if (!impl_ || impl_->publication == nullptr)
        return RealtimeStretchRenderCode::StateRequired;
    if (&program != impl_->publication || program.generation() != impl_->generation ||
        &program.tempo_map() != impl_->tempo_map)
        return RealtimeStretchRenderCode::StalePublication;
    const auto* lane = impl_->find_track(track.id());
    if (lane == nullptr) {
        const auto* audio = track.audio_program();
        return audio != nullptr &&
                       std::any_of(audio->clips().begin(), audio->clips().end(), live_stretch)
                   ? RealtimeStretchRenderCode::StateRequired
                   : RealtimeStretchRenderCode::NotRequired;
    }
    if (lane->clips.empty())
        return RealtimeStretchRenderCode::NotRequired;
    if (lane->poisoned)
        return RealtimeStretchRenderCode::StateRequired;
    if (output.num_channels() != impl_->output_channels ||
        output.num_samples() > impl_->maximum_block_frames ||
        transport.frame_count != output.num_samples() || transport.range_count == 0 ||
        transport.tempo_map != impl_->tempo_map || !valid_transport_ranges(transport) ||
        lane->elapsed_output_frames >
            std::numeric_limits<std::uint64_t>::max() - transport.frame_count)
        return RealtimeStretchRenderCode::StateRequired;
    if (transport.scrubbing)
        return RealtimeStretchRenderCode::UnsupportedScrubbing;
    const auto host_beat_mapping = transport.ranges[0].host_beat_mapping;
    for (std::uint8_t index = 1; index < transport.range_count; ++index) {
        if (transport.ranges[index].host_beat_mapping != host_beat_mapping)
            return RealtimeStretchRenderCode::StateRequired;
    }

    const bool producer_changed =
        lane->producer_initialized && lane->host_mapped_producer != host_beat_mapping;
    auto expected = lane->expected_document_start;
    auto next_input = lane->next_document_input;
    auto pending_output = lane->pending_host_output;
    bool gap = producer_changed;
    for (std::uint8_t index = 0; index < transport.range_count; ++index) {
        const auto& range = transport.ranges[index];
        const auto start =
            host_beat_mapping
                ? host_mapped_document_sample_at_output_offset(range, *impl_->tempo_map, 0)
                : static_cast<long double>(range.timeline_sample_start.value);
        const auto end = host_beat_mapping ? host_mapped_document_sample_at_output_offset(
                                                 range, *impl_->tempo_map, range.frame_count)
                                           : start + range.frame_count;
        if (!(end > start) || !std::isfinite(start) || !std::isfinite(end))
            return RealtimeStretchRenderCode::ImpossibleRatio;
        const bool identity_changed = range.discontinuity || !lane->has_identity ||
                                      lane->playback_epoch != range.playback_epoch ||
                                      lane->loop_pass_index != range.loop_pass_index;
        if ((producer_changed && index == 0) || identity_changed ||
            std::abs(start - expected) > 1.0e-6L) {
            gap = true;
            expected = start;
            next_input = start;
            pending_output = 0.0L;
        }

        if (host_beat_mapping) {
            auto covered = start;
            while (has_meaningful_document_span(covered, end)) {
                const auto segment_end = std::min(end, next_input + 1.0L);
                const auto contribution =
                    host_output_for_document_span(range, *impl_->tempo_map, covered, segment_end);
                const auto density = contribution / (segment_end - covered);
                if (!std::isfinite(contribution) || !std::isfinite(density) ||
                    density < 1.0L / impl_->maximum_ratio || density > impl_->maximum_ratio)
                    return RealtimeStretchRenderCode::ImpossibleRatio;
                pending_output += contribution;
                covered = segment_end;
                if (!has_meaningful_document_span(covered, next_input + 1.0L)) {
                    if (pending_output < 1.0L / impl_->maximum_ratio ||
                        pending_output > impl_->maximum_ratio)
                        return RealtimeStretchRenderCode::ImpossibleRatio;
                    next_input += 1.0L;
                    pending_output = 0.0L;
                }
            }
        }
        expected = end;
    }
    if (!host_beat_mapping) {
        for (const auto& clip : lane->clips) {
            const auto retained =
                producer_changed ? static_cast<std::uint64_t>(impl_->latency) : clip.fifo_count;
            if (retained > clip.fifo_capacity ||
                static_cast<std::uint64_t>(transport.frame_count) > clip.fifo_capacity - retained)
                return RealtimeStretchRenderCode::Backpressure;
        }
        return gap ? RealtimeStretchRenderCode::GapIdentityChanged
                   : RealtimeStretchRenderCode::Rendered;
    }
    for (const auto& clip : lane->clips) {
        const auto buffered = static_cast<std::uint64_t>(clip.processor.available_stretched());
        const auto effective_buffered = gap ? 0u : buffered;
        const auto available_capacity =
            static_cast<std::uint64_t>(clip.processor.output_free_space()) + buffered;
        const auto effective_free =
            gap ? available_capacity
                : static_cast<std::uint64_t>(clip.processor.output_free_space());
        const auto priming_reserve =
            effective_buffered < impl_->latency ? impl_->latency - effective_buffered : 0u;
        const auto required = static_cast<std::uint64_t>(transport.frame_count) + priming_reserve;
        if (required > effective_free)
            return RealtimeStretchRenderCode::Backpressure;
        const auto queued = static_cast<std::uint64_t>(clip.fifo_count) + buffered;
        if (queued > clip.fifo_capacity ||
            static_cast<std::uint64_t>(transport.frame_count) > clip.fifo_capacity - queued)
            return RealtimeStretchRenderCode::Backpressure;
    }
    return gap ? RealtimeStretchRenderCode::GapIdentityChanged
               : RealtimeStretchRenderCode::Rendered;
}

RealtimeStretchRenderCode RealtimeStretchProgramRuntime::process_track(
    const PlaybackProgram& program, const TrackProgram& track, const TrackMixerProgram& mixer,
    const TransportSnapshot& transport, audio::BufferView<float> output) noexcept {
    runtime::ScopedNoAlloc no_alloc;
    if (!impl_) {
        output.clear();
        return RealtimeStretchRenderCode::StateRequired;
    }
    auto* lane = impl_->find_track(track.id());
    const auto preflight = preflight_track(program, track, transport, output);
    if (preflight == RealtimeStretchRenderCode::NotRequired)
        return preflight;
    const bool gap_observation = preflight == RealtimeStretchRenderCode::GapIdentityChanged;
    if (preflight != RealtimeStretchRenderCode::Rendered && !gap_observation) {
        output.clear();
        return preflight;
    }

    const bool host_mapped_producer = transport.ranges[0].host_beat_mapping;
    if (lane->producer_initialized && lane->host_mapped_producer != host_mapped_producer) {
        if (lane->host_mapped_producer && !impl_->finalize_segment(*lane))
            return impl_->poison(*lane, output, RealtimeStretchRenderCode::Backpressure);
        for (auto& clip : lane->clips)
            clip.processor.reset();
        lane->has_identity = false;
        lane->pending_host_output = 0.0L;
        lane->analysis_host_output = 0.0L;
        lane->analysis_input_count = 0;
    }
    lane->producer_initialized = true;
    lane->host_mapped_producer = host_mapped_producer;

    if (host_mapped_producer) {
        for (auto& clip : lane->clips)
            if (!impl_->drain_processor(*lane, clip))
                return impl_->poison(*lane, output, RealtimeStretchRenderCode::Backpressure);
    }

    const auto elapsed_before = lane->elapsed_output_frames;
    for (std::uint8_t range_index = 0; range_index < transport.range_count; ++range_index) {
        const auto& range = transport.ranges[range_index];
        const auto range_start =
            host_mapped_producer
                ? host_mapped_document_sample_at_output_offset(range, *impl_->tempo_map, 0)
                : static_cast<long double>(range.timeline_sample_start.value);
        const auto range_end = host_mapped_producer
                                   ? host_mapped_document_sample_at_output_offset(
                                         range, *impl_->tempo_map, range.frame_count)
                                   : range_start + range.frame_count;
        const bool cut = range.discontinuity || !lane->has_identity ||
                         lane->playback_epoch != range.playback_epoch ||
                         lane->loop_pass_index != range.loop_pass_index ||
                         std::abs(range_start - lane->expected_document_start) > 1.0e-6L;
        if (cut) {
            if (host_mapped_producer && !impl_->finalize_segment(*lane))
                return impl_->poison(*lane, output, RealtimeStretchRenderCode::Backpressure);
            lane->playback_epoch = range.playback_epoch;
            lane->loop_pass_index = range.loop_pass_index;
            lane->has_identity = true;
            lane->expected_document_start = range_start;
            lane->next_document_input = range_start;
            lane->pending_host_output = 0.0L;
        }
        if (host_mapped_producer) {
            auto covered = range_start;
            while (has_meaningful_document_span(covered, range_end)) {
                const auto segment_end = std::min(range_end, lane->next_document_input + 1.0L);
                const auto contribution =
                    host_output_for_document_span(range, *impl_->tempo_map, covered, segment_end);
                lane->pending_host_output += contribution;
                covered = segment_end;
                if (has_meaningful_document_span(covered, lane->next_document_input + 1.0L))
                    continue;

                auto boundary = 0;
                for (const auto& clip : lane->clips) {
                    const auto clip_boundary = clip.processor.samples_until_next_analysis_frame();
                    if (clip_boundary <= 0)
                        return impl_->poison(*lane, output,
                                             RealtimeStretchRenderCode::StateRequired);
                    boundary = boundary == 0 ? clip_boundary : std::min(boundary, clip_boundary);
                }
                lane->analysis_host_output += lane->pending_host_output;
                ++lane->analysis_input_count;
                if (boundary == 1) {
                    const auto ratio =
                        static_cast<float>(lane->analysis_host_output / lane->analysis_input_count);
                    if (!std::isfinite(ratio) || ratio < 1.0f / impl_->maximum_ratio ||
                        ratio > impl_->maximum_ratio)
                        return impl_->poison(*lane, output,
                                             RealtimeStretchRenderCode::ImpossibleRatio);
                    for (auto& clip_lane : lane->clips)
                        clip_lane.processor.set_time_ratio(ratio);
                }
                for (auto& clip_lane : lane->clips) {
                    const auto channels = clip_lane.clip->audio->num_channels();
                    for (std::size_t channel = 0; channel < channels; ++channel) {
                        auto* scratch =
                            lane->input_scratch.data() + channel * impl_->maximum_block_frames;
                        lane->input_ptrs[channel] = scratch;
                        scratch[0] =
                            artifact_sample(*clip_lane.clip, channel, lane->next_document_input);
                    }
                    const auto status = clip_lane.processor.feed(lane->input_ptrs.data(), 1);
                    if (status != signal::PitchTimeStreamFeedStatus::accepted)
                        return impl_->poison(*lane, output,
                                             status ==
                                                     signal::PitchTimeStreamFeedStatus::backpressure
                                                 ? RealtimeStretchRenderCode::Backpressure
                                                 : RealtimeStretchRenderCode::StateRequired);
                }
                lane->next_document_input += 1.0L;
                lane->pending_host_output = 0.0L;
                if (boundary == 1) {
                    lane->analysis_host_output = 0.0L;
                    lane->analysis_input_count = 0;
                }
            }
        } else {
            for (auto& clip_lane : lane->clips) {
                impl_->configure_output(*lane, clip_lane);
                const auto channels = clip_lane.clip->audio->num_channels();
                for (std::uint32_t frame = 0; frame < range.frame_count; ++frame) {
                    const auto document_position =
                        static_cast<long double>(range.timeline_sample_start.value) + frame;
                    for (std::size_t channel = 0; channel < channels; ++channel)
                        lane->output_ptrs[channel][0] =
                            artifact_sample(*clip_lane.clip, channel, document_position);
                    if (!clip_lane.enqueue(lane->output_ptrs.data(), 1))
                        return impl_->poison(*lane, output,
                                             RealtimeStretchRenderCode::Backpressure);
                }
            }
        }
        lane->expected_document_start = range_end;
        lane->elapsed_output_frames += range.frame_count;
    }

    if (impl_->fail_after_mutation_for_test) {
        impl_->fail_after_mutation_for_test = false;
        return impl_->poison(*lane, output, RealtimeStretchRenderCode::Underflow);
    }

    if (host_mapped_producer) {
        for (auto& clip : lane->clips)
            if (!impl_->drain_processor(*lane, clip))
                return impl_->poison(*lane, output, RealtimeStretchRenderCode::Backpressure);
    }

    const auto elapsed_after = lane->elapsed_output_frames;
    const auto due_before = elapsed_before > impl_->latency ? elapsed_before - impl_->latency : 0u;
    const auto due_after = elapsed_after > impl_->latency ? elapsed_after - impl_->latency : 0u;
    const auto due = static_cast<std::uint32_t>(due_after - due_before);
    const auto due_offset = transport.frame_count - due;

    // Stage the non-Stretch path and its automation first. input_scratch is no
    // longer needed after all feeds complete, so it safely holds the delayed
    // per-sample mixer factors without another callback-time allocation.
    MixerRun mixer_run(mixer);
    const auto delay_length = std::max<std::uint32_t>(1, impl_->latency);
    for (std::uint32_t frame = 0; frame < transport.frame_count; ++frame) {
        const TransportRange* range = nullptr;
        for (std::uint8_t index = 0; index < transport.range_count; ++index) {
            const auto& candidate = transport.ranges[index];
            if (frame >= candidate.sample_offset &&
                frame < candidate.sample_offset + candidate.frame_count) {
                range = &candidate;
                break;
            }
        }
        if (range == nullptr)
            return impl_->poison(*lane, output, RealtimeStretchRenderCode::StateRequired);
        const auto local_frame = frame - range->sample_offset;
        const auto fraction =
            static_cast<long double>(local_frame) / static_cast<long double>(range->frame_count);
        const auto document_tick =
            range->host_beat_mapping
                ? tick_start(*range) + (tick_end(*range) - tick_start(*range)) * fraction
                : impl_->tempo_map->fractional_samples_to_ticks(
                      static_cast<long double>(range->timeline_sample_start.value) + local_frame);
        for (std::size_t channel = 0; channel < output.num_channels(); ++channel) {
            const auto delay_index = channel * delay_length + lane->delay_cursor;
            auto destination = output.channel(channel);
            const auto current_audio = destination[frame];
            destination[frame] = lane->audio_delay[delay_index];
            lane->audio_delay[delay_index] = current_audio;
            auto* factors = lane->input_scratch.data() + channel * impl_->maximum_block_frames;
            factors[frame] = lane->mixer_delay[delay_index];
            lane->mixer_delay[delay_index] =
                mixer_run.factor(document_tick, channel, output.num_channels());
        }
        lane->delay_cursor = (lane->delay_cursor + 1u) % delay_length;
    }

    for (auto& clip_lane : lane->clips) {
        if (clip_lane.fifo_count < due)
            return impl_->poison(*lane, output, RealtimeStretchRenderCode::Underflow);
        const auto clip_channels = clip_lane.clip->audio->num_channels();
        impl_->configure_output(*lane, clip_lane);
        if (!clip_lane.dequeue(lane->output_ptrs.data(), due))
            return impl_->poison(*lane, output, RealtimeStretchRenderCode::Underflow);
        clip_lane.dequeued_total += due;
        for (std::size_t channel = 0; channel < output.num_channels(); ++channel) {
            auto destination = output.channel(channel);
            for (std::uint32_t frame = 0; frame < due; ++frame) {
                float value = 0.0f;
                if (clip_channels == 1) {
                    value = lane->output_ptrs[0][frame];
                } else if (output.num_channels() == 1) {
                    for (std::size_t source = 0; source < clip_channels; ++source)
                        value += lane->output_ptrs[source][frame];
                    value /= static_cast<float>(clip_channels);
                } else if (channel < clip_channels) {
                    value = lane->output_ptrs[channel][frame];
                }
                destination[due_offset + frame] += value;
            }
        }
    }

    for (std::size_t channel = 0; channel < output.num_channels(); ++channel) {
        auto destination = output.channel(channel);
        const auto* factors = lane->input_scratch.data() + channel * impl_->maximum_block_frames;
        for (std::uint32_t frame = 0; frame < transport.frame_count; ++frame)
            destination[frame] *= factors[frame];
    }
    return gap_observation ? RealtimeStretchRenderCode::GapIdentityChanged
                           : RealtimeStretchRenderCode::Rendered;
}

std::uint32_t RealtimeStretchProgramRuntime::latency_samples() const noexcept {
    return impl_ ? impl_->latency : 0;
}
std::uint64_t RealtimeStretchProgramRuntime::reserved_runtime_bytes() const noexcept {
    return impl_ ? impl_->reserved_bytes : 0;
}
bool RealtimeStretchProgramRuntime::track_uses_realtime_stretch(
    timeline::ItemId track_id) const noexcept {
    if (!impl_)
        return false;
    const auto* track = impl_->find_track(track_id);
    return track != nullptr && !track->clips.empty();
}
void RealtimeStretchProgramRuntime::reset() noexcept {
    runtime::ScopedNoAlloc no_alloc;
    if (!impl_)
        return;
    for (auto& track : impl_->tracks)
        impl_->reset_track_state(track);
}

void RealtimeStretchProgramRuntime::force_post_mutation_failure_for_test() noexcept {
    if (impl_)
        impl_->fail_after_mutation_for_test = true;
}

void RealtimeStretchProgramRuntime::force_prepare_allocation_failure_for_test() noexcept {
    if (impl_)
        impl_->fail_prepare_allocation_for_test = true;
}

} // namespace pulp::playback
