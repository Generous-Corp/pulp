#pragma once

#include <pulp/music/detail/random_range.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>

namespace pulp::music {

enum class MarkovError : std::uint8_t {
    none = 0,
    empty_model,
    capacity_exceeded,
    weight_count_mismatch,
    empty_transition_row,
};

// Fixed-capacity prepared transition table. prepare() is the control-thread
// operation; next() is const, bounded, allocation-free evaluation.
template <std::size_t MaxStates = 16>
class PreparedMarkovModel {
  public:
    static_assert(MaxStates > 0, "Markov capacity must be positive");
    static_assert(MaxStates <= std::numeric_limits<std::size_t>::max() / MaxStates,
                  "Markov table dimensions overflow size_t");
    static constexpr std::size_t capacity = MaxStates;

    [[nodiscard]] constexpr MarkovError
    prepare(std::size_t state_count,
            std::span<const std::uint32_t> weights) noexcept {
        clear();
        if (state_count == 0)
            return MarkovError::empty_model;
        if (state_count > MaxStates)
            return MarkovError::capacity_exceeded;
        if (weights.size() != state_count * state_count)
            return MarkovError::weight_count_mismatch;

        for (std::size_t row = 0; row < state_count; ++row) {
            std::uint64_t total = 0;
            for (std::size_t column = 0; column < state_count; ++column) {
                const auto weight = weights[row * state_count + column];
                total += weight;
                cumulative_[row * MaxStates + column] = total;
            }
            if (total == 0) {
                clear();
                return MarkovError::empty_transition_row;
            }
            totals_[row] = total;
        }
        state_count_ = state_count;
        return MarkovError::none;
    }

    constexpr void clear() noexcept {
        cumulative_.fill(0);
        totals_.fill(0);
        state_count_ = 0;
    }

    constexpr std::size_t state_count() const noexcept { return state_count_; }

    [[nodiscard]] constexpr std::optional<std::size_t>
    next(std::size_t current_state, std::uint64_t random_word) const noexcept {
        if (current_state >= state_count_)
            return std::nullopt;
        const auto draw = detail::reduce_random_word(random_word, totals_[current_state]);
        for (std::size_t column = 0; column < state_count_; ++column)
            if (draw < cumulative_[current_state * MaxStates + column])
                return column;
        return std::nullopt;
    }

  private:
    std::array<std::uint64_t, MaxStates * MaxStates> cumulative_{};
    std::array<std::uint64_t, MaxStates> totals_{};
    std::size_t state_count_ = 0;
};

} // namespace pulp::music
