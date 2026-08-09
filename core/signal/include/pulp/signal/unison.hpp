#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include <pulp/signal/rng.hpp>

namespace pulp::signal {

enum class UnisonGainLaw : std::uint8_t { PeakSafe, EqualPower };

/// Control-side configuration for a fixed unison layout.
///
/// Detune and drift are non-negative cents. Stereo spread and phase
/// randomization are normalized magnitudes in [0, 1]. A nonzero drift amount
/// requires a nonzero period in absolute audio frames. `configure()` rejects
/// invalid or non-finite input without changing the active layout.
struct UnisonSpec {
    std::size_t voice_count = 1;
    double detune_cents = 0.0;
    double stereo_spread = 0.0;
    double phase_randomization = 0.0;
    double drift_cents = 0.0;
    std::uint64_t drift_period_frames = 2048;
    UnisonGainLaw gain_law = UnisonGainLaw::PeakSafe;
};

struct UnisonVoiceParameters {
    double detune_cents = 0.0;
    double pan = 0.0;
    double gain = 1.0;
    double phase = 0.0;
};

template <std::size_t MaximumVoices = 16>
class UnisonLayout {
public:
    static_assert(MaximumVoices > 0);

    bool configure(const UnisonSpec& spec, std::uint64_t seed = 0,
                   std::uint64_t note_instance_id = 0) noexcept {
        if (spec.voice_count == 0 || spec.voice_count > MaximumVoices ||
            !finite_nonnegative(spec.detune_cents) || !unit(spec.stereo_spread) ||
            !unit(spec.phase_randomization) || !finite_nonnegative(spec.drift_cents) ||
            (spec.drift_cents > 0.0 && spec.drift_period_frames == 0) ||
            !valid_gain_law(spec.gain_law)) {
            return false;
        }
        std::array<UnisonVoiceParameters, MaximumVoices> next{};
        const double gain = spec.gain_law == UnisonGainLaw::PeakSafe
                                ? 1.0 / static_cast<double>(spec.voice_count)
                                : 1.0 / std::sqrt(static_cast<double>(spec.voice_count));
        for (std::size_t i = 0; i < spec.voice_count; ++i) {
            const double position = spec.voice_count == 1
                                        ? 0.0
                                        : -1.0 + 2.0 * static_cast<double>(i) /
                                                       static_cast<double>(spec.voice_count - 1);
            const auto key = mix64(seed, note_instance_id, i + 0x554e49534f4eull);
            next[i] = {position * spec.detune_cents,
                       position * spec.stereo_spread,
                       gain,
                       spec.voice_count == 1 ? 0.0
                                             : unit_from(key) * spec.phase_randomization};
        }
        voices_ = next;
        spec_ = spec;
        seed_ = seed;
        note_instance_id_ = note_instance_id;
        configured_ = true;
        return true;
    }

    bool configured() const noexcept { return configured_; }
    std::size_t size() const noexcept { return configured_ ? spec_.voice_count : 0; }
    const UnisonVoiceParameters& operator[](std::size_t index) const noexcept {
        return voices_[index];
    }
    const UnisonSpec& spec() const noexcept { return spec_; }

    // Stateless absolute-frame drift makes results independent of callback
    // partitioning. Adjacent deterministic knots are smoothly interpolated.
    double drift_cents(std::size_t voice, std::uint64_t absolute_frame) const noexcept {
        if (!configured_ || voice >= spec_.voice_count || spec_.drift_cents == 0.0)
            return 0.0;
        const auto period = spec_.drift_period_frames;
        const auto knot = absolute_frame / period;
        double t = static_cast<double>(absolute_frame % period) / static_cast<double>(period);
        t = t * t * (3.0 - 2.0 * t);
        const auto base = mix64(seed_, note_instance_id_, voice + 0x4452494654ull);
        const double a = bipolar_from(mix64(base, knot, 0));
        const double b = bipolar_from(mix64(base, knot + 1, 0));
        return ((1.0 - t) * a + t * b) * spec_.drift_cents;
    }

private:
    static bool valid_gain_law(UnisonGainLaw law) noexcept {
        return law == UnisonGainLaw::PeakSafe || law == UnisonGainLaw::EqualPower;
    }
    static bool finite_nonnegative(double value) noexcept {
        return std::isfinite(value) && value >= 0.0;
    }
    static bool unit(double value) noexcept {
        return std::isfinite(value) && value >= 0.0 && value <= 1.0;
    }

    std::array<UnisonVoiceParameters, MaximumVoices> voices_{};
    UnisonSpec spec_{};
    std::uint64_t seed_ = 0;
    std::uint64_t note_instance_id_ = 0;
    bool configured_ = false;
};

} // namespace pulp::signal
