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

/// Reports EDITOR parameter edits to the host, in protocol order, exactly once.
///
/// Owns three guarantees the ad-hoc VST3 wiring did not have:
///
///  1. **Provenance is explicit.** A change is reported only when it happened
///     on the editor thread and outside any host/processor write scope. An open
///     gesture is no longer taken as evidence of who wrote the value.
///  2. **One snapshot per reported edit.** The normalized value is read once
///     and handed to every callback for that edit, so a concurrent write cannot
///     split one logical change across two different values.
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
        /// Report a value. `normalized` is the single snapshot for this edit.
        std::function<void(state::ParamID, float normalized)> value;
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

private:
    void on_begin(state::ParamID id);
    void on_end(state::ParamID id);
    void on_value(state::ParamID id);

    state::StateStore& store_;
    Callbacks callbacks_;
    std::thread::id editor_thread_;
    state::ListenerToken listener_;
};

}  // namespace pulp::format
