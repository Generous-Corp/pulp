// editor_host.cpp — the default ParamPort over a StateStore. See
// editor_host.hpp for the contract.

#include <pulp/format/editor_host.hpp>

#include <pulp/state/listener_token.hpp>
#include <pulp/state/store.hpp>

namespace pulp::format {

StateStoreParamPort::StateStoreParamPort(state::StateStore& store) : store_(store) {}

StateStoreParamPort::~StateStoreParamPort() {
    release_all();
}

std::span<const state::ParamInfo> StateStoreParamPort::params() const {
    return store_.all_params();
}

const state::ParamInfo* StateStoreParamPort::info(state::ParamID id) const {
    return store_.info(id);
}

float StateStoreParamPort::get(state::ParamID id) const {
    return store_.get_value(id);
}

void StateStoreParamPort::begin(state::ParamID id) {
    if (store_.info(id) == nullptr)
        return;
    // The store counts leases across every port and binding on this store, so
    // the host sees one begin for overlapping gestures; this port counts its
    // own share so it can close exactly what it opened.
    store_.acquire_gesture(id);
    ++held_[id];
}

void StateStoreParamPort::set(state::ParamID id, float plain_value) {
    if (store_.info(id) == nullptr)
        return;
    if (open_gestures(id) > 0) {
        store_.set_value(id, plain_value);
        return;
    }
    // A write outside a gesture is a complete edit. Hosts expect every
    // editor value change inside a begin/end bracket (VST3 performEdit is
    // only valid between beginEdit and endEdit), so bracket it here.
    store_.acquire_gesture(id);
    store_.set_value(id, plain_value);
    store_.release_gesture(id);
}

void StateStoreParamPort::end(state::ParamID id) {
    const auto found = held_.find(id);
    if (found == held_.end())
        return;
    if (--found->second == 0)
        held_.erase(found);
    store_.release_gesture(id);
}

ParamSubscription StateStoreParamPort::add_listener(Listener listener) {
    // Main-thread delivery coalesces audio-thread writes to their latest
    // value; Reconcile replays the current value after a state restore so an
    // editor's cached control positions follow the restored state.
    auto token = std::make_shared<state::ListenerToken>(
        store_.add_listener(std::move(listener), state::ListenerThread::Main,
                            state::ListenerRestoreBehavior::Reconcile));
    return ParamSubscription([token] { token->reset(); });
}

void StateStoreParamPort::release_all() {
    // Detach the ledger before calling into the store: a host end callback may
    // re-enter this port.
    auto held = std::move(held_);
    held_.clear();
    for (const auto& [id, count] : held)
        for (int i = 0; i < count; ++i)
            store_.release_gesture(id);
}

int StateStoreParamPort::open_gestures(state::ParamID id) const {
    const auto found = held_.find(id);
    return found == held_.end() ? 0 : found->second;
}

} // namespace pulp::format
