// runtime_evaluator.hpp — narrow optional execution seam for Runtime.*
#pragma once

#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>

namespace pulp::inspect {

inline constexpr std::size_t kRuntimeEvalMaxCodeBytes = 64u * 1024u;
inline constexpr std::size_t kRuntimeEvalMaxResultBytes = 1024u * 1024u;
inline constexpr std::size_t kRuntimeEvalMaxResponseBytes = 1024u * 1024u;
inline constexpr std::chrono::milliseconds kRuntimeEvalDeadline{2000};

struct RuntimeEvaluatorCapabilities {
    std::string engine;
    bool can_evaluate = false;
    bool can_interrupt = false;
    bool can_break = false;
    bool can_step = false;
    bool can_inspect_locals = false;
};

struct RuntimeEvaluationResult {
    bool ok = false;
    bool timed_out = false;
    bool busy = false;
    bool detached = false;
    std::string json;
    std::string error;
};

/// Host-injected arbitrary-execution boundary. The base inspector owns only
/// routing and status; implementations live in separately linked components.
class RuntimeEvaluator {
public:
    virtual ~RuntimeEvaluator() = default;
    virtual RuntimeEvaluatorCapabilities capabilities() const = 0;
    virtual RuntimeEvaluationResult evaluate(std::string_view code) = 0;
    virtual bool interrupt() = 0;
    /// A component-specific marker retained through the evaluator vtable.
    virtual std::string_view binary_marker() const noexcept = 0;
};

} // namespace pulp::inspect
