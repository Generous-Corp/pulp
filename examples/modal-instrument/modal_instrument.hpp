#pragma once

// ModalInstrument -- one polyphonic, spec-driven modal instrument.
//
// The thesis of the physical-modelling platform made concrete: an instrument
// is a ModalSpec (a data file of modes + excitation + position maps), and this
// one Processor plays any of them. It ships with a tuned-marimba spec built in
// so it makes sound out of the box; load another spec with set_spec().
//
// Polyphony is a fixed pool of ModalVoice, each a modal resonator bank, all
// allocated in prepare(). A note-on picks a free (or stolen) voice, retunes
// the spec's modes to the note's pitch, resolves them at the current
// strike/pickup position, and strikes the voice with an additive contact
// pulse. Nothing is reset on note-on -- a strike adds energy to a body, it
// does not rebuild it -- which is what lets rapid repeats superpose instead of
// machine-gunning, and lets two notes ring on two voices without either
// cutting the other.
//
// A fixed one-in/one-out topology with no runtime routing: a Processor, not a
// graph. The polyphony is internal, which is a Processor concern, not a reason
// to reach for SignalGraph.

#include "modal_voice.hpp"

#include <pulp/audio/buffer.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/signal/modal_spec.hpp>
#include <pulp/state/store.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

namespace pulp::examples {

enum ModalInstrumentParams {
    kModalStrikePosition = 0,
    kModalPickupPosition = 1,
    kModalLevel = 2,
};

/// The tuned-marimba default, built in code so the plugin makes sound with no
/// file present. Mirrors examples/modal-specs/marimba-bar-a3.json: A3 = 220 Hz
/// with the arch-tuned bar ratios 1 : 3.9 : 9.2. No shape maps, so strike and
/// pickup position have no effect on this spec -- load a spec with maps (the
/// ideal-string example) to hear position.
inline signal::ModalSpec default_marimba_spec() {
    signal::ModalSpec spec;
    spec.name = "marimba-bar-a3";
    spec.modes = {
        {220.0f, 1.6f, 1.0f},
        {858.0f, 0.45f, 0.35f},
        {2024.0f, 0.18f, 0.12f},
    };
    spec.excitation.contact_s = 0.0009;
    spec.excitation.velocity = 1.0;
    spec.tolerances.freq_cents = 5.0;
    spec.tolerances.t60_rel = 0.06;
    spec.tolerances.gain_rel = 0.05;
    spec.tolerances.verify_seconds = 2.0;
    return spec;
}

class ModalInstrument : public format::Processor {
public:
    static constexpr int kNumVoices = 16;

    /// MIDI note whose pitch equals the spec's authored mode frequencies.
    /// A3 = 220 Hz = MIDI 57 for the marimba default; call set_root_note() when
    /// loading a spec authored at a different pitch (the ideal-string example is
    /// A2 = 110 Hz = MIDI 45).
    static constexpr uint8_t kDefaultRootNote = 57;

    ModalInstrument() : spec_(default_marimba_spec()) {}

    format::PluginDescriptor descriptor() const override {
        return {
            .name = "ModalInstrument",
            .manufacturer = "Pulp",
            .bundle_id = "com.pulp.modal-instrument",
            .version = "1.0.0",
            .category = format::PluginCategory::Instrument,
            .input_buses = {},
            .output_buses = {{"Main Out", 2, false}},
            .accepts_midi = true,
        };
    }

    void define_parameters(state::StateStore& store) override {
        // Strike and pickup position are the physically-real timbre controls:
        // through a spec's shape maps they weight each mode by phi(position), so
        // striking a mode's node kills it and a pickup at a node mutes it. For a
        // spec with no maps (the marimba) they are inert by construction.
        store.add_parameter({.id = kModalStrikePosition,
                             .name = "Strike Position",
                             .unit = "",
                             .range = {0.0f, 1.0f, 0.5f, 0.001f}});
        store.add_parameter({.id = kModalPickupPosition,
                             .name = "Pickup Position",
                             .unit = "",
                             .range = {0.0f, 1.0f, 0.5f, 0.001f}});
        store.add_parameter({.id = kModalLevel,
                             .name = "Level",
                             .unit = "%",
                             .range = {0.0f, 100.0f, 80.0f, 0.1f}});
    }

    /// Replace the loaded spec. Loader thread only (re-sizes scratch and, if
    /// already prepared, re-prepares the voice pool for the new mode count).
    void set_spec(signal::ModalSpec spec) {
        spec_ = std::move(spec);
        if (sample_rate_ > 0.0) allocate(sample_rate_, prepared_block_);
    }

    /// Set the MIDI note whose pitch matches the spec's authored frequencies.
    void set_root_note(uint8_t note) { root_note_ = note; }

    const signal::ModalSpec& spec() const noexcept { return spec_; }

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

        // A timeline discontinuity is the one time the ringing bodies should be
        // cleared: the host jumped elsewhere and the tails belong to a moment no
        // longer playing. A note-on is categorically not this and never resets.
        if (ctx.should_reset_dsp_state())
            for (auto& v : voices_) v.reset();

        const float strike_pos = state().get_value(kModalStrikePosition);
        const float pickup_pos = state().get_value(kModalPickupPosition);
        const float level = state().get_value(kModalLevel) / 100.0f;

        const int block = std::min(n_samples, static_cast<int>(mix_.size()));
        std::fill(mix_.begin(), mix_.begin() + block, 0.0f);

        // Sample-accurate synth loop: render every voice up to each event's
        // offset, then act on the event, so a note lands on the sample the host
        // timestamped and a mid-block strike injects its pulse at that offset.
        int cursor = 0;
        const auto event_count = static_cast<int>(midi_in.size());
        for (int e = 0; e < event_count; ++e) {
            const auto& event = midi_in[static_cast<std::size_t>(e)];
            const int offset = std::clamp(event.sample_offset, cursor, block);
            render_segment(cursor, offset - cursor);
            cursor = offset;
            if (event.is_note_on() && event.velocity() > 0)
                trigger(event.note(), event.velocity(), strike_pos, pickup_pos);
        }
        render_segment(cursor, block - cursor);

        auto left = output.channel(0);
        const bool stereo = output.num_channels() > 1;
        auto right = output.channel(stereo ? 1 : 0);
        for (int i = 0; i < block; ++i) {
            const float s = mix_[static_cast<std::size_t>(i)] * level;
            left[static_cast<std::size_t>(i)] = s;
            if (stereo) right[static_cast<std::size_t>(i)] = s;
        }
        // A block larger than the prepared max is a host-contract violation, but
        // never emit uninitialized samples for the untouched tail if it happens.
        for (int i = block; i < n_samples; ++i) {
            left[static_cast<std::size_t>(i)] = 0.0f;
            if (stereo) right[static_cast<std::size_t>(i)] = 0.0f;
        }
    }

    /// @internal Test hook: number of voices currently ringing.
    int active_voice_count() const {
        int n = 0;
        for (const auto& v : voices_) n += v.active() ? 1 : 0;
        return n;
    }

private:
    void allocate(double sample_rate, int max_block) {
        sample_rate_ = sample_rate;
        prepared_block_ = std::max(max_block, 1);
        const int max_modes = std::max(static_cast<int>(spec_.modes.size()), 1);
        const int max_contact = std::max(spec_.contact_samples(sample_rate), 1);
        voices_.resize(kNumVoices);
        for (auto& v : voices_)
            v.prepare(sample_rate, max_modes, max_contact, prepared_block_);
        resolved_.assign(spec_.modes.size(), signal::ModalMode{});
        mix_.assign(static_cast<std::size_t>(prepared_block_), 0.0f);
        max_t60_ = 0.0;
        for (const auto& m : spec_.modes)
            max_t60_ = std::max(max_t60_, static_cast<double>(m.t60_s));
    }

    void render_segment(int offset, int count) {
        if (count <= 0) return;
        float* out = mix_.data() + offset;
        for (auto& v : voices_) v.render(out, count);
    }

    void trigger(uint8_t note, uint8_t velocity, float strike_pos, float pickup_pos) {
        // Resolve the spec at the current strike/pickup position (bakes the
        // phi(strike) * phi(pickup) mode coupling into each gain), then retune
        // every mode to the note's pitch by the equal-tempered ratio from root.
        signal::resolve_modes(spec_, strike_pos, pickup_pos, resolved_);
        const double ratio =
            std::pow(2.0, (static_cast<int>(note) - static_cast<int>(root_note_)) / 12.0);
        for (auto& m : resolved_)
            m.freq_hz = static_cast<float>(m.freq_hz * ratio);

        ModalVoice& v = pick_voice();
        v.set_modes(resolved_);
        const int contact = std::max(spec_.contact_samples(sample_rate_), 1);
        // Normalize the contact pulse to unit area so its DURATION shapes the
        // excitation spectrum (a long contact rolls off the high modes) without
        // also changing loudness: a raised cosine over n samples sums to n/2, so
        // scaling the peak by 2/n makes the injected impulse area equal the
        // velocity-scaled strike force regardless of contact length. This keeps
        // the instrument near unity out of the box instead of clipping.
        const float area_norm = contact > 1 ? 2.0f / static_cast<float>(contact) : 1.0f;
        const float amp = static_cast<float>(spec_.excitation.velocity) *
                          (static_cast<float>(velocity) / 127.0f) * area_norm;
        v.arm(amp, contact, note, next_age_++, max_t60_);
    }

    /// Prefer a free voice; otherwise steal the oldest ringing one. Round-robin
    /// over free voices spreads rapid repeats across the pool so they superpose.
    ModalVoice& pick_voice() {
        ModalVoice* oldest = &voices_[0];
        for (auto& v : voices_) {
            if (!v.active()) return v;
            if (v.age() < oldest->age()) oldest = &v;
        }
        return *oldest;
    }

    signal::ModalSpec spec_;
    std::vector<ModalVoice> voices_;
    std::vector<signal::ModalMode> resolved_;  ///< pre-sized retune scratch
    std::vector<float> mix_;                    ///< per-block mono accumulator
    double sample_rate_ = 0.0;
    double max_t60_ = 0.0;
    int prepared_block_ = 512;
    uint8_t root_note_ = kDefaultRootNote;
    uint64_t next_age_ = 1;
};

inline std::unique_ptr<format::Processor> create_modal_instrument() {
    return std::make_unique<ModalInstrument>();
}

}  // namespace pulp::examples
