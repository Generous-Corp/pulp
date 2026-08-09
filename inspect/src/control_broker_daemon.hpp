#pragma once

#include <pulp/inspect/control_grants.hpp>
#include <pulp/inspect/control_trusted_host_inventory.hpp>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace pulp::inspect {

struct ControlBrokerDaemonConfig {
    std::filesystem::path runtime_root;
    std::filesystem::path state_root;
    std::string sdk_version;
    std::filesystem::path executable_path;
    std::uint64_t process_generation = 0;
    /// Exact broker-owned launch intents accepted from enrolled clients.
    /// Empty disables remote host preparation. Executable, arguments, working
    /// directory, and tier must all match one entry.
    std::vector<ControlTrustedHostLaunchIntent> trusted_host_allowlist;
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
  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pulp::inspect
