#pragma once

#include <pulp/inspect/control_execution.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>

namespace pulp::inspect {

/// Dormant broker-to-host route selector. It owns connection liveness and
/// correlation only; broker identity, grants, admission, receipts, artifacts,
/// and settlement remain with ControlBroker and ControlService.
class ControlHostRouter {
  public:
    using ConnectionGeneration = std::uint64_t;
    using Sender = std::function<bool(const ControlEnvelope&)>;

    ControlHostRouter();
    ~ControlHostRouter();
    ControlHostRouter(const ControlHostRouter&) = delete;
    ControlHostRouter& operator=(const ControlHostRouter&) = delete;

    bool attach(const ControlRegistrationId& registration_id,
                ConnectionGeneration connection_generation, Sender sender);
    /// Attach one exact Pulp-host slot. The immutable instance identity and
    /// process/slot generation are checked again at dispatch, so unloading and
    /// recreating a slot cannot retarget an admitted operation.
    bool attach_slot(const ControlRegistrationId& registration_id,
                     ConnectionGeneration connection_generation, std::string instance_id,
                     std::string instance_generation, Sender sender);
    void detach(const ControlRegistrationId& registration_id,
                ConnectionGeneration connection_generation) noexcept;
    bool connected(const ControlRegistrationId& registration_id) const;

    /// Accepts only host-to-broker progress and completion frames for the exact
    /// attached registration generation. Other directions fail closed.
    bool receive(const ControlRegistrationId& registration_id,
                 ConnectionGeneration connection_generation, const ControlEnvelope& envelope);

    ControlOperationExecutor executor() const;
    void stop() noexcept;

  private:
    class Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace pulp::inspect
