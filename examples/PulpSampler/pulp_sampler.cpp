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
        state.runtime_host_sample_rate = published.host_sample_rate;
    } else if (restored_runtime_state_available_) {
        state.has_runtime_state = true;
        state.runtime_state = restored_runtime_state_;
        state.runtime_host_sample_rate = restored_runtime_host_sample_rate_;
    } else if (pending_runtime_state_available_) {
        state.has_runtime_state = true;
        state.runtime_state = pending_runtime_state_;
        state.runtime_host_sample_rate = pending_runtime_host_sample_rate_;
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
            runtime_state, parsed.state.runtime_host_sample_rate, candidate);
        if (!heritage_ready(result))
            return false;
        if (!apply_heritage_replacement(std::move(candidate), result))
            return false;
        if (result == PulpSamplerHeritageStatus::Ready && runtime_state != nullptr) {
            restored_runtime_state_ = *runtime_state;
            restored_runtime_host_sample_rate_ = parsed.state.runtime_host_sample_rate;
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
        pending_runtime_host_sample_rate_ = parsed.state.runtime_host_sample_rate;
        pending_runtime_state_available_ = true;
        restored_runtime_state_ = *runtime_state;
        restored_runtime_host_sample_rate_ = parsed.state.runtime_host_sample_rate;
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
                                        max_block_frames_, nullptr, 0.0, candidate);
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
            maximum_consumption > 128.0)
            return std::numeric_limits<std::size_t>::max();
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
    if (heritage_.configured()) {
        const auto profile = heritage_.configured_profile();
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
    }
    const auto stream_rate = ctx.sample_rate * clock_ratio;
    const auto stream_frames_double =
        std::ceil(static_cast<double>(ctx.max_buffer_size) * clock_ratio);
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
    if (heritage_processing) {
        persistent = add(persistent, sizeof(audio::SampleHeritageEngine));
        const auto input_ratio = ctx.sample_rate / machine_rate;
        const auto return_ratio = machine_rate * clock_ratio / ctx.sample_rate;
        persistent = add(persistent, sinc_bytes(std::max(input_ratio, return_ratio)));
        const auto machine_frames = static_cast<std::size_t>(
            std::ceil(static_cast<double>(ctx.max_buffer_size) * return_ratio));
        const auto input_frames = static_cast<std::size_t>(stream_frames_double);
        persistent =
            add(persistent, multiply(multiply(static_cast<std::size_t>(ctx.output_channels),
                                              add(machine_frames, input_frames)),
                                     sizeof(float)));
        persistent = add(persistent, multiply(static_cast<std::size_t>(ctx.output_channels),
                                              2u * 48u * sizeof(float)));
    }
    usage.persistent_bytes = persistent;
    const auto scratch_frames = static_cast<std::size_t>(ctx.max_buffer_size);
    if (scratch_frames <=
        std::numeric_limits<std::size_t>::max() / (kMaxOutputChannels * sizeof(float))) {
        usage.block_scratch_bytes = scratch_frames * kMaxOutputChannels * sizeof(float);
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
    restored_runtime_host_sample_rate_ = 0.0;
    if (heritage_.configured()) {
        const auto* runtime_state =
            pending_runtime_state_available_ ? &pending_runtime_state_ : nullptr;
        const auto result =
            heritage_.prepare(host_sample_rate_, prepared_output_channels_, max_block_frames_,
                              runtime_state, pending_runtime_host_sample_rate_);
        if (!heritage_ready(result)) {
            fail_prepare({.status = PulpSamplerPrepareStatus::HeritagePrepareFailed});
            return;
        } else {
            if (result == PulpSamplerHeritageStatus::Ready && runtime_state != nullptr) {
                restored_runtime_state_ = *runtime_state;
                restored_runtime_host_sample_rate_ = pending_runtime_host_sample_rate_;
                restored_runtime_state_available_ = true;
            }
        }
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
    pending_runtime_host_sample_rate_ = 0.0;
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
        static_cast<float>(static_cast<double>(host_sample_rate_) *
                           SamplerHeritageRuntime::replacement_clock_ratio(candidate)),
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
        return bind_stream_domain_checked(target).prepared();
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
    restored_runtime_host_sample_rate_ = 0.0;
    pending_runtime_host_sample_rate_ = 0.0;
}

void PulpSamplerProcessor::publish_heritage_runtime_snapshot() noexcept {
    const auto captured = heritage_.capture_runtime_state();
    PublishedHeritageRuntimeState published;
    published.valid = captured.valid();
    if (published.valid) {
        published.state = captured.state;
        published.host_sample_rate = host_sample_rate_;
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
        {.sinc_fallback_selections = sinc_fallback_selections_});
    interpolation_snapshot_available_.store(true, std::memory_order_release);
}

void PulpSamplerProcessor::fail_prepare(PulpSamplerPrepareResult result) noexcept {
    release();
    prepare_result_.write(result);
}

} // namespace pulp::examples
