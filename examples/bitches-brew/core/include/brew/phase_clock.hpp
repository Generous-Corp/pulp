#pragma once

// The clock every cyclic generator in this suite runs on.
//
// The LFO and the Step LFO are the same plug-in above the waist: one turns a cycle
// count into a waveform, the other turns it into a step index. Everything below —
// the eight sync modes, the origin a reset moves, the accumulator the two
// wall-clock modes need, the drift a parked `Transport` builds up — is identical,
// subtle, and exactly the sort of thing that rots when it is written twice.
//
// So it is written once, here, with no waveform in it and no `Processor` either:
// the caller fills a `Transport` from whatever its host handed it, and gets back a
// cycle count. See brew/lfo.hpp for what the eight modes mean.

#include <brew/clock.hpp>   // Swing, swing_unwarp
#include <brew/lfo.hpp>     // SyncMode

#include <cstdint>

namespace pulp::examples::brew {

/// What a host told us about this block. A mirror of the fields of
/// `format::ProcessContext` a clock can read, so brew-core stays free of the
/// format layer.
struct Transport {
    double position_beats = 0.0;
    double position_seconds = 0.0;
    /// Derived from the tempo, and non-zero even while the transport is parked —
    /// the wall-clock modes need a rate to run at.
    double beats_per_sample = 0.0;
    double sample_rate = 48000.0;
    bool playing = false;
    /// True on the first block of a run. Latch it from the host's own flag *or*
    /// from a false→true edge on `playing`, because not every host sets the flag.
    bool play_edge = false;

    /// The beat position advances within the block only while the transport rolls.
    [[nodiscard]] double beats_at(double n) const noexcept {
        return position_beats + (playing ? beats_per_sample * n : 0.0);
    }
    [[nodiscard]] double seconds_at(double n) const noexcept {
        return position_seconds + (playing && sample_rate > 0.0 ? n / sample_rate : 0.0);
    }
};

/// How fast, and in which of the two units.
struct ClockSettings {
    SyncMode mode = SyncMode::transport;
    /// Hertz, for the `Free*` modes. Already clamped: see `free_hz`.
    double hz = 1.0;
    /// Beats in one cycle, for the beat modes. See `cycle_beats`.
    double cycle_beats = 1.0;
    /// Warps the beat timeline. Meaningless in the hertz modes, and ignored there.
    Swing swing{};
};

/// A cycle counter with an origin, an accumulator, and a drift.
///
/// One instance per independent voice. It holds three numbers because the eight
/// modes need three different notions of "where we are":
///
///   `origin`  — what a reset moved. Every mode measures cycles from it, so one
///               mechanism retriggers all of them.
///   `wall`    — the accumulator `Free` and `Tempo` run on. There is no timeline in
///               either mode, which is exactly why neither bounces bit-identically.
///   `drift`   — how far a parked `Transport` has run past the timeline. Discarded
///               on the play edge, so the phase snaps back onto the position and a
///               bounce cannot depend on how long the transport sat stopped.
class PhaseClock {
public:
    void reset() noexcept {
        origin_beats_ = 0.0;
        origin_seconds_ = 0.0;
        wall_ = 0.0;
        drift_ = 0.0;
    }

    /// Move the point cycles are counted from — a note-on, or a `Free2` play edge.
    void set_origin(double beats, double seconds) noexcept {
        origin_beats_ = beats;
        origin_seconds_ = seconds;
        wall_ = 0.0;
        drift_ = 0.0;
    }

    /// Call once per block, before the sample loop. Handles the play edge.
    void begin_block(const ClockSettings& s, const Transport& t) noexcept {
        if (!t.play_edge) return;
        if (sync_resets_on_play(s.mode)) set_origin(t.position_beats, t.position_seconds);
        // Rolling means locked. Zeroed *before* the block renders, not after: a
        // drift still on the books through the first playing block is a bounce that
        // depends on how long the user left the transport stopped.
        drift_ = 0.0;
    }

    /// Elapsed cycles at sample `n` of the block. The one place the modes differ.
    [[nodiscard]] double cycles_at(const ClockSettings& s, const Transport& t,
                                   double n) const noexcept {
        switch (s.mode) {
            case SyncMode::free:
            case SyncMode::trig_free:
                // Wall clock, always. The one mode with no timeline in it at all.
                // A trigger mode's clock is the same clock; what the trigger gates
                // is the pattern reading it, one step boundary at a time.
                if (!(t.sample_rate > 0.0)) return wall_;
                return wall_ + n * s.hz / t.sample_rate;

            case SyncMode::tempo:
            case SyncMode::trig_tempo:
                // Wall clock too, but at the tempo's rate rather than a hertz knob's.
                if (!(s.cycle_beats > 0.0)) return wall_;
                return wall_ + n * t.beats_per_sample / s.cycle_beats;

            case SyncMode::transport: {
                // Locked while playing, free-running while parked. That is the only
                // thing separating it from `Transport2`.
                const double parked = t.playing || !(s.cycle_beats > 0.0)
                                          ? 0.0
                                          : n * t.beats_per_sample / s.cycle_beats;
                return beats_cycles(s, t, n) + drift_ + parked;
            }

            case SyncMode::transport2:
                return beats_cycles(s, t, n);

            case SyncMode::free2:
            case SyncMode::free3:
                return (t.seconds_at(n) - origin_seconds_) * s.hz;

            case SyncMode::start_stop:
                return 0.0;

            case SyncMode::quadrature:
                break;   // resolved to the leader's clock before this is called
        }
        return 0.0;
    }

    /// Call once per block, after the sample loop. Advances whatever accumulates.
    void end_block(const ClockSettings& s, const Transport& t, int frames) noexcept {
        const double n = static_cast<double>(frames);
        if (s.mode == SyncMode::free || s.mode == SyncMode::trig_free) {
            if (t.sample_rate > 0.0) wall_ += n * s.hz / t.sample_rate;
        } else if (s.mode == SyncMode::tempo || s.mode == SyncMode::trig_tempo) {
            if (s.cycle_beats > 0.0) wall_ += n * t.beats_per_sample / s.cycle_beats;
        } else if (s.mode == SyncMode::transport && !t.playing) {
            // Only while parked. `begin_block` already zeroed it on the play edge.
            if (s.cycle_beats > 0.0) drift_ += n * t.beats_per_sample / s.cycle_beats;
        }
    }

    [[nodiscard]] double origin_beats() const noexcept { return origin_beats_; }
    [[nodiscard]] double origin_seconds() const noexcept { return origin_seconds_; }

private:
    /// Cycles from the origin, on the (possibly swung) beat timeline. Swing warps
    /// the *position* before the position becomes a phase — mapping a sounding beat
    /// back to the straight beat it stands for, which is the coordinate a cycle
    /// count is measured in.
    [[nodiscard]] double beats_cycles(const ClockSettings& s, const Transport& t,
                                      double n) const noexcept {
        return cycles_from_beats(t.beats_at(n), origin_beats_, s.cycle_beats, s.swing);
    }

    double origin_beats_ = 0.0;
    double origin_seconds_ = 0.0;
    double wall_ = 0.0;
    double drift_ = 0.0;
};

}  // namespace pulp::examples::brew
