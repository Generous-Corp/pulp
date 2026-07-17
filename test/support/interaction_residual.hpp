#pragma once

// Black-box probes for how a voice carries state across hits, and for whether
// a timbre change is explainable as level alone.
//
// Both probes are pure arithmetic over rendered buffers: they need no access
// to a voice's internals, so the same code measures our own Processor and a
// hosted reference plugin.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <span>
#include <vector>

namespace pulp::test::audio {

/// A single excitation event: `velocity` applied at `frame`.
struct Hit {
    std::size_t frame = 0;
    float velocity = 1.0f;
};

/// Renders `frames` samples of mono output from a *freshly constructed* voice
/// driven by `hits`.
///
/// Constructing fresh per call is the contract the residual depends on. A
/// render from a warm voice would fold the previous render's tail into the
/// result and the residual would measure the harness instead of the voice.
using HitRenderFn =
    std::function<std::vector<float>(const std::vector<Hit>& hits, std::size_t frames)>;

/// `I = pair - (solo_first + solo_second)`: what the second hit did that it
/// would not have done into a quiet voice.
///
/// The residual is zero for two very different reasons, and separating them is
/// the point of the probe:
///
///   - A voice that resets on note-on, or allocates a fresh voice per hit, has
///     no cross-hit state at all. Its pair render *is* the sum of its solo
///     renders, sample for sample, so `I` is identically zero — bit-exact, at
///     every gap.
///   - A persistent but strictly *linear* resonator obeys superposition, so its
///     `I` is zero to within floating-point rounding at every gap.
///
/// A nonzero, gap-dependent `I` therefore means the voice keeps state across
/// hits *and* couples that state back into its own behaviour nonlinearly. That
/// is a fingerprint no amount of level or envelope tuning can fake, and it is
/// visible from outside the box.
struct InteractionResidual {
    std::size_t gap_frames = 0;
    /// pair - (solo_first + solo_second), sample-aligned from frame 0.
    std::vector<float> residual;
    double pair_rms = 0.0;
    double residual_rms = 0.0;
    /// residual_rms / pair_rms, or 0.0 when the pair render is silent.
    double ratio = 0.0;
};

inline double rms_of(std::span<const float> x) {
    if (x.empty()) return 0.0;
    double acc = 0.0;
    for (const float v : x) {
        const double d = static_cast<double>(v);
        acc += d * d;
    }
    return std::sqrt(acc / static_cast<double>(x.size()));
}

/// Renders the pair and the two solos through `render` and differences them.
///
/// The three renders are independent calls, so `render` must build a fresh
/// voice each time (see HitRenderFn).
inline InteractionResidual measure_interaction_residual(const HitRenderFn& render,
                                                        const Hit& first,
                                                        const Hit& second,
                                                        std::size_t frames) {
    const std::vector<float> pair = render({first, second}, frames);
    const std::vector<float> solo_first = render({first}, frames);
    const std::vector<float> solo_second = render({second}, frames);

    InteractionResidual out;
    out.gap_frames = second.frame - first.frame;

    // A conforming renderer returns exactly `frames`; clamping keeps a
    // misbehaving one from reading out of range instead of corrupting memory.
    const std::size_t n =
        std::min(std::min(pair.size(), solo_first.size()), std::min(solo_second.size(), frames));

    out.residual.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        out.residual[i] = pair[i] - (solo_first[i] + solo_second[i]);
    }

    out.pair_rms = rms_of(std::span<const float>(pair.data(), n));
    out.residual_rms = rms_of(out.residual);
    out.ratio = (out.pair_rms > 0.0) ? out.residual_rms / out.pair_rms : 0.0;
    return out;
}

/// Measures the residual at each gap in `gaps_frames`, with the second hit
/// placed `gap` frames after the first.
///
/// The *shape* of ratio-vs-gap is the readable result: a persistent nonlinear
/// resonator decays toward zero as the gap outruns its own ring, while a
/// per-hit voice reports zero everywhere.
inline std::vector<InteractionResidual> sweep_interaction_residual(
    const HitRenderFn& render, const Hit& first, float second_velocity,
    std::span<const std::size_t> gaps_frames, std::size_t frames) {
    std::vector<InteractionResidual> out;
    out.reserve(gaps_frames.size());
    for (const std::size_t gap : gaps_frames) {
        const Hit second{first.frame + gap, second_velocity};
        out.push_back(measure_interaction_residual(render, first, second, frames));
    }
    return out;
}

/// The least-squares gain that best explains `target` as a scaled `model`, and
/// what is left over.
///
/// This is the fairest possible test of "the loud hit is just the quiet hit
/// turned up": rather than asserting some gain, it hands the gain-only
/// hypothesis its own optimum and reports how much of `target` still cannot be
/// reached. `residual_ratio` near 0 means level explains the difference; near 1
/// means it explains nothing. A negative `gain` means the two are
/// anti-correlated, so the hypothesis is not merely weak but wrong in sign.
struct GainNull {
    double gain = 0.0;
    /// ||target - gain*model|| / ||target||, or 0.0 when target is silent.
    double residual_ratio = 0.0;
};

inline GainNull best_fit_gain_null(std::span<const float> target, std::span<const float> model) {
    const std::size_t n = std::min(target.size(), model.size());

    double dot = 0.0;
    double model_energy = 0.0;
    double target_energy = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(target[i]);
        const double m = static_cast<double>(model[i]);
        dot += t * m;
        model_energy += m * m;
        target_energy += t * t;
    }

    GainNull out;
    // A silent model can explain nothing, so the optimal gain is 0 and the
    // whole target survives.
    out.gain = (model_energy > 0.0) ? dot / model_energy : 0.0;

    double resid_energy = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double d = static_cast<double>(target[i]) - out.gain * static_cast<double>(model[i]);
        resid_energy += d * d;
    }
    out.residual_ratio = (target_energy > 0.0) ? std::sqrt(resid_energy / target_energy) : 0.0;
    return out;
}

}  // namespace pulp::test::audio
