#pragma once

#include "shared_io_compute_plan.hpp"
#include "shared_io_convolution_pipeline.hpp"
#include "shared_io_trace.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace pulp::gpu_audio::detail {

// Private prepared-provider owner for the shared-I/O convolution experiment.
// A coordinating owner constructs exactly one provider and its matching
// program, then transfers both here. The callback remains entirely inside the
// pipeline; service() is the sole serialized non-RT provider/plan entry point.
class SharedIoConvolutionSession {
  public:
    using Callback = SharedIoConvolutionPipeline::Callback;
    using Delivery = SharedIoConvolutionPipeline::Delivery;

    struct Config {
        SharedIoConvolutionPipeline::Config pipeline;
        std::uint32_t slots = 0;
        std::uint32_t sample_rate = 0;
        SharedIoRequest requested_path = SharedIoRequest::Auto;
        SharedIoPath active_path = SharedIoPath::SharedHostPointer;
        MissPolicy miss_policy = MissPolicy::CpuFallback;
        bool shared_host_pointer_capable = true;
        bool cpu_fallback_prepared = true;
        SharedIoTraceConfig trace;
    };

    struct ProviderPair {
        std::unique_ptr<SharedIoArenaProvider> provider;
        // Must have been created by provider before transfer. The arena keeps
        // it alive through every terminal completion and releases it before
        // provider slots retire.
        std::unique_ptr<SharedIoPreparedProgram> program;
    };

    struct ServiceResult {
        std::size_t submitted = 0;
        std::size_t refused = 0;
        std::size_t completions = 0;
        std::size_t terminal_records = 0;
        std::size_t dropped_ingress = 0;
        bool fenced = false;
    };

    SharedIoConvolutionSession() = default;
    // Destruction attempts release(). If the provider cannot prove its drain,
    // the arena quarantines the prepared program and storage rather than
    // freeing potentially live GPU-backed resources; callers that need retry
    // recovery must retain this session and call release() explicitly.
    ~SharedIoConvolutionSession();
    SharedIoConvolutionSession(const SharedIoConvolutionSession&) = delete;
    SharedIoConvolutionSession& operator=(const SharedIoConvolutionSession&) = delete;

    // All lifetime/epoch operations require callback, service, and diagnostic
    // drain callers to be stopped and joined. The owner may then drain the
    // recorder itself; it must never race a second diagnostic consumer.
    // Host/quiescent only. A failed preparation retains the paired arena
    // transaction until release proves physical cleanup; callers must keep the
    // session alive for that retry.
    bool prepare(ProviderPair, Config);

    // Callback only: fixed bridge records and the executor watermark, with no
    // provider/program/plan access.
    Callback begin_callback(std::span<const float> samples) noexcept;
    Callback begin_callback(std::span<const float> samples, std::uint64_t sequence,
                            std::uint64_t callback_start_ns = 0) noexcept {
        return prepared_ ? pipeline_.begin_callback(samples, sequence, callback_start_ns)
                          : Callback{};
    }
    void request_recovery(SharedIoRecoveryReason reason) noexcept {
        pipeline_.request_recovery(reason);
    }
    SharedIoRecoveryReason recovery_reason() const noexcept {
        return pipeline_.recovery_reason();
    }
    bool complete_callback_delivery(const Callback& callback,
                                    SharedIoDeliveryDisposition actual,
                                    std::uint64_t callback_end_ns = 0,
                                    std::uint64_t result_visible_ns = 0) noexcept {
        return pipeline_.complete_callback_delivery(callback, actual, callback_end_ns,
                                                    result_visible_ns);
    }
    Delivery consume_output(const Callback&, std::span<float> output,
                            bool defer_delivery = false) noexcept;

    // Serialized non-RT only. It owns provider polling, plan completion
    // disposition, planar-to-complex packing, submission, and bridge leases.
    ServiceResult service(std::uint64_t now_ns) noexcept;

    // Host/quiescent only after the callback is stopped and all callback-held
    // leases returned. Drains the old provider phase, discards old plan tokens,
    // then advances both plan and pipeline to the same monotonic epoch.
    bool fence_and_reprime() noexcept;
    // Same stopped/joined caller contract. Drains exact physical work but keeps
    // the bridge CPU-only; offline processing cannot silently reactivate it.
    bool fence_for_offline() noexcept;
    bool release() noexcept;

    bool prepared() const noexcept {
        return prepared_;
    }
    bool fenced() const noexcept {
        return failed_ || pipeline_.fenced();
    }
    std::uint64_t epoch() const noexcept {
        return pipeline_.epoch();
    }
    std::uint64_t next_sequence() const noexcept {
        return pipeline_.next_sequence();
    }
    const SharedIoComputePlan::Telemetry& telemetry() const noexcept {
        return plan_.telemetry();
    }
    SharedIoTelemetrySnapshot trace_telemetry() const noexcept {
        return trace_telemetry_.snapshot();
    }
    SharedIoTraceStats trace_stats() const noexcept {
        return trace_recorder_ ? trace_recorder_->stats() : SharedIoTraceStats{};
    }
    SharedIoTraceStats last_closed_trace_stats() const noexcept {
        return last_closed_trace_stats_;
    }
    bool trace_recording_enabled() const noexcept {
        return trace_recorder_ != nullptr && trace_recorder_->enabled();
    }
    // Exactly one serialized diagnostic consumer, stopped before any lifetime/epoch operation.
    SharedIoTraceDrainResult drain_trace(std::uint32_t budget = 256) noexcept;
    template <class Sink> std::uint32_t drain_trace_records(std::uint32_t budget, Sink&& sink) {
        if (!trace_recorder_)
            return 0;
        return trace_recorder_->drain_worker_records(budget, std::forward<Sink>(sink));
    }
    template <class Sink> std::uint32_t drain_trace_admissions(std::uint32_t budget, Sink&& sink) {
        if (!trace_recorder_)
            return 0;
        return trace_recorder_->drain_admissions(budget, std::forward<Sink>(sink));
    }

  private:
    SharedIoExecutionContract execution_contract() const noexcept;
    bool prepare_trace_generation() noexcept;
    void close_trace_generation() noexcept;
    void drain_trace_until_empty() noexcept;
    static std::uint64_t trace_now_ns() noexcept;
    bool pack_input(const SharedIoConvolutionPipeline::Lease&, SharedIoArena::WriteLease&) noexcept;
    void fail_closed() noexcept;
    bool drain_completions(std::uint64_t now_ns, ServiceResult&) noexcept;
    bool submit_available(ServiceResult&) noexcept;
    bool discard_all_completions() noexcept;
    bool drain_quiescent() noexcept;

    // Declared before plan_ so it outlives plan_ during destruction.
    std::unique_ptr<SharedIoArenaProvider> provider_;
    SharedIoComputePlan plan_;
    SharedIoConvolutionPipeline pipeline_;
    struct TraceSlot {
        SharedIoSlotLedger::SlotToken token;
        SharedIoTraceRecord record;
        bool active = false;
    };
    void trace_admit(SharedIoSlotLedger::SlotToken) noexcept;
    void trace_stage(SharedIoSlotLedger::SlotToken, SharedIoTraceStage) noexcept;
    void trace_terminal(SharedIoSlotLedger::SlotToken, SharedIoGpuTerminalDisposition,
                        SharedIoFallbackReason = SharedIoFallbackReason::None) noexcept;
    std::vector<TraceSlot> trace_slots_;
    SharedIoTelemetry trace_telemetry_;
    std::unique_ptr<SharedIoTraceRecorder> trace_recorder_;
    SharedIoTraceConfig trace_template_{};
    SharedIoTraceStats last_closed_trace_stats_{};
    std::optional<SharedIoConvolutionPipeline::Lease> pending_ingress_;
    std::vector<float> terminal_;
    Config config_{};
    bool prepared_ = false;
    bool failed_ = false;
};

} // namespace pulp::gpu_audio::detail
