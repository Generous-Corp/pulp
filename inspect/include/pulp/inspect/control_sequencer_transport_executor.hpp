#pragma once

#include <pulp/inspect/control_execution.hpp>

#include <functional>
#include <optional>

namespace pulp::playback {
class MasterTransport;
}

namespace pulp::inspect {

/// The exact live transport an admitted registration may observe and edit. The
/// resolver owns the lifetime contract: the transport must stay alive for the
/// synchronous call. A null transport is a refusal, never a reason to answer
/// from the tick-domain helper instead: the helper validates tick structure and
/// knows nothing about the sample-domain bounds the transport enforces, so its
/// verdict cannot stand in for the transport's.
struct ControlSequencerTransportTarget {
    ControlRegistrationId registration_id;
    ControlHostTier host_tier = ControlHostTier::SharedPluginHost;
    playback::MasterTransport* transport = nullptr;
};

using ControlSequencerTransportTargetResolver =
    std::function<std::optional<ControlSequencerTransportTarget>(const ControlAdmissionPlan&)>;

/// Implements dev.pulp.sequencer/transport.loop.read@1. It reports the
/// transport's own published playhead. A transport that has never published
/// (sequence 0) is unprepared, and the operation refuses rather than inventing
/// a loop that no transport has accepted.
ControlOperationExecutor make_control_sequencer_transport_read_executor(
    ControlSequencerTransportTargetResolver resolve_target);

/// Implements dev.pulp.sequencer/transport.loop.write@1. Endpoints arrive
/// already snapped; the operation never snaps for the caller. The receipt
/// carries the transport's acceptance, so every refusal the transport can
/// return is mapped to its own message and the tick-domain collapsed-span
/// refusal is distinguishable from all of them. Intended to be wrapped by
/// ControlMainThreadExecutor; it refuses an off-main call whenever a real host
/// dispatcher is installed.
ControlOperationExecutor make_control_sequencer_transport_write_executor(
    ControlSequencerTransportTargetResolver resolve_target);

} // namespace pulp::inspect
