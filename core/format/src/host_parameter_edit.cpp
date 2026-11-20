// host_parameter_edit.cpp — see host_parameter_edit.hpp for why provenance is
// recorded at the write rather than inferred from an open gesture.

#include <pulp/format/host_parameter_edit.hpp>

namespace pulp::format {

void HostWriteEchoGuard::note_host_write(state::ParamID id) noexcept {
    const std::uint64_t key = static_cast<std::uint64_t>(id) + 1;
    const std::int64_t now = now_ns();
    std::size_t index = (key * 0x9E3779B97F4A7C15ull) >> 32;
    for (std::size_t i = 0; i < kProbe; ++i) {
        auto& slot = slots_[(index + i) % kSlots];
        std::uint64_t current = slot.key.load(std::memory_order_acquire);
        if (current == 0 &&
            slot.key.compare_exchange_strong(current, key, std::memory_order_acq_rel))
            current = key;
        if (current == key) {
            slot.stamp_ns.store(now, std::memory_order_release);
            return;
        }
    }
}

bool HostWriteEchoGuard::is_echo(const state::StateStore& store, state::ParamID id) const noexcept {
    return is_echo_at(store, id, now_ns());
}

bool HostWriteEchoGuard::is_echo_at(const state::StateStore& store, state::ParamID id,
                                    std::int64_t now) const noexcept {
    const std::uint64_t key = static_cast<std::uint64_t>(id) + 1;
    std::size_t index = (key * 0x9E3779B97F4A7C15ull) >> 32;
    for (std::size_t i = 0; i < kProbe; ++i) {
        const auto& slot = slots_[(index + i) % kSlots];
        const std::uint64_t current = slot.key.load(std::memory_order_acquire);
        if (current == 0)
            return false;
        if (current != key)
            continue;
        const auto stamp = slot.stamp_ns.load(std::memory_order_acquire);
        if (stamp == 0 || now - stamp > kWindowNs)
            return false;
        return !store.gesture_open(id);
    }
    return false;
}

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
    listener_ = store_.add_listener([this](state::ParamID id, float value) { on_value(id, value); },
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

void HostParameterEditBridge::on_value(state::ParamID id, float plain) {
    // Remember the host's writes made off the editor thread: the audio
    // thread's automation, one write per block, is what a plug-in mirrors back.
    // A host write on the editor thread (setParamNormalized, a state restore)
    // is a discrete event, and by thread alone it cannot be told from the
    // user's edit that follows it, so it arms nothing. A write on another
    // thread that is not the processor's own output is the host's too: the
    // audio thread applies only host automation to the store.
    if (std::this_thread::get_id() != editor_thread_) {
        if (!ScopedProcessorParameterWrite::active())
            echo_guard_.note_host_write(id);
        return;
    }
    if (!would_report()) return;
    // The plug-in mirroring a host-written value back (often quantised) is
    // not an edit of that parameter.
    if (echo_guard_.is_echo(store_, id))
        return;
    if (!callbacks_.value) return;
    // The value this write stored, not a re-read: another listener on this
    // write may already have run a host automation write, and the store would
    // then hold the host's value rather than the user's.
    report(id, plain);
}

void HostParameterEditBridge::report(state::ParamID id, float plain) {
    // ONE snapshot, handed to every consumer of this edit, so performEdit and
    // setParamNormalized cannot carry two different values for one change.
    const auto* info = store_.info(id);
    callbacks_.value(id, info ? info->range.normalize(plain) : plain, plain);
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
    if (callbacks_.value)
        report(id, store_.get_value(id));
    if (callbacks_.end) callbacks_.end(id);
}

}  // namespace pulp::format
