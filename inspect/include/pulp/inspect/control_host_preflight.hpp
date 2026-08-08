#pragma once

#include <pulp/inspect/control_host_bootstrap.hpp>
#include <pulp/platform/child_process.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace pulp::inspect {

/// Private transport preflight for an already-spawned host. It authenticates
/// the writer of the inherited carrier but does not register or admit it.
enum class ControlHostPreflightStatus : std::uint8_t {
    Accepted,
    Unsupported,
    InvalidChannel,
    EntropyUnavailable,
    Timeout,
    MalformedMessage,
    DirectionMismatch,
    NonceMismatch,
    PeerUnavailable,
    ProcessMismatch,
    AuthorityRejected,
    SendFailed,
    BootstrapInvalid,
};

struct ControlHostPreflightDiagnostics {
    ControlHostPreflightStatus status = ControlHostPreflightStatus::InvalidChannel;
    std::string explanation;
};

/// Challenge the process on a private inherited channel, verify the kernel-
/// observed peer after its response, then release an already-minted bootstrap.
std::optional<VerifiedControlPeerIdentity>
preflight_control_host(platform::ChildProcessInputChannel channel, std::int64_t expected_process_id,
                       ControlPeerRole role, const ControlPeerVerifier& verifier,
                       ControlHostBootstrapBytes bootstrap,
                       std::chrono::milliseconds timeout = std::chrono::seconds(3),
                       ControlHostPreflightDiagnostics* diagnostics = nullptr);

/// Complete the child side of one inherited preflight and decode its bootstrap.
/// The supplied handle is consumed and closed on every path.
std::optional<ControlHostBootstrapRecord> receive_control_host_preflight(
    ControlHostBootstrapHandle handle, std::chrono::milliseconds timeout = std::chrono::seconds(3),
    std::optional<std::chrono::system_clock::time_point> now = std::nullopt,
    ControlHostPreflightDiagnostics* diagnostics = nullptr);

} // namespace pulp::inspect
