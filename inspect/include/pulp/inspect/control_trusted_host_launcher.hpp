#pragma once

#include <pulp/inspect/control_host_enrollment.hpp>
#include <pulp/inspect/control_host_preflight.hpp>
#include <pulp/inspect/control_trusted_host_inventory.hpp>
#include <pulp/platform/child_process.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace pulp::inspect {

enum class ControlTrustedHostLaunchStatus : std::uint8_t {
    Launched,
    InvalidConfiguration,
    InventoryUnavailable,
    SpawnFailed,
    PreflightRejected,
    EnrollmentRejected,
    BootstrapRejected,
};

std::string_view control_trusted_host_launch_status_id(ControlTrustedHostLaunchStatus status);

struct ControlTrustedHostLauncherConfig {
    std::filesystem::path endpoint_path;
    ControlPeerExpectation expected_broker;
    std::uint64_t broker_generation = 0;
    std::chrono::milliseconds preflight_timeout = std::chrono::seconds(3);
};

struct ControlTrustedHostLaunchResult {
    ControlTrustedHostLaunchStatus status = ControlTrustedHostLaunchStatus::InvalidConfiguration;
    std::unique_ptr<platform::ChildProcess> process;
    ControlHostPreflightDiagnostics preflight;
    std::string explanation;

    bool launched() const noexcept {
        return status == ControlTrustedHostLaunchStatus::Launched && process != nullptr;
    }
};

/// Broker composition seam for one inventory-approved raw host executable.
///
/// launch() consumes a broker-owned snapshot, spawns that staged executable on
/// a private inherited channel, verifies the live child through kernel peer
/// evidence, creates one endpoint enrollment for that exact process, and only
/// then releases the enrollment bootstrap. It does not start the endpoint,
/// grant a client, or install an executor.
class ControlTrustedHostLauncher {
  public:
    ControlTrustedHostLauncher(ControlTrustedHostInventory& inventory,
                               ControlHostEnrollmentStore& enrollments,
                               ControlTrustedHostLauncherConfig config);

    ControlTrustedHostLauncher(const ControlTrustedHostLauncher&) = delete;
    ControlTrustedHostLauncher& operator=(const ControlTrustedHostLauncher&) = delete;

    ControlTrustedHostLaunchResult launch(std::string_view inventory_id,
                                          platform::ProcessOptions options = {});

  private:
    ControlTrustedHostInventory* inventory_ = nullptr;
    ControlHostEnrollmentStore* enrollments_ = nullptr;
    ControlTrustedHostLauncherConfig config_;
};

} // namespace pulp::inspect
