#include <pulp/inspect/control_host_router.hpp>

#include <pulp/runtime/crypto.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pulp::inspect {
namespace {

using namespace std::chrono_literals;

ControlExecutionOutcome failure(ControlResultCode code, ControlRetryClassification retry,
                                std::string explanation) {
    return {
        .terminal_state = ControlReceiptState::Failed,
        .result = {.result_code = code, .retry = retry, .explanation = std::move(explanation)},
    };
}

ControlExecutionOutcome checkpoint_outcome(ControlExecutionCheckpoint checkpoint) {
    switch (checkpoint) {
    case ControlExecutionCheckpoint::Cancelled:
        return {
            .terminal_state = ControlReceiptState::Cancelled,
            .result = {.result_code = ControlResultCode::Cancelled,
                       .explanation = "operation cancelled before host dispatch",
                       .cancellation_reason = "cancelled-before-host-dispatch"},
        };
    case ControlExecutionCheckpoint::DeadlineExceeded:
        return failure(ControlResultCode::DeadlineExceeded, ControlRetryClassification::Never,
                       "operation deadline elapsed before host dispatch");
    case ControlExecutionCheckpoint::AuthorityRevoked:
        return {
            .terminal_state = ControlReceiptState::Cancelled,
            .result = {.result_code = ControlResultCode::Cancelled,
                       .explanation = "operation authority revoked before host dispatch",
                       .cancellation_reason = "authority-revoked-before-host-dispatch"},
        };
    case ControlExecutionCheckpoint::Continue:
        break;
    }
    return failure(ControlResultCode::InternalError, ControlRetryClassification::Never,
                   "invalid execution checkpoint");
}

ControlExecutionOutcome unknown_after_delivery(std::string explanation, bool deferred) {
    return {
        .terminal_state = ControlReceiptState::UnknownNeedsRefresh,
        .result = {.result_code = ControlResultCode::UnknownNeedsRefresh,
                   .retry = ControlRetryClassification::AfterRefresh,
                   .explanation = std::move(explanation)},
        .deferred = deferred,
    };
}

std::string route_id() {
    const auto bytes = runtime::secure_random_bytes(16);
    return bytes ? "route-" + runtime::hex_encode(*bytes) : std::string{};
}

std::int64_t unix_ms_now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

class ControlHostRouter::Impl {
  public:
    struct Host {
        ConnectionGeneration generation = 0;
        Sender sender;
    };

    struct Pending {
        std::mutex mutex;
        std::condition_variable ready;
        std::string route_id;
        ControlRegistrationId registration_id;
        ConnectionGeneration generation = 0;
        ControlProgressReporter report_progress;
        ControlDeferredCompletion complete_deferred;
        std::optional<ControlExecutionOutcome> outcome;
        bool delivery_started = false;
        bool returned_deferred = false;
        bool cancel_sent = false;
        bool finished = false;
    };

    bool attach(const ControlRegistrationId& registration_id, ConnectionGeneration generation,
                Sender sender) {
        if (!registration_id || generation == 0 || !sender)
            return false;
        std::lock_guard lock(mutex);
        if (stopping || hosts.contains(registration_id.value))
            return false;
        hosts.emplace(registration_id.value, Host{generation, std::move(sender)});
        return true;
    }

    void detach(const ControlRegistrationId& registration_id, ConnectionGeneration generation) {
        std::vector<std::shared_ptr<Pending>> detached;
        {
            std::lock_guard lock(mutex);
            const auto host = hosts.find(registration_id.value);
            if (host == hosts.end() || host->second.generation != generation)
                return;
            hosts.erase(host);
            for (auto it = pending.begin(); it != pending.end();) {
                if (it->second->registration_id == registration_id &&
                    it->second->generation == generation) {
                    detached.push_back(it->second);
                    it = pending.erase(it);
                } else {
                    ++it;
                }
            }
        }
        for (const auto& route : detached) {
            bool deferred = false;
            bool delivery_started = false;
            {
                std::lock_guard lock(route->mutex);
                deferred = route->returned_deferred;
                delivery_started = route->delivery_started;
            }
            finish(route,
                   delivery_started
                       ? unknown_after_delivery(
                             "host disconnected during or after execution delivery", deferred)
                       : failure(ControlResultCode::HostUnavailable,
                                 ControlRetryClassification::AfterBackoff,
                                 "host disconnected before execution delivery"));
        }
    }

    bool connected(const ControlRegistrationId& registration_id) const {
        std::lock_guard lock(mutex);
        return !stopping && hosts.contains(registration_id.value);
    }

    bool receive(const ControlRegistrationId& registration_id, ConnectionGeneration generation,
                 const ControlEnvelope& envelope) {
        if (!control_envelope_allowed(envelope, ControlEnvelopeDirection::HostToBroker))
            return false;
        const auto route = std::visit(
            [](const auto& payload) -> std::string_view {
                using T = std::decay_t<decltype(payload)>;
                if constexpr (std::is_same_v<T, ControlHostProgressEnvelope> ||
                              std::is_same_v<T, ControlHostCompleteEnvelope>)
                    return payload.route_id;
                return {};
            },
            envelope.payload);
        if (route.empty())
            return false;
        std::shared_ptr<Pending> operation;
        {
            std::lock_guard lock(mutex);
            const auto found = pending.find(std::string(route));
            if (found == pending.end() || found->second->registration_id != registration_id ||
                found->second->generation != generation)
                return false;
            operation = found->second;
            if (std::holds_alternative<ControlHostCompleteEnvelope>(envelope.payload))
                pending.erase(found);
        }
        if (const auto* progress = std::get_if<ControlHostProgressEnvelope>(&envelope.payload)) {
            ControlProgressReporter reporter;
            {
                std::lock_guard lock(operation->mutex);
                if (operation->outcome)
                    return false;
                reporter = operation->report_progress;
            }
            if (reporter)
                (void)reporter(progress->current, progress->total, progress->detail_json);
            return true;
        }
        const auto& completed = std::get<ControlHostCompleteEnvelope>(envelope.payload);
        finish(operation, {.terminal_state = completed.terminal_state,
                           .result = {.result_code = completed.result_code,
                                      .retry = completed.retry,
                                      .explanation = completed.explanation,
                                      .detail_json = completed.detail_json,
                                      .cancellation_reason = completed.cancellation_reason}});
        return true;
    }

    ControlExecutionOutcome execute(const ControlAdmissionPlan& plan,
                                    const ControlRequestEnvelope& request,
                                    const ControlExecutionContext& context) {
        if (!context.checkpoint)
            return failure(ControlResultCode::HostUnavailable,
                           ControlRetryClassification::AfterBackoff,
                           "host execution checkpoint is unavailable");
        const auto initial = context.checkpoint();
        if (initial != ControlExecutionCheckpoint::Continue)
            return checkpoint_outcome(initial);

        Host host;
        auto operation = std::make_shared<Pending>();
        operation->registration_id = plan.registration_id;
        operation->report_progress = context.report_progress;
        operation->complete_deferred = context.complete_deferred;
        {
            std::lock_guard lock(mutex);
            const auto found = hosts.find(plan.registration_id.value);
            if (stopping || found == hosts.end())
                return failure(ControlResultCode::HostUnavailable,
                               ControlRetryClassification::AfterBackoff,
                               "no authenticated host owns the registration");
            host = found->second;
            operation->generation = host.generation;
            operation->delivery_started = true;
            bool inserted = false;
            for (unsigned attempt = 0; attempt != 4; ++attempt) {
                operation->route_id = route_id();
                if (!operation->route_id.empty() &&
                    (inserted = pending.emplace(operation->route_id, operation).second))
                    break;
            }
            if (!inserted)
                return failure(ControlResultCode::ResourceExhausted,
                               ControlRetryClassification::AfterBackoff,
                               "host route identity is unavailable");
        }

        const ControlEnvelope frame{
            .payload = ControlHostExecuteEnvelope{.route_id = operation->route_id,
                                                  .receipt_id = plan.receipt_id.value,
                                                  .operation_id = request.operation_id,
                                                  .operation_version = request.operation_version,
                                                  .deadline_unix_ms = plan.deadline_unix_ms,
                                                  .expected_state_generation =
                                                      request.expected_state_generation,
                                                  .params_json = request.params_json}};
        if (!host.sender(frame)) {
            erase(operation);
            return failure(ControlResultCode::HostUnavailable,
                           ControlRetryClassification::AfterBackoff,
                           "host execution frame could not be delivered");
        }
        for (;;) {
            std::unique_lock route_lock(operation->mutex);
            if (operation->outcome)
                return std::move(*operation->outcome);
            const auto remaining_ms = plan.deadline_unix_ms - unix_ms_now();
            if (remaining_ms <= 0) {
                operation->returned_deferred = true;
                operation->cancel_sent = true;
                route_lock.unlock();
                (void)host.sender(ControlEnvelope{
                    .payload = ControlHostCancelEnvelope{.route_id = operation->route_id,
                                                         .reason = "deadline-exceeded"}});
                return unknown_after_delivery("host execution crossed its response deadline", true);
            }
            operation->ready.wait_for(route_lock,
                                      std::min(25ms, std::chrono::milliseconds{remaining_ms}));
            if (operation->outcome)
                return std::move(*operation->outcome);
            route_lock.unlock();
            const auto checkpoint = context.checkpoint();
            if (checkpoint != ControlExecutionCheckpoint::Continue) {
                route_lock.lock();
                if (operation->outcome)
                    return std::move(*operation->outcome);
                operation->returned_deferred = true;
                const bool should_cancel = !operation->cancel_sent;
                operation->cancel_sent = true;
                route_lock.unlock();
                if (should_cancel) {
                    const auto reason = checkpoint == ControlExecutionCheckpoint::Cancelled
                                            ? "client-cancelled"
                                        : checkpoint == ControlExecutionCheckpoint::DeadlineExceeded
                                            ? "deadline-exceeded"
                                            : "authority-revoked";
                    (void)host.sender(
                        ControlEnvelope{.payload = ControlHostCancelEnvelope{
                                            .route_id = operation->route_id, .reason = reason}});
                }
                return unknown_after_delivery(
                    checkpoint == ControlExecutionCheckpoint::AuthorityRevoked
                        ? "host execution continued after authority revocation"
                    : checkpoint == ControlExecutionCheckpoint::Cancelled
                        ? "host execution continued after client cancellation"
                        : "host execution continued after its deadline",
                    true);
            }
        }
    }

    void stop() {
        std::vector<std::pair<ControlRegistrationId, ConnectionGeneration>> snapshot;
        {
            std::lock_guard lock(mutex);
            if (stopping)
                return;
            stopping = true;
            for (const auto& [registration, host] : hosts)
                snapshot.emplace_back(ControlRegistrationId{registration}, host.generation);
        }
        for (const auto& [registration, generation] : snapshot)
            detach(registration, generation);
    }

  private:
    void erase(const std::shared_ptr<Pending>& operation) {
        std::lock_guard lock(mutex);
        const auto found = pending.find(operation->route_id);
        if (found != pending.end() && found->second == operation)
            pending.erase(found);
    }

    static void finish(const std::shared_ptr<Pending>& operation, ControlExecutionOutcome outcome) {
        ControlDeferredCompletion deferred;
        {
            std::lock_guard lock(operation->mutex);
            if (operation->finished)
                return;
            operation->finished = true;
            if (operation->returned_deferred)
                deferred = operation->complete_deferred;
            else
                operation->outcome = outcome;
        }
        operation->ready.notify_all();
        if (deferred)
            deferred(std::move(outcome));
    }

    mutable std::mutex mutex;
    bool stopping = false;
    std::unordered_map<std::string, Host> hosts;
    std::unordered_map<std::string, std::shared_ptr<Pending>> pending;
};

ControlHostRouter::ControlHostRouter() : impl_(std::make_shared<Impl>()) {}
ControlHostRouter::~ControlHostRouter() {
    stop();
}

bool ControlHostRouter::attach(const ControlRegistrationId& registration_id,
                               ConnectionGeneration generation, Sender sender) {
    return impl_->attach(registration_id, generation, std::move(sender));
}

void ControlHostRouter::detach(const ControlRegistrationId& registration_id,
                               ConnectionGeneration generation) noexcept {
    impl_->detach(registration_id, generation);
}

bool ControlHostRouter::connected(const ControlRegistrationId& registration_id) const {
    return impl_->connected(registration_id);
}

bool ControlHostRouter::receive(const ControlRegistrationId& registration_id,
                                ConnectionGeneration generation, const ControlEnvelope& envelope) {
    return impl_->receive(registration_id, generation, envelope);
}

ControlOperationExecutor ControlHostRouter::executor() const {
    const auto impl = impl_;
    return [impl](const ControlAdmissionPlan& plan, const ControlRequestEnvelope& request,
                  const ControlExecutionContext& context) {
        return impl->execute(plan, request, context);
    };
}

void ControlHostRouter::stop() noexcept {
    impl_->stop();
}

} // namespace pulp::inspect
