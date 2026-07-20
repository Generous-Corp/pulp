#include "pulp_sampler.hpp"

namespace pulp::examples {

PulpSamplerProcessor::PulpSamplerProcessor(PulpSamplerConfig config) : config_(config) {}

PulpSamplerProcessor::~PulpSamplerProcessor() {
    release();
}

format::PluginDescriptor PulpSamplerProcessor::descriptor() const {
    return {
        .name = "PulpSampler",
        .manufacturer = "Pulp",
        .bundle_id = "com.pulp.sampler",
        .version = "1.0.0",
        .category = format::PluginCategory::Instrument,
        .input_buses = {},
        .output_buses = {{"Audio Out", 2}},
        .accepts_midi = true,
        .produces_midi = false,
        .tail_samples = -1,
    };
}

int PulpSamplerProcessor::latency_samples() const {
    std::lock_guard lock(control_mutex_);
    return heritage_.latency_samples();
}

void PulpSamplerProcessor::define_parameters(state::StateStore& store) {
    store.add_parameter(
        {.id = kSamplerGain, .name = "Gain", .unit = "dB", .range = {-60, 12, 0, 0.1f}});
    store.add_parameter(
        {.id = kSamplerAttack, .name = "Attack", .unit = "ms", .range = {0, 5000, 10, 1}});
    store.add_parameter(
        {.id = kSamplerDecay, .name = "Decay", .unit = "ms", .range = {0, 5000, 100, 1}});
    store.add_parameter(
        {.id = kSamplerSustain, .name = "Sustain", .unit = "%", .range = {0, 100, 80, 1}});
    store.add_parameter(
        {.id = kSamplerRelease, .name = "Release", .unit = "ms", .range = {0, 10000, 200, 1}});
    store.add_parameter(
        {.id = kSamplerPitch, .name = "Pitch", .unit = "st", .range = {-24, 24, 0, 1}});
    store.add_parameter({.id = kSamplerLoop, .name = "Loop", .unit = "", .range = {0, 1, 0, 1}});
    store.add_parameter(
        {.id = kSamplerReverse, .name = "Reverse", .unit = "", .range = {0, 1, 0, 1}});
    store.add_parameter(
        {.id = kSamplerInterpolation, .name = "Interpolation", .unit = "", .range = {0, 5, 2, 1}});
    store.add_parameter({.id = kSamplerHeritageClockRatio,
                         .name = "Heritage Clock Ratio",
                         .unit = "x",
                         .range = {0.25f, 4.0f, 1.0f, 0.01f}});
}

std::vector<std::uint8_t> PulpSamplerProcessor::serialize_plugin_state() const {
    std::lock_guard lock(control_mutex_);
    SamplerHeritagePersistentState state;
    state.enabled = heritage_.configured();
    if (!state.enabled)
        return {};
    state.profile = heritage_.configured_profile();

    if (callback_runtime_snapshot_available_.load(std::memory_order_acquire)) {
        const auto published = callback_runtime_snapshot_.read();
        state.has_runtime_state = published.valid;
        state.runtime_state = published.state;
    } else if (restored_runtime_state_available_) {
        state.has_runtime_state = true;
        state.runtime_state = restored_runtime_state_;
    } else if (pending_runtime_state_available_) {
        state.has_runtime_state = true;
        state.runtime_state = pending_runtime_state_;
    }
    auto written = write_sampler_heritage_state(state);
    if (!written.valid() && state.has_runtime_state) {
        // A bad continuation snapshot must not make the strict outer state
        // envelope silently look like "heritage disabled". Preserve the
        // validated profile and reset only the optional RNG continuation.
        state.has_runtime_state = false;
        written = write_sampler_heritage_state(state);
    }
    return written.valid() ? written.bytes : std::vector<std::uint8_t>{};
}

bool PulpSamplerProcessor::deserialize_plugin_state(std::span<const std::uint8_t> bytes) {
    std::lock_guard lock(control_mutex_);
    const auto parsed = parse_sampler_heritage_state(bytes);
    if (!parsed.valid())
        return false;

    if (!parsed.state.enabled) {
        return disable_heritage() == PulpSamplerHeritageStatus::Disabled;
    }

    const auto* runtime_state =
        parsed.state.has_runtime_state ? &parsed.state.runtime_state : nullptr;
    if (prepared_) {
        SamplerHeritageRuntime::PreparedReplacement candidate;
        const auto result = heritage_.stage_replacement(
            parsed.state.profile, host_sample_rate_, prepared_output_channels_, max_block_frames_,
            SamplerStreamingRuntime::maximum_pitch_ratio(), runtime_state,
            candidate);
        if (!heritage_ready(result))
            return false;
        if (!apply_heritage_replacement(std::move(candidate), result))
            return false;
        if (result == PulpSamplerHeritageStatus::Ready && runtime_state != nullptr) {
            restored_runtime_state_ = *runtime_state;
            restored_runtime_state_available_ = true;
        }
        return true;
    }

    const auto configured = heritage_.configure(parsed.state.profile);
    if (configured != PulpSamplerHeritageStatus::PendingPrepare)
        return false;
    reset_all_voices();
    clear_heritage_runtime_persistence();
    if (runtime_state != nullptr) {
        pending_runtime_state_ = *runtime_state;
        pending_runtime_state_available_ = true;
        restored_runtime_state_ = *runtime_state;
        restored_runtime_state_available_ = true;
    }
    return true;
}

bool PulpSamplerProcessor::load_sample(const float* data, int num_samples, float sample_rate) {
    std::lock_guard lock(control_mutex_);
    return source_publication_.load_mono(*streaming_, data, num_samples, sample_rate);
}

bool PulpSamplerProcessor::load_sample_stereo(const float* interleaved, int num_frames,
                                              float sample_rate) {
    std::lock_guard lock(control_mutex_);
    return source_publication_.load_stereo(*streaming_, interleaved, num_frames, sample_rate);
}

PulpSamplerLoadResult PulpSamplerProcessor::load_sample_file_result(std::string_view path) {
#if defined(PULP_SAMPLER_TEST_HOOKS)
    control_load_attempts_for_test_.fetch_add(1, std::memory_order_release);
#endif
    std::lock_guard lock(control_mutex_);
#if defined(PULP_SAMPLER_TEST_HOOKS)
    control_load_entries_for_test_.fetch_add(1, std::memory_order_release);
#endif
    auto result = streaming_->load_sample_file_result(path);
    if (result.loaded())
        result.selection_generation = streaming_->published_source().selection_generation;
    last_load_result_.write(result);
    return result;
}

bool PulpSamplerProcessor::load_sample_file(std::string_view path) {
    return load_sample_file_result(path).loaded();
}

PulpSamplerLoadResult PulpSamplerProcessor::last_load_result() const noexcept {
    return last_load_result_.read();
}

bool PulpSamplerProcessor::set_config(PulpSamplerConfig config) {
    std::lock_guard lock(control_mutex_);
    if (prepared_)
        return false;
    config_ = config;
    return true;
}

PulpSamplerConfig PulpSamplerProcessor::config() const {
    std::lock_guard lock(control_mutex_);
    return config_;
}

PulpSamplerPrepareResult PulpSamplerProcessor::prepare_result() const noexcept {
    return prepare_result_.read();
}

bool PulpSamplerProcessor::has_sample() const {
    std::lock_guard lock(control_mutex_);
    return streaming_->published_source().kind != SamplerPublishedSourceKind::None;
}

int PulpSamplerProcessor::sample_length() const {
    std::lock_guard lock(control_mutex_);
    const auto source = streaming_->published_source();
    const auto frames = source.kind == SamplerPublishedSourceKind::Streamed
                            ? source.streamed.total_frames
                            : source.resident.num_frames;
    return frames > static_cast<std::uint64_t>(std::numeric_limits<int>::max())
               ? std::numeric_limits<int>::max()
               : static_cast<int>(frames);
}

PulpSamplerStreamStats PulpSamplerProcessor::stream_stats() const {
    std::lock_guard lock(control_mutex_);
    return streaming_->stats();
}

PulpSamplerHeritageStatus
PulpSamplerProcessor::set_heritage_profile(const audio::SampleHeritageProfile& profile) {
#if defined(PULP_SAMPLER_TEST_HOOKS)
    control_heritage_attempts_for_test_.fetch_add(1, std::memory_order_release);
#endif
    std::lock_guard lock(control_mutex_);
#if defined(PULP_SAMPLER_TEST_HOOKS)
    control_heritage_entries_for_test_.fetch_add(1, std::memory_order_release);
#endif
    if (prepared_) {
        SamplerHeritageRuntime::PreparedReplacement candidate;
        const auto result =
            heritage_.stage_replacement(profile, host_sample_rate_, prepared_output_channels_,
                                        max_block_frames_,
                                        SamplerStreamingRuntime::maximum_pitch_ratio(),
                                        nullptr, candidate);
        if (!heritage_ready(result)) {
            return result;
        }
        if (!apply_heritage_replacement(std::move(candidate), result))
            return PulpSamplerHeritageStatus::PrepareFailed;
        return result;
    }

    const auto configured = heritage_.configure(profile);
    if (configured != PulpSamplerHeritageStatus::PendingPrepare) {
        return configured;
    }
    reset_all_voices();
    clear_heritage_runtime_persistence();
    return configured;
}

PulpSamplerHeritageStatus PulpSamplerProcessor::disable_heritage() {
    std::lock_guard lock(control_mutex_);
    const auto previous_latency = latency_samples();
    if (prepared_ && !rebind_stream_domain({host_sample_rate_, max_block_frames_})) {
        return PulpSamplerHeritageStatus::PrepareFailed;
    }
    reset_all_voices();
    heritage_.disable();
    clear_heritage_runtime_persistence();
    if (latency_samples() != previous_latency)
        flag_latency_changed();
    return PulpSamplerHeritageStatus::Disabled;
}

PulpSamplerHeritageDiagnostics PulpSamplerProcessor::heritage_diagnostics() const {
    std::lock_guard lock(control_mutex_);
    return heritage_.diagnostics();
}

PulpSamplerDiagnostics PulpSamplerProcessor::diagnostics() const noexcept {
#if defined(PULP_SAMPLER_TEST_HOOKS)
    control_diagnostics_attempts_for_test_.fetch_add(1, std::memory_order_release);
#endif
    try {
        std::lock_guard lock(control_mutex_);
#if defined(PULP_SAMPLER_TEST_HOOKS)
        control_diagnostics_entries_for_test_.fetch_add(1, std::memory_order_release);
#endif
        const auto stream = streaming_->stats();
        PulpSamplerDiagnostics result{
            .snapshot_epoch = next_diagnostics_epoch_locked(),
            .prepare = prepare_result_.read(),
            .last_load = last_load_result_.read(),
            .preload = streaming_->preload_policy(),
            .heritage = heritage_.diagnostics(),
            .streaming_memory_capacity_bytes = stream.total_memory_capacity_bytes,
            .current_streaming_memory_bytes = stream.current_total_memory_bytes,
            .peak_streaming_memory_bytes = stream.peak_total_memory_bytes,
        };
        if (envelope_snapshot_available_.load(std::memory_order_acquire))
            result.envelope = envelope_snapshot_.read();
        if (interpolation_snapshot_available_.load(std::memory_order_acquire))
            result.interpolation = interpolation_snapshot_.read();
        diagnostics_snapshot_.write(result);
        return diagnostics_snapshot_.read();
    } catch (...) {
        return diagnostics_snapshot_.read();
    }
}

format::PrepareResourceUsage
PulpSamplerProcessor::estimate_prepare_resources(const format::PrepareContext& ctx) const {
    std::lock_guard lock(control_mutex_);
    format::PrepareResourceUsage usage;
    usage.block_size = std::max(0, ctx.max_buffer_size);
    usage.input_channels = std::max(0, ctx.input_channels);
    usage.output_channels = std::max(0, ctx.output_channels);
    usage.voices = kMaxVoices;
    const auto narrowed_rate = static_cast<float>(ctx.sample_rate);
    if (!std::isfinite(ctx.sample_rate) || ctx.sample_rate <= 0.0 ||
        !std::isfinite(narrowed_rate) || narrowed_rate <= 0.0f || ctx.max_buffer_size <= 0 ||
        ctx.output_channels <= 0 || ctx.output_channels > static_cast<int>(kMaxOutputChannels))
        return usage;
    auto add = [](std::size_t left, std::size_t right) noexcept {
        return right > std::numeric_limits<std::size_t>::max() - left
                   ? std::numeric_limits<std::size_t>::max()
                   : left + right;
    };
    auto multiply = [](std::size_t left, std::size_t right) noexcept {
        return left != 0 && right > std::numeric_limits<std::size_t>::max() / left
                   ? std::numeric_limits<std::size_t>::max()
                   : left * right;
    };
    auto sinc_bytes = [&](double maximum_consumption) noexcept {
        if (!(maximum_consumption > 0.0) || !std::isfinite(maximum_consumption) ||
            maximum_consumption > audio::kMaximumDenseSampleSincConsumption)
            return std::numeric_limits<std::size_t>::max();
        if (maximum_consumption < audio::kMaximumDenseSampleSincConsumption)
            maximum_consumption = std::nextafter(
                maximum_consumption,
                std::numeric_limits<double>::infinity());
        const auto intervals =
            maximum_consumption <= 1.0 ? 0.0 : std::ceil(std::log2(maximum_consumption) * 4.0);
        if (intervals >= 32.0)
            return std::numeric_limits<std::size_t>::max();
        return multiply(static_cast<std::size_t>(intervals) + 1,
                        (512u + 1u) * (2u * 24u) * sizeof(float));
    };

    double clock_ratio = 1.0;
    double machine_rate = ctx.sample_rate;
    bool heritage_processing = false;
    const bool heritage_configured = heritage_.configured();
    bool typed_profile = false;
    bool voice_processing = false;
    bool bus_processing = false;
    bool pitch_active = false;
    bool clock_automation_active = false;
    double max_transpose_semitones = 24.0;
    bool live_cyclic_active = false;
    double live_factor = 1.0;
    double live_cycle_ms = 0.0;
    double live_splice_ms = 0.0;
    std::size_t live_shuffle_divisions = 1;
    bool dynamic_identity_voice = true;
    audio::SampleHeritagePitchFamily pitch_family =
        audio::SampleHeritagePitchFamily::VariableClock;
    if (heritage_configured) {
        const auto profile = heritage_.configured_profile();
        typed_profile = !profile.voice.empty() || !profile.bus.empty() ||
                        !profile.record_commit.empty();
        for (const auto& stage : profile.stages) {
            if (stage.bypass)
                continue;
            heritage_processing = true;
            if (const auto* machine =
                    std::get_if<audio::SampleHeritageMachineDomainStage>(&stage.parameters))
                machine_rate = machine->sample_rate;
            if (const auto* clock =
                    std::get_if<audio::SampleHeritageClockPitchStage>(&stage.parameters))
                clock_ratio = clock->ratio;
        }
        for (const auto& block : profile.voice) {
            if (block.bypass) continue;
            heritage_processing = true;
            voice_processing = true;
            if (const auto* machine =
                    std::get_if<audio::SampleHeritageVoiceMachineDomainBlock>(
                        &block.parameters)) {
                machine_rate = machine->sample_rate;
                dynamic_identity_voice = dynamic_identity_voice &&
                    machine->sample_rate == ctx.sample_rate;
            } else if (const auto* clock =
                    std::get_if<audio::SampleHeritageVoiceClockBlock>(
                        &block.parameters)) {
                clock_ratio = clock->ratio;
                clock_automation_active = true;
                dynamic_identity_voice = dynamic_identity_voice &&
                    clock->ratio == 1.0;
            } else if (const auto* pitch =
                    std::get_if<audio::SampleHeritageVoicePitchBlock>(
                        &block.parameters)) {
                pitch_active = true;
                pitch_family = pitch->family;
                max_transpose_semitones = pitch->max_transpose_semitones;
                dynamic_identity_voice = dynamic_identity_voice &&
                    pitch->family ==
                        audio::SampleHeritagePitchFamily::VariableClock;
            } else if (const auto* live =
                    std::get_if<audio::SampleHeritageVoiceLiveCyclicStretchBlock>(
                        &block.parameters)) {
                live_cyclic_active = true;
                live_factor = live->factor;
                live_cycle_ms = live->cycle_ms;
                live_splice_ms = live->splice_ms;
                live_shuffle_divisions = live->shuffle_divisions == 0
                    ? 1u
                    : live->shuffle_divisions;
                dynamic_identity_voice = false;
            } else {
                dynamic_identity_voice = false;
            }
        }
        bus_processing = std::any_of(
            profile.bus.begin(), profile.bus.end(),
            [](const auto& block) { return !block.bypass; });
        heritage_processing = heritage_processing || bus_processing;
    }
    const auto stream_rate = ctx.sample_rate;
    const auto input_ratio = ctx.sample_rate / (machine_rate * clock_ratio);
    const auto return_ratio = machine_rate * clock_ratio / ctx.sample_rate;
    const auto maximum_profile_pitch_factor = std::min(
        audio::SampleHeritagePitchProcessor::kMaximumFactor,
        std::exp2(max_transpose_semitones / 12.0));
    const auto requested_runtime_clock_factor =
        (pitch_active &&
                 pitch_family == audio::SampleHeritagePitchFamily::VariableClock
             ? maximum_profile_pitch_factor
             : 1.0) *
        (clock_automation_active
             ? SamplerHeritageRuntime::kMaximumClockMultiplier
             : 1.0);
    const auto maximum_runtime_clock_factor =
        SamplerHeritageRuntime::bounded_runtime_clock_factor(
            machine_rate, clock_ratio, ctx.sample_rate,
            requested_runtime_clock_factor);
    if (!(maximum_runtime_clock_factor >= 1.0)) return usage;
    const auto maximum_note_pitch_factor =
        pitch_active &&
                pitch_family == audio::SampleHeritagePitchFamily::VariableClock
            ? std::min(maximum_profile_pitch_factor,
                       maximum_runtime_clock_factor)
            : 1.0;
    const auto maximum_artifact_clock_factor = std::min(
        clock_automation_active
            ? SamplerHeritageRuntime::kMaximumClockMultiplier
            : 1.0,
        maximum_runtime_clock_factor / maximum_note_pitch_factor);
    auto frame_bound = [](std::size_t frames, double ratio,
                          bool identity) noexcept {
        if (identity) return frames;
        const auto value = std::ceil(static_cast<double>(frames) * ratio);
        if (!std::isfinite(value) || value < 0.0 ||
            value >= static_cast<double>(std::numeric_limits<std::size_t>::max()))
            return std::numeric_limits<std::size_t>::max();
        return static_cast<std::size_t>(value) + 1;
    };
    auto fixed_pitch_bound = [](std::size_t frames, double factor,
                                bool linear) noexcept {
        if (frames == 0) return std::size_t{0};
        const auto last = static_cast<double>(frames - 1) * factor;
        const auto highest = linear ? std::ceil(last) : std::floor(last);
        if (!std::isfinite(highest) || highest < 0.0 ||
            highest >= static_cast<double>(std::numeric_limits<std::size_t>::max()))
            return std::numeric_limits<std::size_t>::max();
        return static_cast<std::size_t>(highest) + 1;
    };
    const auto block_frames = static_cast<std::size_t>(ctx.max_buffer_size);
    const auto maximum_machine_frames = heritage_processing
        ? frame_bound(block_frames,
                      return_ratio * maximum_runtime_clock_factor,
                      return_ratio == 1.0 &&
                          maximum_runtime_clock_factor == 1.0)
        : block_frames;
    auto maximum_pre_live_machine_frames = maximum_machine_frames;
    audio::SampleHeritageLiveCyclicResources live_resources;
    std::size_t live_cycle_frames = 0;
    std::size_t live_splice_frames = 0;
    if (live_cyclic_active) {
        const auto rounded_frames = [&](double milliseconds,
                                        bool allow_zero) noexcept {
            const auto value =
                std::floor(milliseconds * machine_rate * 0.001 + 0.5);
            if (!std::isfinite(value) || value < (allow_zero ? 0.0 : 1.0) ||
                value >= static_cast<double>(
                    std::numeric_limits<std::size_t>::max()))
                return std::numeric_limits<std::size_t>::max();
            return static_cast<std::size_t>(value);
        };
        live_cycle_frames = rounded_frames(live_cycle_ms, false);
        live_splice_frames = rounded_frames(live_splice_ms, true);
        if (live_cycle_frames == std::numeric_limits<std::size_t>::max() ||
            live_splice_frames == std::numeric_limits<std::size_t>::max())
            return usage;
        const audio::SampleHeritageLiveCyclicConfig config{
            .factor = live_factor,
            .cycle_samples = live_cycle_frames,
            .crossfade_samples = live_splice_frames,
            .shuffle_divisions = live_shuffle_divisions,
            .linked_channels = true,
            .seed = 1,
            .shuffle = live_shuffle_divisions == 1
                ? audio::SampleHeritageLiveCyclicShuffle::Identity
                : audio::SampleHeritageLiveCyclicShuffle::FisherYates,
            .max_block_samples = maximum_machine_frames,
            .channel_count = static_cast<std::size_t>(ctx.output_channels)};
        live_resources =
            audio::SampleHeritageLiveCyclicStretch::resources_for(config);
        if (!live_resources.valid()) return usage;
        maximum_pre_live_machine_frames = live_resources.maximum_input_frames;
    }
    const auto maximum_input_ratio =
        input_ratio / maximum_artifact_clock_factor;
    const auto maximum_engine_input_frames = heritage_processing
        ? frame_bound(maximum_pre_live_machine_frames, maximum_input_ratio,
                      maximum_input_ratio == 1.0)
        : block_frames;
    auto maximum_heritage_input_frames = maximum_engine_input_frames;
    if (pitch_active &&
        pitch_family != audio::SampleHeritagePitchFamily::VariableClock) {
        maximum_heritage_input_frames = fixed_pitch_bound(
            maximum_engine_input_frames,
            maximum_profile_pitch_factor,
            pitch_family == audio::SampleHeritagePitchFamily::EarlyLinear);
    }
    const auto admitted_stream_runtime_clock_factor = std::min(
        maximum_runtime_clock_factor,
        SamplerStreamingRuntime::maximum_pitch_ratio() /
            (live_cyclic_active ? 1.0 / live_factor : 1.0));
    auto maximum_stream_input_frames = maximum_engine_input_frames;
    if ((pitch_active &&
         pitch_family == audio::SampleHeritagePitchFamily::VariableClock) ||
        (!pitch_active && clock_automation_active)) {
        const auto stream_machine_frames = frame_bound(
            block_frames,
            return_ratio * admitted_stream_runtime_clock_factor, false);
        auto stream_pre_live_frames = stream_machine_frames;
        if (live_cyclic_active) {
            const audio::SampleHeritageLiveCyclicConfig stream_live_config{
                .factor = live_factor,
                .cycle_samples = live_cycle_frames,
                .crossfade_samples = live_splice_frames,
                .shuffle_divisions = live_shuffle_divisions,
                .linked_channels = true,
                .seed = 1,
                .shuffle = live_shuffle_divisions == 1
                    ? audio::SampleHeritageLiveCyclicShuffle::Identity
                    : audio::SampleHeritageLiveCyclicShuffle::FisherYates,
                .max_block_samples = stream_machine_frames,
                .channel_count = static_cast<std::size_t>(ctx.output_channels)};
            const auto stream_live_resources =
                audio::SampleHeritageLiveCyclicStretch::resources_for(
                    stream_live_config);
            if (!stream_live_resources.valid()) return usage;
            stream_pre_live_frames =
                stream_live_resources.maximum_input_frames;
        }
        maximum_stream_input_frames = frame_bound(
            stream_pre_live_frames, maximum_input_ratio,
            maximum_input_ratio == 1.0);
    } else if (pitch_active) {
        maximum_stream_input_frames = clock_automation_active
            ? maximum_heritage_input_frames
            : fixed_pitch_bound(
                  maximum_engine_input_frames, maximum_profile_pitch_factor,
                  pitch_family == audio::SampleHeritagePitchFamily::EarlyLinear);
    }
    const auto stream_frames_double =
        static_cast<double>(maximum_stream_input_frames);
    if (!std::isfinite(stream_rate) || stream_rate <= 0.0 ||
        stream_rate > std::numeric_limits<float>::max() || !std::isfinite(stream_frames_double) ||
        stream_frames_double <= 0.0 ||
        stream_frames_double > std::numeric_limits<std::uint32_t>::max())
        return usage;
    const auto streaming = SamplerStreamingRuntime::estimate_prepare(
        static_cast<float>(stream_rate), static_cast<std::uint32_t>(stream_frames_double),
        config_.streaming_memory_budget_bytes);
    auto persistent =
        streaming.configured_streaming_memory_bytes > std::numeric_limits<std::size_t>::max()
            ? std::numeric_limits<std::size_t>::max()
            : static_cast<std::size_t>(streaming.configured_streaming_memory_bytes);
    // Two 2-channel, 60-second-at-48k resident slots.
    persistent = add(persistent, 46'080'000u);
    // The fixed 140 dB/0.025 transition design is 369 doubles.
    persistent = add(persistent, 369u * sizeof(double));
    persistent = add(persistent, sinc_bytes(SamplerStreamingRuntime::maximum_pitch_ratio() *
                                            SamplerStreamingRuntime::maximum_source_sample_rate() /
                                            stream_rate));
    // Stream slots, decode/service records, queues, and worker metadata.
    // This conservative bound is separate from the caller's audio-storage cap.
    persistent = add(persistent, sizeof(SamplerStreamingRuntime));
    persistent = add(persistent, 1u << 20);
    if (heritage_configured)
        persistent = add(persistent,
                         SamplerHeritageRuntime::prepared_object_bytes());
    if (heritage_processing) {
        const auto channels = static_cast<std::size_t>(ctx.output_channels);
        const auto voice_engine_count =
            typed_profile && voice_processing
                ? SamplerHeritageRuntime::kVoiceSlots
                : std::size_t{0};
        const auto legacy_engine_count = typed_profile ? std::size_t{0}
                                                       : std::size_t{1};
        const auto maximum_consumption = std::max(
            input_ratio, return_ratio * maximum_runtime_clock_factor);
        if (input_ratio != 1.0 || return_ratio != 1.0 ||
            maximum_runtime_clock_factor > 1.0)
            persistent = add(persistent, sinc_bytes(maximum_consumption));

        auto engine_dynamic_bytes = [&](std::size_t machine_frames,
                                        double runtime_factor,
                                        double engine_input_ratio,
                                        double engine_return_ratio,
                                        bool dynamic_identity) noexcept {
            auto bytes = multiply(multiply(channels, machine_frames),
                                  sizeof(float));
            std::size_t history_frames =
                engine_input_ratio == 1.0 ? 0u : 48u;
            if (runtime_factor > 1.0) {
                const auto minimum_return_ratio =
                    engine_return_ratio / runtime_factor;
                const auto fixed_latency = std::ceil(
                    static_cast<double>(audio::kHighQualitySampleSincHalfWidth) /
                    minimum_return_ratio);
                const auto variable_history = std::ceil(
                    fixed_latency * engine_return_ratio * runtime_factor) + 50.0;
                if (!std::isfinite(variable_history) || variable_history < 0.0 ||
                    variable_history >= static_cast<double>(
                        std::numeric_limits<std::size_t>::max()))
                    return std::numeric_limits<std::size_t>::max();
                history_frames = add(
                    history_frames,
                    static_cast<std::size_t>(variable_history));
                if (dynamic_identity) {
                    const auto identity_delay = static_cast<std::size_t>(
                        std::ceil(static_cast<double>(
                                      audio::kHighQualitySampleSincHalfWidth) /
                                  minimum_return_ratio));
                    history_frames = add(history_frames, identity_delay);
                }
            } else if (engine_return_ratio != 1.0) {
                history_frames = add(history_frames, 48u);
            }
            return add(bytes, multiply(multiply(channels, history_frames),
                                       sizeof(float)));
        };
        const auto voice_engine_bytes = engine_dynamic_bytes(
            maximum_machine_frames, maximum_runtime_clock_factor,
            input_ratio, return_ratio, dynamic_identity_voice);
        auto live_engine_bytes = std::size_t{0};
        if (live_cyclic_active) {
            live_engine_bytes = add(
                live_resources.persistent_bytes,
                multiply(multiply(channels,
                                  maximum_pre_live_machine_frames),
                         sizeof(float)));
        }
        persistent = add(
            persistent,
            multiply(voice_engine_count,
                     add(voice_engine_bytes, live_engine_bytes)));
        if (legacy_engine_count != 0)
            persistent = add(
                persistent,
                engine_dynamic_bytes(maximum_machine_frames, 1.0,
                                     input_ratio, return_ratio, false));

        persistent = add(
            persistent,
            multiply(multiply(channels,
                              add(maximum_heritage_input_frames,
                                  maximum_engine_input_frames)),
                     sizeof(float)));
        if (pitch_active &&
            pitch_family != audio::SampleHeritagePitchFamily::VariableClock) {
            persistent = add(
                persistent,
                multiply(multiply(voice_engine_count, channels),
                         2u * sizeof(float)));
        }
    }
    usage.persistent_bytes = persistent;
    const auto scratch_frames = block_frames;
    const auto source_scratch_frames = heritage_processing
        ? std::max(scratch_frames, maximum_heritage_input_frames)
        : scratch_frames;
    if (scratch_frames <=
            std::numeric_limits<std::size_t>::max() /
                (kMaxOutputChannels * sizeof(float)) &&
        source_scratch_frames <=
            std::numeric_limits<std::size_t>::max() /
                (kMaxSampleChannels * sizeof(float))) {
        usage.block_scratch_bytes = add(
            scratch_frames * kMaxOutputChannels * sizeof(float),
            source_scratch_frames * kMaxSampleChannels * sizeof(float));
        usage.block_scratch_bytes = add(usage.block_scratch_bytes,
                                        scratch_frames * sizeof(std::uint8_t));
    } else {
        usage.block_scratch_bytes = std::numeric_limits<std::size_t>::max();
    }
    return usage;
}

void PulpSamplerProcessor::prepare(const format::PrepareContext& ctx) {
    std::lock_guard lock(control_mutex_);
    try {
        prepare_transaction(ctx);
    } catch (...) {
        fail_prepare({.status = PulpSamplerPrepareStatus::AllocationFailure});
    }
}

void PulpSamplerProcessor::prepare_transaction(const format::PrepareContext& ctx) {
    const auto previous_latency = latency_samples();
    release();
    last_load_result_.write({});
    envelope_lifetime_ = {};
    envelope_snapshot_available_.store(false, std::memory_order_release);
    sinc_fallback_selections_ = 0;
    resident_mip_suppressions_ = 0;
    streamed_mip_suppressions_ = 0;
    sinc_promotion_suppressions_ = 0;
    interpolation_snapshot_available_.store(false, std::memory_order_release);
    const auto narrowed_rate = static_cast<float>(ctx.sample_rate);
    if (!std::isfinite(ctx.sample_rate) || ctx.sample_rate <= 0.0 ||
        !std::isfinite(narrowed_rate) || narrowed_rate <= 0.0f || ctx.max_buffer_size <= 0 ||
        ctx.output_channels <= 0 || ctx.output_channels > static_cast<int>(kMaxOutputChannels) ||
        ctx.input_channels < 0) {
        prepare_result_.write({.status = PulpSamplerPrepareStatus::InvalidHostConfiguration});
        return;
    }
    if (const auto exceeded = check_prepare_resource_limits(ctx);
        exceeded != format::PrepareResourceLimit::None) {
        prepare_result_.write({.status = PulpSamplerPrepareStatus::ResourceLimitExceeded,
                               .exceeded_limit = exceeded});
        return;
    }
    host_sample_rate_ = narrowed_rate;
    max_block_frames_ = static_cast<std::uint32_t>(ctx.max_buffer_size);
    prepared_output_channels_ = static_cast<std::uint32_t>(ctx.output_channels);

    try {
        for (std::uint32_t ch = 0; ch < kMaxOutputChannels; ++ch)
            voice_scratch_[ch].assign(max_block_frames_, 0.0f);
        bus_voice_activity_.assign(max_block_frames_, 0);
    } catch (...) {
        fail_prepare({.status = PulpSamplerPrepareStatus::AllocationFailure});
        return;
    }
    if (!source_publication_.prepare()) {
        fail_prepare({.status = PulpSamplerPrepareStatus::AllocationFailure});
        return;
    }
    callback_runtime_snapshot_available_.store(false, std::memory_order_release);
    restored_runtime_state_available_ = false;
    if (heritage_.configured()) {
        const auto* runtime_state =
            pending_runtime_state_available_ ? &pending_runtime_state_ : nullptr;
        const auto result =
            heritage_.prepare(host_sample_rate_, prepared_output_channels_, max_block_frames_,
                              SamplerStreamingRuntime::maximum_pitch_ratio(),
                              runtime_state);
        if (!heritage_ready(result)) {
            fail_prepare({.status = PulpSamplerPrepareStatus::HeritagePrepareFailed});
            return;
        } else {
            if (result == PulpSamplerHeritageStatus::Ready && runtime_state != nullptr) {
                restored_runtime_state_ = *runtime_state;
                restored_runtime_state_available_ = true;
            }
        }
    }
    try {
        const auto source_scratch_frames = std::max(
            static_cast<std::size_t>(max_block_frames_),
            heritage_.maximum_input_frames());
        for (std::uint32_t channel = 0; channel < kMaxSampleChannels; ++channel) {
            stream_source_scratch_[channel].assign(source_scratch_frames, 0.0f);
            stream_source_scratch_ptrs_[channel] =
                stream_source_scratch_[channel].data();
        }
    } catch (...) {
        fail_prepare({.status = PulpSamplerPrepareStatus::AllocationFailure});
        return;
    }
    const auto stream_result = prepare_stream_domain();
    if (!stream_result.prepared()) {
        fail_prepare(stream_result);
        return;
    }
    for (auto& voice : voices_)
        reset_voice(voice);
    for (auto& generation : requester_generations_)
        generation = 0;
    for (auto& cancellation : pending_cancellations_)
        cancellation = {};
#if defined(PULP_SAMPLER_TEST_HOOKS)
    retire_reverse_attack_after_horizon_for_test_ = false;
    stream_rate_capacity_override_for_test_ = 0.0;
    last_stream_demand_fps_for_test_ = 0.0;
    last_lookahead_demand_fps_for_test_ = 0.0;
#endif
    source_publication_.acknowledge_audio(streaming_->published_source(), voices_, *streaming_);
    prepared_ = true;
    prepare_result_.write(stream_result);
    pending_runtime_state_available_ = false;
    if (latency_samples() != previous_latency)
        flag_latency_changed();
}

void PulpSamplerProcessor::release() {
#if defined(PULP_SAMPLER_TEST_HOOKS)
    control_release_attempts_for_test_.fetch_add(1, std::memory_order_release);
#endif
    std::lock_guard lock(control_mutex_);
#if defined(PULP_SAMPLER_TEST_HOOKS)
    control_release_entries_for_test_.fetch_add(1, std::memory_order_release);
#endif
    for (auto& voice : voices_)
        reset_voice(voice);
    for (auto& cancellation : pending_cancellations_)
        cancellation = {};
    streaming_->release();
    source_publication_.release();
    sinc_bank_->release();
    heritage_.release_processing();
    for (auto& scratch : voice_scratch_)
        std::vector<float>().swap(scratch);
    voice_scratch_ptrs_.fill(nullptr);
    for (auto& scratch : stream_source_scratch_)
        std::vector<float>().swap(scratch);
    stream_source_scratch_ptrs_.fill(nullptr);
    std::vector<std::uint8_t>().swap(bus_voice_activity_);
    prepared_ = false;
    prepare_result_.write({});
    last_load_result_.write({});
    envelope_snapshot_available_.store(false, std::memory_order_release);
    interpolation_snapshot_available_.store(false, std::memory_order_release);
}

bool PulpSamplerProcessor::heritage_ready(PulpSamplerHeritageStatus status) noexcept {
    return status == PulpSamplerHeritageStatus::Ready ||
           status == PulpSamplerHeritageStatus::ReadyRuntimeResetForHostRate;
}

std::uint64_t PulpSamplerProcessor::next_diagnostics_epoch_locked() const noexcept {
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    diagnostics_snapshot_epoch_ =
        diagnostics_snapshot_epoch_ > maximum - 2 ? 2 : diagnostics_snapshot_epoch_ + 2;
    return diagnostics_snapshot_epoch_;
}

PulpSamplerProcessor::StreamDomain PulpSamplerProcessor::current_stream_domain() const noexcept {
    return {static_cast<float>(stream_output_sample_rate_), maximum_stream_block_frames_};
}

PulpSamplerProcessor::StreamDomain PulpSamplerProcessor::replacement_stream_domain(
    const SamplerHeritageRuntime::PreparedReplacement& candidate) const noexcept {
    return {
        static_cast<float>(host_sample_rate_),
        static_cast<std::uint32_t>(
            SamplerHeritageRuntime::replacement_maximum_input_frames(candidate, max_block_frames_)),
    };
}

PulpSamplerPrepareResult
PulpSamplerProcessor::bind_stream_domain_checked(StreamDomain domain,
                                                 std::uint64_t streaming_budget) {
    if (!std::isfinite(domain.sample_rate) || domain.sample_rate <= 0.0f ||
        domain.maximum_block_frames == 0) {
        return {.status = PulpSamplerPrepareStatus::InvalidHostConfiguration};
    }
    const auto maximum_source_frames_per_output =
        SamplerStreamingRuntime::maximum_pitch_ratio() *
        SamplerStreamingRuntime::maximum_source_sample_rate() /
        static_cast<double>(domain.sample_rate);
    auto candidate_sinc = std::make_unique<audio::SampleSincKernelBank>();
    if (!candidate_sinc->build_dense_for_maximum_consumption(maximum_source_frames_per_output)) {
        return {.status = PulpSamplerPrepareStatus::AllocationFailure};
    }
    auto candidate_streaming = std::make_unique<SamplerStreamingRuntime>();
#if defined(PULP_SAMPLER_TEST_HOOKS)
    if (fail_next_stream_domain_prepare_for_test_) {
        fail_next_stream_domain_prepare_for_test_ = false;
        candidate_streaming->fail_next_prepare_for_test();
    }
    if (fail_next_stream_service_prepare_for_test_) {
        fail_next_stream_service_prepare_for_test_ = false;
        candidate_streaming->fail_next_service_prepare_for_test();
    }
    if (fail_next_stream_slot_allocation_for_test_) {
        fail_next_stream_slot_allocation_for_test_ = false;
        candidate_streaming->fail_next_slot_allocation_for_test();
    }
    if (fail_next_stream_thread_start_for_test_) {
        fail_next_stream_thread_start_for_test_ = false;
        candidate_streaming->fail_next_thread_start_for_test();
    }
#endif
    auto result = candidate_streaming->prepare_checked(
        domain.sample_rate, domain.maximum_block_frames, streaming_budget);
    if (!result.prepared())
        return result;
#if defined(PULP_SAMPLER_TEST_HOOKS)
    if (fail_next_stream_domain_source_restore_for_test_) {
        fail_next_stream_domain_source_restore_for_test_ = false;
        candidate_streaming->fail_next_retained_source_publish_for_test();
    }
#endif
    if (!streaming_->clone_published_source_to(*candidate_streaming)) {
        result.status = PulpSamplerPrepareStatus::AllocationFailure;
        return result;
    }
    streaming_.swap(candidate_streaming);
    sinc_bank_.swap(candidate_sinc);
    stream_output_sample_rate_ = domain.sample_rate;
    maximum_stream_block_frames_ = domain.maximum_block_frames;
    const auto published = streaming_->published_source();
    auto last_load = last_load_result_.read();
    if (last_load.loaded() && published.kind == SamplerPublishedSourceKind::Streamed) {
        last_load.selection_generation = published.selection_generation;
        last_load_result_.write(last_load);
    }
    return result;
}

bool PulpSamplerProcessor::rebind_stream_domain(StreamDomain target) noexcept {
    const auto current = current_stream_domain();
    if (target == current)
        return true;
    try {
        return bind_stream_domain_checked(
                   target, config_.streaming_memory_budget_bytes)
            .prepared();
    } catch (...) {
        return false;
    }
}

bool PulpSamplerProcessor::apply_heritage_replacement(
    SamplerHeritageRuntime::PreparedReplacement&& candidate, PulpSamplerHeritageStatus ready) {
    const auto previous_latency = latency_samples();
    if (!rebind_stream_domain(replacement_stream_domain(candidate)))
        return false;
    heritage_.publish_replacement(std::move(candidate), ready);
    reset_all_voices();
    clear_heritage_runtime_persistence();
    if (latency_samples() != previous_latency)
        flag_latency_changed();
    return true;
}

void PulpSamplerProcessor::clear_heritage_runtime_persistence() noexcept {
    callback_runtime_snapshot_available_.store(false, std::memory_order_release);
    restored_runtime_state_available_ = false;
    pending_runtime_state_available_ = false;
}

void PulpSamplerProcessor::publish_heritage_runtime_snapshot() noexcept {
    const auto captured = heritage_.capture_runtime_state();
    PublishedHeritageRuntimeState published;
    published.valid = captured.valid();
    if (published.valid) {
        published.state = captured.state;
    }
    callback_runtime_snapshot_.write(published);
    callback_runtime_snapshot_available_.store(published.valid, std::memory_order_release);
}

std::uint64_t PulpSamplerProcessor::saturated_add(std::uint64_t left,
                                                  std::uint64_t right) noexcept {
    return right > std::numeric_limits<std::uint64_t>::max() - left
               ? std::numeric_limits<std::uint64_t>::max()
               : left + right;
}

void PulpSamplerProcessor::add_envelope_stats(
    audio::SampleStarvationEnvelopeStats& target,
    const audio::SampleStarvationEnvelopeStats& source) noexcept {
    target.predicted_events = saturated_add(target.predicted_events, source.predicted_events);
    target.insufficient_lead_events =
        saturated_add(target.insufficient_lead_events, source.insufficient_lead_events);
    target.emergency_events = saturated_add(target.emergency_events, source.emergency_events);
    target.starved_frames = saturated_add(target.starved_frames, source.starved_frames);
    target.recovery_events = saturated_add(target.recovery_events, source.recovery_events);
}

void PulpSamplerProcessor::harvest_voice_envelope(const SamplerVoice& voice) noexcept {
    if (voice.streamed)
        add_envelope_stats(envelope_lifetime_, voice.stream_reader.starvation_stats());
}

void PulpSamplerProcessor::reset_voice(SamplerVoice& voice) noexcept {
    harvest_voice_envelope(voice);
    const auto slot = static_cast<std::size_t>(&voice - voices_);
    if (slot < kMaxVoices)
        heritage_.reset_voice(slot);
    voice.reset();
}

void PulpSamplerProcessor::publish_envelope_diagnostics() noexcept {
    PulpSamplerEnvelopeDiagnostics result;
    result.lifetime = envelope_lifetime_;
    for (const auto& voice : voices_) {
        if (!voice.active || !voice.streamed)
            continue;
        ++result.active_streamed_voices;
        add_envelope_stats(result.lifetime, voice.stream_reader.starvation_stats());
        switch (voice.stream_reader.starvation_mode()) {
        case audio::SampleStarvationMode::Ready:
            ++result.ready_voices;
            break;
        case audio::SampleStarvationMode::FadingOut:
            ++result.fading_out_voices;
            break;
        case audio::SampleStarvationMode::Silent:
            ++result.silent_voices;
            break;
        case audio::SampleStarvationMode::Recovering:
            ++result.recovering_voices;
            break;
        }
    }
    envelope_snapshot_.write(result);
    envelope_snapshot_available_.store(true, std::memory_order_release);
}

void PulpSamplerProcessor::publish_interpolation_diagnostics() noexcept {
    interpolation_snapshot_.write(
        {.sinc_fallback_selections = sinc_fallback_selections_,
         .resident_mip_suppressions = resident_mip_suppressions_,
         .streamed_mip_suppressions = streamed_mip_suppressions_,
         .sinc_promotion_suppressions = sinc_promotion_suppressions_});
    interpolation_snapshot_available_.store(true, std::memory_order_release);
}

void PulpSamplerProcessor::fail_prepare(PulpSamplerPrepareResult result) noexcept {
    release();
    prepare_result_.write(result);
}

} // namespace pulp::examples
