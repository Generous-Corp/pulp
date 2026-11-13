// SPDX-License-Identifier: MIT
#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <pulp/host/graph_types.hpp>
#include <pulp/host/plugin_slot.hpp>
#include <string>
#include <vector>

namespace pulp::host {

enum class IsolationReason : std::uint32_t {
    none,
    unsupported,
    worker_missing,
    identity_mismatch,
    launch_failed,
    lifecycle_timeout,
    lifecycle_failed,
    deadline_missed,
    queue_full,
    invalid_completion,
    nonfinite_audio,
    plugin_failed,
    stopped,
    worker_exited,
    registry_full,
    quarantine_active,
    checkpoint_invalid,
};
enum class IsolationLifecycle : std::uint32_t { loaded, prepared, stopped, terminal };
enum class IsolationCleanup : std::uint32_t { pending, complete, incomplete, custody_lost };

/// Detached snapshot. A terminal reason never clears, including after cleanup.
struct IsolationStatus {
    IsolationReason reason = IsolationReason::none;
    std::uint64_t generation = 0;
    std::uint64_t block_sequence = 0;
    IsolationLifecycle lifecycle = IsolationLifecycle::loaded;
    IsolationCleanup cleanup = IsolationCleanup::pending;
    int worker_pid = -1;
};

struct IsolatedClapOptions {
    PluginSelection selection;
    /// Explicit companion, built from the same source revision as the host.
    std::string worker_path;
    /// Required executable-byte SHA-256 values. No vendor code runs in parent.
    std::string plugin_sha256;
    std::string worker_sha256;
};

namespace detail {
struct IsolatedClapSession;
}

/// Control-thread-only lifecycle. Before pause/checkpoint/stop, drain graph
/// readers and stop invoking process(). No automatic restart or DSP carry.
class IsolatedClapControl {
  public:
    ~IsolatedClapControl();
    IsolationStatus status() const noexcept;
    bool pause();
    bool checkpoint();
    IsolationCleanup stop();
    /// Deadline-limited custody cleanup without invoking vendor destruction.
    /// An incomplete result retains the finite reservation. A later explicit
    /// call may finish cleanup; creation and recovery remain refused meanwhile.
    IsolationCleanup cleanup(std::chrono::steady_clock::time_point deadline);
    /// Last accepted bounded checkpoint, empty until one has been accepted. A
    /// recovered generation starts with the checkpoint it was restored from.
    std::vector<std::uint8_t> accepted_checkpoint() const;

  private:
    friend struct IsolatedClapAccess;
    explicit IsolatedClapControl(std::shared_ptr<detail::IsolatedClapSession>);
    std::shared_ptr<detail::IsolatedClapSession> session_;
};

struct IsolatedClapResult {
    std::unique_ptr<PluginSlot> slot;
    std::shared_ptr<IsolatedClapControl> control;
    IsolationReason error = IsolationReason::none;
    explicit operator bool() const noexcept {
        return slot != nullptr && control != nullptr;
    }
};

/// MacClapStereo64V1: arm64 macOS, 48kHz, stereo, 64 frames, headless CLAP
/// effect with zero reported latency/tail. Adds 64 samples of graph PDC latency.
/// Refuses unsupported requests; never falls back to in-process loading.
IsolatedClapResult load_isolated_clap(const IsolatedClapOptions&);

/// Requires complete old custody cleanup and an accepted checkpoint. Constructs
/// a fresh generation; the old proxy remains terminal and cannot be revived.
IsolatedClapResult recover_isolated_clap(const IsolatedClapOptions&,
                                         const IsolatedClapControl& old);

class SignalGraph;

/// Typed outcome of restoring a terminal isolated graph node from its checkpoint.
enum class IsolatedNodeRecovery : std::uint32_t {
    recovered,
    not_isolated_node,   ///< The node does not publish an isolated proxy.
    control_mismatch,    ///< `old` does not control the node's published proxy.
    not_terminal,        ///< The node is still healthy; recovery never preempts it.
    identity_mismatch,   ///< Options name a different plugin type or plugin binary.
    no_checkpoint,       ///< `old` never accepted a checkpoint.
    cleanup_incomplete,  ///< Old worker custody is not released; no new worker spawns.
    worker_failed,       ///< The fresh worker did not start or rejected the checkpoint.
    publication_refused, ///< The graph refused publication; see last_swap_diagnostics().
};

struct IsolatedNodeRecoveryResult {
    IsolatedNodeRecovery outcome = IsolatedNodeRecovery::not_isolated_node;
    /// Worker-side reason when outcome is worker_failed.
    IsolationReason worker_error = IsolationReason::none;
    /// Control for the recovered generation; null unless recovered.
    std::shared_ptr<IsolatedClapControl> control;
    explicit operator bool() const noexcept {
        return outcome == IsolatedNodeRecovery::recovered;
    }
};

/// Control-thread recovery of a prepared graph's terminal isolated node: waits
/// (bounded) for the old worker's custody cleanup, spawns a fresh generation
/// restored from `old`'s accepted checkpoint, and publishes it through
/// SignalGraph::publish_isolated_recovery(). The callback renders terminal
/// silence until that publication and never allocates, locks or performs IPC for
/// it; siblings keep rendering throughout. Every refusal leaves the old silent
/// node published. The checkpoint is the one accepted while the node was not
/// processing (pause() is terminal for a node a graph is still rendering).
IsolatedNodeRecoveryResult recover_isolated_graph_node(SignalGraph& graph, NodeId node,
                                                       const IsolatedClapOptions& options,
                                                       IsolatedClapControl& old);

/// Graph integration identifies the opt-in proxy without changing PluginSlot ABI.
bool is_isolated_plugin_slot(const PluginSlot&) noexcept;

} // namespace pulp::host
