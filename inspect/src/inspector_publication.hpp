#pragma once

#include <pulp/inspect/discovery_publisher.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <span>

namespace pulp::inspect::detail {

class InspectorPublication {
public:
    bool publish(
        InspectorDiscoveryPublisher& publisher,
        InspectorDiscoveryRecord record,
        std::span<const std::uint8_t> token,
        std::chrono::milliseconds heartbeat_interval) {
        const auto ttl = std::max(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::seconds(30)),
            heartbeat_interval * 3);
        std::lock_guard lock(mutex_);
        clear_locked();
        if (!publisher.publish(record, token, ttl))
            return false;
        publisher_ = &publisher;
        ttl_ = ttl;
        heartbeat_interval_ = heartbeat_interval;
        next_heartbeat_ =
            std::chrono::steady_clock::now() + heartbeat_interval_;
        return true;
    }

    void refresh_if_due(std::chrono::steady_clock::time_point now) {
        std::lock_guard lock(mutex_);
        if (!publisher_ || now < next_heartbeat_)
            return;
        (void)publisher_->refresh(ttl_);
        next_heartbeat_ = now + heartbeat_interval_;
    }

    void clear() {
        std::lock_guard lock(mutex_);
        clear_locked();
    }

private:
    void clear_locked() {
        if (publisher_)
            publisher_->remove();
        publisher_ = nullptr;
        next_heartbeat_ = {};
    }

    std::mutex mutex_;
    InspectorDiscoveryPublisher* publisher_ = nullptr;
    std::chrono::milliseconds ttl_{};
    std::chrono::milliseconds heartbeat_interval_{};
    std::chrono::steady_clock::time_point next_heartbeat_{};
};

} // namespace pulp::inspect::detail
