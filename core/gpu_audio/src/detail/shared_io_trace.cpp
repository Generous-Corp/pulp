#include "shared_io_trace.hpp"

#include <pulp/runtime/trace.hpp>

#include <limits>

namespace pulp::gpu_audio::detail {

bool SharedIoTraceRecord::valid() const noexcept {
    if (static_cast<unsigned>(kind) > static_cast<unsigned>(SharedIoTraceKind::Recovery) ||
        generation == 0 || valid_stages >> kSharedIoTraceStageCount != 0 ||
        static_cast<unsigned>(outcome) > static_cast<unsigned>(SharedIoTraceOutcome::Cancelled) ||
        static_cast<unsigned>(reason) >
            static_cast<unsigned>(SharedIoFallbackReason::CompletionFailed) ||
        static_cast<unsigned>(gpu_reason) >
            static_cast<unsigned>(SharedIoFallbackReason::CompletionFailed) ||
        static_cast<unsigned>(delivery_reason) >
            static_cast<unsigned>(SharedIoFallbackReason::CompletionFailed) ||
        static_cast<unsigned>(gpu_terminal) >
            static_cast<unsigned>(SharedIoGpuTerminalDisposition::CancelledTeardown) ||
        static_cast<unsigned>(delivery) >
            static_cast<unsigned>(SharedIoDeliveryDisposition::InvalidRejected))
        return false;
    if (kind == SharedIoTraceKind::Eligible || kind == SharedIoTraceKind::Recovery)
        return !gpu_work_admitted && !output_eligible &&
               gpu_terminal == SharedIoGpuTerminalDisposition::None &&
               delivery == SharedIoDeliveryDisposition::None &&
               (kind != SharedIoTraceKind::Recovery || next_generation > generation);
    if ((kind == SharedIoTraceKind::Terminal && (!gpu_work_admitted || output_eligible)) ||
        (kind == SharedIoTraceKind::Delivery && (gpu_work_admitted || !output_eligible)))
        return false;
    if (gpu_work_admitted != (gpu_terminal != SharedIoGpuTerminalDisposition::None))
        return false;
    if (output_eligible != (delivery != SharedIoDeliveryDisposition::None))
        return false;
    if (gpu_terminal == SharedIoGpuTerminalDisposition::CompletedAccepted &&
        outcome != SharedIoTraceOutcome::Success)
        return false;
    if ((gpu_terminal == SharedIoGpuTerminalDisposition::DeviceLost) !=
        (outcome == SharedIoTraceOutcome::DeviceLost))
        return false;
    if ((gpu_terminal == SharedIoGpuTerminalDisposition::CancelledTeardown) !=
        (outcome == SharedIoTraceOutcome::Cancelled))
        return false;
    if (gpu_terminal == SharedIoGpuTerminalDisposition::LateRejected &&
        outcome != SharedIoTraceOutcome::LateRejected)
        return false;
    if (gpu_terminal == SharedIoGpuTerminalDisposition::StaleRejected &&
        outcome != SharedIoTraceOutcome::StaleRejected)
        return false;
    const bool provider_failure = outcome == SharedIoTraceOutcome::SubmissionRejected ||
                                  outcome == SharedIoTraceOutcome::CompletionFailed;
    if ((gpu_terminal == SharedIoGpuTerminalDisposition::ProviderFailed) != provider_failure)
        return false;
    const auto summary_reason =
        delivery_reason != SharedIoFallbackReason::None ? delivery_reason : gpu_reason;
    if (reason != summary_reason)
        return false;
    bool seen = false;
    std::uint64_t previous = 0;
    for (std::size_t i = 0; i < cpu_ns.size(); ++i) {
        if ((valid_stages & (1u << i)) == 0)
            continue;
        if (cpu_ns[i] > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
            (seen && cpu_ns[i] < previous))
            return false;
        previous = cpu_ns[i];
        seen = true;
    }
    return (seen || kind == SharedIoTraceKind::Delivery) &&
           (!gpu_elapsed_available ||
            gpu_elapsed_ns <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()));
}

const char* shared_io_gpu_terminal_name(SharedIoGpuTerminalDisposition value) noexcept {
    switch (value) {
    case SharedIoGpuTerminalDisposition::None:
        return "none";
    case SharedIoGpuTerminalDisposition::CompletedAccepted:
        return "completed_accepted";
    case SharedIoGpuTerminalDisposition::StaleRejected:
        return "stale_rejected";
    case SharedIoGpuTerminalDisposition::LateRejected:
        return "late_rejected";
    case SharedIoGpuTerminalDisposition::ProviderFailed:
        return "provider_failed";
    case SharedIoGpuTerminalDisposition::DeviceLost:
        return "device_lost";
    case SharedIoGpuTerminalDisposition::CancelledTeardown:
        return "cancelled_teardown";
    }
    return "unknown";
}

const char* shared_io_delivery_name(SharedIoDeliveryDisposition value) noexcept {
    switch (value) {
    case SharedIoDeliveryDisposition::None:
        return "none";
    case SharedIoDeliveryDisposition::GpuDelivered:
        return "gpu_delivered";
    case SharedIoDeliveryDisposition::CpuFallbackDelivered:
        return "cpu_fallback_delivered";
    case SharedIoDeliveryDisposition::SilenceDelivered:
        return "silence_delivered";
    case SharedIoDeliveryDisposition::PassthroughDelivered:
        return "passthrough_delivered";
    case SharedIoDeliveryDisposition::Priming:
        return "priming";
    case SharedIoDeliveryDisposition::InvalidRejected:
        return "invalid_rejected";
    }
    return "unknown";
}

const char* shared_io_trace_outcome_name(SharedIoTraceOutcome value) noexcept {
    switch (value) {
    case SharedIoTraceOutcome::Success:
        return "success";
    case SharedIoTraceOutcome::SubmissionRejected:
        return "submission_rejected";
    case SharedIoTraceOutcome::CompletionFailed:
        return "completion_failed";
    case SharedIoTraceOutcome::DeadlineExceeded:
        return "deadline_exceeded";
    case SharedIoTraceOutcome::StaleRejected:
        return "stale_rejected";
    case SharedIoTraceOutcome::LateRejected:
        return "late_rejected";
    case SharedIoTraceOutcome::DeviceLost:
        return "device_lost";
    case SharedIoTraceOutcome::Cancelled:
        return "cancelled";
    }
    return "unknown";
}

const char* shared_io_fallback_reason_name(SharedIoFallbackReason value) noexcept {
    switch (value) {
    case SharedIoFallbackReason::None:
        return "none";
    case SharedIoFallbackReason::UnsupportedProvider:
        return "unsupported_provider";
    case SharedIoFallbackReason::UnsupportedFeature:
        return "unsupported_feature";
    case SharedIoFallbackReason::ProviderMismatch:
        return "provider_mismatch";
    case SharedIoFallbackReason::AllocationFailed:
        return "allocation_failed";
    case SharedIoFallbackReason::DeviceLost:
        return "device_lost";
    case SharedIoFallbackReason::Timeout:
        return "timeout";
    case SharedIoFallbackReason::SubmissionRejected:
        return "submission_rejected";
    case SharedIoFallbackReason::DeadlineExceeded:
        return "deadline_exceeded";
    case SharedIoFallbackReason::InputSaturated:
        return "input_saturated";
    case SharedIoFallbackReason::SequenceGap:
        return "sequence_gap";
    case SharedIoFallbackReason::CompletionFailed:
        return "completion_failed";
    case SharedIoFallbackReason::Teardown:
        return "teardown";
    }
    return "unknown";
}

SharedIoTraceDuration shared_io_trace_duration(const SharedIoTraceRecord& record,
                                               SharedIoTraceStage begin,
                                               SharedIoTraceStage end) noexcept {
    if (!record.has(begin) || !record.has(end))
        return {};
    const auto a = record.cpu_ns[static_cast<std::size_t>(begin)];
    const auto b = record.cpu_ns[static_cast<std::size_t>(end)];
    if (b < a)
        return {};
    return {b - a, true};
}

bool SharedIoTraceRecorder::publish_completed(const SharedIoTraceRecord& record) noexcept {
    return publish(record, false);
}

bool SharedIoTraceRecorder::publish_callback(const SharedIoTraceRecord& record) noexcept {
    return publish(record, true);
}

bool SharedIoTraceRecorder::publish(const SharedIoTraceRecord& record, bool callback) noexcept {
    if (!enabled())
        return false;
    attempted_.fetch_add(1, std::memory_order_relaxed);
    if (record.generation != config_.generation || !record.valid() ||
        callback != (record.kind == SharedIoTraceKind::Eligible ||
                     record.kind == SharedIoTraceKind::Delivery)) {
        invalid_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const bool ordinary_success =
        record.outcome == SharedIoTraceOutcome::Success &&
        record.reason == SharedIoFallbackReason::None &&
        record.gpu_reason == SharedIoFallbackReason::None &&
        record.delivery_reason == SharedIoFallbackReason::None &&
        (record.gpu_terminal == SharedIoGpuTerminalDisposition::CompletedAccepted ||
         record.delivery == SharedIoDeliveryDisposition::GpuDelivered);
    if (ordinary_success && record.sequence % config_.success_stride != 0) {
        sampled_out_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (!(callback ? callback_queue_ : queue_).try_push(record))
        return false;
    enqueued_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool SharedIoTraceRecorder::publish_admission(std::uint64_t generation,
                                              std::uint64_t sequence) noexcept {
    if (!enabled() || !config_.capture_admissions)
        return false;
    admissions_attempted_.fetch_add(1, std::memory_order_relaxed);
    if (generation != config_.generation) {
        invalid_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (!admissions_.try_push({generation, sequence}))
        return false;
    admissions_enqueued_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

SharedIoTraceStats SharedIoTraceRecorder::stats() const noexcept {
    return {.admissions_attempted = admissions_attempted_.load(std::memory_order_relaxed),
            .admissions_enqueued = admissions_enqueued_.load(std::memory_order_relaxed),
            .admissions_dropped = admissions_.overflow_count(),
            .admissions_drained = admissions_drained_.load(std::memory_order_relaxed),
            .attempted = attempted_.load(std::memory_order_relaxed),
            .sampled_out = sampled_out_.load(std::memory_order_relaxed),
            .invalid = invalid_.load(std::memory_order_relaxed),
            .enqueued = enqueued_.load(std::memory_order_relaxed),
            .dropped = queue_.overflow_count() + callback_queue_.overflow_count(),
            .drained = drained_.load(std::memory_order_relaxed)};
}

namespace {
[[maybe_unused]] std::int64_t elapsed(const SharedIoTraceRecord& record, SharedIoTraceStage begin,
                                      SharedIoTraceStage end) noexcept {
    const auto value = shared_io_trace_duration(record, begin, end);
    return value.available ? static_cast<std::int64_t>(value.ns) : -1;
}

void emit_record([[maybe_unused]] const SharedIoTraceConfig& config,
                 [[maybe_unused]] const SharedIoTraceRecord& r) noexcept {
    using S [[maybe_unused]] = SharedIoTraceStage;
    if (r.kind == SharedIoTraceKind::Eligible) {
        PULP_TRACE_INSTANT_ARGS("gpu", "gpu.audio.eligible", "schema", 2, "engine_id",
                                config.engine_id, "generation", r.generation, "sequence",
                                r.sequence);
        return;
    }
    if (r.kind == SharedIoTraceKind::Delivery) {
        PULP_TRACE_INSTANT_ARGS("gpu", "gpu.audio.delivery", "schema", 2, "engine_id",
                                config.engine_id, "generation", r.generation, "sequence",
                                r.sequence, "output_eligible", true, "delivery",
                                shared_io_delivery_name(r.delivery), "delivery_reason",
                                shared_io_fallback_reason_name(r.delivery_reason));
        return;
    }
    if (r.kind == SharedIoTraceKind::Recovery) {
        PULP_TRACE_INSTANT_ARGS("gpu", "gpu.audio.recovery", "schema", 2, "engine_id",
                                config.engine_id, "generation", r.generation, "sequence",
                                r.sequence, "next_generation", r.next_generation, "quiescent",
                                true);
        return;
    }
    PULP_TRACE_INSTANT_ARGS(
        "gpu", "gpu.audio.terminal", "schema", 2, "engine_id", config.engine_id, "generation",
        r.generation, "sequence", r.sequence, "outcome", shared_io_trace_outcome_name(r.outcome),
        "reason", shared_io_fallback_reason_name(r.reason), "gpu_reason",
        shared_io_fallback_reason_name(r.gpu_reason), "delivery_reason",
        shared_io_fallback_reason_name(r.delivery_reason), "valid_stages", r.valid_stages,
        "gpu_work_admitted", r.gpu_work_admitted, "output_eligible", r.output_eligible,
        "gpu_terminal", shared_io_gpu_terminal_name(r.gpu_terminal), "delivery",
        shared_io_delivery_name(r.delivery), "scheduled_ns",
        r.has(S::Scheduled) ? static_cast<std::int64_t>(r.cpu_ns[0]) : -1, "worker_entry_ns",
        r.has(S::WorkerEntry) ? static_cast<std::int64_t>(r.cpu_ns[1]) : -1,
        "completion_observed_ns",
        r.has(S::CompletionObserved) ? static_cast<std::int64_t>(r.cpu_ns[6]) : -1, "admission_ns",
        elapsed(r, S::Scheduled, S::WorkerEntry), "encode_ns",
        elapsed(r, S::EncodeBegin, S::EncodeEnd), "submit_call_ns",
        elapsed(r, S::SubmitBegin, S::SubmitEnd), "pre_submit_ns",
        elapsed(r, S::Scheduled, S::SubmitBegin), "submit_to_observed_ns",
        elapsed(r, S::SubmitBegin, S::CompletionObserved), "scheduled_to_observed_ns",
        elapsed(r, S::Scheduled, S::CompletionObserved), "gpu_elapsed_available",
        r.gpu_elapsed_available, "gpu_elapsed_ns",
        r.gpu_elapsed_available ? static_cast<std::int64_t>(r.gpu_elapsed_ns) : -1);
}

void emit_admission([[maybe_unused]] const SharedIoTraceConfig& config,
                    [[maybe_unused]] const SharedIoTraceAdmission& admission) noexcept {
    PULP_TRACE_INSTANT_ARGS("gpu", "gpu.audio.admission", "schema", 2, "engine_id",
                            config.engine_id, "generation", admission.generation, "sequence",
                            admission.sequence);
}
} // namespace

SharedIoTraceDrainResult
drain_shared_io_trace(SharedIoTraceRecorder& recorder,
                      [[maybe_unused]] const SharedIoTelemetrySnapshot& telemetry,
                      std::uint32_t budget) noexcept {
    SharedIoTraceDrainResult result;
    result.tracing_compiled = runtime::kTracingEnabled;
    result.recording_enabled = recorder.enabled();
    if (!result.recording_enabled)
        return result;
    [[maybe_unused]] const auto& config = recorder.config();
    // Repeat metadata at each bounded drain: enabling capture after prepare
    // must still reveal the exact generation and sampling policy.
    PULP_TRACE_INSTANT_ARGS(
        "gpu", "gpu.audio.session", "schema", 2, "engine_id", config.engine_id, "generation",
        config.generation, "path", static_cast<unsigned>(config.contract.active_path), "block_size",
        config.contract.block_size, "sample_rate", config.contract.sample_rate, "channels",
        config.contract.channels, "lead_blocks", config.contract.algorithmic_lead_blocks,
        "pipeline_depth", config.contract.pipeline_depth, "success_stride", config.success_stride,
        "capture_admissions", config.capture_admissions, "cpu_clock", "worker.monotonic",
        "event_time", "drain", "gpu_clock_mapped", false);
    const auto emit_terminal = [&](const SharedIoTraceRecord& record) {
        emit_record(config, record);
    };
    const auto emit_admitted = [&](const SharedIoTraceAdmission& admission) {
        emit_admission(config, admission);
    };
    std::uint32_t admissions_drained = 0;
    const bool terminals_first = recorder.terminals_first_for_next_drain();
    const auto first_budget = budget == 0 ? 0 : (budget + 1) / 2;
    if (terminals_first)
        result.records_drained = recorder.drain_worker_records(first_budget, emit_terminal);
    else
        admissions_drained = recorder.drain_admissions(first_budget, emit_admitted);
    auto remaining = budget - result.records_drained - admissions_drained;
    if (terminals_first)
        admissions_drained += recorder.drain_admissions(remaining, emit_admitted);
    else
        result.records_drained += recorder.drain_worker_records(remaining, emit_terminal);
    remaining = budget - result.records_drained - admissions_drained;
    if (terminals_first)
        result.records_drained += recorder.drain_worker_records(remaining, emit_terminal);
    else
        admissions_drained += recorder.drain_admissions(remaining, emit_admitted);
    [[maybe_unused]] const auto stats = recorder.stats();
    PULP_TRACE_INSTANT_ARGS(
        "gpu", "gpu.audio.counters", "schema", 2, "engine_id", config.engine_id, "generation",
        config.generation, "capture_admissions", config.capture_admissions, "admissions_attempted",
        stats.admissions_attempted, "admissions_enqueued", stats.admissions_enqueued,
        "admissions_dropped", stats.admissions_dropped, "admissions_drained",
        stats.admissions_drained, "attempted", stats.attempted, "sampled_out", stats.sampled_out,
        "invalid", stats.invalid, "enqueued", stats.enqueued, "dropped", stats.dropped, "drained",
        stats.drained, "callback_blocks", telemetry.callback_blocks, "submitted_blocks",
        telemetry.submitted_blocks, "deadline_misses", telemetry.deadline_misses, "fallback_blocks",
        telemetry.fallback_blocks, "late_completions", telemetry.late_completions, "resync_drops",
        telemetry.resync_drops, "input_drops", telemetry.input_drops, "payload_bytes_copied",
        telemetry.payload_bytes_copied);
    if constexpr (runtime::kTracingEnabled)
        result.emission_attempts = admissions_drained + result.records_drained + 2;
    return result;
}

} // namespace pulp::gpu_audio::detail
