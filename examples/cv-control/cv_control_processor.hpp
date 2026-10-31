#pragma once

#include "cv_control.hpp"
#include <pulp/format/processor.hpp>
#include <pulp/midi/buffer.hpp>
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
        mapping_.expire();
        timeout_samples_ = static_cast<std::uint64_t>(std::max(1.0, sample_rate_ * 0.25));
        consumed_sequence_ = input_.read().sequence;
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
        mapping_.expire();
    }

    void process(audio::BufferView<float>& output, const audio::BufferView<const float>&,
                 midi::MidiBuffer& midi_in, midi::MidiBuffer&,
                 const format::ProcessContext&) override {
        std::optional<ControlSample> newest;
#if !defined(PULP_WASM)
        const auto& published = input_.read();
        if (published.sequence != 0 && published.sequence != consumed_sequence_) {
            consumed_sequence_ = published.sequence;
            newest = published;
        }
#endif
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
        sample.sequence = ++publication_sequence_;
        input_.write(sample);
    }

#if !defined(PULP_WASM)
    void on_osc(const osc::Message& message) {
        if (message.address != "/pulp/cv/face" && message.address != "/pulp/control")
            return;
        if (message.args.size() < 3 || !std::holds_alternative<float>(message.args[0]) ||
            !std::holds_alternative<float>(message.args[1]) ||
            !std::holds_alternative<std::int32_t>(message.args[2]) ||
            (message.args.size() > 3 && !std::holds_alternative<std::int32_t>(message.args[3])))
            return;
        publish_control({0, std::get<float>(message.args[0]), std::get<float>(message.args[1]),
                         std::get<std::int32_t>(message.args[2]) != 0});
    }
#endif

    FaceMapping mapping_;
    runtime::TripleBuffer<ControlSample> input_;
    std::uint64_t publication_sequence_ = 0;
    std::uint64_t consumed_sequence_ = 0;
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
