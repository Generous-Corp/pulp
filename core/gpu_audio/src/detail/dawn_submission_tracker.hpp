#pragma once

#include <cstdint>
#include <optional>

namespace pulp::gpu_audio::detail {

// Dawn-free reducer for one stable per-slot submission record. Callback
// threads publish evidence; the serialized provider poll supplies the current
// loss/error snapshot and is the only caller that consumes a terminal result.
class DawnSubmissionTracker {
  public:
    using Generation = std::uint64_t;

    enum class QueueResult : std::uint8_t { Pending, Success, Error, Cancelled };
    enum class ScopeResult : std::uint8_t { Pending, Clean, Error };
    enum class Terminal : std::uint8_t { RetiredSuccess, RetiredFailure };

    struct Observation {
        std::uint64_t uncaptured_error_generation = 0;
        bool device_lost = false;
        bool physically_drained = false;
    };

    bool begin(Generation generation, std::uint64_t uncaptured_error_generation) noexcept;
    bool record_queue(Generation generation, QueueResult result) noexcept;
    bool record_scope(Generation generation, ScopeResult result) noexcept;
    bool mark_expired(Generation generation) noexcept;
    std::optional<Terminal> observe(Generation generation, const Observation& observation) noexcept;

    bool active() const noexcept {
        return active_;
    }
    bool expired() const noexcept {
        return expired_;
    }
    Generation generation() const noexcept {
        return generation_;
    }
    std::uint64_t rejected_evidence() const noexcept {
        return rejected_evidence_;
    }

  private:
    bool accepts(Generation generation) noexcept;

    Generation generation_ = 0;
    std::uint64_t uncaptured_error_baseline_ = 0;
    std::uint64_t rejected_evidence_ = 0;
    QueueResult queue_ = QueueResult::Pending;
    ScopeResult scope_ = ScopeResult::Pending;
    bool active_ = false;
    bool expired_ = false;
};

} // namespace pulp::gpu_audio::detail
