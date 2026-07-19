#pragma once

#include <pulp/runtime/spsc_queue.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <vector>

namespace pulp::audio {

enum class SampleStreamDemandClass : std::uint8_t {
    Attack,
    Sustain,
    Release,
};

struct SampleStreamPageRequest {
    std::uint64_t source_id = 0;
    std::uint64_t source_generation = 0;
    std::uint64_t requester_id = 0;
    std::uint64_t requester_generation = 0;
    std::uint64_t page_index = 0;
    std::uint64_t start_frame = 0;
    std::uint64_t frame_count = 0;
    std::uint64_t resident_source_frames = 0;
    double consumption_frames_per_second = 0.0;
    SampleStreamDemandClass demand_class = SampleStreamDemandClass::Sustain;
    std::uint64_t sequence = 0;
};

enum class SampleStreamScheduleStatus : std::uint8_t {
    Inserted,
    Refreshed,
    Full,
    Invalid,
};

struct SampleStreamSchedulerStats {
    std::size_t pending = 0;
    std::size_t capacity = 0;
    std::uint64_t inserted = 0;
    std::uint64_t refreshed = 0;
    std::uint64_t rejected_full = 0;
    std::uint64_t rejected_invalid = 0;
    std::uint64_t displaced_less_urgent = 0;
    std::uint64_t coalesced = 0;
    std::uint64_t cancelled = 0;
};

template<std::size_t Capacity>
class SampleStreamRequestInbox {
public:
    bool try_push(const SampleStreamPageRequest& request) noexcept {
        return queue_.try_push(request);
    }

    std::optional<SampleStreamPageRequest> try_pop() noexcept {
        return queue_.try_pop();
    }

    runtime::SpscQueueTelemetry telemetry() const noexcept {
        return queue_.telemetry();
    }

    static constexpr std::size_t capacity() noexcept { return Capacity; }

private:
    runtime::SpscQueue<SampleStreamPageRequest, Capacity> queue_;
};

/// Single-owner background scheduler for shared sample-cache page fills.
/// prepare(), submit_or_refresh(), cancellation, and pop_most_urgent() belong to
/// one scheduler thread. Audio code only pushes trivially-owned requests into a
/// prepared SampleStreamRequestInbox.
class SampleStreamScheduler {
public:
    bool prepare(std::size_t capacity) {
        reset();
        if (capacity == 0) return false;
        try {
            pending_.reserve(capacity);
        } catch (...) {
            return false;
        }
        capacity_ = capacity;
        return true;
    }

    void reset() noexcept {
        pending_.clear();
        capacity_ = 0;
        next_sequence_ = 1;
        stats_ = {};
    }

    bool prepared() const noexcept { return capacity_ != 0; }

    SampleStreamScheduleStatus submit_or_refresh(
        SampleStreamPageRequest request) noexcept {
        if (!prepared() || !valid(request)) {
            ++stats_.rejected_invalid;
            return SampleStreamScheduleStatus::Invalid;
        }

        auto existing = std::find_if(pending_.begin(), pending_.end(),
            [&request](const SampleStreamPageRequest& candidate) noexcept {
                return same_demand(candidate, request);
            });
        if (existing != pending_.end()) {
            const auto stable_sequence = existing->sequence;
            *existing = request;
            existing->sequence = stable_sequence;
            ++stats_.refreshed;
            return SampleStreamScheduleStatus::Refreshed;
        }

        if (pending_.size() >= capacity_) {
            auto least = pending_.begin();
            for (auto candidate = std::next(least); candidate != pending_.end(); ++candidate) {
                if (more_urgent(*least, *candidate)) least = candidate;
            }
            if (!strictly_more_urgent(request, *least)) {
                ++stats_.rejected_full;
                return SampleStreamScheduleStatus::Full;
            }
            pending_.erase(least);
            ++stats_.displaced_less_urgent;
        }
        if (next_sequence_ == 0) renumber_sequences();
        request.sequence = next_sequence_++;
        pending_.push_back(request);
        ++stats_.inserted;
        return SampleStreamScheduleStatus::Inserted;
    }

    template<std::size_t Capacity>
    std::size_t drain(SampleStreamRequestInbox<Capacity>& inbox) noexcept {
        std::size_t drained = 0;
        while (auto request = inbox.try_pop()) {
            submit_or_refresh(*request);
            ++drained;
        }
        return drained;
    }

    std::optional<SampleStreamPageRequest> most_urgent() const noexcept {
        return most_urgent_if([](const SampleStreamPageRequest&) noexcept {
            return true;
        });
    }

    template<typename Predicate>
    std::optional<SampleStreamPageRequest> most_urgent_if(
        Predicate&& predicate) const noexcept {
        if (pending_.empty()) return std::nullopt;
        auto best = pending_.end();
        for (auto candidate = pending_.begin(); candidate != pending_.end(); ++candidate) {
            if (!predicate(*candidate)) continue;
            if (best == pending_.end() || more_urgent(*candidate, *best)) best = candidate;
        }
        if (best == pending_.end()) return std::nullopt;
        return *best;
    }

    bool has_page_interest(const SampleStreamPageRequest& request) const noexcept {
        return std::any_of(pending_.begin(), pending_.end(),
            [&request](const SampleStreamPageRequest& candidate) noexcept {
                return same_page(candidate, request);
            });
    }

    std::size_t complete_page(const SampleStreamPageRequest& request) noexcept {
        const auto old_size = pending_.size();
        std::erase_if(pending_, [&request](const SampleStreamPageRequest& candidate) noexcept {
            return same_page(candidate, request);
        });
        const auto removed = old_size - pending_.size();
        if (removed > 1) stats_.coalesced += removed - 1;
        return removed;
    }

    std::optional<SampleStreamPageRequest> pop_most_urgent() noexcept {
        auto request = most_urgent();
        if (!request) return std::nullopt;
        complete_page(*request);
        return request;
    }

    std::size_t cancel_source_generation(std::uint64_t source_id,
                                         std::uint64_t source_generation) noexcept {
        const auto old_size = pending_.size();
        std::erase_if(pending_, [source_id, source_generation](
                                  const SampleStreamPageRequest& request) noexcept {
            return request.source_id == source_id &&
                   request.source_generation == source_generation;
        });
        const auto removed = old_size - pending_.size();
        stats_.cancelled += removed;
        return removed;
    }

    std::size_t cancel_requester(std::uint64_t requester_id,
                                 std::uint64_t requester_generation) noexcept {
        const auto old_size = pending_.size();
        std::erase_if(pending_, [requester_id, requester_generation](
                                  const SampleStreamPageRequest& request) noexcept {
            return request.requester_id == requester_id &&
                   request.requester_generation == requester_generation;
        });
        const auto removed = old_size - pending_.size();
        stats_.cancelled += removed;
        return removed;
    }

    SampleStreamSchedulerStats stats() const noexcept {
        auto result = stats_;
        result.pending = pending_.size();
        result.capacity = capacity_;
        return result;
    }

private:
    static bool valid(const SampleStreamPageRequest& request) noexcept {
        return request.source_id != 0 && request.source_generation != 0 &&
               request.requester_id != 0 && request.requester_generation != 0 &&
               request.frame_count != 0 &&
               request.consumption_frames_per_second > 0.0 &&
               std::isfinite(request.consumption_frames_per_second);
    }

    static bool same_page(const SampleStreamPageRequest& left,
                          const SampleStreamPageRequest& right) noexcept {
        return left.source_id == right.source_id &&
               left.source_generation == right.source_generation &&
               left.page_index == right.page_index;
    }

    static bool same_demand(const SampleStreamPageRequest& left,
                            const SampleStreamPageRequest& right) noexcept {
        return same_page(left, right) &&
               left.requester_id == right.requester_id &&
               left.requester_generation == right.requester_generation;
    }

    static bool more_urgent(const SampleStreamPageRequest& left,
                            const SampleStreamPageRequest& right) noexcept {
        const long double left_scaled =
            static_cast<long double>(left.resident_source_frames) *
            static_cast<long double>(right.consumption_frames_per_second);
        const long double right_scaled =
            static_cast<long double>(right.resident_source_frames) *
            static_cast<long double>(left.consumption_frames_per_second);
        if (left_scaled != right_scaled) return left_scaled < right_scaled;
        if (left.demand_class != right.demand_class)
            return left.demand_class < right.demand_class;
        return left.sequence < right.sequence;
    }

    static bool strictly_more_urgent(const SampleStreamPageRequest& left,
                                     const SampleStreamPageRequest& right) noexcept {
        const long double left_scaled =
            static_cast<long double>(left.resident_source_frames) *
            static_cast<long double>(right.consumption_frames_per_second);
        const long double right_scaled =
            static_cast<long double>(right.resident_source_frames) *
            static_cast<long double>(left.consumption_frames_per_second);
        if (left_scaled != right_scaled) return left_scaled < right_scaled;
        return left.demand_class < right.demand_class;
    }

    void renumber_sequences() noexcept {
        std::sort(pending_.begin(), pending_.end(),
            [](const auto& left, const auto& right) noexcept {
                return left.sequence < right.sequence;
            });
        std::uint64_t sequence = 1;
        for (auto& request : pending_) request.sequence = sequence++;
        next_sequence_ = sequence;
    }

    std::vector<SampleStreamPageRequest> pending_;
    std::size_t capacity_ = 0;
    std::uint64_t next_sequence_ = 1;
    SampleStreamSchedulerStats stats_{};
};

}  // namespace pulp::audio
