#pragma once

// Sync — a clock and run/stop gate, locked to the host transport.
//
// Two outputs, both normalized CV (see brew/cv.hpp):
//
//   Channel 0 — Clock. A pulse train at the rate `Clock Type` selects, scaled by
//               Multiplier and Divisor. Each pulse is `Trigger Length` long,
//               clamped so the gate always falls before the next edge
//               (brew/pulse.hpp). A `Trigger Length` of zero is not a
//               zero-width pulse: it selects a 50% duty square wave.
//   Channel 1 — Run. What it carries depends on `Run Signal`. `Run` is a level,
//               high for the whole run — that is what DIN sync needs. `Start`,
//               `Start/Stop` and `Stop` are pulses, and a step sequencer's reset
//               input wants `Start`.
//
// Whatever is at the inputs is summed into the outputs, so a stack of these can
// drive one pair of jacks.
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
// Periodic reset is the one feature measured from the run origin rather than
// from an absolute beat: "reset every four bars" means four bars since the user
// pressed play. It rides the same pulse shaper as the run signal, off a grid
// anchored to `RunSegment::origin_beats`.
//
// Not implemented, deliberately. FSK tape sync needs a continuous phase
// accumulator — re-deriving its phase from the position each block would click at
// every block boundary and mis-decode at the receiver — so it wants designing, not
// bolting on.

#include <brew/clock.hpp>
#include <brew/param_text.hpp>
#include <brew/cv.hpp>
#include <brew/pulse.hpp>
#include <brew/run_segment.hpp>
#include <brew/sync.hpp>

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
        kClockType = 14,
        kRunType = 15,
        kResetBeats = 16,
        kResetUnit = 17,
        kRunPulseMs = 18,
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

    // ── How each control reads, here and in the host ─────────────────────────
    //
    // Two of these are not formatting but meaning: a zero on `Reset Beats` is the
    // off switch, and a zero on `Trigger Length` selects a 50%-duty square rather
    // than a zero-width pulse. A host that showed "0" for either would be
    // describing a control that does not exist.

    /// "24pp", not "24": the PPQN knob next door also reads 24 by default, and two
    /// adjacent knobs showing the same number invite the reading that one of them
    /// is doing nothing.
    static std::string clock_type_name(float v) {
        static const char* const kNames[] = {"Off", "24pp", "48pp", "Cust"};
        return text::named_at(kNames, kClockTypeCount, v);
    }
    static std::string run_signal_name(float v) {
        static const char* const kNames[] = {"Run", "Start", "St/Sp", "Stop"};
        return text::named_at(kNames, kRunSignalCount, v);
    }
    static std::string note_unit_name(float v) {
        return text::divisor_name(v);
    }
    static std::string beats_or_off(float v) {
        return v < 0.5f ? std::string("Off") : text::whole(v);
    }
    static std::string width_or_square(float v) {
        return v <= 0.0f ? std::string("50%") : text::millis(v);
    }

    void define_parameters(state::StateStore& store) override {
        // 24 ppqn is DIN sync, and it is what most hardware expects. `Custom`
        // hands the rate to the knob below; `Off` silences the clock output, for
        // a patch that wants only the run signal.
        store.add_parameter(
            {.id = kClockType,
             .name = "Clock Type",
             .unit = "",
             .range = {0.0f, static_cast<float>(kClockTypeCount - 1),
                       static_cast<float>(static_cast<int>(ClockType::ppqn24)), 1.0f},
             .to_string = clock_type_name});
        store.add_parameter({.id = kPulsesPerBeat,
                             .name = "Pulses Per Beat",
                             .unit = "ppqn",
                             .range = {1.0f, 48.0f, kDinSyncPulsesPerBeat, 1.0f},
                             .to_string = text::whole});
        // Derived from the clock's own bounds rather than repeated here: a knob
        // whose range outruns the warp's invertible interval silently clamps, and
        // the user is left turning a control that stopped doing anything.
        store.add_parameter({.id = kSwingPercent,
                             .name = "Swing",
                             .unit = "%",
                             .range = {static_cast<float>(kMinSwing * 100.0),
                                       static_cast<float>(kMaxSwing * 100.0),
                                       50.0f, 0.1f},
                             .to_string = text::percent1});
        // Which subdivision's off-beat moves. Off is the eighth, on the
        // sixteenth — the same two choices every swing control offers, because
        // they are the two that correspond to how people count.
        store.add_parameter({.id = kSwingUnit,
                             .name = "Swing Sixteenths",
                             .unit = "",
                             .range = {0.0f, 1.0f, 0.0f, 1.0f},
                             .to_string = text::on_off});
        store.add_parameter({.id = kMultiplier,
                             .name = "Multiplier",
                             .unit = "",
                             .range = {1.0f, 16.0f, 1.0f, 1.0f},
                             .to_string = text::whole});
        store.add_parameter({.id = kDivisor,
                             .name = "Divisor",
                             .unit = "",
                             .range = {1.0f, 16.0f, 1.0f, 1.0f},
                             .to_string = text::whole});
        // 5 ms clears the ~1 ms DAC floor with margin and stays well under the
        // 8.3 ms period of 24 ppqn at 300 BPM. Zero is not a zero-length pulse:
        // it means "not a trigger at all", and the clock becomes a 50% duty
        // square wave, which is what some receivers want to see.
        store.add_parameter({.id = kTriggerLengthMs,
                             .name = "Trigger Length",
                             .unit = "ms",
                             .range = {0.0f, 100.0f, 5.0f, 0.1f},
                             .to_string = width_or_square});
        // What the run output carries. `Run` is a level held high for the whole
        // run — DIN sync needs that. The other three are pulses, and `Start` is
        // what a hardware step sequencer wants on its reset input.
        store.add_parameter(
            {.id = kRunType,
             .name = "Run Signal",
             .unit = "",
             .range = {0.0f, static_cast<float>(kRunSignalCount - 1), 0.0f, 1.0f},
             .to_string = run_signal_name});
        // The length of a run *pulse*. Separate from the clock's trigger length:
        // a reset pulse is read by a different input, on different hardware, with
        // a different minimum width.
        store.add_parameter({.id = kRunPulseMs,
                             .name = "Run Pulse Length",
                             .unit = "ms",
                             .range = {0.1f, 100.0f, 5.0f, 0.1f},
                             .to_string = text::millis});
        // Reset not just at transport start, but every `Reset Beats` x
        // `Reset Unit`. Zero beats is off. Only meaningful when the run output is
        // pulsed: a reset pulse cannot be picked out of a run *level*.
        store.add_parameter({.id = kResetBeats,
                             .name = "Reset Beats",
                             .unit = "",
                             .range = {0.0f, 64.0f, 0.0f, 1.0f},
                             .to_string = beats_or_off});
        store.add_parameter(
            {.id = kResetUnit,
             .name = "Reset Unit",
             .unit = "",
             .range = {0.0f, static_cast<float>(kNoteUnitCount - 1),
                       static_cast<float>(static_cast<int>(NoteUnit::quarter)), 1.0f},
             .to_string = note_unit_name});
        store.add_parameter({.id = kRunLevel,
                             .name = "Run Level",
                             .unit = "",
                             .range = {0.0f, 1.0f, 1.0f, 0.01f},
                             .to_string = text::fraction_percent});
        store.add_parameter({.id = kSkipFirst,
                             .name = "Skip First Pulse",
                             .unit = "",
                             .range = {0.0f, 1.0f, 0.0f, 1.0f},
                             .to_string = text::on_off});
        store.add_parameter({.id = kWaitForBar,
                             .name = "Wait For Bar",
                             .unit = "",
                             .range = {0.0f, 1.0f, 0.0f, 1.0f},
                             .to_string = text::on_off});
        store.add_parameter({.id = kOutputScale,
                             .name = "Output Scale",
                             .unit = "",
                             .range = {0.0f, 1.0f, 1.0f, 0.001f},
                             .to_string = text::fraction_percent});
        store.add_parameter({.id = kInvert,
                             .name = "Invert",
                             .unit = "",
                             .range = {0.0f, 1.0f, 0.0f, 1.0f},
                             .to_string = text::on_off});
        // Hold the clock off for this long after the transport starts. Measured
        // from the run origin, so two runs starting at different beats behave
        // identically.
        store.add_parameter({.id = kFirstDelayMs,
                             .name = "First Delay",
                             .unit = "ms",
                             .range = {0.0f, 1000.0f, 0.0f, 1.0f},
                             .to_string = text::millis});
        // Slide the whole pulse train earlier or later. A DAC, its reconstruction
        // filter, and the receiving gate input all add latency, so a clock that is
        // sample-accurate in software arrives late at the hardware. Negative
        // values pull the pulses ahead of the beat to compensate.
        store.add_parameter({.id = kOffsetMs,
                             .name = "Offset",
                             .unit = "ms",
                             .range = {-50.0f, 50.0f, 0.0f, 0.1f},
                             .to_string = text::signed_millis});
    }

    void prepare(const format::PrepareContext&) override { hard_reset(); }

    /// The size this editor actually needs: three rows of knobs, the toggles, and
    /// the two lamps. Without this override a host opens the plug-in at
    /// Processor's 400x300 default — a geometry the layout was never checked at.
    std::pair<uint32_t, uint32_t> editor_size() const override {
        return {360, 462};
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

    [[nodiscard]] ClockType clock_type() const noexcept {
        return enum_from_param<ClockType>(state().get_value(kClockType), kClockTypeCount);
    }

    [[nodiscard]] RunSignal run_signal() const noexcept {
        return enum_from_param<RunSignal>(state().get_value(kRunType), kRunSignalCount);
    }

    /// Beats between periodic reset pulses. Zero when the feature is off, which
    /// includes every setting of a *level* run output — a reset pulse buried in a
    /// run level is not a reset anybody can read.
    [[nodiscard]] double current_reset_interval() const noexcept {
        if (!run_signal_is_pulsed(run_signal())) return 0.0;
        return reset_interval_beats(
            static_cast<double>(state().get_value(kResetBeats)),
            enum_from_param<NoteUnit>(state().get_value(kResetUnit), kNoteUnitCount));
    }

    [[nodiscard]] double current_edges_per_beat() const noexcept {
        return edges_per_beat(
            clock_pulses_per_beat(clock_type(),
                                  static_cast<double>(state().get_value(kPulsesPerBeat))),
            static_cast<int>(std::lround(state().get_value(kMultiplier))),
            static_cast<int>(std::lround(state().get_value(kDivisor))));
    }

    /// The clock's pulse width in samples. A `Trigger Length` of zero is not a
    /// zero-width pulse — it selects a 50% duty square wave, so the width is half
    /// the period and each edge retriggers exactly as the previous gate falls.
    [[nodiscard]] static std::int64_t clock_width_samples(double requested_ms,
                                                          double sample_rate,
                                                          double period_samples) noexcept {
        if (requested_ms > 0.0)
            return trigger_width_samples(requested_ms, sample_rate, period_samples);
        const auto half = static_cast<std::int64_t>(std::llround(period_samples * 0.5));
        return half > 0 ? half : 1;
    }

    void process(audio::BufferView<float>& output,
                 const audio::BufferView<const float>& input,
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
        // clock that ignores the flag keeps pulsing after the user pressed Bypass.
        //
        // Zero, not a wire. Sync sums its inputs into its outputs, so passing them
        // through looks like the natural reading of "take this plug-in out of the
        // chain" — but the chain ends at a modular system. Whatever a bypassed
        // generator emits goes straight to a jack, and a track carrying audio
        // would arrive at a VCO's pitch input at full scale. Hence the suite's
        // rule: a bypassed *generator* is silent, and a bypassed *processor*
        // (Function, Quantizer, CV To OSC) passes its input through untouched.
        //
        // Drop the in-flight pulses so un-bypassing cannot resume a gate mid-high,
        // but leave the edge grid alone — it is position-derived, so it resumes on
        // the correct edge by itself.
        if (ctx.is_bypassed) {
            pulse_.reset();
            run_pulse_.reset();
            was_playing_ = ctx.is_playing;
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

        const RunSignal run_type = run_signal();
        const bool stopping = was_playing_ && !ctx.is_playing;
        was_playing_ = ctx.is_playing;

        // A stop pulse has to be emitted *after* the transport has stopped, so it
        // is rendered on the stopped path — and it must be allowed to finish
        // there, across as many stopped blocks as its width needs.
        if (stopping && run_signal_pulses_on_stop(run_type))
            run_pulse_.trigger(run_pulse_width(ctx.sample_rate));

        // Stopped: no clock. The run gate sits at its stop level, except for a
        // stop pulse still draining. Note the grid is NOT reset here — the play
        // edge does that, so a host that stutters `is_playing` between blocks
        // cannot re-arm the first pulse.
        if (!ctx.is_playing) {
            pulse_.reset();
            fill(clock_ch, frames, resolve_output(0.0f, scale, invert));
            const float pulse_high =
                resolve_output(state().get_value(kRunLevel), scale, invert);
            const float low = resolve_output(0.0f, scale, invert);
            int written = 0;
            render_pulse_until(run_pulse_, run_ch, written, frames, pulse_high, low);
            add_input(clock_ch, input, 0, frames);
            add_input(run_ch, input, 1, frames);
            publish_lamps(0.0f, run_pulse_.high() ? 1.0f : 0.0f);
            return;
        }

        if (ctx.transport_started) {
            begin_run(ctx);
            if (run_signal_pulses_on_start(run_type))
                run_pulse_.trigger(run_pulse_width(ctx.sample_rate));
        }

        const double epb = current_edges_per_beat();
        const double bps = beats_per_sample(ctx.tempo_bpm, ctx.sample_rate);
        const double period =
            pulse_period_samples(ctx.sample_rate, ctx.tempo_bpm, epb);
        const std::int64_t width = clock_width_samples(
            static_cast<double>(state().get_value(kTriggerLengthMs)),
            ctx.sample_rate, period);

        const float run_lamp = render_run(run_ch, frames, run_type, ctx, bps, scale, invert);

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

        // The plug-in sums whatever is at its inputs into its outputs, so a stack
        // of Sync instances can drive one pair of jacks. Added after the pulse is
        // shaped, and clamped once, at the jack.
        add_input(clock_ch, input, 0, frames);
        add_input(run_ch, input, 1, frames);

        // A pulse narrower than the block would flicker a lamp driven by the
        // final sample, so latch on "an edge happened anywhere in this block".
        publish_lamps(pulsed || pulse_.high() ? 1.0f : 0.0f, run_lamp);
    }

    void process(format::ProcessBuffers& audio,
                 midi::MidiBuffer& midi_in,
                 midi::MidiBuffer& midi_out,
                 const format::ProcessContext& ctx) override {
        if (auto* out = audio.main_output()) {
            static const audio::BufferView<const float> kNoInput;
            const auto* in = audio.main_input();
            process(*out, in ? *in : kNoInput, midi_in, midi_out, ctx);
        }
    }

private:
    /// Drop everything that is about *this instant*: the in-flight pulse and the
    /// edge dedupe. Keeps the run segment.
    void flush() noexcept {
        grid_.reset();
        pulse_.reset();
    }

    /// The width of a run pulse, in samples. Unlike the clock's trigger it has no
    /// period to weld shut, so only the DAC floor bounds it from below.
    [[nodiscard]] std::int64_t run_pulse_width(double sample_rate) const noexcept {
        if (!(sample_rate > 0.0)) return 1;
        const auto w = static_cast<std::int64_t>(std::llround(
            static_cast<double>(state().get_value(kRunPulseMs)) * sample_rate / 1000.0));
        const auto floor_w =
            static_cast<std::int64_t>(std::llround(sample_rate * kMinTriggerSeconds));
        return std::max<std::int64_t>(1, std::max(w, floor_w));
    }

    /// Fill the run channel and return the level the lamp should show.
    ///
    /// `Run` is a held level. The other three are pulses, and the periodic reset
    /// rides the same pulse shaper: its edges come off a grid measured from the
    /// run origin, not from the absolute timeline, because "reset every four
    /// bars" means four bars *since the user pressed play*.
    [[nodiscard]] float render_run(float* dst, int frames, RunSignal type,
                                   const format::ProcessContext& ctx, double bps,
                                   float scale, bool invert) noexcept {
        const float level = resolve_output(state().get_value(kRunLevel), scale, invert);
        if (type == RunSignal::run) {
            fill(dst, frames, level);
            return std::abs(level);
        }

        const float low = resolve_output(0.0f, scale, invert);
        const double interval = current_reset_interval();
        int written = 0;
        if (interval > 0.0) {
            const std::int64_t w = run_pulse_width(ctx.sample_rate);
            reset_grid_.advance(
                ctx.position_beats - run_.origin_beats, 1.0 / interval, bps, frames,
                [&](int offset, std::int64_t index) {
                    // Index 0 sits exactly on the play edge, which the transport
                    // start already pulsed. Firing it again would double-trigger
                    // a sequencer's reset input on every run.
                    if (index <= 0) return;
                    render_pulse_until(run_pulse_, dst, written, offset, level, low);
                    run_pulse_.trigger(w);
                });
        }
        render_pulse_until(run_pulse_, dst, written, frames, level, low);
        return run_pulse_.high() ? std::abs(level) : 0.0f;
    }

    /// Sum one input channel into an output channel, clamped once at the jack.
    /// A missing input channel contributes nothing.
    static void add_input(float* dst, const audio::BufferView<const float>& in,
                          std::size_t channel, int frames) noexcept {
        if (dst == nullptr || channel >= in.num_channels()) return;
        const float* src = in.channel_ptr(channel);
        if (src == nullptr) return;
        for (int n = 0; n < frames; ++n)
            dst[n] = std::clamp(dst[n] + src[n], -kFullScale, kFullScale);
    }

    /// Flush, and forget the run too. Only on `prepare()`, where there is no run.
    void hard_reset() noexcept {
        flush();
        run_pulse_.reset();
        reset_grid_.reset();
        was_playing_ = false;
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
        // The periodic-reset grid counts from the run origin, so a new run must
        // start it over — otherwise the second run's first interval is measured
        // from the first run's start.
        reset_grid_.reset();
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
        render_pulse_until(pulse_, dst, written, limit, high, low);
    }

    /// As `render_until`, for any shaper. The countdown is consumed whether or
    /// not there is a channel to write it to, so a mono bus does not leave a
    /// pulse hanging high forever.
    static void render_pulse_until(PulseShaper& shaper, float* dst, int& written,
                                   int limit, float high, float low) noexcept {
        for (; written < limit; ++written) {
            const bool on = shaper.tick();
            if (dst != nullptr) dst[written] = on ? high : low;
        }
    }

    ClockGrid grid_;
    ClockGrid reset_grid_;
    PulseShaper pulse_;
    PulseShaper run_pulse_;
    RunSegment run_;
    /// The previous block's transport state, so the stop *edge* can be seen. A
    /// stop pulse is emitted after `is_playing` has already gone false, so there
    /// is nothing else left to detect it by.
    bool was_playing_ = false;
    std::atomic<float> clock_lamp_{0.0f};
    std::atomic<float> run_lamp_{0.0f};
};

inline std::unique_ptr<format::Processor> create_sync() {
    return std::make_unique<SyncProcessor>();
}

}  // namespace pulp::examples::brew
