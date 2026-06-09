#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/audio_stream_handoff.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/audio/planar_audio_ring_buffer.hpp>
#include <pulp/audio/published_sample_store.hpp>
#include <pulp/audio/realtime_sample_recorder.hpp>
#include <pulp/audio/rolling_audio_capture_buffer.hpp>
#include <pulp/audio/sample_slot_bank.hpp>
#include <pulp/audio/sample_slot_materializer.hpp>
#include <pulp/runtime/scoped_no_alloc.hpp>

#include <cstdint>
#include <cmath>
#include <utility>
#include <vector>

using pulp::audio::AudioStreamHandoff;
using pulp::audio::AudioStreamHandoffConfig;
using pulp::audio::AudioStreamHandoffPullResult;
using pulp::audio::Buffer;
using pulp::audio::BufferView;
using pulp::audio::PlanarAudioRingBuffer;
using pulp::audio::PublishedSampleView;
using pulp::audio::PublishedSampleStore;
using pulp::audio::PublishedSampleStoreConfig;
using pulp::audio::RealtimeSampleRecorder;
using pulp::audio::RealtimeSampleRecorderCommand;
using pulp::audio::RealtimeSampleRecorderCommandType;
using pulp::audio::RealtimeSampleRecorderConfig;
using pulp::audio::RealtimeSampleRecorderEvent;
using pulp::audio::RealtimeSampleRecorderEventType;
using pulp::audio::RealtimeSampleRecorderState;
using pulp::audio::RealtimeSampleRecorderTimingType;
using pulp::audio::RollingAudioCaptureBuffer;
using pulp::audio::RollingAudioCaptureBufferConfig;
using pulp::audio::RollingAudioCaptureMaterializeStatus;
using pulp::audio::SampleSlotBank;
using pulp::audio::SampleSlotState;
using pulp::audio::materialize_completed_recording_to_slot;
using pulp::audio::publish_completed_recording_to_slot;

namespace {

BufferView<const float> const_view(const Buffer<float>& buffer,
                                   std::vector<const float*>& ptrs) {
    ptrs.resize(buffer.num_channels());
    for (std::size_t ch = 0; ch < buffer.num_channels(); ++ch) {
        ptrs[ch] = buffer.channel(ch).data();
    }
    return {ptrs.data(), buffer.num_channels(), buffer.num_samples()};
}

void fill_sequence(Buffer<float>& buffer, float first) {
    for (std::size_t ch = 0; ch < buffer.num_channels(); ++ch) {
        for (std::size_t i = 0; i < buffer.num_samples(); ++i) {
            buffer.channel(ch)[i] = first + static_cast<float>(i) +
                                    static_cast<float>(ch) * 1000.0f;
        }
    }
}

}  // namespace

TEST_CASE("RollingAudioCaptureBuffer enforces long-duration byte budgets",
          "[audio][sampler][capture]") {
    const auto frames_60s = static_cast<std::uint64_t>(48000 * 60);
    const auto bytes =
        RollingAudioCaptureBuffer::estimate_bytes(2, frames_60s, sizeof(float));
    REQUIRE(bytes == 23040000);

    RollingAudioCaptureBuffer capture;
    const auto prepared = capture.prepare_seconds(2, 48000.0, 60.0, bytes);
    REQUIRE(prepared.ok);
    REQUIRE(prepared.allocated_frames == frames_60s);
    REQUIRE(prepared.allocated_bytes == bytes);
    REQUIRE(capture.capacity_frames() == frames_60s);

    const auto rejected_reprepare = capture.prepare_seconds(2, 48000.0, 60.0, bytes - 1);
    REQUIRE_FALSE(rejected_reprepare.ok);
    REQUIRE(capture.capacity_frames() == 0);

    RollingAudioCaptureBuffer too_small;
    const auto rejected = too_small.prepare_seconds(2, 48000.0, 60.0, bytes - 1);
    REQUIRE_FALSE(rejected.ok);
    REQUIRE(too_small.capacity_frames() == 0);
}

TEST_CASE("RollingAudioCaptureBuffer snapshots and materializes wrapped audio",
          "[audio][sampler][capture]") {
    RollingAudioCaptureBuffer capture;
    REQUIRE(capture.prepare(RollingAudioCaptureBufferConfig{2, 5, sizeof(float)}));

    Buffer<float> first(2, 4);
    fill_sequence(first, 1.0f);
    std::vector<const float*> first_ptrs;
    capture.append(const_view(first, first_ptrs), 4);

    Buffer<float> second(2, 4);
    fill_sequence(second, 100.0f);
    std::vector<const float*> second_ptrs;
    capture.append(const_view(second, second_ptrs), 4);

    const auto snapshot = capture.snapshot_last(5);
    REQUIRE(snapshot.valid);
    REQUIRE(snapshot.frame_count == 5);
    REQUIRE(snapshot.start_frame == 3);

    Buffer<float> out(3, 5);
    const auto result = capture.materialize_quiescent(snapshot, out.view());
    REQUIRE(result.status == RollingAudioCaptureMaterializeStatus::Ok);
    REQUIRE(result.frames_copied == 5);
    REQUIRE(out.channel(0)[0] == 4.0f);
    REQUIRE(out.channel(0)[1] == 100.0f);
    REQUIRE(out.channel(0)[4] == 103.0f);
    REQUIRE(out.channel(1)[0] == 1004.0f);
    REQUIRE(out.channel(1)[4] == 1103.0f);
    REQUIRE(out.channel(2)[0] == 0.0f);
}

TEST_CASE("RollingAudioCaptureBuffer reports stale snapshots as overwritten",
          "[audio][sampler][capture]") {
    RollingAudioCaptureBuffer capture;
    REQUIRE(capture.prepare(RollingAudioCaptureBufferConfig{1, 4, sizeof(float)}));

    Buffer<float> first(1, 4);
    fill_sequence(first, 1.0f);
    std::vector<const float*> first_ptrs;
    capture.append(const_view(first, first_ptrs), 4);
    const auto stale = capture.snapshot_last(4);

    Buffer<float> second(1, 5);
    fill_sequence(second, 100.0f);
    std::vector<const float*> second_ptrs;
    capture.append(const_view(second, second_ptrs), 5);

    Buffer<float> out(1, 4);
    const auto result = capture.materialize_quiescent(stale, out.view());
    REQUIRE(result.status == RollingAudioCaptureMaterializeStatus::Overwritten);
    REQUIRE(result.frames_copied == 0);

    const auto reset_stale = capture.snapshot_last(2);
    REQUIRE(reset_stale.valid);
    capture.reset();
    Buffer<float> third(1, 4);
    fill_sequence(third, 200.0f);
    std::vector<const float*> third_ptrs;
    capture.append(const_view(third, third_ptrs), 4);
    const auto reset_result = capture.materialize_quiescent(reset_stale, out.view());
    REQUIRE(reset_result.status != RollingAudioCaptureMaterializeStatus::Ok);
    REQUIRE(reset_result.frames_copied == 0);
}

TEST_CASE("RollingAudioCaptureBuffer holds snapshots for live materialization",
          "[audio][sampler][capture]") {
    RollingAudioCaptureBuffer capture;
    REQUIRE(capture.prepare(RollingAudioCaptureBufferConfig{1, 6, sizeof(float)}));

    Buffer<float> first(1, 6);
    fill_sequence(first, 1.0f);
    std::vector<const float*> first_ptrs;
    capture.append(const_view(first, first_ptrs), 6);

    const auto held = capture.begin_hold_last(4);
    REQUIRE(held.valid);
    REQUIRE(capture.hold_active());
    REQUIRE(capture.frames_discarded_while_held() == 0);

    Buffer<float> ignored(1, 6);
    fill_sequence(ignored, 100.0f);
    std::vector<const float*> ignored_ptrs;
    capture.append(const_view(ignored, ignored_ptrs), 6);
    REQUIRE(capture.frames_discarded_while_held() == 6);

    Buffer<float> out(1, 4);
    const auto held_result = capture.materialize_held(held, out.view());
    REQUIRE(held_result.status == RollingAudioCaptureMaterializeStatus::Ok);
    REQUIRE(held_result.frames_copied == 4);
    REQUIRE(out.channel(0)[0] == 3.0f);
    REQUIRE(out.channel(0)[3] == 6.0f);

    capture.end_hold();
    REQUIRE_FALSE(capture.hold_active());
    const auto released_result = capture.materialize_held(held, out.view());
    REQUIRE(released_result.status == RollingAudioCaptureMaterializeStatus::Overwritten);

    capture.append(const_view(ignored, ignored_ptrs), 6);
    REQUIRE(capture.frames_discarded_while_held() == 6);

    const auto resumed = capture.snapshot_last(4);
    REQUIRE(resumed.valid);
    const auto resumed_result = capture.materialize_quiescent(resumed, out.view());
    REQUIRE(resumed_result.status == RollingAudioCaptureMaterializeStatus::Ok);
    REQUIRE(out.channel(0)[0] == 102.0f);
    REQUIRE(out.channel(0)[3] == 105.0f);

    const auto newer_hold = capture.begin_hold_last(2);
    REQUIRE(newer_hold.valid);
    const auto superseded_result = capture.materialize_held(held, out.view());
    REQUIRE(superseded_result.status == RollingAudioCaptureMaterializeStatus::Overwritten);
    capture.end_hold();
    REQUIRE_FALSE(capture.hold_active());
    REQUIRE(capture.frames_discarded_while_held() == 6);
    capture.reset();
    REQUIRE(capture.frames_discarded_while_held() == 0);
}

TEST_CASE("RollingAudioCaptureBuffer hold tokens release the active hold",
          "[audio][sampler][capture]") {
    RollingAudioCaptureBuffer capture;
    REQUIRE(capture.prepare(RollingAudioCaptureBufferConfig{1, 6, sizeof(float)}));

    Buffer<float> source(1, 6);
    fill_sequence(source, 1.0f);
    std::vector<const float*> source_ptrs;
    capture.append(const_view(source, source_ptrs), 6);

    Buffer<float> out(1, 4);
    {
        auto hold = capture.hold_last(4);
        REQUIRE(hold.valid());
        REQUIRE(capture.hold_active());

        Buffer<float> ignored(1, 6);
        fill_sequence(ignored, 100.0f);
        std::vector<const float*> ignored_ptrs;
        capture.append(const_view(ignored, ignored_ptrs), 6);

        const auto held_result = capture.materialize_held(hold, out.view());
        REQUIRE(held_result.status == RollingAudioCaptureMaterializeStatus::Ok);
        REQUIRE(held_result.frames_copied == 4);
        REQUIRE(out.channel(0)[0] == 3.0f);
        REQUIRE(out.channel(0)[3] == 6.0f);

        auto moved = std::move(hold);
        REQUIRE_FALSE(hold.valid());
        REQUIRE(moved.valid());
        REQUIRE(capture.hold_active());
    }
    REQUIRE_FALSE(capture.hold_active());

    Buffer<float> resumed(1, 6);
    fill_sequence(resumed, 200.0f);
    std::vector<const float*> resumed_ptrs;
    capture.append(const_view(resumed, resumed_ptrs), 6);
    const auto snapshot = capture.snapshot_last(2);
    REQUIRE(snapshot.valid);
    const auto resumed_result = capture.materialize_quiescent(snapshot, out.view());
    REQUIRE(resumed_result.status == RollingAudioCaptureMaterializeStatus::Ok);
    REQUIRE(out.channel(0)[0] == 204.0f);
    REQUIRE(out.channel(0)[1] == 205.0f);
}

TEST_CASE("RollingAudioCaptureBuffer hold tokens do not release newer holds",
          "[audio][sampler][capture]") {
    RollingAudioCaptureBuffer capture;
    REQUIRE(capture.prepare(RollingAudioCaptureBufferConfig{1, 8, sizeof(float)}));

    Buffer<float> first(1, 8);
    fill_sequence(first, 1.0f);
    std::vector<const float*> first_ptrs;
    capture.append(const_view(first, first_ptrs), 8);

    auto old_hold = capture.hold_last(4);
    REQUIRE(old_hold.valid());

    auto newer_hold = capture.hold_last(2);
    REQUIRE(newer_hold.valid());
    REQUIRE(capture.hold_active());

    old_hold.release();
    REQUIRE(capture.hold_active());
    newer_hold.release();
    REQUIRE_FALSE(capture.hold_active());

    auto first_identical = capture.hold_last(4);
    REQUIRE(first_identical.valid());
    auto second_identical = capture.hold_last(4);
    REQUIRE(second_identical.valid());
    first_identical.release();
    REQUIRE(capture.hold_active());
    second_identical.release();
    REQUIRE_FALSE(capture.hold_active());
}

TEST_CASE("RollingAudioCaptureBuffer zero-channel appends do not advance snapshots",
          "[audio][sampler][capture]") {
    RollingAudioCaptureBuffer capture;
    REQUIRE(capture.prepare(RollingAudioCaptureBufferConfig{1, 4, sizeof(float)}));

    BufferView<const float> no_source(nullptr, 0, 4);
    capture.append(no_source, 4);
    REQUIRE_FALSE(capture.snapshot_last(4).valid);

    Buffer<float> source(1, 2);
    fill_sequence(source, 1.0f);
    std::vector<const float*> source_ptrs;
    capture.append(const_view(source, source_ptrs), 2);
    const auto snapshot = capture.snapshot_last(2);
    REQUIRE(snapshot.valid);
    REQUIRE(snapshot.frame_count == 2);
}
