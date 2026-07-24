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
// RT contract: prepare() allocates; process/reset allocate nothing.

#include <pulp/signal/denormal.hpp>
#include <pulp/signal/fdn/config.hpp>
#include <pulp/signal/fdn/frac_delay.hpp>
#include <pulp/signal/fdn/modulation.hpp>

#include <algorithm>
#include <array>
#include <cmath>

namespace pulp::signal::fdn {

// One Schroeder allpass: y[n] = -g*x[n] + x[n-d] + g*y[n-d], in the
// single-buffer form (one delay, one state).
template <typename SampleType = float>
class AllpassStage {
public:
    void prepare(int max_delay_samples) { line_.prepare(max_delay_samples); }
    void reset() { line_.reset(); }

    void set_delay(int samples) {
        delay_ = std::clamp(samples, 2, line_.max_delay());
    }
    int delay() const { return delay_; }

    SampleType process(SampleType x, SampleType g) {
        const SampleType delayed = line_.read(delay_);
        const SampleType w = snap_to_zero(static_cast<SampleType>(x + g * delayed));
        line_.push(w);
        return static_cast<SampleType>(delayed - g * w);
    }

    // Fractional-delay path, used only when flutter is engaged. Kept separate
    // from process() so the common case stays on the integer read.
    SampleType process(SampleType x, SampleType g, double fractional_delay) {
        const SampleType delayed = line_.read(fractional_delay);
        const SampleType w = snap_to_zero(static_cast<SampleType>(x + g * delayed));
        line_.push(w);
        return static_cast<SampleType>(delayed - g * w);
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
    void prepare(double max_base_ms, double max_sample_rate, int num_stages) {
        num_stages_ = std::clamp(num_stages, 0, kMaxDiffusionStages);
        double base = max_base_ms;
        for (int s = 0; s < num_stages_; ++s) {
            const int cap =
                static_cast<int>(base * 0.001 * max_sample_rate) + kHermiteGuard;
            stages_[s].prepare(cap);
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
    }

    void reset() {
        for (int s = 0; s < num_stages_; ++s) stages_[s].reset();
    }

    int num_stages() const { return num_stages_; }

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
    std::array<AllpassStage<SampleType>, kMaxDiffusionStages> stages_{};
    std::array<double, kMaxDiffusionStages> base_delay_{};
    int num_stages_ = 0;
};

}  // namespace pulp::signal::fdn
