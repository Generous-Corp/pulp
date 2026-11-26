#pragma once

// host_parameter_edit.hpp — format-neutral provenance for editor parameter
// writes (WAH-5).
//
// The problem this replaces: the VST3 adapter decided a StateStore write came
// from the editor by checking whether a GESTURE was open. That is an inference,
// not a fact, and it is wrong in a case users hit routinely — host automation
// arriving while the user holds a knob. The automation write landed in the
// store, the "is a gesture open?" test said yes, and the adapter echoed the
// host's own value back at it through performEdit(). It also read
// `get_normalized(id)` twice per reported edit, so a write landing between the
// two reads produced a performEdit and a setParamNormalized carrying DIFFERENT
// values for one logical change.
//
// The fix is to record provenance where it is known — at the write — instead of
// guessing at it afterwards:
//
//   * host-originated writes run inside `ScopedHostParameterWrite`;
//   * processor-originated writes run inside `ScopedProcessorParameterWrite`;
//   * everything else on the editor thread is, by elimination, an editor write.
//
// This is the model CLAP already used (thread id + explicit host/process
// flags), lifted out of clap_adapter.cpp so VST3 gets it too. CLAP keeps its
// own outbound SPSC queue for DELIVERY — the protocol needs events placed in
// the process block — and only shares the provenance DECISION. That split is
// deliberate: absorbing CLAP's queue here would weaken its ordering guarantees
// for no gain.

#include <pulp/state/listener_token.hpp>
#include <pulp/state/store.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <thread>

namespace pulp::format {

/// Marks the enclosing scope as a HOST-originated parameter write.
///
/// Wrap every point where the host's value reaches the StateStore: the
/// automation applied during process(), a setState/preset restore, a
/// host-driven controller write. Without the mark, an edit bridge on the same
/// thread cannot tell the host's own value from the user's and will echo it
/// back — which hosts variously treat as a fight for control, a spurious
/// automation write, or an undo-history entry the user never made.
///
/// Reentrant and thread-local: nesting is counted, and a mark on one thread
/// never suppresses a genuine editor write on another.
class ScopedHostParameterWrite {
public:
    ScopedHostParameterWrite() noexcept { ++depth(); }
    ScopedHostParameterWrite(const ScopedHostParameterWrite&) = delete;
    ScopedHostParameterWrite& operator=(const ScopedHostParameterWrite&) = delete;
    ~ScopedHostParameterWrite() noexcept { --depth(); }

    static bool active() noexcept { return depth() > 0; }

private:
    static int& depth() noexcept {
        thread_local int d = 0;
        return d;
    }
};

/// Marks the enclosing scope as a PROCESSOR-originated parameter write (a
/// plug-in writing its own output parameters during process()). Those reach the
/// host through the adapter's own output-parameter path; reporting them as
/// editor edits would double-report them.
class ScopedProcessorParameterWrite {
public:
    ScopedProcessorParameterWrite() noexcept { ++depth(); }
    ScopedProcessorParameterWrite(const ScopedProcessorParameterWrite&) = delete;
    ScopedProcessorParameterWrite& operator=(const ScopedProcessorParameterWrite&) = delete;
    ~ScopedProcessorParameterWrite() noexcept { --depth(); }

    static bool active() noexcept { return depth() > 0; }

private:
    static int& depth() noexcept {
        thread_local int d = 0;
        return d;
    }
};

/// True when the calling thread is inside a host- or processor-originated
/// parameter write, i.e. when an edit bridge must stay silent.
inline bool in_non_editor_parameter_write() noexcept {
    return ScopedHostParameterWrite::active() ||
           ScopedProcessorParameterWrite::active();
}

/// Remembers which parameters the host wrote recently, so a plug-in that
/// mirrors a host-written value back into the store is not reported as editing
/// that parameter.
///
/// The write-scope marks above cover the host's own write only. A plug-in that
/// later re-writes the value on the main thread (typically after quantising it
/// to a device step) is, by thread alone, indistinguishable from an editor
/// edit, so each block's automation came back at the host as a plug-in edit.
/// Hosts such as REAPER then treat the parameter as touched and stop applying
/// its envelope. The guard makes that impossible: a non-host write to a
/// parameter the host wrote within `kWindowNs` is not reported, unless the
/// editor holds a gesture on it (a real user edit).
///
/// Only host writes made off the main thread are remembered (automation applied
/// on the audio thread). A host write on the main thread (a controller call, a
/// state restore) is a discrete event that, by thread alone, cannot be told
/// from the user's edit right after it, so it arms nothing.
///
/// What is NOT hidden: a write after the window, a write to a parameter the
/// host has not touched, and anything inside an open gesture. A write inside
/// the window is hidden even when it is a genuine plug-in change; during live
/// automation the host's envelope owns the parameter in that window anyway.
///
/// `note_host_write` is real-time safe (a bounded probe over a fixed table, no
/// allocation, no locks). A full table fails open: the write is simply not
/// remembered, so the worst case is the old behaviour for that parameter.
class HostWriteEchoGuard {
  public:
    /// How long a host write keeps a parameter's non-host writes quiet. Long
    /// enough to cover a main-thread listener pump, short enough that a user
    /// edit after the automation stops is reported at once.
    static constexpr std::int64_t kWindowNs = 150'000'000;

    /// Record that the host wrote @p id now. Any thread, real-time safe.
    void note_host_write(state::ParamID id) noexcept;

    /// True when @p id was host-written within the window and the editor has no
    /// gesture open on it. Call on the main thread, for a non-host write.
    bool is_echo(const state::StateStore& store, state::ParamID id) const noexcept;

    /// Test seam: same as `is_echo` with an explicit clock reading.
    bool is_echo_at(const state::StateStore& store, state::ParamID id,
                    std::int64_t now_ns) const noexcept;

    static std::int64_t now_ns() noexcept {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

  private:
    static constexpr std::size_t kSlots = 1024;
    static constexpr std::size_t kProbe = 16;
    struct Slot {
        std::atomic<std::uint64_t> key{0}; // id + 1; 0 means empty
        std::atomic<std::int64_t> stamp_ns{0};
    };
    std::array<Slot, kSlots> slots_{};
};

/// Reports EDITOR parameter edits to the host, in protocol order, exactly once.
///
/// Owns three guarantees the ad-hoc VST3 wiring did not have:
///
///  1. **Provenance is explicit.** A change is reported only when it happened
///     on the editor thread and outside any host/processor write scope. An open
///     gesture is no longer taken as evidence of who wrote the value.
///  2. **One snapshot per reported edit.** An edit is reported with the value
///     the editor wrote, taken from the write itself, never re-read from the
///     store: a host automation write landing between the edit and the report
///     would otherwise be reported to the host as the user's edit. The same
///     snapshot is handed to every callback for that edit, so a concurrent
///     write cannot split one logical change across two different values.
///  3. **One place that closes a gesture.** The final value report and the
///     end-of-gesture notification are emitted together, in that order, so a
///     host that latches on end records where the control actually landed.
///
/// Threading: construct on the editor/UI thread — the constructor captures that
/// thread's id as the editor thread. The store listener runs INLINE on the
/// writing thread (like CLAP's), which is what makes the thread check
/// meaningful; a marshalled listener would deliver every write on the UI thread
/// and lose the origin.
class HostParameterEditBridge {
public:
    /// Protocol callbacks, in the order they are invoked for a gesture:
    /// `begin` → `value`(one or more) → `value` → `end`.
    struct Callbacks {
        /// Host gesture open (VST3 beginEdit, CLAP GestureBegin).
        std::function<void(state::ParamID)> begin;
        /// Report a value. `normalized` and `plain` are the same single
        /// snapshot for this edit; `plain` lets a format map it to the host's
        /// normalized domain in its own precision.
        std::function<void(state::ParamID, float normalized, float plain)> value;
        /// Host gesture close (VST3 endEdit, CLAP GestureEnd).
        std::function<void(state::ParamID)> end;
    };

    HostParameterEditBridge(state::StateStore& store, Callbacks callbacks);
    ~HostParameterEditBridge();

    HostParameterEditBridge(const HostParameterEditBridge&) = delete;
    HostParameterEditBridge& operator=(const HostParameterEditBridge&) = delete;

    /// The thread this bridge treats as the editor. Exposed for tests and for
    /// an adapter that needs to assert its own threading assumptions.
    std::thread::id editor_thread() const noexcept { return editor_thread_; }

    /// Whether a call on THIS thread right now would be reported. Exposed so a
    /// format that keeps its own delivery queue (CLAP) can share the decision
    /// without sharing the transport.
    bool would_report() const noexcept;

    /// The echo guard this bridge feeds and consults. A format that keeps its
    /// own delivery queue (CLAP) or listener (AU) shares it the same way.
    HostWriteEchoGuard& echo_guard() noexcept {
        return echo_guard_;
    }

private:
    void on_begin(state::ParamID id);
    void on_end(state::ParamID id);
    void on_value(state::ParamID id, float plain);
    void report(state::ParamID id, float plain);

    state::StateStore& store_;
    Callbacks callbacks_;
    std::thread::id editor_thread_;
    HostWriteEchoGuard echo_guard_;
    state::ListenerToken listener_;
};

}  // namespace pulp::format
