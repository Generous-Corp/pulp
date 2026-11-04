// editor_host.cpp — the default ParamPort over a StateStore. See
// editor_host.hpp for the contract.

#include <pulp/format/editor_host.hpp>

#include <pulp/state/listener_token.hpp>
#include <pulp/state/store.hpp>

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <utility>
#include <vector>
#include <unordered_map>
namespace pulp::format {

// The port's state, shared with its subscriptions so one can be cancelled
// after the port is gone.
//
// The lock guards only this state: the port never calls the store while
// holding it. A call into the store can reach the host (gesture and value
// callbacks), and the host may hold its own lock while it disposes of the
// plug-in, which takes this lock through `disconnect()`; calling out under
// this lock could deadlock the two. Instead each store call registers itself
// as a user, and `disconnect()` cuts the store off and then waits for the
// users already inside it, so the store is never touched once disconnect()
// returns. The lock is recursive because a host callback may re-enter the
// port.
struct StateStoreParamPort::Link {
    mutable std::recursive_mutex mutex;
    mutable std::condition_variable_any idle;
    state::StateStore* store = nullptr;
    mutable int users = 0;
    std::unordered_map<state::ParamID, int> held;
    std::vector<std::shared_ptr<state::ListenerToken>> tokens;
    // The parameter list as it was when the port was made, so the pointers
    // params() and info() hand out stay valid for the port's lifetime, store
    // or no store.
    std::vector<state::ParamInfo> infos;

    // Run `use` on the store, outside the lock. False when disconnected.
    template <typename Use> bool with_store(Use&& use) const {
        state::StateStore* current = nullptr;
        {
            std::lock_guard lock(mutex);
            if (store == nullptr)
                return false;
            current = store;
            ++users;
        }
        use(*current);
        std::lock_guard lock(mutex);
        if (--users == 0)
            idle.notify_all();
        return true;
    }

    bool known(state::ParamID id) const {
        return std::any_of(infos.begin(), infos.end(),
                           [id](const state::ParamInfo& p) { return p.id == id; });
    }
};

StateStoreParamPort::StateStoreParamPort(state::StateStore& store)
    : link_(std::make_shared<Link>()) {
    link_->store = &store;
    const auto all = store.all_params();
    link_->infos.assign(all.begin(), all.end());
}

StateStoreParamPort::~StateStoreParamPort() {
    release_all();
}

std::span<const state::ParamInfo> StateStoreParamPort::params() const {
    return link_->infos;
}

const state::ParamInfo* StateStoreParamPort::info(state::ParamID id) const {
    for (const auto& p : link_->infos)
        if (p.id == id)
            return &p;
    return nullptr;
}

float StateStoreParamPort::get(state::ParamID id) const {
    float value = 0.0f;
    link_->with_store([&](state::StateStore& store) { value = store.get_value(id); });
    return value;
}

void StateStoreParamPort::begin(state::ParamID id) {
    if (!link_->known(id))
        return;
    link_->with_store([&](state::StateStore& store) {
        // The store counts leases across every port and binding on this
        // store, so the host sees one begin for overlapping gestures; this
        // port counts its own share so it can close exactly what it opened.
        store.acquire_gesture(id);
        std::lock_guard lock(link_->mutex);
        ++link_->held[id];
    });
}

void StateStoreParamPort::set(state::ParamID id, float plain_value) {
    if (!link_->known(id))
        return;
    const bool in_gesture = open_gestures(id) > 0;
    link_->with_store([&](state::StateStore& store) {
        if (in_gesture) {
            store.set_value(id, plain_value);
            return;
        }
        // A write outside a gesture is a complete edit. Hosts expect every
        // editor value change inside a begin/end bracket (VST3 performEdit is
        // only valid between beginEdit and endEdit), so bracket it here.
        store.acquire_gesture(id);
        store.set_value(id, plain_value);
        store.release_gesture(id);
    });
}

void StateStoreParamPort::end(state::ParamID id) {
    {
        std::lock_guard lock(link_->mutex);
        const auto found = link_->held.find(id);
        if (found == link_->held.end())
            return;
        if (--found->second == 0)
            link_->held.erase(found);
    }
    link_->with_store([&](state::StateStore& store) { store.release_gesture(id); });
}

ParamSubscription StateStoreParamPort::add_listener(Listener listener) {
    std::shared_ptr<state::ListenerToken> token;
    const bool connected = link_->with_store([&](state::StateStore& store) {
        // Main-thread delivery coalesces audio-thread writes to their latest
        // value; Reconcile replays the current value after a state restore so
        // an editor's cached control positions follow the restored state.
        token = std::make_shared<state::ListenerToken>(
            store.add_listener(std::move(listener), state::ListenerThread::Main,
                               state::ListenerRestoreBehavior::Reconcile));
    });
    if (!connected || !token)
        return {};
    {
        std::lock_guard lock(link_->mutex);
        link_->tokens.push_back(token);
    }
    std::weak_ptr<Link> weak = link_;
    // A token holds its store's listener registry weakly, so resetting it is
    // safe after the store is gone.
    return ParamSubscription([weak, token] {
        if (auto link = weak.lock()) {
            std::lock_guard lock(link->mutex);
            auto& tokens = link->tokens;
            tokens.erase(std::remove(tokens.begin(), tokens.end(), token), tokens.end());
        }
        token->reset();
    });
}

void StateStoreParamPort::release_all() {
    std::unordered_map<state::ParamID, int> ledger;
    {
        // Detach the ledger under the lock; call the store after it.
        std::lock_guard lock(link_->mutex);
        ledger = std::move(link_->held);
        link_->held.clear();
    }
    if (ledger.empty())
        return;
    link_->with_store([&](state::StateStore& store) {
        for (const auto& [id, count] : ledger)
            for (int i = 0; i < count; ++i)
                store.release_gesture(id);
    });
}

void StateStoreParamPort::disconnect() {
    state::StateStore* store = nullptr;
    std::unordered_map<state::ParamID, int> ledger;
    std::vector<std::shared_ptr<state::ListenerToken>> tokens;
    {
        std::unique_lock lock(link_->mutex);
        store = std::exchange(link_->store, nullptr);
        // No new store call can start; let the ones already inside finish,
        // so nothing touches the store once this returns.
        link_->idle.wait(lock, [this] { return link_->users == 0; });
        ledger = std::move(link_->held);
        link_->held.clear();
        tokens = std::move(link_->tokens);
        link_->tokens.clear();
    }
    if (store == nullptr)
        return;
    for (const auto& [id, count] : ledger)
        for (int i = 0; i < count; ++i)
            store->release_gesture(id);
    for (auto& token : tokens)
        token->reset();
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
