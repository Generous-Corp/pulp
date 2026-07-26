#pragma once

/// @file zero_latency_convolver.hpp
/// Zero-latency non-uniform partitioned convolution (direct FIR head +
/// doubling FFT tail), with the IR ingest policy — resample, DC-block,
/// Schroeder tail trim, normalization — that makes an arbitrary measured
/// impulse response usable at any session rate.
///
/// RT contract: `prepare(sample_rate, max_block, channels)` and
///   `load_impulse_response(...)` MAY allocate (FFT scratch, partition spectra,
///   input-history rings, predelay ring). `process(...)`, `reset()`, and every
///   `set_*` allocate NOTHING and take no locks on the audio thread. All FFT
///   work for a block is bounded by the distributed schedule described below —
///   there is no "if behind, catch up" branch, so a late host block cannot
///   cascade. `latency_samples()` is the literal constant 0.
///
/// ## Why this ships alongside `NonUniformPartitionedConvolverT`, not inside it
///
/// `convolver_non_uniform.hpp` already implements a *two-stage* Gardner
/// convolver: an overlap-save head at the host block size B plus one
/// larger-block tail at K*B (K = 4 by default). It is correct and it is also
/// zero-latency. It is deliberately left alone, because the two blocks answer
/// different questions and merging them would make one of them worse:
///
///   * `NonUniformPartitionedConvolverT` is a *two-stage* engine. Its tail is a
///     single uniform partition size, so a 1.4 s IR at B = 128, K = 4 becomes
///     127 partitions of 512 taps — the per-sample cost grows LINEARLY with IR
///     length past the head. It also runs its whole tail transform inside the
///     one block where its frame closes. For the short IRs it is used with (a
///     few thousand taps) that is the right, simple engine and it should keep
///     its ~280-line shape.
///   * `ZeroLatencyConvolverT` is the *geometric* engine: partition sizes
///     double, so per-sample cost grows as O(log M) in IR length, and every
///     transform larger than the host block is sliced across the samples of its
///     own scheduling margin so the worst block costs about what the mean block
///     costs. It also owns the IR-ingest policy (resampling, normalization,
///     tail trim), true-stereo 2x2 routing, and the send/mix section.
///
/// Folding the geometric schedule into the two-stage class would replace its
/// entire body, and every existing caller would pay the ingest and scheduling
/// machinery it does not use. They share the same published lineage (Gardner
/// 1995) at different altitudes, which is why this header names the other one
/// here rather than duplicating its rationale.
///
/// M22 (speaker/cabinet emulation) is this module's sibling: the *sampled-IR*
/// path is this class, the *physics* path is M22's. M22 should cite
/// `ZeroLatencyConvolverT` + `IrNormalizeMode::peak` for its cabinet-IR loader
/// (see the cookbook at the bottom of this file) and must not restate the
/// partitioning, ingest, or latency argument.
///
/// ## The schedule, and why it is not the pure-doubling one
///
/// Split the convolution by delay: `y[n] = sum_k h[k] x[n-k]` becomes a direct
/// time-domain FIR over `h[0, N0)` plus a set of frequency-domain partitions
/// over disjoint ranges of `h`. A partition of block length `L` covering IR
/// delays `[d, d+L)` runs overlap-save at FFT size `K = 2L`; the frame that
/// closes at input time `T` produces the output samples `[T+d-L+1, T+d]`, so
/// its **scheduling margin** — the time between the frame closing and the first
/// output it owns falling due — is
///
///     margin = d - L + 1                                                  (*)
///
/// Gardner's zero-latency result is that a partition may take up to its margin
/// to compute and still be on time (Gardner 1995). Load balancing needs
/// `margin >= L`, i.e. `d >= 2L - 1`: only then can a length-`L` transform be
/// spread over the `L` samples before it is due.
///
/// The *pure-doubling* schedule (head `[0, N0)`, then one partition of length
/// `L_i` covering `[L_i, 2 L_i)`) sets `d = L`, which by (*) gives a margin of
/// **one sample** at every level — not `L_i`. It is arithmetically correct as a
/// partition of the convolution, but it cannot be sliced: every level must run
/// its whole transform in the block where its frame closes, and the largest
/// level's transform then lands entirely in one audio block. That is exactly
/// the peak this design is supposed to remove.
///
/// The shipped schedule is Gardner's canonical two-partitions-per-size form,
/// stated here in the one shape that makes it uniform:
///
///     a GROUP of block length L holds two partitions of length L and covers
///     the IR range [2L, 4L)
///
/// with `L_g = L_min * 2^g`, `L_min = N0/2`, so group `g` covers
/// `[N0 * 2^g, N0 * 2^(g+1))` and the groups tile `[N0, N0 * 2^P)` exactly.
/// The binding margin is the first partition's, `2L - L + 1 = L + 1` — one full
/// period, which is what makes slicing legal. Both partitions in a group have
/// delays differing by `L`, so they share one forward and one inverse transform
/// through a frequency-domain delay line (the uniform sub-convolver / FDL of
/// Garcia 2002 and Wefers 2015), and the group's per-sample cost is
///
///     10 * log2(2L) + 16   real flops        (2 transforms + 2 spectral MACs)
///
/// against `10 * log2(2L) + 8` for a pure-doubling level of the same size, on
/// the standard cost model where a length-K REAL transform costs
/// `2.5 * K * log2 K`. The shipped schedule needs one more level for the same
/// IR length but each level is half the size, and the two totals land within
/// 2 %: for 48 kHz, B = 128, N0 = 128, M = 65536 taps the shipped schedule
/// costs 1390 flops/sample against 1408 for the unschedulable one. The
/// bounded-peak property is therefore algorithmically free.
///
/// **What this implementation actually costs, and why it is higher.** The
/// resumable transform below is a COMPLEX radix-2 FFT run on real input with a
/// zero imaginary part, which is `5 * K * log2 K` — exactly twice the real-FFT
/// model. The measured cost for the case above is 2512 flops/sample rather than
/// 1390. That 1.8x is a deliberate, documented gap: a packed real transform
/// (half-length complex FFT plus an untangle stage) would recover it, but the
/// untangle has to become two more resumable phases in the cursor, and the
/// property this module is accountable for is the *bounded peak*, not the
/// constant factor. `Per-block compute bound` asserts peak/mean; the absolute
/// figure is read from `last_block_cost()` and stated, not asserted.
/// `Schedule margin` asserts the other half — that every level has room to be
/// sliced at all.
///
/// ## References
///   * W. G. Gardner, "Efficient Convolution without Input-Output Delay",
///     J. Audio Eng. Soc. 43(3):127-136, March 1995 — the zero-latency
///     non-uniform partition scheme and the deadline argument.
///   * T. G. Stockham Jr., "High-Speed Convolution and Correlation",
///     Proc. AFIPS Spring Joint Computer Conf. 28:229-233, 1966 — FFT block
///     convolution.
///   * A. V. Oppenheim & R. W. Schafer, *Discrete-Time Signal Processing*,
///     3rd ed., Prentice Hall, 2009, section 8.7 — overlap-save mechanics.
///   * G. Garcia, "Optimal Filter Partition for Efficient Convolution with
///     Short Input/Output Delay", Proc. 113th AES Convention, 2002 — the
///     multi-partition-per-size family and the shared forward transform.
///   * F. Wefers, *Partitioned Convolution Algorithms for Real-Time
///     Auralization*, PhD dissertation, RWTH Aachen, Logos Verlag, 2015,
///     Ch. 3-5 — FDL and load balancing.
///   * E. Battenberg & R. Avizienis, "Implementing Real-Time Partitioned
///     Convolution Algorithms on Conventional Operating Systems", Proc.
///     DAFx-11, 2011 — single-thread scheduling of partition transforms.
///   * J. W. Cooley & J. W. Tukey, "An Algorithm for the Machine Calculation of
///     Complex Fourier Series", Math. Comp. 19(90):297-301, 1965 — the radix-2
///     transform and its O(K log K) cost model.

// `convolver_non_uniform.hpp` is named above for the reconciliation, but is
// deliberately NOT included: it pulls the convolver message/slot machinery and
// its CHOC FIFO dependency, and nothing here uses its types.
#include <pulp/signal/denormal.hpp>
#include <pulp/signal/scoped_flush_denormals.hpp>
#include <pulp/signal/units.hpp>
#include <pulp/signal/windowed_sinc_design.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace pulp::signal {

/// IR amplitude policy applied at load, after resampling and tail trim.
/// The right choice is data-dependent — a reverb IR wants constant broadband
/// send level, a cabinet IR wants a flat 0 dB peak — so this is a parameter,
/// not a constant.
enum class IrNormalizeMode {
    none,    ///< File passes through untouched (match / linear-phase EQ kernels).
    peak,    ///< max|h| = 1 (cabinet / mic-response IRs).
    energy,  ///< sum h^2 = 1 per output ear (default; convolution reverb).
};

namespace detail {

/// Resumable in-place radix-2 complex FFT (Cooley & Tukey 1965).
///
/// The convolver needs a transform it can *stop in the middle of*: a length-K
/// transform whose deadline is L = K/2 samples away must be spread over those
/// samples, otherwise the block where its frame closes carries the whole cost.
/// A library transform (vDSP, FFTW) is a single opaque call and cannot be
/// sliced, which is the only reason this exists next to `pulp::signal::FftT`.
///
/// Work is metered in *estimated real flops* rather than in elements, so that
/// slicing by budget balances flops rather than loop iterations: a permuted
/// gather and a radix-2 butterfly differ by an order of magnitude in cost and
/// would otherwise produce a very lumpy schedule.
///
/// RT contract: `prepare()` allocates. `Cursor::begin_*()` / `Cursor::advance()`
/// allocate nothing.
template <typename SampleType>
class SlicedFftPlanT {
public:
    // Cost weights, in real flops per work item. A radix-2 butterfly is one
    // complex multiply (6 flops) plus two complex adds (4 flops); a permuted
    // gather or an in-place swap is one load/store pair; the inverse's
    // conjugate and conjugate-scale passes are a negate, and a negate plus two
    // multiplies, respectively.
    static constexpr long kGatherCost = 1;
    static constexpr long kButterflyCost = 10;
    static constexpr long kConjugateCost = 2;
    static constexpr long kScaleCost = 4;
    static constexpr long kMacCost = 8;

    void prepare(int size) {
        size_ = size;
        log2_size_ = 0;
        for (int n = size; n > 1; n >>= 1) ++log2_size_;
        reversed_.resize(static_cast<std::size_t>(size));
        for (int i = 0; i < size; ++i) {
            int r = 0;
            for (int b = 0; b < log2_size_; ++b)
                if (i & (1 << b)) r |= 1 << (log2_size_ - 1 - b);
            reversed_[static_cast<std::size_t>(i)] = r;
        }
        // Twiddles are held in double regardless of SampleType: they are a
        // fixed table, so computing them in double costs nothing at run time
        // while keeping the float transform's error floor set by the
        // accumulation rather than by the table.
        twiddles_.resize(static_cast<std::size_t>(std::max(1, size / 2)));
        constexpr double two_pi = 6.283185307179586476925286766559;
        for (int i = 0; i < size / 2; ++i) {
            const double angle = -two_pi * static_cast<double>(i) / static_cast<double>(size);
            twiddles_[static_cast<std::size_t>(i)] = {std::cos(angle), std::sin(angle)};
        }
    }

    int size() const { return size_; }
    int log2_size() const { return log2_size_; }

    /// Metered cost of one complete forward transform (permuted gather plus all
    /// butterfly stages) and of one complete inverse (both conjugate passes,
    /// the in-place permute, and all butterfly stages). Used to size the
    /// per-sample slicing budget.
    long forward_cost() const {
        return static_cast<long>(size_) * kGatherCost +
               static_cast<long>(log2_size_) * static_cast<long>(size_ / 2) * kButterflyCost;
    }
    long inverse_cost() const {
        return forward_cost() + static_cast<long>(size_) * (kConjugateCost + kScaleCost);
    }

    /// Cursor over one transform. `begin_*()` arms it; `advance()` performs
    /// work until the budget is spent or the transform completes.
    ///
    /// The gather phase reads the source through a power-of-two ring
    /// (`gather_src` / `gather_base` / `gather_mask`) rather than from `data`,
    /// which is what lets the convolver's input history keep advancing while a
    /// transform over an older frame is still in flight — no snapshot copy is
    /// needed, and therefore no unsliceable O(K) memcpy lands in the block
    /// where the frame closes.
    struct Cursor {
        enum class Phase { idle, gather, butterflies, conjugate, permute, scale };

        Phase phase = Phase::idle;
        int cursor = 0;
        int stage = 1;
        bool inverse = false;
        const SampleType* gather_src = nullptr;
        std::size_t gather_base = 0;
        std::size_t gather_mask = 0;

        bool done() const { return phase == Phase::idle; }

        /// Forward transform of `ring[(base + i) & mask]`, i in [0, size).
        void begin_forward_from_ring(const SampleType* ring, std::size_t base, std::size_t mask) {
            phase = Phase::gather;
            cursor = 0;
            stage = 1;
            inverse = false;
            gather_src = ring;
            gather_base = base;
            gather_mask = mask;
        }

        /// Inverse transform of whatever already sits in `data`, computed as
        /// `conj(fft(conj(x)))/N`.
        void begin_inverse_in_place() {
            phase = Phase::conjugate;
            cursor = 0;
            stage = 1;
            inverse = true;
            gather_src = nullptr;
        }

        void cancel() { phase = Phase::idle; }

        long advance(const SlicedFftPlanT& plan, std::complex<SampleType>* data, long budget) {
            long spent = 0;
            const int n = plan.size_;
            while (phase != Phase::idle && spent < budget) {
                switch (phase) {
                case Phase::gather: {
                    const int take = affordable(budget - spent, kGatherCost, n - cursor);
                    if (take <= 0) return spent;
                    for (int i = cursor; i < cursor + take; ++i) {
                        const std::size_t src =
                            (gather_base +
                             static_cast<std::size_t>(plan.reversed_[static_cast<std::size_t>(i)])) &
                            gather_mask;
                        data[i] = {gather_src[src], SampleType{0}};
                    }
                    cursor += take;
                    spent += static_cast<long>(take) * kGatherCost;
                    if (cursor >= n) { phase = Phase::butterflies; cursor = 0; stage = 1; }
                    break;
                }
                case Phase::butterflies: {
                    const int total = n / 2;
                    const int take = affordable(budget - spent, kButterflyCost, total - cursor);
                    if (take <= 0) return spent;
                    const int len = 1 << stage;
                    const int half = len / 2;
                    const int step = n / len;
                    for (int b = cursor; b < cursor + take; ++b) {
                        const int group = b / half;
                        const int j = b - group * half;
                        const int i = group * len;
                        const auto& tw = plan.twiddles_[static_cast<std::size_t>(j * step)];
                        const std::complex<SampleType> w(static_cast<SampleType>(tw.real()),
                                                         static_cast<SampleType>(tw.imag()));
                        const auto u = data[i + j];
                        const auto v = data[i + j + half] * w;
                        data[i + j] = u + v;
                        data[i + j + half] = u - v;
                    }
                    cursor += take;
                    spent += static_cast<long>(take) * kButterflyCost;
                    if (cursor >= total) {
                        cursor = 0;
                        ++stage;
                        if (stage > plan.log2_size_)
                            phase = inverse ? Phase::scale : Phase::idle;
                    }
                    break;
                }
                case Phase::conjugate: {
                    const int take = affordable(budget - spent, kConjugateCost, n - cursor);
                    if (take <= 0) return spent;
                    for (int i = cursor; i < cursor + take; ++i)
                        data[i] = std::conj(data[i]);
                    cursor += take;
                    spent += static_cast<long>(take) * kConjugateCost;
                    if (cursor >= n) { phase = Phase::permute; cursor = 0; }
                    break;
                }
                case Phase::permute: {
                    const int take = affordable(budget - spent, kGatherCost, n - cursor);
                    if (take <= 0) return spent;
                    for (int i = cursor; i < cursor + take; ++i) {
                        const int j = plan.reversed_[static_cast<std::size_t>(i)];
                        if (i < j) std::swap(data[i], data[j]);
                    }
                    cursor += take;
                    spent += static_cast<long>(take) * kGatherCost;
                    if (cursor >= n) { phase = Phase::butterflies; cursor = 0; stage = 1; }
                    break;
                }
                case Phase::scale: {
                    const int take = affordable(budget - spent, kScaleCost, n - cursor);
                    if (take <= 0) return spent;
                    const SampleType inv_n = SampleType{1} / static_cast<SampleType>(n);
                    for (int i = cursor; i < cursor + take; ++i)
                        data[i] = std::conj(data[i]) * inv_n;
                    cursor += take;
                    spent += static_cast<long>(take) * kScaleCost;
                    if (cursor >= n) phase = Phase::idle;
                    break;
                }
                case Phase::idle:
                    return spent;
                }
            }
            return spent;
        }

    private:
        static int affordable(long budget, long unit_cost, int remaining) {
            if (budget <= 0 || remaining <= 0) return 0;
            return static_cast<int>(std::min<long>(budget / unit_cost, remaining));
        }
    };

    /// Blocking forward transform of a contiguous real buffer, zero-padded to
    /// `size()`. Load-time only (building the IR partition spectra).
    void transform_real_padded(const double* src, int src_len,
                               std::complex<SampleType>* dst) const {
        for (int i = 0; i < size_; ++i) {
            const int r = reversed_[static_cast<std::size_t>(i)];
            const double v = r < src_len ? src[r] : 0.0;
            dst[i] = {static_cast<SampleType>(v), SampleType{0}};
        }
        for (int stage = 1; stage <= log2_size_; ++stage) {
            const int len = 1 << stage;
            const int half = len / 2;
            const int step = size_ / len;
            for (int i = 0; i < size_; i += len) {
                for (int j = 0; j < half; ++j) {
                    const auto& tw = twiddles_[static_cast<std::size_t>(j * step)];
                    const std::complex<SampleType> w(static_cast<SampleType>(tw.real()),
                                                     static_cast<SampleType>(tw.imag()));
                    const auto u = dst[i + j];
                    const auto v = dst[i + j + half] * w;
                    dst[i + j] = u + v;
                    dst[i + j + half] = u - v;
                }
            }
        }
    }

private:
    int size_ = 0;
    int log2_size_ = 0;
    std::vector<int> reversed_;
    std::vector<std::complex<double>> twiddles_;
};

}  // namespace detail

/// Zero-latency non-uniform partitioned convolution engine.
///
/// See the file doc block for the schedule and its lineage. Channel policy
/// follows the true-stereo contract: 1 IR channel is applied to both ears,
/// 2 IR channels are dual-mono (no crossfeed), 4 IR channels are the full 2x2
/// matrix in the order [LL, LR, RL, RR] and are only cross-routed when
/// `set_true_stereo(true)` is engaged.
template <typename SampleType = float>
class ZeroLatencyConvolverT {
public:
    // ── Design parameters ────────────────────────────────────────────────────
    // Every value below is ours to tune and is declared with default + range.

    /// Head length N0, in taps. [design parameter, default = host block size,
    /// range 64..512] Tying N0 to the block size is what lets the head be a
    /// plain dot product over samples that are all present in the current
    /// block, which is the zero-latency property (see the file doc block).
    static constexpr int kHeadLenMin = 64;
    static constexpr int kHeadLenMax = 512;

    /// IR trim on the convolved path. [dp, default 0 dB, range -24..+24 dB]
    static constexpr double kIrGainDbDefault = 0.0;
    static constexpr double kIrGainDbMin = -24.0;
    static constexpr double kIrGainDbMax = 24.0;

    /// Pre-IR delay of the wet path. [dp, default 0 ms, range 0..250 ms]
    /// Signal delay, not I/O latency — `latency_samples()` is 0 regardless.
    static constexpr double kPredelayMsDefault = 0.0;
    static constexpr double kPredelayMsMin = 0.0;
    static constexpr double kPredelayMsMax = 250.0;

    /// Wet / dry mix, in percent. [dp, defaults 100 % / 0 %, range 0..100 %]
    static constexpr double kWetPercentDefault = 100.0;
    static constexpr double kDryPercentDefault = 0.0;

    /// Send-EQ corners. [dp, defaults 20 Hz / 20 kHz, ranges 20..500 Hz and
    /// 1 k..20 kHz] Each endpoint is defined as OFF: at `lowcut_hz == 20` the
    /// high-pass is bypassed and at `highcut_hz == 20000` the low-pass is
    /// bypassed, so the default send path is an exact identity. Without that
    /// rule a 1-pole filter sits in the path at the default setting and the
    /// engine cannot reproduce its own impulse response, which the impulse
    /// identity acceptance test requires to floating-point round-off.
    static constexpr double kLowcutHzDefault = 20.0;
    static constexpr double kLowcutHzMin = 20.0;
    static constexpr double kLowcutHzMax = 500.0;
    static constexpr double kHighcutHzDefault = 20000.0;
    static constexpr double kHighcutHzMin = 1000.0;
    static constexpr double kHighcutHzMax = 20000.0;

    /// Mid/side width of the wet output. [dp, default 100 %, range 0..200 %]
    static constexpr double kWidthPercentDefault = 100.0;
    static constexpr double kWidthPercentMin = 0.0;
    static constexpr double kWidthPercentMax = 200.0;

    /// IR truncation floor on the Schroeder backward-energy curve.
    /// [dp, default -72 dB, range -90..-40 dB]
    static constexpr double kTailTrimDbDefault = -72.0;
    static constexpr double kTailTrimDbMin = -90.0;
    static constexpr double kTailTrimDbMax = -40.0;

    /// Raised-cosine fade applied to the last part of the kept IR so the
    /// truncation cannot click. [dp, default 5 ms, range 2..20 ms]
    static constexpr double kTailFadeMsDefault = 5.0;
    static constexpr double kTailFadeMsMin = 2.0;
    static constexpr double kTailFadeMsMax = 20.0;

    /// Resampler kernel width, in taps per polyphase branch.
    /// [dp, default 32, range 16..64] More taps tighten the stopband at
    /// proportionally higher load-time cost; load-time only, never on the
    /// audio thread.
    static constexpr int kResampTapsPerPhaseDefault = 32;
    static constexpr int kResampTapsPerPhaseMin = 16;
    static constexpr int kResampTapsPerPhaseMax = 64;

    /// House Kaiser window shape, fixed by the series contract's
    /// oversampling / resampling law — not a tunable of this module.
    static constexpr double kResampKaiserBeta = 8.0;

    /// Peak-to-mean per-block cost slack the distributed schedule is allowed.
    /// [dp, default 1.5, range 1.2..2.0] Absorbs the fact that a sliced
    /// transform's phases (permuted gather, butterfly stages, conjugate passes)
    /// have different per-item costs, so equal budgets do not produce exactly
    /// equal flop counts.
    static constexpr double kBlockCostSlackDefault = 1.5;
    static constexpr double kBlockCostSlackMin = 1.2;
    static constexpr double kBlockCostSlackMax = 2.0;

    /// Below this, energies are treated as zero. Guards a divide and a log of
    /// an all-silent IR; any value far below the single-precision noise floor
    /// works, range 1e-30..1e-18.
    static constexpr double kSilenceEpsilon = 1e-24;

    // ── Lifecycle ────────────────────────────────────────────────────────────

    /// Size the engine. May allocate. `channels` is the audio channel count
    /// (1 or 2); the IR channel count is independent and arrives with the IR.
    void prepare(double sample_rate, int max_block, int channels) {
        sample_rate_ = (std::isfinite(sample_rate) && sample_rate > 0.0) ? sample_rate
                                                                         : 48000.0;
        max_block_ = std::max(1, max_block);
        channels_ = std::clamp(channels, 1, 2);
        head_len_ = std::clamp(round_up_pow2(max_block_), kHeadLenMin, kHeadLenMax);

        predelay_len_ = static_cast<int>(std::ceil(units::ms_to_samples(
                            kPredelayMsMax, sample_rate_))) + max_block_ + 1;
        predelay_.assign(2, std::vector<SampleType>(static_cast<std::size_t>(predelay_len_),
                                                    SampleType{0}));
        predelay_write_ = 0;
        wet_scratch_.assign(2, std::vector<SampleType>(
                                   static_cast<std::size_t>(std::max(max_block_, kHeadLenMax)),
                                   SampleType{0}));
        hp_state_.assign(2, OnePole{});
        lp_state_.assign(2, OnePole{});
        update_send_eq();
        set_predelay_ms(predelay_ms_);
        is_prepared_ = true;
    }

    /// Ingest an impulse response. Worker / prepare context — MAY allocate.
    ///
    /// Pipeline, in order: channel map, resample to the session rate with
    /// broadband energy preserved by measurement, DC removal, Schroeder tail
    /// trim plus raised-cosine fade, amplitude normalization, then the
    /// partition spectra and the cached L1 norms.
    ///
    /// A channel count other than 1, 2, or 4 is a load error: the previous IR
    /// is kept and `false` is returned.
    bool load_impulse_response(const SampleType* const* ir_channels,
                               int ir_channel_count,
                               int ir_length,
                               double ir_sample_rate) {
        if (!is_prepared_) return false;
        if (ir_channels == nullptr || ir_length <= 0) return false;
        if (ir_channel_count != 1 && ir_channel_count != 2 && ir_channel_count != 4)
            return false;
        if (!std::isfinite(ir_sample_rate) || ir_sample_rate <= 0.0) return false;
        for (int c = 0; c < ir_channel_count; ++c) {
            if (ir_channels[c] == nullptr) return false;
            for (int i = 0; i < ir_length; ++i)
                if (!std::isfinite(static_cast<double>(ir_channels[c][i]))) return false;
        }

        // 1. Channel map into double working buffers. The whole ingest runs in
        //    double so the prepared IR is not shaped by the sample type.
        std::vector<std::vector<double>> work(static_cast<std::size_t>(ir_channel_count));
        for (int c = 0; c < ir_channel_count; ++c) {
            auto& dst = work[static_cast<std::size_t>(c)];
            dst.resize(static_cast<std::size_t>(ir_length));
            for (int i = 0; i < ir_length; ++i)
                dst[static_cast<std::size_t>(i)] = static_cast<double>(ir_channels[c][i]);
        }

        // 2. Resample, preserving broadband energy by measurement.
        if (ir_sample_rate > 0.0 && std::abs(ir_sample_rate - sample_rate_) > 1e-9)
            for (auto& channel : work)
                channel = resample_energy_preserving(channel, ir_sample_rate, sample_rate_);

        // 3. DC removal — measurement DC would otherwise integrate in the wet
        //    sum. Per channel, since the offset is a property of the capture.
        for (auto& channel : work) {
            if (channel.empty()) continue;
            double mean = 0.0;
            for (double v : channel) mean += v;
            mean /= static_cast<double>(channel.size());
            for (double& v : channel) v -= mean;
        }

        // 4. Schroeder tail trim + raised-cosine fade-out.
        trim_tail(work);

        // 5. Amplitude normalization.
        normalize(work, ir_channel_count);

        // 6/7. Publish: taps, L1 norms, partition schedule, spectra.
        publish(work, ir_channel_count);
        return true;
    }

    // ── Parameters (RT-safe; no allocation) ──────────────────────────────────

    void set_ir_gain_db(double db) {
        if (!std::isfinite(db)) return;
        ir_gain_db_ = std::clamp(db, kIrGainDbMin, kIrGainDbMax);
        ir_gain_lin_ = units::db_to_linear(ir_gain_db_);
    }
    void set_predelay_ms(double ms) {
        if (!std::isfinite(ms)) return;
        predelay_ms_ = std::clamp(ms, kPredelayMsMin, kPredelayMsMax);
        const int limit = std::max(0, predelay_len_ - max_block_ - 1);
        predelay_samples_ = std::clamp(
            static_cast<int>(std::lround(units::ms_to_samples(predelay_ms_, sample_rate_))),
            0, limit);
    }
    void set_true_stereo(bool on) {
        true_stereo_ = on;
        refresh_active_cells();
    }
    void set_wet_percent(double percent) {
        if (std::isfinite(percent)) wet_gain_ = std::clamp(percent, 0.0, 100.0) / 100.0;
    }
    void set_dry_percent(double percent) {
        if (std::isfinite(percent)) dry_gain_ = std::clamp(percent, 0.0, 100.0) / 100.0;
    }
    void set_width_percent(double percent) {
        if (!std::isfinite(percent)) return;
        width_ = std::clamp(percent, kWidthPercentMin, kWidthPercentMax) / 100.0;
    }
    void set_lowcut_hz(double hz) {
        if (!std::isfinite(hz)) return;
        lowcut_hz_ = std::clamp(hz, kLowcutHzMin, kLowcutHzMax);
        update_send_eq();
    }
    void set_highcut_hz(double hz) {
        if (!std::isfinite(hz)) return;
        highcut_hz_ = std::clamp(hz, kHighcutHzMin, kHighcutHzMax);
        update_send_eq();
    }

    /// Load-time policy knobs. Changing any of them takes effect on the NEXT
    /// `load_impulse_response()` — by definition they re-read the IR.
    void set_normalize_mode(IrNormalizeMode mode) { normalize_mode_ = mode; }
    void set_tail_trim_db(double db) {
        if (!std::isfinite(db)) return;
        tail_trim_db_ = std::clamp(db, kTailTrimDbMin, kTailTrimDbMax);
    }
    void set_tail_fade_ms(double ms) {
        if (!std::isfinite(ms)) return;
        tail_fade_ms_ = std::clamp(ms, kTailFadeMsMin, kTailFadeMsMax);
    }
    void set_resample_taps_per_phase(int taps) {
        resample_taps_ = std::clamp(taps, kResampTapsPerPhaseMin, kResampTapsPerPhaseMax);
    }

    // ── Audio ────────────────────────────────────────────────────────────────

    /// Convolve `n <= max_block` samples. Allocation-free and lock-free.
    void process(const SampleType* const* in, SampleType* const* out, int n) {
        ScopedFlushDenormals flush_denormals;
        block_cost_ = 0;
        if (n <= 0) return;

        if (!loaded_) {
            // No IR is a wire scaled by the dry control only. Passing the input
            // through the WET path would be a convolution with an implied
            // delta, which is audibly indistinguishable from a working engine
            // and would hide a failed load.
            for (int c = 0; c < channels_; ++c)
                for (int i = 0; i < n; ++i)
                    out[c][i] = static_cast<SampleType>(dry_gain_) * finite_sample(in[c][i]);
            return;
        }

        int done = 0;
        while (done < n) {
            // Every group period is a power-of-two multiple of the smallest, so
            // all scheduling boundaries lie on the L_min grid: clipping the
            // chunk to that grid means a chunk can never straddle a boundary.
            const int to_grid =
                smallest_period_ -
                static_cast<int>(counter_ % static_cast<std::uint64_t>(smallest_period_));
            const int chunk = std::min(n - done, to_grid);

            render_chunk(in, done, chunk);
            mix_chunk(in, out, done, chunk);

            counter_ += static_cast<std::uint64_t>(chunk);
            advance_groups();
            done += chunk;
        }
    }

    /// Drop all convolution history. Allocation-free.
    void reset() {
        for (auto& ring : history_) std::fill(ring.begin(), ring.end(), SampleType{0});
        history_write_ = 0;
        counter_ = 0;
        for (auto& g : groups_) g.reset();
        for (auto& ring : predelay_) std::fill(ring.begin(), ring.end(), SampleType{0});
        predelay_write_ = 0;
        for (auto& s : hp_state_) s = OnePole{};
        for (auto& s : lp_state_) s = OnePole{};
        block_cost_ = 0;
    }

    /// The headline invariant. Always 0 — the head is a direct FIR over samples
    /// that are all present in the current block, and every FFT partition's
    /// output is due at least its own block length after its frame closes.
    int latency_samples() const { return 0; }

    /// `ir_gain_linear * ||h||_1`, the exact l-infinity operator norm of the
    /// wet path: `|y[n]| <= ||h||_1 * max|x|`, with equality approached by the
    /// sign-matched input `x[n] = sign(h[N-1-n])`. For a multi-channel IR this
    /// is the worst ear, i.e. the largest sum of `||h||_1` over the cells
    /// feeding one output channel under the current routing. Measured at load,
    /// never estimated.
    ///
    /// This is the WET-path bound the registry cites. The bound on the mixed
    /// output additionally includes the dry sum:
    /// `dry_gain + wet_gain * worst_case_gain()`.
    double worst_case_gain() const { return ir_gain_lin_ * l1_norm(); }

    /// `||h||_1` under the current channel routing, without the IR trim.
    double l1_norm() const {
        if (!loaded_) return 0.0;
        double worst = 0.0;
        for (int dst = 0; dst < 2; ++dst) {
            double sum = 0.0;
            for (int cell = 0; cell < cell_count_; ++cell) {
                const Cell& k = cells_[static_cast<std::size_t>(cell)];
                if (cell_active(cell) && k.dst == dst) sum += k.l1;
            }
            worst = std::max(worst, sum);
        }
        return worst;
    }

    // ── Introspection (tests and the catalog node read these) ────────────────

    bool is_loaded() const { return loaded_; }
    int head_length() const { return head_len_; }
    int num_levels() const { return static_cast<int>(groups_.size()); }
    int level_block_length(int level) const {
        return groups_[static_cast<std::size_t>(level)].block_len;
    }
    int level_ir_start(int level) const {
        return groups_[static_cast<std::size_t>(level)].ir_start;
    }
    int level_partitions(int level) const {
        return groups_[static_cast<std::size_t>(level)].num_partitions;
    }
    /// Scheduling margin of level `i`, in samples: the time between its frame
    /// closing and the first output it owns falling due, `d - L + 1`. Load
    /// balancing requires this to be at least the level's own block length.
    int level_margin(int level) const {
        const auto& g = groups_[static_cast<std::size_t>(level)];
        return g.ir_start - g.block_len + 1;
    }
    int prepared_ir_length() const { return prepared_len_; }
    int prepared_ir_channels() const { return ir_channels_; }
    const SampleType* prepared_ir(int channel) const {
        return prepared_[static_cast<std::size_t>(channel)].data();
    }
    /// Estimated real flops performed by the most recent `process()` call.
    /// Always-on integer accounting; the per-block compute bound test reads it.
    long long last_block_cost() const { return block_cost_; }

    double sample_rate() const { return sample_rate_; }
    IrNormalizeMode normalize_mode() const { return normalize_mode_; }
    double tail_trim_db() const { return tail_trim_db_; }
    double tail_fade_ms() const { return tail_fade_ms_; }
    double predelay_ms() const { return predelay_ms_; }
    int predelay_samples() const { return predelay_samples_; }

private:
    static SampleType finite_sample(SampleType sample) noexcept {
        return std::isfinite(static_cast<double>(sample)) ? sample : SampleType{0};
    }

    using FftCursor = typename detail::SlicedFftPlanT<SampleType>::Cursor;

    // ── Partition group: two length-L partitions covering IR [2L, 4L) ────────
    struct Group {
        int block_len = 0;       // L
        int fft_size = 0;        // K = 2L
        int ir_start = 0;        // d of the first partition = 2L
        int num_partitions = 0;  // 1 or 2
        int slot = 0;            // FDL slot the in-flight frame writes to

        detail::SlicedFftPlanT<SampleType> plan;

        // Per audio channel: FDL of forward spectra, depth num_partitions.
        std::vector<std::vector<std::complex<SampleType>>> spectra;     // [ch*np + p][K]
        // Per cell: IR partition spectra, accumulator, ping-pong output.
        std::vector<std::vector<std::complex<SampleType>>> ir_spectra;  // [cell*np + p][K]
        std::vector<std::vector<std::complex<SampleType>>> accum;       // [cell][K]
        std::vector<std::vector<SampleType>> emit;                      // [cell][L]
        std::vector<std::vector<SampleType>> ready;                     // [cell][L]

        // Sliced-execution state for the period currently in flight.
        long total_cost = 0;
        long granted = 0;
        std::size_t frame_base = 0;  // ring index of the closed frame's first sample
        int job = 0;
        int job_step = 0;
        int job_cursor = 0;
        FftCursor fft;
        bool armed = false;
        bool has_result = false;

        void reset() {
            for (auto& s : spectra) std::fill(s.begin(), s.end(), std::complex<SampleType>{});
            for (auto& a : accum) std::fill(a.begin(), a.end(), std::complex<SampleType>{});
            for (auto& e : emit) std::fill(e.begin(), e.end(), SampleType{0});
            for (auto& r : ready) std::fill(r.begin(), r.end(), SampleType{0});
            slot = 0;
            granted = 0;
            frame_base = 0;
            job = 0;
            job_step = 0;
            job_cursor = 0;
            fft.cancel();
            armed = false;
            has_result = false;
        }
    };

    struct Cell {
        int src = 0;
        int dst = 0;
        int ir_channel = 0;
        double l1 = 0.0;
        std::vector<SampleType> head;  // N0 direct-FIR taps
    };

    struct OnePole {
        double z = 0.0;
        double prev_x = 0.0;
    };

    // ── Chunk rendering ──────────────────────────────────────────────────────

    void render_chunk(const SampleType* const* in, int offset, int chunk) {
        const std::size_t mask = history_mask_;

        // Send EQ into the input-history rings. The head and every partition
        // read the same rings, so the EQ is applied exactly once.
        for (int c = 0; c < channels_; ++c) {
            SampleType* ring = history_[static_cast<std::size_t>(c)].data();
            auto& hp = hp_state_[static_cast<std::size_t>(c)];
            auto& lp = lp_state_[static_cast<std::size_t>(c)];
            for (int i = 0; i < chunk; ++i) {
                double x = static_cast<double>(finite_sample(in[c][offset + i]));
                if (hp_active_) {
                    hp.z = snap_to_zero(hp_coef_ * (hp.z + x - hp.prev_x));
                    hp.prev_x = x;
                    x = hp.z;
                }
                if (lp_active_) {
                    lp.z = snap_to_zero(lp.z + lp_coef_ * (x - lp.z));
                    x = lp.z;
                }
                ring[(history_write_ + static_cast<std::size_t>(i)) & mask] =
                    static_cast<SampleType>(x);
            }
        }
        history_write_ = (history_write_ + static_cast<std::size_t>(chunk)) & mask;

        for (int c = 0; c < 2; ++c)
            std::fill_n(wet_scratch_[static_cast<std::size_t>(c)].begin(), chunk, SampleType{0});

        // Direct-form head: the whole zero-latency claim lives in this loop.
        // Every tap reads a sample at or before the current one, so the current
        // block's near-field is complete the moment the block arrives.
        const std::size_t head_base = (history_write_ - static_cast<std::size_t>(chunk)) & mask;
        for (int cell = 0; cell < cell_count_; ++cell) {
            if (!cell_active(cell)) continue;
            const Cell& k = cells_[static_cast<std::size_t>(cell)];
            const SampleType* ring = history_[static_cast<std::size_t>(k.src)].data();
            SampleType* wet = wet_scratch_[static_cast<std::size_t>(k.dst)].data();
            const SampleType* taps = k.head.data();
            for (int i = 0; i < chunk; ++i) {
                const std::size_t n_index = head_base + static_cast<std::size_t>(i);
                SampleType acc{0};
                for (int t = 0; t < head_len_; ++t)
                    acc += taps[t] * ring[(n_index - static_cast<std::size_t>(t)) & mask];
                wet[i] += acc;
            }
        }
        block_cost_ += static_cast<long long>(active_cells_) * head_len_ * chunk * 2;

        // FFT partitions: each group emits the block of output it finished one
        // period ago. `phase` is the position inside the current period, and by
        // construction the chunk cannot straddle a period boundary.
        for (auto& g : groups_) {
            const int phase =
                static_cast<int>(counter_ % static_cast<std::uint64_t>(g.block_len));
            for (int cell = 0; cell < cell_count_; ++cell) {
                if (!cell_active(cell)) continue;
                const Cell& k = cells_[static_cast<std::size_t>(cell)];
                const SampleType* src = g.emit[static_cast<std::size_t>(cell)].data() + phase;
                SampleType* wet = wet_scratch_[static_cast<std::size_t>(k.dst)].data();
                for (int i = 0; i < chunk; ++i) wet[i] += src[i];
            }
        }
        block_cost_ += static_cast<long long>(groups_.size()) * active_cells_ * chunk;
    }

    void mix_chunk(const SampleType* const* in, SampleType* const* out, int offset, int chunk) {
        const SampleType gain = static_cast<SampleType>(wet_gain_ * ir_gain_lin_);
        const SampleType dry = static_cast<SampleType>(dry_gain_);
        const SampleType width = static_cast<SampleType>(width_);

        for (int i = 0; i < chunk; ++i) {
            // Predelay on the wet path only. The engine is LTI, so delaying the
            // send and delaying the wet return are the same filter; the return
            // side costs one ring per audio channel instead of one per IR cell.
            SampleType w[2] = {SampleType{0}, SampleType{0}};
            const int write = (predelay_write_ + i) % predelay_len_;
            const int read = (write - predelay_samples_ + predelay_len_) % predelay_len_;
            for (int c = 0; c < channels_; ++c) {
                auto& ring = predelay_[static_cast<std::size_t>(c)];
                ring[static_cast<std::size_t>(write)] =
                    wet_scratch_[static_cast<std::size_t>(c)][static_cast<std::size_t>(i)];
                w[c] = ring[static_cast<std::size_t>(read)];
            }
            if (channels_ == 2 && width != SampleType{1}) {
                const SampleType mid = (w[0] + w[1]) * SampleType{0.5};
                const SampleType side = (w[0] - w[1]) * SampleType{0.5} * width;
                w[0] = mid + side;
                w[1] = mid - side;
            }
            for (int c = 0; c < channels_; ++c)
                out[c][offset + i] = dry * finite_sample(in[c][offset + i]) + gain * w[c];
        }
        predelay_write_ = (predelay_write_ + chunk) % predelay_len_;
    }

    // ── Group scheduling ─────────────────────────────────────────────────────

    void advance_groups() {
        for (auto& g : groups_) {
            const int period = g.block_len;
            const int phase = static_cast<int>(counter_ % static_cast<std::uint64_t>(period));
            // `counter_` has already advanced past this chunk, so phase == 0
            // means the chunk ended exactly on a period boundary.
            const int elapsed = phase == 0 ? period : phase;

            if (g.armed) {
                // Grant work in proportion to how far through the window we
                // are. At the window's end the grant is exactly `total_cost`,
                // so the transform completes without a catch-up branch: a late
                // host block cannot make the next one do double work.
                const long target =
                    static_cast<long>((static_cast<long long>(g.total_cost) * elapsed) / period);
                run_group_work(g, target - g.granted);
                g.granted = target;
            }

            if (phase != 0) continue;

            // Period boundary. Close the period out before publishing: the
            // proportional grant above can land exactly on the last item of the
            // last job, in which case the work is finished but the job walker
            // has not yet stepped past the final cell to say so. This call is
            // free when the period is genuinely complete (it consumes no
            // budget) and is NOT a catch-up branch — `total_cost` is the exact
            // metered cost of the period, so there is never real work left.
            if (g.armed) run_group_work(g, g.total_cost);

            if (g.has_result) {
                for (int cell = 0; cell < cell_count_; ++cell)
                    g.emit[static_cast<std::size_t>(cell)].swap(
                        g.ready[static_cast<std::size_t>(cell)]);
                g.has_result = false;
            } else {
                for (auto& e : g.emit) std::fill(e.begin(), e.end(), SampleType{0});
            }
            arm_group(g);
        }
    }

    /// Arm the group's next period of work over the frame that just closed.
    /// The frame is the last `2L` samples of the input history and is captured
    /// here as a ring INDEX, not a copy: the gather phase reads through the
    /// ring later, so the boundary block carries no unsliceable O(K) memcpy.
    /// The ring is at least `4L` long, so the frame survives the whole `L`-sample
    /// compute window even though the writer keeps advancing.
    void arm_group(Group& g) {
        g.slot = static_cast<int>((counter_ / static_cast<std::uint64_t>(g.block_len)) %
                                  static_cast<std::uint64_t>(g.num_partitions));
        g.frame_base = (history_write_ - static_cast<std::size_t>(g.fft_size)) & history_mask_;
        g.granted = 0;
        g.job = 0;
        g.job_step = 0;
        g.job_cursor = 0;
        g.fft.cancel();
        g.armed = true;
    }

    /// Job list for one period, in order:
    ///   jobs [0, channels)           forward transform of each audio channel's
    ///                                closed frame into its FDL slot
    ///   jobs [channels, channels+C)  per cell: spectral MAC over the FDL's
    ///                                lower half, Hermitian mirror, inverse
    ///                                transform, and the overlap-save extract
    void run_group_work(Group& g, long budget) {
        if (!g.armed || budget <= 0) return;
        const int np = g.num_partitions;
        const int K = g.fft_size;
        const int L = g.block_len;
        const int half = K / 2;  // Nyquist bin
        long spent = 0;

        while (spent < budget) {
            if (g.job < channels_) {
                // ── forward transform of channel g.job into its FDL slot ──
                auto& spec = g.spectra[static_cast<std::size_t>(g.job * np + g.slot)];
                if (g.fft.done() && g.job_step == 0) {
                    g.fft.begin_forward_from_ring(history_[static_cast<std::size_t>(g.job)].data(),
                                                  g.frame_base, history_mask_);
                    g.job_step = 1;
                }
                spent += g.fft.advance(g.plan, spec.data(), budget - spent);
                if (!g.fft.done()) break;
                ++g.job;
                g.job_step = 0;
                g.job_cursor = 0;
                continue;
            }

            const int cell = g.job - channels_;
            if (cell >= cell_count_) {
                g.armed = false;
                g.has_result = true;
                break;
            }
            if (!cell_active(cell)) {
                std::fill(g.ready[static_cast<std::size_t>(cell)].begin(),
                          g.ready[static_cast<std::size_t>(cell)].end(), SampleType{0});
                ++g.job;
                g.job_step = 0;
                g.job_cursor = 0;
                continue;
            }

            auto& acc = g.accum[static_cast<std::size_t>(cell)];
            const Cell& k = cells_[static_cast<std::size_t>(cell)];

            if (g.job_step == 0) {
                // ── spectral multiply-accumulate over the FDL ──
                // Partition p covers IR delays [2L + pL, 3L + pL) and consumes
                // the frame from p periods ago, so both partitions land on the
                // SAME output range and share one inverse transform. Input and
                // IR spectra are both Hermitian, so only bins [0, K/2] are
                // independent; the mirror step below reconstructs the rest.
                const long unit = detail::SlicedFftPlanT<SampleType>::kMacCost * np;
                const int remaining = half + 1 - g.job_cursor;
                const int take = static_cast<int>(
                    std::min<long>((budget - spent) / unit, remaining));
                if (take <= 0) break;
                for (int p = 0; p < np; ++p) {
                    const int slot = (g.slot - p + np) % np;
                    const std::complex<SampleType>* x =
                        g.spectra[static_cast<std::size_t>(k.src * np + slot)].data();
                    const std::complex<SampleType>* h =
                        g.ir_spectra[static_cast<std::size_t>(cell * np + p)].data();
                    std::complex<SampleType>* a = acc.data();
                    if (p == 0)
                        for (int i = g.job_cursor; i < g.job_cursor + take; ++i)
                            a[i] = x[i] * h[i];
                    else
                        for (int i = g.job_cursor; i < g.job_cursor + take; ++i)
                            a[i] += x[i] * h[i];
                }
                g.job_cursor += take;
                spent += static_cast<long>(take) * unit;
                if (g.job_cursor > half) {
                    g.job_step = 1;
                    g.job_cursor = half + 1;
                }
                continue;
            }

            if (g.job_step == 1) {
                // ── Hermitian mirror onto the upper half ──
                const int take = static_cast<int>(
                    std::min<long>(budget - spent, K - g.job_cursor));
                if (take <= 0) break;
                std::complex<SampleType>* a = acc.data();
                for (int i = g.job_cursor; i < g.job_cursor + take; ++i)
                    a[i] = std::conj(a[K - i]);
                g.job_cursor += take;
                spent += take;
                if (g.job_cursor >= K) {
                    g.job_step = 2;
                    g.job_cursor = 0;
                    g.fft.begin_inverse_in_place();
                }
                continue;
            }

            if (g.job_step == 2) {
                spent += g.fft.advance(g.plan, acc.data(), budget - spent);
                if (!g.fft.done()) break;
                g.job_step = 3;
                g.job_cursor = 0;
                continue;
            }

            // ── extract: overlap-save keeps the LAST L samples ──
            const int take = static_cast<int>(std::min<long>(budget - spent, L - g.job_cursor));
            if (take <= 0) break;
            SampleType* dst = g.ready[static_cast<std::size_t>(cell)].data();
            for (int i = g.job_cursor; i < g.job_cursor + take; ++i)
                dst[i] = acc[static_cast<std::size_t>(L + i)].real();
            g.job_cursor += take;
            spent += take;
            if (g.job_cursor >= L) {
                ++g.job;
                g.job_step = 0;
                g.job_cursor = 0;
            }
        }
        block_cost_ += spent;
    }

    // ── IR preparation ───────────────────────────────────────────────────────

    /// Zero-phase (centred-kernel) windowed-sinc resampling with the broadband
    /// energy of the source preserved by MEASUREMENT: the resampled kernel is
    /// rescaled by `sqrt(E_before / E_after)`. A rate-ratio formula is not used
    /// because windowed-sinc resampling does not preserve `sum h^2` exactly —
    /// passband ripple, transition rolloff, and an arbitrary rational ratio all
    /// perturb it.
    ///
    /// The kernel is centred on the fractional source position rather than run
    /// as a causal FIR, so it has zero group delay and the IR's first arrival
    /// stays at index 0 by construction. A causal implementation would need its
    /// leading `taps/2` samples trimmed to restore that, which is why the spec
    /// carries a leading-delay trim; this formulation has nothing to trim, and
    /// the first-arrival test asserts the outcome either way.
    std::vector<double> resample_energy_preserving(const std::vector<double>& src,
                                                   double src_rate,
                                                   double dst_rate) const {
        if (src.empty()) return src;
        constexpr double pi = 3.14159265358979323846;
        const double ratio = dst_rate / src_rate;
        const int src_len = static_cast<int>(src.size());
        const int dst_len = std::max(1, static_cast<int>(std::ceil(src_len * ratio)));

        // Anti-image / anti-alias cutoff, normalised to the SOURCE Nyquist.
        const double fc = std::min(1.0, ratio);
        // Kernel half-width in source samples. Decimation widens the kernel so
        // the tap count per output sample stays at the design value.
        const double half = 0.5 * resample_taps_ / fc;
        const int reach = static_cast<int>(std::ceil(half)) + 1;
        const double norm = bessel_i0(kResampKaiserBeta);

        std::vector<double> dst(static_cast<std::size_t>(dst_len), 0.0);
        for (int j = 0; j < dst_len; ++j) {
            const double centre = static_cast<double>(j) / ratio;
            const int i0 = static_cast<int>(std::floor(centre));
            double sum = 0.0;
            for (int i = i0 - reach; i <= i0 + reach; ++i) {
                if (i < 0 || i >= src_len) continue;
                const double x = centre - static_cast<double>(i);
                if (std::abs(x) > half) continue;
                const double t = x / half;
                const double w =
                    bessel_i0(kResampKaiserBeta * std::sqrt(std::max(0.0, 1.0 - t * t))) / norm;
                const double s = std::abs(x) < 1e-12 ? fc : std::sin(pi * fc * x) / (pi * x);
                sum += src[static_cast<std::size_t>(i)] * s * w;
            }
            dst[static_cast<std::size_t>(j)] = sum;
        }

        double e_before = 0.0, e_after = 0.0;
        for (double v : src) e_before += v * v;
        for (double v : dst) e_after += v * v;
        if (e_after > kSilenceEpsilon && e_before > kSilenceEpsilon) {
            const double correction = std::sqrt(e_before / e_after);
            for (double& v : dst) v *= correction;
        }
        return dst;
    }

    /// Truncate on the Schroeder backward-energy curve and fade the kept tail.
    void trim_tail(std::vector<std::vector<double>>& work) const {
        constexpr double pi = 3.14159265358979323846;
        if (work.empty() || work[0].empty()) return;
        const std::size_t len = work[0].size();

        // Backward-integrated energy summed over channels: the decay of the
        // measurement as a whole, so channels are never trimmed to different
        // lengths (that would smear the image at the truncation point).
        std::vector<double> schroeder(len + 1, 0.0);
        for (std::size_t i = len; i-- > 0;) {
            double e = 0.0;
            for (const auto& ch : work) e += ch[i] * ch[i];
            schroeder[i] = schroeder[i + 1] + e;
        }
        if (schroeder[0] <= kSilenceEpsilon) return;

        const double floor_lin = std::pow(10.0, tail_trim_db_ / 10.0) * schroeder[0];
        std::size_t keep = len;
        for (std::size_t i = 0; i < len; ++i)
            if (schroeder[i] < floor_lin) { keep = i; break; }
        keep = std::max<std::size_t>(keep, 1);

        const auto fade_target = static_cast<std::size_t>(
            std::max<long>(1, std::lround(units::ms_to_samples(tail_fade_ms_, sample_rate_))));
        const std::size_t fade = std::min(keep, fade_target);
        for (auto& ch : work) {
            ch.resize(keep);
            for (std::size_t i = 0; i < fade; ++i) {
                // Raised cosine from just under 1 down to 0 over the final
                // `fade` samples, so the last kept sample is exactly zero.
                const double t = static_cast<double>(i + 1) / static_cast<double>(fade);
                ch[keep - fade + i] *= 0.5 * (1.0 + std::cos(pi * t));
            }
        }
    }

    void normalize(std::vector<std::vector<double>>& work, int ir_channel_count) const {
        if (normalize_mode_ == IrNormalizeMode::none) return;
        double scale = 1.0;
        if (normalize_mode_ == IrNormalizeMode::peak) {
            double peak = 0.0;
            for (const auto& ch : work)
                for (double v : ch) peak = std::max(peak, std::abs(v));
            if (peak > kSilenceEpsilon) scale = 1.0 / peak;
        } else {
            // Energy per OUTPUT ear, so a stereo or true-stereo IR keeps its
            // balance: ONE scalar for all channels, sized by the loudest ear.
            double worst = 0.0;
            if (ir_channel_count == 1)
                worst = energy(work[0]);
            else if (ir_channel_count == 2)
                worst = std::max(energy(work[0]), energy(work[1]));
            else
                worst = std::max(energy(work[0]) + energy(work[2]),
                                 energy(work[1]) + energy(work[3]));
            if (worst > kSilenceEpsilon) scale = 1.0 / std::sqrt(worst);
        }
        for (auto& ch : work)
            for (double& v : ch) v *= scale;
    }

    static double energy(const std::vector<double>& v) {
        double e = 0.0;
        for (double x : v) e += x * x;
        return e;
    }

    void publish(const std::vector<std::vector<double>>& work, int ir_channel_count) {
        ir_channels_ = ir_channel_count;
        prepared_len_ = static_cast<int>(work[0].size());

        prepared_.assign(static_cast<std::size_t>(ir_channel_count), {});
        for (int c = 0; c < ir_channel_count; ++c) {
            auto& dst = prepared_[static_cast<std::size_t>(c)];
            dst.resize(static_cast<std::size_t>(prepared_len_));
            for (int i = 0; i < prepared_len_; ++i)
                dst[static_cast<std::size_t>(i)] = static_cast<SampleType>(
                    work[static_cast<std::size_t>(c)][static_cast<std::size_t>(i)]);
        }

        build_cells(work, ir_channel_count);

        // Groups tile [N0, N0 * 2^P). Group g has L = (N0/2) * 2^g and covers
        // [2L, 4L) = [N0 * 2^g, N0 * 2^(g+1)).
        groups_.clear();
        smallest_period_ = head_len_;
        if (prepared_len_ > head_len_) {
            smallest_period_ = head_len_ / 2;
            for (int L = head_len_ / 2; 2 * L < prepared_len_; L *= 2)
                groups_.push_back(build_group(L, work));
        }

        // Input history ring: the largest group's frame (2L) must survive the
        // L samples of its compute window, so 3L is the floor; the next power
        // of two gives masking for free and covers the head as well.
        int ring = std::max(2 * head_len_, 4 * max_block_);
        for (const auto& g : groups_) ring = std::max(ring, 4 * g.block_len);
        ring = round_up_pow2(ring);
        history_.assign(2, std::vector<SampleType>(static_cast<std::size_t>(ring), SampleType{0}));
        history_mask_ = static_cast<std::size_t>(ring) - 1;

        loaded_ = true;
        reset();
    }

    void build_cells(const std::vector<std::vector<double>>& work, int ir_channel_count) {
        cells_.clear();
        auto add = [&](int src, int dst, int ir_channel) {
            Cell c;
            c.src = src;
            c.dst = dst;
            c.ir_channel = ir_channel;
            const auto& h = work[static_cast<std::size_t>(ir_channel)];
            for (double v : h) c.l1 += std::abs(v);
            c.head.assign(static_cast<std::size_t>(head_len_), SampleType{0});
            const int n = std::min<int>(head_len_, static_cast<int>(h.size()));
            for (int i = 0; i < n; ++i)
                c.head[static_cast<std::size_t>(i)] =
                    static_cast<SampleType>(h[static_cast<std::size_t>(i)]);
            cells_.push_back(std::move(c));
        };

        if (ir_channel_count == 1) {
            add(0, 0, 0);
            if (channels_ == 2) add(1, 1, 0);
        } else if (ir_channel_count == 2) {
            add(0, 0, 0);
            if (channels_ == 2) add(1, 1, 1);
        } else {
            // [LL, LR, RL, RR]: cell (src, dst) reads IR channel 2*src + dst.
            add(0, 0, 0);
            add(0, 1, 1);
            if (channels_ == 2) {
                add(1, 0, 2);
                add(1, 1, 3);
            }
        }
        cell_count_ = static_cast<int>(cells_.size());
        refresh_active_cells();
    }

    /// A cross cell (src != dst) only contributes when true-stereo routing is
    /// engaged; a 4-channel IR with `true_stereo == false` therefore runs its
    /// [LL, RR] diagonal as dual-mono. Gating rather than rebuilding is what
    /// keeps `set_true_stereo()` allocation-free on the audio thread.
    bool cell_active(int cell) const {
        const Cell& k = cells_[static_cast<std::size_t>(cell)];
        return k.src == k.dst || true_stereo_;
    }

    void refresh_active_cells() {
        active_cells_ = 0;
        for (int c = 0; c < cell_count_; ++c)
            if (cell_active(c)) ++active_cells_;
    }

    Group build_group(int L, const std::vector<std::vector<double>>& work) {
        Group g;
        g.block_len = L;
        g.fft_size = 2 * L;
        g.ir_start = 2 * L;
        g.num_partitions = (3 * L < prepared_len_) ? 2 : 1;
        g.plan.prepare(g.fft_size);

        const int np = g.num_partitions;
        const int K = g.fft_size;
        g.spectra.assign(static_cast<std::size_t>(2 * np),
                         std::vector<std::complex<SampleType>>(static_cast<std::size_t>(K)));
        g.ir_spectra.assign(static_cast<std::size_t>(cell_count_ * np),
                            std::vector<std::complex<SampleType>>(static_cast<std::size_t>(K)));
        g.accum.assign(static_cast<std::size_t>(cell_count_),
                       std::vector<std::complex<SampleType>>(static_cast<std::size_t>(K)));
        g.emit.assign(static_cast<std::size_t>(cell_count_),
                      std::vector<SampleType>(static_cast<std::size_t>(L), SampleType{0}));
        g.ready.assign(static_cast<std::size_t>(cell_count_),
                       std::vector<SampleType>(static_cast<std::size_t>(L), SampleType{0}));

        std::vector<double> segment(static_cast<std::size_t>(L), 0.0);
        for (int cell = 0; cell < cell_count_; ++cell) {
            const auto& h =
                work[static_cast<std::size_t>(cells_[static_cast<std::size_t>(cell)].ir_channel)];
            for (int p = 0; p < np; ++p) {
                const int start = g.ir_start + p * L;
                std::fill(segment.begin(), segment.end(), 0.0);
                for (int i = 0; i < L; ++i) {
                    const int idx = start + i;
                    if (idx < static_cast<int>(h.size()))
                        segment[static_cast<std::size_t>(i)] = h[static_cast<std::size_t>(idx)];
                }
                g.plan.transform_real_padded(
                    segment.data(), L, g.ir_spectra[static_cast<std::size_t>(cell * np + p)].data());
            }
        }

        // Metered cost of one period: one forward per audio channel, then per
        // cell a spectral MAC over the independent bins, the Hermitian mirror,
        // an inverse, and the extract.
        g.total_cost = static_cast<long>(channels_) * g.plan.forward_cost();
        g.total_cost +=
            static_cast<long>(cell_count_) *
            (static_cast<long>(K / 2 + 1) * detail::SlicedFftPlanT<SampleType>::kMacCost * np +
             static_cast<long>(K / 2 - 1) + g.plan.inverse_cost() + L);
        return g;
    }

    // ── Send EQ ──────────────────────────────────────────────────────────────

    void update_send_eq() {
        // Endpoint == bypass; see the parameter declaration above.
        hp_active_ = lowcut_hz_ > kLowcutHzMin;
        lp_active_ = highcut_hz_ < kHighcutHzMax;
        constexpr double two_pi = 6.283185307179586476925286766559;
        const double hp_w = two_pi * lowcut_hz_ / sample_rate_;
        hp_coef_ = 1.0 / (1.0 + hp_w);
        const double lp_w = two_pi * highcut_hz_ / sample_rate_;
        lp_coef_ = std::clamp(lp_w / (1.0 + lp_w), 0.0, 1.0);
    }

    static int round_up_pow2(int v) {
        if (v <= 1) return 1;
        int p = 1;
        while (p < v) p <<= 1;
        return p;
    }

    // ── State ────────────────────────────────────────────────────────────────
    double sample_rate_ = 48000.0;
    int max_block_ = 512;
    int channels_ = 2;
    int head_len_ = 128;
    bool is_prepared_ = false;
    bool loaded_ = false;

    std::vector<std::vector<SampleType>> history_;  // per audio channel
    std::size_t history_mask_ = 0;
    std::size_t history_write_ = 0;
    std::uint64_t counter_ = 0;
    int smallest_period_ = 1;

    std::vector<Group> groups_;
    std::vector<Cell> cells_;
    int cell_count_ = 0;
    int active_cells_ = 0;

    std::vector<std::vector<SampleType>> prepared_;
    int prepared_len_ = 0;
    int ir_channels_ = 0;

    std::vector<std::vector<SampleType>> wet_scratch_;
    std::vector<std::vector<SampleType>> predelay_;
    int predelay_len_ = 1;
    int predelay_write_ = 0;
    int predelay_samples_ = 0;

    std::vector<OnePole> hp_state_;
    std::vector<OnePole> lp_state_;
    bool hp_active_ = false;
    bool lp_active_ = false;
    double hp_coef_ = 1.0;
    double lp_coef_ = 1.0;

    IrNormalizeMode normalize_mode_ = IrNormalizeMode::energy;
    double tail_trim_db_ = kTailTrimDbDefault;
    double tail_fade_ms_ = kTailFadeMsDefault;
    int resample_taps_ = kResampTapsPerPhaseDefault;

    double ir_gain_db_ = kIrGainDbDefault;
    double ir_gain_lin_ = 1.0;
    double predelay_ms_ = kPredelayMsDefault;
    double wet_gain_ = kWetPercentDefault / 100.0;
    double dry_gain_ = kDryPercentDefault / 100.0;
    double width_ = kWidthPercentDefault / 100.0;
    double lowcut_hz_ = kLowcutHzDefault;
    double highcut_hz_ = kHighcutHzDefault;
    bool true_stereo_ = false;

    long long block_cost_ = 0;
};

using ZeroLatencyConvolver = ZeroLatencyConvolverT<float>;
using ZeroLatencyConvolver64 = ZeroLatencyConvolverT<double>;

/// ## Use-case cookbook
///
/// * **Convolution reverb (the default).** Energy-normalized hall/plate/room
///   IR, `wet 100 % / dry 0 %` on a send, `predelay 10-40 ms` to separate the
///   source from the space, `lowcut 80-120 Hz` to keep the tail out of the low
///   end. Zero latency means it is also safe as an insert on a live vocal.
/// * **Cabinet / IR loader (this is what M22 cites).** `IrNormalizeMode::peak`,
///   `wet 100 % / dry 0 %`, short IR (<= 4096 taps) — the whole thing runs in
///   the head plus one or two groups.
/// * **True-stereo space.** 4-channel [LL, LR, RL, RR] IR,
///   `set_true_stereo(true)`, `width 100-140 %`.
/// * **Match / linear-phase EQ.** A corrective FIR as a 1-channel IR,
///   `dry 0 %`, `IrNormalizeMode::none`.
/// * **Parallel dry + wet.** `dry 100 %` plus wet at taste: the dry path is
///   bit-exact and both paths are zero-latency, so they sum phase-coherently.

}  // namespace pulp::signal
