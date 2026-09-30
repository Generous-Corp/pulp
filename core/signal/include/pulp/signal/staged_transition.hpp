#pragma once

/// @file staged_transition.hpp
/// Amortise a bounded piece of transition work over several audio callbacks.
///
/// A state transition (a freeze latch, an engage pre-roll, a mode switch)
/// must not cost more in its callback than a steady-state callback does: the
/// host has one fixed deadline per buffer and drops the buffer that misses
/// it. When the transition needs work that cannot be precomputed in
/// prepare(), split it into `total` equal steps and run only the steps that
/// are due in each callback.
///
/// The tracker owns no work and no storage; each run call takes the step
/// callable, `void(int step_index)`, which must be bounded and
/// allocation-free. Steps run in index order, each exactly once per begin().
///
/// @code
/// StagedTransition engage;
/// engage.begin(24);                       // at the latch: schedule, run nothing
/// // every callback:
/// engage.run_due(frames_since_latch, frames_over_which_to_spread,
///                [&](int step) { synthesise_frame(step); });
/// @endcode
///
/// RT contract: every member is allocation-free and lock-free; the cost of a
/// run call is the cost of the steps it runs.

#include <algorithm>
#include <cstdint>

namespace pulp::signal {

class StagedTransition {
public:
    /// Steps that must have completed once `elapsed` of `horizon` units have
    /// passed: ceil(total * elapsed / horizon), clamped to [0, total]. A
    /// non-positive horizon means "all of it now". Pure.
    static constexpr int due_steps(int total, std::int64_t elapsed,
                                   std::int64_t horizon) noexcept {
        if (total <= 0) return 0;
        if (horizon <= 0 || elapsed >= horizon) return total;
        if (elapsed <= 0) return 0;
        const std::int64_t num = static_cast<std::int64_t>(total) * elapsed;
        return static_cast<int>((num + horizon - 1) / horizon);
    }

    /// Schedule `total_steps` steps; runs nothing. Replaces any schedule in
    /// progress (its remaining steps are dropped).
    void begin(int total_steps) noexcept {
        total_ = std::max(total_steps, 0);
        done_ = 0;
    }

    /// Drop the schedule without running the remaining steps.
    void cancel() noexcept {
        total_ = 0;
        done_ = 0;
    }

    /// Run every step due by `elapsed` of `horizon` (see due_steps()).
    /// Returns the number of steps run.
    template <typename Step>
    int run_due(std::int64_t elapsed, std::int64_t horizon, Step&& step) {
        return run_until(due_steps(total_, elapsed, horizon), step);
    }

    /// Run up to `count` further steps. Returns the number run.
    template <typename Step>
    int run_next(int count, Step&& step) {
        return run_until(done_ + std::max(count, 0), step);
    }

    /// Run every remaining step now (for a consumer that needs the finished
    /// state immediately). Returns the number run.
    template <typename Step>
    int finish_now(Step&& step) {
        return run_until(total_, step);
    }

    bool active() const noexcept { return done_ < total_; }
    int total() const noexcept { return total_; }
    int completed() const noexcept { return done_; }
    int remaining() const noexcept { return total_ - done_; }

private:
    template <typename Step>
    int run_until(int target, Step& step) {
        target = std::min(target, total_);
        const int start = done_;
        while (done_ < target) {
            const int index = done_;
            ++done_; // advance first so a step may inspect remaining()
            step(index);
        }
        return done_ - start;
    }

    int total_ = 0;
    int done_ = 0;
};

} // namespace pulp::signal
