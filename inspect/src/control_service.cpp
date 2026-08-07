#include <pulp/inspect/control_service.hpp>

#include <choc/text/choc_UTF8.h>

#include <algorithm>
#include <condition_variable>
#include <exception>
#include <memory>
#include <mutex>
#include <utility>

namespace pulp::inspect {
namespace {

ControlReceiptEnvelope protocol_receipt(const ControlOperationReceipt& receipt,
                                        std::string_view response_request_id = {}) {
    return {
        .request_id = response_request_id.empty() ? receipt.binding.request_id
                                                  : std::string(response_request_id),
        .receipt_id = receipt.receipt_id.value,
        .operation_id = receipt.binding.operation_id,
        .operation_version = receipt.binding.operation_version,
        .state = receipt.state,
        .result_code = receipt.result.result_code,
        .retry = receipt.result.retry,
        .explanation = receipt.result.explanation,
        .detail_json = receipt.result.detail_json,
        .artifacts = receipt.result.artifacts,
    };
}

ControlServiceResult receipt_response(const ControlOperationReceipt& receipt,
                                      std::string_view response_request_id = {}) {
    return {
        .status = ControlServiceStatus::Responded,
        .response =
            ControlEnvelope{
                .schema_version = kControlProtocolVersion,
                .payload = protocol_receipt(receipt, response_request_id),
            },
    };
}

std::string persistable_internal_failure_explanation(std::string_view explanation) {
    constexpr std::string_view fallback = "executor threw an invalid exception message";
    if (explanation.empty() || explanation.size() > kControlReceiptMaximumExplanationBytes ||
        explanation.find('\0') != std::string_view::npos ||
        choc::text::findInvalidUTF8Data(explanation.data(), explanation.size()) != nullptr)
        return std::string(fallback);
    return std::string(explanation);
}

bool persistable_executor_text(std::string_view value, std::size_t maximum_bytes) {
    return value.size() <= maximum_bytes && value.find('\0') == std::string_view::npos &&
           choc::text::findInvalidUTF8Data(value.data(), value.size()) == nullptr;
}

bool persistable_executor_metadata(const ControlOperationResult& result) {
    return persistable_executor_text(result.explanation, kControlReceiptMaximumExplanationBytes) &&
           persistable_executor_text(result.cancellation_reason,
                                     kControlReceiptMaximumCancellationReasonBytes) &&
           result.evidence_ids.size() <= kControlReceiptMaximumEvidenceIds &&
           std::ranges::all_of(result.evidence_ids, [](const auto& evidence) {
               return !evidence.empty() &&
                      persistable_executor_text(evidence, kControlReceiptMaximumEvidenceIdBytes);
           });
}

ControlOperationResult internal_failure(std::string_view explanation) {
    return {
        .result_code = ControlResultCode::InternalError,
        .retry = ControlRetryClassification::Never,
        .explanation = persistable_internal_failure_explanation(explanation),
    };
}

} // namespace

std::string_view control_service_status_id(ControlServiceStatus status) {
    switch (status) {
    case ControlServiceStatus::Responded:
        return "responded";
    case ControlServiceStatus::InvalidEnvelope:
        return "invalid-envelope";
    case ControlServiceStatus::UnsupportedMessage:
        return "unsupported-message";
    case ControlServiceStatus::AdmissionDenied:
        return "admission-denied";
    case ControlServiceStatus::ReceiptTransitionFailed:
        return "receipt-transition-failed";
    case ControlServiceStatus::NegotiationRequired:
        return "negotiation-required";
    case ControlServiceStatus::ResourceExhausted:
        return "resource-exhausted";
    }
    return "invalid-envelope";
}

ControlService::ControlService(ControlBroker& broker, Executor executor, ProgressSink progress_sink,
                               ControlServiceConfig config)
    : broker_(broker), executor_(std::move(executor)), progress_sink_(std::move(progress_sink)),
      config_(config), completion_owner_(std::make_shared<CompletionOwner>()) {
    completion_owner_->broker = &broker_;
    if (config_.maximum_progress_events_per_operation == 0)
        config_.maximum_progress_events_per_operation = 1;
}

ControlService::~ControlService() {
    std::lock_guard lock(completion_owner_->mutex);
    if (completion_owner_->broker) {
        for (auto& [receipt_id, terminalize] : completion_owner_->deferred_operations) {
            (void)receipt_id;
            terminalize(*completion_owner_->broker);
        }
        completion_owner_->deferred_operations.clear();
    }
    completion_owner_->broker = nullptr;
}

ControlService::Session::~Session() {
    close();
}

ControlService::Session::Session(Session&& other) noexcept
    : service_(std::exchange(other.service_, nullptr)),
      broker_(std::exchange(other.broker_, nullptr)), peer_(std::move(other.peer_)),
      client_id_(std::move(other.client_id_)),
      negotiated_features_(std::move(other.negotiated_features_)) {}

ControlService::Session& ControlService::Session::operator=(Session&& other) noexcept {
    if (this == &other)
        return *this;
    close();
    service_ = std::exchange(other.service_, nullptr);
    broker_ = std::exchange(other.broker_, nullptr);
    peer_ = std::move(other.peer_);
    client_id_ = std::move(other.client_id_);
    negotiated_features_ = std::move(other.negotiated_features_);
    return *this;
}

void ControlService::Session::close() noexcept {
    service_ = nullptr;
    negotiated_features_.reset();
    if (!broker_)
        return;
    auto* broker = std::exchange(broker_, nullptr);
    (void)broker->disconnect_client(client_id_, peer_, "control-session-disconnect");
}

ControlServiceResult ControlService::Session::dispatch(std::string_view encoded_envelope) {
    return service_->dispatch(*this, encoded_envelope);
}

ControlArtifactReadResult ControlService::Session::read_artifact(std::string_view artifact_id,
                                                                 std::uint64_t offset,
                                                                 std::size_t maximum_bytes) {
    return service_->read_artifact(*this, artifact_id, offset, maximum_bytes);
}

ControlArtifactReadResult ControlService::read_artifact(Session& session,
                                                        std::string_view artifact_id,
                                                        std::uint64_t offset,
                                                        std::size_t maximum_bytes) {
    if (!session.negotiated_features_) {
        return {
            .status = ControlArtifactStatus::Unauthorized,
            .explanation = "control protocol negotiation is required",
        };
    }
    if (std::ranges::find(*session.negotiated_features_, "artifacts") ==
        session.negotiated_features_->end()) {
        return {
            .status = ControlArtifactStatus::Unauthorized,
            .explanation = "artifacts feature was not negotiated",
        };
    }
    return broker_.read_artifact(session.peer_, session.client_id_, artifact_id, offset,
                                 maximum_bytes);
}

ControlServiceResult ControlService::dispatch(Session& session, std::string_view encoded_envelope) {
    ControlProtocolDiagnostics diagnostics;
    const auto envelope = decode_control_envelope(encoded_envelope, &diagnostics);
    if (!envelope) {
        return {
            .status = ControlServiceStatus::InvalidEnvelope,
            .explanation = std::move(diagnostics.explanation),
        };
    }
    if (const auto* offer = std::get_if<ControlNegotiationOffer>(&envelope->payload)) {
        const auto client = broker_.client(session.client_id_);
        if (!client || client->peer_fingerprint != session.peer_.fingerprint()) {
            return {
                .status = ControlServiceStatus::AdmissionDenied,
                .admission_status = ControlAdmissionStatus::IdentityMismatch,
                .explanation = "negotiation client does not match the authenticated connection",
            };
        }
        const ControlNegotiationOffer local{
            .versions = {kControlProtocolVersion, kControlProtocolVersion},
            .mandatory_features = {"receipts"},
            .optional_features = {"artifacts", "cancellation", "progress"},
        };
        auto negotiated = negotiate_control_protocol(local, *offer);
        if (negotiated.status == ControlNegotiationStatus::Accepted)
            session.negotiated_features_ = negotiated.features;
        return {
            .status = ControlServiceStatus::Responded,
            .response =
                ControlEnvelope{
                    .schema_version = kControlProtocolVersion,
                    .payload = std::move(negotiated),
                },
        };
    }
    if (const auto* request = std::get_if<ControlRequestEnvelope>(&envelope->payload)) {
        return dispatch_request(session, *request);
    }
    if (const auto* cancellation = std::get_if<ControlCancelEnvelope>(&envelope->payload)) {
        if (!session.negotiated_features_) {
            return {
                .status = ControlServiceStatus::NegotiationRequired,
                .explanation = "control protocol negotiation is required",
            };
        }
        if (std::ranges::find(*session.negotiated_features_, "cancellation") ==
            session.negotiated_features_->end()) {
            return {
                .status = ControlServiceStatus::UnsupportedMessage,
                .explanation = "cancellation feature was not negotiated",
            };
        }
        const auto cancelled = broker_.cancel_operation(
            session.peer_, session.client_id_, cancellation->request_id, cancellation->reason);
        if (cancelled.status == ControlOperationStoreStatus::CancellationRequested &&
            cancelled.receipt) {
            return receipt_response(*cancelled.receipt);
        }
        return {
            .status = ControlServiceStatus::ReceiptTransitionFailed,
            .explanation = cancelled.error,
        };
    }
    return {
        .status = ControlServiceStatus::UnsupportedMessage,
        .explanation = "service accepts negotiation offers and requests only",
    };
}

ControlServiceResult ControlService::dispatch_request(Session& session,
                                                      const ControlRequestEnvelope& request) {
    if (request.client_id != session.client_id_.value) {
        return {
            .status = ControlServiceStatus::AdmissionDenied,
            .admission_status = ControlAdmissionStatus::IdentityMismatch,
            .explanation = "request client does not match the authenticated connection",
        };
    }
    if (!session.negotiated_features_) {
        return {
            .status = ControlServiceStatus::NegotiationRequired,
            .explanation = "control protocol negotiation is required",
        };
    }
    const auto* operation =
        resolve_control_operation(request.operation_id, request.operation_version);
    if (operation && operation->artifact_binding.produced &&
        std::ranges::find(*session.negotiated_features_, "artifacts") ==
            session.negotiated_features_->end()) {
        return {
            .status = ControlServiceStatus::UnsupportedMessage,
            .explanation = "artifacts feature was not negotiated",
        };
    }
    auto admission = executor_ ? broker_.admit_operation(session.peer_, request)
                               : broker_.replay_operation(session.peer_, request);
    if (admission.receipt && !admission.should_dispatch &&
        (admission.status == ControlAdmissionStatus::Admitted ||
         admission.status == ControlAdmissionStatus::CancelledBeforeDispatch)) {
        // Once admission has produced durable truth, project that receipt even
        // when post-persist revalidation cancelled it before dispatch. A bare
        // denial would discard the receipt ID needed for correlated replay.
        return receipt_response(*admission.receipt, request.request_id);
    }
    if (!executor_) {
        if (admission.status == ControlAdmissionStatus::IdempotencyConflict ||
            admission.status == ControlAdmissionStatus::RequestIdConflict ||
            admission.status == ControlAdmissionStatus::ReplayWindowExpired) {
            return {
                .status = ControlServiceStatus::AdmissionDenied,
                .admission_status = admission.status,
                .explanation = std::string(control_admission_status_id(admission.status)),
            };
        }
        return {
            .status = ControlServiceStatus::UnsupportedMessage,
            .explanation = "no control operation executor is installed",
        };
    }
    if (admission.status != ControlAdmissionStatus::Admitted || !admission.receipt) {
        return {
            .status = ControlServiceStatus::AdmissionDenied,
            .admission_status = admission.status,
            .explanation = std::string(control_admission_status_id(admission.status)),
        };
    }
    if (!admission.plan) {
        return {
            .status = ControlServiceStatus::ReceiptTransitionFailed,
            .explanation = "admitted operation has no execution plan",
        };
    }
    const auto running = broker_.begin_operation(session.peer_, *admission.plan);
    if (running.status != ControlOperationStoreStatus::Transitioned || !running.receipt) {
        return {
            .status = ControlServiceStatus::ReceiptTransitionFailed,
            .explanation = running.error,
        };
    }
    if (running.receipt->state != ControlReceiptState::Running) {
        if (control_receipt_state_is_terminal(running.receipt->state))
            return receipt_response(*running.receipt);
        return {
            .status = ControlServiceStatus::ReceiptTransitionFailed,
            .explanation = "operation did not enter the running state before dispatch",
        };
    }

    struct ProgressState {
        std::mutex mutex;
        std::condition_variable condition;
        std::optional<ControlProgressEnvelope> previous;
        std::optional<ControlOperationReceipt> settled_receipt;
        bool backpressured = false;
        bool progress_closed = false;
        bool settlement_started = false;
        bool settlement_finished = false;
    };
    auto progress_state = std::make_shared<ProgressState>();
    const bool progress_negotiated = std::ranges::find(*session.negotiated_features_, "progress") !=
                                     session.negotiated_features_->end();
    const auto progress_sink = progress_sink_;
    const auto maximum_progress_events = config_.maximum_progress_events_per_operation;
    const ProgressReporter report_progress =
        [progress_state, progress_negotiated, progress_sink, maximum_progress_events,
         request_id = request.request_id, receipt_id = admission.plan->receipt_id.value](
            std::uint64_t current, std::uint64_t total, std::string detail_json) {
            std::lock_guard lock(progress_state->mutex);
            if (!progress_negotiated || !progress_sink || progress_state->backpressured ||
                progress_state->progress_closed)
                return false;
            ControlProgressEnvelope progress{
                .request_id = request_id,
                .receipt_id = receipt_id,
                .sequence = progress_state->previous ? progress_state->previous->sequence + 1 : 1,
                .current = current,
                .total = total,
                .detail_json = std::move(detail_json),
            };
            if (progress.sequence > maximum_progress_events ||
                (progress_state->previous &&
                 !valid_control_progress_transition(*progress_state->previous, progress)) ||
                encode_control_envelope({
                                            .schema_version = kControlProtocolVersion,
                                            .payload = progress,
                                        })
                    .empty() ||
                !progress_sink(progress)) {
                progress_state->backpressured = true;
                return false;
            }
            progress_state->previous = std::move(progress);
            return true;
        };

    const auto plan = *admission.plan;
    const auto peer = session.peer_;
    const auto completion_owner = completion_owner_;
    const auto settle =
        [completion_owner, peer, plan, progress_state](
            ControlExecutionOutcome outcome) -> std::optional<ControlOperationReceipt> {
        std::unique_lock owner_lock(completion_owner->mutex);
        {
            std::unique_lock state_lock(progress_state->mutex);
            if (progress_state->settlement_started) {
                owner_lock.unlock();
                progress_state->condition.wait(state_lock,
                                               [&] { return progress_state->settlement_finished; });
                return progress_state->settled_receipt;
            }
            progress_state->settlement_started = true;
            progress_state->progress_closed = true;
        }

        std::optional<ControlOperationReceipt> settled_receipt;
        try {
            auto* broker = completion_owner->broker;
            if (!control_receipt_state_is_terminal(outcome.terminal_state)) {
                outcome.terminal_state = ControlReceiptState::Failed;
                outcome.result = internal_failure("executor returned a non-terminal receipt state");
            }
            if (outcome.terminal_state == ControlReceiptState::Completed ||
                outcome.terminal_state == ControlReceiptState::CompletedAfterRevocation) {
                const auto* operation =
                    resolve_control_operation(plan.operation_id, plan.operation_version);
                ControlJsonSchemaDiagnostics diagnostics;
                if (!operation ||
                    !validate_control_output_json_schema(
                        outcome.result.detail_json, operation->output_schema_json, &diagnostics)) {
                    outcome.terminal_state = ControlReceiptState::Failed;
                    outcome.result = internal_failure(
                        "executor result did not satisfy the operation output schema");
                }
            }
            if (!persistable_executor_metadata(outcome.result)) {
                outcome.terminal_state = ControlReceiptState::Failed;
                outcome.result = internal_failure("executor returned invalid terminal metadata");
            }
            if (outcome.terminal_state == ControlReceiptState::Failed &&
                !outcome.result.result_code) {
                outcome.result.result_code = ControlResultCode::InternalError;
            }
            if (broker) {
                auto finished = broker->finish_operation(peer, plan, outcome.terminal_state,
                                                         std::move(outcome.result));
                if (finished.status == ControlOperationStoreStatus::InvalidRequest) {
                    finished = broker->finish_operation(
                        peer, plan, ControlReceiptState::Failed,
                        internal_failure("executor returned an invalid terminal result"));
                }
                if (finished.status == ControlOperationStoreStatus::Transitioned)
                    settled_receipt = std::move(finished.receipt);
            }
        } catch (...) {
            // Deferred callbacks run after the response fence. Never let a
            // broker/schema failure escape or strand duplicate settlers.
            settled_receipt.reset();
        }
        completion_owner->deferred_operations.erase(plan.receipt_id.value);
        {
            std::lock_guard state_lock(progress_state->mutex);
            progress_state->settled_receipt = settled_receipt;
            progress_state->settlement_finished = true;
        }
        progress_state->condition.notify_all();
        return settled_receipt;
    };
    const ControlExecutionContext context{
        .report_progress = report_progress,
        .checkpoint =
            [completion_owner, peer, plan] {
                std::lock_guard lock(completion_owner->mutex);
                return completion_owner->broker
                           ? completion_owner->broker->execution_checkpoint(peer, plan)
                           : ControlExecutionCheckpoint::AuthorityRevoked;
            },
        .complete_deferred =
            [settle](ControlExecutionOutcome outcome) mutable { (void)settle(std::move(outcome)); },
    };

    ControlExecutionOutcome outcome;
    try {
        outcome = executor_(plan, request, context);
    } catch (const std::exception& error) {
        outcome.terminal_state = ControlReceiptState::Failed;
        outcome.result = internal_failure(error.what());
    } catch (...) {
        outcome.terminal_state = ControlReceiptState::Failed;
        outcome.result = internal_failure("executor threw an unknown exception");
    }
    if (outcome.deferred) {
        {
            std::lock_guard owner_lock(completion_owner->mutex);
            std::lock_guard state_lock(progress_state->mutex);
            progress_state->progress_closed = true;
            if (!progress_state->settlement_started && completion_owner->broker) {
                completion_owner->deferred_operations.emplace(
                    plan.receipt_id.value, [peer, plan, progress_state](ControlBroker& broker) {
                        {
                            std::lock_guard state_lock(progress_state->mutex);
                            progress_state->settlement_started = true;
                            progress_state->progress_closed = true;
                        }
                        std::optional<ControlOperationReceipt> settled_receipt;
                        try {
                            auto finished = broker.finish_operation(
                                peer, plan, ControlReceiptState::UnknownNeedsRefresh,
                                {
                                    .result_code = ControlResultCode::UnknownNeedsRefresh,
                                    .retry = ControlRetryClassification::AfterRefresh,
                                    .explanation = "control service shut down before deferred "
                                                   "operation completion was confirmed",
                                });
                            if (finished.status == ControlOperationStoreStatus::Transitioned)
                                settled_receipt = std::move(finished.receipt);
                        } catch (...) {
                            settled_receipt.reset();
                        }
                        {
                            std::lock_guard state_lock(progress_state->mutex);
                            progress_state->settled_receipt = std::move(settled_receipt);
                            progress_state->settlement_finished = true;
                        }
                        progress_state->condition.notify_all();
                    });
            }
        }
        auto pending = *running.receipt;
        pending.state = ControlReceiptState::UnknownNeedsRefresh;
        pending.result = {
            .result_code = ControlResultCode::UnknownNeedsRefresh,
            .retry = ControlRetryClassification::AfterRefresh,
            .explanation = "operation is still running after the response deadline",
        };
        return receipt_response(pending);
    }
    const auto finished = settle(std::move(outcome));
    if (!finished) {
        return {
            .status = ControlServiceStatus::ReceiptTransitionFailed,
            .explanation = "operation completion could not be persisted",
        };
    }
    return receipt_response(*finished);
}

} // namespace pulp::inspect
