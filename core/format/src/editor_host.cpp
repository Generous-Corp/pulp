// editor_host.cpp — the default ParamPort over a StateStore. See
// editor_host.hpp for the contract.

#include <pulp/format/editor_host.hpp>

#include <pulp/state/listener_token.hpp>
#include <pulp/state/store.hpp>

#include <algorithm>
#include <mutex>
#include <vector>
#include <unordered_map>
namespace pulp::format {

// The port's state, shared with its subscriptions so one can be cancelled
// after the port is gone. One recursive lock covers it all: the store's
// gesture callbacks may re-enter the port, and `disconnect()` may come from
// another thread.
struct StateStoreParamPort::Link {
    mutable std::recursive_mutex mutex;
    state::StateStore* store = nullptr;
    std::unordered_map<state::ParamID, int> held;
    std::vector<std::shared_ptr<state::ListenerToken>> tokens;

    void release_all_locked() {
        // Detach the ledger before calling into the store: a host end
        // callback may re-enter this port.
        auto ledger = std::move(held);
        held.clear();
        if (store == nullptr)
            return;
        for (const auto& [id, count] : ledger)
            for (int i = 0; i < count; ++i)
                store->release_gesture(id);
    }
};

StateStoreParamPort::StateStoreParamPort(state::StateStore& store)
    : link_(std::make_shared<Link>()) {
    link_->store = &store;
}

StateStoreParamPort::~StateStoreParamPort() {
    release_all();
}

std::span<const state::ParamInfo> StateStoreParamPort::params() const {
    std::lock_guard lock(link_->mutex);
    if (link_->store == nullptr)
        return {};
    return link_->store->all_params();
}

const state::ParamInfo* StateStoreParamPort::info(state::ParamID id) const {
    std::lock_guard lock(link_->mutex);
    return link_->store ? link_->store->info(id) : nullptr;
}

float StateStoreParamPort::get(state::ParamID id) const {
    std::lock_guard lock(link_->mutex);
    return link_->store ? link_->store->get_value(id) : 0.0f;
}

void StateStoreParamPort::begin(state::ParamID id) {
    std::lock_guard lock(link_->mutex);
    auto* store = link_->store;
    if (store == nullptr || store->info(id) == nullptr)
        return;
    // The store counts leases across every port and binding on this store, so
    // the host sees one begin for overlapping gestures; this port counts its
    // own share so it can close exactly what it opened.
    store->acquire_gesture(id);
    ++link_->held[id];
}

void StateStoreParamPort::set(state::ParamID id, float plain_value) {
    std::lock_guard lock(link_->mutex);
    auto* store = link_->store;
    if (store == nullptr || store->info(id) == nullptr)
        return;
    if (open_gestures(id) > 0) {
        store->set_value(id, plain_value);
        return;
    }
    // A write outside a gesture is a complete edit. Hosts expect every
    // editor value change inside a begin/end bracket (VST3 performEdit is
    // only valid between beginEdit and endEdit), so bracket it here.
    store->acquire_gesture(id);
    store->set_value(id, plain_value);
    store->release_gesture(id);
}

void StateStoreParamPort::end(state::ParamID id) {
    std::lock_guard lock(link_->mutex);
    const auto found = link_->held.find(id);
    if (found == link_->held.end())
        return;
    if (--found->second == 0)
        link_->held.erase(found);
    if (link_->store != nullptr)
        link_->store->release_gesture(id);
}

ParamSubscription StateStoreParamPort::add_listener(Listener listener) {
    std::lock_guard lock(link_->mutex);
    if (link_->store == nullptr)
        return {};
    // Main-thread delivery coalesces audio-thread writes to their latest
    // value; Reconcile replays the current value after a state restore so an
    // editor's cached control positions follow the restored state.
    auto token = std::make_shared<state::ListenerToken>(
        link_->store->add_listener(std::move(listener), state::ListenerThread::Main,
                                   state::ListenerRestoreBehavior::Reconcile));
    link_->tokens.push_back(token);
    std::weak_ptr<Link> weak = link_;
    return ParamSubscription([weak, token] {
        auto link = weak.lock();
        if (!link) {
            token->reset();
            return;
        }
        std::lock_guard lock(link->mutex);
        // After disconnect() the token is already reset, so this touches no
        // store.
        token->reset();
        auto& tokens = link->tokens;
        tokens.erase(std::remove(tokens.begin(), tokens.end(), token), tokens.end());
    });
}

void StateStoreParamPort::release_all() {
    std::lock_guard lock(link_->mutex);
    link_->release_all_locked();
}

void StateStoreParamPort::disconnect() {
    std::lock_guard lock(link_->mutex);
    link_->release_all_locked();
    for (auto& token : link_->tokens)
        token->reset();
    link_->tokens.clear();
    link_->store = nullptr;
}

bool StateStoreParamPort::connected() const {
    std::lock_guard lock(link_->mutex);
    return link_->store != nullptr;
}

int StateStoreParamPort::open_gestures(state::ParamID id) const {
    std::lock_guard lock(link_->mutex);
    const auto found = link_->held.find(id);
    return found == link_->held.end() ? 0 : found->second;
}

} // namespace pulp::format
