#pragma once

#include <pulp/inspect/control_peer.hpp>
#include <pulp/inspect/control_protocol.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace pulp::inspect {

struct ControlBrokerHealth {
    std::string sdk_version;
    ControlProtocolRange protocol_versions;
    std::string broker_id;
    std::uint64_t process_generation = 0;
};

enum class ControlBrokerHealthProbeStatus : std::uint8_t {
    Unavailable,
    ReachableUnverified,
    HealthyVerified,
    Incompatible,
    Malformed,
};

struct ControlHealthProbeConfig {
    std::filesystem::path endpoint_path;
    std::optional<ControlPeerExpectation> expected_broker;
    std::chrono::milliseconds connect_timeout = std::chrono::seconds(3);
    std::chrono::milliseconds write_timeout = std::chrono::seconds(3);
    std::chrono::milliseconds frame_read_timeout = std::chrono::seconds(3);
};

struct ControlBrokerHealthProbeResult {
    ControlBrokerHealthProbeStatus status = ControlBrokerHealthProbeStatus::Unavailable;
    std::optional<ControlBrokerHealth> health;
    std::string error_code;
    std::string explanation;

    bool healthy() const {
        return status == ControlBrokerHealthProbeStatus::HealthyVerified;
    }
};

/// Performs an observational local carrier probe without opening a control
/// session. Without an expected broker identity the strongest possible result
/// is ReachableUnverified, and no health request is sent.
ControlBrokerHealthProbeResult
probe_control_broker(const ControlHealthProbeConfig& config,
                     std::chrono::milliseconds response_timeout = std::chrono::seconds(3));

} // namespace pulp::inspect
