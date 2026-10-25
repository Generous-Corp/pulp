#pragma once

// Shared measurement helpers for the oversampler and half-band suites.

#include <pulp/signal/oversampling.hpp>
#include <pulp/signal/scoped_flush_denormals.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>
#include <vector>

namespace pulp::test::oversampling {

inline constexpr double kPi = std::numbers::pi;

// The settled tail of a render — the part after the filters have filled and the
// measurement is of steady state rather than of the transient.
//
// The projection analyzers take the segment itself rather than a start offset,
// and they fit both quadratures, so the tone's phase at the segment start is
// solved for rather than assumed. Handing them a subspan is therefore exactly
// equivalent to fitting from an absolute sample index: the sin/cos split
// rotates, the amplitude and the residual do not move.
inline std::span<const double> settled_tail(const std::vector<double>& samples, std::size_t begin) {
    return std::span<const double>(samples).subspan(begin);
}

// Rejection of a pure tone injected *inside* the callback, i.e. content the
// oversampled DSP produced rather than content the caller supplied. `frequency`
// is in cycles per BASE sample, so 0.5 is base Nyquist and factor/2 is the
// oversampled Nyquist. Returns how far the surviving output RMS sits below the
// injected tone's RMS.
inline double injected_tone_rejection_db(pulp::signal::Oversampler64::Kind kind,
                                         pulp::signal::Oversampler64::Quality quality,
                                         pulp::signal::Oversampler64::Factor factor,
                                         double frequency) {
    pulp::signal::Oversampler64 oversampler;
    oversampler.set_kind(kind);
    oversampler.set_quality(quality);
    oversampler.set_factor(factor);

    const double oversampled_frequency = frequency / static_cast<double>(factor);
    constexpr std::size_t frames = 8192;
    const std::size_t settled = static_cast<std::size_t>(2 * oversampler.latency_samples() + 512);

    std::size_t callback_frame = 0;
    double energy = 0.0;
    for (std::size_t i = 0; i < frames; ++i) {
        const double output = oversampler.process(0.0, [&](double) {
            const double value =
                std::sin(2.0 * kPi * oversampled_frequency * static_cast<double>(callback_frame));
            ++callback_frame;
            return value;
        });
        if (i >= settled)
            energy += output * output;
    }
    const double mean_square = energy / static_cast<double>(frames - settled);
    // A unit-amplitude sine has mean square 0.5.
    return 10.0 * std::log10(0.5 / (mean_square + 1e-300));
}

// Clears flush-to-zero for the scope, so a test sees the arithmetic a host
// without FTZ (offline renders, x86 defaults) would run.
class FlushToZeroOff {
  public:
    FlushToZeroOff() {
#if defined(PULP_FLUSH_DENORMALS_X86)
        saved_ = _mm_getcsr();
        _mm_setcsr(saved_ & ~pulp::signal::detail::kMxcsrFtz);
#elif defined(PULP_FLUSH_DENORMALS_ARM64)
        saved_ = pulp::signal::detail::read_fpcr();
        pulp::signal::detail::write_fpcr(saved_ & ~pulp::signal::detail::kFpcrFz);
#endif
    }
    ~FlushToZeroOff() {
#if defined(PULP_FLUSH_DENORMALS_X86)
        _mm_setcsr(static_cast<unsigned int>(saved_));
#elif defined(PULP_FLUSH_DENORMALS_ARM64)
        pulp::signal::detail::write_fpcr(saved_);
#endif
    }
    FlushToZeroOff(const FlushToZeroOff&) = delete;
    FlushToZeroOff& operator=(const FlushToZeroOff&) = delete;

  private:
#if defined(PULP_FLUSH_DENORMALS_X86)
    unsigned int saved_ = 0;
#else
    std::uint64_t saved_ = 0;
#endif
};

} // namespace pulp::test::oversampling
