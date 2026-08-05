#pragma once

#include <pulp/inspect/main_thread_rpc.hpp>
#include <pulp/inspect/runtime_evaluator.hpp>
#include <pulp/inspect/session.hpp>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>

namespace pulp::format::detail {

// Owns the narrow concurrency exception needed by Runtime.evaluate: evaluation
// is marshaled to the UI thread while Runtime.interrupt remains callable from
// the authenticated controller's transport thread.
class StandaloneRuntimeEvalDispatch {
public:
    using RuntimeHandler =
        std::function<inspect::InspectorMessage(const inspect::InspectorMessage&)>;
    using EvaluatorHandler = std::function<inspect::InspectorMessage(
        inspect::RuntimeEvaluator*)>;

    void install(inspect::InspectorSession& session,
                 std::shared_ptr<inspect::InspectorMainThreadRpc> rpc,
                 RuntimeHandler runtime_handler);

    inspect::InspectorMessage with_evaluator(
        const inspect::InspectorMessage& request,
        inspect::RuntimeEvaluator* evaluator,
        const EvaluatorHandler& handler);

private:
    inspect::InspectorMessage evaluate(const inspect::InspectorMessage& request);
    inspect::InspectorMessage interrupt(const inspect::InspectorMessage& request);

    std::shared_ptr<inspect::InspectorMainThreadRpc> rpc_;
    RuntimeHandler runtime_handler_;
    std::atomic<bool> request_active_{false};
    std::mutex evaluator_mutex_;
    inspect::RuntimeEvaluator* active_evaluator_ = nullptr;
};

} // namespace pulp::format::detail
