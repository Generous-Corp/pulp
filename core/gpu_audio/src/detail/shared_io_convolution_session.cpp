#include "shared_io_convolution_session.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>

namespace pulp::gpu_audio::detail {

namespace {

bool convolution_geometry_valid(const SharedIoConvolutionPipeline::Config& config) noexcept {
    return config.capacity != 0 && config.channels != 0 && config.block_size != 0 &&
           config.fft_size != 0 && config.ir_length != 0 &&
           std::uint64_t(config.block_size) + config.ir_length - 1u <= config.fft_size;
}

std::optional<std::size_t>
complex_slot_bytes(const SharedIoConvolutionPipeline::Config& config) noexcept {
    const auto floats = std::uint64_t(config.channels) * config.fft_size * 2u;
    if (floats > std::numeric_limits<std::size_t>::max() / sizeof(float))
        return std::nullopt;
    return static_cast<std::size_t>(floats) * sizeof(float);
}

} // namespace

SharedIoConvolutionSession::~SharedIoConvolutionSession() {
    (void)release();
}

std::uint64_t SharedIoConvolutionSession::trace_now_ns() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

SharedIoExecutionContract SharedIoConvolutionSession::execution_contract() const noexcept {
    SharedIoExecutionContract contract;
    contract.channels = config_.pipeline.channels;
    contract.block_size = config_.pipeline.block_size;
    contract.sample_rate = config_.sample_rate;
    contract.algorithmic_lead_blocks = config_.pipeline.lead_blocks;
    contract.pipeline_depth = config_.pipeline.capacity;
    contract.provider_slots = config_.slots;
    contract.requested_path = config_.requested_path;
    contract.active_path = config_.active_path;
    contract.miss_policy = config_.miss_policy;
    contract.shared_host_pointer_capable = config_.shared_host_pointer_capable;
    contract.cpu_fallback_prepared = config_.cpu_fallback_prepared;
    return contract;
}

bool SharedIoConvolutionSession::prepare_trace_generation() noexcept {
    trace_recorder_.reset();
    trace_telemetry_.reset();
    trace_template_ = config_.trace;
    if (!trace_template_.enabled)
        return true;

    auto contract = execution_contract();
    trace_template_.contract = contract;
    trace_template_.generation = plan_.preparation_epoch();
    try {
        trace_recorder_ = std::make_unique<SharedIoTraceRecorder>(trace_template_);
    } catch (...) {
        trace_recorder_.reset();
        return true;
    }
    if (!trace_recorder_->enabled()) {
        trace_recorder_.reset();
        return true;
    }
    try {
        trace_slots_.assign(config_.slots, {});
    } catch (...) {
        trace_recorder_.reset();
    }
    pipeline_.set_trace(trace_recorder_.get(), trace_recorder_ ? &trace_telemetry_ : nullptr);
    return true;
}

SharedIoTraceDrainResult SharedIoConvolutionSession::drain_trace(std::uint32_t budget) noexcept {
    if (!trace_recorder_)
        return {};
    return drain_shared_io_trace(*trace_recorder_, trace_telemetry_.snapshot(), budget);
}

void SharedIoConvolutionSession::drain_trace_until_empty() noexcept {
    if (!trace_recorder_)
        return;
    for (std::uint32_t attempt = 0; attempt < 4; ++attempt) {
        const auto before = trace_recorder_->stats();
        (void)drain_trace(static_cast<std::uint32_t>(SharedIoTraceRecorder::capacity));
        const auto after = trace_recorder_->stats();
        if (after.enqueued == after.drained &&
            after.admissions_enqueued == after.admissions_drained)
            break;
        if (after.drained == before.drained &&
            after.admissions_drained == before.admissions_drained)
            break;
    }
}

void SharedIoConvolutionSession::close_trace_generation() noexcept {
    drain_trace_until_empty();
    last_closed_trace_stats_ = trace_recorder_ ? trace_recorder_->stats() : SharedIoTraceStats{};
    pipeline_.set_trace(nullptr);
    trace_recorder_.reset();
    trace_slots_.clear();
    trace_telemetry_.reset();
}

void SharedIoConvolutionSession::trace_admit(SharedIoSlotLedger::SlotToken token) noexcept {
    if (!trace_recorder_ || token.slot >= trace_slots_.size())
        return;
    auto& slot = trace_slots_[token.slot];
    slot = {};
    slot.token = token;
    slot.active = true;
    slot.record.generation = token.preparation_epoch;
    slot.record.sequence = token.stream_sequence;
    slot.record.gpu_work_admitted = true;
    if (config_.active_path == SharedIoPath::SharedHostPointer)
        slot.record.transfer_counters_available = true;
    // Admission begins at the actual physical input lease. Callback time is
    // intentionally unavailable: the callback never reads a diagnostic clock.
    slot.record.set(SharedIoTraceStage::Scheduled, trace_now_ns());
    slot.record.set(SharedIoTraceStage::WorkerEntry, trace_now_ns());
    (void)trace_recorder_->publish_admission(token.preparation_epoch, token.stream_sequence);
}

void SharedIoConvolutionSession::trace_stage(SharedIoSlotLedger::SlotToken token,
                                             SharedIoTraceStage stage) noexcept {
    if (!trace_recorder_ || token.slot >= trace_slots_.size())
        return;
    auto& slot = trace_slots_[token.slot];
    if (slot.active && slot.token == token)
        slot.record.set(stage, trace_now_ns());
}

void SharedIoConvolutionSession::trace_terminal(SharedIoSlotLedger::SlotToken token,
                                                SharedIoGpuTerminalDisposition disposition,
                                                SharedIoFallbackReason reason) noexcept {
    if (!trace_recorder_ || token.slot >= trace_slots_.size())
        return;
    auto& slot = trace_slots_[token.slot];
    if (!slot.active || slot.token != token)
        return;
    slot.active = false;
    trace_telemetry_.record_retired(disposition ==
                                    SharedIoGpuTerminalDisposition::CompletedAccepted);
    auto& record = slot.record;
    record.set(SharedIoTraceStage::RetirementObserved, trace_now_ns());
    record.gpu_terminal = disposition;
    record.gpu_reason = record.reason = reason;
    switch (disposition) {
    case SharedIoGpuTerminalDisposition::CompletedAccepted:
        record.outcome = SharedIoTraceOutcome::Success;
        break;
    case SharedIoGpuTerminalDisposition::CancelledTeardown:
        record.outcome = SharedIoTraceOutcome::Cancelled;
        break;
    case SharedIoGpuTerminalDisposition::StaleRejected:
        record.outcome = SharedIoTraceOutcome::StaleRejected;
        break;
    case SharedIoGpuTerminalDisposition::LateRejected:
        record.outcome = SharedIoTraceOutcome::LateRejected;
        break;
    case SharedIoGpuTerminalDisposition::DeviceLost:
        record.outcome = SharedIoTraceOutcome::DeviceLost;
        break;
    default:
        record.outcome = reason == SharedIoFallbackReason::SubmissionRejected
                             ? SharedIoTraceOutcome::SubmissionRejected
                             : SharedIoTraceOutcome::CompletionFailed;
        break;
    }
    (void)trace_recorder_->publish_completed(record);
}

bool SharedIoConvolutionSession::prepare(ProviderPair pair, Config config) {
    // A failed arena transaction may retain its provider/program while a
    // physical drain barrier is retried. Do not overwrite that owner with a
    // new provider: the plan still points at the retained transaction.
    if (prepared_ || provider_ || plan_.prepared() || !pair.provider || !pair.program ||
        config.slots == 0 || !convolution_geometry_valid(config.pipeline))
        return false;
    const auto bytes = complex_slot_bytes(config.pipeline);
    if (!bytes)
        return false;
    const auto terminal_floats = *bytes / sizeof(float);
    try {
        terminal_.assign(terminal_floats, 0.f);
    } catch (...) {
        return false;
    }
    provider_ = std::move(pair.provider);
    if (!plan_.prepare(*provider_,
                       {.slots = config.slots,
                        .input_bytes_per_slot = *bytes,
                        .output_bytes_per_slot = *bytes},
                       std::move(pair.program))) {
        // A failed arena transaction may still be physically live. Retain the
        // provider and let explicit release()/destruction retry its barrier.
        return false;
    }
    if (!pipeline_.prepare(config.pipeline, plan_.preparation_epoch())) {
        (void)plan_.release();
        return false;
    }
    config_ = config;
    if (config_.trace.enabled && config_.trace.engine_id == 0) {
        static std::atomic<std::uint64_t> next_engine{1};
        config_.trace.engine_id = next_engine.fetch_add(1, std::memory_order_relaxed);
        if (config_.trace.engine_id == 0)
            config_.trace.enabled = false;
    }
    failed_ = false;
    prepared_ = true;
    (void)prepare_trace_generation();
    return true;
}

SharedIoConvolutionSession::Callback
SharedIoConvolutionSession::begin_callback(std::span<const float> samples) noexcept {
    return prepared_ ? pipeline_.begin_callback(samples) : Callback{};
}

SharedIoConvolutionSession::Delivery
SharedIoConvolutionSession::consume_output(const Callback& callback, std::span<float> output,
                                           bool defer_delivery) noexcept {
    return pipeline_.consume_output(callback, output, defer_delivery);
}

void SharedIoConvolutionSession::fail_closed() noexcept {
    failed_ = true;
    // This atomic bridge gate keeps subsequent callbacks CPU-only. The service
    // thread may still drain an already-held ingress lease, but no new GPU
    // ingress is admitted after an unprovable completion disposition.
    pipeline_.request_recovery(SharedIoRecoveryReason::ProviderFailure);
}

bool SharedIoConvolutionSession::pack_input(const SharedIoConvolutionPipeline::Lease& ingress,
                                            SharedIoArena::WriteLease& destination) noexcept {
    const auto samples = ingress.samples();
    const auto expected_samples =
        std::size_t(config_.pipeline.channels) * config_.pipeline.block_size;
    if (samples.size() != expected_samples ||
        destination.bytes.size() != terminal_.size() * sizeof(float))
        return false;
    std::fill(destination.bytes.begin(), destination.bytes.end(), std::byte{0});
    for (std::uint32_t channel = 0; channel < config_.pipeline.channels; ++channel) {
        for (std::uint32_t frame = 0; frame < config_.pipeline.block_size; ++frame) {
            const auto source = samples[std::size_t(channel) * config_.pipeline.block_size + frame];
            const auto complex_float =
                (std::size_t(channel) * config_.pipeline.fft_size + frame) * 2u;
            std::memcpy(destination.bytes.data() + complex_float * sizeof(float), &source,
                        sizeof(source));
        }
    }
    return true;
}

bool SharedIoConvolutionSession::drain_completions(std::uint64_t now_ns,
                                                   ServiceResult& result) noexcept {
    if (!prepared_)
        return false;
    plan_.drain(now_ns); // plan drain polls the one owned provider first.
    if (provider_->device_lost())
        pipeline_.request_recovery(SharedIoRecoveryReason::ProviderLost);
    while (const auto completion = plan_.pop_completion()) {
        ++result.completions;
        const auto& token = completion->token.slot;
        trace_stage(token, SharedIoTraceStage::CompletionObserved);
        const SharedIoConvolutionPipeline::Stamp stamp{token.preparation_epoch,
                                                       token.stream_sequence};
        if (completion->status != SharedIoArena::CompletionStatus::RetiredSuccess) {
            trace_terminal(token,
                           provider_->device_lost()
                               ? SharedIoGpuTerminalDisposition::DeviceLost
                               : SharedIoGpuTerminalDisposition::ProviderFailed,
                           provider_->device_lost() ? SharedIoFallbackReason::DeviceLost
                                                    : SharedIoFallbackReason::CompletionFailed);
            const bool discarded = plan_.discard_completion(*completion);
            const bool recorded =
                pipeline_.record_terminal(stamp, SharedIoConvolutionPipeline::Terminal::Failed, {});
            if (recorded)
                ++result.terminal_records;
            if (!discarded || (!recorded && !pipeline_.fenced())) {
                fail_closed();
                return false;
            }
            continue;
        }
        auto output = plan_.acquire_output(*completion);
        if (!output || output->bytes.size() != terminal_.size() * sizeof(float)) {
            trace_terminal(token, SharedIoGpuTerminalDisposition::ProviderFailed);
            const bool discarded = plan_.discard_completion(*completion);
            const bool recorded =
                pipeline_.record_terminal(stamp, SharedIoConvolutionPipeline::Terminal::Failed, {});
            if (recorded)
                ++result.terminal_records;
            if (!discarded || (!recorded && !pipeline_.fenced())) {
                fail_closed();
                return false;
            }
            continue;
        }
        std::memcpy(terminal_.data(), output->bytes.data(), output->bytes.size());
        // A success has no delivery meaning until the exact physical output
        // lease is relinquished. If that disposition cannot be proven, fence
        // the session and record failure rather than leaving a success hole.
        if (!plan_.release_output({output->token})) {
            trace_terminal(token, SharedIoGpuTerminalDisposition::ProviderFailed,
                           SharedIoFallbackReason::Teardown);
            if (pipeline_.record_terminal(stamp, SharedIoConvolutionPipeline::Terminal::Failed, {}))
                ++result.terminal_records;
            fail_closed();
            return false;
        }
        auto accepted_terminal = SharedIoConvolutionPipeline::Terminal::Failed;
        const bool recorded = pipeline_.record_terminal(
            stamp, SharedIoConvolutionPipeline::Terminal::Success, terminal_, &accepted_terminal);
        if (!recorded)
            trace_terminal(token, SharedIoGpuTerminalDisposition::StaleRejected,
                           SharedIoFallbackReason::SequenceGap);
        else
            trace_terminal(token,
                           accepted_terminal == SharedIoConvolutionPipeline::Terminal::Success
                               ? SharedIoGpuTerminalDisposition::CompletedAccepted
                               : SharedIoGpuTerminalDisposition::ProviderFailed);
        if (recorded)
            ++result.terminal_records;
        else if (!pipeline_.fenced()) {
            fail_closed();
            return false;
        }
    }
    pipeline_.drain_terminals();
    result.fenced = pipeline_.fenced();
    return true;
}

bool SharedIoConvolutionSession::submit_available(ServiceResult& result) noexcept {
    while (prepared_) {
        if (!pending_ingress_)
            pending_ingress_ = pipeline_.acquire_input();
        if (!pending_ingress_)
            return true;
        if (fenced() || pending_ingress_->stamp().epoch != plan_.preparation_epoch()) {
            if (!pipeline_.release_input(*pending_ingress_))
                return false;
            pending_ingress_.reset();
            ++result.dropped_ingress;
            continue;
        }
        if (!pipeline_.begin_worker_admission())
            return true;
        struct AdmissionReservation {
            SharedIoConvolutionPipeline& pipeline;
            ~AdmissionReservation() {
                pipeline.end_worker_admission();
            }
        } admission{pipeline_};
        auto input = plan_.acquire_input(pending_ingress_->stamp().sequence, 0);
        if (!input)
            return true; // retain the bridge lease until a physical slot retires.
        const auto submit = SharedIoComputePlan::SubmitToken{input->token, 0};
        trace_admit(input->token);
        trace_stage(input->token, SharedIoTraceStage::EncodeBegin);
        if (!pack_input(*pending_ingress_, *input)) {
            trace_stage(input->token, SharedIoTraceStage::EncodeEnd);
            trace_terminal(input->token, SharedIoGpuTerminalDisposition::ProviderFailed,
                           SharedIoFallbackReason::SubmissionRejected);
            const auto stamp = pending_ingress_->stamp();
            const bool cancelled = plan_.cancel(submit);
            const bool released = pipeline_.release_input(*pending_ingress_);
            // The bridge sequence was admitted before the provider-slot
            // geometry was checked. Once both physical and bridge leases have
            // been returned, it still needs a terminal failure so the
            // executor cannot retain a chronological hole. If either exact
            // disposition is unprovable, retain the bridge lease when held
            // and stop GPU delivery rather than forgetting its ownership.
            if (released)
                pending_ingress_.reset();
            if (cancelled && released) {
                if (pipeline_.record_terminal(stamp, SharedIoConvolutionPipeline::Terminal::Failed,
                                              {}))
                    ++result.terminal_records;
                pipeline_.drain_terminals();
            }
            fail_closed();
            return false;
        }
        trace_stage(input->token, SharedIoTraceStage::EncodeEnd);
        if (trace_recorder_)
            trace_telemetry_.record_payload_copy(input->bytes.size());
        if (!pipeline_.release_input(*pending_ingress_)) {
            trace_terminal(input->token, SharedIoGpuTerminalDisposition::ProviderFailed,
                           SharedIoFallbackReason::Teardown);
            (void)plan_.cancel(submit);
            return false;
        }
        pending_ingress_.reset();
        trace_stage(input->token, SharedIoTraceStage::SubmitBegin);
        const auto accepted = plan_.submit(submit);
        trace_stage(input->token, SharedIoTraceStage::SubmitEnd);
        if (accepted) {
            ++result.submitted;
            if (trace_recorder_)
                trace_telemetry_.record_submit();
        } else {
            trace_terminal(input->token, SharedIoGpuTerminalDisposition::ProviderFailed,
                           SharedIoFallbackReason::SubmissionRejected);
            ++result.refused;
            // The arena has rolled its pre-submit token back. The bridge and
            // chronological OLA still need one terminal disposition for this
            // admitted ingress, otherwise every later terminal waits behind a
            // permanent sequence hole.
            const bool recorded = pipeline_.record_terminal(
                {submit.slot.preparation_epoch, submit.slot.stream_sequence},
                SharedIoConvolutionPipeline::Terminal::Failed, {});
            if (recorded)
                ++result.terminal_records;
            if (!recorded && !pipeline_.fenced()) {
                fail_closed();
                return false;
            }
            pipeline_.drain_terminals();
        }
    }
    return false;
}

SharedIoConvolutionSession::ServiceResult
SharedIoConvolutionSession::service(std::uint64_t now_ns) noexcept {
    ServiceResult result;
    if (!prepared_)
        return result;
    if (!drain_completions(now_ns, result)) {
        fail_closed();
        result.fenced = true;
        return result;
    }
    if (!submit_available(result)) {
        fail_closed();
        result.fenced = true;
        return result;
    }
    // Deterministic fakes may retire synchronously; one second drain turns
    // those exact terminals into pipeline records without another callback.
    (void)drain_completions(now_ns, result);
    result.fenced = fenced();
    return result;
}

bool SharedIoConvolutionSession::discard_all_completions() noexcept {
    while (const auto completion = plan_.pop_completion()) {
        if (!plan_.discard_completion(*completion))
            return false;
    }
    return true;
}

bool SharedIoConvolutionSession::drain_quiescent() noexcept {
    if (!prepared_)
        return false;
    pipeline_.suspend_delivery();
    if (pending_ingress_) {
        if (!pipeline_.release_input(*pending_ingress_))
            return false;
        pending_ingress_.reset();
    }
    if (!provider_->drain()) {
        fail_closed();
        return false;
    }
    ServiceResult drained;
    if (!drain_completions(0, drained) || !discard_all_completions()) {
        fail_closed();
        return false;
    }
    return true;
}

bool SharedIoConvolutionSession::fence_for_offline() noexcept {
    request_recovery(SharedIoRecoveryReason::OfflineFence);
    return drain_quiescent();
}

bool SharedIoConvolutionSession::fence_and_reprime() noexcept {
    if (!drain_quiescent())
        return false;
    if (provider_->device_lost()) {
        pipeline_.request_recovery(SharedIoRecoveryReason::ProviderLost);
        return false;
    }
    if (!provider_->can_resume_after_drain()) {
        fail_closed();
        return false;
    }
    if (!plan_.reprime_when_quiescent()) {
        fail_closed();
        return false;
    }
    // If the plan has advanced but the bridge cannot enter that same epoch,
    // delivery must remain suspended. There is no safe retry that can infer
    // which phase owns a callback record; release and a fresh prepare are the
    // recovery boundary.
    if (!pipeline_.fence_and_reprime(plan_.preparation_epoch())) {
        fail_closed();
        return false;
    }
    if (trace_recorder_) {
        SharedIoTraceRecord record;
        record.kind = SharedIoTraceKind::Recovery;
        record.generation = trace_recorder_->config().generation;
        record.next_generation = plan_.preparation_epoch();
        record.sequence = pipeline_.next_sequence();
        (void)trace_recorder_->publish_completed(record);
    }
    close_trace_generation();
    (void)prepare_trace_generation();
    failed_ = false;
    return true;
}

bool SharedIoConvolutionSession::release() noexcept {
    if (!prepared_) {
        if (!plan_.release())
            return false;
        terminal_.clear();
        provider_.reset();
        return true;
    }
    // Stop callbacks before this boundary. Harvest physical terminal results
    // before arena release consumes its terminal inbox internally.
    if (!drain_quiescent())
        return false;
    ServiceResult drained;
    (void)drain_completions(0, drained);
    if (!plan_.release())
        return false;
    for (const auto& slot : trace_slots_)
        if (slot.active)
            trace_terminal(slot.token, SharedIoGpuTerminalDisposition::CancelledTeardown,
                           SharedIoFallbackReason::Teardown);
    close_trace_generation();
    prepared_ = false;
    failed_ = false;
    terminal_.clear();
    provider_.reset();
    return true;
}

} // namespace pulp::gpu_audio::detail
