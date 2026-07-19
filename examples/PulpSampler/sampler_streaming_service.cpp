#include "sampler_streaming_runtime.hpp"

namespace pulp::examples {

void SamplerStreamingRuntime::service_loop() noexcept {
    while (service_running_.load(std::memory_order_acquire)) {
        service_.update_audio_generations(
            audio_active_generation_.load(std::memory_order_acquire),
            audio_completed_generation_.load(std::memory_order_acquire));
        process_file_request();
#if defined(PULP_SAMPLER_TEST_HOOKS)
        const auto command_drain_paused =
            service_command_drain_paused_for_test_.load(std::memory_order_acquire);
        service_command_drain_paused_ack_for_test_.store(command_drain_paused,
                                                         std::memory_order_release);
        if (!command_drain_paused)
#endif
        {
            service_.drain_commands(commands_);
        }
        service_.drain_completions();
        collect_unpublished_rollbacks();
        if (!service_dispatch_paused_.load(std::memory_order_acquire)) {
            for (std::uint32_t attempt = 0; attempt < kSourceCapacity * 2; ++attempt) {
                const auto dispatched = service_.dispatch_once();
                if (!dispatch_made_owner_progress(dispatched)) {
                    break;
                }
            }
        }
        retire_sources();
        pages_published_.store(service_.telemetry().completions_published,
                               std::memory_order_relaxed);
        decode_failure_events_.store(service_.decode_telemetry().decode_errors,
                                     std::memory_order_relaxed);
        const auto cache = service_.cache_stats();
        cache_pages_retired_.store(cache.pages_retired, std::memory_order_relaxed);
        cache_pages_reused_.store(cache.retired_pages_reused, std::memory_order_relaxed);
        const auto decode = service_.decode_telemetry();
        decode_source_outstanding_high_water_.store(decode.source_outstanding_high_water,
                                                    std::memory_order_relaxed);
        decode_completed_frames_.store(decode.completed_frames, std::memory_order_relaxed);
        same_source_reader_concurrency_high_water_.store(
            decode.same_source_reader_concurrency_high_water, std::memory_order_relaxed);
        cache_async_reservations_high_water_.store(cache.async_reservations_high_water,
                                                   std::memory_order_relaxed);
        active_reservations_high_water_.store(service_.telemetry().active_reservations_high_water,
                                              std::memory_order_relaxed);

        std::unique_lock lock(service_wait_mutex_);
        service_wake_.wait_for(lock, std::chrono::milliseconds(1), [this] {
            return !service_running_.load(std::memory_order_acquire) ||
                   file_request_pending_snapshot();
        });
    }
}

bool SamplerStreamingRuntime::file_request_pending_snapshot() noexcept {
    std::lock_guard lock(file_request_mutex_);
    return file_request_pending_;
}

bool SamplerStreamingRuntime::dispatch_made_owner_progress(
    const audio::SampleStreamAsyncDispatchResult& dispatched) noexcept {
    if (dispatched.status == audio::SampleStreamAsyncDispatchStatus::Queued)
        return true;
    if (dispatched.status != audio::SampleStreamAsyncDispatchStatus::Deferred)
        return false;
    // These owner-side outcomes consume a ready/stale request or begin a
    // bounded retirement. Keep draining within the fixed attempt budget;
    // stopping after the first already-ready demand lets harmless refresh
    // traffic head-of-line block pages that actually need decoding.
    return dispatched.reserve_status == audio::SampleStreamAsyncReserveStatus::AlreadyReady ||
           dispatched.reserve_status == audio::SampleStreamAsyncReserveStatus::StaleSource ||
           dispatched.reserve_status == audio::SampleStreamAsyncReserveStatus::PageRetired;
}

void SamplerStreamingRuntime::process_file_request() noexcept {
    std::unique_ptr<PreparedStreamedFile> prepared;
    {
        std::lock_guard lock(file_request_mutex_);
        if (!file_request_pending_)
            return;
        prepared = std::move(file_request_prepared_);
        file_request_pending_ = false;
    }

    PulpSamplerLoadStatus status = PulpSamplerLoadStatus::InternalFailure;
    try {
        status =
            prepared ? publish_streamed_file(*prepared) : PulpSamplerLoadStatus::InternalFailure;
    } catch (const std::bad_alloc&) {
        status = PulpSamplerLoadStatus::AllocationFailure;
    } catch (...) {
        status = PulpSamplerLoadStatus::InternalFailure;
    }
    // A completed result promises that every unpublished staging lease has
    // already rolled back. Destroy the staged bundle before waking the
    // caller; otherwise it can observe transient retained preload bytes.
    prepared.reset();
    {
        std::lock_guard lock(file_request_mutex_);
        file_request_result_.status =
            status == PulpSamplerLoadStatus::Ok && !service_running_.load(std::memory_order_acquire)
                ? PulpSamplerLoadStatus::ShuttingDown
                : status;
        file_request_complete_ = true;
    }
    file_request_changed_.notify_all();
}

PulpSamplerLoadResult SamplerStreamingRuntime::submit_staged_file(StagedStreamedFile staged) {
    std::unique_lock request_lock(file_request_mutex_);
    if (!service_running_.load(std::memory_order_acquire)) {
        staged.result.status = PulpSamplerLoadStatus::ShuttingDown;
        return staged.result;
    }
    if (file_request_pending_) {
        staged.result.status = PulpSamplerLoadStatus::Busy;
        return staged.result;
    }
    file_request_prepared_ = std::move(staged.prepared);
    file_request_result_ = staged.result;
    file_request_complete_ = false;
    file_request_pending_ = true;
    service_wake_.notify_all();
    file_request_changed_.wait(request_lock, [this] {
        return file_request_complete_ || !service_running_.load(std::memory_order_acquire);
    });
    if (!file_request_complete_)
        file_request_result_.status = PulpSamplerLoadStatus::ShuttingDown;
    return file_request_result_;
}

bool SamplerStreamingRuntime::prepare_streamed_preloads(PreparedStreamedFile& prepared,
                                                        PulpSamplerLoadResult& result) {
    for (std::uint32_t member = 0; member < prepared.member_count; ++member) {
        const auto& file = prepared.files[member];
        const auto logical_rate = prepared.logical_rates[member];
        if (!file.valid || !file.supports_ranged_read ||
            file.channels != prepared.files[0].channels || !(logical_rate > 0.0) ||
            !std::isfinite(logical_rate) || logical_rate > kMaximumSourceRate) {
            result.status = member == 0 ? PulpSamplerLoadStatus::UnsupportedCodec
                                        : PulpSamplerLoadStatus::PublishFailed;
            return false;
        }
        auto& contract = prepared.contracts[member];
        contract = {
            .source_sample_rate = logical_rate,
            .host_sample_rate = static_cast<double>(host_sample_rate_),
            .maximum_playback_ratio = kMaximumPitchRatio,
            .certified_io_latency_seconds = kCertifiedIoLatencySeconds,
            .scheduler_margin_seconds = kSchedulerMarginSeconds,
            .decoder_latency_seconds = kDecoderLatencySeconds,
            .maximum_host_block_frames = maximum_host_block_frames_,
            .interpolation_guard_frames = audio::kHighQualitySampleSincHalfWidth,
        };
        const auto block_source_frames =
            std::ceil(static_cast<double>(maximum_host_block_frames_) * logical_rate /
                      static_cast<double>(host_sample_rate_) * kMaximumPitchRatio);
        if (!std::isfinite(block_source_frames) ||
            block_source_frames >= static_cast<double>(std::numeric_limits<std::uint64_t>::max())) {
            result.status = PulpSamplerLoadStatus::InvalidPreloadContract;
            return false;
        }
        contract.loop_prefetch_guard_frames = static_cast<std::uint64_t>(block_source_frames);
        const auto required = audio::evaluate_sample_preload_contract(contract);
        if (!required.valid() || required.required_preload_frames == 0) {
            result.status = PulpSamplerLoadStatus::InvalidPreloadContract;
            return false;
        }
        const auto preload_frames = std::min(file.total_frames, required.required_preload_frames);
        contract.configured_preload_frames = preload_frames;
        const auto preload_bytes =
            audio::checked_sample_storage_bytes(file.channels, preload_frames);
        if (!preload_bytes || *preload_bytes > std::numeric_limits<std::uint64_t>::max() -
                                                   result.requested_streaming_memory_bytes) {
            result.status = PulpSamplerLoadStatus::StreamingMemoryBudgetExceeded;
            return false;
        }
        result.requested_streaming_memory_bytes += *preload_bytes;
        auto reservation =
            memory_governor_.reserve(audio::SampleMemoryCategory::Preload, *preload_bytes);
        if (!reservation.acquired()) {
            result.status = reservation.status == audio::SampleMemoryReserveStatus::BudgetExceeded
                                ? PulpSamplerLoadStatus::StreamingMemoryBudgetExceeded
                                : PulpSamplerLoadStatus::InternalFailure;
            return false;
        }
        prepared.preload_leases[member] = std::move(reservation.lease);
        prepared.preloads[member].resize(file.channels, static_cast<std::size_t>(preload_frames));
        if (file.binding.read(0, prepared.preloads[member].view(), preload_frames,
                              std::stop_token{}) != preload_frames) {
            result.status = PulpSamplerLoadStatus::PreloadReadFailed;
            return false;
        }
        prepared.preload_counts[member] = preload_frames;
        if (member == 0) {
            result.channels = file.channels;
            result.sample_rate = static_cast<std::uint32_t>(logical_rate);
            result.total_frames = file.total_frames;
            result.required_preload_frames = required.required_preload_frames;
            result.configured_preload_frames = preload_frames;
        }
    }
    return true;
}

SamplerStreamingRuntime::StagedStreamedFile
SamplerStreamingRuntime::stage_retained_streamed_file() {
    StagedStreamedFile staged;
    if (!retained_stream_recipe_.valid()) {
        staged.result.status = PulpSamplerLoadStatus::NotPrepared;
        return staged;
    }
    auto prepared = std::make_unique<PreparedStreamedFile>();
    prepared->files = retained_stream_recipe_.files;
    prepared->logical_rates = retained_stream_recipe_.logical_rates;
    prepared->octaves = retained_stream_recipe_.octaves;
    prepared->member_count = retained_stream_recipe_.member_count;
    staged.result.codec_capability = PulpSamplerCodecCapability::Ranged;
    staged.result.sidecar_level_count = prepared->member_count - 1;
    staged.result.sidecar_status = prepared->member_count > 1
                                       ? PulpSamplerSidecarStatus::Loaded
                                       : PulpSamplerSidecarStatus::NotPresent;
    if (!prepare_streamed_preloads(*prepared, staged.result))
        return staged;
    staged.result.status = PulpSamplerLoadStatus::NotAttempted;
    staged.prepared = std::move(prepared);
    return staged;
}

SamplerStreamingRuntime::StagedStreamedFile
SamplerStreamingRuntime::stage_streamed_file(std::string_view path) {
    StagedStreamedFile staged;
    auto& result = staged.result;
#if defined(PULP_SAMPLER_TEST_HOOKS)
    file_stage_attempts_for_test_.fetch_add(1, std::memory_order_relaxed);
    if (throw_during_file_stage_for_test_.exchange(false, std::memory_order_acq_rel)) {
        throw std::bad_alloc{};
    }
#endif
    const bool has_sidecar = sampler_stream_mip_sidecar_exists(path);
#if defined(PULP_SAMPLER_TEST_HOOKS)
    file_stage_paused_ack_for_test_.store(
        file_stage_paused_for_test_.load(std::memory_order_acquire), std::memory_order_release);
    while (file_stage_paused_for_test_.load(std::memory_order_acquire) &&
           service_running_.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    file_stage_paused_ack_for_test_.store(false, std::memory_order_release);
#endif
    if (!service_running_.load(std::memory_order_acquire)) {
        result.status = PulpSamplerLoadStatus::ShuttingDown;
        return staged;
    }
    if (!has_registered_audio_reader(path)) {
        result.status = PulpSamplerLoadStatus::UnsupportedCodec;
        return staged;
    }

    std::shared_ptr<audio::MemoryMappedAudioReader> retained_base;
    auto base = audio::make_memory_mapped_frame_reader(
        path, false, false, std::numeric_limits<std::uint64_t>::max(), &retained_base);
    if (!base.valid || retained_base == nullptr) {
        result.status = PulpSamplerLoadStatus::OpenFailed;
        return staged;
    }
    result.channels = base.channels;
    result.sample_rate = base.sample_rate;
    result.total_frames = base.total_frames;
    copy_fixed_name(result.codec_name, retained_base->info().format);
    result.codec_capability = base.supports_ranged_read
                                  ? PulpSamplerCodecCapability::Ranged
                                  : PulpSamplerCodecCapability::DecodeOnceFallback;
    if (!base.supports_ranged_read) {
        result.status = PulpSamplerLoadStatus::DecodeOnceFallbackRejected;
        return staged;
    }
    if (base.channels == 0 || base.channels > kMaximumChannels) {
        result.status = PulpSamplerLoadStatus::UnsupportedChannelCount;
        return staged;
    }
    if (base.sample_rate == 0 || base.sample_rate > kMaximumSourceRate) {
        result.status = PulpSamplerLoadStatus::UnsupportedSampleRate;
        return staged;
    }
    // Rebind the retained snapshot to strict ranged-only reads after capability
    // inspection. The permissive probe must never leak its decode-once fallback
    // into an admitted streaming source or copy a large source twice.
    base = audio::make_retained_memory_mapped_frame_reader(
        retained_base, true, has_sidecar);
    if (!base.valid || retained_base == nullptr) {
        result.status = PulpSamplerLoadStatus::OpenFailed;
        return staged;
    }
    // The file may have been replaced between the capability probe and
    // strict reopen. Validate and report the handle that will actually be
    // registered, rather than retaining probe-era metadata.
    result.channels = base.channels;
    result.sample_rate = base.sample_rate;
    result.total_frames = base.total_frames;
    copy_fixed_name(result.codec_name, retained_base->info().format);
    result.codec_capability = PulpSamplerCodecCapability::Ranged;
    if (base.channels == 0 || base.channels > kMaximumChannels) {
        result.status = PulpSamplerLoadStatus::UnsupportedChannelCount;
        return staged;
    }
    if (base.sample_rate == 0 || base.sample_rate > kMaximumSourceRate) {
        result.status = PulpSamplerLoadStatus::UnsupportedSampleRate;
        return staged;
    }

    auto sidecar = has_sidecar ? load_sampler_stream_mip_sidecar(path, base, retained_base)
                               : SamplerStreamMipSidecar{};
    result.sidecar_status = sidecar.status == SamplerStreamMipSidecarStatus::Valid
                                ? PulpSamplerSidecarStatus::Loaded
                            : sidecar.status == SamplerStreamMipSidecarStatus::Invalid
                                ? PulpSamplerSidecarStatus::IgnoredInvalid
                                : PulpSamplerSidecarStatus::NotPresent;
    auto prepared = std::make_unique<PreparedStreamedFile>();
    auto& files = prepared->files;
    auto& logical_rates = prepared->logical_rates;
    auto& octaves = prepared->octaves;
    prepared->member_count = 1;
    logical_rates[0] = static_cast<double>(base.sample_rate);
    files[0] = std::move(base);
    if (sidecar.status == SamplerStreamMipSidecarStatus::Valid) {
        prepared->member_count += sidecar.level_count;
        result.sidecar_level_count = sidecar.level_count;
        for (std::uint32_t level = 0; level < sidecar.level_count; ++level) {
            files[level + 1] = std::move(sidecar.levels[level].reader);
            logical_rates[level + 1] = sidecar.levels[level].sample_rate;
            octaves[level + 1] = sidecar.levels[level].octave;
        }
    }

    if (!prepare_streamed_preloads(*prepared, result))
        return staged;
    if (!service_running_.load(std::memory_order_acquire)) {
        result.status = PulpSamplerLoadStatus::ShuttingDown;
        return staged;
    }
    result.status = PulpSamplerLoadStatus::NotAttempted;
    staged.prepared = std::move(prepared);
    return staged;
}

PulpSamplerLoadStatus
SamplerStreamingRuntime::publish_streamed_file(PreparedStreamedFile& prepared) {
    StreamedSlot* slot = nullptr;
    std::size_t slot_index = slots_.size();
    for (std::size_t index = 0; index < slots_.size(); ++index) {
        if (slots_[index] && !slots_[index]->occupied) {
            slot = slots_[index].get();
            slot_index = index;
            break;
        }
    }
    if (slot == nullptr)
        return PulpSamplerLoadStatus::BundleCapacityExceeded;

    StreamedSourceRecipe candidate_recipe;
    try {
        candidate_recipe.files = prepared.files;
        candidate_recipe.logical_rates = prepared.logical_rates;
        candidate_recipe.octaves = prepared.octaves;
        candidate_recipe.member_count = prepared.member_count;
    } catch (...) {
        return PulpSamplerLoadStatus::AllocationFailure;
    }

    for (auto& member : slot->members) {
        member.asset.release();
        member.source = {};
        member.present = false;
        member.retirement_scheduled = false;
    }
    slot->member_count = 0;
    slot->selection_generation = 0;

    auto& files = prepared.files;
    auto& logical_rates = prepared.logical_rates;
    auto& octaves = prepared.octaves;
    auto& contracts = prepared.contracts;
    auto& preloads = prepared.preloads;
    auto& preload_leases = prepared.preload_leases;
    auto& preload_counts = prepared.preload_counts;
    const auto member_count = prepared.member_count;

#if defined(PULP_SAMPLER_TEST_HOOKS)
    if (block_next_reverse_decode_for_test_.exchange(false, std::memory_order_acq_rel)) {
        auto reader = std::move(files[0].binding.read);
        files[0].binding.read = [this, reader = std::move(reader)](
                                    std::uint64_t start, audio::BufferView<float> destination,
                                    std::uint64_t frames, std::stop_token stop_token) {
            reverse_decode_entered_for_test_.store(true, std::memory_order_release);
            while (!release_reverse_decode_for_test_.load(std::memory_order_acquire) &&
                   service_running_.load(std::memory_order_acquire) &&
                   !stop_token.stop_requested()) {
                std::this_thread::yield();
            }
            if (!service_running_.load(std::memory_order_acquire) || stop_token.stop_requested()) {
                return std::uint64_t{0};
            }
            return reader(start, destination, frames, stop_token);
        };
        files[0].binding.stop_mode = audio::FrameReaderStopMode::Cooperative;
    }
#endif

    std::array<audio::SampleStreamCacheSourceView, kMaximumBundleMembers> stream_views{};
    try {
        for (std::uint32_t member = 0; member < member_count; ++member) {
#if defined(PULP_SAMPLER_TEST_HOOKS)
            if (fail_after_stream_member_count_for_test_.load(std::memory_order_acquire) ==
                static_cast<int>(member)) {
                discard_unpublished_slot(*slot);
                return PulpSamplerLoadStatus::SourceRegistrationFailed;
            }
            auto reader = std::move(files[member].binding.read);
            files[member].binding.read = [this, reader = std::move(reader)](
                                             std::uint64_t start,
                                             audio::BufferView<float> destination,
                                             std::uint64_t frames, std::stop_token stop_token) {
                if (fail_next_stream_decode_for_test_.exchange(false, std::memory_order_acq_rel)) {
                    return std::uint64_t{0};
                }
                return reader(start, destination, frames, stop_token);
            };
#endif
            const auto source = take_source_token(
                slot_index * kMaximumBundleMembers + member);
            if (!source) {
                discard_unpublished_slot(*slot);
                return PulpSamplerLoadStatus::SourceRegistrationFailed;
            }
            const auto added = service_.add_source(
                {
                    .token = *source,
                    .channels = files[member].channels,
                    .total_frames = files[member].total_frames,
                    .page_frames = page_frames_,
                    .cache_page_count = kCachePagesPerSource,
                },
                std::move(files[member].binding));
            if (!added.added()) {
                discard_unpublished_slot(*slot);
                return PulpSamplerLoadStatus::SourceRegistrationFailed;
            }
            auto& owned = slot->members[member];
            owned.source = *source;
            owned.present = true;
            stream_views[member] = added.view;
        }
#if defined(PULP_SAMPLER_TEST_HOOKS)
        if (fail_after_stream_member_count_for_test_.load(std::memory_order_acquire) ==
            static_cast<int>(member_count)) {
            discard_unpublished_slot(*slot);
            return PulpSamplerLoadStatus::SourceRegistrationFailed;
        }
#endif

        if (preload_counts[0] < files[0].total_frames) {
            const auto prewarm = prewarm_reverse_entry_pages(slot->members[0].source,
                                                             stream_views[0], files[0].total_frames,
                                                             preload_counts[0], logical_rates[0]);
            if (prewarm != ReversePrewarmStatus::Ready) {
                discard_unpublished_slot(*slot);
                if (prewarm == ReversePrewarmStatus::TimedOut)
                    return PulpSamplerLoadStatus::ReversePrewarmTimedOut;
                if (prewarm == ReversePrewarmStatus::ShuttingDown)
                    return PulpSamplerLoadStatus::ShuttingDown;
                return PulpSamplerLoadStatus::PublishFailed;
            }
        }

        for (std::uint32_t member = 0; member < member_count; ++member) {
            const audio::SampleAssetConfig asset_config{
                .asset = {next_asset_id_++, 1},
                .source = slot->members[member].source,
                .channels = files[member].channels,
                .total_frames = files[member].total_frames,
                .sample_rate = logical_rates[member],
                .preload_frames = preload_counts[member],
                .preload_contract = contracts[member],
                .stream_source = stream_views[member],
            };
            if (!slot->members[member].asset.prepare_owned(
                    asset_config, std::move(preloads[member]), std::move(preload_leases[member]))) {
                discard_unpublished_slot(*slot);
                return PulpSamplerLoadStatus::PublishFailed;
            }
        }

#if defined(PULP_SAMPLER_TEST_HOOKS)
        bundle_publish_paused_ack_for_test_.store(
            pause_before_bundle_publish_for_test_.load(std::memory_order_acquire),
            std::memory_order_release);
        while (pause_before_bundle_publish_for_test_.load(std::memory_order_acquire) &&
               service_running_.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        bundle_publish_paused_ack_for_test_.store(false, std::memory_order_release);
#endif

        std::lock_guard lock(file_request_mutex_);
        if (!service_running_.load(std::memory_order_acquire)) {
            discard_unpublished_slot(*slot);
            return PulpSamplerLoadStatus::ShuttingDown;
        }
        SamplerStreamMipPyramidView streamed_mips;
        streamed_mips.level_count = member_count - 1;
        for (std::uint32_t member = 1; member < member_count; ++member) {
            streamed_mips.levels[member - 1] = {
                .asset = slot->members[member].asset.view(),
                .octave = octaves[member],
            };
        }
        const auto selection_generation = next_selection_generation();
        if (!selection_generation) {
            discard_unpublished_slot(*slot);
            return PulpSamplerLoadStatus::GenerationExhausted;
        }
        slot->member_count = member_count;
        slot->selection_generation = *selection_generation;
        slot->occupied = true;
        published_source_.write({
            .kind = SamplerPublishedSourceKind::Streamed,
            .selection_generation = slot->selection_generation,
            .streamed = slot->members[0].asset.view(),
            .streamed_mips = streamed_mips,
        });
        retained_stream_recipe_ = std::move(candidate_recipe);
        active_sources_.fetch_add(1, std::memory_order_relaxed);
        preload_frames_.store(preload_counts[0], std::memory_order_relaxed);
        return PulpSamplerLoadStatus::Ok;
    } catch (const std::bad_alloc&) {
        discard_unpublished_slot(*slot);
        return PulpSamplerLoadStatus::AllocationFailure;
    } catch (...) {
        discard_unpublished_slot(*slot);
        return PulpSamplerLoadStatus::InternalFailure;
    }
}

SamplerStreamingRuntime::ReversePrewarmStatus SamplerStreamingRuntime::prewarm_reverse_entry_pages(
    audio::SampleStreamSourceToken source, const audio::SampleStreamCacheSourceView& view,
    std::uint64_t total_frames, std::uint64_t preload_frames, double sample_rate) noexcept {
    const auto tail_frame = total_frames - 1;
    const auto first_frame = total_frames > preload_frames ? total_frames - preload_frames : 0;
    const auto first_page = first_frame / page_frames_;
    const auto last_page = tail_frame / page_frames_;
    const auto horizon_page_count = last_page - first_page + 1;
    if (horizon_page_count > kCachePagesPerSource)
        return ReversePrewarmStatus::ScheduleFailed;
    const audio::SampleStreamRequesterToken requester{kAdmissionRequesterId, source.source_id};
    for (auto page = first_page; page <= last_page; ++page) {
        const auto page_end = page == last_page ? total_frames : (page + 1) * page_frames_;
        const auto scheduled = service_.request_page({
            .source = source,
            .requester = requester,
            .page_index = page,
            .resident_source_frames = tail_frame - (page_end - 1),
            .consumption_frames_per_second = static_cast<double>(sample_rate) * kMaximumPitchRatio,
            .demand_class = audio::SampleStreamDemandClass::Attack,
        });
        if (scheduled != audio::SampleStreamScheduleStatus::Inserted &&
            scheduled != audio::SampleStreamScheduleStatus::Refreshed) {
            (void)service_.cancel_requester(requester);
            return ReversePrewarmStatus::ScheduleFailed;
        }
    }
#if defined(PULP_SAMPLER_TEST_HOOKS)
    reverse_prewarm_pending_for_test_.store(true, std::memory_order_release);
#endif

    auto prewarm_timeout =
        reverse_prewarm_timeout_for_pages(static_cast<std::uint32_t>(horizon_page_count));
#if defined(PULP_SAMPLER_TEST_HOOKS)
    if (reverse_prewarm_timeout_override_for_test_)
        prewarm_timeout = reverse_prewarm_timeout_for_test_;
#endif
    const auto deadline = std::chrono::steady_clock::now() + prewarm_timeout;
    const auto reverse_horizon_ready = [&] {
        for (auto page = first_page; page <= last_page; ++page) {
            const auto frame = std::min(tail_frame, page * page_frames_);
            if (!view.window->ready_page_for_frame(source.source_generation, frame).valid) {
                return false;
            }
        }
        return true;
    };
    while (service_running_.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        service_.drain_commands(commands_);
        service_.drain_completions();
        if (reverse_horizon_ready()) {
            (void)service_.cancel_requester(requester);
#if defined(PULP_SAMPLER_TEST_HOOKS)
            reverse_prewarm_pending_for_test_.store(false, std::memory_order_release);
#endif
            return service_running_.load(std::memory_order_acquire)
                       ? ReversePrewarmStatus::Ready
                       : ReversePrewarmStatus::ShuttingDown;
        }
        if (!service_dispatch_paused_.load(std::memory_order_acquire)) {
            for (std::uint32_t attempt = 0; attempt < kSourceCapacity * 2; ++attempt) {
                if (!dispatch_made_owner_progress(service_.dispatch_once())) {
                    break;
                }
            }
        }
        std::this_thread::yield();
    }
    service_.drain_completions();
    const bool ready = reverse_horizon_ready();
    (void)service_.cancel_requester(requester);
#if defined(PULP_SAMPLER_TEST_HOOKS)
    reverse_prewarm_pending_for_test_.store(false, std::memory_order_release);
#endif
    if (!service_running_.load(std::memory_order_acquire))
        return ReversePrewarmStatus::ShuttingDown;
    return ready ? ReversePrewarmStatus::Ready : ReversePrewarmStatus::TimedOut;
}

std::chrono::milliseconds
SamplerStreamingRuntime::reverse_prewarm_timeout_for_pages(std::uint32_t page_count) noexcept {
    constexpr auto kMinimumTimeout = std::chrono::milliseconds{250};
    const auto per_page_milliseconds = std::ceil(
        1000.0 * (kCertifiedIoLatencySeconds + kSchedulerMarginSeconds + kDecoderLatencySeconds));
    const auto derived =
        std::chrono::milliseconds{static_cast<std::int64_t>(per_page_milliseconds * page_count)};
    return std::max(kMinimumTimeout, derived);
}

void SamplerStreamingRuntime::discard_unpublished_slot(StreamedSlot& slot) noexcept {
    for (auto& member : slot.members)
        member.asset.release();
    for (auto& member : slot.members) {
        if (member.present)
            rollback_unpublished_source(member.source);
        member.source = {};
        member.present = false;
        member.retirement_scheduled = false;
    }
    slot.member_count = 0;
    slot.selection_generation = 0;
    slot.occupied = false;
}

void SamplerStreamingRuntime::rollback_unpublished_source(
    audio::SampleStreamSourceToken source) noexcept {
#if defined(PULP_SAMPLER_TEST_HOOKS)
    unpublished_rollback_attempts_for_test_.fetch_add(1, std::memory_order_relaxed);
#endif
    (void)service_.cancel_pending_source_demands(source);
    service_.drain_completions();
    if (service_.discard_unpublished_source(source))
        return;
    for (auto& pending : unpublished_rollbacks_) {
        if (pending.source_id == 0) {
            pending = source;
#if defined(PULP_SAMPLER_TEST_HOOKS)
            unpublished_rollback_count_for_test_.fetch_add(1, std::memory_order_release);
#endif
            return;
        }
    }
}

void SamplerStreamingRuntime::collect_unpublished_rollbacks() noexcept {
    for (auto& pending : unpublished_rollbacks_) {
        if (pending.source_id == 0)
            continue;
        (void)service_.cancel_pending_source_demands(pending);
        if (service_.discard_unpublished_source(pending)) {
            pending = {};
#if defined(PULP_SAMPLER_TEST_HOOKS)
            unpublished_rollback_count_for_test_.fetch_sub(1, std::memory_order_release);
#endif
        }
    }
}

void SamplerStreamingRuntime::retire_sources() noexcept {
    const auto current = published_source_.read();
    const auto acknowledged = stream_audio_ack_selection_.load(std::memory_order_acquire);
    for (auto& owned : slots_) {
        if (!owned || !owned->occupied)
            continue;
        auto& slot = *owned;
        const bool current_slot = current.kind == SamplerPublishedSourceKind::Streamed &&
                                  current.selection_generation == slot.selection_generation;
        if (!current_slot && acknowledged > slot.selection_generation) {
            for (auto& member : slot.members)
                member.asset.release();
            for (auto& member : slot.members) {
                if (!member.present || member.retirement_scheduled)
                    continue;
                const auto status = service_.retire_source_after_asset_unpublish(member.source);
                if (status == audio::SampleStreamSourceRetireStatus::Scheduled ||
                    status == audio::SampleStreamSourceRetireStatus::AlreadyScheduled ||
                    status == audio::SampleStreamSourceRetireStatus::StaleSource) {
                    member.retirement_scheduled = true;
                }
            }
        }
    }

    (void)service_.collect_retired_sources();
    for (auto& owned : slots_) {
        if (!owned || !owned->occupied)
            continue;
        bool all_retired = true;
        for (const auto& member : owned->members) {
            if (member.present && service_.cache_service().contains_source(member.source)) {
                all_retired = false;
                break;
            }
        }
        if (!all_retired)
            continue;
        for (auto& member : owned->members) {
            member.source = {};
            member.present = false;
            member.retirement_scheduled = false;
        }
        owned->member_count = 0;
        owned->selection_generation = 0;
        owned->occupied = false;
        sources_retired_.fetch_add(1, std::memory_order_relaxed);
        active_sources_.fetch_sub(1, std::memory_order_relaxed);
    }
}

} // namespace pulp::examples
