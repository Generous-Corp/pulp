#include "dawn_submission_tracker.hpp"

namespace pulp::gpu_audio::detail {

bool DawnSubmissionTracker::begin(Generation generation,
                                  std::uint64_t uncaptured_error_generation) noexcept {
    if (active_ || generation == 0)
        return false;
    generation_ = generation;
    uncaptured_error_baseline_ = uncaptured_error_generation;
    queue_ = QueueResult::Pending;
    scope_ = ScopeResult::Pending;
    expired_ = false;
    active_ = true;
    return true;
}

bool DawnSubmissionTracker::accepts(Generation generation) noexcept {
    if (!active_ || generation != generation_) {
        ++rejected_evidence_;
        return false;
    }
    return true;
}

bool DawnSubmissionTracker::record_queue(Generation generation, QueueResult result) noexcept {
    if (!accepts(generation) || result == QueueResult::Pending || queue_ != QueueResult::Pending) {
        if (active_ && generation == generation_)
            ++rejected_evidence_;
        return false;
    }
    queue_ = result;
    return true;
}

bool DawnSubmissionTracker::record_scope(Generation generation, ScopeResult result) noexcept {
    if (!accepts(generation) || result == ScopeResult::Pending || scope_ != ScopeResult::Pending) {
        if (active_ && generation == generation_)
            ++rejected_evidence_;
        return false;
    }
    scope_ = result;
    return true;
}

bool DawnSubmissionTracker::mark_expired(Generation generation) noexcept {
    if (!accepts(generation) || expired_) {
        if (active_ && generation == generation_ && expired_)
            ++rejected_evidence_;
        return false;
    }
    expired_ = true;
    return true;
}

std::optional<DawnSubmissionTracker::Terminal>
DawnSubmissionTracker::observe(Generation generation, const Observation& observation) noexcept {
    if (!accepts(generation))
        return std::nullopt;

    const bool poisoned = queue_ == QueueResult::Error || queue_ == QueueResult::Cancelled ||
                          scope_ == ScopeResult::Error || observation.device_lost ||
                          observation.uncaptured_error_generation != uncaptured_error_baseline_;
    std::optional<Terminal> result;
    if (poisoned) {
        if (observation.physically_drained)
            result = Terminal::RetiredFailure;
    } else if (queue_ == QueueResult::Success && scope_ == ScopeResult::Clean) {
        result = Terminal::RetiredSuccess;
    } else if (observation.physically_drained) {
        result = Terminal::RetiredFailure;
    }

    if (result)
        active_ = false;
    return result;
}

} // namespace pulp::gpu_audio::detail
