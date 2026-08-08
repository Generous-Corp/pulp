#pragma once

#include <pulp/timebase/coordinate_random.hpp>

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace pulp::music {

using PatternEventId = std::uint64_t;

enum class PatternEventRole : std::uint8_t {
    anchor = 0,
    primary,
    ornament,
    fill,
};

struct PatternEvent {
    PatternEventId id = 0;
    timebase::TickPosition onset{};
    std::uint16_t accent = 1000;
    PatternEventRole role = PatternEventRole::primary;

    constexpr auto operator<=>(const PatternEvent&) const = default;
};

enum class PatternDevelopmentError : std::uint8_t {
    none = 0,
    capacity_exceeded,
    zero_event_id,
    duplicate_event_id,
    duplicate_onset,
    invalid_role,
    accent_out_of_range,
    invalid_set_operation,
    conflicting_event,
    density_out_of_range,
    density_below_anchor_count,
    invalid_region,
    morph_amount_out_of_range,
};

namespace detail {

constexpr bool valid_pattern_event_role(PatternEventRole role) noexcept {
    return role == PatternEventRole::anchor || role == PatternEventRole::primary ||
           role == PatternEventRole::ornament || role == PatternEventRole::fill;
}

constexpr bool event_less(const PatternEvent& lhs, const PatternEvent& rhs) noexcept {
    return lhs.onset < rhs.onset || (lhs.onset == rhs.onset && lhs.id < rhs.id);
}

constexpr std::uint64_t event_priority(std::uint64_t seed, timebase::RandomCoordinate coordinate,
                                       const PatternEvent& event) noexcept {
    coordinate.tick = event.onset;
    coordinate.stream ^= event.id;
    return timebase::coordinate_random(seed, coordinate);
}

constexpr std::uint64_t event_threshold(std::uint64_t seed, timebase::RandomCoordinate coordinate,
                                        const PatternEvent& event) noexcept {
    return timebase::detail::multiply_high(event_priority(seed, coordinate, event), 1001u);
}

constexpr std::uint64_t positive_distance(std::int64_t low, std::int64_t high) noexcept {
    if (low >= 0)
        return static_cast<std::uint64_t>(high - low);
    const auto low_magnitude = static_cast<std::uint64_t>(-(low + 1)) + 1u;
    if (high < 0)
        return low_magnitude - (static_cast<std::uint64_t>(-(high + 1)) + 1u);
    return low_magnitude + static_cast<std::uint64_t>(high);
}

constexpr std::int64_t add_unsigned(std::int64_t value, std::uint64_t amount) noexcept {
    if (value >= 0)
        return value + static_cast<std::int64_t>(amount);
    const auto magnitude = static_cast<std::uint64_t>(-(value + 1)) + 1u;
    if (amount < magnitude) {
        const auto remaining = magnitude - amount;
        return -static_cast<std::int64_t>(remaining - 1u) - 1;
    }
    return static_cast<std::int64_t>(amount - magnitude);
}

constexpr std::int64_t subtract_unsigned(std::int64_t value, std::uint64_t amount) noexcept {
    if (value <= 0) {
        const auto magnitude = static_cast<std::uint64_t>(-(value + 1)) + 1u;
        const auto result_magnitude = magnitude + amount;
        return -static_cast<std::int64_t>(result_magnitude - 1u) - 1;
    }
    const auto positive = static_cast<std::uint64_t>(value);
    if (amount <= positive)
        return static_cast<std::int64_t>(positive - amount);
    const auto magnitude = amount - positive;
    return -static_cast<std::int64_t>(magnitude - 1u) - 1;
}

constexpr std::uint64_t scale_distance(std::uint64_t distance, std::uint16_t amount) noexcept {
    return (distance / 1000u) * amount + ((distance % 1000u) * amount) / 1000u;
}

constexpr std::int64_t interpolate_integer(std::int64_t from, std::int64_t to,
                                           std::uint16_t amount) noexcept {
    if (amount == 0)
        return from;
    if (amount == 1000)
        return to;
    if (from < to)
        return add_unsigned(from, scale_distance(positive_distance(from, to), amount));
    return subtract_unsigned(from, scale_distance(positive_distance(to, from), amount));
}

} // namespace detail

// Produces a stable nonzero ID from a musical coordinate. Callers remain
// responsible for choosing distinct coordinates within one pattern.
[[nodiscard]] constexpr PatternEventId
make_pattern_event_id(std::uint64_t seed, timebase::RandomCoordinate coordinate) noexcept {
    const auto value = timebase::coordinate_random(seed, coordinate);
    return value == 0 ? PatternEventId{1} : value;
}

template <std::size_t MaxEvents = 64> class DevelopmentPattern {
  public:
    static_assert(MaxEvents > 0, "DevelopmentPattern capacity must be positive");
    static_assert(MaxEvents <= 64, "Pattern development is bounded to 64 events");
    static constexpr std::size_t capacity = MaxEvents;

    [[nodiscard]] constexpr PatternDevelopmentError
    assign(std::span<const PatternEvent> events) noexcept {
        if (events.size() > MaxEvents)
            return PatternDevelopmentError::capacity_exceeded;

        DevelopmentPattern candidate;
        for (const auto& event : events) {
            const auto error = candidate.insert(event);
            if (error != PatternDevelopmentError::none)
                return error;
        }
        *this = candidate;
        return PatternDevelopmentError::none;
    }

    [[nodiscard]] constexpr PatternDevelopmentError insert(PatternEvent event) noexcept {
        if (size_ == MaxEvents)
            return PatternDevelopmentError::capacity_exceeded;
        if (event.id == 0)
            return PatternDevelopmentError::zero_event_id;
        if (!detail::valid_pattern_event_role(event.role))
            return PatternDevelopmentError::invalid_role;
        if (event.accent > 1000)
            return PatternDevelopmentError::accent_out_of_range;
        for (std::size_t index = 0; index < size_; ++index) {
            if (events_[index].id == event.id)
                return PatternDevelopmentError::duplicate_event_id;
            if (events_[index].onset == event.onset)
                return PatternDevelopmentError::duplicate_onset;
        }

        std::size_t insertion = size_;
        while (insertion > 0 && detail::event_less(event, events_[insertion - 1])) {
            events_[insertion] = events_[insertion - 1];
            --insertion;
        }
        events_[insertion] = event;
        ++size_;
        return PatternDevelopmentError::none;
    }

    constexpr std::size_t size() const noexcept {
        return size_;
    }
    constexpr bool empty() const noexcept {
        return size_ == 0;
    }

    [[nodiscard]] constexpr std::optional<PatternEvent> event(std::size_t index) const noexcept {
        if (index >= size_)
            return std::nullopt;
        return events_[index];
    }

    [[nodiscard]] constexpr std::optional<PatternEvent> find_id(PatternEventId id) const noexcept {
        for (std::size_t index = 0; index < size_; ++index)
            if (events_[index].id == id)
                return events_[index];
        return std::nullopt;
    }

    [[nodiscard]] constexpr std::optional<PatternEvent>
    find_onset(timebase::TickPosition onset) const noexcept {
        for (std::size_t index = 0; index < size_; ++index)
            if (events_[index].onset == onset)
                return events_[index];
        return std::nullopt;
    }

    constexpr std::size_t anchor_count() const noexcept {
        std::size_t result = 0;
        for (std::size_t index = 0; index < size_; ++index)
            result += events_[index].role == PatternEventRole::anchor;
        return result;
    }

    constexpr auto operator<=>(const DevelopmentPattern&) const = default;

  private:
    std::array<PatternEvent, MaxEvents> events_{};
    std::size_t size_ = 0;
};

template <std::size_t MaxEvents = 64> struct PatternDevelopmentResult {
    DevelopmentPattern<MaxEvents> pattern;
    PatternDevelopmentError error = PatternDevelopmentError::none;

    constexpr explicit operator bool() const noexcept {
        return error == PatternDevelopmentError::none;
    }
};

enum class PatternSetOperation : std::uint8_t {
    set_union = 0,
    intersection,
    difference,
    symmetric_difference,
};

// Set membership is defined by exact onset. Intersection and difference retain
// the left record. Union rejects two different records at one onset so stable
// IDs and roles are never silently rewritten.
template <std::size_t MaxEvents = 64>
[[nodiscard]] constexpr PatternDevelopmentResult<MaxEvents>
pattern_set(const DevelopmentPattern<MaxEvents>& lhs, const DevelopmentPattern<MaxEvents>& rhs,
            PatternSetOperation operation) noexcept {
    PatternDevelopmentResult<MaxEvents> result;
    if (operation != PatternSetOperation::set_union &&
        operation != PatternSetOperation::intersection &&
        operation != PatternSetOperation::difference &&
        operation != PatternSetOperation::symmetric_difference) {
        result.error = PatternDevelopmentError::invalid_set_operation;
        return result;
    }

    for (std::size_t index = 0; index < lhs.size(); ++index) {
        const auto event = *lhs.event(index);
        const auto other = rhs.find_onset(event.onset);
        const bool include = operation == PatternSetOperation::set_union ||
                             (operation == PatternSetOperation::intersection && other) ||
                             (operation == PatternSetOperation::difference && !other) ||
                             (operation == PatternSetOperation::symmetric_difference && !other);
        if (other &&
            (operation == PatternSetOperation::set_union ||
             operation == PatternSetOperation::symmetric_difference) &&
            *other != event) {
            result.error = PatternDevelopmentError::conflicting_event;
            result.pattern = {};
            return result;
        }
        if (include) {
            result.error = result.pattern.insert(event);
            if (result.error != PatternDevelopmentError::none) {
                result.pattern = {};
                return result;
            }
        }
    }
    if (operation == PatternSetOperation::intersection ||
        operation == PatternSetOperation::difference)
        return result;
    for (std::size_t index = 0; index < rhs.size(); ++index) {
        const auto event = *rhs.event(index);
        if (lhs.find_onset(event.onset))
            continue;
        result.error = result.pattern.insert(event);
        if (result.error != PatternDevelopmentError::none) {
            result.pattern = {};
            return result;
        }
    }
    return result;
}

struct DensitySelection {
    std::size_t target_onsets = 0;
    std::uint64_t seed = 0;
    timebase::RandomCoordinate coordinate{};
};

// Anchors are mandatory. Every other event has one coordinate-derived rank, so
// selecting k events is an exact subset of selecting k+1 with the same recipe.
template <std::size_t MaxEvents = 64>
[[nodiscard]] constexpr PatternDevelopmentResult<MaxEvents>
select_pattern_density(const DevelopmentPattern<MaxEvents>& source,
                       DensitySelection selection) noexcept {
    PatternDevelopmentResult<MaxEvents> result;
    if (selection.target_onsets > source.size()) {
        result.error = PatternDevelopmentError::density_out_of_range;
        return result;
    }
    const auto anchors = source.anchor_count();
    if (selection.target_onsets < anchors) {
        result.error = PatternDevelopmentError::density_below_anchor_count;
        return result;
    }

    const auto selected_nonanchors = selection.target_onsets - anchors;
    for (std::size_t index = 0; index < source.size(); ++index) {
        const auto event = *source.event(index);
        bool include = event.role == PatternEventRole::anchor;
        if (!include) {
            const auto priority =
                detail::event_priority(selection.seed, selection.coordinate, event);
            std::size_t rank = 0;
            for (std::size_t other_index = 0; other_index < source.size(); ++other_index) {
                const auto other = *source.event(other_index);
                if (other.role == PatternEventRole::anchor)
                    continue;
                const auto other_priority =
                    detail::event_priority(selection.seed, selection.coordinate, other);
                rank += other_priority < priority ||
                        (other_priority == priority && other.id < event.id);
            }
            include = rank < selected_nonanchors;
        }
        if (include) {
            result.error = result.pattern.insert(event);
            if (result.error != PatternDevelopmentError::none)
                return result;
        }
    }
    return result;
}

struct RegionalFillSelection {
    timebase::TickPosition begin{};
    timebase::TickPosition end{};
    std::size_t target_region_onsets = 0;
    std::uint64_t seed = 0;
    timebase::RandomCoordinate coordinate{};
};

// Outside [begin,end) the base is byte-identical. Inside it, base anchors are
// mandatory and the union of base and candidate events is density-selected.
template <std::size_t MaxEvents = 64>
[[nodiscard]] constexpr PatternDevelopmentResult<MaxEvents>
apply_regional_fill(const DevelopmentPattern<MaxEvents>& base,
                    const DevelopmentPattern<MaxEvents>& candidates,
                    RegionalFillSelection selection) noexcept {
    PatternDevelopmentResult<MaxEvents> result;
    if (selection.begin >= selection.end) {
        result.error = PatternDevelopmentError::invalid_region;
        return result;
    }

    DevelopmentPattern<MaxEvents> region;
    for (std::size_t index = 0; index < base.size(); ++index) {
        const auto event = *base.event(index);
        if (event.onset < selection.begin || event.onset >= selection.end) {
            result.error = result.pattern.insert(event);
            if (result.error != PatternDevelopmentError::none) {
                result.pattern = {};
                return result;
            }
        } else {
            result.error = region.insert(event);
            if (result.error != PatternDevelopmentError::none) {
                result.pattern = {};
                return result;
            }
        }
    }
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        const auto event = *candidates.event(index);
        if (event.onset < selection.begin || event.onset >= selection.end)
            continue;
        const auto existing = region.find_onset(event.onset);
        if (existing) {
            if (*existing != event) {
                result.error = PatternDevelopmentError::conflicting_event;
                result.pattern = {};
                return result;
            }
            continue;
        }
        result.error = region.insert(event);
        if (result.error != PatternDevelopmentError::none) {
            result.pattern = {};
            return result;
        }
    }

    const auto selected = select_pattern_density(
        region, {selection.target_region_onsets, selection.seed, selection.coordinate});
    if (!selected) {
        result.error = selected.error;
        result.pattern = {};
        return result;
    }
    for (std::size_t index = 0; index < selected.pattern.size(); ++index) {
        result.error = result.pattern.insert(*selected.pattern.event(index));
        if (result.error != PatternDevelopmentError::none) {
            result.pattern = {};
            return result;
        }
    }
    return result;
}

struct PatternMorphSelection {
    std::uint16_t amount = 0;
    std::uint64_t seed = 0;
    timebase::RandomCoordinate coordinate{};
};

// Shared IDs interpolate exact integer onsets and accents. Events unique to A
// disappear and events unique to B appear at coordinate-keyed thresholds.
template <std::size_t MaxEvents = 64>
[[nodiscard]] constexpr PatternDevelopmentResult<MaxEvents>
morph_patterns(const DevelopmentPattern<MaxEvents>& a, const DevelopmentPattern<MaxEvents>& b,
               PatternMorphSelection selection) noexcept {
    PatternDevelopmentResult<MaxEvents> result;
    if (selection.amount > 1000) {
        result.error = PatternDevelopmentError::morph_amount_out_of_range;
        return result;
    }
    if (selection.amount == 0) {
        result.pattern = a;
        return result;
    }
    if (selection.amount == 1000) {
        result.pattern = b;
        return result;
    }

    for (std::size_t index = 0; index < a.size(); ++index) {
        auto event = *a.event(index);
        const auto other = b.find_id(event.id);
        bool include = true;
        if (other) {
            event.onset.value = detail::interpolate_integer(event.onset.value, other->onset.value,
                                                            selection.amount);
            const auto accent_delta =
                static_cast<std::int32_t>(other->accent) - static_cast<std::int32_t>(event.accent);
            event.accent = static_cast<std::uint16_t>(static_cast<std::int32_t>(event.accent) +
                                                      accent_delta * selection.amount / 1000);
            if (selection.amount >= 500)
                event.role = other->role;
        } else {
            const auto threshold =
                detail::event_threshold(selection.seed, selection.coordinate, event);
            include = selection.amount <= threshold;
        }
        if (include) {
            result.error = result.pattern.insert(event);
            if (result.error != PatternDevelopmentError::none) {
                result.pattern = {};
                return result;
            }
        }
    }
    for (std::size_t index = 0; index < b.size(); ++index) {
        const auto event = *b.event(index);
        if (a.find_id(event.id))
            continue;
        const auto threshold = detail::event_threshold(selection.seed, selection.coordinate, event);
        if (selection.amount <= threshold)
            continue;
        result.error = result.pattern.insert(event);
        if (result.error != PatternDevelopmentError::none) {
            result.pattern = {};
            return result;
        }
    }
    return result;
}

} // namespace pulp::music
