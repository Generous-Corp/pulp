#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/audio_stream_handoff.hpp>
#include <pulp/audio/waveform_overview.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/audio/loop_point_analyzer.hpp>
#include <pulp/audio/loop_renderer.hpp>
#include <pulp/audio/realtime_sample_recorder.hpp>
#include <pulp/audio/rolling_audio_capture_buffer.hpp>
#include <pulp/audio/sampler_looper_metrics.hpp>
#include <pulp/audio/sample_slot_bank.hpp>
#include <pulp/audio/sample_slot_materializer.hpp>
#include <pulp/runtime/scoped_no_alloc.hpp>

#include <cmath>
#include <cstdint>
#include <vector>

using pulp::audio::AudioStreamHandoff;
using pulp::audio::AudioStreamHandoffConfig;
using pulp::audio::Buffer;
using pulp::audio::BufferView;
using pulp::audio::LoopPointAnalysisConfig;
using pulp::audio::LoopPointAnalyzer;
using pulp::audio::LoopRenderResult;
using pulp::audio::LoopRenderer;
using pulp::audio::RealtimeSampleRecorder;
using pulp::audio::RealtimeSampleRecorderConfig;
using pulp::audio::RollingAudioCaptureBuffer;
using pulp::audio::RollingAudioCaptureBufferConfig;
using pulp::audio::SampleSlotBank;
using pulp::audio::SampleSlotState;
using pulp::audio::publish_completed_recording_to_slot;

namespace {

constexpr double kPi = 3.14159265358979323846;

BufferView<const float> const_view(const Buffer<float>& buffer,
                                   std::vector<const float*>& ptrs) {
    ptrs.resize(buffer.num_channels());
    for (std::size_t ch = 0; ch < buffer.num_channels(); ++ch) {
        ptrs[ch] = buffer.channel(ch).data();
    }
    return {ptrs.data(), buffer.num_channels(), buffer.num_samples()};
}

void fill_periodic_generated_source(Buffer<float>& buffer, std::uint64_t period_frames) {
    for (std::size_t i = 0; i < buffer.num_samples(); ++i) {
        const auto phase =
            2.0 * kPi * static_cast<double>(i % period_frames) /
            static_cast<double>(period_frames);
        buffer.channel(0)[i] = static_cast<float>(std::sin(phase));
    }
}

}  // namespace

TEST_CASE("Sampler looper primitives integrate without provider dependencies",
          "[audio][sampler][looper][integration]") {
    constexpr std::uint64_t kFrames = 128;
    constexpr std::uint64_t kLoopFrames = 64;

    AudioStreamHandoff handoff;
    REQUIRE(handoff.prepare(AudioStreamHandoffConfig{
        1, 1, 48000.0, 48000.0, 256, 128, 128}));

    RollingAudioCaptureBuffer capture;
    REQUIRE(capture.prepare(RollingAudioCaptureBufferConfig{1, 256, sizeof(float)}));

    Buffer<float> generated(1, kFrames);
    fill_periodic_generated_source(generated, kLoopFrames);
    std::vector<const float*> generated_ptrs;
    REQUIRE(handoff.push(const_view(generated, generated_ptrs), kFrames) == kFrames);

    Buffer<float> host_block(1, kFrames);
    {
        pulp::runtime::ScopedNoAlloc guard;
        const auto pulled = handoff.pull(host_block.view(), kFrames);
        REQUIRE(pulled.rendered_frames == kFrames);
        REQUIRE_FALSE(pulled.underrun);
        capture.append(host_block.view(), kFrames);
    }

    const auto snapshot = capture.snapshot_last(kFrames);
    REQUIRE(snapshot.valid);

    RealtimeSampleRecorder recorder;
    REQUIRE(recorder.prepare(RealtimeSampleRecorderConfig{1, kFrames, 48000.0}));
    REQUIRE(recorder.materialize_snapshot_quiescent(capture, snapshot));

    SampleSlotBank slots;
    REQUIRE(slots.prepare(1, 1, kFrames));
    std::vector<const float*> recorder_scratch(recorder.num_channels());
    REQUIRE(publish_completed_recording_to_slot(recorder, slots, recorder_scratch));
    REQUIRE(slots.slot_state(0) == SampleSlotState::Published);
    const auto published = slots.read_published_view();
    REQUIRE(published.valid);
    REQUIRE(published.num_frames == kFrames);

    std::vector<const float*> slot_ptrs(published.num_channels);
    for (std::uint32_t ch = 0; ch < published.num_channels; ++ch) {
        slot_ptrs[ch] = slots.channel_data(published, ch);
    }
    BufferView<const float> published_audio(slot_ptrs.data(),
                                            published.num_channels,
                                            static_cast<std::size_t>(published.num_frames));

    const auto thumbnail = pulp::audio::WaveformOverview::build_from_buffer_view(
        published_audio, static_cast<std::uint32_t>(published.sample_rate), 16);
    REQUIRE_FALSE(thumbnail.empty());
    const auto thumbnail_info = thumbnail.info();
    REQUIRE(thumbnail_info.num_source_frames == published.num_frames);
    REQUIRE(thumbnail_info.num_channels == published.num_channels);
    std::vector<float> peak_pairs(32);
    REQUIRE(thumbnail.render_min_max(pulp::audio::WaveformOverview::kAllChannels,
                                     16,
                                     peak_pairs.data()) == 16);

    LoopPointAnalysisConfig loop_config;
    loop_config.start_hint_frame = 0;
    loop_config.end_hint_frame = kLoopFrames;
    loop_config.search_radius_frames = 2;
    loop_config.window_frames = 32;
    loop_config.min_loop_frames = 32;
    loop_config.source_sample_rate = 48000.0;

    LoopPointAnalyzer analyzer;
    const auto loops = analyzer.analyze(published_audio, loop_config);
    REQUIRE(loops.ok);
    REQUIRE_FALSE(loops.candidates.empty());
    auto loop = loops.candidates.front().region;
    REQUIRE(loop.end_frame - loop.start_frame == kLoopFrames);

    Buffer<float> rendered(1, kLoopFrames + 16);
    LoopRenderer renderer;
    REQUIRE(renderer.set_region(loop, published.num_frames));
    renderer.start();
    LoopRenderResult render_result;
    {
        pulp::runtime::ScopedNoAlloc guard;
        render_result = renderer.render(published_audio,
                                        rendered.view(),
                                        rendered.num_samples());
        REQUIRE(render_result.rendered_frames == rendered.num_samples());
        REQUIRE(render_result.wrapped);
        REQUIRE(render_result.max_sample_delta < 0.25f);
    }

    const auto metrics = pulp::audio::collect_sampler_looper_metrics(
        &handoff, &capture, &recorder, &slots, &render_result);
    REQUIRE(metrics.stream_underrun_frames == 0);
    REQUIRE(metrics.capture_allocated_bytes ==
            RollingAudioCaptureBuffer::estimate_bytes(1, 256));
    REQUIRE(metrics.recorder_state == pulp::audio::RealtimeSampleRecorderState::Idle);
    REQUIRE(metrics.published_sample_valid);
    REQUIRE(metrics.active_generation == published.generation);
    REQUIRE(metrics.loop_wrapped);
    REQUIRE(metrics.loop_max_sample_delta < 0.25f);
}
