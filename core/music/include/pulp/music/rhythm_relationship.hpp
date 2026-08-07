#pragma once

#include <pulp/music/pattern.hpp>

#include <compare>
#include <cstddef>
#include <cstdint>

namespace pulp::music {

enum class RhythmRelationship : std::uint8_t {
    coincident = 0,
    complementary,
    independent,
};

enum class RhythmLengthMapping : std::uint8_t {
    wrap = 0,
    proportional,
};

enum class RhythmCollisionPolicy : std::uint8_t {
    allow_source_overlap = 0,
    avoid_source_overlap,
};

enum class RhythmDensityPolicy : std::uint8_t {
    preserve_candidates = 0,
    exact_onsets,
};

enum class RhythmRelationshipError : std::uint8_t {
    none = 0,
    empty_source,
    empty_target,
    capacity_exceeded,
    invalid_relationship,
    invalid_length_mapping,
    invalid_collision_policy,
    invalid_density_policy,
    insufficient_candidates,
};

struct RhythmDrawCoordinate {
    std::uint64_t seed = 0;
    std::int64_t cycle = 0;
    std::uint32_t lane = 0;

    constexpr auto operator<=>(const RhythmDrawCoordinate&) const = default;
};

struct RhythmRelationshipConfig {
    std::size_t target_steps = 16;
    RhythmRelationship relationship = RhythmRelationship::coincident;
    RhythmLengthMapping length_mapping = RhythmLengthMapping::wrap;
    std::int64_t phase_steps = 0;
    RhythmCollisionPolicy collision = RhythmCollisionPolicy::allow_source_overlap;
    RhythmDensityPolicy density = RhythmDensityPolicy::preserve_candidates;
    std::size_t target_onsets = 0;
    RhythmDrawCoordinate draw{};
};

template <std::size_t MaxSteps = 64> struct RhythmRelationshipResult {
    BinaryPattern<MaxSteps> pattern;
    RhythmRelationshipError error = RhythmRelationshipError::none;
    constexpr explicit operator bool() const noexcept {
        return error == RhythmRelationshipError::none;
    }
};

namespace detail {

// SplitMix64's stateless finalizer turns the decision coordinate into a draw;
// no mutable stream means lane evaluation order cannot change the result.
constexpr std::uint64_t rhythm_mix(std::uint64_t value) noexcept {
    value += 0x9E3779B97F4A7C15ull;
    value = (value ^ (value >> 30u)) * 0xBF58476D1CE4E5B9ull;
    value = (value ^ (value >> 27u)) * 0x94D049BB133111EBull;
    return value ^ (value >> 31u);
}

constexpr std::uint64_t rhythm_draw(RhythmDrawCoordinate coordinate,
                                    std::size_t target_step) noexcept {
    auto value = rhythm_mix(coordinate.seed);
    value = rhythm_mix(value ^ static_cast<std::uint64_t>(coordinate.cycle));
    value = rhythm_mix(value ^ (static_cast<std::uint64_t>(coordinate.lane) << 32u));
    return rhythm_mix(value ^ static_cast<std::uint64_t>(target_step));
}

constexpr bool valid_relationship(RhythmRelationship value) noexcept {
    return value == RhythmRelationship::coincident || value == RhythmRelationship::complementary ||
           value == RhythmRelationship::independent;
}

constexpr bool valid_length_mapping(RhythmLengthMapping value) noexcept {
    return value == RhythmLengthMapping::wrap || value == RhythmLengthMapping::proportional;
}

constexpr bool valid_collision_policy(RhythmCollisionPolicy value) noexcept {
    return value == RhythmCollisionPolicy::allow_source_overlap ||
           value == RhythmCollisionPolicy::avoid_source_overlap;
}

constexpr bool valid_density_policy(RhythmDensityPolicy value) noexcept {
    return value == RhythmDensityPolicy::preserve_candidates ||
           value == RhythmDensityPolicy::exact_onsets;
}

constexpr std::size_t delayed_step(std::size_t step, std::size_t length,
                                   std::int64_t phase_steps) noexcept {
    const auto signed_length = static_cast<std::int64_t>(length);
    auto phase = phase_steps % signed_length;
    if (phase < 0)
        phase += signed_length;
    return (step + length - static_cast<std::size_t>(phase)) % length;
}

template <std::size_t MaxSteps>
constexpr std::size_t mapped_source_step(const BinaryPattern<MaxSteps>& source,
                                         const RhythmRelationshipConfig& config,
                                         std::size_t target_step) noexcept {
    const auto shifted = delayed_step(target_step, config.target_steps, config.phase_steps);
    if (config.length_mapping == RhythmLengthMapping::wrap)
        return shifted % source.size();
    return shifted * source.size() / config.target_steps;
}

template <std::size_t MaxSteps>
constexpr bool relationship_candidate(const BinaryPattern<MaxSteps>& source,
                                      const RhythmRelationshipConfig& config,
                                      std::size_t target_step) noexcept {
    const auto source_step = mapped_source_step(source, config, target_step);
    const bool source_onset = *source.at(source_step);
    bool candidate = false;
    switch (config.relationship) {
    case RhythmRelationship::coincident:
        candidate = source_onset;
        break;
    case RhythmRelationship::complementary:
        candidate = !source_onset;
        break;
    case RhythmRelationship::independent:
        candidate = true;
        break;
    }
    return candidate &&
           (config.collision == RhythmCollisionPolicy::allow_source_overlap || !source_onset);
}

} // namespace detail

// Derives a target lane without allocation or hidden state. Positive phase
// delays the relationship on the target grid. Wrap mapping repeats source steps;
// proportional mapping scales the source cycle once across the target length.
template <std::size_t MaxSteps = 64>
[[nodiscard]] constexpr RhythmRelationshipResult<MaxSteps>
derive_rhythm_relationship(const BinaryPattern<MaxSteps>& source,
                           const RhythmRelationshipConfig& config) noexcept {
    RhythmRelationshipResult<MaxSteps> result;
    if (source.empty()) {
        result.error = RhythmRelationshipError::empty_source;
        return result;
    }
    if (config.target_steps == 0 || config.target_steps > MaxSteps) {
        result.error = config.target_steps == 0 ? RhythmRelationshipError::empty_target
                                                : RhythmRelationshipError::capacity_exceeded;
        return result;
    }
    if (!detail::valid_relationship(config.relationship)) {
        result.error = RhythmRelationshipError::invalid_relationship;
        return result;
    }
    if (!detail::valid_length_mapping(config.length_mapping)) {
        result.error = RhythmRelationshipError::invalid_length_mapping;
        return result;
    }
    if (!detail::valid_collision_policy(config.collision)) {
        result.error = RhythmRelationshipError::invalid_collision_policy;
        return result;
    }
    if (!detail::valid_density_policy(config.density)) {
        result.error = RhythmRelationshipError::invalid_density_policy;
        return result;
    }

    (void)result.pattern.resize(config.target_steps);
    std::size_t candidate_count = 0;
    for (std::size_t step = 0; step < config.target_steps; ++step)
        candidate_count += detail::relationship_candidate(source, config, step);

    if (config.density == RhythmDensityPolicy::exact_onsets &&
        config.target_onsets > candidate_count) {
        result.error = RhythmRelationshipError::insufficient_candidates;
        return result;
    }

    for (std::size_t step = 0; step < config.target_steps; ++step) {
        if (!detail::relationship_candidate(source, config, step))
            continue;
        if (config.density == RhythmDensityPolicy::preserve_candidates) {
            (void)result.pattern.set(step, true);
            continue;
        }

        const auto priority = detail::rhythm_draw(config.draw, step);
        std::size_t rank = 0;
        for (std::size_t other = 0; other < config.target_steps; ++other) {
            if (!detail::relationship_candidate(source, config, other))
                continue;
            const auto other_priority = detail::rhythm_draw(config.draw, other);
            rank += other_priority < priority || (other_priority == priority && other < step);
        }
        if (rank < config.target_onsets)
            (void)result.pattern.set(step, true);
    }
    return result;
}

} // namespace pulp::music
