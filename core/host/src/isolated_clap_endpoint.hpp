// SPDX-License-Identifier: MIT
#pragma once
#include "isolated_clap_protocol.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <type_traits>
#include <pulp/host/isolated_plugin_slot.hpp>

namespace pulp::host::isolation {

/// The production callback endpoint, also exercised with adversarial peer
/// publications. Caller validates the admitted host buffer shape first.
class CallbackEndpoint {
  public:
    std::uint64_t sequence() const noexcept {
        return sequence_;
    }

    /// The completion is copied out of the shared mapping once, then validated
    /// and emitted from that private copy, so a peer that keeps writing the
    /// mapping cannot change samples between the finiteness check and the copy.
    /// `after_validate` runs between those two steps; it exists so tests can
    /// rewrite the mapping there, and production callers leave it empty.
    template <typename AfterValidate = void (*)() noexcept>
    IsolationReason render(Mapping& mapping, std::uint64_t generation, float* out0, float* out1,
                           const float* in0, const float* in1,
                           AfterValidate after_validate = nullptr) noexcept {
        Slot* completion = sequence_ ? &mapping.completions[(sequence_ - 1) % kSlots] : nullptr;
        // Exactly one acquire at entry; a late result cannot shift the stream.
        const auto entry_state = completion ? completion->ready.load(std::memory_order_acquire) : 0;
        Block request;
        request.generation = generation;
        request.sequence = sequence_;
        // Snapshot before silence/output writes, supporting in-place callers.
        std::copy_n(in0, kFrames, request.audio.data());
        std::copy_n(in1, kFrames, request.audio.data() + kFrames);
        std::fill_n(out0, kFrames, 0.0f);
        std::fill_n(out1, kFrames, 0.0f);
        Block block;
        if (sequence_) {
            if (entry_state == 0)
                return IsolationReason::deadline_missed;
            if (entry_state != 1)
                return IsolationReason::invalid_completion;
            block = completion->block;
            // Compiler barrier: every later read of the completion uses the
            // private copy, never a second load from the shared mapping.
            std::atomic_signal_fence(std::memory_order_seq_cst);
            if (!valid_block(block, generation, sequence_ - 1))
                return block.result == BlockResult::plugin_error
                           ? IsolationReason::plugin_failed
                           : IsolationReason::invalid_completion;
            for (const float sample : block.audio)
                if (!std::isfinite(sample))
                    return IsolationReason::nonfinite_audio;
            if constexpr (!std::is_pointer_v<AfterValidate>)
                after_validate();
            else if (after_validate)
                after_validate();
        }
        auto& next = mapping.requests[sequence_ % kSlots];
        if (next.ready.load(std::memory_order_acquire) != 0)
            return IsolationReason::queue_full;
        if (completion) {
            std::copy_n(block.audio.data(), kFrames, out0);
            std::copy_n(block.audio.data() + kFrames, kFrames, out1);
            completion->ready.store(0, std::memory_order_release);
        }
        next.block = request;
        next.ready.store(1, std::memory_order_release);
        ++sequence_;
        return IsolationReason::none;
    }

  private:
    std::uint64_t sequence_ = 0;
};
} // namespace pulp::host::isolation
