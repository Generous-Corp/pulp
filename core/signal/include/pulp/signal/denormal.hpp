#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace pulp::signal {

/// Denormal-flushing utility.
///
/// Denormal floating-point values cause severe CPU stalls on x86 when
/// they enter recursive paths like IIR filter state or reverb feedback.
/// `snap_to_zero` returns 0 when the input is below a threshold whose
/// magnitude is large enough to catch denormals (and very-quiet tails)
/// but small enough not to be audible. The threshold is 1e-15 (~-300 dB
/// FS for `float`; see `snap_threshold()` below) — the single source of
/// truth for the magnitude quoted here.
///
/// Use at the end of each recursive state update in IIR / SVF / TPT /
/// Ladder / Ballistics / Reverb feedback paths.
///
/// @code
/// last_out_ = pulp::signal::snap_to_zero(coef * last_out_ + input);
/// @endcode
///
/// Build-time toggle: define PULP_DSP_ENABLE_SNAP_TO_ZERO=0 to compile
/// out the check (the function becomes the identity). Default: ON.
#ifndef PULP_DSP_ENABLE_SNAP_TO_ZERO
#define PULP_DSP_ENABLE_SNAP_TO_ZERO 1
#endif

/// Threshold in absolute value below which a sample is treated as zero.
/// 1e-15f corresponds to about -300 dB FS for `float` — well below any
/// audible level and well above the denormal range (~1.4e-45f).
template<typename T>
inline constexpr T snap_threshold() { return T(1e-15); }

/// Returns 0 if |x| < snap_threshold, else x. Branchless on most compilers
/// (compiles to a `cmpltps` + `andps` pair on x86-SSE).
///
/// RT contract: `snap_threshold()`, scalar/buffer `snap_to_zero()`, and
/// `is_denormal()` are allocation-free. `is_denormal()` is intended for
/// diagnostics, not audio hot paths.
template<typename T>
inline T snap_to_zero(T x) {
#if PULP_DSP_ENABLE_SNAP_TO_ZERO
    // One comparison on |x| rather than two joined by &&: the same result for
    // every input (NaN fails it and passes through; -0 becomes +0 as before),
    // and no short-circuit for a compiler to turn into control flow, which
    // keeps loops that snap vectorizable under GCC.
    return std::abs(x) < snap_threshold<T>() ? T(0) : x;
#else
    return x;
#endif
}

/// Snap an entire buffer in place.
template<typename T>
inline void snap_to_zero(T* samples, int n) {
#if PULP_DSP_ENABLE_SNAP_TO_ZERO
    for (int i = 0; i < n; ++i)
        samples[i] = snap_to_zero(samples[i]);
#else
    (void) samples;
    (void) n;
#endif
}

/// Samples a block path runs between denormal snaps of its recursive state.
///
/// Snapping once per chunk instead of after every sample keeps the snap out of
/// the per-sample recursion. It changes nothing above snap_threshold(): a
/// state at or above it at the start of a chunk cannot decay into the float
/// subnormal range within 64 samples unless its pole is inside |z| < 0.44
/// (1e-15 * 0.44^64 is still above FLT_MIN), and a faster-decaying state
/// falls through that range within the chunk. Either way, subnormal residency
/// is bounded by one chunk however long the buffer is. Block paths that also
/// rescan their input for faults use the same chunk as their unit.
inline constexpr std::size_t kSnapChunkSamples = 64;

/// One chunk of a block: samples [begin, end).
struct SnapChunk {
    std::size_t begin;
    std::size_t end;
};

/// The chunks of `[0, num_samples)`, consecutive and at most
/// kSnapChunkSamples long, as a range. The caller snaps its register-resident
/// state at the end of each chunk.
///
/// A range rather than a callback on purpose: a loop body passed as a lambda
/// that captures a pass's state arrays by reference kept that state in memory
/// instead of registers and made an eight-section cascade 1.7x slower; the
/// range-for keeps the body in the caller's own scope.
///
/// @code
/// for (const auto [begin, end] : snap_chunks(n)) {
///     for (std::size_t i = begin; i < end; ++i)
///         s = recursion(s, buffer[i]);
///     s = snap_to_zero(s);
/// }
/// @endcode
class SnapChunks {
  public:
    explicit constexpr SnapChunks(std::size_t num_samples) noexcept : size_(num_samples) {}

    class iterator {
      public:
        constexpr iterator(std::size_t begin, std::size_t size) noexcept
            : begin_(begin), size_(size) {}
        constexpr SnapChunk operator*() const noexcept {
            return {begin_,
                    size_ - begin_ < kSnapChunkSamples ? size_ : begin_ + kSnapChunkSamples};
        }
        constexpr iterator& operator++() noexcept {
            begin_ = size_ - begin_ < kSnapChunkSamples ? size_ : begin_ + kSnapChunkSamples;
            return *this;
        }
        constexpr bool operator!=(const iterator& other) const noexcept {
            return begin_ != other.begin_;
        }

      private:
        std::size_t begin_;
        std::size_t size_;
    };

    constexpr iterator begin() const noexcept {
        return {0, size_};
    }
    constexpr iterator end() const noexcept {
        return {size_, size_};
    }

  private:
    std::size_t size_;
};

constexpr SnapChunks snap_chunks(std::size_t num_samples) noexcept {
    return SnapChunks(num_samples);
}

/// Return true if @p x is a denormal (subnormal) float.
/// Useful for diagnostics; not for hot paths.
template<typename T>
inline bool is_denormal(T x) {
    return x != T(0) && std::fpclassify(x) == FP_SUBNORMAL;
}

} // namespace pulp::signal
