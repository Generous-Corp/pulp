#pragma once

#include <pulp/inspect/discovery_publisher.hpp>
#include <pulp/inspect/publication_binding.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

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
    ~InspectorPublication() {
        clear_after_endpoint_stop();
    }

    bool publish(
        InspectorDiscoveryPublisher& publisher,
        InspectorDiscoveryRecord record,
        std::span<const std::uint8_t> token,
        std::chrono::milliseconds heartbeat_interval,
        std::vector<std::shared_ptr<InspectorPublicationBinding>>
            bindings = {}) {
        const auto ttl = publication_ttl_for(heartbeat_interval);
        const auto next_heartbeat = next_heartbeat_after(
            std::chrono::steady_clock::now(), heartbeat_interval);
        if (!ttl || !next_heartbeat)
            return false;
        std::optional<ActivePublication> retired;
        if (!begin_transition(retired, false, false))
            return false;
        retire_hidden(retired);
        if (!publisher.prepare(std::move(record), token, *ttl))
            return finish_failed_transition();
        const auto* prepared_record = publisher.record()
            ? &*publisher.record()
            : nullptr;
        if (!prepared_record) {
            publisher.remove();
            return finish_failed_transition();
        }
        std::string publication_id = prepared_record->publication_id;
        auto binding_lease =
            PublicationBindingLease::acquire(
                std::move(bindings), *prepared_record);
        if (!binding_lease) {
            publisher.remove();
            return finish_failed_transition();
        }
        std::optional<ActivePublication> candidate;
        candidate.emplace(
            publisher, std::move(publication_id),
            std::move(*binding_lease), *ttl,
            heartbeat_interval, *next_heartbeat);
        {
            // Keep publication_id() blocked across the visibility commit:
            // clients can observe either no publication or the exact identity
            // of the committed record, never an empty transitional identity.
            std::lock_guard lock(mutex_);
            if (clear_requested_) {
                candidate->hide_visibility();
            } else {
                active_.emplace(std::move(*candidate));
                candidate.reset();
            }
            if (active_ && publisher.commit()) {
                transition_in_progress_ = false;
                clear_requested_ = false;
                return true;
            }
            if (active_) {
                if (!active_->hide_visibility()) {
                    // Keep the lease paired with discoverable files. The
                    // caller will stop the endpoint, then clear explicitly.
                    transition_in_progress_ = false;
                    clear_requested_ = false;
                    return false;
                }
                candidate.emplace(std::move(*active_));
                active_.reset();
            }
        }
        retire_hidden(candidate);
        return finish_failed_transition();
    }

    /// Returns false when the publication was lost and its endpoint must stop.
    bool refresh_if_due(std::chrono::steady_clock::time_point now) {
        std::lock_guard lock(mutex_);
        if (transition_in_progress_ || !active_ ||
            now < active_->next_heartbeat)
            return true;
        const auto next_heartbeat =
            next_heartbeat_after(now, active_->heartbeat_interval);
        if (next_heartbeat &&
            active_->publisher->refresh(active_->ttl)) {
            active_->next_heartbeat = *next_heartbeat;
            return true;
        }
        // Hide immediately when possible, but retain the publication binding
        // until the caller has stopped the endpoint and invokes
        // clear_after_endpoint_stop(). Authenticated clients must not outlive
        // the authority generation through which they were admitted.
        (void)active_->hide_visibility();
        return false;
    }

    /// Retire after the owning server has stopped accepting connections.
    /// Endpoint shutdown is the fail-safe when discovery files cannot be
    /// removed after ownership loss or an I/O error.
    void clear_after_endpoint_stop() {
        std::optional<ActivePublication> retired;
        if (!begin_transition(retired, true, true))
            return;
        retire_hidden(retired);
        finish_transition();
    }

    std::string publication_id() const {
        std::lock_guard lock(mutex_);
        return active_ ? active_->publication_id : std::string{};
    }

private:
    class PublicationBindingLease {
    public:
        static std::optional<PublicationBindingLease> acquire(
            std::vector<std::shared_ptr<InspectorPublicationBinding>>
                bindings,
            const InspectorDiscoveryRecord& record) {
            PublicationBindingLease aggregate;
            try {
                for (auto& binding : bindings) {
                    if (!binding)
                        return std::nullopt;
                    auto lease = binding->bind_publication(record);
                    if (!lease)
                        return std::nullopt;
                    aggregate.bindings_.push_back({
                        std::move(binding),
                        std::move(lease),
                    });
                }
                return aggregate;
            } catch (...) {
                return std::nullopt;
            }
        }

        PublicationBindingLease(PublicationBindingLease&&) noexcept = default;
        PublicationBindingLease& operator=(
            PublicationBindingLease&&) noexcept = default;

        ~PublicationBindingLease() = default;

        void release() noexcept {
            bindings_.clear();
        }

        PublicationBindingLease(const PublicationBindingLease&) = delete;
        PublicationBindingLease& operator=(
            const PublicationBindingLease&) = delete;

    private:
        struct HeldBinding {
            // Member order makes the lease die before its binding object.
            std::shared_ptr<InspectorPublicationBinding> binding;
            std::unique_ptr<InspectorPublicationLease> lease;
        };

        PublicationBindingLease() = default;
        std::vector<HeldBinding> bindings_;
    };

    struct ActivePublication {
        ActivePublication(
            InspectorDiscoveryPublisher& publisher_in,
            std::string publication_id_in,
            PublicationBindingLease binding_in,
            std::chrono::milliseconds ttl_in,
            std::chrono::milliseconds heartbeat_interval_in,
            std::chrono::steady_clock::time_point next_heartbeat_in) noexcept
            : publisher(&publisher_in),
              publication_id(std::move(publication_id_in)),
              binding(std::move(binding_in)),
              ttl(ttl_in),
              heartbeat_interval(heartbeat_interval_in),
              next_heartbeat(next_heartbeat_in) {}

        ActivePublication(ActivePublication&& other) noexcept
            : publisher(std::exchange(other.publisher, nullptr)),
              publication_id(std::move(other.publication_id)),
              binding(std::move(other.binding)),
              ttl(other.ttl),
              heartbeat_interval(other.heartbeat_interval),
              next_heartbeat(other.next_heartbeat) {}

        ~ActivePublication() = default;

        bool hide_visibility() noexcept {
            if (!publisher)
                return true;
            try {
                return publisher->hide();
            } catch (...) {
                return false;
            }
        }

        void retire_hidden() noexcept {
            if (!publisher)
                return;
            binding.release();
            try {
                publisher->remove();
            } catch (...) {
                // Cleanup paths cannot propagate extension failures.
            }
            publisher = nullptr;
        }

        ActivePublication(const ActivePublication&) = delete;
        ActivePublication& operator=(const ActivePublication&) = delete;
        ActivePublication& operator=(ActivePublication&&) = delete;

        InspectorDiscoveryPublisher* publisher;
        std::string publication_id;
        PublicationBindingLease binding;
        std::chrono::milliseconds ttl;
        std::chrono::milliseconds heartbeat_interval;
        std::chrono::steady_clock::time_point next_heartbeat;
    };

    bool begin_transition(
        std::optional<ActivePublication>& retired,
        bool request_clear_if_busy,
        bool endpoint_stopped) {
        std::lock_guard lock(mutex_);
        if (transition_in_progress_) {
            if (request_clear_if_busy)
                clear_requested_ = true;
            return false;
        }
        transition_in_progress_ = true;
        clear_requested_ = false;
        if (active_) {
            if (!active_->hide_visibility() && !endpoint_stopped) {
                transition_in_progress_ = false;
                return false;
            }
            retired.emplace(std::move(*active_));
            active_.reset();
        }
        return true;
    }

    static void retire_hidden(
        std::optional<ActivePublication>& retired) noexcept {
        if (retired) {
            retired->retire_hidden();
            retired.reset();
        }
    }

    void finish_transition() noexcept {
        std::lock_guard lock(mutex_);
        transition_in_progress_ = false;
        clear_requested_ = false;
    }

    bool finish_failed_transition() noexcept {
        finish_transition();
        return false;
    }

    mutable std::mutex mutex_;
    std::optional<ActivePublication> active_;
    bool transition_in_progress_ = false;
    bool clear_requested_ = false;
};

} // namespace pulp::inspect::detail
