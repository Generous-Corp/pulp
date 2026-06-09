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

TEST_CASE("RealtimeSampleRecorder captures a block-offset span",
          "[audio][sampler][recorder]") {
    RealtimeSampleRecorder recorder;
    REQUIRE(recorder.prepare(RealtimeSampleRecorderConfig{2, 16, 48000.0}));

    RealtimeSampleRecorderCommand start;
    start.sequence_id = 1;
    start.type = RealtimeSampleRecorderCommandType::Start;
    start.timing.type = RealtimeSampleRecorderTimingType::BlockOffset;
    start.timing.frame_offset = 2;
    RealtimeSampleRecorderCommand stop;
    stop.sequence_id = 2;
    stop.type = RealtimeSampleRecorderCommandType::Stop;
    stop.timing.type = RealtimeSampleRecorderTimingType::BlockOffset;
    stop.timing.frame_offset = 6;
    REQUIRE(recorder.enqueue_command(start));
    REQUIRE(recorder.enqueue_command(stop));

    Buffer<float> source(2, 8);
    fill_sequence(source, 1.0f);
    std::vector<const float*> source_ptrs;
    {
        pulp::runtime::ScopedNoAlloc guard;
        recorder.process(const_view(source, source_ptrs), 8);
    }

    REQUIRE(recorder.state() == RealtimeSampleRecorderState::Completed);
    REQUIRE(recorder.frames_recorded() == 4);
    REQUIRE(recorder.channel_data(0)[0] == 3.0f);
    REQUIRE(recorder.channel_data(0)[3] == 6.0f);
    REQUIRE(recorder.channel_data(1)[0] == 1003.0f);

    RealtimeSampleRecorderEvent event;
    REQUIRE(recorder.pop_event(event));
    REQUIRE(event.type == RealtimeSampleRecorderEventType::Started);
    REQUIRE(event.sequence_id == 1);
    REQUIRE(event.block_offset == 2);
    REQUIRE(recorder.pop_event(event));
    REQUIRE(event.type == RealtimeSampleRecorderEventType::Completed);
    REQUIRE(event.sequence_id == 2);
    REQUIRE(event.frames_recorded == 4);
    REQUIRE(event.block_offset == 6);
}

TEST_CASE("RealtimeSampleRecorder completes at its fixed frame target",
          "[audio][sampler][recorder]") {
    RealtimeSampleRecorder recorder;
    REQUIRE(recorder.prepare(RealtimeSampleRecorderConfig{1, 16, 48000.0}));

    RealtimeSampleRecorderCommand start;
    start.sequence_id = 10;
    start.type = RealtimeSampleRecorderCommandType::Start;
    start.max_frames = 3;
    REQUIRE(recorder.enqueue_command(start));

    Buffer<float> source(1, 8);
    fill_sequence(source, 10.0f);
    std::vector<const float*> source_ptrs;
    recorder.process(const_view(source, source_ptrs), 8);

    REQUIRE(recorder.state() == RealtimeSampleRecorderState::Completed);
    REQUIRE(recorder.frames_recorded() == 3);
    REQUIRE(recorder.channel_data(0)[0] == 10.0f);
    REQUIRE(recorder.channel_data(0)[2] == 12.0f);
    REQUIRE(recorder.stats().total_recorded_frames == 3);

    RealtimeSampleRecorderEvent event;
    REQUIRE(recorder.pop_event(event));
    REQUIRE(event.sequence_id == 10);
    REQUIRE(event.type == RealtimeSampleRecorderEventType::Started);
    REQUIRE(recorder.pop_event(event));
    REQUIRE(event.sequence_id == 10);
    REQUIRE(event.type == RealtimeSampleRecorderEventType::Completed);
}

TEST_CASE("RealtimeSampleRecorder starts with armed settings by default",
          "[audio][sampler][recorder]") {
    RealtimeSampleRecorder recorder;
    REQUIRE(recorder.prepare(RealtimeSampleRecorderConfig{1, 16, 48000.0}));

    RealtimeSampleRecorderCommand arm;
    arm.sequence_id = 20;
    arm.type = RealtimeSampleRecorderCommandType::Arm;
    arm.max_frames = 8;
    arm.cancel_on_transport_jump = false;
    RealtimeSampleRecorderCommand start;
    start.sequence_id = 21;
    start.type = RealtimeSampleRecorderCommandType::Start;
    REQUIRE(recorder.enqueue_command(arm));
    REQUIRE(recorder.enqueue_command(start));

    Buffer<float> source(1, 4);
    fill_sequence(source, 10.0f);
    std::vector<const float*> source_ptrs;
    {
        pulp::runtime::ScopedNoAlloc guard;
        recorder.process(const_view(source, source_ptrs), 4);
    }
    REQUIRE(recorder.state() == RealtimeSampleRecorderState::Recording);
    REQUIRE(recorder.frames_recorded() == 4);

    {
        pulp::runtime::ScopedNoAlloc guard;
        recorder.process(const_view(source, source_ptrs), 4, true);
    }
    REQUIRE(recorder.state() == RealtimeSampleRecorderState::Completed);
    REQUIRE(recorder.frames_recorded() == 8);

    RealtimeSampleRecorderEvent event;
    REQUIRE(recorder.pop_event(event));
    REQUIRE(event.sequence_id == 20);
    REQUIRE(event.type == RealtimeSampleRecorderEventType::CommandAccepted);
    REQUIRE(recorder.pop_event(event));
    REQUIRE(event.sequence_id == 21);
    REQUIRE(event.type == RealtimeSampleRecorderEventType::Started);
    REQUIRE(recorder.pop_event(event));
    REQUIRE(event.sequence_id == 21);
    REQUIRE(event.type == RealtimeSampleRecorderEventType::Completed);
}

TEST_CASE("RealtimeSampleRecorder cancels armed or active capture on transport jump",
          "[audio][sampler][recorder]") {
    RealtimeSampleRecorder recorder;
    REQUIRE(recorder.prepare(RealtimeSampleRecorderConfig{1, 16, 48000.0}));

    RealtimeSampleRecorderCommand start;
    start.sequence_id = 11;
    start.type = RealtimeSampleRecorderCommandType::Start;
    start.cancel_on_transport_jump = true;
    REQUIRE(recorder.enqueue_command(start));

    Buffer<float> source(1, 4);
    fill_sequence(source, 1.0f);
    std::vector<const float*> source_ptrs;
    recorder.process(const_view(source, source_ptrs), 4);
    REQUIRE(recorder.state() == RealtimeSampleRecorderState::Recording);
    REQUIRE(recorder.frames_recorded() == 4);

    recorder.process(const_view(source, source_ptrs), 4, true);
    REQUIRE(recorder.state() == RealtimeSampleRecorderState::Cancelled);

    RealtimeSampleRecorderEvent event;
    REQUIRE(recorder.pop_event(event));
    REQUIRE(event.type == RealtimeSampleRecorderEventType::Started);
    REQUIRE(recorder.pop_event(event));
    REQUIRE(event.sequence_id == 11);
    REQUIRE(event.type == RealtimeSampleRecorderEventType::Cancelled);
}

TEST_CASE("RealtimeSampleRecorder materializes into a sample slot",
          "[audio][sampler][recorder]") {
    RealtimeSampleRecorder recorder;
    REQUIRE(recorder.prepare(RealtimeSampleRecorderConfig{2, 16, 48000.0}));
    RealtimeSampleRecorderCommand start;
    start.sequence_id = 1;
    start.type = RealtimeSampleRecorderCommandType::Start;
    start.max_frames = 4;
    REQUIRE(recorder.enqueue_command(start));

    Buffer<float> source(2, 8);
    fill_sequence(source, 1.0f);
    std::vector<const float*> source_ptrs;
    recorder.process(const_view(source, source_ptrs), 8);
    REQUIRE(recorder.state() == RealtimeSampleRecorderState::Completed);

    SampleSlotBank bank;
    REQUIRE(bank.prepare(1, 2, 16));
    const auto slot = bank.reserve_slot(2, 4, 48000.0);
    REQUIRE(slot == 0);
    REQUIRE(bank.slot_num_channels(0) == 2);
    REQUIRE(bank.slot_num_frames(0) == 4);
    std::vector<const float*> channel_scratch(recorder.num_channels());
    REQUIRE(materialize_completed_recording_to_slot(recorder,
                                                    bank,
                                                    0,
                                                    channel_scratch));
    REQUIRE(bank.slot_state(0) == SampleSlotState::Completed);
    REQUIRE(bank.slot_channel_data_for_test(0, 0)[3] == 4.0f);
    REQUIRE(bank.slot_channel_data_for_test(0, 1)[3] == 1004.0f);
    REQUIRE(recorder.state() == RealtimeSampleRecorderState::Idle);
    REQUIRE(recorder.frames_recorded() == 0);
}

TEST_CASE("RealtimeSampleRecorder rejects slot materialization shape mismatches",
          "[audio][sampler][recorder]") {
    RealtimeSampleRecorder recorder;
    REQUIRE(recorder.prepare(RealtimeSampleRecorderConfig{2, 8, 48000.0}));
    RealtimeSampleRecorderCommand start;
    start.type = RealtimeSampleRecorderCommandType::Start;
    start.max_frames = 4;
    REQUIRE(recorder.enqueue_command(start));

    Buffer<float> source(2, 4);
    fill_sequence(source, 1.0f);
    std::vector<const float*> source_ptrs;
    recorder.process(const_view(source, source_ptrs), 4);
    REQUIRE(recorder.state() == RealtimeSampleRecorderState::Completed);

    SampleSlotBank bank;
    REQUIRE(bank.prepare(1, 2, 8));
    REQUIRE(bank.reserve_slot(2, 3, 48000.0) == 0);
    std::vector<const float*> channel_scratch(recorder.num_channels());
    REQUIRE_FALSE(materialize_completed_recording_to_slot(recorder,
                                                          bank,
                                                          0,
                                                          channel_scratch));
    REQUIRE(bank.slot_state(0) == SampleSlotState::Reserved);
    REQUIRE(recorder.state() == RealtimeSampleRecorderState::Completed);
    REQUIRE(bank.release_slot(0));
    REQUIRE(bank.slot_state(0) == SampleSlotState::Free);
}

TEST_CASE("RealtimeSampleRecorder publishes completed recordings through high-level helper",
          "[audio][sampler][recorder]") {
    RealtimeSampleRecorder recorder;
    REQUIRE(recorder.prepare(RealtimeSampleRecorderConfig{1, 8, 48000.0}));
    RealtimeSampleRecorderCommand start;
    start.type = RealtimeSampleRecorderCommandType::Start;
    start.max_frames = 4;
    REQUIRE(recorder.enqueue_command(start));

    Buffer<float> source(1, 4);
    fill_sequence(source, 10.0f);
    std::vector<const float*> source_ptrs;
    recorder.process(const_view(source, source_ptrs), 4);
    REQUIRE(recorder.state() == RealtimeSampleRecorderState::Completed);

    SampleSlotBank bank;
    REQUIRE(bank.prepare(1, 1, 8));
    std::vector<const float*> channel_scratch(recorder.num_channels());
    REQUIRE(publish_completed_recording_to_slot(recorder, bank, channel_scratch));

    const auto published = bank.read_published_view();
    REQUIRE(published.valid);
    REQUIRE(bank.slot_state(published.slot_index) == SampleSlotState::Published);
    REQUIRE(bank.channel_data(published, 0)[0] == 10.0f);
    REQUIRE(bank.channel_data(published, 0)[3] == 13.0f);
    REQUIRE(recorder.state() == RealtimeSampleRecorderState::Idle);
    REQUIRE(recorder.frames_recorded() == 0);
}

TEST_CASE("RealtimeSampleRecorder high-level publish preserves recording on failure",
          "[audio][sampler][recorder]") {
    RealtimeSampleRecorder recorder;
    REQUIRE(recorder.prepare(RealtimeSampleRecorderConfig{1, 8, 48000.0}));
    RealtimeSampleRecorderCommand start;
    start.type = RealtimeSampleRecorderCommandType::Start;
    start.max_frames = 4;
    REQUIRE(recorder.enqueue_command(start));

    Buffer<float> source(1, 4);
    fill_sequence(source, 1.0f);
    std::vector<const float*> source_ptrs;
    recorder.process(const_view(source, source_ptrs), 4);
    REQUIRE(recorder.state() == RealtimeSampleRecorderState::Completed);

    SampleSlotBank bank;
    REQUIRE(bank.prepare(1, 1, 2));
    std::vector<const float*> channel_scratch(recorder.num_channels());
    REQUIRE_FALSE(publish_completed_recording_to_slot(recorder, bank, channel_scratch));
    REQUIRE(recorder.state() == RealtimeSampleRecorderState::Completed);
    REQUIRE(recorder.frames_recorded() == 4);
    REQUIRE_FALSE(bank.read_published_view().valid);
}

TEST_CASE("RealtimeSampleRecorder materializes a rolling capture snapshot off RT",
          "[audio][sampler][recorder]") {
    RollingAudioCaptureBuffer capture;
    REQUIRE(capture.prepare(RollingAudioCaptureBufferConfig{1, 8, sizeof(float)}));
    Buffer<float> source(1, 6);
    fill_sequence(source, 20.0f);
    std::vector<const float*> source_ptrs;
    capture.append(const_view(source, source_ptrs), 6);
    const auto snapshot = capture.snapshot_last(4);
    REQUIRE(snapshot.valid);

    RealtimeSampleRecorder recorder;
    REQUIRE(recorder.prepare(RealtimeSampleRecorderConfig{1, 8, 48000.0}));
    REQUIRE(recorder.materialize_snapshot_quiescent(capture, snapshot));
    REQUIRE(recorder.state() == RealtimeSampleRecorderState::Completed);
    REQUIRE(recorder.frames_recorded() == 4);
    REQUIRE(recorder.channel_data(0)[0] == 22.0f);
    REQUIRE(recorder.channel_data(0)[3] == 25.0f);
    REQUIRE(recorder.stats().total_recorded_frames == 4);

    RealtimeSampleRecorderEvent event;
    REQUIRE(recorder.pop_event(event));
    REQUIRE(event.type == RealtimeSampleRecorderEventType::Completed);
    REQUIRE(event.frames_recorded == 4);
}

TEST_CASE("RealtimeSampleRecorder materializes a held rolling snapshot off RT",
          "[audio][sampler][recorder]") {
    RollingAudioCaptureBuffer capture;
    REQUIRE(capture.prepare(RollingAudioCaptureBufferConfig{1, 8, sizeof(float)}));
    Buffer<float> source(1, 6);
    fill_sequence(source, 20.0f);
    std::vector<const float*> source_ptrs;
    capture.append(const_view(source, source_ptrs), 6);
    const auto snapshot = capture.begin_hold_last(4);
    REQUIRE(snapshot.valid);

    Buffer<float> ignored(1, 8);
    fill_sequence(ignored, 100.0f);
    std::vector<const float*> ignored_ptrs;
    capture.append(const_view(ignored, ignored_ptrs), 8);

    RealtimeSampleRecorder recorder;
    REQUIRE(recorder.prepare(RealtimeSampleRecorderConfig{1, 8, 48000.0}));
    REQUIRE(recorder.materialize_held_snapshot(capture, snapshot));
    capture.end_hold();

    REQUIRE(recorder.state() == RealtimeSampleRecorderState::Completed);
    REQUIRE(recorder.frames_recorded() == 4);
    REQUIRE(recorder.channel_data(0)[0] == 22.0f);
    REQUIRE(recorder.channel_data(0)[3] == 25.0f);
    REQUIRE(recorder.stats().total_recorded_frames == 4);

    RealtimeSampleRecorderEvent event;
    REQUIRE(recorder.pop_event(event));
    REQUIRE(event.type == RealtimeSampleRecorderEventType::Completed);
    REQUIRE(event.frames_recorded == 4);
}

TEST_CASE("RealtimeSampleRecorder rejects released held rolling snapshots",
          "[audio][sampler][recorder]") {
    RollingAudioCaptureBuffer capture;
    REQUIRE(capture.prepare(RollingAudioCaptureBufferConfig{1, 8, sizeof(float)}));
    Buffer<float> source(1, 6);
    fill_sequence(source, 20.0f);
    std::vector<const float*> source_ptrs;
    capture.append(const_view(source, source_ptrs), 6);
    const auto snapshot = capture.begin_hold_last(4);
    REQUIRE(snapshot.valid);
    capture.end_hold();

    RealtimeSampleRecorder recorder;
    REQUIRE(recorder.prepare(RealtimeSampleRecorderConfig{1, 8, 48000.0}));
    REQUIRE_FALSE(recorder.materialize_held_snapshot(capture, snapshot));
    REQUIRE(recorder.state() == RealtimeSampleRecorderState::Failed);
    REQUIRE(recorder.frames_recorded() == 0);

    RealtimeSampleRecorderEvent event;
    REQUIRE(recorder.pop_event(event));
    REQUIRE(event.type == RealtimeSampleRecorderEventType::Failed);
}

TEST_CASE("RealtimeSampleRecorder reports queue-full through metrics only",
          "[audio][sampler][recorder]") {
    RealtimeSampleRecorder recorder;
    REQUIRE(recorder.prepare(RealtimeSampleRecorderConfig{1, 8, 48000.0}));

    RealtimeSampleRecorderCommand reset;
    reset.type = RealtimeSampleRecorderCommandType::Reset;
    std::uint32_t accepted = 0;
    for (std::uint32_t i = 0; i < RealtimeSampleRecorder::kCommandQueueCapacity + 8; ++i) {
        reset.sequence_id = i + 1;
        if (recorder.enqueue_command(reset)) ++accepted;
    }
    REQUIRE(accepted <= RealtimeSampleRecorder::kCommandQueueCapacity);
    REQUIRE(recorder.stats().dropped_commands > 0);
    RealtimeSampleRecorderEvent queue_event;
    REQUIRE_FALSE(recorder.pop_event(queue_event));

    recorder.reset();
    Buffer<float> source(1, 1);
    std::vector<const float*> source_ptrs;
    for (std::uint32_t i = 0; i < RealtimeSampleRecorder::kEventQueueCapacity + 8; ++i) {
        reset.sequence_id = 1000 + i;
        REQUIRE(recorder.enqueue_command(reset));
        recorder.process(const_view(source, source_ptrs), 1);
    }
    REQUIRE(recorder.stats().dropped_events > 0);
}

TEST_CASE("RealtimeSampleRecorder queued reset invalidates without clearing captured storage",
          "[audio][sampler][recorder]") {
    RealtimeSampleRecorder recorder;
    REQUIRE(recorder.prepare(RealtimeSampleRecorderConfig{1, 8, 48000.0}));

    RealtimeSampleRecorderCommand start;
    start.sequence_id = 1;
    start.type = RealtimeSampleRecorderCommandType::Start;
    start.max_frames = 2;
    REQUIRE(recorder.enqueue_command(start));

    Buffer<float> source(1, 4);
    fill_sequence(source, 1.0f);
    std::vector<const float*> source_ptrs;
    recorder.process(const_view(source, source_ptrs), 4);
    REQUIRE(recorder.state() == RealtimeSampleRecorderState::Completed);
    REQUIRE(recorder.channel_data(0)[0] == 1.0f);
    REQUIRE(recorder.channel_data(0)[1] == 2.0f);

    RealtimeSampleRecorderCommand reset;
    reset.sequence_id = 2;
    reset.type = RealtimeSampleRecorderCommandType::Reset;
    REQUIRE(recorder.enqueue_command(reset));
    recorder.process(const_view(source, source_ptrs), 0);

    REQUIRE(recorder.state() == RealtimeSampleRecorderState::Idle);
    REQUIRE(recorder.frames_recorded() == 0);
    // Live Reset runs on the audio thread, so it invalidates the logical
    // recording without clearing the whole preallocated storage buffer.
    REQUIRE(recorder.channel_data(0)[0] == 1.0f);
    REQUIRE(recorder.channel_data(0)[1] == 2.0f);
}

TEST_CASE("RealtimeSampleRecorder drains commands on zero-frame callbacks",
          "[audio][sampler][recorder]") {
    RealtimeSampleRecorder recorder;
    REQUIRE(recorder.prepare(RealtimeSampleRecorderConfig{1, 8, 48000.0}));

    RealtimeSampleRecorderCommand start;
    start.sequence_id = 1;
    start.type = RealtimeSampleRecorderCommandType::Start;
    REQUIRE(recorder.enqueue_command(start));

    Buffer<float> empty(1, 0);
    recorder.process(empty.view(), 0);
    REQUIRE(recorder.state() == RealtimeSampleRecorderState::Recording);

    RealtimeSampleRecorderCommand cancel;
    cancel.sequence_id = 2;
    cancel.type = RealtimeSampleRecorderCommandType::Cancel;
    REQUIRE(recorder.enqueue_command(cancel));
    recorder.process(empty.view(), 0);
    REQUIRE(recorder.state() == RealtimeSampleRecorderState::Cancelled);

    RealtimeSampleRecorderEvent event;
    REQUIRE(recorder.pop_event(event));
    REQUIRE(event.type == RealtimeSampleRecorderEventType::Started);
    REQUIRE(recorder.pop_event(event));
    REQUIRE(event.type == RealtimeSampleRecorderEventType::Cancelled);
}

TEST_CASE("RealtimeSampleRecorder drains commands but records no audio for zero-channel sources",
          "[audio][sampler][recorder]") {
    RealtimeSampleRecorder recorder;
    REQUIRE(recorder.prepare(RealtimeSampleRecorderConfig{1, 8, 48000.0}));

    RealtimeSampleRecorderCommand start;
    start.sequence_id = 1;
    start.type = RealtimeSampleRecorderCommandType::Start;
    REQUIRE(recorder.enqueue_command(start));

    BufferView<const float> no_source(nullptr, 0, 4);
    recorder.process(no_source, 4);
    REQUIRE(recorder.state() == RealtimeSampleRecorderState::Recording);
    REQUIRE(recorder.frames_recorded() == 0);

    RealtimeSampleRecorderCommand cancel;
    cancel.sequence_id = 2;
    cancel.type = RealtimeSampleRecorderCommandType::Cancel;
    REQUIRE(recorder.enqueue_command(cancel));
    recorder.process(no_source, 4);
    REQUIRE(recorder.state() == RealtimeSampleRecorderState::Cancelled);
    REQUIRE(recorder.frames_recorded() == 0);

    RealtimeSampleRecorderEvent event;
    REQUIRE(recorder.pop_event(event));
    REQUIRE(event.sequence_id == 1);
    REQUIRE(event.type == RealtimeSampleRecorderEventType::Started);
    REQUIRE(event.frames_recorded == 0);
    REQUIRE(recorder.pop_event(event));
    REQUIRE(event.sequence_id == 2);
    REQUIRE(event.type == RealtimeSampleRecorderEventType::Cancelled);
    REQUIRE(event.frames_recorded == 0);
}

TEST_CASE("RealtimeSampleRecorder rejects over-capacity record requests",
          "[audio][sampler][recorder]") {
    RealtimeSampleRecorder recorder;
    REQUIRE(recorder.prepare(RealtimeSampleRecorderConfig{1, 4, 48000.0}));

    RealtimeSampleRecorderCommand start;
    start.sequence_id = 99;
    start.type = RealtimeSampleRecorderCommandType::Start;
    start.max_frames = 5;
    REQUIRE(recorder.enqueue_command(start));

    Buffer<float> empty(1, 0);
    recorder.process(empty.view(), 0);
    REQUIRE(recorder.state() == RealtimeSampleRecorderState::Idle);
    REQUIRE(recorder.frames_recorded() == 0);

    RealtimeSampleRecorderEvent event;
    REQUIRE(recorder.pop_event(event));
    REQUIRE(event.sequence_id == 99);
    REQUIRE(event.type == RealtimeSampleRecorderEventType::CommandRejected);

    RealtimeSampleRecorderCommand arm;
    arm.sequence_id = 100;
    arm.type = RealtimeSampleRecorderCommandType::Arm;
    arm.max_frames = 5;
    REQUIRE(recorder.enqueue_command(arm));
    recorder.process(empty.view(), 0);
    REQUIRE(recorder.state() == RealtimeSampleRecorderState::Idle);
    REQUIRE(recorder.pop_event(event));
    REQUIRE(event.sequence_id == 100);
    REQUIRE(event.type == RealtimeSampleRecorderEventType::CommandRejected);
}

TEST_CASE("RealtimeSampleRecorder rejects overwrite before completed data is consumed",
          "[audio][sampler][recorder]") {
    RealtimeSampleRecorder recorder;
    REQUIRE(recorder.prepare(RealtimeSampleRecorderConfig{1, 8, 48000.0}));

    RealtimeSampleRecorderCommand start_a;
    start_a.sequence_id = 1;
    start_a.type = RealtimeSampleRecorderCommandType::Start;
    start_a.max_frames = 2;
    RealtimeSampleRecorderCommand start_b;
    start_b.sequence_id = 2;
    start_b.type = RealtimeSampleRecorderCommandType::Start;
    start_b.timing.type = RealtimeSampleRecorderTimingType::BlockOffset;
    start_b.timing.frame_offset = 4;
    REQUIRE(recorder.enqueue_command(start_a));
    REQUIRE(recorder.enqueue_command(start_b));

    Buffer<float> source(1, 8);
    fill_sequence(source, 1.0f);
    std::vector<const float*> ptrs;
    recorder.process(const_view(source, ptrs), 8);
    REQUIRE(recorder.state() == RealtimeSampleRecorderState::Completed);
    REQUIRE(recorder.frames_recorded() == 2);
    REQUIRE(recorder.channel_data(0)[0] == 1.0f);
    REQUIRE(recorder.channel_data(0)[1] == 2.0f);

    RealtimeSampleRecorderEvent event;
    REQUIRE(recorder.pop_event(event));
    REQUIRE(event.type == RealtimeSampleRecorderEventType::Started);
    REQUIRE(recorder.pop_event(event));
    REQUIRE(event.type == RealtimeSampleRecorderEventType::Completed);
    REQUIRE(recorder.pop_event(event));
    REQUIRE(event.sequence_id == 2);
    REQUIRE(event.type == RealtimeSampleRecorderEventType::CommandRejected);

    RealtimeSampleRecorderCommand arm;
    arm.sequence_id = 3;
    arm.type = RealtimeSampleRecorderCommandType::Arm;
    REQUIRE(recorder.enqueue_command(arm));

    Buffer<float> empty(1, 0);
    recorder.process(empty.view(), 0);
    REQUIRE(recorder.state() == RealtimeSampleRecorderState::Completed);
    REQUIRE(recorder.frames_recorded() == 2);
    REQUIRE(recorder.channel_data(0)[0] == 1.0f);
    REQUIRE(recorder.channel_data(0)[1] == 2.0f);
    REQUIRE(recorder.pop_event(event));
    REQUIRE(event.sequence_id == 3);
    REQUIRE(event.type == RealtimeSampleRecorderEventType::CommandRejected);
}

TEST_CASE("RealtimeSampleRecorder emits rejection events for invalid stop and cancel",
          "[audio][sampler][recorder]") {
    RealtimeSampleRecorder recorder;
    REQUIRE(recorder.prepare(RealtimeSampleRecorderConfig{1, 8, 48000.0}));

    RealtimeSampleRecorderCommand stop;
    stop.sequence_id = 7;
    stop.type = RealtimeSampleRecorderCommandType::Stop;
    RealtimeSampleRecorderCommand cancel;
    cancel.sequence_id = 8;
    cancel.type = RealtimeSampleRecorderCommandType::Cancel;
    REQUIRE(recorder.enqueue_command(stop));
    REQUIRE(recorder.enqueue_command(cancel));

    Buffer<float> empty(1, 0);
    recorder.process(empty.view(), 0);

    RealtimeSampleRecorderEvent event;
    REQUIRE(recorder.pop_event(event));
    REQUIRE(event.sequence_id == 7);
    REQUIRE(event.type == RealtimeSampleRecorderEventType::CommandRejected);
    REQUIRE(recorder.pop_event(event));
    REQUIRE(event.sequence_id == 8);
    REQUIRE(event.type == RealtimeSampleRecorderEventType::CommandRejected);
}

TEST_CASE("RealtimeSampleRecorder hot path runs under no-allocation guard",
          "[audio][sampler][rt][recorder]") {
    RealtimeSampleRecorder recorder;
    REQUIRE(recorder.prepare(RealtimeSampleRecorderConfig{1, 16, 48000.0}));
    RealtimeSampleRecorderCommand start;
    start.sequence_id = 1;
    start.type = RealtimeSampleRecorderCommandType::Start;
    start.max_frames = 4;
    REQUIRE(recorder.enqueue_command(start));

    Buffer<float> source(1, 8);
    fill_sequence(source, 1.0f);
    std::vector<const float*> source_ptrs;
    const auto input = const_view(source, source_ptrs);
    {
        pulp::runtime::ScopedNoAlloc guard;
        recorder.process(input, 8);
    }
    REQUIRE(recorder.state() == RealtimeSampleRecorderState::Completed);
    REQUIRE(recorder.frames_recorded() == 4);
}
