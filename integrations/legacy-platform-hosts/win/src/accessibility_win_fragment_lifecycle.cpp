#include "accessibility_win_fragment_lifecycle.hpp"

#ifdef _WIN32

#include "accessibility_win_providers.hpp"
#include <UIAutomation.h>
#include <algorithm>
#include <cassert>
#include <utility>

namespace pulp::view {
namespace {

void disconnect_fragment(PulpFragmentProvider* fragment) {
    if (!fragment) return;
    UiaDisconnectProvider(fragment);
    fragment->Release();
}

} // namespace

UiaFragmentLifecycle::ReadLease::ReadLease(
    UiaFragmentLifecycle& lifecycle)
    : lifecycle_(lifecycle) {
    if (!lifecycle_.available_.load(std::memory_order_acquire)) return;
    lock_ = std::unique_lock<std::mutex>(lifecycle_.reader_mutex_);
    available_ = lifecycle_.available_.load(std::memory_order_relaxed);
}

void UiaFragmentLifecycle::publish(
    std::vector<PulpFragmentProvider*>&& providers,
    int first_child, int last_child) {
    std::lock_guard lock(reader_mutex_);
    assert(active_.empty());
    assert(!available_.load(std::memory_order_relaxed));
    const auto retirement_capacity =
        retired_.size() + draining_.size() + providers.size();
    retired_.reserve(retirement_capacity);
    draining_.reserve(retirement_capacity);
    active_ = std::move(providers);
    first_child_ = first_child;
    last_child_ = last_child;
    available_.store(true, std::memory_order_release);
}

void UiaFragmentLifecycle::retire_active() {
    available_.store(false, std::memory_order_release);
    std::vector<PulpFragmentProvider*> retiring;
    {
        std::lock_guard drain_readers(reader_mutex_);
        retiring.swap(active_);
        first_child_ = -1;
        last_child_ = -1;
    }
    // Close every provider gate before the first outbound COM call. A
    // disconnect can pump the STA and re-enter realm teardown; at that point no
    // later fragment in this batch may still expose its borrowed View/session.
    for (auto*& provider : retiring) {
        if (!provider) continue;
        if (!provider->retire()) {
            retired_.push_back(provider);
            provider = nullptr;
        }
    }
    // Older deferred providers may now disconnect. This must follow the gate
    // pass above because their disconnects are just as reentrant as this
    // batch's and a nested release cannot see the detached active_ vector.
    drain();
    for (auto* provider : retiring) {
        if (provider) disconnect_fragment(provider);
    }
}

void UiaFragmentLifecycle::retire_unpublished(
    std::vector<PulpFragmentProvider*> providers) {
    for (auto*& provider : providers) {
        if (!provider) continue;
        if (!provider->retire()) {
            retired_.push_back(provider);
            provider = nullptr;
        }
    }
    for (auto* provider : providers) {
        if (provider) disconnect_fragment(provider);
    }
}

void UiaFragmentLifecycle::rebuild(UiaSession* session, View* root) {
    const auto generation = ++publication_generation_;
    retire_active();
    if (generation != publication_generation_ || !root) return;

    auto topology = build_uia_fragment_topology(*root);
    std::vector<PulpFragmentProvider*> fragments;
    fragments.reserve(topology.nodes.size());
    // Preallocate rollback storage before constructing a provider. Cleanup of
    // a partial batch must not allocate while propagating construction failure.
    const auto retirement_capacity =
        retired_.size() + draining_.size() + topology.nodes.size();
    retired_.reserve(retirement_capacity);
    draining_.reserve(retirement_capacity);
    try {
        for (auto& node : topology.nodes)
            fragments.push_back(new PulpFragmentProvider(session, node));
    } catch (...) {
        retire_unpublished(std::move(fragments));
        throw;
    }
    if (generation != publication_generation_) {
        retire_unpublished(std::move(fragments));
        return;
    }
    try {
        publish(std::move(fragments), topology.first_child,
                topology.last_child);
    } catch (...) {
        retire_unpublished(std::move(fragments));
        throw;
    }
}

void UiaFragmentLifecycle::release() {
    ++publication_generation_;
    retire_active();
}

void UiaFragmentLifecycle::drain() {
    if (draining_active_) return;
    draining_active_ = true;
    draining_.clear();
    draining_.swap(retired_);
    try {
        for (auto& slot : draining_) {
            auto* provider = slot;
            if (!provider || provider->can_disconnect()) {
                slot = nullptr;
                if (provider) disconnect_fragment(provider);
            } else {
                retired_.push_back(provider);
                slot = nullptr;
            }
        }
    } catch (...) {
        for (auto*& provider : draining_) {
            if (provider)
                retired_.push_back(std::exchange(provider, nullptr));
        }
        draining_.clear();
        draining_active_ = false;
        throw;
    }
    draining_.clear();
    draining_active_ = false;
}

void UiaFragmentLifecycle::abandon_retired() {
    std::vector<PulpFragmentProvider*> abandoned;
    abandoned.swap(retired_);
    for (auto* provider : abandoned) {
        if (provider) provider->Release();
    }
}

void UiaFragmentLifecycle::retain_retired(std::shared_ptr<void> owner) {
    if (!owner) return;
    for (auto* provider : retired_) {
        if (provider) provider->retain_until_idle(owner);
    }
    if (draining_active_) {
        for (auto* provider : draining_) {
            if (provider) provider->retain_until_idle(owner);
        }
    }
}

std::size_t UiaFragmentLifecycle::retired_size() const noexcept {
    auto size = retired_.size();
    if (draining_active_) {
        size += static_cast<std::size_t>(std::count_if(
            draining_.begin(), draining_.end(),
            [](const auto* provider) { return provider != nullptr; }));
    }
    return size;
}

} // namespace pulp::view

#endif
