#pragma once

/// @file mirrored_history_buffer.hpp
/// Fixed-capacity contiguous sample history for single-thread DSP algorithms.

#include <algorithm>
#include <cstddef>
#include <limits>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace pulp::signal {

namespace detail {

template <typename SampleType>
concept MirroredHistorySample =
    std::is_arithmetic_v<SampleType> &&
    std::is_same_v<SampleType, std::remove_cv_t<SampleType>> &&
    !std::is_same_v<SampleType, bool>;

} // namespace detail

/// A single-thread circular history whose complete oldest-to-newest window is
/// always physically contiguous.
///
/// `prepare()` allocates two copies of the requested capacity. Each `push()`
/// writes the new sample into both mirrors and advances one bounded cursor, so
/// its cost does not depend on wrap position. `window()` then returns the last
/// `capacity()` samples ordered oldest to newest without a copy or modulo walk.
/// Unfilled positions and positions restored by `reset()` contain zeroes.
///
/// This is DSP-local history, not a queue and not a synchronization primitive.
/// It is not safe for concurrent producer/consumer access. After `prepare()`,
/// `push()`, `window()`, `reset()`, and the accessors allocate no memory.
template <detail::MirroredHistorySample SampleType>
class MirroredHistoryBuffer {
public:
    MirroredHistoryBuffer() = default;
    MirroredHistoryBuffer(const MirroredHistoryBuffer&) = default;
    MirroredHistoryBuffer& operator=(const MirroredHistoryBuffer&) = default;

    MirroredHistoryBuffer(MirroredHistoryBuffer&& other) noexcept
        : storage_(std::move(other.storage_)),
          capacity_(std::exchange(other.capacity_, 0u)),
          write_pos_(std::exchange(other.write_pos_, 0u)) {}

    MirroredHistoryBuffer& operator=(MirroredHistoryBuffer&& other) noexcept {
        if (this == &other)
            return *this;
        storage_ = std::move(other.storage_);
        capacity_ = std::exchange(other.capacity_, 0u);
        write_pos_ = std::exchange(other.write_pos_, 0u);
        return *this;
    }

    void prepare(std::size_t capacity) {
        if (capacity > std::numeric_limits<std::size_t>::max() / 2u)
            throw std::length_error("MirroredHistoryBuffer capacity is too large");
        std::vector<SampleType> storage(capacity * 2u, SampleType{0});
        storage_.swap(storage);
        capacity_ = capacity;
        write_pos_ = 0u;
    }

    void reset() noexcept {
        std::fill(storage_.begin(), storage_.end(), SampleType{0});
        write_pos_ = 0u;
    }

    void push(SampleType sample) noexcept {
        if (capacity_ == 0u)
            return;
        storage_[write_pos_] = sample;
        storage_[write_pos_ + capacity_] = sample;
        if (++write_pos_ == capacity_)
            write_pos_ = 0u;
    }

    /// The view remains memory-valid until prepare(), but consume it before
    /// the next push() or reset() because those calls change its contents and
    /// may change which physical span represents the ordered history.
    std::span<const SampleType> window() const noexcept {
        if (capacity_ == 0u)
            return {};
        return {storage_.data() + write_pos_, capacity_};
    }

    std::size_t capacity() const noexcept {
        return capacity_;
    }
    bool empty() const noexcept {
        return capacity_ == 0u;
    }

private:
    std::vector<SampleType> storage_;
    std::size_t capacity_ = 0u;
    std::size_t write_pos_ = 0u;
};

} // namespace pulp::signal
