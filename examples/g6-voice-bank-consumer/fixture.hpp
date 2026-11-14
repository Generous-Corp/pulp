#pragma once

#include "a1_reference_vectors.hpp"

#include <pulp/format/processor.hpp>
#include <pulp/host/voice_bank_binding.hpp>
#include <pulp/state/store.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <utility>

namespace pulp::examples::g6 {
using namespace pulp;
using namespace pulp::host;
inline constexpr float kBankGain = 0.3535533905932738f;

class ProbeVoice final : public midi::MpeSynthVoice {
  public:
    void set_sample_rate(double rate) noexcept {
        rate_ = rate;
    }
    void on_note_on(const midi::MpeNoteState& note) override {
        midi::MpeSynthVoice::on_note_on(note);
        phase_ = 0.0;
        release_frame_ = 0;
        set_smoothing(0.0f);
    }
    void on_note_off() override {
        midi::MpeSynthVoice::on_note_off();
        release_frame_ = 0;
    }
    void reset() override {
        midi::MpeSynthVoice::reset();
        phase_ = 0.0;
        release_frame_ = 0;
    }
    void render(float* out, int frames) override {
        render_stereo(out, nullptr, frames);
    }
    void render_stereo(float* left, float* right, int frames) {
        for (int i = 0; i < frames; ++i) {
            float left_sample = 0.0f;
            float right_sample = 0.0f;
            if (active()) {
                advance_smoothers();
                const double frequency =
                    440.0 *
                    std::exp2((static_cast<double>(note_number()) - 69.0 + pitch_bend()) / 12.0);
                const float triangle = phase_ < 0.5 ? static_cast<float>(4.0 * phase_ - 1.0)
                                                    : static_cast<float>(3.0 - 4.0 * phase_);
                const float factor =
                    releasing() ? (4.0f - static_cast<float>(release_frame_)) * 0.25f : 1.0f;
                const float source = triangle * (static_cast<float>(velocity()) / 127.0f);
                const float shaped = source * factor;
                left_sample = shaped * (1.0f + 0.25f * pressure());
                right_sample = shaped * (0.5f + 0.25f * timbre());
                phase_ += frequency / rate_;
                if (phase_ >= 1.0)
                    phase_ -= 1.0;
                if (releasing() && ++release_frame_ == 4)
                    finish_release();
            }
            left[i] = left_sample;
            if (right)
                right[i] = right_sample;
        }
    }

  private:
    double rate_ = 48000.0;
    double phase_ = 0.0;
    unsigned release_frame_ = 0;
};

class ProbeProcessor final : public format::Processor {
  public:
    explicit ProbeProcessor(ProbeVoice& voice) : voice_(voice) {}
    format::PluginDescriptor descriptor() const override {
        format::PluginDescriptor d;
        d.name = "Authored Voice";
        d.manufacturer = "Pulp Test";
        d.bundle_id = "com.pulp.test.authored-voice";
        d.version = "1.0.0";
        d.category = format::PluginCategory::Instrument;
        d.output_buses = {{"Stereo", 2, false}};
        return d;
    }
    void define_parameters(state::StateStore& store) override {
        store.add_parameter(
            {.id = 1, .name = "level", .range = state::ParamRange::linear(0.0f, 2.0f, 1.0f)});
    }
    void prepare(const format::PrepareContext& context) override {
        voice_.set_sample_rate(context.sample_rate);
    }
    void process(audio::BufferView<float>& output, const audio::BufferView<const float>&,
                 midi::MidiBuffer&, midi::MidiBuffer&, const format::ProcessContext&) override {
        auto* left = output.channel_ptr(0);
        auto* right = output.channel_ptr(1);
        voice_.render_stereo(left, right, static_cast<int>(output.num_samples()));
        const float gain = state().get_value(1) * kBankGain;
        for (std::size_t i = 0; i < output.num_samples(); ++i) {
            left[i] *= gain;
            right[i] *= gain;
        }
    }

  private:
    ProbeVoice& voice_;
};

class A1ScriptedVoice final : public midi::MpeSynthVoice {
  public:
    void bind(const a1::Case& fixture, std::uint32_t slot) noexcept {
        fixture_ = &fixture;
        slot_ = slot;
    }
    void on_note_on(const midi::MpeNoteState& note) override {
        midi::MpeSynthVoice::on_note_on(note);
        release_frame_ = 0;
    }
    void on_note_off() override {
        midi::MpeSynthVoice::on_note_off();
        release_frame_ = 0;
    }
    void render(float* out, int frames) override {
        render_stereo(out, nullptr, frames);
    }
    void render_stereo(float* left, float* right, int frames) noexcept {
        for (int i = 0; i < frames; ++i) {
            float l = 0.0f;
            float r = 0.0f;
            if (active() && fixture_) {
                const bool replacement = fixture_->kind == a1::Kind::Steal && note_number() == 70;
                const bool in_capture = frame_ >= fixture_->capture_start &&
                                        frame_ - fixture_->capture_start < fixture_->frames;
                const auto index = in_capture ? frame_ - fixture_->capture_start : 0;
                const auto replacement_index = index - fixture_->steal_sample;
                const float source_l = !in_capture   ? 0.0f
                                       : replacement ? fixture_->new_left[replacement_index]
                                                     : fixture_->left[slot_][index];
                const float source_r = !in_capture   ? 0.0f
                                       : replacement ? fixture_->new_right[replacement_index]
                                                     : fixture_->right[slot_][index];
                const float factor =
                    releasing() ? (4.0f - static_cast<float>(release_frame_)) * 0.25f : 1.0f;
                l = source_l * factor;
                r = source_r * factor;
                if (releasing() && ++release_frame_ == 4)
                    finish_release();
            }
            left[i] = l;
            if (right)
                right[i] = r;
            ++frame_;
        }
    }

  private:
    const a1::Case* fixture_ = nullptr;
    std::uint32_t slot_ = 0;
    std::size_t frame_ = 0;
    unsigned release_frame_ = 0;
};

class A1ScriptedProcessor final : public format::Processor {
  public:
    explicit A1ScriptedProcessor(A1ScriptedVoice& voice) : voice_(voice) {}
    format::PluginDescriptor descriptor() const override {
        format::PluginDescriptor result;
        result.name = "A1 Scripted Voice";
        result.manufacturer = "Pulp Test";
        result.bundle_id = "com.pulp.test.a1-scripted-voice";
        result.version = "1.0.0";
        result.category = format::PluginCategory::Instrument;
        result.output_buses = {{"Stereo", 2, false}};
        return result;
    }
    void define_parameters(state::StateStore& store) override {
        store.add_parameter(
            {.id = 1, .name = "level", .range = state::ParamRange::linear(0.0f, 2.0f, 1.0f)});
    }
    void prepare(const format::PrepareContext&) override {}
    void process(audio::BufferView<float>& output, const audio::BufferView<const float>&,
                 midi::MidiBuffer&, midi::MidiBuffer&, const format::ProcessContext&) override {
        auto* left = output.channel_ptr(0);
        auto* right = output.channel_ptr(1);
        voice_.render_stereo(left, right, static_cast<int>(output.num_samples()));
        const float effective = state().get_value(1) * kBankGain;
        for (std::size_t i = 0; i < output.num_samples(); ++i) {
            left[i] *= effective;
            right[i] *= effective;
        }
    }

  private:
    A1ScriptedVoice& voice_;
};

SubgraphTemplate definition() {
    SubgraphTemplate t;
    t.identity = "test.authored.voice.v1";
    t.binding_path = {"voice"};
    SubgraphNodeSpec node;
    node.key = "osc";
    node.type = NodeType::Plugin;
    node.channels = 2;
    node.processor_type_id = "test.authored.voice";
    node.processor_type_version = 1;
    node.processor_descriptor_version = "1.0.0";
    t.nodes.push_back(std::move(node));
    t.exposed_ports = {{"audio_out.left", "osc", 0, true, PortKind::Audio},
                       {"audio_out.right", "osc", 1, true, PortKind::Audio}};
    t.parameters = {{"level", "osc", 1.0f, 0.0f, 2.0f, 1, "level"}};
    return t;
}

std::unique_ptr<VoiceBankBinding>
make_bank(const VoiceTemplateSchema& authored, VoiceBankBindingResult& status,
          SubgraphTemplate declared = definition(),
          midi::MpeConfig config = midi::MpeConfig::standard_lower()) {
    auto owner = std::make_unique<MpeVoiceBankOwner<ProbeVoice>>(
        [](ProbeVoice& voice, std::uint32_t) -> std::unique_ptr<format::Processor> {
            return std::make_unique<ProbeProcessor>(voice);
        },
        std::array<VoiceBankInputEndpoint, 4>{
            {{"note_in", 0}, {"pitch", 1}, {"pressure", 2}, {"timbre", 3}}});
    return VoiceBankBinding::prepare(authored, std::move(declared), std::move(owner), "audio_out",
                                     48000, 256, status, config);
}

const auto& owner(const VoiceBankBinding& bank) {
    return static_cast<const MpeVoiceBankOwner<ProbeVoice>&>(bank.owner());
}

VoiceTemplateSchema schema() {
    VoiceTemplateSchema result;
    result.template_namespace = "probe";
    result.template_name = "osc";
    result.template_revision = 1;
    auto port = [](const char* name, VoiceBindingType type, VoiceBindingDirection direction,
                   VoiceBindingShape shape, VoiceBindingRate rate, std::uint32_t ordinal) {
        VoiceBindingSpec spec;
        spec.path = {"voice"};
        spec.target = name;
        spec.target_kind = VoiceBindingTarget::Port;
        spec.type = type;
        spec.direction = direction;
        spec.shape = shape;
        spec.rate = rate;
        spec.ordinal = ordinal;
        return spec;
    };
    result.bindings = {
        port("note_in", VoiceBindingType::Event, VoiceBindingDirection::Input,
             VoiceBindingShape::Scalar, VoiceBindingRate::Event, 0),
        port("pitch", VoiceBindingType::Control, VoiceBindingDirection::Input,
             VoiceBindingShape::Scalar, VoiceBindingRate::Sample, 1),
        port("pressure", VoiceBindingType::Control, VoiceBindingDirection::Input,
             VoiceBindingShape::Scalar, VoiceBindingRate::Sample, 2),
        port("timbre", VoiceBindingType::Control, VoiceBindingDirection::Input,
             VoiceBindingShape::Scalar, VoiceBindingRate::Sample, 3),
        port("audio_out", VoiceBindingType::Audio, VoiceBindingDirection::Output,
             VoiceBindingShape::Stereo, VoiceBindingRate::Sample, 4),
    };
    VoiceBindingSpec level;
    level.path = {"voice"};
    level.target = "level";
    level.target_kind = VoiceBindingTarget::Parameter;
    level.type = VoiceBindingType::Control;
    level.direction = VoiceBindingDirection::Input;
    level.shape = VoiceBindingShape::Scalar;
    level.rate = VoiceBindingRate::Block;
    result.bindings.push_back(level);
    return result;
}

std::unique_ptr<VoiceBankBinding> make_a1_bank(const a1::Case& fixture,
                                               VoiceBankBindingResult& status) {
    auto voice_owner = std::make_unique<MpeVoiceBankOwner<A1ScriptedVoice>>(
        [&fixture](A1ScriptedVoice& voice,
                   std::uint32_t slot) -> std::unique_ptr<format::Processor> {
            voice.bind(fixture, slot);
            return std::make_unique<A1ScriptedProcessor>(voice);
        },
        std::array<VoiceBankInputEndpoint, 4>{
            {{"note_in", 0}, {"pitch", 1}, {"pressure", 2}, {"timbre", 3}}});
    return VoiceBankBinding::prepare(schema(), definition(), std::move(voice_owner), "audio_out",
                                     48000, 256, status);
}

// VoiceBankBinding has no parameter getter, so the consumer keeps the value
// copy it persists. Only declared per-slot parameter values are captured;
// notes, generations, pedals, epochs and release tails are not state.
VoiceBankState capture_levels(const VoiceBankBinding& bank, const std::array<float, 8>& levels) {
    VoiceBankState state;
    state.manifest_sha256 = bank.manifest().sha256;
    for (std::uint32_t slot = 0; slot < levels.size(); ++slot)
        state.parameters.push_back({slot, {"voice"}, "level", levels[slot]});
    return state;
}

// Replays restored values onto a freshly prepared bank. A refused value means
// the caller must discard that bank; the published bank is never edited.
bool reapply_state(VoiceBankBinding& bank, const VoiceBankState& state) {
    for (const auto& value : state.parameters)
        if (!bank.set_slot_parameter(value.slot_ordinal, value.target, value.value))
            return false;
    return true;
}

HostEventRecord event(std::uint64_t epoch, std::uint64_t sample, std::uint64_t sequence,
                      midi::MidiEvent message) {
    HostEventRecord result;
    result.epoch = epoch;
    result.absolute_sample = static_cast<std::int64_t>(sample);
    result.producer = 1;
    result.producer_sequence = sequence;
    result.midi = message;
    return result;
}

} // namespace pulp::examples::g6
