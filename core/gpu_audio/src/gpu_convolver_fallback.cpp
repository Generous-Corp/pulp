#include <pulp/gpu_audio/gpu_convolver.hpp>

#include "detail/neural_input_sanitizer.hpp"

#include <algorithm>

namespace pulp::gpu_audio {

// Shared by the render and no-render builds.
void GpuConvolver::prime_fallback(const audio::BufferView<const float>& input,
                                  uint32_t n) noexcept {
    if (!prepared_ || n != block_ || fallback_delay_blocks_ == 0)
        return;
    float* slot_base =
        fb_delay_.data() + static_cast<std::size_t>(fb_delay_idx_) * block_ * channels_;
    for (uint32_t ch = 0; ch < channels_ && ch < fallback_.size(); ++ch) {
        float* slot = slot_base + static_cast<std::size_t>(ch) * block_;
        float* due = fb_out_.data() + static_cast<std::size_t>(ch) * block_;
        // The slot currently holds conv(input(t-L)), this block's due
        // substitute. Stash it, then overwrite with conv(input(t)).
        std::copy_n(slot, block_, due);
        if (ch >= input.num_channels()) {
            std::fill_n(slot, block_, 0.0f);
            continue;
        }
        // The GPU paths' policy: a non-finite sample enters as zero and the
        // history is kept, so the substitute continues the GPU's IR tail. The
        // slot is free once stashed and serves as the sanitized input; the
        // convolver runs in place.
        (void)detail::sanitize_nonfinite(input.channel_ptr(ch), slot, n);
        fallback_[ch].process(slot, slot, static_cast<std::size_t>(n));
    }
    fb_delay_idx_ = (fb_delay_idx_ + 1) % fallback_delay_blocks_;
}

} // namespace pulp::gpu_audio
