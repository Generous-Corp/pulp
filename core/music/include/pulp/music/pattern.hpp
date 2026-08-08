#pragma once

#include <pulp/music/detail/random_range.hpp>

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace pulp::music {

enum class PatternError : std::uint8_t {
    none = 0,
    empty_pattern,
    capacity_exceeded,
    pulses_exceed_steps,
    invalid_probability,
    invalid_mode,
    unsupported_recipe_version,
};

template <std::size_t MaxSteps = 64>
class BinaryPattern {
  public:
    static_assert(MaxSteps > 0, "BinaryPattern capacity must be positive");
    static constexpr std::size_t capacity = MaxSteps;

    [[nodiscard]] constexpr PatternError resize(std::size_t size,
                                                bool fill = false) noexcept {
        if (size > MaxSteps)
            return PatternError::capacity_exceeded;
        values_.fill(0);
        size_ = size;
        for (std::size_t index = 0; index < size_; ++index)
            values_[index] = static_cast<std::uint8_t>(fill);
        return PatternError::none;
    }

    [[nodiscard]] constexpr PatternError
    assign(std::span<const std::uint8_t> values) noexcept {
        if (values.size() > MaxSteps)
            return PatternError::capacity_exceeded;
        values_.fill(0);
        size_ = values.size();
        for (std::size_t index = 0; index < size_; ++index)
            values_[index] = static_cast<std::uint8_t>(values[index] != 0);
        return PatternError::none;
    }

    constexpr std::size_t size() const noexcept { return size_; }
    constexpr bool empty() const noexcept { return size_ == 0; }

    [[nodiscard]] constexpr std::optional<bool> at(std::size_t index) const noexcept {
        if (index >= size_)
            return std::nullopt;
        return values_[index] != 0;
    }

    [[nodiscard]] constexpr bool set(std::size_t index, bool value) noexcept {
        if (index >= size_)
            return false;
        values_[index] = static_cast<std::uint8_t>(value);
        return true;
    }

    constexpr std::size_t onset_count() const noexcept {
        std::size_t result = 0;
        for (std::size_t index = 0; index < size_; ++index)
            result += values_[index] != 0;
        return result;
    }

    constexpr auto operator<=>(const BinaryPattern&) const = default;

  private:
    std::array<std::uint8_t, MaxSteps> values_{};
    std::size_t size_ = 0;
};

template <std::size_t MaxSteps = 64>
struct PatternResult {
    BinaryPattern<MaxSteps> pattern;
    PatternError error = PatternError::none;
    constexpr explicit operator bool() const noexcept {
        return error == PatternError::none;
    }
};

// Distributes pulses by the accumulator form of Euclid's algorithm. The
// canonical rotation starts with an onset. Positive rotation delays every onset
// by that many steps; negative rotation advances it. Zero pulses produce a
// silent pattern.
template <std::size_t MaxSteps = 64>
[[nodiscard]] constexpr PatternResult<MaxSteps>
euclidean_pattern(std::size_t steps, std::size_t pulses,
                  std::int64_t rotation = 0) noexcept {
    PatternResult<MaxSteps> result;
    if (steps == 0) {
        result.error = PatternError::empty_pattern;
        return result;
    }
    if (steps > MaxSteps) {
        result.error = PatternError::capacity_exceeded;
        return result;
    }
    if (pulses > steps) {
        result.error = PatternError::pulses_exceed_steps;
        return result;
    }
    (void)result.pattern.resize(steps);
    if (pulses == 0)
        return result;

    std::size_t bucket = steps - pulses;
    for (std::size_t step = 0; step < steps; ++step) {
        bucket += pulses;
        if (bucket >= steps) {
            bucket -= steps;
            (void)result.pattern.set(step, true);
        }
    }
    const auto signed_steps = static_cast<std::int64_t>(steps);
    auto normalized_rotation = rotation % signed_steps;
    if (normalized_rotation < 0)
        normalized_rotation += signed_steps;
    if (normalized_rotation != 0) {
        const auto shift = static_cast<std::size_t>(normalized_rotation);
        const auto canonical = result.pattern;
        for (std::size_t destination = 0; destination < steps; ++destination) {
            const auto source = (destination + steps - shift) % steps;
            (void)result.pattern.set(destination, *canonical.at(source));
        }
    }
    return result;
}

inline constexpr std::uint16_t euclidean_pattern_recipe_version = 1;

// These named scalar fields form the persistence contract. Object bytes are not
// a wire format: integrations serialize the fields individually and reject
// versions they do not understand.
struct EuclideanPatternRecipe {
    std::uint16_t version = euclidean_pattern_recipe_version;
    std::uint16_t steps = 16;
    std::uint16_t pulses = 4;
    std::int64_t rotation = 0;

    constexpr auto operator<=>(const EuclideanPatternRecipe&) const = default;
};

template <std::size_t MaxSteps = 64>
[[nodiscard]] constexpr PatternResult<MaxSteps>
materialize_pattern(EuclideanPatternRecipe recipe) noexcept {
    if (recipe.version != euclidean_pattern_recipe_version) {
        PatternResult<MaxSteps> result;
        result.error = PatternError::unsupported_recipe_version;
        return result;
    }
    return euclidean_pattern<MaxSteps>(recipe.steps, recipe.pulses, recipe.rotation);
}

enum class PatternWalkerMode : std::uint8_t {
    forward = 0,
    reverse,
    ping_pong,
    random,
};

template <std::size_t MaxSteps = 64>
class PatternWalker {
  public:
    static_assert(MaxSteps > 0, "PatternWalker capacity must be positive");

    [[nodiscard]] constexpr PatternError
    configure(std::size_t length, PatternWalkerMode mode) noexcept {
        if (length == 0) {
            length_ = 0;
            return PatternError::empty_pattern;
        }
        if (length > MaxSteps) {
            length_ = 0;
            return PatternError::capacity_exceeded;
        }
        if (mode != PatternWalkerMode::forward && mode != PatternWalkerMode::reverse
            && mode != PatternWalkerMode::ping_pong
            && mode != PatternWalkerMode::random) {
            length_ = 0;
            return PatternError::invalid_mode;
        }
        length_ = length;
        mode_ = mode;
        reset();
        return PatternError::none;
    }

    constexpr void reset() noexcept {
        cursor_ = mode_ == PatternWalkerMode::reverse && length_ > 0 ? length_ - 1 : 0;
        direction_ = 1;
    }

    [[nodiscard]] constexpr std::optional<std::size_t>
    next() noexcept {
        if (length_ == 0 || mode_ == PatternWalkerMode::random)
            return std::nullopt;
        return next_deterministic();
    }

    [[nodiscard]] constexpr std::optional<std::size_t>
    next(std::uint64_t random_word) noexcept {
        if (length_ == 0)
            return std::nullopt;
        if (mode_ == PatternWalkerMode::random)
            return static_cast<std::size_t>(
                detail::reduce_random_word(random_word, length_));
        return next_deterministic();
    }

    constexpr std::size_t length() const noexcept { return length_; }
    constexpr PatternWalkerMode mode() const noexcept { return mode_; }

  private:
    constexpr std::size_t next_deterministic() noexcept {
        const auto result = cursor_;
        switch (mode_) {
            case PatternWalkerMode::forward:
                cursor_ = (cursor_ + 1) % length_;
                break;
            case PatternWalkerMode::reverse:
                cursor_ = cursor_ == 0 ? length_ - 1 : cursor_ - 1;
                break;
            case PatternWalkerMode::ping_pong:
                advance_ping_pong();
                break;
            case PatternWalkerMode::random:
                break;
        }
        return result;
    }

    constexpr void advance_ping_pong() noexcept {
        if (length_ == 1)
            return;
        if (direction_ > 0 && cursor_ + 1 == length_) {
            direction_ = -1;
            --cursor_;
        } else if (direction_ < 0 && cursor_ == 0) {
            direction_ = 1;
            ++cursor_;
        } else if (direction_ > 0) {
            ++cursor_;
        } else {
            --cursor_;
        }
    }

    std::size_t length_ = 0;
    std::size_t cursor_ = 0;
    int direction_ = 1;
    PatternWalkerMode mode_ = PatternWalkerMode::forward;
};

enum class CellularBoundary : std::uint8_t {
    fixed_off = 0,
    wrap,
};

template <std::size_t MaxSteps = 64>
[[nodiscard]] constexpr PatternResult<MaxSteps>
cellular_evolve(const BinaryPattern<MaxSteps>& input, std::uint8_t rule,
                CellularBoundary boundary = CellularBoundary::wrap) noexcept {
    PatternResult<MaxSteps> result;
    if (input.empty()) {
        result.error = PatternError::empty_pattern;
        return result;
    }
    if (boundary != CellularBoundary::fixed_off && boundary != CellularBoundary::wrap) {
        result.error = PatternError::invalid_mode;
        return result;
    }
    (void)result.pattern.resize(input.size());
    for (std::size_t index = 0; index < input.size(); ++index) {
        const bool center = *input.at(index);
        const bool left = index > 0 ? *input.at(index - 1)
                                   : boundary == CellularBoundary::wrap
                                         ? *input.at(input.size() - 1)
                                         : false;
        const bool right = index + 1 < input.size() ? *input.at(index + 1)
                                                    : boundary == CellularBoundary::wrap
                                                          ? *input.at(0)
                                                          : false;
        const auto neighborhood = static_cast<unsigned>((left ? 4u : 0u)
                                                        | (center ? 2u : 0u)
                                                        | (right ? 1u : 0u));
        (void)result.pattern.set(index, ((rule >> neighborhood) & 1u) != 0);
    }
    return result;
}

struct MutationChance {
    std::uint32_t numerator = 0;
    std::uint32_t denominator = 1;
};

template <std::size_t MaxSteps = 64>
struct ShiftRegisterResult {
    BinaryPattern<MaxSteps> pattern;
    PatternError error = PatternError::none;
    bool output = false;
    bool mutated = false;
    constexpr explicit operator bool() const noexcept {
        return error == PatternError::none;
    }
};

// Rotates the final bit back to the first cell and optionally flips that copied
// bit according to the caller-supplied deterministic random word.
template <std::size_t MaxSteps = 64>
[[nodiscard]] constexpr ShiftRegisterResult<MaxSteps>
looping_shift_register(const BinaryPattern<MaxSteps>& input, MutationChance chance,
                       std::uint64_t random_word) noexcept {
    ShiftRegisterResult<MaxSteps> result;
    result.pattern = input;
    if (input.empty()) {
        result.error = PatternError::empty_pattern;
        return result;
    }
    if (chance.denominator == 0 || chance.numerator > chance.denominator) {
        result.error = PatternError::invalid_probability;
        return result;
    }

    result.output = *input.at(input.size() - 1);
    if (chance.numerator != 0) {
        result.mutated = chance.numerator == chance.denominator
                         || detail::reduce_random_word(random_word, chance.denominator)
                                < chance.numerator;
    }
    for (std::size_t index = input.size() - 1; index > 0; --index)
        (void)result.pattern.set(index, *input.at(index - 1));
    (void)result.pattern.set(0, result.output != result.mutated);
    return result;
}

} // namespace pulp::music
