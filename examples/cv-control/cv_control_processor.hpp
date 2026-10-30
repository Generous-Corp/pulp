#pragma once

#include "cv_control.hpp"
#include <pulp/format/processor.hpp>
#include <pulp/midi/buffer.hpp>
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
    }

    void process(audio::BufferView<float>& output, const audio::BufferView<const float>&,
                 midi::MidiBuffer& midi_in, midi::MidiBuffer&,
                 const format::ProcessContext&) override {
        for (const auto& event : midi_in) {
            if (event.is_note_on()) {
                frequency_ = 440.0 * std::pow(2.0, (event.note() - 69) / 12.0);
                gate_ = event.velocity() / 127.0f;
            } else if (event.is_note_off()) {
                gate_ = 0.0f;
            }
        }

        const float base = state().get_value(kBrightness);
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
        mapping_.submit(
            ControlSample{frame.sequence, frame.mouth_open, frame.confidence, frame.tracked});
    }
    void expire_face_frame() noexcept {
        mapping_.expire();
    }
    const FaceMapping& mapping() const noexcept {
        return mapping_;
    }

  private:
#if !defined(PULP_WASM)
    void on_osc(const osc::Message& message) {
        if (message.address == "/pulp/cv/face" && message.args.size() >= 3) {
            submit_face_frame({static_cast<std::uint64_t>(message.get_int(3)), message.get_float(0),
                               message.get_float(1), message.get_int(2) != 0});
        } else if (message.address == "/pulp/control" && message.args.size() >= 3) {
            mapping_.submit({static_cast<std::uint64_t>(message.get_int(3)), message.get_float(0),
                             message.get_float(1), message.get_int(2) != 0});
        }
    }
#endif

    FaceMapping mapping_;
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
