#include <pulp/inspect/control_host_router.hpp>

#include <pulp/runtime/base64.hpp>
#include <pulp/runtime/crypto.hpp>

#include <choc/text/choc_JSON.h>

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
        std::string instance_id;
        std::string instance_generation;
        Sender sender;
        struct Projection {
            std::string authority_id;
            ControlClientId client_id;
            ControlGrantId grant_id;
        };
        std::unordered_map<std::string, Projection> projections;
        std::unordered_map<std::string, std::string> controller_authorities;
    };

    struct Pending {
        std::mutex mutex;
        std::condition_variable ready;
        std::string route_id;
        ControlRegistrationId registration_id;
        ConnectionGeneration generation = 0;
        ControlProgressReporter report_progress;
        ControlDeferredCompletion complete_deferred;
        ControlExecutionGuard checkpoint;
        ControlArtifactPublisher publish_artifact;
        std::size_t maximum_artifact_bytes = 0;
        std::optional<ControlExecutionOutcome> outcome;
        bool delivery_started = false;
        bool returned_deferred = false;
        bool cancel_sent = false;
        bool finished = false;
    };

    bool attach(const ControlRegistrationId& registration_id, ConnectionGeneration generation,
                Sender sender) {
        return attach_slot(registration_id, generation, {}, {}, std::move(sender));
    }

    bool attach_slot(const ControlRegistrationId& registration_id, ConnectionGeneration generation,
                     std::string instance_id, std::string instance_generation, Sender sender) {
        if (!registration_id || generation == 0 || !sender)
            return false;
        if (instance_id.empty() != instance_generation.empty())
            return false;
        std::lock_guard lock(mutex);
        if (stopping || hosts.contains(registration_id.value))
            return false;
        hosts.emplace(registration_id.value,
                      Host{generation, std::move(instance_id), std::move(instance_generation),
                           std::move(sender), {}, {}});
        return true;
    }

    void end_authority(const ControlClientId& client_id,
                       const ControlRegistrationId& registration_id,
                       const ControlGrantId& grant_id, std::string_view reason) {
        if (reason.empty())
            return;
        std::vector<std::pair<Sender, std::string>> notifications;
        {
            std::lock_guard lock(mutex);
            for (auto& [registration, host] : hosts) {
                if (registration_id && registration != registration_id.value)
                    continue;
                for (auto it = host.projections.begin(); it != host.projections.end();) {
                    const auto& projection = it->second;
                    if ((!client_id || projection.client_id == client_id) &&
                        (!grant_id || projection.grant_id == grant_id)) {
                        notifications.emplace_back(host.sender, projection.authority_id);
                        it = host.projections.erase(it);
                    } else {
                        ++it;
                    }
                }
                if (client_id && !grant_id)
                    host.controller_authorities.erase(client_id.value);
            }
        }
        for (auto& [sender, authority_id] : notifications)
            (void)sender(ControlEnvelope{.payload = ControlHostAuthorityEndEnvelope{
                                             .authority_id = std::move(authority_id),
                                             .reason = std::string(reason)}});
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
        ControlExecutionOutcome outcome{
            .terminal_state = completed.terminal_state,
            .result = {.result_code = completed.result_code,
                       .retry = completed.retry,
                       .explanation = completed.explanation,
                       .detail_json = completed.detail_json,
                       .cancellation_reason = completed.cancellation_reason}};
        if (!completed.artifact_publications.empty()) {
            std::size_t aggregate = 0;
            for (const auto& publication : completed.artifact_publications) {
                const auto checkpoint = operation->checkpoint
                                            ? operation->checkpoint()
                                            : ControlExecutionCheckpoint::AuthorityRevoked;
                const auto padding = publication.bytes_base64.ends_with("==")
                                         ? 2u
                                     : publication.bytes_base64.ends_with("=") ? 1u : 0u;
                const auto decoded_upper_bound =
                    publication.bytes_base64.size() / 4 * 3 - padding;
                if (checkpoint != ControlExecutionCheckpoint::Continue ||
                    decoded_upper_bound > operation->maximum_artifact_bytes ||
                    decoded_upper_bound >
                        kControlHostMaximumArtifactPublicationBytes - aggregate) {
                    outcome = checkpoint == ControlExecutionCheckpoint::Continue
                                  ? failure(ControlResultCode::ResourceExhausted,
                                            ControlRetryClassification::Never,
                                            "host artifact publication exceeded its broker bound")
                                  : checkpoint_outcome(checkpoint);
                    break;
                }
                const auto bytes = runtime::base64_decode(publication.bytes_base64);
                if (!bytes || bytes->empty() ||
                    bytes->size() > kControlHostMaximumArtifactPublicationBytes - aggregate ||
                    !operation->publish_artifact ||
                    publication.reference_id.rfind("host-publication-", 0) != 0) {
                    outcome = checkpoint == ControlExecutionCheckpoint::Continue
                                  ? failure(ControlResultCode::InvalidRequest,
                                            ControlRetryClassification::Never,
                                            "host artifact publication was malformed or forged")
                                  : checkpoint_outcome(checkpoint);
                    break;
                }
                aggregate += bytes->size();
                const auto reference = choc::json::getEscapedQuotedString(
                    publication.reference_id);
                const auto offset = outcome.result.detail_json.find(reference);
                if (offset == std::string::npos) {
                    outcome = failure(ControlResultCode::InvalidRequest,
                                      ControlRetryClassification::Never,
                                      "host artifact reference was not bound to its result");
                    break;
                }
                const auto stored = operation->publish_artifact(
                    *bytes, {.content_type = publication.content_type,
                             .sensitivity = publication.sensitivity,
                             .redaction_state = publication.redaction_state,
                             .lifetime = std::chrono::milliseconds{publication.lifetime_ms}});
                if (stored.status != ControlArtifactStatus::Stored || !stored.metadata) {
                    outcome = failure(stored.status == ControlArtifactStatus::ResourceExhausted
                                          ? ControlResultCode::ResourceExhausted
                                          : ControlResultCode::InternalError,
                                      ControlRetryClassification::Never,
                                      "broker rejected host artifact publication");
                    break;
                }
                const auto broker_id = choc::json::getEscapedQuotedString(
                    stored.metadata->artifact_id);
                outcome.result.detail_json.replace(offset, reference.size(), broker_id);
                outcome.result.artifacts.push_back(
                    {.artifact_id = stored.metadata->artifact_id,
                     .media_type = stored.metadata->content_type,
                     .byte_size = stored.metadata->byte_size});
            }
        }
        finish(operation, std::move(outcome));
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
        std::string authority_id;
        std::string controller_authority_id;
        auto operation = std::make_shared<Pending>();
        operation->registration_id = plan.registration_id;
        operation->report_progress = context.report_progress;
        operation->complete_deferred = context.complete_deferred;
        operation->checkpoint = context.checkpoint;
        operation->publish_artifact = context.publish_artifact;
        operation->maximum_artifact_bytes =
            std::min(context.maximum_artifact_bytes,
                     kControlHostMaximumArtifactPublicationBytes);
        {
            std::lock_guard lock(mutex);
            const auto found = hosts.find(plan.registration_id.value);
            if (stopping || found == hosts.end())
                return failure(ControlResultCode::HostUnavailable,
                               ControlRetryClassification::AfterBackoff,
                               "no authenticated host owns the registration");
            host = found->second;
            if (!host.instance_id.empty() &&
                (host.instance_id != plan.instance_id ||
                 host.instance_generation != plan.instance_generation)) {
                return failure(ControlResultCode::SessionStale,
                               ControlRetryClassification::AfterRefresh,
                               "admitted instance no longer identifies the attached host slot");
            }
            operation->generation = host.generation;
            const auto projection_key = plan.client_id.value + "\n" + plan.grant_id.value;
            auto projection = found->second.projections.find(projection_key);
            if (projection == found->second.projections.end()) {
                const auto bytes = runtime::secure_random_bytes(16);
                if (!bytes)
                    return failure(ControlResultCode::ResourceExhausted,
                                   ControlRetryClassification::AfterBackoff,
                                   "host authority projection is unavailable");
                Host::Projection value{.authority_id =
                                           "authority-" + runtime::hex_encode(*bytes),
                                       .client_id = plan.client_id,
                                       .grant_id = plan.grant_id};
                projection = found->second.projections.emplace(projection_key,
                                                               std::move(value)).first;
            }
            authority_id = projection->second.authority_id;
            auto controller = found->second.controller_authorities.find(plan.client_id.value);
            if (controller == found->second.controller_authorities.end()) {
                const auto bytes = runtime::secure_random_bytes(16);
                if (!bytes)
                    return failure(ControlResultCode::ResourceExhausted,
                                   ControlRetryClassification::AfterBackoff,
                                   "controller authority projection is unavailable");
                controller = found->second.controller_authorities
                                 .emplace(plan.client_id.value,
                                          "controller-" + runtime::hex_encode(*bytes))
                                 .first;
            }
            controller_authority_id = controller->second;
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
                                                  .authority_id = authority_id,
                                                  .controller_authority_id =
                                                      controller_authority_id,
                                                  .broker_id = plan.broker_id.value,
                                                  .session_id = plan.session_id,
                                                  .instance_id = plan.instance_id,
                                                  .publication_id = plan.publication_id,
                                                  .instance_generation = plan.instance_generation,
                                                  .capability_id = std::string(capability_id(plan.capability)),
                                                  .manifest_digest = plan.manifest_digest,
                                                  .producer_artifact_digest =
                                                      plan.producer_artifact_digest,
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
                    if (checkpoint == ControlExecutionCheckpoint::AuthorityRevoked)
                        end_authority(plan.client_id, plan.registration_id, plan.grant_id,
                                      "authority-revoked");
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

bool ControlHostRouter::attach_slot(const ControlRegistrationId& registration_id,
                                    ConnectionGeneration generation, std::string instance_id,
                                    std::string instance_generation, Sender sender) {
    return impl_->attach_slot(registration_id, generation, std::move(instance_id),
                              std::move(instance_generation), std::move(sender));
}

void ControlHostRouter::detach(const ControlRegistrationId& registration_id,
                               ConnectionGeneration generation) noexcept {
    impl_->detach(registration_id, generation);
}

bool ControlHostRouter::connected(const ControlRegistrationId& registration_id) const {
    return impl_->connected(registration_id);
}

void ControlHostRouter::end_authority(const ControlClientId& client_id,
                                      const ControlRegistrationId& registration_id,
                                      const ControlGrantId& grant_id,
                                      std::string_view reason) noexcept {
    impl_->end_authority(client_id, registration_id, grant_id, reason);
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
