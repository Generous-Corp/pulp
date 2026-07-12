#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/audio_stream_handoff.hpp>
#include <pulp/audio/audio_tap.hpp>
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

TEST_CASE("PlanarAudioRingBuffer exposes requested usable capacity",
          "[audio][sampler][ring]") {
    PlanarAudioRingBuffer ring;
    REQUIRE_FALSE(ring.prepare(0, 4));
    REQUIRE_FALSE(ring.prepare(2, 0));

    REQUIRE(ring.prepare(2, 4));
    REQUIRE(ring.num_channels() == 2);
    REQUIRE(ring.capacity_frames() == 4);
    REQUIRE(ring.available_frames() == 0);
    REQUIRE(ring.free_frames() == 4);

    REQUIRE_FALSE(ring.prepare(2, 0));
    REQUIRE(ring.num_channels() == 0);
    REQUIRE(ring.capacity_frames() == 0);
    REQUIRE(ring.free_frames() == 0);
}

TEST_CASE("PlanarAudioRingBuffer preserves order across wraparound",
          "[audio][sampler][ring]") {
    PlanarAudioRingBuffer ring;
    REQUIRE(ring.prepare(2, 5));

    Buffer<float> first(2, 5);
    fill_sequence(first, 1.0f);
    std::vector<const float*> first_ptrs;
    REQUIRE(ring.write(const_view(first, first_ptrs), 5) == 5);

    Buffer<float> discarded(2, 3);
    REQUIRE(ring.read(discarded.view(), 3));

    Buffer<float> second(2, 3);
    fill_sequence(second, 100.0f);
    std::vector<const float*> second_ptrs;
    REQUIRE(ring.write(const_view(second, second_ptrs), 3) == 3);

    Buffer<float> out(2, 5);
    REQUIRE(ring.read(out.view(), 5));
    REQUIRE(out.channel(0)[0] == 4.0f);
    REQUIRE(out.channel(0)[1] == 5.0f);
    REQUIRE(out.channel(0)[2] == 100.0f);
    REQUIRE(out.channel(0)[3] == 101.0f);
    REQUIRE(out.channel(0)[4] == 102.0f);
    REQUIRE(out.channel(1)[0] == 1004.0f);
    REQUIRE(out.channel(1)[4] == 1102.0f);
}

TEST_CASE("PlanarAudioRingBuffer reports overrun and underrun with zero-fill",
          "[audio][sampler][ring]") {
    PlanarAudioRingBuffer ring;
    REQUIRE(ring.prepare(1, 2));

    Buffer<float> source(1, 4);
    fill_sequence(source, 10.0f);
    std::vector<const float*> source_ptrs;
    REQUIRE(ring.write(const_view(source, source_ptrs), 4) == 2);
    REQUIRE(ring.stats().dropped_write_frames == 2);
    REQUIRE(ring.stats().overrun_frames == 2);

    Buffer<float> out(1, 4);
    REQUIRE_FALSE(ring.read(out.view(), 4));
    REQUIRE(out.channel(0)[0] == 10.0f);
    REQUIRE(out.channel(0)[1] == 11.0f);
    REQUIRE(out.channel(0)[2] == 0.0f);
    REQUIRE(out.channel(0)[3] == 0.0f);
    REQUIRE(ring.stats().underrun_frames == 2);
}

TEST_CASE("PlanarAudioRingBuffer zero-fills missing and extra channels",
          "[audio][sampler][ring]") {
    PlanarAudioRingBuffer ring;
    REQUIRE(ring.prepare(2, 3));

    Buffer<float> mono(1, 3);
    fill_sequence(mono, 1.0f);
    std::vector<const float*> mono_ptrs;
    REQUIRE(ring.write(const_view(mono, mono_ptrs), 3) == 3);

    Buffer<float> out(3, 3);
    REQUIRE(ring.read(out.view(), 3));
    REQUIRE(out.channel(0)[2] == 3.0f);
    REQUIRE(out.channel(1)[0] == 0.0f);
    REQUIRE(out.channel(1)[2] == 0.0f);
    REQUIRE(out.channel(2)[0] == 0.0f);
    REQUIRE(out.channel(2)[2] == 0.0f);
}

TEST_CASE("PlanarAudioRingBuffer zero-channel reads do not drain audio",
          "[audio][sampler][ring]") {
    PlanarAudioRingBuffer ring;
    REQUIRE(ring.prepare(1, 4));

    Buffer<float> source(1, 3);
    fill_sequence(source, 1.0f);
    std::vector<const float*> source_ptrs;
    REQUIRE(ring.write(const_view(source, source_ptrs), 3) == 3);

    BufferView<float> no_channels(nullptr, 0, 2);
    REQUIRE(ring.read(no_channels, 2));
    REQUIRE(ring.available_frames() == 3);

    Buffer<float> out(1, 3);
    REQUIRE(ring.read(out.view(), 3));
    REQUIRE(out.channel(0)[0] == 1.0f);
    REQUIRE(out.channel(0)[2] == 3.0f);
}

TEST_CASE("PlanarAudioRingBuffer zero-channel writes do not advance audio",
          "[audio][sampler][ring]") {
    PlanarAudioRingBuffer ring;
    REQUIRE(ring.prepare(1, 4));

    BufferView<const float> no_source(nullptr, 0, 3);
    REQUIRE(ring.write(no_source, 3) == 0);
    REQUIRE(ring.available_frames() == 0);
    REQUIRE(ring.free_frames() == 4);
    REQUIRE(ring.stats().overrun_frames == 0);
    REQUIRE(ring.stats().dropped_write_frames == 0);
}

TEST_CASE("AudioTap drops overflow in whole aligned frames", "[audio][tap]") {
    pulp::audio::AudioTap tap;
    REQUIRE(tap.prepare(2, 2));
    Buffer<float> source(2, 4);
    for (std::size_t frame = 0; frame < 4; ++frame) {
        source.channel(0)[frame] = static_cast<float>(frame + 1);
        source.channel(1)[frame] = static_cast<float>(101 + frame);
    }
    std::vector<const float*> pointers;
    REQUIRE(tap.push(const_view(source, pointers)) == 2);
    REQUIRE(tap.stats().dropped_write_frames == 2);

    Buffer<float> output(2, 2);
    REQUIRE(tap.read(output.view()));
    REQUIRE(output.channel(0)[1] == 2.0f);
    REQUIRE(output.channel(1)[1] == 102.0f);
}
