#pragma once

// PreparedPiano -- a polyphonic prepared-string instrument.
//
// A prepared piano places an object against or through a string. What no sample
// library can hold is that the preparation's TYPE, POSITION and STRENGTH are
// continuous and per-note: sliding a bolt along the string, or pressing it in
// harder, is a physically coherent family of sounds, not a switch between
// recorded takes. This instrument makes those three the knobs.
//
// Each note is a PreparedString -- a linear stiff string plus one lumped
// contact at a movable point. Three preparation types share that one structure:
//
//   * Buzz (a loose bolt): a one-sided collision at the contact point. The
//     string rattles against the object, which transfers energy up the spectrum
//     -- measurably more energy above 2 kHz than the clean string, rising with
//     strike velocity (a bolt rattles more when the string is hit harder), and
//     decaying faster than the fundamental (the rattle also damps the note).
//   * Mute (felt/rubber): raises the decay of the modes with an antinode under
//     the felt, leaving the modes with a node there untouched -- a control-rate
//     edit to each mode's T60, no per-sample collision.
//   * Mass (a screw): shifts the frequency of the modes with an antinode under
//     the mass, leaving node modes put. (On a 1-D string a point mass shifts
//     modes; it does not split them -- true splitting needs a 2-D degeneracy.)
//
// The un-prepared limit (Strength 0) is a clean linear stiff string: every
// prepared render is compared against that baseline.
//
// A fixed one-in/one-out topology with internal polyphony: a Processor, not a
// graph. Nothing is reset on note-on -- a strike adds energy to a still-ringing
// string -- so repeats superpose and two notes ring independently.

#include "prepared_string.hpp"

#include <pulp/audio/buffer.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/signal/modal_bank.hpp>
#include <pulp/state/store.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

namespace pulp::examples {

enum PreparedPianoParams {
    kPrepLevel = 0,
    kPrepTune = 1,
    kPrepDecay = 2,
    kPrepTone = 3,
    kPrepType = 4,      ///< 0 = Buzz, 1 = Mute, 2 = Mass
    kPrepPosition = 5,
    kPrepStrength = 6,
};

enum class PrepType { Buzz = 0, Mute = 1, Mass = 2 };

class PreparedPiano : public format::Processor {
public:
    static constexpr int kNumVoices = 16;
    static constexpr int kNumModes = 48;

    /// MIDI note whose pitch equals the built-in string's mode frequencies:
    /// E2 = 82.41 Hz = MIDI 40.
    static constexpr uint8_t kRootNote = 40;

    /// Fixed strike and pickup positions on the string, in [0,1]. The hammer
    /// falls near one end (like a piano hammer at ~1/8 of the length) and the
    /// pickup listens near the bridge. Only the PREPARATION position moves.
    static constexpr float kStrikePos = 0.12f;
    static constexpr float kPickupPos = 0.28f;

    format::PluginDescriptor descriptor() const override {
        return {
            .name = "PreparedPiano",
            .manufacturer = "Pulp",
            .bundle_id = "com.pulp.prepared-piano",
            .version = "1.0.0",
            .category = format::PluginCategory::Instrument,
            .input_buses = {},
            .output_buses = {{"Main Out", 2, false}},
            .accepts_midi = true,
        };
    }

    void define_parameters(state::StateStore& store) override {
        // The uniform four, matching the kit convention.
        store.add_parameter({.id = kPrepLevel, .name = "Level", .unit = "%",
                             .range = {0.0f, 100.0f, 80.0f, 0.1f}});
        store.add_parameter({.id = kPrepTune, .name = "Tune", .unit = "x",
                             .range = {0.5f, 2.0f, 1.0f, 0.01f}});
        store.add_parameter({.id = kPrepDecay, .name = "Decay", .unit = "%",
                             .range = {0.0f, 100.0f, 50.0f, 0.1f}});
        // Tone is the mallet hardness: it sets the strike contact duration, and
        // a shorter contact has a flatter spectrum that reaches the high modes.
        store.add_parameter({.id = kPrepTone, .name = "Tone", .unit = "%",
                             .range = {0.0f, 100.0f, 60.0f, 0.1f}});
        // The three preparation controls -- the physically-real, per-note,
        // continuous surface a sample library cannot hold.
        store.add_parameter({.id = kPrepType, .name = "Prep Type", .unit = "",
                             .range = {0.0f, 2.0f, 0.0f, 1.0f}});
        store.add_parameter({.id = kPrepPosition, .name = "Prep Position", .unit = "",
                             .range = {0.0f, 1.0f, 0.28f, 0.001f}});
        store.add_parameter({.id = kPrepStrength, .name = "Prep Strength", .unit = "%",
                             .range = {0.0f, 100.0f, 0.0f, 0.1f}});
    }

    void prepare(const format::PrepareContext& ctx) override {
        allocate(ctx.sample_rate, ctx.max_buffer_size);
    }

    void process(audio::BufferView<float>& output,
                 const audio::BufferView<const float>&,
                 midi::MidiBuffer& midi_in,
                 midi::MidiBuffer&,
                 const format::ProcessContext& ctx) override {
        const auto n_samples = static_cast<int>(output.num_samples());
        if (n_samples <= 0) return;

        // The one time to clear the ringing strings: a timeline discontinuity.
        // A note-on is never this.
        if (ctx.should_reset_dsp_state())
            for (auto& v : voices_) v.str.reset();

        const float level = state().get_value(kPrepLevel) / 100.0f;
        const float tune = state().get_value(kPrepTune);
        const float decay = state().get_value(kPrepDecay) / 100.0f;
        const float tone = state().get_value(kPrepTone) / 100.0f;
        const auto type = static_cast<PrepType>(
            std::clamp(static_cast<int>(std::lround(state().get_value(kPrepType))), 0, 2));
        const float position = std::clamp(state().get_value(kPrepPosition), 0.0f, 1.0f);
        const float strength = std::clamp(state().get_value(kPrepStrength) / 100.0f, 0.0f, 1.0f);

        const int block = std::min(n_samples, static_cast<int>(mix_.size()));
        std::fill(mix_.begin(), mix_.begin() + block, 0.0f);

        int cursor = 0;
        const auto event_count = static_cast<int>(midi_in.size());
        for (int e = 0; e < event_count; ++e) {
            const auto& event = midi_in[static_cast<std::size_t>(e)];
            const int offset = std::clamp(event.sample_offset, cursor, block);
            render_segment(cursor, offset - cursor);
            cursor = offset;
            if (event.is_note_on() && event.velocity() > 0)
                trigger(event.note(), event.velocity(), tune, decay, tone, type,
                        position, strength);
        }
        render_segment(cursor, block - cursor);

        auto left = output.channel(0);
        const bool stereo = output.num_channels() > 1;
        auto right = output.channel(stereo ? 1 : 0);
        for (int i = 0; i < block; ++i) {
            const float s = mix_[static_cast<std::size_t>(i)] * level * kOutputTrim;
            left[static_cast<std::size_t>(i)] = s;
            if (stereo) right[static_cast<std::size_t>(i)] = s;
        }
        for (int i = block; i < n_samples; ++i) {
            left[static_cast<std::size_t>(i)] = 0.0f;
            if (stereo) right[static_cast<std::size_t>(i)] = 0.0f;
        }
    }

    /// @internal Test hook.
    int active_voice_count() const {
        int n = 0;
        for (const auto& v : voices_) n += v.str.active() ? 1 : 0;
        return n;
    }

private:
    struct Voice {
        PreparedString str;
        uint64_t age = 0;
        uint8_t note = 0;
    };

    // ── The built-in string: a stiff steel string at E2. ────────────────────
    // f_n = n * f0 * sqrt(1 + B n^2) is the stiff-string dispersion law; the top
    // partials ride progressively sharp of the harmonic series, which a render
    // recovers as B ~ 2e-4. T60 falls with partial number and gain ~ 1/sqrt(n).
    // Mode i carries index n = i + 1, so phi_n(x) = sin(n*pi*x) is analytic.
    static constexpr double kF0 = 82.4069;   // E2
    static constexpr double kB = 2.0e-4;     // inharmonicity coefficient

    static double base_freq(int n) {
        return n * kF0 * std::sqrt(1.0 + kB * static_cast<double>(n) * n);
    }
    static double base_t60(int n) { return 4.0 * std::pow(static_cast<double>(n), -0.7); }
    static double base_gain(int n) { return 1.0 / std::sqrt(static_cast<double>(n)); }
    static double phi(int n, double x) {
        return std::sin(3.14159265358979323846 * static_cast<double>(n) * x);
    }

    void allocate(double sample_rate, int max_block) {
        sample_rate_ = sample_rate;
        prepared_block_ = std::max(max_block, 1);
        // Longest strike contact the Tone knob can ask for, plus headroom.
        const int max_contact = std::max(contact_samples(0.0f), 1) + 4;
        voices_.resize(kNumVoices);
        for (auto& v : voices_)
            v.str.prepare(sample_rate, kNumModes, max_contact, prepared_block_);
        modes_.assign(kNumModes, signal::ModalMode{});
        strike_w_.assign(kNumModes, 0.0f);
        pickup_w_.assign(kNumModes, 0.0f);
        contact_w_.assign(kNumModes, 0.0f);
        mix_.assign(static_cast<std::size_t>(prepared_block_), 0.0f);
    }

    /// Strike contact duration in samples. Tone 1 (bright) is the shortest
    /// contact (~0.15 ms); Tone 0 (dull) the longest (~2.5 ms), which rolls the
    /// high modes off. Duration in seconds so it is sample-rate independent.
    int contact_samples(float tone) const {
        const double ms = 2.5 - 2.35 * static_cast<double>(std::clamp(tone, 0.0f, 1.0f));
        return std::max(1, static_cast<int>(ms * 1.0e-3 * sample_rate_ + 0.5));
    }

    void render_segment(int offset, int count) {
        if (count <= 0) return;
        float* out = mix_.data() + offset;
        for (auto& v : voices_) v.str.render(out, count);
    }

    void trigger(uint8_t note, uint8_t velocity, float tune, float decay, float tone,
                 PrepType type, float position, float strength) {
        const double pitch_ratio =
            std::pow(2.0, (static_cast<int>(note) - static_cast<int>(kRootNote)) / 12.0) *
            static_cast<double>(tune);
        // Decay knob scales every mode's T60 around the authored value.
        const double decay_scale = kDecayMin + (kDecayMax - kDecayMin) *
                                                    static_cast<double>(decay);

        double max_t60 = 0.0;
        for (int i = 0; i < kNumModes; ++i) {
            const int n = i + 1;
            double freq = base_freq(n) * pitch_ratio;
            double t60 = base_t60(n) * decay_scale;

            if (type == PrepType::Mute) {
                // Add damping in proportion to phi(prep)^2: sigma -> sigma + d.
                // sigma = ln(10^3)/T60 (T60 is the -60 dB time).
                const double p = phi(n, position);
                const double sigma = 6.907755 / std::max(t60, 1.0e-4) +
                                     static_cast<double>(strength) * kDampMax * p * p;
                t60 = 6.907755 / sigma;
            } else if (type == PrepType::Mass) {
                // Point mass lowers the modes with an antinode under it.
                const double p = phi(n, position);
                freq *= 1.0 - static_cast<double>(strength) * kMassMax * p * p;
            }

            modes_[static_cast<std::size_t>(i)] = {static_cast<float>(freq),
                                                   static_cast<float>(t60),
                                                   static_cast<float>(base_gain(n))};
            strike_w_[static_cast<std::size_t>(i)] = static_cast<float>(phi(n, kStrikePos));
            pickup_w_[static_cast<std::size_t>(i)] = static_cast<float>(phi(n, kPickupPos));
            // The contact couples through phi at the preparation position (buzz).
            contact_w_[static_cast<std::size_t>(i)] =
                static_cast<float>(phi(n, position));
            max_t60 = std::max(max_t60, t60);
        }

        Voice& v = pick_voice();
        v.str.set_modes(modes_, strike_w_, pickup_w_, contact_w_);

        // Buzz: a one-sided contact whose gap drops as Strength rises (the bolt
        // pressed closer to the string). Mute/Mass disable the contact.
        if (type == PrepType::Buzz && strength > 0.0f) {
            const double gap = kGapOpen +
                               (kGapClosed - kGapOpen) * static_cast<double>(strength);
            v.str.set_contact(gap, kContactStiffness);
        } else {
            v.str.set_contact(1.0e30, 0.0);
        }

        const int contact = contact_samples(tone);
        const float area = contact > 1 ? 2.0f / static_cast<float>(contact) : 1.0f;
        const float amp = (static_cast<float>(velocity) / 127.0f) * area;
        v.note = note;
        v.age = next_age_++;
        v.str.arm(amp, contact, max_t60);
    }

    Voice& pick_voice() {
        Voice* oldest = &voices_[0];
        for (auto& v : voices_) {
            if (!v.str.active()) return v;
            if (v.age < oldest->age) oldest = &v;
        }
        return *oldest;
    }

    // Contact + preparation calibration (built-in string, displacement peak ~1.9
    // at velocity 1). Gap in the string's displacement units: at full Strength
    // the bolt sits just above rest so the string rattles it every swing; open,
    // it clears any normal strike. Stiffness is well into the rigid regime, so
    // the closed-form solve behaves as a hard barrier.
    static constexpr double kGapOpen = 1.4;
    static constexpr double kGapClosed = 0.05;
    static constexpr double kContactStiffness = 8000.0;
    // Mute: sigma added at a full-strength antinode (Np/s). Mass: max fractional
    // frequency drop at a full-strength antinode.
    static constexpr double kDampMax = 45.0;
    static constexpr double kMassMax = 0.09;
    // Decay knob maps to a T60 scale in [kDecayMin, kDecayMax].
    static constexpr double kDecayMin = 0.35;
    static constexpr double kDecayMax = 1.6;
    // Keeps the summed modal output near unity out of the box.
    static constexpr float kOutputTrim = 0.6f;

    std::vector<Voice> voices_;
    std::vector<signal::ModalMode> modes_;   ///< per-note retune scratch
    std::vector<float> strike_w_, pickup_w_, contact_w_;
    std::vector<float> mix_;
    double sample_rate_ = 0.0;
    int prepared_block_ = 512;
    uint64_t next_age_ = 1;
};

inline std::unique_ptr<format::Processor> create_prepared_piano() {
    return std::make_unique<PreparedPiano>();
}

}  // namespace pulp::examples
