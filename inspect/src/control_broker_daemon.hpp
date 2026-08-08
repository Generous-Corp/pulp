#pragma once

#include <pulp/inspect/control_grants.hpp>
#include <pulp/inspect/control_trusted_host_launcher.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace pulp::inspect {

struct ControlBrokerDaemonConfig {
    std::filesystem::path runtime_root;
    std::filesystem::path state_root;
    std::string sdk_version;
    std::filesystem::path executable_path;
    std::uint64_t process_generation = 0;
    std::function<ControlConsentDecision(const VerifiedControlPeerIdentity&,
                                         const ControlGrantRequest&)>
        decide_consent;
};

class ControlBrokerDaemon {
  public:
    explicit ControlBrokerDaemon(ControlBrokerDaemonConfig config = {});
    ~ControlBrokerDaemon();

    ControlBrokerDaemon(const ControlBrokerDaemon&) = delete;
    ControlBrokerDaemon& operator=(const ControlBrokerDaemon&) = delete;

    bool start();
    void stop() noexcept;
    bool is_running() const noexcept;
    const std::filesystem::path& endpoint_path() const noexcept;
    const std::filesystem::path& state_directory() const noexcept;
    ControlTrustedHostInventoryPrepareResult
    prepare_trusted_host(const ControlTrustedHostLaunchIntent& intent);
    ControlTrustedHostLaunchResult
    launch_trusted_host(std::string_view inventory_id, platform::ProcessOptions options = {});

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pulp::inspect
