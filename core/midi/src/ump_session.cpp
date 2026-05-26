// Cross-platform UMP Session — virtual endpoint registry + dispatch.
//
// macOS plan item 8.1. The OS-backed half (CoreMIDI 2.0 client +
// physical endpoint open) lives in `platform/mac/ump_session_coremidi.mm`
// and is wired in via the `os_backend_*` weak hooks declared at the
// bottom of this file. On platforms without an OS backend the hooks
// are no-ops and the session is virtual-endpoints-only — which is
// exactly what the headless test target exercises.
//
// Threading:
//   - `register_virtual_endpoint` / `unregister_virtual_endpoint` take
//     the impl mutex.
//   - `enumerate_endpoints` snapshots under the same mutex and copies
//     out, so callers iterate without holding it.
//   - `open_endpoint` is mutex-free for the virtual lookup; the OS
//     backend manages its own state.

#include <pulp/midi/ump_session.hpp>

#include "ump_session_backend.hpp"

#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace pulp::midi {

// ── VirtualUmpEndpoint ─────────────────────────────────────────────

VirtualUmpEndpoint::VirtualUmpEndpoint(VirtualUmpEndpointConfig cfg)
    : loopback_(cfg.loopback) {
    info_.id = cfg.name;
    info_.name = std::move(cfg.name);
    info_.is_virtual = true;
    info_.direction = cfg.direction;
}

void VirtualUmpEndpoint::set_receive_callback(UmpReceiveCallback cb) {
    std::lock_guard<std::mutex> lk(cb_mu_);
    cb_ = std::move(cb);
}

bool VirtualUmpEndpoint::send(const UmpPacket& packet) {
    if (!info_.direction.can_send) return false;
    if (!open_.load(std::memory_order_acquire)) return false;
    sent_.fetch_add(1, std::memory_order_relaxed);
    if (loopback_) {
        // Loopback: synchronously fire the receive callback with the
        // packet we just sent. Tests assert the callback fired.
        UmpReceiveCallback local_cb;
        {
            std::lock_guard<std::mutex> lk(cb_mu_);
            local_cb = cb_;
        }
        if (local_cb) {
            delivered_.fetch_add(1, std::memory_order_relaxed);
            local_cb(packet, 0.0);
        }
    }
    return true;
}

bool VirtualUmpEndpoint::deliver(const UmpPacket& packet, double timestamp_sec) {
    if (!open_.load(std::memory_order_acquire)) return false;
    if (!info_.direction.can_receive) return false;
    UmpReceiveCallback local_cb;
    {
        std::lock_guard<std::mutex> lk(cb_mu_);
        local_cb = cb_;
    }
    if (!local_cb) return false;
    delivered_.fetch_add(1, std::memory_order_relaxed);
    local_cb(packet, timestamp_sec);
    return true;
}

void VirtualUmpEndpoint::close() {
    open_.store(false, std::memory_order_release);
    std::lock_guard<std::mutex> lk(cb_mu_);
    cb_ = nullptr;
}

// ── UmpSession::Impl ───────────────────────────────────────────────

struct UmpSession::Impl {
    UmpSessionConfig cfg;

    mutable std::mutex mu;
    std::unordered_map<std::string, std::shared_ptr<VirtualUmpEndpoint>> virtuals;

    // Wires from a virtual endpoint id → list of receiver virtual endpoint ids.
    // Set up by `wire_virtual_loopback`. The actual wiring is implemented by
    // installing a receive callback on `from` that calls `deliver()` on each
    // target.
    std::unordered_map<std::string, std::vector<std::string>> wires;

    // True iff the OS backend (e.g. CoreMIDI 2.0) was successfully
    // initialised. Set by `os_backend_init`; reset by `os_backend_shutdown`.
    bool os_active = false;

    // Opaque OS backend state (CoreMIDI MIDIClientRef wrapper etc.).
    // Owned by the platform .mm; we hold a void* and let the backend
    // shutdown hook reclaim it.
    void* os_state = nullptr;
};

// OS backend hook table. Each platform implementation strong-overrides
// these by installing the vtable from a static initialiser; if no backend
// file is linked, the no-op defaults stay in place and the session
// reports `os_backend_active() == false`.

namespace ump_os {

// Mutable singleton, patched by the platform registrar.
inline OsBackendVTable& vtable() {
    static OsBackendVTable v;
    return v;
}

} // namespace ump_os

// Called from the platform .mm/.cpp static initialiser to install hooks.
void register_ump_os_backend(const ump_os::OsBackendVTable& v) {
    ump_os::vtable() = v;
}

// ── UmpSession ─────────────────────────────────────────────────────

UmpSession::UmpSession() : UmpSession(UmpSessionConfig{}) {}

UmpSession::UmpSession(UmpSessionConfig cfg) : impl_(std::make_unique<Impl>()) {
    impl_->cfg = std::move(cfg);
    if (impl_->cfg.enable_os_backend && ump_os::vtable().init) {
        impl_->os_active = ump_os::vtable().init(impl_->cfg, &impl_->os_state);
    }
}

UmpSession::~UmpSession() {
    if (impl_->os_active && ump_os::vtable().shutdown) {
        ump_os::vtable().shutdown(impl_->os_state);
    }
    // Virtual endpoints close themselves when the shared_ptr drops, but
    // explicitly closing them here makes lifetime ordering obvious for
    // anyone reading a crash log.
    std::lock_guard<std::mutex> lk(impl_->mu);
    for (auto& [_, ep] : impl_->virtuals) {
        if (ep) ep->close();
    }
    impl_->virtuals.clear();
}

bool UmpSession::os_backend_active() const noexcept {
    return impl_->os_active;
}

std::vector<UmpEndpointInfo> UmpSession::enumerate_endpoints() const {
    std::vector<UmpEndpointInfo> out;
    if (impl_->os_active && ump_os::vtable().enumerate) {
        out = ump_os::vtable().enumerate(impl_->os_state);
    }
    std::lock_guard<std::mutex> lk(impl_->mu);
    out.reserve(out.size() + impl_->virtuals.size());
    for (const auto& [_, ep] : impl_->virtuals) {
        if (ep) out.push_back(ep->info());
    }
    return out;
}

UmpEndpoint* UmpSession::open_endpoint(const std::string& id, UmpOpenStatus* status) {
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        auto it = impl_->virtuals.find(id);
        if (it != impl_->virtuals.end() && it->second) {
            if (status) *status = UmpOpenStatus::Ok;
            return it->second.get();
        }
    }
    if (impl_->os_active && ump_os::vtable().open) {
        return ump_os::vtable().open(impl_->os_state, id, status);
    }
    if (status) {
        *status = impl_->os_active ? UmpOpenStatus::NotFound
                                   : UmpOpenStatus::OsBackendUnavailable;
    }
    return nullptr;
}

std::shared_ptr<VirtualUmpEndpoint>
UmpSession::register_virtual_endpoint(VirtualUmpEndpointConfig cfg) {
    auto ep = std::make_shared<VirtualUmpEndpoint>(std::move(cfg));
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->virtuals[ep->info().id] = ep;
    return ep;
}

bool UmpSession::unregister_virtual_endpoint(const std::string& id) {
    std::shared_ptr<VirtualUmpEndpoint> dropped;
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        auto it = impl_->virtuals.find(id);
        if (it == impl_->virtuals.end()) return false;
        dropped = std::move(it->second);
        impl_->virtuals.erase(it);
        impl_->wires.erase(id);
        for (auto& [_, targets] : impl_->wires) {
            targets.erase(std::remove(targets.begin(), targets.end(), id),
                          targets.end());
        }
    }
    if (dropped) dropped->close();
    return true;
}

std::size_t UmpSession::virtual_endpoint_count() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->virtuals.size();
}

bool UmpSession::wire_virtual_loopback(const std::string& from_id,
                                       const std::string& to_id) {
    std::shared_ptr<VirtualUmpEndpoint> from;
    std::shared_ptr<VirtualUmpEndpoint> to;
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        auto fit = impl_->virtuals.find(from_id);
        auto tit = impl_->virtuals.find(to_id);
        if (fit == impl_->virtuals.end() || tit == impl_->virtuals.end()) return false;
        from = fit->second;
        to = tit->second;
        impl_->wires[from_id].push_back(to_id);
    }
    if (!from || !to) return false;
    // Install a callback on `from` that forwards every received packet
    // into `to`. We capture `to` by shared_ptr so the wire stays valid
    // even if the session's internal map is mutated concurrently
    // (unregister will still close the target, after which `deliver`
    // returns false harmlessly).
    auto target = to;
    from->set_receive_callback([target](const UmpPacket& p, double ts) {
        if (target) target->deliver(p, ts);
    });
    return true;
}

} // namespace pulp::midi
