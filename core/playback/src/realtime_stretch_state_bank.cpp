#include <pulp/playback/audio_renderer.hpp>
#include <pulp/playback/program.hpp>
#include <pulp/playback/realtime_stretch_state_bank.hpp>

#include <pulp/runtime/scoped_no_alloc.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace pulp::playback {
namespace {

RealtimeStretchStateBankAdmission reject(RealtimeStretchStateBankError code,
                                         timeline::ItemId clip_id = {}, std::uint64_t actual = 0,
                                         std::uint64_t limit = 0,
                                         audio::RealtimeTimeStretchPrepareStatus processor_status =
                                             audio::RealtimeTimeStretchPrepareStatus::prepared)
    noexcept {
    return {code, clip_id, actual, limit, 0, processor_status};
}

audio::RealtimeTimeStretchConfig processor_config(const RealtimeStretchStateSpec& spec,
                                                  std::uint32_t maximum_block_frames) noexcept {
    audio::RealtimeTimeStretchConfig config;
    config.quality = spec.quality;
    config.channels = static_cast<int>(spec.channels);
    config.max_block = static_cast<int>(maximum_block_frames);
    config.max_time_ratio = spec.max_time_ratio;
    return config;
}

} // namespace

RealtimeStretchStateBankAdmission
admit_realtime_stretch_state_bank(std::span<const RealtimeStretchStateSpec> specs,
                                  double sample_rate, std::uint32_t maximum_block_frames,
                                  const AudioRendererLimits& limits) noexcept {
    if (!std::isfinite(sample_rate) || sample_rate <= 0.0 || maximum_block_frames == 0 ||
        maximum_block_frames > limits.max_block_frames ||
        maximum_block_frames > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
        limits.max_realtime_stretch_allocation_bytes == 0)
        return reject(RealtimeStretchStateBankError::InvalidConfiguration);
    if (specs.size() > limits.max_realtime_stretch_states)
        return reject(RealtimeStretchStateBankError::StateLimitExceeded, {}, specs.size(),
                      limits.max_realtime_stretch_states);
    std::uint64_t retained_bytes = 0;
    for (std::size_t index = 0; index < specs.size(); ++index) {
        const auto& spec = specs[index];
        if (!spec.clip_id.valid())
            return reject(RealtimeStretchStateBankError::InvalidIdentity, spec.clip_id);
        for (std::size_t prior = 0; prior < index; ++prior)
            if (specs[prior].clip_id == spec.clip_id)
                return reject(RealtimeStretchStateBankError::DuplicateIdentity, spec.clip_id);
        if (spec.channels == 0 || spec.channels > limits.max_channels ||
            spec.channels > static_cast<std::uint32_t>(audio::kRealtimeTimeStretchMaximumChannels))
            return reject(
                RealtimeStretchStateBankError::ChannelLimitExceeded, spec.clip_id, spec.channels,
                std::min(limits.max_channels,
                         static_cast<std::uint32_t>(audio::kRealtimeTimeStretchMaximumChannels)));
        if (!std::isfinite(spec.max_time_ratio) || spec.max_time_ratio < 1.0f ||
            !std::isfinite(limits.realtime_stretch_max_time_ratio) ||
            limits.realtime_stretch_max_time_ratio < 1.0f ||
            spec.max_time_ratio > limits.realtime_stretch_max_time_ratio)
            return reject(RealtimeStretchStateBankError::TimeRatioLimitExceeded, spec.clip_id);

        audio::RealtimeTimeStretchPreparedGeometry geometry;
        const auto status = audio::checked_realtime_time_stretch_prepared_geometry(
            processor_config(spec, maximum_block_frames),
            limits.max_realtime_stretch_allocation_bytes, geometry);
        if (status != audio::RealtimeTimeStretchPrepareStatus::prepared)
            return reject(RealtimeStretchStateBankError::ProcessorPrepareRejected, spec.clip_id, 0,
                          limits.max_realtime_stretch_allocation_bytes, status);
        std::uint64_t next_retained_bytes = 0;
        if (geometry.retained_bytes > std::numeric_limits<std::uint64_t>::max() - retained_bytes)
            return reject(RealtimeStretchStateBankError::StateBytesExceeded, {},
                          std::numeric_limits<std::uint64_t>::max(),
                          limits.max_realtime_stretch_state_bytes);
        next_retained_bytes = retained_bytes + geometry.retained_bytes;
        retained_bytes = next_retained_bytes;
        if (retained_bytes > limits.max_realtime_stretch_state_bytes)
            return reject(RealtimeStretchStateBankError::StateBytesExceeded, {}, retained_bytes,
                          limits.max_realtime_stretch_state_bytes);
    }

    return {RealtimeStretchStateBankError::None,
            {},
            specs.size(),
            limits.max_realtime_stretch_states,
            retained_bytes,
            audio::RealtimeTimeStretchPrepareStatus::prepared};
}

RealtimeStretchStateBankAdmission
admit_realtime_stretch_program(const PlaybackProgram& program, double sample_rate,
                               std::uint32_t maximum_block_frames,
                               const AudioRendererLimits& limits) {
    std::vector<RealtimeStretchStateSpec> specs;
    for (const auto& track : program.tracks()) {
        if (const auto* audio = track->audio_program()) {
            for (const auto& clip : audio->clips()) {
                if (clip.source_time_mapping !=
                    AudioClipRendererProgram::SourceTimeMapping::OfflineStretchArtifact)
                    continue;
                specs.push_back({clip.id, static_cast<std::uint32_t>(clip.audio->num_channels()),
                                 audio::RealtimeTimeStretchQuality::low_latency,
                                 limits.realtime_stretch_max_time_ratio});
            }
        }
    }
    return admit_realtime_stretch_state_bank(specs, sample_rate, maximum_block_frames, limits);
}

struct RealtimeStretchStateBank::State {
    explicit State(RealtimeStretchStateSpec state_spec) noexcept : spec(state_spec) {}
    RealtimeStretchStateSpec spec;
    audio::RealtimeTimeStretchProcessor processor;
    std::uint64_t playback_epoch = 0;
    bool has_playback_epoch = false;
};

RealtimeStretchStateBank::RealtimeStretchStateBank() = default;
RealtimeStretchStateBank::~RealtimeStretchStateBank() = default;
RealtimeStretchStateBank::RealtimeStretchStateBank(RealtimeStretchStateBank&&) noexcept = default;
RealtimeStretchStateBank&
RealtimeStretchStateBank::operator=(RealtimeStretchStateBank&&) noexcept = default;

RealtimeStretchStateBankAdmission
RealtimeStretchStateBank::prepare(std::span<const RealtimeStretchStateSpec> specs,
                                  double sample_rate, std::uint32_t maximum_block_frames,
                                  const AudioRendererLimits& limits) {
    const auto admission =
        admit_realtime_stretch_state_bank(specs, sample_rate, maximum_block_frames, limits);
    if (!admission)
        return admission;

#if defined(__cpp_exceptions)
    try {
#endif
        std::vector<std::unique_ptr<State>> candidate;
        candidate.reserve(specs.size());
        for (const auto& spec : specs) {
            auto state = std::make_unique<State>(spec);
            const auto status =
                state->processor.prepare(sample_rate, processor_config(spec, maximum_block_frames),
                                         limits.max_realtime_stretch_allocation_bytes);
            if (status != audio::RealtimeTimeStretchPrepareStatus::prepared)
                return reject(RealtimeStretchStateBankError::ProcessorPrepareRejected, spec.clip_id,
                              0, limits.max_realtime_stretch_allocation_bytes, status);
            candidate.push_back(std::move(state));
        }
        states_.swap(candidate);
        reserved_state_bytes_ = admission.reserved_state_bytes;
        return admission;
#if defined(__cpp_exceptions)
    } catch (const std::bad_alloc&) {
        return reject(RealtimeStretchStateBankError::AllocationFailed);
    } catch (const std::length_error&) {
        return reject(RealtimeStretchStateBankError::AllocationFailed);
    }
#endif
}

audio::RealtimeTimeStretchProcessor*
RealtimeStretchStateBank::state_for_epoch(timeline::ItemId clip_id,
                                          std::uint64_t playback_epoch) noexcept {
    runtime::ScopedNoAlloc no_alloc;
    const auto found = std::find_if(states_.begin(), states_.end(), [clip_id](const auto& state) {
        return state->spec.clip_id == clip_id;
    });
    if (found == states_.end())
        return nullptr;
    auto& state = **found;
    if (!state.has_playback_epoch || state.playback_epoch != playback_epoch) {
        state.processor.reset();
        state.playback_epoch = playback_epoch;
        state.has_playback_epoch = true;
    }
    return &state.processor;
}

const audio::RealtimeTimeStretchProcessor*
RealtimeStretchStateBank::find(timeline::ItemId clip_id) const noexcept {
    runtime::ScopedNoAlloc no_alloc;
    const auto found = std::find_if(states_.begin(), states_.end(), [clip_id](const auto& state) {
        return state->spec.clip_id == clip_id;
    });
    return found == states_.end() ? nullptr : &(*found)->processor;
}

void RealtimeStretchStateBank::reset() noexcept {
    runtime::ScopedNoAlloc no_alloc;
    for (auto& state : states_) {
        state->processor.reset();
        state->playback_epoch = 0;
        state->has_playback_epoch = false;
    }
}

} // namespace pulp::playback
