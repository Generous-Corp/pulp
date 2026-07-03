// Hot-reloadable DSP "logic" for the hot-reload SYNTH demo (instrument variant).
//
// This is the half you EDIT + recompile while the host keeps playing notes. The
// shell (a ReloadableShell built into the loaded AU/VST3/CLAP instrument) watches
// the compiled .dylib and hot-swaps this synth in live — hold a chord and hear
// the timbre morph with no reopen.
//
// It is a small polyphonic subtractive-ish synth driven by MIDI. The STABLE
// contract the shell mirrors (must not change across reloads):
//     id 1  "Level"    0..1
//     id 2  "Release"  0.02..3 s
//
// The one thing you EDIT LIVE is the OSCILLATOR SHAPE below. Sine is pure/soft;
// Saw is bright/buzzy — a night-and-day timbre change (unlike the subtler tremolo
// demo). Flip kOsc, run rebuild_logic.sh, and the held notes change character
// under your ears, click-free (the crossfade + RT-safe swap engine).

#include <pulp/format/processor.hpp>
#include <pulp/format/reload/reload_abi.hpp>
#include <pulp/state/store.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

using namespace pulp;

namespace {

// ── EDIT ME LIVE ────────────────────────────────────────────────────────────
enum class Osc { Sine, Saw };
constexpr Osc kOsc = Osc::Sine;
// ─────────────────────────────────────────────────────────────────────────────

constexpr int kMaxVoices = 16;
constexpr float kPi = 3.14159265358979323846f;

class PolySynth final : public format::Processor {
public:
    format::PluginDescriptor descriptor() const override {
        format::PluginDescriptor d;
        d.name = "Pulp Hot-Reload Synth";
        d.manufacturer = "Pulp";
        d.bundle_id = "com.pulp.hot-reload-synth";
        d.version = "1.0.0";
        d.category = format::PluginCategory::Instrument;
        d.input_buses = {};                     // an instrument: no audio input
        d.output_buses = {{"Out", 2}};
        d.accepts_midi = true;                  // MIDI in drives the voices
        return d;
    }

    void define_parameters(state::StateStore& s) override {
        s.add_parameter({.id = 1, .name = "Level", .unit = "",
                         .range = {0.0f, 1.0f, 0.4f, 0.0f}});
        s.add_parameter({.id = 2, .name = "Release", .unit = "s",
                         .range = {0.02f, 3.0f, 0.4f, 0.0f}});
    }

    void prepare(const format::PrepareContext& ctx) override {
        sample_rate_ = ctx.sample_rate > 0 ? ctx.sample_rate : 48000.0;
        for (auto& v : voices_) v = Voice{};
    }

    void process(audio::BufferView<float>& out,
                 const audio::BufferView<const float>& /*in*/,
                 midi::MidiBuffer& midi_in, midi::MidiBuffer&,
                 const format::ProcessContext&) override {
        const float level = std::clamp(state().get_value(1), 0.0f, 1.0f);
        const float release_s = std::clamp(state().get_value(2), 0.02f, 3.0f);
        const float rel_step = 1.0f / (release_s * static_cast<float>(sample_rate_));
        const float atk_step = 1.0f / (0.005f * static_cast<float>(sample_rate_));  // 5ms attack

        // Apply all note events up front (block-accurate enough for a demo).
        for (const auto& ev : midi_in) {
            const auto& m = ev.message;
            if (m.isNoteOn())       note_on(m.getNoteNumber(), m.getVelocity());
            else if (m.isNoteOff()) note_off(m.getNoteNumber());
        }

        const std::size_t frames = out.num_samples();
        for (std::size_t n = 0; n < frames; ++n) {
            float mono = 0.0f;
            for (auto& v : voices_) {
                if (!v.active) continue;
                // AR envelope.
                if (v.releasing) { v.env -= rel_step; if (v.env <= 0.0f) { v.active = false; v.env = 0.0f; } }
                else if (v.env < 1.0f) { v.env = std::min(1.0f, v.env + atk_step); }
                mono += osc(v.phase) * v.env * v.amp;
                v.phase += v.inc;
                if (v.phase >= 1.0f) v.phase -= 1.0f;
            }
            mono *= level * 0.25f;                        // headroom for polyphony
            for (std::size_t c = 0; c < out.num_channels(); ++c) out.channel(c)[n] = mono;
        }
    }

private:
    struct Voice {
        bool active = false;
        bool releasing = false;
        int note = -1;
        float phase = 0.0f;
        float inc = 0.0f;
        float env = 0.0f;
        float amp = 0.0f;
    };

    static float osc(float phase01) {
        if constexpr (kOsc == Osc::Sine) return std::sin(2.0f * kPi * phase01);
        else                             return 2.0f * phase01 - 1.0f;   // naive saw: bright/buzzy
    }

    void note_on(int note, int vel) {
        Voice* slot = nullptr;
        for (auto& v : voices_) if (!v.active) { slot = &v; break; }
        if (!slot) slot = &voices_[0];                    // steal voice 0 if full
        slot->active = true; slot->releasing = false; slot->note = note;
        slot->phase = 0.0f; slot->env = 0.0f;
        slot->amp = static_cast<float>(vel) / 127.0f;
        const double freq = 440.0 * std::pow(2.0, (note - 69) / 12.0);
        slot->inc = static_cast<float>(freq / sample_rate_);
    }

    void note_off(int note) {
        for (auto& v : voices_)
            if (v.active && !v.releasing && v.note == note) v.releasing = true;
    }

    double sample_rate_ = 48000.0;
    std::array<Voice, kMaxVoices> voices_{};
};

}  // namespace

PULP_RELOAD_LOGIC(new PolySynth())
