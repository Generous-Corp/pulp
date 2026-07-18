#pragma once

#include "pulp_sampler.hpp"
#include "rt_allocation_probe.hpp"

#include <catch2/catch_test_macros.hpp>
#include <pulp/format/plugin_state_io.hpp>
#include <pulp/runtime/scope_guard.hpp>

#include <pulp/audio/audio_file.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <numeric>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace pulp;
using namespace pulp::examples;

namespace pulp::examples {

struct PulpSamplerHeritageTestAccess {
    static void pause_file_stage(PulpSamplerProcessor& processor,
                                 bool paused) noexcept {
        processor.streaming_->pause_file_stage_for_test(paused);
    }

    static bool file_stage_paused(
        const PulpSamplerProcessor& processor) noexcept {
        return processor.streaming_->file_stage_paused_for_test();
    }

    static std::pair<std::uint64_t, std::uint64_t> control_heritage_counts(
        const PulpSamplerProcessor& processor) noexcept {
        return {
            processor.control_heritage_attempts_for_test_.load(
                std::memory_order_acquire),
            processor.control_heritage_entries_for_test_.load(
                std::memory_order_acquire),
        };
    }

    static void fail_next_plan(PulpSamplerProcessor& processor) noexcept {
        processor.heritage_.fail_next_plan_for_test();
    }

    static std::size_t active_streamed_voices(
        const PulpSamplerProcessor& processor) noexcept {
        return static_cast<std::size_t>(std::count_if(
            std::begin(processor.voices_), std::end(processor.voices_),
            [](const SamplerVoice& voice) {
                return voice.active && voice.streamed;
            }));
    }

    static double active_streamed_position(
        const PulpSamplerProcessor& processor) noexcept {
        for (const auto& voice : processor.voices_) {
            if (voice.active && voice.streamed)
                return voice.stream_reader.cursor().position();
        }
        return -1.0;
    }

    static void force_stream_rate_capacity(PulpSamplerProcessor& processor,
                                           double frames_per_second) noexcept {
        processor.stream_rate_capacity_override_for_test_ = frames_per_second;
    }

    static double stream_output_sample_rate(
        const PulpSamplerProcessor& processor) noexcept {
        return processor.stream_output_sample_rate_;
    }

    static std::uint32_t maximum_stream_block_frames(
        const PulpSamplerProcessor& processor) noexcept {
        return processor.maximum_stream_block_frames_;
    }

    static audio::SamplePreloadContract preload_contract(
        const PulpSamplerProcessor& processor) noexcept {
        return processor.streaming_->published_source().streamed.preload_contract;
    }

    static double last_stream_demand_fps(
        const PulpSamplerProcessor& processor) noexcept {
        return processor.last_stream_demand_fps_for_test_;
    }

    static double last_lookahead_demand_fps(
        const PulpSamplerProcessor& processor) noexcept {
        return processor.last_lookahead_demand_fps_for_test_;
    }

    static bool has_retained_streamed_source(
        const PulpSamplerProcessor& processor) noexcept {
        return processor.streaming_->has_retained_streamed_source_for_test();
    }

    static void fail_next_stream_domain_prepare(
        PulpSamplerProcessor& processor) noexcept {
        processor.fail_next_stream_domain_prepare_for_test_ = true;
    }

    static void fail_next_stream_domain_source_restore(
        PulpSamplerProcessor& processor) noexcept {
        processor.fail_next_stream_domain_source_restore_for_test_ = true;
    }

    static bool retire_reverse_tail_page(
        PulpSamplerProcessor& processor) noexcept {
        const auto published = processor.streaming_->published_source();
        if (published.kind != SamplerPublishedSourceKind::Streamed ||
            published.streamed.stream_source.window == nullptr ||
            published.streamed.total_frames == 0) {
            return false;
        }
        auto* window = published.streamed.stream_source.window;
        const auto tail = window->ready_page_for_frame(
            published.streamed.source.source_generation,
            published.streamed.total_frames - 1);
        return tail.valid && window->retire_page(tail.page_index, 1);
    }
};

}  // namespace pulp::examples

namespace {

template <typename Predicate>
bool wait_for_heritage_condition(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(2);
    while (!predicate() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    return predicate();
}

struct HeritageTempWav {
    std::string path;

    explicit HeritageTempWav(std::string_view label,
                             std::uint64_t frames = 500000) {
        static std::atomic<std::uint64_t> sequence{0};
        path = (std::filesystem::temp_directory_path() /
                (std::string("pulp_sampler_heritage_") +
                 std::string(label) + "_" +
                 std::to_string(
                     std::chrono::steady_clock::now().time_since_epoch().count()) +
                 "_" + std::to_string(sequence.fetch_add(1)) + ".wav"))
                   .string();
        audio::AudioFileData data;
        data.sample_rate = 48000;
        data.channels = {
            std::vector<float>(static_cast<std::size_t>(frames), 0.25f)};
        REQUIRE(audio::write_wav_file(path, data,
                                      audio::WavBitDepth::Float32));
    }

    ~HeritageTempWav() {
        std::error_code error;
        std::filesystem::remove(path, error);
    }
};

audio::SampleHeritageProfile clock_profile(double ratio,
                                           bool bypass = false) {
    return {
        .schema_version = audio::kSampleHeritageProfileSchemaVersion,
        .profile_id = bypass ? "neutral.sampler-bypass-v2"
                             : "neutral.sampler-clock-v2",
        .host_sample_rate = 48000.0,
        .stages = {{bypass, audio::SampleHeritageClockPitchStage{ratio}}},
    };
}

[[maybe_unused]] audio::SampleHeritageProfile two_leg_profile() {
    return {
        .schema_version = audio::kSampleHeritageProfileSchemaVersion,
        .profile_id = "neutral.sampler-two-leg-v2",
        .host_sample_rate = 48000.0,
        .stages = {
            {false, audio::SampleHeritageMachineDomainStage{32000.0}},
            {false, audio::SampleHeritageClockPitchStage{1.25}},
        },
    };
}

[[maybe_unused]] audio::SampleHeritageProfile continued_noise_profile() {
    return {
        .schema_version = audio::kSampleHeritageProfileSchemaVersion,
        .profile_id = "neutral.sampler-state-v2",
        .host_sample_rate = 48000.0,
        .stages = {{false,
                    audio::SampleHeritageNoiseStage{
                        0.01f, 0x12345678u,
                        audio::SampleHeritageSeedPolicy::
                            ContinueSerializedState}}},
    };
}

[[maybe_unused]] audio::SampleHeritageProfile clock_output_profile(double ratio,
                                                                   float gain) {
    return {
        .schema_version = audio::kSampleHeritageProfileSchemaVersion,
        .profile_id = "neutral.sampler-clock-output-v2",
        .host_sample_rate = 48000.0,
        .stages = {
            {false, audio::SampleHeritageClockPitchStage{ratio}},
            {false, audio::SampleHeritageOutputStage{gain}},
        },
    };
}

[[maybe_unused]] std::vector<float> make_sine(
    std::size_t frames, double frequency = 440.0,
    double sample_rate = 48000.0) {
    std::vector<float> result(frames);
    constexpr double pi = 3.1415926535897932384626433832795;
    for (std::size_t frame = 0; frame < frames; ++frame) {
        result[frame] = static_cast<float>(
            std::sin(2.0 * pi * frequency * static_cast<double>(frame) /
                     sample_rate));
    }
    return result;
}

struct HeritageFixture {
    state::StateStore store;
    PulpSamplerProcessor processor;
    std::uint32_t maximum_block_frames = 0;
    double sample_rate = 48000.0;

    HeritageFixture(std::uint32_t maximum_frames,
                    const audio::SampleHeritageProfile* profile = nullptr,
                    double prepared_sample_rate = 48000.0)
        : maximum_block_frames(maximum_frames),
          sample_rate(prepared_sample_rate) {
        processor.set_state_store(&store);
        processor.define_parameters(store);
        store.set_value(kSamplerAttack, 0.0f);
        store.set_value(kSamplerDecay, 0.0f);
        store.set_value(kSamplerSustain, 100.0f);
        if (profile != nullptr) {
            REQUIRE(processor.set_heritage_profile(*profile) ==
                    PulpSamplerHeritageStatus::PendingPrepare);
        }
        format::PrepareContext context;
        context.sample_rate = sample_rate;
        context.max_buffer_size = static_cast<int>(maximum_frames);
        context.input_channels = 0;
        context.output_channels = 2;
        processor.prepare(context);
    }

    void load(std::span<const float> sample) {
        REQUIRE(processor.load_sample(sample.data(),
                                      static_cast<int>(sample.size()),
                                      48000.0f));
    }
};

std::vector<float> render(HeritageFixture& fixture,
                          std::span<const std::size_t> partitions,
                          std::size_t note_on_frame = 0,
                          int note = 60) {
    std::vector<float> result;
    std::size_t absolute_frame = 0;
    bool note_sent = false;
    for (const auto frames : partitions) {
        std::vector<float> left(frames, 0.0f);
        std::vector<float> right(frames, 0.0f);
        float* output_ptrs[]{left.data(), right.data()};
        const float* input_ptrs[]{nullptr, nullptr};
        audio::BufferView<float> output(output_ptrs, 2, frames);
        audio::BufferView<const float> input(input_ptrs, 0, frames);
        midi::MidiBuffer midi_in;
        midi::MidiBuffer midi_out;
        if (!note_sent && note_on_frame >= absolute_frame &&
            note_on_frame <= absolute_frame + frames) {
            auto event = midi::MidiEvent::note_on(0, note, 127);
            event.sample_offset =
                static_cast<std::int32_t>(note_on_frame - absolute_frame);
            midi_in.add(event);
            note_sent = true;
        }
        format::ProcessContext context{fixture.sample_rate,
                                       static_cast<int>(frames)};
        fixture.processor.process(output, input, midi_in, midi_out, context);
        result.insert(result.end(), left.begin(), left.end());
        absolute_frame += frames;
    }
    return result;
}

[[maybe_unused]] double tone_projection(std::span<const float> samples,
                                        double frequency,
                                        double sample_rate) {
    constexpr double pi = 3.1415926535897932384626433832795;
    double sine = 0.0;
    double cosine = 0.0;
    for (std::size_t frame = 0; frame < samples.size(); ++frame) {
        const auto phase = 2.0 * pi * frequency *
                           static_cast<double>(frame) / sample_rate;
        sine += static_cast<double>(samples[frame]) * std::sin(phase);
        cosine += static_cast<double>(samples[frame]) * std::cos(phase);
    }
    return 2.0 * std::hypot(sine, cosine) /
           static_cast<double>(samples.size());
}

}  // namespace
