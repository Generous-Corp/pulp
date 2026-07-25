#include <pulp/timeline/model.hpp>
#include <pulp/timeline/note_modifier.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <utility>
#include <vector>

using namespace pulp;
using namespace pulp::timeline;

namespace {

template <typename T, typename E> T take(runtime::Result<T, E> result) {
    REQUIRE(result.has_value());
    return std::move(result).value();
}

std::vector<NoteEvent> two_notes() {
    return {{{7}, {0}, {480}, 0xffff, 60, 0}, {{8}, {480}, {480}, 0x8000, 64, 0}};
}

NoteModifier chance(std::uint64_t note, std::uint16_t probability) {
    NoteModifier modifier;
    modifier.note_id = {note};
    modifier.probability = probability;
    return modifier;
}

// How often a note sounds across `passes` consecutive loop passes.
std::size_t sounding_passes(const NoteModifier& modifier, std::uint64_t seed, ItemId note,
                            std::uint64_t passes) {
    const auto key = note_modifier_draw_key(seed, note);
    std::size_t sounded = 0;
    for (std::uint64_t pass = 0; pass < passes; ++pass)
        if (note_modifier_sounds(modifier, key, pass))
            ++sounded;
    return sounded;
}

} // namespace

TEST_CASE("Note modifiers attach to notes and normalize to a canonical order",
          "[timeline][note-modifier]") {
    auto content = take(NoteContent::create(
        two_notes(), {chance(8, note_probability_certain / 2), chance(7, 1)}, 0xC0FFEE));
    REQUIRE(content.modifiers().size() == 2);
    REQUIRE(content.modifiers()[0].note_id == ItemId{7});
    REQUIRE(content.modifiers()[1].note_id == ItemId{8});
    REQUIRE(content.modifier_seed() == 0xC0FFEE);
    REQUIRE(content.modifier_for({7}) != nullptr);
    REQUIRE(content.modifier_for({8}) != nullptr);
    REQUIRE(content.modifier_for({9}) == nullptr);

    // Notes alone stay exactly as cheap as before: no modifiers, zero seed.
    const auto plain = take(NoteContent::create(two_notes()));
    REQUIRE(plain.modifiers().empty());
    REQUIRE(plain.modifier_seed() == 0);
    REQUIRE(plain.modifier_for({7}) == nullptr);
}

TEST_CASE("Note modifiers are rejected when they cannot describe a real decision",
          "[timeline][note-modifier]") {
    // Names a note the content does not contain.
    REQUIRE_FALSE(NoteContent::create(two_notes(), {chance(99, 1)}, 0));
    // Two entries for one note: the content would have two answers.
    REQUIRE_FALSE(NoteContent::create(two_notes(), {chance(7, 1), chance(7, 2)}, 0));
    // Neutral: a second encoding of a note that already plays that way.
    REQUIRE_FALSE(NoteContent::create(two_notes(), {chance(7, note_probability_certain)}, 0));

    NoteModifier zero_period = chance(7, 1);
    zero_period.condition = NoteConditionKind::EveryNth;
    zero_period.condition_period = 0;
    REQUIRE_FALSE(NoteContent::create(two_notes(), {zero_period}, 0));

    NoteModifier offset_past_period = chance(7, 1);
    offset_past_period.condition = NoteConditionKind::EveryNth;
    offset_past_period.condition_period = 4;
    offset_past_period.condition_offset = 4;
    REQUIRE_FALSE(NoteContent::create(two_notes(), {offset_past_period}, 0));

    // An unread period/offset must stay canonical so one behavior has one form.
    NoteModifier noisy_always = chance(7, 1);
    noisy_always.condition_period = 3;
    REQUIRE_FALSE(NoteContent::create(two_notes(), {noisy_always}, 0));

    NoteModifier no_ratchet = chance(7, 1);
    no_ratchet.ratchet_count = 0;
    REQUIRE_FALSE(NoteContent::create(two_notes(), {no_ratchet}, 0));

    NoteModifier huge_ratchet = chance(7, 1);
    huge_ratchet.ratchet_count = note_ratchet_maximum + 1;
    REQUIRE_FALSE(NoteContent::create(two_notes(), {huge_ratchet}, 0));
}

TEST_CASE("Note probability is exact at both endpoints", "[timeline][note-modifier][determinism]") {
    NoteModifier never = chance(7, 0);
    NoteModifier always = chance(7, note_probability_certain);
    always.ratchet_count = 2; // keeps it non-neutral without touching probability

    // Every draw in a long sweep, not a sampled few: an endpoint that leaked
    // even one contrary decision would be a rounding band, not an endpoint.
    for (std::uint64_t seed = 0; seed < 32; ++seed) {
        const auto never_key = note_modifier_draw_key(seed, {7});
        const auto always_key = note_modifier_draw_key(seed, {8});
        for (std::uint64_t pass = 0; pass < 512; ++pass) {
            REQUIRE_FALSE(note_modifier_sounds(never, never_key, pass));
            REQUIRE(note_modifier_sounds(always, always_key, pass));
        }
    }
}

TEST_CASE("The same seed replays the same sounding decisions", "[timeline][note-modifier][determinism]") {
    const auto modifier = chance(7, note_probability_certain / 2);
    constexpr std::uint64_t passes = 4'096;

    // Same seed, evaluated twice: byte-identical decision sequences. The
    // evaluator carries no state, so the second sweep cannot see the first.
    std::vector<bool> first;
    std::vector<bool> second;
    const auto key = note_modifier_draw_key(0x5EED, {7});
    for (std::uint64_t pass = 0; pass < passes; ++pass)
        first.push_back(note_modifier_sounds(modifier, key, pass));
    for (std::uint64_t pass = 0; pass < passes; ++pass)
        second.push_back(note_modifier_sounds(modifier, key, pass));
    REQUIRE(first == second);

    // Interleaving the two sweeps still agrees, which is the property that
    // rules out an order-dependent generator hiding behind a stable loop.
    for (std::uint64_t pass = 0; pass < passes; ++pass)
        REQUIRE(note_modifier_sounds(modifier, key, passes - 1 - pass) ==
                first[static_cast<std::size_t>(passes - 1 - pass)]);

    // A different seed decides differently. Both sweeps sound roughly half the
    // time, so agreement on all 4096 passes would be about 2^-4096 by chance;
    // any real agreement here means the seed is not reaching the draw.
    const auto other_key = note_modifier_draw_key(0x5EEE, {7});
    std::vector<bool> other;
    for (std::uint64_t pass = 0; pass < passes; ++pass)
        other.push_back(note_modifier_sounds(modifier, other_key, pass));
    REQUIRE(other != first);

    // The note identity is in the draw too: two notes under one seed must not
    // move in lockstep, or a whole clip would sound or vanish together.
    const auto sibling_key = note_modifier_draw_key(0x5EED, {8});
    std::vector<bool> sibling;
    for (std::uint64_t pass = 0; pass < passes; ++pass)
        sibling.push_back(note_modifier_sounds(modifier, sibling_key, pass));
    REQUIRE(sibling != first);

    // A half-chance note should land near half over a long sweep. The band is
    // wide enough never to flake and narrow enough to catch a draw that is
    // stuck, constant, or ignoring the pass index.
    const auto sounded = sounding_passes(modifier, 0x5EED, {7}, passes);
    REQUIRE(sounded > passes / 3);
    REQUIRE(sounded < passes * 2 / 3);
}

TEST_CASE("Note conditions select passes exactly", "[timeline][note-modifier][determinism]") {
    NoteModifier first_only = chance(7, note_probability_certain);
    first_only.condition = NoteConditionKind::First;
    const auto first_key = note_modifier_draw_key(1, {7});
    REQUIRE(note_modifier_sounds(first_only, first_key, 0));
    for (std::uint64_t pass = 1; pass < 64; ++pass)
        REQUIRE_FALSE(note_modifier_sounds(first_only, first_key, pass));

    NoteModifier every_fourth = chance(7, note_probability_certain);
    every_fourth.condition = NoteConditionKind::EveryNth;
    every_fourth.condition_period = 4;
    every_fourth.condition_offset = 2;
    for (std::uint64_t pass = 0; pass < 64; ++pass)
        REQUIRE(note_modifier_sounds(every_fourth, first_key, pass) == (pass % 4 == 2));

    NoteModifier fill = chance(7, note_probability_certain);
    fill.condition = NoteConditionKind::Fill;
    fill.condition_period = 8;
    for (std::uint64_t pass = 0; pass < 64; ++pass)
        REQUIRE(note_modifier_sounds(fill, first_key, pass) == (pass % 8 == 7));

    // Condition and probability compose: a zero chance silences an admitted
    // pass rather than the condition overriding it.
    NoteModifier silent_fill = fill;
    silent_fill.probability = 0;
    for (std::uint64_t pass = 0; pass < 64; ++pass)
        REQUIRE_FALSE(note_modifier_sounds(silent_fill, first_key, pass));
}

TEST_CASE("Condition names round trip and unknown spellings are refused",
          "[timeline][note-modifier]") {
    for (const auto condition : {NoteConditionKind::Always, NoteConditionKind::EveryNth,
                                 NoteConditionKind::First, NoteConditionKind::Fill}) {
        NoteConditionKind parsed{};
        REQUIRE(parse_note_condition(note_condition_name(condition), parsed));
        REQUIRE(parsed == condition);
    }
    NoteConditionKind unused{};
    REQUIRE_FALSE(parse_note_condition("", unused));
    REQUIRE_FALSE(parse_note_condition("every_other", unused));
    REQUIRE_FALSE(parse_note_condition("Always", unused));
}

TEST_CASE("Replacing a note keeps its modifiers and the content seed",
          "[timeline][note-modifier]") {
    auto content = take(NoteContent::create(two_notes(), {chance(7, 1024)}, 99));
    auto updated = take(content.replace_note({{7}, {0}, {240}, 0x4000, 61, 0}));
    REQUIRE(updated.modifier_seed() == 99);
    REQUIRE(updated.modifiers().size() == 1);
    REQUIRE(updated.modifiers()[0].probability == 1024);
    REQUIRE(updated.notes()[0].pitch == 61);
}
