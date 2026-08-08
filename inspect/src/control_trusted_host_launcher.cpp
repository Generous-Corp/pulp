#include <pulp/inspect/control_trusted_host_launcher.hpp>

#include <algorithm>
#include <exception>
#include <utility>

namespace pulp::inspect {
namespace {

bool complete_peer(const ControlPeerEvidence& peer) {
    return peer.role == ControlPeerRole::TrustedHostBridge && !peer.user_id.empty() &&
           peer.process_id > 0 && !peer.process_start_id.empty() &&
           !peer.executable_identity.empty() && !peer.publisher_id.empty();
}

bool valid_config(const ControlTrustedHostLauncherConfig& config) {
    return config.endpoint_path.is_absolute() && config.broker_generation != 0 &&
           config.preflight_timeout > std::chrono::milliseconds::zero() &&
           complete_peer(config.expected_broker.evidence);
}

} // namespace

std::string_view control_trusted_host_launch_status_id(ControlTrustedHostLaunchStatus status) {
    switch (status) {
    case ControlTrustedHostLaunchStatus::Launched:
        return "launched";
    case ControlTrustedHostLaunchStatus::InvalidConfiguration:
        return "invalid_configuration";
    case ControlTrustedHostLaunchStatus::InventoryUnavailable:
        return "inventory_unavailable";
    case ControlTrustedHostLaunchStatus::SpawnFailed:
        return "spawn_failed";
    case ControlTrustedHostLaunchStatus::PreflightRejected:
        return "preflight_rejected";
    case ControlTrustedHostLaunchStatus::EnrollmentRejected:
        return "enrollment_rejected";
    case ControlTrustedHostLaunchStatus::BootstrapRejected:
        return "bootstrap_rejected";
    }
    return "invalid_configuration";
}

ControlTrustedHostLauncher::ControlTrustedHostLauncher(ControlTrustedHostInventory& inventory,
                                                       ControlHostEnrollmentStore& enrollments,
                                                       ControlTrustedHostLauncherConfig config)
    : inventory_(&inventory), enrollments_(&enrollments), config_(std::move(config)) {}

ControlTrustedHostLaunchResult
ControlTrustedHostLauncher::launch(std::string_view inventory_id,
                                   platform::ProcessOptions options) {
    ControlTrustedHostLaunchResult result;
    if (!inventory_ || !enrollments_ || !valid_config(config_) || inventory_id.empty()) {
        result.explanation = "the trusted host launcher configuration is incomplete";
        return result;
    }

    auto snapshot = inventory_->consume(inventory_id);
    if (!snapshot || snapshot->broker_generation() != config_.broker_generation ||
        snapshot->expires_at() <= std::chrono::steady_clock::now()) {
        result.status = ControlTrustedHostLaunchStatus::InventoryUnavailable;
        result.explanation = "the trusted host inventory claim is missing, stale, or replayed";
        return result;
    }

    const auto executable = snapshot->executable().string();
    const auto arguments = snapshot->arguments();
    options.working_directory = snapshot->working_directory().string();
    options.standard_input_timeout_ms = std::max(
        options.standard_input_timeout_ms,
        static_cast<int>(std::min<std::int64_t>(config_.preflight_timeout.count() + 1000, 60'000)));

    const auto static_expectation = snapshot->static_expectation();
    const auto expected_user = config_.expected_broker.evidence.user_id;
    const ControlPeerVerifier verifier(
        [static_expectation, expected_user](const ControlPeerEvidence& peer) {
            return peer.role == ControlPeerRole::StandaloneHost && peer.user_id == expected_user &&
                   peer.process_id > 0 && !peer.process_start_id.empty() &&
                   peer.executable_identity == static_expectation.executable_identity &&
                   peer.publisher_id == static_expectation.publisher_id;
        });

    auto process = std::make_unique<platform::ChildProcess>();
    auto provider_status = ControlTrustedHostLaunchStatus::BootstrapRejected;
    std::string provider_explanation;
    bool preflight_attempted = false;
    auto snapshot_for_session = std::move(*snapshot);
    const bool started = process->start_with_standard_input_channel(
        executable, arguments,
        [&](int child_process_id, platform::ChildProcessInputChannel channel) {
            preflight_attempted = true;
            auto verified = preflight_control_host(
                std::move(channel), child_process_id, ControlPeerRole::StandaloneHost, verifier,
                [&](const VerifiedControlPeerIdentity& child) {
                    const auto now = std::chrono::steady_clock::now();
                    const auto expires_at = std::min(snapshot_for_session.expires_at(),
                                                     now + config_.preflight_timeout);
                    auto enrollment = enrollments_->create(std::move(snapshot_for_session), child,
                                                           config_.broker_generation, expires_at);
                    if (!enrollment.ticket) {
                        provider_status = ControlTrustedHostLaunchStatus::EnrollmentRejected;
                        provider_explanation =
                            "the exact child could not obtain one-use enrollment";
                        return ControlHostBootstrapBytes{};
                    }

                    ControlHostBootstrapRecord bootstrap;
                    bootstrap.endpoint_path = config_.endpoint_path;
                    bootstrap.expected_broker = config_.expected_broker;
                    bootstrap.enrollment_id = std::move(enrollment.ticket->enrollment_id);
                    bootstrap.expires_at_unix_ms =
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            (std::chrono::system_clock::now() + (expires_at - now))
                                .time_since_epoch())
                            .count();
                    auto encoded = encode_control_host_bootstrap(bootstrap);
                    if (encoded.empty()) {
                        provider_explanation = "the enrollment bootstrap could not be encoded";
                        return ControlHostBootstrapBytes{};
                    }
                    return encoded;
                },
                config_.preflight_timeout, &result.preflight);
            return verified.has_value();
        },
        options);

    if (!started) {
        if (provider_status == ControlTrustedHostLaunchStatus::EnrollmentRejected ||
            !provider_explanation.empty()) {
            result.status = provider_status;
            result.explanation = std::move(provider_explanation);
        } else if (preflight_attempted &&
                   result.preflight.status != ControlHostPreflightStatus::Accepted) {
            result.status = ControlTrustedHostLaunchStatus::PreflightRejected;
            result.explanation = result.preflight.explanation;
        } else {
            result.status = ControlTrustedHostLaunchStatus::SpawnFailed;
            result.explanation = "the staged trusted host could not be spawned";
        }
        return result;
    }

    result.status = ControlTrustedHostLaunchStatus::Launched;
    result.process = std::move(process);
    result.explanation.clear();
    return result;
}

} // namespace pulp::inspect
