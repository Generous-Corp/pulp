#pragma once

#include <pulp/inspect/discovery_publisher.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>

namespace pulp::inspect::detail {

inline std::optional<std::chrono::milliseconds> publication_ttl_for(
    std::chrono::milliseconds heartbeat_interval) {
    if (heartbeat_interval <= std::chrono::milliseconds(0) ||
        heartbeat_interval > std::chrono::milliseconds::max() / 3) {
        return std::nullopt;
    }
    return std::max(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::seconds(30)),
        heartbeat_interval * 3);
}

inline std::optional<std::chrono::steady_clock::time_point>
next_heartbeat_after(
    std::chrono::steady_clock::time_point now,
    std::chrono::milliseconds heartbeat_interval) {
    if (heartbeat_interval <= std::chrono::milliseconds(0))
        return std::nullopt;
    const auto maximum_interval = std::chrono::duration_cast<
        std::chrono::milliseconds>(
        std::chrono::steady_clock::duration::max());
    if (heartbeat_interval > maximum_interval)
        return std::nullopt;
    const auto interval = std::chrono::duration_cast<
        std::chrono::steady_clock::duration>(heartbeat_interval);
    if (now >
        std::chrono::steady_clock::time_point::max() - interval) {
        return std::nullopt;
    }
    return now + interval;
}

class InspectorPublication {
public:
    bool publish(
        InspectorDiscoveryPublisher& publisher,
        InspectorDiscoveryRecord record,
        std::span<const std::uint8_t> token,
        std::chrono::milliseconds heartbeat_interval) {
        const auto ttl = publication_ttl_for(heartbeat_interval);
        const auto next_heartbeat = next_heartbeat_after(
            std::chrono::steady_clock::now(), heartbeat_interval);
        if (!ttl || !next_heartbeat)
            return false;
        std::lock_guard lock(mutex_);
        clear_locked();
        if (!publisher.publish(record, token, *ttl))
            return false;
        publisher_ = &publisher;
        ttl_ = *ttl;
        heartbeat_interval_ = heartbeat_interval;
        next_heartbeat_ = *next_heartbeat;
        return true;
    }

    void refresh_if_due(std::chrono::steady_clock::time_point now) {
        std::lock_guard lock(mutex_);
        if (!publisher_ || now < next_heartbeat_)
            return;
        const auto next_heartbeat =
            next_heartbeat_after(now, heartbeat_interval_);
        if (!next_heartbeat || !publisher_->refresh(ttl_)) {
            clear_locked();
            return;
        }
        next_heartbeat_ = *next_heartbeat;
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
