// MIDI panic helper: message generation, held-note offs, classification.

#include <catch2/catch_test_macros.hpp>

#include <pulp/midi/panic.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

using namespace pulp::midi;

namespace {

std::vector<MidiEvent> make_panic(const PanicScope& scope, std::int32_t offset = 0) {
    std::vector<MidiEvent> out(kMaxPanicMessages);
    out.resize(panic_messages(scope, out, offset));
    return out;
}

std::size_t count_kind(const std::vector<MidiEvent>& events, PanicMessageKind kind) {
    std::size_t n = 0;
    for (const auto& e : events)
        if (classify_panic_message(e) == kind)
            ++n;
    return n;
}

} // namespace

static_assert(noexcept(panic_messages(PanicScope{}, std::span<MidiEvent>{})));
static_assert(noexcept(held_note_offs(std::declval<const MidiKeyboardState&>(),
                                      std::span<MidiEvent>{})));
static_assert(noexcept(all_note_offs(std::span<MidiEvent>{})));
static_assert(panic_message_count(PanicScope{}) == 80);
static_assert(panic_message_count(PanicScope::release_only()) == 64);

TEST_CASE("panic_messages emits the five messages on every channel in order", "[midi][panic]") {
    const auto events = make_panic(PanicScope{});
    REQUIRE(events.size() == 80);
    REQUIRE(events.size() == panic_message_count(PanicScope{}));

    for (unsigned ch = 0; ch < 16; ++ch) {
        const auto* first = &events[ch * 5];
        // sustain off, All Notes Off, All Sound Off, Reset All Controllers
        CHECK(first[0].data()[0] == (0xB0 | ch));
        CHECK(first[0].data()[1] == 64);
        CHECK(first[0].data()[2] == 0);
        CHECK(first[1].data()[1] == 123);
        CHECK(first[2].data()[1] == 120);
        CHECK(first[3].data()[1] == 121);
        for (int i = 1; i < 4; ++i) {
            CHECK(first[i].data()[0] == (0xB0 | ch));
            CHECK(first[i].data()[2] == 0);
        }
        // pitch bend centre: LSB 0, MSB 0x40 (8192)
        CHECK(first[4].data()[0] == (0xE0 | ch));
        CHECK(first[4].data()[1] == 0x00);
        CHECK(first[4].data()[2] == 0x40);
    }
    for (const auto& e : events)
        CHECK(classify_panic_message(e).has_value());
}

TEST_CASE("panic_messages honours the channel mask and the kind switches", "[midi][panic]") {
    PanicScope scope;
    scope.channel_mask = 0b0000'0000'0000'0101; // channels 0 and 2
    const auto two = make_panic(scope);
    REQUIRE(two.size() == 10);
    CHECK(two[0].data()[0] == 0xB0);
    CHECK(two[5].data()[0] == 0xB2);

    scope.channel_mask = 0;
    CHECK(make_panic(scope).empty());
    CHECK(panic_message_count(scope) == 0);

    // Control: the default scope DOES contain CC 120; release-only must not.
    const auto full = make_panic(PanicScope{});
    const auto release = make_panic(PanicScope::release_only());
    CHECK(count_kind(full, PanicMessageKind::all_sound_off) == 16);
    CHECK(count_kind(release, PanicMessageKind::all_sound_off) == 0);
    CHECK(count_kind(release, PanicMessageKind::all_notes_off) == 16);
    CHECK(release.size() == 64);

    PanicScope only_bend{.all_sound_off = false,
                         .all_notes_off = false,
                         .reset_all_controllers = false,
                         .sustain_off = false};
    const auto bend = make_panic(only_bend);
    REQUIRE(bend.size() == 16);
    for (const auto& e : bend)
        CHECK(classify_panic_message(e) == PanicMessageKind::pitch_bend_centre);
}

TEST_CASE("panic_messages stamps the sample offset and truncates on whole events",
          "[midi][panic]") {
    for (const auto& e : make_panic(PanicScope{}, 37))
        CHECK(e.sample_offset == 37);

    std::array<MidiEvent, 7> small{};
    const auto n = panic_messages(PanicScope{}, small);
    CHECK(n == 7);
    CHECK(n < panic_message_count(PanicScope{}));
    CHECK(panic_messages(PanicScope{}, std::span<MidiEvent>{}) == 0);
}

TEST_CASE("held_note_offs sends note-offs only for keys that are down", "[midi][panic]") {
    MidiKeyboardState keys;
    keys.process(MidiEvent::note_on(0, 60, 100));
    keys.process(MidiEvent::note_on(0, 64, 90));
    keys.process(MidiEvent::note_on(3, 72, 80));
    keys.process(MidiEvent::note_on(0, 67, 70));
    keys.process(MidiEvent::note_off(0, 64));   // released: must not appear
    keys.process(MidiEvent::note_on(0, 67, 0)); // velocity-0 note-on is a release

    std::array<MidiEvent, kMaxNoteOffs> buf{};
    const auto n = held_note_offs(keys, buf, 0xFFFF, 12);
    REQUIRE(n == 2);
    CHECK(buf[0].data()[0] == 0x80);
    CHECK(buf[0].data()[1] == 60);
    CHECK(buf[1].data()[0] == 0x83);
    CHECK(buf[1].data()[1] == 72);
    CHECK(buf[0].sample_offset == 12);

    // Channel mask: only channel 3.
    CHECK(held_note_offs(keys, buf, 1u << 3) == 1);
    // Nothing held -> nothing sent (the precise form never invents notes).
    keys.reset();
    CHECK(held_note_offs(keys, buf) == 0);

    // Truncation at a whole event.
    keys.process(MidiEvent::note_on(0, 60, 100));
    keys.process(MidiEvent::note_on(0, 61, 100));
    std::array<MidiEvent, 1> one{};
    CHECK(held_note_offs(keys, one) == 1);
}

TEST_CASE("all_note_offs covers every key on the selected channels", "[midi][panic]") {
    std::vector<MidiEvent> buf(kMaxNoteOffs);
    REQUIRE(all_note_offs(buf) == 2048);
    CHECK(buf.front().data()[0] == 0x80);
    CHECK(buf.front().data()[1] == 0);
    CHECK(buf.back().data()[0] == 0x8F);
    CHECK(buf.back().data()[1] == 127);

    CHECK(all_note_offs(buf, 0b10) == 128);
    CHECK(buf[0].data()[0] == 0x81);
    CHECK(all_note_offs(buf, 0) == 0);
}

TEST_CASE("classify_panic_message accepts only the exact panic messages", "[midi][panic]") {
    CHECK(classify_panic_message(MidiEvent::cc(5, 120, 0)) == PanicMessageKind::all_sound_off);
    CHECK(classify_panic_message(MidiEvent::cc(5, 123, 0)) == PanicMessageKind::all_notes_off);
    CHECK(classify_panic_message(MidiEvent::cc(5, 121, 0)) ==
          PanicMessageKind::reset_all_controllers);
    CHECK(classify_panic_message(MidiEvent::cc(5, 64, 0)) == PanicMessageKind::sustain_off);
    CHECK(classify_panic_message(MidiEvent::pitch_bend(5, 8192)) ==
          PanicMessageKind::pitch_bend_centre);

    // Near misses are not panic messages.
    CHECK_FALSE(classify_panic_message(MidiEvent::cc(5, 123, 127)).has_value());
    CHECK_FALSE(classify_panic_message(MidiEvent::cc(5, 64, 127)).has_value());
    CHECK_FALSE(classify_panic_message(MidiEvent::cc(5, 7, 0)).has_value());
    CHECK_FALSE(classify_panic_message(MidiEvent::pitch_bend(5, 8191)).has_value());
    CHECK_FALSE(classify_panic_message(MidiEvent::note_off(5, 60)).has_value());
    CHECK_FALSE(classify_panic_message(MidiEvent::program_change(5, 0)).has_value());
}
