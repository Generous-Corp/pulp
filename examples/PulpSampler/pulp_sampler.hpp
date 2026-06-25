#pragma once

/// PulpSampler — sample-buffer sampler with MIDI triggering and ADSR envelope.
/// Demonstrates: controller-thread sample-slot publication, primitive loop
/// rendering, ADSR, pitch shifting, and processor parameter serialization.

#include "sampler_components.hpp"

#include <pulp/audio/buffer.hpp>
#include <pulp/audio/loop_renderer.hpp>
#include <pulp/audio/loop_types.hpp>
#include <pulp/audio/sample_key_map.hpp>
#include <pulp/audio/sample_slot_bank.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/signal/adsr.hpp>

#include <algorithm>
#include <atomic>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

namespace pulp::examples {

enum SamplerParams : state::ParamID {
    kSamplerGain     = 1,
    kSamplerAttack   = 2,
    kSamplerDecay    = 3,
    kSamplerSustain  = 4,
    kSamplerRelease  = 5,
    kSamplerPitch    = 6,  // semitones offset
    kSamplerLoop     = 7,  // 0 = one-shot, 1 = loop
};

class PulpSamplerProcessor : public format::Processor {
public:
    static constexpr int kMaxVoices = 8;
    static constexpr std::uint32_t kMaxSampleChannels = SamplerSampleStore::kMaxChannels;
    static constexpr std::uint32_t kMaxOutputChannels = 8;

    format::PluginDescriptor descriptor() const override {
        return {
            .name = "PulpSampler",
            .manufacturer = "Pulp",
            .bundle_id = "com.pulp.sampler",
            .version = "1.0.0",
            .category = format::PluginCategory::Instrument,
            .input_buses = {},
            .output_buses = {{"Audio Out", 2}},
            .accepts_midi = true,
            .produces_midi = false,
            .tail_samples = 0,
        };
    }

    void define_parameters(state::StateStore& store) override {
        store.add_parameter({.id = kSamplerGain, .name = "Gain",
            .unit = "dB", .range = {-60, 12, 0, 0.1f}});
        store.add_parameter({.id = kSamplerAttack, .name = "Attack",
            .unit = "ms", .range = {0, 5000, 10, 1}});
        store.add_parameter({.id = kSamplerDecay, .name = "Decay",
            .unit = "ms", .range = {0, 5000, 100, 1}});
        store.add_parameter({.id = kSamplerSustain, .name = "Sustain",
            .unit = "%", .range = {0, 100, 80, 1}});
        store.add_parameter({.id = kSamplerRelease, .name = "Release",
            .unit = "ms", .range = {0, 10000, 200, 1}});
        store.add_parameter({.id = kSamplerPitch, .name = "Pitch",
            .unit = "st", .range = {-24, 24, 0, 1}});
        store.add_parameter({.id = kSamplerLoop, .name = "Loop",
            .unit = "", .range = {0, 1, 0, 1}});
    }

    /// Load a mono sample buffer. Call off the audio thread after prepare().
    bool load_sample(const float* data, int num_samples, float sample_rate) {
        return sample_store_.load_mono(data,
                                       num_samples,
                                       sample_rate,
                                       audio_ack_generation_.load(std::memory_order_acquire));
    }

    /// Load a sample from interleaved stereo. Call off the audio thread after prepare().
    bool load_sample_stereo(const float* interleaved, int num_frames, float sample_rate) {
        return sample_store_.load_interleaved_stereo(
            interleaved,
            num_frames,
            sample_rate,
            audio_ack_generation_.load(std::memory_order_acquire));
    }

    bool has_sample() const { return sample_store_.has_sample(); }
    int sample_length() const { return sample_store_.sample_length(); }

    void prepare(const format::PrepareContext& ctx) override {
        host_sample_rate_ = static_cast<float>(ctx.sample_rate);
        max_block_frames_ = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(ctx.max_buffer_size));
        prepared_output_channels_ = std::clamp<std::uint32_t>(
            static_cast<std::uint32_t>(ctx.output_channels), 1, kMaxOutputChannels);

        for (std::uint32_t ch = 0; ch < kMaxOutputChannels; ++ch) {
            voice_scratch_[ch].assign(max_block_frames_, 0.0f);
        }
        sample_store_.prepare();
        for (auto& voice : voices_) voice.reset();
        publish_audio_acknowledgement(sample_store_.read_published_view());
    }

    void process(
        audio::BufferView<float>& output,
        const audio::BufferView<const float>&,
        midi::MidiBuffer& midi_in,
        midi::MidiBuffer&,
        const format::ProcessContext&) override
    {
        clear_output(output);

        const auto published = sample_store_.read_published_view();
        const bool can_trigger = sample_store_.slot_view_valid(published);

        const auto params = current_params();
        const auto block_frames = static_cast<std::uint32_t>(output.num_samples());
        midi_in.sort();

        std::uint32_t cursor = 0;
        for (std::size_t i = 0; i < midi_in.size(); ++i) {
            const auto& event = midi_in[i];
            const auto offset = static_cast<std::uint32_t>(
                std::clamp(event.sample_offset, 0, static_cast<int32_t>(block_frames)));
            if (offset > cursor) {
                render_active_voices(output, cursor, offset - cursor, params);
            }

            if (event.message.isNoteOn() && can_trigger) {
                trigger_note(event.message.getNoteNumber(),
                             static_cast<float>(event.message.getVelocity()) / 127.0f,
                             published,
                             params);
            } else if (event.message.isNoteOff()) {
                release_note(event.message.getNoteNumber());
            }
            cursor = offset;
        }

        if (cursor < block_frames) {
            render_active_voices(output, cursor, block_frames - cursor, params);
        }

        publish_audio_acknowledgement(published);
    }

private:
    struct RenderParams {
        float gain = 1.0f;
        signal::Adsr::Params adsr;
        float pitch_semitones = 0.0f;
        bool loop = false;
    };

    SamplerSampleStore sample_store_;
    audio::SampleKeyMap key_map_;
    std::array<std::vector<float>, kMaxOutputChannels> voice_scratch_{};
    std::array<float*, kMaxOutputChannels> voice_scratch_ptrs_{};
    std::atomic<std::uint64_t> audio_ack_generation_{0};
    float host_sample_rate_ = 44100.0f;
    std::uint32_t max_block_frames_ = 512;
    std::uint32_t prepared_output_channels_ = 2;
    SamplerVoice voices_[kMaxVoices]{};

    std::uint64_t audio_safe_generation(const audio::PublishedSampleView& published) const noexcept {
        std::array<audio::PublishedSampleView, kMaxVoices> active_views{};
        std::size_t active_count = 0;
        for (const auto& voice : voices_) {
            if (!voice.active || !voice.sample.valid) continue;
            active_views[active_count++] = voice.sample;
        }
        return audio::SampleSlotBank::oldest_active_generation(
            published, active_views.data(), active_count);
    }

    void publish_audio_acknowledgement(
        const audio::PublishedSampleView& published) noexcept {
        const auto generation = audio_safe_generation(published);
        audio_ack_generation_.store(generation, std::memory_order_release);
    }

    RenderParams current_params() const {
        RenderParams params;
        const float gain_db = state().get_value(kSamplerGain);
        params.gain = std::pow(10.0f, gain_db / 20.0f);
        params.adsr.attack = state().get_value(kSamplerAttack) / 1000.0f;
        params.adsr.decay = state().get_value(kSamplerDecay) / 1000.0f;
        params.adsr.sustain = state().get_value(kSamplerSustain) / 100.0f;
        params.adsr.release = state().get_value(kSamplerRelease) / 1000.0f;
        params.pitch_semitones = state().get_value(kSamplerPitch);
        params.loop = state().get_value(kSamplerLoop) >= 0.5f;
        return params;
    }

    static void clear_output(audio::BufferView<float>& output) noexcept {
        for (std::size_t ch = 0; ch < output.num_channels(); ++ch) {
            std::fill_n(output.channel_ptr(ch), output.num_samples(), 0.0f);
        }
    }

    audio::LoopRegion make_region(const audio::PublishedSampleView& sample,
                                  bool loop) const noexcept {
        audio::LoopRegion region;
        region.start_frame = 0;
        region.end_frame = sample.num_frames;
        region.source_sample_rate = sample.sample_rate;
        region.playback_mode = loop ? audio::LoopPlaybackMode::Forward
                                    : audio::LoopPlaybackMode::OneShot;
        region.interpolation = audio::LoopInterpolationMode::Linear;
        region.crossfade_curve = audio::LoopCrossfadeCurve::Linear;
        if (loop && sample.num_frames >= 32) {
            region.crossfade_frames =
                std::min<std::uint64_t>({64, sample.num_frames / 8, sample.num_frames / 2});
        }
        return region;
    }

    double playback_speed(int note,
                          const audio::PublishedSampleView& sample,
                          const RenderParams& params) const noexcept {
        return key_map_.playback_rate_for_note(note,
                                               sample.sample_rate,
                                               static_cast<double>(host_sample_rate_),
                                               params.pitch_semitones);
    }

    void render_active_voices(audio::BufferView<float>& output,
                              std::uint32_t start_frame,
                              std::uint32_t frames,
                              const RenderParams& params) noexcept {
        if (frames == 0) return;

        const auto output_channels = std::min<std::uint32_t>(
            {static_cast<std::uint32_t>(output.num_channels()),
             prepared_output_channels_,
             kMaxOutputChannels});
        if (output_channels == 0) return;

        for (std::uint32_t ch = 0; ch < output_channels; ++ch) {
            voice_scratch_ptrs_[ch] = voice_scratch_[ch].data();
        }

        std::uint32_t rendered = 0;
        while (rendered < frames) {
            const auto chunk = std::min(frames - rendered, max_block_frames_);
            audio::BufferView<float> scratch(voice_scratch_ptrs_.data(),
                                             output_channels,
                                             chunk);
            for (auto& voice : voices_) {
                if (!voice.active) continue;

                std::array<const float*, kMaxSampleChannels> sample_ptrs{};
                if (!sample_store_.populate_channel_ptrs(voice.sample,
                                                         sample_ptrs.data(),
                                                         sample_ptrs.size())) {
                    voice.reset();
                    continue;
                }
                audio::BufferView<const float> source(
                    sample_ptrs.data(),
                    voice.sample.num_channels,
                    static_cast<std::size_t>(voice.sample.num_frames));

                voice.adsr.set_params(params.adsr);
                voice.renderer.set_playback_rate(playback_speed(voice.note, voice.sample, params));
                // LoopRenderer::render() is overwrite-only, so this scratch
                // buffer can be reused for each voice before additive mixdown.
                const auto loop_result = voice.renderer.render(source, scratch, chunk);

                bool voice_finished = false;
                for (std::uint32_t i = 0; i < chunk; ++i) {
                    const float env = voice.adsr.next();
                    if (env <= 0.0001f && voice.released) {
                        voice_finished = true;
                        break;
                    }

                    const float scale = env * voice.velocity * params.gain;
                    for (std::uint32_t ch = 0; ch < output_channels; ++ch) {
                        output.channel_ptr(ch)[start_frame + rendered + i] +=
                            voice_scratch_[ch][i] * scale;
                    }
                }

                if (voice_finished || !loop_result.active) {
                    voice.reset();
                }
            }
            rendered += chunk;
        }
    }

    void trigger_note(int note,
                      float velocity,
                      const audio::PublishedSampleView& sample,
                      const RenderParams& params) {
        SamplerVoice* target = nullptr;
        for (auto& voice : voices_) {
            if (!voice.active) {
                target = &voice;
                break;
            }
        }
        if (target == nullptr) target = &voices_[0];

        const auto region = make_region(sample, params.loop);
        const auto speed = playback_speed(note, sample, params);
        if (speed == 0.0) return;
        target->start(note, velocity, speed, host_sample_rate_, sample, region, sample.num_frames);
    }

    void release_note(int note) {
        for (auto& voice : voices_) {
            if (voice.active && voice.note == note && !voice.released) {
                voice.release();
            }
        }
    }
};

inline std::unique_ptr<format::Processor> create_pulp_sampler() {
    return std::make_unique<PulpSamplerProcessor>();
}

} // namespace pulp::examples
