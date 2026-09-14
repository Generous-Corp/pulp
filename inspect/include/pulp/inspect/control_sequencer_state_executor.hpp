#pragma once

#include <pulp/inspect/control_execution.hpp>
#include <pulp/state/sequencer_state_channel.hpp>

#include <functional>
#include <optional>

namespace pulp::inspect {

/// Exact live sequencer channel resolved only after broker admission. The
/// resolver binds the admission registration to one host-owned
/// `state::SequencerStateChannel`.
///
/// The channel is strictly single-producer/single-consumer per side: the engine
/// (audio) thread owns every `audio_*` method, and exactly one UI/message-thread
/// consumer owns the applied-echo FIFO. Neither sequencer operation is that
/// consumer. They therefore use only the non-destructive UI-side reads
/// (`ui_read_latest_snapshot`, `ui_read_playhead`, `ui_resync_required_epoch`)
/// and the UI-side producer `ui_try_submit`, and never touch
/// `ui_try_pop_applied` or any `audio_*` method. Both executors are intended to
/// be wrapped by ControlMainThreadExecutor and refuse an off-main call whenever
/// a real host dispatcher is installed, so the host's own main-thread UI owner
/// and these operations never race the single-writer command FIFO.
struct ControlSequencerStateTarget {
    ControlRegistrationId registration_id;
    ControlHostTier host_tier = ControlHostTier::SharedPluginHost;
    state::SequencerStateChannel* channel = nullptr;
};

using ControlSequencerStateTargetResolver =
    std::function<std::optional<ControlSequencerStateTarget>(const ControlAdmissionPlan&)>;

/// Implements dev.pulp.sequencer/state.read@1. The published snapshot reference
/// is copied out before the executor returns, so no caller can observe the
/// triple buffer after a later publish has recycled it.
ControlOperationExecutor
make_control_sequencer_state_read_executor(ControlSequencerStateTargetResolver resolve_target);

/// Implements dev.pulp.sequencer/state.edit@1. Success means the typed edit was
/// accepted by the lock-free command FIFO on the legal host thread. A full FIFO
/// is a typed retryable `ResourceExhausted` refusal, never a silent drop and
/// never a block on the audio thread. Engine application is asynchronous, so the
/// receipt reports acceptance with the assigned client sequence rather than
/// claiming the engine already folded the edit in.
ControlOperationExecutor
make_control_sequencer_state_edit_executor(ControlSequencerStateTargetResolver resolve_target);

} // namespace pulp::inspect
