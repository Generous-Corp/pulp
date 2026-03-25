#pragma once

#include <cstddef>
#include <optional>
#include <choc/containers/choc_SingleReaderSingleWriterFIFO.h>

namespace pulp::runtime {

// Single-Producer Single-Consumer lock-free queue
// Designed for audio thread → UI thread communication
// Wraps choc::fifo::SingleReaderSingleWriterFIFO
template<typename T, std::size_t Capacity>
class SpscQueue {
    static_assert(Capacity > 0, "Capacity must be > 0");

public:
    SpscQueue() {
        fifo_.reset(Capacity);
    }

    // Producer: push an item. Returns false if full.
    bool try_push(const T& item) {
        return fifo_.push(item);
    }

    bool try_push(T&& item) {
        return fifo_.push(std::move(item));
    }

    // Consumer: pop an item. Returns nullopt if empty.
    std::optional<T> try_pop() {
        T item{};
        if (fifo_.pop(item)) {
            return item;
        }
        return std::nullopt;
    }

    bool empty() const {
        return fifo_.getUsedSlots() == 0;
    }

    std::size_t size_approx() const {
        return fifo_.getUsedSlots();
    }

    static constexpr std::size_t capacity() { return Capacity; }

private:
    choc::fifo::SingleReaderSingleWriterFIFO<T> fifo_;
};

} // namespace pulp::runtime
