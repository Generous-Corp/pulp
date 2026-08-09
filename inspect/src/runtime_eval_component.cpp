// runtime_eval_component.cpp — high-risk live-realm execution component

#include <pulp/inspect/runtime_eval_component.hpp>

#if defined(_MSC_VER)
#define PULP_CONTROL_COMPONENT_MARKER __declspec(dllexport)
#else
#define PULP_CONTROL_COMPONENT_MARKER __attribute__((used, visibility("default")))
#endif

extern "C" PULP_CONTROL_COMPONENT_MARKER const volatile char
    pulp_control_runtime_eval_capability_marker_v1[] =
        "PULP_INSPECT_CAPABILITY_RUNTIME_EVAL_V1";

#undef PULP_CONTROL_COMPONENT_MARKER

#include <pulp/view/script_inspector_bridge.hpp>
#include <pulp/view/scripted_ui.hpp>

#include <string>
#include <optional>
#include <utility>

namespace pulp::inspect {
namespace {

constexpr char kHighRiskBinaryMarker[] =
    "PULP_INSPECT_RUNTIME_EVAL_HIGH_RISK_COMPONENT_V1";

class ScriptRuntimeEvaluator final : public RuntimeEvaluator {
public:
    explicit ScriptRuntimeEvaluator(view::ScriptInspectorBridge* bridge)
        : bridge_(bridge) {}
    explicit ScriptRuntimeEvaluator(ScriptedUiSessionVisitor visit_session)
        : visit_session_(std::move(visit_session)) {}

    RuntimeEvaluatorCapabilities capabilities() const override {
        RuntimeEvaluatorCapabilities out;
        std::optional<view::ScriptInspectorBridge::Capabilities> capabilities;
        with_bridge([&](view::ScriptInspectorBridge& bridge) {
            capabilities = bridge.capabilities();
        });
        if (!capabilities)
            return out;
        const auto& caps = *capabilities;
        out.engine = caps.engine;
        out.can_evaluate = caps.can_evaluate;
        out.can_interrupt = caps.can_interrupt;
        out.can_break = caps.can_break;
        out.can_step = caps.can_step;
        out.can_inspect_locals = caps.can_inspect_locals;
        return out;
    }

    RuntimeEvaluationResult evaluate(std::string_view code, std::chrono::milliseconds timeout,
                                     std::size_t maximum_result_bytes) override {
        RuntimeEvaluationResult out;
        if (code.size() > kRuntimeEvalMaxCodeBytes) {
            out.error = "Runtime.evaluate code exceeds the 65536-byte limit";
            return out;
        }
        if (code.find('\0') != std::string_view::npos) {
            out.error = "Runtime.evaluate code contains a NUL byte";
            return out;
        }
        std::optional<view::ScriptInspectorBridge::EvalResult> result;
        with_bridge([&](view::ScriptInspectorBridge& bridge) {
            result = bridge.evaluate(std::string(code), timeout, maximum_result_bytes);
        });
        if (!result) {
            out.detached = true;
            out.error = "no scripted-UI engine attached";
            return out;
        }
        out.ok = result->ok;
        out.timed_out = result->timed_out;
        out.busy = result->busy;
        out.detached = result->detached;
        out.json = result->json;
        out.error = result->error;
        if (out.ok && out.json.size() > maximum_result_bytes) {
            out = RuntimeEvaluationResult{};
            out.error = "Runtime.evaluate result exceeds the 1048576-byte limit";
        }
        return out;
    }

    bool interrupt() override {
        bool interrupted = false;
        with_bridge([&](view::ScriptInspectorBridge& bridge) {
            interrupted = bridge.interrupt();
        });
        return interrupted;
    }

    std::string_view binary_marker() const noexcept override {
        return kHighRiskBinaryMarker;
    }

private:
    void with_bridge(const std::function<void(view::ScriptInspectorBridge&)>& visitor) const {
        if (!visitor)
            return;
        if (visit_session_) {
            visit_session_([&](view::ScriptedUiSession* session) {
                if (session)
                    visitor(*session->script_inspector());
            });
            return;
        }
        if (bridge_)
            visitor(*bridge_);
    }

    view::ScriptInspectorBridge* bridge_ = nullptr;
    ScriptedUiSessionVisitor visit_session_;
};

} // namespace

std::unique_ptr<RuntimeEvaluator>
make_script_runtime_evaluator(view::ScriptInspectorBridge* bridge) {
    return std::make_unique<ScriptRuntimeEvaluator>(bridge);
}

std::unique_ptr<RuntimeEvaluator>
make_script_runtime_evaluator(ScriptedUiSessionVisitor visit_session) {
    return std::make_unique<ScriptRuntimeEvaluator>(std::move(visit_session));
}

} // namespace pulp::inspect
