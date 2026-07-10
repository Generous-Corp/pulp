#pragma once

// Trigger — a note becomes a voltage.
//
// Two independent channels, each turning MIDI notes (or a voltage on its own input)
// into one of four signals: a `Gate` while a note is held, a short `Trigger` pulse
// when one sounds, an `Envelope` through three attack stages and two release ones,
// or a `Velocity` voltage sampled and held from the last note. On an eight-output
// interface that is a gate and an envelope out of one instance, which is exactly
// how the two jacks are used.
//
// `Voltage Min` and `Voltage Max` set the window every signal lands in, and `Min`
// above `Max` is not an error — it is how an envelope is inverted, and it is why
// this plug-in has no `Invert` toggle where the rest of the suite does.
//
// This is the one plug-in whose output is not a pure function of the host's
// position, and it cannot be: a note is an event, not a coordinate. What it gets
// instead is `brew/envelope.hpp`'s pure `envelope_at` — the shape the editor draws
// and the tests measure the running state machine against, sample for sample.

#include <brew/channels.hpp>
#include <brew/param_text.hpp>
#include <brew/cv.hpp>
#include <brew/envelope.hpp>
#include <brew/gate.hpp>
#include <brew/lfo.hpp>     // Multiplier, multiplier_value
#include <brew/smooth.hpp>
#include <brew/sync.hpp>    // enum_from_param

#include <pulp/format/processor.hpp>
#include <pulp/midi/buffer.hpp>
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

/// Defaults that name an enumerator rather than a magic float.
inline constexpr float kTriggerDefaultMode =
    static_cast<float>(static_cast<int>(TriggerSignal::gate));
inline constexpr float kTriggerDefaultMult =
    static_cast<float>(static_cast<int>(Multiplier::one));

/// Middle C, in the note numbering every host agrees on even when it disagrees
/// about what to call it.
inline constexpr float kDefaultNote = 60.0f;

/// The longest a stage can be asked to take before `Mult` gets hold of it. Ten
/// seconds times a thousand is nearly three hours, which is past the point where a
/// control stops being an envelope.
inline constexpr float kMaxStageSeconds = 10.0f;

/// How long the scope holds the sustain when it draws the shape. Long enough to
/// read as a sustain, short enough that the release still has room.
inline constexpr double kScopeSustainSeconds = 0.35;

class TriggerProcessor : public format::Processor {
public:
    // Parameter IDs are part of the persisted state contract. Never renumber.
    // The right channel's twin sits `kRightChannelStride` above each of these.
    enum ParamId : state::ParamID {
        kMode = 1,
        kNote = 2,
        kAnyNote = 3,
        kSmoothMs = 4,
        kCvEnable = 5,
        kCvThreshold = 6,
        kVoltageMin = 7,
        kVoltageMax = 8,
        kLengthMs = 9,
        kOverride = 10,
        kOverrideValue = 11,
        // The envelope, in the order it is walked.
        kLevelA1 = 12,
        kTimeA1 = 13,
        kCurveA1 = 14,
        kLevelA2 = 15,
        kTimeA2 = 16,
        kCurveA2 = 17,
        kTimeA3 = 18,
        kCurveA3 = 19,
        kSustain = 20,
        kLevelR1 = 21,
        kTimeR1 = 22,
        kCurveR1 = 23,
        kTimeR2 = 24,
        kCurveR2 = 25,
        kMult = 26,
        kExponential = 27,
        kResetToZero = 28,
        kVelocityAmount = 29,
    };

    /// One row of a control table: the id and the label the editor shows. Kept
    /// beside the parameter definitions so a control cannot be defined and then
    /// left off the panel.
    struct ControlSpec {
        state::ParamID id;
        const char* label;
    };

    format::PluginDescriptor descriptor() const override {
        return {
            .name = "Trigger",
            .manufacturer = "Bitches Brew",
            .bundle_id = "com.bitchesbrew.trigger",
            .version = "0.1.0",
            .category = format::PluginCategory::Effect,
            .input_buses = {{"CV Trigger", 2}},
            .output_buses = {{"Trigger", 2}},
            .accepts_midi = true,
        };
    }

    /// The four signals a channel can carry, by the names the editor and the host
    /// both show.
    static std::string mode_name(float v) {
        static const char* const kNames[] = {"Gate", "Trig", "Env", "Vel"};
        return text::named_at(kNames, kTriggerSignalCount, v);
    }

    void define_parameters(state::StateStore& store) override {
        for (std::size_t ch = 0; ch < kChannelCount; ++ch) {
            const std::string s = channel_suffix(ch);
            // `fmt` is how the value reads — in this editor, and in the host's own
            // parameter list and automation lane.
            auto add = [&](state::ParamID id, const char* name, const char* unit,
                           state::ParamRange range, std::string (*fmt)(float)) {
                store.add_parameter({.id = static_cast<state::ParamID>(param_for(id, ch)),
                                     .name = name + s,
                                     .unit = unit,
                                     .range = range,
                                     .to_string = fmt});
            };

            // Which of the four signals reaches the jack.
            add(kMode, "Mode", "",
                {0.0f, static_cast<float>(kTriggerSignalCount - 1), kTriggerDefaultMode,
                 1.0f},
                mode_name);
            add(kNote, "Note", "", {0.0f, 127.0f, kDefaultNote, 1.0f}, text::whole);
            add(kAnyNote, "Any Note", "", {0.0f, 1.0f, 0.0f, 1.0f}, text::on_off);
            add(kSmoothMs, "Smooth", "ms", {-500.0f, 500.0f, 0.0f, 0.0f}, text::smooth);

            // A voltage on the input can fire the channel as well as — or instead
            // of — a note. A DC level, not an amplitude: the threshold is compared
            // against the sample, and a Schmitt trigger rides out the slew.
            add(kCvEnable, "CV Trigger", "", {0.0f, 1.0f, 0.0f, 1.0f}, text::on_off);
            add(kCvThreshold, "Threshold", "", {-1.0f, 1.0f, 0.5f, 0.0f}, text::signed_plain);

            // The window every signal lands in. Min above Max inverts.
            add(kVoltageMin, "Voltage Min", "", {-1.0f, 1.0f, 0.0f, 0.0f}, text::signed_plain);
            add(kVoltageMax, "Voltage Max", "", {-1.0f, 1.0f, 1.0f, 0.0f}, text::signed_plain);

            // How long the pulse in `Trigger` mode lasts.
            add(kLengthMs, "Length", "ms", {0.1f, 100.0f, 5.0f, 0.0f}, text::compact_millis);

            // Force the jack to a voltage, whatever the notes are doing. It still
            // passes through `Smooth`, so releasing it does not snap.
            add(kOverride, "Override", "", {0.0f, 1.0f, 0.0f, 1.0f}, text::on_off);
            add(kOverrideValue, "Override Value", "", {-1.0f, 1.0f, 0.0f, 0.0f}, text::signed_plain);

            // A1 rises to its own level. Leave that level at zero and `Time A1` is
            // a delay before the envelope starts, because a rise to zero from zero
            // is a wait.
            add(kLevelA1, "Level A1", "", {0.0f, 1.0f, 0.0f, 0.0f}, text::plain);
            add(kTimeA1, "Time A1", "s", {0.0f, kMaxStageSeconds, 0.0f, 0.0f}, text::seconds);
            add(kCurveA1, "Curve A1", "", {-1.0f, 1.0f, 0.0f, 0.0f}, text::signed_plain);
            // A2 is the attack: it rises to the peak.
            add(kLevelA2, "Level A2", "", {0.0f, 1.0f, 1.0f, 0.0f}, text::plain);
            add(kTimeA2, "Time A2", "s", {0.0f, kMaxStageSeconds, 0.01f, 0.0f}, text::seconds);
            add(kCurveA2, "Curve A2", "", {-1.0f, 1.0f, 0.0f, 0.0f}, text::signed_plain);
            // A3 is the decay: it falls from the peak to the sustain.
            add(kTimeA3, "Time A3", "s", {0.0f, kMaxStageSeconds, 0.1f, 0.0f}, text::seconds);
            add(kCurveA3, "Curve A3", "", {-1.0f, 1.0f, 0.0f, 0.0f}, text::signed_plain);
            add(kSustain, "Sustain", "", {0.0f, 1.0f, 0.7f, 0.0f}, text::plain);
            // R1 is the release. R2 is the tail after it, ending at zero.
            add(kLevelR1, "Level R1", "", {0.0f, 1.0f, 0.0f, 0.0f}, text::plain);
            add(kTimeR1, "Time R1", "s", {0.0f, kMaxStageSeconds, 0.2f, 0.0f}, text::seconds);
            add(kCurveR1, "Curve R1", "", {-1.0f, 1.0f, 0.0f, 0.0f}, text::signed_plain);
            add(kTimeR2, "Time R2", "s", {0.0f, kMaxStageSeconds, 0.0f, 0.0f}, text::seconds);
            add(kCurveR2, "Curve R2", "", {-1.0f, 1.0f, 0.0f, 0.0f}, text::signed_plain);

            add(kMult, "Mult", "",
                {0.0f, static_cast<float>(kMultiplierCount - 1), kTriggerDefaultMult,
                 1.0f},
                text::multiplier_name);
            add(kExponential, "Exp", "", {0.0f, 1.0f, 0.0f, 1.0f}, text::on_off);
            add(kResetToZero, "RTZ", "", {0.0f, 1.0f, 1.0f, 1.0f}, text::on_off);
            add(kVelocityAmount, "Vel", "", {-1.0f, 1.0f, 0.0f, 0.0f}, text::signed_plain);
        }
    }

    /// Every knob the editor must expose, per channel, in panel order.
    [[nodiscard]] static constexpr std::array<ControlSpec, 24> controls() {
        return {{{kMode, "Mode"},
                 {kNote, "Note"},
                 {kVelocityAmount, "Vel"},
                 {kSmoothMs, "Smooth"},
                 {kLengthMs, "Length"},
                 {kCvThreshold, "Thresh"},
                 {kVoltageMin, "V Min"},
                 {kVoltageMax, "V Max"},
                 {kOverrideValue, "Value"},
                 {kMult, "Mult"},
                 {kLevelA1, "Lvl A1"},
                 {kTimeA1, "Time A1"},
                 {kCurveA1, "Crv A1"},
                 {kLevelA2, "Lvl A2"},
                 {kTimeA2, "Time A2"},
                 {kCurveA2, "Crv A2"},
                 {kTimeA3, "Time A3"},
                 {kCurveA3, "Crv A3"},
                 {kSustain, "Sustain"},
                 {kLevelR1, "Lvl R1"},
                 {kTimeR1, "Time R1"},
                 {kCurveR1, "Crv R1"},
                 {kTimeR2, "Time R2"},
                 {kCurveR2, "Crv R2"}}};
    }

    /// The toggles, which no readout can show.
    [[nodiscard]] static constexpr std::array<ControlSpec, 5> toggles() {
        return {{{kAnyNote, "Any Note"},
                 {kCvEnable, "CV Trig"},
                 {kOverride, "Override"},
                 {kExponential, "Exp"},
                 {kResetToZero, "RTZ"}}};
    }

    void prepare(const format::PrepareContext&) override { hard_reset(); }

    /// Two scopes, ten rows of controls, two toggle rows, two captions.
    std::pair<uint32_t, uint32_t> editor_size() const override { return {380, 1258}; }

    std::unique_ptr<view::View> create_view() override;

    // ── The settings the pure transfer reads ─────────────────────────────────

    [[nodiscard]] TriggerSignal mode(std::size_t ch) const noexcept {
        return enum_from_param<TriggerSignal>(get(kMode, ch), kTriggerSignalCount);
    }

    /// The envelope's whole shape, as the editor and the tests see it.
    [[nodiscard]] EnvelopeSpec envelope_spec(std::size_t ch) const noexcept {
        EnvelopeSpec s;
        s.attack[0] = {get(kLevelA1, ch), get(kTimeA1, ch), get(kCurveA1, ch)};
        s.attack[1] = {get(kLevelA2, ch), get(kTimeA2, ch), get(kCurveA2, ch)};
        s.attack[2] = {0.0f, get(kTimeA3, ch), get(kCurveA3, ch)};
        s.sustain = get(kSustain, ch);
        s.release[0] = {get(kLevelR1, ch), get(kTimeR1, ch), get(kCurveR1, ch)};
        s.release[1] = {0.0f, get(kTimeR2, ch), get(kCurveR2, ch)};
        s.exponential = as_toggle(get(kExponential, ch));
        s.reset_to_zero = as_toggle(get(kResetToZero, ch));
        s.time_multiplier =
            multiplier_value(enum_from_param<Multiplier>(get(kMult, ch), kMultiplierCount));
        return s;
    }

    /// Whether a channel is currently holding a note, from either source.
    [[nodiscard]] bool held(std::size_t ch) const noexcept {
        return held_[ch].load(std::memory_order_relaxed);
    }

    /// The voltage the channel last put on the jack. The editor's rail.
    [[nodiscard]] float display_value(std::size_t ch) const noexcept {
        return display_value_[ch].load(std::memory_order_relaxed);
    }

    /// Where the envelope is, so the editor can say so rather than guess.
    [[nodiscard]] Stage display_stage(std::size_t ch) const noexcept {
        return static_cast<Stage>(display_stage_[ch].load(std::memory_order_relaxed));
    }

    /// The signal the channel emits, at a unipolar `[0,1]` level, mapped into its
    /// voltage window. Shared by `process()`, the editor, and the tests.
    [[nodiscard]] float voltage_of(std::size_t ch, float unipolar) const noexcept {
        return map_voltage(unipolar, get(kVoltageMin, ch), get(kVoltageMax, ch));
    }

    /// The envelope's level `seconds` after a note-on, with velocity applied. The
    /// scope draws exactly this, and so does the test that pins the state machine.
    [[nodiscard]] float envelope_value_at(std::size_t ch, double seconds,
                                          float velocity = 1.0f) const noexcept {
        return envelope_at(envelope_spec(ch), seconds) *
               velocity_gain(get(kVelocityAmount, ch), velocity);
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

        // Bypass means stop driving the patch: a bypassed *generator* is silent, and
        // a held gate at the far end of the cable is a held note.
        if (ctx.is_bypassed) {
            for (std::size_t c = 0; c < channels; ++c)
                if (float* dst = output.channel_ptr(c))
                    for (int n = 0; n < frames; ++n) dst[n] = 0.0f;
            hard_reset();
            return;
        }

        if (ctx.reset_requested) hard_reset();

        const double sr = ctx.sample_rate > 0.0 ? ctx.sample_rate : 48000.0;

        // Sample-accurate note handling needs the events in time order. The sort is
        // allocation-free — MidiBuffer sizes its scratch for exactly this call.
        midi_in.sort();

        const std::size_t shared = std::min(kChannelCount, input.num_channels());

        for (std::size_t ch = 0; ch < kChannelCount && ch < channels; ++ch) {
            float* dst = output.channel_ptr(ch);
            if (dst == nullptr) continue;

            const EnvelopeSpec spec = envelope_spec(ch);
            const TriggerSignal signal = mode(ch);
            const bool any_note = as_toggle(get(kAnyNote, ch));
            const int note = static_cast<int>(std::lround(get(kNote, ch)));
            const bool cv_on = as_toggle(get(kCvEnable, ch));
            const float threshold = get(kCvThreshold, ch);
            const double length = static_cast<double>(get(kLengthMs, ch)) * 0.001;
            const bool overridden = as_toggle(get(kOverride, ch));
            const float override_value = get(kOverrideValue, ch);
            const float vel_amount = get(kVelocityAmount, ch);
            const float ms = get(kSmoothMs, ch);
            const float* src = ch < shared ? input.channel_ptr(ch) : nullptr;

            auto event = midi_in.begin();
            const auto events_end = midi_in.end();

            for (int n = 0; n < frames; ++n) {
                while (event != events_end && event->sample_offset <= n) {
                    if (event->is_note_on() && matches(*event, note, any_note)) {
                        // Every note that sounds retriggers: that is what makes the
                        // `Trigger` pulse fire once per note rather than once per
                        // phrase, and it is what a legato envelope has to opt out
                        // of by clearing `RTZ`.
                        ++midi_held_[ch];
                        velocity_[ch] = static_cast<float>(event->velocity()) / 127.0f;
                        fire(ch, spec, length, sr);
                    } else if (event->is_note_off() && matches(*event, note, any_note)) {
                        if (midi_held_[ch] > 0) --midi_held_[ch];
                    }
                    ++event;
                }

                // The CV input is a second, independent way in. Its rising edge is a
                // note-on at full velocity; the two sources are OR-ed, not gated by
                // one another, because the manual's own words are "as well as, or
                // instead of".
                if (cv_on && src != nullptr) {
                    if (cv_gate_[ch].process(src[n], threshold,
                                             threshold - kGateHysteresis)) {
                        velocity_[ch] = 1.0f;
                        fire(ch, spec, length, sr);
                    }
                } else if (cv_gate_[ch].is_high()) {
                    // Switching the input off releases whatever it was holding,
                    // rather than latching the gate for the rest of the session.
                    cv_gate_[ch].reset();
                }

                const bool holding =
                    midi_held_[ch] > 0 || (cv_on && src != nullptr && cv_gate_[ch].is_high());
                if (was_held_[ch] && !holding) envelope_[ch].note_off();
                was_held_[ch] = holding;

                // Every generator advances every sample, whatever the mode is and
                // whatever `Override` says. A mode switch that stepped into a frozen
                // envelope would emit whatever level the last note left behind.
                const float env = envelope_[ch].process(spec, sr);
                const bool pulse_high = pulse_[ch].process();

                float unipolar = 0.0f;
                switch (signal) {
                    case TriggerSignal::gate: unipolar = holding ? 1.0f : 0.0f; break;
                    case TriggerSignal::trigger: unipolar = pulse_high ? 1.0f : 0.0f; break;
                    case TriggerSignal::envelope:
                        unipolar = env * velocity_gain(vel_amount, velocity_[ch]);
                        break;
                    case TriggerSignal::velocity:
                        // Sampled and held: a velocity that fell back to zero on
                        // note-off would slam whatever it is patched to.
                        unipolar = velocity_[ch];
                        break;
                }

                // `Override Value` is a [-1, +1] parameter and `StateStore` clamps a
                // write to its range, so the jack's rails are the range itself. The
                // computed path still goes through `map_voltage`, which does clamp.
                const float target = overridden ? override_value : voltage_of(ch, unipolar);
                dst[n] = smooth_[ch].process(target, ms, sr);
            }

            held_[ch].store(was_held_[ch], std::memory_order_relaxed);
            display_value_[ch].store(dst[frames - 1], std::memory_order_relaxed);
            display_stage_[ch].store(static_cast<int>(envelope_[ch].stage(spec)),
                                     std::memory_order_relaxed);
        }

        // An output channel with no jack behind it is silent, not uninitialized.
        for (std::size_t c = kChannelCount; c < channels; ++c)
            if (float* dst = output.channel_ptr(c))
                for (int n = 0; n < frames; ++n) dst[n] = 0.0f;
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
    [[nodiscard]] float get(state::ParamID id, std::size_t ch) const noexcept {
        return state().get_value(static_cast<state::ParamID>(param_for(id, ch)));
    }

    /// Does this event belong to this channel's note?
    [[nodiscard]] static bool matches(const midi::MidiEvent& e, int note,
                                      bool any_note) noexcept {
        return any_note || static_cast<int>(e.note()) == note;
    }

    /// A new note sounds: restart the envelope and the pulse together.
    void fire(std::size_t ch, const EnvelopeSpec& spec, double length,
              double sr) noexcept {
        envelope_[ch].note_on(spec);
        pulse_[ch].fire(length, sr);
    }

    void hard_reset() noexcept {
        for (std::size_t ch = 0; ch < kChannelCount; ++ch) {
            envelope_[ch].reset();
            pulse_[ch].reset();
            cv_gate_[ch].reset();
            smooth_[ch].reset(0.0f);
            midi_held_[ch] = 0;
            was_held_[ch] = false;
            velocity_[ch] = 0.0f;
            held_[ch].store(false, std::memory_order_relaxed);
            display_value_[ch].store(0.0f, std::memory_order_relaxed);
            display_stage_[ch].store(static_cast<int>(Stage::idle),
                                     std::memory_order_relaxed);
        }
    }

    std::array<Envelope, kChannelCount> envelope_{};
    std::array<Pulse, kChannelCount> pulse_{};
    std::array<SchmittGate, kChannelCount> cv_gate_{};
    std::array<Smoother, kChannelCount> smooth_{};

    /// How many matching notes are down. A count, not a flag: releasing the first
    /// of two held notes must not close the gate.
    std::array<int, kChannelCount> midi_held_{};
    std::array<bool, kChannelCount> was_held_{};
    std::array<float, kChannelCount> velocity_{};

    std::array<std::atomic<bool>, kChannelCount> held_{};
    std::array<std::atomic<float>, kChannelCount> display_value_{};
    std::array<std::atomic<int>, kChannelCount> display_stage_{};
};

inline std::unique_ptr<format::Processor> create_trigger() {
    return std::make_unique<TriggerProcessor>();
}

}  // namespace pulp::examples::brew
