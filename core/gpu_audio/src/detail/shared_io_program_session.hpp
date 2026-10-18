#pragma once

#include "shared_io_compute_plan.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace pulp::gpu_audio::detail {

// Backend-neutral lifecycle owner for a prepared shared-I/O compute program.
// This is intentionally private: it exposes the existing fixed-slot and
// terminal-retirement contract without promising a particular DSP, backend, or
// realtime safety property.
class SharedIoProgramSession {
  public:
    struct ProviderPair {
        std::unique_ptr<SharedIoArenaProvider> provider;
        std::unique_ptr<SharedIoPreparedProgram> program;
    };
    using Config = SharedIoComputePlan::Config;
    using SubmitToken = SharedIoComputePlan::SubmitToken;
    using Completion = SharedIoComputePlan::Completion;
    using LifecycleReceipt = SharedIoComputePlan::LifecycleReceipt;
    using CalibrationObservation = SharedIoCalibrationObservation;
    using InputLease = SharedIoArena::WriteLease;
    using OutputLease = SharedIoArena::OutputLease;

    SharedIoProgramSession() = default;
    ~SharedIoProgramSession();
    SharedIoProgramSession(const SharedIoProgramSession&) = delete;
    SharedIoProgramSession& operator=(const SharedIoProgramSession&) = delete;

    bool prepare(ProviderPair pair, Config config);
    // Non-owning observation only; invalidated after successful release.
    SharedIoArenaProvider* owned_provider() const noexcept {
        return provider_.get();
    }
    bool prepared() const noexcept {
        return prepared_;
    }
    std::uint64_t preparation_epoch() const noexcept {
        return plan_.preparation_epoch();
    }

    std::optional<InputLease> acquire_input(std::uint64_t sequence,
                                            std::uint64_t deadline_ns) noexcept;
    bool submit(const SubmitToken& token) noexcept;
    bool cancel(const SubmitToken& token) noexcept;
    std::size_t service(std::uint64_t now_ns) noexcept;
    std::size_t service_until(std::uint64_t now_ns, std::uint64_t deadline_ns) noexcept;
    std::optional<Completion> pop_completion() noexcept;
    std::optional<OutputLease> acquire_output(const Completion& completion) noexcept;
    bool release_output(const SharedIoArena::ReleaseRecord& record) noexcept;
    bool expire_delivery(const Completion& completion) noexcept;
    bool discard_completion(const Completion& completion) noexcept;
    bool reprime_when_quiescent() noexcept;
    // Observe final provider counters after physical retirement succeeds and
    // before ownership is destroyed. No observer fires on a failed barrier or
    // a repeated release after the provider has already been destroyed.
    struct ReleaseObserver {
        void* context = nullptr;
        void (*observe)(void*, const SharedIoArenaProvider&) noexcept = nullptr;
    };
    bool release(ReleaseObserver observer) noexcept;
    bool release() noexcept;
    // Test/dispatcher owner only, after callback and service have stopped.
    // Null retains a failed physical barrier for retry. Returning the owner
    // proves resource drain, not that its device remains reusable.
    std::unique_ptr<SharedIoArenaProvider> release_to_owner() noexcept;
    const SharedIoComputePlan::Telemetry& telemetry() const noexcept {
        return plan_.telemetry();
    }
    // Quiescent diagnostic extraction. An empty result while work is active is
    // an intentional fail-closed result, not proof that no receipt exists.
    std::vector<LifecycleReceipt> take_lifecycle_receipts() {
        return plan_.take_lifecycle_receipts();
    }
    std::vector<CalibrationObservation> take_calibration_observations() {
        return plan_.take_calibration_observations();
    }
    bool lifecycle_receipt_valid() const noexcept {
        return plan_.lifecycle_receipt_valid();
    }
    bool lifecycle_receipt_overflow() const noexcept {
        return plan_.lifecycle_receipt_overflow();
    }
    bool completion_receipt_overflow() const noexcept {
        return plan_.completion_receipt_overflow();
    }
    bool receipt_overflow() const noexcept {
        return plan_.receipt_overflow();
    }

  private:
    std::unique_ptr<SharedIoArenaProvider> provider_;
    SharedIoComputePlan plan_;
    bool prepared_ = false;
};

} // namespace pulp::gpu_audio::detail
