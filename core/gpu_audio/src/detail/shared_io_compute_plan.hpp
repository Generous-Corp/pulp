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
        SharedIoArena::CompletionStatus status =
            SharedIoArena::CompletionStatus::RetiredFailed;
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
    };

    SharedIoComputePlan() = default;
    ~SharedIoComputePlan() = default;
    SharedIoComputePlan(const SharedIoComputePlan&) = delete;
    SharedIoComputePlan& operator=(const SharedIoComputePlan&) = delete;

    bool prepare(SharedIoArenaProvider& provider, const Config& config);
    bool prepared() const noexcept { return arena_.prepared(); }
    std::uint64_t preparation_epoch() const noexcept { return arena_.preparation_epoch(); }
    std::optional<SharedIoArena::WriteLease>
    acquire_input(std::uint64_t sequence, std::uint64_t deadline_ns) noexcept;
    bool submit(const SubmitToken& token) noexcept;
    std::size_t drain(std::uint64_t now_ns) noexcept;
    std::optional<Completion> pop_completion() noexcept;
    bool release_output(const SharedIoArena::ReleaseRecord& record) noexcept {
        return arena_.release_output(record);
    }
    bool release() noexcept { return arena_.release(); }
    const Telemetry& telemetry() const noexcept { return telemetry_; }

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
