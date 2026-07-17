#pragma once

// GongInstrument -- a polyphonic struck gong / tam-tam.
//
// A generic circular metal plate: a dense inharmonic thin-plate modal cloud
// (hundreds of modes, ringing for many seconds) plus the bounded nonlinear
// bloom -- the amplitude-dependent up-cascade that makes a hard strike's
// spectrum swell after the attack. One Processor, a fixed pool of GongVoice.
//
// A note-on picks a free (or stolen) voice, retunes the plate cloud to the
// note's pitch (an alloc-free uniform-scale retune -- the triad set is
// scale-invariant, so it is not rebuilt), applies the current Decay/Tone/Bloom,
// and strikes it with an additive contact pulse. Nothing is reset on note-on --
// striking a plate adds energy to a body, it does not rebuild it -- so rapid
// repeats superpose and two notes ring on two voices without cutting each other.
//
// A fixed one-in/one-out topology with internal polyphony and an internal
// nonlinearity: a Processor, not a graph. The polyphony and the mode coupling
// are Processor concerns, not a reason to reach for SignalGraph.
//
// Controls (all control-rate, none allocate): Level, Tune, Decay, Tone, Strike
// Hardness, Bloom. Tone is a one-pole spectral tilt on the mono readout -- by
// linearity, filtering the summed modes is identical to tilting each mode's
// readout weight, and it never touches the physics. Bloom is the coupling
// amount: 0 is a pure linear inharmonic cloud, 1 is the full cascade.

#include "gong_voice.hpp"

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

enum GongParams {
    kGongLevel = 0,
    kGongTune = 1,
    kGongDecay = 2,
    kGongTone = 3,
    kGongHardness = 4,
    kGongBloom = 5,
};

class GongInstrument : public format::Processor {
public:
    static constexpr int kNumVoices = 8;

    /// The plate fundamental (2,0) is authored at 55 Hz = A1 = MIDI 33, so a
    /// held A1 plays the gong at its designed pitch; other notes retune it.
    static constexpr double kFundamentalHz = 55.0;
    static constexpr uint8_t kRootNote = 33;

    /// Peak strike gain at full velocity. Chosen with the area-normalized
    /// contact pulse so the injected strike energy is independent of hardness,
    /// and so a full-velocity strike drives the bloom into its tuned regime.
    /// This sets the INTERNAL modal energy scale the bloom coupling is tuned to.
    static constexpr float kStrikeGain = 6.0f;

    /// Output normalization: the summed modal readout at that internal energy
    /// scale is large (hundreds of modes), so the readout is scaled to keep a
    /// full-velocity strike near unity peak. Kept SEPARATE from kStrikeGain so
    /// output loudness can be set without disturbing the bloom's energy tuning.
    static constexpr float kOutputNorm = 0.012f;

    format::PluginDescriptor descriptor() const override {
        return {
            .name = "Gong",
            .manufacturer = "Pulp",
            .bundle_id = "com.pulp.gong",
            .version = "1.0.0",
            .category = format::PluginCategory::Instrument,
            .input_buses = {},
            .output_buses = {{"Main Out", 2, false}},
            .accepts_midi = true,
        };
    }

    void define_parameters(state::StateStore& store) override {
        store.add_parameter({.id = kGongLevel, .name = "Level", .unit = "%",
                             .range = {0.0f, 100.0f, 80.0f, 0.1f}});
        store.add_parameter({.id = kGongTune, .name = "Tune", .unit = "st",
                             .range = {-12.0f, 12.0f, 0.0f, 0.01f}});
        store.add_parameter({.id = kGongDecay, .name = "Decay", .unit = "x",
                             .range = {0.25f, 4.0f, 1.0f, 0.001f}});
        store.add_parameter({.id = kGongTone, .name = "Tone", .unit = "",
                             .range = {0.0f, 1.0f, 0.5f, 0.001f}});
        // Strike Hardness: 1 = hard (short contact, bright attack + more low-mode
        // energy for the cascade to pump); 0 = soft (long contact, dark attack).
        store.add_parameter({.id = kGongHardness, .name = "Strike Hardness", .unit = "",
                             .range = {0.0f, 1.0f, 0.6f, 0.001f}});
        // Bloom: the nonlinear coupling amount. 0 = linear inharmonic cloud,
        // 1 = full tam-tam up-cascade.
        store.add_parameter({.id = kGongBloom, .name = "Bloom", .unit = "",
                             .range = {0.0f, 1.0f, 0.7f, 0.001f}});
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

        // A timeline discontinuity is the one time the ringing bodies should be
        // cleared. A note-on is never this.
        if (ctx.should_reset_dsp_state()) {
            for (auto& v : voices_) v.reset();
            tone_lp_ = 0.0f;
        }

        const float level = state().get_value(kGongLevel) / 100.0f;
        const float tone = state().get_value(kGongTone);

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
                trigger(event.note(), event.velocity());
        }
        render_segment(cursor, block - cursor);

        // Tone: one-pole tilt on the summed mono readout. dark -> blend toward
        // the low-passed signal; bright -> boost the (signal - low-pass) high
        // shelf. Linear, so equivalent to tilting each mode's readout weight.
        constexpr float kToneCutoffCoef = 0.15f;  // ~1.2 kHz corner at 48 kHz
        const float dark = tone < 0.5f ? (0.5f - tone) * 2.0f : 0.0f;
        const float bright = tone > 0.5f ? (tone - 0.5f) * 2.0f : 0.0f;

        auto left = output.channel(0);
        const bool stereo = output.num_channels() > 1;
        auto right = output.channel(stereo ? 1 : 0);
        for (int i = 0; i < block; ++i) {
            const float x = mix_[static_cast<std::size_t>(i)];
            tone_lp_ += kToneCutoffCoef * (x - tone_lp_);
            const float shaped = x * (1.0f - dark) + tone_lp_ * dark
                               + bright * (x - tone_lp_);
            const float s = shaped * level * kOutputNorm;
            left[static_cast<std::size_t>(i)] = s;
            if (stereo) right[static_cast<std::size_t>(i)] = s;
        }
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

    /// @internal Test hook: modes in the generated plate spectrum.
    int spectrum_mode_count() const { return static_cast<int>(spectrum_.modes.size()); }

private:
    void allocate(double sample_rate, int max_block) {
        sample_rate_ = sample_rate;
        prepared_block_ = std::max(max_block, 1);
        spectrum_ = generate_plate_spectrum(kFundamentalHz);
        const int max_modes = std::max(static_cast<int>(spectrum_.modes.size()), 1);
        const int max_contact = kSoftContactSamples + 1;
        voices_.resize(kNumVoices);
        for (auto& v : voices_) {
            v.prepare(sample_rate, max_modes, kMaxPartners, max_contact, prepared_block_);
            v.set_modes(spectrum_.modes, spectrum_.out_weights);
        }
        mix_.assign(static_cast<std::size_t>(prepared_block_), 0.0f);
        tone_lp_ = 0.0f;
    }

    void render_segment(int offset, int count) {
        if (count <= 0) return;
        float* out = mix_.data() + offset;
        for (auto& v : voices_) v.render(out, count);
    }

    void trigger(uint8_t note, uint8_t velocity) {
        const float tune_st = state().get_value(kGongTune);
        const float decay = state().get_value(kGongDecay);
        const float hardness = state().get_value(kGongHardness);
        const float bloom = state().get_value(kGongBloom);

        const double ratio =
            std::pow(2.0, (static_cast<int>(note) - static_cast<int>(kRootNote)) / 12.0
                              + static_cast<double>(tune_st) / 12.0);

        // Hardness -> contact duration: hard (1) short, soft (0) long.
        const int contact = static_cast<int>(std::lround(
            kHardContactSamples + (1.0f - hardness) * (kSoftContactSamples - kHardContactSamples)));
        const float area_norm = contact > 1 ? 2.0f / static_cast<float>(contact) : 1.0f;
        const float amp = kStrikeGain * (static_cast<float>(velocity) / 127.0f) * area_norm;

        GongVoice& v = pick_voice();
        v.retune(ratio);
        v.set_decay_scale(decay);
        v.set_bloom(bloom);
        v.arm(amp, contact, note, next_age_++, spectrum_.max_t60_s * decay);
    }

    GongVoice& pick_voice() {
        GongVoice* oldest = &voices_[0];
        for (auto& v : voices_) {
            if (!v.active()) return v;
            if (v.age() < oldest->age()) oldest = &v;
        }
        return *oldest;
    }

    static constexpr int kMaxPartners = 16;
    static constexpr int kHardContactSamples = 8;
    static constexpr int kSoftContactSamples = 80;

    PlateSpectrum spectrum_;
    std::vector<GongVoice> voices_;
    std::vector<float> mix_;   ///< per-block mono accumulator
    double sample_rate_ = 0.0;
    int prepared_block_ = 512;
    float tone_lp_ = 0.0f;     ///< Tone one-pole state
    uint64_t next_age_ = 1;
};

inline std::unique_ptr<format::Processor> create_gong() {
    return std::make_unique<GongInstrument>();
}

}  // namespace pulp::examples
