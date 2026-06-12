#pragma once

/// @file budget_policy.hpp
/// Small runtime policy helper for budgeted work and graceful degradation.

#include <cstdint>

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

} // namespace pulp::runtime
