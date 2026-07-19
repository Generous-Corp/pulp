#include "sampler_streaming_runtime.hpp"

namespace pulp::examples {

SamplerStreamingRuntime::~SamplerStreamingRuntime() {
    release();
}

PulpSamplerPrepareResult
SamplerStreamingRuntime::estimate_prepare(float host_sample_rate,
                                          std::uint32_t maximum_host_block_frames,
                                          std::uint64_t streaming_memory_budget_bytes) noexcept {
    return derive_prepare(host_sample_rate, maximum_host_block_frames,
                          streaming_memory_budget_bytes)
        .result;
}

bool SamplerStreamingRuntime::prepare(float host_sample_rate,
                                      std::uint32_t maximum_host_block_frames) {
    return prepare_checked(host_sample_rate, maximum_host_block_frames).prepared();
}

PulpSamplerPrepareResult
SamplerStreamingRuntime::prepare_checked(float host_sample_rate,
                                         std::uint32_t maximum_host_block_frames,
                                         std::uint64_t streaming_memory_budget_bytes) {
    release();
    const auto derived =
        derive_prepare(host_sample_rate, maximum_host_block_frames, streaming_memory_budget_bytes);
    auto result = derived.result;
    if (!result.prepared())
        return result;
#if defined(PULP_SAMPLER_TEST_HOOKS)
    if (fail_next_prepare_for_test_.exchange(false, std::memory_order_acq_rel)) {
        result.status = PulpSamplerPrepareStatus::AllocationFailure;
        return result;
    }
#endif
    host_sample_rate_ = host_sample_rate;
    maximum_host_block_frames_ = maximum_host_block_frames;
    selection_generation_ = 0;
    stream_audio_ack_selection_.store(0, std::memory_order_relaxed);
    audio_active_generation_.store(1, std::memory_order_relaxed);
    audio_completed_generation_.store(0, std::memory_order_relaxed);
    next_audio_generation_.store(1, std::memory_order_relaxed);
    audio_completed_page_read_epoch_ = 0;
    pages_published_.store(0, std::memory_order_relaxed);
    starved_frames_.store(0, std::memory_order_relaxed);
    service_starvation_events_.store(0, std::memory_order_relaxed);
    decode_failure_events_.store(0, std::memory_order_relaxed);
    invalid_preload_contract_events_.store(0, std::memory_order_relaxed);
    stale_generation_events_.store(0, std::memory_order_relaxed);
    normal_end_of_source_events_.store(0, std::memory_order_relaxed);
    invalid_render_contract_events_.store(0, std::memory_order_relaxed);
    cache_pages_retired_.store(0, std::memory_order_relaxed);
    cache_pages_reused_.store(0, std::memory_order_relaxed);
    decode_source_outstanding_high_water_.store(0, std::memory_order_relaxed);
    decode_completed_frames_.store(0, std::memory_order_relaxed);
    same_source_reader_concurrency_high_water_.store(0, std::memory_order_relaxed);
    cache_async_reservations_high_water_.store(0, std::memory_order_relaxed);
    active_reservations_high_water_.store(0, std::memory_order_relaxed);
    aggregate_rate_admission_rejections_.store(0, std::memory_order_relaxed);
    aggregate_rate_automation_rejections_.store(0, std::memory_order_relaxed);
    decode_scratch_bytes_.store(0, std::memory_order_relaxed);
    sources_retired_.store(0, std::memory_order_relaxed);
    active_sources_.store(0, std::memory_order_relaxed);
    preload_frames_.store(0, std::memory_order_relaxed);
    service_dispatch_paused_.store(false, std::memory_order_relaxed);
#if defined(PULP_SAMPLER_TEST_HOOKS)
    fail_next_stream_decode_for_test_.store(false, std::memory_order_relaxed);
    reverse_prewarm_pending_for_test_.store(false, std::memory_order_relaxed);
    block_next_reverse_decode_for_test_.store(false, std::memory_order_relaxed);
    reverse_decode_entered_for_test_.store(false, std::memory_order_relaxed);
    release_reverse_decode_for_test_.store(true, std::memory_order_relaxed);
    unpublished_rollback_count_for_test_.store(0, std::memory_order_relaxed);
    unpublished_rollback_attempts_for_test_.store(0, std::memory_order_relaxed);
    fail_after_stream_member_count_for_test_.store(-1, std::memory_order_relaxed);
    pause_before_bundle_publish_for_test_.store(false, std::memory_order_relaxed);
    bundle_publish_paused_ack_for_test_.store(false, std::memory_order_relaxed);
    service_command_drain_paused_for_test_.store(false, std::memory_order_relaxed);
    service_command_drain_paused_ack_for_test_.store(false, std::memory_order_relaxed);
    file_stage_paused_for_test_.store(false, std::memory_order_relaxed);
    file_stage_paused_ack_for_test_.store(false, std::memory_order_relaxed);
    file_stage_attempts_for_test_.store(0, std::memory_order_relaxed);
    throw_during_file_stage_for_test_.store(false, std::memory_order_relaxed);
    reverse_prewarm_timeout_override_for_test_ = false;
    reverse_prewarm_timeout_for_test_ = std::chrono::milliseconds{0};
#endif
    unpublished_rollbacks_.fill({});
    next_source_generations_.fill(1);
    published_source_.write({});
    file_request_result_ = {};

    page_frames_ = derived.page_frames;
    const auto governor_capacity =
        result.configured_streaming_memory_bytes - derived.decode_scratch_bytes;
    if (!memory_governor_.prepare(governor_capacity)) {
        result.status = PulpSamplerPrepareStatus::AllocationFailure;
        release();
        return result;
    }
    decode_scratch_bytes_.store(derived.decode_scratch_bytes, std::memory_order_relaxed);

    const auto service_config = audio::SampleStreamAsyncServiceConfig{
        .cache =
            {
                .scheduler_capacity = kCommandCapacity,
                .source_identity_capacity = kSourceCapacity,
                .page_memory_budget_bytes = 0,
                .memory_governor = memory_governor_.handle(),
            },
        .decode =
            {
                .worker_count = kWorkerCount,
                .source_capacity = kSourceCapacity,
                .maximum_channels = kMaximumChannels,
                .maximum_frames_per_job = page_frames_,
                .maximum_outstanding_jobs_per_source = kMaximumOutstandingJobsPerSource,
            },
    };
#if defined(PULP_SAMPLER_TEST_HOOKS)
    if (fail_next_service_prepare_for_test_.exchange(false, std::memory_order_acq_rel)) {
        result.status = PulpSamplerPrepareStatus::StreamingServiceUnavailable;
        release();
        return result;
    }
#endif
    const auto service_prepare = service_.prepare_checked(service_config);
    service_prepare_status_.store(service_prepare.status, std::memory_order_relaxed);
    service_ready_ = service_prepare.prepared();
    if (!service_ready_) {
        result.status = PulpSamplerPrepareStatus::AllocationFailure;
        release();
        return result;
    }
    try {
#if defined(PULP_SAMPLER_TEST_HOOKS)
        if (fail_next_prepare_allocation_for_test_.exchange(false, std::memory_order_acq_rel)) {
            throw std::bad_alloc{};
        }
#endif
        for (auto& slot : slots_)
            slot = std::make_unique<StreamedSlot>();
        service_running_.store(true, std::memory_order_release);
#if defined(PULP_SAMPLER_TEST_HOOKS)
        if (fail_next_thread_start_for_test_.exchange(false, std::memory_order_acq_rel)) {
            throw std::system_error(
                std::make_error_code(std::errc::resource_unavailable_try_again));
        }
#endif
        service_thread_ = std::thread([this] { service_loop(); });
    } catch (...) {
        result.status = PulpSamplerPrepareStatus::AllocationFailure;
        release();
        return result;
    }
    return result;
}

void SamplerStreamingRuntime::release() noexcept {
    {
        std::lock_guard lock(file_request_mutex_);
        service_running_.store(false, std::memory_order_release);
        file_request_complete_ = true;
        file_request_result_.status = PulpSamplerLoadStatus::ShuttingDown;
    }
#if defined(PULP_SAMPLER_TEST_HOOKS)
    release_reverse_decode_for_test_.store(true, std::memory_order_release);
    pause_before_bundle_publish_for_test_.store(false, std::memory_order_release);
    file_stage_paused_for_test_.store(false, std::memory_order_release);
#endif
    service_wake_.notify_all();
    file_request_changed_.notify_all();
    if (service_thread_.joinable())
        service_thread_.join();
    std::lock_guard source_lock(source_load_mutex_);

    published_source_.write({});
    if (service_ready_) {
        service_.update_audio_generations(
            audio_active_generation_.load(std::memory_order_acquire),
            audio_completed_generation_.load(std::memory_order_acquire));
        service_.drain_commands(commands_);
    }
    service_.release();
    decode_scratch_bytes_.store(0, std::memory_order_relaxed);
    // SampleStreamAsyncService::release() joins every decode worker while
    // the decode pool deliberately retains its telemetry until prepare().
    // Snapshot only after that join so a late short-read cannot disappear.
    decode_failure_events_.store(service_.decode_telemetry().decode_errors,
                                 std::memory_order_relaxed);
    unpublished_rollbacks_.fill({});
    next_source_generations_.fill(0);
#if defined(PULP_SAMPLER_TEST_HOOKS)
    unpublished_rollback_count_for_test_.store(0, std::memory_order_relaxed);
#endif
    for (auto& slot : slots_) {
        if (slot) {
            for (auto& member : slot->members)
                member.asset.release();
        }
        slot.reset();
    }
    retained_stream_recipe_ = {};
    service_ready_ = false;
    active_sources_.store(0, std::memory_order_relaxed);
    {
        std::lock_guard lock(file_request_mutex_);
        file_request_pending_ = false;
        file_request_prepared_.reset();
    }
    (void)memory_governor_.release();
}

std::optional<audio::SampleStreamSourceToken>
SamplerStreamingRuntime::take_source_token(std::size_t identity_index) noexcept {
    if (identity_index >= next_source_generations_.size())
        return std::nullopt;
    auto& next_generation = next_source_generations_[identity_index];
    if (next_generation == 0)
        return std::nullopt;
    const audio::SampleStreamSourceToken token{
        static_cast<std::uint64_t>(identity_index) + 1,
        next_generation,
    };
    next_generation = next_generation == std::numeric_limits<std::uint64_t>::max()
                          ? 0
                          : next_generation + 1;
    return token;
}

bool SamplerStreamingRuntime::republish_resident(const SamplerPublishedSource& retained) noexcept {
    if (retained.kind != SamplerPublishedSourceKind::Resident || !retained.resident.valid) {
        return false;
    }
    std::lock_guard lock(source_load_mutex_);
    const auto generation = next_selection_generation();
    if (!generation)
        return false;
    auto published = retained;
    published.selection_generation = *generation;
    published_source_.write(published);
    service_wake_.notify_all();
    return true;
}

std::optional<std::uint64_t> SamplerStreamingRuntime::next_selection_generation() noexcept {
    if (selection_generation_ == std::numeric_limits<std::uint64_t>::max())
        return std::nullopt;
    return ++selection_generation_;
}

PulpSamplerLoadResult SamplerStreamingRuntime::load_sample_file_result(std::string_view path) {
    PulpSamplerLoadResult result;
    if (path.empty()) {
        result.status = PulpSamplerLoadStatus::EmptyPath;
        return result;
    }
    std::lock_guard source_lock(source_load_mutex_);
    if (!service_running_.load(std::memory_order_acquire)) {
        result.status = PulpSamplerLoadStatus::NotPrepared;
        return result;
    }
    if (active_sources_.load(std::memory_order_acquire) >= kBundleSlotCount) {
        result.status = PulpSamplerLoadStatus::BundleCapacityExceeded;
        return result;
    }

    StagedStreamedFile staged;
    try {
        staged = stage_streamed_file(path);
    } catch (const std::bad_alloc&) {
        result.status = PulpSamplerLoadStatus::AllocationFailure;
        return result;
    } catch (...) {
        result.status = PulpSamplerLoadStatus::InternalFailure;
        return result;
    }
    if (!staged.prepared)
        return staged.result;
    return submit_staged_file(std::move(staged));
}

bool SamplerStreamingRuntime::load_sample_file(std::string_view path) {
    return load_sample_file_result(path).loaded();
}

bool SamplerStreamingRuntime::restore_retained_streamed_source() {
    std::lock_guard source_lock(source_load_mutex_);
    if (!service_running_.load(std::memory_order_acquire) || !retained_stream_recipe_.valid()) {
        return false;
    }
    auto staged = stage_retained_streamed_file();
    return staged.prepared && submit_staged_file(std::move(staged)).loaded();
}

bool SamplerStreamingRuntime::clone_published_source_to(SamplerStreamingRuntime& target) {
    std::lock_guard source_lock(source_load_mutex_);
    const auto retained = published_source_.read();
    if (retained.kind == SamplerPublishedSourceKind::None)
        return true;
    if (retained.kind == SamplerPublishedSourceKind::Resident)
        return target.republish_resident(retained);
    if (retained.kind != SamplerPublishedSourceKind::Streamed || !retained_stream_recipe_.valid()) {
        return false;
    }
    target.retained_stream_recipe_ = retained_stream_recipe_;
    return target.restore_retained_streamed_source();
}

#if defined(PULP_SAMPLER_TEST_HOOKS)

void SamplerStreamingRuntime::pause_file_stage_for_test(bool paused) noexcept {
    file_stage_paused_for_test_.store(paused, std::memory_order_release);
}
#endif

#if defined(PULP_SAMPLER_TEST_HOOKS)

bool SamplerStreamingRuntime::file_stage_paused_for_test() const noexcept {
    return file_stage_paused_ack_for_test_.load(std::memory_order_acquire);
}
#endif

#if defined(PULP_SAMPLER_TEST_HOOKS)

bool SamplerStreamingRuntime::has_retained_streamed_source_for_test() const noexcept {
    return retained_stream_recipe_.valid();
}
#endif

#if defined(PULP_SAMPLER_TEST_HOOKS)

void SamplerStreamingRuntime::fail_next_retained_source_publish_for_test() noexcept {
    fail_after_stream_member_count_for_test_.store(0, std::memory_order_release);
}
#endif

#if defined(PULP_SAMPLER_TEST_HOOKS)

void SamplerStreamingRuntime::fail_next_prepare_for_test() noexcept {
    fail_next_prepare_for_test_.store(true, std::memory_order_release);
}
#endif

#if defined(PULP_SAMPLER_TEST_HOOKS)

void SamplerStreamingRuntime::fail_next_thread_start_for_test() noexcept {
    fail_next_thread_start_for_test_.store(true, std::memory_order_release);
}
#endif

#if defined(PULP_SAMPLER_TEST_HOOKS)

void SamplerStreamingRuntime::fail_next_service_prepare_for_test() noexcept {
    fail_next_service_prepare_for_test_.store(true, std::memory_order_release);
}
#endif

#if defined(PULP_SAMPLER_TEST_HOOKS)

void SamplerStreamingRuntime::fail_next_slot_allocation_for_test() noexcept {
    fail_next_prepare_allocation_for_test_.store(true, std::memory_order_release);
}
#endif

#if defined(PULP_SAMPLER_TEST_HOOKS)

bool SamplerStreamingRuntime::fully_released_for_test() const noexcept {
    if (service_ready_ || service_running_.load(std::memory_order_acquire) ||
        service_thread_.joinable() || service_.prepared())
        return false;
    return std::all_of(slots_.begin(), slots_.end(),
                       [](const auto& slot) { return slot == nullptr; });
}
#endif

SamplerPublishedSource SamplerStreamingRuntime::published_source() const noexcept {
    return published_source_.read();
}

PulpSamplerStreamStats SamplerStreamingRuntime::stats() const noexcept {
    const auto memory = memory_governor_.stats();
    const auto decode_scratch_bytes = decode_scratch_bytes_.load(std::memory_order_relaxed);
    return {
        .service_prepare_status = service_prepare_status_.load(std::memory_order_relaxed),
        .decode_worker_count = service_ready_ ? kWorkerCount : 0,
        .pages_published = pages_published_.load(std::memory_order_relaxed),
        .starved_output_frames = starved_frames_.load(std::memory_order_relaxed),
        .service_starvation_events = service_starvation_events_.load(std::memory_order_relaxed),
        .decode_failure_events = decode_failure_events_.load(std::memory_order_relaxed),
        .invalid_preload_contract_events =
            invalid_preload_contract_events_.load(std::memory_order_relaxed),
        .stale_generation_events = stale_generation_events_.load(std::memory_order_relaxed),
        .normal_end_of_source_events = normal_end_of_source_events_.load(std::memory_order_relaxed),
        .invalid_render_contract_events =
            invalid_render_contract_events_.load(std::memory_order_relaxed),
        .cache_pages_retired = cache_pages_retired_.load(std::memory_order_relaxed),
        .cache_pages_reused = cache_pages_reused_.load(std::memory_order_relaxed),
        .decode_source_outstanding_high_water =
            decode_source_outstanding_high_water_.load(std::memory_order_relaxed),
        .decode_completed_frames = decode_completed_frames_.load(std::memory_order_relaxed),
        .same_source_reader_concurrency_high_water =
            same_source_reader_concurrency_high_water_.load(std::memory_order_relaxed),
        .cache_async_reservations_high_water =
            cache_async_reservations_high_water_.load(std::memory_order_relaxed),
        .active_reservations_high_water =
            active_reservations_high_water_.load(std::memory_order_relaxed),
        .aggregate_rate_admission_rejections =
            aggregate_rate_admission_rejections_.load(std::memory_order_relaxed),
        .aggregate_rate_automation_rejections =
            aggregate_rate_automation_rejections_.load(std::memory_order_relaxed),
        .decode_scratch_bytes = decode_scratch_bytes,
        .total_memory_capacity_bytes = memory.capacity_bytes + decode_scratch_bytes,
        .current_total_memory_bytes = memory.current_total_bytes + decode_scratch_bytes,
        .peak_total_memory_bytes = memory.peak_total_bytes + decode_scratch_bytes,
        .sources_retired = sources_retired_.load(std::memory_order_relaxed),
        .active_sources = active_sources_.load(std::memory_order_relaxed),
        .preload_frames = preload_frames_.load(std::memory_order_relaxed),
        .memory = memory,
    };
}

PulpSamplerPreloadPolicy SamplerStreamingRuntime::preload_policy() const noexcept {
    PulpSamplerPreloadPolicy policy;
    const auto source = published_source_.read();
    if (source.kind != SamplerPublishedSourceKind::Streamed)
        return policy;
    policy.selection_generation = source.selection_generation;
    const auto& contract = source.streamed.preload_contract;
    const auto evaluated = audio::evaluate_sample_preload_contract(contract);
    policy.source_sample_rate = contract.source_sample_rate;
    policy.host_sample_rate = contract.host_sample_rate;
    policy.maximum_playback_ratio = contract.maximum_playback_ratio;
    policy.certified_io_latency_seconds = contract.certified_io_latency_seconds;
    policy.scheduler_margin_seconds = contract.scheduler_margin_seconds;
    policy.decoder_latency_seconds = contract.decoder_latency_seconds;
    policy.maximum_host_block_frames = contract.maximum_host_block_frames;
    policy.interpolation_guard_frames = contract.interpolation_guard_frames;
    policy.loop_prefetch_guard_frames = contract.loop_prefetch_guard_frames;
    policy.required_preload_frames = evaluated.valid() ? evaluated.required_preload_frames : 0;
    policy.configured_preload_frames = contract.configured_preload_frames;
    policy.page_frames = source.streamed.stream_source.page_frames;
    return policy;
}

SamplerStreamingRuntime::CommandInbox& SamplerStreamingRuntime::command_inbox() noexcept {
    return commands_;
}

SamplerAudioCallbackToken SamplerStreamingRuntime::begin_audio_callback() noexcept {
    // Capture the page barrier before any page lookup in this callback.
    // A retirement published after this load requires a later callback to
    // complete before that page can be cleared and reused.
    const auto page_read_epoch = service_.begin_audio_page_read();
    const auto generation = next_audio_generation_.fetch_add(1, std::memory_order_relaxed) + 1;
    audio_active_generation_.store(generation, std::memory_order_release);
    return {generation, page_read_epoch};
}

void SamplerStreamingRuntime::complete_audio_callback(SamplerAudioCallbackToken token) noexcept {
    if (!token.valid())
        return;
    // All page reads are complete before this acknowledgement. Source
    // generation completion remains a separate retirement domain.
    if (token.page_read_epoch != audio_completed_page_read_epoch_) {
        service_.complete_audio_page_read(token.page_read_epoch);
        audio_completed_page_read_epoch_ = token.page_read_epoch;
    }
    audio_completed_generation_.store(token.callback_generation, std::memory_order_release);
}

void SamplerStreamingRuntime::acknowledge_selection(std::uint64_t generation) noexcept {
    stream_audio_ack_selection_.store(generation, std::memory_order_release);
}

void SamplerStreamingRuntime::record_voice_outcome(audio::SampleStreamVoiceOutcomeClass outcome,
                                                   std::uint64_t starved_frames) noexcept {
    switch (outcome) {
    case audio::SampleStreamVoiceOutcomeClass::None:
        return;
    case audio::SampleStreamVoiceOutcomeClass::ServiceStarvation:
        service_starvation_events_.fetch_add(1, std::memory_order_relaxed);
        starved_frames_.fetch_add(starved_frames, std::memory_order_relaxed);
        return;
    case audio::SampleStreamVoiceOutcomeClass::InvalidPreloadContract:
        invalid_preload_contract_events_.fetch_add(1, std::memory_order_relaxed);
        return;
    case audio::SampleStreamVoiceOutcomeClass::StaleGeneration:
        stale_generation_events_.fetch_add(1, std::memory_order_relaxed);
        return;
    case audio::SampleStreamVoiceOutcomeClass::NormalEndOfSource:
        normal_end_of_source_events_.fetch_add(1, std::memory_order_relaxed);
        return;
    case audio::SampleStreamVoiceOutcomeClass::InvalidRenderContract:
        invalid_render_contract_events_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
}

void SamplerStreamingRuntime::record_aggregate_rate_admission_rejection() noexcept {
    aggregate_rate_admission_rejections_.fetch_add(1, std::memory_order_relaxed);
}

void SamplerStreamingRuntime::record_aggregate_rate_automation_rejection() noexcept {
    aggregate_rate_automation_rejections_.fetch_add(1, std::memory_order_relaxed);
}

SamplerStreamingRuntime::DerivedPrepare
SamplerStreamingRuntime::derive_prepare(float host_sample_rate,
                                        std::uint32_t maximum_host_block_frames,
                                        std::uint64_t configured_budget) noexcept {
    DerivedPrepare derived;
    auto& result = derived.result;
    if (!std::isfinite(host_sample_rate) || host_sample_rate <= 0.0f ||
        maximum_host_block_frames == 0) {
        result.status = PulpSamplerPrepareStatus::InvalidHostConfiguration;
        return derived;
    }
    const auto maximum_source_frames_per_block =
        std::ceil(static_cast<double>(maximum_host_block_frames) * kMaximumPitchRatio *
                  kMaximumSourceRate / static_cast<double>(host_sample_rate));
    if (!std::isfinite(maximum_source_frames_per_block) || maximum_source_frames_per_block < 0.0 ||
        maximum_source_frames_per_block >
            static_cast<double>(std::numeric_limits<std::uint64_t>::max())) {
        result.status = PulpSamplerPrepareStatus::InvalidHostConfiguration;
        return derived;
    }
    const auto maximum_preload = audio::evaluate_sample_preload_contract({
        .source_sample_rate = kMaximumSourceRate,
        .host_sample_rate = static_cast<double>(host_sample_rate),
        .maximum_playback_ratio = kMaximumPitchRatio,
        .certified_io_latency_seconds = kCertifiedIoLatencySeconds,
        .scheduler_margin_seconds = kSchedulerMarginSeconds,
        .decoder_latency_seconds = kDecoderLatencySeconds,
        .maximum_host_block_frames = maximum_host_block_frames,
        .interpolation_guard_frames = audio::kHighQualitySampleSincHalfWidth,
        .loop_prefetch_guard_frames = static_cast<std::uint64_t>(maximum_source_frames_per_block),
    });
    if (!maximum_preload.valid()) {
        result.status = PulpSamplerPrepareStatus::InvalidHostConfiguration;
        return derived;
    }
    const auto cache_working_set_page_frames =
        std::ceil((static_cast<double>(maximum_preload.required_preload_frames) +
                   maximum_source_frames_per_block) /
                  static_cast<double>(kPagesPerVoiceWorkingSet - 1));
    if (!std::isfinite(cache_working_set_page_frames) || cache_working_set_page_frames < 0.0 ||
        cache_working_set_page_frames >
            static_cast<double>(std::numeric_limits<std::uint64_t>::max())) {
        result.status = PulpSamplerPrepareStatus::InvalidHostConfiguration;
        return derived;
    }
    derived.page_frames =
        std::max<std::uint64_t>({kDefaultPageFrames,
                                 static_cast<std::uint64_t>(maximum_source_frames_per_block /
                                                            kSourceAdvancePageDemandsPerRegion) +
                                     1,
                                 static_cast<std::uint64_t>(cache_working_set_page_frames)});
    const auto page_budget = audio::checked_sample_storage_bytes(
        kMaximumChannels, derived.page_frames, kSourceCapacity * kCachePagesPerSource);
    const auto preload_budget = audio::checked_sample_storage_bytes(
        kMaximumChannels, maximum_preload.required_preload_frames, kSourceCapacity);
    const auto decode_budget = audio::checked_sample_storage_bytes(
        kMaximumChannels, derived.page_frames,
        static_cast<std::uint64_t>(kWorkerCount) * kMaximumOutstandingJobsPerSource);
    if (!page_budget || !preload_budget || !decode_budget ||
        *preload_budget > std::numeric_limits<std::uint64_t>::max() - *page_budget ||
        *decode_budget >
            std::numeric_limits<std::uint64_t>::max() - (*page_budget + *preload_budget)) {
        result.status = PulpSamplerPrepareStatus::AllocationFailure;
        return derived;
    }
    result.required_streaming_memory_bytes = *page_budget + *preload_budget + *decode_budget;
    result.configured_streaming_memory_bytes =
        configured_budget == 0 ? result.required_streaming_memory_bytes : configured_budget;
    derived.decode_scratch_bytes = *decode_budget;
    result.status =
        result.configured_streaming_memory_bytes < result.required_streaming_memory_bytes
            ? PulpSamplerPrepareStatus::StreamingMemoryBudgetTooSmall
            : PulpSamplerPrepareStatus::Ok;
    return derived;
}

bool SamplerStreamingRuntime::has_registered_audio_reader(std::string_view path) {
    auto extension = std::filesystem::path(path).extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    return audio::FormatRegistry::instance().find_reader(extension) != nullptr;
}

} // namespace pulp::examples
