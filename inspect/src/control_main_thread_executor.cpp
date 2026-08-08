#include <pulp/inspect/control_main_thread_executor.hpp>

#include <algorithm>
#include <atomic>
#include <exception>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace pulp::inspect {
namespace {

std::optional<std::int64_t> next_rpc_request_id() {
    static std::atomic<std::int64_t> next{1};
    auto current = next.load(std::memory_order_relaxed);
    while (current > 0) {
        const auto following =
            current == std::numeric_limits<std::int64_t>::max() ? 0 : current + 1;
        if (next.compare_exchange_weak(current, following, std::memory_order_relaxed,
                                       std::memory_order_relaxed)) {
            return current;
        }
    }
    return std::nullopt;
}

ControlExecutionOutcome failed(ControlResultCode code, ControlRetryClassification retry,
                               std::string explanation) {
    return {
        .terminal_state = ControlReceiptState::Failed,
        .result =
            {
                .result_code = code,
                .retry = retry,
                .explanation = std::move(explanation),
            },
    };
}

ControlExecutionOutcome cancelled(std::string explanation) {
    return {
        .terminal_state = ControlReceiptState::Cancelled,
        .result =
            {
                .result_code = ControlResultCode::Cancelled,
                .retry = ControlRetryClassification::Never,
                .explanation = explanation,
                .cancellation_reason = std::move(explanation),
            },
    };
}

ControlExecutionOutcome unknown_after_started_timeout() {
    return {
        .terminal_state = ControlReceiptState::UnknownNeedsRefresh,
        .result =
            {
                .result_code = ControlResultCode::UnknownNeedsRefresh,
                .retry = ControlRetryClassification::AfterRefresh,
                .explanation = "main-thread execution crossed its response deadline",
            },
        .deferred = true,
    };
}

ControlExecutionOutcome handler_failure(std::string explanation) {
    return failed(ControlResultCode::InternalError, ControlRetryClassification::Never,
                  std::move(explanation));
}

ControlExecutionOutcome checkpoint_failure(ControlExecutionCheckpoint checkpoint) {
    switch (checkpoint) {
    case ControlExecutionCheckpoint::Continue:
        return {};
    case ControlExecutionCheckpoint::DeadlineExceeded:
        return failed(ControlResultCode::DeadlineExceeded, ControlRetryClassification::Never,
                      "operation deadline elapsed before main-thread apply");
    case ControlExecutionCheckpoint::Cancelled:
        return cancelled("operation cancelled before main-thread apply");
    case ControlExecutionCheckpoint::AuthorityRevoked:
        return cancelled("operation authority was revoked before main-thread apply");
    }
    return cancelled("operation authority is unavailable");
}

std::optional<bool> timeout_may_have_applied(const InspectorMessage& response) {
    if (response.error_data_json.find(R"("mayHaveApplied":true)") != std::string::npos)
        return true;
    if (response.error_data_json.find(R"("mayHaveApplied":false)") != std::string::npos)
        return false;
    return std::nullopt;
}

} // namespace

class ControlMainThreadExecutor::Impl {
  public:
    Impl(std::shared_ptr<InspectorMainThreadRpc> rpc_in, Handler handler_in)
        : rpc(std::move(rpc_in)), handler(std::move(handler_in)) {}

    ControlExecutionOutcome execute(const ControlAdmissionPlan& plan,
                                    const ControlRequestEnvelope& request,
                                    const ControlExecutionContext& context) const {
        if (!rpc || !handler) {
            return failed(ControlResultCode::HostUnavailable,
                          ControlRetryClassification::AfterBackoff,
                          "main-thread control executor is unavailable");
        }
        const auto request_id = next_rpc_request_id();
        if (!request_id) {
            return failed(ControlResultCode::ResourceExhausted, ControlRetryClassification::Never,
                          "main-thread RPC request sequence is exhausted");
        }
        const auto now = std::chrono::system_clock::now();
        const auto deadline =
            std::chrono::system_clock::time_point{std::chrono::milliseconds{plan.deadline_unix_ms}};
        if (deadline <= now)
            return checkpoint_failure(ControlExecutionCheckpoint::DeadlineExceeded);
        auto timeout = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        if (timeout <= std::chrono::milliseconds::zero())
            timeout = std::chrono::milliseconds{1};
        timeout = std::min(timeout, rpc->default_timeout());

        struct SharedOutcome {
            std::mutex mutex;
            std::optional<ControlExecutionOutcome> value;
            bool fenced = false;
        };
        auto outcome = std::make_shared<SharedOutcome>();
        auto plan_copy = plan;
        auto request_copy = request;
        auto context_copy = context;
        auto handler_copy = handler;
        const auto response = rpc->call_queued_only(
            *request_id,
            [outcome, plan = std::move(plan_copy), request = std::move(request_copy),
             context = std::move(context_copy), handler = std::move(handler_copy),
             id = *request_id]() mutable {
                const ControlProgressReporter gated_progress =
                    [outcome, progress = context.report_progress](
                        std::uint64_t current, std::uint64_t total, std::string detail_json) {
                        std::lock_guard lock(outcome->mutex);
                        if (outcome->fenced)
                            return false;
                        return progress && progress(current, total, std::move(detail_json));
                    };
                ControlExecutionOutcome value;
                const auto checkpoint = context.checkpoint
                                            ? context.checkpoint()
                                            : ControlExecutionCheckpoint::AuthorityRevoked;
                if (checkpoint == ControlExecutionCheckpoint::Continue) {
                    auto handler_context = context;
                    handler_context.report_progress = gated_progress;
                    handler_context.complete_deferred = {};
                    try {
                        value = handler(plan, request, handler_context);
                        if (value.deferred) {
                            value = handler_failure(
                                "main-thread handler returned a nested deferred outcome");
                        }
                    } catch (const std::exception& error) {
                        value = handler_failure(error.what());
                    } catch (...) {
                        value = handler_failure("main-thread handler threw an unknown exception");
                    }
                } else {
                    value = checkpoint_failure(checkpoint);
                }
                ControlDeferredCompletion deferred;
                {
                    std::lock_guard lock(outcome->mutex);
                    if (outcome->fenced)
                        deferred = context.complete_deferred;
                    else
                        outcome->value = value;
                }
                if (deferred)
                    deferred(std::move(value));
                return make_response(id, "{}");
            },
            {}, timeout);

        if (!response.is_error) {
            std::lock_guard lock(outcome->mutex);
            outcome->fenced = true;
            if (outcome->value)
                return std::move(*outcome->value);
            return failed(ControlResultCode::InternalError, ControlRetryClassification::Never,
                          "main-thread handler returned no typed outcome");
        }
        {
            std::lock_guard lock(outcome->mutex);
            outcome->fenced = true;
        }
        if (response.error_code == "main_thread_timeout") {
            const auto may_have_applied = timeout_may_have_applied(response);
            if (!may_have_applied || *may_have_applied) {
                std::optional<ControlExecutionOutcome> completed_at_fence;
                {
                    std::lock_guard lock(outcome->mutex);
                    if (outcome->value)
                        completed_at_fence = std::move(outcome->value);
                }
                if (completed_at_fence && context.complete_deferred)
                    context.complete_deferred(std::move(*completed_at_fence));
                return unknown_after_started_timeout();
            }
            return failed(ControlResultCode::DeadlineExceeded, ControlRetryClassification::Never,
                          "main-thread execution timed out before it started");
        }
        if (response.error_code == "dispatch_queue_full") {
            return failed(ControlResultCode::ResourceExhausted,
                          ControlRetryClassification::AfterBackoff,
                          "main-thread execution queue is full");
        }
        if (response.error_code == "dispatch_cancelled") {
            return cancelled("main-thread execution cancelled during teardown");
        }
        if (response.error_code == "main_thread_unavailable") {
            return failed(ControlResultCode::HostUnavailable,
                          ControlRetryClassification::AfterBackoff,
                          "main-thread dispatcher is unavailable");
        }
        if (response.error_code == "direct_main_thread_forbidden") {
            return failed(ControlResultCode::HostUnavailable,
                          ControlRetryClassification::AfterBackoff,
                          "main-thread control execution requires a worker-thread caller");
        }
        return failed(ControlResultCode::InternalError, ControlRetryClassification::Never,
                      response.params_json.empty() ? "main-thread execution failed"
                                                   : response.params_json);
    }

    std::shared_ptr<InspectorMainThreadRpc> rpc;
    Handler handler;
};

ControlMainThreadExecutor::ControlMainThreadExecutor(std::shared_ptr<InspectorMainThreadRpc> rpc,
                                                     Handler handler)
    : impl_(std::make_shared<Impl>(std::move(rpc), std::move(handler))) {}

ControlOperationExecutor ControlMainThreadExecutor::executor() const {
    const auto impl = impl_;
    return [impl](const ControlAdmissionPlan& plan, const ControlRequestEnvelope& request,
                  const ControlExecutionContext& context) {
        return impl->execute(plan, request, context);
    };
}

} // namespace pulp::inspect
