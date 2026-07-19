#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <type_traits>
#include <utility>

namespace pulp::audio {

class SampleMemoryGovernorHandle;

class SampleMemoryGovernorEpoch {
public:
    SampleMemoryGovernorEpoch() = default;
    bool valid() const noexcept { return identity_ != nullptr; }
    friend bool operator==(SampleMemoryGovernorEpoch,
                           SampleMemoryGovernorEpoch) noexcept = default;

private:
    friend class SampleMemoryGovernorHandle;
    friend class SampleMemoryLease;

    explicit SampleMemoryGovernorEpoch(const void* identity) noexcept
        : identity_(identity) {}

    const void* identity_ = nullptr;
};

static_assert(std::is_trivially_copyable_v<SampleMemoryGovernorEpoch>);

enum class SampleMemoryCategory : std::uint8_t {
    Preload,
    Page,
};

struct SampleMemoryGovernorStats {
    std::uint64_t capacity_bytes = 0;
    std::uint64_t current_preload_bytes = 0;
    std::uint64_t current_page_bytes = 0;
    std::uint64_t current_total_bytes = 0;
    std::uint64_t peak_preload_bytes = 0;
    std::uint64_t peak_page_bytes = 0;
    std::uint64_t peak_total_bytes = 0;
    std::uint64_t rejected_preload_count = 0;
    std::uint64_t rejected_preload_bytes = 0;
    std::uint64_t rejected_page_count = 0;
    std::uint64_t rejected_page_bytes = 0;
    std::uint64_t invalid_request_count = 0;
};

namespace detail {

struct SampleMemoryGovernorState {
    explicit SampleMemoryGovernorState(std::uint64_t capacity) noexcept
        : stats{.capacity_bytes = capacity} {}

    std::mutex mutex;
    SampleMemoryGovernorStats stats;
    bool accepting_reservations = true;
};

inline void saturating_add(std::uint64_t& target, std::uint64_t value) noexcept {
    target = value > std::numeric_limits<std::uint64_t>::max() - target
                 ? std::numeric_limits<std::uint64_t>::max()
                 : target + value;
}

}  // namespace detail

/// Move-only reservation against one shared sampler-memory cap.
///
/// Leases may outlive the SampleMemoryGovernor facade that issued them. Reset
/// and destruction release the reservation exactly once. All operations are
/// background/control-thread operations; they are not audio-callback safe.
class SampleMemoryLease {
public:
    SampleMemoryLease() = default;
    SampleMemoryLease(const SampleMemoryLease&) = delete;
    SampleMemoryLease& operator=(const SampleMemoryLease&) = delete;

    SampleMemoryLease(SampleMemoryLease&& other) noexcept
        : state_(std::move(other.state_)),
          category_(other.category_),
          bytes_(std::exchange(other.bytes_, 0)) {}

    SampleMemoryLease& operator=(SampleMemoryLease&& other) noexcept {
        if (this == &other) return *this;
        reset();
        state_ = std::move(other.state_);
        category_ = other.category_;
        bytes_ = std::exchange(other.bytes_, 0);
        return *this;
    }

    ~SampleMemoryLease() { reset(); }

    explicit operator bool() const noexcept { return state_ != nullptr && bytes_ != 0; }
    SampleMemoryCategory category() const noexcept { return category_; }
    std::uint64_t bytes() const noexcept { return bytes_; }

    SampleMemoryGovernorEpoch epoch() const noexcept {
        return SampleMemoryGovernorEpoch(state_.get());
    }

    bool issued_by(const SampleMemoryGovernorHandle& governor) const noexcept;

    void reset() noexcept {
        if (!state_ || bytes_ == 0) {
            state_.reset();
            bytes_ = 0;
            return;
        }

        {
            const std::scoped_lock lock(state_->mutex);
            auto& stats = state_->stats;
            auto& category_bytes = category_ == SampleMemoryCategory::Preload
                                       ? stats.current_preload_bytes
                                       : stats.current_page_bytes;
            category_bytes -= std::min(category_bytes, bytes_);
            stats.current_total_bytes -= std::min(stats.current_total_bytes, bytes_);
        }
        state_.reset();
        bytes_ = 0;
    }

private:
    friend class SampleMemoryGovernor;
    friend class SampleMemoryGovernorHandle;

    SampleMemoryLease(std::shared_ptr<detail::SampleMemoryGovernorState> state,
                      SampleMemoryCategory category,
                      std::uint64_t bytes) noexcept
        : state_(std::move(state)), category_(category), bytes_(bytes) {}

    std::shared_ptr<detail::SampleMemoryGovernorState> state_;
    SampleMemoryCategory category_ = SampleMemoryCategory::Preload;
    std::uint64_t bytes_ = 0;
};

enum class SampleMemoryReserveStatus : std::uint8_t {
    Acquired,
    NotPrepared,
    InvalidByteCount,
    InvalidCategory,
    BudgetExceeded,
};

struct SampleMemoryReserveResult {
    SampleMemoryReserveStatus status = SampleMemoryReserveStatus::NotPrepared;
    SampleMemoryLease lease;

    bool acquired() const noexcept {
        return status == SampleMemoryReserveStatus::Acquired &&
               static_cast<bool>(lease);
    }
};

/// Copyable lifetime-safe reservation/stats endpoint. Handles keep the shared
/// accounting state alive without borrowing the SampleMemoryGovernor facade.
class SampleMemoryGovernorHandle {
public:
    SampleMemoryGovernorHandle() = default;

    explicit operator bool() const noexcept { return prepared(); }
    bool prepared() const noexcept {
        if (!state_) return false;
        const std::scoped_lock lock(state_->mutex);
        return state_->accepting_reservations;
    }

    SampleMemoryReserveResult reserve(SampleMemoryCategory category,
                                      std::uint64_t bytes) const noexcept {
        if (!state_) return {SampleMemoryReserveStatus::NotPrepared, {}};

        const std::scoped_lock lock(state_->mutex);
        auto& stats = state_->stats;
        if (!state_->accepting_reservations)
            return {SampleMemoryReserveStatus::NotPrepared, {}};
        if (category != SampleMemoryCategory::Preload &&
            category != SampleMemoryCategory::Page) {
            detail::saturating_add(stats.invalid_request_count, 1);
            return {SampleMemoryReserveStatus::InvalidCategory, {}};
        }
        if (bytes == 0) {
            detail::saturating_add(stats.invalid_request_count, 1);
            return {SampleMemoryReserveStatus::InvalidByteCount, {}};
        }
        if (stats.current_total_bytes > stats.capacity_bytes ||
            bytes > stats.capacity_bytes - stats.current_total_bytes) {
            auto& rejected_count = category == SampleMemoryCategory::Preload
                                       ? stats.rejected_preload_count
                                       : stats.rejected_page_count;
            auto& rejected_bytes = category == SampleMemoryCategory::Preload
                                       ? stats.rejected_preload_bytes
                                       : stats.rejected_page_bytes;
            detail::saturating_add(rejected_count, 1);
            detail::saturating_add(rejected_bytes, bytes);
            return {SampleMemoryReserveStatus::BudgetExceeded, {}};
        }

        auto& category_bytes = category == SampleMemoryCategory::Preload
                                   ? stats.current_preload_bytes
                                   : stats.current_page_bytes;
        auto& category_peak = category == SampleMemoryCategory::Preload
                                  ? stats.peak_preload_bytes
                                  : stats.peak_page_bytes;
        category_bytes += bytes;
        stats.current_total_bytes += bytes;
        category_peak = std::max(category_peak, category_bytes);
        stats.peak_total_bytes = std::max(stats.peak_total_bytes,
                                          stats.current_total_bytes);
        return {
            SampleMemoryReserveStatus::Acquired,
            SampleMemoryLease(state_, category, bytes),
        };
    }

    SampleMemoryGovernorStats stats() const noexcept {
        if (!state_) return {};
        const std::scoped_lock lock(state_->mutex);
        return state_->stats;
    }

    bool issued(const SampleMemoryLease& lease) const noexcept {
        return state_ != nullptr && lease.state_ == state_;
    }

    SampleMemoryGovernorEpoch epoch() const noexcept {
        return SampleMemoryGovernorEpoch(state_.get());
    }

private:
    friend class SampleMemoryGovernor;

    explicit SampleMemoryGovernorHandle(
        std::shared_ptr<detail::SampleMemoryGovernorState> state) noexcept
        : state_(std::move(state)) {}

    std::shared_ptr<detail::SampleMemoryGovernorState> state_;
};

inline bool SampleMemoryLease::issued_by(
    const SampleMemoryGovernorHandle& governor) const noexcept {
    return governor.issued(*this);
}

/// Checked planar sample-storage formula shared by preload and page admission.
/// Returns nullopt for zero dimensions or when the product cannot fit uint64_t.
inline std::optional<std::uint64_t> checked_sample_storage_bytes(
    std::uint32_t channels,
    std::uint64_t frames_per_buffer,
    std::uint32_t buffer_count = 1,
    std::uint32_t bytes_per_sample = sizeof(float)) noexcept {
    if (channels == 0 || frames_per_buffer == 0 || buffer_count == 0 ||
        bytes_per_sample == 0) {
        return std::nullopt;
    }

    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t bytes = channels;
    for (const auto factor : {
             frames_per_buffer,
             static_cast<std::uint64_t>(buffer_count),
             static_cast<std::uint64_t>(bytes_per_sample),
         }) {
        if (factor > maximum / bytes) return std::nullopt;
        bytes *= factor;
    }
    return bytes;
}

/// Shared off-audio-thread admission governor for resident preload heads and
/// stream-cache pages. A reservation succeeds only when the new combined total
/// fits the configured cap; the returned lease owns the accounting lifetime.
class SampleMemoryGovernor {
public:
    SampleMemoryGovernor() = default;
    SampleMemoryGovernor(const SampleMemoryGovernor&) = delete;
    SampleMemoryGovernor& operator=(const SampleMemoryGovernor&) = delete;
    SampleMemoryGovernor(SampleMemoryGovernor&&) noexcept = default;
    SampleMemoryGovernor& operator=(SampleMemoryGovernor&&) noexcept = delete;

    bool prepare(std::uint64_t capacity_bytes) {
        if (capacity_bytes == 0) return false;
        if (state_) {
            const std::scoped_lock lock(state_->mutex);
            if (state_->stats.current_total_bytes != 0) return false;
        }
        try {
            auto replacement =
                std::make_shared<detail::SampleMemoryGovernorState>(capacity_bytes);
            if (state_) {
                const std::scoped_lock lock(state_->mutex);
                if (state_->stats.current_total_bytes != 0) return false;
                state_->accepting_reservations = false;
            }
            state_ = std::move(replacement);
            return true;
        } catch (...) {
            return false;
        }
    }

    /// Drops the facade only when no outstanding leases remain.
    bool release() noexcept {
        if (!state_) return true;
        {
            const std::scoped_lock lock(state_->mutex);
            if (state_->stats.current_total_bytes != 0) return false;
            state_->accepting_reservations = false;
        }
        state_.reset();
        return true;
    }

    bool prepared() const noexcept { return handle().prepared(); }

    SampleMemoryGovernorHandle handle() const noexcept {
        return SampleMemoryGovernorHandle(state_);
    }

    SampleMemoryReserveResult reserve(SampleMemoryCategory category,
                                      std::uint64_t bytes) noexcept {
        return handle().reserve(category, bytes);
    }

    SampleMemoryGovernorStats stats() const noexcept {
        return handle().stats();
    }

private:
    std::shared_ptr<detail::SampleMemoryGovernorState> state_;
};

}  // namespace pulp::audio
