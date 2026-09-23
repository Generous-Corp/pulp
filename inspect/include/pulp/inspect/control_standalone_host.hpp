#pragma once

#include <pulp/format/standalone_control_host.hpp>
#include <pulp/inspect/control_host_development_executor.hpp>
#include <pulp/inspect/control_sample_region_target.hpp>
#include <pulp/inspect/control_timeline_document_session_executor.hpp>
#include <pulp/state/sequencer_state_channel.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string_view>

namespace pulp::inspect {

class RuntimeEvaluator;
enum class ControlTelemetrySensitivity : std::uint8_t;

} // namespace pulp::inspect

namespace pulp::view::motion {
struct RenderCostSnapshot;
}

namespace pulp::format {
class ViewBridge;
}

namespace pulp::playback {
class MasterTransport;
}

namespace pulp::inspect {

/// Creates the canonical broker-enrolled host bridge for an explicitly
/// control-enabled Standalone executable. A direct launch remains inert; only
/// the broker's inherited, kernel-authenticated bootstrap can open the host.
std::unique_ptr<format::StandaloneControlHost> make_control_standalone_host();

namespace detail {

struct StandaloneControlAuthorHooks {
    std::function<ControlTelemetrySensitivity(std::string_view)> telemetry_classifier;
    std::function<view::motion::RenderCostSnapshot()> motion_cost_probe;
    std::filesystem::path motion_fixture_path;
    std::function<std::vector<ControlDiagnosticItem>()> diagnostics;
    std::function<ControlAuthoringApplyResult(const ControlAuthoringChanges&)> apply_authoring;
    /// The live master transport the sequencer loop capabilities observe and
    /// edit. An absent hook leaves both operations refusing with an unavailable
    /// host rather than answering from a tick-domain helper the transport never
    /// saw.
    std::function<playback::MasterTransport*()> sequencer_transport;
};

using StandaloneControlAuthorHooksFactory = StandaloneControlAuthorHooks (*)(format::Processor&);

/// Installs optional author-owned observability inputs. Unspecified telemetry
/// remains sensitive, and absent Motion probes/fixtures remain unavailable.
bool install_standalone_control_author_hooks_factory(
    StandaloneControlAuthorHooksFactory factory) noexcept;
StandaloneControlAuthorHooks create_standalone_control_author_hooks(format::Processor& processor);

using StandaloneRuntimeEvaluatorFactory =
    std::shared_ptr<RuntimeEvaluator> (*)(format::Processor&, format::ViewBridge&);

using StandaloneSequencerStateChannelFactory =
    std::shared_ptr<state::SequencerStateChannel> (*)(format::Processor&);

/// Installs the author-owned sequencer state channel the control operations
/// dev.pulp.sequencer/state.read@1 and dev.pulp.sequencer/state.edit@1 bind to.
/// A host that installs nothing keeps both operations refusing with a typed
/// HostUnavailable, so the seam is opt-in rather than a silent empty grid.
///
/// The returned channel must be the SAME channel the host's engine thread
/// drives, and the host remains its single UI-side applied-echo consumer: the
/// operations only read the published snapshot/playhead and submit commands.
bool install_standalone_sequencer_state_channel_factory(
    StandaloneSequencerStateChannelFactory factory) noexcept;
std::shared_ptr<state::SequencerStateChannel>
create_standalone_sequencer_state_channel(format::Processor& processor);

/// Installed only by a research-unsafe author target that also links the
/// separately shipped high-risk evaluator archive.
bool install_standalone_runtime_evaluator_factory(
    StandaloneRuntimeEvaluatorFactory factory) noexcept;
std::shared_ptr<RuntimeEvaluator> create_standalone_runtime_evaluator(format::Processor& processor,
                                                                      format::ViewBridge& bridge);

using StandaloneTimelineDocumentSessionFactory =
    std::optional<ControlTimelineDocumentSessionSource> (*)(const ControlAdmissionPlan&);

/// Installs the host-owned binding between the broker's admitted plan and a
/// live `timeline::DocumentSession`. A Standalone executable owns no canonical
/// project, so without an installed factory the operation resolves to no
/// source and the executor refuses; a host that does own a timeline — Forge
/// Sequencer and Forge Modular both do — installs one and the same unified
/// control reaches its live document.
bool install_standalone_timeline_document_session_factory(
    StandaloneTimelineDocumentSessionFactory factory) noexcept;
std::optional<ControlTimelineDocumentSessionSource>
create_standalone_timeline_document_session_source(const ControlAdmissionPlan& plan);

/// Installs the product-owned binding between a live Processor graph and the
/// unified sample-region executors. The factory must return a target backed by
/// the same graph and StateStore used by the processor instance.
using StandaloneSampleRegionTargetFactory = std::shared_ptr<ControlSampleRegionTarget> (*)(
    format::Processor&, state::StateStore&, ControlSampleRegionGeneration&);
bool install_standalone_sample_region_target_factory(
    StandaloneSampleRegionTargetFactory factory) noexcept;
std::shared_ptr<ControlSampleRegionTarget>
create_standalone_sample_region_target(format::Processor& processor, state::StateStore& store,
                                       ControlSampleRegionGeneration& generation);

} // namespace detail

} // namespace pulp::inspect
