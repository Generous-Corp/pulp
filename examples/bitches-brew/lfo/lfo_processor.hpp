#pragma once

// LFO — two independent modulation sources, tempo-locked or free-running.
//
// Two channels with identical, independent controls. Set the right channel's sync
// mode to `Quad` and give it a 90° `Phase` and the pair traces a circle, which is
// how a two-axis modulation (a filter's cutoff and resonance, a panner's X and Y)
// is driven from one oscillator. That used to be hard-wired into channel 1; making
// it a mode means the second LFO can also just be a second LFO.
//
// The phase is derived from the host's position, never accumulated — see
// brew/lfo.hpp. So the modulation is bit-identical across bounces, lands where the
// timeline says it should after a locate, and never drifts against the host over a
// long session. The two exceptions are `Free` and `Tempo`, which by definition keep
// running while the transport is parked and therefore cannot be a function of a
// position that is not moving. The editor says which modes those are.
//
// Per-sample, not per-block. A block-rate LFO steps its value 512 samples at a
// time, and a stepped control voltage is an audible zipper on anything it drives.

#include <brew/channels.hpp>
#include <brew/param_text.hpp>
#include <brew/clock.hpp>  // beats_per_sample
#include <brew/cv.hpp>
#include <brew/lfo.hpp>
#include <brew/phase_clock.hpp>
#include <brew/smooth.hpp>
#include <brew/sync.hpp>  // NoteUnit, enum_from_param

#include <pulp/format/processor.hpp>
#include <pulp/state/store.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace pulp::examples::brew {

/// Keeps the two channels' hashes apart. Two LFOs with the same `Seed` are still
/// two LFOs, and a sample-and-hold that stepped to the same level on both would be
/// one LFO wearing a second cable.
[[nodiscard]] inline constexpr std::uint32_t channel_hash_salt(std::size_t ch) noexcept {
    return ch == 0 ? 0u : 0x5bf03635u;
}

/// Enum defaults, as the floats a parameter range wants. Spelled through `int`
/// because a scoped enum's conversion straight to a floating-point type is not
/// something every toolchain in the matrix agrees about.
inline constexpr float kDefaultSync = static_cast<float>(static_cast<int>(SyncMode::transport));
inline constexpr float kDefaultMultiplier = static_cast<float>(static_cast<int>(Multiplier::one));
inline constexpr float kDefaultDivisor = static_cast<float>(static_cast<int>(NoteUnit::quarter));
inline constexpr float kDefaultInputMode = static_cast<float>(static_cast<int>(InputMode::off));

class LfoProcessor : public format::Processor {
public:
    /// The scope marker hides until a block has been rendered. A marker at phase
    /// zero on a plug-in that has never run says the LFO is sitting at zero, which
    /// is a different claim from "nothing has happened yet".
    LfoProcessor() {
        for (auto& p : display_phase_) p.store(-1.0f, std::memory_order_relaxed);
    }

    // Parameter IDs are part of the persisted state contract. Never renumber.
    //
    // Id 14 was a `Free Run` toggle before the eight sync modes replaced it. It
    // stays retired rather than reused: a project restoring a boolean into an
    // enum's slot would select a mode nobody chose.
    enum ParamId : state::ParamID {
        kBeats = 1,
        kPhaseDegrees = 2,
        kSine = 3,
        kTriangle = 4,
        kSaw = 5,
        kSquare = 6,
        kPulseWidth = 7,
        kRandom = 8,
        kAsymmetry = 9,
        kOffset = 10,
        kSeed = 11,
        kOutputScale = 12,
        kInvert = 13,
        kSpeedHz = 15,
        kSwingPercent = 16,
        kSwingUnit = 17,
        kSmoothMs = 18,
        kSyncMode = 19,
        kMultiplier = 20,
        kDivisor = 21,
        kTriplet = 22,
        kNoise = 23,
        kInputMode = 24,
        kResetByNote = 25,
    };

    format::PluginDescriptor descriptor() const override {
        return {
            .name = "LFO",
            .manufacturer = "Bitches Brew",
            .bundle_id = "com.bitchesbrew.lfo",
            .version = "0.1.0",
            .category = format::PluginCategory::Effect,
            .input_buses = {{"Main In", 2}},
            .output_buses = {{"LFO L / R", 2}},
            // `Reset By Note` needs note-ons. An AU host only routes MIDI to an
            // effect packaged `aumf`, which this flag is what selects.
            .accepts_midi = true,
        };
    }

    /// `fmt` is how the value reads — in this editor, and in the host's own
    /// parameter list and automation lane. A plain function pointer rather than a
    /// `std::function`, so the table stays `constexpr`.
    struct ControlSpec {
        state::ParamID id;
        const char* name;
        const char* unit;
        state::ParamRange range;
        std::string (*fmt)(float);
    };

    /// Every control an LFO channel has. Registered once per channel: the two
    /// channels are independent and identical.
    static constexpr std::array<ControlSpec, 24> controls() {
        return {{
            // How the LFO is told what time it is. See brew/lfo.hpp for the table.
            // `Transport` is the default: locked to the timeline, exactly
            // reproducible, and what a modulation source is usually wanted for.
            {kSyncMode, "Sync", "",
             {0.0f, static_cast<float>(kSyncModeCount - 1),
              kDefaultSync, 1.0f},
             text::sync_name},

            // Hertz modes: Speed × Multiplier.
            {kSpeedHz, "Speed", "Hz", {0.01f, 40.0f, 1.0f, 0.01f}, text::compact},
            {kMultiplier, "Multiplier", "",
             {0.0f, static_cast<float>(kMultiplierCount - 1),
              kDefaultMultiplier, 1.0f},
             text::multiplier_name},

            // Beat modes: Beats of the note Divisor names, less a third if Triplet.
            // Beats is fractional on purpose — automating it through a non-integer
            // value is a legitimate way to sweep the rate.
            {kBeats, "Beats", "", {0.0625f, 16.0f, 1.0f, 0.0625f}, text::compact},
            {kDivisor, "Divisor", "",
             {0.0f, static_cast<float>(kNoteUnitCount - 1),
              kDefaultDivisor, 1.0f},
             text::divisor_name},
            {kTriplet, "Triplet", "", {0.0f, 1.0f, 0.0f, 1.0f}, text::on_off},

            // A constant offset against the locked cycle, and the point a reset
            // snaps back to.
            {kPhaseDegrees, "Phase", "deg", {0.0f, 360.0f, 0.0f, 1.0f}, text::degrees},

            // Four depths, summed. Bipolar, so a shape can be subtracted as easily
            // as added. Sine defaults to full and the rest to zero, which
            // reproduces the single-shape selector this replaced.
            {kSine, "Sine", "", {-1.0f, 1.0f, 1.0f, 0.001f}, text::signed_plain},
            {kTriangle, "Triangle", "", {-1.0f, 1.0f, 0.0f, 0.001f}, text::signed_plain},
            {kSaw, "Saw", "", {-1.0f, 1.0f, 0.0f, 0.001f}, text::signed_plain},
            {kSquare, "Square", "", {-1.0f, 1.0f, 0.0f, 0.001f}, text::signed_plain},
            {kPulseWidth, "Pulse Width", "", {0.01f, 0.99f, 0.5f, 0.001f}, text::plain},

            // A sample-and-hold: one level per cycle, held flat. A hash of the
            // cycle index, so a bounce lands identically. See brew/random.hpp.
            {kRandom, "Random", "", {-1.0f, 1.0f, 0.0f, 0.001f}, text::signed_plain},
            // The ungated source: a new level every sample. Hashed on the sample
            // index, so it too renders identically every time.
            {kNoise, "Noise", "", {-1.0f, 1.0f, 0.0f, 0.001f}, text::signed_plain},
            {kSeed, "Seed", "", {0.0f, 255.0f, 0.0f, 1.0f}, text::whole},

            // Where the waveform's centre falls in time. A pulse-width control
            // generalized to every shape.
            {kAsymmetry, "Asymmetry", "", {0.01f, 0.99f, 0.5f, 0.001f}, text::plain},
            // A constant offset. Set it to +1 with a half-scale mix and the output
            // is unipolar, which is what a VCA or an envelope-depth input wants.
            {kOffset, "Offset", "", {-1.0f, 1.0f, 0.0f, 0.001f}, text::signed_plain},

            // The same swing Sync applies to its clock, on the same beat timeline.
            // 50% is straight, and bit-identically so.
            {kSwingPercent, "Swing", "%",
             {static_cast<float>(kMinSwing * 100.0),
              static_cast<float>(kMaxSwing * 100.0), 50.0f, 0.1f},
             text::percent1},
            {kSwingUnit, "Swing Sixteenths", "", {0.0f, 1.0f, 0.0f, 1.0f}, text::on_off},

            // What the plug-in does with the voltage on its input bus. Off by
            // default: a modulation source that read its input by default would
            // scream the first time it was dropped on an audio track.
            {kInputMode, "Input Mode", "",
             {0.0f, static_cast<float>(kInputModeCount - 1),
              kDefaultInputMode, 1.0f},
             text::input_mode_name},

            // A MIDI note-on snaps the phase back to `Phase`.
            {kResetByNote, "Reset By Note", "", {0.0f, 1.0f, 0.0f, 1.0f}, text::on_off},

            // Positive slews at a constant rate; negative low-passes. Milliseconds
            // for a full swing, as DC's does. Zero is a wire — and it is the
            // default, because a smoothed LFO is the only control here that costs
            // the plug-in its exact locate-invariance. See the note on `smooth_`.
            {kSmoothMs, "Smooth", "ms", {-1000.0f, 1000.0f, 0.0f, 0.1f}, text::smooth},

            {kOutputScale, "Output Scale", "", {0.0f, 1.0f, 1.0f, 0.001f}, text::fraction_percent},
            {kInvert, "Invert", "", {0.0f, 1.0f, 0.0f, 1.0f}, text::on_off},
        }};
    }

    void define_parameters(state::StateStore& store) override {
        for (std::size_t ch = 0; ch < kChannelCount; ++ch)
            for (const auto& c : controls())
                store.add_parameter(
                    {.id = static_cast<state::ParamID>(param_for(c.id, ch)),
                     .name = std::string(c.name) + channel_suffix(ch),
                     .unit = c.unit,
                     .range = c.range,
                     .to_string = c.fmt});
    }

    // ── Reading one channel's knobs ──────────────────────────────────────────

    [[nodiscard]] float get(state::ParamID id, std::size_t ch) const noexcept {
        return state().get_value(static_cast<state::ParamID>(param_for(id, ch)));
    }

    [[nodiscard]] SyncMode sync_mode(std::size_t ch) const noexcept {
        const auto mode = enum_from_param<SyncMode>(get(kSyncMode, ch), kSyncModeCount);
        // The left channel has no other channel to follow. Rather than lock it to
        // itself — a silent no-op that would look like a broken mode — it falls
        // back to the mode `Quad` is a phase-shifted copy of.
        if (mode == SyncMode::quadrature && ch == 0) return SyncMode::transport;
        return mode;
    }

    [[nodiscard]] InputMode input_mode(std::size_t ch) const noexcept {
        return enum_from_param<InputMode>(get(kInputMode, ch), kInputModeCount);
    }

    [[nodiscard]] double channel_hz(std::size_t ch) const noexcept {
        return free_hz(static_cast<double>(get(kSpeedHz, ch)),
                       enum_from_param<Multiplier>(get(kMultiplier, ch),
                                                   kMultiplierCount));
    }

    [[nodiscard]] double channel_cycle_beats(std::size_t ch) const noexcept {
        return cycle_beats(static_cast<double>(get(kBeats, ch)),
                           enum_from_param<NoteUnit>(get(kDivisor, ch), kNoteUnitCount),
                           as_toggle(get(kTriplet, ch)));
    }

    /// The swing the beat timeline is warped by. At 50% this returns a `Swing`
    /// that `swing_active` reports inactive, so the phase is untouched.
    [[nodiscard]] Swing current_swing(std::size_t ch = 0) const noexcept {
        return {.unit_beats = as_toggle(get(kSwingUnit, ch)) ? kSixteenthBeats
                                                             : kEighthBeats,
                .amount = static_cast<double>(get(kSwingPercent, ch)) / 100.0};
    }

    /// A snapshot of one channel's mix, taken once per block rather than once per
    /// sample. The editor's scope reads the same struct, so the picture cannot
    /// drift from the signal.
    [[nodiscard]] LfoMix mix(std::size_t ch = 0) const noexcept {
        return {
            .sine = get(kSine, ch),
            .triangle = get(kTriangle, ch),
            .saw = get(kSaw, ch),
            .square = get(kSquare, ch),
            .random = get(kRandom, ch),
            .noise = get(kNoise, ch),
            .pulse_width = get(kPulseWidth, ch),
            .asymmetry = get(kAsymmetry, ch),
            .offset = get(kOffset, ch),
            .seed = static_cast<std::uint32_t>(get(kSeed, ch)) ^ channel_hash_salt(ch),
        };
    }

    /// The phase offset a channel's waveform carries: its `Phase` knob, plus the
    /// quarter cycle a follower stands behind its leader.
    [[nodiscard]] double phase_offset(std::size_t ch) const noexcept {
        return static_cast<double>(get(kPhaseDegrees, ch)) / 360.0;
    }

    void prepare(const format::PrepareContext&) override {
        for (auto& s : smooth_) s.reset(0.0f);
        hard_reset();
    }

    /// The size this editor actually needs: two channel blocks, each a scope and
    /// four rows. Without this override a host opens the plug-in at Processor's
    /// 400x300 default — a geometry the layout was never checked at.
    std::pair<uint32_t, uint32_t> editor_size() const override { return {380, 1116}; }

    /// Defined in lfo_view.cpp so the audio translation units never see the
    /// view stack.
    std::unique_ptr<view::View> create_view() override;

    /// Phase at the end of the last rendered block, for the editor's scope marker.
    /// Written once per block on the audio thread with relaxed ordering: a marker
    /// one frame stale is invisible, and synchronizing for it would not be.
    /// Negative means "not running" — the marker hides rather than lying.
    [[nodiscard]] float display_phase(std::size_t ch = 0) const noexcept {
        return display_phase_[std::min(ch, kChannelCount - 1)].load(
            std::memory_order_relaxed);
    }

    // ── The pure transfer, shared by process(), the editor, and the tests ─────

    /// The shape at a number of elapsed cycles, before the output stage and before
    /// any smoothing.
    ///
    /// Phase and cycle index take the *same* offset. Keying the sample-and-hold on
    /// an un-offset index while the phase is offset makes the held value step in
    /// the middle of the visible cycle.
    [[nodiscard]] float shape_at_cycles(const LfoMix& m, double cycles, double offset,
                                        std::int64_t sample = 0) const noexcept {
        return lfo_mix_value(m, phase_at(cycles, offset), cycle_at(cycles, offset),
                             sample);
    }

    /// The value a channel emits at a number of elapsed cycles, after the shared
    /// output stage. Unsmoothed: smoothing has state, so only `process()` can apply
    /// it, and the scope draws this.
    [[nodiscard]] float value_at_cycles(std::size_t ch, double cycles,
                                        float input = 0.0f) const noexcept {
        const float shaped = shape_at_cycles(mix(ch), cycles, phase_offset(ch));
        return resolve_output(apply_input(input_mode(ch), shaped, input),
                              get(kOutputScale, ch), as_toggle(get(kInvert, ch)));
    }

    /// Beat-mode convenience: the value at a beat position, measured from the
    /// project origin.
    [[nodiscard]] float value_at(double position_beats,
                                 std::size_t ch = 0) const noexcept {
        return value_at_cycles(
            ch, cycles_from_beats(position_beats, 0.0, channel_cycle_beats(ch),
                                  current_swing(ch)));
    }

    /// Hertz-mode convenience: the value at a number of seconds.
    [[nodiscard]] float value_at_time(double position_seconds,
                                      std::size_t ch = 0) const noexcept {
        return value_at_cycles(
            ch, cycles_from_seconds(position_seconds, 0.0, channel_hz(ch)));
    }

    // ── The audio callback ───────────────────────────────────────────────────

    void process(audio::BufferView<float>& output,
                 const audio::BufferView<const float>& input,
                 midi::MidiBuffer& midi_in,
                 midi::MidiBuffer&,
                 const format::ProcessContext& ctx) override {
        const std::size_t channels = output.num_channels();
        const int frames = static_cast<int>(output.num_samples());
        if (channels == 0 || frames <= 0) return;

        // Bypass means stop driving the patch: hold the outputs at zero rather than
        // freeze them at whatever voltage the cycle happened to reach. A bypassed
        // *generator* is silent — see brew/cv.hpp — because the far end of the cable
        // is a VCO's pitch input, and a held voltage there is a held wrong note.
        if (ctx.is_bypassed) {
            for (std::size_t c = 0; c < channels; ++c)
                if (float* dst = output.channel_ptr(c))
                    for (int n = 0; n < frames; ++n) dst[n] = 0.0f;
            for (auto& sm : smooth_) sm.reset(0.0f);
            for (auto& p : display_phase_) p.store(-1.0f, std::memory_order_relaxed);
            was_playing_ = ctx.is_playing;
            return;
        }

        if (ctx.reset_requested) hard_reset();

        const Transport transport = transport_from(ctx);

        // Noise is keyed on the sample index. While the transport runs that index is
        // the timeline's, so a bounce is identical; while it is parked the index
        // keeps climbing, because a noise source that froze with the playhead would
        // look broken rather than deterministic.
        if (ctx.is_playing) noise_index_ = ctx.position_samples;

        // Sample-accurate note resets need the events in time order. The sort is
        // allocation-free — MidiBuffer sizes its scratch for exactly this call.
        midi_in.sort();

        std::array<ClockSettings, kChannelCount> settings{};
        for (std::size_t ch = 0; ch < kChannelCount; ++ch) {
            settings[ch] = clock_settings(ch, ctx.is_playing);
            clock_[ch].begin_block(settings[ch], transport);
        }

        const std::size_t shared = std::min(channels, input.num_channels());

        for (std::size_t ch = 0; ch < kChannelCount && ch < channels; ++ch) {
            float* dst = output.channel_ptr(ch);
            if (dst == nullptr) continue;

            const SyncMode mode = sync_mode(ch);
            // A follower reads the leader's clock, not its own. Its own `Phase` still
            // applies, which is what makes the pair trace a circle at 90°.
            const std::size_t clock_ch = mode == SyncMode::quadrature ? 0 : ch;
            const SyncMode clock_mode = sync_mode(clock_ch);

            const LfoMix m = mix(ch);
            const double offset = phase_offset(ch);
            const float ms = get(kSmoothMs, ch);
            const float scale = get(kOutputScale, ch);
            const bool inv = as_toggle(get(kInvert, ch));
            const InputMode im = input_mode(ch);
            const bool resets = as_toggle(get(kResetByNote, ch));
            const float* src = ch < shared ? input.channel_ptr(ch) : nullptr;

            auto note = midi_in.begin();
            const auto notes_end = midi_in.end();

            double last_cycles = 0.0;
            for (int n = 0; n < frames; ++n) {
                const double dn = static_cast<double>(n);

                // Every mode measures cycles from an origin, so moving the origin at
                // the note's own sample is the one mechanism that retriggers all of
                // them — and the result is still a pure function of the timeline the
                // note sits on.
                while (resets && note != notes_end && note->sample_offset <= n) {
                    if (note->is_note_on())
                        clock_[ch].set_origin(transport.beats_at(dn),
                                              transport.seconds_at(dn));
                    ++note;
                }

                const double cycles =
                    clock_[clock_ch].cycles_at(settings[clock_ch], transport, dn);
                last_cycles = cycles;

                float shaped;
                if (clock_mode == SyncMode::start_stop) {
                    // Not really an LFO: the transport, as a square wave whose period
                    // is the session. `Offset` still applies, and `Smooth` turns the
                    // play edge into a fade-in.
                    shaped = start_stop_value(ctx.is_playing) + m.offset;
                } else {
                    shaped = shape_at_cycles(m, cycles, offset,
                                             noise_index_ + static_cast<std::int64_t>(n));
                }

                const float in = src ? src[n] : 0.0f;
                dst[n] = resolve_output(
                    smooth_[ch].process(apply_input(im, shaped, in), ms, sr_of(ctx)), scale,
                    inv);
            }

            display_phase_[ch].store(
                clock_mode == SyncMode::start_stop
                    ? (ctx.is_playing ? 0.0f : -1.0f)
                    : static_cast<float>(phase_at(last_cycles, offset)),
                std::memory_order_relaxed);
        }

        // An output channel with no LFO behind it is silent, not left uninitialized.
        for (std::size_t c = kChannelCount; c < channels; ++c)
            if (float* dst = output.channel_ptr(c))
                for (int n = 0; n < frames; ++n) dst[n] = 0.0f;

        for (std::size_t ch = 0; ch < kChannelCount; ++ch)
            clock_[ch].end_block(settings[ch], transport, frames);
        if (!ctx.is_playing) noise_index_ += frames;

        was_playing_ = ctx.is_playing;
    }

    void process(format::ProcessBuffers& audio,
                 midi::MidiBuffer& midi_in,
                 midi::MidiBuffer& midi_out,
                 const format::ProcessContext& ctx) override {
        if (auto* out = audio.main_output()) {
            audio::BufferView<const float> empty;
            const auto* in = audio.main_input();
            process(*out, in ? *in : empty, midi_in, midi_out, ctx);
        }
    }

private:
    [[nodiscard]] static double sr_of(const format::ProcessContext& ctx) noexcept {
        return ctx.sample_rate > 0.0 ? ctx.sample_rate : 48000.0;
    }

    [[nodiscard]] Transport transport_from(const format::ProcessContext& ctx) noexcept {
        const double sr = sr_of(ctx);
        Transport t;
        t.position_beats = ctx.position_beats;
        t.position_seconds = static_cast<double>(ctx.position_samples) / sr;
        t.beats_per_sample = beats_per_sample(ctx.tempo_bpm, sr);
        t.sample_rate = sr;
        t.playing = ctx.is_playing;
        // `transport_started` is the host's own flag; the `was_playing_` comparison
        // catches the hosts that never set it.
        t.play_edge = (ctx.is_playing && !was_playing_) || ctx.transport_started;
        return t;
    }

    /// One channel's rate, in whichever unit its mode reads.
    ///
    /// Swing warps a beat timeline. With the playhead parked there is nothing to
    /// push late, and warping a frozen position only moves the value the scope is
    /// showing — so it is dropped while stopped rather than applied to a still frame.
    [[nodiscard]] ClockSettings clock_settings(std::size_t ch, bool playing) const noexcept {
        return {.mode = sync_mode(ch),
                .hz = channel_hz(ch),
                .cycle_beats = channel_cycle_beats(ch),
                .swing = playing ? current_swing(ch) : Swing{}};
    }

    void hard_reset() noexcept {
        for (auto& c : clock_) c.reset();
        noise_index_ = 0;
        was_playing_ = false;
    }

    /// One per channel. Zero milliseconds is a wire — bit for bit — which is why it
    /// is the default: a smoother carries state between blocks, so a non-zero
    /// `Smooth` is the only control on this plug-in that makes the output depend on
    /// how the playhead arrived rather than only on where it is. The dependence is
    /// bounded by the smoother's own settling time, and a bounce from a fixed start
    /// is still bit-identical every render.
    std::array<Smoother, kChannelCount> smooth_{};

    /// The eight sync modes, one independent counter each. See brew/phase_clock.hpp.
    std::array<PhaseClock, kChannelCount> clock_{};

    std::int64_t noise_index_ = 0;
    bool was_playing_ = false;

    std::array<std::atomic<float>, kChannelCount> display_phase_{};
};

inline std::unique_ptr<format::Processor> create_lfo() {
    return std::make_unique<LfoProcessor>();
}

}  // namespace pulp::examples::brew
