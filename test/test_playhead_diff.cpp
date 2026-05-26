// Item 1.3 — AudioPlayHead transport-extension adapter wiring helpers.
//
// `detail::derive_bar_from_beats` and `detail::compute_playhead_changes`
// are the two pieces of pure logic the VST3, AU v2 / v3, and CLAP
// adapters share when they populate the new `ProcessContext` fields
// from their respective host APIs. Each adapter then queries its host
// (VST3 `Vst::ProcessContext`, AU v2 `CallHostBeatAndTempo` /
// `CallHostMusicalTimeLocation` / `CallHostTransportState`, AU v3
// `musicalContextBlock` / `transportStateBlock`, CLAP
// `clap_event_transport`) and feeds the populated context through the
// two helpers — so pinning the helpers covers the cross-adapter
// contract end-to-end. Per-host validation that actually drives a real
// DAW (Logic, Cubase, Bitwig, Reaper) is captured as a follow-up under
// item 1.3 of the macOS plan.
//
// The adapter wiring itself is exercised by:
//   * test_clap_entry.cpp / test_clap_host_validation.cpp — CLAP
//     adapter loads + render at the dlopen level.
//   * test_vst3_plugin_state.cpp — VST3 SingleComponentEffect state
//     round-trip.
//   * test_au_v2_effect.cpp + test_au_v2_cocoa_ui.mm — AU v2 surface.
//   * test_au_plugin_state.mm — AU v3 state surface.
// Those tests don't drive the host's playhead push API (no host is
// running), so the field-population path here lives behind a host
// driver. That gap is the planned 1.3 acceptance test once a real-DAW
// harness exists.

#include <catch2/catch_test_macros.hpp>

#include <pulp/format/detail/playhead_diff.hpp>
#include <pulp/format/processor.hpp>

using pulp::format::FrameRate;
using pulp::format::ProcessContext;
using pulp::format::detail::PlayheadSnapshot;
using pulp::format::detail::compute_playhead_changes;
using pulp::format::detail::derive_bar_from_beats;

TEST_CASE("derive_bar_from_beats: 4/4 maps every 4 beats to one bar",
          "[format][playhead][item-13][derive-bar]") {
    ProcessContext ctx;
    ctx.time_sig_numerator = 4;
    ctx.time_sig_denominator = 4;

    ctx.position_beats = 0.0;
    derive_bar_from_beats(ctx);
    REQUIRE(ctx.bar == 0);

    ctx.position_beats = 3.999;
    derive_bar_from_beats(ctx);
    REQUIRE(ctx.bar == 0);

    ctx.position_beats = 4.0;
    derive_bar_from_beats(ctx);
    REQUIRE(ctx.bar == 1);

    ctx.position_beats = 16.5;
    derive_bar_from_beats(ctx);
    REQUIRE(ctx.bar == 4);
}

TEST_CASE("derive_bar_from_beats: 3/4 maps every 3 beats to one bar",
          "[format][playhead][item-13][derive-bar]") {
    ProcessContext ctx;
    ctx.time_sig_numerator = 3;
    ctx.time_sig_denominator = 4;

    ctx.position_beats = 0.0;
    derive_bar_from_beats(ctx);
    REQUIRE(ctx.bar == 0);

    ctx.position_beats = 2.999;
    derive_bar_from_beats(ctx);
    REQUIRE(ctx.bar == 0);

    ctx.position_beats = 3.0;
    derive_bar_from_beats(ctx);
    REQUIRE(ctx.bar == 1);

    ctx.position_beats = 9.5;
    derive_bar_from_beats(ctx);
    REQUIRE(ctx.bar == 3);
}

TEST_CASE("derive_bar_from_beats: 6/8 maps every 3 quarter notes to one bar",
          "[format][playhead][item-13][derive-bar]") {
    // 6/8 = 6 eighths per bar = 3 quarter notes per bar.
    ProcessContext ctx;
    ctx.time_sig_numerator = 6;
    ctx.time_sig_denominator = 8;

    ctx.position_beats = 0.0;
    derive_bar_from_beats(ctx);
    REQUIRE(ctx.bar == 0);

    ctx.position_beats = 2.999;
    derive_bar_from_beats(ctx);
    REQUIRE(ctx.bar == 0);

    ctx.position_beats = 3.0;
    derive_bar_from_beats(ctx);
    REQUIRE(ctx.bar == 1);

    ctx.position_beats = 9.0;
    derive_bar_from_beats(ctx);
    REQUIRE(ctx.bar == 3);
}

TEST_CASE("derive_bar_from_beats: degenerate time signatures stay at bar 0",
          "[format][playhead][item-13][derive-bar][edge]") {
    ProcessContext ctx;
    ctx.position_beats = 17.0;

    SECTION("numerator <= 0") {
        ctx.time_sig_numerator = 0;
        ctx.time_sig_denominator = 4;
        derive_bar_from_beats(ctx);
        REQUIRE(ctx.bar == 0);
    }

    SECTION("denominator <= 0") {
        ctx.time_sig_numerator = 4;
        ctx.time_sig_denominator = 0;
        derive_bar_from_beats(ctx);
        REQUIRE(ctx.bar == 0);
    }

    SECTION("both <= 0") {
        ctx.time_sig_numerator = 0;
        ctx.time_sig_denominator = 0;
        derive_bar_from_beats(ctx);
        REQUIRE(ctx.bar == 0);
    }
}

TEST_CASE("compute_playhead_changes: first call after construction reports no changes",
          "[format][playhead][item-13][change-flags]") {
    PlayheadSnapshot snapshot;
    REQUIRE_FALSE(snapshot.has_previous);

    ProcessContext ctx;
    ctx.tempo_bpm = 137.5;
    ctx.time_sig_numerator = 7;
    ctx.time_sig_denominator = 8;
    ctx.is_playing = true;
    ctx.is_recording = true;
    ctx.is_looping = true;

    compute_playhead_changes(ctx, snapshot);

    REQUIRE_FALSE(ctx.tempo_changed);
    REQUIRE_FALSE(ctx.time_sig_changed);
    REQUIRE_FALSE(ctx.transport_changed);

    REQUIRE(snapshot.has_previous);
    REQUIRE(snapshot.tempo_bpm == 137.5);
    REQUIRE(snapshot.time_sig_numerator == 7);
    REQUIRE(snapshot.time_sig_denominator == 8);
    REQUIRE(snapshot.is_playing);
    REQUIRE(snapshot.is_recording);
    REQUIRE(snapshot.is_looping);
}

TEST_CASE("compute_playhead_changes: second identical call still reports no changes",
          "[format][playhead][item-13][change-flags]") {
    PlayheadSnapshot snapshot;
    ProcessContext ctx;
    ctx.tempo_bpm = 120.0;
    ctx.time_sig_numerator = 4;
    ctx.time_sig_denominator = 4;

    compute_playhead_changes(ctx, snapshot);  // first call seeds the snapshot
    compute_playhead_changes(ctx, snapshot);  // identical context

    REQUIRE_FALSE(ctx.tempo_changed);
    REQUIRE_FALSE(ctx.time_sig_changed);
    REQUIRE_FALSE(ctx.transport_changed);
}

TEST_CASE("compute_playhead_changes: tempo bump flips tempo_changed only",
          "[format][playhead][item-13][change-flags]") {
    PlayheadSnapshot snapshot;
    ProcessContext seed;
    seed.tempo_bpm = 120.0;
    compute_playhead_changes(seed, snapshot);

    ProcessContext next;
    next.tempo_bpm = 140.0;  // changed
    next.time_sig_numerator = 4;
    next.time_sig_denominator = 4;
    compute_playhead_changes(next, snapshot);

    REQUIRE(next.tempo_changed);
    REQUIRE_FALSE(next.time_sig_changed);
    REQUIRE_FALSE(next.transport_changed);
}

TEST_CASE("compute_playhead_changes: time-sig numerator change flips time_sig_changed",
          "[format][playhead][item-13][change-flags]") {
    PlayheadSnapshot snapshot;
    ProcessContext seed;
    seed.time_sig_numerator = 4;
    seed.time_sig_denominator = 4;
    compute_playhead_changes(seed, snapshot);

    ProcessContext next;
    next.tempo_bpm = 120.0;
    next.time_sig_numerator = 3;  // changed
    next.time_sig_denominator = 4;
    compute_playhead_changes(next, snapshot);

    REQUIRE_FALSE(next.tempo_changed);
    REQUIRE(next.time_sig_changed);
    REQUIRE_FALSE(next.transport_changed);
}

TEST_CASE("compute_playhead_changes: time-sig denominator change flips time_sig_changed",
          "[format][playhead][item-13][change-flags]") {
    PlayheadSnapshot snapshot;
    ProcessContext seed;
    seed.time_sig_numerator = 4;
    seed.time_sig_denominator = 4;
    compute_playhead_changes(seed, snapshot);

    ProcessContext next;
    next.tempo_bpm = 120.0;
    next.time_sig_numerator = 4;
    next.time_sig_denominator = 8;  // changed
    compute_playhead_changes(next, snapshot);

    REQUIRE_FALSE(next.tempo_changed);
    REQUIRE(next.time_sig_changed);
    REQUIRE_FALSE(next.transport_changed);
}

TEST_CASE("compute_playhead_changes: is_playing flip raises transport_changed",
          "[format][playhead][item-13][change-flags]") {
    PlayheadSnapshot snapshot;
    ProcessContext seed;
    seed.is_playing = false;
    compute_playhead_changes(seed, snapshot);

    ProcessContext next;
    next.tempo_bpm = 120.0;
    next.time_sig_numerator = 4;
    next.time_sig_denominator = 4;
    next.is_playing = true;  // changed
    compute_playhead_changes(next, snapshot);

    REQUIRE_FALSE(next.tempo_changed);
    REQUIRE_FALSE(next.time_sig_changed);
    REQUIRE(next.transport_changed);
}

TEST_CASE("compute_playhead_changes: is_recording or is_looping flip raises transport_changed",
          "[format][playhead][item-13][change-flags]") {
    PlayheadSnapshot snapshot;
    ProcessContext seed;
    compute_playhead_changes(seed, snapshot);

    SECTION("recording flip") {
        ProcessContext next;
        next.tempo_bpm = 120.0;
        next.time_sig_numerator = 4;
        next.time_sig_denominator = 4;
        next.is_recording = true;
        compute_playhead_changes(next, snapshot);
        REQUIRE(next.transport_changed);
    }

    SECTION("looping flip") {
        ProcessContext next;
        next.tempo_bpm = 120.0;
        next.time_sig_numerator = 4;
        next.time_sig_denominator = 4;
        next.is_looping = true;
        compute_playhead_changes(next, snapshot);
        REQUIRE(next.transport_changed);
    }
}

TEST_CASE("compute_playhead_changes: multiple fields can flip in the same block",
          "[format][playhead][item-13][change-flags]") {
    PlayheadSnapshot snapshot;
    ProcessContext seed;
    seed.tempo_bpm = 100.0;
    seed.time_sig_numerator = 4;
    seed.time_sig_denominator = 4;
    seed.is_playing = false;
    compute_playhead_changes(seed, snapshot);

    ProcessContext next;
    next.tempo_bpm = 200.0;
    next.time_sig_numerator = 7;
    next.time_sig_denominator = 8;
    next.is_playing = true;
    compute_playhead_changes(next, snapshot);

    REQUIRE(next.tempo_changed);
    REQUIRE(next.time_sig_changed);
    REQUIRE(next.transport_changed);
}

TEST_CASE("compute_playhead_changes: returning to previous values clears the flags",
          "[format][playhead][item-13][change-flags]") {
    PlayheadSnapshot snapshot;
    ProcessContext seed;
    seed.tempo_bpm = 120.0;
    seed.time_sig_numerator = 4;
    seed.time_sig_denominator = 4;
    compute_playhead_changes(seed, snapshot);

    ProcessContext bump;
    bump.tempo_bpm = 180.0;
    bump.time_sig_numerator = 4;
    bump.time_sig_denominator = 4;
    compute_playhead_changes(bump, snapshot);
    REQUIRE(bump.tempo_changed);

    ProcessContext steady;
    steady.tempo_bpm = 180.0;
    steady.time_sig_numerator = 4;
    steady.time_sig_denominator = 4;
    compute_playhead_changes(steady, snapshot);

    REQUIRE_FALSE(steady.tempo_changed);
    REQUIRE_FALSE(steady.time_sig_changed);
    REQUIRE_FALSE(steady.transport_changed);
}
