// SPDX-License-Identifier: MIT
#pragma once
#include "isolated_clap_protocol.hpp"
#include <algorithm>
#include <cmath>
#include <pulp/host/isolated_plugin_slot.hpp>

namespace pulp::host::isolation {

/// The production callback endpoint, also exercised with adversarial peer
/// publications. Caller validates the admitted host buffer shape first.
class CallbackEndpoint {
  public:
    std::uint64_t sequence() const noexcept {
        return sequence_;
    }

    IsolationReason render(Mapping& mapping, std::uint64_t generation, float* out0, float* out1,
                           const float* in0, const float* in1) noexcept {
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
        if (sequence_) {
            if (entry_state == 0)
                return IsolationReason::deadline_missed;
            if (entry_state != 1)
                return IsolationReason::invalid_completion;
            const auto& block = completion->block;
            if (!valid_block(block, generation, sequence_ - 1))
                return block.result == BlockResult::plugin_error
                           ? IsolationReason::plugin_failed
                           : IsolationReason::invalid_completion;
            for (const float sample : block.audio)
                if (!std::isfinite(sample))
                    return IsolationReason::nonfinite_audio;
        }
        auto& next = mapping.requests[sequence_ % kSlots];
        if (next.ready.load(std::memory_order_acquire) != 0)
            return IsolationReason::queue_full;
        if (completion) {
            std::copy_n(completion->block.audio.data(), kFrames, out0);
            std::copy_n(completion->block.audio.data() + kFrames, kFrames, out1);
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
