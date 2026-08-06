#include "standalone_runtime_eval_dispatch.hpp"

#include <pulp/inspect/protocol.hpp>

#include <utility>

namespace pulp::format::detail {

void StandaloneRuntimeEvalDispatch::install(
    inspect::InspectorSession& session,
    std::shared_ptr<inspect::InspectorMainThreadRpc> rpc,
    RuntimeHandler runtime_handler) {
    rpc_ = std::move(rpc);
    runtime_handler_ = std::move(runtime_handler);
    session.set_concurrent_request_handler(
        std::string(inspect::methods::kRuntimeInterrupt),
        [this](const inspect::InspectorRequestContext&,
               const inspect::InspectorMessage& request) {
            return interrupt(request);
        });
    session.set_concurrent_request_handler(
        std::string(inspect::methods::kRuntimeEvaluate),
        [this](const inspect::InspectorRequestContext&,
               const inspect::InspectorMessage& request) {
            return evaluate(request);
        });
}

inspect::InspectorMessage StandaloneRuntimeEvalDispatch::evaluate(
    const inspect::InspectorMessage& request) {
    bool expected = false;
    if (!request_active_.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
        return inspect::make_error(
            request.id,
            "Runtime.evaluate busy: another evaluation is in flight");
    }
    struct RequestGuard {
        std::atomic<bool>& active;
        ~RequestGuard() { active.store(false, std::memory_order_release); }
    } request_guard{request_active_};
    if (!rpc_ || !runtime_handler_)
        return inspect::make_error(request.id, "Runtime.evaluate dispatcher unavailable");
    const auto owned_request = request;
    return rpc_->call(request.id, [this, owned_request] {
        return runtime_handler_(owned_request);
    });
}

inspect::InspectorMessage StandaloneRuntimeEvalDispatch::interrupt(
    const inspect::InspectorMessage& request) {
    bool interrupted = false;
    {
        // The evaluator is borrowed only while the main-thread request is
        // inside DomainHandler. Retaining this lock through interrupt() keeps
        // that short-lived adapter alive across the cross-thread call.
        std::lock_guard lock(evaluator_mutex_);
        if (active_evaluator_)
            interrupted = active_evaluator_->interrupt();
    }
    return inspect::make_response(
        request.id,
        interrupted ? R"({"interrupted":true})"
                    : R"({"interrupted":false})");
}

inspect::InspectorMessage StandaloneRuntimeEvalDispatch::with_evaluator(
    const inspect::InspectorMessage& request,
    inspect::RuntimeEvaluator* evaluator,
    const EvaluatorHandler& handler) {
    const bool expose =
        evaluator && request.method == inspect::methods::kRuntimeEvaluate;
    if (expose) {
        std::lock_guard lock(evaluator_mutex_);
        active_evaluator_ = evaluator;
    }
    struct ActiveEvaluatorGuard {
        StandaloneRuntimeEvalDispatch& owner;
        inspect::RuntimeEvaluator* evaluator;
        ~ActiveEvaluatorGuard() {
            if (!evaluator)
                return;
            std::lock_guard lock(owner.evaluator_mutex_);
            if (owner.active_evaluator_ == evaluator)
                owner.active_evaluator_ = nullptr;
        }
    } active_guard{*this, expose ? evaluator : nullptr};
    return handler(evaluator);
}

} // namespace pulp::format::detail
