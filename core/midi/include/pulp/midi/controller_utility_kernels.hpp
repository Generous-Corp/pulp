#pragma once

#include <pulp/midi/utility_contract.hpp>

namespace pulp::midi {

using ControllerCurve = float (*)(float normalized, float amount) noexcept;

inline float linear_controller_curve(float normalized, float) noexcept {
    return std::clamp(normalized, 0.0f, 1.0f);
}

struct ControllerMapRule {
    std::uint8_t input_channel = 0;
    std::uint8_t input_cc = 1;
    std::uint8_t output_channel = 0;
    std::uint8_t output_cc = 1;
    float minimum = 0.0f;
    float maximum = 1.0f;
    float curve_amount = 0.0f;
    float smoothing_seconds = 0.0f;
    ControllerCurve curve = linear_controller_curve;
    bool enabled = false;
};

template <std::size_t MaximumMappings = 32> class ControllerMapper {
  public:
    static_assert(MaximumMappings > 0);
    static constexpr MidiUtilityContract contract() noexcept {
        return {MaximumMappings, MaximumMappings, MidiUtilityOverflowPolicy::DropUnstarted,
                MidiUtilitySameSampleOrder::InputStable,
                MidiUtilityTransportRequirement::MonotonicSamples};
    }

    bool set_rule(std::size_t index, ControllerMapRule rule) noexcept {
        if (index >= MaximumMappings)
            return false;
        if (rule.input_channel > 15 || rule.output_channel > 15 || rule.input_cc > 127 ||
            rule.output_cc > 127)
            return false;
        if (!std::isfinite(rule.minimum) || !std::isfinite(rule.maximum) ||
            !std::isfinite(rule.curve_amount) || !std::isfinite(rule.smoothing_seconds))
            return false;
        if (rule.minimum > rule.maximum)
            std::swap(rule.minimum, rule.maximum);
        if (rule.smoothing_seconds < 0.0f)
            rule.smoothing_seconds = 0.0f;
        if (rule.curve == nullptr)
            rule.curve = linear_controller_curve;
        rules_[index] = rule;
        states_[index] = {};
        return true;
    }

    MidiUtilityProcessReport process(const MidiBuffer& input, MidiBuffer& output,
                                     double sample_rate,
                                     timebase::SamplePosition block_start = {}) noexcept {
        if (utility_detail::blocks_alias(input, output))
            return {0, input.size(), 0, false};
        utility_detail::clear_output(output);
        MidiUtilityProcessReport report;
        if (!utility_detail::ready(output))
            return {0, input.size(), 0, false};
        if (!std::isfinite(sample_rate))
            return {0, input.size(), 0, false};
        for (const auto& event : input) {
            if (!event.is_cc()) {
                utility_detail::emit(output, event, report);
                continue;
            }
            bool mapped = false;
            for (std::size_t i = 0; i < MaximumMappings; ++i) {
                const auto& rule = rules_[i];
                if (!rule.enabled || rule.input_channel != event.channel() ||
                    rule.input_cc != event.cc_number())
                    continue;
                mapped = true;
                auto& state = states_[i];
                const float normalized = static_cast<float>(event.cc_value()) / 127.0f;
                const float raw_curve = rule.curve(normalized, rule.curve_amount);
                if (!std::isfinite(raw_curve)) {
                    ++report.dropped;
                    report.complete = false;
                    continue;
                }
                const float curved = std::clamp(raw_curve, 0.0f, 1.0f);
                const double target =
                    std::lerp(static_cast<double>(rule.minimum), static_cast<double>(rule.maximum),
                              static_cast<double>(curved));
                if (!std::isfinite(target)) {
                    ++report.dropped;
                    report.complete = false;
                    continue;
                }
                double current = target;
                if (!state.initialized || rule.smoothing_seconds <= 0.0f || sample_rate <= 0.0) {
                    current = target;
                } else {
                    const auto event_sample = utility_detail::saturating_sample_add(
                        block_start.value, event.sample_offset);
                    const auto elapsed =
                        nonnegative_sample_distance(event_sample, state.last_sample);
                    const double coefficient =
                        std::exp(-static_cast<double>(elapsed) /
                                 (sample_rate * static_cast<double>(rule.smoothing_seconds)));
                    current = std::lerp(target, state.current, coefficient);
                }
                if (!std::isfinite(current)) {
                    ++report.dropped;
                    report.complete = false;
                    continue;
                }
                state.target = target;
                state.current = current;
                state.initialized = true;
                state.last_sample =
                    utility_detail::saturating_sample_add(block_start.value, event.sample_offset);
                const int value =
                    static_cast<int>(std::lround(std::clamp(current, 0.0, 1.0) * 127.0));
                auto mapped_event = MidiEvent::cc(rule.output_channel, rule.output_cc,
                                                  static_cast<std::uint8_t>(value));
                mapped_event.sample_offset = event.sample_offset;
                utility_detail::emit(output, mapped_event, report);
            }
            if (!mapped)
                utility_detail::emit(output, event, report);
        }
        utility_detail::copy_sidecars(input, output, report);
        return report;
    }

    float value(std::size_t index) const noexcept {
        return index < MaximumMappings && states_[index].initialized
                   ? static_cast<float>(states_[index].current)
                   : 0.0f;
    }

    void reset() noexcept {
        states_.fill({});
    }
    void flush() noexcept {
        reset();
    }
    void hot_swap_reset() noexcept {
        reset();
    }

  private:
    static constexpr std::uint64_t nonnegative_sample_distance(std::int64_t later,
                                                               std::int64_t earlier) noexcept {
        if (later <= earlier)
            return 0;
        return static_cast<std::uint64_t>(later) - static_cast<std::uint64_t>(earlier);
    }

    struct State {
        double current = 0.0;
        double target = 0.0;
        std::int64_t last_sample = 0;
        bool initialized = false;
    };
    std::array<ControllerMapRule, MaximumMappings> rules_{};
    std::array<State, MaximumMappings> states_{};
};

struct ScaleAwareMpeSpec {
    float input_bend_range_semitones = MpeVoiceTracker::kDefaultMemberBendSemitones;
    float bend_range_degrees = 2.0f;
    float glide_seconds = 0.0f;
};

class ScaleAwareMpePitch {
  public:
    static constexpr MidiUtilityContract contract() noexcept {
        return {1, 1, MidiUtilityOverflowPolicy::DropUnstarted,
                MidiUtilitySameSampleOrder::InputStable,
                MidiUtilityTransportRequirement::MonotonicSamples};
    }

    explicit constexpr ScaleAwareMpePitch(ScaleAwareMpeSpec spec = {}) noexcept
        : spec_(sanitize(spec)) {}

    float target_cents(const MpeNoteState& note, const music::Scale& scale) noexcept {
        const int source_note = std::min<int>(note.note, 127);
        const int anchor = utility_detail::nearest_scale_note(source_note, scale);
        const float finite_bend =
            std::isfinite(note.pitch_bend_semitones) ? note.pitch_bend_semitones : 0.0f;
        const float normalized =
            std::clamp(finite_bend / spec_.input_bend_range_semitones, -1.0f, 1.0f);
        const float degree_position = normalized * spec_.bend_range_degrees;
        const int low_degree = static_cast<int>(std::floor(degree_position));
        const int high_degree = static_cast<int>(std::ceil(degree_position));
        const float fraction = degree_position - static_cast<float>(low_degree);
        const int anchor_degree = nearest_degree(anchor, scale);
        const auto low = scale.degree_to_semitones(anchor_degree + low_degree).value_or(0) -
                         scale.degree_to_semitones(anchor_degree).value_or(0);
        const auto high = scale.degree_to_semitones(anchor_degree + high_degree).value_or(0) -
                          scale.degree_to_semitones(anchor_degree).value_or(0);
        const double scale_bend =
            static_cast<double>(low) + static_cast<double>(high - low) * fraction;
        const double computed_cents =
            static_cast<double>((anchor - source_note) * 100) + scale_bend * 100.0;
        target_cents_ = std::isfinite(computed_cents) &&
                                std::abs(computed_cents) <=
                                    static_cast<double>(std::numeric_limits<float>::max())
                            ? static_cast<float>(computed_cents)
                            : 0.0f;
        if (!initialized_) {
            current_cents_ = target_cents_;
            initialized_ = true;
        }
        return target_cents_;
    }

    float advance(std::uint32_t samples, double sample_rate) noexcept {
        if (!initialized_)
            return 0.0f;
        if (spec_.glide_seconds <= 0.0f || sample_rate <= 0.0 || !std::isfinite(sample_rate)) {
            current_cents_ = target_cents_;
            return current_cents_;
        }
        const double coefficient =
            std::exp(-static_cast<double>(samples) /
                     (sample_rate * static_cast<double>(spec_.glide_seconds)));
        const double next = static_cast<double>(target_cents_) +
                            (static_cast<double>(current_cents_) - target_cents_) * coefficient;
        current_cents_ = std::isfinite(next) ? static_cast<float>(next) : target_cents_;
        return current_cents_;
    }

    float current_cents() const noexcept {
        return current_cents_;
    }
    void reset() noexcept {
        initialized_ = false;
        current_cents_ = target_cents_ = 0.0f;
    }
    void replace_spec(ScaleAwareMpeSpec spec) noexcept {
        spec_ = sanitize(spec);
        reset();
    }

  private:
    static constexpr ScaleAwareMpeSpec sanitize(ScaleAwareMpeSpec spec) noexcept {
        if (!(spec.input_bend_range_semitones > 0.0f) ||
            !std::isfinite(spec.input_bend_range_semitones))
            spec.input_bend_range_semitones = MpeVoiceTracker::kDefaultMemberBendSemitones;
        if (!(spec.bend_range_degrees >= 0.0f) || !std::isfinite(spec.bend_range_degrees))
            spec.bend_range_degrees = 0.0f;
        spec.bend_range_degrees = std::min(spec.bend_range_degrees, 128.0f);
        if (!(spec.glide_seconds >= 0.0f) || !std::isfinite(spec.glide_seconds))
            spec.glide_seconds = 0.0f;
        return spec;
    }

    static int nearest_degree(int note, const music::Scale& scale) noexcept {
        const int root = static_cast<int>(scale.root());
        const int approximate_octave = (note - root) / 12;
        const int center = approximate_octave * static_cast<int>(scale.degree_count());
        int best_degree = center;
        int best_distance = std::numeric_limits<int>::max();
        for (int degree = center - static_cast<int>(scale.degree_count());
             degree <= center + static_cast<int>(scale.degree_count()); ++degree) {
            const auto semitones = scale.degree_to_semitones(degree);
            if (!semitones)
                continue;
            const int candidate = root + *semitones;
            const int distance = std::abs(candidate - note);
            if (distance < best_distance || (distance == best_distance && candidate < note)) {
                best_degree = degree;
                best_distance = distance;
            }
        }
        return best_degree;
    }

    ScaleAwareMpeSpec spec_{};
    float current_cents_ = 0.0f;
    float target_cents_ = 0.0f;
    bool initialized_ = false;
};

} // namespace pulp::midi
