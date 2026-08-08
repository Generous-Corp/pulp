#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace pulp::inspect {

struct ControlBrokerDaemonConfig {
    std::filesystem::path runtime_root;
    std::string sdk_version;
    std::uint64_t process_generation = 0;
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

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pulp::inspect
