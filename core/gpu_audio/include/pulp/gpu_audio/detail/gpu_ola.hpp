#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace pulp::gpu_audio::detail {

/// Guarded overlap-add carry for one channel of a block-convolution readback.
///
/// Adds this block's full-length (`fft_size`) result into the persistent OLA
/// accumulator `carry`, emits the first `n` samples to `out`, then shifts the
/// carry left by `n` and zero-fills the tail. `src` holds the time-domain
/// result with a `src_stride`-sample stride between successive real samples
/// (stride 2 for an interleaved-complex readback where only the real part is
/// used; stride 1 for a packed real buffer).
///
/// Finiteness guard (MF-4): a GPU readback can contain a NaN/Inf (device hiccup,
/// driver bug, denormal blow-up). Without a guard, `carry += NaN` poisons the
/// accumulator for the REST of the session — every later block of that channel
/// is NaN. So before touching the carry we scan the readback: if ANY sample is
/// non-finite we treat the block as poisoned, RESET the carry to zero, and emit
/// silence for this block. The damage is bounded to the single bad block and the
/// channel self-heals on the next finite readback instead of dying permanently.
///
/// Returns true if the block was finite and accumulated; false if it was
/// poisoned (carry reset, `out` silenced) so a caller may count telemetry.
inline bool overlap_add_block(float* carry, const float* src, std::size_t src_stride,
                              float* out, std::uint32_t fft_size, std::uint32_t n) noexcept {
    // Scan first so a poisoned block never mutates the carry.
    for (std::uint32_t i = 0; i < fft_size; ++i) {
        if (!std::isfinite(src[static_cast<std::size_t>(i) * src_stride])) {
            for (std::uint32_t k = 0; k < fft_size; ++k) carry[k] = 0.0f;
            for (std::uint32_t k = 0; k < n; ++k) out[k] = 0.0f;
            return false;
        }
    }

    for (std::uint32_t i = 0; i < fft_size; ++i)
        carry[i] += src[static_cast<std::size_t>(i) * src_stride];
    for (std::uint32_t i = 0; i < n; ++i) out[i] = carry[i];
    for (std::uint32_t i = 0; i + n < fft_size; ++i) carry[i] = carry[i + n];
    for (std::uint32_t i = fft_size - n; i < fft_size; ++i) carry[i] = 0.0f;
    return true;
}

}  // namespace pulp::gpu_audio::detail
