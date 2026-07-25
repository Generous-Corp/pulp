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

// SplitMix64's finalizer: a full-avalanche bijection on 64 bits. Deliberately
// its own copy rather than a shared utility — a change to the launch mixer must
// not silently re-roll every authored note in every existing document.
constexpr std::uint64_t note_modifier_mix(std::uint64_t value) noexcept {
    value += 0x9E3779B97F4A7C15ULL;
    value ^= value >> 30;
    value *= 0xBF58476D1CE4E5B9ULL;
    value ^= value >> 27;
    value *= 0x94D049BB133111EBULL;
    value ^= value >> 31;
    return value;
}

// Probability is a 16-bit fraction of `note_probability_certain`. The endpoints
// are exact: zero never sounds and `note_probability_certain` always sounds,
// with no rounding band at either end.
inline constexpr std::uint16_t note_probability_certain = 0xffffu;

// The largest authored retrigger count. A ratchet subdivides the note's own
// span, so the bound is what keeps a subdivision from collapsing below one
// tick for musically plausible note lengths; a document asking for more is
// rejected at construction rather than silently clamped.
inline constexpr std::uint16_t note_ratchet_maximum = 64u;

// When a note sounds, relative to the loop pass it is being evaluated for.
// `EveryNth` and `Fill` read `period` and `offset`; `Always` and `First` ignore
// both. Pass indices count from zero at the transport's monotonic origin.
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

// Persisted spelling of a condition kind. The wire form is a name rather than
// an ordinal so a future kind can be inserted without renumbering documents.
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

// Inverse of note_condition_name. Returns false for an unknown spelling; a
// reader that cannot name a condition must refuse the document rather than
// guess a default that would silently change how it plays.
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

// A modifier attached to one note. Notes without an entry play unconditionally
// and once, which is why the companion array is sparse rather than parallel:
// a document that authors no modifiers carries no modifier data at all.
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

// True when the modifier changes nothing about how the note plays. Such an
// entry is redundant, so construction rejects it rather than admitting two
// encodings of the same document.
constexpr bool note_modifier_is_neutral(const NoteModifier& modifier) noexcept {
    return modifier.probability == note_probability_certain &&
           modifier.condition == NoteConditionKind::Always && modifier.ratchet_count == 1;
}

// Structural validity, independent of whether the referenced note exists.
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

// Folds the authored seed and the note identity into the key the audio thread
// draws from. Computing it once at compile time keeps the per-event work on the
// audio thread to a single mix.
constexpr std::uint64_t note_modifier_draw_key(std::uint64_t seed, ItemId note_id) noexcept {
    return note_modifier_mix(note_modifier_mix(seed) ^ note_id.value);
}

// One probability draw: the note's key against the loop pass being evaluated.
// The pair is unique per (note, pass) and stable across replays, which is what
// makes a probabilistic note reproducible.
struct NoteModifierDraw {
    std::uint64_t key = 0;
    std::uint64_t pass_index = 0;

    constexpr std::uint64_t value() const noexcept {
        return note_modifier_mix(key ^ (pass_index + 0x9E3779B97F4A7C15ULL));
    }
};

// Exact at both endpoints: zero is silent for every draw, and
// `note_probability_certain` sounds for every draw. The interior maps the draw
// onto [0, note_probability_certain), so probability p sounds for exactly p of
// the certain-many residues.
constexpr bool note_probability_sounds(std::uint16_t probability, NoteModifierDraw draw) noexcept {
    if (probability == 0)
        return false;
    if (probability >= note_probability_certain)
        return true;
    return draw.value() % note_probability_certain < probability;
}

// Whether the pass-relative condition admits this pass.
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

// The complete gate: a note sounds when its condition admits the pass and its
// probability draw succeeds. The condition is checked first so a passless note
// consumes no draw reasoning at all — the draw is pure, so this is an ordering
// of readability, not of results.
constexpr bool note_modifier_sounds(const NoteModifier& modifier, std::uint64_t draw_key,
                                    std::uint64_t pass_index) noexcept {
    if (!note_condition_holds(modifier.condition, modifier.condition_period,
                              modifier.condition_offset, pass_index))
        return false;
    return note_probability_sounds(modifier.probability, NoteModifierDraw{draw_key, pass_index});
}

} // namespace pulp::timeline
