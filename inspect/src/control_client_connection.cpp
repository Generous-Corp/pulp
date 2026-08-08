#include <pulp/inspect/control_client_connection.hpp>

#include "control_static_code_identity.hpp"

#include <pulp/events/interprocess_connection.hpp>
#include <pulp/runtime/base64.hpp>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <utility>

namespace pulp::inspect {
namespace {

using events::InterprocessConnection;
using events::IpcTransport;

std::optional<ControlArtifactStatus> artifact_status_from_id(std::string_view id) {
    if (id == "stored")
        return ControlArtifactStatus::Stored;
    if (id == "read")
        return ControlArtifactStatus::Read;
    if (id == "invalid-request")
        return ControlArtifactStatus::InvalidRequest;
    if (id == "unauthorized")
        return ControlArtifactStatus::Unauthorized;
    if (id == "not-found")
        return ControlArtifactStatus::NotFound;
    if (id == "corrupt")
        return ControlArtifactStatus::Corrupt;
    if (id == "resource-exhausted")
        return ControlArtifactStatus::ResourceExhausted;
    if (id == "io-error")
        return ControlArtifactStatus::IoError;
    return std::nullopt;
}

std::optional<ControlArtifactSensitivity> sensitivity_from_id(std::string_view id) {
    if (id == "public")
        return ControlArtifactSensitivity::Public;
    if (id == "internal")
        return ControlArtifactSensitivity::Internal;
    if (id == "sensitive")
        return ControlArtifactSensitivity::Sensitive;
    if (id == "restricted")
        return ControlArtifactSensitivity::Restricted;
    return std::nullopt;
}

std::optional<ControlArtifactDeletionState> deletion_state_from_id(std::string_view id) {
    if (id == "active")
        return ControlArtifactDeletionState::Active;
    if (id == "deleted")
        return ControlArtifactDeletionState::Deleted;
    return std::nullopt;
}

std::optional<ControlArtifactRedactionState> redaction_state_from_id(std::string_view id) {
    if (id == "original")
        return ControlArtifactRedactionState::Original;
    if (id == "redacted")
        return ControlArtifactRedactionState::Redacted;
    return std::nullopt;
}

std::optional<ControlArtifactMetadata>
artifact_metadata_from_wire(const ControlArtifactWireMetadata& wire) {
    const auto sensitivity = sensitivity_from_id(wire.sensitivity_id);
    const auto deletion_state = deletion_state_from_id(wire.deletion_state_id);
    const auto redaction_state = redaction_state_from_id(wire.redaction_state_id);
    if (!sensitivity || !deletion_state || !redaction_state)
        return std::nullopt;
    return ControlArtifactMetadata{
        .artifact_id = wire.artifact_id,
        .lineage =
            {
                .broker_id = wire.broker_id,
                .receipt_id = wire.receipt_id,
                .producer_client_id = wire.producer_client_id,
                .producer_registration_id = wire.producer_registration_id,
                .session_id = wire.session_id,
                .instance_id = wire.instance_id,
                .publication_id = wire.publication_id,
                .producer_capability_id = wire.producer_capability_id,
                .producer_operation_id = wire.producer_operation_id,
                .producer_operation_version = wire.producer_operation_version,
                .original_grant_id = wire.original_grant_id,
                .consent_decision_id = wire.consent_decision_id,
                .manifest_digest = wire.manifest_digest,
                .producer_artifact_digest = wire.producer_artifact_digest,
            },
        .sha256 = wire.sha256,
        .byte_size = wire.byte_size,
        .content_type = wire.content_type,
        .created_at_unix_ms = wire.created_at_unix_ms,
        .expires_at_unix_ms = wire.expires_at_unix_ms,
        .sensitivity = *sensitivity,
        .deletion_state = *deletion_state,
        .redaction_state = *redaction_state,
    };
}

} // namespace

struct ControlClientConnection::Impl {
    enum class ResponseKind : std::uint8_t {
        None,
        Negotiation,
        Receipt,
        SessionOpen,
        Management,
        ArtifactRead,
        Health,
    };

    struct ExchangeResult {
        std::optional<ControlEnvelope> response;
        std::string error_code;
        std::string explanation;
    };

    std::filesystem::path endpoint_path;
    std::optional<ControlPeerExpectation> expected_broker;
    std::filesystem::path expected_broker_executable;
    std::chrono::milliseconds connect_timeout;
    InterprocessConnection connection;
    mutable std::mutex state_mutex;
    std::condition_variable response_ready;
    std::mutex request_mutex;
    std::optional<ControlEnvelope> pending_response;
    std::string pending_error_code;
    std::string pending_explanation;
    std::string last_error_code;
    std::string last_explanation;
    ProgressSink progress_sink;
    bool carrier_connected = false;
    bool broker_verified = false;
    bool session_open = false;
    bool awaiting_response = false;
    ResponseKind expected_response = ResponseKind::None;
    std::string expected_request_id;
    bool reached_during_last_connect = false;
    std::atomic<std::uint64_t> next_request{1};

    Impl(std::filesystem::path path, std::optional<ControlPeerExpectation> expectation,
         std::filesystem::path broker_executable, std::chrono::milliseconds connect_timeout_in,
         std::chrono::milliseconds write_timeout, std::chrono::milliseconds frame_read_timeout)
        : endpoint_path(std::move(path)), expected_broker(std::move(expectation)),
          expected_broker_executable(std::move(broker_executable)),
          connect_timeout(connect_timeout_in) {
        connection.set_max_message_bytes(kControlMaximumEnvelopeBytes);
        connection.set_write_timeout(write_timeout);
        connection.set_frame_read_timeout(frame_read_timeout);
        connection.set_on_message(
            [this](const void* data, std::size_t size) { receive(data, size); });
        connection.set_on_disconnected([this] { disconnected(); });
    }

    std::string request_id(std::string_view kind) {
        return "client-" + std::string(kind) + "-" +
               std::to_string(next_request.fetch_add(1, std::memory_order_relaxed));
    }

    void set_last_error(std::string code, std::string explanation) {
        std::lock_guard lock(state_mutex);
        last_error_code = std::move(code);
        last_explanation = std::move(explanation);
    }

    void fail_pending(std::string code, std::string explanation) {
        {
            std::lock_guard lock(state_mutex);
            pending_response.reset();
            pending_error_code = code;
            pending_explanation = explanation;
            last_error_code = std::move(code);
            last_explanation = std::move(explanation);
            awaiting_response = false;
            expected_response = ResponseKind::None;
            expected_request_id.clear();
        }
        response_ready.notify_all();
    }

    void poison_connection(std::string code, std::string explanation) {
        {
            std::lock_guard lock(state_mutex);
            pending_response.reset();
            pending_error_code = code;
            pending_explanation = explanation;
            last_error_code = std::move(code);
            last_explanation = std::move(explanation);
            carrier_connected = false;
            broker_verified = false;
            session_open = false;
            awaiting_response = false;
            expected_response = ResponseKind::None;
            expected_request_id.clear();
        }
        response_ready.notify_all();
        connection.disconnect();
    }

    void shutdown() noexcept {
        connection.set_on_message({});
        connection.set_on_disconnected({});
        connection.disconnect();
        disconnected();
    }

    void disconnected() {
        {
            std::lock_guard lock(state_mutex);
            carrier_connected = false;
            broker_verified = false;
            session_open = false;
            if (awaiting_response && pending_error_code.empty()) {
                pending_error_code = "connection-lost";
                pending_explanation = "the control carrier disconnected before responding";
                last_error_code = pending_error_code;
                last_explanation = pending_explanation;
            }
            awaiting_response = false;
            expected_response = ResponseKind::None;
            expected_request_id.clear();
        }
        response_ready.notify_all();
    }

    void receive(const void* data, std::size_t size) {
        ControlProtocolDiagnostics diagnostics;
        auto envelope = decode_control_envelope(
            std::string_view(static_cast<const char*>(data), size), &diagnostics);
        if (!envelope) {
            poison_connection("malformed-response", diagnostics.explanation);
            return;
        }

        if (const auto* progress = std::get_if<ControlProgressEnvelope>(&envelope->payload)) {
            ProgressSink sink;
            bool valid = false;
            {
                std::lock_guard lock(state_mutex);
                valid = carrier_connected && broker_verified && session_open && awaiting_response &&
                        expected_response == ResponseKind::Receipt &&
                        progress->request_id == expected_request_id;
                sink = progress_sink;
            }
            if (!valid) {
                poison_connection("unexpected-progress",
                                  "progress arrived without an active authenticated request");
                return;
            }
            if (sink) {
                try {
                    sink(*progress);
                } catch (...) {
                    poison_connection("progress-callback-failed",
                                      "the progress sink threw an exception");
                }
            }
            return;
        }

        bool unexpected = false;
        {
            std::lock_guard lock(state_mutex);
            bool correlated = false;
            if (const auto* error = std::get_if<ControlErrorEnvelope>(&envelope->payload)) {
                correlated =
                    expected_response == ResponseKind::Negotiation ||
                    (!expected_request_id.empty() && error->request_id == expected_request_id);
            } else {
                switch (expected_response) {
                case ResponseKind::Negotiation:
                    correlated =
                        std::holds_alternative<ControlNegotiationResult>(envelope->payload);
                    break;
                case ResponseKind::Receipt:
                    if (const auto* receipt =
                            std::get_if<ControlReceiptEnvelope>(&envelope->payload))
                        correlated = receipt->request_id == expected_request_id;
                    break;
                case ResponseKind::SessionOpen:
                    if (const auto* opened =
                            std::get_if<ControlSessionOpenResult>(&envelope->payload))
                        correlated = opened->request_id == expected_request_id;
                    break;
                case ResponseKind::Management:
                    if (const auto* result =
                            std::get_if<ControlManagementResult>(&envelope->payload))
                        correlated = result->request_id == expected_request_id;
                    break;
                case ResponseKind::ArtifactRead:
                    if (const auto* artifact =
                            std::get_if<ControlArtifactReadResponseEnvelope>(&envelope->payload))
                        correlated = artifact->request_id == expected_request_id;
                    break;
                case ResponseKind::Health:
                    if (const auto* health = std::get_if<ControlHealthResult>(&envelope->payload))
                        correlated = health->request_id == expected_request_id;
                    break;
                case ResponseKind::None:
                    break;
                }
            }
            if (!awaiting_response || pending_response || !correlated) {
                unexpected = true;
            } else {
                pending_response = std::move(envelope);
            }
        }
        if (unexpected) {
            poison_connection("unexpected-response",
                              "the carrier received a response without a matching request");
            return;
        }
        response_ready.notify_all();
    }

    bool connect() {
        std::lock_guard request_lock(request_mutex);
        connection.disconnect();
        {
            std::lock_guard lock(state_mutex);
            carrier_connected = false;
            broker_verified = false;
            session_open = false;
            awaiting_response = false;
            pending_response.reset();
            expected_response = ResponseKind::None;
            expected_request_id.clear();
            pending_error_code.clear();
            pending_explanation.clear();
            last_error_code.clear();
            last_explanation.clear();
            reached_during_last_connect = false;
        }
        if (endpoint_path.empty() || !endpoint_path.is_absolute() || connect_timeout.count() <= 0) {
            set_last_error("invalid-connection-config",
                           "the endpoint must be absolute and the connect timeout positive");
            return false;
        }
        if (!connection.connect(endpoint_path.string(), IpcTransport::LocalSocket,
                                connect_timeout)) {
            set_last_error("unavailable", "the control carrier is unavailable");
            return false;
        }
        {
            std::lock_guard lock(state_mutex);
            carrier_connected = true;
            reached_during_last_connect = true;
        }
        bool verified = false;
        if (expected_broker) {
            verified = verify_control_peer(connection, *expected_broker).has_value();
        } else if (!expected_broker_executable.empty()) {
            const auto expected = detail::inspect_static_code_identity(expected_broker_executable);
            const auto observed = observe_control_peer(connection, ControlPeerRole::Client);
            verified = expected && observed &&
                       observed->executable_identity == expected->executable_identity &&
                       observed->publisher_id == expected->publisher_id;
        }
        if (!verified) {
            connection.disconnect();
            set_last_error("broker-verification-failed",
                           "the live local peer did not match the expected broker");
            return false;
        }
        {
            std::lock_guard lock(state_mutex);
            broker_verified = true;
        }
        return true;
    }

    void disconnect() noexcept {
        connection.disconnect();
        disconnected();
    }

    ExchangeResult exchange(std::string_view encoded_envelope, std::chrono::milliseconds timeout,
                            bool require_session, ResponseKind response_kind,
                            std::string request_id) {
        std::unique_lock request_lock(request_mutex);
        {
            std::lock_guard lock(state_mutex);
            if (!carrier_connected || !broker_verified || !connection.is_connected()) {
                return {
                    .error_code = "not-connected",
                    .explanation = "an authenticated broker connection is required",
                };
            }
            if (require_session && !session_open) {
                return {
                    .error_code = "session-required",
                    .explanation = "an open control session is required",
                };
            }
            if (timeout.count() <= 0) {
                return {
                    .error_code = "invalid-timeout",
                    .explanation = "the response timeout must be positive",
                };
            }
            pending_response.reset();
            pending_error_code.clear();
            pending_explanation.clear();
            awaiting_response = true;
            expected_response = response_kind;
            expected_request_id = std::move(request_id);
        }

        if (!connection.send_message(encoded_envelope)) {
            fail_pending("send-failed", "the control request could not be sent");
            connection.disconnect();
        }

        std::unique_lock state_lock(state_mutex);
        const bool completed = response_ready.wait_for(state_lock, timeout, [&] {
            return pending_response.has_value() || !awaiting_response;
        });
        if (!completed) {
            awaiting_response = false;
            pending_error_code = "timeout";
            pending_explanation = "the control request timed out";
            last_error_code = pending_error_code;
            last_explanation = pending_explanation;
            expected_response = ResponseKind::None;
            expected_request_id.clear();
            state_lock.unlock();
            connection.disconnect();
            return {
                .error_code = "timeout",
                .explanation = "the control request timed out",
            };
        }

        awaiting_response = false;
        expected_response = ResponseKind::None;
        expected_request_id.clear();
        if (pending_response) {
            auto response = std::move(pending_response);
            pending_response.reset();
            return {.response = std::move(response)};
        }
        return {
            .error_code = pending_error_code.empty() ? "connection-lost" : pending_error_code,
            .explanation = pending_explanation.empty()
                               ? "the control carrier disconnected before responding"
                               : pending_explanation,
        };
    }

    ExchangeResult exchange(const ControlEnvelope& envelope, std::chrono::milliseconds timeout,
                            bool require_session) {
        ResponseKind response_kind = ResponseKind::None;
        std::string request_id;
        if (std::holds_alternative<ControlNegotiationOffer>(envelope.payload)) {
            response_kind = ResponseKind::Negotiation;
        } else if (const auto* request = std::get_if<ControlRequestEnvelope>(&envelope.payload)) {
            response_kind = ResponseKind::Receipt;
            request_id = request->request_id;
        } else if (const auto* cancellation =
                       std::get_if<ControlCancelEnvelope>(&envelope.payload)) {
            response_kind = ResponseKind::Receipt;
            request_id = cancellation->request_id;
        } else if (const auto* open = std::get_if<ControlSessionOpenEnvelope>(&envelope.payload)) {
            response_kind = ResponseKind::SessionOpen;
            request_id = open->request_id;
        } else if (const auto* management =
                       std::get_if<ControlManagementEnvelope>(&envelope.payload)) {
            response_kind = ResponseKind::Management;
            request_id = management->request_id;
        } else if (const auto* artifact =
                       std::get_if<ControlArtifactReadEnvelope>(&envelope.payload)) {
            response_kind = ResponseKind::ArtifactRead;
            request_id = artifact->request_id;
        } else if (const auto* health = std::get_if<ControlHealthEnvelope>(&envelope.payload)) {
            response_kind = ResponseKind::Health;
            request_id = health->request_id;
        }
        const auto encoded = encode_control_envelope(envelope);
        if (encoded.empty() || response_kind == ResponseKind::None) {
            return {
                .error_code = "invalid-control-envelope",
                .explanation = "the control envelope is invalid",
            };
        }
        return exchange(encoded, timeout, require_session, response_kind, std::move(request_id));
    }

    void mark_session_open() {
        std::lock_guard lock(state_mutex);
        session_open = true;
    }
};

ControlClientConnection::ControlClientConnection(ControlClientConnectionConfig config)
    : impl_(std::make_unique<Impl>(
          std::move(config.endpoint_path),
          config.expected_broker.evidence.user_id.empty()
              ? std::optional<ControlPeerExpectation>{}
              : std::optional<ControlPeerExpectation>{std::move(config.expected_broker)},
          std::move(config.expected_broker_executable), config.connect_timeout,
          config.write_timeout, config.frame_read_timeout)) {}

ControlClientConnection::ControlClientConnection(const ControlHealthProbeConfig& config)
    : impl_(std::make_unique<Impl>(config.endpoint_path, config.expected_broker,
                                   std::filesystem::path{}, config.connect_timeout,
                                   config.write_timeout, config.frame_read_timeout)) {}

ControlClientConnection::~ControlClientConnection() {
    impl_->shutdown();
}

bool ControlClientConnection::connect() {
    return impl_->connect();
}

ControlSessionOpenResult ControlClientConnection::open_session(std::string_view admission_id,
                                                               std::chrono::milliseconds timeout) {
    const auto request_id = impl_->request_id("open");
    auto local_error = [&](std::string code, std::string explanation) {
        return ControlSessionOpenResult{
            .request_id = request_id,
            .accepted = false,
            .error_code = std::move(code),
            .explanation = std::move(explanation),
        };
    };
    {
        std::lock_guard lock(impl_->state_mutex);
        if (impl_->session_open)
            return local_error("session-already-open", "the control session is already open");
    }
    auto exchanged =
        impl_->exchange(ControlEnvelope{.payload =
                                            ControlSessionOpenEnvelope{
                                                .request_id = request_id,
                                                .admission_id = std::string(admission_id),
                                            }},
                        timeout, false);
    if (!exchanged.response)
        return local_error(std::move(exchanged.error_code), std::move(exchanged.explanation));
    if (const auto* error = std::get_if<ControlErrorEnvelope>(&exchanged.response->payload)) {
        if (error->request_id != request_id) {
            disconnect();
            return local_error("unexpected-response",
                               "session open returned an unrelated error response");
        }
        return local_error(error->error_code, error->explanation);
    }
    const auto* opened = std::get_if<ControlSessionOpenResult>(&exchanged.response->payload);
    if (!opened || opened->request_id != request_id) {
        disconnect();
        return local_error("unexpected-response", "session open returned an unrelated response");
    }
    if (opened->accepted)
        impl_->mark_session_open();
    return *opened;
}

ControlManagementResult ControlClientConnection::manage(std::string_view command,
                                                        std::string_view params_json,
                                                        std::chrono::milliseconds timeout) {
    const auto request_id = impl_->request_id("management");
    auto local_error = [&](std::string code, std::string explanation) {
        return ControlManagementResult{.request_id = request_id,
                                       .status_id = std::move(code),
                                       .explanation = std::move(explanation)};
    };
    const bool enrollment = command == "enroll";
    auto exchanged = impl_->exchange(
        ControlEnvelope{.payload =
                            ControlManagementEnvelope{.request_id = request_id,
                                                      .command = std::string(command),
                                                      .params_json = std::string(params_json)}},
        timeout, !enrollment);
    if (!exchanged.response)
        return local_error(std::move(exchanged.error_code), std::move(exchanged.explanation));
    if (const auto* error = std::get_if<ControlErrorEnvelope>(&exchanged.response->payload))
        return local_error(error->error_code, error->explanation);
    const auto* result = std::get_if<ControlManagementResult>(&exchanged.response->payload);
    if (!result || result->request_id != request_id) {
        disconnect();
        return local_error("unexpected-response", "management returned an unrelated response");
    }
    if (enrollment && result->status_id == "accepted")
        impl_->mark_session_open();
    return *result;
}

void ControlClientConnection::disconnect() noexcept {
    impl_->disconnect();
}

bool ControlClientConnection::is_connected() const {
    std::lock_guard lock(impl_->state_mutex);
    return impl_->carrier_connected && impl_->broker_verified && impl_->connection.is_connected();
}

bool ControlClientConnection::is_session_open() const {
    std::lock_guard lock(impl_->state_mutex);
    return impl_->carrier_connected && impl_->broker_verified && impl_->session_open &&
           impl_->connection.is_connected();
}

std::string ControlClientConnection::last_error_code() const {
    std::lock_guard lock(impl_->state_mutex);
    return impl_->last_error_code;
}

std::string ControlClientConnection::last_error_explanation() const {
    std::lock_guard lock(impl_->state_mutex);
    return impl_->last_explanation;
}

void ControlClientConnection::set_progress_sink(ProgressSink sink) {
    std::lock_guard lock(impl_->state_mutex);
    impl_->progress_sink = std::move(sink);
}

ControlTransportDispatchResult
ControlClientConnection::dispatch_probe(const ControlEnvelope& envelope,
                                        std::chrono::milliseconds timeout) {
    auto exchanged = impl_->exchange(envelope, timeout, false);
    if (!exchanged.response) {
        return {
            .error_code = std::move(exchanged.error_code),
            .explanation = std::move(exchanged.explanation),
        };
    }
    if (const auto* error = std::get_if<ControlErrorEnvelope>(&exchanged.response->payload)) {
        const auto* health = std::get_if<ControlHealthEnvelope>(&envelope.payload);
        if (health && error->request_id != health->request_id) {
            disconnect();
            return {
                .error_code = "unexpected-response",
                .explanation = "the broker returned an unrelated probe error",
            };
        }
        return {
            .error_code = error->error_code,
            .explanation = error->explanation,
        };
    }
    const auto encoded = encode_control_envelope(*exchanged.response);
    if (encoded.empty()) {
        disconnect();
        return {
            .error_code = "malformed-response",
            .explanation = "the broker response could not be encoded canonically",
        };
    }
    return {.encoded_response = encoded};
}

ControlTransportDispatchResult
ControlClientConnection::dispatch(std::string_view encoded_envelope,
                                  std::chrono::milliseconds timeout) {
    ControlProtocolDiagnostics diagnostics;
    const auto request = decode_control_envelope(encoded_envelope, &diagnostics);
    if (!request || !(std::holds_alternative<ControlNegotiationOffer>(request->payload) ||
                      std::holds_alternative<ControlRequestEnvelope>(request->payload) ||
                      std::holds_alternative<ControlCancelEnvelope>(request->payload))) {
        return {
            .error_code = "invalid-control-request",
            .explanation = request ? "the transport does not dispatch this envelope type"
                                   : diagnostics.explanation,
        };
    }
    auto exchanged = impl_->exchange(*request, timeout, true);
    if (!exchanged.response) {
        return {
            .error_code = std::move(exchanged.error_code),
            .explanation = std::move(exchanged.explanation),
        };
    }
    if (const auto* error = std::get_if<ControlErrorEnvelope>(&exchanged.response->payload)) {
        return {
            .error_code = error->error_code,
            .explanation = error->explanation,
        };
    }
    const auto encoded = encode_control_envelope(*exchanged.response);
    if (encoded.empty()) {
        disconnect();
        return {
            .error_code = "malformed-response",
            .explanation = "the broker response could not be encoded canonically",
        };
    }
    return {.encoded_response = encoded};
}

ControlArtifactReadResult
ControlClientConnection::read_artifact(std::string_view artifact_id, std::uint64_t offset,
                                       std::size_t maximum_bytes,
                                       std::chrono::milliseconds timeout) {
    const auto request_id = impl_->request_id("artifact");
    auto exchanged =
        impl_->exchange(ControlEnvelope{.payload =
                                            ControlArtifactReadEnvelope{
                                                .request_id = request_id,
                                                .artifact_id = std::string(artifact_id),
                                                .offset = offset,
                                                .maximum_bytes = maximum_bytes,
                                            }},
                        timeout, true);
    if (!exchanged.response) {
        return {
            .status = exchanged.error_code == "invalid-control-envelope"
                          ? ControlArtifactStatus::InvalidRequest
                          : ControlArtifactStatus::IoError,
            .explanation = exchanged.error_code + ": " + exchanged.explanation,
        };
    }
    if (const auto* error = std::get_if<ControlErrorEnvelope>(&exchanged.response->payload)) {
        if (error->request_id != request_id) {
            disconnect();
            return {
                .status = ControlArtifactStatus::Corrupt,
                .explanation = "the broker returned an unrelated artifact error",
            };
        }
        return {
            .status = ControlArtifactStatus::IoError,
            .explanation = error->error_code + ": " + error->explanation,
        };
    }
    const auto* response =
        std::get_if<ControlArtifactReadResponseEnvelope>(&exchanged.response->payload);
    const auto status = response ? artifact_status_from_id(response->status_id) : std::nullopt;
    const auto bytes = response ? runtime::base64_decode(response->bytes_base64) : std::nullopt;
    std::optional<ControlArtifactMetadata> metadata;
    if (response && response->metadata)
        metadata = artifact_metadata_from_wire(*response->metadata);
    const bool read_without_metadata =
        status && *status == ControlArtifactStatus::Read && !metadata;
    const bool wrong_artifact = metadata && metadata->artifact_id != artifact_id;
    const bool chunk_out_of_range =
        metadata && bytes &&
        (offset > metadata->byte_size || bytes->size() > metadata->byte_size - offset);
    if (!response || response->request_id != request_id || !status || !bytes ||
        (response->metadata && !metadata) || read_without_metadata || wrong_artifact ||
        chunk_out_of_range || bytes->size() > maximum_bytes) {
        disconnect();
        return {
            .status = ControlArtifactStatus::Corrupt,
            .explanation = "the broker returned an invalid artifact-read response",
        };
    }
    return {
        .status = *status,
        .metadata = std::move(metadata),
        .bytes = std::move(*bytes),
        .eof = response->eof,
        .explanation = response->explanation,
    };
}

bool ControlClientConnection::carrier_was_reached() const {
    std::lock_guard lock(impl_->state_mutex);
    return impl_->reached_during_last_connect;
}

bool ControlClientConnection::broker_was_verified() const {
    std::lock_guard lock(impl_->state_mutex);
    return impl_->broker_verified;
}

} // namespace pulp::inspect
