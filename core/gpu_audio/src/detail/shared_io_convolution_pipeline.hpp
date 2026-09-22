#pragma once

#include "shared_io_convolution_executor.hpp"
#include "shared_io_stamped_bridge.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace pulp::gpu_audio::detail {

// Private composition point for the staged shared-memory convolution path.
//
// The callback only writes or reads fixed bridge records and advances a
// watermark. A single serialized non-RT dispatcher owns ingress leases,
// terminal records, OLA carry, and egress publication. It deliberately owns no
// Dawn object: the subsequent provider slice supplies terminal FFT readbacks.
// GpuAudioTransport/GpuConvolver remain the public, continuously-primed CPU
// fallback path until that provider is admitted.
class SharedIoConvolutionPipeline {
  public:
    using Stamp = SharedIoStampedBridge::Stamp;
    using Callback = SharedIoStampedBridge::Callback;
    using Delivery = SharedIoStampedBridge::Delivery;
    using Publication = SharedIoStampedBridge::Publication;
    using Lease = SharedIoStampedBridge::Lease;
    using Terminal = SharedIoConvolutionExecutor::Terminal;

    struct Config {
        std::uint32_t capacity = 0;
        std::uint32_t channels = 0;
        std::uint32_t block_size = 0;
        std::uint32_t fft_size = 0;
        std::uint32_t ir_length = 0;
        std::uint32_t lead_blocks = SharedIoStampedBridge::kLeadBlocks;
        bool capture_callback_timing = false;
    };

    // Host/quiescent only. The two components receive one geometry and epoch
    // so every record retains one common absolute sequence identity.
    bool prepare(Config config, std::uint64_t epoch, std::uint64_t first_sequence = 0);

    void set_trace(SharedIoTraceRecorder* trace, SharedIoTelemetry* telemetry = nullptr) noexcept {
        bridge_.set_trace(trace, telemetry);
    }

    // Callback only. These functions perform bounded fixed-record copies and
    // atomics only: no GPU API, allocation, lock, wait, Objective-C, or Perfetto.
    Callback begin_callback(std::span<const float> samples) noexcept;
    Callback begin_callback(std::span<const float> samples, std::uint64_t sequence,
                            std::uint64_t callback_start_ns = 0) noexcept {
        return bridge_.begin_callback(samples, sequence, callback_start_ns);
    }
    void request_recovery(SharedIoRecoveryReason reason) noexcept {
        bridge_.request_recovery(reason);
    }
    SharedIoRecoveryReason recovery_reason() const noexcept {
        return bridge_.recovery_reason();
    }
    bool complete_callback_delivery(const Callback& callback,
                                    SharedIoDeliveryDisposition actual,
                                    std::uint64_t callback_end_ns = 0,
                                    std::uint64_t result_visible_ns = 0) noexcept {
        return bridge_.complete_callback_delivery(callback, actual, callback_end_ns,
                                                  result_visible_ns);
    }
    Delivery consume_output(const Callback&, std::span<float> output,
                            bool defer_delivery = false) noexcept;

    // Serialized non-RT dispatcher only. An ingress lease remains immutable
    // until release_input(). The provider later returns the same stamp as a
    // terminal FFT block to record_terminal(), then drain_terminals() performs
    // chronological OLA and publishes any ready wet record.
    bool begin_worker_admission() noexcept {
        return bridge_.begin_worker_admission();
    }
    void end_worker_admission() noexcept {
        bridge_.end_worker_admission();
    }
    std::optional<Lease> acquire_input() noexcept;
    bool release_input(const Lease&) noexcept;
    bool record_terminal(Stamp, Terminal, std::span<const float> interleaved_time,
                         Terminal* accepted_terminal = nullptr) noexcept;
    std::size_t drain_terminals() noexcept;

    // Host/quiescent only after callback invocation has stopped and returned
    // all bridge leases. CPU-only delivery begins before the executor is reset;
    // the next epoch starts at the callback's current absolute sequence.
    bool fence_and_reprime(std::uint64_t new_epoch) noexcept;

    // Serialized non-RT fail-closed admission gate. Existing callback calls
    // become CPU-only through the bridge atomic; no callback accesses worker
    // state to observe this transition.
    void suspend_delivery() noexcept;

    bool prepared() const noexcept {
        return prepared_;
    }
    bool fenced() const noexcept {
        return executor_.fenced() || recovery_reason() != SharedIoRecoveryReason::None;
    }
    std::uint64_t epoch() const noexcept {
        return epoch_;
    }
    std::uint64_t next_sequence() const noexcept {
        return bridge_.next_sequence();
    }
    std::uint64_t valid_from_sequence() const noexcept {
        return executor_.valid_from_sequence();
    }
    std::uint32_t lead_blocks() const noexcept {
        return bridge_.lead_blocks();
    }

  private:
    SharedIoStampedBridge bridge_;
    SharedIoConvolutionExecutor executor_;
    std::uint64_t epoch_ = 0;
    bool prepared_ = false;
};

} // namespace pulp::gpu_audio::detail
