#pragma once

#include <pulp/timebase/coordinate_random.hpp>

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace pulp::music {

/// Stable, nonzero identity for an event across pattern transformations.
using PatternEventId = std::uint64_t;

/// Describes how an event participates in density, fill, and morph operations.
enum class PatternEventRole : std::uint8_t {
    /// Mandatory structural event that density operations always retain.
    anchor = 0,
    /// Ordinary event eligible for deterministic selection.
    primary,
    /// Decorative event eligible for deterministic selection.
    ornament,
    /// Event intended to supply regional variation.
    fill,
};

/// One uniquely identified onset in a development pattern.
struct PatternEvent {
    /// Stable nonzero event identity.
    PatternEventId id = 0;
    /// Musical onset in timeline ticks.
    timebase::TickPosition onset{};
    /// Relative emphasis in the inclusive range `[0, 1000]`.
    std::uint16_t accent = 1000;
    /// Structural role used by development operations.
    PatternEventRole role = PatternEventRole::primary;

    constexpr auto operator<=>(const PatternEvent&) const = default;
};

/// Result codes returned by bounded pattern-development operations.
enum class PatternDevelopmentError : std::uint8_t {
    /// The operation completed successfully.
    none = 0,
    /// The result would exceed the pattern's fixed capacity.
    capacity_exceeded,
    /// An event used the reserved zero identity.
    zero_event_id,
    /// Two events used the same identity.
    duplicate_event_id,
    /// Two events occupied the same onset.
    duplicate_onset,
    /// An event role was outside the declared enumeration.
    invalid_role,
    /// An event accent exceeded 1000.
    accent_out_of_range,
    /// A set operation was outside the declared enumeration.
    invalid_set_operation,
    /// Two inputs disagreed about the event at one identity or onset.
    conflicting_event,
    /// A requested density exceeded the available event count.
    density_out_of_range,
    /// A requested density could not retain every anchor.
    density_below_anchor_count,
    /// A regional operation used an empty or reversed half-open interval.
    invalid_region,
    /// A morph amount exceeded 1000.
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

/// Produces a stable nonzero ID from a seed and musical coordinate.
///
/// Callers remain responsible for choosing distinct coordinates within one
/// pattern.
[[nodiscard]] constexpr PatternEventId
make_pattern_event_id(std::uint64_t seed, timebase::RandomCoordinate coordinate) noexcept {
    const auto value = timebase::coordinate_random(seed, coordinate);
    return value == 0 ? PatternEventId{1} : value;
}

/// Fixed-capacity, onset-ordered collection used by development operations.
template <std::size_t MaxEvents = 64> class DevelopmentPattern {
  public:
    static_assert(MaxEvents > 0, "DevelopmentPattern capacity must be positive");
    static_assert(MaxEvents <= 64, "Pattern development is bounded to 64 events");
    /// Maximum number of events the pattern can hold.
    static constexpr std::size_t capacity = MaxEvents;

    /// Replaces the pattern with validated events, preserving the old value on error.
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

    /// Inserts one validated event in onset order.
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

    /// Returns the number of stored events.
    constexpr std::size_t size() const noexcept {
        return size_;
    }
    /// Returns true when the pattern contains no events.
    constexpr bool empty() const noexcept {
        return size_ == 0;
    }

    /// Returns the event at an onset-ordered index, or no value when out of range.
    [[nodiscard]] constexpr std::optional<PatternEvent> event(std::size_t index) const noexcept {
        if (index >= size_)
            return std::nullopt;
        return events_[index];
    }

    /// Finds an event by stable identity.
    [[nodiscard]] constexpr std::optional<PatternEvent> find_id(PatternEventId id) const noexcept {
        for (std::size_t index = 0; index < size_; ++index)
            if (events_[index].id == id)
                return events_[index];
        return std::nullopt;
    }

    /// Finds an event at an exact timeline onset.
    [[nodiscard]] constexpr std::optional<PatternEvent>
    find_onset(timebase::TickPosition onset) const noexcept {
        for (std::size_t index = 0; index < size_; ++index)
            if (events_[index].onset == onset)
                return events_[index];
        return std::nullopt;
    }

    /// Returns the number of mandatory anchor events.
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

/// Pattern-development output paired with an explicit result code.
template <std::size_t MaxEvents = 64> struct PatternDevelopmentResult {
    /// Result pattern, empty when the operation cannot produce a valid value.
    DevelopmentPattern<MaxEvents> pattern;
    /// Operation result code.
    PatternDevelopmentError error = PatternDevelopmentError::none;

    /// Returns true when the operation completed successfully.
    constexpr explicit operator bool() const noexcept {
        return error == PatternDevelopmentError::none;
    }
};

/// Supported onset-keyed set operations.
enum class PatternSetOperation : std::uint8_t {
    /// Include events found in either input.
    set_union = 0,
    /// Include events found in both inputs.
    intersection,
    /// Include events found only in the left input.
    difference,
    /// Include events found in exactly one input.
    symmetric_difference,
};

/// Applies an onset-keyed set operation to two patterns.
///
/// Intersection and difference retain the left record. Union rejects two
/// different records at one onset so stable IDs and roles are never silently
/// rewritten.
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
        if (other && operation == PatternSetOperation::set_union && *other != event) {
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

/// Parameters for deterministic density selection.
struct DensitySelection {
    /// Exact number of output onsets, including mandatory anchors.
    std::size_t target_onsets = 0;
    /// Stable recipe seed.
    std::uint64_t seed = 0;
    /// Coordinate namespace used to rank non-anchor events.
    timebase::RandomCoordinate coordinate{};
};

/// Selects an exact event count while retaining every anchor.
///
/// Every non-anchor has one coordinate-derived rank, so selecting `k` events
/// is an exact subset of selecting `k + 1` with the same recipe.
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

/// Parameters for deterministic replacement within a half-open region.
struct RegionalFillSelection {
    /// Inclusive start of the edited region.
    timebase::TickPosition begin{};
    /// Exclusive end of the edited region.
    timebase::TickPosition end{};
    /// Exact number of onsets to retain inside the region.
    std::size_t target_region_onsets = 0;
    /// Stable recipe seed.
    std::uint64_t seed = 0;
    /// Coordinate namespace used to rank region events.
    timebase::RandomCoordinate coordinate{};
};

/// Applies candidate events within a selected half-open region.
///
/// Outside `[begin, end)` the base is unchanged. Inside it, base anchors are
/// mandatory and the union of base and candidate events is density-selected.
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

    std::size_t region_events = 0;
    std::size_t region_anchors = 0;
    std::size_t outside_events = 0;
    for (std::size_t index = 0; index < base.size(); ++index) {
        const auto event = *base.event(index);
        if (event.onset < selection.begin || event.onset >= selection.end) {
            ++outside_events;
            result.error = result.pattern.insert(event);
            if (result.error != PatternDevelopmentError::none) {
                result.pattern = {};
                return result;
            }
        } else {
            ++region_events;
            region_anchors += event.role == PatternEventRole::anchor;
        }
    }
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        const auto event = *candidates.event(index);
        if (event.onset < selection.begin || event.onset >= selection.end)
            continue;
        const auto base_event = base.find_id(event.id);
        if (base_event && *base_event != event) {
            result.error = PatternDevelopmentError::conflicting_event;
            result.pattern = {};
            return result;
        }
        const auto existing = base.find_onset(event.onset);
        if (existing) {
            if (*existing != event) {
                result.error = PatternDevelopmentError::conflicting_event;
                result.pattern = {};
                return result;
            }
            continue;
        }
        ++region_events;
    }

    if (selection.target_region_onsets > region_events) {
        result.error = PatternDevelopmentError::density_out_of_range;
        result.pattern = {};
        return result;
    }
    if (selection.target_region_onsets < region_anchors) {
        result.error = PatternDevelopmentError::density_below_anchor_count;
        result.pattern = {};
        return result;
    }
    if (outside_events + selection.target_region_onsets > MaxEvents) {
        result.error = PatternDevelopmentError::capacity_exceeded;
        result.pattern = {};
        return result;
    }

    const auto selected_nonanchors = selection.target_region_onsets - region_anchors;
    const auto rank = [&](const PatternEvent& event) constexpr noexcept {
        const auto priority = detail::event_priority(selection.seed, selection.coordinate, event);
        std::size_t result_rank = 0;
        const auto add_to_rank = [&](const PatternEvent& other) constexpr noexcept {
            const auto other_priority =
                detail::event_priority(selection.seed, selection.coordinate, other);
            result_rank +=
                other_priority < priority || (other_priority == priority && other.id < event.id);
        };
        for (std::size_t index = 0; index < base.size(); ++index) {
            const auto other = *base.event(index);
            if (other.onset >= selection.begin && other.onset < selection.end &&
                other.role != PatternEventRole::anchor)
                add_to_rank(other);
        }
        for (std::size_t index = 0; index < candidates.size(); ++index) {
            const auto other = *candidates.event(index);
            if (other.onset < selection.begin || other.onset >= selection.end ||
                base.find_onset(other.onset))
                continue;
            add_to_rank(other);
        }
        return result_rank;
    };

    for (std::size_t index = 0; index < base.size(); ++index) {
        const auto event = *base.event(index);
        if (event.onset < selection.begin || event.onset >= selection.end)
            continue;
        if (event.role != PatternEventRole::anchor && rank(event) >= selected_nonanchors)
            continue;
        result.error = result.pattern.insert(event);
        if (result.error != PatternDevelopmentError::none) {
            result.pattern = {};
            return result;
        }
    }
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        const auto event = *candidates.event(index);
        if (event.onset < selection.begin || event.onset >= selection.end ||
            base.find_onset(event.onset) || rank(event) >= selected_nonanchors)
            continue;
        result.error = result.pattern.insert(event);
        if (result.error != PatternDevelopmentError::none) {
            result.pattern = {};
            return result;
        }
    }
    return result;
}

/// Parameters for deterministic interpolation between two patterns.
struct PatternMorphSelection {
    /// Interpolation amount in the inclusive range `[0, 1000]`.
    std::uint16_t amount = 0;
    /// Stable recipe seed for events unique to one input.
    std::uint64_t seed = 0;
    /// Coordinate namespace for unique-event thresholds.
    timebase::RandomCoordinate coordinate{};
};

/// Morphs two patterns using exact integer interpolation and stable thresholds.
///
/// Shared IDs interpolate onsets and accents. Events unique to `a` disappear
/// and events unique to `b` appear at coordinate-keyed thresholds.
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
