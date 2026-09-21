#pragma once

#include <pulp/inspect/control_execution.hpp>
#include <cstdint>
#include <functional>
#include <memory>

namespace pulp::host {
class SignalGraph;
struct BakedPlan;
struct SampleRegionProof;
}
namespace pulp::state { class StateStore; }

namespace pulp::inspect {

/// The host shares this authority with every topology publication for one graph.
/// Host-main serialization keeps generation checks and adoption one transaction.
struct ControlSampleRegionGeneration {
    std::uint64_t value = 1;
};

class ControlSampleRegionTarget final {
  public:
    using FrozenProof = std::function<host::SampleRegionProof(std::uint32_t, int)>;
    static std::shared_ptr<ControlSampleRegionTarget> editable(
        host::SignalGraph&, state::StateStore&, ControlSampleRegionGeneration&);
    /// The plan and proof provider belong to the admitted signed processor.
    /// They must remain alive until the Standalone executor has drained.
    static std::shared_ptr<ControlSampleRegionTarget> frozen(
        const host::BakedPlan&, state::StateStore&, ControlSampleRegionGeneration&,
        FrozenProof, std::function<bool()> prepared);
    ~ControlSampleRegionTarget();
    bool can_read() const noexcept;
    bool can_edit() const noexcept;
    bool uses_state_store(const state::StateStore&) const noexcept;
    void set_preparation_context(double sample_rate, int max_block_size) noexcept;
    ControlExecutionOutcome read(const ControlAdmissionPlan&, const ControlRequestEnvelope&,
                                 const ControlExecutionContext&);
    ControlExecutionOutcome edit(const ControlAdmissionPlan&, const ControlRequestEnvelope&,
                                 const ControlExecutionContext&);

  private:
    struct Impl;
    explicit ControlSampleRegionTarget(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> impl_;
};

using ControlSampleRegionTargetResolver =
    std::function<std::shared_ptr<ControlSampleRegionTarget>(const ControlAdmissionPlan&)>;

} // namespace pulp::inspect
