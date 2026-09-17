#include "shared_io_convolution_executor.hpp"

#include <algorithm>

namespace pulp::gpu_audio::detail {

bool SharedIoConvolutionExecutor::prepare(const Config& config, std::uint64_t epoch,
                                          std::uint64_t first_sequence) {
    if (prepared_ || !config.capacity || !config.channels || !config.block || !config.ir_length ||
        std::uint64_t(config.block) + config.ir_length - 1u > config.fft_size)
        return false;
    c_ = config;
    try {
        entries_.assign(c_.capacity, {});
        terminal_.assign(std::size_t(c_.capacity) * c_.channels * c_.fft_size * 2, 0);
        carry_.assign(std::size_t(c_.channels) * c_.fft_size, 0);
        ready_.assign(std::size_t(c_.capacity) * c_.channels * c_.block, 0);
        ready_state_ = std::make_unique<std::atomic<std::uint64_t>[]>(c_.capacity);
    } catch (...) {
        c_ = {};
        entries_.clear();
        terminal_.clear();
        carry_.clear();
        ready_.clear();
        ready_state_.reset();
        return false;
    }
    prepared_ = true;
    return fence_and_reprime(epoch, first_sequence);
}

bool SharedIoConvolutionExecutor::fence_and_reprime(std::uint64_t epoch,
                                                    std::uint64_t first_sequence) noexcept {
    // The caller has already stopped callback invocation. This store is a
    // fail-closed guard for later entrants, not a join with an in-progress
    // callback that observed the prior false value.
    // Fail closed before touching worker-owned state. A callback that races the
    // reset after the external stop observes the fence and cannot claim a newly
    // cleared slot.
    fenced_.store(true, std::memory_order_release);
    for (std::uint32_t i = 0; i < c_.capacity; ++i) {
        const auto state = ready_state_[i].load(std::memory_order_acquire);
        if (state != kEmpty && (state & kClaimed) != 0)
            return false;
    }
    epoch_ = epoch;
    next_ = first_sequence;
    std::fill(entries_.begin(), entries_.end(), Entry{});
    std::fill(carry_.begin(), carry_.end(), 0);
    for (std::uint32_t i = 0; i < c_.capacity; ++i)
        ready_state_[i].store(kEmpty, std::memory_order_release);
    watermark_set_.store(false, std::memory_order_release);
    watermark_.store(0, std::memory_order_release);
    valid_from_ = first_sequence + (c_.ir_length - 1 + c_.block - 1) / c_.block;
    // New epoch is visible only after every field and output slot is reset.
    fenced_.store(false, std::memory_order_release);
    return true;
}

bool SharedIoConvolutionExecutor::record_terminal(std::uint64_t epoch, std::uint64_t sequence,
                                                  Terminal terminal,
                                                  std::span<const float> time) noexcept {
    if (epoch != epoch_ || fenced_.load(std::memory_order_acquire) || sequence < next_ ||
        sequence >= next_ + c_.capacity)
        return false;
    auto& entry = entries_[sequence % c_.capacity];
    if (entry.present)
        return false;
    entry = {epoch, sequence, terminal, true};
    if (terminal == Terminal::Success) {
        const auto floats = std::size_t(c_.channels) * c_.fft_size * 2;
        if (time.size() != floats)
            entry.terminal = Terminal::Failed;
        else
            std::copy(time.begin(), time.end(),
                      terminal_.begin() + std::size_t(sequence % c_.capacity) * floats);
    }
    return true;
}

std::size_t SharedIoConvolutionExecutor::collect() noexcept {
    std::size_t count = 0;
    while (!fenced_.load(std::memory_order_acquire)) {
        auto& entry = entries_[next_ % c_.capacity];
        if (!entry.present || entry.sequence != next_ || entry.epoch != epoch_)
            break;
        const auto sequence = next_;
        const auto terminal = entry.terminal;
        entry = {};
        ++next_;
        ++count;
        if (terminal != Terminal::Success) {
            fenced_.store(true, std::memory_order_release);
            break;
        }
        const auto slot = sequence % c_.capacity;
        if (ready_state_[slot].load(std::memory_order_acquire) != kEmpty) {
            fenced_.store(true, std::memory_order_release);
            break;
        }
        const auto terminal_base = std::size_t(slot) * c_.channels * c_.fft_size * 2;
        const auto ready_base = std::size_t(slot) * c_.channels * c_.block;
        for (std::uint32_t ch = 0; ch < c_.channels; ++ch) {
            auto* carry = carry_.data() + std::size_t(ch) * c_.fft_size;
            const auto* source =
                terminal_.data() + terminal_base + std::size_t(ch) * c_.fft_size * 2;
            auto* output = ready_.data() + ready_base + std::size_t(ch) * c_.block;
            for (std::uint32_t i = 0; i < c_.fft_size; ++i)
                carry[i] += source[2 * i];
            std::copy_n(carry, c_.block, output);
            std::move(carry + c_.block, carry + c_.fft_size, carry);
            std::fill(carry + c_.fft_size - c_.block, carry + c_.fft_size, 0.f);
        }
        const bool missed = watermark_set_.load(std::memory_order_acquire) &&
                            sequence <= watermark_.load(std::memory_order_acquire);
        if (sequence >= valid_from_ && !missed)
            ready_state_[slot].store(sequence, std::memory_order_release);
    }
    return count;
}

void SharedIoConvolutionExecutor::advance_callback_watermark(std::uint64_t sequence) noexcept {
    watermark_.store(sequence, std::memory_order_release);
    watermark_set_.store(true, std::memory_order_release);
}

std::span<const float> SharedIoConvolutionExecutor::take_ready(std::uint64_t sequence) noexcept {
    if (fenced_.load(std::memory_order_acquire) || sequence < valid_from_ || sequence & kClaimed)
        return {};
    auto& state = ready_state_[sequence % c_.capacity];
    auto expected = sequence;
    if (!state.compare_exchange_strong(expected, sequence | kClaimed, std::memory_order_acq_rel,
                                       std::memory_order_acquire))
        return {};
    return {ready_.data() + std::size_t(sequence % c_.capacity) * c_.channels * c_.block,
            std::size_t(c_.channels) * c_.block};
}

bool SharedIoConvolutionExecutor::release_ready(std::uint64_t sequence) noexcept {
    if (sequence & kClaimed)
        return false;
    auto expected = sequence | kClaimed;
    return ready_state_[sequence % c_.capacity].compare_exchange_strong(
        expected, kEmpty, std::memory_order_release, std::memory_order_acquire);
}

} // namespace pulp::gpu_audio::detail
