#include <catch2/catch_test_macros.hpp>
#include <pulp/midi/mpe_voice_tracker.hpp>
#include <pulp/midi/ump.hpp>

using namespace pulp::midi;

// ────────────────────────────────────────────────────────────────────────
// MpeVoiceTracker consumes UMP per-note management (status 0xF0) and
// assignable per-note CC (status 0x10) through the shared UMP factories.
// ────────────────────────────────────────────────────────────────────────

namespace {

UmpPacket note_on(uint8_t channel, uint8_t note, uint16_t velocity = 0x8000) {
    return UmpPacket::note_on_2(/*group=*/0, channel, note, velocity);
}

} // namespace

TEST_CASE("MpeVoiceTracker assignable PNC is ignored without a configured index",
          "[midi][mpe][per-note-management]") {
    MpeVoiceTracker tracker{MpeConfig::standard_lower(4)};
    REQUIRE_FALSE(tracker.assignable_timbre_index().has_value());

    REQUIRE(tracker.process(note_on(/*ch=*/1, /*note=*/60)));
    REQUIRE(tracker.active_count() == 1);

    // Send assignable per-note CC index 17 with half-scale value.
    auto pkt = UmpPacket::assignable_per_note_cc(
        /*group=*/0, /*channel=*/1, /*note=*/60,
        /*cc_index=*/17, /*value=*/0x80000000u);
    REQUIRE(tracker.process(pkt));

    const auto* n = tracker.find(1, 60);
    REQUIRE(n != nullptr);
    REQUIRE(n->timbre == 0.0f); // tracker has no index bound → ignored
}

TEST_CASE("MpeVoiceTracker assignable PNC routes to timbre when index is bound",
          "[midi][mpe][per-note-management]") {
    MpeVoiceTracker tracker{MpeConfig::standard_lower(4)};
    tracker.set_assignable_timbre_index(uint8_t{17});
    REQUIRE(tracker.assignable_timbre_index() == uint8_t{17});

    REQUIRE(tracker.process(note_on(/*ch=*/1, /*note=*/60)));

    // Wrong index — must NOT affect timbre.
    auto wrong = UmpPacket::assignable_per_note_cc(
        /*group=*/0, /*channel=*/1, /*note=*/60,
        /*cc_index=*/4, /*value=*/0xFFFFFFFFu);
    REQUIRE(tracker.process(wrong));
    REQUIRE(tracker.find(1, 60)->timbre == 0.0f);

    // Right index — applies to timbre.
    auto right = UmpPacket::assignable_per_note_cc(
        /*group=*/0, /*channel=*/1, /*note=*/60,
        /*cc_index=*/17, /*value=*/0x80000000u);
    REQUIRE(tracker.process(right));
    const auto* n = tracker.find(1, 60);
    REQUIRE(n != nullptr);
    // 0x80000000 / 0xFFFFFFFF ≈ 0.5
    REQUIRE(n->timbre > 0.4999f);
    REQUIRE(n->timbre < 0.5001f);
}

TEST_CASE("MpeVoiceTracker per-note management reset returns expression to defaults",
          "[midi][mpe][per-note-management]") {
    MpeVoiceTracker tracker{MpeConfig::standard_lower(4)};
    REQUIRE(tracker.process(note_on(/*ch=*/2, /*note=*/64)));

    // Apply some per-note expression first.
    REQUIRE(tracker.process(UmpPacket::per_note_pitch_bend(
        /*group=*/0, /*channel=*/2, /*note=*/64, /*value=*/0xFFFFFFFFu)));
    REQUIRE(tracker.process(UmpPacket::registered_per_note_cc(
        /*group=*/0, /*channel=*/2, /*note=*/64,
        /*cc_index=*/74, /*value=*/0xFFFFFFFFu)));

    const auto* n_before = tracker.find(2, 64);
    REQUIRE(n_before != nullptr);
    REQUIRE(n_before->pitch_bend_semitones > 0.0f);
    REQUIRE(n_before->timbre > 0.99f);

    // Per-note management with reset flag.
    auto mgmt = UmpPacket::per_note_management(
        /*group=*/0, /*channel=*/2, /*note=*/64,
        /*flags=*/UmpPacket::kPerNoteResetControllers);
    REQUIRE(tracker.process(mgmt));

    const auto* n_after = tracker.find(2, 64);
    REQUIRE(n_after != nullptr);
    REQUIRE(n_after->pitch_bend_semitones == 0.0f);
    REQUIRE(n_after->pressure == 0.0f);
    REQUIRE(n_after->timbre == 0.0f);
    REQUIRE_FALSE(n_after->detached); // reset alone does not detach
}

TEST_CASE("MpeVoiceTracker per-note management detach blocks channel-level updates",
          "[midi][mpe][per-note-management]") {
    MpeVoiceTracker tracker{MpeConfig::standard_lower(4)};
    REQUIRE(tracker.process(note_on(/*ch=*/3, /*note=*/72)));

    // Detach the note from channel-level controllers.
    auto detach = UmpPacket::per_note_management(
        /*group=*/0, /*channel=*/3, /*note=*/72,
        /*flags=*/UmpPacket::kPerNoteDetachControllers);
    REQUIRE(tracker.process(detach));
    REQUIRE(tracker.find(3, 72)->detached);

    // Channel-level pitch bend (status 0xE0) — must NOT propagate to the
    // detached note.
    REQUIRE(tracker.process(UmpPacket::pitch_bend_2(
        /*group=*/0, /*channel=*/3, /*value=*/0xFFFFFFFFu)));
    REQUIRE(tracker.find(3, 72)->pitch_bend_semitones == 0.0f);

    // Channel-level pressure (status 0xD0) — must NOT propagate.
    UmpPacket pressure;
    pressure.word_count = 2;
    pressure.words[0] = (0x4u << 28) | (uint32_t{0xD0u | 3u} << 16);
    pressure.words[1] = 0xFFFFFFFFu;
    REQUIRE(tracker.process(pressure));
    REQUIRE(tracker.find(3, 72)->pressure == 0.0f);

    // Channel-level CC74 (status 0xB0) — must NOT propagate.
    REQUIRE(tracker.process(UmpPacket::cc_2(
        /*group=*/0, /*channel=*/3, /*controller=*/74, /*value=*/0xFFFFFFFFu)));
    REQUIRE(tracker.find(3, 72)->timbre == 0.0f);

    // Per-note targeted message — DOES apply even on a detached note.
    REQUIRE(tracker.process(UmpPacket::per_note_pitch_bend(
        /*group=*/0, /*channel=*/3, /*note=*/72, /*value=*/0xFFFFFFFFu)));
    REQUIRE(tracker.find(3, 72)->pitch_bend_semitones > 0.0f);
}

TEST_CASE("MpeVoiceTracker note-on retrigger clears detach state",
          "[midi][mpe][per-note-management]") {
    MpeVoiceTracker tracker{MpeConfig::standard_lower(4)};
    REQUIRE(tracker.process(note_on(/*ch=*/1, /*note=*/60)));

    REQUIRE(tracker.process(UmpPacket::per_note_management(
        /*group=*/0, /*channel=*/1, /*note=*/60,
        /*flags=*/UmpPacket::kPerNoteDetachControllers)));
    REQUIRE(tracker.find(1, 60)->detached);

    // Retrigger note-on without an intervening note-off.
    REQUIRE(tracker.process(note_on(/*ch=*/1, /*note=*/60, /*velocity=*/0x4000)));
    REQUIRE_FALSE(tracker.find(1, 60)->detached);

    // Channel-level pitch bend now propagates again.
    REQUIRE(tracker.process(UmpPacket::pitch_bend_2(
        /*group=*/0, /*channel=*/1, /*value=*/0xFFFFFFFFu)));
    REQUIRE(tracker.find(1, 60)->pitch_bend_semitones > 0.0f);
}

TEST_CASE("MpeVoiceTracker note-on retrigger re-seeds expression from channel cache",
          "[midi][mpe][per-note-management][regression]") {
    // Sequence: note-on → detach → channel pitch bend changes (withheld
    // from the detached note but recorded in the per-channel cache) →
    // note-on retrigger (no intervening note-off). The re-attached note
    // must resume at the CURRENT channel value, not the stale value frozen
    // at detach time. The fresh-slot allocation path already seeds from the
    // per-channel cache; the retrigger path must do the same.
    MpeVoiceTracker tracker{MpeConfig::standard_lower(4)};
    REQUIRE(tracker.process(note_on(/*ch=*/1, /*note=*/60)));
    REQUIRE(tracker.find(1, 60)->pitch_bend_semitones == 0.0f);

    // Detach the live note from channel-level controllers.
    REQUIRE(tracker.process(UmpPacket::per_note_management(
        /*group=*/0, /*channel=*/1, /*note=*/60,
        /*flags=*/UmpPacket::kPerNoteDetachControllers)));
    REQUIRE(tracker.find(1, 60)->detached);

    // Channel-level pitch bend changes WHILE the note is detached. The
    // detached note keeps 0, but the per-channel cache now holds the new
    // bend value.
    REQUIRE(tracker.process(UmpPacket::pitch_bend_2(
        /*group=*/0, /*channel=*/1, /*value=*/0xFFFFFFFFu)));
    REQUIRE(tracker.find(1, 60)->pitch_bend_semitones == 0.0f); // withheld

    // Retrigger the same (channel, note) without a note-off. The slot is
    // re-attached (detach cleared) and must adopt the current channel bend.
    REQUIRE(tracker.process(note_on(/*ch=*/1, /*note=*/60, /*velocity=*/0x4000)));
    const auto* n = tracker.find(1, 60);
    REQUIRE(n != nullptr);
    REQUIRE_FALSE(n->detached);
    // Old code: stale 0.0 (frozen at detach). New code: current channel
    // bend — a near-full-scale bend over the default 48-semitone member
    // range lands close to +48.
    REQUIRE(n->pitch_bend_semitones > 0.0f);
    REQUIRE(n->pitch_bend_semitones > 47.0f);
}

TEST_CASE("MpeVoiceTracker D+S per-note management preserves live note state",
          "[midi][mpe][per-note-management][regression][issue-2860]") {
    // Per the MIDI 2.0 UMP spec § Per-Note Management, when both detach
    // and reset bits are set, detach takes effect on the currently
    // sounding note (its controller state is preserved for the rest of
    // its lifecycle) and reset arms for future note-ons at this index.
    // Earlier impl zeroed expression on the live note, breaking the
    // expressive playback the D+S note-rotation flow depends on.
    MpeVoiceTracker tracker{MpeConfig::standard_lower(4)};
    REQUIRE(tracker.process(note_on(/*ch=*/2, /*note=*/64)));
    REQUIRE(tracker.process(UmpPacket::per_note_pitch_bend(
        /*group=*/0, /*channel=*/2, /*note=*/64, /*value=*/0xFFFFFFFFu)));
    const float live_pitch_bend = tracker.find(2, 64)->pitch_bend_semitones;
    REQUIRE(live_pitch_bend > 0.0f);

    auto combo = UmpPacket::per_note_management(
        /*group=*/0, /*channel=*/2, /*note=*/64,
        /*flags=*/static_cast<uint8_t>(
            UmpPacket::kPerNoteResetControllers
            | UmpPacket::kPerNoteDetachControllers));
    REQUIRE(tracker.process(combo));

    const auto* n = tracker.find(2, 64);
    REQUIRE(n != nullptr);
    // D+S: live note state PRESERVED + detach applied.
    REQUIRE(n->pitch_bend_semitones == live_pitch_bend);
    REQUIRE(n->detached);

    // Channel-level pitch bend updates still skip the detached note.
    REQUIRE(tracker.process(UmpPacket::pitch_bend_2(
        /*group=*/0, /*channel=*/2, /*value=*/0u))); // bend toward 0
    REQUIRE(tracker.find(2, 64)->pitch_bend_semitones == live_pitch_bend);
}

TEST_CASE("MpeVoiceTracker per-note management only targets the matching note",
          "[midi][mpe][per-note-management]") {
    MpeVoiceTracker tracker{MpeConfig::standard_lower(4)};
    REQUIRE(tracker.process(note_on(/*ch=*/1, /*note=*/60)));
    REQUIRE(tracker.process(note_on(/*ch=*/1, /*note=*/64)));

    REQUIRE(tracker.process(UmpPacket::per_note_management(
        /*group=*/0, /*channel=*/1, /*note=*/60,
        /*flags=*/UmpPacket::kPerNoteDetachControllers)));

    REQUIRE(tracker.find(1, 60)->detached);
    REQUIRE_FALSE(tracker.find(1, 64)->detached);

    // Channel-level pitch bend reaches note 64 (not detached) but
    // skips note 60.
    REQUIRE(tracker.process(UmpPacket::pitch_bend_2(
        /*group=*/0, /*channel=*/1, /*value=*/0xFFFFFFFFu)));
    REQUIRE(tracker.find(1, 60)->pitch_bend_semitones == 0.0f);
    REQUIRE(tracker.find(1, 64)->pitch_bend_semitones > 0.0f);
}

TEST_CASE("MpeVoiceTracker set_assignable_timbre_index masks to 7-bit range",
          "[midi][mpe][per-note-management]") {
    MpeVoiceTracker tracker;
    tracker.set_assignable_timbre_index(uint8_t{0xFF}); // out of 7-bit range
    REQUIRE(tracker.assignable_timbre_index() == uint8_t{0x7F}); // masked

    tracker.set_assignable_timbre_index(std::nullopt);
    REQUIRE_FALSE(tracker.assignable_timbre_index().has_value());
}
