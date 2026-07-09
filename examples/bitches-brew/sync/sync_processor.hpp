#pragma once

// Sync — a clock and run/stop gate, locked to the host transport.
//
// Two outputs, both normalized CV (see brew/cv.hpp):
//
//   Channel 0 — Clock. A pulse train at `Pulses Per Beat`, scaled by Multiplier
//               and Divisor. Each pulse is `Trigger Length` long, clamped so the
//               gate always falls before the next edge (brew/pulse.hpp).
//   Channel 1 — Run/Stop. High while the host transport is running, low when it
//               is stopped. DIN-sync's run gate.
//
// The clock is derived from the host's reported position, never accumulated
// (brew/clock.hpp). This is the single most important decision in the plug-in:
// it means "hit play at bar 5" emits the pulses that belong to bar 5 and nothing
// else. An accumulator would have to catch up from wherever it left off, and the
// audible result is a burst of pulses at the moment the transport starts — which
// then arrives at a modular system as a fistful of spurious clock ticks.
//
// The features that *are* run-relative (skip the first pulse, wait for the bar
// line) hang off one explicit origin captured on the play edge, in
// `brew/run_segment.hpp`. Nothing else in this plug-in carries musical state.
//
// Swing is the one thing here that bends the grid, and it does so by warping the
// beat axis rather than by nudging edges after they are found — see `clock.hpp`.
// Straight (50%) is bit-identical to no swing.
//
// Not implemented, deliberately. FSK tape sync needs a continuous phase
// accumulator — re-deriving its phase from the position each block would click at
// every block boundary and mis-decode at the receiver — so it wants designing, not
// bolting on. Periodic reset stays out until its documented behavior is in hand;
// a plausible guess would ship as a green test asserting the guess.

#include <brew/clock.hpp>
#include <brew/cv.hpp>
#include <brew/pulse.hpp>
#include <brew/run_segment.hpp>

#include <pulp/format/processor.hpp>
#include <pulp/state/store.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

namespace pulp::examples::brew {

class SyncProcessor : public format::Processor {
public:
    // Parameter IDs are part of the persisted state contract. Never renumber.
    enum ParamId : state::ParamID {
        kPulsesPerBeat = 1,
        kMultiplier = 2,
        kDivisor = 3,
        kTriggerLengthMs = 4,
        kRunLevel = 5,
        kSkipFirst = 6,
        kWaitForBar = 7,
        kOutputScale = 8,
        kInvert = 9,
        kFirstDelayMs = 10,
        kOffsetMs = 11,
        kSwingPercent = 12,
        kSwingUnit = 13,
    };

    /// DIN sync runs at 24 pulses per quarter note. It is the default because it
    /// is the thing most hardware expects.
    static constexpr float kDinSyncPulsesPerBeat = 24.0f;

    format::PluginDescriptor descriptor() const override {
        return {
            .name = "Sync",
            .manufacturer = "Bitches Brew",
            .bundle_id = "com.bitchesbrew.sync",
            .version = "0.1.0",
            .category = format::PluginCategory::Effect,
            .input_buses = {{"Main In", 2}},
            .output_buses = {{"Clock / Run", 2}},
        };
    }

    void define_parameters(state::StateStore& store) override {
        store.add_parameter({.id = kPulsesPerBeat,
                             .name = "Pulses Per Beat",
                             .unit = "ppqn",
                             .range = {1.0f, 48.0f, kDinSyncPulsesPerBeat, 1.0f}});
        store.add_parameter({.id = kSwingPercent,
                             .name = "Swing",
                             .unit = "%",
                             .range = {25.0f, 75.0f, 50.0f, 0.1f}});
        // Which subdivision's off-beat moves. Off is the eighth, on the
        // sixteenth — the same two choices every swing control offers, because
        // they are the two that correspond to how people count.
        store.add_parameter({.id = kSwingUnit,
                             .name = "Swing Sixteenths",
                             .unit = "",
                             .range = {0.0f, 1.0f, 0.0f, 1.0f}});
        store.add_parameter({.id = kMultiplier,
                             .name = "Multiplier",
                             .unit = "",
                             .range = {1.0f, 16.0f, 1.0f, 1.0f}});
        store.add_parameter({.id = kDivisor,
                             .name = "Divisor",
                             .unit = "",
                             .range = {1.0f, 16.0f, 1.0f, 1.0f}});
        // 5 ms clears the ~1 ms DAC floor with margin and stays well under the
        // 8.3 ms period of 24 ppqn at 300 BPM.
        store.add_parameter({.id = kTriggerLengthMs,
                             .name = "Trigger Length",
                             .unit = "ms",
                             .range = {0.1f, 100.0f, 5.0f, 0.1f}});
        store.add_parameter({.id = kRunLevel,
                             .name = "Run Level",
                             .unit = "",
                             .range = {0.0f, 1.0f, 1.0f, 0.01f}});
        store.add_parameter({.id = kSkipFirst,
                             .name = "Skip First Pulse",
                             .unit = "",
                             .range = {0.0f, 1.0f, 0.0f, 1.0f}});
        store.add_parameter({.id = kWaitForBar,
                             .name = "Wait For Bar",
                             .unit = "",
                             .range = {0.0f, 1.0f, 0.0f, 1.0f}});
        store.add_parameter({.id = kOutputScale,
                             .name = "Output Scale",
                             .unit = "",
                             .range = {0.0f, 1.0f, 1.0f, 0.001f}});
        store.add_parameter({.id = kInvert,
                             .name = "Invert",
                             .unit = "",
                             .range = {0.0f, 1.0f, 0.0f, 1.0f}});
        // Hold the clock off for this long after the transport starts. Measured
        // from the run origin, so two runs starting at different beats behave
        // identically.
        store.add_parameter({.id = kFirstDelayMs,
                             .name = "First Delay",
                             .unit = "ms",
                             .range = {0.0f, 1000.0f, 0.0f, 1.0f}});
        // Slide the whole pulse train earlier or later. A DAC, its reconstruction
        // filter, and the receiving gate input all add latency, so a clock that is
        // sample-accurate in software arrives late at the hardware. Negative
        // values pull the pulses ahead of the beat to compensate.
        store.add_parameter({.id = kOffsetMs,
                             .name = "Offset",
                             .unit = "ms",
                             .range = {-50.0f, 50.0f, 0.0f, 0.1f}});
    }

    void prepare(const format::PrepareContext&) override { hard_reset(); }

    /// The size this editor actually needs: three rows of knobs, the toggles, and
    /// the two lamps. Without this override a host opens the plug-in at
    /// Processor's 400x300 default — a geometry the layout was never checked at.
    std::pair<uint32_t, uint32_t> editor_size() const override {
        return {360, 306};
    }

    /// Defined in sync_view.cpp so the audio translation units never see the
    /// view stack.
    std::unique_ptr<view::View> create_view() override;

    /// Peak clock level and run-gate level of the last block, for the editor's
    /// indicator lamps. Written once per block on the audio thread with relaxed
    /// ordering: a lamp that reads a stale value for one frame is invisible, and
    /// paying for synchronization here would not be.
    [[nodiscard]] float clock_lamp() const noexcept {
        return clock_lamp_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] float run_lamp() const noexcept {
        return run_lamp_.load(std::memory_order_relaxed);
    }

    /// Milliseconds expressed in beats at a given tempo. Zero for a stopped clock.
    [[nodiscard]] static double ms_to_beats(double ms, double tempo_bpm) noexcept {
        if (!(tempo_bpm > 0.0)) return 0.0;
        return ms / 1000.0 * tempo_bpm / 60.0;
    }

    /// The edges-per-beat implied by the current settings. Exposed so a test can
    /// predict the edge grid without duplicating the parameter plumbing.
    /// The beat-axis warp the current settings ask for. Straight (50%) yields a
    /// `Swing` that `swing_active` reports inactive, so the clock is untouched.
    [[nodiscard]] Swing current_swing() const noexcept {
        return {.unit_beats = as_toggle(state().get_value(kSwingUnit))
                                  ? kSixteenthBeats
                                  : kEighthBeats,
                .amount = static_cast<double>(state().get_value(kSwingPercent)) /
                          100.0};
    }

    [[nodiscard]] double current_edges_per_beat() const noexcept {
        return edges_per_beat(
            static_cast<double>(state().get_value(kPulsesPerBeat)),
            static_cast<int>(std::lround(state().get_value(kMultiplier))),
            static_cast<int>(std::lround(state().get_value(kDivisor))));
    }

    void process(audio::BufferView<float>& output,
                 const audio::BufferView<const float>&,
                 midi::MidiBuffer&,
                 midi::MidiBuffer&,
                 const format::ProcessContext& ctx) override {
        const std::size_t channels = output.num_channels();
        const int frames = static_cast<int>(output.num_samples());
        if (channels == 0 || frames <= 0) return;

        const float scale = state().get_value(kOutputScale);
        const bool invert = as_toggle(state().get_value(kInvert));

        float* clock_ch = output.channel_ptr(0);
        float* run_ch = channels > 1 ? output.channel_ptr(1) : nullptr;

        // Bypass means stop driving the patch. Some hosts bypass by
        // short-circuiting process(); others keep calling it, and on those a
        // clock that ignores the flag keeps pulsing after the user pressed
        // Bypass. Drop the in-flight pulse so un-bypassing cannot resume a gate
        // mid-high, but leave the edge grid alone — it is position-derived, so
        // it resumes on the correct edge by itself.
        if (ctx.is_bypassed) {
            pulse_.reset();
            const float off = resolve_output(0.0f, scale, invert);
            fill(clock_ch, frames, off);
            fill(run_ch, frames, off);
            publish_lamps(0.0f, 0.0f);
            return;
        }

        // The host asked us to flush. Drop the in-flight pulse and the edge
        // dedupe, but keep the run segment: a reset mid-run must not re-arm
        // "skip first pulse" and swallow a pulse the user already heard start.
        if (ctx.reset_requested) flush();

        // Stopped: no clock, and the run gate sits at its stop level. Note the
        // grid is NOT reset here — the play edge does that, so a host that
        // stutters is_playing between blocks cannot re-arm the first pulse.
        if (!ctx.is_playing) {
            const float stopped = resolve_output(0.0f, scale, invert);
            fill(clock_ch, frames, stopped);
            fill(run_ch, frames, stopped);
            publish_lamps(0.0f, 0.0f);
            return;
        }

        if (ctx.transport_started) begin_run(ctx);

        const float run_level =
            resolve_output(state().get_value(kRunLevel), scale, invert);
        fill(run_ch, frames, run_level);

        const double epb = current_edges_per_beat();
        const double bps = beats_per_sample(ctx.tempo_bpm, ctx.sample_rate);
        const double period =
            pulse_period_samples(ctx.sample_rate, ctx.tempo_bpm, epb);
        const std::int64_t width = trigger_width_samples(
            static_cast<double>(state().get_value(kTriggerLengthMs)),
            ctx.sample_rate, period);

        const bool skip_first = as_toggle(state().get_value(kSkipFirst));
        const float high = resolve_output(kFullScale, scale, invert);
        const float low = resolve_output(0.0f, scale, invert);

        // Walk the block once, filling up to each edge and then retriggering. The
        // grid calls back in ascending sample order, so `written` only moves
        // forward.
        // Sliding the *window* rather than the emitted sample offsets keeps the
        // grid contiguous block to block, so the dedupe and the backwards-jump
        // re-arm still see a monotonic timeline. Offsetting after the fact would
        // let a pulse land outside the block it was derived in.
        const double offset_beats =
            ms_to_beats(static_cast<double>(state().get_value(kOffsetMs)),
                        ctx.tempo_bpm);

        const Swing swing = current_swing();

        int written = 0;
        bool pulsed = false;
        grid_.advance(ctx.position_beats - offset_beats, epb, bps, frames, swing,
                      [&](int offset, std::int64_t index) {
                          // The gate lives in host-position time, so compare the
                          // beat the pulse actually *emits* at, not the grid beat
                          // it was derived from — those differ by the offset, and
                          // by the swing.
                          const double at =
                              ClockGrid::edge_beats(index, epb, swing) +
                              offset_beats;
                          if (!run_.passes_gate(at)) return;
                          if (skip_first && !run_.skip_consumed) {
                              run_.skip_consumed = true;
                              return;
                          }
                          run_.skip_consumed = true;
                          render_until(clock_ch, written, offset, high, low);
                          pulse_.trigger(width);
                          pulsed = true;
                      });
        render_until(clock_ch, written, frames, high, low);

        // A pulse narrower than the block would flicker a lamp driven by the
        // final sample, so latch on "an edge happened anywhere in this block".
        publish_lamps(pulsed || pulse_.high() ? 1.0f : 0.0f,
                      std::abs(run_level));
    }

    void process(format::ProcessBuffers& audio,
                 midi::MidiBuffer& midi_in,
                 midi::MidiBuffer& midi_out,
                 const format::ProcessContext& ctx) override {
        if (auto* out = audio.main_output()) {
            audio::BufferView<const float> unused_input;
            process(*out, unused_input, midi_in, midi_out, ctx);
        }
    }

private:
    /// Drop everything that is about *this instant*: the in-flight pulse and the
    /// edge dedupe. Keeps the run segment.
    void flush() noexcept {
        grid_.reset();
        pulse_.reset();
    }

    /// Flush, and forget the run too. Only on `prepare()`, where there is no run.
    void hard_reset() noexcept {
        flush();
        run_ = RunSegment{};
    }

    /// Capture the run origin on the play edge and re-arm everything that is
    /// defined relative to it.
    void begin_run(const format::ProcessContext& ctx) noexcept {
        const double start =
            as_toggle(state().get_value(kWaitForBar))
                ? next_bar_at_or_after(
                      ctx.position_beats,
                      beats_per_bar(ctx.time_sig_numerator, ctx.time_sig_denominator))
                : ctx.position_beats;

        // The delay is converted to beats once, from the tempo at the play edge.
        // A tempo change during the delay therefore does not re-time it — the
        // alternative, re-deriving it every block, would let a tempo automation
        // ramp move a gate the user set in milliseconds.
        const double delay = ms_to_beats(
            static_cast<double>(state().get_value(kFirstDelayMs)), ctx.tempo_bpm);

        run_.begin(ctx.position_beats, start + delay);
        flush();
    }

    void publish_lamps(float clock, float run) noexcept {
        clock_lamp_.store(clock, std::memory_order_relaxed);
        run_lamp_.store(run, std::memory_order_relaxed);
    }

    static void fill(float* dst, int frames, float value) noexcept {
        if (dst == nullptr) return;
        for (int n = 0; n < frames; ++n) dst[n] = value;
    }

    /// Advance the pulse countdown from `written` up to `limit`, writing the gate
    /// level for each sample. `written` is left at `limit`.
    void render_until(float* dst, int& written, int limit, float high,
                      float low) noexcept {
        for (; written < limit; ++written) {
            const bool on = pulse_.tick();
            if (dst != nullptr) dst[written] = on ? high : low;
        }
    }

    ClockGrid grid_;
    PulseShaper pulse_;
    RunSegment run_;
    std::atomic<float> clock_lamp_{0.0f};
    std::atomic<float> run_lamp_{0.0f};
};

inline std::unique_ptr<format::Processor> create_sync() {
    return std::make_unique<SyncProcessor>();
}

}  // namespace pulp::examples::brew
