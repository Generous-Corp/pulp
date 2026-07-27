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
#include <limits>
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
/// Public design constants and ingest validation shared by every convolver
/// specialization. Inheritance preserves the established Class::kConstant API.
struct ZeroLatencyConvolverDesign {
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

    /// Proves every resampler geometry value is finite and representable before
    /// the ingest path casts it to `int` or allocates source/destination work.
    /// Public so registration-time wrappers can reject unusable IR metadata
    /// before retaining a shared copy.
    static bool valid_resample_geometry(int source_length, double source_rate,
                                        double destination_rate, int taps_per_phase) noexcept {
        if (source_length <= 0 || taps_per_phase <= 0 ||
            !std::isfinite(source_rate) || source_rate <= 0.0 ||
            !std::isfinite(destination_rate) || destination_rate <= 0.0)
            return false;
        constexpr double int_max = static_cast<double>(std::numeric_limits<int>::max());
        const double ratio = destination_rate / source_rate;
        if (!std::isfinite(ratio) || ratio <= 0.0) return false;

        const double destination_length =
            std::ceil(static_cast<double>(source_length) * ratio);
        if (!std::isfinite(destination_length) || destination_length < 1.0 ||
            destination_length > int_max)
            return false;

        const double cutoff = std::min(1.0, ratio);
        const double half_width = 0.5 * static_cast<double>(taps_per_phase) / cutoff;
        const double reach = std::ceil(half_width) + 1.0;
        if (!std::isfinite(half_width) || !std::isfinite(reach) || reach < 1.0 ||
            reach > int_max)
            return false;

        // The kernel loop forms `i0 +/- reach` in signed-int arithmetic. Its
        // largest possible centre is the final source sample; reserve one more
        // value so the loop increment after the last iteration cannot overflow.
        return reach <= int_max - static_cast<double>(source_length);
    }

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


};

namespace detail {

// ── Partition group: two length-L partitions covering IR [2L, 4L) ────────
template <typename SampleType>
struct ZeroLatencyConvolverGroup {
    using FftCursor = typename SlicedFftPlanT<SampleType>::Cursor;
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

template <typename SampleType>
struct ZeroLatencyConvolverCell {
    int src = 0;
    int dst = 0;
    int ir_channel = 0;
    double l1 = 0.0;
    std::vector<SampleType> head;  // N0 direct-FIR taps
};

struct ZeroLatencyConvolverOnePole {
    double z = 0.0;
    double prev_x = 0.0;
};

}  // namespace detail

}  // namespace pulp::signal
