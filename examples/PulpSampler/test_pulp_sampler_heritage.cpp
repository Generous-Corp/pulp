#include "pulp_sampler.hpp"
#include "rt_allocation_probe.hpp"

#include <catch2/catch_test_macros.hpp>
#include <pulp/format/plugin_state_io.hpp>

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
#include <vector>

using namespace pulp;
using namespace pulp::examples;

namespace pulp::examples {

struct PulpSamplerHeritageTestAccess {
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

audio::SampleHeritageProfile two_leg_profile() {
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

audio::SampleHeritageProfile continued_noise_profile() {
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

audio::SampleHeritageProfile clock_output_profile(double ratio, float gain) {
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

std::vector<float> make_sine(std::size_t frames,
                             double frequency = 440.0,
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

double tone_projection(std::span<const float> samples,
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

TEST_CASE("PulpSampler heritage render is bitwise callback-partition invariant",
          "[audio][sampler][heritage][partition]") {
    const auto profile = two_leg_profile();
    auto sample = make_sine(48000);
    HeritageFixture whole(1024, &profile);
    HeritageFixture split(1024, &profile);
    whole.load(sample);
    split.load(sample);
    constexpr std::array one{std::size_t{1024}};
    constexpr std::array many{std::size_t{1}, std::size_t{31},
                              std::size_t{128}, std::size_t{7},
                              std::size_t{333}, std::size_t{524}};
    REQUIRE(std::accumulate(many.begin(), many.end(), std::size_t{0}) == 1024);
    REQUIRE(render(whole, one) == render(split, many));
}

TEST_CASE("PulpSampler heritage clock ratio raises sampler pitch",
          "[audio][sampler][heritage][pitch]") {
    const auto profile = clock_profile(2.0);
    auto sample = make_sine(48000);
    HeritageFixture fixture(4096, &profile);
    fixture.load(sample);
    constexpr std::array block{std::size_t{4096}};
    const auto output = render(fixture, block);
    const auto measured = std::span<const float>(output).subspan(256);
    REQUIRE(tone_projection(measured, 880.0, 48000.0) > 0.8);
    REQUIRE(tone_projection(measured, 880.0, 48000.0) >
            tone_projection(measured, 440.0, 48000.0) * 8.0);
}

TEST_CASE("PulpSampler heritage preserves host-offset MIDI causality",
          "[audio][sampler][heritage][midi]") {
    const auto profile = clock_profile(2.0);
    auto sample = make_sine(48000);
    HeritageFixture fixture(256, &profile);
    fixture.load(sample);
    constexpr std::array block{std::size_t{256}};
    const auto output = render(fixture, block, 64);
    REQUIRE(std::all_of(output.begin(), output.begin() + 64,
                        [](float sample_value) { return sample_value == 0.0f; }));
    REQUIRE(std::any_of(output.begin() + 64, output.end(),
                        [](float sample_value) {
                            return std::abs(sample_value) > 0.01f;
                        }));
}

TEST_CASE("PulpSampler heritage reports and renders causal impulse latency",
          "[audio][sampler][heritage][latency]") {
    const auto profile = clock_profile(2.0);
    std::vector<float> impulse(512, 0.0f);
    impulse[0] = 1.0f;
    HeritageFixture fixture(128, &profile);
    fixture.load(impulse);
    constexpr std::array block{std::size_t{128}};
    const auto output = render(fixture, block);
    const auto peak = static_cast<std::size_t>(std::distance(
        output.begin(), std::max_element(output.begin(), output.end(),
            [](float left, float right) {
                return std::abs(left) < std::abs(right);
            })));
    REQUIRE(fixture.processor.latency_samples() == 12);
    REQUIRE(peak == 12);
    REQUIRE(fixture.processor.descriptor().tail_samples == -1);
}

TEST_CASE("PulpSampler all-bypassed heritage uses the exact legacy render path",
          "[audio][sampler][heritage][bypass]") {
    const auto profile = clock_profile(2.0, true);
    auto sample = make_sine(48000);
    HeritageFixture disabled(512);
    HeritageFixture bypassed(512, &profile);
    disabled.load(sample);
    bypassed.load(sample);
    constexpr std::array blocks{std::size_t{17}, std::size_t{63},
                                std::size_t{128}, std::size_t{304}};
    REQUIRE(render(disabled, blocks) == render(bypassed, blocks));
    REQUIRE(bypassed.processor.latency_samples() == 0);
}

TEST_CASE("PulpSampler sizes streamed contracts in the heritage clock domain",
          "[audio][sampler][heritage][stream][capacity]") {
    HeritageTempWav source("clock_domain", 2000000);
    const auto profile = clock_profile(2.0);
    HeritageFixture clocked(64, &profile);
    REQUIRE(clocked.processor.load_sample_file(source.path));
    REQUIRE(PulpSamplerHeritageTestAccess::stream_output_sample_rate(
                clocked.processor) == 96000.0);
    REQUIRE(PulpSamplerHeritageTestAccess::maximum_stream_block_frames(
                clocked.processor) > 64);
    const auto contract = PulpSamplerHeritageTestAccess::preload_contract(
        clocked.processor);
    REQUIRE(contract.host_sample_rate == 96000.0);
    REQUIRE(contract.maximum_host_block_frames ==
            PulpSamplerHeritageTestAccess::maximum_stream_block_frames(
                clocked.processor));
    constexpr std::array block{std::size_t{64}};
    (void) render(clocked, block);
    const auto clocked_position =
        PulpSamplerHeritageTestAccess::active_streamed_position(
            clocked.processor);
    REQUIRE(clocked_position >= 127.0);
    REQUIRE(clocked_position <= 129.0);
    for (int callback = 0; callback < 16; ++callback)
        (void) render(clocked, block, 65);
    REQUIRE(PulpSamplerHeritageTestAccess::last_lookahead_demand_fps(
                clocked.processor) == 96000.0);

    const auto bypass = clock_profile(2.0, true);
    HeritageFixture neutral(64, &bypass);
    REQUIRE(PulpSamplerHeritageTestAccess::stream_output_sample_rate(
                neutral.processor) == 48000.0);
    REQUIRE(PulpSamplerHeritageTestAccess::maximum_stream_block_frames(
                neutral.processor) == 64);

    HeritageFixture reverse_clocked(64, &profile);
    reverse_clocked.store.set_value(kSamplerReverse, 1.0f);
    REQUIRE(reverse_clocked.processor.load_sample_file(source.path));
    REQUIRE(PulpSamplerHeritageTestAccess::retire_reverse_tail_page(
        reverse_clocked.processor));
    (void) render(reverse_clocked, block);
    REQUIRE(PulpSamplerHeritageTestAccess::last_stream_demand_fps(
                reverse_clocked.processor) == 96000.0);
}

TEST_CASE("PulpSampler transactionally rebinds loaded streams across clock changes",
          "[audio][sampler][heritage][stream][configuration]") {
    HeritageTempWav source("clock_rebind");
    const auto profile = clock_profile(2.0);
    HeritageFixture fixture(64, &profile);
    REQUIRE(fixture.processor.load_sample_file(source.path));
    REQUIRE(PulpSamplerHeritageTestAccess::has_retained_streamed_source(
        fixture.processor));
    const auto source_frames = fixture.processor.sample_length();
    const auto replacement = clock_profile(1.25);
    const auto before = fixture.processor.heritage_diagnostics();
    const auto latency_before = fixture.processor.latency_samples();

    PulpSamplerHeritageTestAccess::fail_next_stream_domain_prepare(
        fixture.processor);
    REQUIRE(fixture.processor.set_heritage_profile(replacement) ==
            PulpSamplerHeritageStatus::PrepareFailed);
    REQUIRE(fixture.processor.heritage_diagnostics().profile() ==
            profile.profile_id);
    REQUIRE(fixture.processor.sample_length() == source_frames);
    REQUIRE(PulpSamplerHeritageTestAccess::stream_output_sample_rate(
                fixture.processor) == 96000.0);
    constexpr std::array proof_block{std::size_t{64}};
    const auto after_prepare_failure = render(fixture, proof_block);
    REQUIRE(std::any_of(after_prepare_failure.begin(),
                        after_prepare_failure.end(), [](float value) {
        return std::abs(value) > 0.001f;
    }));

    PulpSamplerHeritageTestAccess::fail_next_stream_domain_source_restore(
        fixture.processor);
    REQUIRE(fixture.processor.set_heritage_profile(replacement) ==
            PulpSamplerHeritageStatus::PrepareFailed);
    REQUIRE(fixture.processor.heritage_diagnostics().profile() ==
            profile.profile_id);
    REQUIRE(fixture.processor.sample_length() == source_frames);
    REQUIRE(PulpSamplerHeritageTestAccess::stream_output_sample_rate(
                fixture.processor) == 96000.0);
    const auto after_restore_failure = render(fixture, proof_block, 65);
    REQUIRE(std::any_of(after_restore_failure.begin(),
                        after_restore_failure.end(), [](float value) {
        return std::abs(value) > 0.001f;
    }));

    REQUIRE(fixture.processor.set_heritage_profile(replacement) ==
            PulpSamplerHeritageStatus::Ready);
    REQUIRE(pulp_sampler_heritage_status_name(
                PulpSamplerHeritageStatus::StreamDomainRebindRequired) ==
            "stream-domain-rebind-required");
    REQUIRE(fixture.processor.has_sample());
    const auto after_replacement = fixture.processor.heritage_diagnostics();
    REQUIRE(after_replacement.profile() == replacement.profile_id);
    REQUIRE(after_replacement.clock_ratio == 1.25);
    REQUIRE(after_replacement.rate_admission_rejections ==
            before.rate_admission_rejections);
    REQUIRE(after_replacement.rate_automation_rejections ==
            before.rate_automation_rejections);
    REQUIRE(fixture.processor.latency_samples() != latency_before);
    REQUIRE(PulpSamplerHeritageTestAccess::stream_output_sample_rate(
                fixture.processor) == 60000.0);
    const auto contract = PulpSamplerHeritageTestAccess::preload_contract(
        fixture.processor);
    REQUIRE(contract.host_sample_rate == 60000.0);

    const auto same_clock = clock_output_profile(2.0, 0.5f);
    REQUIRE(fixture.processor.set_heritage_profile(same_clock) ==
            PulpSamplerHeritageStatus::Ready);
    REQUIRE(fixture.processor.has_sample());
    REQUIRE(fixture.processor.heritage_diagnostics().profile() ==
            same_clock.profile_id);
    REQUIRE(PulpSamplerHeritageTestAccess::stream_output_sample_rate(
                fixture.processor) == 96000.0);
    REQUIRE(fixture.processor.disable_heritage() ==
            PulpSamplerHeritageStatus::Disabled);
    REQUIRE(fixture.processor.has_sample());
    REQUIRE(PulpSamplerHeritageTestAccess::stream_output_sample_rate(
                fixture.processor) == 48000.0);
    constexpr std::array disabled_block{std::size_t{64}};
    REQUIRE(render(fixture, disabled_block).size() == 64);
}

TEST_CASE("PulpSampler rejects streamed pitch times heritage clock above four",
          "[audio][sampler][heritage][stream][admission]") {
    HeritageTempWav source("pitch_cap", 2000000);
    const auto profile = clock_profile(2.0);
    HeritageFixture fixture(64, &profile);
    fixture.store.set_value(kSamplerLoop, 1.0f);
    REQUIRE(fixture.processor.load_sample_file(source.path));
    constexpr std::array block{std::size_t{64}};

    (void) render(fixture, block, 0, 72);
    REQUIRE(PulpSamplerHeritageTestAccess::active_streamed_voices(
                fixture.processor) == 1);
    REQUIRE(fixture.processor.stream_stats()
                .invalid_preload_contract_events == 0);
    (void) render(fixture, block, 0, 73);
    REQUIRE(PulpSamplerHeritageTestAccess::active_streamed_voices(
                fixture.processor) == 1);
    const auto heritage = fixture.processor.heritage_diagnostics();
    REQUIRE(heritage.rate_admission_rejections == 1);
    REQUIRE(fixture.processor.stream_stats()
                .aggregate_rate_admission_rejections == 0);

    const auto slow_profile = clock_profile(0.5);
    HeritageFixture slow(64, &slow_profile);
    slow.store.set_value(kSamplerLoop, 1.0f);
    REQUIRE(slow.processor.load_sample_file(source.path));
    (void) render(slow, block, 0, 96);
    for (int callback = 0; callback < 16; ++callback)
        (void) render(slow, block, 65, 96);
    REQUIRE(PulpSamplerHeritageTestAccess::active_streamed_voices(
                slow.processor) == 1);
    REQUIRE(slow.processor.heritage_diagnostics()
                .rate_admission_rejections == 0);
    REQUIRE(slow.processor.stream_stats()
                .invalid_preload_contract_events == 0);
    REQUIRE(PulpSamplerHeritageTestAccess::last_lookahead_demand_fps(
                slow.processor) == 192000.0);
}

TEST_CASE("PulpSampler prepared state recall rebinds without dropping its source",
          "[audio][sampler][heritage][state][stream][configuration]") {
    HeritageTempWav source("state_domain_rebind");
    const auto initial_profile = clock_profile(2.0);
    HeritageFixture fixture(64, &initial_profile);
    REQUIRE(fixture.processor.load_sample_file(source.path));
    const auto source_frames = fixture.processor.sample_length();
    std::error_code remove_error;
    REQUIRE(std::filesystem::remove(source.path, remove_error));
    REQUIRE_FALSE(remove_error);
    REQUIRE(PulpSamplerHeritageTestAccess::has_retained_streamed_source(
        fixture.processor));

    const auto restored_profile = clock_profile(1.25);
    HeritageFixture saved(64, &restored_profile);
    const auto state = saved.processor.serialize_plugin_state();
    REQUIRE_FALSE(state.empty());
    REQUIRE(fixture.processor.deserialize_plugin_state(state));
    REQUIRE(fixture.processor.has_sample());
    REQUIRE(fixture.processor.sample_length() == source_frames);
    REQUIRE(fixture.processor.heritage_diagnostics().profile() ==
            restored_profile.profile_id);
    REQUIRE(PulpSamplerHeritageTestAccess::stream_output_sample_rate(
                fixture.processor) == 60000.0);
    constexpr std::array block{std::size_t{64}};
    const auto output = render(fixture, block);
    REQUIRE(std::any_of(output.begin(), output.end(), [](float value) {
        return std::abs(value) > 0.001f;
    }));

    REQUIRE(fixture.processor.deserialize_plugin_state({}));
    REQUIRE(fixture.processor.has_sample());
    REQUIRE(fixture.processor.sample_length() == source_frames);
    REQUIRE(fixture.processor.heritage_diagnostics().status ==
            PulpSamplerHeritageStatus::Disabled);
    REQUIRE(PulpSamplerHeritageTestAccess::stream_output_sample_rate(
                fixture.processor) == 48000.0);

    auto invalid = clock_profile(0.75);
    invalid.schema_version = audio::kSampleHeritageProfileSchemaVersion + 1;
    REQUIRE(fixture.processor.set_heritage_profile(invalid) ==
            PulpSamplerHeritageStatus::InvalidProfile);
    REQUIRE(fixture.processor.has_sample());
    REQUIRE(fixture.processor.sample_length() == source_frames);
    fixture.processor.release();
    REQUIRE_FALSE(PulpSamplerHeritageTestAccess::has_retained_streamed_source(
        fixture.processor));
}

TEST_CASE("PulpSampler multiplies streamed source throughput by heritage clock",
          "[audio][sampler][heritage][stream][admission]") {
    HeritageTempWav source("aggregate_clock");
    const auto profile = clock_profile(2.0);
    HeritageFixture fixture(64, &profile);
    fixture.store.set_value(kSamplerLoop, 1.0f);
    REQUIRE(fixture.processor.load_sample_file(source.path));
    PulpSamplerHeritageTestAccess::force_stream_rate_capacity(
        fixture.processor, 60000.0);
    constexpr std::array block{std::size_t{64}};
    (void) render(fixture, block);

    REQUIRE(PulpSamplerHeritageTestAccess::active_streamed_voices(
                fixture.processor) == 0);
    const auto heritage = fixture.processor.heritage_diagnostics();
    REQUIRE(heritage.rate_admission_rejections == 1);
    REQUIRE(fixture.processor.stream_stats()
                .aggregate_rate_admission_rejections == 0);

    const auto bypass = clock_profile(2.0, true);
    HeritageFixture neutral(64, &bypass);
    neutral.store.set_value(kSamplerLoop, 1.0f);
    REQUIRE(neutral.processor.load_sample_file(source.path));
    PulpSamplerHeritageTestAccess::force_stream_rate_capacity(
        neutral.processor, 60000.0);
    (void) render(neutral, block);
    REQUIRE(PulpSamplerHeritageTestAccess::active_streamed_voices(
                neutral.processor) == 1);
    REQUIRE(neutral.processor.heritage_diagnostics()
                .rate_admission_rejections == 0);
}

TEST_CASE("PulpSampler counts clock-driven streamed automation separately",
          "[audio][sampler][heritage][stream][automation]") {
    HeritageTempWav source("automation_clock");
    const auto profile = clock_profile(2.0);
    HeritageFixture fixture(64, &profile);
    fixture.store.set_value(kSamplerLoop, 1.0f);
    REQUIRE(fixture.processor.load_sample_file(source.path));
    constexpr std::array block{std::size_t{64}};
    (void) render(fixture, block);
    REQUIRE(PulpSamplerHeritageTestAccess::active_streamed_voices(
                fixture.processor) == 1);

    fixture.store.set_value(kSamplerPitch, 13.0f);
    (void) render(fixture, block, 65);
    REQUIRE(PulpSamplerHeritageTestAccess::active_streamed_voices(
                fixture.processor) == 0);
    const auto heritage = fixture.processor.heritage_diagnostics();
    REQUIRE(heritage.rate_automation_rejections == 1);
    REQUIRE(heritage.rate_admission_rejections == 0);
    REQUIRE(fixture.processor.stream_stats()
                .aggregate_rate_automation_rejections == 0);
}

TEST_CASE("PulpSampler pins heritage aggregate automation to the safe rate",
          "[audio][sampler][heritage][stream][automation][capacity]") {
    HeritageTempWav source("automation_aggregate_clock");
    const auto profile = clock_profile(2.0);
    HeritageFixture fixture(64, &profile);
    fixture.store.set_value(kSamplerLoop, 1.0f);
    REQUIRE(fixture.processor.load_sample_file(source.path));
    PulpSamplerHeritageTestAccess::force_stream_rate_capacity(
        fixture.processor, 210000.0);
    constexpr std::array attack{std::size_t{64}};
    (void) render(fixture, attack);
    (void) render(fixture, attack);
    for (int callback = 0; callback < 16; ++callback)
        (void) render(fixture, attack, 65);
    REQUIRE(PulpSamplerHeritageTestAccess::active_streamed_voices(
                fixture.processor) == 2);
    const auto before = PulpSamplerHeritageTestAccess::active_streamed_position(
        fixture.processor);

    // Pitch +5 remains below the effective pitch cap, but its clock-multiplied
    // source throughput exceeds the forced aggregate certificate.
    fixture.store.set_value(kSamplerPitch, 5.0f);
    constexpr std::array fade_head{std::size_t{16}};
    (void) render(fixture, fade_head, 17);
    const auto after = PulpSamplerHeritageTestAccess::active_streamed_position(
        fixture.processor);
    REQUIRE(after - before >= 31.0);
    REQUIRE(after - before <= 33.0);
    REQUIRE(fixture.processor.heritage_diagnostics()
                .rate_automation_rejections == 2);
    REQUIRE(fixture.processor.stream_stats()
                .aggregate_rate_automation_rejections == 0);
    REQUIRE(PulpSamplerHeritageTestAccess::last_lookahead_demand_fps(
                fixture.processor) == 96000.0);
    REQUIRE(PulpSamplerHeritageTestAccess::active_streamed_voices(
                fixture.processor) == 2);

    // A later note must count both still-reading fade voices at their pinned
    // rates. Skipping them would incorrectly admit this third voice.
    constexpr std::array fade_tail{std::size_t{16}};
    (void) render(fixture, fade_tail);
    REQUIRE(PulpSamplerHeritageTestAccess::active_streamed_voices(
                fixture.processor) == 0);
    REQUIRE(fixture.processor.heritage_diagnostics()
                .rate_admission_rejections == 1);
}

TEST_CASE("PulpSampler pins legacy aggregate automation to the safe rate",
          "[audio][sampler][heritage][stream][automation][capacity]") {
    HeritageTempWav source("automation_aggregate_legacy");
    const auto profile = clock_profile(2.0);
    HeritageFixture fixture(64, &profile);
    fixture.store.set_value(kSamplerLoop, 1.0f);
    REQUIRE(fixture.processor.load_sample_file(source.path));
    constexpr std::array attack{std::size_t{64}};
    (void) render(fixture, attack);
    for (int callback = 0; callback < 16; ++callback)
        (void) render(fixture, attack, 65);
    PulpSamplerHeritageTestAccess::force_stream_rate_capacity(
        fixture.processor, 60000.0);
    const auto before = PulpSamplerHeritageTestAccess::active_streamed_position(
        fixture.processor);

    fixture.store.set_value(kSamplerPitch, 5.0f);
    constexpr std::array fade_head{std::size_t{16}};
    (void) render(fixture, fade_head, 17);
    const auto after = PulpSamplerHeritageTestAccess::active_streamed_position(
        fixture.processor);
    REQUIRE(after - before >= 31.0);
    REQUIRE(after - before <= 33.0);
    REQUIRE(fixture.processor.heritage_diagnostics()
                .rate_automation_rejections == 0);
    REQUIRE(fixture.processor.stream_stats()
                .aggregate_rate_automation_rejections == 1);
    REQUIRE(PulpSamplerHeritageTestAccess::last_lookahead_demand_fps(
                fixture.processor) == 96000.0);
}

TEST_CASE("PulpSampler rejects heritage replacement without disturbing runtime",
          "[audio][sampler][heritage][configuration]") {
    const auto profile = clock_profile(2.0);
    auto sample = make_sine(48000);
    HeritageFixture fixture(64, &profile);
    fixture.load(sample);
    constexpr std::array attack{std::size_t{64}};
    (void) render(fixture, attack);
    (void) fixture.processor.consume_latency_changed_flag();

    auto invalid = clock_profile(1.25);
    invalid.schema_version = audio::kSampleHeritageProfileSchemaVersion + 1;
    REQUIRE(fixture.processor.set_heritage_profile(invalid) ==
            PulpSamplerHeritageStatus::InvalidProfile);
    REQUIRE(fixture.processor.latency_samples() == 12);
    REQUIRE_FALSE(fixture.processor.consume_latency_changed_flag());
    const auto diagnostics = fixture.processor.heritage_diagnostics();
    REQUIRE(diagnostics.status == PulpSamplerHeritageStatus::Ready);
    REQUIRE(diagnostics.profile() == profile.profile_id);

    constexpr std::array continuation{std::size_t{64}};
    const auto output = render(fixture, continuation, 65);
    REQUIRE(std::any_of(output.begin(), output.end(), [](float value) {
        return std::abs(value) > 0.01f;
    }));

    const auto replacement = clock_profile(1.25);
    REQUIRE(fixture.processor.set_heritage_profile(replacement) ==
            PulpSamplerHeritageStatus::Ready);
    REQUIRE(fixture.processor.has_sample());
    REQUIRE(fixture.processor.sample_length() ==
            static_cast<int>(sample.size()));
    REQUIRE(PulpSamplerHeritageTestAccess::stream_output_sample_rate(
                fixture.processor) == 60000.0);
    const auto rebound_output = render(fixture, continuation);
    REQUIRE(std::any_of(rebound_output.begin(), rebound_output.end(),
                        [](float value) {
                            return std::abs(value) > 0.01f;
                        }));
}

TEST_CASE("PulpSampler notifies host only when heritage latency changes",
          "[audio][sampler][heritage][latency]") {
    HeritageFixture fixture(64);
    REQUIRE_FALSE(fixture.processor.consume_latency_changed_flag());
    const auto active = clock_profile(2.0);
    REQUIRE(fixture.processor.set_heritage_profile(active) ==
            PulpSamplerHeritageStatus::Ready);
    REQUIRE(fixture.processor.consume_latency_changed_flag());
    REQUIRE(fixture.processor.latency_samples() == 12);

    REQUIRE(fixture.processor.set_heritage_profile(active) ==
            PulpSamplerHeritageStatus::Ready);
    REQUIRE_FALSE(fixture.processor.consume_latency_changed_flag());
    REQUIRE(fixture.processor.disable_heritage() ==
            PulpSamplerHeritageStatus::Disabled);
    REQUIRE(fixture.processor.consume_latency_changed_flag());
    REQUIRE(fixture.processor.latency_samples() == 0);
}

TEST_CASE("PulpSampler rejects heritage output above fixed channel capacity",
          "[audio][sampler][heritage][bounds]") {
    const auto profile = clock_profile(2.0);
    auto sample = make_sine(48000);
    HeritageFixture fixture(16, &profile);
    fixture.load(sample);
    std::array<std::array<float, 16>, 9> channels{};
    std::array<float*, 9> output_ptrs{};
    for (std::size_t channel = 0; channel < channels.size(); ++channel)
        output_ptrs[channel] = channels[channel].data();
    const float* input_ptrs[]{nullptr, nullptr};
    audio::BufferView<float> output(output_ptrs.data(), output_ptrs.size(), 16);
    audio::BufferView<const float> input(input_ptrs, 0, 16);
    midi::MidiBuffer midi_in;
    midi::MidiBuffer midi_out;
    midi_in.add(midi::MidiEvent::note_on(0, 60, 127));
    format::ProcessContext context{48000.0, 16};
    fixture.processor.process(output, input, midi_in, midi_out, context);
    for (const auto& channel : channels)
        REQUIRE(std::all_of(channel.begin(), channel.end(),
                            [](float value) { return value == 0.0f; }));
    const auto diagnostics = fixture.processor.heritage_diagnostics();
    REQUIRE(diagnostics.status == PulpSamplerHeritageStatus::RenderFailed);
    REQUIRE(diagnostics.render_failures == 1);
}

TEST_CASE("PulpSampler heritage render-plan failure latches silence",
          "[audio][sampler][heritage][failure]") {
    const auto profile = clock_profile(2.0);
    auto sample = make_sine(48000);
    HeritageFixture fixture(64, &profile);
    fixture.load(sample);
    PulpSamplerHeritageTestAccess::fail_next_plan(fixture.processor);
    constexpr std::array blocks{std::size_t{64}, std::size_t{64}};
    const auto output = render(fixture, blocks);
    REQUIRE(std::all_of(output.begin(), output.end(),
                        [](float sample_value) { return sample_value == 0.0f; }));
    const auto diagnostics = fixture.processor.heritage_diagnostics();
    REQUIRE(diagnostics.status == PulpSamplerHeritageStatus::RenderPlanFailed);
    REQUIRE(diagnostics.render_plan_failures == 1);
}

TEST_CASE("PulpSampler outer state round-trip resumes heritage RNG",
          "[audio][sampler][heritage][state]") {
    const auto profile = continued_noise_profile();
    HeritageFixture source(64, &profile);
    source.store.set_value(kSamplerGain, -9.0f);
    constexpr std::array block{std::size_t{64}};
    (void) render(source, block);

    const auto before = parse_sampler_heritage_state(
        source.processor.serialize_plugin_state());
    REQUIRE(before.valid());
    REQUIRE(before.state.has_runtime_state);
    REQUIRE(before.state.runtime_state.rng_state_count == 1);
    const auto saved_rng =
        before.state.runtime_state.rng_states[0].random_state;

    const auto envelope =
        format::plugin_state_io::serialize(source.store, source.processor);
    HeritageFixture restored(64);
    REQUIRE(format::plugin_state_io::deserialize(
        envelope, restored.store, restored.processor));
    REQUIRE(restored.store.get_value(kSamplerGain) == -9.0f);
    const auto diagnostics = restored.processor.heritage_diagnostics();
    REQUIRE(diagnostics.status == PulpSamplerHeritageStatus::Ready);
    REQUIRE(diagnostics.runtime_state_status ==
            audio::SampleHeritageRuntimeStateStatus::Ok);

    // The restored snapshot remains serializable before the first callback.
    const auto immediate = parse_sampler_heritage_state(
        restored.processor.serialize_plugin_state());
    REQUIRE(immediate.valid());
    REQUIRE(immediate.state.has_runtime_state);
    REQUIRE(immediate.state.runtime_state.rng_states[0].random_state ==
            saved_rng);

    // Callback-end publication atomically replaces it with the advanced RNG.
    (void) render(restored, block);
    const auto advanced = parse_sampler_heritage_state(
        restored.processor.serialize_plugin_state());
    REQUIRE(advanced.valid());
    REQUIRE(advanced.state.has_runtime_state);
    REQUIRE(advanced.state.runtime_state.rng_states[0].random_state !=
            saved_rng);
}

TEST_CASE("PulpSampler state restored before prepare reaches first callback",
          "[audio][sampler][heritage][state]") {
    const auto profile = continued_noise_profile();
    HeritageFixture source(64, &profile);
    constexpr std::array block{std::size_t{64}};
    (void) render(source, block);
    const auto saved = parse_sampler_heritage_state(
        source.processor.serialize_plugin_state());
    REQUIRE(saved.valid());
    REQUIRE(saved.state.has_runtime_state);

    state::StateStore store;
    PulpSamplerProcessor restored;
    restored.set_state_store(&store);
    restored.define_parameters(store);
    REQUIRE(restored.deserialize_plugin_state(
        source.processor.serialize_plugin_state()));
    format::PrepareContext context;
    context.sample_rate = 48000.0;
    context.max_buffer_size = 64;
    context.input_channels = 0;
    context.output_channels = 2;
    restored.prepare(context);

    const auto diagnostics = restored.heritage_diagnostics();
    REQUIRE(diagnostics.status == PulpSamplerHeritageStatus::Ready);
    REQUIRE(diagnostics.runtime_state_status ==
            audio::SampleHeritageRuntimeStateStatus::Ok);
    const auto immediate = parse_sampler_heritage_state(
        restored.serialize_plugin_state());
    REQUIRE(immediate.valid());
    REQUIRE(immediate.state.has_runtime_state);
    REQUIRE(immediate.state.runtime_state.rng_states[0].random_state ==
            saved.state.runtime_state.rng_states[0].random_state);
}

TEST_CASE("PulpSampler downstream prepare failure preserves pending RNG for retry",
          "[audio][sampler][heritage][state][failure]") {
    const auto profile = continued_noise_profile();
    HeritageFixture source(64, &profile);
    constexpr std::array block{std::size_t{64}};
    (void) render(source, block);
    const auto saved_bytes = source.processor.serialize_plugin_state();
    const auto saved = parse_sampler_heritage_state(saved_bytes);
    REQUIRE(saved.valid());
    REQUIRE(saved.state.has_runtime_state);

    auto initialize = [&](PulpSamplerProcessor& processor,
                          state::StateStore& store) {
        processor.set_state_store(&store);
        processor.define_parameters(store);
        REQUIRE(processor.deserialize_plugin_state(saved_bytes));
    };
    auto prepare = [](PulpSamplerProcessor& processor) {
        format::PrepareContext context;
        context.sample_rate = 48000.0;
        context.max_buffer_size = 64;
        context.input_channels = 0;
        context.output_channels = 2;
        processor.prepare(context);
    };
    auto advance = [](PulpSamplerProcessor& processor) {
        std::array<float, 64> left{}, right{};
        float* outputs[]{left.data(), right.data()};
        const float* inputs[]{nullptr, nullptr};
        audio::BufferView<float> output(outputs, 2, left.size());
        audio::BufferView<const float> input(inputs, 0, left.size());
        midi::MidiBuffer midi_in, midi_out;
        format::ProcessContext context{48000.0, 64};
        processor.process(output, input, midi_in, midi_out, context);
    };

    state::StateStore retry_store;
    PulpSamplerProcessor retry;
    initialize(retry, retry_store);
    PulpSamplerHeritageTestAccess::fail_next_stream_domain_prepare(retry);
    prepare(retry);
    REQUIRE(retry.prepare_result().status ==
            PulpSamplerPrepareStatus::AllocationFailure);
    auto after_failure = parse_sampler_heritage_state(
        retry.serialize_plugin_state());
    REQUIRE(after_failure.valid());
    REQUIRE(after_failure.state.has_runtime_state);
    REQUIRE(after_failure.state.runtime_state.rng_states[0].random_state ==
            saved.state.runtime_state.rng_states[0].random_state);

    prepare(retry);
    REQUIRE(retry.prepare_result().prepared());
    const auto immediate = parse_sampler_heritage_state(
        retry.serialize_plugin_state());
    REQUIRE(immediate.state.runtime_state.rng_states[0].random_state ==
            saved.state.runtime_state.rng_states[0].random_state);
    advance(retry);
    const auto retry_advanced = parse_sampler_heritage_state(
        retry.serialize_plugin_state());

    state::StateStore direct_store;
    PulpSamplerProcessor direct;
    initialize(direct, direct_store);
    prepare(direct);
    advance(direct);
    const auto direct_advanced = parse_sampler_heritage_state(
        direct.serialize_plugin_state());
    REQUIRE(retry_advanced.state.runtime_state.rng_states[0].random_state ==
            direct_advanced.state.runtime_state.rng_states[0].random_state);
    REQUIRE(retry_advanced.state.runtime_state.rng_states[0].random_state !=
            saved.state.runtime_state.rng_states[0].random_state);
}

TEST_CASE("PulpSampler outer state resets RNG when host rate changes",
          "[audio][sampler][heritage][state]") {
    const auto profile = continued_noise_profile();
    HeritageFixture source(64, &profile);
    constexpr std::array block{std::size_t{64}};
    (void) render(source, block);
    const auto envelope =
        format::plugin_state_io::serialize(source.store, source.processor);

    HeritageFixture restored(64, nullptr, 44100.0);
    REQUIRE(format::plugin_state_io::deserialize(
        envelope, restored.store, restored.processor));
    const auto diagnostics = restored.processor.heritage_diagnostics();
    REQUIRE(diagnostics.status ==
            PulpSamplerHeritageStatus::ReadyRuntimeResetForHostRate);
    REQUIRE(diagnostics.runtime_state_status ==
            audio::SampleHeritageRuntimeStateStatus::NotPrepared);

    const auto reset = parse_sampler_heritage_state(
        restored.processor.serialize_plugin_state());
    REQUIRE(reset.valid());
    REQUIRE(reset.state.enabled);
    REQUIRE_FALSE(reset.state.has_runtime_state);
    REQUIRE(reset.state.profile.host_sample_rate == 48000.0);

    // Once the 44.1 kHz runtime advances and is saved, another 44.1 kHz
    // restore resumes that execution state even though the profile was authored
    // at 48 kHz.
    (void) render(restored, block);
    const auto at_44100 = format::plugin_state_io::serialize(
        restored.store, restored.processor);
    const auto saved_at_44100 = parse_sampler_heritage_state(
        restored.processor.serialize_plugin_state());
    REQUIRE(saved_at_44100.valid());
    REQUIRE(saved_at_44100.state.has_runtime_state);
    REQUIRE(saved_at_44100.state.runtime_host_sample_rate == 44100.0);

    auto legacy_v1_at_44100 = restored.processor.serialize_plugin_state();
    legacy_v1_at_44100.erase(
        legacy_v1_at_44100.begin() + kSamplerHeritageStateV1HeaderBytes,
        legacy_v1_at_44100.begin() + kSamplerHeritageStateHeaderBytes);
    legacy_v1_at_44100[4] = 1;
    HeritageFixture legacy_same_rate(64, nullptr, 44100.0);
    REQUIRE(legacy_same_rate.processor.deserialize_plugin_state(
        legacy_v1_at_44100));
    REQUIRE(legacy_same_rate.processor.heritage_diagnostics().status ==
            PulpSamplerHeritageStatus::ReadyRuntimeResetForHostRate);
    REQUIRE(legacy_same_rate.processor.heritage_diagnostics()
                .runtime_state_status ==
            audio::SampleHeritageRuntimeStateStatus::NotPrepared);
    REQUIRE_FALSE(parse_sampler_heritage_state(
        legacy_same_rate.processor.serialize_plugin_state())
                      .state.has_runtime_state);

    HeritageFixture same_rate(64, nullptr, 44100.0);
    REQUIRE(format::plugin_state_io::deserialize(
        at_44100, same_rate.store, same_rate.processor));
    REQUIRE(same_rate.processor.heritage_diagnostics().status ==
            PulpSamplerHeritageStatus::Ready);
    REQUIRE(same_rate.processor.heritage_diagnostics().runtime_state_status ==
            audio::SampleHeritageRuntimeStateStatus::Ok);
    const auto resumed = parse_sampler_heritage_state(
        same_rate.processor.serialize_plugin_state());
    REQUIRE(resumed.valid());
    REQUIRE(resumed.state.has_runtime_state);
    REQUIRE(resumed.state.runtime_state.rng_states[0].random_state ==
            saved_at_44100.state.runtime_state.rng_states[0].random_state);

    HeritageFixture changed_again(64, nullptr, 48000.0);
    REQUIRE(format::plugin_state_io::deserialize(
        at_44100, changed_again.store, changed_again.processor));
    REQUIRE(changed_again.processor.heritage_diagnostics().status ==
            PulpSamplerHeritageStatus::ReadyRuntimeResetForHostRate);
    REQUIRE_FALSE(parse_sampler_heritage_state(
        changed_again.processor.serialize_plugin_state()).state.has_runtime_state);
}

TEST_CASE("Prepared PulpSampler heritage callbacks allocate nothing",
          "[audio][sampler][heritage][rt]") {
    const auto profile = clock_profile(1.25);
    auto sample = make_sine(48000);
    HeritageFixture fixture(16, &profile);
    fixture.store.set_value(kSamplerLoop, 1.0f);
    fixture.load(sample);
    constexpr std::array attack{std::size_t{16}};
    (void) render(fixture, attack);

    std::array<float, 16> left{};
    std::array<float, 16> right{};
    float* output_ptrs[]{left.data(), right.data()};
    const float* input_ptrs[]{nullptr, nullptr};
    audio::BufferView<float> output(output_ptrs, 2, left.size());
    audio::BufferView<const float> input(input_ptrs, 0, left.size());
    midi::MidiBuffer midi_in;
    midi::MidiBuffer midi_out;
    format::ProcessContext context{48000.0, static_cast<int>(left.size())};
    pulp::test::RtAllocationProbe probe;
    for (int callback = 0; callback < 10000; ++callback)
        fixture.processor.process(output, input, midi_in, midi_out, context);
    REQUIRE_FALSE(probe.saw_allocation());
}
