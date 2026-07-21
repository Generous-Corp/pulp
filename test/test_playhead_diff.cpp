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
using pulp::format::TransportField;
using pulp::format::detail::PlayheadSnapshot;
using pulp::format::detail::TransportDiffMode;
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
    REQUIRE_FALSE(ctx.transport_jump);

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

TEST_CASE("compute_playhead_changes: continuous sample-position advance is not a jump",
          "[format][playhead][phase2][transport-jump]") {
    PlayheadSnapshot snapshot;
    ProcessContext seed;
    seed.sample_rate = 48000.0;
    seed.num_samples = 256;
    seed.position_samples = 1024;
    seed.is_playing = true;
    compute_playhead_changes(seed, snapshot);

    ProcessContext next;
    next.sample_rate = 48000.0;
    next.num_samples = 256;
    next.position_samples = 1280;
    next.is_playing = true;
    compute_playhead_changes(next, snapshot);

    REQUIRE_FALSE(next.transport_jump);
}

TEST_CASE("compute_playhead_changes: playing sample-position discontinuity is a jump",
          "[format][playhead][phase2][transport-jump]") {
    PlayheadSnapshot snapshot;
    ProcessContext seed;
    seed.sample_rate = 48000.0;
    seed.num_samples = 256;
    seed.position_samples = 1024;
    seed.is_playing = true;
    compute_playhead_changes(seed, snapshot);

    ProcessContext next;
    next.sample_rate = 48000.0;
    next.num_samples = 256;
    next.position_samples = 4096;
    next.is_playing = true;
    compute_playhead_changes(next, snapshot);

    REQUIRE(next.transport_jump);
    REQUIRE_FALSE(next.transport_changed);
}

TEST_CASE("compute_playhead_changes: stopped sample-position change is a jump",
          "[format][playhead][phase2][transport-jump]") {
    PlayheadSnapshot snapshot;
    ProcessContext seed;
    seed.position_samples = 2048;
    seed.is_playing = false;
    compute_playhead_changes(seed, snapshot);

    ProcessContext next;
    next.position_samples = 1024;
    next.is_playing = false;
    compute_playhead_changes(next, snapshot);

    REQUIRE(next.transport_jump);
    REQUIRE_FALSE(next.transport_changed);
}

TEST_CASE("compute_playhead_changes: transport edge with continuous sample advance is not a jump",
          "[format][playhead][phase2][transport-jump]") {
    PlayheadSnapshot snapshot;
    ProcessContext seed;
    seed.sample_rate = 48000.0;
    seed.num_samples = 256;
    seed.position_samples = 1024;
    seed.is_playing = true;
    compute_playhead_changes(seed, snapshot);

    ProcessContext stopped;
    stopped.sample_rate = 48000.0;
    stopped.num_samples = 256;
    stopped.position_samples = 1280;
    stopped.is_playing = false;
    compute_playhead_changes(stopped, snapshot);

    REQUIRE(stopped.transport_changed);
    REQUIRE_FALSE(stopped.transport_jump);
}

TEST_CASE("compute_playhead_changes: beat-position fallback distinguishes continuous advance",
          "[format][playhead][phase2][transport-jump]") {
    PlayheadSnapshot snapshot;
    ProcessContext seed;
    seed.sample_rate = 48000.0;
    seed.num_samples = 480;
    seed.tempo_bpm = 120.0;
    seed.position_beats = 4.0;
    seed.is_playing = true;
    compute_playhead_changes(seed, snapshot);

    ProcessContext next = seed;
    next.position_beats = 4.02;
    compute_playhead_changes(next, snapshot);
    REQUIRE_FALSE(next.transport_jump);

    ProcessContext jumped = next;
    jumped.position_beats = 8.0;
    compute_playhead_changes(jumped, snapshot);
    REQUIRE(jumped.transport_jump);
}

// ---------------------------------------------------------------------------
// transport_started — the run-start edge.
//
// Pressing play at a parked position leaves position_samples where it was, so
// `transport_jump` is (correctly) false and `should_reset_dsp_state()` returns
// false. A tempo-synced generator that keyed its phase reset off
// should_reset_dsp_state() therefore never reset on play: it resumed a stale
// phase and emitted every backlogged event at once — a pulse burst on the
// first block of playback. `transport_started` is the signal that was missing.
//
// It is deliberately NOT folded into should_reset_dsp_state(): a transport
// start is not a timeline discontinuity, and reverb/delay tails must survive it.

TEST_CASE("compute_playhead_changes: play edge raises transport_started without a jump",
          "[format][playhead][change-flags][transport-start]") {
    PlayheadSnapshot snapshot;

    // Parked at a non-zero position, stopped. Establishes `has_previous`.
    ProcessContext parked;
    parked.sample_rate = 48000.0;
    parked.num_samples = 512;
    parked.tempo_bpm = 120.0;
    parked.is_playing = false;
    parked.position_samples = 96000;
    parked.position_beats = 4.0;
    compute_playhead_changes(parked, snapshot);
    REQUIRE_FALSE(parked.transport_started);

    // The user hits play. The playhead has not moved.
    ProcessContext rolling;
    rolling.sample_rate = 48000.0;
    rolling.num_samples = 512;
    rolling.tempo_bpm = 120.0;
    rolling.is_playing = true;
    rolling.position_samples = 96000;
    rolling.position_beats = 4.0;
    compute_playhead_changes(rolling, snapshot);

    REQUIRE(rolling.transport_started);
    REQUIRE(rolling.transport_changed);
    // The whole point: this is a start, not a seek. Nothing moved, so nothing
    // may claim a discontinuity, and tails must not be told to reset.
    REQUIRE_FALSE(rolling.transport_jump);
    REQUIRE_FALSE(rolling.should_reset_dsp_state());
}

TEST_CASE("compute_playhead_changes: continued playback does not re-raise transport_started",
          "[format][playhead][change-flags][transport-start]") {
    PlayheadSnapshot snapshot;

    ProcessContext first;
    first.sample_rate = 48000.0;
    first.num_samples = 512;
    first.tempo_bpm = 120.0;
    first.is_playing = true;
    first.position_samples = 0;
    compute_playhead_changes(first, snapshot);
    REQUIRE(first.transport_started);  // first block, already rolling

    ProcessContext second;
    second.sample_rate = 48000.0;
    second.num_samples = 512;
    second.tempo_bpm = 120.0;
    second.is_playing = true;
    second.position_samples = 512;  // advanced normally
    compute_playhead_changes(second, snapshot);

    REQUIRE_FALSE(second.transport_started);
    REQUIRE_FALSE(second.transport_changed);
    REQUIRE_FALSE(second.transport_jump);
}

TEST_CASE("compute_playhead_changes: first block reports a start only when already playing",
          "[format][playhead][change-flags][transport-start]") {
    SECTION("instantiated mid-playback: the first block begins a run") {
        // A plugin dropped onto a track while the transport is already rolling
        // has no previous block to diff against. Reporting no start here would
        // leave a clock with no run origin until the user stopped and
        // restarted the transport.
        PlayheadSnapshot snapshot;
        ProcessContext ctx;
        ctx.is_playing = true;
        compute_playhead_changes(ctx, snapshot);

        REQUIRE(ctx.transport_started);
        // Still no *changes* on a first block — there is nothing to diff.
        REQUIRE_FALSE(ctx.transport_changed);
        REQUIRE_FALSE(ctx.transport_jump);
    }

    SECTION("instantiated while stopped: no run has begun") {
        PlayheadSnapshot snapshot;
        ProcessContext ctx;
        ctx.is_playing = false;
        compute_playhead_changes(ctx, snapshot);

        REQUIRE_FALSE(ctx.transport_started);
    }
}

TEST_CASE("compute_playhead_changes: stop edge and seek do not raise transport_started",
          "[format][playhead][change-flags][transport-start]") {
    PlayheadSnapshot snapshot;

    ProcessContext rolling;
    rolling.sample_rate = 48000.0;
    rolling.num_samples = 512;
    rolling.tempo_bpm = 120.0;
    rolling.is_playing = true;
    rolling.position_samples = 0;
    compute_playhead_changes(rolling, snapshot);

    SECTION("seek while playing is a jump, not a start") {
        ProcessContext seeked;
        seeked.sample_rate = 48000.0;
        seeked.num_samples = 512;
        seeked.tempo_bpm = 120.0;
        seeked.is_playing = true;
        seeked.position_samples = 480000;  // not previous + num_samples
        compute_playhead_changes(seeked, snapshot);

        REQUIRE_FALSE(seeked.transport_started);
        REQUIRE(seeked.transport_jump);
    }

    SECTION("stopping is a transport change, not a start") {
        ProcessContext stopped;
        stopped.sample_rate = 48000.0;
        stopped.num_samples = 512;
        stopped.tempo_bpm = 120.0;
        stopped.is_playing = false;
        stopped.position_samples = 512;
        compute_playhead_changes(stopped, snapshot);

        REQUIRE_FALSE(stopped.transport_started);
        REQUIRE(stopped.transport_changed);
    }
}

TEST_CASE("compute_playhead_changes: stop then play raises transport_started again",
          "[format][playhead][change-flags][transport-start]") {
    PlayheadSnapshot snapshot;
    const auto block = [&](bool playing, int64_t pos) {
        ProcessContext ctx;
        ctx.sample_rate = 48000.0;
        ctx.num_samples = 512;
        ctx.tempo_bpm = 120.0;
        ctx.is_playing = playing;
        ctx.position_samples = pos;
        compute_playhead_changes(ctx, snapshot);
        return ctx;
    };

    REQUIRE(block(true, 0).transport_started);        // first block, rolling
    REQUIRE_FALSE(block(true, 512).transport_started); // still rolling
    REQUIRE_FALSE(block(false, 1024).transport_started);
    // Re-arm: a second run must produce a second start edge, or a generator
    // that captured its run origin on the first start never re-anchors.
    REQUIRE(block(true, 1024).transport_started);
}

TEST_CASE("field-valid playhead accepts zero positions as authoritative",
          "[format][playhead][validity][transport-jump]") {
    SECTION("sample position advances continuously from zero") {
        PlayheadSnapshot snapshot;
        ProcessContext first;
        first.sample_rate = 48000.0;
        first.num_samples = 64;
        first.is_playing = true;
        first.position_samples = 0;
        first.transport_validity.set(TransportField::Playing);
        first.transport_validity.set(TransportField::SamplePosition);
        compute_playhead_changes(first, snapshot, TransportDiffMode::FieldValidity);

        ProcessContext next = first;
        next.position_samples = 64;
        compute_playhead_changes(next, snapshot, TransportDiffMode::FieldValidity);
        REQUIRE_FALSE(next.transport_jump);
    }

    SECTION("beat position advances continuously from zero") {
        PlayheadSnapshot snapshot;
        ProcessContext first;
        first.sample_rate = 48000.0;
        first.num_samples = 64;
        first.is_playing = true;
        first.tempo_bpm = 120.0;
        first.position_beats = 0.0;
        first.transport_validity.set(TransportField::Playing);
        first.transport_validity.set(TransportField::Tempo);
        first.transport_validity.set(TransportField::BeatPosition);
        compute_playhead_changes(first, snapshot, TransportDiffMode::FieldValidity);

        ProcessContext next = first;
        next.position_beats = (64.0 / 48000.0) * (120.0 / 60.0);
        compute_playhead_changes(next, snapshot, TransportDiffMode::FieldValidity);
        REQUIRE_FALSE(next.transport_jump);
    }
}

TEST_CASE("field validity acquisition and loss raise change flags",
          "[format][playhead][validity][change-flags]") {
    PlayheadSnapshot snapshot;
    ProcessContext unavailable;
    unavailable.tempo_bpm = 120.0;
    compute_playhead_changes(
        unavailable, snapshot, TransportDiffMode::FieldValidity);

    ProcessContext acquired = unavailable;
    acquired.transport_validity.set(TransportField::Tempo);
    compute_playhead_changes(acquired, snapshot, TransportDiffMode::FieldValidity);
    REQUIRE(acquired.tempo_changed);

    ProcessContext lost = unavailable;
    compute_playhead_changes(lost, snapshot, TransportDiffMode::FieldValidity);
    REQUIRE(lost.tempo_changed);
    REQUIRE_FALSE(lost.transport_jump);
}

TEST_CASE("field-valid play state is required for a transport start",
          "[format][playhead][validity][transport-start]") {
    SECTION("unavailable play state") {
        PlayheadSnapshot snapshot;
        ProcessContext ctx;
        ctx.is_playing = true;
        compute_playhead_changes(ctx, snapshot, TransportDiffMode::FieldValidity);
        REQUIRE_FALSE(ctx.transport_started);
    }

    SECTION("available play state") {
        PlayheadSnapshot snapshot;
        ProcessContext ctx;
        ctx.is_playing = true;
        ctx.transport_validity.set(TransportField::Playing);
        compute_playhead_changes(ctx, snapshot, TransportDiffMode::FieldValidity);
        REQUIRE(ctx.transport_started);
    }
}

TEST_CASE("position validity transitions do not synthesize jumps",
          "[format][playhead][validity][transport-jump]") {
    SECTION("sample position disappears") {
        PlayheadSnapshot snapshot;
        ProcessContext first;
        first.sample_rate = 48000.0;
        first.num_samples = 64;
        first.position_samples = 4096;
        first.transport_validity.set(TransportField::SamplePosition);
        compute_playhead_changes(first, snapshot, TransportDiffMode::FieldValidity);

        ProcessContext next;
        next.sample_rate = 48000.0;
        next.num_samples = 64;
        compute_playhead_changes(next, snapshot, TransportDiffMode::FieldValidity);
        REQUIRE_FALSE(next.transport_jump);
    }

    SECTION("position source switches without overlap") {
        PlayheadSnapshot snapshot;
        ProcessContext samples;
        samples.sample_rate = 48000.0;
        samples.num_samples = 64;
        samples.position_samples = 4096;
        samples.transport_validity.set(TransportField::SamplePosition);
        compute_playhead_changes(samples, snapshot, TransportDiffMode::FieldValidity);

        ProcessContext beats;
        beats.sample_rate = 48000.0;
        beats.num_samples = 64;
        beats.position_beats = 12.0;
        beats.transport_validity.set(TransportField::BeatPosition);
        compute_playhead_changes(beats, snapshot, TransportDiffMode::FieldValidity);
        REQUIRE_FALSE(beats.transport_jump);
    }

    SECTION("remaining beat source establishes continuity") {
        PlayheadSnapshot snapshot;
        ProcessContext first;
        first.sample_rate = 48000.0;
        first.num_samples = 64;
        first.is_playing = true;
        first.tempo_bpm = 120.0;
        first.position_beats = 4.0;
        first.position_samples = 96000;
        first.transport_validity.set(TransportField::Playing);
        first.transport_validity.set(TransportField::Tempo);
        first.transport_validity.set(TransportField::BeatPosition);
        first.transport_validity.set(TransportField::SamplePosition);
        compute_playhead_changes(first, snapshot, TransportDiffMode::FieldValidity);

        ProcessContext next = first;
        next.position_beats += (64.0 / 48000.0) * (120.0 / 60.0);
        next.transport_validity.set(TransportField::SamplePosition, false);
        compute_playhead_changes(next, snapshot, TransportDiffMode::FieldValidity);
        REQUIRE_FALSE(next.transport_jump);
    }
}
