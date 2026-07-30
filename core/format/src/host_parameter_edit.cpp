// host_parameter_edit.cpp — see host_parameter_edit.hpp for why provenance is
// recorded at the write rather than inferred from an open gesture.

#include <pulp/format/host_parameter_edit.hpp>

namespace pulp::format {

HostParameterEditBridge::HostParameterEditBridge(state::StateStore& store,
                                                 Callbacks callbacks)
    : store_(store),
      callbacks_(std::move(callbacks)),
      editor_thread_(std::this_thread::get_id()) {
    store_.set_gesture_callbacks([this](state::ParamID id) { on_begin(id); },
                                 [this](state::ParamID id) { on_end(id); });
    // INLINE on the writing thread (ListenerThread::Audio), deliberately.
    //
    // A Main-thread listener is marshalled: every write — including host
    // automation applied on the audio thread — is delivered later, on the UI
    // thread, with its origin erased. That is precisely how host automation
    // used to be echoed back at the host as an editor edit. Running inline
    // keeps the write's thread and its enclosing origin scope observable, which
    // is the whole basis of the provenance decision.
    //
    // RT-safety: the guards below return before touching any callback unless
    // the write came from the editor thread, so the audio thread's path through
    // here is a thread-id compare and two thread_local reads.
    listener_ = store_.add_listener(
        [this](state::ParamID id, float) { on_value(id); },
        state::ListenerThread::Audio);
}

HostParameterEditBridge::~HostParameterEditBridge() {
    // Drop the listener BEFORE clearing the gesture callbacks: the listener is
    // the one that can still fire from another thread, and it must not observe
    // a half-torn-down bridge.
    listener_.reset();
    store_.set_gesture_callbacks({}, {});
}

bool HostParameterEditBridge::would_report() const noexcept {
    return std::this_thread::get_id() == editor_thread_ &&
           !in_non_editor_parameter_write();
}

void HostParameterEditBridge::on_begin(state::ParamID id) {
    if (!would_report()) return;
    if (callbacks_.begin) callbacks_.begin(id);
}

void HostParameterEditBridge::on_value(state::ParamID id) {
    if (!would_report()) return;
    if (!callbacks_.value) return;
    // ONE snapshot, handed to every consumer of this edit. The old code read
    // get_normalized() twice — once for performEdit and once for
    // setParamNormalized — so a write landing between them reported one logical
    // change as two different values.
    callbacks_.value(id, store_.get_normalized(id));
}

void HostParameterEditBridge::on_end(state::ParamID id) {
    if (!would_report()) return;
    // No local "is this gesture open?" set is needed: StateStore::end_gesture
    // (and release_gesture) only invoke this callback when a gesture was
    // genuinely open, so an unbalanced end never reaches here. The adapter used
    // to keep its own `editing_params_` set to answer that question — and then
    // reused it as a provenance test, which is the inference this class exists
    // to remove.
    // Report the final value BEFORE closing the gesture, so a host that latches
    // on end records where the control actually landed. Emitting both from one
    // place is what stops the two halves from drifting: the adapter used to
    // have a near-copy of this sequence in its end-gesture callback.
    if (callbacks_.value) callbacks_.value(id, store_.get_normalized(id));
    if (callbacks_.end) callbacks_.end(id);
}

}  // namespace pulp::format
