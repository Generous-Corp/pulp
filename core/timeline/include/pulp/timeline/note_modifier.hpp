#pragma once

#include <pulp/timeline/item_id.hpp>

#include <compare>
#include <cstdint>
#include <string_view>

// Per-note playback modifiers: whether a note sounds on a given pass, and how
// many times it retriggers when it does.
//
// Evaluation is a pure function of (draw key, pass index) — never of hidden
// mutable state and never of evaluation order. Two notes deciding in the same
// block cannot perturb each other, so an engine replaying the same document
// against the same transport trace makes the same sounding decisions every
// time. That is the replay contract; a stateful generator would break it.
//
// The draw key folds the authored seed and the note identity together once, at
// compile time, so the audio thread only mixes the key with the pass index.
namespace pulp::timeline {

/** @addtogroup timeline_model
 * @{
 */

/// Mixes one 64-bit value with the stable note-modifier avalanche function.
///
/// This copy is deliberately independent of other randomization surfaces so
/// changing another mixer cannot re-roll authored notes.
constexpr std::uint64_t note_modifier_mix(std::uint64_t value) noexcept {
    value += 0x9E3779B97F4A7C15ULL;
    value ^= value >> 30;
    value *= 0xBF58476D1CE4E5B9ULL;
    value ^= value >> 27;
    value *= 0x94D049BB133111EBULL;
    value ^= value >> 31;
    return value;
}

/// Denominator and exact "always sounds" value for authored note probability.
inline constexpr std::uint16_t note_probability_certain = 0xffffu;

/// Largest admitted authored retrigger count.
inline constexpr std::uint16_t note_ratchet_maximum = 64u;

/// Pass-relative condition controlling when a note may sound.
///
/// `EveryNth` and `Fill` read the modifier period; `EveryNth` also reads its
/// offset. Pass indices count from zero at the transport's monotonic origin.
enum class NoteConditionKind : std::uint8_t {
    // Sounds on every pass.
    Always,
    // Sounds when pass % period == offset.
    EveryNth,
    // Sounds only on the first pass.
    First,
    // Sounds on the last pass of each group of `period` — the "fill" bar.
    Fill,
};

/// Returns the canonical persisted spelling of `condition`.
constexpr std::string_view note_condition_name(NoteConditionKind condition) noexcept {
    switch (condition) {
    case NoteConditionKind::Always:
        return "always";
    case NoteConditionKind::EveryNth:
        return "every_nth";
    case NoteConditionKind::First:
        return "first";
    case NoteConditionKind::Fill:
        return "fill";
    }
    return "always";
}

/// Parses a canonical condition name into `out`.
/// @return `false` for an unknown spelling, leaving `out` unchanged.
constexpr bool parse_note_condition(std::string_view name, NoteConditionKind& out) noexcept {
    if (name == "always") {
        out = NoteConditionKind::Always;
        return true;
    }
    if (name == "every_nth") {
        out = NoteConditionKind::EveryNth;
        return true;
    }
    if (name == "first") {
        out = NoteConditionKind::First;
        return true;
    }
    if (name == "fill") {
        out = NoteConditionKind::Fill;
        return true;
    }
    return false;
}

/// Sparse playback modifier attached to one note identity.
///
/// Notes without an entry play unconditionally and once. Probability is a
/// fraction of `note_probability_certain`; ratchets subdivide the note's own
/// duration and are deterministic.
struct NoteModifier {
    ItemId note_id;
    // Chance the note sounds, out of `note_probability_certain`.
    std::uint16_t probability = note_probability_certain;
    // Pass grouping for `EveryNth` / `Fill`. Must be at least one.
    std::uint16_t condition_period = 1;
    // Which pass within the group `EveryNth` selects. Must be below the period.
    std::uint16_t condition_offset = 0;
    // How many times the note retriggers within its own span. One is a plain
    // note; the count is authored, not drawn, so ratcheting is fully
    // deterministic independent of the seed.
    std::uint16_t ratchet_count = 1;
    NoteConditionKind condition = NoteConditionKind::Always;

    constexpr auto operator<=>(const NoteModifier&) const = default;
};

/// Returns whether `modifier` encodes unconditional, single-play behavior.
constexpr bool note_modifier_is_neutral(const NoteModifier& modifier) noexcept {
    return modifier.probability == note_probability_certain &&
           modifier.condition == NoteConditionKind::Always && modifier.ratchet_count == 1;
}

/// Checks structural validity independently of whether the note identity exists.
constexpr bool note_modifier_well_formed(const NoteModifier& modifier) noexcept {
    if (modifier.ratchet_count == 0 || modifier.ratchet_count > note_ratchet_maximum)
        return false;
    switch (modifier.condition) {
    case NoteConditionKind::Always:
    case NoteConditionKind::First:
        // The period and offset are unread here, so only their canonical
        // encoding is admitted; otherwise one behavior would have many
        // byte representations.
        return modifier.condition_period == 1 && modifier.condition_offset == 0;
    case NoteConditionKind::EveryNth:
        return modifier.condition_period != 0 &&
               modifier.condition_offset < modifier.condition_period;
    case NoteConditionKind::Fill:
        return modifier.condition_period != 0 && modifier.condition_offset == 0;
    }
    return false;
}

/// Derives the deterministic draw key for one authored seed and note identity.
constexpr std::uint64_t note_modifier_draw_key(std::uint64_t seed, ItemId note_id) noexcept {
    return note_modifier_mix(note_modifier_mix(seed) ^ note_id.value);
}

/// One deterministic probability draw for a note key and loop pass.
struct NoteModifierDraw {
    std::uint64_t key = 0;
    std::uint64_t pass_index = 0;

    /// Returns the stable mixed value for this key/pass pair.
    constexpr std::uint64_t value() const noexcept {
        return note_modifier_mix(key ^ (pass_index + 0x9E3779B97F4A7C15ULL));
    }
};

/// Tests an authored probability against `draw`.
///
/// Zero is always silent and `note_probability_certain` always sounds. Interior
/// values map exactly onto the same-size residue range.
constexpr bool note_probability_sounds(std::uint16_t probability, NoteModifierDraw draw) noexcept {
    if (probability == 0)
        return false;
    if (probability >= note_probability_certain)
        return true;
    return draw.value() % note_probability_certain < probability;
}

/// Returns whether a pass-relative condition admits `pass_index`.
///
/// Invalid zero periods return false for periodic conditions.
constexpr bool note_condition_holds(NoteConditionKind condition, std::uint16_t period,
                                    std::uint16_t offset, std::uint64_t pass_index) noexcept {
    switch (condition) {
    case NoteConditionKind::Always:
        return true;
    case NoteConditionKind::First:
        return pass_index == 0;
    case NoteConditionKind::EveryNth:
        return period != 0 && pass_index % period == offset;
    case NoteConditionKind::Fill:
        return period != 0 && pass_index % period == static_cast<std::uint64_t>(period - 1);
    }
    return true;
}

/// Applies both the modifier's condition and deterministic probability gate.
///
/// `draw_key` normally comes from note_modifier_draw_key(). The function is
/// pure and evaluation order cannot change its result.
constexpr bool note_modifier_sounds(const NoteModifier& modifier, std::uint64_t draw_key,
                                    std::uint64_t pass_index) noexcept {
    if (!note_condition_holds(modifier.condition, modifier.condition_period,
                              modifier.condition_offset, pass_index))
        return false;
    return note_probability_sounds(modifier.probability, NoteModifierDraw{draw_key, pass_index});
}

/// @}

} // namespace pulp::timeline
