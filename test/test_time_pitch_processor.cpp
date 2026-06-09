#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/buffer.hpp>
#include <pulp/audio/signalsmith_time_pitch_processor.hpp>

#include <cmath>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

using pulp::audio::AnalyzerAvailability;
using pulp::audio::AnalyzerCapability;
using pulp::audio::Buffer;
using pulp::audio::BufferView;
using pulp::audio::SignalsmithTimePitchProcessor;
using pulp::audio::TimePitchPrepareConfig;
using pulp::audio::TimePitchPrepareStatus;
using pulp::audio::TimePitchProcessSpec;
using pulp::audio::TimePitchProcessStatus;
using pulp::audio::analyzer_descriptor_has_capability;
using pulp::audio::time_pitch_prepare_status_name;
using pulp::audio::time_pitch_process_status_name;

namespace {

BufferView<const float> const_view(const Buffer<float>& buffer,
                                   std::vector<const float*>& ptrs) {
    ptrs.resize(buffer.num_channels());
    for (std::size_t ch = 0; ch < buffer.num_channels(); ++ch) {
        ptrs[ch] = buffer.channel(ch).data();
    }
    return {ptrs.data(), buffer.num_channels(), buffer.num_samples()};
}

TimePitchPrepareConfig valid_prepare_config() {
    TimePitchPrepareConfig config;
    config.sample_rate = 48000.0;
    config.channels = 2;
    config.max_input_frames = 256;
    config.max_output_frames = 512;
    return config;
}

}  // namespace

TEST_CASE("SignalsmithTimePitchProcessor reports package availability",
          "[audio][time-pitch][packages]") {
    SignalsmithTimePitchProcessor processor;
    const auto& descriptor = processor.descriptor();

    REQUIRE(descriptor.id == "package.signalsmith-stretch.time-pitch");
    REQUIRE(descriptor.package_id == "signalsmith-stretch");
    REQUIRE(analyzer_descriptor_has_capability(descriptor, AnalyzerCapability::TimeStretch));
    REQUIRE(analyzer_descriptor_has_capability(descriptor, AnalyzerCapability::PitchShift));

#ifdef PULP_HAS_SIGNALSMITH_STRETCH
    REQUIRE(SignalsmithTimePitchProcessor::available());
    REQUIRE(descriptor.availability == AnalyzerAvailability::Available);
#else
    REQUIRE_FALSE(SignalsmithTimePitchProcessor::available());
    REQUIRE(descriptor.availability == AnalyzerAvailability::MissingPackage);
#endif
}

TEST_CASE("SignalsmithTimePitchProcessor unavailable path is explicit",
          "[audio][time-pitch][packages]") {
    SignalsmithTimePitchProcessor processor;
    Buffer<float> input(2, 64);
    Buffer<float> output(2, 128);
    std::vector<const float*> input_ptrs;

#ifndef PULP_HAS_SIGNALSMITH_STRETCH
    const auto prepared = processor.prepare(valid_prepare_config());
    REQUIRE_FALSE(prepared.ok);
    REQUIRE(prepared.status == TimePitchPrepareStatus::unavailable);
    REQUIRE(prepared.provenance.provider_id == processor.descriptor().id);
    const auto result = processor.process(const_view(input, input_ptrs),
                                          output.view(),
                                          TimePitchProcessSpec{});
    REQUIRE_FALSE(result.ok);
    REQUIRE(result.status == TimePitchProcessStatus::unavailable);
    REQUIRE(result.provenance.provider_id == processor.descriptor().id);
#else
    const auto prepared = processor.prepare(valid_prepare_config());
    REQUIRE(prepared.ok);
    REQUIRE(prepared.status == TimePitchPrepareStatus::ok);
    processor.release();
    const auto result = processor.process(const_view(input, input_ptrs),
                                          output.view(),
                                          TimePitchProcessSpec{});
    REQUIRE_FALSE(result.ok);
    REQUIRE(result.status == TimePitchProcessStatus::not_prepared);
#endif
}

TEST_CASE("SignalsmithTimePitchProcessor prepare is safe after move",
          "[audio][time-pitch][packages]") {
    SignalsmithTimePitchProcessor processor;
    SignalsmithTimePitchProcessor moved_to = std::move(processor);
    (void)moved_to;

    const auto prepared = processor.prepare(valid_prepare_config());
#ifdef PULP_HAS_SIGNALSMITH_STRETCH
    REQUIRE(prepared.ok);
    REQUIRE(prepared.status == TimePitchPrepareStatus::ok);
#else
    REQUIRE_FALSE(prepared.ok);
    REQUIRE(prepared.status == TimePitchPrepareStatus::unavailable);
#endif
}

TEST_CASE("SignalsmithTimePitchProcessor validates prepare and process contracts",
          "[audio][time-pitch][packages]") {
    SignalsmithTimePitchProcessor processor;

    auto bad_config = valid_prepare_config();
    bad_config.channels = 0;
    const auto invalid_prepare = processor.prepare(bad_config);
    REQUIRE_FALSE(invalid_prepare.ok);
    REQUIRE(invalid_prepare.status == TimePitchPrepareStatus::invalid_config);

    REQUIRE(std::string_view(time_pitch_prepare_status_name(
                TimePitchPrepareStatus::allocation_failed)) ==
            "allocation-failed");
    REQUIRE(std::string_view(time_pitch_process_status_name(
                TimePitchProcessStatus::frame_budget_exceeded)) ==
            "frame-budget-exceeded");

#ifdef PULP_HAS_SIGNALSMITH_STRETCH
    const auto prepared = processor.prepare(valid_prepare_config());
    REQUIRE(prepared.ok);

    Buffer<float> input(1, 64);
    Buffer<float> output(2, 128);
    std::vector<const float*> input_ptrs;
    auto mismatch = processor.process(const_view(input, input_ptrs),
                                      output.view(),
                                      TimePitchProcessSpec{});
    REQUIRE_FALSE(mismatch.ok);
    REQUIRE(mismatch.status == TimePitchProcessStatus::channel_mismatch);

    Buffer<float> stereo_input(2, 64);
    for (std::size_t i = 0; i < stereo_input.num_samples(); ++i) {
        stereo_input.channel(0)[i] = i % 8 == 0 ? 1.0f : 0.0f;
        stereo_input.channel(1)[i] = i % 11 == 0 ? 0.5f : 0.0f;
    }
    Buffer<float> stereo_output(2, 128);
    std::vector<const float*> stereo_ptrs;
    TimePitchProcessSpec spec;
    spec.source_sample_rate = 48000.0;
    spec.time_ratio = 2.0;
    spec.pitch_shift_semitones = 7.0;
    const auto rendered = processor.process(const_view(stereo_input, stereo_ptrs),
                                            stereo_output.view(),
                                            spec);
    REQUIRE(rendered.ok);
    REQUIRE(rendered.status == TimePitchProcessStatus::ok);
    REQUIRE(rendered.input_frames_consumed == 64);
    REQUIRE(rendered.output_frames_produced == 128);

    float absolute_sum = 0.0f;
    for (std::size_t ch = 0; ch < stereo_output.num_channels(); ++ch) {
        for (std::size_t i = 0; i < rendered.output_frames_produced; ++i) {
            REQUIRE(std::isfinite(stereo_output.channel(ch)[i]));
            absolute_sum += std::abs(stereo_output.channel(ch)[i]);
        }
    }
    REQUIRE(absolute_sum > 0.0f);
#endif
}
