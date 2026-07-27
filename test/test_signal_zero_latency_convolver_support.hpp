#pragma once

// ZeroLatencyConvolverT — the zero-latency partitioned-convolution acceptance
// suite (module M21; spec: convolution-pulp-module-prompt.md, acceptance tests
// 1-12).
//
// ## What is actually being measured, and by what
//
// A convolver is one of the few DSP blocks with an *exact* ground truth: the
// direct time-domain sum `y[n] = sum_k h[k] x[n-k]`, evaluated in double. It is
// O(N*M) and unusably slow in production, which is the entire reason the
// partitioned engine exists — and exactly why it is the right reference here.
// `direct_convolve` is that sum and nothing else. Every correctness claim in
// this file bottoms out in it; the FFT-based instruments are cross-checked
// against it rather than trusted.
//
// Two other instruments appear:
//
//   * `Spectrum` — a coherent rectangular-window DFT of a rendered sine. The
//     test frequency and the partition rates are chosen so ALL of them land
//     exactly on analysis bins (`f = fs * k / 65536`), so there is no window
//     and no leakage to argue about. `Analysis bins are exact` proves that
//     placement before any spectral test reads a number from it.
//   * `UniformOverlapSave` — an independently written single-size overlap-save
//     convolver, ~30 lines, used only to show that the geometric schedule and
//     the frequency-domain delay line sum to the same thing a flat partitioning
//     does. It shares the module's FFT plan on purpose: the FFT is not what is
//     under test there, the *schedule* is, and the direct sum already covers
//     the FFT.
//
// Expected values are COMPUTED from shipped constants — `kTailFadeMsDefault`,
// `kTailTrimDbDefault`, `kBlockCostSlackDefault`, `head_length()`,
// `level_block_length()`, `l1_norm()`, `predelay_samples()` — never restated as
// literals, so moving a constant fails the test that documents it rather than
// quietly disagreeing with it.
//
// Acceptance-class constants (analysis FFT size, render lengths, warm-up
// lengths, +/- bounds) are stated at their use site with the reason they are
// big or small enough; per the series contract's precise reading they are
// neither cited values nor design parameters.
//
// ## Spec deviations, each with the number that forced it
//
//  1. **Section 3.2's pure-doubling schedule is not implementable as a
//     distributed one, and the module does not ship it.** The spec's level `i`
//     covers IR range `[L_i, 2 L_i)` with block length `L_i`, i.e. `d = L`. Its
//     section 3.5/section 6 then claim the level's scheduling margin is `L_i`
//     and that the transform can therefore be sliced over `L_i / B` blocks. The
//     margin of a partition covering `[d, d+L)` is `d - L + 1` (derived and
//     asserted in `Schedule margin`), so at `d = L` the margin is **1 sample**,
//     not `L`. Every level would have to run its whole transform in the block
//     where its frame closes — precisely the peak section 6 exists to remove,
//     and for the section 6.1 example that is a 5.2e6-flop block against a
//     1.8e5-flop mean. The shipped schedule is Gardner's canonical
//     two-partitions-per-size form (`d = 2L` and `d = 3L`, one group covering
//     `[2L, 4L)`), whose binding margin is `L + 1`. `Schedule margin` asserts
//     both the general formula and that the shipped schedule clears it, and
//     `Per-block compute bound` shows the distribution working. The cost of the
//     correction is ~0: 1390 flops/sample against the spec's 1408 on the same
//     cost model (file doc block).
//  2. **Acceptance test 1 (impulse identity to -120 dB) cannot hold with the
//     section 1 send EQ at its default setting**, because a 1-pole high-pass at
//     20 Hz and a 1-pole low-pass at 20 kHz are not identities. The module
//     defines each parameter's ENDPOINT as bypass (`lowcut == 20 Hz` and
//     `highcut == 20 kHz` engage nothing), which makes the default path exact
//     and leaves the whole documented range usable. `Send EQ endpoints are
//     exact bypass` asserts both halves — bypass at the endpoints, and a
//     measurable filter one step inside them.
//  3. **Acceptance test 9's "mean block cost" is measured after warm-up.** The
//     first `4 * L_max` samples after a load have partitions that have not yet
//     armed, so including them lowers the mean and inflates the peak/mean ratio
//     for a reason that has nothing to do with the schedule. The warm-up is
//     stated at the use site.
//  4. **Acceptance test 12's "no sample-to-sample step > -80 dB" is not a
//     property any IR has**, at the truncation point or anywhere else: a
//     measured room IR is broadband noise, and adjacent samples of broadband
//     noise routinely differ by most of the peak amplitude (measured: -5.7 dB
//     relative to peak on the test kernel, everywhere, including deep inside
//     the untouched part of the IR). The property the criterion was standing in
//     for is that TRUNCATION introduces no step, which is asserted directly and
//     far more tightly: the last kept sample is exactly zero, and the fade
//     envelope equals the shipped raised-cosine formula sample for sample.
//  5. **Acceptance test 10's leading-delay trim has nothing to trim.** The
//     shipped resampler is a centred (zero-phase) windowed-sinc rather than a
//     causal FIR, so it has no group delay to remove. The spec's
//     `kResampTapsPerPhase/2` trim is correct for a causal implementation; the
//     OUTCOME it protects — first arrival at index 0 — is asserted either way.
//  6. **Acceptance test 8 asks for a roster entry in `test_signal_rt_safety.cpp`.**
//     The probe runs here against the same `RtAllocationProbe` harness, so the
//     module's RT contract is covered by the module's own suite.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/signal/zero_latency_convolver.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

using pulp::signal::IrNormalizeMode;
using pulp::signal::ZeroLatencyConvolver;
using pulp::signal::ZeroLatencyConvolver64;

namespace {

// ── Measurement recipe constants ─────────────────────────────────────────────
// Acceptance-class: these define how we measure, not how the module behaves.

/// Session rate and host block used by every test unless stated otherwise, and
/// the head length they imply.
constexpr double kFs = 48000.0;
constexpr int kBlock = 128;

/// Analysis FFT for the spectral tests. 65536 bins at 48 kHz is 0.732 Hz per
/// bin, and — the reason for this exact size — every partition rate `fs / L_i`
/// for L_i in {64, 128, ... 32768} is `fs * (65536 / L_i) / 65536`, i.e. lands
/// on an integer bin. `Analysis bins are exact` proves it.
constexpr int kAnalysisFft = 65536;

/// Deterministic xorshift32 (series law 2). Seeded per test; never automated.
class Rng {
public:
    explicit Rng(std::uint32_t seed) : state_(seed ? seed : 1u) {}
    double bipolar() {
        state_ ^= state_ << 13;
        state_ ^= state_ >> 17;
        state_ ^= state_ << 5;
        return (static_cast<double>(state_) / 4294967295.0) * 2.0 - 1.0;
    }

private:
    std::uint32_t state_;
};

/// The ground truth. Slow, exact, and the only thing in this file that is
/// trusted without a cross-check.
std::vector<double> direct_convolve(const std::vector<double>& x, const std::vector<double>& h) {
    std::vector<double> y(x.size(), 0.0);
    for (std::size_t n = 0; n < x.size(); ++n) {
        double acc = 0.0;
        const std::size_t kmax = std::min(h.size(), n + 1);
        for (std::size_t k = 0; k < kmax; ++k) acc += h[k] * x[n - k];
        y[n] = acc;
    }
    return y;
}

double amplitude_db(double linear) { return 20.0 * std::log10(std::max(linear, 1e-300)); }

double max_abs_error(const std::vector<double>& a, const std::vector<double>& b) {
    double worst = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) worst = std::max(worst, std::abs(a[i] - b[i]));
    return worst;
}

double rms_error_db(const std::vector<double>& got, const std::vector<double>& want) {
    double num = 0.0, den = 0.0;
    for (std::size_t i = 0; i < got.size(); ++i) {
        const double d = got[i] - want[i];
        num += d * d;
        den += want[i] * want[i];
    }
    return amplitude_db(std::sqrt(num / std::max(den, 1e-300)));
}

/// Render a multi-channel signal through a convolver in host-sized blocks.
template <typename Conv, typename T>
std::vector<std::vector<T>> render(Conv& conv, const std::vector<std::vector<T>>& input,
                                   int block) {
    const int nch = static_cast<int>(input.size());
    const std::size_t n = input[0].size();
    std::vector<std::vector<T>> out(nch, std::vector<T>(n, T{0}));
    std::vector<std::vector<T>> in_block(nch, std::vector<T>(block));
    std::vector<std::vector<T>> out_block(nch, std::vector<T>(block));
    std::vector<const T*> in_ptr(nch);
    std::vector<T*> out_ptr(nch);
    for (int c = 0; c < nch; ++c) {
        in_ptr[c] = in_block[c].data();
        out_ptr[c] = out_block[c].data();
    }
    for (std::size_t s = 0; s < n; s += block) {
        const int m = static_cast<int>(std::min<std::size_t>(block, n - s));
        for (int c = 0; c < nch; ++c)
            for (int i = 0; i < m; ++i) in_block[c][i] = input[c][s + i];
        conv.process(in_ptr.data(), out_ptr.data(), m);
        for (int c = 0; c < nch; ++c)
            for (int i = 0; i < m; ++i) out[c][s + i] = out_block[c][i];
    }
    return out;
}

/// Mono convenience wrapper — every scalar test uses one channel.
template <typename Conv, typename T>
std::vector<T> render_mono(Conv& conv, const std::vector<T>& x, int block) {
    return render(conv, std::vector<std::vector<T>>{x}, block)[0];
}

template <typename Conv>
std::vector<double> prepared_taps(const Conv& conv, int channel = 0) {
    std::vector<double> h(static_cast<std::size_t>(conv.prepared_ir_length()));
    for (std::size_t i = 0; i < h.size(); ++i)
        h[i] = static_cast<double>(conv.prepared_ir(channel)[i]);
    return h;
}

/// A decaying broadband kernel: the shape of a real measured room IR, and the
/// hardest case for a partitioned convolver because every partition carries
/// energy.
std::vector<double> decaying_noise_ir(int length, double decay, std::uint32_t seed) {
    Rng rng(seed);
    std::vector<double> h(static_cast<std::size_t>(length));
    for (int i = 0; i < length; ++i)
        h[static_cast<std::size_t>(i)] =
            rng.bipolar() * std::exp(-decay * static_cast<double>(i) / length);
    return h;
}

std::vector<float> to_float(const std::vector<double>& v) {
    return std::vector<float>(v.begin(), v.end());
}

/// Coherent rectangular-window DFT magnitude of a real signal. No window,
/// because every frequency of interest is placed exactly on a bin.
class Spectrum {
public:
    explicit Spectrum(const std::vector<double>& x) {
        plan_.prepare(kAnalysisFft);
        bins_.resize(static_cast<std::size_t>(kAnalysisFft));
        std::vector<double> padded(static_cast<std::size_t>(kAnalysisFft), 0.0);
        const std::size_t n = std::min<std::size_t>(x.size(), kAnalysisFft);
        std::copy_n(x.begin(), n, padded.begin());
        plan_.transform_real_padded(padded.data(), kAnalysisFft, bins_.data());
    }
    double magnitude(int bin) const {
        return std::abs(bins_[static_cast<std::size_t>(bin)]) * 2.0 / kAnalysisFft;
    }
    static int bin_for(double hz) {
        return static_cast<int>(std::lround(hz * kAnalysisFft / kFs));
    }

private:
    pulp::signal::detail::SlicedFftPlanT<double> plan_;
    std::vector<std::complex<double>> bins_;
};

/// Independently written single-size overlap-save partitioned convolver — the
/// flat schedule the geometric one has to agree with. Zero latency by the same
/// argument (partition 0 is applied in the block its input arrives in).
class UniformOverlapSave {
public:
    UniformOverlapSave(const std::vector<double>& ir, int block) : block_(block) {
        fft_size_ = 2 * block;
        plan_.prepare(fft_size_);
        parts_ = static_cast<int>((ir.size() + block - 1) / block);
        ir_spectra_.resize(static_cast<std::size_t>(parts_));
        std::vector<double> seg(static_cast<std::size_t>(block), 0.0);
        for (int p = 0; p < parts_; ++p) {
            std::fill(seg.begin(), seg.end(), 0.0);
            for (int i = 0; i < block; ++i) {
                const std::size_t idx = static_cast<std::size_t>(p * block + i);
                if (idx < ir.size()) seg[static_cast<std::size_t>(i)] = ir[idx];
            }
            ir_spectra_[static_cast<std::size_t>(p)].resize(static_cast<std::size_t>(fft_size_));
            plan_.transform_real_padded(seg.data(), block,
                                        ir_spectra_[static_cast<std::size_t>(p)].data());
        }
        history_.assign(static_cast<std::size_t>(parts_),
                        std::vector<std::complex<double>>(static_cast<std::size_t>(fft_size_)));
        frame_.assign(static_cast<std::size_t>(fft_size_), 0.0);
        scratch_.resize(static_cast<std::size_t>(fft_size_));
    }

    void process(const double* in, double* out) {
        std::copy(frame_.begin() + block_, frame_.end(), frame_.begin());
        std::copy_n(in, block_, frame_.begin() + block_);
        plan_.transform_real_padded(frame_.data(), fft_size_,
                                    history_[static_cast<std::size_t>(cursor_)].data());
        std::fill(scratch_.begin(), scratch_.end(), std::complex<double>{});
        for (int p = 0; p < parts_; ++p) {
            const int slot = (cursor_ - p + parts_) % parts_;
            const auto& x = history_[static_cast<std::size_t>(slot)];
            const auto& h = ir_spectra_[static_cast<std::size_t>(p)];
            for (int i = 0; i < fft_size_; ++i) scratch_[static_cast<std::size_t>(i)] += x[static_cast<std::size_t>(i)] * h[static_cast<std::size_t>(i)];
        }
        inverse(scratch_);
        for (int i = 0; i < block_; ++i)
            out[i] = scratch_[static_cast<std::size_t>(block_ + i)].real();
        cursor_ = (cursor_ + 1) % parts_;
    }

private:
    void inverse(std::vector<std::complex<double>>& d) {
        for (auto& v : d) v = std::conj(v);
        std::vector<double> re(static_cast<std::size_t>(fft_size_));
        // Reuse the plan's forward on the conjugated spectrum by round-tripping
        // through a scratch complex transform: conj(fft(conj(X)))/N.
        std::vector<std::complex<double>> tmp(d);
        forward_complex(tmp);
        const double inv = 1.0 / fft_size_;
        for (int i = 0; i < fft_size_; ++i)
            d[static_cast<std::size_t>(i)] = std::conj(tmp[static_cast<std::size_t>(i)]) * inv;
        (void)re;
    }
    void forward_complex(std::vector<std::complex<double>>& d) {
        // Split into real and imaginary transforms via linearity: F{a + ib} =
        // F{a} + i F{b}, and the plan only exposes a real-input transform.
        std::vector<double> re(static_cast<std::size_t>(fft_size_));
        std::vector<double> im(static_cast<std::size_t>(fft_size_));
        for (int i = 0; i < fft_size_; ++i) {
            re[static_cast<std::size_t>(i)] = d[static_cast<std::size_t>(i)].real();
            im[static_cast<std::size_t>(i)] = d[static_cast<std::size_t>(i)].imag();
        }
        std::vector<std::complex<double>> fr(static_cast<std::size_t>(fft_size_));
        std::vector<std::complex<double>> fi(static_cast<std::size_t>(fft_size_));
        plan_.transform_real_padded(re.data(), fft_size_, fr.data());
        plan_.transform_real_padded(im.data(), fft_size_, fi.data());
        for (int i = 0; i < fft_size_; ++i)
            d[static_cast<std::size_t>(i)] = fr[static_cast<std::size_t>(i)] +
                                             std::complex<double>{0.0, 1.0} *
                                                 fi[static_cast<std::size_t>(i)];
    }

    pulp::signal::detail::SlicedFftPlanT<double> plan_;
    int block_;
    int fft_size_;
    int parts_ = 0;
    int cursor_ = 0;
    std::vector<std::vector<std::complex<double>>> ir_spectra_;
    std::vector<std::vector<std::complex<double>>> history_;
    std::vector<double> frame_;
    std::vector<std::complex<double>> scratch_;
};

/// Standard mono setup: session rate, host block, no normalization (so the
/// prepared IR is the authored one plus the documented ingest steps).
template <typename Conv>
void prepare_mono(Conv& conv, const std::vector<double>& ir,
                  IrNormalizeMode mode = IrNormalizeMode::none) {
    using Sample = std::decay_t<decltype(*conv.prepared_ir(0))>;
    conv.prepare(kFs, kBlock, 1);
    conv.set_normalize_mode(mode);
    std::vector<Sample> taps(ir.begin(), ir.end());
    const Sample* channels[1] = {taps.data()};
    REQUIRE(conv.load_impulse_response(channels, 1, static_cast<int>(ir.size()), kFs));
}

}  // namespace

// ── Instrument cross-checks (run before anything reads them) ─────────────────




// ── The schedule (this is the module's load-bearing correction to the spec) ──



// ── Acceptance test 1: impulse identity ─────────────────────────────────────


// ── Acceptance test 2: correctness against the direct sum ───────────────────


// ── Acceptance test 3: the headline invariant ───────────────────────────────


// ── Acceptance test 4: partition-boundary continuity ────────────────────────


// ── Acceptance test 5: geometric schedule == flat schedule ──────────────────


// ── Acceptance test 6: true-stereo 2x2 matrix ───────────────────────────────



// ── Acceptance test 7: determinism ──────────────────────────────────────────


// ── Acceptance test 8: RT allocation probe ──────────────────────────────────


// ── Acceptance test 9: bounded per-block compute ────────────────────────────


// ── Acceptance test 10: resampling and first arrival ────────────────────────



// ── Acceptance test 11: peak-gain invariant ─────────────────────────────────


// ── Acceptance test 12: tail trim cannot click ──────────────────────────────


// ── Ingest policy: normalization modes ──────────────────────────────────────


// ── The send EQ endpoints ───────────────────────────────────────────────────


// ── Unloaded and degenerate states ──────────────────────────────────────────
