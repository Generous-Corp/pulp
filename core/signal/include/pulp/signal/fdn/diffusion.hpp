#pragma once

// Two-stage diffusion: a cascade of Schroeder allpasses on the input path, and
// a second, gentler cascade inside the loop.
//
// The split is the point. The input cascade can be aggressive — it is heard
// once, and short geometrically-shrinking stage delays smear the early energy
// without smearing the onset. The in-loop cascade is re-applied on every pass,
// so the same coefficient there compounds into metal; it runs at a markedly
// lower g. The pre-cascade also alternates its coefficient's SIGN between the
// left and right input, so the two rails decorrelate from the very first
// reflection instead of relying on the tank to do it.
//
// Block form. A stage whose delay is at least the block length reads only
// samples written before the block began, so the whole block runs along time
// as one vectorizable pass over two disjoint spans of the stage's ring. The
// per-sample and block forms compute the same expression per sample. The block
// form takes finite, bounded input — its callers sanitize at their entry — and
// with |g| < 1 an allpass maps bounded input to bounded output, so it snaps
// the stored value against denormals and carries no per-sample finiteness
// test.
//
// RT contract: prepare() allocates; process/reset allocate nothing.

#include <pulp/signal/denormal.hpp>
#include <pulp/signal/fdn/config.hpp>
#include <pulp/signal/fdn/frac_delay.hpp>
#include <pulp/signal/detail/simd_lanes.hpp>
#include <pulp/signal/fdn/modulation.hpp>
#include <pulp/signal/fdn/sanitize.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <type_traits>

namespace pulp::signal::fdn {

namespace lanes = signal::detail::lanes;

// One Schroeder allpass: y[n] = -g*x[n] + x[n-d] + g*y[n-d], in the
// single-buffer form (one delay, one state).
template <typename SampleType = float>
class AllpassStage {
  public:
    void prepare(int max_delay_samples, std::size_t stagger = 0) {
        line_.prepare(max_delay_samples, stagger);
    }
    void reset() { line_.reset(); }

    void set_delay(int samples) {
        delay_ = std::clamp(samples, 2, line_.max_delay());
    }
    int delay() const { return delay_; }
    FracDelayT<SampleType>& line() {
        return line_;
    }

    SampleType process(SampleType x, SampleType g) {
        x = finite_or_zero(x);
        const SampleType delayed = finite_or_zero(line_.read(delay_));
        const SampleType w =
            finite_or_zero(snap_to_zero(static_cast<SampleType>(x + g * delayed)));
        line_.push(w);
        return finite_or_zero(static_cast<SampleType>(delayed - g * w));
    }

    // Fractional-delay path, used only when flutter is engaged. Kept separate
    // from process() so the common case stays on the integer read.
    SampleType process(SampleType x, SampleType g, double fractional_delay) {
        x = finite_or_zero(x);
        const SampleType delayed = finite_or_zero(line_.read(fractional_delay));
        const SampleType w =
            finite_or_zero(snap_to_zero(static_cast<SampleType>(x + g * delayed)));
        line_.push(w);
        return finite_or_zero(static_cast<SampleType>(delayed - g * w));
    }

    // Block of up to kMaxBlockSamples, in place. Requires n <= delay(), which
    // makes the delayed span and the span being written disjoint. Both spans
    // are contiguous: the ring mirrors its start past its end.
    void process_block(SampleType* x, int n, SampleType g) {
        SampleType* buf = line_.data();
        const std::size_t w = line_.write_index();
        const std::size_t r = (w - static_cast<std::size_t>(delay_)) & line_.mask();
        run(x, buf + r, buf + w, n, g);
        line_.advance(n);
    }

    // The stage expression over one contiguous piece.
    static void run(SampleType* __restrict x, const SampleType* __restrict delayed,
                    SampleType* __restrict stored, int n, SampleType g) {
        for (int k = 0; k < n; ++k) {
            const SampleType v = snap_to_zero(static_cast<SampleType>(x[k] + g * delayed[k]));
            stored[k] = v;
            x[k] = static_cast<SampleType>(delayed[k] - g * v);
        }
    }

    // Fluttered block: per-sample fractional delays. Requires every delay to
    // be at least n + 1 samples.
    void process_block(SampleType* x, int n, SampleType g, const double* delays) {
        SampleType stored[kMaxBlockSamples];
        for (int k = 0; k < n; ++k) {
            const SampleType delayed = line_.read_hermite_at(k, delays[k]);
            stored[k] = snap_to_zero(static_cast<SampleType>(x[k] + g * delayed));
            x[k] = static_cast<SampleType>(delayed - g * stored[k]);
        }
        line_.push_block(stored, n);
    }

private:
    FracDelayT<SampleType> line_;
    int delay_ = 2;
};

// A cascade of up to kMaxDiffusionStages allpasses whose delays shrink
// geometrically from a base length.
template <typename SampleType = float>
class AllpassCascade {
  public:
    // `max_base_ms` sizes the allocation for the longest base the engine can
    // ask for at the highest tank rate; configure() only re-derives lengths.
    //
    // `stagger` offsets this cascade's rings within their allocations (see
    // FracDelayT::prepare); each stage is offset one more step past it.
    void prepare(double max_base_ms, double max_sample_rate, int num_stages,
                 std::size_t stagger = 0) {
        num_stages_ = std::clamp(num_stages, 0, kMaxDiffusionStages);
        double base = max_base_ms;
        for (int s = 0; s < num_stages_; ++s) {
            const int cap =
                static_cast<int>(base * 0.001 * max_sample_rate) + kHermiteGuard;
            stages_[s].prepare(cap, stagger + static_cast<std::size_t>(s) * kRingStaggerSamples);
            base *= kDiffusionStageShrink;
        }
    }

    // Re-derive the stage delays for a tank rate. `offset_samples` staggers one
    // cascade against another so two channels never share a stage length.
    void configure(double base_ms, double sample_rate, int offset_samples) {
        double base = base_ms;
        for (int s = 0; s < num_stages_; ++s) {
            const int len =
                static_cast<int>(base * 0.001 * sample_rate) + offset_samples;
            stages_[s].set_delay(len);
            base_delay_[static_cast<std::size_t>(s)] = static_cast<double>(len);
            base *= kDiffusionStageShrink;
        }
        min_delay_ = std::numeric_limits<int>::max();
        for (int s = 0; s < num_stages_; ++s)
            min_delay_ = std::min(min_delay_, stages_[s].delay());
    }

    void reset() {
        for (int s = 0; s < num_stages_; ++s) stages_[s].reset();
    }

    int num_stages() const { return num_stages_; }
    AllpassStage<SampleType>& stage(int s) { return stages_[s]; }

    // Total delay the cascade contributes to a loop it sits in. An allpass is
    // lossless but not delay-free, and a decay law that ignores it computes the
    // per-pass gain for the wrong loop length.
    double total_delay() const {
        double sum = 0.0;
        for (int s = 0; s < num_stages_; ++s)
            sum += base_delay_[static_cast<std::size_t>(s)];
        return sum;
    }

    SampleType process(SampleType x, SampleType g) {
        for (int s = 0; s < num_stages_; ++s) x = stages_[s].process(x, g);
        return x;
    }

    // Shortest stage delay — the longest block process_block() may take.
    int min_delay() const {
        return min_delay_;
    }

    // Block forms of the two paths below. Same per-sample arithmetic; see the
    // file note for the length bound.
    // Any n <= kMaxBlockSamples; split so each piece fits every stage.
    void process_block(SampleType* x, int n, SampleType g) {
        const int piece_max = std::max(1, min_delay());
        for (int offset = 0; offset < n; offset += piece_max) {
            const int piece = std::min(piece_max, n - offset);
            if (num_stages_ == 4) {
                fused4(x + offset, piece, g);
            } else {
                for (int s = 0; s < num_stages_; ++s)
                    stages_[s].process_block(x + offset, piece, g);
            }
        }
    }

    void process_block_fluttered(SampleType* x, int n, SampleType g, const double* lfo,
                                 double depth) {
        double delays[kMaxBlockSamples];
        for (int s = 0; s < num_stages_; ++s) {
            const double d = base_delay_[static_cast<std::size_t>(s)];
            for (int k = 0; k < n; ++k)
                delays[k] = d * (1.0 + depth * lfo[k]);
            stages_[s].process_block(x, n, g, delays);
        }
    }

    // Flutter path: every stage delay becomes fractional and wobbles by
    // `depth` (a fraction of its own length) around a shared LFO value.
    SampleType process_fluttered(SampleType x, SampleType g, double lfo, double depth) {
        for (int s = 0; s < num_stages_; ++s) {
            const double d = base_delay_[static_cast<std::size_t>(s)];
            x = stages_[s].process(x, g, d * (1.0 + depth * lfo));
        }
        return x;
    }

  private:
    // The four-stage cascade (the engine's only shape) as one pass along
    // time. Each stage is the same expression AllpassStage::run applies; only
    // the loop is shared.
    void fused4(SampleType* x, int n, SampleType g) {
        SampleType* d[4];
        SampleType* o[4];
        spans4(n, d, o);
        run_spans4(x, n, g, d, o);
        commit4(n);
    }

public:
    // The four stages' delayed and stored spans for the next n samples (the
    // rings mirror their starts, so every span is contiguous). With
    // commit4(), lets a caller run many cascades through run_spans4() without
    // per-cascade setup. Requires four stages and n <= min_delay().
    bool spans4(int n, SampleType** delayed, SampleType** stored) {
        if (num_stages_ != 4 || n > min_delay_) return false;
        for (int s = 0; s < 4; ++s) {
            FracDelayT<SampleType>& line = stages_[s].line();
            const std::size_t w = line.write_index();
            const std::size_t r = (w - static_cast<std::size_t>(stages_[s].delay())) & line.mask();
            delayed[s] = line.data() + r;
            stored[s] = line.data() + w;
        }
        return true;
    }

    void commit4(int n) {
        for (int s = 0; s < 4; ++s) stages_[s].line().advance(n);
    }

    // Four stages over one contiguous piece: the expression
    // AllpassStage::run applies, stage after stage, four samples at a time
    // as explicit vectors (the compiler cannot prove the eight spans
    // disjoint, and its runtime checks cost more than a short block saves),
    // then a scalar tail.
    static void run_spans4(SampleType* x, int n, SampleType g, SampleType* const* d,
                           SampleType* const* o) {
        int k = 0;
        if constexpr (std::is_same_v<SampleType, float>) {
            const lanes::F4 gv = lanes::splat(g);
            for (; k + 4 <= n; k += 4) {
                lanes::F4 v = lanes::load(x + k);
                for (int s = 0; s < 4; ++s) {
                    const lanes::F4 delayed = lanes::load(d[s] + k);
                    const lanes::F4 u = snap(v + gv * delayed);
                    lanes::store(o[s] + k, u);
                    v = delayed - gv * u;
                }
                lanes::store(x + k, v);
            }
        }
        for (; k < n; ++k) {
            SampleType v = x[k];
            for (int s = 0; s < 4; ++s) {
                const SampleType u = snap_to_zero(static_cast<SampleType>(v + g * d[s][k]));
                o[s][k] = u;
                v = static_cast<SampleType>(d[s][k] - g * u);
            }
            x[k] = v;
        }
    }

private:
    // snap_to_zero on four lanes.
    static lanes::F4 snap(lanes::F4 v) {
        return lanes::select(lanes::less(lanes::abs(v), lanes::splat(snap_threshold<float>())),
                             lanes::splat(0.0f), v);
    }

    std::array<AllpassStage<SampleType>, kMaxDiffusionStages> stages_{};
    std::array<double, kMaxDiffusionStages> base_delay_{};
    int num_stages_ = 0;
    int min_delay_ = 1;
};

}  // namespace pulp::signal::fdn
