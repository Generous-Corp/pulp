#pragma once

#include <pulp/inspect/control_host_ui_executor.hpp>

#include <memory>
#include <string>

namespace pulp::view {
class View;
class WindowHost;
} // namespace pulp::view

namespace pulp::inspect {

struct ControlStandaloneUiAdapterConfig {
    view::View& root;
    view::WindowHost& window;
    std::string instance_id;
    std::string instance_generation;
    std::string view_generation;
    float capture_scale = 1.0f;
};

/// Ordinary Standalone implementation of the Pulp-owned UI adapter seam.
/// The adapter borrows root/window; disconnect the executor while its fenced
/// main-thread RPC and the borrowed view/window are still alive.
class ControlStandaloneUiAdapter final : public InspectorCaptureSource,
                                         public ControlHostUiTargetAdapter {
  public:
    static std::shared_ptr<ControlStandaloneUiAdapter>
    create(ControlStandaloneUiAdapterConfig config);
    ~ControlStandaloneUiAdapter() override;

    InspectorCapture capture_png() override;
    InspectorCapture capture_node_png(const ControlUiExactTarget& target) override;
    ControlUiApplyStatus dispatch_input(const ControlUiExactTarget& target,
                                        const ControlUiAuthorityOwner& owner,
                                        const ControlUiInput& input) override;
    void release_controller(
        const std::optional<ControlUiAuthorityOwner>& owner = std::nullopt) noexcept override;

  private:
    class Impl;
    explicit ControlStandaloneUiAdapter(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace pulp::inspect
