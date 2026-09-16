#pragma once

#include "shared_io_compute_plan.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace pulp::gpu_audio::detail {

class SharedIoTransportBridge {
  public:
    enum class Disposition : std::uint8_t { Deliver, SuppressLate, Reprime };
    struct Result {
        std::uint64_t sequence = 0;
        SharedIoArena::CompletionStatus status =
            SharedIoArena::CompletionStatus::RetiredFailed;
        bool late = false;
        Disposition disposition = Disposition::Reprime;
    };
    bool prepare(std::uint32_t capacity, std::uint64_t first_sequence);
    bool record(const SharedIoComputePlan::Completion& completion) noexcept;
    std::optional<Result> collect_next() noexcept;
    void reset(std::uint64_t first_sequence) noexcept;
    std::uint64_t next_sequence() const noexcept { return next_sequence_; }
    std::size_t pending() const noexcept;

  private:
    struct Entry {
        Result result;
        bool present = false;
    };
    std::vector<Entry> entries_;
    std::uint64_t next_sequence_ = 0;
};

} // namespace pulp::gpu_audio::detail
