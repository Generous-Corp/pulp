#pragma once

/// @file scoped_flush_denormals.hpp
/// Hardware flush-to-zero RAII guard for the audio callback boundary.
///
/// Denormal (subnormal) floating-point values cause severe CPU stalls when
/// they enter recursive DSP state — IIR/SVF filter memory, reverb and delay
/// feedback, envelope tails. `pulp::signal::snap_to_zero` (denormal.hpp) fixes
/// this per-value where DSP opts in, but it cannot protect code that forgets
/// to call it. `ScopedFlushDenormals` sets the CPU's flush-to-zero mode for the
/// lifetime of the scope so *every* operation flushes denormal results to zero,
/// then restores the caller's previous mode on exit.
///
/// Wrap the audio callback body (alongside `runtime::ScopedNoAlloc`):
/// @code
/// void process(...) {
///     pulp::signal::ScopedFlushDenormals flush_denormals;
///     // ... DSP ...
/// }
/// @endcode
///
/// This is part of Pulp's numeric-mode policy; see
/// docs/guides/dsp-threading.md ("Numeric mode: denormals & determinism").
///
/// **Intended usage:** wrap the audio *callback boundary* — a region whose body
/// is an opaque function call such as `Processor::process()`. Setting the FP
/// mode is not a compiler reordering barrier for FP arithmetic, so do not rely
/// on it around arbitrary *inline* DSP in the same function; the compiler may
/// schedule such arithmetic across the mode change. An opaque call in between
/// (the normal case) prevents that hoisting. For inline recursive math, use
/// `snap_to_zero` (denormal.hpp) instead/as well.
///
/// Platform coverage:
///   * x86-64 (SSE2): sets MXCSR FTZ (flush denormal *results* to zero). FTZ
///     is universal on SSE and cannot fault. DAZ (treat denormal *inputs* as
///     zero) is intentionally NOT set: it is only valid when MXCSR_MASK bit 6
///     reports support (Intel says writing an unsupported MXCSR bit can #GP),
///     so enabling it portably needs an FXSAVE/MXCSR_MASK probe. FTZ alone
///     fixes the dominant recursive-feedback denormal stall; for denormal
///     inputs use `snap_to_zero`. 32-bit x86 uses the no-op fallback.
///   * AArch64 (Apple Silicon, ARM64 Linux/Android) under GCC/Clang: sets
///     FPCR.FZ (bit 24). FZ flushes denormal results, and also inputs in the
///     default FPCR.AH=0 mode; under FPCR.AH=1 it is result-flush only. Treat
///     the AArch64 guard as result-flush, not a guaranteed input-DAZ.
///   * Anything else (e.g. MSVC/ARM64, 32-bit x86): a no-op guard — correct,
///     just without hardware acceleration. DSP should still call
///     `snap_to_zero` in recursive paths regardless of this guard.

#include <cstdint>

#if defined(__x86_64__) || defined(_M_X64)
#define PULP_FLUSH_DENORMALS_X86 1
#include <pmmintrin.h>  // DAZ (SSE3 intrinsics header; also pulls SSE)
#include <xmmintrin.h>  // FTZ + MXCSR access
#elif (defined(__aarch64__) || defined(_M_ARM64)) && defined(__GNUC__)
#define PULP_FLUSH_DENORMALS_ARM64 1
#endif

namespace pulp::signal {
namespace detail {

#if defined(PULP_FLUSH_DENORMALS_X86)
// FTZ (bit 15) only — universally supported on SSE and never faults. DAZ
// (bit 6) is deliberately omitted; see the header doc comment.
inline constexpr unsigned int kMxcsrFtz = 0x8000u;  // flush-to-zero (results)
#elif defined(PULP_FLUSH_DENORMALS_ARM64)
inline constexpr std::uint64_t kFpcrFz = std::uint64_t(1) << 24;  // FPCR.FZ

inline std::uint64_t read_fpcr() noexcept {
    std::uint64_t fpcr = 0;
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(fpcr));
    return fpcr;
}
inline void write_fpcr(std::uint64_t fpcr) noexcept {
    __asm__ __volatile__("msr fpcr, %0" : : "r"(fpcr));
}
#endif

}  // namespace detail

/// True when this build sets the hardware flush-to-zero mode. False on the
/// no-op fallback (e.g. MSVC/ARM64), where DSP must rely on `snap_to_zero`.
#if defined(PULP_FLUSH_DENORMALS_X86) || defined(PULP_FLUSH_DENORMALS_ARM64)
inline constexpr bool kHardwareFlushSupported = true;
#else
inline constexpr bool kHardwareFlushSupported = false;
#endif

/// @return true if the CPU is currently flushing denormals to zero. On the
///         no-op fallback platform this is always false.
inline bool denormals_are_flushed() noexcept {
#if defined(PULP_FLUSH_DENORMALS_X86)
    return (_mm_getcsr() & detail::kMxcsrFtz) != 0;
#elif defined(PULP_FLUSH_DENORMALS_ARM64)
    return (detail::read_fpcr() & detail::kFpcrFz) != 0;
#else
    return false;
#endif
}

class ScopedFlushDenormals {
public:
    ScopedFlushDenormals() noexcept {
#if defined(PULP_FLUSH_DENORMALS_X86)
        saved_csr_ = _mm_getcsr();
        _mm_setcsr(saved_csr_ | detail::kMxcsrFtz);
#elif defined(PULP_FLUSH_DENORMALS_ARM64)
        saved_fpcr_ = detail::read_fpcr();
        detail::write_fpcr(saved_fpcr_ | detail::kFpcrFz);
#endif
    }

    ~ScopedFlushDenormals() noexcept {
#if defined(PULP_FLUSH_DENORMALS_X86)
        _mm_setcsr(saved_csr_);
#elif defined(PULP_FLUSH_DENORMALS_ARM64)
        detail::write_fpcr(saved_fpcr_);
#endif
    }

    ScopedFlushDenormals(const ScopedFlushDenormals&) = delete;
    ScopedFlushDenormals& operator=(const ScopedFlushDenormals&) = delete;
    ScopedFlushDenormals(ScopedFlushDenormals&&) = delete;
    ScopedFlushDenormals& operator=(ScopedFlushDenormals&&) = delete;

private:
#if defined(PULP_FLUSH_DENORMALS_X86)
    unsigned int saved_csr_ = 0;
#elif defined(PULP_FLUSH_DENORMALS_ARM64)
    std::uint64_t saved_fpcr_ = 0;
#endif
};

}  // namespace pulp::signal
