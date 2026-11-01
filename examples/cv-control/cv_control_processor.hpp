#pragma once

#include "cv_control.hpp"
#include <pulp/format/processor.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/runtime/external_input.hpp>
#include <pulp/runtime/triple_buffer.hpp>
#if !defined(PULP_WASM)
#include <pulp/osc/osc.hpp>
#endif
#include <cmath>
#include <cstdlib>
#include <numbers>
#include <pulp/audio/buffer.hpp>

namespace pulp::examples::cv_control {

enum FaceToneParam : state::ParamID {
    kBrightness = 100,
    kVolume = 101,
};

/// A deliberately small synth proving additive CV modulation. The saved base
/// brightness remains in StateStore; the face value is an ephemeral offset.
class FaceToneProcessor final : public format::Processor {
  public:
    format::PluginDescriptor descriptor() const override {
        return {.name = "CV Face Tone",
                .manufacturer = "Pulp",
                .bundle_id = "com.pulp.cv-face-tone",
                .version = "0.1.0",
                .category = format::PluginCategory::Instrument,
                .input_buses = {},
                .output_buses = {{"Audio Out", 2}},
                .accepts_midi = true};
    }

    void define_parameters(state::StateStore& store) override {
        store.add_parameter(
            {.id = kBrightness, .name = "Brightness", .unit = "%", .range = {0.0f, 1.0f, 0.35f}});
        store.add_parameter(
            {.id = kVolume, .name = "Volume", .unit = "dB", .range = {-60.0f, 0.0f, -12.0f}});
    }

    void prepare(const format::PrepareContext& ctx) override {
        sample_rate_ = ctx.sample_rate;
        phase_ = 0.0;
        gate_ = 0.0f;
        input_gate_.expire();
        browser_gate_.expire();
        mapping_.expire();
        timeout_samples_ = static_cast<std::uint64_t>(std::max(1.0, sample_rate_ * 0.25));
#if !defined(PULP_WASM)
        if (const char* raw = std::getenv("PULP_CV_OSC_PORT")) {
            char* end = nullptr;
            const long port = std::strtol(raw, &end, 10);
            if (end != raw && port > 0 && port < 65536) {
                osc_.listen(static_cast<std::uint16_t>(port),
                            [this](const osc::Message& message) { on_osc(message); });
            }
        }
#endif
    }

    void release() override {
#if !defined(PULP_WASM)
        osc_.stop();
#endif
        input_gate_.expire();
        browser_gate_.expire();
        mapping_.expire();
    }

    void process(audio::BufferView<float>& output, const audio::BufferView<const float>&,
                 midi::MidiBuffer& midi_in, midi::MidiBuffer&,
                 const format::ProcessContext&) override {
        std::optional<ControlSample> newest;
#if !defined(PULP_WASM)
        const auto& published = input_.read();
        const auto verdict = input_gate_.submit(published, policy_);
        if (verdict == runtime::ExternalInputVerdict::accepted)
            newest = ControlSample{input_gate_.sequence(), input_gate_.value(0, 0.0f),
                                   published.confidence, true};
#endif
        // Browser multi-value packets use the existing WAM SysEx sidecar, but
        // still pass through the same bounded external-input policy as native
        // providers before they can affect the control mapping.
        for (const auto& sysex : midi_in.sysex()) {
            if (const auto frame = decode_midi_sysex(sysex.data)) {
                const auto verdict = browser_gate_.submit(*frame, browser_policy_);
                if (verdict == runtime::ExternalInputVerdict::accepted)
                    newest = ControlSample{browser_gate_.sequence(), browser_gate_.value(0, 0.0f),
                                           frame->confidence, true};
            }
        }
        for (const auto& event : midi_in) {
            // A single short MIDI message carries one coherent sample on every
            // surface. This example reserves channel-16 pitch bend.
            if (event.size() == 3 && event.data()[0] == 0xEF) {
                if (const auto sample = decode_midi_control(event.data()[0], event.data()[1],
                                                            event.data()[2], ++midi_sequence_)) {
                    newest = *sample;
                    continue;
                }
            }
            if (event.is_note_on()) {
                frequency_ = 440.0 * std::pow(2.0, (event.note() - 69) / 12.0);
                gate_ = event.velocity() / 127.0f;
            } else if (event.is_note_off()) {
                gate_ = 0.0f;
            }
        }

        if (newest) {
            mapping_.submit(*newest);
        } else {
            input_gate_.advance(output.num_samples(), policy_);
            browser_gate_.advance(output.num_samples(), browser_policy_);
            mapping_.advance(output.num_samples(), timeout_samples_);
        }

        // Include host modulation while leaving the saved/automated base alone.
        const float base = state().get_modulated(kBrightness);
        const float effective = std::clamp(base + (mapping_.value() * 0.5f), 0.0f, 1.0f);
        const float volume = std::pow(10.0f, state().get_value(kVolume) / 20.0f);
        state().set_display_modulation(kBrightness, effective);
        auto left = output.channel(0);
        auto right = output.channel(output.num_channels() > 1 ? 1 : 0);
        for (std::size_t i = 0; i < left.size(); ++i) {
            const float wave = static_cast<float>(std::sin(phase_ * 2.0 * std::numbers::pi));
            const float sample = wave * gate_ * volume * (0.25f + effective * 0.75f);
            left[i] = sample;
            right[i] = sample;
            phase_ += frequency_ / sample_rate_;
            if (phase_ >= 1.0)
                phase_ -= 1.0;
        }
    }

    void submit_face_frame(FaceFrame frame) noexcept {
        publish_control({frame.sequence, frame.mouth_open, frame.confidence, frame.tracked});
    }
    void expire_face_frame() noexcept {
        publish_control({0, 0.0f, 0.0f, false});
    }

  private:
    // Exactly one selected native provider owns this writer role. The audio
    // callback is the only reader and owns all mapping and timeout state.
    void publish_control(ControlSample sample) noexcept {
        runtime::ExternalInputFrame<1> frame;
        frame.source_id = 1;
        frame.epoch = epoch_;
        frame.sequence = ++publication_sequence_;
        frame.provider_time_ns = frame.sequence;
        frame.status = sample.valid ? runtime::ExternalInputStatus::valid
                                    : runtime::ExternalInputStatus::unavailable;
        frame.confidence = sample.confidence;
        frame.count = 1;
        frame.values[0] = sample.value;
        input_.write(frame);
    }

#if !defined(PULP_WASM)
    void on_osc(const osc::Message& message) {
        if (const auto sample = cv_control::decode_osc_control(message))
            publish_control(*sample);
    }
#endif

    FaceMapping mapping_;
    runtime::TripleBuffer<runtime::ExternalInputFrame<1>> input_;
    runtime::ExternalInputGate<1> input_gate_;
    runtime::ExternalInputPolicy policy_{1, 0.6f, 12000, 1};
    runtime::ExternalInputGate<16> browser_gate_;
    runtime::ExternalInputPolicy browser_policy_{1, 0.6f, 12000, 0};
    std::uint64_t publication_sequence_ = 0;
    std::uint32_t epoch_ = 1;
    std::uint64_t midi_sequence_ = 0;
    std::uint64_t timeout_samples_ = 12000;
#if !defined(PULP_WASM)
    osc::Receiver osc_;
#endif
    double sample_rate_ = 48000.0;
    double phase_ = 0.0;
    double frequency_ = 220.0;
    float gate_ = 0.0f;
};

inline std::unique_ptr<format::Processor> create_face_tone() {
    return std::make_unique<FaceToneProcessor>();
}

} // namespace pulp::examples::cv_control
