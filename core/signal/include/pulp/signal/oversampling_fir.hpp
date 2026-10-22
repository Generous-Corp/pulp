#pragma once

/// @file oversampling_fir.hpp
/// Linear-phase 2x FIR stage used by the realtime oversampler.

#include <pulp/signal/fir_filter.hpp>
#include <pulp/signal/windowed_sinc_design.hpp>
#include <pulp/simd/simd.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

namespace pulp::signal {

/// Pulp's low-latency linear-phase half-band design. Character-delay
/// hysteresis and the drum output stage share these exact constants so the
/// "house pair" cannot drift into two almost-identical filters.
inline constexpr std::size_t kHouseHalfBandTaps = 65;
inline constexpr double kHouseHalfBandStopbandDb = 81.3;  // Kaiser beta ~= 8
inline constexpr double kHouseHalfBandStageOnePassband = 0.45;
inline constexpr double kHouseHalfBandStageTwoPassband = 0.225;

namespace detail {

// Fixed-storage linear-phase half-band stage for nonlinear processors whose
// configuration and processing paths must both remain allocation-free. The
// general LinearPhaseOversamplingStage2x below supports selectable quality and
// tap counts; this stage deliberately fixes the 65-tap prototype so its state
// can live entirely inline.
//
// Half-band structure: apart from the centre, only odd-offset taps are
// non-zero, and an odd offset always reaches a sample of the opposite time
// parity. The stage therefore keeps one linear history per parity; each
// output is the centre tap on its own-parity history plus one contiguous
// 32-tap pulp::simd::dot over the other parity's history. Histories move
// their newest samples back to the front once every kCapacity writes.
class FixedHalfBandFir65 {
  public:
    FixedHalfBandFir65() noexcept {
        // Force the shared coefficient tables to initialize off the audio path.
        static_cast<void>(coefficients());
        static_cast<void>(odd_taps_oldest_first());
    }

    double process(double input) noexcept {
        auto& own = parity_[phase_];
        auto& other = parity_[phase_ ^ 1u];
        if (own.head == kHistory)
            own.compact();
        own.samples[own.head++] = input;
        // x[n - 32] is 16 own-parity samples back; x[n - 1 - 2j] for
        // j = 0..31 are the other parity's newest 32, oldest first.
        const double centre = coefficients()[kCentre] * own.samples[own.head - 1 - kCentre / 2];
        const double odd = pulp::simd::dot(other.samples.data() + other.head - kOddTaps,
                                           odd_taps_oldest_first().data(), kOddTaps);
        phase_ ^= 1u;
        return centre + odd;
    }

    void reset() noexcept {
        for (auto& p : parity_) {
            p.samples.fill(0.0);
            p.head = kKeep;
        }
        phase_ = 0;
    }

    /// The 65 prototype taps (symmetric; centre index 32; even offsets zero).
    static const std::array<double, 65>& coefficients() noexcept {
        static const std::array<double, kTaps> result = design();
        return result;
    }

  private:
    static constexpr std::size_t kTaps = 65;
    static constexpr std::size_t kCentre = (kTaps - 1) / 2;
    static constexpr std::size_t kOddTaps = (kTaps - 1) / 2;
    // Each parity history must reach back kOddTaps samples (the dot) and
    // kCentre / 2 + 1 samples (the centre tap).
    static constexpr std::size_t kKeep = kOddTaps;
    static constexpr std::size_t kCapacity = 64;
    static constexpr std::size_t kHistory = kKeep + kCapacity;
    static constexpr double kBeta = 8.0;

    static double sinc(double x) noexcept {
        if (std::abs(x) < 1.0e-15)
            return 1.0;
        constexpr double kPi = 3.141592653589793238462643383279502884;
        const double pix = kPi * x;
        return std::sin(pix) / pix;
    }

    static std::array<double, kTaps> design() noexcept {
        std::array<double, kTaps> result{};
        const double denominator = bessel_i0(kBeta);
        double sum = 0.0;
        for (std::size_t tap = 0; tap < kTaps; ++tap) {
            const int offset =
                static_cast<int>(tap) - static_cast<int>(kCentre);
            if (offset != 0 && (offset & 1) == 0) {
                result[tap] = 0.0;
                continue;
            }
            const double position =
                static_cast<double>(offset) / static_cast<double>(kCentre);
            const double window =
                bessel_i0(kBeta * std::sqrt(
                                      std::max(0.0, 1.0 - position * position))) /
                denominator;
            result[tap] =
                0.5 * sinc(0.5 * static_cast<double>(offset)) * window;
            sum += result[tap];
        }
        for (double& coefficient : result)
            coefficient /= sum;
        return result;
    }

    // Odd-offset taps t = 63, 61, ..., 1 (oldest sample first).
    static const std::array<double, kOddTaps>& odd_taps_oldest_first() noexcept {
        static const std::array<double, kOddTaps> result = [] {
            std::array<double, kOddTaps> taps{};
            for (std::size_t j = 0; j < kOddTaps; ++j)
                taps[j] = coefficients()[kTaps - 2 - 2 * j];
            return taps;
        }();
        return result;
    }

    struct ParityHistory {
        // [0, head) holds past same-parity input, oldest first; head >= kKeep.
        std::array<double, kHistory> samples{};
        std::size_t head = kKeep;

        void compact() noexcept {
            std::copy(samples.begin() + static_cast<std::ptrdiff_t>(head - kKeep),
                      samples.begin() + static_cast<std::ptrdiff_t>(head), samples.begin());
            head = kKeep;
        }
    };

    std::array<ParityHistory, 2> parity_{};
    unsigned phase_ = 0;
};

/// The Kaiser-windowed lowpass prototype both linear-phase 2x stage forms
/// split into polyphase branches. Fractions are expressed against the input
/// rate; the filter runs at twice that rate, so base Nyquist is 0.25
/// cycles/output-sample and the transition ends there.
inline std::vector<double> linear_phase_oversampling_prototype(double passband_edge_fraction,
                                                               double stopband_db,
                                                               std::size_t taps) {
    const double passband = 0.5 * passband_edge_fraction;
    constexpr double stopband = 0.25;
    const double transition = stopband - passband;
    const double cutoff = passband + 0.5 * transition;
    return design_windowed_sinc(taps, cutoff, kaiser_beta_for_stopband(stopband_db));
}

template <typename SampleType> class LinearPhaseOversamplingStage2x {
  public:
    void configure(double passband_edge_fraction, double stopband_db, std::size_t taps) {
        auto prototype =
            linear_phase_oversampling_prototype(passband_edge_fraction, stopband_db, taps);

        std::vector<SampleType> up_even;
        std::vector<SampleType> up_odd;
        std::vector<SampleType> down_even;
        std::vector<SampleType> down_odd;
        up_even.reserve((prototype.size() + 1) / 2);
        up_odd.reserve(prototype.size() / 2);
        down_even.reserve((prototype.size() + 1) / 2);
        down_odd.reserve(prototype.size() / 2);
        for (std::size_t i = 0; i < prototype.size(); ++i) {
            const auto coefficient = static_cast<SampleType>(prototype[i]);
            auto& up_phase = (i & 1u) == 0u ? up_even : up_odd;
            auto& down_phase = (i & 1u) == 0u ? down_even : down_odd;
            up_phase.push_back(SampleType{2} * coefficient);
            down_phase.push_back(coefficient);
        }
        up_even_.set_coefficients(std::move(up_even));
        up_odd_.set_coefficients(std::move(up_odd));
        down_even_.set_coefficients(std::move(down_even));
        down_odd_.set_coefficients(std::move(down_odd));
        taps_ = taps;
    }

    void reset() {
        up_even_.reset();
        up_odd_.reset();
        down_even_.reset();
        down_odd_.reset();
        previous_odd_ = SampleType{0};
    }

    void upsample(SampleType input, SampleType& even, SampleType& odd) {
        even = up_even_.process(input);
        odd = up_odd_.process(input);
    }

    SampleType downsample(SampleType even, SampleType odd) {
        const SampleType output = down_even_.process(even) + down_odd_.process(previous_odd_);
        previous_odd_ = odd;
        return output;
    }

    std::size_t taps() const noexcept {
        return taps_;
    }

  private:
    FirFilterT<SampleType> up_even_;
    FirFilterT<SampleType> up_odd_;
    FirFilterT<SampleType> down_even_;
    FirFilterT<SampleType> down_odd_;
    SampleType previous_odd_ = SampleType{0};
    std::size_t taps_ = 0;
};

/// Whether the target guarantees a hardware fused multiply-add at compile
/// time. Without one (x86-64's SSE2 baseline, an MSVC build below /arch:AVX2),
/// std::fma is a library call per tap that also blocks vectorization, which
/// made this loop more than an order of magnitude slower on an x86-64
/// baseline build.
#if defined(__FMA__) || defined(__AVX2__) || defined(__aarch64__) || defined(_M_ARM64) ||          \
    defined(__ARM_FEATURE_FMA)
#define PULP_SIGNAL_HARDWARE_FMA 1
#else
#define PULP_SIGNAL_HARDWARE_FMA 0
#endif

/// acc + a * b for the block FIR: one fused multiply-add where the hardware
/// has it, a separate multiply and add otherwise. Either way the same
/// expression runs for every output, so partition exactness holds; the two
/// forms differ only by rounding, and only between targets.
template <typename SampleType>
inline SampleType fir_multiply_add(SampleType a, SampleType b, SampleType acc) noexcept {
#if PULP_SIGNAL_HARDWARE_FMA
    return std::fma(a, b, acc);
#else
    return acc + a * b;
#endif
}

/// One polyphase branch of a block 2x stage: a direct-form FIR whose every
/// output is the taps summed oldest-first, one multiply-add each (fused where
/// the hardware has it; see fir_multiply_add()).
///
/// That fixed per-output order is the point. A vectorised correlation
/// (vDSP_conv, a tree-reduced dot) regroups the sum differently depending on
/// how many outputs one call produces, so the same signal fed in different
/// block sizes rounds differently. Here sixteen outputs advance together in
/// registers and a ragged tail runs the identical sequence one output at a
/// time, so the result is bit-identical for any partition of the input —
/// including one sample at a time.
template <typename SampleType> class FixedOrderFirBranch {
  public:
    static constexpr std::size_t kBlock = 256;

    void set_coefficients(const std::vector<SampleType>& time_order) {
        reversed_.assign(time_order.rbegin(), time_order.rend());
        history_.assign(reversed_.empty() ? 0 : reversed_.size() - 1 + kBlock, SampleType{0});
        reset();
    }

    void reset() {
        std::fill(history_.begin(), history_.end(), SampleType{0});
    }

    std::size_t taps() const noexcept {
        return reversed_.size();
    }

    /// `n <= kBlock`. `output` must not alias `input`.
    void process(const SampleType* input, SampleType* output, std::size_t n) noexcept {
        const std::size_t taps = reversed_.size();
        const std::size_t keep = taps - 1;
        SampleType* history = history_.data();
        std::copy_n(input, n, history + keep);
        const SampleType* h = reversed_.data();
        constexpr std::size_t kLanes = 16;
        std::size_t i = 0;
        for (; i + kLanes <= n; i += kLanes) {
            SampleType acc[kLanes] = {};
            const SampleType* x = history + i;
            for (std::size_t k = 0; k < taps; ++k) {
                const SampleType c = h[k];
                for (std::size_t lane = 0; lane < kLanes; ++lane)
                    acc[lane] = fir_multiply_add(c, x[k + lane], acc[lane]);
            }
            std::copy_n(acc, kLanes, output + i);
        }
        for (; i < n; ++i) {
            SampleType acc = SampleType{0};
            const SampleType* x = history + i;
            for (std::size_t k = 0; k < taps; ++k)
                acc = fir_multiply_add(h[k], x[k], acc);
            output[i] = acc;
        }
        std::copy(history + n, history + n + keep, history);
    }

  private:
    std::vector<SampleType> reversed_;
    std::vector<SampleType> history_;
};

/// The block form of `LinearPhaseOversamplingStage2x`, for `OversamplerT`:
/// the same prototype and the same polyphase split, run a block at a time.
/// Output is bit-identical for every partition of the input stream (see
/// `FixedOrderFirBranch`), and agrees with the per-sample stage to rounding.
template <typename SampleType> class LinearPhaseOversamplingBlockStage2x {
  public:
    static constexpr std::size_t kBlock = FixedOrderFirBranch<SampleType>::kBlock;

    void configure(double passband_edge_fraction, double stopband_db, std::size_t taps) {
        const auto prototype =
            linear_phase_oversampling_prototype(passband_edge_fraction, stopband_db, taps);
        std::vector<SampleType> up_even, up_odd, down_even, down_odd;
        for (std::size_t i = 0; i < prototype.size(); ++i) {
            const auto coefficient = static_cast<SampleType>(prototype[i]);
            ((i & 1u) == 0u ? up_even : up_odd).push_back(SampleType{2} * coefficient);
            ((i & 1u) == 0u ? down_even : down_odd).push_back(coefficient);
        }
        up_even_.set_coefficients(up_even);
        up_odd_.set_coefficients(up_odd);
        down_even_.set_coefficients(down_even);
        down_odd_.set_coefficients(down_odd);
        taps_ = taps;
        reset();
    }

    void reset() {
        up_even_.reset();
        up_odd_.reset();
        down_even_.reset();
        down_odd_.reset();
        previous_odd_ = SampleType{0};
    }

    std::size_t taps() const noexcept {
        return taps_;
    }

    /// `n` input samples to `2n` interleaved outputs. `output` must not alias
    /// `input`.
    void upsample(const SampleType* input, SampleType* output, std::size_t n) noexcept {
        for (std::size_t done = 0; done < n; done += kBlock) {
            const std::size_t count = std::min(kBlock, n - done);
            up_even_.process(input + done, even_.data(), count);
            up_odd_.process(input + done, odd_.data(), count);
            SampleType* out = output + 2 * done;
            for (std::size_t i = 0; i < count; ++i) {
                out[2 * i] = even_[i];
                out[2 * i + 1] = odd_[i];
            }
        }
    }

    /// `2n` interleaved inputs to `n` outputs, the odd branch one input sample
    /// behind. `output` must not alias `input`.
    void downsample(const SampleType* input, SampleType* output, std::size_t n) noexcept {
        for (std::size_t done = 0; done < n; done += kBlock) {
            const std::size_t count = std::min(kBlock, n - done);
            const SampleType* in = input + 2 * done;
            for (std::size_t i = 0; i < count; ++i) {
                split_even_[i] = in[2 * i];
                split_odd_[i] = i == 0 ? previous_odd_ : in[2 * i - 1];
            }
            previous_odd_ = in[2 * count - 1];
            down_even_.process(split_even_.data(), even_.data(), count);
            down_odd_.process(split_odd_.data(), odd_.data(), count);
            for (std::size_t i = 0; i < count; ++i)
                output[done + i] = even_[i] + odd_[i];
        }
    }

  private:
    FixedOrderFirBranch<SampleType> up_even_;
    FixedOrderFirBranch<SampleType> up_odd_;
    FixedOrderFirBranch<SampleType> down_even_;
    FixedOrderFirBranch<SampleType> down_odd_;
    std::array<SampleType, kBlock> split_even_{};
    std::array<SampleType, kBlock> split_odd_{};
    std::array<SampleType, kBlock> even_{};
    std::array<SampleType, kBlock> odd_{};
    SampleType previous_odd_ = SampleType{0};
    std::size_t taps_ = 0;
};

} // namespace detail

} // namespace pulp::signal
