#include <pulp/inspect/control_host_preflight.hpp>

#include <pulp/events/interprocess_connection.hpp>
#include <pulp/inspect/control_protocol.hpp>
#include <pulp/runtime/base64.hpp>
#include <pulp/runtime/crypto.hpp>

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace pulp::inspect {
namespace {

struct Inbox {
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<ControlEnvelope> messages;
    bool failed = false;
    bool malformed = false;
};

enum class ReceiveStatus : std::uint8_t { Message, Timeout, Malformed, Disconnected };

struct ReceiveResult {
    ReceiveStatus status = ReceiveStatus::Timeout;
    std::optional<ControlEnvelope> message;
};

void set_diagnostics(ControlHostPreflightDiagnostics* diagnostics,
                     ControlHostPreflightStatus status, std::string explanation) {
    if (diagnostics)
        *diagnostics = {.status = status, .explanation = std::move(explanation)};
}

void close_handle(ControlHostBootstrapHandle handle) {
#ifdef _WIN32
    const auto native = static_cast<HANDLE>(handle);
    if (native != nullptr && native != INVALID_HANDLE_VALUE)
        CloseHandle(native);
#else
    if (handle >= 0)
        ::close(handle);
#endif
}

std::shared_ptr<Inbox> install_inbox(events::InterprocessConnection& connection) {
    auto inbox = std::make_shared<Inbox>();
    connection.set_on_message([inbox](const void* data, std::size_t size) {
        const auto text = std::string_view(static_cast<const char*>(data), size);
        auto decoded = decode_control_envelope(text);
        std::lock_guard lock(inbox->mutex);
        if (!decoded || inbox->messages.size() >= 2) {
            inbox->failed = true;
            inbox->malformed = true;
        } else {
            inbox->messages.push_back(std::move(*decoded));
        }
        inbox->ready.notify_all();
    });
    connection.set_on_disconnected([inbox] {
        std::lock_guard lock(inbox->mutex);
        inbox->failed = true;
        inbox->ready.notify_all();
    });
    return inbox;
}

ReceiveResult receive_until(const std::shared_ptr<Inbox>& inbox,
                            std::chrono::steady_clock::time_point deadline) {
    std::unique_lock lock(inbox->mutex);
    inbox->ready.wait_until(lock, deadline,
                            [&] { return inbox->failed || !inbox->messages.empty(); });
    if (inbox->malformed)
        return {.status = ReceiveStatus::Malformed};
    if (!inbox->messages.empty()) {
        auto result = std::move(inbox->messages.front());
        inbox->messages.pop_front();
        return {.status = ReceiveStatus::Message, .message = std::move(result)};
    }
    if (inbox->failed)
        return {.status = ReceiveStatus::Disconnected};
    return {.status = ReceiveStatus::Timeout};
}

bool send(events::InterprocessConnection& connection, ControlEnvelope envelope) {
    const auto encoded = encode_control_envelope(envelope);
    return !encoded.empty() && connection.send_message(encoded);
}

bool send_sensitive(events::InterprocessConnection& connection, std::string nonce,
                    std::string bootstrap_base64) {
    ControlEnvelope envelope{
        .payload = ControlHostPreflightBootstrapEnvelope{
            .nonce = std::move(nonce), .bootstrap_base64 = std::move(bootstrap_base64)}};
    auto encoded = encode_control_envelope(envelope);
    auto& payload = std::get<ControlHostPreflightBootstrapEnvelope>(envelope.payload);
    runtime::secure_zero_memory(payload.bootstrap_base64.data(), payload.bootstrap_base64.size());
    const bool sent = !encoded.empty() && connection.send_message(encoded);
    runtime::secure_zero_memory(encoded.data(), encoded.size());
    return sent;
}

ControlHostPreflightStatus receive_failure_status(ReceiveStatus status) {
    return status == ReceiveStatus::Malformed ? ControlHostPreflightStatus::MalformedMessage
                                              : ControlHostPreflightStatus::Timeout;
}

std::intptr_t native_handle_value(ControlHostBootstrapHandle handle) {
#ifdef _WIN32
    return reinterpret_cast<std::intptr_t>(handle);
#else
    return static_cast<std::intptr_t>(handle);
#endif
}

std::optional<std::string> make_nonce() {
    const auto random = runtime::secure_random_bytes(32);
    if (!random)
        return std::nullopt;
    return runtime::sha256_hex(
        std::string_view(reinterpret_cast<const char*>(random->data()), random->size()));
}

bool attach(events::InterprocessConnection& connection, std::intptr_t handle,
            std::chrono::milliseconds timeout) {
    if (timeout <= std::chrono::milliseconds::zero())
        return false;
    connection.set_max_message_bytes(32u * 1024u);
    connection.set_write_timeout(timeout);
    connection.set_frame_read_timeout(timeout);
    connection.set_secure_receive_buffer(true);
    return connection.attach_inherited_local_socket(handle);
}

} // namespace

std::optional<VerifiedControlPeerIdentity>
preflight_control_host(platform::ChildProcessInputChannel channel, std::int64_t expected_process_id,
                       ControlPeerRole role, const ControlPeerVerifier& verifier,
                       ControlHostBootstrapBytes bootstrap, std::chrono::milliseconds timeout,
                       ControlHostPreflightDiagnostics* diagnostics) {
    if (!channel || expected_process_id <= 0 || bootstrap.empty() ||
        timeout <= std::chrono::milliseconds::zero()) {
        set_diagnostics(diagnostics, ControlHostPreflightStatus::InvalidChannel,
                        "the inherited preflight channel or input is invalid");
        return std::nullopt;
    }
    const auto nonce = make_nonce();
    if (!nonce) {
        set_diagnostics(diagnostics, ControlHostPreflightStatus::EntropyUnavailable,
                        "the preflight nonce could not be generated");
        return std::nullopt;
    }

    events::InterprocessConnection connection;
    const auto inbox = install_inbox(connection);
    if (!attach(connection, channel.native_handle(), timeout)) {
        set_diagnostics(diagnostics, ControlHostPreflightStatus::Unsupported,
                        "the inherited local-socket carrier is unsupported");
        return std::nullopt;
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    if (!send(connection,
              ControlEnvelope{.payload = ControlHostPreflightChallengeEnvelope{*nonce}})) {
        set_diagnostics(diagnostics, ControlHostPreflightStatus::SendFailed,
                        "the preflight challenge could not be sent");
        return std::nullopt;
    }
    auto response = receive_until(inbox, deadline);
    if (!response.message) {
        set_diagnostics(diagnostics, receive_failure_status(response.status),
                        "the host did not return a valid preflight response");
        return std::nullopt;
    }
    if (!control_envelope_allowed(*response.message, ControlEnvelopeDirection::HostToLauncher)) {
        set_diagnostics(diagnostics, ControlHostPreflightStatus::DirectionMismatch,
                        "the host sent a role-incompatible preflight frame");
        return std::nullopt;
    }
    const auto* echoed =
        std::get_if<ControlHostPreflightResponseEnvelope>(&response.message->payload);
    if (!echoed || echoed->nonce != *nonce) {
        set_diagnostics(diagnostics, ControlHostPreflightStatus::NonceMismatch,
                        "the host did not echo the active preflight nonce");
        return std::nullopt;
    }

    auto observed = observe_control_peer(connection, role);
    if (!observed) {
        set_diagnostics(diagnostics, ControlHostPreflightStatus::PeerUnavailable,
                        "the host peer identity could not be observed");
        return std::nullopt;
    }
    if (observed->process_id != expected_process_id) {
        set_diagnostics(diagnostics, ControlHostPreflightStatus::ProcessMismatch,
                        "the responding process is not the spawned child");
        return std::nullopt;
    }
    auto verified = verifier.verify(std::move(*observed));
    if (!verified) {
        set_diagnostics(diagnostics, ControlHostPreflightStatus::AuthorityRejected,
                        "the observed host is outside trusted launcher policy");
        return std::nullopt;
    }

    auto encoded_bootstrap =
        runtime::base64_encode(bootstrap.bytes().data(), bootstrap.bytes().size());
    const bool delivered = send_sensitive(connection, *nonce, std::move(encoded_bootstrap));
    if (!delivered) {
        set_diagnostics(diagnostics, ControlHostPreflightStatus::SendFailed,
                        "the verified bootstrap frame could not be sent");
        return std::nullopt;
    }
    set_diagnostics(diagnostics, ControlHostPreflightStatus::Accepted, {});
    return verified;
}

std::optional<ControlHostBootstrapRecord>
receive_control_host_preflight(ControlHostBootstrapHandle handle, std::chrono::milliseconds timeout,
                               std::optional<std::chrono::system_clock::time_point> now,
                               ControlHostPreflightDiagnostics* diagnostics) {
    events::InterprocessConnection connection;
    const auto inbox = install_inbox(connection);
    const auto attached = attach(connection, native_handle_value(handle), timeout);
    close_handle(handle);
    if (!attached) {
        set_diagnostics(diagnostics, ControlHostPreflightStatus::Unsupported,
                        "the inherited local-socket carrier is unsupported");
        return std::nullopt;
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    auto challenge = receive_until(inbox, deadline);
    if (!challenge.message) {
        set_diagnostics(diagnostics, receive_failure_status(challenge.status),
                        "the launcher did not send a valid preflight challenge");
        return std::nullopt;
    }
    if (!control_envelope_allowed(*challenge.message, ControlEnvelopeDirection::LauncherToHost)) {
        set_diagnostics(diagnostics, ControlHostPreflightStatus::DirectionMismatch,
                        "the launcher sent a role-incompatible preflight frame");
        return std::nullopt;
    }
    const auto* request =
        std::get_if<ControlHostPreflightChallengeEnvelope>(&challenge.message->payload);
    if (!request ||
        !send(connection,
              ControlEnvelope{.payload = ControlHostPreflightResponseEnvelope{request->nonce}})) {
        set_diagnostics(diagnostics, ControlHostPreflightStatus::SendFailed,
                        "the preflight response could not be sent");
        return std::nullopt;
    }

    auto delivery = receive_until(inbox, deadline);
    if (!delivery.message) {
        set_diagnostics(diagnostics, receive_failure_status(delivery.status),
                        "the verified bootstrap frame was not received");
        return std::nullopt;
    }
    if (!control_envelope_allowed(*delivery.message, ControlEnvelopeDirection::LauncherToHost)) {
        set_diagnostics(diagnostics, ControlHostPreflightStatus::DirectionMismatch,
                        "the launcher sent a role-incompatible bootstrap frame");
        return std::nullopt;
    }
    auto* bootstrap_message =
        std::get_if<ControlHostPreflightBootstrapEnvelope>(&delivery.message->payload);
    if (!bootstrap_message) {
        set_diagnostics(diagnostics, ControlHostPreflightStatus::NonceMismatch,
                        "the bootstrap frame does not belong to this preflight");
        return std::nullopt;
    }
    const bool nonce_matches = bootstrap_message->nonce == request->nonce;
    if (!nonce_matches) {
        runtime::secure_zero_memory(bootstrap_message->bootstrap_base64.data(),
                                    bootstrap_message->bootstrap_base64.size());
        set_diagnostics(diagnostics, ControlHostPreflightStatus::NonceMismatch,
                        "the bootstrap frame does not belong to this preflight");
        return std::nullopt;
    }
    auto bytes = runtime::base64_decode(bootstrap_message->bootstrap_base64);
    runtime::secure_zero_memory(bootstrap_message->bootstrap_base64.data(),
                                bootstrap_message->bootstrap_base64.size());
    if (!bytes || bytes->size() > kControlHostBootstrapMaximumBytes) {
        set_diagnostics(diagnostics, ControlHostPreflightStatus::BootstrapInvalid,
                        "the bootstrap frame is invalid or oversized");
        return std::nullopt;
    }
    ControlHostBootstrapDiagnostics bootstrap_diagnostics;
    auto record = decode_control_host_bootstrap(
        *bytes, now.value_or(std::chrono::system_clock::now()), &bootstrap_diagnostics);
    runtime::secure_zero_memory(bytes->data(), bytes->size());
    if (!record) {
        set_diagnostics(diagnostics, ControlHostPreflightStatus::BootstrapInvalid,
                        bootstrap_diagnostics.explanation);
        return std::nullopt;
    }
    set_diagnostics(diagnostics, ControlHostPreflightStatus::Accepted, {});
    return record;
}

} // namespace pulp::inspect
