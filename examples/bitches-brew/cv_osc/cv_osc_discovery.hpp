#pragma once

// Finding an OSC receiver on the network instead of asking the user to type its
// address.
//
// Receivers advertise themselves over mDNS as `_osc._udp`. Pulp already ships the
// browser; all this adds is (a) a snapshot the editor can read without touching
// the discovery callback's thread, and (b) somewhere to hang the honest answer
// when the platform has no mDNS backend at all.
//
// That last point is why `start()` returns a bool. On a build with no backend the
// browse silently discovers nothing, which is indistinguishable from a network
// with no receivers on it — so the editor asks, and says which one it is.

#include <brew/cv_osc.hpp>

#include <pulp/events/service_discovery.hpp>

#include <algorithm>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace pulp::examples::brew {

/// A receiver we found, reduced to the two things the Target field needs.
struct DiscoveredReceiver {
    std::string name;
    OscTarget target;
};

class OscDiscovery {
public:
    ~OscDiscovery() { stop(); }

    /// Browse with the platform's mDNS backend. False means the platform has
    /// none — not that the network is empty.
    bool start() { return start_with(nullptr); }

    /// Browse with a caller-supplied backend. The tests use this; it is the only
    /// way to assert what discovery does without a live network on the machine.
    bool start_with(std::unique_ptr<events::NetworkServiceDiscovery::Backend> backend) {
        stop();
        auto on_event = [this](events::ServiceDiscoveryAction action,
                               const events::NetworkServiceDiscovery::Service& svc) {
            action == events::ServiceDiscoveryAction::ServiceFound ? add(svc) : remove(svc);
        };
        browser_ = backend ? events::ServiceBrowser::with_backend(
                                 std::move(backend), kOscServiceType, on_event)
                           : std::make_unique<events::ServiceBrowser>(kOscServiceType,
                                                                     on_event);
        return browser_->is_browsing();
    }

    void stop() {
        browser_.reset();
        const std::lock_guard<std::mutex> lock(mutex_);
        found_.clear();
    }

    [[nodiscard]] bool running() const noexcept { return browser_ != nullptr; }

    /// A copy, not a reference. The browse callback runs on whatever thread the
    /// backend chose, and an editor iterating a live vector is a crash waiting
    /// for a receiver to appear mid-paint.
    [[nodiscard]] std::vector<DiscoveredReceiver> receivers() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return found_;
    }

private:
    void add(const events::NetworkServiceDiscovery::Service& svc) {
        // A resolved address beats a hostname: `.local` names only resolve on a
        // machine whose resolver speaks mDNS, and the socket call happens on the
        // sender thread where a failed lookup is a stall.
        const std::string& host = !svc.address.empty() ? svc.address : svc.hostname;
        if (!is_valid_osc_host(host)) return;
        if (svc.port < kMinOscPort || svc.port > kMaxOscPort) return;

        const std::lock_guard<std::mutex> lock(mutex_);
        const auto same = [&](const DiscoveredReceiver& r) { return r.name == svc.name; };
        auto it = std::find_if(found_.begin(), found_.end(), same);
        // Rediscovery is normal — a backend re-announces on every refresh — and a
        // receiver that moved port should replace its old entry, not sit beside it.
        if (it != found_.end())
            it->target = OscTarget{host, svc.port};
        else
            found_.push_back({svc.name, OscTarget{host, svc.port}});
    }

    void remove(const events::NetworkServiceDiscovery::Service& svc) {
        const std::lock_guard<std::mutex> lock(mutex_);
        std::erase_if(found_, [&](const DiscoveredReceiver& r) { return r.name == svc.name; });
    }

    std::unique_ptr<events::ServiceBrowser> browser_;
    mutable std::mutex mutex_;
    std::vector<DiscoveredReceiver> found_;
};

}  // namespace pulp::examples::brew
