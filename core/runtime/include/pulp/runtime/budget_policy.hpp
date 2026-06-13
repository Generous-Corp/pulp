#pragma once

/// @file budget_policy.hpp
/// Small runtime policy helper for budgeted work and graceful degradation.

#include <cstdint>
#include <limits>

namespace pulp::runtime {

enum class RuntimeWorkLane : std::uint8_t {
    CriticalAudio = 0,
    Interactive,
    Background,
    Opportunistic,
};

enum class RuntimeBudgetAction : std::uint8_t {
    Run = 0,
    Defer,
    Shed,
    Bypass,
};

struct RuntimeBudgetPolicy {
    std::uint64_t critical_audio_reserve = 0;
    bool shed_background_on_overload = false;
    bool shed_opportunistic_on_overload = true;
};

struct RuntimeBudgetRequest {
    RuntimeWorkLane lane = RuntimeWorkLane::Background;
    std::uint64_t estimated_cost = 0;
    std::uint64_t remaining_budget = 0;
    bool required = false;
    bool overload_active = false;
};

struct RuntimeBudgetDecision {
    RuntimeBudgetAction action = RuntimeBudgetAction::Run;
    const char* reason = "within-budget";

    bool should_run() const noexcept { return action == RuntimeBudgetAction::Run; }
};

struct RuntimeBudgetFrameStats {
    std::uint32_t run_count = 0;
    std::uint32_t defer_count = 0;
    std::uint32_t shed_count = 0;
    std::uint32_t bypass_count = 0;
    std::uint64_t consumed_budget = 0;
    std::uint64_t remaining_budget = 0;
};

inline const char* to_string(RuntimeBudgetAction action) noexcept {
    switch (action) {
        case RuntimeBudgetAction::Run:    return "run";
        case RuntimeBudgetAction::Defer:  return "defer";
        case RuntimeBudgetAction::Shed:   return "shed";
        case RuntimeBudgetAction::Bypass: return "bypass";
    }
    return "bypass";
}

inline RuntimeBudgetDecision evaluate_runtime_budget(
    const RuntimeBudgetRequest& request,
    const RuntimeBudgetPolicy& policy = {}) noexcept {
    if (request.lane == RuntimeWorkLane::CriticalAudio) {
        return {RuntimeBudgetAction::Run, "critical-audio"};
    }

    if (request.overload_active) {
        if (request.lane == RuntimeWorkLane::Opportunistic
            && policy.shed_opportunistic_on_overload) {
            return {RuntimeBudgetAction::Shed, "overload-shed-opportunistic"};
        }
        if (request.lane == RuntimeWorkLane::Background
            && policy.shed_background_on_overload) {
            return {RuntimeBudgetAction::Shed, "overload-shed-background"};
        }
    }

    const std::uint64_t reserve =
        request.lane == RuntimeWorkLane::Interactive
            ? policy.critical_audio_reserve
            : 0;
    const bool has_budget =
        request.remaining_budget >= reserve
        && request.estimated_cost <= request.remaining_budget - reserve;

    if (has_budget) return {RuntimeBudgetAction::Run, "within-budget"};

    if (request.required) return {RuntimeBudgetAction::Defer, "required-defer"};
    if (request.lane == RuntimeWorkLane::Interactive)
        return {RuntimeBudgetAction::Defer, "interactive-defer"};
    if (request.lane == RuntimeWorkLane::Opportunistic)
        return {RuntimeBudgetAction::Shed, "budget-shed-opportunistic"};
    return {RuntimeBudgetAction::Bypass, "budget-bypass-background"};
}

class RuntimeBudgetFrame {
public:
    explicit RuntimeBudgetFrame(std::uint64_t budget,
                                RuntimeBudgetPolicy policy = {},
                                bool overload_active = false) noexcept
        : policy_(policy)
        , remaining_budget_(budget)
        , overload_active_(overload_active) {
        stats_.remaining_budget = remaining_budget_;
    }

    RuntimeBudgetDecision evaluate(RuntimeWorkLane lane,
                                   std::uint64_t estimated_cost,
                                   bool required = false) noexcept {
        RuntimeBudgetRequest request;
        request.lane = lane;
        request.estimated_cost = estimated_cost;
        request.remaining_budget = remaining_budget_;
        request.required = required;
        request.overload_active = overload_active_;

        const auto decision = evaluate_runtime_budget(request, policy_);
        record(decision.action, estimated_cost);
        return decision;
    }

    std::uint64_t remaining_budget() const noexcept { return remaining_budget_; }
    bool overload_active() const noexcept { return overload_active_; }
    const RuntimeBudgetPolicy& policy() const noexcept { return policy_; }
    const RuntimeBudgetFrameStats& stats() const noexcept { return stats_; }

    void set_overload_active(bool active) noexcept { overload_active_ = active; }

    void add_budget(std::uint64_t budget) noexcept {
        const auto previous = remaining_budget_;
        remaining_budget_ += budget;
        if (remaining_budget_ < previous) {
            remaining_budget_ = std::numeric_limits<std::uint64_t>::max();
        }
        stats_.remaining_budget = remaining_budget_;
    }

private:
    void record(RuntimeBudgetAction action, std::uint64_t estimated_cost) noexcept {
        switch (action) {
            case RuntimeBudgetAction::Run:
                ++stats_.run_count;
                if (stats_.consumed_budget
                    > std::numeric_limits<std::uint64_t>::max() - estimated_cost) {
                    stats_.consumed_budget = std::numeric_limits<std::uint64_t>::max();
                } else {
                    stats_.consumed_budget += estimated_cost;
                }
                if (remaining_budget_ >= estimated_cost) {
                    remaining_budget_ -= estimated_cost;
                } else {
                    remaining_budget_ = 0;
                }
                break;
            case RuntimeBudgetAction::Defer:
                ++stats_.defer_count;
                break;
            case RuntimeBudgetAction::Shed:
                ++stats_.shed_count;
                break;
            case RuntimeBudgetAction::Bypass:
                ++stats_.bypass_count;
                break;
        }
        stats_.remaining_budget = remaining_budget_;
    }

    RuntimeBudgetPolicy policy_{};
    std::uint64_t remaining_budget_ = 0;
    bool overload_active_ = false;
    RuntimeBudgetFrameStats stats_{};
};

} // namespace pulp::runtime
