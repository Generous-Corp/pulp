#pragma once

#include <pulp/timebase/compiled_meter_map.hpp>
#include <pulp/timebase/tick.hpp>
#include <pulp/timeline/item_id.hpp>

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <vector>

/// @file clip_launch.hpp
/// Immutable authored scene, slot, quantization, and follow-action model.
///
/// Where the linear model places clips at absolute timeline positions, this
/// surface describes what is launchable and how it quantizes. Playback resolves
/// triggers sample-accurately and owns transport launch progress.
namespace pulp::timeline {

/** @addtogroup timeline_model
 * @{
 */

namespace detail {
class SlotListStore;
class SlotListAccess;
} // namespace detail

/// Musical-boundary quantization for one launch.
///
/// The boundary set is `{ phase + k * grid : k in Z }` on the transport's
/// monotonic clock. A non-positive grid means immediate launch. `phase` aligns
/// the grid to an offset such as a section start.
struct LaunchQuantize {
    timebase::TickDuration grid{0};
    timebase::TickPosition phase{0};

    /// Returns whether launch occurs without boundary quantization.
    constexpr bool immediate() const noexcept {
        return grid.value <= 0;
    }
    constexpr auto operator<=>(const LaunchQuantize&) const = default;
};

/// Returns an immediate, phase-zero launch policy.
constexpr LaunchQuantize launch_immediate() noexcept {
    return {timebase::TickDuration{0}, timebase::TickPosition{0}};
}

/// Returns a phase-zero grid spanning `count` quarter notes.
///
/// A non-positive count produces immediate launch semantics.
constexpr LaunchQuantize launch_every_quarters(std::int64_t count) noexcept {
    return {timebase::TickDuration{count * timebase::kTicksPerQuarter}, timebase::TickPosition{0}};
}

/// Returns a phase-zero grid spanning `count` bars under `meter`.
///
/// A bar spans `numerator * (4 / denominator)` quarter notes; the arithmetic
/// remains in exact canonical ticks for every supported denominator.
constexpr LaunchQuantize launch_every_bars(std::int64_t count,
                                           timebase::MeterSignature meter) noexcept {
    const std::int64_t ticks_per_bar =
        timebase::kTicksPerQuarter * meter.numerator * 4 / meter.denominator;
    return {timebase::TickDuration{count * ticks_per_bar}, timebase::TickPosition{0}};
}

/// Action applied when a slot's follow timer elapses.
enum class FollowActionKind : std::uint8_t {
    None,     // keep sounding; the slot is left alone
    Stop,     // stop this slot
    Again,    // relaunch this slot
    Previous, // launch the nearest non-empty slot before this one, wrapping
    Next,     // launch the nearest non-empty slot after this one, wrapping
    First,    // launch the first non-empty slot in the lane
    Last,     // launch the last non-empty slot in the lane
    Any,      // launch a non-empty slot chosen at random, this one included
    Other,    // launch a non-empty slot chosen at random, this one excluded
    Jump,     // launch the slot named by `target`
};

/// One weighted follow-action candidate.
///
/// `target` is read only by Jump. `weight` is the relative probability within
/// its set; zero makes the candidate unreachable without removing it.
struct FollowAction {
    FollowActionKind kind = FollowActionKind::None;
    ItemId target{};
    std::uint16_t weight = 1;

    constexpr auto operator<=>(const FollowAction&) const = default;
};

/// Bounded weighted follow behavior attached to a slot.
///
/// Stores up to `kMaxChoices` candidates, their period, and the number of
/// periods before firing. A non-positive grid disables the timer. The duration
/// is measured from launch and therefore has no shared-grid phase.
struct FollowActionSet {
    static constexpr std::size_t kMaxChoices = 4;

    std::array<FollowAction, kMaxChoices> choices{};
    std::uint8_t choice_count = 0;
    std::uint32_t repetitions = 1;
    timebase::TickDuration grid{0};

    /// Returns the populated prefix of `choices`.
    ///
    /// An out-of-range `choice_count` is clamped rather than trusted.
    constexpr std::span<const FollowAction> active() const noexcept {
        const std::size_t count = choice_count < kMaxChoices ? choice_count : kMaxChoices;
        return {choices.data(), count};
    }

    /// Returns whether this set has candidates, a positive period, and repetitions.
    constexpr bool enabled() const noexcept {
        return !active().empty() && grid.value > 0 && repetitions > 0;
    }

    constexpr bool operator==(const FollowActionSet&) const = default;
};

/// Builds a one-candidate follow set with unit weight.
constexpr FollowActionSet follow_action(FollowActionKind kind, timebase::TickDuration grid,
                                        std::uint32_t repetitions = 1) noexcept {
    FollowActionSet set;
    set.choices[0] = FollowAction{kind, ItemId{}, 1};
    set.choice_count = 1;
    set.repetitions = repetitions;
    set.grid = grid;
    return set;
}

/// One authored launch slot.
///
/// `clip_id` references a Project-owned clip; an invalid `clip_id` is the
/// explicit empty-slot representation. Sequences persist slots through scenes,
/// while playback owns runtime trigger and progress state.
struct Slot {
    ItemId id;
    ItemId clip_id;
    LaunchQuantize launch_quantize{};
    FollowActionSet follow{};

    /// Returns whether this slot references no launchable clip.
    constexpr bool empty() const noexcept {
        return !clip_id.valid();
    }
    constexpr bool operator==(const Slot&) const = default;
};

/// Immutable authored slot order with persistent structural sharing.
///
/// Fresh Scene values may be assembled from a vector. A Sequence-owned Scene
/// uses a persistent AVL-backed store, so edits path-copy affected index paths
/// and share untouched nodes with prior snapshots.
class SlotList {
  public:
    /// Forward iterator that retains raw or persistent slot storage.
    class Iterator {
      public:
        using value_type = Slot;
        using difference_type = std::ptrdiff_t;
        using pointer = const Slot*;
        using reference = const Slot&;
        using iterator_category = std::forward_iterator_tag;

        /// Borrows the current slot; the iterator's retained storage keeps it alive.
        const Slot& operator*() const noexcept;
        /// Returns the address of the current borrowed slot.
        const Slot* operator->() const noexcept;
        /// Advances in authored order.
        Iterator& operator++() noexcept;
        /// Advances in authored order and returns the prior iterator value.
        Iterator operator++(int) noexcept {
            auto copy = *this;
            ++*this;
            return copy;
        }
        /// Compares iterator position and retained storage identity.
        bool operator==(const Iterator&) const noexcept = default;

      private:
        friend class SlotList;
        Iterator(std::shared_ptr<const std::vector<Slot>> raw,
                 std::shared_ptr<const detail::SlotListStore> store, std::size_t raw_index,
                 ItemId current) noexcept
            : raw_(std::move(raw)), store_(std::move(store)), raw_index_(raw_index),
              current_(current) {}
        std::shared_ptr<const std::vector<Slot>> raw_;
        std::shared_ptr<const detail::SlotListStore> store_;
        std::size_t raw_index_ = 0;
        ItemId current_;
    };

    /// Constructs an empty list.
    SlotList();
    /// Takes ownership of slots in authored order.
    SlotList(std::vector<Slot> slots);
    /// Copies slots in authored order.
    SlotList(std::initializer_list<Slot> slots);

    /// Returns the number of authored slots.
    std::size_t size() const noexcept;
    /// Returns whether the list contains no slots.
    bool empty() const noexcept {
        return size() == 0;
    }
    /// Borrows the slot at `index`; `index` must be less than `size()`.
    const Slot& operator[](std::size_t index) const noexcept;
    /// Borrows the first slot; the list must be nonempty.
    const Slot& front() const noexcept {
        return (*this)[0];
    }
    /// Borrows the last slot; the list must be nonempty.
    const Slot& back() const noexcept {
        return (*this)[size() - 1];
    }
    /// Finds a slot by identity, or returns null.
    const Slot* find(ItemId id) const noexcept;
    /// Returns the first iterator in authored order.
    Iterator begin() const noexcept;
    /// Returns the end sentinel.
    Iterator end() const noexcept;
    /// Reports whether both snapshots share their complete backing store.
    bool shares_storage_with(const SlotList& other) const noexcept;
    /// Compares authored slot values in order.
    bool operator==(const SlotList& other) const noexcept;

  private:
    friend class detail::SlotListAccess;
    explicit SlotList(std::shared_ptr<const detail::SlotListStore> store) noexcept
        : store_(std::move(store)) {}
    std::shared_ptr<const std::vector<Slot>> raw_;
    std::shared_ptr<const detail::SlotListStore> store_;
};

/// Authored grouping of slots presented as one launchable column.
///
/// This value does not define runtime scene-to-track arbitration.
struct Scene {
    ItemId id;
    std::string name;
    SlotList slots;

    bool operator==(const Scene&) const = default;
};

// Follow-action resolution
// ------------------------
// Resolution is a pure function of the lane contents, the acting slot, and a
// draw index — never of hidden mutable state — so an engine replaying the same
// document with the same transport trace resolves the same successor every
// time. Random kinds draw from a stateless hash of (seed, slot id, draw index):
// two slots deciding in the same block cannot perturb each other's draw, and
// evaluation order is therefore irrelevant to the result.

/// Applies the deterministic SplitMix64 finalizer used by follow draws.
constexpr std::uint64_t follow_action_mix(std::uint64_t value) noexcept {
    value += 0x9E3779B97F4A7C15ULL;
    value ^= value >> 30;
    value *= 0xBF58476D1CE4E5B9ULL;
    value ^= value >> 27;
    value *= 0x94D049BB133111EBULL;
    value ^= value >> 31;
    return value;
}

/// Stable identity of one reproducible follow-action decision.
///
/// `seed` is the session's launch seed, `slot` is the deciding slot, and
/// `draw_index` counts decisions already resolved by that slot.
struct FollowDraw {
    std::uint64_t seed = 0;
    ItemId slot{};
    std::uint64_t draw_index = 0;

    /// Returns the deterministic 64-bit draw for this decision identity.
    constexpr std::uint64_t value() const noexcept {
        return follow_action_mix(follow_action_mix(seed ^ slot.value) ^
                                 (draw_index + 0x9E3779B97F4A7C15ULL));
    }
};

/// Selects one active candidate by relative weight.
///
/// Each candidate owns a half-open interval proportional to its weight.
/// Returns `kMaxChoices` when the set is empty or every weight is zero.
constexpr std::size_t select_follow_action(const FollowActionSet& set, FollowDraw draw) noexcept {
    const auto candidates = set.active();
    std::uint64_t total = 0;
    for (const auto& candidate : candidates)
        total += candidate.weight;
    if (total == 0)
        return FollowActionSet::kMaxChoices;
    std::uint64_t pick = draw.value() % total;
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        const std::uint64_t weight = candidates[index].weight;
        if (pick < weight)
            return index;
        pick -= weight;
    }
    return FollowActionSet::kMaxChoices; // unreachable: pick < total
}

/// Runtime request produced by resolving an authored follow action.
enum class FollowOutcome : std::uint8_t {
    None, // leave the acting slot sounding
    Stop, // stop the acting slot
    Play, // launch `slot_index` (which may be the acting slot: "again")
};

/// Deterministic outcome and optional target slot of follow resolution.
struct FollowResolution {
    static constexpr std::size_t kNoSlot = static_cast<std::size_t>(-1);

    FollowOutcome outcome = FollowOutcome::None;
    std::size_t slot_index = kNoSlot;
    ItemId slot_id{};

    constexpr auto operator<=>(const FollowResolution&) const = default;
};

namespace follow_detail {

constexpr std::size_t count_non_empty(std::span<const Slot> lane) noexcept {
    std::size_t count = 0;
    for (const auto& slot : lane)
        count += slot.empty() ? 0u : 1u;
    return count;
}

// The n-th non-empty slot in `lane`, or kNoSlot when fewer exist.
constexpr std::size_t nth_non_empty(std::span<const Slot> lane, std::size_t n) noexcept {
    for (std::size_t index = 0; index < lane.size(); ++index) {
        if (lane[index].empty())
            continue;
        if (n == 0)
            return index;
        --n;
    }
    return FollowResolution::kNoSlot;
}

// The nearest non-empty slot after (or before) `from`, wrapping around the lane
// and stepping over empty slots. `from` itself is only the answer when it is
// the sole non-empty slot.
constexpr std::size_t step_non_empty(std::span<const Slot> lane, std::size_t from,
                                     bool forward) noexcept {
    const std::size_t size = lane.size();
    if (size == 0)
        return FollowResolution::kNoSlot;
    std::size_t index = from;
    for (std::size_t taken = 0; taken < size; ++taken) {
        index = forward ? (index + 1) % size : (index + size - 1) % size;
        if (!lane[index].empty())
            return index;
    }
    return FollowResolution::kNoSlot;
}

// Launching an empty slot would sound nothing, so it resolves to None instead.
constexpr FollowResolution play(std::span<const Slot> lane, std::size_t index) noexcept {
    if (index == FollowResolution::kNoSlot || index >= lane.size() || lane[index].empty())
        return {};
    return {FollowOutcome::Play, index, lane[index].id};
}

} // namespace follow_detail

/// Resolves the successor of `self_index` under that slot's follow-action set.
///
/// Resolution is allocation-free and depends only on `lane`, `self_index`, and
/// `draw`. An unsatisfied candidate, such as a missing jump target, resolves to
/// None rather than silently stopping the slot.
constexpr FollowResolution resolve_follow_action(std::span<const Slot> lane, std::size_t self_index,
                                                 FollowDraw draw) noexcept {
    if (self_index >= lane.size())
        return {};
    const FollowActionSet& set = lane[self_index].follow;
    if (!set.enabled())
        return {};
    const auto candidates = set.active();
    const std::size_t choice = select_follow_action(set, draw);
    if (choice >= candidates.size())
        return {};
    const FollowAction& action = candidates[choice];

    switch (action.kind) {
    case FollowActionKind::None:
        return {};
    case FollowActionKind::Stop:
        return {FollowOutcome::Stop, self_index, lane[self_index].id};
    case FollowActionKind::Again:
        return follow_detail::play(lane, self_index);
    case FollowActionKind::Previous:
        return follow_detail::play(lane, follow_detail::step_non_empty(lane, self_index, false));
    case FollowActionKind::Next:
        return follow_detail::play(lane, follow_detail::step_non_empty(lane, self_index, true));
    case FollowActionKind::First:
        return follow_detail::play(lane, follow_detail::nth_non_empty(lane, 0));
    case FollowActionKind::Last: {
        const std::size_t count = follow_detail::count_non_empty(lane);
        if (count == 0)
            return {};
        return follow_detail::play(lane, follow_detail::nth_non_empty(lane, count - 1));
    }
    case FollowActionKind::Any: {
        const std::size_t count = follow_detail::count_non_empty(lane);
        if (count == 0)
            return {};
        // A second mix of the same draw keeps the candidate choice and the slot
        // choice independent without a second draw index.
        const std::uint64_t roll = follow_action_mix(draw.value());
        return follow_detail::play(lane, follow_detail::nth_non_empty(lane, roll % count));
    }
    case FollowActionKind::Other: {
        const bool self_counts = !lane[self_index].empty();
        const std::size_t count = follow_detail::count_non_empty(lane);
        const std::size_t others = count - (self_counts ? 1u : 0u);
        if (others == 0)
            // Nothing else to reach for: the acting slot plays again, matching
            // the single-clip lane case.
            return self_counts ? follow_detail::play(lane, self_index) : FollowResolution{};
        const std::uint64_t roll = follow_action_mix(draw.value());
        std::size_t pick = static_cast<std::size_t>(roll % others);
        for (std::size_t index = 0; index < lane.size(); ++index) {
            if (index == self_index || lane[index].empty())
                continue;
            if (pick == 0)
                return follow_detail::play(lane, index);
            --pick;
        }
        return {};
    }
    case FollowActionKind::Jump:
        if (!action.target.valid())
            return {};
        for (std::size_t index = 0; index < lane.size(); ++index) {
            if (lane[index].id == action.target)
                return follow_detail::play(lane, index);
        }
        return {};
    }
    return {};
}

/// @}

} // namespace pulp::timeline
