// runtime_eval_component.cpp — high-risk live-realm execution component

#include <pulp/inspect/runtime_eval_component.hpp>

#include <pulp/view/script_inspector_bridge.hpp>

#include <string>

namespace pulp::inspect {
namespace {

constexpr char kHighRiskBinaryMarker[] =
    "PULP_INSPECT_RUNTIME_EVAL_HIGH_RISK_COMPONENT_V1";

class ScriptRuntimeEvaluator final : public RuntimeEvaluator {
public:
    explicit ScriptRuntimeEvaluator(view::ScriptInspectorBridge* bridge)
        : bridge_(bridge) {}

    RuntimeEvaluatorCapabilities capabilities() const override {
        RuntimeEvaluatorCapabilities out;
        if (!bridge_)
            return out;
        const auto caps = bridge_->capabilities();
        out.engine = caps.engine;
        out.can_evaluate = caps.can_evaluate;
        out.can_interrupt = caps.can_interrupt;
        out.can_break = caps.can_break;
        out.can_step = caps.can_step;
        out.can_inspect_locals = caps.can_inspect_locals;
        return out;
    }

    RuntimeEvaluationResult evaluate(std::string_view code) override {
        RuntimeEvaluationResult out;
        if (code.size() > kRuntimeEvalMaxCodeBytes) {
            out.error = "Runtime.evaluate code exceeds the 65536-byte limit";
            return out;
        }
        if (code.find('\0') != std::string_view::npos) {
            out.error = "Runtime.evaluate code contains a NUL byte";
            return out;
        }
        if (!bridge_) {
            out.detached = true;
            out.error = "no scripted-UI engine attached";
            return out;
        }
        const auto result = bridge_->evaluate(std::string(code), kRuntimeEvalDeadline);
        out.ok = result.ok;
        out.timed_out = result.timed_out;
        out.busy = result.busy;
        out.detached = result.detached;
        out.json = result.json;
        out.error = result.error;
        if (out.ok && out.json.size() > kRuntimeEvalMaxResultBytes) {
            out = RuntimeEvaluationResult{};
            out.error = "Runtime.evaluate result exceeds the 1048576-byte limit";
        }
        return out;
    }

    bool interrupt() override {
        return bridge_ && bridge_->interrupt();
    }

    std::string_view binary_marker() const noexcept override {
        return kHighRiskBinaryMarker;
    }

private:
    view::ScriptInspectorBridge* bridge_ = nullptr;
};

} // namespace

std::unique_ptr<RuntimeEvaluator>
make_script_runtime_evaluator(view::ScriptInspectorBridge* bridge) {
    return std::make_unique<ScriptRuntimeEvaluator>(bridge);
}

} // namespace pulp::inspect
