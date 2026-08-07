#pragma once

#include <pulp/inspect/control_admission.hpp>

#include <cstdint>
#include <functional>
#include <string>

namespace pulp::inspect {

struct ControlExecutionOutcome {
    ControlReceiptState terminal_state = ControlReceiptState::Completed;
    ControlOperationResult result;
    /// The caller must return an UnknownNeedsRefresh observation without
    /// terminalizing the durable receipt. `complete_deferred` will settle it
    /// when the already-started legal-thread work actually returns.
    bool deferred = false;
};

using ControlProgressReporter =
    std::function<bool(std::uint64_t current, std::uint64_t total, std::string detail_json)>;
using ControlExecutionGuard = std::function<ControlExecutionCheckpoint()>;
using ControlDeferredCompletion = std::function<void(ControlExecutionOutcome)>;

struct ControlExecutionContext {
    ControlProgressReporter report_progress;
    ControlExecutionGuard checkpoint;
    ControlDeferredCompletion complete_deferred;
};

/// Trusted in-process adapter contract. An executor must never wait past the
/// plan deadline itself: it either completes within the bound or promptly
/// returns `deferred = true` and later invokes `complete_deferred`. The service
/// cannot safely preempt arbitrary C++ running in its process. The supplied
/// main-thread adapter enforces this contract with a fenced, bounded RPC.
using ControlOperationExecutor = std::function<ControlExecutionOutcome(
    const ControlAdmissionPlan&, const ControlRequestEnvelope&, const ControlExecutionContext&)>;

} // namespace pulp::inspect
