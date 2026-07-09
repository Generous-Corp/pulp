#pragma once

// Bendr — a Pulp example effect built entirely from public pulp::signal
// primitives.
//
// Composes RealtimePitchTimeProcessor (with built-in FreezeHold +
// TransientPhasePolicy), PitchedFeedbackDelay (with a second low-latency
// pitch processor inside the feedback loop), and DryWetMixer into one
// effect surface: realtime pitch ±12 st, independent/linked formant
// control with preserve mode, freeze with MIDI pitch over held audio,
// tempo-syncable pitched feedback delay, and dry/wet with latency
// compensation. The primitives are used unmodified from core/signal.

#include <pulp/format/processor.hpp>
#include <pulp/signal/dry_wet_mixer.hpp>
#include <pulp/signal/fft.hpp>
#include <pulp/signal/pitched_feedback_delay.hpp>
#include <pulp/signal/realtime_pitch_time_processor.hpp>
#include <pulp/state/midi_parameter_map.hpp>
#include <pulp/runtime/triple_buffer.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <complex>
#include <memory>
#include <vector>

namespace pulp::view { class View; }

namespace bendr {
// Magnitude spectrum (dB) published lock-free from the audio thread to
// the UI for the live spectrum display. 256 log-ready bins.
inline constexpr int kSpectrumBins = 256;
using SpectrumFrame = std::array<float, kSpectrumBins>;
using SpectrumBus = pulp::runtime::TripleBuffer<SpectrumFrame>;
}

namespace bendr {

namespace sig = pulp::signal;

enum Params : pulp::state::ParamID {
    kPitch = 1,         // semitones
    kFormant = 2,       // semitones (offset; absolute when unlinked)
    kLink = 3,          // formant follows pitch + offset
    kPreserve = 4,      // formant preservation under pitch shift
    kFreeze = 5,
    kDelayOn = 6,
    kDelayMs = 7,
    kDelaySync = 8,     // 0 = ms mode, 1..6 = 1/16..2 beats
    kFeedback = 9,      // %
    kLoopPitch = 10,    // pitch processor inside the feedback loop
    kMix = 11,          // %
    kMidiPitch = 12,    // MIDI notes/wheel drive pitch
    kBypass = 13,
};

// Adapter exposing the loop pitch processor to PitchedFeedbackDelay.
class LoopPitch final : public sig::FeedbackLoopProcessor {
public:
    sig::RealtimePitchTimeProcessor processor;
    bool frozen = false;
    int loop_latency_samples() const override { return processor.latency_samples(); }
    bool loop_is_frozen() const override { return frozen; }
    void loop_process(const float* const* in, float* const* out, int n) override {
        processor.process(in, out, n);
    }
};

class ReferenceProcessor : public pulp::format::Processor {
public:
    pulp::format::PluginDescriptor descriptor() const override {
        return {
            .name = "Bendr",
            .manufacturer = "Pulp",
            .bundle_id = "local.bendr.bendr",
            .version = "0.1.0",
            .category = pulp::format::PluginCategory::Effect,
            // aumf (MusicEffect): receives MIDI so the frozen spectral hold can
            // be played chromatically and notes/bend can drive pitch (kMidiPitch).
            // Requires all three to agree: this flag, the aumf type in
            // Info.plist.au, and PULP_AU_MIDI_PLUGIN in au_v2_entry.cpp (which
            // registers through AUMIDIEffectFactory so MusicDeviceMIDIEvent
            // dispatches — auval fails "-4 IN CALL MusicDeviceMIDIEvent" with the
            // plain AUBaseFactory). The "adjusting a param kills the instrument"
            // symptom was NEVER the AU type — that was the editor stealing the
            // host's keyboard (Logic Musical Typing), fixed in the SDK
            // plugin-view host focus contract (parts 1+2).
            .input_buses = {{"Audio In", 2}},
            .output_buses = {{"Audio Out", 2}},
            .accepts_midi = true,
        };
    }

    void define_parameters(pulp::state::StateStore& store) override {
        store.add_parameter({.id = kPitch, .name = "Pitch", .unit = "st",
                             .range = {-12.0f, 12.0f, 0.0f, 0.0f}});
        store.add_parameter({.id = kFormant, .name = "Formant", .unit = "st",
                             .range = {-12.0f, 12.0f, 0.0f, 0.0f}});
        store.add_parameter({.id = kLink, .name = "Link", .unit = "",
                             .range = {0.0f, 1.0f, 0.0f, 1.0f}});
        store.add_parameter({.id = kPreserve, .name = "Preserve", .unit = "",
                             .range = {0.0f, 1.0f, 1.0f, 1.0f}});
        store.add_parameter({.id = kFreeze, .name = "Freeze", .unit = "",
                             .range = {0.0f, 1.0f, 0.0f, 1.0f}});
        store.add_parameter({.id = kDelayOn, .name = "Delay", .unit = "",
                             .range = {0.0f, 1.0f, 0.0f, 1.0f}});
        store.add_parameter({.id = kDelayMs, .name = "Time", .unit = "ms",
                             .range = {30.0f, 2000.0f, 350.0f, 0.0f}});
        store.add_parameter({.id = kDelaySync, .name = "Sync", .unit = "",
                             .range = {0.0f, 6.0f, 0.0f, 1.0f}});
        store.add_parameter({.id = kFeedback, .name = "Feedback", .unit = "%",
                             .range = {0.0f, 95.0f, 40.0f, 0.0f}});
        store.add_parameter({.id = kLoopPitch, .name = "LoopPitch", .unit = "",
                             .range = {0.0f, 1.0f, 1.0f, 1.0f}});
        store.add_parameter({.id = kMix, .name = "Mix", .unit = "%",
                             .range = {0.0f, 100.0f, 100.0f, 0.0f}});
        store.add_parameter({.id = kMidiPitch, .name = "MIDI Pitch", .unit = "",
                             .range = {0.0f, 1.0f, 0.0f, 1.0f}});
        store.add_parameter({.id = kBypass, .name = "Bypass", .unit = "",
                             .range = {0.0f, 1.0f, 0.0f, 1.0f}});
    }

    void prepare(const pulp::format::PrepareContext& ctx) override {
        sample_rate_ = ctx.sample_rate;
        const int max_block = std::max(32, ctx.max_buffer_size);

        sig::RealtimePitchTimeConfig main_config;
        main_config.quality = sig::PitchTimeQuality::quality;
        main_config.channels = 2;
        main_config.max_block = max_block;
        main_config.formant_mode = sig::FormantMode::preserve;
        main_config.noise_morphing = true; // v2: transparent noise/texture stretch
        pitch_.prepare(sample_rate_, main_config);

        sig::RealtimePitchTimeConfig loop_config;
        loop_config.quality = sig::PitchTimeQuality::low_latency;
        loop_config.channels = 2;
        loop_config.max_block = max_block;
        loop_pitch_.processor.prepare(sample_rate_, loop_config);

        sig::PitchedFeedbackDelay::Config delay_config;
        delay_config.max_delay_seconds = 2.5f;
        delay_config.channels = 2;
        delay_config.max_block = max_block;
        delay_.prepare(sample_rate_, delay_config);

        mixer_.set_wet_latency(pitch_.latency_samples());
        mixer_.prepare(2, max_block);

        wet_.assign(static_cast<size_t>(2) * max_block, 0.0f);
        delay_out_.assign(static_cast<size_t>(2) * max_block, 0.0f);

        silence_.assign(static_cast<size_t>(max_block), 0.0f);

        max_block_ = max_block;

        // Published for the editor's latency readout. The vocoder's latency is
        // geometry-derived, so it is only known once prepare() has picked a
        // sample rate and block size.
        latency_ms_.store(
            static_cast<float>(pitch_.latency_samples() / sample_rate_ * 1000.0),
            std::memory_order_relaxed);
    }

    int latency_samples() const override { return pitch_.latency_samples(); }

    // Reported latency in milliseconds, for the editor. Written on prepare(),
    // read by the UI thread.
    const std::atomic<float>& latency_ms() const { return latency_ms_; }

    // Native GPU UI. Defined in reference_view.cpp to keep the UI header
    // (which includes this one) out of the audio-only TUs.
    std::unique_ptr<pulp::view::View> create_view() override;

    // Proportional, aspect-locked editor window.
    pulp::format::ViewSize view_size() const override {
        pulp::format::ViewSize s;
        s.preferred_width = 760;
        s.preferred_height = 560;
        s.min_width = 560;
        s.min_height = 412;
        s.max_width = 1520;
        s.max_height = 1120;
        s.aspect_ratio = 760.0 / 560.0;
        return s;
    }

    void release() override {}

    void process(pulp::audio::BufferView<float>& output,
                 const pulp::audio::BufferView<const float>& input,
                 pulp::midi::MidiBuffer& midi_in, pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext& ctx) override {
        const int n = static_cast<int>(output.num_samples());
        const int channels = 2;  // the pipeline is built stereo
        // The host can instantiate this AU with a mono (or absent) input/
        // output bus — never index a channel the buffer doesn't have. A mono
        // input feeds both internal lanes; output writes only the channels
        // that exist. Block size is guaranteed <= the prepared max_buffer_size
        // by the format adapter (it clamps oversized host renders), so the
        // fixed-size scratch buffers below are always in range.
        const int in_ch = static_cast<int>(input.num_channels());
        const int out_ch = static_cast<int>(output.num_channels());
        const float* in0 = in_ch > 0 ? input.channel(0).data() : silence_.data();
        const float* in1 = in_ch > 1 ? input.channel(1).data() : in0;
        const float* ins[2] = {in0, in1};

        if (state().get_value(kBypass) >= 0.5f) {
            for (int ch = 0; ch < out_ch; ++ch) {
                const float* sgl = (ch == 0) ? in0 : in1;
                auto out = output.channel(static_cast<size_t>(ch));
                std::copy(sgl, sgl + n, out.begin());
            }
            return;
        }

        // ── MIDI ──
        // Apply any pending learn/map commands from the UI, then route
        // incoming CC to mapped parameters (records as automation).
        midi_map_.pump();
        const bool midi_pitch = state().get_value(kMidiPitch) >= 0.5f;
        for (std::size_t e = 0; e < midi_in.size(); ++e) {
            const auto& evt = midi_in[e];
            if (evt.is_cc())
                midi_map_.handle_cc(state(), evt.channel(), evt.cc_number(), evt.cc_value());
            if (midi_pitch) {
                if (evt.is_note_on())
                    midi_note_offset_ = std::clamp(static_cast<float>(evt.note()) - 60.0f,
                                                   -12.0f, 12.0f);
                else if (evt.is_pitch_bend())
                    midi_bend_ = (static_cast<float>(evt.message.getPitchWheelValue()) - 8192.0f)
                                 / 8192.0f * 2.0f; // wheel = +/-2 st, clipped with note below
            }
        }

        const float pitch_param = state().get_value(kPitch);
        const float pitch = midi_pitch
            ? std::clamp(midi_note_offset_ + midi_bend_, -12.0f, 12.0f)
            : pitch_param;
        const bool linked = state().get_value(kLink) >= 0.5f;
        const float formant_offset = state().get_value(kFormant);
        // Linked: formant rides pitch plus the offset; unlinked: absolute.
        const float formant = linked
            ? std::clamp(pitch + formant_offset, -12.0f, 12.0f)
            : formant_offset;

        pitch_.set_pitch_semitones(pitch);
        pitch_.set_formant_semitones(formant);
        pitch_.set_formant_mode(state().get_value(kPreserve) >= 0.5f
                                    ? sig::FormantMode::preserve
                                    : sig::FormantMode::follow);
        // Spectral freeze (FreezeHold inside the vocoder): the hold is a
        // stationary resynthesis — averaged magnitudes, phases advancing at
        // each bin's instantaneous frequency — so there is no loop boundary
        // to hear, and pitch/formant keep bending the held audio. The
        // delay's internal loop-pitch is still gated by freeze so feedback
        // can't run away.
        const bool freeze_on = state().get_value(kFreeze) >= 0.5f;
        pitch_.set_frozen(freeze_on);
        loop_pitch_.processor.set_pitch_semitones(pitch);
        loop_pitch_.frozen = freeze_on;

        const bool delay_on = state().get_value(kDelayOn) >= 0.5f;
        const int sync = static_cast<int>(state().get_value(kDelaySync));
        if (sync > 0 && ctx.tempo_bpm > 0.0) {
            static constexpr double divisions[] = {0.25, 0.5, 1.0, 2.0, 4.0, 8.0};
            delay_.set_delay_sync(ctx.tempo_bpm, divisions[std::min(sync - 1, 5)] * 0.25);
        } else {
            delay_.set_delay_ms(state().get_value(kDelayMs));
        }
        delay_.set_feedback(state().get_value(kFeedback) / 100.0f);
        delay_.set_loop_processor(state().get_value(kLoopPitch) >= 0.5f ? &loop_pitch_
                                                                        : nullptr);
        mixer_.set_mix(state().get_value(kMix) / 100.0f);

        // ── audio path: dry tap → pitch → (+ delay send) → dry/wet ──
        float* wet[2] = {wet_.data(), wet_.data() + max_block_};
        float* dly[2] = {delay_out_.data(), delay_out_.data() + max_block_};

        // The freeze lives inside the vocoder (wet path); the dry tap always
        // carries the live input, so at Mix < 100% you can play over the
        // frozen pad.
        mixer_.push_dry(ins, channels, n);
        pitch_.process(ins, wet, n);

        if (delay_on) {
            const float* wet_const[2] = {wet[0], wet[1]};
            delay_.process(wet_const, dly, n);
            for (int ch = 0; ch < channels; ++ch)
                for (int i = 0; i < n; ++i)
                    wet[ch][i] += dly[ch][i];
        }

        mixer_.mix_wet(wet, channels, n);
        for (int ch = 0; ch < std::min(channels, out_ch); ++ch) {
            auto out = output.channel(static_cast<size_t>(ch));
            std::copy(wet[ch], wet[ch] + n, out.begin());
        }

        publish_spectrum(wet[0], n);
    }

    // Lock-free latest spectrum for the UI display (UI is sole reader).
    SpectrumBus& spectrum_bus() { return spectrum_bus_; }

    // MIDI-learn map: the UI arms learn / sets mappings; the audio thread
    // pumps + routes incoming CC to parameters (records as automation).
    pulp::state::MidiParameterMap& midi_map() { return midi_map_; }

    // Test access (private repo only).
    sig::RealtimePitchTimeProcessor& pitch_processor() { return pitch_; }

private:
    // Accumulate the wet output into a ring; once per block run a windowed
    // FFT and publish a 256-bin dB magnitude spectrum. RT-safe: the Fft is
    // preallocated and the TripleBuffer write never blocks.
    void publish_spectrum(const float* mono, int n) {
        for (int i = 0; i < n; ++i) {
            spec_ring_[static_cast<size_t>(spec_pos_)] = mono[i];
            spec_pos_ = (spec_pos_ + 1) % kSpectrumFft;
        }
        for (int i = 0; i < kSpectrumFft; ++i) {
            const float w = 0.5f - 0.5f * std::cos(2.0f * 3.14159265f * i / kSpectrumFft);
            spec_time_[static_cast<size_t>(i)] =
                spec_ring_[static_cast<size_t>((spec_pos_ + i) % kSpectrumFft)] * w;
        }
        spec_fft_.forward_real(spec_time_.data(), spec_freq_.data());
        SpectrumFrame frame;
        for (int k = 0; k < kSpectrumBins; ++k) {
            const float mag = std::abs(spec_freq_[static_cast<size_t>(k)]) / (kSpectrumFft * 0.25f);
            frame[static_cast<size_t>(k)] = 20.0f * std::log10(mag + 1e-7f);
        }
        spectrum_bus_.write(frame);
    }

    static constexpr int kSpectrumFft = 2 * kSpectrumBins; // 512

    double sample_rate_ = 48000.0;
    int max_block_ = 4096;

    sig::RealtimePitchTimeProcessor pitch_;
    LoopPitch loop_pitch_;
    sig::PitchedFeedbackDelay delay_;
    sig::DryWetMixer mixer_;

    std::vector<float> wet_;
    std::vector<float> delay_out_;
    std::vector<float> silence_;       // zero buffer for an absent input bus
    float midi_note_offset_ = 0.0f;
    float midi_bend_ = 0.0f;
    std::atomic<float> latency_ms_{0.0f};

    SpectrumBus spectrum_bus_;
    pulp::state::MidiParameterMap midi_map_;
    pulp::signal::Fft spec_fft_{kSpectrumFft};
    std::array<float, kSpectrumFft> spec_ring_{};
    std::array<float, kSpectrumFft> spec_time_{};
    std::array<std::complex<float>, kSpectrumFft> spec_freq_{};
    int spec_pos_ = 0;
};

inline std::unique_ptr<pulp::format::Processor> create_reference() {
    return std::make_unique<ReferenceProcessor>();
}

} // namespace bendr
