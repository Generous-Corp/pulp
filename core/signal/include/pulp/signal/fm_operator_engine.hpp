#pragma once

#include <pulp/signal/denormal.hpp>
#include <pulp/signal/envelope.hpp>
#include <pulp/signal/fast_math.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <type_traits>

namespace pulp::signal {

/// How an operator derives its unmodulated frequency.
enum class FmOperatorFrequencyMode : std::uint8_t {
    ratio,    ///< `base_frequency_hz * ratio`.
    fixed_hz, ///< An absolute frequency in Hz, independent of the played key.
};

/// Alias behavior for the modulation network.
///
/// Sine operators have no unmodulated harmonics, but FM/PM sidebands are not
/// band-limited. `bounded` preserves the requested modulation and clamps the
/// instantaneous frequency to the representable band. `bright_band_safe`
/// additionally fades routed modulation according to the destination's base
/// frequency plus current absolute Hz deviation between 65% and 90% of
/// Nyquist, where even the first upper sideband has little room. It is a
/// conservative bright-note policy, not a claim that arbitrary deep PM at a
/// low base frequency is alias-free.
enum class FmOperatorAliasPolicy : std::uint8_t {
    bounded,
    bright_band_safe,
};

/// Fixed-capacity routing for an operator network.
///
/// Matrix indices are `[destination][source]`. Phase entries are radians of
/// phase deviation per unit source output. Frequency entries are Hz of
/// instantaneous-frequency deviation per unit source output. Both may be
/// active on the same edge; keeping separate matrices makes the often-blurred
/// distinction between phase modulation and frequency modulation explicit.
/// Carrier gains are linear. When their sum exceeds unity, the engine divides
/// the weighted output by that sum, so adding full-scale carriers does not
/// silently change level; a sub-unity sum remains intentional attenuation.
template <typename SampleType, std::size_t MaxOperators = 8> class FmOperatorRoutingT {
  public:
    static_assert(std::is_floating_point_v<SampleType>);
    static_assert(MaxOperators > 0 && MaxOperators <= 64);

    static constexpr SampleType kMaxPhaseDeviationRadians =
        SampleType{8} * std::numbers::pi_v<SampleType>;
    static constexpr SampleType kMaxFrequencyDeviationHz = SampleType{96000};

    FmOperatorRoutingT() {
        carrier_gains_[0] = SampleType{1};
    }

    void clear() noexcept {
        for (auto& row : phase_radians_)
            row.fill(SampleType{0});
        for (auto& row : frequency_hz_)
            row.fill(SampleType{0});
        carrier_gains_.fill(SampleType{0});
    }

    void set_phase_modulation_radians(std::size_t destination, std::size_t source,
                                      SampleType radians) noexcept {
        if (!valid_index_(destination) || !valid_index_(source) || !std::isfinite(radians))
            return;
        phase_radians_[destination][source] =
            std::clamp(radians, -kMaxPhaseDeviationRadians, kMaxPhaseDeviationRadians);
    }

    void set_frequency_modulation_hz(std::size_t destination, std::size_t source,
                                     SampleType deviation_hz) noexcept {
        if (!valid_index_(destination) || !valid_index_(source) || !std::isfinite(deviation_hz))
            return;
        frequency_hz_[destination][source] =
            std::clamp(deviation_hz, -kMaxFrequencyDeviationHz, kMaxFrequencyDeviationHz);
    }

    void set_carrier_gain(std::size_t op, SampleType linear_gain) noexcept {
        if (!valid_index_(op) || !std::isfinite(linear_gain))
            return;
        carrier_gains_[op] = std::clamp(linear_gain, SampleType{0}, SampleType{1});
    }

    SampleType phase_modulation_radians(std::size_t destination,
                                        std::size_t source) const noexcept {
        return valid_index_(destination) && valid_index_(source)
                   ? phase_radians_[destination][source]
                   : SampleType{0};
    }

    SampleType frequency_modulation_hz(std::size_t destination, std::size_t source) const noexcept {
        return valid_index_(destination) && valid_index_(source)
                   ? frequency_hz_[destination][source]
                   : SampleType{0};
    }

    SampleType carrier_gain(std::size_t op) const noexcept {
        return valid_index_(op) ? carrier_gains_[op] : SampleType{0};
    }

  private:
    static constexpr bool valid_index_(std::size_t index) noexcept {
        return index < MaxOperators;
    }

    std::array<std::array<SampleType, MaxOperators>, MaxOperators> phase_radians_{};
    std::array<std::array<SampleType, MaxOperators>, MaxOperators> frequency_hz_{};
    std::array<SampleType, MaxOperators> carrier_gains_{};
};

namespace detail {

template <typename SampleType>
inline SampleType fm_bright_band_gain(SampleType base_hz, SampleType sample_rate,
                                      FmOperatorAliasPolicy policy) noexcept {
    if (policy == FmOperatorAliasPolicy::bounded)
        return SampleType{1};
    const SampleType nyquist = sample_rate * SampleType{0.5};
    const SampleType normalized = std::abs(base_hz) / nyquist;
    constexpr SampleType start = SampleType{0.65};
    constexpr SampleType end = SampleType{0.90};
    if (normalized <= start)
        return SampleType{1};
    if (normalized >= end)
        return SampleType{0};
    const SampleType x = (normalized - start) / (end - start);
    const SampleType smooth = x * x * (SampleType{3} - SampleType{2} * x);
    return SampleType{1} - smooth;
}

/// Canonical one-sample-delayed operator evaluator shared by melodic and drum
/// engines. Delaying every edge uniformly permits cycles and makes evaluation
/// order irrelevant; it is the routing law already used by the 6/8-op drums.
template <typename SampleType, std::size_t MaxOperators, typename FrequencyFn,
          typename PhaseRouteFn, typename FrequencyRouteFn, typename CarrierGainFn,
          typename OscillatorFn>
SampleType render_fm_operator_sample(
    std::size_t operator_count, SampleType sample_rate, FmOperatorAliasPolicy alias_policy,
    std::array<SampleType, MaxOperators>& phases, std::array<SampleType, MaxOperators>& previous,
    const std::array<SampleType, MaxOperators>& amplitudes,
    const std::array<SampleType, MaxOperators>& feedback_radians, FrequencyFn&& base_frequency,
    PhaseRouteFn&& phase_route, FrequencyRouteFn&& frequency_route, CarrierGainFn&& carrier_gain,
    OscillatorFn&& oscillator) noexcept {
    std::array<SampleType, MaxOperators> current{};
    const auto fail_closed = [&]() noexcept {
        phases.fill(SampleType{0});
        previous.fill(SampleType{0});
        return SampleType{0};
    };
    if (operator_count > MaxOperators || !std::isfinite(sample_rate) ||
        sample_rate <= SampleType{0})
        return fail_closed();
    const SampleType frequency_limit = sample_rate * SampleType{0.49};

    for (std::size_t destination = 0; destination < operator_count; ++destination) {
        const SampleType base_hz = base_frequency(destination);
        if (!std::isfinite(base_hz) || !std::isfinite(amplitudes[destination]) ||
            !std::isfinite(feedback_radians[destination]))
            return fail_closed();
        SampleType phase_offset = SampleType{0};
        SampleType frequency_offset = SampleType{0};
        for (std::size_t source = 0; source < operator_count; ++source) {
            if (!std::isfinite(previous[source]))
                return fail_closed();
            const SampleType phase_depth = phase_route(destination, source);
            const SampleType frequency_depth = frequency_route(destination, source);
            if (!std::isfinite(phase_depth) || !std::isfinite(frequency_depth))
                return fail_closed();
            phase_offset += previous[source] * phase_depth;
            frequency_offset += previous[source] * frequency_depth;
            if (!std::isfinite(phase_offset) || !std::isfinite(frequency_offset))
                return fail_closed();
        }
        const SampleType modulation_gain = fm_bright_band_gain(
            std::abs(base_hz) + std::abs(frequency_offset), sample_rate, alias_policy);
        phase_offset *= modulation_gain;
        frequency_offset *= modulation_gain;
        phase_offset += previous[destination] * feedback_radians[destination] *
                        fm_bright_band_gain(base_hz, sample_rate, alias_policy);
        if (!std::isfinite(phase_offset) || !std::isfinite(frequency_offset))
            return fail_closed();

        const SampleType instantaneous_hz =
            std::clamp(base_hz + frequency_offset, -frequency_limit, frequency_limit);
        phases[destination] += instantaneous_hz / sample_rate;
        phases[destination] -= std::floor(phases[destination]);
        if (!std::isfinite(phases[destination]))
            return fail_closed();
        current[destination] = oscillator(destination, phases[destination], phase_offset,
                                          instantaneous_hz / sample_rate);
        current[destination] *= amplitudes[destination];
        if (!std::isfinite(current[destination]))
            return fail_closed();
    }

    SampleType output = SampleType{0};
    SampleType carrier_sum = SampleType{0};
    for (std::size_t op = 0; op < operator_count; ++op) {
        const SampleType gain = carrier_gain(op);
        if (!std::isfinite(gain) || gain < SampleType{0})
            return fail_closed();
        output += current[op] * gain;
        carrier_sum += gain;
        if (!std::isfinite(output) || !std::isfinite(carrier_sum))
            return fail_closed();
    }
    if (carrier_sum > SampleType{1})
        output /= carrier_sum;

    for (std::size_t op = 0; op < operator_count; ++op)
        current[op] = snap_to_zero(current[op]);

    previous = current;
    return snap_to_zero(output);
}

} // namespace detail

/// Bounded melodic FM/PM operator engine.
///
/// Up to `MaxOperators` sine operators share the same fixed-capacity routing
/// representation. Every route reads the source's previous sample, matching
/// Pulp's drum FM6/FM8 law and permitting arbitrary cycles without a topology
/// sort. Ratio mode follows `base_frequency_hz`; fixed-Hz mode does not. Each
/// operator has a DAHDSR envelope, a linear level, key scaling in dB/octave,
/// and bounded self-feedback in radians.
///
/// RT contract: construction, `prepare()`, and all setters own fixed storage
/// and allocate nothing. `note_on()`, `note_off()`, `process()`, and `reset()`
/// allocate nothing, take no locks, perform no I/O, and have work bounded by
/// `MaxOperators * MaxOperators`. Setters are control/setup operations.
///
/// Time contract: the engine has zero processing latency. After `note_off()`,
/// `tail_samples()` is the exact longest configured linear release in samples.
/// Reset produces exact silence and restores the same phase/envelope state as a
/// fresh prepared engine.
template <typename SampleType = float, std::size_t MaxOperators = 8> class FmOperatorEngineT {
  public:
    static_assert(std::is_floating_point_v<SampleType>);
    static_assert(MaxOperators > 0 && MaxOperators <= 64);

    using Routing = FmOperatorRoutingT<SampleType, MaxOperators>;
    using FrequencyMode = FmOperatorFrequencyMode;
    using AliasPolicy = FmOperatorAliasPolicy;
    using TrigProfile = FastTrigProfile;

    struct Envelope {
        double delay_ms = 0.0;
        double attack_ms = 5.0;
        double hold_ms = 0.0;
        double decay_ms = 100.0;
        SampleType sustain = SampleType{1};
        double release_ms = 100.0;
        float attack_curve = 0.0f;
        float decay_curve = 0.0f;
        float release_curve = 0.0f;
    };

    static constexpr std::size_t max_operator_count() noexcept {
        return MaxOperators;
    }
    static constexpr bool supports_realtime_trig_profiles() noexcept {
        return std::is_same_v<SampleType, float>;
    }
    static constexpr SampleType kMinRatio = SampleType{1} / SampleType{64};
    static constexpr SampleType kMaxRatio = SampleType{64};
    static constexpr SampleType kMinFrequencyHz = SampleType{0.01};
    static constexpr SampleType kMaxFrequencyHz = SampleType{96000};
    static constexpr SampleType kMaxFeedbackRadians =
        SampleType{8} * std::numbers::pi_v<SampleType>;
    static constexpr SampleType kMaxKeyScalingDbPerOctave = SampleType{24};
    static constexpr SampleType kMaxKeyScalingMagnitudeDb = SampleType{60};
    static constexpr double kMinSampleRate = 1000.0;
    static constexpr double kMaxSampleRate = 768000.0;

    FmOperatorEngineT() {
        ratios_.fill(SampleType{1});
        fixed_hz_.fill(SampleType{440});
        levels_.fill(SampleType{1});
        key_reference_hz_.fill(SampleType{440});
        apply_envelopes_();
    }

    void prepare(double sample_rate) noexcept {
        if (std::isfinite(sample_rate) && sample_rate > 0.0)
            sample_rate_ = std::clamp(sample_rate, kMinSampleRate, kMaxSampleRate);
        for (auto& envelope : envelopes_)
            envelope.prepare(sample_rate_);
        reset();
    }

    void reset() noexcept {
        phases_.fill(SampleType{0});
        previous_.fill(SampleType{0});
        for (auto& envelope : envelopes_)
            envelope.reset();
    }

    void note_on(SampleType velocity = SampleType{1}) noexcept {
        if (!std::isfinite(velocity))
            velocity = SampleType{0};
        velocity = std::clamp(velocity, SampleType{0}, SampleType{1});
        phases_.fill(SampleType{0});
        previous_.fill(SampleType{0});
        for (std::size_t op = 0; op < operator_count_; ++op)
            envelopes_[op].note_on(velocity);
    }

    void note_off() noexcept {
        for (std::size_t op = 0; op < operator_count_; ++op)
            envelopes_[op].note_off();
    }

    void set_operator_count(std::size_t count) noexcept {
        const std::size_t bounded = std::clamp(count, std::size_t{1}, MaxOperators);
        if (bounded == operator_count_)
            return;
        operator_count_ = bounded;
        // Operator-count changes are setup-time topology changes. Reset the
        // whole voice so an operator cannot resume a stale envelope or phase
        // after being disabled and later re-enabled.
        reset();
    }
    std::size_t operator_count() const noexcept {
        return operator_count_;
    }

    void set_base_frequency_hz(SampleType hz) noexcept {
        if (!std::isfinite(hz))
            return;
        base_frequency_hz_ = std::clamp(hz, kMinFrequencyHz, kMaxFrequencyHz);
    }
    SampleType base_frequency_hz() const noexcept {
        return base_frequency_hz_;
    }

    void set_operator_frequency_mode(std::size_t op, FrequencyMode mode) noexcept {
        if (op >= MaxOperators || (mode != FrequencyMode::ratio && mode != FrequencyMode::fixed_hz))
            return;
        frequency_modes_[op] = mode;
    }
    FrequencyMode operator_frequency_mode(std::size_t op) const noexcept {
        return op < MaxOperators ? frequency_modes_[op] : FrequencyMode::ratio;
    }

    void set_operator_ratio(std::size_t op, SampleType ratio) noexcept {
        if (op >= MaxOperators || !std::isfinite(ratio))
            return;
        ratios_[op] = std::clamp(ratio, kMinRatio, kMaxRatio);
    }
    SampleType operator_ratio(std::size_t op) const noexcept {
        return op < MaxOperators ? ratios_[op] : SampleType{1};
    }

    void set_operator_fixed_frequency_hz(std::size_t op, SampleType hz) noexcept {
        if (op >= MaxOperators || !std::isfinite(hz))
            return;
        fixed_hz_[op] = std::clamp(hz, kMinFrequencyHz, kMaxFrequencyHz);
    }
    SampleType operator_fixed_frequency_hz(std::size_t op) const noexcept {
        return op < MaxOperators ? fixed_hz_[op] : SampleType{0};
    }

    void set_operator_level(std::size_t op, SampleType linear_level) noexcept {
        if (op >= MaxOperators || !std::isfinite(linear_level))
            return;
        levels_[op] = std::clamp(linear_level, SampleType{0}, SampleType{1});
    }
    SampleType operator_level(std::size_t op) const noexcept {
        return op < MaxOperators ? levels_[op] : SampleType{0};
    }

    void set_operator_feedback_radians(std::size_t op, SampleType radians) noexcept {
        if (op >= MaxOperators || !std::isfinite(radians))
            return;
        feedback_radians_[op] = std::clamp(radians, -kMaxFeedbackRadians, kMaxFeedbackRadians);
    }
    SampleType operator_feedback_radians(std::size_t op) const noexcept {
        return op < MaxOperators ? feedback_radians_[op] : SampleType{0};
    }

    void set_operator_key_scaling(std::size_t op, SampleType db_per_octave,
                                  SampleType reference_hz = SampleType{440}) noexcept {
        if (op >= MaxOperators || !std::isfinite(db_per_octave) || !std::isfinite(reference_hz))
            return;
        key_scaling_db_per_octave_[op] =
            std::clamp(db_per_octave, -kMaxKeyScalingDbPerOctave, kMaxKeyScalingDbPerOctave);
        key_reference_hz_[op] = std::clamp(reference_hz, kMinFrequencyHz, kMaxFrequencyHz);
    }

    void set_operator_envelope(std::size_t op, Envelope envelope) noexcept {
        if (op >= MaxOperators)
            return;
        sanitize_envelope_(envelope);
        envelope_specs_[op] = envelope;
        apply_envelope_(op);
    }
    const Envelope& operator_envelope(std::size_t op) const noexcept {
        return envelope_specs_[std::min(op, MaxOperators - 1)];
    }

    void set_routing(const Routing& routing) noexcept {
        routing_ = routing;
    }
    Routing& routing() noexcept {
        return routing_;
    }
    const Routing& routing() const noexcept {
        return routing_;
    }

    void set_alias_policy(AliasPolicy policy) noexcept {
        if (policy != AliasPolicy::bounded && policy != AliasPolicy::bright_band_safe)
            return;
        alias_policy_ = policy;
    }
    AliasPolicy alias_policy() const noexcept {
        return alias_policy_;
    }

    /// Selects the sine implementation used by this engine instance.
    ///
    /// Reference math remains the default. The real-time profiles are available
    /// only for float engines because their adapted coefficients and measured
    /// error budgets are float-specific. Selection is a setup/control operation
    /// and is dispatched outside the operator loop. Recursive FM can amplify
    /// even small numerical differences, so non-reference profiles are explicit
    /// sound/performance alternatives rather than transparent replacements.
    bool set_trig_profile(TrigProfile profile) noexcept {
        if (profile != TrigProfile::reference && profile != TrigProfile::realtime_efficient &&
            profile != TrigProfile::realtime_precise)
            return false;
        if constexpr (!supports_realtime_trig_profiles()) {
            if (profile != TrigProfile::reference)
                return false;
        }
        trig_profile_ = profile;
        return true;
    }
    TrigProfile trig_profile() const noexcept {
        return trig_profile_;
    }

    bool is_active() const noexcept {
        for (std::size_t op = 0; op < operator_count_; ++op) {
            if (envelopes_[op].active())
                return true;
        }
        return false;
    }

    int latency_samples() const noexcept {
        return 0;
    }

    int tail_samples() const noexcept {
        long double longest = 0.0L;
        for (std::size_t op = 0; op < operator_count_; ++op) {
            const long double samples = static_cast<long double>(envelope_specs_[op].release_ms) *
                                        0.001L * static_cast<long double>(sample_rate_);
            longest = std::max(longest, std::round(samples));
        }
        return longest > static_cast<long double>(std::numeric_limits<int>::max())
                   ? std::numeric_limits<int>::max()
                   : static_cast<int>(longest);
    }

    SampleType process() noexcept {
        switch (trig_profile_) {
            case TrigProfile::realtime_efficient:
                if constexpr (supports_realtime_trig_profiles())
                    return process_one_<TrigProfile::realtime_efficient>();
                break;
            case TrigProfile::realtime_precise:
                if constexpr (supports_realtime_trig_profiles())
                    return process_one_<TrigProfile::realtime_precise>();
                break;
            case TrigProfile::reference: break;
        }
        return process_one_<TrigProfile::reference>();
    }

    void process(SampleType* output, std::size_t sample_count) noexcept {
        if (output == nullptr)
            return;
        switch (trig_profile_) {
            case TrigProfile::realtime_efficient:
                if constexpr (supports_realtime_trig_profiles()) {
                    process_block_<TrigProfile::realtime_efficient>(output, sample_count);
                    return;
                }
                break;
            case TrigProfile::realtime_precise:
                if constexpr (supports_realtime_trig_profiles()) {
                    process_block_<TrigProfile::realtime_precise>(output, sample_count);
                    return;
                }
                break;
            case TrigProfile::reference: break;
        }
        process_block_<TrigProfile::reference>(output, sample_count);
    }

  private:
    template <TrigProfile Profile> SampleType process_one_() noexcept {
        if (!is_active())
            return SampleType{0};

        std::array<SampleType, MaxOperators> amplitudes{};
        for (std::size_t op = 0; op < operator_count_; ++op) {
            const SampleType key_gain = key_gain_(op);
            amplitudes[op] = envelopes_[op].next() * levels_[op] * key_gain;
        }

        const auto frequency = [this](std::size_t op) noexcept {
            return frequency_modes_[op] == FrequencyMode::ratio ? base_frequency_hz_ * ratios_[op]
                                                                : fixed_hz_[op];
        };
        const auto phase_route = [this](std::size_t destination, std::size_t source) noexcept {
            return routing_.phase_modulation_radians(destination, source);
        };
        const auto frequency_route = [this](std::size_t destination, std::size_t source) noexcept {
            return routing_.frequency_modulation_hz(destination, source);
        };
        const auto carrier = [this](std::size_t op) noexcept { return routing_.carrier_gain(op); };
        const auto sine = [](std::size_t, SampleType phase_cycles, SampleType phase_offset_radians,
                             SampleType) noexcept {
            if constexpr (std::is_same_v<SampleType, float> &&
                          Profile != TrigProfile::reference) {
                constexpr float inverse_two_pi = 0.15915494309189533577f;
                float cycles = phase_cycles + phase_offset_radians * inverse_two_pi;
                cycles -= std::floor(cycles);
                return FastMath::sin_cycles<Profile>(cycles);
            } else {
                constexpr SampleType two_pi = SampleType{2} * std::numbers::pi_v<SampleType>;
                return std::sin(two_pi * phase_cycles + phase_offset_radians);
            }
        };

        return detail::render_fm_operator_sample(
            operator_count_, static_cast<SampleType>(sample_rate_), alias_policy_, phases_,
            previous_, amplitudes, feedback_radians_, frequency, phase_route, frequency_route,
            carrier, sine);
    }

    template <TrigProfile Profile>
    void process_block_(SampleType* output, std::size_t sample_count) noexcept {
        for (std::size_t sample = 0; sample < sample_count; ++sample)
            output[sample] = process_one_<Profile>();
    }
    static void sanitize_envelope_(Envelope& envelope) noexcept {
        if (!std::isfinite(envelope.delay_ms))
            envelope.delay_ms = 0.0;
        if (!std::isfinite(envelope.attack_ms))
            envelope.attack_ms = 0.0;
        if (!std::isfinite(envelope.hold_ms))
            envelope.hold_ms = 0.0;
        if (!std::isfinite(envelope.decay_ms))
            envelope.decay_ms = 0.0;
        if (!std::isfinite(envelope.release_ms))
            envelope.release_ms = 0.0;
        if (!std::isfinite(envelope.sustain))
            envelope.sustain = SampleType{0};
        if (!std::isfinite(envelope.attack_curve))
            envelope.attack_curve = 0.0f;
        if (!std::isfinite(envelope.decay_curve))
            envelope.decay_curve = 0.0f;
        if (!std::isfinite(envelope.release_curve))
            envelope.release_curve = 0.0f;
        envelope.delay_ms = std::clamp(envelope.delay_ms, 0.0, 60000.0);
        envelope.attack_ms = std::clamp(envelope.attack_ms, 0.0, 60000.0);
        envelope.hold_ms = std::clamp(envelope.hold_ms, 0.0, 60000.0);
        envelope.decay_ms = std::clamp(envelope.decay_ms, 0.0, 60000.0);
        envelope.release_ms = std::clamp(envelope.release_ms, 0.0, 60000.0);
        envelope.sustain = std::clamp(envelope.sustain, SampleType{0}, SampleType{1});
        envelope.attack_curve = std::clamp(envelope.attack_curve, -1.0f, 1.0f);
        envelope.decay_curve = std::clamp(envelope.decay_curve, -1.0f, 1.0f);
        envelope.release_curve = std::clamp(envelope.release_curve, -1.0f, 1.0f);
    }

    void apply_envelope_(std::size_t op) noexcept {
        auto& envelope = envelopes_[op];
        const auto& spec = envelope_specs_[op];
        envelope.set_delay_ms(spec.delay_ms);
        envelope.set_attack_ms(spec.attack_ms);
        envelope.set_hold_ms(spec.hold_ms);
        envelope.set_decay_ms(spec.decay_ms);
        envelope.set_sustain(spec.sustain);
        envelope.set_release_ms(spec.release_ms);
        envelope.set_attack_curve(spec.attack_curve);
        envelope.set_decay_curve(spec.decay_curve);
        envelope.set_release_curve(spec.release_curve);
    }

    void apply_envelopes_() noexcept {
        for (std::size_t op = 0; op < MaxOperators; ++op)
            apply_envelope_(op);
    }

    SampleType key_gain_(std::size_t op) const noexcept {
        const SampleType octaves = std::log2(base_frequency_hz_ / key_reference_hz_[op]);
        const SampleType db = std::clamp(key_scaling_db_per_octave_[op] * octaves,
                                         -kMaxKeyScalingMagnitudeDb, kMaxKeyScalingMagnitudeDb);
        return std::pow(SampleType{10}, db / SampleType{20});
    }

    double sample_rate_ = 48000.0;
    std::size_t operator_count_ = 1;
    SampleType base_frequency_hz_ = SampleType{440};
    AliasPolicy alias_policy_ = AliasPolicy::bright_band_safe;
    TrigProfile trig_profile_ = TrigProfile::reference;
    Routing routing_{};

    std::array<FrequencyMode, MaxOperators> frequency_modes_{};
    std::array<SampleType, MaxOperators> ratios_{};
    std::array<SampleType, MaxOperators> fixed_hz_{};
    std::array<SampleType, MaxOperators> levels_{};
    std::array<SampleType, MaxOperators> feedback_radians_{};
    std::array<SampleType, MaxOperators> key_scaling_db_per_octave_{};
    std::array<SampleType, MaxOperators> key_reference_hz_{};
    std::array<Envelope, MaxOperators> envelope_specs_{};
    std::array<DahdsrT<SampleType>, MaxOperators> envelopes_{};
    std::array<SampleType, MaxOperators> phases_{};
    std::array<SampleType, MaxOperators> previous_{};
};

using FmOperatorRouting = FmOperatorRoutingT<float>;
using FmOperatorRouting64 = FmOperatorRoutingT<double>;
using FmOperatorEngine = FmOperatorEngineT<float>;
using FmOperatorEngine64 = FmOperatorEngineT<double>;

} // namespace pulp::signal
