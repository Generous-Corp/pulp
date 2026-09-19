#pragma once

#include "shared_io_arena.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace pulp::gpu_audio::detail {

class SharedIoComputePlan {
  public:
    struct Config {
        std::uint32_t slots = 0;
        std::size_t input_bytes_per_slot = 0;
        std::size_t output_bytes_per_slot = 0;
    };
    struct SubmitToken {
        SharedIoSlotLedger::SlotToken slot;
        std::uint64_t deadline_ns = 0;
    };
    struct Completion {
        SubmitToken token;
        SharedIoArena::CompletionStatus status = SharedIoArena::CompletionStatus::RetiredFailed;
        bool late = false;
    };
    struct Telemetry {
        std::uint64_t payload_bytes_copied = 0;
        std::uint64_t encode_submit_ns = 0;
        std::uint64_t gpu_elapsed_ns = 0;
        std::uint64_t completion_wall_ns = 0;
        std::uint64_t misses = 0;
        std::uint64_t late_completions = 0;
        std::uint64_t high_water_in_flight = 0;
        // The shared Dawn provider has no timestamp-query/occupancy certificate yet.
        bool gpu_timing_available = false;
        bool occupancy_available = false;
    };

    SharedIoComputePlan() = default;
    ~SharedIoComputePlan() = default;
    SharedIoComputePlan(const SharedIoComputePlan&) = delete;
    SharedIoComputePlan& operator=(const SharedIoComputePlan&) = delete;

    bool prepare(SharedIoArenaProvider& provider, const Config& config);
    bool prepare(SharedIoArenaProvider& provider, const Config& config,
                 std::unique_ptr<SharedIoPreparedProgram> program);
    bool prepared() const noexcept {
        return arena_.prepared();
    }
    std::uint64_t preparation_epoch() const noexcept {
        return arena_.preparation_epoch();
    }
    std::optional<SharedIoArena::WriteLease> acquire_input(std::uint64_t sequence,
                                                           std::uint64_t deadline_ns) noexcept;
    bool submit(const SubmitToken& token) noexcept;
    // A pre-submit refusal must return the write lease to the fixed ledger.
    bool cancel(const SubmitToken& token) noexcept;
    std::size_t drain(std::uint64_t now_ns) noexcept;
    std::optional<Completion> pop_completion() noexcept;
    std::optional<SharedIoArena::OutputLease>
    acquire_output(const Completion& completion) noexcept {
        if (completion.status != SharedIoArena::CompletionStatus::RetiredSuccess)
            return std::nullopt;
        return arena_.acquire_output(completion.token.slot.preparation_epoch,
                                     completion.token.slot.stream_sequence);
    }
    bool expire_delivery(const Completion& completion) noexcept {
        return arena_.expire_delivery(completion.token.slot);
    }
    // Terminal failure and expired success retain storage until this explicit
    // non-RT disposition relinquishes the exact token.
    bool discard_completion(const Completion& completion) noexcept {
        return arena_.discard(completion.token.slot);
    }
    // Host/quiescent only. Reuses the persistent slot allocations while
    // advancing epoch identity, so prior completion/token records are stale.
    bool reprime_when_quiescent() noexcept;
    bool release_output(const SharedIoArena::ReleaseRecord& record) noexcept {
        return arena_.release_output(record);
    }
    bool release() noexcept {
        return arena_.release();
    }
    const Telemetry& telemetry() const noexcept {
        return telemetry_;
    }

  private:
    struct Pending {
        SubmitToken token;
        bool active = false;
    };
    static void on_terminal(void* context, const SharedIoSlotLedger::SlotToken& token,
                            SharedIoArena::CompletionStatus status) noexcept;
    void record_terminal(const SharedIoSlotLedger::SlotToken& token,
                         SharedIoArena::CompletionStatus status) noexcept;

    SharedIoArena arena_;
    std::vector<Pending> pending_;
    std::vector<Completion> completions_;
    std::size_t completion_read_ = 0;
    std::size_t completion_write_ = 0;
    Telemetry telemetry_;
};

} // namespace pulp::gpu_audio::detail
